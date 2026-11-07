# fuxi GPU bring-up log

Every change made while getting the A740 driver to retire a fence on fuxi, and what each experiment
answered. Newest last.

## Method note (important)

A framed submission that the command processor refuses leaves it stopped: every later submission only
queues behind it. **Only the first framed submission after boot is a real test.** Sweeping several values
in one boot therefore proves nothing beyond the first value; each remaining value must be its own boot.
This invalidated the tail of several earlier sweeps and is why the entries below mark which value was
actually tested.

## Defects found and fixed

| # | Defect | Evidence | State |
| --- | --- | --- | --- |
| 1 | `gGpuMaps`/`gGpuCpu`/`gGpuBuffers` were `[5]`; the driver fills `GpuBufferCount` = 11, writing six entries past the end into the neighbouring arrays | The data abort read `0x8d6c46000` out of `gGpuMaps[0]`, which is a buffer's physical address; buffers 5..10 were never allocated | fixed, `package/FuxiGpuPowerProbe.cpp` |
| 2 | publish passed a literal `input_bits` of 32, so `clientWindowBytes` returned 0 and every client's `mount()` answered `kIOReturnNoResources` | `mount_fail 3 rc e00002be` before, `opens 4` after | fixed, uses `NativeAdrenoGpuContextInputBits()` (48) |
| 3 | `openCpAperture()` was missing, so `tableMode()` always chose 1 and the CP never had its SMMU aperture opened | copied from sheng; needs the fourth `scmCall` argument | added |
| 4 | `CP_SMMU_TABLE_UPDATE` carried four dwords; Linux emits two, and the two zero words are read as the next packet header | prologue dump showed `70d30004`; now `70d30002`, and BoMap went 4/2 -> 6/0 | fixed, Adreno740 `3990523` |
| 5 | The Adreno740 submodule was reset to its pinned `a9a2cbb` on every build, silently discarding driver changes | `git reflog` showed a checkout away from the working commit | fixed: parent gitlink + `components.toml` pinned to the bring-up commit |

## Ruled out (each a real boot, first-submission-only where noted)

- SQE firmware version: sheng's 75380-byte blob behaves exactly like fuxi's 74804-byte one. Restored; must stay fuxi's.
- Address-space geometry: `ias 48`, window 4 GiB..256 TiB. Not the limit.
- Process page table (`ArmSmmu4KLive`): all eleven kernel buffers read back mapped at their kernel IOVAs, ring at 0x200000.
- Table switching mode: mode 1 (CPU writes TTBR0), mode 2 (CP switches in ring) and the kernel-table-only mode all stop at the same fence.
- `RBBM_PERFCTR_SRAM_INIT` (0x050e) and its status bit (0x050f bit 0): driven from the CPU the status reads 1 after one 10 us poll. The register and bit are right.
- `CP_WAIT_TIMESTAMP` removed from the prologue: no change (single test).
- Prologue truncated to one packet (`CP_THREAD_CONTROL` only): still no fence retired (single test, first submission).

## Instruments added

- `package/FuxiDisplayProbe.c`: GPU addon handover/install/mount report, GPU power stage, Metal diag (opens,
  name_reads, starter, report, grants), per-method call/failure/status table, fence wait counters, CP stall
  snapshot, table-switch counters, cp_check/cp_image, `vm_check` read-back, and the prologue dump.
- Adreno740: `native_adreno_prologue`/`_info` (the exact prologue words), `native_adreno_vm_check`,
  `native_adreno_prologue_level`, `native_adreno_reset_bits`, `native_adreno_thread_ctrl`,
  `native_adreno_no_bv`, `native_adreno_fence_level` — the last five let the board vary one prologue or
  fence element per boot; each defaults to the shipping value.

## Breakthrough: CP_THREAD_CONTROL stalls the command processor

Bisected one test per boot (only the first framed submission of a boot counts):

| Test | Boot arguments | Result |
| --- | --- | --- |
| minimal prologue + minimal fence | `fuxi-probe-prologue=1 fuxi-probe-fence=4` | fence not retired |
| same, thread control payload 0 | `... fuxi-probe-tctrl=0` | fence not retired |
| ring state at that moment | printed | `current 1 poisoned 0`, so the write pointer is given |
| **a plain submission carrying one CP_THREAD_CONTROL** | probe sends `submit()` with `CP_THREAD_CONTROL(0)` then a no-op | **fence not retired** |

The last row is the answer. The probe's own plain submissions retire fences (fence 6 is its own
CP_MEM_WRITE). Adding a single `CP_THREAD_CONTROL` to that same, otherwise working submission stops the
command processor, with no fault and no progress. So the framed path was never the problem, and neither was
the prologue's length or any payload: **this command processor refuses CP_THREAD_CONTROL**, and both the
framed prologue and the framed fence emit it (and so do client IBs).

Nothing in this port programs the BV ring — `gpuStart` writes `CP_BV_APRIV_CNTL` and
`CP_BV_RB_RPTR_ADDR` but never `CP_BV_RB_BASE`, `CP_BV_RB_CNTL` or its write pointer. A thread control
that names a thread whose ring is not running is the obvious candidate, and it explains why sheng works
with the same driver code: its board programs what fuxi's does not.

## Next step

Program the BV ring in `gpuStart`, mirroring the BR ring that already works (`CP_BV_RB_BASE`,
`CP_BV_RB_CNTL`, and its write pointer through `CP_BV_RB_WPTR`), then re-run the plain+`CP_THREAD_CONTROL`
test — the cheapest decisive check, because it needs no client and no framed path.

## Open question

A framed submission never retires a fence, while the probe's own plain `gRing.submit()` does (fence 6 is
its own CP_MEM_WRITE and it completes). The framed path differs in two ways: the prologue and
`emitFramedFence`. Testing them one at a time, one boot each, is the next step.
