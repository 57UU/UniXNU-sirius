/** @file
  Test driver for UniXNU injection: installs UNIXNU_SAMPLE_PROTOCOL
  carrying the load options it was started with.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/UniXNUSample.h>
#include <Protocol/LoadedImage.h>

STATIC UNIXNU_SAMPLE_PROTOCOL  mSample = { UNIXNU_SAMPLE_SIGNATURE, NULL };

EFI_STATUS
EFIAPI
SampleDriverEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Loaded;
  EFI_HANDLE                 Handle;

  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded)) &&
      (Loaded->LoadOptions != NULL) && (Loaded->LoadOptionsSize >= sizeof (CHAR16)))
  {
    mSample.LoadOptions = AllocateCopyPool (Loaded->LoadOptionsSize, Loaded->LoadOptions);
  }

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (&Handle, &gUniXNUSampleProtocolGuid, &mSample, NULL);
}
