#!/usr/bin/env python3
"""Derive the original IOStorageFamily addresses a built-in block driver binds to.

IOStorageFamily is a kext of the original kernelcache, not part of the XNU source, so an
archive that subclasses IOBlockStorageDevice leaves undefined symbols the kernel link
cannot resolve. The prelink keeps every component at a constant slide and rejoins it with
the native kernel, so the addresses the driver needs are the addresses of the original
kernelcache plus that slide.

This script derives them from the pinned original kernelcache alone and states the
evidence for each one:

  * the public vtable slot order (scripts/data/storage-vtable-names.json, pinned to the
    IOStorageFamily source revision) names every slot;
  * the vtable is located in the kext __DATA_CONST by requiring every slot whose name the
    kernel itself exports to hold that export, which identifies one table only;
  * the protected constructor is the single function that stores table+16, and its branch
    target resolves through a loader thunk to IOService's constructor;
  * the base destructor is the branch the deleting destructor takes, resolving to
    IOService's destructor;
  * the metaclass is the constant the getMetaClass getter materialises.

The result is a small assembly file of absolute symbols, which the linker resolves without
an IOStorageFamily definition. Nothing is taken from a live boot: the numbers come from the
kernelcache the prelink consumes and are re-checked against the prelinked kernel whenever
that file already exists.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np

MODULE = 'com.apple.iokit.IOStorageFamily'
EXPECTED_SLOTS = 190
EXPECTED_PURE = 14
EXPECTED_KERNEL_BINDINGS = 176
DEFAULT_SLIDE = -0x1000000

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernelcache', type=Path, help='original kernelcache the prelink reads')
parser.add_argument('--slide', type=lambda value: int(value, 0), default=DEFAULT_SLIDE)
parser.add_argument('--object', type=Path, default=ROOT / 'build/native-emmc/NativeEmmc.o',
                    help='driver object whose undefined storage symbols are to be bound')
parser.add_argument('--table', type=Path, default=ROOT / 'scripts/data/storage-vtable-names.json')
parser.add_argument('--output', type=Path, default=ROOT / 'build/native-emmc')
args = parser.parse_args()


def require(value, message):
    if not value:
        raise SystemExit('derive-storage-bindings: ' + message)


def symbols_of(path):
    data = path.read_bytes()
    image = np.macho(data)
    at = next(offset for command, offset, size in image['commands'] if command == 2)
    symoff, count, stroff, strsize = struct.unpack_from('<4I', data, at + 8)
    result = []
    for index in range(count):
        strx, kind, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
        if stroff + strx >= len(data):
            continue
        end = data.find(b'\0', stroff + strx)
        if strx >= strsize or end < 0:
            continue
        result.append((data[stroff + strx:end].decode(), kind, value))
    return result


def kernelcache_path():
    """The kernelcache the prelink consumes, matched against the last prelink plan."""
    if args.kernelcache:
        return args.kernelcache, None
    plan_path = ROOT.parent / 'kernel/kernel.json'
    wanted = json.loads(plan_path.read_text())['original_sha256'] if plan_path.is_file() else None
    found = sorted(ROOT.parents[3].glob('ipsw/*/kernelcache.decompressed'))
    require(found, 'no kernelcache under out/ipsw; pass --kernelcache')
    if wanted:
        for path in found:
            if hashlib.sha256(path.read_bytes()).hexdigest() == wanted:
                return path, wanted
    require(len(found) == 1, 'several kernelcaches and no plan to choose by; pass --kernelcache')
    return found[0], wanted


table = json.loads(args.table.read_text())
methods = table['methods']
require(len(methods) == EXPECTED_SLOTS and methods.count('___cxa_pure_virtual') == EXPECTED_PURE,
        'the recorded vtable slot table is not the pinned one')

path, wanted = kernelcache_path()
kernel = path.read_bytes()
original_sha = hashlib.sha256(kernel).hexdigest()
if wanted:
    require(original_sha == wanted, f'{path} is not the kernelcache the last prelink used')
original = np.macho(kernel)
segments = original['segments']

info = next((s for s in segments if s['name'] == '__PRELINK_INFO'), None)
require(info, 'the kernelcache has no __PRELINK_INFO segment')
at = np.backed_offset(segments, info['va'], info['filesize'], len(kernel))
metadata = np.unserialize_xml(kernel[at:at + info['filesize']])
entry = next((e for e in metadata['_PrelinkInfoDictionary'] if e.get('CFBundleIdentifier') == MODULE), None)
require(entry, f'{MODULE} is not in the kernelcache')
require(entry.get('_PrelinkExecutableLoadAddr') and entry.get('_PrelinkExecutableSize'),
        f'{MODULE} has no executable')
header = np.backed_offset(segments, entry['_PrelinkExecutableLoadAddr'], 32, len(kernel))
module = np.macho(kernel, header)
require(module['kind'] == 11, f'{MODULE} is not a kext image')
code = next((s for s in module['segments'] if s['name'] == '__TEXT_EXEC'), None)
constant = next((s for s in module['segments'] if s['name'] == '__DATA_CONST'), None)
require(code and constant, f'{MODULE} lacks __TEXT_EXEC or __DATA_CONST')


def read(address, length):
    at = np.backed_offset(segments, address, length, len(kernel))
    return kernel[at:at + length]


def relative_branch(address, instruction):
    immediate = instruction & 0x3ffffff
    if immediate & 0x2000000:
        immediate -= 0x4000000
    return address + immediate * 4


def page_target(address, instruction, add):
    immediate = ((instruction >> 5 & 0x7ffff) << 2) | (instruction >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    return (address & ~4095) + immediate * 4096 + (add >> 10 & 4095)


exports = np.exports(kernel, original)
by_name = {name: address for address, names in exports.items() for name in names}
known = [name for name in methods if name in by_name]
require(len(known) == EXPECTED_KERNEL_BINDINGS,
        f'the kernel exports {len(known)} of the {EXPECTED_SLOTS} slot names, not {EXPECTED_KERNEL_BINDINGS}')

blob = read(constant['va'], constant['size'])
candidates = []
for at in range(0, len(blob) - (EXPECTED_SLOTS + 2) * 8 + 1, 8):
    if blob[at:at + 16] != bytes(16):
        continue
    pointers = struct.unpack_from('<' + 'Q' * EXPECTED_SLOTS, blob, at + 16)
    if all(pointers[slot] == by_name[name] for slot, name in enumerate(methods) if name in by_name):
        candidates.append((constant['va'] + at, pointers))
require(len(candidates) == 1, f'{len(candidates)} block-storage dispatch tables match the pinned slot order')
vtable, pointers = candidates[0]


def inside_code(address):
    return code['va'] <= address < code['va'] + code['size']


for slot in (0, 1):
    require(inside_code(pointers[slot]) and read(pointers[slot], 4) == struct.pack('<I', 0xd4200020),
            f'slot {slot} is not the abstract complete-object destructor trap')
owned = sum(1 for address in pointers if inside_code(address))
require(owned == EXPECTED_SLOTS - EXPECTED_KERNEL_BINDINGS,
        f'{owned} slots are owned by the kext, not {EXPECTED_SLOTS - EXPECTED_KERNEL_BINDINGS}')

getter = pointers[7]
first, add, ret = struct.unpack('<3I', read(getter, 12))
require(first & 0x9f00001f == 0x90000000 and add & 0xffc003ff == 0x91000000 and ret == 0xd65f03c0,
        'slot 7 is not a single-constant metaclass getter')
metaclass = page_target(getter, first, add)
require(any(s['va'] <= metaclass < s['va'] + s['size'] for s in module['segments'] if s['size']),
        'the metaclass getter does not materialise an address inside the kext')

constructors = []
for at in range(code['va'], code['va'] + code['size'] - 32, 4):
    words = struct.unpack('<8I', read(at, 32))
    if (words[:2] != (0xa9bf7bfd, 0x910003fd) or words[2] & 0xfc000000 != 0x94000000 or
            words[3] & 0x9f00001f != 0x90000008 or words[4] & 0xffc003ff != 0x91000108 or
            words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0)):
        continue
    if page_target(at + 12, words[3], words[4]) == vtable + 16:
        constructors.append((at, relative_branch(at + 8, words[2])))
require(len(constructors) == 1, f'{len(constructors)} constructors store the storage dispatch table')
constructor, constructor_branch = constructors[0]
destructor = pointers[0] - 4
branch = struct.unpack('<I', read(destructor, 4))[0]
require(branch & 0xfc000000 == 0x14000000, 'the base destructor is not a branch')


def thunk_targets(address):
    """Names of the export a loader thunk at address loads, for the constructor/destructor check."""
    first, load, jump = struct.unpack('<3I', read(address, 12))
    if not (first & 0x9f00001f == 0x90000010 and load & 0xffc003ff == 0xf9400210 and jump == 0xd61f0200):
        return []
    slot = page_target(address, first, 0) + (load >> 10 & 4095) * 8
    return exports.get(struct.unpack('<Q', read(slot, 8))[0], [])


require('__ZN9IOServiceC2EPK11OSMetaClass' in thunk_targets(constructor_branch),
        'the protected constructor does not forward to the actual IOService constructor')
require('__ZN9IOServiceD2Ev' in thunk_targets(relative_branch(destructor, branch)),
        'the base destructor does not forward to the actual IOService destructor')

addresses = {'__ZTV20IOBlockStorageDevice': vtable,
             '__ZN20IOBlockStorageDevice10gMetaClassE': metaclass,
             '__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': constructor,
             '__ZN20IOBlockStorageDeviceD2Ev': destructor}
for slot, name in enumerate(methods):
    if name.startswith('__ZN20IOBlockStorageDevice') or name.startswith('__ZNK20IOBlockStorageDevice'):
        require(inside_code(pointers[slot]), f'slot {slot} {name} is not inside the kext code')
        addresses[name] = pointers[slot]

undefined = sorted({name for name, kind, value in symbols_of(args.object)
                    if kind & 0x0e == 0 and 'IOBlockStorageDevice' in name})
require(undefined, f'{args.object} imports no IOStorageFamily symbol; nothing to bind')
missing = [name for name in undefined if name not in addresses]
require(not missing, 'the driver imports storage symbols the pinned slot order does not name: ' + ', '.join(missing))
require(not any(name.endswith(('D0Ev', 'D1Ev')) for name in undefined),
        'the driver must not use the abstract complete or deleting destruction')

bindings = {name: addresses[name] + args.slide for name in undefined}

# The prelinked kernel, when the previous step already made it, holds the same code at the
# slid address: compare it, so a wrong slide cannot silence the check.
plan_path = ROOT.parent / 'kernel/kernel.json'
kernel_path = ROOT.parent / 'kernel/kernel.macho'
verified = None
if plan_path.is_file() and kernel_path.is_file():
    plan = json.loads(plan_path.read_text())
    require(plan['original_sha256'] == original_sha, 'the prelinked kernel was made from another kernelcache')
    extents = [e for e in plan['extents'] if e['module'] == MODULE]
    require(extents, f'the prelinked kernel does not carry {MODULE}')
    slid = {e['segment']: e['va'] for e in extents}
    for segment in module['segments']:
        if segment['name'] in slid:
            require(slid[segment['name']] - segment['va'] == args.slide,
                    f'the prelink slid {segment["name"]} by {slid[segment["name"]] - segment["va"]:#x}')
    prelinked = kernel_path.read_bytes()
    image = np.macho(prelinked)
    length = 96
    at_prelinked = np.backed_offset(image['segments'], constructor + args.slide, length, len(prelinked))
    require(prelinked[at_prelinked:at_prelinked + length] == read(constructor, length),
            'the prelinked kernel does not hold the same constructor at the slid address')
    verified = dict(prelinked_kernel_sha256=hashlib.sha256(prelinked).hexdigest(), constructor_at=hex(constructor + args.slide))

args.output.mkdir(parents=True, exist_ok=True)
assembly = args.output / 'original-bindings.S'
assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n' for name, value in sorted(bindings.items())))

record = dict(module=MODULE, kernelcache=str(path.relative_to(ROOT.parents[0])) if path.is_relative_to(ROOT.parents[0]) else str(path),
              kernelcache_sha256=original_sha, slide=args.slide, source_revision=table['source_revision'],
              slot_table=table['reference_object_sha256'], method_slots=EXPECTED_SLOTS,
              kernel_exported_slots=len(known), kext_owned_slots=owned, pure_virtual_slots=EXPECTED_PURE,
              dispatch_table=hex(vtable), dispatch_table_bound=hex(vtable + args.slide),
              metaclass=hex(metaclass), metaclass_bound=hex(metaclass + args.slide),
              protected_constructor=hex(constructor), protected_constructor_forwards_to='IOService::IOService(OSMetaClass const*)',
              base_destructor=hex(destructor), base_destructor_forwards_to='IOService::~IOService()',
              abstract_destructors_are_traps=True, imports=sorted(undefined), bindings={k: hex(v) for k, v in sorted(bindings.items())},
              prelinked_kernel_verified=verified)
(args.output / 'bindings.json').write_text(json.dumps(record, indent=2) + '\n')
print(f'PASS: {len(bindings)} original storage symbols bound (dispatch table {vtable + args.slide:#x}, '
      f'{len(known)} kernel-exported and {owned} kext-owned slots agree)')
print(f'      {assembly}')
