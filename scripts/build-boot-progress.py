#!/usr/bin/env python3
# From UniXNU-fajita scripts/build-boot-progress.py (BSD-2-Clause-Patent), which is from UniXNU-sheng:
# compiles the boot progress display and its board diagnostic set against the recorded XNU ABI.
"""Build the boot progress display and diagnostics of kits/sirius against the actual source-kernel ABI.

Font.h is vendored in the kit (from Mu-Silicium FrameBufferSerialPortLib), so this does not need
a UEFI checkout. The board is eMMC: the UFS log writer of the sheng set stays unused at runtime.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/boot-progress'
OUT.mkdir(parents=True, exist_ok=True)
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
source = ROOT / 'kits/sirius/BootProgress.c'
obj = OUT / 'BootProgress.o'
command = []
args = record['arguments']
i = 0
while i < len(args):
    arg = args[i]
    if arg == record['file']:
        command.append(str(source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            command += ['-o', str(obj)]
        i += 1
    elif arg == '-Xclang' and args[i+1].startswith('-W'):
        i += 1
    elif arg == '-D' and args[i+1].startswith('XNU_BOOT_PROGRESS='):
        i += 1
    elif not arg.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
        command.append(arg)
    i += 1
command += ['-DXNU_BOOT_PROGRESS=1', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT / '.cache/xnu-work/osfmk'),
            '-I', str(ROOT / 'kits/sirius')]
subprocess.run(command, cwd=record['directory'], check=True, timeout=90)
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')

fault_source = ROOT / 'kits/sirius/BootFaultProbe.c'
fault_obj = OUT / 'BootFaultProbe.o'
fault_command = [str(fault_source) if arg == str(source) else str(fault_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(fault_command, cwd=record['directory'], check=True, timeout=90)

policy_source = ROOT / 'kits/sirius/BootWatchdogPolicy.c'
policy_obj = OUT / 'BootWatchdogPolicy.o'
policy_record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json').read_text().strip().rstrip(','))
policy_command = []
policy_args = policy_record['arguments']
j = 0
while j < len(policy_args):
    arg = policy_args[j]
    if arg == policy_record['file']:
        policy_command.append(str(policy_source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            policy_command += ['-o', str(policy_obj)]
        j += 1
    elif arg == '-Xclang' and policy_args[j+1].startswith('-W'):
        j += 1
    elif not arg.startswith('-W'):
        policy_command.append(arg)
    j += 1
policy_command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror']
subprocess.run(policy_command, cwd=policy_record['directory'], check=True, timeout=90)

diag_source = ROOT / 'kits/sirius/BootDiagExport.c'
diag_obj = OUT / 'BootDiagExport.c.o'
diag_command = [str(diag_source) if arg == str(policy_source) else str(diag_obj) if arg == str(policy_obj) else arg for arg in policy_command]
subprocess.run(diag_command, cwd=policy_record['directory'], check=True, timeout=90)

stub_source = ROOT / 'kits/sirius/BootProgressStubs.c'
stub_obj = OUT / 'BootProgressStubs.o'
stub_command = [str(stub_source) if arg == str(policy_source) else str(stub_obj) if arg == str(policy_obj) else arg for arg in policy_command]
subprocess.run(stub_command, cwd=policy_record['directory'], check=True, timeout=90)

# The fastboot reset (sheng-autoreset=<seconds>): a plain PSCI reset. BootLog.c (the UFS log
# writer) is not compiled: this board has no UFS.
reset_source = ROOT / 'kits/sirius/BootReset.c'
reset_obj = OUT / 'BootReset.o'
reset_command = [str(reset_source) if arg == str(source) else str(reset_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(reset_command, cwd=record['directory'], check=True, timeout=90)

archive = OUT / 'libBootProgress.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(fault_obj), str(policy_obj), str(diag_obj), str(stub_obj), str(reset_obj)], check=True, timeout=30)
(OUT / 'build-report.json').write_text(json.dumps({
    'watchdog_policy_sha256': hashlib.sha256(policy_source.read_bytes()).hexdigest(),
    'diag_sha256': hashlib.sha256(diag_source.read_bytes()).hexdigest(),
    'fault_sha256': hashlib.sha256(fault_source.read_bytes()).hexdigest(),
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'lifecycle_sha256': hashlib.sha256((source.parent / 'BootProgressLifecycle.h').read_bytes()).hexdigest(),
    'core_sha256': hashlib.sha256((source.parent / 'BootProgressCore.h').read_bytes()).hexdigest(),
    'display_sha256': hashlib.sha256((source.parent / 'BootProgressDisplay.h').read_bytes()).hexdigest(),
    'pulse_sha256': hashlib.sha256((source.parent / 'BootProgressPulse.h').read_bytes()).hexdigest(),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'hardware_execution': False,
}, indent=2) + '\n')
print('PASS: strict source-kernel ARM64 boot diagnostic build')
