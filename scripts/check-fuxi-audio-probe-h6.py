#!/usr/bin/env python3
"""H6: prove the SMEM allocator's exact byte diff and final publication on a shadow image."""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"


with tempfile.TemporaryDirectory() as temporary:
    executable = Path(temporary) / "qcom-smem-shadow-host"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-DQCOM_SMEM_TEST_TRACE_WRITES",
        str(PACKAGE / "tests/qcom_smem_shadow_host.c"),
        str(PACKAGE / "QcomSmem.c"),
        "-o", str(executable),
    ], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)

sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS QcomSmemShadow:") else 1)
