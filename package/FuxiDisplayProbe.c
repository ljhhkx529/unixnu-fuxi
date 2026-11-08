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
extern volatile unsigned int native_adreno_mark_log[32];
extern volatile unsigned int native_adreno_mark_log_count;
extern volatile unsigned int native_adreno_vm_check[8];
/* The first 28 dwords of the client's newest IB (Adreno740 NativeAdrenoSubmit.cpp): [0] fence, [1]/[2] iova,
 * [3] bytes, then the words. */
extern volatile unsigned int native_adreno_ib_log[4][4+256];
extern volatile unsigned int native_adreno_ib_log_count;
extern volatile unsigned int native_adreno_ib_peek;
extern volatile unsigned int native_adreno_bv_patched[4];
extern volatile unsigned int native_adreno_bv_seen[8];
extern volatile unsigned int native_adreno_wc_audit[6];
extern volatile unsigned int native_adreno_wc_distrusted;
extern volatile unsigned int native_adreno_ib_ind[16];
extern volatile unsigned int native_adreno_empty_ib[4];
extern void native_gpu_ib_state(unsigned int *out);
extern volatile unsigned int native_adreno_amble[16];
extern volatile unsigned int native_adreno_ib_bufs[8][10];
extern volatile unsigned int native_adreno_ib_bufs_count;
extern volatile unsigned int native_adreno_ib_hist[16][6];
/* The touch probe's own report row and where its thread is (FuxiTouchProbe.cpp): the row is refreshed by the probe, so
 * reading it here costs the touch thread nothing. */
extern char native_touch_rows[4][120];
extern volatile unsigned int native_touch_loop_step, native_touch_step;
extern volatile unsigned int native_touch_irq_stats[4];
extern volatile int native_touch_error;
extern volatile unsigned int native_adreno_wait_mem[4];
extern volatile unsigned int native_adreno_patch_walk[6];
extern volatile unsigned int native_adreno_patch_target[52];
extern volatile unsigned int native_adreno_prologue[96];
extern volatile unsigned int native_adreno_prologue_info[4];
/* The fence event ring (Adreno740 NativeAdrenoSubmit.cpp): type 1 submit, 2 wait-enter, 3 wait-complete, 4 wait-timeout. */
extern volatile unsigned int native_adreno_fence_events[32][4];
extern volatile unsigned int native_adreno_fence_events_count;
/* The CP stall snapshot Adreno740 keeps when a fence wait times out (NativeAdrenoSubmit.cpp): the ring words, the CP's
 * own state and the last command it fetched. */
extern volatile unsigned int native_adreno_stall[29];
extern volatile unsigned int native_adreno_stall_mem[59];
extern volatile unsigned int native_adreno_ring_log[4][4 + 112];
extern volatile unsigned int native_adreno_ring_log_count;
extern int native_adreno_fences(unsigned int *submitted, unsigned int *retired);
extern volatile unsigned long long native_gop_wait_stats[4];
/* What the display and the GPU hand-off have done, for the boot log. One line per counter so a failing path
 * is named rather than inferred. */
void fuxi_display_client_report(void) {
    unsigned int submitted = 0, retired = 0;
    const int live = native_adreno_fences(&submitted, &retired);
    printf("fuxi-display: fence live %d submitted %u retired %u\n", live, submitted, retired);
    printf("fuxi-touch: loop %u step %u err %d irq %u/%u\n%s\n", native_touch_loop_step, native_touch_step,
           native_touch_error, native_touch_irq_stats[1], native_touch_irq_stats[2], native_touch_rows[3]);
    printf("fuxi-display: clients %u pid %d frames %u\n",
           native_legacy_client_calls, native_legacy_client_pid, native_legacy_frame_count);
    printf("fuxi-display: gop offers %llu done %llu fail %llu abandon %llu\n",
           native_gop_gpu_offers, native_gop_gpu_completions, native_gop_gpu_failures, native_gop_gpu_abandoned);
    {
        unsigned int ib[28] = {0};
        native_gpu_ib_state(ib);
        printf("fuxi-metal: audit wait_mem seen %u (the runtime must emit none)\n", native_adreno_wait_mem[0]);
    printf("fuxi-metal: wc audit n%u lines%u dirty%u stale%u last%x rewc%u distrusted%u\n",
           native_adreno_wc_audit[0], native_adreno_wc_audit[1], native_adreno_wc_audit[2],
           native_adreno_wc_audit[3], native_adreno_wc_audit[4], native_adreno_wc_audit[5],
           native_adreno_wc_distrusted);
        printf("fuxi-metal: patch_walk streams %u links %u skipped %u bounded %u tctrl %u changed %u\n",
               native_adreno_patch_walk[0], native_adreno_patch_walk[1],
               native_adreno_patch_walk[2], native_adreno_patch_walk[3],
               native_adreno_patch_walk[4], native_adreno_patch_walk[5]);
        if (native_adreno_patch_target[2]) {
            printf("fuxi-metal: patch_target %x%08x words %u peek %x:", native_adreno_patch_target[1],
                   native_adreno_patch_target[0], native_adreno_patch_target[2], native_adreno_patch_target[3]);
            for (unsigned base = 0; base < native_adreno_patch_target[2] && base < 48; base += 12) {
                printf("\nfuxi-metal:   target+%u:", base);
                for (unsigned k = base; k < base + 12 && k < native_adreno_patch_target[2] && k < 48; ++k)
                    printf(" %x", native_adreno_patch_target[4 + k]);
            }
            printf("\n");
        }
    printf("fuxi-display: cp ib1 %x%08x rem %x ib2 %x%08x rem %x sds %x%08x rem %x\n",
               ib[1], ib[0], ib[2], ib[4], ib[3], ib[5], ib[7], ib[6], ib[8]);
        printf("fuxi-display: cp mrb %x%08x rem %x vsd %x wptr %x rptr %x\n",
               ib[10], ib[9], ib[11], ib[12], ib[13], ib[14]);
        printf("fuxi-display: cp status %x %x %x %x fault %x status1 %x protect %x\n",
               ib[15], ib[16], ib[17], ib[18], ib[19], ib[20], ib[21]);
        printf("fuxi-display: cp roq rb %x ib1 %x ib2 %x sds %x mrb %x vsd %x\n",
               ib[24], ib[22], ib[23], ib[25], ib[26], ib[27]);
        if (native_adreno_ring_log_count) {
            volatile unsigned int *r = native_adreno_ring_log[(native_adreno_ring_log_count - 1) & 3];
            printf("fuxi-display: ring f%u %x..%x n%u:", r[0], r[1], r[2], r[3]);
            for (unsigned k = 0; k < r[3] && k < 16; ++k) printf(" %x", r[4 + k]);
            printf("\n");
        }
        if (native_adreno_stall[0]) {
            volatile unsigned int *s = native_adreno_stall;
            volatile unsigned int *m = native_adreno_stall_mem;
            printf("fuxi-display: stall f%u waits %u rptr %x wptr %x ib1 %x%08x rem %x ib2 %x%08x rem %x\n",
                   s[0], s[1], s[10], s[11], s[13], s[12], s[14], s[16], s[15], s[17]);
            printf("fuxi-display: stall fault %x %x %x %x br_at %x:", m[0], m[1], m[2], m[3], m[4]);
            for (unsigned k = 0; k < 8; ++k) printf(" %x", m[5 + k]);
            printf("\n");
            printf("fuxi-display: stall bv_at %x:", m[21]);
            for (unsigned k = 0; k < 8; ++k) printf(" %x", m[22 + k]);
            printf("\n");
        }
    }
    printf("fuxi-display: gop_wait calls %llu timeouts %llu last_ms %llu\n",
           native_gop_wait_stats[0], native_gop_wait_stats[1], native_gop_wait_stats[2]);
    {
        struct FuxiMetalDiagFields *d = &native_adreno_metal_diag;
        printf("fuxi-metal: opens %u busy %u bad %u name_reads %u reader %s starter %s\n",
               d->opens, d->busy, d->bad, d->name_reads,
               d->name_reader[0] ? d->name_reader : "(none)", d->starter[0][0] ? d->starter[0] : "(none)");
        printf("fuxi-metal: client calls %u last_sel %x last_st %x tunnel %u attach %x inflight %u\n",
               d->calls, d->last_selector, d->last_status, d->tunnel_calls, d->attach_status, d->inflight);
        printf("fuxi-metal: checks macf %x res %x type %x pass %x..%x probes %u mb %u/%u\n",
               d->chk_macf, d->chk_res, d->chk_type, d->type_pass_lo, d->type_pass_hi, d->probes,
               d->mb_calls, d->mb_maps);
    }
    {
        struct FuxiMetalDiagFields *d = &native_adreno_metal_diag;
        {
            const unsigned total = native_adreno_mark_log_count;
            printf("fuxi-metal: marklog %u:", total);
            const unsigned from = total > 32 ? total - 32 : 0;
            for (unsigned k = from; k < total; ++k) printf(" %x", native_adreno_mark_log[k & 31]);
            printf("\n");
        }
        printf("fuxi-metal: scanout marks %u:", d->marks_total);
        for (unsigned k = 0; k < 16; ++k) printf(" %x", d->marks[k]);
        printf("\n");
        printf("fuxi-metal: scanout text %s\n", d->report2[0] ? d->report2 : "(none)");
    }
    {
        const unsigned total = native_adreno_ib_log_count;
        if (total) {
            volatile unsigned int *e = native_adreno_ib_log[(total - 1) & 3];
            printf("fuxi-metal: client_ib fence %u iova %x%08x bytes %u peek %x\n",
                   e[0], e[2], e[1], e[3], native_adreno_ib_peek);
            for (unsigned k = 0; k < 256; k += 16) {
                char ibl[160]; unsigned il = 0;
                for (unsigned j = k; j < k + 16; ++j) il += (unsigned)snprintf(ibl + il, sizeof ibl - il, " %08x", e[4 + j]);
                printf("fuxi-metal: ib+%03u%s\n", k, ibl);
            }
        }
    }
    printf("fuxi-metal: ib_ind n %u peek %x addr %x%08x size %u words %x %x %x %x %x %x %x %x\n",
           native_adreno_ib_ind[0], native_adreno_ib_ind[1], native_adreno_ib_ind[3], native_adreno_ib_ind[2],
           native_adreno_ib_ind[4], native_adreno_ib_ind[5], native_adreno_ib_ind[6], native_adreno_ib_ind[7],
           native_adreno_ib_ind[8], native_adreno_ib_ind[9], native_adreno_ib_ind[10], native_adreno_ib_ind[11],
           native_adreno_ib_ind[12]);
    printf("fuxi-metal: ib_ind sample st %x cached %x dram %x\n",
           native_adreno_ib_ind[13], native_adreno_ib_ind[14], native_adreno_ib_ind[15]);
    {
        unsigned int submitted = 0, retired = 0;
        (void)native_adreno_fences(&submitted, &retired);
        printf("fuxi-metal: slots submitted %u retired %u\n", submitted, retired);
        for (unsigned s = 0; s < 4; ++s) {
            volatile unsigned int *e = native_adreno_ib_log[s];
            if (!e[0]) continue;
            printf("fuxi-metal:   slot %u fence %u%s iova %x%08x bytes %u head %x %x %x %x %x %x %x %x\n",
                   s, e[0], e[0] > retired ? " LIVE" : "", e[2], e[1], e[3],
                   e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11]);
        }
    }
    printf("fuxi-metal: ib_bufs %u\n", native_adreno_ib_bufs_count);
    for (unsigned b = 0; b < native_adreno_ib_bufs_count && b < 8; ++b)
        printf("fuxi-metal:   b%u addr %x%08x bytes %u peek %x head %x %x %x %x %x %x %x\n", b,
               native_adreno_ib_bufs[b][1], native_adreno_ib_bufs[b][0], native_adreno_ib_bufs[b][2],
               native_adreno_ib_bufs[b][9], native_adreno_ib_bufs[b][3], native_adreno_ib_bufs[b][4],
               native_adreno_ib_bufs[b][5], native_adreno_ib_bufs[b][6], native_adreno_ib_bufs[b][7],
               native_adreno_ib_bufs[b][8]);
    for (unsigned h = 0; h < 16; ++h) {
        if (!native_adreno_ib_hist[h][0]) continue;
        printf("fuxi-metal: hist f%u ambles %u addr %x%08x peek %x first %x\n",
               native_adreno_ib_hist[h][0], native_adreno_ib_hist[h][1], native_adreno_ib_hist[h][3],
               native_adreno_ib_hist[h][2], native_adreno_ib_hist[h][4], native_adreno_ib_hist[h][5]);
    }
    printf("fuxi-metal: amble n %u peek %x addr %x%08x words %x %x %x %x %x %x %x %x\n",
           native_adreno_amble[0], native_adreno_amble[1], native_adreno_amble[3], native_adreno_amble[2],
           native_adreno_amble[4], native_adreno_amble[5], native_adreno_amble[6], native_adreno_amble[7],
           native_adreno_amble[8], native_adreno_amble[9], native_adreno_amble[10], native_adreno_amble[11]);
    printf("fuxi-metal: empty_ib dropped %u last %x\n", native_adreno_empty_ib[0], native_adreno_empty_ib[1]);
    printf("fuxi-metal: bv_seen sel3 %u sel2 %u sel1_nobin %u other %u last %x %x %x %x\n",
           native_adreno_bv_seen[0], native_adreno_bv_seen[1], native_adreno_bv_seen[2], native_adreno_bv_seen[3],
           native_adreno_bv_seen[4], native_adreno_bv_seen[5], native_adreno_bv_seen[6], native_adreno_bv_seen[7]);
    printf("fuxi-metal: bv_patch ok %u last_bytes %u refused %u back %x\n",
           native_adreno_bv_patched[0], native_adreno_bv_patched[1], native_adreno_bv_patched[2],
           native_adreno_bv_patched[3]);
    {
        char methodLine[200]; unsigned used = 0;
        for (unsigned k = 0; k < 20; ++k) {
            if (!native_adreno_method_stats[k][0] || used > 160) continue;
            used += (unsigned)snprintf(methodLine + used, sizeof methodLine - used, " m%u=%u/%u",
                                       k, native_adreno_method_stats[k][0], native_adreno_method_stats[k][1]);
        }
        printf("fuxi-metal: methods%s\n", used ? methodLine : " (none)");
    }
}
