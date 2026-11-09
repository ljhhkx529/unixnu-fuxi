/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomTcsrHwlock.h"

int qcom_tcsr_offset(const struct QcomTcsrConfig *config, uint32_t id, uint32_t *offset)
{
    if (!config || !offset) return QcomTcsrBadId;
    if (!config->locks_per_bank || id >= config->locks_per_bank) return QcomTcsrBadId;
    /* 32-bit arithmetic on purpose: the block is far below 4 GiB and a wrapping offset must be impossible rather than
     * merely unlikely. */
    const uint64_t at = (uint64_t)config->base + (uint64_t)id * config->stride;
    if (at > UINT32_MAX) return QcomTcsrBadId;
    *offset = (uint32_t)at;
    return QcomTcsrOk;
}

int qcom_tcsr_try_acquire(struct QcomTcsr *tcsr, uint32_t id)
{
    uint32_t offset = 0;
    if (!tcsr || !tcsr->io.read || !tcsr->io.write) return QcomTcsrBadId;
    if (qcom_tcsr_offset(&tcsr->config, id, &offset) != QcomTcsrOk) return QcomTcsrBadId;
    /* Claim, then confirm: the read-back is the whole protocol, because another processor can win the same write. */
    tcsr->io.write(tcsr->io.context, offset, tcsr->config.owner_id);
    return tcsr->io.read(tcsr->io.context, offset) == tcsr->config.owner_id ? QcomHwlockOk : QcomHwlockBusy;
}

int qcom_tcsr_release(struct QcomTcsr *tcsr, uint32_t id)
{
    uint32_t offset = 0;
    if (!tcsr || !tcsr->io.read || !tcsr->io.write) return QcomTcsrBadId;
    if (qcom_tcsr_offset(&tcsr->config, id, &offset) != QcomTcsrOk) return QcomTcsrBadId;
    const uint32_t owner = tcsr->io.read(tcsr->io.context, offset);
    /* Mainline writes zero even when the owner is someone else; it reports the mismatch first. This returns the same
     * information to its caller instead of printing, because a probe must not print inside a critical section. */
    tcsr->io.write(tcsr->io.context, offset, 0);
    return owner == tcsr->config.owner_id ? QcomHwlockOk : QcomTcsrOwnerMismatch;
}

static int adapter_try_acquire(void *context, uint32_t id)
{
    return qcom_tcsr_try_acquire((struct QcomTcsr *)context, id);
}

static int adapter_release(void *context, uint32_t id)
{
    return qcom_tcsr_release((struct QcomTcsr *)context, id);
}

void qcom_tcsr_init(struct QcomTcsr *tcsr, const struct QcomTcsrConfig *config, const struct QcomTcsrIo *io)
{
    if (!tcsr || !config || !io) return;
    tcsr->config = *config;
    tcsr->io = *io;
    tcsr->device.try_acquire = adapter_try_acquire;
    tcsr->device.release = adapter_release;
    tcsr->device.context = tcsr;
}
