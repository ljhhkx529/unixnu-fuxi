/*
 * Stage 2a stubs for the legacy display hooks.
 *
 * bsd_init references native_legacy_display_probe() and
 * native_legacy_display_late_probe() unconditionally. NativeAdrenoPublish also
 * references the GOP hand-off entry points, whose real implementations live in
 * NativeGOPPresenter (built as part of the full LegacyDisplay archive). Both
 * depend on a prelink candidate
 * (build/native-prelink/native-owner-legacy-display-*-candidate.macho) that
 * cannot exist before the kernel links, so the real archive cannot run on a
 * first build.
 *
 * Linking these no-ops lets the kernel link. The boot progress layer still
 * draws to the handed-over frame buffer, so console visibility does not depend
 * on the display hooks, and the Adreno hand-off path reports "not ready" until
 * the real presenter is linked. Replace them once the prelink exists.
 */

void
native_legacy_display_probe (
  void
  )
{
}

void
native_legacy_display_late_probe (
  void
  )
{
}

int
native_gop_handoff_call (
  unsigned int op,
  void *task,
  const void *in,
  unsigned long in_size,
  void *out,
  unsigned long out_size
  )
{
  (void)op;(void)task;(void)in;(void)in_size;(void)out;(void)out_size;
  return -536870203; /* kIOReturnNotReady */
}

void *
native_gop_handoff_memory (
  void
  )
{
  return 0;
}
