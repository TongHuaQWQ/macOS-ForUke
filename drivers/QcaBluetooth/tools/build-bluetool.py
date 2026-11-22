#!/usr/bin/env python3
"""Build and sign the arm64e BlueTool XPC service for the Data addon."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, default=root/'out/userland')
p.add_argument('--sdk', type=Path)
p.add_argument('--os', choices=['ios27', 'macos27'], default='ios27')
a = p.parse_args()
SDK, TARGET = ('macosx', 'arm64e-apple-macos27.0') if a.os == 'macos27' else ('iphoneos', 'arm64e-apple-ios27.0')
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
sdk = str(a.sdk or subprocess.check_output(['xcrun', '--sdk', SDK, '--show-sdk-path'], text=True).strip())
source = root/'userland/BlueTool.c'
binary = out/'BlueTool'
cc = ['xcrun', '--sdk', SDK, 'clang', '-target', TARGET, '-isysroot', sdk,
      '-O2', '-Wall', '-Wextra', '-Werror', '-fblocks', '-fstack-protector-strong', str(source), '-o', str(binary)]
subprocess.run(cc, check=True)
entitlements = out/'BlueTool.entitlements.plist'
entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier',
                'org.unixnu.bluetooth.BlueTool', '--entitlements', str(entitlements), str(binary)], check=True)
subprocess.run(['codesign', '--verify', '--strict', str(binary)], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', str(binary)], text=True, capture_output=True, check=True).stderr
cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
if not cdhash:
    raise SystemExit('Signed BlueTool has no CDHash')
job = {'Label': 'com.apple.BlueTool', 'ProgramArguments': ['/usr/sbin/BlueTool', '-R'],
       'MachServices': {'com.apple.BlueTool': True}, 'UserName': 'root',
       'POSIXSpawnType': 'Interactive', 'EnableTransactions': True, 'EnablePressuredExit': False, 'ThrottleInterval': 3}
(out/'com.apple.BlueTool.plist').write_bytes(plistlib.dumps(job))
(out/'report.json').write_text(json.dumps(dict(built=True, target=TARGET,
    hardware_verified=False, cdhash=cdhash[1], inputs={'BlueTool.c': hashlib.sha256(source.read_bytes()).hexdigest()},
    files={f:hashlib.sha256((out/f).read_bytes()).hexdigest() for f in ('BlueTool','com.apple.BlueTool.plist')},
    command=cc), indent=2)+'\n')
print('Built signed arm64e BlueTool: '+str(binary)+'; CDHash '+cdhash[1])
