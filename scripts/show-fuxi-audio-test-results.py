#!/usr/bin/env python3
"""Read the fuxi audio test-result database without touching the device."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = ROOT / "tests/fuxi-audio/results.json"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, default=DEFAULT)
    parser.add_argument("--id", help="show one run instead of the latest")
    parser.add_argument("--json", action="store_true", help="print the selected run as JSON")
    args = parser.parse_args()

    db = json.loads(args.database.read_text())
    runs = db.get("runs", [])
    if not runs:
        raise SystemExit("no runs in database")
    run = next((r for r in runs if r.get("id") == args.id), None) if args.id else runs[0]
    if run is None:
        raise SystemExit(f"run {args.id!r} not found")
    if args.json:
        print(json.dumps(run, indent=2, sort_keys=True))
        return 0
    scm = run.get("scm", {})
    smp = run.get("smp2p", {})
    print(f"{run.get('id')} result={run.get('result')} when={run.get('when')}")
    print(f"  kernel={run.get('kernel_sha256')}")
    print(f"  boot={run.get('boot_sha256')}")
    print(f"  session={run.get('cdc_session')} stage={run.get('stage')} last_error={run.get('last_error')}")
    print(f"  scm_mask={scm.get('stage_mask')} x0={scm.get('last_x0')} x1={scm.get('last_x1')} "
          f"op={scm.get('last_op')} pas={scm.get('last_pas')}")
    print(f"  smp2p present={smp.get('present')} index={smp.get('entry_index')} "
          f"value={smp.get('entry_value')} ready={smp.get('ready')} irq={smp.get('irq_count','not recorded')} "
          f"kick={smp.get('kick_count','not recorded')}")
    print(f"  next={run.get('next')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
