#!/usr/bin/env python3
"""The porting hook's own test: a rule that is never exercised is a rule that might not work.

Each case feeds the hook a tool call and reads its decision back. The cases cover the four
rules and, for rule 4, both directions: a write to an unrelated file is refused while a defect
is waiting, and a write to the defect's own file is allowed.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOOK = ROOT / ".claude/hooks/porting_rules.py"

# The hook's own state lives beside it. The test points it at a scratch copy so a run never
# disturbs the real state.
STATE = HOOK.parent / "state.json"


def run_hook(payload: dict, state_path: Path, blockers_path: Path,
             map_path: Path | None = None) -> str:
    """Run the hook with a payload and return its decision."""
    environment = {"PATH": "/usr/bin:/bin:/usr/local/bin",
                   "PORTING_HOOK_STATE": str(state_path),
                   "PORTING_HOOK_BLOCKERS": str(blockers_path)}
    if map_path is not None:
        environment["PORTING_HOOK_MAP"] = str(map_path)
    result = subprocess.run(
        [sys.executable, str(HOOK)],
        input=json.dumps(payload),
        capture_output=True,
        text=True,
        env=environment,
    )
    if result.returncode != 0:
        raise SystemExit(f"the hook failed: {result.stderr}")
    output = json.loads(result.stdout)
    return output["hookSpecificOutput"]["permissionDecision"]


def write_call(path: str) -> dict:
    return {"tool_name": "Write", "tool_input": {"file_path": path, "content": "x"}}


def read_call(path: str) -> dict:
    return {"tool_name": "Read", "tool_input": {"file_path": path}}


def grep_call(pattern: str) -> dict:
    return {"tool_name": "Grep", "tool_input": {"pattern": pattern, "path": "/tmp/lx/linux"}}


def map_with(rows: str, pending: str = "") -> str:
    """A scratch map with the given table rows and an optional pending body."""
    text = (
        "# Map\n\n"
        "| 参数 | 来源 | 目标 |\n"
        "| --- | --- | --- |\n"
        f"{rows}\n"
    )
    if pending:
        text += f"\n## 待查的上游源码\n\n{pending}\n"
    return text


def main() -> int:
    failures = 0

    with tempfile.TemporaryDirectory() as temporary:
        scratch = Path(temporary)
        state = scratch / "state.json"

        # Rule 4 is exercised against a scratch blockers file, so the repository's own
        # blockers never gate the test.
        blockers = scratch / "PORTING_BLOCKERS.md"
        blockers.write_text(
            "# Blockers\n\n"
            "## 已发现的缺陷\n\n"
            "### a defect in the qrtr layer\n\n"
            "The framing is wrong in `package/FuxiModemPas.h`.\n",
            encoding="utf-8")

        # A write to an unrelated file is refused while the defect waits.
        state.write_text(json.dumps({"defects": {"a defect in the qrtr layer":
                                                 "package/FuxiModemPas.h"}}), encoding="utf-8")
        decision = run_hook(write_call(str(ROOT / "package/Other.c")), state, blockers)
        if decision != "deny":
            print("FAIL: an unrelated write was allowed while a defect was waiting")
            failures += 1

        # A write to the defect's own file is allowed: that is the fix.
        decision = run_hook(write_call(str(ROOT / "package/FuxiModemPas.h")), state, blockers)
        if decision != "allow":
            print("FAIL: the write that fixes the defect was refused")
            failures += 1

        # With the notes free of defects, a write goes through. The notes are the source of
        # truth: an empty state file does not release the block while the notes still name a
        # defect.
        empty_blockers = scratch / "no-blockers.md"
        empty_blockers.write_text("# Blockers\n\nnothing is wrong\n", encoding="utf-8")
        state.write_text(json.dumps({"defects": {}}), encoding="utf-8")
        decision = run_hook(write_call(str(ROOT / "package/Other.c")), state, empty_blockers)
        if decision != "allow":
            print("FAIL: a write was refused with no defect noted")
            failures += 1

        # A defect in the notes blocks an unrelated write even when the state is empty.
        state.write_text(json.dumps({"defects": {}}), encoding="utf-8")
        decision = run_hook(write_call(str(ROOT / "package/Other.c")), state, blockers)
        if decision != "deny":
            print("FAIL: a defect in the notes did not block an unrelated write")
            failures += 1

        # ---- rule 2: the map is the only entry point ----

        map_path = scratch / "PORTING_MAP.md"
        # The map cites the RPMh source for the rail vote, so reading it again is refused.
        map_path.write_text(map_with(
            "| 轨投票 | Linux `drivers/pmdomain/qcom/rpmhpd.c` | `package/QcomRpmh.c` |\n"),
            encoding="utf-8")
        decision = run_hook(read_call("/tmp/lx/linux/drivers/pmdomain/qcom/rpmhpd.c"),
                            state, blockers, map_path)
        if decision != "deny":
            print("FAIL: reading a source the map cites was allowed")
            failures += 1

        # A file the map lists as pending work is a declared work item: reading it is allowed.
        map_path.write_text(map_with(
            "| 轨投票 | Linux `drivers/pmdomain/qcom/rpmhpd.c` | `package/QcomRpmh.c` |\n",
            pending="`drivers/pmdomain/qcom/rpmhpd.c` 的 corner 语义还没有写进 map"),
            encoding="utf-8")
        decision = run_hook(read_call("/tmp/lx/linux/drivers/pmdomain/qcom/rpmhpd.c"),
                            state, blockers, map_path)
        if decision != "allow":
            print("FAIL: reading a file the map lists as pending work was refused")
            failures += 1

        # A file the map cites nowhere is new ground and may be read.
        map_path.write_text(map_with(
            "| 轨投票 | Linux `drivers/pmdomain/qcom/rpmhpd.c` | `package/QcomRpmh.c` |\n"),
            encoding="utf-8")
        decision = run_hook(read_call("/tmp/lx/linux/drivers/pmdomain/qcom/rpmh.c"),
                            state, blockers, map_path)
        if decision != "allow":
            print("FAIL: reading a file the map does not cite was refused")
            failures += 1

        # Our own code is never an upstream lookup, however many paths the map names.
        decision = run_hook(read_call(str(ROOT / "package/QcomRpmh.c")), state, blockers, map_path)
        if decision != "allow":
            print("FAIL: reading our own source was refused")
            failures += 1

        # The board's own device tree is first-hand evidence and is always allowed.
        decision = run_hook(read_call("/tmp/x13/work/device-info/fuxi-live.dts"),
                            state, blockers, map_path)
        if decision != "allow":
            print("FAIL: reading the board's own device tree was refused")
            failures += 1

        # ---- rule 3: a search for a parameter the map carries is refused ----

        map_path.write_text(map_with(
            "| 轨 `cx.lvl` / `mss.lvl` | Linux `rpmhpd.c` | `package/QcomRpmh.c` |\n"),
            encoding="utf-8")
        decision = run_hook(grep_call("rpmhpd.c"), state, blockers, map_path)
        if decision != "deny":
            print("FAIL: searching the upstream tree for a cited file was allowed")
            failures += 1

    # The hook must not crash on an empty payload.
    try:
        run_hook({}, scratch / "state.json", scratch / "PORTING_BLOCKERS.md")
    except SystemExit:
        print("FAIL: the hook failed on an empty payload")
        failures += 1

    if failures:
        print(f"FAIL porting rules: {failures} case(s)")
        return 1
    print("PASS porting rules: 12 cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
