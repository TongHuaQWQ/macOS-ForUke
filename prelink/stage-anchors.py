#!/usr/bin/env python3
"""Stage this board's reviewed pairing anchors into the prelink kit.

kit/XnuPrelink reviews a handful of original functions by hand when no shape method can reach
them, and it keys every record by the kernelcache it was reviewed against
(fixtures/fileset-<build>-symbols.json, inputs.kernelcache_sha256). That pin is a property of the
board - it is board.toml [kernel.prelink] kernelcache_sha256 - so the records live in this
repository, and the kit only needs them at build time. This copies them in after checking that the
pin actually matches the board, so an IPSW bump cannot silently apply the wrong identities.
"""
import argparse
import json
import re
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--device', default='.', type=Path, help='device repository root')
parser.add_argument('--core', required=True, type=Path, help='core UniXNU checkout')
options = parser.parse_args()

fixtures = sorted((options.device / 'prelink').glob('fileset-*-symbols.json'))
if not fixtures:
    sys.exit(f'error: no prelink/fileset-*-symbols.json under {options.device}')
board_text = (options.device / 'board.toml').read_text()
found = re.search(r'^kernelcache_sha256\s*=\s*"([0-9a-fA-F]{64})"', board_text, re.M)
if not found:
    sys.exit('error: board.toml has no [kernel.prelink] kernelcache_sha256')
board_pin = found.group(1).lower()
target = options.core / 'kits/XnuPrelink/fixtures'
if not target.is_dir():
    sys.exit(f'error: {target} is not a directory (is the XnuPrelink submodule checked out?)')
staged = 0
for path in fixtures:
    record = json.loads(path.read_text())
    pinned = record['inputs']['kernelcache_sha256'].lower()
    if pinned != board_pin:
        print(f'skip {path.name}: reviewed against {pinned}, board pins {board_pin}')
        continue
    (target / path.name).write_text(path.read_text())
    print(f'staged {path.name}: {len(record["functions"])} reviewed functions for {pinned[:16]}')
    staged += 1
if not staged:
    sys.exit('error: no reviewed anchor file matches this board\'s kernelcache pin')
