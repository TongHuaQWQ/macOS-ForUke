#!/usr/bin/env python3
"""Fail when patches/xnu/ has drifted from the component's own patch profile.

`patches/xnu/series` is what the tree applies, but the ported patches live in
package/qcom-usb-debug/patches/darwin24/ - the component owns them, the board
copies them in. Only the copies take effect, so a fix written in one place and
not the other is invisible until a 45-minute round dies on it. That happened
once (STATE 73: the printf.c guard and master_cpu fixes landed in the component
copy only, and the next round applied the stale one).

Cheap enough to run before every push and as the first step of the kernel
workflow.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'package/qcom-usb-debug/patches/darwin24'
COPIES = {
    'zzzzzzzzzzzzzzzzzz-qcom-usb-debug-console.patch': '0001-qcom-usb-debug-console.patch',
    'zzzzzzzzzzzzzzzzzz-qcom-usb-debug-worker.patch': '0002-qcom-usb-debug-worker.patch',
}


def main():
    problems = []
    for copy_name, origin_name in COPIES.items():
        copy = ROOT / 'patches/xnu' / copy_name
        origin = PACKAGE / origin_name
        if not copy.is_file():
            problems.append(f'{copy_name}: missing (patches/xnu/series applies it)')
            continue
        if not origin.is_file():
            problems.append(f'{origin.relative_to(ROOT)}: missing (the component owns the patch)')
            continue
        if copy.read_bytes() != origin.read_bytes():
            problems.append(
                f'{copy_name}: differs from {origin.relative_to(ROOT)}\n'
                f'  the tree applies the copy, so the component copy would never take effect;\n'
                f'  run: cp {origin.relative_to(ROOT)} patches/xnu/{copy_name}')
    series = (ROOT / 'patches/xnu/series').read_text().splitlines()
    for copy_name in COPIES:
        if copy_name not in series:
            problems.append(f'patches/xnu/series: {copy_name} is not listed')
    for problem in problems:
        print(f'error: {problem}', file=sys.stderr)
    if problems:
        return 1
    print(f'patch copies match the component ({len(COPIES)} file(s), series order checked)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
