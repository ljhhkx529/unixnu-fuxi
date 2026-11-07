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
extern int snprintf(char *str, unsigned long size, const char *format, ...);
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
/* Per-ABI-method calls and last status: [method][0] calls, [1] last status, [2] last input size, [3] last output size
 * (Adreno740 NativeAdrenoPublish.cpp). */
extern volatile unsigned int native_adreno_method_stats[20][4];
/* The fence wait's own view: [0] fence value waited for polled outside the lock, [1] ring words read, [2] polls,
 * [3] ??, [4] polls that saw the fence still pending (Adreno740 NativeAdrenoPublish.cpp). */
extern volatile unsigned long long native_adreno_wait_outer[4];
extern volatile unsigned int native_adreno_fence_wait[8];
/* qcom_scm_set_gpu_smmu_aperture(0): [0] availability answer, [1] the call's x0 (0 or a negative SMC status). */
extern volatile unsigned int native_adreno_cp_aperture[4];
/* checkCpSwitch's steps (createVm, probeBuffer, submit, waitAll, readTtbr0; 0 pass, ffffffff not reached) and the
 * table switching the driver settled on ([6] the setTableSwitching result, [7] the mode in use). */
extern volatile unsigned int native_adreno_cp_check[6];
/* The bank's image around the switch check: [0..7] before, [8..15] after, [16]/[17] the kit status of each,
 * [18..20] a fault the switch left latched (FAR lo/hi, FSYNR0), [21] the status after clearing it. */
extern volatile unsigned int native_adreno_cp_image[22];
extern volatile unsigned int native_adreno_table_switches[8];
/* The process table createVm built, read back through the VM: [0] a buffer whose spans are not all mapped,
 * [1] its index, [2] the status, [3] how many were checked, [4]/[5] the ring's IOVA and bytes there. */
/* The framed prologue as written into the ring (Adreno740 NativeAdrenoRing.cpp emitPrologue):
 * [0] words, [1] reset, [2] table, [3] stamps, then the words. */
extern volatile unsigned int native_adreno_vm_check[8];
/* The first 28 dwords of the client's newest IB (Adreno740 NativeAdrenoSubmit.cpp): [0] fence, [1]/[2] iova,
 * [3] bytes, then the words. */
extern volatile unsigned int native_adreno_ib_log[4][4+28];
extern volatile unsigned int native_adreno_ib_log_count;
extern volatile unsigned int native_adreno_ib_peek;
extern volatile unsigned int native_adreno_prologue[96];
extern volatile unsigned int native_adreno_prologue_info[4];
/* The fence event ring (Adreno740 NativeAdrenoSubmit.cpp): type 1 submit, 2 wait-enter, 3 wait-complete, 4 wait-timeout. */
extern volatile unsigned int native_adreno_fence_events[32][4];
extern volatile unsigned int native_adreno_fence_events_count;
/* The CP stall snapshot Adreno740 keeps when a fence wait times out (NativeAdrenoSubmit.cpp): the ring words, the CP's
 * own state and the last command it fetched. */
extern volatile unsigned int native_adreno_stall[29];
extern int native_adreno_fences(unsigned int *submitted, unsigned int *retired);
extern volatile unsigned long long native_gop_wait_stats[4];
void fuxi_display_client_report(void) {
    printf("fuxi-display: gop_wait calls %llu timeouts %llu last_ms %llu\n",
           native_gop_wait_stats[0], native_gop_wait_stats[1], native_gop_wait_stats[2]);
    {
        unsigned int s = 0, r = 0;
        const int live = native_adreno_fences(&s, &r);
        printf("fuxi-display: late_fence live %d submitted %u retired %u\n", live, s, r);
    }
    struct FuxiMetalDiagFields *m = &native_adreno_metal_diag;
    printf("fuxi-metal: opens %u busy %u bad %u foreign %u/%u name_reads %u reader %s starter %s\n",
           m->opens, m->busy, m->bad, m->foreign_opens, m->foreign_type, m->name_reads,
           m->name_reader[0] ? m->name_reader : "(none)", m->starter[0][0] ? m->starter[0] : "(none)");
    {
        char line[240]; int n = 0;
        for (unsigned i = 0; i < 20; ++i) {
            if (!native_adreno_method_stats[i][0]) continue;
            n += snprintf(line + n, sizeof line - n, " m%u=%u/%u/%x", i, native_adreno_method_stats[i][0],
                          native_adreno_method_stats[i][1], native_adreno_method_stats[i][2]);
            if (n > 200) break;
        }
        printf("fuxi-metal: methods%s\n", n ? line : " (none)");
        {
            const unsigned total = native_adreno_fence_events_count;
            char line[260]; int n = 0;
            const unsigned first = total > 12 ? total - 12 : 0;
            for (unsigned i = first; i < total && n < 230; ++i) {
                volatile unsigned int *e = native_adreno_fence_events[i & 31];
                n += snprintf(line + n, sizeof line - n, " t%u/f%u", e[0], e[1]);
            }
            printf("fuxi-metal: fence_events count %u%s\n", total, n ? line : " (none)");
        }
        {
            char line[300]; int n = 0;
            for (unsigned i = 0; i < 29 && n < 270; ++i)
                n += snprintf(line + n, sizeof line - n, " %x", native_adreno_stall[i]);
            printf("fuxi-metal: stall%s\n", n ? line : " (none)");
        }
        printf("fuxi-metal: cp_aperture avail %x rc %x check %x mode %u\n",
               native_adreno_cp_aperture[0], native_adreno_cp_aperture[1],
               native_adreno_cp_aperture[2], native_adreno_cp_aperture[3]);
        printf("fuxi-metal: cp_image before %x %x %x %x %x %x %x %x kit %x\n",
               native_adreno_cp_image[0], native_adreno_cp_image[1], native_adreno_cp_image[2],
               native_adreno_cp_image[3], native_adreno_cp_image[4], native_adreno_cp_image[5],
               native_adreno_cp_image[6], native_adreno_cp_image[7], native_adreno_cp_image[16]);
        printf("fuxi-metal: cp_image after %x %x %x %x %x %x %x %x kit %x fault %x,%x,%x clr %x\n",
               native_adreno_cp_image[8], native_adreno_cp_image[9], native_adreno_cp_image[10],
               native_adreno_cp_image[11], native_adreno_cp_image[12], native_adreno_cp_image[13],
               native_adreno_cp_image[14], native_adreno_cp_image[15], native_adreno_cp_image[17],
               native_adreno_cp_image[18], native_adreno_cp_image[19], native_adreno_cp_image[20],
               native_adreno_cp_image[21]);
        /* [0] CP_SMMU_TABLE_UPDATE packets the ring emitted, [2]/[3] VMs created/destroyed, [4]/[5] live now/peak,
         * [6] the last table error, [7] createVm's step (0 on success). */
        {
            const unsigned total = native_adreno_ib_log_count;
            if (total) {
                volatile unsigned int *e = native_adreno_ib_log[(total - 1) & 3];
                printf("fuxi-metal: client_ib fence %u iova %x%08x bytes %u peek %x\n", e[0], e[2], e[1], e[3],
                       native_adreno_ib_peek);
                for (unsigned i = 0; i < 28; i += 8) {
                    char line[160]; int m = 0;
                    for (unsigned j = i; j < i + 8 && j < 28; ++j)
                        m += snprintf(line + m, sizeof line - m, " %08x", e[4 + j]);
                    printf("fuxi-metal:   ib+%02u%s\n", i, line);
                }
            }
        }
        {
            const unsigned n = native_adreno_prologue_info[0];
            printf("fuxi-metal: prologue words %u reset %u table %u stamps %u\n",
                   n, native_adreno_prologue_info[1], native_adreno_prologue_info[2], native_adreno_prologue_info[3]);
            for (unsigned i = 0; i < n && i < 96; i += 8) {
                char line[160]; int m = 0;
                for (unsigned j = i; j < i + 8 && j < n; ++j)
                    m += snprintf(line + m, sizeof line - m, " %08x", native_adreno_prologue[j]);
                printf("fuxi-metal:   +%02u%s\n", i, line);
            }
        }
        printf("fuxi-metal: vm_check fail %u at %u st %x checked %u ring_iova %x ring_bytes %x\n",
               native_adreno_vm_check[0], native_adreno_vm_check[1], native_adreno_vm_check[2],
               native_adreno_vm_check[3], native_adreno_vm_check[4], native_adreno_vm_check[5]);
        printf("fuxi-metal: tbl_switches %u %u %u %u %u %u %u %u\n",
               native_adreno_table_switches[0], native_adreno_table_switches[1],
               native_adreno_table_switches[2], native_adreno_table_switches[3],
               native_adreno_table_switches[4], native_adreno_table_switches[5],
               native_adreno_table_switches[6], native_adreno_table_switches[7]);
        printf("fuxi-metal: cp_check %x/%x/%x/%x/%x/%x switching %x/%x\n",
               native_adreno_cp_check[0], native_adreno_cp_check[1], native_adreno_cp_check[2],
               native_adreno_cp_check[3], native_adreno_cp_check[4], native_adreno_cp_check[5],
               native_adreno_table_switches[6], native_adreno_table_switches[7]);
        printf("fuxi-metal: wait_outer %llu/%llu/%llu/%llu fence_wait %u/%u/%u/%u %u/%u/%u/%u\n",
               native_adreno_wait_outer[0], native_adreno_wait_outer[1], native_adreno_wait_outer[2],
               native_adreno_wait_outer[3], native_adreno_fence_wait[0], native_adreno_fence_wait[1],
               native_adreno_fence_wait[2], native_adreno_fence_wait[3], native_adreno_fence_wait[4],
               native_adreno_fence_wait[5], native_adreno_fence_wait[6], native_adreno_fence_wait[7]);
    }
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
