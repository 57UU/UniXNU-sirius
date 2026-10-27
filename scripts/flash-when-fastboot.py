#!/usr/bin/env python3
"""Flash a sirius payload as soon as the phone is in ABL fastboot.

The port has one flashing target: ABL's fastboot (the `product: sirius` gadget with
an empty `version-bootloader`). U-Boot's own fastboot answers with a
`version-bootloader: U-Boot ...` line, cannot write partitions, and can be left
with `fastboot erase misc` plus `fastboot reboot`.

The script waits for the phone instead of asking for a key press first, so a
build can be flashed the moment the phone is put into fastboot. It never touches
a device that does not report `product: sirius`.

  python3 scripts/flash-when-fastboot.py /tmp/sirius-payload.img --timeout 1800
"""

import argparse
import hashlib
import os
import subprocess
import sys
import time


def fastboot(*args: str, timeout: int = 30) -> str:
    """Run fastboot and return its stdout, or "" when it fails."""
    try:
        done = subprocess.run(
            ["fastboot", *args],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return ""
    if done.returncode != 0:
        return ""
    return done.stdout


def one_line(text: str, key: str) -> str:
    for line in text.splitlines():
        line = line.strip()
        if line.startswith(key):
            return line.split(":", 1)[1].strip()
    return ""


def device_state() -> str:
    """'none', 'abl' or 'uboot', with the serial appended as ':<serial>'."""
    listing = fastboot("devices", timeout=15)
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        serial = fields[0]
        info = fastboot("-s", serial, "getvar", "version-bootloader", timeout=15)
        if "U-Boot" in info:
            return "uboot:" + serial
        product = one_line(fastboot("-s", serial, "getvar", "product", timeout=15), "product")
        if product == "sirius":
            return "abl:" + serial
        return "other:" + serial
    return "none"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", help="payload volume image to flash")
    parser.add_argument("--partition", default="vendor", help="partition (default: vendor)")
    parser.add_argument("--timeout", type=int, default=1800, help="seconds to wait (default: 1800)")
    parser.add_argument("--poll", type=int, default=10, help="seconds between checks (default: 10)")
    args = parser.parse_args()

    if not os.path.isfile(args.image):
        print(f"no such image: {args.image}", file=sys.stderr)
        return 2
    size = os.path.getsize(args.image)
    with open(args.image, "rb") as handle:
        digest = hashlib.file_digest(handle, "sha256").hexdigest()
    print(f"image {args.image} {size} bytes sha256 {digest}", flush=True)

    deadline = time.monotonic() + args.timeout
    last = ""
    polls = 0
    while time.monotonic() < deadline:
        state = device_state()
        polls += 1
        # A gadget that is still initialising answers neither query; report the state
        # when it changes and once a minute otherwise, so a silent run is not mistaken
        # for a stuck one.
        if state != last or polls % 6 == 0:
            print(f"device: {state}", flush=True)
            last = state
        kind, _, serial = state.partition(":")
        if kind == "abl":
            print(f"flashing {args.partition} on {serial}", flush=True)
            out = fastboot("-s", serial, "flash", args.partition, args.image, timeout=600)
            print(out.strip() or "(no output)", flush=True)
            if "Finished" not in out:
                return 1
            print("rebooting", flush=True)
            print(fastboot("-s", serial, "reboot", timeout=60).strip(), flush=True)
            return 0
        if kind == "uboot":
            # U-Boot's fastboot cannot write this volume, but it can erase the BCB
            # that sent it there, which is how the next boot returns to UniXNU.
            print(f"{serial} is in U-Boot fastboot: erasing misc and rebooting", flush=True)
            fastboot("-s", serial, "erase", "misc", timeout=60)
            fastboot("-s", serial, "reboot", timeout=60)
            last = ""
        time.sleep(args.poll)
    print("timed out waiting for ABL fastboot", file=sys.stderr)
    return 3


if __name__ == "__main__":
    sys.exit(main())
