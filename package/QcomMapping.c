/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomMapping.h"

#define QCOM_MAP_DEFAULT_PAGE 0x4000ull

static uint64_t page_of(const struct QcomMapping *mapping)
{
    return mapping->page_size ? mapping->page_size : QCOM_MAP_DEFAULT_PAGE;
}

int qcom_map(struct QcomMapping *mapping, const struct QcomMappingBackend *backend, uint64_t page_size,
             uint64_t physical, uint64_t length)
{
    if (!mapping) return QcomMapNoBackend;
    if (mapping->mapped) return QcomMapAlreadyMapped;
    if (!backend || !backend->acquire) return QcomMapNoBackend;
    if (!length) return QcomMapZeroLength;

    mapping->backend = backend;
    mapping->page_size = page_size;
    const uint64_t page = page_of(mapping);
    /* The last byte must exist: this refuses a range that would wrap, which an unaligned base makes easy to miss. */
    if (physical + length - 1ull < physical) return QcomMapOverflow;
    if (physical + length - 1ull > UINT64_MAX - page) return QcomMapOverflow;

    const uint64_t base = physical & ~(page - 1ull);
    const uint64_t offset = physical - base;
    uint64_t size = offset + length;
    if (size + page - 1ull < size) return QcomMapOverflow;
    size = (size + page - 1ull) & ~(page - 1ull);

    void *window = backend->acquire(backend->context, base, size);
    if (!window) {
        /* Nothing is recorded: a failed attempt must not look like a mapping that needs unmapping. */
        mapping->backend = 0;
        mapping->window_base = 0;
        mapping->window_size = 0;
        mapping->window = 0;
        mapping->pointer = 0;
        return QcomMapBackendError;
    }

    mapping->physical = physical;
    mapping->length = length;
    mapping->window_base = base;
    mapping->window_size = size;
    mapping->window = window;
    mapping->pointer = (void *)((uintptr_t)window + (uintptr_t)offset);
    mapping->mapped = 1;
    return QcomMapOk;
}

void *qcom_map_pointer(const struct QcomMapping *mapping)
{
    if (!mapping || !mapping->mapped) return 0;
    return mapping->pointer;
}

int qcom_unmap(struct QcomMapping *mapping)
{
    if (!mapping) return QcomMapNotMapped;
    if (!mapping->mapped) return QcomMapNotMapped;
    const struct QcomMappingBackend *backend = mapping->backend;
    if (!backend || !backend->release) {
        /* The window exists but cannot be released: keep the state so a caller can retry instead of leaking silently. */
        return QcomMapNoBackend;
    }
    mapping->mapped = 0;
    backend->release(backend->context, mapping->window);
    mapping->window = 0;
    mapping->pointer = 0;
    mapping->window_base = 0;
    mapping->window_size = 0;
    mapping->backend = 0;
    return QcomMapOk;
}
