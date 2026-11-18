#!/usr/bin/env python3
"""Read the modem's bring-up state out of the Fuxi CDC diagnostic reply.

The modem's counters ride in the DIAGSTATUS reply's `modem` field, in the same
page/value layout the audio probe uses: the page in bits 16..19 and the value in
bits 20..31. The pages rotate, so one request shows one page; a reading needs
several requests to cover them all.

    python3 scripts/live-fuxi-modem-state.py            # one sweep of all the pages
    python3 scripts/live-fuxi-modem-state.py --watch    # keep sweeping

Read-only: one DIAGSTATUS request per page per sweep.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "scripts/fuxi-usb-debug.sh"

# What each page holds. The names are the ones the probe's comments use.
PAGES = {
    0: "stage",
    1: "last_error",
    2: "scm_x0",
    3: "scm_x1",
    4: "smp2p_ready",
    5: "smp2p_fatal",
    6: "ipcc_irqs",
    7: "loaded_segments",
    8: "mss_rail_found",
    9: "glink_up",
    10: "entries_listed",
    11: "directories_found",
}

# The stage the bring-up reached, in the probe's order.
STAGES = {
    0: "idle",
    1: "firmware-open",
    2: "mapped",
    3: "rails-voted",
    4: "dtb-loaded",
    5: "image-loaded",
    6: "aop-told",
    7: "ready",
    8: "glink-up",
    9: "up",
}


def ask() -> dict | None:
    """One DIAGSTATUS request, as its field dictionary."""
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
        if "modem" in fields:
            return fields
    return None


def sweep() -> dict:
    """One sweep of the rotating pages, as {name: value}."""
    values: dict[str, int] = {}
    for _ in range(24):
        fields = ask()
        if fields is None:
            continue
        try:
            word = int(fields.get("modem", "0"))
        except ValueError:
            continue
        page = (word >> 16) & 0xf
        value = (word >> 20) & 0xfff
        name = PAGES.get(page)
        if name is not None and name not in values:
            values[name] = value
        if len(values) >= len(PAGES):
            break
    return values


def describe(values: dict) -> str:
    if not values:
        return "no modem page yet"
    parts = []
    stage = values.get("stage")
    if stage is not None:
        parts.append(f"stage={stage} ({STAGES.get(stage, 'unknown')})")
    for name in ("last_error", "smp2p_ready", "smp2p_fatal", "glink_up", "mss_rail_found",
                 "loaded_segments", "entries_listed", "directories_found"):
        if name in values:
            parts.append(f"{name}={values[name]}")
    return " ".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--watch", action="store_true", help="keep sweeping")
    parser.add_argument("--interval", type=float, default=3.0)
    args = parser.parse_args()
    while True:
        values = sweep()
        print(json.dumps(values, sort_keys=True))
        print(describe(values), flush=True)
        if not args.watch:
            return 0
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
