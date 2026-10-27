#!/usr/bin/env python3
"""Generate synthetic kext staging smoke fixtures. No Apple files are used."""

# SPDX-License-Identifier: BSD-2-Clause-Patent

import argparse
import shutil
import struct
from pathlib import Path


def bundle(root: Path, name: str, code: bool = True) -> Path:
    path = root / f"{name}.kext" / "Contents"
    path.mkdir(parents=True, exist_ok=True)
    (path / "Info.plist").write_text(
        '<plist version="1.0"><dict><key>CFBundleIdentifier</key>'
        f'<string>org.unixnu.test.{name}</string></dict></plist>\n'
    )
    executable = path / "MacOS" / "Test"
    if code:
        executable.parent.mkdir(exist_ok=True)
        executable.write_bytes(
            struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 2, 11, 1, 24, 0, 0)
            + struct.pack("<II16s", 0x1B, 24, b"synthetic-uuid")
            + b"TEST-KEXT-CODE\0"
        )
    return executable


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path)
    parser.add_argument("binaries", type=Path)
    args = parser.parse_args()
    variants = (
        "v2", "v1", "v1-empty", "missing-loader", "unknown", "nullboot",
        "missing-file", "bad-image", "truncated-command", "empty-plist",
        "duplicate", "bad-path", "bad-type", "wrong-entry",
    )
    for variant in variants:
        esp = args.out / variant
        ca = esp / "EFI" / "UniXNU"
        ca.mkdir(parents=True, exist_ok=True)
        boot = esp / "EFI" / "BOOT"
        boot.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.binaries / "UniXNU.efi", boot / "BOOTAA64.EFI")
        shutil.copy2(args.binaries / "XnuTest.efi", ca / "XnuTest.efi")
        code = bundle(ca / "Kexts", "Code")
        bundle(ca / "Kexts", "Codeless", False)
        kit = ca / "Kits" / "Test.kit"
        bundle(kit, "Kit")
        # This missing bundle must stay scoped to an unselected entry.
        (kit / "Kit.toml").write_text(
            'schema = 1\n[kit]\nname = "KextTest"\nversion = "1.0"\n'
            '[[kext]]\nentry = "other"\nbundle = "Absent.kext"\n'
            '[[kext]]\nentry = "xnu"\nbundle = "Kit.kext"\n'
            'executable = "Contents/MacOS/Test"\n'
        )
        mode = variant if variant in {"v1", "unknown", "nullboot"} else "v2"
        if variant == "v1-empty":
            mode = "v1"
        config = 'version = 1\n[boot]\ntimeout = 0\n'
        if variant != "v1-empty":
            config += '[[kit]]\npath = "Kits/Test.kit"\n'
        if variant != "missing-loader":
            config += f'[[driver]]\npath = "XnuTest.efi"\nargs = "{mode}"\nconnect = false\n'
        entry_type = "efi" if variant == "wrong-entry" else "xnu"
        config += (
            f'[[entry]]\nid = "xnu"\ntype = "{entry_type}"\n'
            'title = "Synthetic XNU request"\npath = "synthetic-kernel"\n'
            'args = "test handoff"\n'
        )
        if variant != "v1-empty":
            config += (
                '[[entry.kext]]\nenabled = false\nbundle = "Absent.kext"\n'
                '[[entry.kext]]\nbundle = "Kexts/Codeless.kext"\n'
                '[[entry.kext]]\nbundle = "Kexts/Code.kext"\n'
                'executable = "Contents/MacOS/Test"\n'
            )
        if variant == "missing-file":
            code.unlink()
        elif variant == "bad-image":
            data = bytearray(code.read_bytes())
            struct.pack_into("<I", data, 4, 0x01000007)  # x86_64
            code.write_bytes(data)
        elif variant == "truncated-command":
            data = bytearray(code.read_bytes())
            struct.pack_into("<I", data, 36, 0xFFFFFFF8)
            code.write_bytes(data)
        elif variant == "empty-plist":
            (code.parent.parent / "Info.plist").write_bytes(b"")
        elif variant == "duplicate":
            config += '[[entry.kext]]\nbundle = "kexts/code.kext"\n'
        elif variant == "bad-path":
            config = config.replace('executable = "Contents/MacOS/Test"', 'executable = "../outside"')
        elif variant == "bad-type":
            config = config.replace('executable = "Contents/MacOS/Test"', 'executable = false')
        (ca / "unixnu.toml").write_text(config)


if __name__ == "__main__":
    main()
