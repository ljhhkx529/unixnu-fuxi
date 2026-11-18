#!/usr/bin/env python3
"""Check that the audio area's record matches the files and scripts that exist.

A porting record is only useful while it describes the tree it sits in. Twice this session the audio area's procedure
list named scripts that had been renamed or deleted, and the drift was only found by reading. This makes it a check:
every path glob the area declares must match something, every script the procedure names must exist, and every suite the
status report runs must have its script.
"""

from __future__ import annotations

import glob
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    problems: list[str] = []
    record_text = (ROOT / "porting.toml").read_text()
    record = tomllib.loads(record_text)
    areas = [a for a in record.get("area", []) if a.get("id") == "audio"]
    if len(areas) != 1:
        print(f"FAIL expected one audio area, found {len(areas)}")
        return 1
    area = areas[0]

    # Every declared path matches at least one file.
    for pattern in area.get("paths", []):
        if not glob.glob(str(ROOT / pattern), recursive=True):
            problems.append(f"paths entry {pattern!r} matches no file")

    # Every script named anywhere in the record's procedure or findings exists.
    named = set(re.findall(r"scripts/[A-Za-z0-9_.-]+\.py", " ".join(
        text for area in record.get("area", [])
        for text in area.get("procedure", []) + area.get("findings", [])
        + [evidence.get("command", "") for evidence in area.get("evidence", [])])))
    for script in sorted(named):
        if (ROOT / script).is_file():
            continue
        # A script of the UniXNU checkout lives in that checkout, not here. It is accepted when
        # a file of that checkout ends with the same path.
        unixnu = ROOT.parent / "UniXNU"
        if unixnu.is_dir() and any(path.endswith("/" + script)
                                   for path in glob.glob(str(unixnu / "**" / Path(script).name), recursive=True)):
            continue
        problems.append(f"the record names {script}, which does not exist")

    # Every suite the status report runs has its script.
    report = (ROOT / "scripts/report-fuxi-audio-status.py").read_text()
    for script in sorted(set(re.findall(r'"(scripts/[A-Za-z0-9_.-]+\.py)"', report))):
        if not (ROOT / script).is_file():
            problems.append(f"the status report runs {script}, which does not exist")

    # Every check script in the tree is reachable from the report or is the record checks themselves.
    for path in sorted(glob.glob(str(ROOT / "scripts/check-*.py"))):
        name = Path(path).name
        if name not in report and f"scripts/{name}" not in named:
            problems.append(f"scripts/{name} exists but nothing runs or records it")

    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    print(f"PASS fuxi audio records: {len(area.get('paths', []))} paths, {len(named)} scripts named, "
          f"{len(area.get('procedure', []))} procedure steps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
