#!/usr/bin/env python3
"""Compile package/UfsLog*.{cpp,c} the way the kernel will, without a runner.

The kernel archive that carries these two files (scripts/build-ufs-log.py) only builds on macOS, so a
mistake in them costs a 25-minute round. Both are freestanding by construction, so a host clang can
answer the same question in about a second:

    clang --target=aarch64-none-elf -ffreestanding ...

against the four kernel headers they actually use, which this script writes as stubs into a temporary
directory. What it proves, in order:

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
    'IOKit/IOLib.h': '''#pragma once
/* Only what package/UfsLogKernel.c uses, with the real declarations' shapes: IOPhysicalAddress is
 * UInt64 (so `unsigned long long`, not uintptr_t) and IOMallocContiguous is deprecated. */
#include <stdint.h>
#include <stddef.h>
#include <mach/mach_time.h>
typedef uint64_t IOPhysicalAddress;
typedef struct IOLock IOLock;
#ifdef __cplusplus
extern "C" {
#endif
void IOLog(const char *format, ...);
void IODelay(unsigned microseconds);
void *IOMallocContiguous(uint64_t size, uint64_t alignment,
                         IOPhysicalAddress *physicalAddress) __attribute__((deprecated));
void IOFreeContiguous(void *address, uint64_t size) __attribute__((deprecated));
IOLock *IOLockAlloc(void);
void IOLockLock(IOLock *lock);
void IOLockUnlock(IOLock *lock);
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('-v', '--verbose', action='store_true', help='show compiler output')
    options = parser.parse_args()
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
