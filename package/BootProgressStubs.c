// SPDX-License-Identifier: BSD-2-Clause-Patent
// Zero-initialised dashboard placeholders BootProgress.c draws but which the
// stage-2 fuxi kernel does not build (no legacy display transfer, no offline
// GPU, no process trace, no cpufreq probe). Zero means "not present"; when a
// subsystem is really built, its archive takes the symbol over and this file
// must drop it. Types mirror the weak_import declarations in BootProgress.c.
#include <stddef.h>
#include <stdint.h>

// Boot process trace.
volatile unsigned boot_process_attempts;
volatile unsigned boot_process_busy;
volatile unsigned boot_process_passes;
char boot_process_rows[10][120];
volatile unsigned boot_process_seen;

// Boot UI trace.
volatile unsigned boot_ui_trace_index;
char boot_ui_trace_label[120];
volatile unsigned boot_ui_trace_phase;
unsigned boot_ui_trace_ready;
char boot_ui_trace_rows[14][120];

// Adreno counters BootProgress reads directly.
volatile uint32_t native_adreno_audit_attr;
volatile uint64_t native_adreno_audit_ticks;
volatile uint32_t native_adreno_coherence[8];
volatile uint32_t native_adreno_wc_distrusted;

// cpufreq / GOP counters.
volatile uint32_t native_cpufreq_stage;

// fuxi touch GPIO overlay (weak_import in BootProgress.c; zero keeps it off).
volatile int native_fuxi_touch_pad;
volatile uint32_t native_fuxi_touch_pins[6];
volatile int native_fuxi_touch_pins_n;
volatile uint32_t native_fuxi_touch_dbg[24];

// Legacy display transfer counters.

// Probe entry points: nothing to capture and nothing to draw.
void boot_ui_trace_snapshot(unsigned phase) { (void)phase; }
void boot_process_capture(void) {}
void boot_process_snapshot(void) {}
void sheng_cpufreq_poll(void) {}
