/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* H6: exercise one private-partition allocation on an ordinary-RAM shadow image.
 *
 * The unused partition bytes are deliberately nonzero.  This catches an allocator that silently clears payload bytes, while the
 * write trace proves that the free pointer is the sole publication and the final write.  No physical mapping or device access is
 * involved. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../QcomSmem.h"

#define SMEM_BYTES 0x2000u
#define TOC_AT (SMEM_BYTES - 0x1000u)
#define PART_AT 0x800u
#define PART_BYTES 0x100u
#define OLD_ITEM 0x1234u
#define NEW_ITEM 0x01adu
#define OLD_FREE (32u + 16u + 8u)
#define NEW_SIZE 13u
#define NEW_ALIGNED 16u
#define NEW_FREE (OLD_FREE + 16u + NEW_ALIGNED)
#define MAX_WRITES 8u

typedef struct {
    uint32_t offset;
    uint32_t value;
    int publishes;
} Write;

static Write writes[MAX_WRITES];
static unsigned write_count;
static int checks;
static int failed;

void qcom_smem_test_trace_write(uint32_t offset, uint32_t value, int publishes)
{
    if (write_count < MAX_WRITES) writes[write_count] = (Write){offset, value, publishes};
    ++write_count;
}

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {
        ++failed;
        fprintf(stderr, "FAIL %s\n", what);
    }
}

static void le32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static uint64_t checksum(const uint8_t *bytes, size_t count)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void build(uint8_t *image)
{
    memset(image, 0, SMEM_BYTES);
    const uint32_t hosts = QCOM_SMEM_HOST_APPS | (uint32_t)QCOM_SMEM_HOST_GLOBAL << 16;
    le32(image + PART_AT + 0, 0x54525024u);
    le32(image + PART_AT + 4, hosts);
    le32(image + PART_AT + 8, PART_BYTES);
    le32(image + PART_AT + 12, OLD_FREE);
    le32(image + PART_AT + 16, PART_BYTES);
    le32(image + PART_AT + 32, 0xa5a5u | (OLD_ITEM << 16));
    le32(image + PART_AT + 36, 8u);
    le32(image + PART_AT + 40, 0u);
    memset(image + PART_AT + OLD_FREE, 0xcc, PART_BYTES - OLD_FREE);

    le32(image + TOC_AT + 0, 0x434f5424u);
    le32(image + TOC_AT + 4, 1u);
    le32(image + TOC_AT + 8, 1u);
    le32(image + TOC_AT + 32, PART_AT);
    le32(image + TOC_AT + 36, PART_BYTES);
    le32(image + TOC_AT + 40, 0u);
    le32(image + TOC_AT + 44, hosts);
    le32(image + TOC_AT + 48, 0u);
}

static int allowed_diff(size_t offset)
{
    const size_t entry = PART_AT + OLD_FREE;
    const size_t free_offset = PART_AT + 12u;
    return (offset >= entry && offset < entry + 16u) ||
           (offset >= free_offset && offset < free_offset + 4u);
}

int main(void)
{
    static uint8_t image[SMEM_BYTES];
    static uint8_t before[SMEM_BYTES];
    static uint8_t after_success[SMEM_BYTES];
    build(image);
    memcpy(before, image, sizeof image);

    QcomSmem smem = {(volatile uint8_t *)image, SMEM_BYTES};
    QcomSmemPartition part;
    check(qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part) == QcomSmemOk,
          "the equivalent snapshot parses");
    const uint64_t before_checksum = checksum(before, sizeof before);
    check(qcom_smem_alloc(&smem, &part, NEW_ITEM, NEW_SIZE) == QcomSmemOk, "one shadow item allocates");
    check(checksum(image, sizeof image) != before_checksum, "the successful allocation changes the image checksum");

    unsigned unexpected = 0;
    for (size_t i = 0; i < sizeof image; ++i) {
        if (image[i] != before[i] && !allowed_diff(i)) ++unexpected;
    }
    check(unexpected == 0u, "the byte diff is confined to the entry header and free offset");
    check(memcmp(image + PART_AT + OLD_FREE + 16u, before + PART_AT + OLD_FREE + 16u, NEW_ALIGNED) == 0,
          "allocation leaves the nonzero payload bytes untouched");
    check(qcom_smem_read32(&smem, PART_AT + 12u) == NEW_FREE, "the free offset advances by one entry and aligned payload");

    uint32_t item_offset = 0;
    uint32_t item_size = 0;
    check(qcom_smem_find(&smem, &part, NEW_ITEM, &item_offset, &item_size) == QcomSmemOk &&
          item_offset == PART_AT + OLD_FREE + 16u && item_size == NEW_SIZE,
          "the parser finds the new item with its unpadded size");
    check(qcom_smem_item_count(&smem, &part) == 2, "reparsing sees exactly one additional item");

    check(write_count == 5u, "allocation performs four entry writes and one publication");
    check(writes[0].offset == PART_AT + OLD_FREE && writes[0].publishes == 0,
          "the first write starts the entry header");
    check(writes[3].offset == PART_AT + OLD_FREE + 12u && writes[3].publishes == 0,
          "all four entry words precede publication");
    check(writes[4].offset == PART_AT + 12u && writes[4].value == NEW_FREE && writes[4].publishes == 1,
          "the final and sole publication advances the free offset");

    memcpy(after_success, image, sizeof image);
    write_count = 0;
    check(qcom_smem_alloc(&smem, &part, NEW_ITEM, NEW_SIZE) == QcomSmemExists,
          "allocating the same item reports Exists");
    check(write_count == 0u && memcmp(after_success, image, sizeof image) == 0,
          "Exists performs no write and leaves the checksum image unchanged");

    write_count = 0;
    check(qcom_smem_alloc(&smem, &part, NEW_ITEM + 1u, PART_BYTES) == QcomSmemNoSpace,
          "an allocation larger than the remaining shadow space reports NoSpace");
    check(write_count == 0u && memcmp(after_success, image, sizeof image) == 0,
          "NoSpace performs no write and leaves the image byte-identical");

    printf("PASS QcomSmemShadow: %d checks before=%016llx after=%016llx\n", checks,
           (unsigned long long)before_checksum, (unsigned long long)checksum(image, sizeof image));
    return failed ? 1 : 0;
}
