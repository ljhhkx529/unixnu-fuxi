#!/usr/bin/env python3
"""Generate, compile and execute the typed Fuxi audio binding on the host."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="fuxi-audio-binding-") as temporary:
    out = Path(temporary)
    generated = out / "FuxiAudioBindingData.inc"
    subprocess.run([sys.executable, ROOT / "scripts/gen-fuxi-audio-binding.py", "--output", generated], check=True)
    executable = out / "fuxi-audio-binding-host"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", ROOT / "package", "-I", out,
                    ROOT / "package/FuxiAudioBinding.c", ROOT / "package/tests/fuxi_audio_binding_host.c",
                    "-o", executable], check=True)
    result = subprocess.run([executable], text=True, capture_output=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    raise SystemExit(result.returncode)
