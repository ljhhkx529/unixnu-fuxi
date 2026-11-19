# 2026-11-19, the modem's runtime path, first run

The image was flashed to the boot partition only (`fastboot flash boot`, serial b4c62a1e). The
kernel is `d924b64d0c6e66d61811a98e8bca5622045fd5b559990ca1fc5bbb0a9957e31b`, the boot image is
`149aa9bbfd8a182c8a740e836e2c474c7b2882e1a48572f6d472d0d07b25784e`, and the kernel arguments carry
`fuxi-adsp=1 fuxi-modem=4`.

## What the run proves

The RSC layout fix works. The bring-up reached stage 3 (rails-voted), which is the first stage
that needs the vote to complete: before the fix the probe used the version 2_7 offsets for a DRV2
id register that reads zero, and the request was written to registers that are not the request
registers, so the done bit never arrived. The vote went out on the forced major-3 layout and
finished.

The ADSP's runtime really is out of the way. The ADSP's bring-up ran before the modem's in
bsd_init and would have taken the IPCC interrupt line 261 and the RSC interrupt line 37, and an
interrupt line cannot be attached twice. The modem's stepIpcc attached both lines without an
exclusive-access failure.

## What the run does not prove

The modem was not started. The secure world refused the modem device tree blob's metadata:

    FUXI_MODEM: the modem device tree blob was refused (scm 4291821499/0)
    FUXI_MODEM: the bring-up stopped at stage 3 (-1)

4291821499 is 0xFFCFFFBB, and the probe publishes its low half as 4027 (0xFBB). The state read
over the control channel agrees:

    stage=3 (rails-voted) last_error=4095 smp2p_ready=0 smp2p_fatal=0 glink_up=0
    mss_rail_found=1 loaded_segments=0

So the failure is in the first PAS call for the DTB image (INIT_IMAGE), not in the rails, not in
the mapping and not in the interrupt lines. Nothing was written to the modem, no RF was touched,
and the SMP2P ready and fatal bits are both clear.

## The open question

Why the secure world refused INIT_IMAGE for the DTB. The call's shape is the same as the ADSP's
proven one: the same command, the same argument descriptor (two arguments, the second read-write),
the same argument order, and the metadata buffer in a physically contiguous high-DRAM segment.
The difference between the two bring-ups is the PAS id (0x26 for the DTB against the ADSP's) and
the image itself. The next step is to find out what the secure world objects to, which the
current probe does not publish: it stops at the first refusal and does not say which argument the
secure world found wrong.
