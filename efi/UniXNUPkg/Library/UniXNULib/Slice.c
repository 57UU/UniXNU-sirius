/** @file
  Block slices: publish a byte range of a GPT partition as its own block
  device (EFI_BLOCK_IO_PROTOCOL), located by partition name through
  EFI_PARTITION_INFO_PROTOCOL.

  Typical use: a partition that starts with a small FAT volume (found by the
  firmware's FAT driver directly, since a FAT boot sector carries its own
  size) and whose remainder should reach the payload as a separate disk.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Internal.h"

#include <Guid/UniXNUBlockSlice.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>

#define SLICE_SIGNATURE  SIGNATURE_32 ('C', 'A', 'S', 'L')

typedef struct {
  UINT32                    Signature;
  EFI_BLOCK_IO_PROTOCOL     BlockIo;
  EFI_BLOCK_IO_MEDIA        Media;
  EFI_BLOCK_IO_PROTOCOL     *Parent;
  EFI_LBA                   Start;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_HANDLE                Handle;
} SLICE_DEVICE;

#define SLICE_FROM_BLOCK_IO(a)  CR (a, SLICE_DEVICE, BlockIo, SLICE_SIGNATURE)

STATIC
EFI_STATUS
EFIAPI
SliceReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CheckRange (
  IN SLICE_DEVICE  *Slice,
  IN UINT32        MediaId,
  IN EFI_LBA       Lba,
  IN UINTN         BufferSize,
  IN VOID          *Buffer
  )
{
  UINTN  Blocks;

  if (MediaId != Slice->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((BufferSize % Slice->Media.BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Blocks = BufferSize / Slice->Media.BlockSize;
  if ((Lba > Slice->Media.LastBlock) || (Blocks > Slice->Media.LastBlock - Lba + 1)) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SliceReadBlocks (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  SLICE_DEVICE  *Slice;
  EFI_STATUS    Status;

  Slice = SLICE_FROM_BLOCK_IO (This);
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  Status = CheckRange (Slice, MediaId, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Slice->Parent->ReadBlocks (Slice->Parent, Slice->Parent->Media->MediaId, Slice->Start + Lba, BufferSize, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
SliceWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  SLICE_DEVICE  *Slice;
  EFI_STATUS    Status;

  Slice = SLICE_FROM_BLOCK_IO (This);
  if (Slice->Media.ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }

  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  Status = CheckRange (Slice, MediaId, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Slice->Parent->WriteBlocks (Slice->Parent, Slice->Parent->Media->MediaId, Slice->Start + Lba, BufferSize, Buffer);
}

STATIC
EFI_STATUS
EFIAPI
SliceFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  SLICE_DEVICE  *Slice;

  Slice = SLICE_FROM_BLOCK_IO (This);
  return Slice->Media.ReadOnly ? EFI_SUCCESS : Slice->Parent->FlushBlocks (Slice->Parent);
}

//
// The handle of the GPT partition whose name is Name, or NULL.
//
STATIC
EFI_HANDLE
FindPartition (
  IN CONST CHAR16  *Name
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   *Handles;
  UINTN                        Count;
  UINTN                        Index;
  EFI_PARTITION_INFO_PROTOCOL  *Info;
  EFI_HANDLE                   Found;
  CHAR16                       Entry[37];

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  Found = NULL;
  for (Index = 0; Index < Count && Found == NULL; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (EFI_ERROR (Status) || (Info->Type != PARTITION_TYPE_GPT)) {
      continue;
    }

    //
    // The GPT name field is not NUL-terminated when all 36 characters are used.
    //
    CopyMem (Entry, Info->Info.Gpt.PartitionName, sizeof (Info->Info.Gpt.PartitionName));
    Entry[36] = L'\0';
    if (StrCmp (Entry, Name) == 0) {
      Found = Handles[Index];
    }
  }

  FreePool (Handles);
  return Found;
}

STATIC
EFI_STATUS
PublishSlice (
  IN UNIXNU_BLOCK_SLICE  *Config
  )
{
  EFI_STATUS                         Status;
  EFI_HANDLE                         Partition;
  EFI_BLOCK_IO_PROTOCOL              *Parent;
  SLICE_DEVICE                       *Slice;
  UINT64                             TotalBlocks;
  UINT64                             StartBlock;
  UINT64                             Blocks;
  UNIXNU_BLOCK_SLICE_DEVICE_PATH  Node;
  UINTN                              NameLen;

  Partition = FindPartition (Config->Partition);
  if (Partition == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->HandleProtocol (Partition, &gEfiBlockIoProtocolGuid, (VOID **)&Parent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Byte offsets must fall on block boundaries of the partition.
  //
  if (((Config->Offset % Parent->Media->BlockSize) != 0) || ((Config->Length % Parent->Media->BlockSize) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  TotalBlocks = Parent->Media->LastBlock + 1;
  StartBlock  = DivU64x32 (Config->Offset, Parent->Media->BlockSize);
  if (StartBlock >= TotalBlocks) {
    return EFI_INVALID_PARAMETER;
  }

  Blocks = (Config->Length == 0) ? TotalBlocks - StartBlock : DivU64x32 (Config->Length, Parent->Media->BlockSize);
  if (Blocks > TotalBlocks - StartBlock) {
    return EFI_INVALID_PARAMETER;
  }

  Slice = AllocateZeroPool (sizeof (SLICE_DEVICE));
  if (Slice == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Slice->Signature = SLICE_SIGNATURE;
  Slice->Parent    = Parent;
  Slice->Start     = StartBlock;
  CopyMem (&Slice->Media, Parent->Media, sizeof (EFI_BLOCK_IO_MEDIA));
  Slice->Media.LastBlock        = Blocks - 1;
  Slice->Media.ReadOnly         = !Config->Writable || Parent->Media->ReadOnly;
  //
  // Present the slice as a whole disk: the payload may put its own
  // partition table or container on it.
  //
  Slice->Media.LogicalPartition = FALSE;
  Slice->Media.MediaPresent     = TRUE;

  Slice->BlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
  Slice->BlockIo.Media       = &Slice->Media;
  Slice->BlockIo.Reset       = SliceReset;
  Slice->BlockIo.ReadBlocks  = SliceReadBlocks;
  Slice->BlockIo.WriteBlocks = SliceWriteBlocks;
  Slice->BlockIo.FlushBlocks = SliceFlushBlocks;

  ZeroMem (&Node, sizeof (Node));
  Node.Header.Header.Type    = MEDIA_DEVICE_PATH;
  Node.Header.Header.SubType = MEDIA_VENDOR_DP;
  SetDevicePathNodeLength (&Node.Header.Header, sizeof (Node));
  CopyGuid (&Node.Header.Guid, &gUniXNUBlockSliceGuid);
  Node.StartingLba    = StartBlock;
  Node.NumberOfBlocks = Blocks;
  NameLen             = MIN (StrLen (Config->Name), ARRAY_SIZE (Node.Name) - 1);
  CopyMem (Node.Name, Config->Name, NameLen * sizeof (CHAR16));

  Slice->DevicePath = AppendDevicePathNode (DevicePathFromHandle (Partition), (EFI_DEVICE_PATH_PROTOCOL *)&Node);
  if (Slice->DevicePath == NULL) {
    FreePool (Slice);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Slice->Handle,
                  &gEfiDevicePathProtocolGuid,
                  Slice->DevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &Slice->BlockIo,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Slice->DevicePath);
    FreePool (Slice);
    return Status;
  }

  UniXNULog (
    "slice %s: partition '%s' blocks %lu+%lu (%u-byte), %a",
    Config->Name,
    Config->Partition,
    StartBlock,
    Blocks,
    Parent->Media->BlockSize,
    Slice->Media.ReadOnly ? "read-only" : "writable"
    );

  //
  // Let partition and file system drivers look at it, as for any new disk.
  //
  gBS->ConnectController (Slice->Handle, NULL, NULL, TRUE);
  return EFI_SUCCESS;
}

VOID
UniXNUPublishSlices (
  IN UNIXNU_CONFIG  *Config
  )
{
  UINTN       Index;
  EFI_STATUS  Status;

  for (Index = 0; Index < Config->SliceCount; Index++) {
    Status = PublishSlice (&Config->Slices[Index]);
    if (EFI_ERROR (Status)) {
      UniXNULog ("slice %s: partition '%s': %r", Config->Slices[Index].Name, Config->Slices[Index].Partition, Status);
    }
  }
}
