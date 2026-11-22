#!/usr/bin/env python3
"""Exercise the production SM7675 UART10 clock, SMMU bypass and WCN6750 firmware filter with a register model."""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[1]
pkg = root / 'package'
out = root / 'drivers/QcaBluetooth/out/platform-test'
out.mkdir(parents=True, exist_ok=True)
sources = [pkg / 'Sm7675GeniUart.c', pkg / 'BluetoothFirmware.c', pkg / 'tests/bluetooth_platform.c']
subprocess.run(
    ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
     '-fsanitize=address,undefined', '-I' + str(pkg), *map(str, sources),
     '-o', str(out / 'platform')],
    check=True,
)
result = subprocess.run([out / 'platform'], text=True, capture_output=True)
(out / 'output.txt').write_text(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit(result.stdout + result.stderr)
(out / 'report.json').write_text(json.dumps(dict(
    passed=True,
    registers_simulated=True,
    hardware_verified=False,
    inputs={str(s): hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},
    output=result.stdout,
), indent=2) + '\n')
print(result.stdout, end='')
