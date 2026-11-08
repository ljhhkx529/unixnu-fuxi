#!/usr/bin/env python3
"""Build and run the GENI I2C core's host tests (package/tests/geni_i2c_host.c).

The core and its tests are sheng's, copied unchanged: the event-driven transfer, the cancel-then-abort escalation and
the error classification are the mechanism this board's amplifiers are reached through, so fuxi does not re-invent
them. What differs per board - the engine's address, its clock and the amplifier's register file - is a value.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / "geni-i2c-host"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                    "-fsanitize=address,undefined",
                    str(PACKAGE / "tests/geni_i2c_host.c"), str(PACKAGE / "GeniI2c.c"), "-o", str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS") else 1)
