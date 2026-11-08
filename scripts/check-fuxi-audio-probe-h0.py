#!/usr/bin/env python3
"""H0: the default boot path has no audio-probe caller.

The first host gate of docs/fuxi-audio-probe-debug-runbook.md. It answers one question with a search rather than an
opinion: can anything in a default build reach native_audio_probe()?

  check-fuxi-audio-probe-h0.py [repo]

Result vocabulary follows the runbook: PASS, FAIL, or INCONCLUSIVE when the check itself cannot be trusted.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
PROBE = ROOT / "package/FuxiAudioProbe.cpp"
CALLERS = ("package", "drivers", "scripts", "patches")

problems: list[str] = []
notes: list[str] = []


def sources() -> list[Path]:
    files: list[Path] = []
    for top in CALLERS:
        base = ROOT / top
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix not in (".c", ".cc", ".cpp", ".h", ".hpp", ".py", ".patch") or not path.is_file():
                continue
            # A checker or a documentation generator is not a build input: naming the function there is not a call.
            if path.name.startswith("check-") or path.name.startswith("report-"):
                continue
            files.append(path)
    return files


def main() -> int:
    if not PROBE.is_file():
        print("INCONCLUSIVE fuxi-audio-probe H0: package/FuxiAudioProbe.cpp is missing")
        return 1

    definition = 0
    calls: list[str] = []
    for path in sources():
        try:
            text = path.read_text(errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if "native_audio_probe" not in line:
                continue
            stripped = line.strip()
            relative = path.relative_to(ROOT)
            # A declaration ends in ';' and a definition carries the body; anything else that names it is a call.
            if re.search(r"void\s+native_audio_probe\s*\(\s*(void)?\s*\)\s*;", stripped):
                notes.append(f"declaration: {relative}:{lineno}")
                continue
            if re.search(r"void\s+native_audio_probe\s*\(\s*(void)?\s*\)\s*\{?\s*$", stripped):
                definition += 1
                notes.append(f"definition: {relative}:{lineno}")
                continue
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
                continue
            calls.append(f"{relative}:{lineno}: {stripped[:80]}")

    if definition == 0:
        problems.append("no definition of native_audio_probe found, so this search proves nothing")
    for call in calls:
        problems.append(f"call site: {call}")

    # The boot argument must not be in the default command line either.
    board = ROOT / "board.toml"
    if board.is_file():
        text = board.read_text()
        for lineno, line in enumerate(text.splitlines(), 1):
            if "fuxi-audio-probe" in line and not line.strip().startswith("#"):
                problems.append(f"board.toml:{lineno} passes fuxi-audio-probe: {line.strip()[:80]}")
    else:
        problems.append("board.toml is missing, so the command line was not checked")

    result = "FAIL" if problems else "PASS"
    print(f"{result} fuxi-audio-probe H0: default boot path has no audio-probe caller")
    for note in notes:
        print(f"  {note}")
    for problem in problems:
        print(f"  {problem}")
    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
