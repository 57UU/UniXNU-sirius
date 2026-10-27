/** @file
  File access through EFI_SIMPLE_FILE_SYSTEM_PROTOCOL and image path
  resolution. Read-only by design: UniXNU never opens a file for writing.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include "Internal.h"

#include <Guid/FileInfo.h>
#include <Protocol/FirmwareVolume2.h>

CHAR16 *
UniXNUAsciiToUnicode (
  IN CONST CHAR8  *String
  )
{
  CHAR16  *Out;
  UINTN   Len;
  UINTN   Index;

  if (String == NULL) {
    return NULL;
  }

  Len = AsciiStrLen (String);
  Out = AllocatePool ((Len + 1) * sizeof (CHAR16));
  if (Out == NULL) {
    return NULL;
  }

  for (Index = 0; Index < Len; Index++) {
    //
    // UTF-8 continuation/lead bytes cannot be shown by the built-in font.
    //
    Out[Index] = ((UINT8)String[Index] < 0x80) ? (CHAR16)String[Index] : L'?';
  }

  Out[Len] = L'\0';
  return Out;
}

INTN
UniXNUStriCmp (
  IN CONST CHAR16  *A,
  IN CONST CHAR16  *B
  )
{
  CHAR16  Ca;
  CHAR16  Cb;

  do {
    Ca = *A++;
    Cb = *B++;
    if ((Ca >= L'a') && (Ca <= L'z')) {
      Ca = (CHAR16)(Ca - L'a' + L'A');
    }

    if ((Cb >= L'a') && (Cb <= L'z')) {
      Cb = (CHAR16)(Cb - L'a' + L'A');
    }
  } while (Ca != L'\0' && Ca == Cb);

  return (INTN)Ca - (INTN)Cb;
}

CHAR16 *
UniXNUNormalizePath (
  IN CONST CHAR16  *Base OPTIONAL,
  IN CONST CHAR8   *Path
  )
{
  UINTN    BaseLen;
  UINTN    PathLen;
  UINTN    Size;
  CHAR16   *Out;
  UINTN    O;
  UINTN    Index;
  BOOLEAN  Absolute;
  CHAR8    C;

  if (Path == NULL) {
    return NULL;
  }

  Absolute = (Path[0] == '\\' || Path[0] == '/');
  BaseLen  = (Base != NULL && !Absolute) ? StrLen (Base) : 0;
  PathLen  = AsciiStrLen (Path);
  Size     = (BaseLen + PathLen + 3) * sizeof (CHAR16);
  Out      = AllocateZeroPool (Size);
  if (Out == NULL) {
    return NULL;
  }

  O = 0;
  if (BaseLen > 0) {
    CopyMem (Out, Base, BaseLen * sizeof (CHAR16));
    O = BaseLen;
    if (Out[O - 1] != L'\\') {
      Out[O++] = L'\\';
    }
  } else {
    Out[O++] = L'\\';
  }

  for (Index = 0; Index < PathLen; Index++) {
    C = (Path[Index] == '/') ? '\\' : Path[Index];
    //
    // Collapse separators so "a//b" and a leading separator do not double up.
    //
    if ((C == '\\') && (O > 0) && (Out[O - 1] == L'\\')) {
      continue;
    }

    Out[O++] = ((UINT8)C < 0x80) ? (CHAR16)C : L'_';
  }

  Out[O] = L'\0';
  return Out;
}

STATIC
EFI_STATUS
OpenRoot (
  IN  EFI_HANDLE         Volume,
  OUT EFI_FILE_PROTOCOL  **Root
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;

  if (Volume == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->HandleProtocol (Volume, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Fs->OpenVolume (Fs, Root);
}

STATIC
EFI_STATUS
OpenPath (
  IN  EFI_HANDLE         Volume,
  IN  CONST CHAR16       *Path,
  OUT EFI_FILE_PROTOCOL  **File
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;

  Status = OpenRoot (Volume, &Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Root->Open (Root, File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  return Status;
}

STATIC
EFI_FILE_INFO *
GetInfo (
  IN EFI_FILE_PROTOCOL  *File
  )
{
  EFI_STATUS     Status;
  UINTN          Size;
  EFI_FILE_INFO  *Info;

  Size   = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &Size, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return NULL;
  }

  Info = AllocatePool (Size);
  if (Info == NULL) {
    return NULL;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &Size, Info);
  if (EFI_ERROR (Status)) {
    FreePool (Info);
    return NULL;
  }

  return Info;
}

EFI_STATUS
UniXNUReadFile (
  IN  EFI_HANDLE    Volume,
  IN  CONST CHAR16  *Path,
  OUT VOID          **Buffer,
  OUT UINTN         *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              ReadSize;
  VOID               *Data;

  *Buffer = NULL;
  *Size   = 0;

  Status = OpenPath (Volume, Path, &File);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Info = GetInfo (File);
  if (Info == NULL) {
    File->Close (File);
    return EFI_DEVICE_ERROR;
  }

  if ((Info->Attribute & EFI_FILE_DIRECTORY) != 0) {
    FreePool (Info);
    File->Close (File);
    return EFI_NOT_FOUND;
  }

  ReadSize = (UINTN)Info->FileSize;
  FreePool (Info);

  //
  // One spare zero byte so text files can be treated as C strings.
  //
  Data = AllocateZeroPool (ReadSize + 1);
  if (Data == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->Read (File, &ReadSize, Data);
  File->Close (File);
  if (EFI_ERROR (Status)) {
    FreePool (Data);
    return Status;
  }

  *Buffer = Data;
  *Size   = ReadSize;
  return EFI_SUCCESS;
}

BOOLEAN
UniXNUFileExists (
  IN EFI_HANDLE    Volume,
  IN CONST CHAR16  *Path
  )
{
  EFI_FILE_PROTOCOL  *File;

  if (EFI_ERROR (OpenPath (Volume, Path, &File))) {
    return FALSE;
  }

  File->Close (File);
  return TRUE;
}

EFI_STATUS
UniXNUListDirectory (
  IN  EFI_HANDLE    Volume,
  IN  CONST CHAR16  *Path,
  OUT CHAR16        ***Names,
  OUT UINTN         *Count
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Dir;
  UINT8              Buffer[SIZE_OF_EFI_FILE_INFO + 512];
  EFI_FILE_INFO      *Info;
  UINTN              Size;
  CHAR16             **List;
  UINTN              Used;
  UINTN              Cap;

  *Names = NULL;
  *Count = 0;

  Status = OpenPath (Volume, Path, &Dir);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cap  = 16;
  Used = 0;
  List = AllocateZeroPool (Cap * sizeof (CHAR16 *));
  if (List == NULL) {
    Dir->Close (Dir);
    return EFI_OUT_OF_RESOURCES;
  }

  Info = (EFI_FILE_INFO *)Buffer;
  for ( ; ;) {
    Size   = sizeof (Buffer);
    Status = Dir->Read (Dir, &Size, Info);
    if (EFI_ERROR (Status) || (Size == 0)) {
      break;
    }

    if ((StrCmp (Info->FileName, L".") == 0) || (StrCmp (Info->FileName, L"..") == 0)) {
      continue;
    }

    if (Used == Cap) {
      List = ReallocatePool (Cap * sizeof (CHAR16 *), Cap * 2 * sizeof (CHAR16 *), List);
      Cap *= 2;
      if (List == NULL) {
        Dir->Close (Dir);
        return EFI_OUT_OF_RESOURCES;
      }
    }

    List[Used++] = AllocateCopyPool (StrSize (Info->FileName), Info->FileName);
  }

  Dir->Close (Dir);
  *Names = List;
  *Count = Used;
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
ParseGuid16 (
  IN  CONST CHAR16  *Text,
  OUT EFI_GUID      *Guid
  )
{
  CHAR8  Ascii[37];
  UINTN  Index;

  if (StrLen (Text) != 36) {
    return FALSE;
  }

  for (Index = 0; Index < 36; Index++) {
    Ascii[Index] = (CHAR8)Text[Index];
  }

  Ascii[36] = '\0';
  return !RETURN_ERROR (AsciiStrToGuid (Ascii, Guid));
}

//
// "fv:<GUID>": the first firmware volume containing that file.
//
STATIC
EFI_DEVICE_PATH_PROTOCOL *
ResolveFvFile (
  IN CONST CHAR16  *GuidText
  )
{
  EFI_STATUS                         Status;
  EFI_GUID                           FileGuid;
  EFI_HANDLE                         *Handles;
  UINTN                              Count;
  UINTN                              Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  EFI_FV_FILETYPE                    Type;
  EFI_FV_FILE_ATTRIBUTES             Attributes;
  UINT32                             AuthStatus;
  UINTN                              Size;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  Node;
  EFI_DEVICE_PATH_PROTOCOL           *Result;

  if (!ParseGuid16 (GuidText, &FileGuid)) {
    return NULL;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiFirmwareVolume2ProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  Result = NULL;
  for (Index = 0; Index < Count && Result == NULL; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fv->ReadFile (Fv, &FileGuid, NULL, &Size, &Type, &Attributes, &AuthStatus);
    if (EFI_ERROR (Status)) {
      continue;
    }

    EfiInitializeFwVolDevicepathNode (&Node, &FileGuid);
    Result = AppendDevicePathNode (DevicePathFromHandle (Handles[Index]), (EFI_DEVICE_PATH_PROTOCOL *)&Node);
  }

  FreePool (Handles);
  return Result;
}

EFI_DEVICE_PATH_PROTOCOL *
UniXNUResolveImagePath (
  IN EFI_HANDLE    Volume,
  IN CONST CHAR16  *Path
  )
{
  if (Path == NULL) {
    return NULL;
  }

  //
  // Normalized paths start with a separator; "fv:" survives as "\fv:".
  //
  if (StrnCmp (Path, L"\\fv:", 4) == 0) {
    return ResolveFvFile (Path + 4);
  }

  if (Volume == NULL) {
    return NULL;
  }

  return FileDevicePath (Volume, Path);
}
