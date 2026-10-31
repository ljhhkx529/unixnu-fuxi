#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Put the fuxi FTS touch probe into a UniXNU-sheng checkout, for a compile and link check inside a real XNU build.

sheng and fuxi share the touch bus (QUP1 SE4, pins 44..47, interrupt pin 25), so the sheng kernel build compiles the
fuxi probe with the same flags, include paths and link as its own TouchProbe.cpp. The result is a sheng kernel whose
touch probe answers to `fuxi-touch=` instead of `sheng-touch=`: it is a build check, not an image to boot.

usage: integrate-sheng.py <this repository> <UniXNU-sheng checkout>
"""
import shutil
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    repo, device = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    package = device / 'package'
    script = device / 'scripts/build-gop-legacy-display.py'
    if not (package / 'TouchProbe.cpp').is_file() or not script.is_file():
        sys.exit(f'{device} does not look like a UniXNU-sheng checkout')

    copies = {
        repo / 'package/FuxiTouchProbe.cpp': package / 'TouchProbe.cpp',
        repo / 'include/FuxiTouchBoard.h': package / 'FuxiTouchBoard.h',
    }
    for name in ('FtsProtocol.c', 'FtsProtocol.h', 'FtsTouchCore.c', 'FtsTouchCore.h'):
        copies[repo / 'drivers/FuxiFtsTouch' / name] = package / name
    for source, target in copies.items():
        shutil.copyfile(source, target)
        print(f'{source.relative_to(repo)} -> {target.relative_to(device)}')

    # The touch C sources are compiled with the kernel C flags into the display archive; add the FTS ones there. QcomRpmh.c,
    # which the probe also uses, is already in that archive (the audio DSP list).
    text = script.read_text()
    anchor = "ROOT/'build/native-touch/NvtFirmware.c']"
    if text.count(anchor) != 1:
        sys.exit(f'{script.name}: expected one touch source list ending with {anchor}')
    text = text.replace(anchor, "ROOT/'build/native-touch/NvtFirmware.c',ROOT/'kits/sheng/FtsProtocol.c',ROOT/'kits/sheng/FtsTouchCore.c']")
    if "'QcomRpmh'" not in text:
        sys.exit(f'{script.name}: QcomRpmh.c is no longer built; add it to the touch sources')
    script.write_text(text)
    print(f'{script.relative_to(device)}: FtsProtocol.c and FtsTouchCore.c added to the touch sources')


if __name__ == '__main__':
    main()
