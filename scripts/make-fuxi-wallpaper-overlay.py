#!/usr/bin/env python3
"""Build the fuxi wallpaper overlay on a Linux host: PNG copies of the sealed HEIC layers of the default wallpaper's animation.

  make-fuxi-wallpaper-overlay.py OS_DISK_IMAGE OUTPUT_DIR

The Linux counterpart of the core scripts/make-wallpaper-overlay.py, which reads a mounted OS volume and converts with macOS sips. Here 7z
takes the sealed animation straight out of the IPSW's OS disk image, with its DMG and APFS handlers, and pillow-heif decodes the HEIC layers,
so neither a mount nor macOS is needed. OUTPUT_DIR gets the animation's index.xml and assetManifest.caml and a main.caml that points at
J120.png and J120-Dark.png next to it, scaled to EDGE pixels. scripts/make-gpu-addon.py --wallpaper OUTPUT_DIR carries it to the kernel, which
mounts it over the sealed animation (patches/xnu/zzy-native-gpu-addon-mount.patch).

Run it with an interpreter that has PIL and pillow-heif; on this host that is the image venv (OVERLAY_PYTHON in build-boot-image.sh).
"""
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image
    import pillow_heif
except ImportError:
    sys.exit('needs PIL and pillow-heif: run it with the image venv python (python -m pip install pillow-heif)')

ANIMATION = ('Library/Wallpaper/Collections/iPadOS_17~ipad.wallpaperCollection/Wallpapers/7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper/'
             '7570.iPadOS_17_Flat_Background-1024w-1366h@2x~ipad.ca')
IMAGES = ('J120', 'J120-Dark')
# The long edge the two layers are sampled at. The core script uses 2732, the iPad's artwork-device-subtype; fuxi's ADT carries 2556.
EDGE = 2556
ENTRIES = ['index.xml', 'assetManifest.caml', 'main.caml'] + [f'assets/{name}.HEIC' for name in IMAGES]


def seven_zip(args, what):
    done = subprocess.run(['7z', *args], capture_output=True, text=True)
    if done.returncode != 0:
        sys.exit(f'7z {what} failed (rc {done.returncode}):\n{done.stdout}\n{done.stderr}')
    return done.stdout


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dmg, output = Path(sys.argv[1]), Path(sys.argv[2])
    if not dmg.is_file():
        sys.exit(f'no OS disk image at {dmg}')
    if output.exists():
        sys.exit('Refuse to overwrite ' + str(output))
    pillow_heif.register_heif_opener()
    with tempfile.TemporaryDirectory() as tmp:
        seven_zip(['x', '-y', '-bsp0', '-bso0', f'-o{tmp}', str(dmg)] + [f'{ANIMATION}/{name}' for name in ENTRIES], 'extracting')
        source = Path(tmp) / ANIMATION
        for name in ENTRIES:
            if not (source / name).is_file():
                sys.exit(f'{dmg} has no {ANIMATION}/{name}')
        output.mkdir(parents=True)
        for name in ('index.xml', 'assetManifest.caml'):
            (output / name).write_bytes((source / name).read_bytes())
        main_caml = (source / 'main.caml').read_text()
        for image in IMAGES:
            main_caml = main_caml.replace(f'"/assets/{image}.HEIC"', f'"/{image}.png"').replace(f'"assets/{image}.HEIC"', f'"{image}.png"')
            with Image.open(source / 'assets' / f'{image}.HEIC') as heic:
                heic.convert('RGB').resize((EDGE, EDGE), Image.LANCZOS).save(
                    output / f'{image}.png', compress_level=9, optimize=True)
        if '.HEIC"/>' in main_caml:
            sys.exit('main.caml still points at a HEIC image')
        (output / 'main.caml').write_text(main_caml)
    for path in sorted(output.iterdir()):
        print(f'{path.stat().st_size:>10} {path.name}')
    print(f'{output}: {EDGE} px layers, packed by make-gpu-addon.py --wallpaper')


if __name__ == '__main__':
    main()
