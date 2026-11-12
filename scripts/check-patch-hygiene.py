#!/usr/bin/env python3
"""check-patch-hygiene.py — catch the two patch defects that reached CI and cost two kernel rounds.

A unified diff body may only contain lines starting with ' ' (context), '+' (added), '-' (removed),
'\\' (no newline marker), '@' (hunk header) or a file header. Two failures were silent:

  * a hunk header whose counts disagree with the lines that follow. `git apply` is not strict about a
    count that is too SMALL: it applies the lines it was told about and the remaining ones land as
    garbage. Round 43211608117 reported `pe_init.c:1079: error: unterminated conditional directive`
    because a `#if` and its `#endif` had four stray lines wedged between them.

  * an added line written with two plus signs ("++\tclock_interval_to_deadline(...)"). The applied
    source then literally begins with a '+', and round 43221930075 died with
    `IOService.cpp:488: invalid argument type 'void' to unary expression`.

Both are cheap to detect and impossible to see in a passing `apply-series` run, because git apply
accepts the result. Usage:

    python3 scripts/check-patch-hygiene.py patches/xnu/*.patch     # exits 1 on any defect
"""
import re
import sys

HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


def check(path):
    problems = []
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.startswith('@@ '):
            # `--- a/x` / `+++ b/x` are file headers, not defects; anything else with a doubled
            # sign outside a hunk body is a hand-editing accident
            if line == '---':
                # git-format-patch's scissors line before the diffstat; not a defect
                i += 1
                continue
            if (line.startswith('++') or line.startswith('--')) and not line.startswith(('--- ', '+++ ')):
                problems.append(f'{path}:{i + 1}: line starts with {line[:2]!r}: {line[:80]}')
            i += 1
            continue
        match = HUNK.match(line)
        if not match:
            problems.append(f'{path}:{i + 1}: unparsable hunk header: {line[:80]}')
            i += 1
            continue
        old_count = int(match.group(2)) if match.group(2) is not None else 1
        new_count = int(match.group(4)) if match.group(4) is not None else 1
        j = i + 1
        old_seen = new_seen = 0
        while j < len(lines):
            body = lines[j]
            if body.startswith('@@ ') or body.startswith('diff ') or body.startswith('--- ') \
                    or body.startswith('+++ '):
                break
            if body.startswith('+'):
                if body.startswith('++'):
                    problems.append(f'{path}:{j + 1}: added line starts with a second plus: {body[:80]}')
                new_seen += 1
            elif body.startswith('-'):
                if body.startswith('--'):
                    problems.append(f'{path}:{j + 1}: removed line starts with a second minus: {body[:80]}')
                old_seen += 1
            elif body.startswith(' ') or body == '':
                old_seen += 1
                new_seen += 1
            elif body.startswith('\\'):
                pass
            else:
                problems.append(f'{path}:{j + 1}: body line starts with {body[:1]!r}: {body[:80]}')
                break
            j += 1
        if (old_seen, new_seen) != (old_count, new_count):
            problems.append(
                f'{path}:{i + 1}: hunk header says -{old_count} +{new_count} but the body has '
                f'-{old_seen} +{new_seen}: {line[:70]}')
        i = j
    return problems


def main():
    paths = sys.argv[1:]
    if not paths:
        print(__doc__)
        return 2
    problems = []
    for path in paths:
        problems += check(path)
    for problem in problems:
        print(problem)
    print(f'{len(paths)} patch(es) checked, {len(problems)} problem(s)')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
