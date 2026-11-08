#!/usr/bin/env python3
"""Build the audio probe's archive: package/FuxiAudioProbe.cpp and the SMEM driver it reads through.

  build-fuxi-audio-probe.py OUTDIR [TARGET]

TARGET is the clang target of the kernel (default arm64-apple-ios17.0). Writes OUTDIR/libFuxiAudioProbe.a and records
the object list in OUTDIR/objects.txt. The probe is inert unless the boot argument fuxi-audio-probe is set.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"
if len(sys.argv) not in (2, 3):
    sys.exit(__doc__)
out = Path(sys.argv[1]).resolve()
target = sys.argv[2] if len(sys.argv) == 3 else "arm64-apple-ios17.0"
sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
out.mkdir(parents=True, exist_ok=True)

sources = [(PACKAGE / "FuxiAudioProbe.cpp", "cc"), (PACKAGE / "QcomSmem.c", "cc")]
objects = []
with tempfile.TemporaryDirectory() as tmp:
    for source, _ in sources:
        obj = Path(tmp) / (source.stem + ".o")
        subprocess.run(["xcrun", "clang", "-target", target, "-isysroot", sdk, "-I", str(PACKAGE),
                        "-O2", "-g0", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-fno-rtti",
                        "-c", str(source), "-o", str(obj)], check=True)
        objects.append(obj)
    archive = out / "libFuxiAudioProbe.a"
    if archive.exists():
        archive.unlink()
    subprocess.run(["xcrun", "libtool", "-static", "-o", str(archive)] + [str(o) for o in objects], check=True)
(out / "objects.txt").write_text("\n".join(o.name for o in objects) + "\n")
print(f"{archive}")
