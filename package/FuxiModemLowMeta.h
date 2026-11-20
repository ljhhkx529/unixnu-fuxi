/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The modem's metadata in a fixed low DRAM window, below 4 GiB.
 *
 * Why a fixed address at all. Linux hands the secure world the metadata with
 * qcom_scm_pas_init_image, whose second argument is the metadata's physical address. Upstream takes that
 * address from dma_alloc_coherent on the SCM platform device, whose masks default to 32 bits
 * (drivers/firmware/qcom/qcom_scm.c, setup_pdev_dma_masks), so upstream's metadata always lands below
 * 4 GiB or the allocation fails outright. On this board it failed: the mainline fuxi port saw
 * modem_dtb's PAS init return -12 and fixed it with CONFIG_ZONE_DMA and CONFIG_ZONE_DMA32, whose only
 * effect is to let the coherent allocation land below 4 GiB.
 *
 * Why a mask cannot do it here. XNU's IOBufferMemoryDescriptor::initWithPhysicalMask turns the mask
 * into a highestMask and an alignment, and IOKernelAllocateWithPhysicalRestrict drops any maximum
 * physical address of 0xFFFFFFFF or below to zero and substitutes KMA_LOMEM instead. On arm64
 * vm_lopage_needed is false (only i386 sets it, osfmk/i386/i386_vm_init.c), so vm_page_grablo falls
 * through to a plain vm_page_grab and the low-memory request is ignored. That is why the audio chain's
 * 32-bit mask produced a physical address of 0x8CFF84000, and why the modem's metadata landed at
 * 0x8d39ab000: both above 4 GiB.
 *
 * Why a fixed window works. IODeviceMemory::withRange(address, bytes)->map(kIOMapAnywhere) maps a
 * chosen physical address, and the audio chain already proved that recipe on this board
 * (package/FuxiAdspProbe.cpp, probeOwnedPcmSmmu, 0xd0000000 of 64 KiB).
 *
 * Why this window. The address has to be below 4 GiB, page aligned, outside every region the device
 * tree reserves, outside the regions the port itself maps, and outside XNU's own allocator. XNU's
 * allocator holds only the kernel's window, which DarwinBoot places at 0x8c0000000 (the device log
 * of 2026-11-19 records the kernel's physical address as 0x8C007C000), so everything below 4 GiB is
 * untouched by the kernel's page allocator and a fixed mapping there cannot alias with it.
 *
 * 0x81480000 is the first free byte of the board's own DRAM row that follows the xbl_ramdump_region
 * (0x81200000 of 0x280000), and the device tree's reserved-memory list puts nothing between that
 * region's end and 0x81a00000 (the xbl_dt_log_merged region). The row it belongs to runs to
 * 0x81980000, so 0x3000 bytes of it fit with room to spare.
 *
 * This is pure data and pure logic with no kernel dependency, so the host test checks the window's
 * geometry and its distance from every region it must not touch.
 */
#ifndef FUXI_MODEM_LOW_META_H
#define FUXI_MODEM_LOW_META_H

#include <stdint.h>

#include "FuxiModemAssign.h"

/* The page size the metadata has to be aligned to. Linux says so in
   drivers/firmware/qcom/qcom_scm.c, qcom_scm_pas_init_image: "make sure it's physically contiguous,
   4K aligned and non-cachable to avoid XPU violations". */
#define FUXI_MODEM_LOW_META_PAGE 0x1000ull

/* The window. The device tree blob's metadata takes the first page and the modem image's metadata the
   two pages after it, because both are alive at the same time: Linux keeps both metadata buffers
   until the modem has come up (drivers/remoteproc/qcom_q6v5_pas.c, qcom_pas_start releases them
   after qcom_q6v5_wait_for_start), and so does this port. */
#define FUXI_MODEM_LOW_META_BASE 0x81480000ull
#define FUXI_MODEM_LOW_META_SPAN 0x3000ull

/* Which of the two metadata buffers. */
enum {
    FuxiModemLowMetaDtb = 0,
    FuxiModemLowMetaImage = 1,
};

/* The device tree blob's metadata is 4044 bytes, which is one page, and the modem image's is 6364
   bytes, which is two. */
#define FUXI_MODEM_LOW_META_DTB_OFFSET 0x0000ull
#define FUXI_MODEM_LOW_META_DTB_SPAN 0x1000ull
#define FUXI_MODEM_LOW_META_IMAGE_OFFSET 0x1000ull
#define FUXI_MODEM_LOW_META_IMAGE_SPAN 0x2000ull

/* The regions the window must not touch. Each one is a region the device tree reserves or the port
   itself maps, and the modem's metadata landing in any of them would corrupt it or the firmware.

   The first two are the ones the window sits between: the xbl_ramdump_region ends exactly where the
   window starts, and the xbl_dt_log_merged region starts after the window ends. The rest are the
   modem's own carveouts, the region the assign hands over, and the windows the port maps itself. */
struct FuxiModemLowMetaRange {
    uint64_t base;
    uint64_t size;
    const char *name;
};

#define FUXI_MODEM_LOW_META_FORBIDDEN_COUNT 10u

static inline struct FuxiModemLowMetaRange fuxi_modem_low_meta_forbidden(unsigned index)
{
    static const struct FuxiModemLowMetaRange ranges[FUXI_MODEM_LOW_META_FORBIDDEN_COUNT] = {
        {0x81200000ull, 0x280000ull, "the xbl ramdump region"},
        {0x81a00000ull, 0x260000ull, "the xbl device tree log"},
        {0x81c60000ull, 0x20000ull, "the command database"},
        {0x81d00000ull, 0x200000ull, "the shared memory window"},
        {0x8a800000ull, 0x10800000ull, "the modem's own memory"},
        {0x9b000000ull, 0x80000ull, "the modem's device tree blob"},
        {FUXI_MODEM_DSM_BASE, FUXI_MODEM_DSM_SIZE, "the DSM region the assign hands over"},
        {0xd0000000ull, 0x10000ull, "the audio chain's low window"},
        {0x00408000ull, 0x10000ull, "the IPCC window"},
        {0x00c30000ull, 0x10000ull, "the AOSS window"},
    };
    return ranges[index];
}

/* Whether two ranges overlap. */
static inline int fuxi_modem_low_meta_overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return (a < b + blen) && (b < a + alen);
}

/* Where a metadata buffer starts inside the window, and how much room it has. */
static inline uint64_t fuxi_modem_low_meta_offset(unsigned which)
{
    return which == FuxiModemLowMetaDtb ? FUXI_MODEM_LOW_META_DTB_OFFSET : FUXI_MODEM_LOW_META_IMAGE_OFFSET;
}

static inline uint64_t fuxi_modem_low_meta_span(unsigned which)
{
    return which == FuxiModemLowMetaDtb ? FUXI_MODEM_LOW_META_DTB_SPAN : FUXI_MODEM_LOW_META_IMAGE_SPAN;
}

/* The physical address of a metadata buffer. */
static inline uint64_t fuxi_modem_low_meta_address(unsigned which)
{
    return FUXI_MODEM_LOW_META_BASE + fuxi_modem_low_meta_offset(which);
}

/* The bytes a metadata buffer of this size occupies, rounded up to a page: the secure world turns on
   memory protection for the pages the buffer covers, so the pages are what has to fit. */
static inline uint64_t fuxi_modem_low_meta_occupied(uint64_t bytes)
{
    return (bytes + FUXI_MODEM_LOW_META_PAGE - 1) & ~(FUXI_MODEM_LOW_META_PAGE - 1);
}

/* Whether a metadata of this size fits in the slot. */
static inline int fuxi_modem_low_meta_fits(unsigned which, uint64_t bytes)
{
    return fuxi_modem_low_meta_occupied(bytes) <= fuxi_modem_low_meta_span(which);
}

#endif
