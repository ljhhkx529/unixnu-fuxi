#!/usr/bin/env python3
"""Build and run the modem RSC layout host check."""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"


with tempfile.TemporaryDirectory() as temporary:
    executable = Path(temporary) / "fuxi-rsc-layout-host"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined",
        "-I", str(PACKAGE),
        str(PACKAGE / "tests/fuxi_rsc_layout_host.c"),
        str(PACKAGE / "QcomRpmh.c"),
        "-o", str(executable),
    ], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)

sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS fuxi_rsc_layout_test") else 1)
