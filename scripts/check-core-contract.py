#!/usr/bin/env python3
"""Fail fast when the UniXNU core checkout cannot build what this board declares.

The device repository is only half of a build: the kernel step, the storage builder and the data-object
placement all live in the core, and the CI job tracks a core branch, not a commit. A core that is one
commit behind therefore costs a whole round instead of a message - and it has, three times:

  * b274, `Unexpected public IOStorageFamily revision: 7edb88fb`: the core's adapter calls
    prepare(storage, '<revision>', out) with the revision written into the call, while feeds.toml pinned
    another one.
  * b275, `module 'source_headers' has no attribute 'prepare'`: this repository shipped the older
    scripts/prepare-storage-headers.py, whose entry point had another signature.
  * b276, `No such file or directory: .../scripts/policies/26C5035r/OS/manifest.bin`: the board sets
    [root] tables_in_userdata, whose whole purpose is to ship the 544-byte signed header instead of the
    108 MiB table, but the core's version_env() never turned that key into XNU_SOURCE_STORAGE_TABLES, so
    build-sheng-source-storage.py took the full-manifest branch and read a file this repository does not
    carry (and must not: OS is 113 MB, over GitHub's file limit).

Each case is a textual contract, so it is checkable in a second without building anything. The checks are
driven by what board.toml actually declares, so a board that does not use these features never trips them.

    python3 scripts/check-core-contract.py [--core ../UniXNU]
"""
import argparse
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return path.read_text() if path.is_file() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, default=Path('../UniXNU'),
                        help='the UniXNU checkout the kernel workflow uses')
    args = parser.parse_args()
    core = args.core.resolve()
    board = tomllib.loads((ROOT / 'board.toml').read_text())
    feeds = tomllib.loads((ROOT / 'feeds.toml').read_text()) if (ROOT / 'feeds.toml').is_file() else {}
    root, userdata = board.get('root', {}), board.get('userdata', {})
    problems = []

    def require(condition, message):
        if not condition:
            problems.append(message)

    if not (core / 'scripts').is_dir():
        raise SystemExit(f'{core}: no scripts/ - pass --core with the UniXNU checkout')

    kernel = read(core / 'scripts/unixnu/kernel.py')
    adapter = read(core / 'scripts/build-sheng-source-storage.py')
    placement = read(core / 'scripts/unixnu/userdata.py')

    # The root-policy tables travel in the root image, not in the kernel bundle.
    if root.get('tables_in_userdata'):
        require(kernel is not None and 'XNU_SOURCE_STORAGE_TABLES' in kernel and 'tables_in_userdata' in kernel,
                'core scripts/unixnu/kernel.py: [root] tables_in_userdata does not reach the kernel step as '
                'XNU_SOURCE_STORAGE_TABLES; build-sheng-source-storage.py will then want the full '
                'manifest.bin, which this repository does not ship')
        require(adapter is not None and 'manifest.header.bin' in adapter,
                'core scripts/build-sheng-source-storage.py: no manifest.header.bin handoff, so the '
                'header-only bundle cannot be built')

    # The tables themselves are placed by the userdata step, from the board declaration.
    if userdata.get('objects'):
        require(placement is not None and "spec.get('objects'" in placement,
                'core scripts/unixnu/userdata.py: [userdata] objects is not read from the board, so the '
                '<window>.vroot tables would never be placed')

    # IoStorageFamily is a refer pin: the adapter hardcodes the revision it accepts.
    pin = next((entry.get('commit') for entry in feeds.get('source', [])
                if entry.get('name') == 'IOStorageFamily'), None)
    if pin and adapter:
        wanted = re.search(r'prepare\(\s*\w+\s*,\s*\'([0-9a-f]{40})\'', adapter)
        if wanted:
            require(wanted.group(1) == pin,
                    f'feeds.toml pins IOStorageFamily {pin[:8]}, but the core adapter accepts '
                    f'{wanted.group(1)[:8]}: pick one of the two')

    # This repository's copy of the header helper must expose the entry point the adapter imports.
    helper = read(ROOT / 'scripts/prepare-storage-headers.py')
    if helper is not None:
        signature = re.search(r'^def prepare\(([^)]*)\)', helper, re.M)
        require(signature is not None and
                [name.strip() for name in signature.group(1).split(',')][:3] == ['source', 'revision', 'out'],
                'scripts/prepare-storage-headers.py: prepare() must be prepare(source, revision, out), the '
                'signature build-sheng-source-storage.py calls')

    if problems:
        print('the core checkout cannot build this board:')
        for problem in problems:
            print(f'  - {problem}')
        print('\nThe core is checked out by ref, so push the contract there (or pin that ref) before a round.')
        return 1
    print(f'core contract {core}: ok for this board')
    return 0


if __name__ == '__main__':
    sys.exit(main())
