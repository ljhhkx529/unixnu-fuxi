/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomHwspinlock.h"

void qcom_hwlock_init(struct QcomHwlock *lock, const struct QcomHwlockDevice *device, uint32_t id)
{
    if (!lock) return;
    lock->device = device;
    lock->id = id;
    lock->held = 0;
    lock->attempts = 0;
}

int qcom_hwlock_acquire(struct QcomHwlock *lock, uint32_t budget_ms, uint32_t (*now_ms)(void *), void *now_context)
{
    if (!lock || !lock->device || !lock->device->try_acquire) return QcomHwlockNoDevice;
    if (lock->held) return QcomHwlockRecursive;
    if (budget_ms && !now_ms) return QcomHwlockDeviceError;

    const uint32_t started = budget_ms ? now_ms(now_context) : 0u;
    const uint32_t deadline = started + budget_ms;
    for (;;) {
        ++lock->attempts;
        const int rc = lock->device->try_acquire(lock->device->context, lock->id);
        if (rc == QcomHwlockOk) {
            lock->held = 1;
            return QcomHwlockOk;
        }
        if (rc != QcomHwlockBusy) return rc == QcomHwlockNoDevice ? QcomHwlockNoDevice : QcomHwlockDeviceError;
        if (!budget_ms) return QcomHwlockBusy;
        /* Wrapping arithmetic keeps the comparison correct across the counter's roll-over. */
        if ((int32_t)(now_ms(now_context) - deadline) >= 0) return QcomHwlockBusy;
    }
}

int qcom_hwlock_release(struct QcomHwlock *lock)
{
    if (!lock || !lock->device || !lock->device->release) return QcomHwlockNoDevice;
    if (!lock->held) return QcomHwlockNotHeld;
    /* Cleared before the call: if the encoding fails, the handle must not claim to hold a lock it could not release. */
    lock->held = 0;
    return lock->device->release(lock->device->context, lock->id) == 0 ? QcomHwlockOk : QcomHwlockDeviceError;
}
