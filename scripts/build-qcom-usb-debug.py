#!/usr/bin/env python3
"""Build libQcomXnuUsbDebug.a for uke against this tree's own compile records.

`drivers/QcomXnuUsbDebug/scripts/build.py` wants an XNU compile record plus explicit ar/nm, because it
never guesses a toolchain. On the macOS runner both come from Xcode, so this wrapper resolves them with
`xcrun -f` and passes the board profile (`soc/Uke.h`) plus the record of the unit the module measures
itself against (pe_kprintf.o.json).

The archive lands in .cache/qcom-usb-debug/, which scripts/build-xnu-4k-baseline.sh force-loads when
XNU_QCOM_USB_DEBUG=1 (see the CONFIG_XNU_QCOM_USB_DEBUG block there).
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# The component's build.py refuses an --output that is inside the module source or outside a tmp/ or
# .cache/ path ("output must be outside source and under tmp/ or .cache/"), so the archive lives under
# the tree's .cache/ and build-xnu-4k-baseline.sh looks for it there.
OUT = ROOT / '.cache/qcom-usb-debug'
# The component rides in the device repository's package/ directory, which the tree links as kits/uke
# (board.toml: kit = "uke"). The archive scripts run with the tree as ROOT, so the module is under
# kits/uke/qcom-usb-debug there and under package/qcom-usb-debug in the device checkout - the same
# kits-first, package-fallback resolution build-boot-log.py uses for its own sources. A feed entry
# cannot be used for this: assemble() only links sources the component catalog resolves to a module,
# and the catalog has no QcomXnuUsbDebug id, which is why "module missing" appeared with one added.
def _module():
    override = os.environ.get('UNIXNU_QCOM_USB_DEBUG_MODULE')
    if override:
        return Path(override)
    for candidate in (ROOT / 'kits/uke/qcom-usb-debug', ROOT / 'package/qcom-usb-debug'):
        if (candidate / 'scripts/build.py').is_file():
            return candidate
    return ROOT / 'kits/uke/qcom-usb-debug'


MODULE = _module()
RECORD = (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json')


def tool(name):
    found = shutil.which(name)
    if found:
        return found
    located = subprocess.run(['xcrun', '-f', name], capture_output=True, text=True)
    if located.returncode == 0 and located.stdout.strip():
        return located.stdout.strip()
    raise SystemExit(f'{name} not found (xcrun -f {name} failed)')


def relaxed_record(source, target):
    """A record the component can compile with: no warning flags, and no flag this compiler rejects.

    Two things in a kernel compile record break an out-of-tree archive build, and both have now cost a
    round. The warning set carries flags a newer clang does not know (the workflow patches
    -Wno-unknown-warning-option into two archive recipes for the same reason) and -Werror, under which a
    foreign revision's warning becomes a failure; and the record is the *kernel link* unit's line, so it
    carries link-only options - '-mlinker-version=1267' was the first one clang refused for '-c'.
    Rather than hard-code a deny list that the next kernel revision will outgrow, drop the -W set and
    then ask this very compiler which remaining flags it accepts, one rejection at a time.
    """
    record = json.loads(source.read_text().rstrip().rstrip(','))
    arguments = record['arguments']
    compiler, rest = arguments[0], [a for a in arguments[1:] if not a.startswith('-W')]
    source_file = record['file']
    keep = [a for a in rest if a not in ('-c', '-fsyntax-only', source_file)]

    for _ in range(16):
        probe = subprocess.run([compiler, '-fsyntax-only', '-x', 'c', *keep, os.devnull],
                               capture_output=True, text=True)
        if probe.returncode == 0:
            break
        bad = re.search(r"unknown argument: '([^']+)'", probe.stderr)
        if not bad:
            break
        rejected = bad.group(1)
        index = next((i for i, a in enumerate(keep)
                      if a == rejected or a.startswith(rejected + '=')), None)
        # The record carries cc1 options too: '-Xclang --target=arm64e-apple-ios26.5.2' is a pair, and
        # cc1 has no --target, so the driver refuses it for '-c'. Dropping such a pair is safe because
        # whatever it set is already set at driver level by '-arch arm64e' (or is a cc1 default).
        forwarded = index is not None and index > 0 and keep[index - 1] == '-Xclang'
        protected = ('--target', '-target', '-arch', '-isysroot', '-I', '-D', '-U', '-isystem', '-x')
        if rejected.startswith(protected) and not forwarded:
            # Dropping the target would compile for the host triple and the archive's LC_BUILD_VERSION
            # audit would then reject it; dropping an include or define changes what the code sees.
            print(f'record: keeping {rejected} (protected); probe said: '
                  f'{probe.stderr.strip().splitlines()[0][:160] if probe.stderr.strip() else "(no output)"}')
            break
        if index is None:
            break
        drop = {index, index - 1} if forwarded else {index}
        keep = [a for i, a in enumerate(keep) if i not in drop]
        print(f'record: dropped {rejected}' + (' with its -Xclang (forwarded to cc1, which has no such '
                                               'option; -arch already sets the triple)' if forwarded
                                               else ' (this compiler rejects it for a compile)'))

    record['arguments'] = [compiler, *keep, *BOARD_DEFINES]
    target.write_text(json.dumps(record, indent=1))
    return target


UNITS = ('kernel/QcomXnuUsbDebug.c', 'kernel/QcomUsbScreenshot.c',
         'usb/Dwc3.c', 'soc/QcomUsbQualify.c', 'soc/QcomUsbDmaDomain.c')

# Board sizing that has to reach both the preflight and the component's own compile line, so it goes
# into the compile record they share rather than into one of the two.
#
# QUD_SCREENSHOT_MAX_BYTES is a kernel-__DATA budget, and __DATA is the scarce resource here: prelink
# places the native kernel's data inside the range Apple's own kernel data occupied
# (fileset_layout.py), and on this board that range is 0x27f8b0 bytes while the rebuilt kernel without
# this archive is 0x248000 - about 223 KiB to spare. The component's 16 MiB default for the screenshot
# capture buffer plus a 4 MiB log ring asked for 4x that, and prelink refused with "Native __DATA
# overlaps a retained segment" (STATE 76). 32 KiB brings the archive's whole footprint to 160 KiB,
# leaving ~62 KiB of margin; real screenshots need the prelink data range widened first.
BOARD_DEFINES = (
    '-DQUD_SCREENSHOT_MAX_BYTES=32768',
    # Diagnostic panic mode compiled in, so the experiment does not depend on the boot argument being
    # parsed: with qcom-usb-panic=2 on the command line the host still saw a served GET_DESCRIPTOR
    # followed by a dead SET_ADDRESS, i.e. nothing panicked (STATE 81). 2 = panic on every control
    # request, printing rt/rq/v/i/l and the phase on the panel.
    '-DQCOM_USB_PANIC_DEFAULT=2',
)

# 'file:line:col: error: ...' from a compile, and 'error: ...' from the driver itself.
DIAGNOSTIC = re.compile(r'(^|: )error: ')


def preflight(record_path):
    """Compile every unit with -fsyntax-only and print all errors.

    The component's build.py stops at the first failing unit, so a round that hits one API rename tells
    us about exactly one rename - and a round is 45 minutes. This pass uses the same flags and include
    set, keeps going, and prints every diagnostic, so one round can carry the whole list.
    """
    record = json.loads(record_path.read_text())
    compiler, *flags = record['arguments']
    defines = ['-I' + str(MODULE), '-I' + str(MODULE / 'kernel'), '-I' + str(MODULE / 'include'),
               '-DQUD_SOC_HEADER="soc/Uke.h"', '-Wno-reserved-identifier', '-Wno-pre-c11-compat']
    problems = 0
    for unit in UNITS:
        source = MODULE / unit
        if not source.is_file():
            continue
        run = subprocess.run([compiler, '-fsyntax-only', '-x', 'c', *flags, *defines, str(source)],
                             cwd=record['directory'], capture_output=True, text=True)
        errors = [l for l in run.stderr.splitlines() if DIAGNOSTIC.search(l)]
        if run.returncode != 0 and not errors:
            # A driver-level diagnostic ('error: unknown argument: ...') has no 'file:line:' prefix and
            # used to be filtered out here - that printed "0 error(s)" for five failing units.
            errors = [l for l in run.stderr.splitlines() if l.strip()][:4] or ['(no output)']
        problems += len(errors)
        for line in errors[:12]:
            print(f'preflight {unit}: {line}')
    print(f'preflight: {problems} error(s) across {len(UNITS)} units')
    return problems


def main():
    if not (MODULE / 'scripts/build.py').is_file():
        raise SystemExit(f'module missing: {MODULE}')
    if not RECORD.is_file():
        raise SystemExit(f'compile record missing: {RECORD} (the kernel build must run before the archives)')
    if OUT.exists():
        # build.py refuses to overwrite; a half-written output from a killed run must not survive either.
        shutil.rmtree(OUT)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    # One relaxation pass, used by both the preflight and the component: computing it twice printed the
    # same "dropped"/"keeping" lines twice and made the log read as though the probe had run twice.
    record = relaxed_record(RECORD, OUT.parent / 'compile-record.json')
    preflight(record)
    command = [
        sys.executable, '-B', str(MODULE / 'scripts/build.py'),
        '--compile-record', str(record),
        '--soc', 'soc/Uke.h',
        '--ar', tool('llvm-ar'),
        '--nm', tool('llvm-nm'),
        '--output', str(OUT),
    ]

    print(' '.join(command))
    result = subprocess.run(command, cwd=str(MODULE))
    if result.returncode != 0:
        # The component sends each compiler's output to that object's .log file, so a failing compile
        # leaves nothing on this stream. Print them, or the next round only says "returned 1".
        for log in sorted(OUT.glob('*.log')):
            print(f'--- {log.name} ---')
            text = log.read_text(errors='replace').splitlines()
            print('\n'.join(text[:120]))
            if len(text) > 120:
                print(f'... {len(text) - 120} more lines')
        raise SystemExit(f'{Path(__file__).name}: component build failed (exit {result.returncode})')
    report = json.loads((OUT / 'build-report.json').read_text())
    print(f"QcomXnuUsbDebug: {report.get('archive_sha256', '?')[:16]} soc={report.get('soc')}")


if __name__ == '__main__':
    main()
