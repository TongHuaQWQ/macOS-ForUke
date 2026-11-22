#!/usr/bin/env python3
"""Stage pinned uke WCN6750 Bluetooth firmware under the kernel's Data addon prefix."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import urllib.request

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--source', type=Path, default=root / '.cache/firmware/qca')
p.add_argument('--btfm', type=Path, default=None,
               help='optional Xiaomi Pad 7 BTFM.bin FAT image to extract missing firmware files from')
p.add_argument('--fetch', action='store_true',
               help='fetch missing inputs when the manifest declares a repository and commit')
a = p.parse_args()

manifest = json.loads((root / 'firmware/bluetooth/manifest.json').read_text())
prefix = re.search(r'^#define SHENG_BT_FIRMWARE_PREFIX "([^"]+)"$',
                   (root / 'package/BluetoothFirmware.h').read_text(), re.M).group(1)
staged = {}
for name, expected in manifest['files'].items():
    if not re.fullmatch(r'msbtfw[0-9a-f]{2}\.(mbn|tlv)|msnv[0-9a-f]{2}\.(bin|b[0-9a-f]{2,4})', name):
        raise SystemExit('Unsupported Bluetooth firmware name: ' + name)
    source = a.source / name
    fetched = False
    if not source.exists() and a.btfm and a.btfm.is_file():
        with tempfile.TemporaryDirectory() as td:
            extracted = Path(td) / name
            subprocess.run(['mcopy', '-i', str(a.btfm), f'::/image/{name}', str(extracted)], check=True)
            data = extracted.read_bytes()
        fetched = True
    elif not source.exists() and a.fetch and manifest.get('repository') and manifest.get('commit'):
        url = manifest['repository'].replace('github.com', 'raw.githubusercontent.com') + '/' + manifest['commit'] + '/qca/' + name
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read()
        fetched = True
    else:
        if not source.exists():
            raise SystemExit(f'Missing Bluetooth firmware file {source} (pass --source DIR or --btfm BTFM.bin)')
        data = source.read_bytes()
    if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
        raise SystemExit('Bluetooth firmware identity mismatch: ' + name)
    if fetched:
        source.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=source.parent, prefix=name + '.', delete=False) as cache:
            temporary = Path(cache.name)
            cache.write(data)
        try:
            os.replace(temporary, source)
        finally:
            temporary.unlink(missing_ok=True)
    staged[name] = data

destination = a.output.resolve() / prefix
destination.mkdir(parents=True, exist_ok=True)
for name, data in staged.items():
    (destination / name).write_bytes(data)
report = dict(staged=True, firmware_prefix=prefix, files=manifest['files'],
              device=manifest.get('device', 'uke'), chipset=manifest.get('chipset', 'WCN6750'),
              source=manifest.get('source', ''), hardware_verified=False)
(a.output / 'bluetooth-firmware.json').write_text(json.dumps(report, indent=2) + '\n')
print('Staged Bluetooth firmware: ' + str(destination))
