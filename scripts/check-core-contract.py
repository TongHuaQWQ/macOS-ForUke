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
import os
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return path.read_text() if path.is_file() else None


def defconfig_values(path):
    """The CONFIG_XNU_* keys of a defconfig, as the kernel step's xnu_env() would see them."""
    values = {}
    for line in read(path).splitlines() if path.is_file() else []:
        line = line.strip()
        if not line.startswith('CONFIG_XNU_') or '=' not in line:
            continue
        key, _, value = line.partition('=')
        value = value.split('#')[0].strip()
        values[key[len('CONFIG_'):]] = value.strip('"')
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, default=Path('../UniXNU'),
                        help='the UniXNU checkout the kernel workflow uses')
    parser.add_argument('--defconfig', default=os.environ.get('UKE_DEFCONFIG', 'uke-macos_defconfig'),
                        help='the configuration the workflow builds (default: UKE_DEFCONFIG)')
    args = parser.parse_args()
    core = args.core.resolve()
    board = tomllib.loads((ROOT / 'board.toml').read_text())
    feeds = tomllib.loads((ROOT / 'feeds.toml').read_text()) if (ROOT / 'feeds.toml').is_file() else {}
    root, userdata, payload = board.get('root', {}), board.get('userdata', {}), board.get('payload', {})
    archives = board.get('kernel', {}).get('archives', [])
    config = defconfig_values(ROOT / 'configs' / f'{args.defconfig}')
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

    # The board's own half of the same handoff. Each item is a way the build can be silently wrong for a
    # whole round instead of failing: the official boards set all of them together (Allenwdk/UniXNU-uke
    # configs/uke_ipados27_defconfig:20-27 with board.toml:230), so every one is checked together here.
    header_only = root.get('tables_in_userdata') or config.get('XNU_SOURCE_STORAGE_TABLES') == 'userdata'
    if header_only:
        require(config.get('XNU_SOURCE_STORAGE') == 'y',
                f'configs/{args.defconfig}: [root] tables_in_userdata wants the source runtime, but '
                'CONFIG_XNU_SOURCE_STORAGE=y is missing')
        require(config.get('XNU_SOURCE_STORAGE_TABLES') == 'userdata',
                f'configs/{args.defconfig}: CONFIG_XNU_SOURCE_STORAGE_TABLES="userdata" is missing; that key '
                'is the official spelling (Allenwdk/UniXNU-uke configs/uke_ipados27_defconfig:22)')
        bundle = payload.get('root_policies')
        require(bundle is not None and 'sheng-source-storage/root-policies.bin' in bundle,
                '[payload] root_policies is missing or does not name '
                '{tree}/build/sheng-source-storage/root-policies.bin: the kernel then carries no policy to '
                'authenticate the root against (payload.py:145-158 ships it as kernel.macho.vroot)')
        entry = next((item for item in archives if 'build-sheng-source-storage.py' in item), None)
        require(entry is not None,
                '[kernel] archives names no build-sheng-source-storage.py entry; the official boards put it '
                'right after component:ShengUfs with --policy-dir (unixnu.com/zh/devices/sheng#arm64e-os)')
        if entry is not None:
            directory = re.search(r'--policy-dir\s+(\S+)', entry)
            require(directory is not None,
                    f'[kernel] archives: "{entry}" passes no --policy-dir')
            if directory is not None:
                policy_dir = ROOT / directory.group(1)
                require(policy_dir.is_dir(),
                        f'[kernel] archives: {directory.group(1)} does not exist')
                for window in (config.get('XNU_SOURCE_STORAGE_POLICIES') or 'OS,SystemOS').split(','):
                    for name in ('manifest.header.bin', 'trusted-key.bin', 'verification.json'):
                        require((policy_dir / window / name).is_file(),
                                f'{directory.group(1)}/{window}/{name}: missing (the adapter reads all three)')
        # One force_load of the storage archive, not two: CONFIG_XNU_NATIVE_UFS already links
        # $ROOT/build/sheng-ufs/libShengUfs.a, which is where the adapter copies its output.
        extra = config.get('XNU_EXTRA_LD_ARCHIVES', '')
        if config.get('XNU_NATIVE_UFS') == 'y':
            require('libShengSourceStorage.a' not in extra,
                    f'configs/{args.defconfig}: libShengSourceStorage.a is in CONFIG_XNU_EXTRA_LD_ARCHIVES '
                    'while CONFIG_XNU_NATIVE_UFS=y already force-loads the same archive through '
                    'build-xnu-4k-baseline.sh:678-683; every symbol in it would collide')
        # The runtime compiles device/drivers/ShengUfs/NativeUfs.cpp with -DNATIVE_UFS_SOURCE_API=1, and
        # that is a different driver from the classic bring-up one. The two are separate directories in
        # the official same-device repository; ours must not point the feed at the classic driver, which
        # also defines the verified-block helpers SourceRootBinding.cpp brings.
        feed = next((entry.get('local') for entry in feeds.get('source', [])
                     if entry.get('name') == 'ShengUfs'), None)
        require(feed is not None, 'feeds.toml: no local ShengUfs source; the runtime has no driver to compile')
        if feed is not None:
            driver = read(ROOT / feed / 'NativeUfs.cpp') or ''
            require('NATIVE_UFS_SOURCE_API' in driver,
                    f'feeds.toml: ShengUfs points at {feed}, which is not the source-API driver (no '
                    'NATIVE_UFS_SOURCE_API); build-sheng-source-storage.py compiles this path with that define')

    # SourceStorage.cpp publishes SoftwareAES unconditionally, so the archive that builds it has to be in
    # the link. This board builds build/software-aes/libSoftwareAES.a but never loaded it until now.
    if (ROOT / 'package/SourceStorage.cpp').is_file() and config.get('XNU_NATIVE_AES') != 'y':
        require('libSoftwareAES.a' in config.get('XNU_EXTRA_LD_ARCHIVES', ''),
                f'configs/{args.defconfig}: package/SourceStorage.cpp calls native_software_aes_create() and '
                'friends, but neither CONFIG_XNU_NATIVE_AES=y nor a libSoftwareAES.a in '
                'CONFIG_XNU_EXTRA_LD_ARCHIVES provides them')

    if problems:
        print('the build inputs do not agree with what this board declares:')
        for problem in problems:
            print(f'  - {problem}')
        print('\nThe core is checked out by ref, so push the contract there (or pin that ref) before a round; '
              'the board items are in this checkout.')
        return 1
    print(f'core contract {core}: ok for this board ({args.defconfig})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
