#!/usr/bin/env python3
"""Stage signed Bluetooth services and pinned WCN6750 firmware for the Data addon."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('target', choices=('ios27', 'macos27'))
parser.add_argument('--os-root', type=Path, required=True)
parser.add_argument('--component', type=Path, help='resolved QcaBluetooth checkout')
parser.add_argument('--firmware-source', type=Path, default=root / '.cache/firmware/qca')
parser.add_argument('--btfm', type=Path, default=None, help='optional BTFM.bin image')
args = parser.parse_args()

out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'report.json').unlink(missing_ok=True)
device = os.environ.get('UNIXNU_DEVICE')
candidates = []
if args.component:
    candidates.append(args.component)
if os.environ.get('QCA_BLUETOOTH_DIR'):
    candidates.append(Path(os.environ['QCA_BLUETOOTH_DIR']))
candidates.append(root / 'drivers/QcaBluetooth')
if device:
    candidates.append(Path(device) / 'drivers/QcaBluetooth')
component = next((c.resolve() for c in candidates if (c / 'include/qbt.h').is_file()), None)
if component is None:
    parser.error('QcaBluetooth module directory missing; tried ' + ', '.join(str(c) for c in candidates))

userland = out / 'userland'
subprocess.run([sys.executable, component / 'tools/build-userland.py', userland,
                '--bluetoothd', args.os_root / 'usr/sbin/bluetoothd', '--os', args.target], check=True)

stage_cmd = [sys.executable, root / 'scripts/stage-bluetooth-firmware.py', out,
             '--source', args.firmware_source, '--fetch']
if args.btfm:
    stage_cmd += ['--btfm', args.btfm]
subprocess.run(stage_cmd, check=True)

firmware = json.loads((out / 'bluetooth-firmware.json').read_text())
report = json.loads((userland / 'report.json').read_text())
# macOS mounts /private/var/ngpu/{sbin,lib} over /usr/{sbin,lib} (macos-addon-sbin.patch);
# iPadOS uses bluetooth-bin and bluetooth-lib.
names = ('sbin', 'lib') if args.target == 'macos27' else ('bluetooth-bin', 'bluetooth-lib')
report['directories'] = {names[0]: 'userland/bin', names[1]: 'userland/lib',
                         firmware['firmware_prefix']: firmware['firmware_prefix']}
report['firmware'] = firmware
marker = dict(native_input_sha256=report['native_input_sha256'],
              files={name: item['sha256'] for name, item in report['files'].items()})
(userland / 'bin/QcaBluetooth.runtime').write_text(json.dumps(marker, sort_keys=True) + '\n')
report['runtime_marker_sha256'] = hashlib.sha256((userland / 'bin/QcaBluetooth.runtime').read_bytes()).hexdigest()
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print('Staged Bluetooth runtime: ' + str(out))
