#!/usr/bin/env python3
"""The TCSR mutex encoding against the mainline driver's semantics (G3's host half)."""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"

with tempfile.TemporaryDirectory() as temporary:
    executable = Path(temporary) / "qcom-tcsr-hwlock-host"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(PACKAGE / "tests/qcom_tcsr_hwlock_host.c"),
        str(PACKAGE / "QcomTcsrHwlock.c"),
        str(PACKAGE / "QcomHwspinlock.c"),
        "-o", str(executable),
    ], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)

sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS QcomTcsrHwlock:") else 1)
