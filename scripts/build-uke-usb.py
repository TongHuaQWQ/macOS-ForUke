#!/usr/bin/env python3
"""Compile the uke USB console driver against the recorded BSD ABI and archive it.

Ported from Yooglery/UniXNU-sirius scripts/build-sirius-usb.py. Two differences: the source lives in
this repository's package/ (the kit is mounted as kits/uke in the build tree), and the object joins
libUkeLastLog.a, which build-xnu-4k-baseline.sh force-loads under CONFIG_XNU_BOOT_LOG=y — this
configuration has CONFIG_XNU_SHENG_PMGR off, so the sirius archive hook does not exist here.

The driver is built as an external archive member because a file added by a patch is not picked up by
the makefile generated from bsd/conf/files.
"""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/uke-usb'
OUT.mkdir(parents=True, exist_ok=True)
source = ROOT / 'kits/uke/uke_usb.c'
if not source.is_file():
    source = ROOT / 'package/uke_usb.c'
obj = OUT / 'uke_usb.o'
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
archive = ROOT / 'build/uke-last-log/libUkeLastLog.a'
if not archive.is_file():
    raise SystemExit(f'{archive} missing: build-boot-log.py must run first (board.toml archives order)')
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
print(f'UkeUsb: added {obj.name} to {archive}')
