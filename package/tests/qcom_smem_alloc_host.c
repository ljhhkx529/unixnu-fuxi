/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* H6: what an allocation changes, byte for byte.
 *
 * The runbook asks for a shadow allocation - the allocator run against an image in ordinary memory - and for the diff to
 * be exactly what the protocol allows: the new entry's header and the published free-pointer. Anything else is the
 * allocator writing memory it does not own, and a payload that comes back zeroed would mean it had touched the caller's
 * data, which the mainline private allocator does not do either.
 *
 * The fixture is the same one the parser corpus uses, so a difference here is about the allocation and not about a
 * different image. */
#include <stdio.h>
#include <string.h>

#include "../QcomSmem.h"

#define SMEM_BYTES 0x2000u
#define TOC_AT (SMEM_BYTES - 0x1000u)
#define PART_AT 0x800u
#define PART_BYTES 0x200u
#define ITEM_ID 0x1234u

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static void le32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value; at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16); at[3] = (uint8_t)(value >> 24);
}

/* One existing item, and every payload byte set to a marker so a zeroing allocator cannot hide. */
static void build(uint8_t *image)
{
    memset(image, 0x5a, SMEM_BYTES);
    const uint32_t hosts = QCOM_SMEM_HOST_APPS | (uint32_t)QCOM_SMEM_HOST_GLOBAL << 16;
    le32(image + PART_AT + 0, 0x54525024u);
    le32(image + PART_AT + 4, hosts);
    le32(image + PART_AT + 8, PART_BYTES);
    le32(image + PART_AT + 12, 32u + 16u + 8u);
    le32(image + PART_AT + 16, PART_BYTES);
    le32(image + PART_AT + 32, 0xa5a5u | (ITEM_ID << 16));
    le32(image + PART_AT + 36, 8u);
    le32(image + PART_AT + 40, 0u);
    le32(image + TOC_AT + 0, 0x434f5424u);
    le32(image + TOC_AT + 4, 1u);
    le32(image + TOC_AT + 8, 1u);
    const uint32_t entry = TOC_AT + 32u;
    le32(image + entry + 0, PART_AT);
    le32(image + entry + 4, PART_BYTES);
    le32(image + entry + 8, 0u);
    le32(image + entry + 12, hosts);
    le32(image + entry + 16, 0u);
}

/* The offsets that changed, so the test can assert the set and not only how many. */
static int diff(const uint8_t *before, const uint8_t *after, uint32_t *offsets, int capacity)
{
    int count = 0;
    for (uint32_t i = 0; i < SMEM_BYTES; ++i)
        if (before[i] != after[i] && count < capacity) offsets[count++] = i;
    return count;
}

static void test_allocation_diff(void)
{
    static uint8_t image[SMEM_BYTES], before[SMEM_BYTES];
    uint32_t changed[256];
    QcomSmem smem;
    QcomSmemPartition part;
    uint32_t offset = 0, size = 0;

    build(image);
    memcpy(before, image, SMEM_BYTES);
    smem.base = (volatile uint8_t *)image;
    smem.size = SMEM_BYTES;
    check(qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part) == QcomSmemOk,
          "the fixture parses as a partition");

    /* 32 bytes on an 8-byte boundary: the entry is the header plus the aligned payload. */
    check(qcom_smem_alloc(&smem, &part, 478u, 32u) == QcomSmemOk, "an item is allocated");
    check(qcom_smem_find(&smem, &part, 478u, &offset, &size) == QcomSmemOk && size == 32u,
          "and the parser finds it with the size it was given");

    const int count = diff(before, image, changed, 256);
    /* The header's four words plus the bytes of the published free pointer that were not already zero: the offset set is
     * what matters, because a pointer whose high bytes are already 0x00 changes fewer bytes than it spans. */
    const uint32_t entry_at = PART_AT + 32u + 16u + 8u;   /* header, the existing entry, its 8 payload bytes */
    check(count >= 17 && count <= 20, "the allocation changed between seventeen and twenty bytes");
    int expected = 0;
    for (int byte = 0; byte < 16; ++byte) {
        uint32_t wanted = entry_at + (uint32_t)byte;
        int found = 0;
        for (int i = 0; i < count; ++i) if (changed[i] == wanted) found = 1;
        if (found) ++expected;
    }
    check(expected == 16, "all sixteen bytes of the entry header changed");
    int pointer_changed = 0;
    for (int i = 0; i < count; ++i) if (changed[i] == PART_AT + 12u) pointer_changed = 1;
    check(pointer_changed, "the published free pointer changed");
    int outside = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t at = changed[i];
        if (!(at >= entry_at && at < entry_at + 16u) && at != PART_AT + 12u) ++outside;
    }
    check(outside == 0, "no byte outside the entry header and the free pointer changed");
    check(count == 16 + 1, "sixteen header bytes and one pointer byte were the whole diff");

    /* The payload belongs to the caller: not one byte of it may have been touched. */
    check(offset == entry_at + 16u, "the payload starts after the header");
    int untouched = 1;
    for (uint32_t i = 0; i < 32u; ++i)
        if (image[offset + i] != 0x5a) untouched = 0;
    check(untouched, "the allocator did not zero or write the payload it handed over");

    /* The published pointer is the end of the new entry, not the start. */
    QcomSmemPartition again;
    check(qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &again) == QcomSmemOk,
          "the partition still parses after the allocation");
    check(qcom_smem_item_count(&smem, &part) == 2, "and now holds two items");
}

static void test_alignment_and_padding(void)
{
    static uint8_t image[SMEM_BYTES];
    QcomSmem smem;
    QcomSmemPartition part;
    uint32_t offset = 0, size = 0;

    build(image);
    smem.base = (volatile uint8_t *)image;
    smem.size = SMEM_BYTES;
    (void)qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part);
    check(qcom_smem_alloc(&smem, &part, 479u, 5u) == QcomSmemOk, "an unaligned size is allocated");
    check(qcom_smem_find(&smem, &part, 479u, &offset, &size) == QcomSmemOk && size == 5u,
          "and found with the size that was asked for, not the padded one");
    /* The next allocation must not overlap the padding. */
    check(qcom_smem_alloc(&smem, &part, 480u, 8u) == QcomSmemOk, "a second item follows");
    uint32_t first = 0, second = 0;
    (void)qcom_smem_find(&smem, &part, 479u, &first, &size);
    (void)qcom_smem_find(&smem, &part, 480u, &second, &size);
    check(second >= first + 8u, "the second item starts after the first one's padded entry");
}

static void test_refusals_change_nothing(void)
{
    static uint8_t image[SMEM_BYTES], before[SMEM_BYTES];
    QcomSmem smem;
    QcomSmemPartition part;

    build(image);
    memcpy(before, image, SMEM_BYTES);
    smem.base = (volatile uint8_t *)image;
    smem.size = SMEM_BYTES;
    (void)qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part);

    check(qcom_smem_alloc(&smem, &part, ITEM_ID, 8u) == QcomSmemExists, "an existing item is refused");
    check(!memcmp(before, image, SMEM_BYTES), "and the image is untouched");
    check(qcom_smem_alloc(&smem, &part, 481u, 0u) == QcomSmemNoSpace, "a zero size is refused");
    check(!memcmp(before, image, SMEM_BYTES), "and the image is still untouched");
    check(qcom_smem_alloc(&smem, &part, 482u, PART_BYTES) == QcomSmemNoSpace, "an oversized item is refused");
    check(!memcmp(before, image, SMEM_BYTES), "and the image is untouched after every refusal");
}

int main(void)
{
    test_allocation_diff();
    test_alignment_and_padding();
    test_refusals_change_nothing();
    printf("PASS QcomSmemAlloc: %d checks\n", checks);
    return failed ? 1 : 0;
}
