#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build signed BlueTool, native bluetoothd and its matching capability library."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import importlib

root = Path(__file__).resolve().parents[1]

def signature(path):
    subprocess.run(['codesign','--verify','--strict',str(path)],check=True)
    detail = subprocess.run(['codesign','--display','--verbose=4',str(path)],check=True,
                            capture_output=True,text=True).stderr
    cdhash = re.search(r'^CDHash=([0-9a-f]{40})$',detail,re.M)
    identifier = re.search(r'^Identifier=(.+)$',detail,re.M)
    if not cdhash or not identifier:raise ValueError('Missing code signature identity: '+path.name)
    return dict(cdhash=cdhash[1],identifier=identifier[1])

def entitlements(path):
    result = subprocess.run(['codesign','--display','--entitlements',':-',str(path)],
                            check=True,capture_output=True)
    return plistlib.loads(result.stdout)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    parser.add_argument('--bluetoothd',type=Path,required=True)
    parser.add_argument('--os',choices=['ios27','macos27'],default='ios27')
    args=parser.parse_args()
    macos=args.os=='macos27'
    native=importlib.import_module('native_macos27' if macos else 'native_ios27')
    NativeImage,ORIGINAL_SHA256,ORIGINAL_UUID,NATIVE_DYLIB=native.NativeImage,native.ORIGINAL_SHA256,native.ORIGINAL_UUID,native.NATIVE_DYLIB
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    if macos:
        # macOS ships a universal daemon; the adapter is for its arm64e slice, which keeps its own signature.
        thin=out/'bluetoothd.arm64e'
        subprocess.run(['lipo',str(args.bluetoothd),'-thin','arm64e','-output',str(thin)],check=True)
        args.bluetoothd=thin
    image=NativeImage(args.bluetoothd)
    original_entitlements=entitlements(args.bluetoothd)
    original_signature=signature(args.bluetoothd)
    (out/'report.json').unlink(missing_ok=True)
    build=out/'build';build.mkdir(exist_ok=True)
    subprocess.run([sys.executable,root/'tools/build-bluetool.py','--output',build,'--os',args.os],check=True)
    subprocess.run([sys.executable,root/'tools/build-type0.py','--output',build,'--os',args.os],check=True)
    for name in ('bin','lib','config'):(out/name).mkdir(exist_ok=True)
    for source,dest in ((build/'BlueTool',out/'bin/BlueTool'),
                        (build/'QcaBTType0.dylib',out/'lib/QcaBTType0.dylib')):
        shutil.copyfile(source,dest);dest.chmod(0o755)
    job=plistlib.loads((build/'com.apple.BlueTool.plist').read_bytes())
    (out/'config/com.apple.BlueTool.plist').write_bytes(plistlib.dumps(job))
    patched=image.vendor_routing().volatile_root_keys().h4_shutdown().capability_adapter()
    daemon=out/'bin/bluetoothd';daemon.write_bytes(patched.data);daemon.chmod(0o755)
    subprocess.run(['codesign','--force','--sign','-','--pagesize','4096',
                    '--preserve-metadata=identifier,entitlements,flags',str(daemon)],check=True)
    current_entitlements=entitlements(daemon)
    signed=signature(daemon)
    if current_entitlements!=original_entitlements or signed['identifier']!=original_signature['identifier']:
        raise ValueError('Native bluetoothd entitlements or identifier changed')
    # Signing can grow __LINKEDIT; code/data addresses and UUID remain fixed.
    from native_ios27 import _inspect
    final=_inspect.Image(daemon)
    if final.uuid!=ORIGINAL_UUID or final.subtype!=0x80000002:
        raise ValueError('Signed native image UUID or arm64e ABI changed')
    if final.data.count(NATIVE_DYLIB+b'\0')!=1:
        raise ValueError('Native capability library load command differs')
    files={}
    for relative in ('bin/BlueTool','bin/bluetoothd','lib/QcaBTType0.dylib'):
        path=out/relative
        files[relative]=dict(signed if relative=='bin/bluetoothd' else signature(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),bytes=path.stat().st_size)
    report=dict(built=True,target='arm64e-apple-macos27.0' if macos else 'arm64e-apple-ios27.0',hardware_verified=False,
                native_input_sha256=ORIGINAL_SHA256,native_uuid=ORIGINAL_UUID,
                native_entitlements_preserved=True,native_entitlement_count=len(original_entitlements),
                files=files,launchd_job_overrides={} if macos else {'com.apple.BlueTool':{
                    key:job[key] for key in ('UserName','EnableTransactions','EnablePressuredExit','ThrottleInterval')}},
                patch=patched.report())
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Built signed native Bluetooth userland: '+str(out))

if __name__=='__main__':main()
