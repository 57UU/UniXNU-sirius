/** @file
  Device path node of block devices published by `[[block_slice]]`.

  A slice handle carries EFI_BLOCK_IO_PROTOCOL and a device path made of the
  source partition's path followed by this vendor media node.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef UNIXNU_BLOCK_SLICE_H_
#define UNIXNU_BLOCK_SLICE_H_

#include <Uefi.h>
#include <Protocol/DevicePath.h>

#define UNIXNU_BLOCK_SLICE_GUID \
  { 0x63e1d76a, 0x715f, 0x4b8c, { 0x85, 0x0a, 0xe1, 0x58, 0xd1, 0x27, 0x0f, 0x9f } }

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH    Header;          ///< MEDIA_DEVICE_PATH / MEDIA_VENDOR_DP, UNIXNU_BLOCK_SLICE_GUID.
  UINT64                StartingLba;     ///< First block of the slice on the source partition.
  UINT64                NumberOfBlocks;
  CHAR16                Name[36];        ///< `name` from the configuration, NUL padded.
} UNIXNU_BLOCK_SLICE_DEVICE_PATH;
#pragma pack ()

extern EFI_GUID  gUniXNUBlockSliceGuid;

#endif
