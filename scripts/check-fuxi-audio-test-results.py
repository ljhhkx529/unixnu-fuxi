#!/usr/bin/env python3
"""Validate the append-only fuxi audio test-result database."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = ROOT / "tests/fuxi-audio/results.json"
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX = re.compile(r"^0x[0-9a-fA-F]+$")
SESSION = re.compile(r"^[0-9a-f]{16}$")
RESULTS = {"PASS_READY", "PASS_GLINK", "PASS_GPR", "PASS_GRAPH", "PASS_SPEAKER", "FAIL", "BLOCKED", "BLOCKED_GPR", "INCONCLUSIVE", "BLOCKED_PRE_SCM_FIX"}


def fail(problems, message):
    problems.append(message)


def main() -> int:
    parser = None
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, default=DEFAULT)
    args = parser.parse_args()
    problems = []
    try:
        db = json.loads(args.database.read_text())
    except Exception as exc:
        print(f"FAIL fuxi audio test results: {exc}")
        return 1

    if db.get("schema_version") != 1:
        fail(problems, "schema_version must be 1")
    for key, value in (("board", "fuxi"), ("soc", "sm8550")):
        if db.get(key) != value:
            fail(problems, f"{key} must be {value}")
    runs = db.get("runs")
    if not isinstance(runs, list) or not runs:
        fail(problems, "runs must be a non-empty list")
        runs = []
    ids = set()
    for index, run in enumerate(runs):
        where = f"runs[{index}]"
        if not isinstance(run, dict):
            fail(problems, f"{where} is not an object")
            continue
        rid = run.get("id")
        if not isinstance(rid, str) or not re.fullmatch(r"[A-Z0-9][A-Z0-9-]{2,63}", rid):
            fail(problems, f"{where}.id is not a stable upper-case id")
        elif rid in ids:
            fail(problems, f"duplicate run id {rid}")
        else:
            ids.add(rid)
        for field in ("kernel_sha256", "boot_sha256"):
            if not isinstance(run.get(field), str) or not HEX64.fullmatch(run[field]):
                fail(problems, f"{where}.{field} is not a 64-character SHA-256")
        if not isinstance(run.get("cdc_session"), str) or not SESSION.fullmatch(run["cdc_session"]):
            fail(problems, f"{where}.cdc_session is not 16 hex digits")
        for field in ("limit", "stage"):
            if not isinstance(run.get(field), int) or run[field] < 0:
                fail(problems, f"{where}.{field} is not a non-negative integer")
        if not isinstance(run.get("last_error"), int):
            fail(problems, f"{where}.last_error is not an integer")
        if run.get("result") not in RESULTS:
            fail(problems, f"{where}.result is outside the fixed set")
        scm = run.get("scm")
        if not isinstance(scm, dict):
            fail(problems, f"{where}.scm is not an object")
            scm = {}
        if not isinstance(scm.get("strict_success"), bool):
            fail(problems, f"{where}.scm.strict_success is not boolean")
        for field in ("stage_mask", "last_x0", "last_x1", "last_op", "last_pas"):
            if not isinstance(scm.get(field), str) or not HEX.fullmatch(scm[field]):
                fail(problems, f"{where}.scm.{field} is not hex")
        smp = run.get("smp2p")
        if not isinstance(smp, dict):
            fail(problems, f"{where}.smp2p is not an object")
            smp = {}
        if smp.get("item") != 443:
            fail(problems, f"{where}.smp2p.item must be 443")
        for field in ("present", "header_valid", "ready"):
            if not isinstance(smp.get(field), bool):
                fail(problems, f"{where}.smp2p.{field} is not boolean")
        for field in ("check_in", "entry_index"):
            if not isinstance(smp.get(field), int):
                fail(problems, f"{where}.smp2p.{field} is not an integer")
        if not isinstance(smp.get("entry_value"), str) or not HEX.fullmatch(smp["entry_value"]):
            fail(problems, f"{where}.smp2p.entry_value is not hex")
        reg = run.get("regression")
        if not isinstance(reg, dict):
            fail(problems, f"{where}.regression is not an object")
            reg = {}
        for field in ("cdc_ping", "panic", "smmu_fault"):
            if not isinstance(reg.get(field), bool):
                fail(problems, f"{where}.regression.{field} is not boolean")
        if run.get("result") == "PASS_READY":
            if scm.get("strict_success") is not True:
                fail(problems, f"{where}: PASS_READY requires strict SCM success")
            if scm.get("stage_mask") not in ("0x3f", "0x3F"):
                fail(problems, f"{where}: PASS_READY requires all six SCM bits")
            if not (smp.get("present") and smp.get("header_valid") and smp.get("ready") and smp.get("check_in") == 0):
                fail(problems, f"{where}: PASS_READY requires a valid ready item 443")
            if run.get("stage") != 11 or run.get("last_error") != 0:
                fail(problems, f"{where}: PASS_READY requires stage 11 and last_error 0")
        if run.get("result") == "PASS_GLINK":
            if run.get("stage") != 13 or run.get("last_error") != 0:
                fail(problems, f"{where}: PASS_GLINK requires stage 13 and last_error 0")
            if scm.get("stage_mask") not in ("0x3f", "0x3F") or scm.get("strict_success") is not True:
                fail(problems, f"{where}: PASS_GLINK requires all six strict SCM stages")
            if not (smp.get("present") and smp.get("header_valid") and smp.get("ready") and smp.get("check_in") == 0):
                fail(problems, f"{where}: PASS_GLINK requires ready item 443")
            glink = run.get("glink", {})
            if not (glink.get("desc_present") and glink.get("tx_present") and glink.get("rx_present")):
                fail(problems, f"{where}: PASS_GLINK requires items 478/479/480")
            if not (glink.get("version_agreed") and glink.get("channel_up")):
                fail(problems, f"{where}: PASS_GLINK requires version agreed and channel up")
        if run.get("result") == "PASS_GPR":
            if run.get("stage") != 13 or run.get("last_error") != 0:
                fail(problems, f"{where}: PASS_GPR requires stage 13 and last_error 0")
            if not run.get("gpr", {}).get("ready"):
                fail(problems, f"{where}: PASS_GPR requires the SPF ready reply")
            if not (smp.get("present") and smp.get("header_valid") and smp.get("ready")):
                fail(problems, f"{where}: PASS_GPR requires ready item 443")
            glink = run.get("glink", {})
            if not (glink.get("version_agreed") and glink.get("channel_up")):
                fail(problems, f"{where}: PASS_GPR requires GLINK version agreed and channel up")
        if run.get("result") == "PASS_GRAPH":
            graph = run.get("graph", {})
            if run.get("stage") != 13 or run.get("last_error") != 0 or not reg.get("cdc_ping"):
                fail(problems, f"{where}: PASS_GRAPH requires stage 13, no error and CDC PING")
            if not (graph.get("open_status") == 0 and graph.get("close_status") == 0 and graph.get("phase") in (6, 13, 18)):
                fail(problems, f"{where}: PASS_GRAPH requires a matched device open and close result")
            if graph.get("phase") in (13, 18) and not (graph.get("stream_open_status") == 0 and graph.get("stream_close_status") == 0):
                fail(problems, f"{where}: stream graph phase requires matched stream open and close results")
            if graph.get("phase") == 18 and not (graph.get("playback_config_status") == 0 and graph.get("shmem_config_status") == 0):
                fail(problems, f"{where}: stream configuration phase requires both SET_CFG results")
            if graph.get("speaker_audible") is not False:
                fail(problems, f"{where}: PASS_GRAPH must not claim audible speaker output")
        if run.get("result") == "PASS_SPEAKER":
            fail(problems, f"{where}: PASS_SPEAKER is reserved until the full speaker acceptance exists")

    if problems:
        for problem in problems:
            print(f"FAIL {problem}")
        print(f"{len(problems)} problem(s)")
        return 1
    print(f"PASS fuxi audio test results: {len(runs)} runs, latest {runs[0]['id']} ({runs[0]['result']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
