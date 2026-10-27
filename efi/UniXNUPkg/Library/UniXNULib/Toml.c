/** @file
  A small TOML subset parser.

  Supported: comments, bare/quoted/dotted keys, [table], [[array.of.tables]],
  basic and literal strings, integers (decimal, 0x, 0o, 0b, '_' separators),
  booleans, arrays (multi-line, nested, trailing comma) and inline tables.
  Not supported: floats, dates, multi-line strings. Unsupported syntax is a
  parse error, never silently ignored.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Internal.h"

typedef struct {
  CONST CHAR8    *Text;
  UINTN          Length;
  UINTN          Pos;
  UINTN          Line;
  CHAR8          *Error;
  UINTN          ErrorSize;
  BOOLEAN        Failed;
} TOML_PARSER;

STATIC
VOID
Fail (
  IN TOML_PARSER  *P,
  IN CONST CHAR8  *Message
  )
{
  if (!P->Failed) {
    AsciiSPrint (P->Error, P->ErrorSize, "line %u: %a", (UINT32)P->Line, Message);
    P->Failed = TRUE;
  }
}

STATIC
CHAR8
Peek (
  IN TOML_PARSER  *P
  )
{
  return (P->Pos < P->Length) ? P->Text[P->Pos] : '\0';
}

STATIC
CHAR8
Advance (
  IN TOML_PARSER  *P
  )
{
  CHAR8  C;

  if (P->Pos >= P->Length) {
    return '\0';
  }

  C = P->Text[P->Pos++];
  if (C == '\n') {
    P->Line++;
  }

  return C;
}

STATIC
VOID
SkipSpaces (
  IN TOML_PARSER  *P
  )
{
  while (Peek (P) == ' ' || Peek (P) == '\t') {
    Advance (P);
  }
}

STATIC
VOID
SkipComment (
  IN TOML_PARSER  *P
  )
{
  if (Peek (P) == '#') {
    while (P->Pos < P->Length && Peek (P) != '\n') {
      Advance (P);
    }
  }
}

//
// Skip whitespace, newlines and comments (inside arrays).
//
STATIC
VOID
SkipBlank (
  IN TOML_PARSER  *P
  )
{
  for ( ; ;) {
    CHAR8  C = Peek (P);

    if ((C == ' ') || (C == '\t') || (C == '\r') || (C == '\n')) {
      Advance (P);
    } else if (C == '#') {
      SkipComment (P);
    } else {
      return;
    }
  }
}

//
// After a value or header only spaces and a comment may follow on the line.
//
STATIC
VOID
ExpectLineEnd (
  IN TOML_PARSER  *P
  )
{
  SkipSpaces (P);
  SkipComment (P);
  if (Peek (P) == '\r') {
    Advance (P);
  }

  if (Peek (P) == '\n') {
    Advance (P);
  } else if (P->Pos < P->Length) {
    Fail (P, "unexpected text after value");
  }
}

STATIC
TOML_NODE *
NewNode (
  IN TOML_PARSER  *P,
  IN TOML_TYPE    Type
  )
{
  TOML_NODE  *Node;

  Node = AllocateZeroPool (sizeof (TOML_NODE));
  if (Node == NULL) {
    Fail (P, "out of memory");
    return NULL;
  }

  Node->Type = Type;
  Node->Line = P->Line;
  return Node;
}

STATIC
VOID
AppendChild (
  IN TOML_NODE  *Parent,
  IN TOML_NODE  *Child
  )
{
  if (Parent->LastChild == NULL) {
    Parent->Child = Child;
  } else {
    Parent->LastChild->Next = Child;
  }

  Parent->LastChild = Child;
}

VOID
TomlFree (
  IN TOML_NODE  *Node
  )
{
  TOML_NODE  *Child;
  TOML_NODE  *Next;

  if (Node == NULL) {
    return;
  }

  for (Child = Node->Child; Child != NULL; Child = Next) {
    Next = Child->Next;
    TomlFree (Child);
  }

  if (Node->Key != NULL) {
    FreePool (Node->Key);
  }

  if (Node->String != NULL) {
    FreePool (Node->String);
  }

  FreePool (Node);
}

TOML_NODE *
TomlGet (
  IN TOML_NODE    *Table,
  IN CONST CHAR8  *Key
  )
{
  TOML_NODE  *Child;

  if ((Table == NULL) || (Table->Type != TomlTable)) {
    return NULL;
  }

  for (Child = Table->Child; Child != NULL; Child = Child->Next) {
    if ((Child->Key != NULL) && (AsciiStrCmp (Child->Key, Key) == 0)) {
      return Child;
    }
  }

  return NULL;
}

CONST CHAR8 *
TomlGetString (
  IN TOML_NODE    *Table,
  IN CONST CHAR8  *Key,
  IN CONST CHAR8  *Default
  )
{
  TOML_NODE  *Node;

  Node = TomlGet (Table, Key);
  return (Node != NULL && Node->Type == TomlString) ? Node->String : Default;
}

INT64
TomlGetInteger (
  IN TOML_NODE    *Table,
  IN CONST CHAR8  *Key,
  IN INT64        Default
  )
{
  TOML_NODE  *Node;

  Node = TomlGet (Table, Key);
  return (Node != NULL && Node->Type == TomlInteger) ? Node->Integer : Default;
}

BOOLEAN
TomlGetBoolean (
  IN TOML_NODE    *Table,
  IN CONST CHAR8  *Key,
  IN BOOLEAN      Default
  )
{
  TOML_NODE  *Node;

  Node = TomlGet (Table, Key);
  return (Node != NULL && Node->Type == TomlBoolean) ? Node->Boolean : Default;
}

UINTN
TomlCount (
  IN TOML_NODE  *Array
  )
{
  UINTN      Count;
  TOML_NODE  *Child;

  if ((Array == NULL) || (Array->Type != TomlArray)) {
    return 0;
  }

  Count = 0;
  for (Child = Array->Child; Child != NULL; Child = Child->Next) {
    Count++;
  }

  return Count;
}

STATIC
BOOLEAN
IsBareKeyChar (
  IN CHAR8  C
  )
{
  return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
         (C >= '0' && C <= '9') || C == '_' || C == '-';
}

STATIC
UINT8
HexValue (
  IN CHAR8  C
  )
{
  if ((C >= '0') && (C <= '9')) {
    return (UINT8)(C - '0');
  }

  if ((C >= 'a') && (C <= 'f')) {
    return (UINT8)(C - 'a' + 10);
  }

  if ((C >= 'A') && (C <= 'F')) {
    return (UINT8)(C - 'A' + 10);
  }

  return 0xFF;
}

//
// Parse "..." or '...' at the cursor into a new NUL-terminated buffer.
// Non-ASCII \u escapes become '?'; raw UTF-8 bytes pass through.
//
STATIC
CHAR8 *
ParseString (
  IN TOML_PARSER  *P
  )
{
  CHAR8    Quote;
  CHAR8    *Out;
  UINTN    Len;
  UINTN    Cap;
  CHAR8    C;
  UINTN    Index;
  UINT32   Code;
  UINTN    Digits;

  Quote = Advance (P);
  Cap   = 32;
  Len   = 0;
  Out   = AllocatePool (Cap);
  if (Out == NULL) {
    Fail (P, "out of memory");
    return NULL;
  }

  for ( ; ;) {
    C = Peek (P);
    if ((C == '\0') || (C == '\n')) {
      Fail (P, "unterminated string");
      FreePool (Out);
      return NULL;
    }

    Advance (P);
    if (C == Quote) {
      break;
    }

    if ((C == '\\') && (Quote == '"')) {
      C = Advance (P);
      switch (C) {
        case 'n':  C = '\n';
          break;
        case 't':  C = '\t';
          break;
        case 'r':  C = '\r';
          break;
        case '\\': C = '\\';
          break;
        case '"':  C = '"';
          break;
        case 'u':
        case 'U':
          Digits = (C == 'u') ? 4 : 8;
          Code   = 0;
          for (Index = 0; Index < Digits; Index++) {
            UINT8  V = HexValue (Advance (P));
            if (V == 0xFF) {
              Fail (P, "bad \\u escape");
              FreePool (Out);
              return NULL;
            }

            Code = (Code << 4) | V;
          }

          C = (Code < 0x80) ? (CHAR8)Code : '?';
          break;
        default:
          Fail (P, "unknown escape in string");
          FreePool (Out);
          return NULL;
      }
    }

    if (Len + 2 > Cap) {
      Out = ReallocatePool (Cap, Cap * 2, Out);
      Cap = Cap * 2;
      if (Out == NULL) {
        Fail (P, "out of memory");
        return NULL;
      }
    }

    Out[Len++] = C;
  }

  Out[Len] = '\0';
  return Out;
}

STATIC
CHAR8 *
ParseKeyPart (
  IN TOML_PARSER  *P
  )
{
  UINTN  Start;
  CHAR8  *Key;

  SkipSpaces (P);
  if ((Peek (P) == '"') || (Peek (P) == '\'')) {
    return ParseString (P);
  }

  Start = P->Pos;
  while (IsBareKeyChar (Peek (P))) {
    Advance (P);
  }

  if (P->Pos == Start) {
    Fail (P, "expected a key");
    return NULL;
  }

  Key = AllocateZeroPool (P->Pos - Start + 1);
  if (Key == NULL) {
    Fail (P, "out of memory");
    return NULL;
  }

  CopyMem (Key, P->Text + Start, P->Pos - Start);
  return Key;
}

//
// Find or create the table member Key of Table. A member that is an array of
// tables resolves to its last element (TOML semantics for [a.b] after [[a]]).
//
STATIC
TOML_NODE *
DescendTable (
  IN TOML_PARSER  *P,
  IN TOML_NODE    *Table,
  IN CHAR8        *Key
  )
{
  TOML_NODE  *Node;

  Node = TomlGet (Table, Key);
  if (Node == NULL) {
    Node = NewNode (P, TomlTable);
    if (Node == NULL) {
      FreePool (Key);
      return NULL;
    }

    Node->Key = Key;
    AppendChild (Table, Node);
    return Node;
  }

  FreePool (Key);
  if (Node->Type == TomlTable) {
    return Node;
  }

  if ((Node->Type == TomlArray) && Node->TableArray && (Node->LastChild != NULL)) {
    return Node->LastChild;
  }

  Fail (P, "key already holds a value");
  return NULL;
}

STATIC
TOML_NODE *
ParseValue (
  IN TOML_PARSER  *P
  );

STATIC
BOOLEAN
ParseKeyValue (
  IN TOML_PARSER  *P,
  IN TOML_NODE    *Table
  );

STATIC
TOML_NODE *
ParseInteger (
  IN TOML_PARSER  *P
  )
{
  TOML_NODE  *Node;
  BOOLEAN    Negative;
  UINT64     Value;
  UINTN      Base;
  UINTN      Digits;
  UINT8      D;
  CHAR8      C;

  Negative = FALSE;
  if ((Peek (P) == '+') || (Peek (P) == '-')) {
    Negative = (Advance (P) == '-');
  }

  Base = 10;
  if ((Peek (P) == '0') && (P->Pos + 1 < P->Length)) {
    C = P->Text[P->Pos + 1];
    if ((C == 'x') || (C == 'o') || (C == 'b')) {
      Base = (C == 'x') ? 16 : ((C == 'o') ? 8 : 2);
      Advance (P);
      Advance (P);
    }
  }

  Value  = 0;
  Digits = 0;
  for ( ; ;) {
    C = Peek (P);
    if ((C == '_') && (Digits > 0)) {
      Advance (P);
      continue;
    }

    D = HexValue (C);
    if ((D == 0xFF) || (D >= Base)) {
      break;
    }

    Value = Value * Base + D;
    Digits++;
    Advance (P);
  }

  if ((Digits == 0) || (Peek (P) == '.') || (Peek (P) == 'e') || (Peek (P) == 'E')) {
    Fail (P, "invalid number (floats are not supported)");
    return NULL;
  }

  Node = NewNode (P, TomlInteger);
  if (Node != NULL) {
    Node->Integer = Negative ? -(INT64)Value : (INT64)Value;
  }

  return Node;
}

STATIC
TOML_NODE *
ParseArray (
  IN TOML_PARSER  *P
  )
{
  TOML_NODE  *Array;
  TOML_NODE  *Item;

  Advance (P);
  Array = NewNode (P, TomlArray);
  if (Array == NULL) {
    return NULL;
  }

  for ( ; ;) {
    SkipBlank (P);
    if (Peek (P) == ']') {
      Advance (P);
      return Array;
    }

    Item = ParseValue (P);
    if (Item == NULL) {
      TomlFree (Array);
      return NULL;
    }

    AppendChild (Array, Item);
    SkipBlank (P);
    if (Peek (P) == ',') {
      Advance (P);
    } else if (Peek (P) != ']') {
      Fail (P, "expected ',' or ']' in array");
      TomlFree (Array);
      return NULL;
    }
  }
}

STATIC
TOML_NODE *
ParseInlineTable (
  IN TOML_PARSER  *P
  )
{
  TOML_NODE  *Table;

  Advance (P);
  Table = NewNode (P, TomlTable);
  if (Table == NULL) {
    return NULL;
  }

  Table->Defined = TRUE;
  SkipSpaces (P);
  if (Peek (P) == '}') {
    Advance (P);
    return Table;
  }

  for ( ; ;) {
    if (!ParseKeyValue (P, Table)) {
      TomlFree (Table);
      return NULL;
    }

    SkipSpaces (P);
    if (Peek (P) == ',') {
      Advance (P);
      continue;
    }

    if (Peek (P) == '}') {
      Advance (P);
      return Table;
    }

    Fail (P, "expected ',' or '}' in inline table");
    TomlFree (Table);
    return NULL;
  }
}

STATIC
TOML_NODE *
ParseValue (
  IN TOML_PARSER  *P
  )
{
  TOML_NODE  *Node;
  CHAR8      C;

  SkipSpaces (P);
  C = Peek (P);
  if ((C == '"') || (C == '\'')) {
    if ((P->Pos + 2 < P->Length) && (P->Text[P->Pos + 1] == C) && (P->Text[P->Pos + 2] == C)) {
      Fail (P, "multi-line strings are not supported");
      return NULL;
    }

    Node = NewNode (P, TomlString);
    if (Node != NULL) {
      Node->String = ParseString (P);
      if (Node->String == NULL) {
        FreePool (Node);
        return NULL;
      }
    }

    return Node;
  }

  if (C == '[') {
    return ParseArray (P);
  }

  if (C == '{') {
    return ParseInlineTable (P);
  }

  if ((P->Length - P->Pos >= 4) && (AsciiStrnCmp (P->Text + P->Pos, "true", 4) == 0)) {
    P->Pos += 4;
    Node    = NewNode (P, TomlBoolean);
    if (Node != NULL) {
      Node->Boolean = TRUE;
    }

    return Node;
  }

  if ((P->Length - P->Pos >= 5) && (AsciiStrnCmp (P->Text + P->Pos, "false", 5) == 0)) {
    P->Pos += 5;
    return NewNode (P, TomlBoolean);
  }

  if (((C >= '0') && (C <= '9')) || (C == '+') || (C == '-')) {
    return ParseInteger (P);
  }

  Fail (P, "expected a value");
  return NULL;
}

//
// key = value, with dotted keys creating intermediate tables.
//
STATIC
BOOLEAN
ParseKeyValue (
  IN TOML_PARSER  *P,
  IN TOML_NODE    *Table
  )
{
  CHAR8      *Key;
  TOML_NODE  *Value;

  Key = ParseKeyPart (P);
  if (Key == NULL) {
    return FALSE;
  }

  SkipSpaces (P);
  while (Peek (P) == '.') {
    Advance (P);
    Table = DescendTable (P, Table, Key);
    if (Table == NULL) {
      return FALSE;
    }

    Key = ParseKeyPart (P);
    if (Key == NULL) {
      return FALSE;
    }

    SkipSpaces (P);
  }

  if (Peek (P) != '=') {
    Fail (P, "expected '='");
    FreePool (Key);
    return FALSE;
  }

  Advance (P);
  if (TomlGet (Table, Key) != NULL) {
    Fail (P, "duplicate key");
    FreePool (Key);
    return FALSE;
  }

  Value = ParseValue (P);
  if (Value == NULL) {
    FreePool (Key);
    return FALSE;
  }

  Value->Key = Key;
  AppendChild (Table, Value);
  return TRUE;
}

//
// [a.b.c] or [[a.b.c]]; returns the table that following keys go into.
//
STATIC
TOML_NODE *
ParseHeader (
  IN TOML_PARSER  *P,
  IN TOML_NODE    *Root
  )
{
  BOOLEAN    IsArray;
  TOML_NODE  *Table;
  TOML_NODE  *Node;
  CHAR8      *Key;

  Advance (P);
  IsArray = (Peek (P) == '[');
  if (IsArray) {
    Advance (P);
  }

  Table = Root;
  for ( ; ;) {
    Key = ParseKeyPart (P);
    if (Key == NULL) {
      return NULL;
    }

    SkipSpaces (P);
    if (Peek (P) == '.') {
      Advance (P);
      Table = DescendTable (P, Table, Key);
      if (Table == NULL) {
        return NULL;
      }

      continue;
    }

    break;
  }

  if (Peek (P) != ']') {
    Fail (P, "expected ']'");
    FreePool (Key);
    return NULL;
  }

  Advance (P);
  if (IsArray) {
    if (Peek (P) != ']') {
      Fail (P, "expected ']]'");
      FreePool (Key);
      return NULL;
    }

    Advance (P);
  }

  ExpectLineEnd (P);
  if (P->Failed) {
    FreePool (Key);
    return NULL;
  }

  Node = TomlGet (Table, Key);
  if (IsArray) {
    if (Node == NULL) {
      Node = NewNode (P, TomlArray);
      if (Node == NULL) {
        FreePool (Key);
        return NULL;
      }

      Node->Key        = Key;
      Node->TableArray = TRUE;
      AppendChild (Table, Node);
    } else {
      FreePool (Key);
      if ((Node->Type != TomlArray) || !Node->TableArray) {
        Fail (P, "[[header]] reuses a non-array key");
        return NULL;
      }
    }

    Table = NewNode (P, TomlTable);
    if (Table == NULL) {
      return NULL;
    }

    Table->Defined = TRUE;
    AppendChild (Node, Table);
    return Table;
  }

  if (Node == NULL) {
    Node = NewNode (P, TomlTable);
    if (Node == NULL) {
      FreePool (Key);
      return NULL;
    }

    Node->Key = Key;
    AppendChild (Table, Node);
  } else {
    FreePool (Key);
    if ((Node->Type != TomlTable) || Node->Defined) {
      Fail (P, "table defined twice");
      return NULL;
    }
  }

  Node->Defined = TRUE;
  return Node;
}

EFI_STATUS
TomlParse (
  IN  CONST CHAR8  *Text,
  IN  UINTN        Length,
  OUT TOML_NODE    **Root,
  OUT CHAR8        *ErrorBuffer,
  IN  UINTN        ErrorBufferSize
  )
{
  TOML_PARSER  P;
  TOML_NODE    *Current;

  ZeroMem (&P, sizeof (P));
  P.Text      = Text;
  P.Length    = Length;
  P.Line      = 1;
  P.Error     = ErrorBuffer;
  P.ErrorSize = ErrorBufferSize;
  ErrorBuffer[0] = '\0';

  //
  // Skip a UTF-8 byte order mark.
  //
  if ((Length >= 3) && ((UINT8)Text[0] == 0xEF) && ((UINT8)Text[1] == 0xBB) && ((UINT8)Text[2] == 0xBF)) {
    P.Pos = 3;
  }

  *Root = NewNode (&P, TomlTable);
  if (*Root == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Current = *Root;
  while (!P.Failed && P.Pos < P.Length) {
    SkipBlank (&P);
    if (P.Pos >= P.Length) {
      break;
    }

    if (Peek (&P) == '[') {
      Current = ParseHeader (&P, *Root);
    } else if (ParseKeyValue (&P, Current)) {
      ExpectLineEnd (&P);
    }
  }

  if (P.Failed) {
    TomlFree (*Root);
    *Root = NULL;
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}
