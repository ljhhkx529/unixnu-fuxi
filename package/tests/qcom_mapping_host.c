/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* H8 of the audio-probe debug runbook: the mapping helper's contract.
 *
 * The mock backend counts acquisitions and releases and returns a fixed page per acquisition, so the arithmetic can be
 * checked exactly: what physical window was requested, how big it was, what pointer the caller received, and whether a
 * refusal reached the backend at all. What this cannot check is the page attributes of a real mapping, which the runbook
 * keeps for the device gate.
 */
#include <stdio.h>
#include <string.h>

#include "../QcomMapping.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

struct mock {
    uintptr_t next;      /* the virtual address handed out; page aligned like a real one */
    uint64_t last_base;
    uint64_t last_size;
    int acquires;
    int releases;
    void *last_released;
    int fail_next;
};

static void *mock_acquire(void *context, uint64_t base, uint64_t size)
{
    struct mock *m = context;
    ++m->acquires;
    m->last_base = base;
    m->last_size = size;
    if (m->fail_next) {m->fail_next = 0; return 0;}
    return (void *)m->next;
}

static void mock_release(void *context, void *address)
{
    struct mock *m = context;
    ++m->releases;
    m->last_released = address;
}

static struct QcomMappingBackend backend_of(struct mock *m)
{
    struct QcomMappingBackend backend = {mock_acquire, mock_release, m};
    return backend;
}

#define PAGE 0x4000ull

static void test_unaligned_within_page(void)
{
    struct mock m = {0x10000000u, 0, 0, 0, 0, 0, 0};
    struct QcomMappingBackend backend = backend_of(&m);
    struct QcomMapping mapping = {0};

    /* 0x81d00123 is 0x123 into its page; 0x40 bytes stay inside it. */
    check(qcom_map(&mapping, &backend, PAGE, 0x81d00123ull, 0x40) == QcomMapOk, "an unaligned range maps");
    check(m.last_base == 0x81d00000ull, "the backend was given the page-aligned base, not the address asked for");
    check(m.last_size == PAGE, "one page covers 0x123 + 0x40");
    check((uintptr_t)qcom_map_pointer(&mapping) == 0x10000000u + 0x123u,
          "the caller's pointer sits at the requested offset, not at the page boundary");
    check(qcom_unmap(&mapping) == QcomMapOk && m.releases == 1 && m.last_released == (void *)0x10000000u,
          "releasing passes back exactly the window the backend handed out");
    check(qcom_map_pointer(&mapping) == 0 && mapping.mapped == 0, "nothing is left mapped");
}

static void test_cross_page(void)
{
    struct mock m = {0x20000000u, 0, 0, 0, 0, 0, 0};
    struct QcomMappingBackend backend = backend_of(&m);
    struct QcomMapping mapping = {0};

    /* 0x3f00 into the page plus 0x200 crosses into the next one. */
    check(qcom_map(&mapping, &backend, PAGE, 0x81d03f00ull, 0x200) == QcomMapOk, "a cross-page range maps");
    check(m.last_base == 0x81d00000ull && m.last_size == 2u * PAGE, "the window grows to two pages");
    check((uintptr_t)qcom_map_pointer(&mapping) == 0x20000000u + 0x3f00u, "the pointer keeps its offset");
    (void)qcom_unmap(&mapping);

    /* A length that is an exact multiple with an offset needs no extra page beyond its own. */
    struct mock n = {0x30000000u, 0, 0, 0, 0, 0, 0};
    struct QcomMappingBackend second = backend_of(&n);
    struct QcomMapping other = {0};
    check(qcom_map(&other, &second, PAGE, 0x81d04000ull, 2u * PAGE) == QcomMapOk, "an aligned multiple maps");
    check(n.last_base == 0x81d04000ull && n.last_size == 2u * PAGE, "the window equals the request");
    check((uintptr_t)qcom_map_pointer(&other) == 0x30000000u, "an aligned start points at the window itself");
    (void)qcom_unmap(&other);
}

static void test_refusals_do_not_reach_backend(void)
{
    struct mock m = {0x40000000u, 0, 0, 0, 0, 0, 0};
    struct QcomMappingBackend backend = backend_of(&m);
    struct QcomMapping mapping = {0};

    check(qcom_map(&mapping, &backend, PAGE, 0x81d00000ull, 0) == QcomMapZeroLength, "zero length is refused");
    check(qcom_map(&mapping, &backend, PAGE, UINT64_MAX - 4ull, 8) == QcomMapOverflow,
          "a range that would wrap is refused");
    check(qcom_map(&mapping, 0, PAGE, 0x81d00000ull, 0x10) == QcomMapNoBackend, "no backend is refused");
    check(m.acquires == 0 && mapping.mapped == 0, "none of the refusals reached the backend");
}

static void test_backend_failure_leaves_nothing(void)
{
    struct mock m = {0x50000000u, 0, 0, 0, 0, 0, 1};
    struct QcomMappingBackend backend = backend_of(&m);
    struct QcomMapping mapping = {0};

    check(qcom_map(&mapping, &backend, PAGE, 0x81d00000ull, 0x10) == QcomMapBackendError,
          "a failing backend is reported as such");
    check(mapping.mapped == 0 && qcom_map_pointer(&mapping) == 0, "a failed mapping leaves no pointer behind");
    check(qcom_unmap(&mapping) == QcomMapNotMapped && m.releases == 0,
          "and there is nothing to unmap, so the backend sees no release");
}

static void test_double_map_and_unmap(void)
{
    struct mock m = {0x60000000u, 0, 0, 0, 0, 0, 0};
    struct QcomMappingBackend backend = backend_of(&m);
    struct QcomMapping mapping = {0};

    check(qcom_map(&mapping, &backend, PAGE, 0x81d00000ull, 0x10) == QcomMapOk, "mapped once");
    check(qcom_map(&mapping, &backend, PAGE, 0x81d00000ull, 0x10) == QcomMapAlreadyMapped,
          "mapping the same handle twice is refused");
    check(m.acquires == 1, "so the backend acquired once");
    check(qcom_unmap(&mapping) == QcomMapOk && m.releases == 1, "the first unmap releases");
    check(qcom_unmap(&mapping) == QcomMapNotMapped && m.releases == 1,
          "a second unmap is refused rather than releasing twice");
}

static void test_page_sizes(void)
{
    struct mock m = {0x70000000u, 0, 0, 0, 0, 0, 0};
    struct QcomMappingBackend backend = backend_of(&m);
    struct QcomMapping mapping = {0};

    /* The same request under a 4 KiB and a 16 KiB page lands in different windows, which is the point of passing it in. */
    check(qcom_map(&mapping, &backend, 0x1000, 0x81d03210ull, 0x40) == QcomMapOk, "4 KiB pages");
    check(m.last_base == 0x81d03000ull && m.last_size == 0x1000, "a 4 KiB page holds this range");
    (void)qcom_unmap(&mapping);

    check(qcom_map(&mapping, &backend, PAGE, 0x81d03210ull, 0x40) == QcomMapOk, "16 KiB pages");
    check(m.last_base == 0x81d00000ull && m.last_size == PAGE, "a 16 KiB page holds it with a different window");
    check((uintptr_t)qcom_map_pointer(&mapping) == 0x70000000u + 0x3210u, "the offset follows the window either way");
    (void)qcom_unmap(&mapping);

    /* A zero page size means the platform default, and the result must match an explicit 16 KiB request. */
    check(qcom_map(&mapping, &backend, 0, 0x81d03210ull, 0x40) == QcomMapOk, "the default page size");
    check(m.last_base == 0x81d00000ull && m.last_size == PAGE, "the default is 16 KiB");
    (void)qcom_unmap(&mapping);
}

int main(void)
{
    test_unaligned_within_page();
    test_cross_page();
    test_refusals_do_not_reach_backend();
    test_backend_failure_leaves_nothing();
    test_double_map_and_unmap();
    test_page_sizes();
    printf("PASS QcomMapping: %d checks\n", checks);
    return failed ? 1 : 0;
}
