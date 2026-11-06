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

/* The GPU addon the loader places after the trust caches. DarwinBoot reads kernel.macho.gpuaddon and writes
 * its address and length into /chosen/memory-map/GpuAddon; this reports what the kernel actually sees, so a
 * missing handover is distinguishable from a refused addon. */
extern int native_ram_block_gpu_addon(const unsigned char **data, unsigned long long *bytes);
extern int printf(const char *format, ...);
extern volatile int native_gpu_addon_stage, native_gpu_addon_status;
extern volatile int native_gpu_addon2_stage, native_gpu_addon2_status;
extern volatile int native_gpu_addon3_stage, native_gpu_addon3_status;
extern volatile int native_gpu_addon_files, native_gpu_addon_written;
extern volatile int native_gpu_addon_same, native_gpu_addon_failed, native_gpu_addon_error;
void fuxi_gpu_addon_report(void) {
    const unsigned char *data = 0;
    unsigned long long bytes = 0;
    int found = native_ram_block_gpu_addon(&data, &bytes);
    printf("fuxi-gpu-addon: handover %d, bytes %llu, install %d/%d/%d err %d, mount %d/%d %d/%d %d/%d\n",
           found, bytes, native_gpu_addon_files, native_gpu_addon_written, native_gpu_addon_same,
           native_gpu_addon_error, native_gpu_addon_stage, native_gpu_addon_status,
           native_gpu_addon2_stage, native_gpu_addon2_status, native_gpu_addon3_stage, native_gpu_addon3_status);
}

/* The GPU power probe publishes its progress in these counters (package/FuxiGpuPowerProbe.cpp); its rows
 * go to the boot-progress page, not the serial log, so a stall there is otherwise invisible. */
extern volatile uint32_t native_gpu_power_stage;
extern volatile uint32_t native_gpu_power_result;
extern volatile uint32_t native_gpu_perf_vote[2];
void fuxi_gpu_power_report(void) {
    printf("fuxi-gpu-power: stage %x result %x vote %u/%u\n",
           native_gpu_power_stage, native_gpu_power_result,
           native_gpu_perf_vote[0], native_gpu_perf_vote[1]);
}
