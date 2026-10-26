/*
 * fuxi device adapter for QcomXnuUsbDebug.
 *
 * The XNU patch still calls the existing fuxi_usb_* hook names. This file maps
 * those device hooks onto the generic qcom_usb_debug_* API and runs the USB
 * event pump on a dedicated kernel thread.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <pexpert/pexpert.h>

#include <QcomXnuUsbDebug.h>

static int fuxi_usb_attempted;
static int fuxi_usb_wait_channel;

static void
fuxi_usb_worker (
  void *argument __unused,
  wait_result_t result __unused
  )
{
  uint64_t deadline;

  thread_set_thread_name (current_thread (), "qcom-usb-debug");
  for ( ; ;) {
    qcom_usb_debug_poll ();
    clock_interval_to_deadline (5, NSEC_PER_MSEC, &deadline);
    (void)assert_wait_deadline (&fuxi_usb_wait_channel, THREAD_UNINT, deadline);
    thread_block (THREAD_CONTINUE_NULL);
  }
}

void
fuxi_usb_debug_start (
  void
  )
{
  thread_t thread;
  kern_return_t result;
  int enabled;

  if (fuxi_usb_attempted != 0) {
    return;
  }

  fuxi_usb_attempted = 1;
  enabled = 0;
  if (!PE_parse_boot_argn ("qcom-usb-debug", &enabled, sizeof (enabled)) ||
      enabled != 1) {
    return;
  }

  kprintf ("FUXI USB: qcom_usb_debug_init enter\n");
  if (!qcom_usb_debug_init ()) {
    kprintf ("FUXI USB: qcom_usb_debug_init failed\n");
    return;
  }
  kprintf ("FUXI USB: qcom_usb_debug_init ok\n");

  result = kernel_thread_start (fuxi_usb_worker, NULL, &thread);
  kprintf ("FUXI USB: worker start result=%d\n", result);
  if (result == KERN_SUCCESS) {
    thread_deallocate (thread);
  }
}

void
fuxi_usb_append (
  const char *data,
  int size
  )
{
  if ((data == NULL) || (size <= 0)) {
    return;
  }

  (void)qcom_usb_debug_write (data, (size_t)size);
}
