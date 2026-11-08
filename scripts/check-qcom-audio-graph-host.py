#!/usr/bin/env python3
"""Build and run the graph-open host model."""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"


with tempfile.TemporaryDirectory() as temporary:
    executable = Path(temporary) / "qcom-audio-graph-host"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined",
        str(PACKAGE / "tests/qcom_audio_graph_host.c"),
        str(PACKAGE / "QcomAudioGraph.c"),
        str(PACKAGE / "ShengGpr.c"),
        "-o", str(executable),
    ], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)

sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS QcomAudioGraph:") else 1)
