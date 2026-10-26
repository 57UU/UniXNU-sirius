#!/usr/bin/env python3
"""Compile the NativeEmmc IOKit driver against the recorded BSD ABI and archive it.

Same shape as build-sirius-emmc.py: the driver is built as an external archive rather than a
bsd/conf/files entry, because the XNU makefile generated from conf/files does not pick up a file
added by a patch. The object joins libShengPlatformPower.a, the archive build-xnu force-loads when
CONFIG_XNU_SHENG_PMGR=1.

The C++ options below are the ones XNU's own kexts use: -fapple-kext gives OSDefineMetaClassAndStructors
its definitions, and exceptions/RTTI are off in the kernel.
"""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-emmc'
OUT.mkdir(parents=True, exist_ok=True)
source = ROOT / 'kits/sirius/NativeEmmc.cpp'
obj = OUT / 'NativeEmmc.o'
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json')
                    .read_text().strip().rstrip(','))
command, index = [], 0
arguments = record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        # The recorded command pins -x c, and -x applies positionally, so the C++ language and
        # options have to come before the source file or clang rejects -std=c++17.
        command.extend([
            '-x', 'c++', '-std=c++17', '-fapple-kext', '-fno-exceptions', '-fno-rtti',
            '-fno-threadsafe-statics', '-nostdinc++',
            # IOStorageFamily is a kext, not part of XNU, so the build stages its headers here.
            '-I', str(ROOT / 'build/storage-abi/include'),
        ])
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(obj)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend(['-w', '-Wno-error'])
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True)
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
(OUT / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-6000:])

archive = ROOT / 'build/sheng-platform-power/libShengPlatformPower.a'
archive.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
print(f'NativeEmmc: added {obj.name} to {archive}')
