#!/usr/bin/env python3
"""Build libQcaBluetooth.a: QcaBluetooth + SM7675 UART10 + WCN6750 power + BluetoothProbe."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/qca-bluetooth'
RECORDS = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
BSD_RECORDS = (f'{RECORDS}/bsd/DEVELOPMENT/kern_exec.o.json',
               f'{RECORDS}/bsd/DEVELOPMENT/bsd_init.o.json',
               f'{RECORDS}/osfmk/DEVELOPMENT/startup.o.json')
C_RECORDS = (f'{RECORDS}/osfmk/DEVELOPMENT/startup.o.json',
             f'{RECORDS}/iokit/DEVELOPMENT/IOPlatformExpert.o.json',
             f'{RECORDS}/bsd/DEVELOPMENT/bsd_init.o.json')
CXX_RECORDS = (f'{RECORDS}/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json',
               f'{RECORDS}/iokit/DEVELOPMENT/IOPlatformExpert.o.json',
               f'{RECORDS}/libkern/DEVELOPMENT/OSObject.cpo.json',
               f'{RECORDS}/libkern/c++/DEVELOPMENT/OSObject.cpo.json',
               f'{RECORDS}/osfmk/DEVELOPMENT/startup.cpo.json')
EXPORTS = ('sheng_bluetooth_probe', 'sheng_bluetooth_hci_dump',
           'sheng_wcn_power_get', 'sheng_wcn_power_put', 'sheng_wcn_power_owners',
           'qbt_transport_init', 'native_bluetooth_stats')


def run(command, **kw):
    return subprocess.run([str(c) for c in command], check=True, **kw)


def capture(command, **kw):
    return subprocess.run([str(c) for c in command], check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, **kw).stdout


def tool(name):
    found = shutil.which(name)
    if found:
        return Path(found)
    for probe in (['xcrun', '-f', name], ['brew', '--prefix', 'llvm']):
        try:
            out = capture(probe).strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            continue
        candidate = Path(out) / name if probe[0] == 'brew' else Path(out)
        if candidate.is_file():
            return candidate
    for base in ('/opt/homebrew/opt/llvm/bin', '/usr/local/opt/llvm/bin',
                 '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin'):
        candidate = Path(base) / name
        if candidate.is_file():
            return candidate
    raise SystemExit(f'{name} not found')


def resolve_module(relative, marker):
    device = os.environ.get('UNIXNU_DEVICE')
    bases = [ROOT] + ([Path(device)] if device else []) + [ROOT / 'device']
    for base in bases:
        candidate = base / relative
        if (candidate / marker).is_file():
            return candidate.resolve()
    if relative.startswith('kits/'):
        for base in bases:
            candidate = base / 'package'
            if (candidate / marker).is_file():
                return candidate.resolve()
    raise SystemExit(f'{relative}: {marker} not found under {ROOT}')


def load_record(candidates, kind):
    for rel in candidates:
        path = ROOT / rel
        if path.is_file():
            text = path.read_text().strip().rstrip(',')
            entries = json.loads(text if text.startswith('[') else f'[{text}]')
            return entries[0]
    raise SystemExit(f'No {kind} compile record found (tried {", ".join(candidates)})')


def compile_unit(source, record, obj, extra, language):
    args = list(record['arguments'])
    out = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg in ('-o', '-MF', '-MT', '-MQ', '-c', '-x') or arg.startswith('-Wp,-MMD,'):
            if arg in ('-o', '-MF', '-MT', '-MQ', '-x'):
                i += 2
            else:
                i += 1
            continue
        if arg in ('-MMD', '-MD', '-MP'):
            i += 1
            continue
        if arg == record.get('file') or (not arg.startswith('-') and (arg.endswith('.c') or arg.endswith('.cpp') or arg.endswith('.s') or arg.endswith('.S'))):
            i += 1
            continue
        out.append(arg)
        i += 1
    out += ['-x', language, '-c', str(source), '-o', str(obj), *extra]
    (obj.with_suffix('.command.json')).write_text(json.dumps(out, indent=2) + '\n')
    result = subprocess.run(out, cwd=record['directory'], text=True, capture_output=True)
    (obj.with_suffix('.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(f'{source.name} failed:\n' + result.stdout + result.stderr)


def defined_symbols(archive):
    defined = set()
    for line in capture([tool('llvm-nm'), '--defined-only', '-g', archive]).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in ('T', 'D', 'B', 'S', 'R', 'C', 'W'):
            name = parts[-1]
            defined.add(name[1:] if name.startswith('_') else name)
    return defined


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true',
                        help='verify source layout and QcaBluetooth Linux source lock without Xcode')
    parser.add_argument('--target', choices=('macosx', 'ios', 'iphoneos'),
                        default=os.environ.get('UNIXNU_XNU_TARGET', 'macosx'))
    args = parser.parse_args()

    component = resolve_module('drivers/QcaBluetooth', 'include/qbt.h')
    kits = resolve_module('kits/uke', 'BluetoothProbe.cpp')
    front = 'src/qbt_tty.c' if args.target == 'macosx' else 'src/qbt_transport.c'

    bsd_sources = [component / f for f in ('src/qbt_core.c', 'src/qbt_linux.c', 'src/qbt_geni.c', front)]
    # BluetoothFirmware.c is compiled into libShengUfs.a by build-sheng-source-storage.py (matching upstream sheng),
    # so only the UART clock RCG unit belongs in libQcaBluetooth.a to avoid duplicate symbols at kernel link time.
    board_c_sources = [kits / 'Sm7675GeniUart.c']
    osfmk_c_sources = [kits / 'Sm7675BtGcc.c']
    cxx_sources = [kits / 'WcnPower.cpp', kits / 'BluetoothProbe.cpp']

    if args.check:
        for path in bsd_sources + board_c_sources + osfmk_c_sources + cxx_sources:
            if not path.is_file():
                raise SystemExit(f'Missing source file: {path}')
        if not (component / 'tools/source-lock.json').is_file():
            raise SystemExit('Missing QcaBluetooth tools/source-lock.json')
        print(f'QcaBluetooth check: OK (component={component}, kits={kits}, front={front})')
        return

    OUT.mkdir(parents=True, exist_ok=True)
    run([sys.executable, component / 'tools/prepare-linux.py'])
    bsd_sources.append(component / 'out/linux/btqca.c')

    bsd_record = load_record(BSD_RECORDS, 'BSD C')
    c_record = load_record(C_RECORDS, 'osfmk C')
    cxx_record = load_record(CXX_RECORDS, 'IOKit C++')

    kernel_includes = []
    for base in (ROOT / '.cache/xnu-work', ROOT):
        for name in ('osfmk', 'iokit', 'libkern', 'bsd'):
            if (base / name).is_dir():
                kernel_includes += ['-I', str(base / name)]

    bt_includes = [
        '-I', str(component / 'include'),
        '-I', str(component / 'src'),
        '-I', str(component / 'shim'),
        '-I', str(component / 'out/linux'),
        '-I', str(kits),
    ]
    objects = []
    for source in bsd_sources + board_c_sources:
        obj = OUT / (source.stem + '.o')
        compile_unit(source, bsd_record, obj,
                     kernel_includes + bt_includes + ['-DKERNEL', '-Wno-sign-compare'], 'c')
        objects.append(obj)
    for source in osfmk_c_sources:
        obj = OUT / (source.stem + '.o')
        compile_unit(source, c_record, obj,
                     kernel_includes + bt_includes + ['-DKERNEL'], 'c')
        objects.append(obj)
    for source in cxx_sources:
        obj = OUT / (source.stem + '.o')
        compile_unit(source, cxx_record, obj,
                     kernel_includes + bt_includes + ['-DKERNEL'], 'c++')
        objects.append(obj)

    archive = OUT / 'libQcaBluetooth.a'
    archive.unlink(missing_ok=True)
    run([tool('llvm-ar'), 'rcs', archive, *objects])
    defined = defined_symbols(archive)
    missing = [name for name in EXPORTS if name not in defined]
    if missing:
        raise SystemExit(f'{archive.name} does not define {", ".join(missing)}')

    report = {
        'built': True,
        'target': args.target,
        'front': front,
        'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        'archive_bytes': archive.stat().st_size,
        'objects': {o.name: hashlib.sha256(o.read_bytes()).hexdigest() for o in objects},
        'source': {str(s): hashlib.sha256(s.read_bytes()).hexdigest()
                   for s in bsd_sources + board_c_sources + osfmk_c_sources + cxx_sources},
        'exports': list(EXPORTS),
    }
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    for obj in objects:
        print('OBJ ' + str(obj))
    print(f'QcaBluetooth: {len(objects)} objects, {archive.stat().st_size} bytes, sha {report["archive_sha256"][:16]}')


if __name__ == '__main__':
    main()
