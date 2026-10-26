#!/usr/bin/env python3
"""Compile the NativeEmmc IOKit driver, bind its storage imports and archive both.

Same shape as build-sirius-emmc.py: the driver is built as an external archive rather than a
bsd/conf/files entry, because the XNU makefile generated from conf/files does not pick up a
file added by a patch. The object joins libShengPlatformPower.a, the archive build-xnu
force-loads when CONFIG_XNU_SHENG_PMGR=1.

IOStorageFamily is a kext of the original kernelcache, not part of the XNU source, so the
kernel link has no definition for the class the driver subclasses. derive-storage-bindings.py
resolves the imports against the original kernelcache at the prelink slide and writes them as
absolute symbols; original-bindings.o is archived next to the driver so the link resolves them
without an IOStorageFamily definition.

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
compiler = record['arguments'][0]
if not Path(compiler).is_file():
    compiler = 'xcrun clang'
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

# The bindings are absolute addresses, so they are derived from the kernelcache and the
# prelink slide after the object exists: the derivation refuses any import it cannot name.
derivation = subprocess.run(['python3', str(ROOT / 'scripts/derive-storage-bindings.py'), '--object', str(obj)],
                            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(OUT / 'derive.log').write_text(derivation.stdout)
if derivation.returncode:
    raise SystemExit(derivation.stdout[-6000:])

assembly = OUT / 'original-bindings.S'
bindings = OUT / 'original-bindings.o'
compiled = subprocess.run([*compiler.split(), '-target', 'arm64-apple-ios17.0', '-Werror',
                           '-Wno-incompatible-sysroot', '-c', str(assembly), '-o', str(bindings)],
                          capture_output=True, text=True)
if compiled.returncode:
    raise SystemExit((compiled.stdout + compiled.stderr)[-4000:])

archive = ROOT / 'build/sheng-platform-power/libShengPlatformPower.a'
archive.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj, bindings], check=True)
print(derivation.stdout.strip().splitlines()[0])
print(f'NativeEmmc: added {obj.name} and {bindings.name} to {archive}')
