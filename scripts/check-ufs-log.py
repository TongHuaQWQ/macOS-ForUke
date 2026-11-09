#!/usr/bin/env python3
"""Compile package/UfsLog*.{cpp,c} the way the kernel will, without a runner.

The kernel archive that carries these two files (scripts/build-ufs-log.py) only builds on macOS, so a
mistake in them costs a 25-minute round. Both are freestanding by construction, so a host clang can
answer the same question in about a second:

    clang --target=aarch64-none-elf -ffreestanding ...

against the three kernel headers they actually use, which this script writes as stubs into a temporary
directory. UfsLogKernel.c declares the five IOKit entry points itself, so no iokit/ header is needed at
all -- that is also why the archive build no longer derives an include path from the compile record.
What it proves, in order:

  * both units compile with -Wall -Wextra at a fixed target and no warnings that matter,
  * they link together (ld.lld -r) while their only remaining references are the kernel services the
    kernel really exports, plus memcpy/bzero/strcmp which xnu defines in osfmk/arm64/{bcopy,bzero}.s
    and osfmk/device/subrs.c -- defining those here instead is a duplicate symbol under -force_load,
    which is why UKE_UFS_LOG_STANDALONE exists only for this check,
  * every symbol UkeLastLog.c calls (native_ufs_log_*, ufs_log_start) is defined by the pair.

Usage: python3 scripts/check-ufs-log.py [-v]
Exit 0 on success, 1 on the first failed step.
"""
import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGET = 'aarch64-none-elf'
DEVICE = Path(__file__).resolve().parents[1] / 'drivers/NativeUfs'

# What the pair is allowed to still import. The first eight are kernel services; the last three are
# xnu's own libc, and a *definition* rather than a reference is what the kernel link rejects.
EXPORTS = ('native_ufs_log_open', 'native_ufs_log_write', 'native_ufs_log_sync', 'ufs_log_start')
IMPORTS = {'IODelay', 'IOLockAlloc', 'IOLockLock', 'IOLockUnlock', 'IOMallocContiguous',
           'mach_absolute_time', 'ml_io_map', 'PE_parse_boot_argn', 'bzero', 'memcpy', 'strcmp'}

STUBS = {
    'pexpert/pexpert.h': '''#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
#ifdef __cplusplus
}
#endif
''',
    'machine/machine_routines.h': '''#pragma once
#include <stdint.h>
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
''',
    'mach/mach_time.h': '''#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t mach_absolute_time(void);
#ifdef __cplusplus
}
#endif
''',
}


def run(command, what, verbose):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    if verbose or result.returncode:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
    if result.returncode:
        raise SystemExit(f'{what} failed (exit {result.returncode})')
    return result.stdout


def tool(name):
    path = shutil.which(name)
    if path is None:
        raise SystemExit(f'{name} not found; the check needs clang, ld.lld and llvm-nm on the host')
    return path


def record_probing():
    """Exercise build-ufs-log.py's compile-record probing, which the round cannot test any other way.

    unixnu's own note says it: "with CONFIG_XNU_BUILD_LTO the records sit next to the LTO objects and the
    path probed above need not exist". So the build must find whatever record is actually there, say
    which one it used, and fail with the paths it probed when there is none -- not with a traceback.
    """
    module = ROOT / 'scripts/build-ufs-log.py'
    spec = importlib.util.spec_from_file_location('build_ufs_log_probe', module)
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    with tempfile.TemporaryDirectory(prefix='ufs-log-record-') as temporary:
        tree = Path(temporary)
        records = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
        (records / 'bsd/DEVELOPMENT').mkdir(parents=True)
        wanted = records / 'bsd/DEVELOPMENT/bsd_init.o.json'
        wanted.write_text('{"file": "bsd_init.c", "directory": ".", "arguments": []}')
        late = records / 'osfmk/DEVELOPMENT/IOPlatformExpert.cpo.json'
        late.parent.mkdir(parents=True)
        late.write_text('{"file": "IOPlatformExpert.cpp", "directory": ".", "arguments": []}')
        build.ROOT, build.RECORDS = tree, records.relative_to(tree).as_posix()

        name, note = build.find_record((build.C_RECORD,), 'c')
        if name != 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json' or note:
            raise SystemExit(f'record probing: expected the exact path with no note, got {name!r} {note!r}')
        name, note = build.find_record((build.CXX_RECORD,), 'cpp')
        if not name.endswith('IOPlatformExpert.cpo.json') or 'no cpp record' not in note:
            raise SystemExit(f'record probing: expected the glob fallback with a note, got {name!r} {note!r}')
        (records / 'bsd/DEVELOPMENT/bsd_init.o.json').unlink()
        (records / 'osfmk/DEVELOPMENT/IOPlatformExpert.cpo.json').unlink()
        try:
            build.find_record((build.C_RECORD,), 'c')
        except SystemExit as failure:
            if 'probed' not in str(failure):
                raise SystemExit(f'record probing: unhelpful failure: {failure}')
        else:
            raise SystemExit('record probing: a missing record must fail, not return something')
        # The transport headers are NOT in <tree>/drivers/NativeUfs: the tree links only the components
        # the configuration selects, and CONFIG_XNU_NATIVE_UFS is off here. UNIXNU_DEVICE points at the
        # checkout that always has them -- archive-18 on run 42662408667 is what happens without this.
        device = tree / 'device'
        (device / 'drivers/NativeUfs').mkdir(parents=True)
        (device / 'drivers/NativeUfs/RootDiagnostics.h').write_text('#pragma once\n')
        empty = tree / 'tree'
        empty.mkdir()
        build.ROOT = empty
        os.environ['UNIXNU_DEVICE'] = str(device)
        found = build.driver_headers()
        if found != device / 'drivers/NativeUfs':
            raise SystemExit(f'driver headers: expected the device checkout, got {found}')
        del os.environ['UNIXNU_DEVICE']
        print('PASS: compile records are probed, the fallback is named, and a missing one fails loudly')
        print('PASS: transport headers come from UNIXNU_DEVICE when the tree does not link NativeUfs')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('-v', '--verbose', action='store_true', help='show compiler output')
    options = parser.parse_args()
    record_probing()
    clang, clangxx = tool('clang'), tool('clang++')
    linker, nm = tool('ld.lld'), tool('llvm-nm')

    with tempfile.TemporaryDirectory(prefix='ufs-log-check-') as temporary:
        work = Path(temporary)
        for name, text in STUBS.items():
            (work / name).parent.mkdir(parents=True, exist_ok=True)
            (work / name).write_text(text)
        common = ['-I', DEVICE, '-I', work, '-O2', '-mgeneral-regs-only', '-Wall', '-Wextra',
                  '-Wno-unused-parameter', '-Wno-unused-function']
        glue = work / 'UfsLogKernel.o'
        transport = work / 'UfsLog.o'
        run([clang, '--target=' + TARGET, '-ffreestanding', '-std=gnu11', *common,
             '-c', ROOT / 'package/UfsLogKernel.c', '-o', glue], 'UfsLogKernel.c', options.verbose)
        run([clangxx, '--target=' + TARGET, '-ffreestanding', '-fno-exceptions', '-fno-rtti',
             '-std=c++17', '-DUKE_UFS_LOG_STANDALONE', *common,
             '-c', ROOT / 'package/UfsLog.cpp', '-o', transport], 'UfsLog.cpp', options.verbose)
        combined = work / 'ufs-log.o'
        run([linker, '-r', transport, glue, '-o', combined], 'ld.lld -r', options.verbose)

        defined = {line.split()[-1] for line in run([nm, '--defined-only', combined], 'llvm-nm', False).splitlines() if line.split()}
        undefined = {line.split()[-1] for line in run([nm, '--undefined-only', combined], 'llvm-nm', False).splitlines() if line.split()}
        missing = [name for name in EXPORTS if name not in defined]
        if missing:
            raise SystemExit('the pair does not define: ' + ', '.join(missing))
        extra = sorted(undefined - IMPORTS)
        if extra:
            raise SystemExit('unexpected imports (the freestanding split leaked): ' + ', '.join(extra))
        if options.verbose:
            print('defined  :', ' '.join(sorted(defined)))
            print('imported :', ' '.join(sorted(undefined)))
        print(f'PASS: both units compile, link together, define {", ".join(EXPORTS)} '
              f'and import only {len(undefined)} kernel symbols')


if __name__ == '__main__':
    main()
