#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Generate the audited iPadOS 27 native capability adapter and its patch report."""
import argparse
import json
from pathlib import Path
from native_ios27 import NativeImage

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    image = NativeImage(args.image).vendor_routing().volatile_root_keys().h4_shutdown().capability_adapter()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image.data)
    report = args.output.with_suffix(args.output.suffix + '.json')
    report.write_text(json.dumps(image.report(), indent=2) + '\n')
    print(f'Native capability adapter: {args.output}; report: {report}')
