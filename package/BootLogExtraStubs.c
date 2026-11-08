// SPDX-License-Identifier: BSD-2-Clause-Patent
// Extra zero-initialised placeholders BootLog.c composes but which the
// stage-2 fuxi kernel does not build. Real definitions come from
// FuxiButtons.cpp (buttons) and BootProcessProbe.c (process wait port).
// native_data_persist_* moved out: NativeRamBlock now defines them.
#include <stdint.h>

/* fuxi_buttons_stats moved out: FuxiButtons.cpp now defines it (fuxi-buttons=1). */
volatile uint32_t native_process_wait_port[10];
