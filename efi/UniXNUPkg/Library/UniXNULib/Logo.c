/** @file
  UniXNU logo: the vector strokes of Docs/Images/unixnu-logo.svg (flattened
  into LogoShape.h by Scripts/gen-logo.py), rasterized at any size with
  integer math (no floating point in firmware) and 4x4 supersampling.

  Each stroke is a round-capped, round-joined polyline. Every segment marks
  the subsamples within its half width in a coverage bitmap, so overlapping
  segments never darken or brighten twice; the bitmap is then averaged into
  pixels blended from the background to the foreground.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Internal.h"
#include "LogoShape.h"

#define SS   4
#define FIX  16

STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mInk = { 0xFF, 0xFF, 0xFF, 0 };

//
// Whether (X,Y) lies within HalfWidth of segment (X0,Y0)-(X1,Y1), all in
// the same fixed-point units. A zero-length segment is a dot.
//
STATIC
BOOLEAN
NearSegment (
  IN INT64  X,
  IN INT64  Y,
  IN INT64  X0,
  IN INT64  Y0,
  IN INT64  X1,
  IN INT64  Y1,
  IN INT64  HalfWidth
  )
{
  INT64  Dx  = X1 - X0;
  INT64  Dy  = Y1 - Y0;
  INT64  Len = Dx * Dx + Dy * Dy;
  INT64  T;
  INT64  Px;
  INT64  Py;

  if (Len == 0) {
    Px = X - X0;
    Py = Y - Y0;
    return Px * Px + Py * Py <= HalfWidth * HalfWidth;
  }

  T = (X - X0) * Dx + (Y - Y0) * Dy;
  if (T < 0) {
    T = 0;
  } else if (T > Len) {
    T = Len;
  }

  //
  // Closest point = P0 + d * T / Len.
  //
  Px = X0 - X + DivS64x64Remainder (Dx * T, Len, NULL);
  Py = Y0 - Y + DivS64x64Remainder (Dy * T, Len, NULL);
  return Px * Px + Py * Py <= HalfWidth * HalfWidth;
}

//
// Mark every subsample of the Sub x Sub bitmap within HalfWidth of the
// segment. Coordinates are fixed point, FIX units per subsample.
//
STATIC
VOID
MarkSegment (
  IN OUT UINT8  *Mask,
  IN     INT64  Sub,
  IN     INT64  X0,
  IN     INT64  Y0,
  IN     INT64  X1,
  IN     INT64  Y1,
  IN     INT64  HalfWidth
  )
{
  INT64  Left;
  INT64  Right;
  INT64  Top;
  INT64  Bottom;
  INT64  Sx;
  INT64  Sy;
  INT64  Bit;

  Left   = MAX (0, (MIN (X0, X1) - HalfWidth) / FIX);
  Right  = MIN (Sub - 1, (MAX (X0, X1) + HalfWidth) / FIX);
  Top    = MAX (0, (MIN (Y0, Y1) - HalfWidth) / FIX);
  Bottom = MIN (Sub - 1, (MAX (Y0, Y1) + HalfWidth) / FIX);
  for (Sy = Top; Sy <= Bottom; Sy++) {
    for (Sx = Left; Sx <= Right; Sx++) {
      if (NearSegment (Sx * FIX + FIX / 2, Sy * FIX + FIX / 2, X0, Y0, X1, Y1, HalfWidth)) {
        Bit            = Sy * Sub + Sx;
        Mask[Bit / 8] |= (UINT8)(1 << (Bit % 8));
      }
    }
  }
}

VOID
UniXNULogoRender (
  OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Buffer,
  IN  UINTN                          Size,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Background
  )
{
  INT64   Sub;
  INT64   Unit;
  UINT8   *Mask;
  UINTN   Stroke;
  UINTN   Index;
  UINTN   Px;
  UINTN   Py;
  UINTN   Sx;
  UINTN   Sy;
  UINTN   Bit;
  UINT32  Cover;
  INT64   HalfWidth;
  CONST INT16  *P;
  CONST INT16  *Q;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Out;

  for (Index = 0; Index < Size * Size; Index++) {
    Buffer[Index] = Background;
  }

  Sub  = (INT64)(Size * SS);
  Unit = Sub * FIX;
  Mask = AllocateZeroPool ((UINTN)((Sub * Sub + 7) / 8));
  if (Mask == NULL) {
    return;
  }

  for (Stroke = 0; Stroke < ARRAY_SIZE (mLogoStrokes); Stroke++) {
    HalfWidth = MAX (FIX / 2, mLogoStrokes[Stroke][2] * Unit / LOGO_DESIGN);
    for (Index = 0; Index < mLogoStrokes[Stroke][1]; Index++) {
      P = mLogoPoints[mLogoStrokes[Stroke][0] + Index];
      Q = mLogoPoints[mLogoStrokes[Stroke][0] + MIN (Index + 1, mLogoStrokes[Stroke][1] - 1u)];
      if ((Index > 0) && (P == Q)) {
        break;
      }

      MarkSegment (
        Mask,
        Sub,
        P[0] * Unit / LOGO_DESIGN,
        P[1] * Unit / LOGO_DESIGN,
        Q[0] * Unit / LOGO_DESIGN,
        Q[1] * Unit / LOGO_DESIGN,
        HalfWidth
        );
    }
  }

  for (Py = 0; Py < Size; Py++) {
    for (Px = 0; Px < Size; Px++) {
      Cover = 0;
      for (Sy = 0; Sy < SS; Sy++) {
        for (Sx = 0; Sx < SS; Sx++) {
          Bit    = (Py * SS + Sy) * (UINTN)Sub + Px * SS + Sx;
          Cover += (Mask[Bit / 8] >> (Bit % 8)) & 1;
        }
      }

      if (Cover == 0) {
        continue;
      }

      Out        = &Buffer[Py * Size + Px];
      Out->Red   = (UINT8)((Background.Red * (SS * SS - Cover) + mInk.Red * Cover) / (SS * SS));
      Out->Green = (UINT8)((Background.Green * (SS * SS - Cover) + mInk.Green * Cover) / (SS * SS));
      Out->Blue  = (UINT8)((Background.Blue * (SS * SS - Cover) + mInk.Blue * Cover) / (SS * SS));
    }
  }

  FreePool (Mask);
}
