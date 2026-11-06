/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Board backends the display driver of GopFramebuffer calls into, for the fuxi bring-up.
 *
 * Most of what this driver calls is bound straight to the original family: package/
 * FuxiLegacyDisplayProbe.cpp publishes the display and owns the swap rings, and build-fuxi-display.py
 * records thirteen further bindings (the surface getters, the vsync sender, the refresh-properties body
 * and the class constructor, destructor, metaclass and vtable) after checking each address against the
 * candidate. Nothing here duplicates those.
 *
 * What is left are the two subsystems fuxi has no driver for yet. Both are weak, so a real
 * implementation takes them over without touching this file.
 *
 *   native_adreno_fences   Adreno740's GPU fence counters. The driver asks whether a hand-off is still
 *                          outstanding; answering false keeps every frame on the CPU copy path, which is
 *                          correct while there is no GPU driver to complete them.
 *   native_ufs_dpu_*       The direct-DPU scanout path. Verbatim from unixnu-fuxi-old package/
 *                          FuxiDpuScanoutStubs.c: this board's NativeUfs backend does not expose it, so
 *                          it stays disabled rather than pretending to map pages.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* native_legacy_vsync_notify is not defined here: it binds to the original family's own sender, which
 * scripts/build-fuxi-display.py records with the other twelve bindings after checking each address
 * against the candidate. A second definition here would shadow the original code. */

/* Event counters and the last interval the driver reported, as sheng's probe keeps them. */
/* Kept for the diagnostic step that reads them; nothing formats them yet, so they stay private and
 * the file needs no kernel string routines (they clash with stdint under the C compile record). */


/* The GPU fence counters of Adreno740, which is not ported yet. The driver declares this weak_import and
 * asks whether a GPU hand-off is still outstanding: answering false keeps every frame on the CPU copy
 * path, which is the correct behaviour while there is no GPU driver to complete them. Unused today
 * because sheng-fb-gpu is off; it becomes a real query when Adreno740 lands. */
__attribute__((weak)) bool native_adreno_fences(uint32_t *submitted, uint32_t *retired) {
    (void)submitted;
    (void)retired;
    return false;
}

/* Verbatim from unixnu-fuxi-old package/FuxiDpuScanoutStubs.c. */
__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count) {
    (void)slot;
    (void)pages;
    (void)count;
    return 0;
}

__attribute__((weak)) volatile uint32_t native_ufs_dpu_state = 0;
