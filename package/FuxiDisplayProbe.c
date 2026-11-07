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

/* Who is using the display and the GPU hand-off. The framebuffer's own user-client hook counts every open and
 * records the pid and the client class, and the presenter counts the frames it accepts from the display. A
 * presenter offer needs a user-space client (NativeGOPPresenter::gpuClient), so a non-zero offer count is the
 * proof that the Metal side connected. */
extern volatile unsigned int native_legacy_client_calls;
extern volatile int native_legacy_client_pid;
extern char native_legacy_client_class[64];
extern volatile unsigned int native_legacy_frame_count;
extern volatile unsigned long long native_gop_gpu_offers, native_gop_gpu_completions;
extern volatile unsigned long long native_gop_gpu_failures, native_gop_gpu_abandoned;
/* Adreno740's own view of the Metal discovery: who opened the accelerator, who read MetalPluginName, which
 * processes the plugin marker came from, and the plugin's last result line. */
struct FuxiMetalDiagFields {
    unsigned int opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type,
        name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out,
        tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16], chk_macf, chk_res,
        chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];
    char reader[3][17];
    char name_reader[17];
    char starter[3][17];
    char report[64];
    char report2[64];
    unsigned int grants;
};
extern struct FuxiMetalDiagFields native_adreno_metal_diag;
/* Where a client's mount() stopped: [0] 1 no process space / 2 open refused / 3 scope refused, [1] its IOReturn,
 * [2] the range base's high half, [3] how many times (Adreno740 NativeAdrenoPublish.cpp). */
extern volatile unsigned int native_adreno_mount_fail[4];
void fuxi_display_client_report(void) {
    struct FuxiMetalDiagFields *m = &native_adreno_metal_diag;
    printf("fuxi-metal: opens %u busy %u bad %u foreign %u/%u name_reads %u reader %s starter %s\n",
           m->opens, m->busy, m->bad, m->foreign_opens, m->foreign_type, m->name_reads,
           m->name_reader[0] ? m->name_reader : "(none)", m->starter[0][0] ? m->starter[0] : "(none)");
    printf("fuxi-metal: mount_fail %u rc %x base %x count %u\n",
           native_adreno_mount_fail[0], native_adreno_mount_fail[1],
           native_adreno_mount_fail[2], native_adreno_mount_fail[3]);
    printf("fuxi-metal: calls %u sel %u st %x ext %u/%u/%x probes %u marks %u macf %u res %u type %u pass %u/%u attach %x inflight %u grants %u report %s\n",
           m->calls, m->last_selector, m->last_status, m->ext_entries, m->ext_selector, m->ext_status,
           m->probes, m->marks_total, m->chk_macf, m->chk_res, m->chk_type, m->type_pass_lo, m->type_pass_hi,
           m->attach_status, m->inflight, m->grants, m->report[0] ? m->report : "(none)");
    printf("fuxi-display: clients %u pid %d class %s frames %u gop offers %llu done %llu fail %llu abandon %llu\n",
           native_legacy_client_calls, native_legacy_client_pid,
           native_legacy_client_class[0] ? native_legacy_client_class : "(none)",
           native_legacy_frame_count, native_gop_gpu_offers, native_gop_gpu_completions,
           native_gop_gpu_failures, native_gop_gpu_abandoned);
}
