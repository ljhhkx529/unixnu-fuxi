# 2026-11-19, the metadata comes out of the .mdt file, and the refusal is unchanged

The image was flashed to the boot partition only (`fastboot flash boot`, serial b4c62a1e), then the
device was rebooted and the runtime bring-up was read over the USB control channel. The kernel
arguments carry `fuxi-adsp=1 fuxi-modem=4`. The kernel is the one built from this tree, and the
boot image is the one built from it.

## What the run says

```
FUXI_MODEM: the dtb has 3 segments, a 148 byte header, hash 2, fixed, a 7992 byte file, a 4096 byte span, 2600468480 to 2600468992
FUXI_MODEM: the image has 32 segments, a 1076 byte header, hash 31, relocatable, a 149013672 byte file, a 276824064 byte span, 2323644416 to 2600468480
FUXI_MODEM: the secure world accepts the pas id 38 (scm 0/0)
FUXI_MODEM: the secure world accepts the pas id 4 (scm 0/0)
FUXI_MODEM: the metadata is 4044 bytes at 8d3a3b000, hash 2, fixed, from the mdt file
FUXI_MODEM: the modem device tree blob was refused (scm 4291821499/0)
FUXI_MODEM: the bring-up stopped at stage 3 (-1)
```

Two things this run settles, and one it does not.

**Settled: the metadata source.** The log says `from the mdt file`, so the metadata is now the
`.mdt`'s own bytes, the header part followed by the hash segment, which is what upstream's
`qcom_mdt_read_metadata` copies out of the `.mdt` (Linux `drivers/soc/qcom/mdt_loader.c`).

**Settled: the refusal does not follow from the metadata's source.** The answer is the same
`0xFFCFFFBB` (4291821499) as before the change. So the metadata's source is not what the secure
world objects to.

**Not settled here, but settled by the mode 11 run below: whether the two sources are the same
bytes.** The mode 11 measurement (packed form against split form, both counted byte for byte) was
run separately, because the two modes cannot run in one boot.

## The address in that line

The metadata landed at `0x8d3a3b000`. That is 35.3 GiB, and it **is** ordinary RAM on this board:
the UEFI memory map (`../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/Library/MemoryMapLib/MemoryMapLib.c`)
has the row `{"DRAM 0x8c0000000", 0x8C0000000, 0x140000000, ...}`, which covers
0x8C0000000..0xA00000000, and the comment on that row records that both measured units list it
as System RAM. An earlier version of this file said the address was "far outside the DRAM of
this board"; that was wrong, and the shape comparison with the audio chain's `0x8CFF84000`
(35.25 GiB) was built on it.

What is true is narrower, and it matters: **this board has no DRAM below 0x80000000.** The
lowest AddMem DRAM row is `DRAM 0x811d0000` of 0x30000 bytes, and everything below 4 GiB is
either reserved (the modem's own `mpss_region` at 0x8A800000, `q6_mpss_dtb_region` at
0x9B000000, `mpss_dsm_region` at 0xD4D00000) or a small carved-out DRAM island. So a
32-bit coherent DMA mask cannot be satisfied by an ordinary allocation on this board at all.

The metadata buffer is allocated with `IOBufferMemoryDescriptor::inTaskWithPhysicalMask`, a
runtime allocation with the physical mask `0xfffffffffffff000`, and this kernel places such
allocations above 4 GiB. Upstream allocates the same buffer with `dma_alloc_coherent` on the
SCM platform device, whose coherent DMA mask defaults to `DMA_BIT_MASK(32)`
(`drivers/base/platform.c`, `setup_pdev_dma_masks`).

This is the same failure the mainline Linux fuxi port hit on this same board. Its handover record
(`docs/交接-新会话-20261030-2110.md`, in the mainline tree at /tmp/x13) names it exactly: the
`modem_dtb` PAS init returned **-12** because `dma_alloc_coherent` of the 4044 byte metadata
returned NULL, and the reason was that with `CONFIG_ZONE_DMA` and `CONFIG_ZONE_DMA32` off and
more than 4 GiB of memory in the system, there was no 32-bit coherent region to fall back to.
Turning those two options on fixed it, and the modem then ran. That port boots this modem on this
device, so the hardware side of the question is settled there.

**What that run does not settle is whether the address width is what the secure world rejects
here.** The later PAS matrix settles it: the ADSP's metadata at `0x8d0b1f000` was **accepted**
with 0/0 while the modem's at the neighbouring `0x8d0b1e000` was refused, and both addresses are
above 4 GiB. A secure world that could only reach 32-bit addresses would have refused both. The
width is therefore not the discriminator between the accepted and the refused blob; the image
content is.

So the port's own established recipe for this — a **fixed low DRAM mapping**, the one the audio
chain already proved works (0xd0000000, 64 KiB, `kIOMapAnywhere` required) — is the right
mechanism to use if the width ever does need constraining, and it is the right shape against
upstream's 32-bit mask. The metadata is 4044 bytes and needs the 4 KiB alignment upstream
requires, so it fits that window. **Not implemented in this round**: the low DRAM window is
64 KiB and already used by the audio chain, so it would need a window of its own.

## What was not touched

Nothing was reset, no segment was written into a carveout, no processor was released, no RF was
touched, and no partition other than `boot` was written.

## The packed form against the split form, measured

A separate boot with `fuxi-modem=11`, same tree, same board, only the mode changed:

```
FUXI_MODEM: the packed test: modem:modem_dtb: the packed and the split forms differ in 0 of 4044 bytes
FUXI_MODEM: the packed test: modem:modem_dtb: the mdt file is 4044 bytes, the hash segment is 3896 bytes at the joined offset 4096
FUXI_MODEM: the packed test: modem:modem_dtb, the packed metadata against the pas id 38 gave scm 4291821499/0 (refused)
FUXI_MODEM: the packed test: modem:adsp_dtb: the packed and the split forms differ in 0 of 4044 bytes
FUXI_MODEM: the packed test: modem:adsp_dtb: the mdt file is 4044 bytes, the hash segment is 3896 bytes at the joined offset 12288
FUXI_MODEM: the packed test: modem:adsp_dtb, the packed metadata against the pas id 36 gave scm 0/0 (accepted)
```

**The two forms are the same bytes on this board: 0 of 4044 differ, for both firmware.** So the
change to take the metadata out of the `.mdt` file is the right shape against upstream and is a
byte-wise no-op here. It is therefore not the reason the modem's blob is refused. The measurement
that showed this before was the withdrawn one; this run counts the differing bytes and can fail.

The `.mdt` file is 4044 bytes: a 148 byte header part followed by the 3896 byte hash segment. The
joined blob carries the same hash segment at 4096 (`modem_dtb`) and 12288 (`adsp_dtb`).

## The slot survey with the .mdt files

A separate boot with `fuxi-modem=12`:

```
FUXI_MODEM: the slot modem_a's modem_dtb: 7992 bytes (0), the mdt 4044 bytes
FUXI_MODEM: the slot modem_b's modem_dtb: 7992 bytes (0), the mdt 4044 bytes
FUXI_MODEM: the two slots' blobs: 7992 bytes compared, 95 differ, the first at 4533
FUXI_MODEM: the two slots' mdt files: 4044 bytes compared, 95 differ, the first at 585
```

The two slots' `.mdt` files differ in the same 95 bytes as their joined blobs, but the first
difference is at 585 in the `.mdt` and at 4533 in the joined blob — the same difference, measured
from two different file layouts. This is the byte-level proof that the two slots hold two
different builds, now on the file the metadata is actually taken from.
