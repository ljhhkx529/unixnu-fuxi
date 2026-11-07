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

## Order does not matter either

    plain+tctrl order 0 seq 7 wait 1 seen 6     (thread control first, as a client IB has it)
    plain+tctrl order 1 seq 8 wait 1 seen 6     (thread control second, after a scratch write)

So CP_THREAD_CONTROL is refused in any position, with any payload, and whether or not the BV window is
programmed. It is the packet itself.

## The way to render without it

Everything else on this board works: the GMU starts, the CP retires the fences of submissions that do not
carry this packet, the clients mount, allocate, map and submit. The one missing piece is that Turnip's IBs
carry CP_THREAD_CONTROL because a context needs to say which threads run.

The driver already walks every client IB when it wraps it in INDIRECT_BUFFER packets, so it can drop that
packet on the way past — a board-level filter, off by default, that rewrites the submission so this CP
accepts it. That is the shortest route to a rendering GPU and it needs no register archaeology.

## The client's IB reads as zeros

Dumping the driver's own record of the client's newest IB (`native_adreno_ib_log`, the first 28 dwords as the
kernel maps them):

    client_ib fence 13 iova 100000000 bytes 48
      ib+00 00000000 00000000 ... (all 28 zero)

Two readings: the client submitted an all-zero IB (unlikely — Turnip writes real commands), or the kernel
cannot read that IOVA (peekWords answers zeros silently). The second is likelier: the buffer lives in the
client's own address space at 0x100000000, which the kernel VM does not map, so this dump says nothing about
the content. It does say the kernel cannot peer into a client IB, which is itself worth remembering.

So the client-IB theory is **not** established, and the controlled result stands on its own: a submission
this port builds itself, carrying one CP_THREAD_CONTROL and nothing else, does not retire a fence.

## Next step

1. **Make the kernel able to read a client IB** — map the client's BO into the kernel VM (or use the BO's CPU
   mapping) — so the dump shows real words. Without that, nothing can be said about what Turnip sends.
2. **Re-check the controlled result against `CP_RB_CNTL`**: 0x20c on this board. The a7xx bit that enables the
   thread group is the one fact still missing, and the vendor KGSL module in the fuxi-firmware repository is
   where to read it (`gen7_ringbuffer_init` at 0x8eb50 is its ring setup).

If the BV window turns out not to be the answer, the alternative is to stop emitting CP_THREAD_CONTROL at
all, which client IBs would also have to honour.

## Open question

A framed submission never retires a fence, while the probe's own plain `gRing.submit()` does (fence 6 is
its own CP_MEM_WRITE and it completes). The framed path differs in two ways: the prologue and
`emitFramedFence`. Testing them one at a time, one boot each, is the next step.


## Breakthrough: SYNC_THREADS is what stops the command processor

The CP init runs a CP_THREAD_CONTROL of its own and completes, so the packet is not refused in general. Its
payload is `CpConcurrentBinDisable = 1u << 27` — bit 27 and nothing else. Every payload this port built for
the prologue and the framed fence set `RingSyncThreads = 1u << 31` as well, and that bit tells the command
processor to wait for the named thread to reach a synchronisation point. On this GPU those threads never do,
so the ring stops with no fault, no progress and no error.

One boot settles it:

    plain + CP_THREAD_CONTROL(0x80000001, sync BR)  -> wait 1 seen 6   (stalled)
    plain + CP_THREAD_CONTROL(0x08000000, bit 27)   -> wait 0 seen 7   (retired)

**Fix**: `native_adreno_no_thread_sync` (default 0) drops bit 31 from the prologue's CP_THREAD_CONTROL
payloads. With it set and client submissions routed through the plain path, the ring retires every fence it
is given: `submitted 8 retired 8`.

### What still fails

With the sync bit fixed, `sheng-gpu-single-table=1` and `native_adreno_plain_submit=1` together get further
than any earlier configuration:

    fence      submitted 8 retired 8      (the port's own submissions all retire)
    clients    opens 4 busy 0             (all four mount)
    Submit     1 call, 0 failures         (the render server's submission is accepted)
    WaitFence  1 call, 1 failure          (its fence still does not retire)

So the machinery is sound and the remaining gap is the **client's own IB content**: the port's submissions
retire whatever they carry, the render server's does not. Next round starts there.

### Round summary

| change | state |
| --- | --- |
| SYNC_THREADS dropped from CP_THREAD_CONTROL (`native_adreno_no_thread_sync`) | kept, enabled by the probe |
| `native_adreno_plain_submit` | enabled by the probe; the framed fence's BV timestamp wait needs it |
| `sheng-gpu-single-table=1` | enabled; clients run on the kernel table since the CP cannot switch |
| BV register probing (`0xa40`..`0xa4f`, `0xa62`, `0xa66`) | programmed, not the blocker |


## The client's submission is the only one left

    late_fence live 1 submitted 9 retired 8      (at 45 s and again at 75 s)

The render server's submission is fence 9. It is accepted, it is queued, and it never retires — the command
processor stops on it and stays stopped. Everything this port builds itself retires (8 of 8).

The plugin's own source already records this shape. `drivers/AdrenoMetal/NativeAdrenoUserTest.h`, in the
replay harness:

> The first packet is CP_THREAD_CONTROL(BOTH) and int-060 stopped on it alone. Before the prefixes, the same
> two dwords run with BR only (1) and BV only (2), so a hang names the thread.

So the packet that stops this CP is the same one everywhere, and Turnip's IBs carry it with SYNC_THREADS set.
Our own submissions stopped carrying it and started retiring.

The same comment records a second lead worth keeping: "Turnip's own fill of this IB stopped in int-062 while
this replay retired", with two controls (device-mapped stores reusing an address after a free, and a
write-combine memcpy). That is about how Turnip *fills* its BOs, not about the packets.

### What round 2 should do

Two ways to stop the client's IB carrying SYNC_THREADS, in order of cost:

1. **Patch the IB in the driver's submit path** — copy the client's IB, walk its packets, clear bit 31 in every
   CP_THREAD_CONTROL, submit the copy. The driver already has scratch space and the BOs' CPU mappings.
2. **Patch Turnip's IB generation**, if the plugin builds it from source here.


### The client IB read is the next thing to fix, not the IB

The submission is 48 bytes and the plugin's own test writes exactly twelve dwords into that BO
(`NativeAdrenoUserTest.h`: `for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];`), so the buffer is populated.
The driver's `peekWords` reads zeros for it at 0x100000000 even in single-table mode, so **the read path is
what is broken**, and nothing can be concluded about the IB's content until it is fixed.

That matters because the two remaining explanations — "Turnip's IB carries SYNC_THREADS" and "the CP cannot
read the IB at all" — are still both open, and only a working read tells them apart.


## Round 2: the write-combining mapping was hiding the client's IB

`peek` of the client's IB reported kIOReturnSuccess and the words were all zero, so the mapping existed and
the buffer really read as zero. The submission was 48 bytes — exactly the twelve dwords the plugin writes —
so the data was written and never became visible. Turning write-combining off (`sheng-gpu-wc=0`) changes
everything:

    before:  client_ib iova 100000000 bytes 48   ib+00 00000000 ...
    after:   client_ib iova 100027000 bytes 932  ib+00 70970001 00000003 70460001 00000019 ...

A 932-byte Turnip IB with real packets, whose first is CP_THREAD_CONTROL with payload 3 (BOTH, **no SYNC
bit**) — so Turnip's own IB was never the problem. And the plugin's own test goes from step 11 to

    report backboard s34 e0

step 34, no error, where every earlier run stopped at 11.

So this board must serve client BO CPU maps write-back; write-combined views do not reach the memory the
command processor reads here.

## What is left for the objective

    gop offers 0 done 0        the presenter still has no client, so no frame is offered to the GPU
    sheng-fb: copy  31         the display still copies on the CPU

The submission path works now. What is missing is a process opening the GPU presenter and the display handing
it frames, plus the last fence (`submitted 11 retired 10`) and SpringBoard's BoMap refusals
(`m3 4/2 kIOReturnBusy`).


### The presenter exists and nobody opens it

    fuxi-display: handoff presenter 1        the GPU hand-off presenter is created, attached and started
    adrenoprobed                             never appears in the log
    gop offers 0 done 0

The addon installs `org.blu-sharky.adrenoprobed.plist` over /System/Library/LaunchDaemons through the bindfs
union (the mount reports stage 5, so both the sealed entry and the owner's are visible by lookup), but launchd
never starts the job. The union merges lookups; whether it merges the directory listing launchd scans is the
open question.

Until a process opens the presenter, the display keeps its CPU copy — that is by design
(`NativeGOPPresenter::offer` needs `gpuClient`).

### Round 2 summary

| change | state |
| --- | --- |
| `sheng-gpu-wc=0` | required on this board: write-combined client BO maps do not reach the memory the CP reads |
| `sheng-gpu-single-table=1` | required: the CP cannot switch page tables from the ring |
| `native_adreno_plain_submit` | enabled by the probe |
| `native_adreno_no_thread_sync` | kept: SYNC_THREADS hangs this CP |
| client IB | 932-byte Turnip IB with real packets, first is CP_THREAD_CONTROL(3) |
| plugin test | reaches step 34 with error 0 |
| display | still the CPU copy; the presenter has no client |


## Round 3: the client's submission retires, the display is a different door

    report backboard s34 e0        the plugin's own ABI run reaches step 34 with no error, twice
    gop_wait calls 0               the GPU presenter was never asked for a frame
    gop offers 0 done 0
    sheng-fb: copy  …              the display still copies on the CPU

The plugin's run walks Query, GetDeviceInfo, two BoAllocs, two BoMaps, two BoCpuMaps, two maps, Submit and
WaitFence before anything else (steps 1..12) and then continues to 34 with error 0, so **the Metal client's
submission retires a fence**: that half of the objective is met.

The display's GPU path is gated differently, and the reason is now precise.
`NativeGOPPresenter::offer` accepts a frame only when `gpuClient` is set, and `gpuClient` is set by the
**WAIT_SWAP** method handler — the first time a client asks the presenter for a frame. The plugin reaches the
presenter through the accelerator's tunnel (methods 0x7000 + NGP_METHOD_*, `GpuScanoutClient.h`), and it
never calls WAIT_SWAP, because it never presents a real frame: `gop_wait calls 0`.

So nothing on the display side is broken. The render server loads the plugin and the plugin's device works,
but no composited frame is taken through it, so no swap is ever offered, so the display keeps its copy.

### What round 4 should establish

Whether backboardd ever creates a Metal command buffer through the plugin at all (the plugin's own counters
and its `NativeBridgeMetalFrontend` swap path), and if not, what it needs to. That is plugin and render-side
work, not kernel.


## Round 4: the plugin never reaches the presenter, not even once

Counters added to the presenter's own tunnel entries (`native_gop_handoff_call`, `native_gop_handoff_memory`)
settle it:

    gop_handoff calls 0 op 0 memory 0 status 0
    gop_wait    calls 0 timeouts 0

So the plugin does not try and fail — **it never asks**. Nothing on the display side is refusing anything: the
presenter is created, attached and started, and no client ever comes near it.

The plugin's accelerator path works (its ABI run reaches step 34 with no error), so the gap is inside the
plugin's presentation path: `NativeBridgeMetalFrontend` never reaches its swap, which is what would call
WAIT_SWAP and set `gpuClient`, which is what would let `offer` accept a frame.

That is render-side work in `drivers/AdrenoMetal`, not kernel work.

### Round 4 summary

| item | state |
| --- | --- |
| presenter tunnel entries counted | added (`native_gop_handoff_stat`) |
| plugin → presenter calls | **0**, so the presentation path is never taken |
| plugin ABI path | reaches step 34, error 0 |
| display path | CPU copy, by design until a client asks |
| probe report function | rewritten to one line per counter (the old block-per-diagnostic version stopped compiling under -Wshadow/-Wconversion) |


## Round 5: the scanout thread exists, and its first step is the suspect

The production plugin does start the GPU scanout: `NativeAdrenoMetalPlugin.mm` spawns a thread that sleeps
three seconds and calls `bridgephase::serveScanout(gpu, device)`. The transport it would use is already
written (`NativeMetalBridgePhase.h`: `link.method(0x7000 + method, …)` and `map(g.conn, 0x5343, …)`), and the
client (`gpuscanout::Client`) is a drop-in `open` + `serveOne` loop.

`serveScanout` marks its progress with `gpu.mark(0x6700)`, `0x6701`, `0x6702`, then `0x6710`..`0x6715` as the
client opens, `0x6720` on the first wait and `0x6730`/`0x6731` per frame. The driver keeps the last sixteen
markers, and **none of them is in 0x67xx** — the newest are `3001 3002 3003 3004 42d5 50ff` repeating. So the
thread runs and stops before its first marker, or its markers never arrive.

Its very first step after marking is to open the accelerator a second time:

    const int openResult = link.openFn ? link.openFn(link.port, mach_task_self(), 0x4148, &link.conn) : -1;

Type **0x4148**, while the render server's own connection uses 0x4147 and the driver grants
`NativeAdrenoPluginOpenType`. The driver's open path refuses any other type as foreign. That is the first thing
to test next round: whether the scanout connection is being opened with a type the driver drops.


## Round 6: the publish marker was missing, and it changes everything

Answering the question directly: **no component and no plugin file was missing** — `Adreno740`, `AdrenoMetal` and
`GopFramebuffer` are all there, the addon carried the whole 34 MiB bundle including
`libvulkan_freedreno.dylib`, and against sheng only `ShengPreflight`/`ShengUfs` differ (fuxi has its own
NativeUfs).

What was missing is a **marker file**. `scripts/add-gpu-plugin-data.py` and `scripts/make-gpu-addon.py` both
document it:

> With `--publish` the seed also carries /private/var/ngpu/addons/NativeAdrenoMetal.publish: the plugin then
> returns the native Metal device to the original discovery in backboardd, so the render server composites
> through it. **Without the file the plugin only runs its tests.**

Our addon was built without `--publish` (8 files). Regenerated with it (9 files), the plugin's behaviour changes
completely:

| before | after |
| --- | --- |
| `report backboard s34 e0` (its self-test) | no report at all — the test path is no longer taken |
| `marklog` empty of 0x68xx | `marklog 3: 6800 6801 6a3b` |
| one client open | `opens 3`, `m8 GetStatus 100`, `m6 WaitFence 553` |

`6800` entered and `6801` Turnip found are the plugin's own documented markers; `6a3b` is its diagnostic word
for this process (bits: deep-framebuffer preference false, MobileGestalt answer true, compression capability
dictionary present, not root). The next marker it would write is `0x6E01`, and it never comes — so device
creation stops between that diagnostic block and the arm64e framebuffer patch that follows it.

### Where round 7 starts

That gap in `NativeAdrenoMetalPlugin.mm` `publish::deviceForPort`: the environment it sets
(`CA_DISABLE_FRAMEBUFFER_COMPRESSION`, and on arm64e the three EDR/headroom variables and three `writeTask`
patches with markers 0x6E01..0x6E03), then the shader-cache directory scan that marks 0x6810+i or 0x68f2 and
ends at 0x6802.


## Round 7: the stuck submission, word for word

Fence 9 is a 1000-byte Turnip IB at 0x100028000, readable (`peek 0`), and its first 128 dwords are an ordinary
command stream:

    70970001 00000003   CP_THREAD_CONTROL, payload 3 (BOTH, no SYNC)
    70460001 00000019   EVENT_WRITE7 0x19 CCU invalidate color
    70460001 00000018   EVENT_WRITE7 0x18 CCU invalidate depth
    70460001 00000028   EVENT_WRITE7 0x28
    70460001 00000033   EVENT_WRITE7 0x33
    70268000            CP_WAIT_FOR_IDLE
    … type-4 register writes to the ~0x8000 range and indirect setup …

and `fence submitted 9 retired 8`: the command processor retires eight submissions and stops on this one.

### The framed path is worse, not better

With SYNC_THREADS gone the prologue is syntactically usable again, so the framed path was worth re-testing —
the original device repository used it. It is not: with `plain_submit=0` the boot log stops at 833 lines where
it otherwise reaches ~900, and the display reports never run at all. The system stops instead of merely the
command processor. So the plain path stays.

### One more thing the log shows

    IOUC AGXDeviceUserClient failed MACF in process pid 29, mediaserverd

mediaserverd is refused the accelerator by the sandbox. That is a separate client from the render server and
does not explain fence 9, but it is the same shape of problem as the plugin's own grants and worth listing.


## Round 7 continued: sheng-gpu-zap was left at 0

The original device repository's own probe defaults `sheng-gpu-zap` to **4**, and its comment says why it
matters:

> sheng-gpu-zap: 0 off, 1 through init_image, 2 through mem_setup and the segment copy, 3 through
> auth_and_reset, 4 (default) also CP_SET_SECURE_MODE 0.

and

> sheng-gpu-zap=0 skips the zap/secure-mode step … **The CP then stays in secure mode**.

This port had it at 0, so the command processor was never taken out of secure mode. The probe does implement
the step (`zapAndSecure()`, limit 4 reaches "TZ authenticated the zap shader and the CP left secure mode"), it
was simply disabled. Restoring 4 moves the counters:

    zap=0:  fence submitted 9  retired 8
    zap=4:  fence submitted 10 retired 9

one more submission through, so secure mode is part of the story — but one fence still never retires and
device creation still stops at marker 0x6a3b. **zap=4 stays**: it matches the repository that worked.

### Where the next round starts

Two independent gaps are now measured, not guessed:

1. device creation stops between marker 0x6a3b and the next one it would write (0x6E01 or 0x68f2), a range
   that contains only `setenv`/`CFPreferences` calls and the shader-cache directory scan around
   `native_bridge_metal_open`;
2. one client submission (fence 9/10, a 1000-byte IB with an ordinary Turnip command stream) is queued and
   never retired.


### The plugin is not crashing, it is stopping

With the ordered marker log (`native_adreno_mark_log`, the diagnostic slots are indexed by value and lose the
order) a publish-mode boot records exactly three markers in the render server:

    6800  entered   6801 Turnip found   6a3b the diagnostic word

No panic, no crash report, no further marker — and the next one it would write is either `0x6E01` (the arm64e
framebuffer patch group) or `0x6810+i`/`0x68f2` (the shader-cache scan). Between `0x6a3b` and those there are
only `setenv`/`CFPreferencesSetAppValue` calls, then per cache directory `mkdir`, `access`,
`removeHalfImported()` and `native_bridge_metal_open()`. One of those two last calls is where it stops, and
neither writes a marker before it does its work.

That is the single most valuable marker to add next: one immediately before `removeHalfImported` and one
immediately before `native_bridge_metal_open`, so the next boot names the call instead of the range.


## Round 8: the client that reaches the accelerator is refused by the sandbox

Two facts from this boot, both new:

    fuxi-metal: opens 3 busy 0 bad 0 name_reads 2 reader mediaserverd starter (none)
    IOUC AGXDeviceUserClient failed MACF in process pid 29, mediaserverd

The only process naming our accelerator is **mediaserverd**, and the sandbox refuses it (`AGXDeviceUserClient
failed MACF`). The render server's own plugin still stops after marker `0x6a3b`.

So there are two separate client problems, not one:

| client | state |
| --- | --- |
| backboardd (the plugin) | enters device creation, stops at `0x6a3b` |
| mediaserverd | reaches the accelerator and is refused by MACF |

`MACF` refusals are a grant problem: the owner-trust bundle the payload passes with
`--owner-trust-cache` is what admits a process, and mediaserverd is evidently not in it.

### The plugin rebuild is blocked by the toolchain, not by the code

Markers were added around `removeHalfImported()` and `native_bridge_metal_open()` in
`NativeAdrenoMetalPlugin.mm` (0x6B10..0x6B13), but `build-adreno-metal-plugin.py` cannot run here:

    error: no such command: '+1.98.0'

that is the bridge's Rust build asking for a toolchain this machine does not have. `--no-replay` gets past the
host-Turnip step, not past that. The plugin sources are therefore edited and staged but **not built**, and the
image in use still carries the previous bundle.


## Round 9: the BV thread is the one that stalls

The plugin now rebuilds (a local `cargo`/`rustc` shim points at the installed 1.98.0 toolchain; the Homebrew
cargo on PATH does not understand `+1.98.0`), and the markers added last round name the call:

    marklog 6: 6800 6801 6a3b 6b10 6b11 6b12

`6b10`/`6b11` bracket `removeHalfImported()` and both appear, so that call is fine. `6b12` is written just
before `native_bridge_metal_open()` and **`6b13` never is**: the runtime's device open is where the render
server stops, and `client calls 383 last_sel 6 last_st e00002d6` shows it sitting in WaitFence, timing out.

The whole 1000-byte IB says why. Its **first** packet is

    70970001 00000003        CP_THREAD_CONTROL, payload 3 = BOTH threads

and it switches threads again further in (`70970001 08000001` = BR with binning disabled, `70970001 00000002` =
BV, `70970001 00000003` = both). This board's BV thread has no runnable ring, so the command processor stops on
the very first packet.

The probe, asked the same question directly:

| payload | meaning | result |
| --- | --- | --- |
| `0x08000000` | no thread selected | retires |
| `0x08000001` | BR only | **retires** |
| `0x00000003` | both threads | stalls |

So BR is healthy and **BV is not**. Everything else about the ring, the fence and the page tables works.

### The way out

Two routes, and the first is far cheaper:

1. **Stop Turnip from using the BV thread** — a concurrent-binning-off build or debug option, so the command
   streams it emits never select BV. The CP init already proves the hardware is happy with bit 27
   (binning disabled).
2. Program the BV ring for real. The write-probe found its window (`0xa40`..`0xa4f` writable, control word at
   `0xa62`, rptr at `0xa66`) but no BASE, and programming everything found did not make thread control work —
   consistent with the thread also needing a GMU-side setup this port never does.


## Round 10: Turnip's no-binning option does not change this stream

`TU_DEBUG=nobin` is the runtime's own no-binning switch (its CI uses `TU_DEBUG = "gmem,nobin"`), so the plugin
now sets it before opening the device:

    setenv("TU_DEBUG", "nobin", 1);

The stream the render server submits is **unchanged**: same 1000 bytes, same first packet `70970001 00000003`.
So this command stream is not a draw that binning shaped — it is the runtime's fixed device-open sequence,
emitted before any binning decision exists. The option cannot help there.

### What that leaves

The only lever left that does not depend on the runtime is to **change the packet in the driver**, which is
possible because the driver already reads these buffers successfully (`peek 0`, real words):

- add a write side to `NativeAdrenoVm` beside `peekWords`, and
- in `NativeAdrenoSubmit::submit`, when a board asks for it, rewrite every CP_THREAD_CONTROL in the client's IB
  so it never selects the BV thread (`payload 3 -> 1`, `payload 2 -> 1`), then submit.

That is board-specific and blunt, and it will not run the binning pass — but it is the decisive experiment:
if fences start retiring once no packet names BV, the diagnosis is proven end to end and the remaining work is
turning that into a correct no-binning path rather than a rewrite.


## Round 11: the IB was rewritten and it made no difference

The driver can now write where it could only read before: `NativeAdrenoVm::pokeWords` (through the buffer's own
mapping when it has one, else through a new board hook `native_adreno_dram_write32`, which fuxi implements as a
read-modify-write through the same uncached physical aperture `dramRead32` uses). With
`native_adreno_bv_to_br` the submit path rewrites every CP_THREAD_CONTROL in the client's IB so it never
selects BV:

    bv_patch ok 1 last_bytes 1000 refused 0

the whole 1000-byte IB was patched, and the fence **still** does not retire:

    fence live 1 submitted 10 retired 9
    marklog 6: 6800 6801 6a3b 6b10 6b11 6b12

So the BV thread is not the only thing that stops this command processor. The probe's own test still shows the
payload difference (3 stalls, 0x08000001 retires), but on the client's stream the thread control was not what
it was waiting on.

### What the stream does after the thread control

The IB uses **indirect buffers**: `703d0004 00027000 00000001 00000000 00000000` is CP_INDIRECT_BUFFER
targeting 0x00027000, and `703e8003 40080980 00027008 …` follows it. If those second-level buffers are not
reachable, the command processor stops exactly like this — no fault, no progress. That is the next thing to
check: whether the memory those packets point at is mapped in the table the CP is using.

The write machinery is worth keeping either way: it is the first time this port could repair a client's command
stream instead of only observing it.


## Round 12: the indirect buffer the CP jumps into is empty

The stream's `703d0004 00027000 00000001 …` decodes to CP_INDIRECT_BUFFER with address
`0x00027000 | (1 << 32)` = **0x100027000**, four pages below the main IB at 0x100028000. The driver now scans
for those packets and reads their target:

    ib_ind n 3 peek 0 addr 100027000 size 0 words 0 0 0 0 0 0 0 0
    ib_ind sample st 0 cached 0 dram 0

The peek succeeds (the address **is** mapped), and the cacheable view and DRAM agree word for word: the buffer
is genuinely all zeros. A zero word is not a PM4 header, so the command processor reads it, finds nothing it can
execute, and stops — with no fault and no progress, exactly the behaviour this board has shown from the start.

So this was never a visibility problem like the main IB had (that one was fixed by `sheng-gpu-wc=0`); the
second-level buffer really is empty at the moment it is submitted.

### What that means

An indirect buffer needs someone to fill it. With the BV thread dead, the most likely writer is gone: a binning
pass produces exactly this kind of control/visibility buffer, and this port never runs one. That reading makes
the whole picture consistent — the stream is built for concurrent binning, the BV thread never runs, its output
buffer stays zero, and the command processor stops reading it.

Either the BV thread has to run (which needs a BV ring this port has never programmed), or the runtime has to be
made to emit a stream that does not depend on it.


## Round 13: the empty indirect buffers are not the blocker either

With `native_adreno_drop_empty_ib` the submit path turns every CP_INDIRECT_BUFFER whose target begins with a zero
word into CP_NOP, keeping the packet's length so the rest of the stream stays aligned:

    empty_ib dropped 3 last 27000
    fence live 1 submitted 10 retired 9

Three jumps neutralised, and the fence still does not retire. So an empty second-level buffer is one symptom of
the missing binning pass, not the thing the command processor waits on.

### What has now been eliminated by direct measurement

| candidate | how it was tested | outcome |
| --- | --- | --- |
| SYNC_THREADS in CP_THREAD_CONTROL | payload 0x08000000 vs 0x80000001 | fixed, was real |
| CP in secure mode | `sheng-gpu-zap=0` vs 4 | real, one more submission passes |
| BV thread selected | whole IB rewritten to BR only | no change |
| empty indirect buffers | three of them rewritten to CP_NOP | no change |
| write-combined client mappings | `sheng-gpu-wc=0` | fixed, was real |
| page-table switching | single table | fixed, was real |
| framed prologue/fence | `plain_submit` both ways | framed is worse |

What is left in the stream is the family of wait packets (`70d58003 …`, and `70928000` CP_WAIT_MEM_WRITES) — waits
for values a binning pass would write. That is consistent with everything above: the stream is built for
concurrent binning, the BV thread never runs, and its results never appear.

### The two remaining routes

1. **Program the BV ring for real.** The write probe located its control word (0xa62, 12 bits) and rptr (0xa66, 20
   bits) and a writable window at 0xa40..0xa4f, but its BASE has never been found; every attempt to program it
   left thread control unchanged.
2. **Build Turnip from source with the binning pass removed**, so the streams it emits never depend on BV. The
   Mesa tree is present (`out/.../tree/.cache/mesa-work`), `build-turnip-darwin.py` builds the iOS dylib, and the
   plugin now rebuilds — so this is a build-hours problem, not an unknown.


## Round 14: the BV offsets in this port were invented, and the real ones kill the bring-up

`NativeAdrenoRing::program()` was writing the BV ring at **0x0a80/0x0a81/0x0a82/0x0a87** — offsets this port
guessed. The write probe of round 6 had already shown that whole range reads the 0xdeafbead sentinel and takes
no writes: nothing lives there. So the BV ring was never programmed at all, by either path.

The real block mirrors BR at **+0x260** (BR: BASE 0x800, CNTL 0x802, RPTR 0x806, WPTR 0x807; the probe found BV's
control word at 0xa62 holding BR's own 0x20c, and its rptr at 0xa66 holding BR's 0x4e). Those CP registers are
per-pipe on a7xx, so the writes now select PIPE_BV through `CP_APERTURE_CNTL_HOST` first.

Result: the bring-up does not finish at all.

    fuxi-gpu: gpuStart buffers call done 0, first bo phys 8d6b01000 bytes 77824
    … nothing further …
    fence live 0 submitted 0 retired 0
    marklog 0:

Writing 0xa60 with the BV pipe selected stops the command processor during the very first `program()`, before a
single submission exists. So the BV block is not simply readable/writable the way BR's is, and this port has no
evidence yet for what actually enables that thread.

`native_adreno_bv_ring` is therefore back to 0, and the driver keeps the corrected offsets behind that knob with a
comment saying they stop the bring-up — so the next attempt does not start from the invented 0xa80 again.


## Round 15: the rewrite is really in memory, and the stall is still there

Round 11's rewrite left one doubt — whether the patched words actually reached the buffer or only the driver's
copy. Reading them back after the write settles it:

    bv_patch ok 1 last_bytes 1000 refused 0 back 10001

`back` packs the first two words the peek now returns: header `0x70970001` and payload **1**, not 3. The client's
first CP_THREAD_CONTROL says BR only in memory, and the fence still does not retire.

So, measured rather than assumed, this port has ruled out, on the client's own stream:

- the thread the packet names (rewritten, read back, unchanged result),
- the three empty second-level buffers (rewritten to CP_NOP, unchanged result),
- SYNC_THREADS, secure mode, write-combining and page-table switching (each fixed earlier and still fixed).

What remains inside those 1000 bytes is Turnip's own state setup: a long run of type-4 register writes in the
0x8000 range and packets this port's decoder has no name for (opcode 0x55 with three payload words, 0x39, 0x47,
and `70928000`, opcode 0x12 with no payload).

### Where that leaves the work

The remaining honest options, in the order they should be tried:

1. **Name the packets.** Decode the whole stream with the PM4 kit instead of by hand, so the packet the
   command processor stops on is identified rather than inferred from what it is not.
2. **Build Turnip without the binning pass**, so the stream is one this port has evidence for (BR only, no
   binning buffers) end to end.
3. Keep hunting the BV ring's enable, which two attempts have now failed at.


## Round 16: the stream decoded, and one of this port's own conclusions was wrong

The whole 250-dword stream, decoded with Mesa's own opcode table (`adreno_pm4.xml`) instead of by hand:

    0   CP_THREAD_CONTROL   {1}
    2   CP_EVENT_WRITE      {0x19} {0x18} {0x28} {0x33}       CCU invalidates
    10  CP_WAIT_FOR_IDLE
    …   ~100 type-4 register writes (0x8000..0xb600 range: RB, SP, TP, VPC, GRAS, UCHE, CP)
    140 CP_COND_REG_EXEC    {0x54000000, 6}                    a conditional block
    169 CP_SET_DRAW_STATE   {0x40000, 0, 0}                    an indirect state block
    173 CP_BV_BR_COUNT_OPS  {2, 1}
    176 CP_THREAD_CONTROL   {0x8000001}                        BR only
    178 CP_SET_AMBLE       {0x26000, 1, 0x10009b}
    182 CP_THREAD_CONTROL   {2}                                BV
    184 CP_SET_AMBLE       {0x2626c, 1, 0x100095}
    188 CP_THREAD_CONTROL   {3}                                both
    190 CP_REG_TEST        {0x82000000, 0x18, 0x10}
    194 CP_SET_AMBLE       {0, 0, 0}
    202 CP_THREAD_CONTROL   {0x8000001}
    204 CP_MEM_WRITE       {0x27000, 1, 0, 0}
    209 CP_INDIRECT_BUFFER {0x27008, 1, …}                     ← filled by the CP itself
    214 CP_MEM_WRITE       {0x27010, 1, 0, 0}
    219 CP_INDIRECT_BUFFER {0x27018, 1, …}
    224 CP_REG_TO_MEM      {0x40080980, 0x27008, 1}
    233 CP_WAIT_FOR_IDLE
    234 CP_REG_TO_MEM      {0x40080980, 0x27018, 1}
    243 CP_EVENT_WRITE      {0x21} {0x20} {0x31}
    249 CP_WAIT_MEM_WRITES

That decode **overturns rounds 12 and 13**: the indirect buffers at 0x27008/0x27018 are not "never filled because the
binning pass is missing" — the stream writes their contents itself with CP_MEM_WRITE two packets earlier and then
jumps through them, which is why they read as zeros when the driver looks *before* the command processor has run.
Rewriting them to CP_NOP was this port breaking a correct idiom, and it is reverted.

### What the decode points at instead

The packets that make the command processor read memory this port has not verified are
**CP_SET_DRAW_STATE {0x40000, …}** and the three **CP_SET_AMBLE** calls (0x26000, 0x2626c). Those are Turnip's
preamble/amble mechanism: small command buffers the CP fetches by address. If one of those addresses is not
mapped in the table the CP is using, the symptom is exactly what this board shows.

That is the next thing to read: what lives at those addresses from the driver's side, the same way the hit-rate of
`peekWords` answered it for the main IB.


## Round 17: CP_SET_AMBLE named, and a blind spot in this port's own instruments

`CP_SET_AMBLE` is opcode **0x55** — the packet the decode could not name. Its first two payload words are the
command buffer the CP jumps to, and in the decoded stream those were `{0x26000, 1, 0x10009b}` and
`{0x2626c, 1, 0x100095}`: addresses in the client's own space, which is exactly the kind of fetch that stops this
command processor when it lands on memory the CP cannot reach.

The driver now records what those addresses hold. It reads none of them:

    amble n 0        (after skipping the terminating amble, which carries address 0)
    amble n 1 peek e00002c2 addr 0    (before skipping: only the empty terminator is in this buffer)

### The blind spot

Every instrument this port has built — the IB log, the indirect-buffer scan, the thread-control rewrite, the amble
probe — reads **`buffers[0]`**, the first buffer of a submission. Turnip submits several: a preamble, the draw
state, the amble bodies. So the decoded stream of round 16 and the empty amble count of this round are statements
about the *first* buffer only, and the packets that actually make the CP fetch by address may live in a later one.

That is the next thing to fix, and it is cheap: record every buffer of a submission, not just the first, then run
the same decode over each.


## Round 18: a submission here is one buffer, so the blind spot was imaginary

Recording every buffer of the newest submission, not just the first:

    ib_bufs 1
      b0 addr 100028000 bytes 16384 peek ... head 70970001 3 70460001 19 70460001 18

**One buffer.** The 16 KiB BO starts with the same three packets as always, and there is no second IB, so round 17's
worry — that the interesting packets live in a later buffer — does not apply to this client on this board.

That makes one thing harder to explain rather than easier: the round-16 decode of this same buffer showed two
`CP_SET_AMBLE` packets with real addresses (0x26000/0x2626c), while the amble probe that scans the same words
records none. The probe and the decode disagree about the same data, which means the probe has a defect, not the
stream.

### Practical state

The instruments now in the driver, all of which are kept:

| instrument | what it answers |
| --- | --- |
| `native_adreno_ib_log` (256 words) | what the client's IB says |
| `native_adreno_ib_bufs` | how many buffers a submission carries, and each one's head |
| `native_adreno_ib_ind`, `_amble` | whether the CP's second-level fetches are reachable |
| `native_adreno_bv_patched`, `_empty_ib` | whether a rewrite landed, read back |
| `native_adreno_mark_log` | the plugin's progress in order |
| `native_adreno_dram_write32` (board) | writing into a buffer the kernel has no mapping for |

The next round should reconcile the probe with the decode — one of them is wrong about the amble packets, and until
that is settled neither can be trusted for the packets after index 170.
