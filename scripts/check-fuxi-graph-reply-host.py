#!/usr/bin/env python3
"""Build and run the host-only graph reply matcher checks."""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    ROOT / "tests/fuxi_graph_reply_host.c",
    ROOT / "package/FuxiGraphReply.c",
    ROOT / "package/ShengGpr.c",
]
CC = "cc"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="fuxi-graph-reply-") as tmp:
        exe = Path(tmp) / "fuxi_graph_reply_host"
        command = [CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "package"),
                   *map(str, SOURCES), "-o", str(exe)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(exe)], check=False, text=True, capture_output=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
