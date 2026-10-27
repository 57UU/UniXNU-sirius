/** @file
  Injection engine: driver loading, ACPI table installation, device tree
  patching, Linux initrd media and entry variables.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Internal.h"

#include <Guid/Fdt.h>
#include <Guid/LinuxEfiInitrdMedia.h>
#include <IndustryStandard/Acpi.h>
#include <Library/FdtLib.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/LoadFile2.h>

//
// Room added to every DTB copy for properties and nodes UniXNU adds.
//
#define UNIXNU_FDT_SLACK  SIZE_64KB

// ---------------------------------------------------------------------------
// Drivers
// ---------------------------------------------------------------------------

VOID
UniXNUConnectAll (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;

  Status = gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
  }

  FreePool (Handles);
}

//
// The menu shows only how many drivers failed. Keep the first reason where it
// can be read, as on a machine without a serial console the log is out of reach.
//
STATIC
VOID
NoteDriverFailure (
  IN UNIXNU_CONFIG      *Config,
  IN CONST CHAR16   *File,
  IN CONST CHAR8    *What,
  IN EFI_STATUS     Status
  )
{
  if (Config->Error[0] == '\0') {
    AsciiSPrint (Config->Error, sizeof (Config->Error), "driver %s: %a: %r", File, What, Status);
  }
}

VOID
UniXNUInjectDrivers (
  IN  EFI_HANDLE       ImageHandle,
  IN  UNIXNU_CONFIG        *Config,
  OUT UNIXNU_INJECT_STATS  *Stats
  )
{
  EFI_STATUS                 Status;
  UINTN                      Index;
  UNIXNU_DRIVER                  *Driver;
  EFI_DEVICE_PATH_PROTOCOL   *Path;
  EFI_HANDLE                 Image;
  EFI_LOADED_IMAGE_PROTOCOL  *Loaded;
  BOOLEAN                    NeedConnect;

  NeedConnect = FALSE;
  for (Index = 0; Index < Config->DriverCount; Index++) {
    Driver = &Config->Drivers[Index];
    Path   = UniXNUResolveImagePath (Driver->Volume, Driver->File);
    if (Path == NULL) {
      UniXNULog ("driver %s (%a): cannot resolve path", Driver->File, Driver->Origin);
      NoteDriverFailure (Config, Driver->File, "cannot resolve path", EFI_NOT_FOUND);
      Stats->DriversFailed++;
      continue;
    }

    Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &Image);
    FreePool (Path);
    if (EFI_ERROR (Status)) {
      //
      // LoadImage may hand back a handle on security failures; drop it.
      //
      if ((Status == EFI_SECURITY_VIOLATION) && (Image != NULL)) {
        gBS->UnloadImage (Image);
      }

      UniXNULog ("driver %s (%a): load failed: %r", Driver->File, Driver->Origin, Status);
      NoteDriverFailure (Config, Driver->File, "load failed", Status);
      Stats->DriversFailed++;
      continue;
    }

    if ((Driver->Args != NULL) &&
        !EFI_ERROR (gBS->HandleProtocol (Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded)))
    {
      Loaded->LoadOptions     = Driver->Args;
      Loaded->LoadOptionsSize = (UINT32)StrSize (Driver->Args);
    }

    Status = gBS->StartImage (Image, NULL, NULL);
    if (EFI_ERROR (Status)) {
      UniXNULog ("driver %s (%a): start failed: %r", Driver->File, Driver->Origin, Status);
      NoteDriverFailure (Config, Driver->File, "start failed", Status);
      Stats->DriversFailed++;
      continue;
    }

    UniXNULog ("driver %s (%a): started", Driver->File, Driver->Origin);
    Stats->DriversLoaded++;
    NeedConnect = NeedConnect || Driver->Connect;
  }

  //
  // UEFI driver-model drivers only bind on ConnectController.
  //
  if (NeedConnect && Config->ConnectAll) {
    UniXNUConnectAll ();
  }
}

// ---------------------------------------------------------------------------
// ACPI
// ---------------------------------------------------------------------------

VOID
UniXNUInjectAcpi (
  IN  UNIXNU_CONFIG        *Config,
  OUT UNIXNU_INJECT_STATS  *Stats
  )
{
  EFI_STATUS               Status;
  EFI_ACPI_TABLE_PROTOCOL  *AcpiTable;
  UINTN                    Index;
  VOID                     *Data;
  UINTN                    Size;
  UINTN                    Key;

  if (Config->AcpiCount == 0) {
    return;
  }

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (EFI_ERROR (Status)) {
    UniXNULog ("acpi: no EFI_ACPI_TABLE_PROTOCOL, %u table(s) skipped", (UINT32)Config->AcpiCount);
    Stats->AcpiFailed += Config->AcpiCount;
    return;
  }

  for (Index = 0; Index < Config->AcpiCount; Index++) {
    Status = UniXNUReadFile (Config->AcpiTables[Index].Volume, Config->AcpiTables[Index].File, &Data, &Size);
    if (!EFI_ERROR (Status)) {
      if ((Size < sizeof (EFI_ACPI_DESCRIPTION_HEADER)) ||
          (((EFI_ACPI_DESCRIPTION_HEADER *)Data)->Length != Size))
      {
        Status = EFI_VOLUME_CORRUPTED;
      } else {
        Status = AcpiTable->InstallAcpiTable (AcpiTable, Data, Size, &Key);
      }

      FreePool (Data);
    }

    UniXNULog ("acpi %s (%a): %r", Config->AcpiTables[Index].File, Config->AcpiTables[Index].Origin, Status);
    if (EFI_ERROR (Status)) {
      Stats->AcpiFailed++;
    } else {
      Stats->AcpiInstalled++;
    }
  }
}

// ---------------------------------------------------------------------------
// Device tree
// ---------------------------------------------------------------------------

CONST CHAR8 *
UniXNUFirmwareCompatible (
  OUT UINTN  *Length
  )
{
  VOID         *Fdt;
  CONST CHAR8  *Value;
  INT32        Len;

  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt)) || (FdtCheckHeader (Fdt) != 0)) {
    return NULL;
  }

  Value = FdtGetProp (Fdt, 0, "compatible", &Len);
  if ((Value == NULL) || (Len <= 0)) {
    return NULL;
  }

  *Length = (UINTN)Len;
  return Value;
}

//
// Offset of Path, creating missing nodes along the way.
//
STATIC
INT32
EnsureNode (
  IN VOID         *Fdt,
  IN CONST CHAR8  *Path
  )
{
  INT32        Offset;
  INT32        Parent;
  CONST CHAR8  *Start;
  CONST CHAR8  *End;

  Offset = FdtPathOffset (Fdt, Path);
  if (Offset >= 0) {
    return Offset;
  }

  Parent = 0;
  Start  = Path;
  while (*Start != '\0') {
    while (*Start == '/') {
      Start++;
    }

    if (*Start == '\0') {
      break;
    }

    End = Start;
    while (*End != '\0' && *End != '/') {
      End++;
    }

    Offset = FdtSubnodeOffsetNameLen (Fdt, Parent, Start, (INT32)(End - Start));
    if (Offset < 0) {
      CHAR8  Name[128];
      UINTN  Len = MIN ((UINTN)(End - Start), sizeof (Name) - 1);

      CopyMem (Name, Start, Len);
      Name[Len] = '\0';
      Offset    = FdtAddSubnode (Fdt, Parent, Name);
      if (Offset < 0) {
        return Offset;
      }
    }

    Parent = Offset;
    Start  = End;
  }

  return Parent;
}

EFI_STATUS
UniXNUApplyDtPatches (
  IN OUT VOID         *Fdt,
  IN     UNIXNU_DT_PATCH  *Patches,
  IN     UINTN        Count
  )
{
  UINTN        Index;
  UNIXNU_DT_PATCH  *Patch;
  INT32        Offset;
  INT32        Result;
  VOID         *Overlay;
  UINTN        Failed;

  Failed = 0;
  for (Index = 0; Index < Count; Index++) {
    Patch  = &Patches[Index];
    Result = 0;
    switch (Patch->Op) {
      case UniXNUDtSet:
        Offset = EnsureNode (Fdt, Patch->Node);
        Result = (Offset < 0) ? Offset : FdtSetProp (Fdt, Offset, Patch->Prop, Patch->Value, (UINT32)Patch->ValueSize);
        break;

      case UniXNUDtDeleteProp:
        Offset = FdtPathOffset (Fdt, Patch->Node);
        Result = (Offset < 0) ? Offset : FdtDelProp (Fdt, Offset, Patch->Prop);
        break;

      case UniXNUDtAddNode:
        Result = EnsureNode (Fdt, Patch->Node);
        Result = (Result < 0) ? Result : 0;
        break;

      case UniXNUDtDeleteNode:
        Offset = FdtPathOffset (Fdt, Patch->Node);
        Result = (Offset < 0) ? Offset : FdtDelNode (Fdt, Offset);
        break;

      case UniXNUDtOverlay:
        //
        // fdt_overlay_apply() consumes the overlay; work on a copy so the
        // entry can be booted again.
        //
        Overlay = AllocateCopyPool (Patch->ValueSize, Patch->Value);
        if (Overlay == NULL) {
          Result = -1;
          break;
        }

        Result = FdtOverlayApply (Fdt, Overlay);
        FreePool (Overlay);
        break;
    }

    if (Result < 0) {
      Failed++;
      UniXNULog (
        "dt: %a: op %u on %a%a%a failed: %a",
        Patch->Origin,
        (UINT32)Patch->Op,
        (Patch->Node != NULL) ? Patch->Node : "(overlay)",
        (Patch->Prop != NULL) ? ":" : "",
        (Patch->Prop != NULL) ? Patch->Prop : "",
        FdtStrerror (Result)
        );
    }
  }

  return (Failed == 0) ? EFI_SUCCESS : EFI_PROTOCOL_ERROR;
}

EFI_STATUS
UniXNUPrepareDeviceTree (
  IN  UNIXNU_CONFIG  *Config,
  IN  UNIXNU_ENTRY   *Entry,
  OUT VOID       **Fdt OPTIONAL,
  OUT UINTN      *FdtSize OPTIONAL
  )
{
  EFI_STATUS            Status;
  VOID                  *Base;
  UINTN                 BaseSize;
  BOOLEAN               FreeBase;
  UINTN                 Size;
  UINTN                 Index;
  EFI_PHYSICAL_ADDRESS  Address;
  VOID                  *Tree;

  if (Entry->Dtb == NULL) {
    return EFI_SUCCESS;
  }

  FreeBase = FALSE;
  if (StrCmp (Entry->Dtb, L"firmware") == 0) {
    Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Base);
    if (EFI_ERROR (Status) || (FdtCheckHeader (Base) != 0)) {
      UniXNULog ("dt: entry wants the firmware DTB but none is installed");
      return EFI_NOT_FOUND;
    }

    BaseSize = Fdt32ToCpu (((FDT_HEADER *)Base)->TotalSize);
  } else {
    Status = UniXNUReadFile (Entry->Volume, Entry->Dtb, &Base, &BaseSize);
    if (EFI_ERROR (Status)) {
      UniXNULog ("dt: cannot read %s: %r", Entry->Dtb, Status);
      return Status;
    }

    FreeBase = TRUE;
    if (FdtCheckHeader (Base) != 0) {
      UniXNULog ("dt: %s is not a flattened device tree", Entry->Dtb);
      FreePool (Base);
      return EFI_VOLUME_CORRUPTED;
    }
  }

  //
  // Overlays need room for their contents in the base tree.
  //
  Size = BaseSize + UNIXNU_FDT_SLACK;
  for (Index = 0; Index < Config->PatchCount; Index++) {
    Size += Config->Patches[Index].ValueSize;
  }

  for (Index = 0; Index < Entry->PatchCount; Index++) {
    Size += Entry->Patches[Index].ValueSize;
  }

  //
  // ACPI reclaim memory survives ExitBootServices(), as the DTB table must.
  //
  Status = gBS->AllocatePages (AllocateAnyPages, EfiACPIReclaimMemory, EFI_SIZE_TO_PAGES (Size), &Address);
  if (EFI_ERROR (Status)) {
    if (FreeBase) {
      FreePool (Base);
    }

    return Status;
  }

  Tree = (VOID *)(UINTN)Address;
  if (FdtOpenInto (Base, Tree, (INT32)Size) != 0) {
    Status = EFI_VOLUME_CORRUPTED;
  }

  if (FreeBase) {
    FreePool (Base);
  }

  if (!EFI_ERROR (Status)) {
    //
    // Global (config + kits) first, then the entry's own, /chosen last.
    //
    UniXNUApplyDtPatches (Tree, Config->Patches, Config->PatchCount);
    UniXNUApplyDtPatches (Tree, Entry->Patches, Entry->PatchCount);
    FdtPack (Tree);
    Status = gBS->InstallConfigurationTable (&gFdtTableGuid, Tree);
  }

  if (EFI_ERROR (Status)) {
    gBS->FreePages (Address, EFI_SIZE_TO_PAGES (Size));
    return Status;
  }

  UniXNULog (
    "dt: installed %u bytes, %u global + %u entry patch(es)",
    Fdt32ToCpu (((FDT_HEADER *)Tree)->TotalSize),
    (UINT32)Config->PatchCount,
    (UINT32)Entry->PatchCount
    );

  if (Fdt != NULL) {
    *Fdt = Tree;
  }

  if (FdtSize != NULL) {
    *FdtSize = Fdt32ToCpu (((FDT_HEADER *)Tree)->TotalSize);
  }

  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Linux initrd media (LINUX_EFI_INITRD_MEDIA_GUID + EFI_LOAD_FILE2_PROTOCOL)
// ---------------------------------------------------------------------------

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          VenMedia;
  EFI_DEVICE_PATH_PROTOCOL    End;
} INITRD_DEVICE_PATH;
#pragma pack ()

typedef struct {
  EFI_LOAD_FILE2_PROTOCOL    LoadFile2;
  VOID                       *Data;
  UINTN                      Size;
} INITRD_CONTEXT;

STATIC CONST INITRD_DEVICE_PATH  mInitrdDevicePath = {
  {
    { MEDIA_DEVICE_PATH, MEDIA_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 }
    },
    LINUX_EFI_INITRD_MEDIA_GUID
  },
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC INITRD_CONTEXT  mInitrd;

STATIC
EFI_STATUS
EFIAPI
InitrdLoadFile2 (
  IN     EFI_LOAD_FILE2_PROTOCOL   *This,
  IN     EFI_DEVICE_PATH_PROTOCOL  *FilePath,
  IN     BOOLEAN                   BootPolicy,
  IN OUT UINTN                     *BufferSize,
  IN     VOID                      *Buffer OPTIONAL
  )
{
  INITRD_CONTEXT  *Context;

  Context = BASE_CR (This, INITRD_CONTEXT, LoadFile2);
  if (BootPolicy) {
    return EFI_UNSUPPORTED;
  }

  if ((BufferSize == NULL) || !IsDevicePathValid (FilePath, 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((FilePath->Type != END_DEVICE_PATH_TYPE) || (FilePath->SubType != END_ENTIRE_DEVICE_PATH_SUBTYPE)) {
    return EFI_NOT_FOUND;
  }

  if ((Buffer == NULL) || (*BufferSize < Context->Size)) {
    *BufferSize = Context->Size;
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (Buffer, Context->Data, Context->Size);
  *BufferSize = Context->Size;
  return EFI_SUCCESS;
}

EFI_STATUS
UniXNUInstallInitrd (
  IN  VOID        *Data,
  IN  UINTN       Size,
  OUT EFI_HANDLE  *Handle
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  EFI_HANDLE                Existing;

  //
  // Only one provider may exist; refuse to shadow somebody else's.
  //
  Path   = (EFI_DEVICE_PATH_PROTOCOL *)&mInitrdDevicePath;
  Status = gBS->LocateDevicePath (&gEfiLoadFile2ProtocolGuid, &Path, &Existing);
  if (!EFI_ERROR (Status) && IsDevicePathEnd (Path)) {
    return EFI_ALREADY_STARTED;
  }

  mInitrd.LoadFile2.LoadFile = InitrdLoadFile2;
  mInitrd.Data               = Data;
  mInitrd.Size               = Size;
  *Handle                    = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                Handle,
                &gEfiDevicePathProtocolGuid,
                &mInitrdDevicePath,
                &gEfiLoadFile2ProtocolGuid,
                &mInitrd.LoadFile2,
                NULL
                );
}

VOID
UniXNUUninstallInitrd (
  IN EFI_HANDLE  Handle
  )
{
  if (Handle == NULL) {
    return;
  }

  gBS->UninstallMultipleProtocolInterfaces (
         Handle,
         &gEfiDevicePathProtocolGuid,
         &mInitrdDevicePath,
         &gEfiLoadFile2ProtocolGuid,
         &mInitrd.LoadFile2,
         NULL
         );
}

// ---------------------------------------------------------------------------
// Variables
// ---------------------------------------------------------------------------

EFI_STATUS
UniXNUSetEntryVariables (
  IN UNIXNU_ENTRY  *Entry
  )
{
  EFI_STATUS   Status;
  EFI_STATUS   Result;
  UINTN        Index;
  UNIXNU_VARIABLE  *Var;

  Result = EFI_SUCCESS;
  for (Index = 0; Index < Entry->VariableCount; Index++) {
    Var    = &Entry->Variables[Index];
    Status = gRT->SetVariable (Var->Name, &Var->Guid, Var->Attributes, Var->DataSize, Var->Data);
    UniXNULog ("variable %g:%s (%u bytes): %r", &Var->Guid, Var->Name, (UINT32)Var->DataSize, Status);
    if (EFI_ERROR (Status)) {
      Result = Status;
    }
  }

  return Result;
}
