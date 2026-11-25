#!/usr/bin/env python3
"""Complete the SoftwareAES archive with the two things package/SourceStorage.cpp needs.

The source storage runtime publishes the AES accelerator itself, through three symbols it declares
strongly:

    extern "C" void source_software_aes_init(void) __asm__("__GLOBAL__sub_I_NativeAES.cpp");
    extern "C" IOService *native_software_aes_create(void);
    extern "C" volatile uint32_t native_aes_kat_status, native_aes_interface_status, native_aes_registered;

The first two do not exist in build/software-aes/libSoftwareAES.a as built by this configuration:

  * `__GLOBAL__sub_I_NativeAES.cpp` is emitted by the compiler as a *local* symbol, so the archive has it
    but nothing outside the object can refer to it;
  * `native_software_aes_create` is not in drivers/SoftwareAES/NativeAES.cpp at all — every board that
    runs the source runtime generates it next to the class.

Both are done for uke by Allenwdk/UniXNU-uke's scripts/build-software-aes-27.py, in the two steps this
script repeats (its lines 41-43 and 118-120):

    generated = NativeAES.cpp + 'extern "C" IOService *native_software_aes_create(void) { return new NativeAES; }'
    objcopy --remove-section=__DATA,__mod_init_func --globalize-symbol=__GLOBAL__sub_I_NativeAES.cpp

Their script also re-derives the inherited-method bindings from the 24A446 kernelcache and checks the
PAC schema of all 181 vtable slots. That half is deliberately NOT repeated here: it is specific to the
iPadOS 24A446 image and its recorded vtable, while this board's bindings are already produced — for the
26C5035r/mac17g train — by build-software-aes-bindings.py (the recorded original-kernelcache addresses in
scripts/data/native-aes-original-bindings.S). This step only adds the factory and globalizes the
initializer, then re-archives the two objects that step produced.

    python3 scripts/build-software-aes-factory.py
"""
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/software-aes'
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_target import TRIPLE  # noqa: E402

# The core's own compiler driver: compile_command() builds the command from a kernel compile record and is
# the only place that knows the recorded flags have to lose -flto (an LTO member is bitcode, and the
# objcopy below cannot read it - b279 died exactly there) and gain -mgeneral-regs-only. Hand-filtering the
# argument list missed the -flto half.
import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location('source_compile', ROOT / 'scripts/build-sheng-source-drivers.py')
source_compile = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(source_compile)


def run(command, **kwargs):
    return subprocess.run([str(item) for item in command], check=True, **kwargs)


def main():
    source = ROOT / 'drivers/SoftwareAES/NativeAES.cpp'
    archive = OUT / 'libSoftwareAES.a'
    bindings = OUT / 'original-bindings.o'
    for path in (source, archive, bindings):
        if not path.is_file():
            raise SystemExit(f'{path} missing: run component:SoftwareAES and build-software-aes-bindings.py first')

    # The recipe of an iokit C++ unit, as build-software-aes-bindings.py and the storage adapter use it:
    # same triple, same -I set, but compiling the generated copy instead of the original file.
    record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/'
                         'IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
    if 'arm64e-apple-ios27' not in ' '.join(record['arguments']):
        raise SystemExit(f"the iokit recipe does not target 27 arm64e: {' '.join(record['arguments'])[:200]}")

    # The copy keeps the ORIGINAL basename on purpose. A static initializer is named after its translation
    # unit: compiling the copy as NativeAES-factory.cpp produces __GLOBAL__sub_I_NativeAES_factory.cpp, and
    # the objcopy below then globalizes nothing (it does not warn), which is exactly how b282 failed. The
    # reference writes its generated copy as NativeAES.cpp for the same reason.
    factory = OUT / 'factory'
    factory.mkdir(exist_ok=True)
    # In macOS 27.3 (26C5035r), getAlgorithmName() is introduced on IOCryptoAccelerator
    # (__ZN19IOCryptoAccelerator16getAlgorithmNameEv); declaring it first on IOAESAccelerator
    # produces the wrong arm64e virtual PAC discriminator.
    header_src = source.with_name('OriginalAESABI.h')
    if header_src.is_file():
        header27 = header_src.read_text()
        head, sep, tail = header27.partition('class IOAESAccelerator')
        if sep:
            if 'virtual const char *getAlgorithmName();' not in head:
                head = head.replace(
                    'OSDeclareDefaultStructors(IOCryptoAccelerator)',
                    'OSDeclareDefaultStructors(IOCryptoAccelerator)\npublic:\n  virtual const char *getAlgorithmName();',
                    1,
                )
            tail = tail.replace(
                'virtual const char *getAlgorithmName();',
                'const char *getAlgorithmName() override;',
            )
            header27 = head + sep + tail
        (factory / 'OriginalAESABI.h').write_text(header27)
    generated = factory / 'NativeAES.cpp'
    generated.write_text(source.read_text() + '''
// Generated by scripts/build-software-aes-factory.py (Allenwdk/UniXNU-uke does the same in
// build-software-aes-27.py): the source storage runtime creates the accelerator through this C entry
// point, which the class itself does not export.
extern "C" IOService *native_software_aes_create(void) { return new NativeAES; }
''')

    object_file = factory / 'NativeAES.cpp.o'
    command = source_compile.compile_command(record, generated, object_file, [],
                                             ['-I', str(factory),
                                              '-I', str(source.parent),
                                              '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                                              '-Wno-missing-designated-field-initializers'])
    result = subprocess.run([str(item) for item in command], cwd=record['directory'],
                            capture_output=True, text=True)
    (OUT / 'factory-compile.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit((result.stdout + result.stderr)[-6000:])

    # Bind IOAESAccelerator to macOS 27.3 (26C5035r) com.apple.iokit.IOCryptoAcceleratorFamily
    # (slid +0x1f000000000) using literal-load trampolines so emit_prelink.py's __TEXT_EXEC shift
    # never shifts PC-relative branches out of IOCryptoAcceleratorFamily into __LINKEDIT.
    aes_bindings_26c5035r = {
        '__ZN16IOAESAccelerator10gMetaClassE': 0xfffffff00cc205c0,
        '__ZN16IOAESAccelerator10performAESEP18IOMemoryDescriptorS1_yP7IOAESIV14IOAESOperationP12IOAESKeyDatayyPFvPviES7_': 0xfffffff00a9d7478,
        '__ZN16IOAESAccelerator10performAESEPvS0_P4taskyP7IOAESIV14IOAESOperationP12IOAESKeyDataPFvS0_iES0_': 0xfffffff00a9d4eac,
        '__ZN16IOAESAccelerator12_completeAESEP23IOAESAcceleratorCommand': 0xfffffff00a9d56b8,
        '__ZN16IOAESAccelerator12_queueLockedEP23IOAESAcceleratorCommand': 0xfffffff00a9d53f8,
        '__ZN16IOAESAccelerator13_startCommandEv': 0xfffffff00a9d55dc,
        '__ZN16IOAESAccelerator14isKeySupportedEj': 0xfffffff00a9d53d4,
        '__ZN16IOAESAccelerator16getAlgorithmNameEv': 0xfffffff00a9d4560,
        '__ZN16IOAESAccelerator17createSpecialKeysEv': 0xfffffff00a9d590c,
        '__ZN16IOAESAccelerator17performAESQuantumEP23IOAESAcceleratorCommand': 0xfffffff00a9d5390,
        '__ZN16IOAESAccelerator18completeAESQuantumEP23IOAESAcceleratorCommand': 0xfffffff00a9d5294,
        '__ZN16IOAESAccelerator18disableHardwareKeyEj': 0xfffffff00a9d53cc,
        '__ZN16IOAESAccelerator19getSupportedOptionsEv': 0xfffffff00a9d53e0,
        '__ZN16IOAESAccelerator19performImmediateAESEP23IOAESAcceleratorCommand': 0xfffffff00a9d53bc,
        '__ZN16IOAESAccelerator20callPlatformFunctionEPK8OSSymbolbPvS3_S3_S3_': 0xfffffff00a9d4d4c,
        '__ZN16IOAESAccelerator5startEP9IOService': 0xfffffff00a9d4570,
        '__ZN16IOAESAcceleratorC2EPK11OSMetaClass': 0xfffffff00a9d4024,
        '__ZN16IOAESAcceleratorD0Ev': 0xfffffff00a9d40a4,
        '__ZN16IOAESAcceleratorD1Ev': 0xfffffff00a9d409c,
        '__ZN16IOAESAcceleratorD2Ev': 0xfffffff00a9d4094,
    }
    aes_text, aes_addresses = '.text\n.p2align 2\n', ''
    for name, va in sorted(aes_bindings_26c5035r.items()):
        if name.endswith('10gMetaClassE'):
            aes_addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
        else:
            aes_addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
            aes_text += (f'.globl {name}\n{name}:\n bti c\n'
                         f' adrp x16, {name}.slot@PAGE\n ldr x16, [x16, {name}.slot@PAGEOFF]\n br x16\n'
                         f'.p2align 3\n{name}.slot:\n .quad {name}.original\n.p2align 2\n')
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    (factory / 'aes-bindings.S').write_text(aes_text)
    (factory / 'aes-addresses.S').write_text(aes_addresses)
    for stem in ('aes-bindings', 'aes-addresses'):
        run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-c',
             factory / (stem + '.S'), '-o', factory / (stem + '.o')])

    objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
    late = OUT / 'NativeAES.late.o'
    run([objcopy, '--remove-section=__DATA,__mod_init_func',
         '--globalize-symbol=__GLOBAL__sub_I_NativeAES.cpp', object_file, late])
    archive.unlink()
    run(['xcrun', 'ar', 'rcs', archive, late, factory / 'aes-bindings.o', factory / 'aes-addresses.o'])

    # Evidence, and the one thing a silent misbuild here would hide. nm prints Mach-O names with the leading
    # underscore, so the initializer reads ___GLOBAL__sub_I_NativeAES.cpp (two from the name, one from the
    # format) — both checks below are substring tests for that reason.
    defined = subprocess.run(['xcrun', 'nm', '-g', str(archive)], capture_output=True, text=True).stdout
    missing = [symbol for symbol in ('__GLOBAL__sub_I_NativeAES.cpp', '_native_software_aes_create')
               if symbol not in defined]
    if missing:
        local = subprocess.run(['xcrun', 'nm', str(object_file)], capture_output=True, text=True).stdout
        init = [line for line in local.splitlines() if 'GLOBAL__sub_I' in line]
        raise SystemExit(f'{", ".join(missing)} not global in {archive}; the object carries '
                         f'{init or "no GLOBAL__sub_I symbol at all"}')
    (OUT / 'factory.json').write_text(json.dumps(dict(
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        generated_sha256=hashlib.sha256(generated.read_bytes()).hexdigest(),
        archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
        triple=TRIPLE, adds_only_the_factory=True), indent=2) + '\n')
    print(f'PASS: {archive.name} exports native_software_aes_create and a global __GLOBAL__sub_I_NativeAES.cpp')


if __name__ == '__main__':
    main()
