#!/usr/bin/env python3
"""Derive the original IOStorageFamily addresses a built-in block driver binds to.

IOStorageFamily is a kext of the original kernelcache, not part of the XNU source, so an
archive that subclasses IOBlockStorageDevice leaves undefined symbols the kernel link cannot
resolve. The prelink keeps every component at a constant slide, so the addresses the driver
needs are the addresses of the original kernelcache plus that slide.

This script derives them from the pinned kernelcache and states the evidence for each one:

  * the public vtable slot order (scripts/data/storage-vtable-names.json, pinned to the
    IOStorageFamily source revision) names every slot;
  * the dispatch table is the only one in the kext __DATA_CONST where all 176 slots whose name
    the kernel exports hold that export, and the other 14 slots are the abstract ones the kext
    replaces with its own BRK traps;
  * the protected constructor is the single function that stores table+16, and its branch target
    resolves through a loader thunk to the actual IOService constructor;
  * the base destructor is the branch the deleting destructor takes, resolving to IOService's;
  * the metaclass is the constant the getMetaClass getter materialises.

--write records the result in scripts/data/storage-bindings.json, which is what the build uses:
the archive step runs before the prelink, so a cold tree has no kernelcache yet. --check compares
the record against a kernelcache and is what a warm tree or a CI job runs after the prelink to
prove the record still matches the pinned image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np

MODULE = 'com.apple.iokit.IOStorageFamily'
EXPECTED_SLOTS = 190
EXPECTED_PURE = 14
EXPECTED_KERNEL_BINDINGS = 176

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernelcache', type=Path, help='original kernelcache the prelink reads')
parser.add_argument('--slide', type=lambda value: int(value, 0), help='prelink slide; from the plan when present')
parser.add_argument('--table', type=Path, default=ROOT / 'scripts/data/storage-vtable-names.json')
parser.add_argument('--record', type=Path, default=ROOT / 'scripts/data/storage-bindings.json')
parser.add_argument('--output', type=Path, default=ROOT / 'build/native-emmc')
parser.add_argument('--write', action='store_true', help='record the derivation for the build to use')
parser.add_argument('--check', action='store_true', help='compare the record with this kernelcache')
args = parser.parse_args()


def require(value, message):
    if not value:
        raise SystemExit('derive-storage-bindings: ' + message)


def plan():
    path = ROOT.parent / 'kernel/kernel.json'
    return json.loads(path.read_text()) if path.is_file() else None


def kernelcache_path():
    if args.kernelcache:
        return args.kernelcache
    known = plan()
    found = sorted(ROOT.parents[3].glob('ipsw/*/kernelcache.decompressed'))
    require(found, 'no kernelcache under out/ipsw; pass --kernelcache (the archive step runs before the '
                   'prelink, so a cold tree only has the recorded addresses)')
    if known:
        for path in found:
            if hashlib.sha256(path.read_bytes()).hexdigest() == known['original_sha256']:
                return path
    require(len(found) == 1, 'several kernelcaches and no plan to choose by; pass --kernelcache')
    return found[0]


def slide():
    if args.slide is not None:
        return args.slide
    known = plan()
    require(known, 'no prelink plan; pass --slide')
    return known['slide']


def derive():
    table = json.loads(args.table.read_text())
    methods = table['methods']
    require(len(methods) == EXPECTED_SLOTS and methods.count('___cxa_pure_virtual') == EXPECTED_PURE,
            'the recorded vtable slot table is not the pinned one')

    path = kernelcache_path()
    kernel = path.read_bytes()
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
        offset = np.backed_offset(segments, address, length, len(kernel))
        return kernel[offset:offset + length]

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
    for offset in range(0, len(blob) - (EXPECTED_SLOTS + 2) * 8 + 1, 8):
        if blob[offset:offset + 16] != bytes(16):
            continue
        pointers = struct.unpack_from('<' + 'Q' * EXPECTED_SLOTS, blob, offset + 16)
        if all(pointers[slot] == by_name[name] for slot, name in enumerate(methods) if name in by_name):
            candidates.append((constant['va'] + offset, pointers))
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
    for offset in range(code['va'], code['va'] + code['size'] - 32, 4):
        words = struct.unpack('<8I', read(offset, 32))
        if (words[:2] != (0xa9bf7bfd, 0x910003fd) or words[2] & 0xfc000000 != 0x94000000 or
                words[3] & 0x9f00001f != 0x90000008 or words[4] & 0xffc003ff != 0x91000108 or
                words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0)):
            continue
        if page_target(offset + 12, words[3], words[4]) == vtable + 16:
            constructors.append((offset, relative_branch(offset + 8, words[2])))
    require(len(constructors) == 1, f'{len(constructors)} constructors store the storage dispatch table')
    constructor, constructor_branch = constructors[0]
    destructor = pointers[0] - 4
    branch = struct.unpack('<I', read(destructor, 4))[0]
    require(branch & 0xfc000000 == 0x14000000, 'the base destructor is not a branch')

    def thunk_targets(address):
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
        if name.startswith(('__ZN20IOBlockStorageDevice', '__ZNK20IOBlockStorageDevice')):
            require(inside_code(pointers[slot]), f'slot {slot} {name} is not inside the kext code')
            addresses[name] = pointers[slot]

    return dict(module=MODULE, kernelcache=str(path), kernelcache_sha256=hashlib.sha256(kernel).hexdigest(),
                slide=slide(), source_revision=table['source_revision'],
                slot_table=table['reference_object_sha256'],
                method_slots=EXPECTED_SLOTS, kernel_exported_slots=len(known), kext_owned_slots=owned,
                pure_virtual_slots=EXPECTED_PURE, dispatch_table=hex(vtable), metaclass=hex(metaclass),
                protected_constructor=hex(constructor),
                protected_constructor_forwards_to='IOService::IOService(OSMetaClass const*)',
                base_destructor=hex(destructor), base_destructor_forwards_to='IOService::~IOService()',
                abstract_destructors_are_traps=True,
                addresses={name: hex(address) for name, address in sorted(addresses.items())})


record = derive()
if args.check:
    stored = json.loads(args.record.read_text())
    differences = [key for key in ('kernelcache_sha256', 'slide', 'dispatch_table', 'metaclass',
                                   'protected_constructor', 'base_destructor', 'addresses')
                   if stored.get(key) != record[key]]
    require(not differences, f'{args.record} does not match this kernelcache: ' + ', '.join(differences))
    print(f'PASS: {args.record.name} matches {record["kernelcache_sha256"][:16]} at slide {record["slide"]:#x}')
    sys.exit(0)

if args.write:
    args.record.write_text(json.dumps(record, indent=2) + '\n')
    print(f'PASS: recorded {len(record["addresses"])} original storage addresses in {args.record}')

args.output.mkdir(parents=True, exist_ok=True)
(args.output / 'derive-report.json').write_text(json.dumps(record, indent=2) + '\n')
print(f'PASS: dispatch table {record["dispatch_table"]} ({record["kernel_exported_slots"]} kernel-exported and '
      f'{record["kext_owned_slots"]} kext-owned slots agree), {len(record["addresses"])} addresses at '
      f'slide {record["slide"]:#x}')
