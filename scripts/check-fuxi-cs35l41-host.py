#!/usr/bin/env python3
"""Build and run the fuxi CS35L41 program tables' host tests (package/tests/fuxi_cs35l41_host.c).

The tables come from Kerfand's UniXNU-Fuxi-Arm64e-iOS17 and are run here
over a register model of the part, so the two properties P4 depends on are
checked before the device sees them: the init program identifies the part
before it configures it, and it leaves the amplifier powered down.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / "fuxi-cs35l41-host"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined",
        str(PACKAGE / "tests/fuxi_cs35l41_host.c"),
        str(PACKAGE / "FuxiCs35l41.c"),
        str(PACKAGE / "Cs35Run.c"),
        "-o", str(exe),
    ], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)

sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS") else 1)
