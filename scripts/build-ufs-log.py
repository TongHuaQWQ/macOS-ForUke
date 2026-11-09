#!/usr/bin/env python3
"""Build libUkeUfsLog.a: the freestanding UFS log transport and the kernel glue under it.

Why an archive of its own
-------------------------
`package/UkeLastLog.c` writes a 16 KiB msgbuf record into the last 32 MiB of userdata every two
seconds, behind UKE_LOG_USERDATA, through three symbols:

    native_ufs_log_open / native_ufs_log_write / native_ufs_log_sync

drivers/NativeUfs/NativeUfs.cpp defines them, and that file cannot be built for this profile (the
verified-root half needs a signed policy directory that does not exist for 25F84, and the file is
written against the iPadOS 17 IOStorageFamily headers -- board.toml records that wall). What *can* be
built is the transport: RootCommands.h, RootQueue.h, SmmuTakeover.h, TransferPoll.h and
RootTransportLayout.h are IOKit-free by construction, which is how the driver's own bare-metal tests
consume them.

The split
---------
  package/UfsLog.cpp        the transport plus a freestanding shim: request list, SCSI read/write, the
                            window check every write goes through, and the attach sequence (stop the
                            firmware's list, plan and program the SMMU context bank for stream 0x60,
                            start our own list, prove it with a NOP OUT).
  package/UfsLogKernel.c    only what the kernel alone can do: map 0x01d84000+0x3000 and the apps
                            SMMU, allocate one physically contiguous request/data area, allocate a
                            lock, read a u32 boot argument.

Both are compiled with this kernel's own compile records, so the target triple, the arm64e PAC
options and the kernel include set are the ones the rest of the kernel used. The record only supplies
the command line that *would* have compiled an existing kernel object. Nothing is added to its -I set
except drivers/NativeUfs, which is this repository's own directory: UfsLogKernel.c declares the five
IOKit entry points it calls instead of including <IOKit/IOLib.h>, because a bsd record has no iokit/
on its include path and deriving one from the record cost the first writer round.

Linking
-------
build-xnu-4k-baseline.sh force-loads ${XNU_EXTRA_LD_ARCHIVES} into the kernel link, and
configs/uke-macos_defconfig points that at {tree}/build/uke-ufs-log/libUkeUfsLog.a. Nothing here is a
kernel entry point, so an archive that was built but not force-loaded simply would not exist in the
image -- which is exactly what the assertion at the end of this script is about.

Checking it without a runner
----------------------------
    python3 scripts/check-ufs-log.py -v

Both files are freestanding, so a host clang reproduces the whole thing: that script writes the four
kernel headers they use as stubs (pexpert/pexpert.h, machine/machine_routines.h, mach/mach_time.h,
IOKit/IOLib.h), compiles both units for aarch64-none-elf, links them with ld.lld -r and asserts the
result defines the four exports and imports nothing but the kernel's own eight services and xnu's
libc. UKE_UFS_LOG_STANDALONE is what gives that check the memcpy/bzero/strcmp the kernel already owns;
without it the object only declares them, which is the shape that goes into the image (defining them
is a duplicate symbol against osfmk/arm64/bcopy.s, bzero.s and subrs.c). The eight are IODelay,
IOLockAlloc/Lock/Unlock, IOMallocContiguous, mach_absolute_time, ml_io_map and PE_parse_boot_argn.
"""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/uke-ufs-log'
OUT.mkdir(parents=True, exist_ok=True)
RECORDS = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
CXX_RECORD = f'{RECORDS}/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_RECORD = f'{RECORDS}/bsd/DEVELOPMENT/bsd_init.o.json'
# native_ufs_log_* are what UkeLastLog.c calls; ufs_log_start is what it calls before them.
EXPORTS = ('native_ufs_log_open', 'native_ufs_log_write', 'native_ufs_log_sync', 'ufs_log_start')
# Imports that must never appear: the kernel link has no C++ runtime.
FORBIDDEN_PREFIXES = ('__cxa_', '__gxx_', '_Unwind_', '__dynamic_cast', '__si_')


def load_record(name):
    return json.loads((ROOT / name).read_text().strip().rstrip(','))


def rewrite(record, source, obj, extra):
    """The record's own command line with our source, our -o, and the flags the port needs."""
    arguments, command, index = record['arguments'], [], 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                command += ['-o', str(obj)]
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    return command + extra


def compile_unit(source, record_name, obj, extra):
    record = load_record(record_name)
    command = rewrite(record, source, obj, extra)
    result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=300)
    (OUT / (obj.stem + '-compile-command.json')).write_text(json.dumps(command, indent=2) + '\n')
    (OUT / (obj.stem + '-compile.log')).write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout[-6000:])
    return hashlib.sha256(source.read_bytes()).hexdigest(), hashlib.sha256(obj.read_bytes()).hexdigest()


def symbols(archive):
    """Global defined names and global undefined names of the archive, as plain symbol names."""
    listing = subprocess.run(['xcrun', 'nm', '-g', str(archive)], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, timeout=60)
    if listing.returncode:
        raise SystemExit(listing.stdout[-2000:])
    defined, undefined = set(), set()
    for line in listing.stdout.splitlines():
        # "0000000000000000 T _name" for a definition, "                 U _name" for a reference, and
        # "archive.a(object.o):" for the member header.
        fields = line.split()
        if len(fields) < 2:
            continue
        kind, name = fields[-2], fields[-1]
        if kind == 'U':
            undefined.add(name.lstrip('_'))
        else:
            defined.add(name.lstrip('_'))
    return defined, undefined


def main():
    """Compile both units with the kernel's own records, archive them, and assert the exports."""
    # Only the driver's own transport headers: package/UfsLogKernel.c declares the five IOKit
    # entry points it calls, so nothing here needs an include path invented from the record.
    includes = ['-I', str(ROOT / 'drivers/NativeUfs')]
    # -Wall -Wextra without -Werror: the headers are the driver's, not this port's, and a warning here must
    # not cost a kernel round. Both logs land in the artifact and the report, so a later round can tighten.
    warnings = ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Wno-unused-parameter',
                '-Wno-unused-function', '-Wno-deprecated-declarations']

    report = {}
    objects = []
    for name, record_name, extra in (
            ('UfsLog.cpp', CXX_RECORD, includes + warnings + ['-fno-exceptions', '-fno-rtti']),
            ('UfsLogKernel.c', C_RECORD, includes + warnings)):
        source = ROOT / 'kits/uke' / name
        if not source.is_file():
            source = ROOT / 'package' / name
        obj = OUT / (Path(name).stem + '.o')
        source_sha, object_sha = compile_unit(source, record_name, obj, extra)
        report[name] = {'source_sha256': source_sha, 'object_sha256': object_sha}
        objects.append(obj)

    archive = OUT / 'libUkeUfsLog.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True, timeout=60)

    defined, undefined = symbols(archive)
    missing = [name for name in EXPORTS if name not in defined]
    if missing:
        raise SystemExit(f'archive does not define {", ".join(missing)}: {sorted(defined)[:40]}')
    # The one thing that makes the userdata half do nothing at all: an export that is only declared.
    imported = [name for name in EXPORTS if name in undefined]
    if imported:
        raise SystemExit(f'archive only imports {", ".join(imported)}')
    runtime = sorted(name for name in undefined if name.startswith(FORBIDDEN_PREFIXES))
    if runtime:
        raise SystemExit('the archive reached for the C++ runtime: ' + ', '.join(runtime))

    # Everything else it imports is a kernel symbol, and the list is evidence rather than a gate: the two
    # files are freestanding, so it should be the eight kernel services and nothing unexpected.
    report['archive_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
    report['exports'] = sorted(EXPORTS)
    report['kernel_imports'] = sorted(undefined)
    report['hardware_execution'] = False
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: libUkeUfsLog.a defines {", ".join(EXPORTS)}; imports {", ".join(sorted(undefined))}')


if __name__ == '__main__':
    main()
