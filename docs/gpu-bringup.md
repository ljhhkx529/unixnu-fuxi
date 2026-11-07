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

## The BV ring, measured

Register dump next to the working BR ring:

    ring br  base 200000  cntl 20c  rptr_addr 300000  wptr 4e  rptr 4e
    ring bv  a80 0  a81 0  a82 0  a87 2  a98 300008  a99 0  ad0 f

The BR ring is fully programmed and has consumed its writes. The BV ring has only its rptr address
(0xa98) and its APRIV control (0xad0) written; **its base and control read back 0**, and its write pointer
holds a stale 2. So the BV thread has no ring, which is consistent with every CP_THREAD_CONTROL stopping
the command processor.

Two attempts to program it both read back 0 afterwards:

1. from `NativeAdrenoRing::program()`, beside the BR register writes — wiped by the CP init that follows
2. from the board probe, after the CP init, next to the existing `CP_BV_RB_RPTR_ADDR` write — no effect

The second result points at **`CP_PROTECT_CNTL`**: the port programs `A740_CP_PROTECT_CNTL` (0x84f) during
hw_init, so a register outside the protected list is not writable by design. `CP_BV_APRIV_CNTL` (0xad0) does
take, so the BV block is not wholly protected — 0xa80..0xa87 specifically is.

## Protection opened: one register moved, two did not

The CP's protect control word is `(1<<0)|(1<<1)|(1<<3)`, and bit 3 is LAST_SPAN_INF_RANGE, so everything
above the last listed span — which includes 0xa80..0xa87 — is read-only. Turning the control word off,
writing the ring, and turning it back on gives:

    before:  ring bv a80 0 a81 0 a82 0 a87 2 a98 300008 a99 0 ad0 f
    after:   ring bv a80 0 a81 0 a82 0 a87 0 a98 300008 a99 0 ad0 f

**0xa87 accepted the write** (2 -> 0), so protection was indeed the blocker for it. **0xa80, 0xa81 and
0xa82 still read back 0** after being written with the ring base and control, so those offsets are not the
BV ring's BASE/CNTL. The test still fails, so zeroing the stale BV write pointer alone does not help.

Conclusion: the BV ring cannot be programmed from this port until the a740 register map places its BASE and
CNTL. The guess (0xa80/0xa82, mirroring BR's 0x800/0x802) is wrong.

## The BV block, located by reading

A read-only sweep of 0xa60..0xac0 found BV's rptr at **0xa66 holding 0x4e**, the same value as BR's rptr at
0x806 — the BV thread does consume the same ring, so the two blocks line up. Writing through the opened
protection window then gave:

    bv programmed base deafbead cntl 20c rptr_addr 0

**0xa62 takes the control word** (reads back 0x20c, the BR value), so BV's CNTL is there. **0xa60 reads back
the 0xdeafbead sentinel and 0xa64 does not take a write**, so BV's BASE and RPTR_ADDR are not at the mirrored
offsets. The board's own BV rptr address write goes to 0xa98, which is where the header puts
CP_BV_RB_RPTR_ADDR, so the BV block is not a straight +0x260 mirror of BR.

Scanning a740_sqe.fw for these offsets as immediates finds nothing (the firmware addresses them base+offset),
so the register map cannot be recovered from the blob this way.

## Where this leaves the fix

The chain is proved end to end except for one packet:

- every client mounts, allocates, maps and submits; the only failure is that the fence never retires
- a plain submission retires fences; the same submission with one CP_THREAD_CONTROL added does not
- so CP_THREAD_CONTROL is the packet this command processor refuses, and the framed prologue, the framed
  fence and client IBs all carry it
- the BV ring it names is unprogrammed (BASE reads as the sentinel), and two attempts to program it failed
  because its BASE/CNTL/RPTR_ADDR positions are not where BR's mirror them

## Write-probing the BV block

With protection off, each register in the window takes a distinctive value, is read back, and is restored
before the next one is touched:

| range | result |
| --- | --- |
| 0xa40..0xa4f | **all writable**, value reads back unchanged; 0xa48 held 0x20000000 beforehand |
| 0xa50..0xa5f | read the 0xdeafbead sentinel, reject writes |
| 0xa60 | sentinel, rejects writes |
| 0xa62 | writable, masked to 12 bits — behaved as a control word (it holds 0x20c, BR's value) |
| 0xa66 | writable, masked to 20 bits — the BV rptr (held 0x4e, BR's value) |
| 0xa67..0xa70 | read back 0, writes dropped |

So the BV control block is 0xa40..0xa4f plus the rptr at 0xa66, and it is **not** a +0x260 mirror of BR.
0xa48 holding 0x20000000 is the only base-like value found; it is the strongest candidate for a BV ring
base (or for the address the CP uses for the BV side).

## Thread control refuses every payload, and so does the client IB

Two more boots:

| test | result |
| --- | --- |
| plain submission + CP_THREAD_CONTROL with payload 0x80000001 (sync BR, the shipping value) | fence not retired |
| client submissions routed through the **plain** path (`native_adreno_plain_submit=1`), so no framed prologue and no framed fence | `submitted 9 retired 6`: the three client submissions queue and never retire |

The first closes the loophole in the earlier plain+tctrl result, which had used payload 0. The second says
the client's own IB carries the same refused packet — which is expected, since Turnip synchronises the BR
and BV threads itself.

So the port cannot render until this CP accepts CP_THREAD_CONTROL, and everything points at the BV thread
having no ring. `native_adreno_plain_submit` is left at 0; it does not help.

## Programming the whole BV window changes nothing

Every candidate in the writable window takes the ring's address and control word:

    bv programmed a40 200000 a42 20c a48 200000 a4a 20c a62 20c a66 0

All six read back exactly what was written, so the BV block is now programmed as far as its registers can be
reached — and plain + CP_THREAD_CONTROL still does not retire a fence. Programming the BV ring is therefore
**not** what makes thread control work.

That leaves two readings:

- CP_THREAD_CONTROL is refused for a reason other than the BV ring (a CP mode bit the port never sets), or
- the packet is fine and something else about a submission whose first packet is CP_THREAD_CONTROL is wrong.

Both are testable without touching the driver: put the packet **second** in the submission (after a scratch
write) and see whether the fence retires, then compare with it first.

## Next step

One boot each, both read-only on the driver side:

1. **Put CP_THREAD_CONTROL second** in the plain submission, after a CP_SCRATCH type-4 write (the packet every
   working submission already starts with). If the fence retires, the packet is fine and its position is what
   matters; if not, the packet itself is refused.
2. **Compare against `CP_RB_CNTL`** (0x20c on this board) with the a7xx bit that enables the thread group. The
   vendor KGSL module in the fuxi-firmware repository is the source for that bit, and its ringbuffer init path
   is now located at `gen7_ringbuffer_init` (0x8eb50) for whoever picks this up.

If the BV window turns out not to be the answer, the alternative is to stop emitting CP_THREAD_CONTROL at
all, which client IBs would also have to honour.

## Open question

A framed submission never retires a fence, while the probe's own plain `gRing.submit()` does (fence 6 is
its own CP_MEM_WRITE and it completes). The framed path differs in two ways: the prologue and
`emitFramedFence`. Testing them one at a time, one boot each, is the next step.
