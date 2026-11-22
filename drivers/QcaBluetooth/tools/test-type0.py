#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Test the production native capability publisher against target descriptors."""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import subprocess
from native_ios27 import NativeImage, DESCRIPTORS, DESCRIPTOR_COUNT, layout_header

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('image', type=Path)
parser.add_argument('--output', type=Path, default=root/'out/type0-test')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
image = NativeImage(args.image)
(out/'qbt_ios27_layout.h').write_text(layout_header())
offset = image.offset(DESCRIPTORS)
fixture = out/'native-descriptors.bin'
fixture.write_bytes(image.original[offset:offset+DESCRIPTOR_COUNT*24])
binary = out/'native-type0-test'
command = ['xcrun','clang','-O1','-g','-Wall','-Wextra','-Werror','-fno-omit-frame-pointer',
           '-fsanitize=address,undefined','-DQBT_TYPE0_TEST','-I',str(out),
           str(root/'tests/native_type0.c'),'-o',str(binary)]
subprocess.run(command, check=True)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=' + ('0' if platform.system() == 'Darwin' else '1'), UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
result = subprocess.run([str(binary),str(fixture)],env=env,text=True,capture_output=True)
print(result.stdout,end='')
if result.returncode:
    raise SystemExit(result.stderr or f'Native capability test exited {result.returncode}')
report = dict(passed=True,sanitizers=['address','undefined'],hardware_verified=False,
              native_sha256=hashlib.sha256(image.original).hexdigest(),command=command,
              result=result.stdout.strip(),inputs={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
              for p in [root/'userland/Type0.c',root/'tests/native_type0.c',root/'tools/native_ios27.py']})
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
