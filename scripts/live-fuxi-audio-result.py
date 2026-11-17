#!/usr/bin/env python3
"""Poll the Fuxi CDC diagnostic reply and update live.json in real time.

Run this as a managed background job while booting/testing:

    python3 scripts/live-fuxi-audio-result.py --interval 3

It is read-only: one DIAGSTATUS request per interval.  The canonical test
database remains append-only; live.json is the current-state view and
live-history.jsonl keeps the timestamped samples.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "scripts/fuxi-usb-debug.sh"
OUT = ROOT / "tests/fuxi-audio/live.json"
HISTORY = ROOT / "tests/fuxi-audio/live-history.jsonl"


def query() -> dict | None:
    result = subprocess.run([str(TOOL), "ctl", "diagstatus"], capture_output=True, text=True, timeout=10)
    if result.returncode != 0:
        return None
    for line in reversed(result.stdout.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            reply = json.loads(line)
        except json.JSONDecodeError:
            continue
        fields = reply.get("fields", {})
        try:
            resources = int(fields.get("resources", "0"))
        except ValueError:
            continue
        page = (resources >> 16) & 0xf
        value = (resources >> 20) & 0xfff
        stage = None
        error = None
        if page == 0:
            stage = value & 0x1f
            error = (value >> 5) & 0x7f
            if error & 0x40:
                error -= 0x80
        return {
            "updated": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
            "session": reply.get("session"),
            "seq": reply.get("seq"),
            "resources": resources,
            "page": page,
            "value": value,
            "stage": stage,
            "last_error": error,
            "raw": fields,
            "source": "fuxi-usb-diagstatus",
        }
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interval", type=float, default=3.0)
    parser.add_argument("--duration", type=float, default=0.0, help="0 means until interrupted")
    args = parser.parse_args()
    start = time.monotonic()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    last_stage = None
    last_error = None
    if OUT.is_file():
        try:
            previous = json.loads(OUT.read_text())
            last_stage = previous.get("stage")
            last_error = previous.get("last_error")
        except (OSError, json.JSONDecodeError):
            pass
    while args.duration <= 0 or time.monotonic() - start < args.duration:
        sample = query()
        if sample:
            if sample.get("page") == 0:
                last_stage = sample.get("stage")
                last_error = sample.get("last_error")
            sample["page_stage"] = sample.get("stage")
            sample["page_last_error"] = sample.get("last_error")
            sample["stage"] = last_stage
            sample["last_error"] = last_error
            sample["observed_monotonic"] = round(time.monotonic() - start, 3)
            temp = OUT.with_suffix(".json.tmp")
            temp.write_text(json.dumps(sample, indent=2) + "\n")
            temp.replace(OUT)
            with HISTORY.open("a") as history:
                history.write(json.dumps(sample, separators=(",", ":")) + "\n")
        time.sleep(max(0.5, args.interval))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
