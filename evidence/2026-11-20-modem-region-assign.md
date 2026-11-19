# 2026-11-20, the region assign: it is accepted, and the blob is still refused

Four images were flashed to the boot partition only (`fastboot flash boot`, serial b4c62a1e), each
with its own kernel. The sequence under test is the one the analysis document asks for: the memory
ownership assignment, the upstream DTB order, and the whole-page metadata. The device is the same
board (Xiaomi 13 / fuxi / SM8550), the firmware is the board's own, and nothing was flashed except
`boot`.

## What was asked for and what was done

| # | The analysis document's item | What the port does now |
| --- | --- | --- |
| 1 | region assign, the only structural difference between the ADSP's path and the modem's | implemented, in `package/FuxiModemAssign.h` and `stepAssign()` |
| 2 | the DTB in upstream's order | the port was already in upstream's order; the analysis document's §2.2 / §3.1 claim was wrong (see below) |
| 3 | the metadata in a whole page | implemented, `allocSecurePage()` |

## The region's shape, from upstream

`drivers/remoteproc/qcom_q6v5_pas.c`, `sm8550_mpss_resource`: `region_assign_idx = 2`,
`region_assign_count = 1`, `region_assign_vmid = QCOM_SCM_VMID_MSS_MSA`. The ADSP's entry carries no
`region_assign_idx`. `region_assign_idx = 2` indexes the remoteproc's own `memory-region` list,
`memory-region = <&mpss_mem>, <&q6_mpss_dtb_mem>, <&mpss_dsm_mem>`, so index 2 is `mpss_dsm_mem` at
`0xd4d00000` of `0x3300000` bytes.

The call itself is the memory-protection service, not the peripheral authentication service: service
`0x0c`, command `0x16` (`QCOM_SCM_SVC_MP`, `QCOM_SCM_MP_ASSIGN`), with seven arguments, whose
argument-type word is `0x1117` (`QCOM_SCM_ARGS(7, RO, VAL, RO, VAL, RO, VAL, VAL)`).

Seven arguments do not fit in SMC64's registers the way the PAS calls do, so the transport is
upstream's (`drivers/firmware/qcom/qcom_scm-smc.c`): `desc.args[0..3]` go in `x2..x5`, and because the
call has more than four arguments, `desc.args[3..9]` are copied into a separate block of seven 64-bit
little-endian values whose physical address is what `x5` ends up carrying. The `x6` register is the
quirk's session, so the block's address cannot go there.

The block upstream builds (`__qcom_scm_assign_mem`) has three parts, each on a 64-byte boundary: the
source owners (one 32-bit VMID each), the memory to map (a 64-bit address and a 64-bit size), and the
new owners (one 24-byte record each: VMID, permission, a 64-bit context, the context's size, one
unused word). For one source and one destination the offsets are 0, 64 and 128, and the block is 192
bytes.

Upstream's block does not need secure memory: `qcom_tzmem_pool_add_memory`
(`drivers/firmware/qcom/qcom_tzmem.c`) backs the pool with an ordinary coherent DMA allocation unless
`CONFIG_QCOM_TZMEM_MODE_SHMBRIDGE` is on, and that option is off by default and is off in the kernel
config this is taken from. This corrects §7 of the analysis document, which called the secure memory
the main cost of this step.

## The device result

The kernel with the assign in it (sha256 `673e255df76cabbd902ab9432aed630866e2e7f6444f4b1359045ad3be27f176`,
image `/tmp/fuxi-assign.img`) ran this:

```
FUXI_MODEM: the assign starts
FUXI_MODEM: the assign has its page
FUXI_MODEM: the assign has written its blocks
FUXI_MODEM: the assign has cleaned its page
FUXI_MODEM: the assign hands d4d00000 of 53477376 bytes from vmid 3 to vmid 15, the block is at 8d53f5000
FUXI_MODEM: the assign's answer is 0/0
FUXI_MODEM: the secure world accepts the pas id 38 (scm 0/0)
FUXI_MODEM: the secure world accepts the pas id 4 (scm 0/0)
FUXI_MODEM: the metadata is 4044 bytes at 8d39ab000, hash 2, fixed, from the mdt file
FUXI_MODEM: pas 38 step 0 answers 4291821499/0
FUXI_MODEM: the modem device tree blob was refused (scm 4291821499/0)
FUXI_MODEM: the bring-up stopped at stage 3 (-1)
```

**The assign was accepted: `scm 0/0`.** The DSM region was handed from the hypervisor's own owner
(VMID 3) to the modem's MSA (VMID 15) with read and write, exactly as upstream's
`sm8550_mpss_resource` asks, and the secure world took it.

**The DTB's INIT_IMAGE is still refused with the same code**: `4291821499` = `0xFFCFFFBB`, on step 0
(INIT_IMAGE) of pas id 38, with the metadata taken out of the `.mdt` file.

So the region assign is **not** the reason for the refusal. The analysis document's item 1 is a code
fact and it is now done and measured, and the measurement says the hypothesis it carried is false.

## The one real defect this round found

The first image of this round hung. The modem's thread printed its two firmware summary lines and then
nothing: it never reached the first line of `stepAssign()`. The cause was the uncacheable mapping the
document's item 3 asks for: `kIOMapInhibitCache` in the metadata's allocation.

`kIOMapInhibitCache` (`IOMapTypes.h`) becomes `MAP_MEM_IO` in `iomd_mem_types[]`
(`IOMemoryDescriptor.cpp`), and `vmProtForCacheMode` then adds `VM_PROT_WRITE`, because the VM needs
write access to change a mapping's cache mode. The hang is inside the allocation itself, before the
first `printf`, so it is in the mapping path and not in the call.

Upstream's own allocation is `dma_alloc_coherent` (`qcom_scm_pas_init_image`), which on ARM64 is
cacheable but coherent; upstream does not ask for an uncacheable mapping and does not clean anything
either, because the coherence is the DMA API's job. The port's equivalent of that coherence is the
`cleanToPoc` the caller does.

The fix: the metadata's buffer is a whole page, physically contiguous and page aligned, and the rest
of the page is zero, and the page is cleaned to the point of coherency before the call — but the
mapping is not forced to be uncacheable. With that change the thread gets past the allocation and the
assign is answered.

## What the analysis document got wrong, and the record now says so

**§2.2 and §3.1 are wrong about the DTB.** The document says upstream's DTB does not go through
`qcom_mdt_load_no_init`, that the DTB's segments are not copied into the carveout, and that upstream's
order is `INIT_IMAGE → AUTH_AND_RESET` with no segment load in between.

Upstream's DTB does go through `qcom_mdt_pas_load` (`qcom_q6v5_pas.c`, `qcom_pas_load`), which calls
`qcom_mdt_load_no_init` (`mdt_loader.c`), which **does** copy the segments: a non-split image is
`memcpy`'d into the carveout and a split one goes through `mdt_load_split_segment`. Upstream's order
for one image is `INIT_IMAGE → (MEM_SETUP if relocatable) → copy the segments → AUTH_AND_RESET`,
which is exactly what the port's `stepPas()` already did. Implementing the document's version would
have moved **away** from upstream.

The difference that was real was the metadata's release timing: upstream releases both metadata
buffers only after `qcom_q6v5_wait_for_start` (`qcom_pas_start`), because the secure world turns on
memory protection for that page during the call. The port used to release the DTB's page right after
the DTB's authentication. That is fixed: both pages are released after the modem is up.

**§7 is wrong about the secure memory.** The assign's parameter block needs no secure memory; see
above.

## The state of the bring-up

Unchanged from 2026-11-19: the modem's device tree blob's INIT_IMAGE is refused with `0xFFCFFFBB`
and the modem is not started. The region assign is now done and accepted, so the list of upstream
steps the port was missing has one entry fewer. The remaining upstream difference is the SCM's own
clocks and bandwidth (`qcom_scm_clk_enable` and `qcom_scm_bw_enable`), which wrap every PAS call
upstream. The port does not do them, and the ADSP's blob is accepted without them, so they are not
the reason for "can it authenticate", but they are the last upstream step the port does not have.

## Host checks

- `python3 scripts/check-fuxi-modem-assign-host.py` → `PASS fuxi_modem_assign_test`. The check reads
  the real header, so it checks the service and command, the argument-type word, the three offsets
  and the block's size, the block's bytes, the board's values, and the external arguments' bytes.
- `python3 scripts/check-fuxi-modem-pas-host.py` → `PASS fuxi_modem_pas_test`.
- `python3 scripts/check-qcom-mdt-host.py` → `PASS QcomMdt: 108 checks`.
- `./unixnu build kernel` → the kernel links; sha256 `05bd08f1594b06dc03fbe65d9499267f71f7a4eaaf41bcf042317c65b347625c`
  for the image that carries the step traces.

## The images

| image | kernel sha256 | what it carried |
| --- | --- | --- |
| `/tmp/fuxi-assign.img` | `cf077cbb0cbaca3ef0a07068982e75f421abdfd6d27043b86c28bcda2d211080` | the assign, the uncacheable mapping: hung |
| `/tmp/fuxi-assign2.img` | `673e255df76cabbd902ab9432aed630866e2e7f6444f4b1359045ad3be27f176` | the assign, the cacheable mapping: the assign accepted, the DTB still refused |
| `/tmp/fuxi-assign4.img` | `05bd08f1594b06dc03fbe65d9499267f71f7a4eaaf41bcf042317c65b347625c` | the same, with the per-step answers in the log |

Only the `boot` partition was written. Nothing else was flashed, and no partition other than `boot`
was touched.
