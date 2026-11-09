#!/usr/bin/env python3
"""H8 of the audio-probe debug runbook: the mapping helper's alignment, length and symmetry contract."""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"

with tempfile.TemporaryDirectory() as temporary:
    executable = Path(temporary) / "qcom-mapping-host"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(PACKAGE / "tests/qcom_mapping_host.c"),
        str(PACKAGE / "QcomMapping.c"),
        "-o", str(executable),
    ], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)

sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS QcomMapping:") else 1)
