# APM gate re-assessment and next muted validation

Static analysis record: [`apm-static-analysis.json`](apm-static-analysis.json).

## 1. What the firmware/Linux source establishes

### Service id

```c
#define GPR_APM_MODULE_IID  1
#define GPR_PRM_MODULE_IID  2
#define GPR_AMDB_MODULE_IID 3
#define GPR_VCPM_MODULE_IID 4
```

APM is destination port **1**. Ports 2 and 3 are not APM; the earlier port
scan is closed.

### `status = 1`

```c
#define ADSP_EOK          0x00000000
#define ADSP_EFAILED      0x00000001  /* General failure */
#define ADSP_EBADPARAM    0x00000002
#define ADSP_EUNSUPPORTED 0x00000003
#define ADSP_ENOTREADY    0x0000000A
```

`status = 1` is **`ADSP_EFAILED`**, a general failure. It must not be
reclassified as ready.

### Linux sequence

`q6apm_probe()` calls `q6apm_get_apm_state()` (GET_SPF_STATE) but does not use
its return value as a probe gate. The first APM operation whose result Linux
actually checks is `q6apm_graph_open()`:

```c
int ret = q6apm_send_cmd_sync(apm, open_pkt, 0);
if (ret)
    dev_err(apm->dev, "GRAPH_OPEN graph %u failed: %d ...");
```

`audioreach_send_cmd_sync()` treats `result.status > 0` as an error, so a
graph-open `BASIC_RESULT status = 0` is the first meaningful APM success in the
Linux path. That does **not** authorize opening a graph in this bring-up; it
only shows that GET_SPF_STATE is not the gate Linux relies on.

## 2. Complete difference: stable vs incident image

| Item | Stable `R-GPR-06` | Incident `I-APM-HEADER-01` |
| --- | --- | --- |
| GET_SPF_STATE request | 24-byte GPR header only (`gpr_spf_next`) | 40 bytes: 24-byte GPR + 16-byte zero APM command header (`gprSpfQuery`) |
| Packet buffer | 24/64-byte storage, 24 used | 64-byte storage, 40 used |
| Timing | 50 queries / 20 s | 50 queries / 20 s |
| GLINK intents | 20 × 512 | 20 × 512 |
| Raw logging | first request/reply only | first request/reply only |
| PAS/RSC/SMP2P/GLINK code | unchanged | unchanged |
| Amplifier state | reset/muted | reset/muted |
| Observed | stable `ADSP_EFAILED`, no reset | USB disconnect, Fastboot, no DIAGSTATUS |

The only functional difference is the **16-byte zero APM command header and
the 40-byte packet size**. The 40-byte form is therefore not a safe next test
without isolating that difference.


## 3. Which Fuxi graph config to use

The only concrete Fuxi **AudioReach/APM** graph in the tree is the playback
subgraph in `facts/audio/binding.toml` (`[path.graph]`), derived from
`Xiaomi-13-tplg.bin`:

```text
apm_iid=1, subgraph_id=1, performance=0, direction=0, scenario=1
container_id=1, position=0, stack=0x4000, domain=0
modules: shmem 0x6003 (0x07001000), stream 0x6001 (0x07001000),
         endpoint 0x6002 (0x0700100A)
links:   0x6003 -> 0x6001 -> 0x6002
```

This graph contains the **Secondary MI2S speaker endpoint**, so it is used
**only for APM_CMD_GRAPH_OPEN**:

- no endpoint `SET_CFG`;
- no shared-memory map;
- no `GRAPH_PREPARE`, `GRAPH_START`, or PCM;
- one `GRAPH_CLOSE` only after a status-0 open;
- amplifiers stay in reset/global mute, no I2C.

Rejected configs:

| Config | Why rejected |
| --- | --- |
| `graph-224.json` (Voice Call TX, handset mic) | q6voice graph, not APM AudioReach; cannot test the APM opcode |
| `graph-272.json` (Voice Call RX, speaker) | q6voice and contains the speaker path |

If a non-speaker APM graph is required instead, the VA/WCD capture topology ids
must first be extracted from `Xiaomi-13-tplg.bin`; they are not in the binding
yet.

### Graph-open-only gate

```text
one APM_CMD_GRAPH_OPEN, one-second deadline, no retry
if status == 0: one APM_CMD_GRAPH_CLOSE
if status != 0: record exact status; no close, no follow-up
no PCM, no amp I2C, no endpoint configuration
```

Acceptance: `GRAPH_OPEN` `BASIC_RESULT status=0`, `GRAPH_CLOSE` bounded,
CDC/SCM/SMP2P alive, no amp activity or PCM.


## 4. Next validation: single-query muted SPF probe

### Safety envelope

- one 24-byte `APM_GET_SPF_STATE` request only;
- one-second answer deadline, no retry loop;
- `fastboot boot` only, no boot-slot write;
- amplifiers remain in reset/global mute; no I2C, no PCM, no graph;
- rollback image: `R-GPR-06` kernel `07720336...` / boot `132948e0...`, or
  `R-GLINK-01` kernel `df199244...` / boot `4d2f1b43...`.

### Procedure

1. Build the stable `R-GPR-06` code with a compile-time `max_queries = 1`.
2. Start the live CDC updater:
   `python3 scripts/live-fuxi-audio-result.py --interval 2`.
3. `fastboot boot` the image; wait for CDC.
4. Read the single reply and append a run with `max_queries=1`.
5. If CDC remains alive and the reply is still `ADSP_EFAILED`, stop. Do not
   open a speaker graph.
6. If the image resets, record an incident and revert to `R-GPR-06`; do not
   retry the 40-byte form.

### Decision after the probe

| Observation | Action |
| --- | --- |
| `ADSP_EFAILED`, CDC alive | GET_SPF_STATE is not a usable hard gate; stop and request approval for a graph-open status-0 gate |
| `APM_SPF_STATE status=1` | true ready; proceed only to the next approved bounded graph step |
| reset/panic/SMMU | stop; revert; do not reuse the image |

No speaker graph, amplifier unmute, or PCM is allowed in this stage.
