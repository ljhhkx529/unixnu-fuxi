/*
 * fuxi diagnostics adapter for QcomXnuUsbDebug.
 *
 * The transport itself is driven by the module's own Darwin23 patches (console tee in
 * serial_console.c/pe_kprintf.c, the polling thread in bsd_init.c) and by
 * fuxi-usb-boot-progress.py, which hands qcom_usb_debug_init() the console before BootProgress
 * takes it over. This file therefore only publishes what the on-screen window and the panic screen
 * need: the phase colour and a few readable rows. It never initialises, polls or writes the
 * controller, so a mistake here cannot disturb the transport.
 */
#include <kern/clock.h>
#include <kern/debug.h>
#include <pexpert/pexpert.h>

#include <QcomXnuUsbDebug.h>

/* The raw GCC/DWC3 words the inherited-state qualification sampled, published by the driver patch.
 * [0]=CFG_GDSCR (bit16 = power-up complete), [1]=GDSCR, [2]=BCR (bit0 = reset), [3..7]=CBCR0..4
 * (bit31 = clock off), [8]=GCTL, [9]=GSNPSID.
 */
extern volatile unsigned int qcom_usb_debug_qusb[10];
/* <libkern/libkern.h> would conflict with <kern/clock.h> over clock_t, so only the prototype this
 * file needs is declared. size_t is unsigned long on LP64. */
extern int snprintf (char *buffer, unsigned long size, const char *format, ...);

/* Panic-screen page background derived from the CDC phase: green = ACTIVE (host enumerated the
 * CDC port), yellow = PREPARING or active-but-not-yet-enumerated, red = not started / failed.
 * BGRA word: A<<24 | R<<16 | G<<8 | B. Computing it here keeps the colour logic out of the panic
 * screen's tight padding budget: the renderer only reads this word.
 */
volatile unsigned int native_usb_panic_bg = 0xFFFF0000u;

/* The status window drawn by BootProgress.c in the bottom-right corner.
 *   row0  phase, error this far
 *   row1  the diagnostic checkpoint the driver reached
 *   row2  the function that refused, or "ok" once the core runs
 *   row3  the qualification result (why an inherited state was refused) or the lease bits
 * The panel colour follows the phase: green ACTIVE, yellow PREPARING, red otherwise.
 */
char native_usb_status_row[10][64] = { "not started", "", "", "", "", "", "", "", "", "" };
volatile unsigned int native_usb_status_color = 0xFFFF0000u;
volatile unsigned int native_usb_status_rows = 1;

/* The error code names the function that refused, in qcom_usb_debug_error order. */
static const char *
fuxi_usb_error_name (unsigned int error)
{
  switch (error) {
  case QCOM_USB_DEBUG_OK: return "ok";
  case QCOM_USB_DEBUG_CONTEXT_TOO_SMALL: return "dwc3_context_bytes";
  case QCOM_USB_DEBUG_BAD_COUNTER: return "cntfrq_el0";
  case QCOM_USB_DEBUG_CACHE_GRANULE_UNSUPPORTED: return "ctr_el0";
  case QCOM_USB_DEBUG_DMA_NOT_CONTIGUOUS: return "validate_static_pages(dma)";
  case QCOM_USB_DEBUG_MAP_FAILED: return "ml_io_map";
  case QCOM_USB_DEBUG_INHERITED_STATE_REFUSED: return "QcomUsbInspectInherited";
  case QCOM_USB_DEBUG_UNSTABLE_POWER_STATE: return "gcc_gdscr";
  case QCOM_USB_DEBUG_CORE_NOT_DEVICE: return "gctl.mode";
  case QCOM_USB_DEBUG_HALT_TIMEOUT: return "halt_inherited";
  case QCOM_USB_DEBUG_DMA_REFUSED: return "QcomUsbDomainInstall";
  case QCOM_USB_DEBUG_CORE_INIT_FAILED: return "dwc3_init";
  case QCOM_USB_DEBUG_CORE_START_FAILED: return "dwc3_start";
  case QCOM_USB_DEBUG_CORE_POLL_FAILED: return "dwc3_poll";
  case QCOM_USB_DEBUG_DEADLINE_EXPIRED: return "2s deadline";
  case QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED: return "callback range";
  case QCOM_USB_DEBUG_RESTORE_FAILED: return "restore";
  case QCOM_USB_DEBUG_VBUS_FAILED: return "acquire_vbus";
  case QCOM_USB_DEBUG_DMA_WIDTH_UNSUPPORTED: return "dma_addressable";
  case QCOM_USB_DEBUG_TABLES_NOT_CONTIGUOUS: return "validate_static_pages(tab)";
  case QCOM_USB_DEBUG_DOMAIN_STATE_INVALID: return "SMMU domain verify";
  case QCOM_USB_DEBUG_ACQUIRE_REFUSED: return "acquire(cpu0)";
  case QCOM_USB_DEBUG_PHY_LEASE_FAILED: return "acquire_phy";
  default: return "unknown";
  }
}

/* The QUSB_RESULT the qualification returned, in QcomUsbQualify.h order. This is the actionable
 * reason an inherited-state refusal happened: the driver's own error code only says "refused".
 */
static const char *
fuxi_usb_qusb_name (unsigned int result)
{
  switch (result) {
  case 0: return "qualified";
  case 1: return "bad-argument";
  case 2: return "read-failed";
  case 3: return "clock-off";
  case 4: return "reset-asserted";
  case 5: return "unknown-dwc3";
  case 6: return "unknown-smmu";
  case 7: return "global-disabled";
  case 8: return "extended-stream-ids";
  case 9: return "no-explicit-stream";
  case 10: return "multiple-streams";
  case 11: return "shared-stream";
  case 12: return "stream-translated";
  case 13: return "stream-faulting";
  case 14: return "unsupported-stream-type";
  case 15: return "power-domain-off";
  case 16: return "core-running";
  case 17: return "write-failed";
  case 18: return "ownership-changed";
  case 19: return "wrong-core-role";
  case 20: return "unsafe-power-state";
  case 21: return "parent-clock-not-ready";
  case 22: return "clock-forced-off";
  case 23: return "clock-timeout";
  default: return "unknown";
  }
}

/* The diagnostic checkpoint names the probe the driver last passed, in
 * qcom_usb_debug_diag_checkpoint order.
 */
static const char *
fuxi_usb_checkpoint_name (unsigned int checkpoint)
{
  switch (checkpoint) {
  case QCOM_USB_DEBUG_DIAG_BEFORE_ACQUIRE: return "before-acquire";
  case QCOM_USB_DEBUG_DIAG_AFTER_ACQUIRE: return "after-acquire";
  case QCOM_USB_DEBUG_DIAG_COUNTER_CACHE: return "counter-cache";
  case QCOM_USB_DEBUG_DIAG_DATA_VTOP: return "dma-vtop";
  case QCOM_USB_DEBUG_DIAG_TABLE_VTOP: return "table-vtop";
  case QCOM_USB_DEBUG_DIAG_MAP_GCC: return "map-gcc";
  case QCOM_USB_DEBUG_DIAG_MAP_USB: return "map-usb";
  case QCOM_USB_DEBUG_DIAG_MAP_SMMU: return "map-smmu";
  case QCOM_USB_DEBUG_DIAG_INSPECT: return "inspect-inherited";
  case QCOM_USB_DEBUG_DIAG_GATES_WIDTH: return "gates-width";
  case QCOM_USB_DEBUG_DIAG_HALT: return "halt-inherited";
  case QCOM_USB_DEBUG_DIAG_DOMAIN: return "smmu-domain";
  case QCOM_USB_DEBUG_DIAG_VERIFY_DOMAIN: return "verify-domain";
  case QCOM_USB_DEBUG_DIAG_VBUS: return "phy-vbus";
  case QCOM_USB_DEBUG_DIAG_CORE_INIT: return "dwc3-init";
  case QCOM_USB_DEBUG_DIAG_CORE_START: return "dwc3-start";
  case QCOM_USB_DEBUG_DIAG_CORE_STARTED: return "dwc3-started";
  case QCOM_USB_DEBUG_DIAG_FIRST_POLL: return "first-poll";
  case QCOM_USB_DEBUG_DIAG_ENUM_DONE: return "enum-done";
  default: return "none";
  }
}

/* Called by the driver's diag_checkpoint() while it holds usb_busy, so nothing here may touch the
 * driver API. It only formats, stores and traces, which is enough to name the last probe before a
 * hang: the console keeps the trail even when the on-screen window stops refreshing.
 */
void
fuxi_usb_probe_step (unsigned int checkpoint)
{
  const char *name = fuxi_usb_checkpoint_name (checkpoint);
  static unsigned int calls;

  calls++;
  /* 200+n is fail(error n); 250/251 mark the driver-lock hand-off; 300+ trace the three mappings. */
  if (checkpoint >= 300u) {
    kprintf ("QUD map %u %s n%u\n", checkpoint,
             checkpoint == 300u ? "gcc-enter" : checkpoint == 301u ? "gcc-done" :
             checkpoint == 302u ? "usb-done" : checkpoint == 303u ? "smmu-done" : "?", calls);
  } else if (checkpoint >= 200u) {
    kprintf ("QUD step %u %s n%u\n", checkpoint,
             checkpoint == 250u ? "acquire-enter" :
             checkpoint == 251u ? "acquire-REFUSED" : "fail", calls);
  } else {
    kprintf ("QUD probe %u %s n%u\n", checkpoint, name, calls);
  }
  snprintf (native_usb_status_row[0], sizeof native_usb_status_row[0],
            "PROBE %u %s", checkpoint, name);
  snprintf (native_usb_status_row[1], sizeof native_usb_status_row[1], "live %u n%u %s",
            checkpoint, calls, name);
  native_usb_status_row[2][0] = 0;
  native_usb_status_row[3][0] = 0;
  native_usb_status_rows = 2;
  native_usb_status_color = 0xFFFFFF00u;
}

/* qcom_usb_debug_get_status leaves the buffer untouched when another context holds the controller
 * (the bring-up itself, or a console tee inside it). BootProgress calls this on every overlay pass;
 * a refused read returns here so the live "PROBE n" rows written by fuxi_usb_probe_step() stay
 * visible instead of being overwritten with an older snapshot.
 */
void
fuxi_usb_refresh_bg (void)
{
  struct qcom_usb_debug_status st;
  const char *name;
  unsigned int i;

  for (i = 0; i < sizeof st; i++) {
    ((unsigned char *)&st)[i] = 0xFF;
  }
  qcom_usb_debug_get_status (&st);
  if (st.phase == 0xFFFFFFFFu) {
    return;
  }
  name = st.phase == 3 ? "ACTIVE" : st.phase == 2 ? "PREPARING" :
         st.phase == 1 ? "DISABLED" : st.phase == 4 ? "STOPPED" :
         st.phase == 5 ? "REFUSED" : st.phase == 6 ? "IO_FAILED" : "NOT_INIT";

  native_usb_panic_bg = (st.phase == 3) ? 0xFF00FF00u :
                        (st.phase == 2) ? 0xFFFFFF00u : 0xFFFF0000u;
  native_usb_status_color = native_usb_panic_bg;

  snprintf (native_usb_status_row[0], sizeof native_usb_status_row[0],
            "STAT %s ph=%u er=%u", name, st.phase, st.error);
  snprintf (native_usb_status_row[1], sizeof native_usb_status_row[1],
            "probe %u %s", st.diag_checkpoint, fuxi_usb_checkpoint_name (st.diag_checkpoint));
  snprintf (native_usb_status_row[2], sizeof native_usb_status_row[2],
            st.phase == 3 ? "ok, core running" : "fail %s", fuxi_usb_error_name (st.error));
  if (st.error == QCOM_USB_DEBUG_INHERITED_STATE_REFUSED) {
    snprintf (native_usb_status_row[3], sizeof native_usb_status_row[3],
              "qres %u %s", st.qcom_result, fuxi_usb_qusb_name (st.qcom_result));
    /* The words the qualification sampled, in the window so they need no serial console:
     * cfg bit16 = power-up complete, gd bit0 = SW_COLLAPSE, bcr bit0 = reset asserted,
     * b0..b4 = CBCR0..4 with bit31 = clock off, gctl[13:12] = 2 for device mode,
     * id = 0x5533xxxx for a DWC3. */
    snprintf (native_usb_status_row[4], sizeof native_usb_status_row[4],
              "id=%08x gctl=%08x", qcom_usb_debug_qusb[9], qcom_usb_debug_qusb[8]);
    snprintf (native_usb_status_row[5], sizeof native_usb_status_row[5],
              "cfg=%08x gd=%08x", qcom_usb_debug_qusb[0], qcom_usb_debug_qusb[1]);
    snprintf (native_usb_status_row[6], sizeof native_usb_status_row[6],
              "bcr=%08x b0=%08x", qcom_usb_debug_qusb[2], qcom_usb_debug_qusb[3]);
    snprintf (native_usb_status_row[7], sizeof native_usb_status_row[7],
              "b1=%08x b2=%08x", qcom_usb_debug_qusb[4], qcom_usb_debug_qusb[5]);
    snprintf (native_usb_status_row[8], sizeof native_usb_status_row[8],
              "b3=%08x b4=%08x", qcom_usb_debug_qusb[6], qcom_usb_debug_qusb[7]);
    native_usb_status_rows = 9;
  } else {
    /* Any other refusal: the lease bits, then the numbers that name the failing driver step.
     * core_result is dwc3_init()'s return (-1 argument, -2 id/params, -3 reset or halt timeout,
     * -4 endpoint command), and the trace fields hold the register and value a timeout sampled. */
    snprintf (native_usb_status_row[3], sizeof native_usb_status_row[3],
              "phy %u/%x vbus %u/%x", st.phy_owned, st.phy_saved, st.vbus_owned, st.vbus_saved);
    snprintf (native_usb_status_row[4], sizeof native_usb_status_row[4],
              "hw %x cr %d id %08x", st.hwparams0, (int)st.core_result,
              qcom_usb_debug_qusb[9]);
    snprintf (native_usb_status_row[5], sizeof native_usb_status_row[5],
              "tr %u a %x b %x", st.last_trace, st.trace_a, st.trace_b);
    native_usb_status_rows = 6;
  }
  /* Once the port is ACTIVE the lease and register rows say nothing useful, and every extra row is
   * another ~40k uncached stores per repaint: keep the success window to the two status rows. */
  if (st.phase == 3u) {
    native_usb_status_rows = 2u;
  }
}
