#!/usr/bin/env python3
"""Capture the sirius XNU USB console (CDC-ACM over DWC3).

The kernel streams its console mirror ring on a bulk IN endpoint, so the host sees the boot log
from the very first line once the gadget enumerates. The port is a plain CDC-ACM tty, so no USB
library is needed: termios raw mode and a read loop are enough.

    sudo python3 scripts/capture-usb-console.py --wait 120 --duration 60

Without --port the script prefers a /dev/serial/by-id link that names the sirius console and
otherwise takes the first /dev/ttyACM* that appears. It needs root (or dialout) because the tty is
root:dialout.

    sudo python3 scripts/capture-usb-console.py --follow --output boot.log

--follow survives re-enumeration: when the read returns end-of-file or the port disappears the
script waits for the port again and resumes appending to the same log.
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
    # A by-id link names the device even when several CDC ports are attached; the console calls
    # itself "sirius XNU USB console", so the interface name carries SIRIUS01.
    for candidate in sorted(glob.glob('/dev/serial/by-id/*')):
        if 'SIRIUS' in candidate.upper() or 'sirius' in candidate:
            return candidate
    for candidate in sorted(glob.glob('/dev/ttyACM*')):
        return candidate
    return None


def wait_for_port(timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        port = find_port()
        if port is not None:
            return port
        time.sleep(0.2)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default=None, help='CDC-ACM device, default: first /dev/ttyACM*')
    parser.add_argument('--wait', type=float, default=60.0, help='seconds to wait for the port')
    parser.add_argument('--duration', type=float, default=0.0, help='seconds to capture, 0 = until Ctrl-C')
    parser.add_argument('--output', default='out/sirius-usb-console.log', help='log file (bytes appended)')
    parser.add_argument('--follow', action='store_true',
                        help='keep waiting for the port after EOF or re-enumeration')
    args = parser.parse_args()

    port = args.port or wait_for_port(args.wait)
    if port is None:
        print(f'no CDC-ACM port appeared within {args.wait:.0f}s', file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(args.output) or '.', exist_ok=True)
    total = 0
    end = time.time() + args.duration if args.duration else None
    with open(args.output, 'ab') as log:
        try:
            while end is None or time.time() < end:
                if port is None:
                    if not args.follow:
                        break
                    port = wait_for_port(args.wait)
                    if port is None:
                        break
                print(f'capturing {port} -> {args.output}', file=sys.stderr)
                try:
                    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY)
                except OSError as error:
                    print(f'{port}: {error}', file=sys.stderr)
                    port = None
                    time.sleep(1.0)
                    continue
                tty.setraw(fd)
                attrs = termios.tcgetattr(fd)
                attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
                attrs[2] |= termios.CS8
                attrs[2] &= ~termios.CRTSCTS
                attrs[4] = attrs[5] = termios.B115200
                termios.tcsetattr(fd, termios.TCSANOW, attrs)
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
                except OSError:
                    pass  # the device went away (reboot, re-enumeration)
                finally:
                    os.close(fd)
                port = None if args.follow else port
                if not args.follow:
                    break
        except KeyboardInterrupt:
            pass
    print(f'\ncaptured {total} bytes', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
