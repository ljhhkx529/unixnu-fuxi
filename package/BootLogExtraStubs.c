// SPDX-License-Identifier: BSD-2-Clause-Patent
// Extra zero-initialised placeholders BootLog.c composes but which the
// stage-1 fuxi kernel does not build. Real definitions come from
// FuxiButtons.cpp (buttons), BootProcessProbe.c (process wait port) and
// the data-persistence backend (native_data_persist_*). Keep these stubs
// until those sources are linked.
#include <stdint.h>

volatile int32_t fuxi_buttons_stats[10];
volatile uint32_t native_process_wait_port[10];

// Data persistence dashboard (NativeRamBlock overlay); not built in stage 1.
volatile uint64_t native_data_persist_errors;
volatile uint64_t native_data_persist_first_lba;
volatile uint32_t native_data_persist_gpt;
volatile uint32_t native_data_persist_gpt_after;
volatile uint32_t native_data_persist_header;
volatile uint64_t native_data_persist_loaded;
volatile uint64_t native_data_persist_pages;
volatile uint64_t native_data_persist_present;
volatile uint32_t native_data_persist_state;
volatile uint64_t native_data_persist_syncs;
volatile uint64_t native_data_persist_writes;
