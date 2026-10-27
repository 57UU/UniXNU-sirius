/** @file
  Interface for an external loader of `type = "xnu"` entries.

  The interface gives the Darwin-style boot path a fixed, documented seam.
  DarwinBoot implements it. Other loaders can implement the same interface.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef UNIXNU_LOADER_H_
#define UNIXNU_LOADER_H_

#include <Uefi.h>

#define UNIXNU_LOADER_PROTOCOL_GUID \
  { 0x0f0a492e, 0x5a6a, 0x49f6, { 0xa1, 0xbf, 0x74, 0x3c, 0x87, 0xab, 0x1c, 0xfe } }

#define UNIXNU_LOADER_REVISION_1  0x00010000
#define UNIXNU_LOADER_REVISION    0x00020000

typedef struct _UNIXNU_LOADER_PROTOCOL UNIXNU_LOADER_PROTOCOL;

/// Unmodified bundle files, in configured dependency order. This is staging,
/// not proof of signature validity or successful kernel loading. The loader
/// must validate metadata, dependencies, ABI and its vendor's boot policy.
typedef struct {
  CONST CHAR16  *BundlePath;
  CONST CHAR16  *PlistPath;       ///< Absolute path on the source volume.
  CONST CHAR16  *ExecutablePath; ///< NULL for a codeless kext.
  CONST VOID    *InfoPlist;
  UINTN         InfoPlistSize;
  CONST VOID    *Executable;     ///< NULL for a codeless kext.
  UINTN         ExecutableSize;
} UNIXNU_LOADER_KEXT;

///
/// Everything UniXNU resolved for the entry. Buffers belong to UniXNU
/// and stay valid for the duration of the Boot() call.
///
typedef struct {
  EFI_HANDLE    Volume;          ///< Volume the entry's paths refer to.
  CONST CHAR16  *KernelPath;     ///< Entry `path`.
  CONST CHAR16  *BootArgs;       ///< Entry `args` (may be NULL).
  VOID          *DeviceTree;     ///< Patched FDT if the entry has `dtb`, else NULL.
  UINTN         DeviceTreeSize;
  VOID          *Ramdisk;        ///< Entry `initrd` contents, or NULL.
  UINTN         RamdiskSize;
  // Revision 2 tail. Revision 1 loaders receive only requests with no kexts.
  CONST UNIXNU_LOADER_KEXT  *Kexts;
  UINTN                   KextCount;
} UNIXNU_LOADER_BOOT_REQUEST;

/**
  Boot a Darwin-style payload. Returns only on failure.
**/
typedef
EFI_STATUS
(EFIAPI *UNIXNU_LOADER_BOOT)(
  IN UNIXNU_LOADER_PROTOCOL     *This,
  IN CONST UNIXNU_LOADER_BOOT_REQUEST  *Request
  );

struct _UNIXNU_LOADER_PROTOCOL {
  UINT32                Revision;
  UNIXNU_LOADER_BOOT    Boot;
};

extern EFI_GUID  gUniXNULoaderProtocolGuid;

#endif
