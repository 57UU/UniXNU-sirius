/** @file
  Protocol installed by the test SampleDriver so the HelloPayload can prove
  that driver injection happened.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef UNIXNU_SAMPLE_PROTOCOL_H_
#define UNIXNU_SAMPLE_PROTOCOL_H_

#define UNIXNU_SAMPLE_PROTOCOL_GUID \
  { 0xaaf1116f, 0x7b72, 0x468c, { 0xa2, 0x9e, 0x36, 0x31, 0x73, 0x2f, 0x2b, 0x84 } }

#define UNIXNU_SAMPLE_SIGNATURE  SIGNATURE_32 ('C', 'A', 'S', 'D')

typedef struct {
  UINT32    Signature;
  CHAR16    *LoadOptions;    ///< What the driver was started with (may be NULL).
} UNIXNU_SAMPLE_PROTOCOL;

extern EFI_GUID  gUniXNUSampleProtocolGuid;

#endif
