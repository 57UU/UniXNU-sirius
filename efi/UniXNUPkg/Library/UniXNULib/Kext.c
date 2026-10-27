/** @file
  Read-only kext staging for external XNU loaders.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Internal.h"

STATIC
BOOLEAN
KextImageValid (
  IN CONST VOID  *Data,
  IN UINTN       Size
  )
{
  CONST UINT8  *Bytes;
  UINT32       Commands;
  UINT32       CommandSize;
  UINT32       Subtype;
  UINTN        Offset;
  UINTN        End;
  UINTN        Index;

  Bytes = Data;
  if ((Size < 32) || (ReadUnaligned32 ((CONST UINT32 *)Bytes) != 0xFEEDFACF) ||
      (ReadUnaligned32 ((CONST UINT32 *)(Bytes + 4)) != 0x0100000C) ||
      (ReadUnaligned32 ((CONST UINT32 *)(Bytes + 12)) != 11))
  {
    return FALSE;
  }

  // Accept thin arm64/arm64e bundles. A loader must enforce its kernel ABI.
  Subtype = ReadUnaligned32 ((CONST UINT32 *)(Bytes + 8)) & 0x00FFFFFF;
  if ((Subtype != 0) && (Subtype != 2)) {
    return FALSE;
  }

  Commands    = ReadUnaligned32 ((CONST UINT32 *)(Bytes + 16));
  CommandSize = ReadUnaligned32 ((CONST UINT32 *)(Bytes + 20));
  if ((CommandSize > Size - 32) || (Commands > CommandSize / 8)) {
    return FALSE;
  }

  End    = 32 + CommandSize;
  Offset = 32;
  for (Index = 0; Index < Commands; Index++) {
    if (End - Offset < 8) {
      return FALSE;
    }

    CommandSize = ReadUnaligned32 ((CONST UINT32 *)(Bytes + Offset + 4));
    if ((CommandSize < 8) || ((CommandSize & 7) != 0) || (CommandSize > End - Offset)) {
      return FALSE;
    }

    Offset += CommandSize;
  }

  return Offset == End;
}

VOID
UniXNUFreeKexts (
  IN UNIXNU_LOADER_KEXT  *Kexts OPTIONAL,
  IN UINTN               Count
  )
{
  UINTN  Index;

  if (Kexts == NULL) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (Kexts[Index].InfoPlist != NULL) {
      FreePool ((VOID *)Kexts[Index].InfoPlist);
    }

    if (Kexts[Index].Executable != NULL) {
      FreePool ((VOID *)Kexts[Index].Executable);
    }
  }

  FreePool (Kexts);
}

STATIC
BOOLEAN
KitKextMatches (
  IN UNIXNU_KEXT   *Kext,
  IN UNIXNU_ENTRY  *Entry
  )
{
  return (Kext->EntryId != NULL) && (Entry->Id != NULL) &&
         (AsciiStrCmp (Kext->EntryId, Entry->Id) == 0);
}

EFI_STATUS
UniXNUStageKexts (
  IN  UNIXNU_CONFIG           *Config,
  IN  UNIXNU_ENTRY            *Entry,
  OUT UNIXNU_LOADER_KEXT  **Kexts,
  OUT UINTN               *Count
  )
{
  EFI_STATUS           Status;
  UINTN                Total;
  UINTN                Index;
  UINTN                Used;
  UINTN                Previous;
  UNIXNU_KEXT              *Source;
  UNIXNU_LOADER_KEXT    *Out;
  VOID                 *Data;
  UINTN                Size;

  *Kexts = NULL;
  *Count = 0;
  if (Config->KextConfigError || Entry->KextConfigError) {
    UniXNULog ("kext: invalid configuration; boot refused");
    return EFI_INVALID_PARAMETER;
  }

  Total = Entry->KextCount;
  for (Index = 0; Index < Config->KextCount; Index++) {
    if (Config->Kexts[Index].Invalid || KitKextMatches (&Config->Kexts[Index], Entry)) {
      Total++;
    }
  }

  if (Total == 0) {
    return EFI_SUCCESS;
  }

  Out = AllocateZeroPool (Total * sizeof (*Out));
  if (Out == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Used   = 0;
  Status = EFI_SUCCESS;
  for (Index = 0; Index < Config->KextCount + Entry->KextCount; Index++) {
    if (Index < Config->KextCount) {
      Source = &Config->Kexts[Index];
      if (!Source->Invalid && !KitKextMatches (Source, Entry)) {
        continue;
      }
    } else {
      Source = &Entry->Kexts[Index - Config->KextCount];
    }

    if (Source->Invalid) {
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }

    for (Previous = 0; Previous < Used; Previous++) {
      if (UniXNUStriCmp (Out[Previous].BundlePath, Source->BundlePath) == 0) {
        UniXNULog ("kext %s: duplicate bundle; boot refused", Source->BundlePath);
        Status = EFI_INVALID_PARAMETER;
        break;
      }
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    Out[Used].BundlePath     = Source->BundlePath;
    Out[Used].PlistPath      = Source->PlistPath;
    Out[Used].ExecutablePath = Source->ExecutablePath;
    Status = UniXNUReadFile (Source->Volume, Source->PlistPath, &Data, &Size);
    if (EFI_ERROR (Status)) {
      UniXNULog ("kext %s: cannot read plist: %r", Source->BundlePath, Status);
      break;
    }

    Out[Used].InfoPlist     = Data;
    Out[Used].InfoPlistSize = Size;
    if (Size == 0) {
      UniXNULog ("kext %s: empty plist; boot refused", Source->BundlePath);
      Status = EFI_LOAD_ERROR;
      break;
    }

    if (Source->ExecutablePath != NULL) {
      Status = UniXNUReadFile (Source->Volume, Source->ExecutablePath, &Data, &Size);
      if (EFI_ERROR (Status)) {
        UniXNULog ("kext %s: cannot read executable: %r", Source->BundlePath, Status);
        break;
      }

      Out[Used].Executable     = Data;
      Out[Used].ExecutableSize = Size;
      if (!KextImageValid (Data, Size)) {
        UniXNULog ("kext %s: invalid AArch64 MH_KEXT_BUNDLE; boot refused", Source->BundlePath);
        Status = EFI_LOAD_ERROR;
        break;
      }
    }

    UniXNULog ("kext %s: staged (%u plist, %u executable bytes)", Source->BundlePath,
           (UINT32)Out[Used].InfoPlistSize, (UINT32)Out[Used].ExecutableSize);
    Used++;
  }

  if (EFI_ERROR (Status)) {
    UniXNUFreeKexts (Out, Total);
    return Status;
  }

  *Kexts = Out;
  *Count = Used;
  return EFI_SUCCESS;
}
