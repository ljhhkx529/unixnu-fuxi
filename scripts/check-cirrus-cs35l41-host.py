#!/usr/bin/env python3
"""Build and run the CS35L41 safe bring-up host model."""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"


with tempfile.TemporaryDirectory() as temporary:
    executable = Path(temporary) / "cirrus-cs35l41-host"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined",
        str(PACKAGE / "tests/cirrus_cs35l41_host.c"),
        str(PACKAGE / "CirrusCs35l41.c"),
        "-o", str(executable),
    ], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)

sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS CirrusCs35l41:") else 1)
