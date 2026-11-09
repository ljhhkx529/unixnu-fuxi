/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Qualcomm shared memory (SMEM): the partition table at the end of the region, the private partitions two processors share, and the
 * items in them (uncached ones growing up from the partition header, cached ones down from its end). Every access is an aligned 32-bit
 * one, so the code works on a device mapping. Allocation must run with the hardware mutex held (the caller takes it); nothing here
 * waits. */
#ifndef QCOM_SMEM_H
#define QCOM_SMEM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_SMEM_HOST_APPS 0u
#define QCOM_SMEM_HOST_GLOBAL 0xfffeu

typedef struct {
    volatile uint8_t *base;
    uint32_t size;
} QcomSmem;

typedef struct {
    uint32_t offset, size, cacheline;  /* offset of the partition header from the SMEM base */
    uint16_t host0, host1;
} QcomSmemPartition;

enum {
    QcomSmemOk = 0,
    QcomSmemNoTable = -1,
    QcomSmemNoPartition = -2,
    QcomSmemBadPartition = -3,
    QcomSmemCorrupt = -4,
    QcomSmemExists = -5,
    QcomSmemNoSpace = -6,
    QcomSmemMissing = -7,
};

/* The partition shared by the two hosts (QCOM_SMEM_HOST_GLOBAL twice for the global one). */
int qcom_smem_partition(const QcomSmem *smem, uint16_t host, uint16_t remote, QcomSmemPartition *out);
/* Offset of the item's data from the SMEM base and its size; QcomSmemMissing when the item is not there. */
int qcom_smem_find(const QcomSmem *smem, const QcomSmemPartition *part, uint16_t item, uint32_t *offset, uint32_t *size);
/* Allocates an uncached item of size bytes (rounded up to 8). QcomSmemExists if it is there already (not an error for callers that
 * only need it to exist). The payload is deliberately left untouched: protocol initialization is a separate transaction. The
 * entry is completed before a release barrier and the final free-offset publication. Hardware mutex held by the caller. */
int qcom_smem_alloc(const QcomSmem *smem, const QcomSmemPartition *part, uint16_t item, uint32_t size);

/* The uncached items a partition holds, for a caller that has to report what is there rather than ask for a name it
 * already knows. QcomSmemMissing past the last one, or a negative error from the walk. */
int qcom_smem_item_count(const QcomSmem *smem, const QcomSmemPartition *part);
int qcom_smem_item_at(const QcomSmem *smem, const QcomSmemPartition *part, uint32_t index, uint16_t *id,
                      uint32_t *offset, uint32_t *size);

uint32_t qcom_smem_read32(const QcomSmem *smem, uint32_t offset);
void qcom_smem_write32(const QcomSmem *smem, uint32_t offset, uint32_t value);

#ifdef __cplusplus
}
#endif
#endif
