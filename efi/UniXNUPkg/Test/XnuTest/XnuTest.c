/** @file
  Synthetic XNU protocol receiver. Never executes a kernel or a kext.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/UniXNULoader.h>
#include <Protocol/LoadedImage.h>

STATIC BOOLEAN  mLegacy;
STATIC UINTN    mCalls;

STATIC
EFI_STATUS
EFIAPI
TestBoot (
  IN UNIXNU_LOADER_PROTOCOL     *This,
  IN CONST UNIXNU_LOADER_BOOT_REQUEST  *Request
  )
{
  UINTN         Index;
  UINTN         Length;
  BOOLEAN       Ok;
  CHAR8         Plist[256];
  CONST CHAR8   *Names[] = { "Kit", "Codeless", "Code" };
  CONST CHAR16  *Suffixes[] = { L"Kit.kext", L"Codeless.kext", L"Code.kext" };
  CONST CHAR8   Marker[] = "TEST-KEXT-CODE";

  Ok = (Request->Volume != NULL) && (Request->KernelPath != NULL) &&
       (StrStr (Request->KernelPath, L"synthetic-kernel") != NULL) &&
       (Request->BootArgs != NULL) && (StrCmp (Request->BootArgs, L"test handoff") == 0);
  if (!Ok) {
    Print (L"UNIXNU-XNU-TEST: bad base request path '%s' args '%s'\n",
           Request->KernelPath, Request->BootArgs);
  }
  if (!mLegacy) {
    Ok = Ok && (Request->KextCount == 3) && (Request->Kexts != NULL);
    for (Index = 0; Ok && (Index < Request->KextCount); Index++) {
      CONST UNIXNU_LOADER_KEXT  *Kext = &Request->Kexts[Index];

      Length = AsciiSPrint (Plist, sizeof (Plist),
                   "<plist version=\"1.0\"><dict><key>CFBundleIdentifier</key>"
                   "<string>org.unixnu.test.%a</string></dict></plist>", Names[Index]);
      Plist[Length++] = '\n';
      Plist[Length]   = '\0';
      Ok = (Kext->BundlePath != NULL) && (StrStr (Kext->BundlePath, Suffixes[Index]) != NULL) &&
           (Kext->PlistPath != NULL) && (Kext->InfoPlist != NULL) &&
           (Kext->InfoPlistSize == AsciiStrLen (Plist)) &&
           (CompareMem (Kext->InfoPlist, Plist, Kext->InfoPlistSize) == 0);
      if (Index == 1) {
        Ok = Ok && (Kext->Executable == NULL) && (Kext->ExecutableSize == 0) &&
             (Kext->ExecutablePath == NULL);
      } else {
        Ok = Ok && (Kext->ExecutablePath != NULL) && (Kext->Executable != NULL) &&
             (Kext->ExecutableSize == 56 + sizeof (Marker)) &&
             (CompareMem ((CONST UINT8 *)Kext->Executable + 56, Marker, sizeof (Marker)) == 0);
      }

      if (!Ok) {
        Print (L"UNIXNU-XNU-TEST: bad bundle %u plist %u expected %u code %u\n",
               (UINT32)Index, (UINT32)Kext->InfoPlistSize,
               (UINT32)AsciiStrLen (Plist), (UINT32)Kext->ExecutableSize);
      }
    }
  }

  Print (L"UNIXNU-XNU-TEST: %s handoff %s call %u\n",
         mLegacy ? L"v1" : L"v2", Ok ? L"PASS" : L"FAIL", (UINT32)++mCalls);
  return EFI_ABORTED;
}

STATIC UNIXNU_LOADER_PROTOCOL  mLoader = { UNIXNU_LOADER_REVISION, TestBoot };

EFI_STATUS
EFIAPI
XnuTestEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Loaded;
  EFI_HANDLE                 Handle;

  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded)) &&
      (Loaded->LoadOptions != NULL))
  {
    mLegacy = (StrCmp (Loaded->LoadOptions, L"v1") == 0);
    if (mLegacy) {
      mLoader.Revision = UNIXNU_LOADER_REVISION_1;
    } else if (StrCmp (Loaded->LoadOptions, L"unknown") == 0) {
      mLoader.Revision = 0x00030000;
    } else if (StrCmp (Loaded->LoadOptions, L"nullboot") == 0) {
      mLoader.Boot = NULL;
    }
  }

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (&Handle, &gUniXNULoaderProtocolGuid, &mLoader, NULL);
}
