#!/usr/bin/env python3
"""Build and run the end-to-end audio session host model."""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "package"


with tempfile.TemporaryDirectory() as temporary:
    executable = Path(temporary) / "qcom-audio-session-host"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined",
        str(PACKAGE / "tests/qcom_audio_session_host.c"),
        str(PACKAGE / "QcomAudioSession.c"),
        str(PACKAGE / "QcomAdsp.c"),
        str(PACKAGE / "QcomAudioPath.c"),
        str(PACKAGE / "QcomAudioReach.c"),
        str(PACKAGE / "QcomMdt.c"),
        str(PACKAGE / "QcomQmp.c"),
        str(PACKAGE / "QcomSmp2p.c"),
        str(PACKAGE / "QcomGlink.c"),
        str(PACKAGE / "ShengGpr.c"),
        "-o", str(executable),
    ], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)

sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith("PASS QcomAudioSession:") else 1)
