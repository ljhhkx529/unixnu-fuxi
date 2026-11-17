#!/usr/bin/env python3
"""Append one validated run to the fuxi audio test-result database.

The database is append-only: an existing id is refused.  The candidate file is
validated by scripts/check-fuxi-audio-test-results.py before it replaces the
canonical database, and the per-run artifact folder is generated on success.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DB = ROOT / "tests/fuxi-audio/results.json"
CHECK = ROOT / "scripts/check-fuxi-audio-test-results.py"


def write_artifacts(db: dict, run: dict) -> Path:
    out = ROOT / "tests/fuxi-audio/runs" / run["id"]
    out.mkdir(parents=True, exist_ok=True)
    (out / "run.json").write_text(json.dumps(run, indent=2) + "\n")
    scm = run.get("scm", {})
    smp = run.get("smp2p", {})
    reg = run.get("regression", {})
    lines = [f"run: {run['id']}", f"result: {run['result']}", f"stage: {run['stage']}",
             f"last_error: {run['last_error']}", f"strict_success: {scm.get('strict_success')}",
             f"stage_mask: {scm.get('stage_mask')}", ""]
    stages = scm.get("stages")
    if stages:
        lines.append(f"{'stage':24s} {'x0':10s} {'x1':10s}")
        for stage in stages:
            lines.append(f"{stage.get('name',''):24s} {stage.get('x0',''):10s} {stage.get('x1',''):10s}")
    elif scm.get("bug"):
        lines += ["no stage table: pre-fix run", f"bug: {scm['bug']}"]
    lines += ["", f"last_x0: {scm.get('last_x0')}", f"last_x1: {scm.get('last_x1')}",
              f"last_op: {scm.get('last_op')}", f"last_pas: {scm.get('last_pas')}"]
    (out / "scm.txt").write_text("\n".join(lines) + "\n")
    (out / "smp2p.txt").write_text("\n".join([
        f"run: {run['id']}", f"item: {smp.get('item')}", f"present: {smp.get('present')}",
        f"header_valid: {smp.get('header_valid')}", f"check_in: {smp.get('check_in')}",
        f"entry_index: {smp.get('entry_index')}", f"entry_value: {smp.get('entry_value')}",
        f"ready: {smp.get('ready')}", f"first_seen_ms: {smp.get('first_seen_ms','not recorded')}",
        f"irq_count: {smp.get('irq_count','not recorded')}", f"kick_count: {smp.get('kick_count','not recorded')}"]) + "\n")
    if isinstance(run.get("gpr"), dict):
        gpr = run["gpr"]
        (out / "gpr.txt").write_text("\n".join([
            f"run: {run['id']}", f"port: {gpr.get('port')}", f"destination_port: {gpr.get('destination_port')}", f"queries: {gpr.get('queries')}",
            f"answers: {gpr.get('answers')}", f"request_opcode: {gpr.get('request_opcode')}",
            f"request_token: {gpr.get('request_token')}", f"response_opcode: {gpr.get('response_opcode')}",
            f"response_token: {gpr.get('response_token')}", f"result_opcode: {gpr.get('result_opcode')}",
            f"token_match: {gpr.get('token_match')}", f"request_matches_result: {gpr.get('request_matches_result')}",
            f"last_opcode: {gpr.get('last_opcode')}", f"last_status: {gpr.get('last_status')}",
            f"payload0: {gpr.get('payload0')}", f"payload1: {gpr.get('payload1')}",
            f"raw_request_hex: {gpr.get('raw_request_hex')}",
            f"raw_reply_hex: {gpr.get('raw_reply_hex')}",
            f"raw_source: {gpr.get('raw_source')}",
            f"sheng_expected_ready_reply: {gpr.get('sheng_expected_ready_reply')}",
            f"spf_state: {gpr.get('spf_state')}", f"ready: {gpr.get('ready')}",
            f"note: {gpr.get('note')}"]) + "\n")
    if isinstance(run.get("glink"), dict):
        glink = run["glink"]
        (out / "glink.txt").write_text("\n".join([
            f"run: {run['id']}", f"desc_item: {glink.get('desc_item')}",
            f"tx_item: {glink.get('tx_item')}", f"rx_item: {glink.get('rx_item')}",
            f"desc_present: {glink.get('desc_present')}", f"tx_present: {glink.get('tx_present')}",
            f"rx_present: {glink.get('rx_present')}", f"desc_size: {glink.get('desc_size')}",
            f"tx_size: {glink.get('tx_size')}", f"rx_size: {glink.get('rx_size')}",
            f"version_agreed: {glink.get('version_agreed')}", f"channel_up: {glink.get('channel_up')}",
            f"broken: {glink.get('broken')}", f"rx_messages: {glink.get('rx_messages')}",
            f"tx_messages: {glink.get('tx_messages')}", f"unknown_opens: {glink.get('unknown_opens')}",
            f"ladder_stage: {glink.get('ladder_stage')}",
            f"size_evidence: {glink.get('size_evidence')}"]) + "\n")
    (out / "regression.txt").write_text("\n".join([
        f"run: {run['id']}", f"cdc_ping: {reg.get('cdc_ping')}", f"data_mount: {reg.get('data_mount')}",
        f"launchd: {reg.get('launchd')}", f"metal: {json.dumps(reg.get('metal'))}",
        f"gop: {json.dumps(reg.get('gop'))}", f"panic: {reg.get('panic')}",
        f"smmu_fault: {reg.get('smmu_fault')}"]) + "\n")
    (out / "summary.md").write_text(
        f"# {run['id']}\n\n- Result: `{run['result']}`\n- When: {run['when']}\n"
        f"- Kernel: `{run['kernel_sha256']}`\n- Boot: `{run['boot_sha256']}`\n"
        f"- CDC session: `{run['cdc_session']}`\n- Stage / last_error: `{run['stage']} / {run['last_error']}`\n"
        f"- SCM mask: `{scm.get('stage_mask')}`, last `{scm.get('last_x0')}/{scm.get('last_x1')}`\n"
        f"- SMP2P 443: present `{smp.get('present')}`, index `{smp.get('entry_index')}`, "
        f"value `{smp.get('entry_value')}`, ready `{smp.get('ready')}`\n"
        f"- Next: {run.get('next')}\n\n{run.get('note','')}\n")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-json", type=Path, required=True, help="one run object to append")
    parser.add_argument("--database", type=Path, default=DEFAULT_DB)
    parser.add_argument("--no-artifacts", action="store_true")
    args = parser.parse_args()

    db = json.loads(args.database.read_text())
    run = json.loads(args.run_json.read_text())
    if not isinstance(run, dict) or not isinstance(run.get("id"), str):
        raise SystemExit("run-json must be one object with an id")
    if not re.fullmatch(r"[A-Z0-9][A-Z0-9-]{2,63}", run["id"]):
        raise SystemExit("id must be upper-case letters, digits and hyphens")
    if any(existing.get("id") == run["id"] for existing in db.get("runs", [])):
        raise SystemExit(f"run id {run['id']} already exists; append a new id")
    run.setdefault("when", dt.datetime.now().astimezone().isoformat(timespec="seconds"))
    db.setdefault("runs", []).insert(0, run)
    db["updated"] = dt.datetime.now().astimezone().isoformat(timespec="seconds")

    candidate = args.database.with_suffix(".json.tmp")
    candidate.write_text(json.dumps(db, indent=2) + "\n")
    check = subprocess.run([sys.executable, str(CHECK), "--database", str(candidate)],
                           capture_output=True, text=True)
    sys.stdout.write(check.stdout)
    sys.stderr.write(check.stderr)
    if check.returncode != 0:
        candidate.unlink(missing_ok=True)
        raise SystemExit("candidate rejected; database unchanged")
    candidate.replace(args.database)
    if not args.no_artifacts:
        out = write_artifacts(db, run)
        print(f"artifacts: {out}")
    print(f"appended {run['id']} to {args.database}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
