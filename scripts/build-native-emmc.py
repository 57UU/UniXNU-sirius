#!/usr/bin/env python3
"""Compile the NativeEmmc IOKit driver, bind its storage imports and archive both.

Same shape as build-sirius-emmc.py: the driver is built as an external archive rather than a
bsd/conf/files entry, because the XNU makefile generated from conf/files does not pick up a
file added by a patch. The object joins libShengPlatformPower.a, the archive build-xnu
force-loads when CONFIG_XNU_SHENG_PMGR=1.

IOStorageFamily is a kext of the original kernelcache, not part of the XNU source, so the
kernel link has no definition for the class the driver subclasses. scripts/data/storage-bindings.json
holds the original addresses derive-storage-bindings.py proved for the pinned kernelcache; at the
prelink slide they are the addresses the driver will call, and original-bindings.o defines them as
absolute symbols so the link resolves without an IOStorageFamily definition.

The record is used rather than re-derived because the archive step runs before the prelink, so a
cold tree has no kernelcache yet. When a kernelcache is reachable the derivation is re-run with
--check, so drift in the pinned image or the slide fails the build instead of the device.

The C++ options below are the ones XNU's own kexts use: -fapple-kext gives OSDefineMetaClassAndStructors
its definitions, and exceptions/RTTI are off in the kernel.
"""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np

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


def undefined_storage_imports(path):
    data = path.read_bytes()
    image = np.macho(data)
    at = next(offset for cmd, offset, size in image['commands'] if cmd == 2)
    symoff, count, stroff, strsize = struct.unpack_from('<4I', data, at + 8)
    names = set()
    for index in range(count):
        strx, kind, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
        if kind & 0x0e or strx >= strsize:
            continue
        end = data.find(b'\0', stroff + strx)
        name = data[stroff + strx:end].decode()
        if 'IOBlockStorageDevice' in name:
            names.add(name)
    return sorted(names)


bindings = json.loads((ROOT / 'scripts/data/storage-bindings.json').read_text())
addresses = {name: int(value, 16) for name, value in bindings['addresses'].items()}
imports = undefined_storage_imports(obj)
if not imports:
    raise SystemExit('NativeEmmc.o imports no IOStorageFamily symbol; the driver shape changed')
missing = [name for name in imports if name not in addresses]
if missing:
    raise SystemExit('the driver imports storage symbols the record does not cover: ' + ', '.join(missing))
if any(name.endswith(('D0Ev', 'D1Ev')) for name in imports):
    raise SystemExit('the driver must not use the abstract complete or deleting destruction')

assembly = OUT / 'original-bindings.S'
assembly.write_text(''.join(f'.globl {name}\n.set {name}, {addresses[name] + bindings["slide"]:#x}\n'
                            for name in imports))
symbols = OUT / 'original-bindings.o'
compiled = subprocess.run([*compiler.split(), '-target', 'arm64-apple-ios17.0', '-Werror',
                           '-Wno-incompatible-sysroot', '-c', str(assembly), '-o', str(symbols)],
                          capture_output=True, text=True)
if compiled.returncode:
    raise SystemExit((compiled.stdout + compiled.stderr)[-4000:])

# A warm tree still has the kernelcache, so the record is proved against it here; a cold tree
# (CI) reaches the prelink afterwards and the workflow checks it there.
if sorted(ROOT.parents[3].glob('ipsw/*/kernelcache.decompressed')):
    check = subprocess.run(['python3', str(ROOT / 'scripts/derive-storage-bindings.py'), '--check'],
                           cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (OUT / 'derive-check.log').write_text(check.stdout)
    if check.returncode:
        raise SystemExit(check.stdout[-4000:])

archive = ROOT / 'build/sheng-platform-power/libShengPlatformPower.a'
archive.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj, symbols], check=True)
print(f'NativeEmmc: {len(imports)} storage imports bound from the recorded kernelcache '
      f'({bindings["kernelcache_sha256"][:16]}, slide {bindings["slide"]:#x})')
print(f'NativeEmmc: added {obj.name} and {symbols.name} to {archive}')
