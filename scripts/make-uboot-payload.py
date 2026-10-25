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
import tempfile


def run_mtool(argv, check=True):
    """Run an mtools command with no controlling terminal: mtools opens /dev/tty for its
    prompts, and a background caller without a console gets stopped by SIGTTIN/SIGTTOU."""
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    result = subprocess.run(['setsid'] + argv, env=env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if check and result.returncode:
        raise SystemExit(f'{" ".join(argv)} failed: {result.stdout}')
    return result


def neutralize_apple_cpu(image):
    """Stop the prelinked AppleARMCPU personality from claiming the CPU nubs.

    AppleARMPlatform.kext ships an AppleARMCPU class whose personality is
    IONameMatch = "cpu" with probe score 100. IODTPlatformExpert matches names by prefix, so
    "cpu" also matches this board's cpu0..cpu7 nodes. Its start validates an Apple-shaped /cpus
    (max_cpus, cpu-id, interrupts, function-*) and panics with "CPU configuration in device tree
    is invalid" on a non-Apple tree. The native ArmCpu owns the cores instead, so the match name
    is changed in the personality plist that travels in the prelinked kernel. The replacement has
    the same length, so nothing in the image moves.
    """
    with tempfile.TemporaryDirectory() as tmp:
        kernel = Path(tmp) / 'kernel.macho'
        run_mtool(['mcopy', '-n', '-o', '-i', str(image), '::/EFI/UniXNU/Payloads/kernel.macho', str(kernel)])
        data = bytearray(kernel.read_bytes())
        anchor = data.find(b'<key>AppleARMCPU</key>')
        if anchor < 0:
            raise SystemExit('AppleARMCPU personality not found in the prelinked kernel')
        window = data.find(b'<key>IONameMatch</key><string>cpu</string>', anchor, anchor + 600)
        if window < 0:
            raise SystemExit('AppleARMCPU IONameMatch is not in the expected form')
        at = window + len(b'<key>IONameMatch</key><string>')
        data[at:at + 3] = b'xpu'
        kernel.write_bytes(data)
        run_mtool(['mcopy', '-o', '-i', str(image), str(kernel), '::/EFI/UniXNU/Payloads/kernel.macho'])
    print('AppleARMCPU personality: IONameMatch cpu -> xpu (the native ArmCpu owns /cpus)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path, required=True, help='the payload.img of "unixnu build payload"')
    parser.add_argument('--unixnu-efi', type=Path, required=True, help='UniXNU.efi (package/build.sh output)')
    parser.add_argument('--darwinboot-efi', type=Path, required=True, help='DarwinBoot.efi (scripts/build-darwinboot.sh output)')
    parser.add_argument('--marker', action='store_true', help='also write the BOOTUNIXNU marker that selects the UniXNU boot')
    parser.add_argument('--output', type=Path, default=None, help='copy the finished image here')
    args = parser.parse_args()

    for path in (args.payload, args.unixnu_efi, args.darwinboot_efi):
        if not path.is_file():
            raise SystemExit(f'{path} not found')

    image = args.payload
    stage = image.parent / 'uboot-stage'
    shutil.rmtree(stage, ignore_errors=True)
    (stage / 'EFI/BOOT').mkdir(parents=True)
    (stage / 'EFI/UniXNU/Payloads').mkdir(parents=True)
    shutil.copyfile(args.unixnu_efi, stage / 'EFI/BOOT/BOOTAA64.EFI')
    shutil.copyfile(args.darwinboot_efi, stage / 'EFI/UniXNU/Payloads/DarwinBoot.efi')
    if args.marker:
        (stage / 'EFI/UniXNU/BOOTUNIXNU').touch()
    run_mtool(['mcopy', '-i', str(image), '-s', '-o', '-Q', '-m', str(stage / 'EFI'), '::/'])
    shutil.rmtree(stage)
    neutralize_apple_cpu(image)

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(image, args.output)
        image = args.output

    listing = run_mtool(['mdir', '-i', str(image), '-/', '::'], check=False)
    print(listing.stdout)
    print(f'U-Boot payload volume ready: {image} ({image.stat().st_size >> 20} MiB)')
    print('Flash to the vendor partition and select it from the U-Boot menu:')
    print('  ssh sirius "sudo dd if=<image> of=/dev/mmcblk0p80 bs=4M conv=fsync"')


if __name__ == '__main__':
    main()
