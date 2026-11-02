// SPDX-License-Identifier: BSD-2-Clause-Patent
// Extra zero-initialised placeholders BootLog.c composes but which the
// stage-1 fuxi kernel does not build. Real definitions come from
// FuxiButtons.cpp (buttons) and BootProcessProbe.c (process wait port);
// keep these stubs until those sources are linked.
#include <stdint.h>

volatile int32_t fuxi_buttons_stats[10];
volatile uint32_t native_process_wait_port[10];
