# Fuxi audio test database

This directory is the stable, append-only result store for the Xiaomi 13
(`fuxi` / SM8550) first-sound work. Other agents should read
[`results.json`](results.json) and the per-run folder under [`runs/`](runs/).
Do not rewrite an older run; add a new one.

## Files

```text
tests/fuxi-audio/
  results.json                 canonical database (one object per booted image)
  schema.json                  JSON Schema for the database
  live.json                    current CDC diagnostic snapshot, updated by the poller
  live-history.jsonl           timestamped live samples
  apm-static-analysis.json     APM service/status static findings and next-test design
  apm-static-analysis.md       readable version
  README.md                    this file
  runs/<ID>/run.json           the same object as in results.json
  runs/<ID>/summary.md         short human-readable summary
  runs/<ID>/scm.txt            six PAS stages with strict x0/x1
  runs/<ID>/smp2p.txt           item 443 header/entry observation
  runs/<ID>/glink.txt           items 478/479/480, version and channel (GLINK runs)
  runs/<ID>/gpr.txt             bounded GPR/SPF query result (GPR runs)
  runs/<ID>/regression.txt      CDC/Data/launchd/Metal/GOP/panic checks
```

`docs/fuxi-audio-test-results.json` is a symlink to `results.json`; the
human-readable overview is `docs/fuxi-audio-test-results.md`.

## Result names

| Result | Meaning |
| --- | --- |
| `PASS_READY` | all six PAS stages returned strict `x0==0 && x1==0`, and SMP2P item 443 has a valid header with `slave-kernel` ready |
| `PASS_GLINK` | `PASS_READY` plus items 478/479/480, GLINK version agreed and `adsp_apps` channel up |
| `PASS_GPR` | `PASS_GLINK` plus a received `SPF_STATE` ready reply |
| `BLOCKED_GPR` | the bounded GPR/SPF query ran but did not receive the ready reply |
| `PASS_SPEAKER` | only after the full speaker acceptance: both amps separately, 5/5 start/tone/stop, mute restored, no fault, platform regression green |
| `BLOCKED` | a concrete stage stopped; the object records stage/error and SCM x0/x1 |
| `BLOCKED_PRE_SCM_FIX` | historical pre-fix image where `x2` was not `pas`; not a current result |
| `FAIL` / `INCONCLUSIVE` | a run that should not be used as a baseline |

**`PASS_READY` is not speaker PASS.** Do not infer sound from it.

## Reading

```sh
jq '.runs[0]' tests/fuxi-audio/results.json
python3 scripts/show-fuxi-audio-test-results.py
python3 scripts/check-fuxi-audio-test-results.py
```

For a running device, keep the live update job active:

```sh
python3 scripts/live-fuxi-audio-result.py --interval 2
```

It writes `live.json` and appends `live-history.jsonl`; `stage`/`last_error`
are retained from the most recent page-0 sample even when the next DIAGSTATUS
returns a different multiplexed page.

The reader script defaults to this database. The checker validates required
fields, SHA-256 lengths, unique IDs, item 443, and the `PASS_READY` invariant
(strict SCM plus ready).

## Appending a run

1. Copy the new object to a temporary JSON file.
2. Append it with the recorder:

```sh
python3 scripts/record-fuxi-audio-run.py --run-json /tmp/fuxi-run.json
```

3. Create `runs/<ID>/` with `run.json`, `summary.md`, `scm.txt`, `smp2p.txt`
   and `regression.txt`. The recorder can create the basic files with
   `--artifacts`.
4. Run `python3 scripts/check-fuxi-audio-test-results.py`.

The recorder appends, never edits existing objects. A new run must have a new
ID and the full 64-character kernel and boot SHA-256 values.

## Current frontier

`R-SCM-STRICT-01` is `PASS_READY`; `R-GLINK-01` is `PASS_GLINK`;
`R-GPR-03/04/05/06/07/08` are `BLOCKED_GPR`. APM is service 1
(`GPR_APM_MODULE_IID`); status 1 is `ADSP_EFAILED` (general failure), not
ready. The 40-byte q6apm-style APM-header image is `I-APM-HEADER-01` and reset
to Fastboot; it is not reused. Source port 1 (`GPR_APM_MODULE_IID`) was also
tested (`R-GPR-07`) and still returns `ADSP_EFAILED`. The stable 24-byte query
with board source port 0x2001 is restored.

The selected next gate is a **GRAPH_OPEN-only** test using the Fuxi
`Xiaomi-13-tplg.bin` playback subgraph (`[path.graph]`): one
`APM_CMD_GRAPH_OPEN`, no endpoint `SET_CFG`, no map, no prepare/start, no PCM,
amplifiers kept muted, and a single `GRAPH_CLOSE` only after status 0. This is
recorded as `A-GRAPH-01`; it is designed but **not safe to run yet**.
`I-GRAPH-OPEN-01` lost CDC on the first attempt. The graph path is disabled by
`kGraphOpenOnly=false` and `R-GPR-08` verifies CDC recovery. See
[`apm-static-analysis.md`](apm-static-analysis.md).
