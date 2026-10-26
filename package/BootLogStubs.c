// SPDX-License-Identifier: MIT
// Absent-board stubs for package/BootLog.c on fuxi.
//
// BootLog.c (carried over unchanged from sheng) weakly imports every
// subsystem the sheng dashboard can show. On fuxi none of those subsystems is
// built (no legacy display, no Adreno, no touch, no boot-progress page), so
// this file supplies zero/empty definitions for them. BootLog.c guards each
// use with `if (&symbol)` and, for the function pointers, also checks the
// pointer value, so the stubs keep those rows empty instead of crashing.
//
// The writer's own entry point (boot_log_tick) and the NativeUfs log window
// functions come from BootLog.c and drivers/NativeUfs respectively; do not
// stub them here.
#include <stddef.h>
#include <stdint.h>

struct native_adreno_client_record { char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal; };
struct native_adreno_asker_record { char name[20]; uint32_t reads; };

char boot_progress_page_text[48][120];
char boot_progress_banner_text[49][120];
volatile unsigned boot_progress_banner_rows;
char boot_progress_ext[8][48];
char boot_progress_ext_wide[2][112];
char boot_progress_panic_text[256];

volatile uint64_t native_gop_wait_stats[7];
volatile uint32_t native_adreno_fence_wait[8] __attribute__((weak));
volatile uint64_t native_adreno_wait_outer[4] __attribute__((weak));
volatile uint32_t native_adreno_export_stats[4] __attribute__((weak));
volatile int32_t native_touch_strokes[16][7];
volatile uint32_t native_touch_stroke_count;
volatile int32_t native_touch_extent[4];
volatile uint32_t native_adreno_method_stats[20][4] __attribute__((weak));
volatile uint64_t native_adreno_vm_phase_us[2][8] __attribute__((weak));
volatile uint32_t native_adreno_map_fail[7][2] __attribute__((weak));
volatile uint32_t native_adreno_pool_stats[8] __attribute__((weak));
volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak));
volatile uint32_t native_touch_edge_stats[4];
volatile uint64_t native_adreno_reassert_ticks __attribute__((weak));
volatile uint64_t native_adreno_cache_stats[5] __attribute__((weak));
struct native_adreno_client_record native_adreno_client_log[8] __attribute__((weak));
struct native_adreno_asker_record native_adreno_metal_askers[12] __attribute__((weak));
char native_gpu_coherence_row[120];
volatile uint64_t native_gpu_idle_sleeps;
volatile uint64_t native_gpu_idle_spins;
volatile uint32_t native_adreno_coherent_skips[4] __attribute__((weak));
volatile uint32_t native_adreno_io_coherent __attribute__((weak));
char native_cpufreq_row[120];
uint32_t native_gpu_perf_vote[2];
volatile uint32_t native_adreno_pool_status __attribute__((weak));
volatile uint32_t native_adreno_pool_wc __attribute__((weak));
volatile uint64_t native_adreno_fence_phys __attribute__((weak));
int (*native_adreno_dram_read32)(uint64_t, uint32_t *) __attribute__((weak));
int (*native_adreno_cached_read32)(uint64_t, uint32_t *) __attribute__((weak));
volatile uint32_t native_adreno_ring_log[4][4 + 112] __attribute__((weak));
volatile uint32_t native_adreno_ring_log_count __attribute__((weak));
volatile uint32_t native_adreno_ib_log[4][4 + 28] __attribute__((weak));
volatile uint32_t native_adreno_ib_log_count __attribute__((weak));
char native_adreno_metal_rows[48][96] __attribute__((weak));
volatile uint32_t native_adreno_view_diag[6] __attribute__((weak));
volatile uint64_t native_adreno_import_diag[10] __attribute__((weak));
volatile uint32_t native_adreno_import_stage __attribute__((weak));
volatile uint64_t native_adreno_import_fail_pa __attribute__((weak));
volatile uint64_t native_adreno_import_fail_offset __attribute__((weak));

const char *boot_progress_tag(void) { return ""; }
size_t boot_progress_profile_rows(char *out, size_t cap) { (void)out; (void)cap; return 0; }
unsigned boot_ui_trace_role_rows(unsigned role, char rows[][120], unsigned count, unsigned mode)
{ (void)role; (void)rows; (void)count; (void)mode; return 0; }
unsigned boot_ui_trace_hot_rows(char rows[][120], unsigned count)
{ (void)rows; (void)count; return 0; }
unsigned native_legacy_properties_now(char *out, unsigned cap) { (void)out; (void)cap; return 0; }
unsigned native_legacy_clients_dump(char *out, unsigned cap) { (void)out; (void)cap; return 0; }
unsigned native_legacy_swap_timeline(char *out, unsigned cap) { (void)out; (void)cap; return 0; }
unsigned native_legacy_prepare_report(char *out, unsigned cap) { (void)out; (void)cap; return 0; }
unsigned native_legacy_state_words(char *out, unsigned cap) { (void)out; (void)cap; return 0; }
int boot_reset_probe(char *out, unsigned cap) { (void)out; (void)cap; return -1; }
int boot_reset_bootloader(char *out, unsigned cap) { (void)out; (void)cap; return -1; }
