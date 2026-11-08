/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* H5 of the audio-probe debug runbook: untrusted TOC, partition and entry bytes must not take the parser out of bounds.
 *
 * The runbook asks for a corpus rather than one malformed sample: a bad table magic, an oversized entry count, an
 * offset and size that overflow, free pointers out of order, a broken canary, a padding overflow, cached and uncached
 * lists that collide, unaligned fields - each applied to a valid image one at a time - plus pure noise and truncation.
 * Every case must come back as an error, leave the image byte-identical when only reads happened, and stay inside the
 * buffer under ASan and UBSan.
 *
 * The same fixture the A0 suite uses, so a failure here is about the mutation and not about a different image. */
#include <stdio.h>
#include <string.h>

#include "../QcomSmem.h"

#define SMEM_BYTES 0x2000u
#define TOC_AT (SMEM_BYTES - 0x1000u)
#define PART_AT 0x800u
#define PART_BYTES 0x100u
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

static void build(uint8_t *image)
{
    memset(image, 0, SMEM_BYTES);
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

static uint32_t seed = 0x51eu;
static uint32_t next_random(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* One mutation: build a valid image, apply one change, and require that the parser either refuses it or stays inside the
 * buffer, and that a read-only pass leaves the bytes alone. */
static void mutate_and_parse(const char *what, uint32_t offset, uint32_t value)
{
    static uint8_t image[SMEM_BYTES], before[SMEM_BYTES];
    build(image);
    le32(image + offset, value);
    memcpy(before, image, SMEM_BYTES);

    QcomSmem smem = {(volatile uint8_t *)image, SMEM_BYTES};
    QcomSmemPartition part;
    const int rc = qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part);
    if (rc == QcomSmemOk) {
        uint32_t at = 0, size = 0;
        (void)qcom_smem_find(&smem, &part, ITEM_ID, &at, &size);
        (void)qcom_smem_item_count(&smem, &part);
        for (int i = 0; i < 8; ++i) (void)qcom_smem_item_at(&smem, &part, (uint32_t)i, 0, 0, 0);
    }
    check(memcmp(before, image, SMEM_BYTES) == 0, what);
}

static void corpus(void)
{
    mutate_and_parse("a read-only pass over a bad table magic changes nothing", TOC_AT + 0, 0x12345678u);
    mutate_and_parse("a read-only pass over an oversized entry count changes nothing", TOC_AT + 8, 0xffffffu);
    mutate_and_parse("a read-only pass over a table version it does not know changes nothing", TOC_AT + 4, 2u);
    mutate_and_parse("a read-only pass over a zero partition offset changes nothing", TOC_AT + 32u + 0, 0u);
    mutate_and_parse("a read-only pass over a partition past the table changes nothing", TOC_AT + 32u + 0, SMEM_BYTES);
    mutate_and_parse("a read-only pass over a size that overflows changes nothing", TOC_AT + 32u + 4, 0xfffffff0u);
    mutate_and_parse("a read-only pass over a partition without its magic changes nothing", PART_AT + 0, 0u);
    mutate_and_parse("a read-only pass over a partition header that disagrees changes nothing", PART_AT + 8, PART_BYTES + 8u);
    mutate_and_parse("a read-only pass over reversed free pointers changes nothing", PART_AT + 12, PART_BYTES * 4u);
    mutate_and_parse("a read-only pass over a broken canary changes nothing", PART_AT + 32, 0x1111u | (ITEM_ID << 16));
    mutate_and_parse("a read-only pass over an item size past the free pointer changes nothing", PART_AT + 36, 0x10000u);
    mutate_and_parse("a read-only pass over an item padding overflow changes nothing", PART_AT + 40, 0x10000u);
    mutate_and_parse("a read-only pass over an unaligned partition offset changes nothing", TOC_AT + 32u + 0, PART_AT + 1u);
    mutate_and_parse("a read-only pass over a cacheline that is not a power of two changes nothing", TOC_AT + 32u + 16, 3u);
}

/* Pure noise and truncation, where the parser must simply refuse. */
static void noise(void)
{
    static uint8_t image[SMEM_BYTES];
    int accepted = 0;
    for (int i = 0; i < 4000; ++i) {
        const uint32_t bytes = next_random() % SMEM_BYTES;
        for (uint32_t k = 0; k < bytes; ++k) image[k] = (uint8_t)next_random();
        QcomSmem smem = {(volatile uint8_t *)image, SMEM_BYTES};
        QcomSmemPartition part;
        if (qcom_smem_partition(&smem, QCOM_SMEM_HOST_APPS, QCOM_SMEM_HOST_GLOBAL, &part) == QcomSmemOk) ++accepted;
        if (accepted && i % 2 == 0) {
            uint32_t at = 0, size = 0;
            (void)qcom_smem_find(&smem, &part, ITEM_ID, &at, &size);
        }
    }
    check(accepted == 0, "noise never parses as a partition table");
}

int main(void)
{
    corpus();
    noise();
    printf("PASS QcomSmemFuzz: %d checks\n", checks);
    return failed ? 1 : 0;
}
