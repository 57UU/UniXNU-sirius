#!/usr/bin/env python3
"""Build a GPT test disk whose named partition starts with a FAT32 volume.

Layout (all sizes in MiB):
  GPT
  ├─ "misc"       --misc MiB, empty
  └─ <--name>     the rest of the disk:
       [0, --fat)          FAT32 volume populated from --tree
       [--fat, ...)        raw; its first bytes are --marker

This models a device partition whose head carries the UniXNU payload
volume and whose tail is handed to the payload as a separate disk through
[[block_slice]]. Needs mkfs.vfat or mformat; files are copied with mcopy
when available, otherwise with the pyfatfs module (run this script with an
interpreter that has it).
"""

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from pathlib import Path

MIB = 1024 * 1024


def write_gpt(path: Path, size: int, misc: int, name: str) -> int:
    sector = 512
    last = size * MIB // sector - 1
    start = 2048 + misc * MIB // sector
    entries = bytearray(128 * 128)
    linux_data = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le
    for index, (first, end, label) in enumerate([
        (2048, start - 1, "misc"), (start, last - 33, name)
    ]):
        encoded = label.encode("utf-16-le")
        if len(encoded) > 72:
            raise ValueError("GPT partition name exceeds 36 UTF-16 units")
        struct.pack_into("<16s16sQQQ72s", entries, index * 128,
                         linux_data, uuid.uuid4().bytes_le, first, end, 0, encoded)
    disk_guid = uuid.uuid4().bytes_le

    def header(current, alternate, table):
        data = bytearray(sector)
        struct.pack_into("<8sIIIIQQQQ16sQIII", data, 0,
                         b"EFI PART", 0x10000, 92, 0, 0, current, alternate,
                         34, last - 33, disk_guid, table, 128, 128,
                         zlib.crc32(entries) & 0xFFFFFFFF)
        struct.pack_into("<I", data, 16, zlib.crc32(data[:92]) & 0xFFFFFFFF)
        return data

    mbr = bytearray(sector)
    struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\x00\x02\x00", 0xEE,
                     b"\xff\xff\xff", 1, min(last, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    with path.open("r+b") as disk:
        for lba, data in [(0, mbr), (1, header(1, last, 2)), (2, entries),
                          (last - 32, entries),
                          (last, header(last, 1, last - 32))]:
            disk.seek(lba * sector)
            disk.write(data)
    return start * sector


def populate_mtools(image: Path, tree: Path) -> None:
    for path in sorted(tree.rglob("*")):
        rel = "::/" + path.relative_to(tree).as_posix()
        if path.is_dir():
            subprocess.run(["mmd", "-i", str(image), rel], check=True)
        else:
            subprocess.run(["mcopy", "-i", str(image), str(path), rel], check=True)


def populate_pyfatfs(image: Path, tree: Path) -> None:
    import warnings

    warnings.filterwarnings("ignore")
    from pyfatfs.PyFatFS import PyFatFS

    fat = PyFatFS(str(image))
    try:
        for path in sorted(tree.rglob("*")):
            rel = "/" + path.relative_to(tree).as_posix()
            if path.is_dir():
                fat.makedirs(rel, recreate=True)
            else:
                with fat.openbin(rel, "wb") as out:
                    out.write(path.read_bytes())
    finally:
        fat.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--tree", type=Path, required=True, help="files for the FAT volume")
    parser.add_argument("--size", type=int, default=256, help="disk size, MiB")
    parser.add_argument("--misc", type=int, default=8, help="first partition size, MiB")
    parser.add_argument("--fat", type=int, default=64, help="FAT volume size, MiB (>= 33)")
    parser.add_argument("--name", default="userdata", help="GPT name of the second partition")
    parser.add_argument("--marker", default="UNIXNU-TAIL-MARKER", help="text at the start of the tail")
    args = parser.parse_args()

    if args.fat < 33 or args.misc + args.fat + 2 >= args.size:
        sys.exit("error: sizes do not fit (FAT32 needs >= 33 MiB)")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "wb") as f:
        f.truncate(args.size * MIB)

    start = write_gpt(args.out, args.size, args.misc, args.name)

    with tempfile.TemporaryDirectory() as tmp:
        fat = Path(tmp) / "fat.img"
        if shutil.which("mkfs.vfat"):
            subprocess.run(["mkfs.vfat", "-F", "32", "-n", "PAYLOAD", "-C", str(fat), str(args.fat * 1024)],
                           check=True, capture_output=True)
        else:
            with fat.open("wb") as image:
                image.truncate(args.fat * MIB)
            subprocess.run(["mformat", "-i", str(fat), "-F", "-v", "PAYLOAD", "::"],
                           check=True, capture_output=True)
        if shutil.which("mcopy") and shutil.which("mmd"):
            populate_mtools(fat, args.tree)
        else:
            populate_pyfatfs(fat, args.tree)
        data = fat.read_bytes()

    with open(args.out, "r+b") as disk:
        disk.seek(start)
        disk.write(data)
        disk.seek(start + args.fat * MIB)
        disk.write(args.marker.encode())

    print(f"{args.out}: partition '{args.name}' at {start} bytes, FAT {args.fat} MiB, tail marker at +{args.fat} MiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
