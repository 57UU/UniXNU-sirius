#!/usr/bin/env python3
"""Compile the sirius USB console driver against the recorded BSD ABI and archive it.

The driver is a port of UniXNU-picasso's bsd/dev/picasso_usb.c. It is built as an external archive
(like the other kits/sirius sources) instead of a bsd/conf/files entry: the XNU makefile generated
from conf/files does not pick up a file added by a patch, so listing it there produced no object.
"""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sirius-usb'
OUT.mkdir(parents=True, exist_ok=True)
source = ROOT / 'kits/sirius/sirius_usb.c'
obj = OUT / 'sirius_usb.o'
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json')
                    .read_text().strip().rstrip(','))
command, index = [], 0
arguments = record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(obj)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
# The ported driver predates this tree's warning set; keep the build quiet but strict about errors.
command.extend(['-w', '-Wno-error'])
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True)
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
(OUT / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-6000:])
# build-xnu-4k-baseline.sh force-loads exactly build/sheng-platform-power/libShengPlatformPower.a
# when CONFIG_XNU_SHENG_PMGR=1, so the USB object joins that archive instead of needing a new
# link hook. ar rcs adds the member and keeps the ones already there.
archive = ROOT / 'build/sheng-platform-power/libShengPlatformPower.a'
archive.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
print(f'SiriusUsb: added {obj.name} to {archive}')
