#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build and sign the version-locked arm64e native capability adapter."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import importlib

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=root/'out/userland')
parser.add_argument('--sdk', type=Path)
parser.add_argument('--os', choices=['ios27', 'macos27'], default='ios27', help='the daemon the library is built for')
args = parser.parse_args()
MACOS = args.os == 'macos27'
native = importlib.import_module('native_macos27' if MACOS else 'native_ios27')
layout_header, ORIGINAL_SHA256, ORIGINAL_UUID = native.layout_header, native.ORIGINAL_SHA256, native.ORIGINAL_UUID
# iPadOS: /usr/lib through the addon's union. macOS: the path the adapter's load command names, in the addon itself.
HEADER = 'qbt_macos27_layout.h' if MACOS else 'qbt_ios27_layout.h'
INSTALL = native.NATIVE_DYLIB.decode() if MACOS else '/usr/lib/QcaBTType0.dylib'
SDK, TARGET = ('macosx', 'arm64e-apple-macos27.0') if MACOS else ('iphoneos', 'arm64e-apple-ios27.0')
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out/HEADER).write_text(layout_header())
sdk = str(args.sdk or subprocess.check_output(['xcrun','--sdk',SDK,'--show-sdk-path'], text=True).strip())
binary = out/'QcaBTType0.dylib'
source = root/'userland/Type0.c'
command = ['xcrun','--sdk',SDK,'clang','-target',TARGET,'-isysroot',sdk,
           '-dynamiclib','-install_name',INSTALL,'-O2','-Wall','-Wextra','-Werror',
           '-fvisibility=hidden','-fstack-protector-strong','-I',str(out)] + \
          (['-DQBT_LAYOUT_HEADER="' + HEADER + '"'] if MACOS else []) + [str(source),'-o',str(binary)]
subprocess.run(command, check=True)
subprocess.run(['codesign','--force','--sign','-','--pagesize','4096','--identifier',
                'org.unixnu.bluetooth.Type0',str(binary)], check=True)
subprocess.run(['codesign','--verify','--strict',str(binary)], check=True)
detail = subprocess.run(['codesign','--display','--verbose=4',str(binary)],text=True,capture_output=True,check=True).stderr
cdhash = re.search(r'^CDHash=([0-9a-f]{40})$',detail,re.M)
if not cdhash:raise SystemExit('Native library has no CDHash')
report = dict(built=True,target=TARGET,hardware_verified=False,
              native_input_sha256=ORIGINAL_SHA256,native_uuid=ORIGINAL_UUID,cdhash=cdhash[1],command=command,
              inputs={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
                      [source,root/'tools'/(native.__name__ + '.py')]},
              files={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [binary,out/HEADER]})
(out/'type0-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'Built signed arm64e native capability adapter: {binary}; CDHash {cdhash[1]}')
