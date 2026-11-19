#!/usr/bin/env python3
"""Assemble the root-policy bundle when the tree was never built by the storage adapter.

`[payload] root_policies` names {tree}/build/sheng-source-storage/root-policies.bin, and
build-sheng-source-storage.py writes it as the last thing it does — but that adapter runs inside the kernel
build on the CI runner (it needs xcrun, the kernel's compile records and the IOStorageFamily headers). A
payload built here from a downloaded kernel artifact therefore has no bundle, and payload.py refuses it:

    unixnu: Root-policy bundle missing or outside handoff bounds: .../build/sheng-source-storage/root-policies.bin

This writes the same file from the signed headers this repository carries. It is the adapter's own layout —
32-byte header, one (offset, length) pair per policy, then the 544-byte signed headers back to back:

    struct.pack('<8sQQQ', b'VROOTB01', 1, len(manifests), cursor) + table + b''.join(manifests)

and it refuses to do it unless every header is 544 bytes, describes the image [root] declares for its window,
and all of them share one owner anchor. Nothing is signed here and nothing is generated: the bytes are the
inputs scripts/policies/<build>/ ships, and the tables they authenticate stay in the root image as the
"<window>.vroot" data objects.

    python3 scripts/assemble-root-policy-bundle.py --tree <build tree> [--policy-dir DIR] [--output FILE]
"""
import argparse
import hashlib
import json
import struct
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True, help='the build tree [payload] root_policies expands against')
    parser.add_argument('--policy-dir', type=Path, help='default: the --policy-dir of the storage adapter entry in [kernel] archives')
    parser.add_argument('--output', type=Path, help='default: <tree>/build/sheng-source-storage/root-policies.bin')
    args = parser.parse_args()

    board = tomllib.loads((ROOT / 'board.toml').read_text())
    root = board.get('root', {})
    entry = next((item for item in board.get('kernel', {}).get('archives', [])
                  if 'build-sheng-source-storage.py' in item), None)
    if entry is None:
        raise SystemExit('[kernel] archives names no build-sheng-source-storage.py entry; no policy directory known')
    policy_dir = args.policy_dir
    if policy_dir is None:
        parts = entry.split()
        policy_dir = Path(parts[parts.index('--policy-dir') + 1]) if '--policy-dir' in parts else Path('scripts/policies')
    policy_dir = policy_dir if policy_dir.is_absolute() else ROOT / policy_dir

    bundle = (args.output or Path(args.tree) / 'build/sheng-source-storage/root-policies.bin')
    windows = [name for name in ('OS', 'SystemOS', 'App') if (policy_dir / name).is_dir()]
    if not windows:
        raise SystemExit(f'{policy_dir}: no OS/SystemOS/App window; nothing to bundle')

    manifests, key = [], None
    for window in windows:
        header = (policy_dir / window / 'manifest.header.bin').read_bytes()
        if len(header) != 544:
            raise SystemExit(f'{policy_dir / window / "manifest.header.bin"}: {len(header)} bytes, expected 544')
        digest = root.get(f'{window.lower()}_image_sha256')
        if not digest:
            raise SystemExit(f'board.toml [root] {window.lower()}_image_sha256 is missing')
        if header[56:88].hex() != digest:
            raise SystemExit(f'{policy_dir / window / "manifest.header.bin"}: describes {header[56:88].hex()} '
                             f'but [root] declares {digest}')
        anchor = (policy_dir / window / 'trusted-key.bin').read_bytes()
        if len(anchor) != 395:
            raise SystemExit(f'{policy_dir / window / "trusted-key.bin"}: {len(anchor)} bytes, expected 395')
        if key is None:
            key = anchor
        elif anchor != key:
            raise SystemExit(f'{policy_dir / window / "trusted-key.bin"}: the windows must share one owner anchor')
        verification = json.loads((policy_dir / window / 'verification.json').read_text())
        if verification.get('image_sha256') != digest or \
                verification.get('trusted_key_sha256') != hashlib.sha256(anchor).hexdigest() or \
                not verification.get('real_c_signature_and_block_verification'):
            raise SystemExit(f'{policy_dir / window / "verification.json"}: does not record a complete verification '
                             'of the image [root] declares')
        manifests.append(header)

    # Exactly build-sheng-source-storage.py's walk: each (offset, length) pair points at its header, and the
    # header's last field is the cursor AFTER the loop, i.e. the whole file's length — which is what
    # payload.py:155-158 checks against the file size.
    cursor = 32 + 16 * len(manifests)
    table = bytearray()
    for manifest in manifests:
        table += struct.pack('<QQ', cursor, len(manifest))
        cursor += len(manifest)
    data = struct.pack('<8sQQQ', b'VROOTB01', 1, len(manifests), cursor) + bytes(table) + b''.join(manifests)
    bundle.parent.mkdir(parents=True, exist_ok=True)
    bundle.write_bytes(data)
    print(f'PASS: {bundle} ({len(data)} bytes, {len(manifests)} policies: {", ".join(windows)}; '
          f'sha256 {hashlib.sha256(data).hexdigest()[:16]})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
