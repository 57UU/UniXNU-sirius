#!/usr/bin/env python3
"""Capture the sirius XNU USB console (CDC-ACM over DWC3).

The kernel streams its console mirror ring on a bulk IN endpoint, so the host sees the boot log
from the very first line once the gadget enumerates. The port is a plain CDC-ACM tty, so no USB
library is needed: termios raw mode and a read loop are enough.

    sudo python3 scripts/capture-usb-console.py --wait 120 --duration 60

Without --port the script takes the first /dev/ttyACM* that appears. It needs root (or dialout)
because the tty is root:dialout.
"""
import argparse
import glob
import os
import select
import sys
import termios
import time
import tty


def find_port():
    for candidate in sorted(glob.glob('/dev/ttyACM*')):
        return candidate
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default=None, help='CDC-ACM device, default: first /dev/ttyACM*')
    parser.add_argument('--wait', type=float, default=60.0, help='seconds to wait for the port')
    parser.add_argument('--duration', type=float, default=0.0, help='seconds to capture, 0 = until Ctrl-C')
    parser.add_argument('--output', default='out/sirius-usb-console.log', help='log file (bytes appended)')
    args = parser.parse_args()

    deadline = time.time() + args.wait
    port = args.port
    while port is None:
        port = find_port()
        if port is not None:
            break
        if time.time() > deadline:
            print(f'no CDC-ACM port appeared within {args.wait:.0f}s', file=sys.stderr)
            return 1
        time.sleep(0.2)

    print(f'capturing {port} -> {args.output}', file=sys.stderr)
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
    attrs[2] |= termios.CS8
    attrs[2] &= ~termios.CRTSCTS
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    os.makedirs(os.path.dirname(args.output) or '.', exist_ok=True)
    total = 0
    end = time.time() + args.duration if args.duration else None
    with open(args.output, 'wb') as log:
        try:
            while end is None or time.time() < end:
                ready, _, _ = select.select([fd], [], [], 0.5)
                if not ready:
                    continue
                chunk = os.read(fd, 4096)
                if not chunk:
                    break
                log.write(chunk)
                log.flush()
                total += len(chunk)
                sys.stdout.write(chunk.decode('utf-8', 'replace'))
                sys.stdout.flush()
        except KeyboardInterrupt:
            pass
    os.close(fd)
    print(f'\ncaptured {total} bytes', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
