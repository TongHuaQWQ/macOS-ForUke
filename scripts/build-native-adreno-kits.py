#!/usr/bin/env python3
"""Add the GMU kit and firmware planner objects to the NativeAdreno archive."""
import hashlib, json, os, subprocess, sys
from pathlib import Path
from kernel_target import TRIPLE, check_object, native_arguments
ROOT = Path(__file__).resolve().parents[1]
if os.environ.get('XNU_NATIVE_ADRENO', '0') != '1' and '--force' not in sys.argv[1:]:
    print('SKIP: NATIVE_ADRENO is off for this configuration')
    raise SystemExit(0)
for header in ('ShengGmuSource.h', 'ShengGmuVotesSource.h', 'ShengGmuCommandDbSource.h'):
    if not (ROOT / 'build/sheng-gmu' / header).is_file():
        sys.exit('build/sheng-gmu/%s is missing; run prepare-uke-gmu.py first' % header)
c_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
cxx_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
out = ROOT / 'build/adreno740'
out.mkdir(parents=True, exist_ok=True)
sources = [ROOT / 'kits/uke' / name for name in
           ('ShengGmu.c', 'ShengGmuVotes.c', 'ShengGmuCommandDb.c')]
sources += [ROOT / 'kits/AdrenoFirmware' / name for name in
            ('AdrenoHfi.c', 'AdrenoHfiStartup.c', 'AdrenoHfiVotes.c', 'AdrenoFirmware.c')]
# The SMMU context/stream/live-table kit and the PM4 packet encoder the driver calls into.
sources += sorted(p for p in (ROOT / 'kits/ArmSmmu4K').glob('ArmSmmu4K*.c'))
sources += [ROOT / 'kits/AdrenoPM4/AdrenoPM4.c']
trace_hash = hashlib.sha256()
for path in sorted((ROOT / 'drivers/Adreno740').glob('Native*')):
    if path.is_file():
        trace_hash.update(path.name.encode())
        trace_hash.update(path.read_bytes())
trace_id = trace_hash.hexdigest()[:16]
objects = []
sources += sorted(p for p in (ROOT / 'drivers/Adreno740').glob('NativeAdreno*.cpp')
                  if p.name not in ('NativeAdrenoClient.cpp', 'NativeAdrenoService.cpp', 'NativeAdrenoAccelerator.cpp'))
# NativeAdrenoAccelerator.cpp is built staged by build-adreno740-accelerator.py (its initializer must not run at boot).
# NativeAdrenoService.cpp is the archive's NativeAdreno.o from build-arm-driver.py; start from that object only.
for source in sources:
    recipe = c_recipe if source.suffix == '.c' else cxx_recipe
    recipe_args = native_arguments(recipe['arguments'])
    obj = out / (source.stem + ('.kit.o' if source.suffix == '.c' else '.o'))
    command, i = [], 0
    while i < len(recipe_args):
        value = recipe_args[i]
        if value == recipe['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ'):
            i += 1
        elif value == '-Xclang' and recipe_args[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        i += 1
    command += ['-I', str(ROOT / 'kits/uke'), '-I', str(ROOT / 'kits/AdrenoFirmware'),
                '-I', str(ROOT / 'build/sheng-gmu'), '-I', str(ROOT / 'kits/AdrenoPM4'), '-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-DNATIVE_GPU_BUILD_ID=0x' + trace_id + 'ULL', '-I', str(ROOT / 'drivers/Adreno740'), '-I', str(ROOT / 'build/adreno740/include'),
                '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-unknown-warning-option', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier']
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    if result.returncode:
        sys.exit(str(source) + ':\n' + result.stderr[-2500:])
    check_object(obj.read_bytes(), source.name)
    objects.append(obj)
# GMU, SQE and ZAP firmware as strong definitions over NativeAdrenoFirmwareDefaults.cpp's weak empty
# ones. When .cache/firmware/uke-gpu holds the verified binaries, embed them into __DATA_CONST,__const.
# When absent on a public runner, emit fixed-size file-backed __DATA_CONST,__const slots with ASCII sentinels
# and _size = 0 so local post-link injection can populate them in native.macho before emit_prelink.py.
inputs = json.loads((ROOT / 'scripts/data/uke-gpu-inputs.json').read_text())
firmware = inputs['firmware']
local = ROOT / '.cache/firmware/uke-gpu'
embedded = [('_native_adreno_firmware_image', 'gmu', 'UKEFW_GMU_SLOT0'),
            ('_native_adreno_sqe_firmware_image', 'sqe', 'UKEFW_SQE_SLOT0'),
            ('_native_adreno_zap_image', 'zap', 'UKEFW_ZAP_SLOT0')]
const_asm = []
slot_asm = []
present = 0
for label, role, sentinel in embedded:
    pin = firmware[role]
    path = local / pin['name']
    if path.is_file():
        blob = path.read_bytes()
        if len(blob) != pin['bytes'] or hashlib.sha256(blob).hexdigest() != pin['sha256']:
            sys.exit('firmware %s does not match its recorded pin' % pin['name'])
        const_asm += ['.p2align 12', '.globl ' + label, label + ':', '.incbin ' + json.dumps(str(path)), label + '_end:',
                      '.p2align 3', '.globl ' + label + '_size', label + '_size:', '.quad ' + label + '_end-' + label]
        present += 1
    else:
        print('INFO: %s absent; emitting __DATA_CONST,__const slot (%d B, _size=0)' % (pin['name'], pin['bytes']))
        if pin['bytes'] < 16:
            sys.exit('firmware %s too small for slot sentinel' % pin['name'])
        slot_asm += ['.p2align 12', '.globl ' + label, label + ':',
                     '.asciz ' + json.dumps(sentinel),
                     '.space %d, 0' % (pin['bytes'] - 16),
                     '.p2align 3', '.globl ' + label + '_size', label + '_size:', '.quad 0']
assembly = []
if const_asm or slot_asm:
    assembly += ['.section __DATA_CONST,__const', *const_asm, *slot_asm]
fw_asm_path = out / ('firmware.S' if present == len(embedded) else 'firmware-slots.S')
fw_asm_path.write_text('\n'.join(assembly) + '\n')
subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-Wno-incompatible-sysroot',
                '-c', fw_asm_path, '-o', out / 'firmware.o'], check=True)
check_object((out / 'firmware.o').read_bytes(), fw_asm_path.name)
objects.append(out / 'firmware.o')
archive = out / 'libAdreno740.a'
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
defined = subprocess.run(['xcrun', 'nm', '-g', archive], capture_output=True, text=True).stdout
missing = [s for s in ('_ShengGmuColdStart', '_ShengGmuBuildStartup',
                       '_ShengGmuCommandDbInputs', '_AdrenoFirmwarePlanGmu',
                       '_ArmSmmu4KAttachStreams', '_ArmSmmu4KBindMmio', '_AdrenoPM4Type7',
                       '_NativeAdrenoGmuContextBringup')
           if (' T ' + s) not in defined and (' S ' + s) not in defined]
if missing:
    sys.exit('Required kit definitions absent: ' + ', '.join(missing))
print('PASS: %d kit objects archived with all eight required definitions present'
      % len(objects))
