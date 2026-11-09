/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The TCSR mutex encoding, as the mainline driver defines it.
 *
 * Source: drivers/hwspinlock/qcom_hwspinlock.c in the Linux kernel (see facts/audio/manifest.toml for the exact revision).
 * The parts that matter here:
 *
 *   - the mutex block holds 32 locks, one per `stride` (the tcsr-mutex match data uses 0x1000, and the board's device
 *     tree passes the same stride through its syscon property);
 *   - a lock is a whole 32-bit register at `offset + id * stride`;
 *   - acquiring means writing the caller's processor id and then reading the register back: the lock is ours only if the
 *     read returns our id, because another processor may have won the same write;
 *   - releasing means writing zero (the mainline driver reads the owner first, complains if it is not us, and writes zero
 *     regardless).
 *
 * This file is the encoding only. The bounded policy around it - deadline, recursion, single owner - lives in
 * QcomHwspinlock, and the two are joined through QcomHwlockDevice.
 */
#ifndef QCOM_TCSR_HWLOCK_H
#define QCOM_TCSR_HWLOCK_H

#include <stdint.h>

#include "QcomHwspinlock.h"

enum {
    QcomTcsrOk = 0,
    QcomTcsrBadId = -1,      /* id outside the bank */
    QcomTcsrOwnerMismatch = -2 /* release found someone else's id in the register */
};

/* Board numbers, all of them parameters: a different SoC passes different values and needs no code change. */
struct QcomTcsrConfig {
    uint32_t base;           /* offset of the first lock inside the mutex block */
    uint32_t stride;         /* bytes between consecutive locks */
    uint32_t locks_per_bank; /* the mainline driver's QCOM_MUTEX_NUM_LOCKS */
    uint32_t owner_id;       /* the mainline driver's QCOM_MUTEX_APPS_PROC_ID */
};

/* Register access, injected so the host can use an array and the kernel an MMIO window. */
struct QcomTcsrIo {
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);
    void *context;
};

struct QcomTcsr {
    struct QcomTcsrConfig config;
    struct QcomTcsrIo io;
    struct QcomHwlockDevice device;  /* the adapter the policy layer calls */
};

/* Register offset for `id`, or QcomTcsrBadId through the return value of the acquire path when it is outside the bank. */
int qcom_tcsr_offset(const struct QcomTcsrConfig *config, uint32_t id, uint32_t *offset);

/* Wires the adapter; after this the struct's `device` can be handed to qcom_hwlock_init. */
void qcom_tcsr_init(struct QcomTcsr *tcsr, const struct QcomTcsrConfig *config, const struct QcomTcsrIo *io);

/* The two encoding calls, usable on their own so a test can count what reached the registers. */
int qcom_tcsr_try_acquire(struct QcomTcsr *tcsr, uint32_t id);
int qcom_tcsr_release(struct QcomTcsr *tcsr, uint32_t id);

#endif
