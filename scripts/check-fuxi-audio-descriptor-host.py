#!/usr/bin/env python3
"""Sanitized host gate for the fixed G2.1 descriptor lifecycle."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"
clang = shutil.which("clang")
if not clang:
    sys.exit("clang is required")
with tempfile.TemporaryDirectory(prefix="fuxi-audio-descriptor-") as temporary:
    executable = Path(temporary) / "descriptor-host"
    command = [clang, "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               str(PACKAGE / "tests/fuxi_audio_descriptor_host.c"),
               str(PACKAGE / "FuxiAudioDescriptor.c"), "-o", str(executable)]
    subprocess.run(command, check=True)
    result = subprocess.run([executable], text=True, capture_output=True)
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS FuxiAudioDescriptor:") else 1)
