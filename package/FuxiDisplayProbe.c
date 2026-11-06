/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Board backends the display driver of GopFramebuffer calls into, for the fuxi bring-up.
 *
 * UniXNU-sheng supplies these from package/LegacyDisplayProbe.cpp, a 596-line probe that also publishes
 * the display, keeps its own swap rings and reads the panel timing. This board is not there yet: the
 * driver object compiles and its 358-entry vtable matches the original, so this file exists to let the
 * link close with the smallest honest behaviour, and each hook becomes real as its own step lands.
 *
 *   native_legacy_vsync_notify   The platform subclass that raises the vblank. fuxi has no panel
 *                                vblank source wired yet (no DPU scanout, no TE line), so the driver's
 *                                own timer keeps the queue moving and this stays a no-op. It is called
 *                                with the driver's lock held, so it must never sleep or allocate.
 *   native_legacy_vsync_observed Counters only, taken from sheng's probe so the diagnostic rows read
 *                                the same numbers.
 *   native_ufs_dpu_map           Weak stubs, byte for byte from unixnu-fuxi-old package/
 *   native_ufs_dpu_state         FuxiDpuScanoutStubs.c: the NativeUfs backend of this board does not
 *                                expose the direct-DPU scanout path, so it stays disabled rather than
 *                                pretending to map pages.
 */
#include <stdint.h>
#include <stddef.h>

/* Nothing raises the vblank yet; the driver's timer interleaves instead. */
void native_legacy_vsync_notify(void *display, uint64_t timestamp, uint64_t continuousTime,
                                uint64_t interval) {
    (void)display;
    (void)timestamp;
    (void)continuousTime;
    (void)interval;
}

/* Event counters and the last interval the driver reported, as sheng's probe keeps them. */
/* Kept for the diagnostic step that reads them; nothing formats them yet, so they stay private and
 * the file needs no kernel string routines (they clash with stdint under the C compile record). */
static volatile uint64_t vsyncEvents[6];
static volatile unsigned vsyncHz;
__attribute__((unused)) static uint64_t vsyncEventsRead(unsigned event) {
    return event < 6 ? vsyncEvents[event] : 0;
}

void native_legacy_vsync_observed(unsigned event, unsigned hz) {
    if (event < 6) vsyncEvents[event] = vsyncEvents[event] + 1;
    vsyncHz = hz;
}

/* Verbatim from unixnu-fuxi-old package/FuxiDpuScanoutStubs.c. */
__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count) {
    (void)slot;
    (void)pages;
    (void)count;
    return 0;
}

__attribute__((weak)) volatile uint32_t native_ufs_dpu_state = 0;
