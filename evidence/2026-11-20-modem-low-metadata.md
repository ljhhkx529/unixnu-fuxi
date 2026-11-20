# 2026-11-20, the metadata below 4 GiB: the secure world accepts it

One image was flashed to the boot partition only (`fastboot flash boot`, serial b4c62a1e). The change
under test is the third item of the analysis document, done the only way it can be done here: the
metadata's address is **fixed below 4 GiB**, in a window the port maps itself.

**The result is the first acceptance of the modem's device tree blob on this board.** Before this
round the blob was refused with `0xFFCFFFBB` on every attempt, whatever the metadata's source,
address or slot. Now both the blob's and the modem image's metadata are accepted, the blob's
AUTH_AND_RESET is accepted, the modem image's INIT_IMAGE and MEM_SETUP are accepted, all 31 of the
modem image's segments are copied into the carveout, and the modem's AUTH_AND_RESET does not come
back.

## Why a fixed address and not a mask

Upstream takes the metadata's address from `dma_alloc_coherent` on the SCM platform device, whose
masks default to 32 bits (`setup_pdev_dma_masks`, `drivers/base/platform.c`), so upstream's metadata
always lands below 4 GiB or the allocation fails. On this board it failed: the mainline fuxi port
saw `modem_dtb`'s PAS init return **-12** and fixed it with `CONFIG_ZONE_DMA` and
`CONFIG_ZONE_DMA32`, whose only effect is to let the coherent allocation land below 4 GiB.

XNU's mask API cannot do the same thing on arm64, and that was traced through the source:

| step | what it does | source |
| --- | --- | --- |
| `IOBufferMemoryDescriptor::initWithPhysicalMask` | turns the mask into a `highestMask` and an alignment | `refer/xnu/iokit/Kernel/IOBufferMemoryDescriptor.cpp:176` |
| `IOKernelAllocateWithPhysicalRestrict` | drops any `maxPhys <= 0xFFFFFFFF` to zero and substitutes `KMA_LOMEM` | `refer/xnu/iokit/Kernel/IOLib.cpp:719` |
| `vm_lopage_needed` | false on arm64; only i386 sets it | `refer/xnu/osfmk/i386/i386_vm_init.c:828` |
| `vm_page_grablo` | falls through to a plain `vm_page_grab`, so the low-memory request is ignored | `refer/xnu/osfmk/vm/vm_resident.c:3051`, `:3181` |

That is exactly what the device measured: the audio chain's 32-bit mask produced a physical address
of `0x8CFF84000` (35.25 GiB), and the modem's metadata landed at `0x8d39ab000` (35.4 GiB). Both
above 4 GiB.

The mechanism that does work is a fixed low-DRAM mapping, and the audio chain already proved that
recipe on this board: `IODeviceMemory::withRange(address, bytes)->map(kIOMapAnywhere | cache)`.
`kIOMapAnywhere` is required — without it the fixed mapping fails silently.

## The window

`package/FuxiModemLowMeta.h` holds the window and the reasons. `0x81480000` of `0x3000` bytes:

- below 4 GiB, page aligned;
- the first free byte of the board's own DRAM row `DRAM 0x81480000` of `0x580000` in
  `../uefi-fuxi`'s `MemoryMapLib.c`, which runs to `0x81980000`;
- the device tree's `reserved-memory` list puts nothing between the `xbl_ramdump_region`
  (`0x81200000` of `0x280000`, whose end is exactly where the window starts) and the
  `xbl_dt_log_merged` region (`0x81a00000`, which is after the window ends);
- clear of the modem's own carveouts (`0x8a800000` of `0x10800000`, `0x9b000000` of `0x80000`), the
  DSM region the assign hands over (`0xd4d00000`), the audio chain's window (`0xd0000000`), the
  command database (`0x81c60000`) and the shared memory window (`0x81d00000`);
- outside XNU's own allocator, whose window DarwinBoot places at `0x8c0000000` (the device log of
  2026-11-19 records the kernel's physical address as `0x8C007C000`), so a fixed mapping there
  cannot alias with a kernel page.

Two slots: the blob's metadata at `0x81480000` (one page; the blob's metadata is 4044 bytes) and the
modem image's at `0x81481000` (two pages; the image's metadata is 6364 bytes). Both are alive at the
same time, because upstream keeps both metadata buffers until the modem has come up
(`qcom_pas_start` releases them after `qcom_q6v5_wait_for_start`).

The mapping is write-combined, not uncacheable. The uncacheable mapping hung the modem's thread on
2026-11-20 (see the region-assign evidence), and the write-combined one is what the audio chain
proved on this board.

## The device result

```
FUXI_MODEM: the assign hands d4d00000 of 53477376 bytes from vmid 3 to vmid 15, the block is at 8d53df000
FUXI_MODEM: the assign's answer is 0/0
FUXI_MODEM: the secure world accepts the pas id 38 (scm 0/0)
FUXI_MODEM: the secure world accepts the pas id 4 (scm 0/0)
FUXI_MODEM: the low metadata window is 81480000 of 12288 bytes, below 4 GiB
FUXI_MODEM: the metadata is 4044 bytes at 81480000, hash 2, fixed, from the mdt file
FUXI_MODEM: pas 38 step 0 answers 0/0
FUXI_MODEM: pas 38 step 2 answers 0/0
FUXI_MODEM: the metadata is 6364 bytes at 81481000, hash 31, relocatable, from the mdt file
FUXI_MODEM: pas 4 step 0 answers 0/0
FUXI_MODEM: pas 4 step 1 answers 0/0
```

Read line by line:

| line | what it means |
| --- | --- |
| `pas 38 step 0 answers 0/0` | the blob's **INIT_IMAGE** is accepted. This is the call that was refused with `0xFFCFFFBB` on every earlier attempt. |
| `pas 38 step 2 answers 0/0` | the blob's **AUTH_AND_RESET** is accepted. The blob is now running. |
| `pas 4 step 0 answers 0/0` | the modem image's **INIT_IMAGE** is accepted. |
| `pas 4 step 1 answers 0/0` | the modem image's **MEM_SETUP** is accepted. |

**The metadata's address was the reason for the refusal.** The only thing that changed between the
run that was refused (`0x8d39ab000`, above 4 GiB) and this one (`0x81480000`, below 4 GiB) is the
address the metadata was handed over at. The metadata's bytes, its source (the `.mdt` file), its
size, its alignment, the region assign, the rails vote, the order of the steps and the argument-type
words were all the same.

This is the same defect the mainline fuxi port hit and fixed with `CONFIG_ZONE_DMA32`, measured
here in XNU's own terms.

## Where the bring-up stops now

The probe's stage reached **5** (`kStLoaded`, the modem image is loaded) and all **31** of the modem
image's segments were copied into the carveout. The modem image's **AUTH_AND_RESET** did not come
back: the log ends after `pas 4 step 1 answers 0/0` and no further `FUXI_MODEM:` line follows. The
kernel keeps running and the machine reaches launchd, so the thread is stuck in the SCM call rather
than panicked.

The diagnostic pages read over the USB control channel say the same: `stage = 5`, `segments = 31`,
`mss rail = 1`, `smp2p ready = 0`, `smp2p fatal = 0`, `glink up = 0`, and the last error is 0.

So the bring-up is one step further than it has ever been: the secure world now accepts and
authenticates the modem's device tree blob and the modem image, and the modem image's reset does
not return.

The one upstream difference that is still open is the SCM's own clocks and bandwidth: upstream wraps
every PAS call in `qcom_scm_clk_enable()` and `qcom_scm_bw_enable()` (`qcom_scm.c`). On this board
the clock half is a no-op, because the `sm8550` device tree's `scm` node carries no `clocks`
property, so `devm_clk_get_optional` returns NULL and `clk_prepare_enable(NULL)` returns 0. The
bandwidth half votes the interconnect path the same node's `interconnects` property names. The port
does not do either.

## Host checks

- `python3 scripts/check-fuxi-modem-low-meta-host.py` → `PASS fuxi_modem_low_meta_test`. The check
  reads the real header: the window is below 4 GiB and page aligned, the two slots are inside the
  window and do not overlap, the blob's 4044 bytes fit in one page and the image's 6364 in two, and
  the window overlaps none of the ten regions it must not touch.
- `python3 scripts/check-fuxi-modem-assign-host.py` → `PASS fuxi_modem_assign_test`.
- `python3 scripts/check-fuxi-modem-pas-host.py` → `PASS fuxi_modem_pas_test`.
- `./unixnu build kernel` → the kernel links; sha256
  `61f320a1eba3032c7ae0ef57ed52bda6861fb40a62cf6215bf6e1bf6daca319b`.

## The image

| image | what it carried |
| --- | --- |
| `/tmp/fuxi-lowmeta.img` | the metadata in the fixed low DRAM window at `0x81480000` |

Only the `boot` partition was written. Nothing else was flashed, and no partition other than `boot`
was touched.
