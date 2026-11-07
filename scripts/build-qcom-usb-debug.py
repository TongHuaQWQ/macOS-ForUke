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


def main():
    if not (MODULE / 'scripts/build.py').is_file():
        raise SystemExit(f'module missing: {MODULE}')
    if not RECORD.is_file():
        raise SystemExit(f'compile record missing: {RECORD} (the kernel build must run before the archives)')
    if OUT.exists():
        # build.py refuses to overwrite; a half-written output from a killed run must not survive either.
        shutil.rmtree(OUT)
    command = [
        sys.executable, '-B', str(MODULE / 'scripts/build.py'),
        '--compile-record', str(RECORD),
        '--soc', 'soc/Uke.h',
        '--ar', tool('llvm-ar'),
        '--nm', tool('llvm-nm'),
        '--output', str(OUT),
    ]
    print(' '.join(command))
    subprocess.run(command, check=True, cwd=str(MODULE))
    report = json.loads((OUT / 'build-report.json').read_text())
    print(f"QcomXnuUsbDebug: {report.get('archive_sha256', '?')[:16]} soc={report.get('soc')}")


if __name__ == '__main__':
    main()
