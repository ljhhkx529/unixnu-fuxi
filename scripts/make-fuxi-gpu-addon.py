#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the fuxi GPU addon payload with the publish marker (UniXNU scripts/make-gpu-addon.py).

The addon carries the owner-built Adreno Metal plugin, adrenoprobed and the daemon plists into
/private/var/ngpu at boot, and the kernel bindfs-mounts its addons/ directory over
/System/Library/Extensions so the original Metal discovery finds NativeAdrenoMetal.bundle.

--publish puts NativeAdrenoMetal.publish beside the bundle. Without that file the plugin only runs its own
tests: with it, it returns the native Metal device to backboardd's discovery and the render server composites
through it. That difference is the whole difference between a GPU that renders and one that does not, so this
script always passes --publish.

usage: make-fuxi-gpu-addon.py [--tree DIR] [--plugin DIR] OUTPUT
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ.get('UNIXNU_TREE', ROOT / '.cache/unixnu')),
                        help='the UniXNU tree that ships scripts/make-gpu-addon.py')
    parser.add_argument('--plugin', type=Path, default=None,
                        help='build/adreno-metal-plugin directory (default: the tree\'s own)')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()

    tool = args.tree / 'scripts/make-gpu-addon.py'
    if not tool.is_file():
        sys.exit(f'{tool} missing: point --tree at the UniXNU checkout')
    plugin = args.plugin or (args.tree / 'build/adreno-metal-plugin')
    if not (plugin / 'NativeAdrenoMetal.bundle').is_dir():
        sys.exit(f'{plugin}/NativeAdrenoMetal.bundle missing: build the plugin first')
    if args.output.exists():
        sys.exit(f'{args.output} exists: remove it first, the tool refuses to overwrite')
    environment = dict(os.environ, ADRENO_PLUGIN_DIR=str(plugin))
    subprocess.run([sys.executable, str(tool), '--publish', str(args.output)], env=environment, check=True)


if __name__ == '__main__':
    main()
