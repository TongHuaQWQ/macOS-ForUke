#!/usr/bin/env python3
"""Reject a call to a file-local helper before the file declares or defines it.

`kernel/QcomXnuUsbDebug.c` cannot be compiled on a workstation (it needs osfmk/* and machine/* from an
XNU tree), so a helper used above its definition only fails 25 minutes into CI, as clang's
"static declaration of 'x' follows non-static declaration". That is exactly how append_hex32 broke the
b106 build. This walks the sources that are not compiled locally and reports any such use.

    scripts/check-declaration-order.py [paths...]

Exit code 1 when a use precedes every declaration and definition. Only names defined inside the same
file are considered, so libc and project-wide symbols are ignored.
"""
import pathlib
import re
import sys

SOURCES = ['kernel/QcomXnuUsbDebug.c', 'usb/Dwc3.c']
# A definition is "static <words> name(" that is not a prototype; a prototype ends with ');'.
DEFINE = re.compile(r'^static\s+(?:[\w*]+\s+)+?\**\s*([A-Za-z_]\w*)\s*\(')
PROTO = re.compile(r'^static\s+(?:[\w*]+\s+)+?\**\s*([A-Za-z_]\w*)\s*\([^;{]*\)\s*;\s*$')


def check(path):
    text = pathlib.Path(path).read_text()
    lines = text.splitlines()
    defined = {}      # name -> definition line index
    declared = {}     # name -> [prototype line indices]
    for index, line in enumerate(lines):
        if PROTO.match(line):
            declared.setdefault(PROTO.match(line).group(1), []).append(index)
            continue
        m = DEFINE.match(line)
        if m:
            defined.setdefault(m.group(1), index)
    problems = []
    for name, defline in defined.items():
        earliest_decl = min(declared.get(name, []) + [defline])
        if earliest_decl == 0:
            continue
        pattern = re.compile(r'(?<![\w.>])' + re.escape(name) + r'\s*\(')
        for index in range(earliest_decl):
            line = lines[index]
            if pattern.search(line) and not line.lstrip().startswith(('*', '/*', '//')):
                problems.append((path, name, index + 1, earliest_decl + 1))
                break
    return problems


def main(argv):
    targets = argv or SOURCES
    problems = []
    for target in targets:
        path = pathlib.Path(target)
        if not path.is_file():
            print(f'skip (missing): {target}', file=sys.stderr)
            continue
        problems += check(path)
    for path, name, used, first in problems:
        print(f'{path}:{used}: {name}() is used before its first declaration (line {first})',
              file=sys.stderr)
    if problems:
        return 1
    print(f'declaration order ok ({len(targets)} file(s))')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
