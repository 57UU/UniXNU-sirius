#!/usr/bin/env python3
"""Finish the U-Boot payload volume: add the loader chain to the unixnu payload image.

The unixnu payload step writes a FAT volume with \\EFI\\UniXNU\\unixnu.toml and the kernel +
ADT sidecars. On this board the UEFI implementation is the device's own U-Boot, so the volume
also carries the EFI binaries itself:

  \\EFI\\BOOT\\BOOTAA64.EFI             UniXNU.efi   (U-Boot: load mmc 0:50 ...; bootefi)
  \\EFI\\UniXNU\\Payloads\\DarwinBoot.efi  the XNU loader driver, referenced by unixnu.toml

Usage:
  make-uboot-payload.py --payload out/sirius/payload/payload.img \\
      --unixnu-efi Build/Out/AARCH64-DEBUG/UniXNU.efi \\
      --darwinboot-efi build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi \\
      [--output sirius-payload.img]

With --output the finished image is copied there; otherwise the payload image is patched in
place. Flash it to the vendor partition (mmcblk0p80) and boot it from the U-Boot menu.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys


def mcopy(image, *args):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    result = subprocess.run(['mcopy', '-i', str(image), *args], env=env)
    if result.returncode:
        raise SystemExit(f'mcopy {" ".join(args)} failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path, required=True, help='the payload.img of "unixnu build payload"')
    parser.add_argument('--unixnu-efi', type=Path, required=True, help='UniXNU.efi (package/build.sh output)')
    parser.add_argument('--darwinboot-efi', type=Path, required=True, help='DarwinBoot.efi (scripts/build-darwinboot.sh output)')
    parser.add_argument('--output', type=Path, default=None, help='copy the finished image here')
    args = parser.parse_args()

    for path in (args.payload, args.unixnu_efi, args.darwinboot_efi):
        if not path.is_file():
            raise SystemExit(f'{path} not found')

    image = args.payload
    mcopy(image, '-s', '-Q', '-m', str(args.unixnu_efi), '::/EFI/BOOT/BOOTAA64.EFI')
    mcopy(image, '-s', '-Q', '-m', str(args.darwinboot_efi), '::/EFI/UniXNU/Payloads/DarwinBoot.efi')

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(image, args.output)
        image = args.output

    listing = subprocess.run(['mdir', '-i', str(image), '-/', '::'], capture_output=True, text=True,
                             env=dict(os.environ, MTOOLS_SKIP_CHECK='1'))
    print(listing.stdout)
    print(f'U-Boot payload volume ready: {image} ({image.stat().st_size >> 20} MiB)')
    print('Flash to the vendor partition and select it from the U-Boot menu:')
    print('  ssh sirius "sudo dd if=<image> of=/dev/mmcblk0p80 bs=4M conv=fsync"')


if __name__ == '__main__':
    main()
