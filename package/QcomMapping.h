/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* A physical-to-virtual mapping helper with one job: get the window right.
 *
 * Everything that maps a device range (SMEM, TCSR, a DSP carveout) has to answer the same three questions before it
 * touches the range, and every one of them has ended a probe early when answered wrongly:
 *
 *   - the mapping is page aligned, so the window starts below the address that was asked for;
 *   - the caller gets a pointer at the offset it asked for, not at the page boundary;
 *   - a mapping that failed leaves nothing behind, so a later unmap is not asked to undo an attempt.
 *
 * The backend is injected: the kernel passes XNU's IOMemoryDescriptor path, the host tests pass a mock that counts calls
 * and can be told to fail. What is proved here is the arithmetic and the symmetry, not the page attributes - the runbook
 * keeps that for the device, where a cache alias or a MAIR mismatch can actually be observed.
 */
#ifndef QCOM_MAPPING_H
#define QCOM_MAPPING_H

#include <stdint.h>

enum {
    QcomMapOk = 0,
    QcomMapZeroLength = -1,
    QcomMapOverflow = -2,
    QcomMapNoBackend = -3,
    QcomMapAlreadyMapped = -4,
    QcomMapNotMapped = -5,
    QcomMapBackendError = -6
};

struct QcomMappingBackend {
    /* Maps [base, base + size) and returns a virtual address, or NULL. The backend receives an already aligned range. */
    void *(*acquire)(void *context, uint64_t base, uint64_t size);
    /* Releases exactly the pointer acquire returned. */
    void (*release)(void *context, void *virtual_address);
    void *context;
};

struct QcomMapping {
    const struct QcomMappingBackend *backend;
    uint64_t page_size;    /* 0 means the platform default of 16 KiB */
    uint64_t physical;     /* what the caller asked for */
    uint64_t length;
    uint64_t window_base;  /* the aligned physical start actually mapped */
    uint64_t window_size;
    void *window;          /* the virtual address of window_base */
    void *pointer;         /* window + (physical - window_base) */
    int mapped;
};

/* Maps `length` bytes starting at `physical`, aligned to `page_size` (16 KiB when the mapping says 0). */
int qcom_map(struct QcomMapping *mapping, const struct QcomMappingBackend *backend, uint64_t page_size,
             uint64_t physical, uint64_t length);

/* The pointer the caller should use, or NULL when nothing is mapped. */
void *qcom_map_pointer(const struct QcomMapping *mapping);

/* Releases the window. Refuses when nothing is mapped rather than forwarding a release to the backend. */
int qcom_unmap(struct QcomMapping *mapping);

#endif
