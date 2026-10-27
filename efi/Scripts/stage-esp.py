#!/usr/bin/env python3
"""Assemble an ESP directory for UniXNU.

Copies one or more source trees into OUT (later trees win), compiles every
*.dts to *.dtb and *.dtso to *.dtbo with dtc (sources are not copied), and
places extra files with --put SRC=DEST (DEST relative to OUT).

Example:
  stage-esp.py -o out/esp UniXNUPkg/Test/Qemu/esp \\
      --put Build/Out/AARCH64-DEBUG/UniXNU.efi=EFI/BOOT/BOOTAA64.EFI
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

DT_SOURCES = {".dts": ".dtb", ".dtso": ".dtbo"}


def copy_tree(src: Path, out: Path) -> None:
    for path in sorted(src.rglob("*")):
        rel = path.relative_to(src)
        dest = out / rel
        if path.is_dir():
            dest.mkdir(parents=True, exist_ok=True)
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        if path.suffix in DT_SOURCES:
            target = dest.with_suffix(DT_SOURCES[path.suffix])
            subprocess.run(
                ["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(target), str(path)],
                check=True,
            )
        else:
            shutil.copy2(path, dest)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trees", nargs="+", type=Path, help="source trees, copied in order")
    parser.add_argument("-o", "--out", type=Path, required=True, help="ESP directory to (re)create")
    parser.add_argument("--put", action="append", default=[], metavar="SRC=DEST", help="extra file to place")
    args = parser.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)

    for tree in args.trees:
        if not tree.is_dir():
            print(f"error: {tree} is not a directory", file=sys.stderr)
            return 1
        copy_tree(tree, args.out)

    for item in args.put:
        src, _, dest = item.partition("=")
        if not dest:
            print(f"error: --put needs SRC=DEST, got {item!r}", file=sys.stderr)
            return 1
        target = args.out / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)

    for path in sorted(args.out.rglob("*")):
        if path.is_file():
            print(f"  {path.relative_to(args.out)}  ({path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
