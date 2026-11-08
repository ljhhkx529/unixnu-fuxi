# fuxi GPU upstream update and sheng comparison (2026-11-08)

## Result

The three GPU components were updated to the latest `github.com/UniXNU` revisions available on 2026-11-08. The fuxi-only ring, VM and diagnostic work was retained on top of the new Adreno740 base. The kernel, payload and boot image build, the host Turnip path, flashing, Metal discovery and live GPU/GOP submission paths were tested.

Metal starts and returns a device on fuxi. The strongest live evidence from `/private/tmp/fuxi-usb/run-20261108-112133/xnu-usb.log` is:

```text
fuxi-metal: opens 9 busy 0 bad 0 ...
fuxi-metal: client calls 650 ... attach 1 inflight 0
fuxi-metal: marklog ... 6800 6801 ... 6814 6802 6803 ...
fuxi-display: fence live 1 submitted 48 retired 48
fuxi-metal: slots submitted 48 retired 48
fuxi-display: gop offers 16 done 16 fail 0 abandon 0
```

The device remained on `/dev/cu.usbmodem101`; `fastboot devices` was empty and USB control `STATUS` returned `OK`. No panic, GPU stall or restart appeared in the successful run.

This qualifies the updated stack as functional, but not as performance-complete: the same status pages recorded 83 GOP/GPU waits reaching the 500 ms timeout and 43 failed method-6 waits, even though every submitted fence eventually retired and every GOP offer completed. That latency needs follow-up before claiming smooth or production-ready Metal.

## Version comparison

| Component | Before fuxi update | sheng reference | Official latest used | Difference after sheng reference |
| --- | --- | --- | --- | --- |
| Adreno740 | `e8363d98e017` over old base | `dea9ad9754da` | `6fa7bae2a5b3` | 7 commits |
| AdrenoMetal | `a0f0287be80a` | `9abfca8ddebd` | `6d7f73437087` | 21 commits |
| GopFramebuffer | `19ef3a75ad2c` | `5edfc66952c4` | `e47c1b8de169` | 7 commits |

Adreno740's checked-out fuxi HEAD is `5c90626b7f2c`: five fuxi commits were rebased on official `6fa7bae2a5b3`. This is intentionally not a clean official-only checkout because dropping those board fixes regresses the working fuxi command stream.

## What the sheng work contributes

The sheng implementation is useful to fuxi in four concrete areas:

1. Two-ring scheduling and task-role priority handling. The updated Adreno740 contains the scheduling work and the later focused trace filtering. fuxi still ships the single-table/plain-submit workaround because its command processor does not yet accept the standard table/thread sequence.
2. Direct-display fence ownership. GopFramebuffer waits for GPU completion before handing a surface to display, and its current diagnostics share the same trimmed trace vocabulary as AdrenoMetal/Adreno740.
3. Metal resource lifetime and cache policy. The current bridge includes bounded image reuse, spare-image room management, queue QoS, warm/lowering/commit paths behind `sheng-metal-flow`, and a newer metal2vulkan base.
4. Retained, focused diagnostics instead of pixel capture. The three components now agree on the seven-issue trace selection, refusal records and GMEM-swap event names, which makes fuxi/sheng logs directly comparable.

The newest changes after the sheng reference are primarily diagnostic and runtime scheduling/cache improvements, not a replacement for fuxi's hardware-specific bring-up fixes.

## Preserved fuxi overlay

The update retained and revalidated the following fuxi behavior:

- two-dword `CP_SMMU_TABLE_UPDATE`;
- process VM read-back and the `sheng-gpu-single-table=1` kernel-table mount;
- the legal `CP_NOP` word `0x70108000` and traversal of every top-level submission buffer;
- independent bounded `PatchWalk` state per top-level IB;
- BV-to-BR thread-control rewrite to `0x08000001`;
- optional removal of `WAIT_MEM` and `SYNC_THREADS`;
- plain-submit, prologue/fence sweep and ring/IB diagnostics;
- uncached VM poke/peek support;
- Metal cache-open markers `6B10` through `6B13`;
- GOP handoff counters.

Upstream conflicts were resolved by keeping its preemption table snapshot and postamble logic while changing the SMMU update payload to two dwords. The Metal cache cleanup also keeps upstream's shared-cache exclusion while retaining the fuxi markers.

## Build compatibility updates required by the new components

The component update exposed parent-tree assumptions that had to be synchronized:

- `components.toml` now pins Metal `6d7f734`, GOP `e47c1b8`, and the rebased fuxi Adreno740 overlay `5c90626` (official base `6fa7bae`).
- metal2vulkan moved from `624bab3918de` to `963f0e64f103`; the patch series now uses the 25 files retained by AdrenoMetal `c2847ce` and drops five patches already upstreamed/removed.
- the build invokes Rust 1.98 through `rustup run`, independent of the Homebrew Cargo found first in `PATH`.
- the host fixture now supplies the XNU GPU-role constants, links `NativeAdrenoPreempt` and host kernel hooks, advertises the new client VA window above `0x100000000`, and handles the priority method like the published user client.
- host Mesa required GNU Bison 3.8.2 rather than Xcode's Bison 2.3.

## Verification

### Source and host gates

- All three component worktrees pass `git diff --check`.
- `python3 scripts/build-arm-driver.py Adreno740`: passed `-Werror` and arm64 object audit.
- host Turnip darwin-kmd: passed, dylib size 22,961,128 bytes.
- host Vulkan path: device creation, a 1 MiB mapping, fill-buffer retirement, eight deferred-BO failure/pressure modes and pipeline-cache hit/miss controls passed. Optional QuartzCore recipe qualification was not run because its exact original `default.metallib` input was not mounted.
- iOS Metal bridge: passed, 238,039,528-byte archive, translator `963f0e64f103`.
- AdrenoMetal bundle, Turnip dylib and probe daemon: built and 4 KiB-signed. Bundle CDHash `8d4eaa44db4cdfce0462157d89a50ae1b913f1ba`.
- owner trust bundle SHA-256: `d67168b824c275feb62b7d09ddd04d0fee994b3a4d4803248488eab74cfa5233`.
- rebuilt GPU addon: 9 files, 12,791,856 bytes, including `NativeAdrenoMetal.publish`.

### Complete image build

```text
kernel=0
payload=0
bootimg=0
```

Build logs are `/tmp/he-upgrade.log`, `/tmp/hf-upgrade.log` and `/tmp/hg-upgrade.log`; the final addon repack used `/tmp/hf-upgrade2.log` and `/tmp/hg-upgrade2.log`.

### Device run

The final boot image was flashed to `boot_a`. The first post-flash boot returned to Fastboot after the one-time Data/addon reset. A second boot was stable. After discovering that the first image still carried the old addon, the addon was rebuilt from the new plugin, payload/boot image were repacked and the final image was flashed again.

Final live counters:

```text
Metal: opens 9, bad 0, client calls 650
Metal markers: 6800 -> 6801 -> 6814 -> 6802 -> 6803
GPU fences: submitted 48, retired 48
GOP: offers 16, done 16, fail 0, abandon 0
GPU faults/stalls/restarts/panics: none observed
Latency warning: gop_wait calls 100, timeouts 83, last timeout 500 ms
```

## Recommendation

Keep these upstream versions: they add useful sheng-derived scheduling, trace and Metal resource-management work and the final fuxi run is functionally successful. Do not remove the fuxi overlay yet. The next bring-up target should be the 500 ms wait population: correlate method-6 waits with the `6730/6731` scanout markers, then test whether the current `sysmem,nocb,nobin`, single-table and thread-control workarounds are still all necessary one at a time.
