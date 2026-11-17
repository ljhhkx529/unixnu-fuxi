#!/usr/bin/env python3
"""Build and run the amplifier bring-up plan's host tests (package/tests/fuxi_amp_bringup_host.c).

The plan is pure arithmetic over the board's facts - which branches to
vote for, how to read their state, what the pin and clock words are - so it
can be checked on the host before a single register is touched.  The rule
that matters is the one Linux's clk-branch.c applies: a branch in hardware
gating mode is never waited for, so its halt bit says nothing.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / "fuxi-amp-bringup-host"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined",
        str(PACKAGE / "tests/fuxi_amp_bringup_host.c"),
        str(PACKAGE / "FuxiAmpBringup.c"),
        "-o", str(exe),
    ], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)

sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS") else 1)
