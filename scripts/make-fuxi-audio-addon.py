#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Add the board's audio server plug-in to the GPU addon (UniXNU scripts/addon-add-dir.py).

  make-fuxi-audio-addon.py --tree DIR --addon IN.ngpu --audio DIR --output OUT.ngpu

The kernel writes every file of the addon into /private/var/ngpu/<path> at boot
(native_gpu_addon_install in bsd/kern/kern_exec.c), and the fourth bindfs mount of the
GPU addon preparation shows /private/var/ngpu/audio over /System/Library/Audio/Plug-Ins/HAL
(patches/xnu/fuxi-audio-addon-mount.patch, UniXNU-sheng's zzyd-sheng-audio-addon-mount.patch).

So the bundle travels as audio/FuxiAudio.driver/* inside the addon: the audio server then
finds the owner's plug-in next to the system's own HAL drivers, and the plug-in's CDHash
is trusted through the owner trust module (scripts/make-fuxi-owner-trust.py).
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
                        help='the UniXNU tree that ships scripts/addon-add-dir.py')
    parser.add_argument('--addon', type=Path, required=True, help='the GPU addon to extend')
    parser.add_argument('--audio', type=Path, required=True,
                        help='the directory that holds FuxiAudio.driver')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    tool = args.tree / 'scripts/addon-add-dir.py'
    if not tool.is_file():
        sys.exit(f'{tool} missing: point --tree at the UniXNU checkout')
    bundle = args.audio / 'FuxiAudio.driver'
    if not bundle.is_dir():
        sys.exit(f'{bundle} missing: build it with scripts/build-fuxi-audio-hal.py')
    if not (bundle / 'FuxiAudio').is_file():
        sys.exit(f'{bundle}/FuxiAudio missing: the bundle is incomplete')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.unlink(missing_ok=True)
    subprocess.run([sys.executable, str(tool), str(args.addon), str(args.output),
                    'audio/FuxiAudio.driver', str(bundle)], check=True)
    print(f'{args.output}: the audio server plug-in travels as audio/FuxiAudio.driver/')


if __name__ == '__main__':
    main()
