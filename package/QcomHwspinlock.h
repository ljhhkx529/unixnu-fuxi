/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* A bounded hardware spinlock wrapper.
 *
 * The lock itself is a hardware protocol: the register layout and the bit that acquires it belong to the SoC and are
 * supplied by the board (qcom_hwlock_device). What this file owns is the policy the runbook in
 * docs/fuxi-audio-probe-debug-runbook.md requires of every caller of it:
 *
 *   - acquire has a deadline and never polls without one;
 *   - a busy lock returns, it never spins forever;
 *   - a handle that already holds the lock refuses a second acquire instead of deadlocking on itself;
 *   - every path that acquired releases, and a path that did not acquire releases nothing;
 *   - a release of a handle that is not held is refused, not forwarded to hardware;
 *   - nothing here logs, sleeps or waits for the USB channel: the caller reports after it lets go.
 *
 * Splitting it this way is deliberate. The policy is provable on the host; the encoding is not, and pretending otherwise
 * would be how a probe ends up writing a lock register with the wrong bit on a real phone.
 */
#ifndef QCOM_HWSPINLOCK_H
#define QCOM_HWSPINLOCK_H

#include <stdint.h>

enum {
    QcomHwlockOk = 0,
    QcomHwlockBusy = -1,      /* the deadline passed before the hardware freed the lock */
    QcomHwlockRecursive = -2, /* this handle already holds it */
    QcomHwlockNotHeld = -3,   /* release of a handle that does not hold the lock */
    QcomHwlockNoDevice = -4,  /* no board encoding: nothing may be attempted */
    QcomHwlockDeviceError = -5 /* the board's encoding reported a failure */
};

/* Board-supplied encoding. try_acquire returns QcomHwlockOk when the hardware now belongs to the caller, QcomHwlockBusy
 * when it belongs to someone else, and any other negative value as a hard error. release must not fail silently. */
struct QcomHwlockDevice {
    int (*try_acquire)(void *context, uint32_t id);
    int (*release)(void *context, uint32_t id);
    void *context;
};

struct QcomHwlock {
    const struct QcomHwlockDevice *device;
    uint32_t id;
    int held;
    uint32_t attempts;   /* how many times the encoding was asked, for the caller's report */
};

void qcom_hwlock_init(struct QcomHwlock *lock, const struct QcomHwlockDevice *device, uint32_t id);

/* Waits at most budget_ms for the lock. A budget of zero means a single attempt and no waiting. */
int qcom_hwlock_acquire(struct QcomHwlock *lock, uint32_t budget_ms, uint32_t (*now_ms)(void *), void *now_context);

/* Releases a lock this handle holds. Any other state is refused without touching the device. */
int qcom_hwlock_release(struct QcomHwlock *lock);

#endif
