/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the signed firmware container: an ELF64 image with a header segment, two loadable segments and a
 * hash segment, plus the split-image reader. Every refusal is checked, because this code decides what a signed
 * image may place in a carveout. */
#include <stdio.h>
#include <string.h>

#include "../QcomMdt.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static void le16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void le32(uint8_t *p, uint32_t v) {le16(p, v & 0xffffu); le16(p + 2, v >> 16);}
static void le64(uint8_t *p, uint64_t v) {le32(p, (uint32_t)v); le32(p + 4, (uint32_t)(v >> 32));}

#define PHOFF 0x40u
#define PHENT 56u
#define PHNUM 4u
#define HEADERS (PHOFF + PHENT * PHNUM)          /* 0x120 */
#define HASH_AT 0x2a0u
#define HASH_BYTES 0x20u
#define IMAGE_BYTES (HASH_AT + HASH_BYTES)       /* 0x2c0 */

static uint8_t image[IMAGE_BYTES];

static void phdr(unsigned i, uint32_t type, uint32_t flags, uint64_t offset, uint64_t address, uint64_t filesz,
                 uint64_t memsz)
{
    uint8_t *p = image + PHOFF + i * PHENT;
    le32(p + 0, type); le32(p + 4, flags);
    le64(p + 8, offset); le64(p + 16, address); le64(p + 24, address);
    le64(p + 32, filesz); le64(p + 40, memsz); le64(p + 48, 0x1000);
}

static void build(uint32_t hash_flags, uint32_t load_flags, uint64_t second_offset)
{
    memset(image, 0, sizeof image);
    image[0] = 0x7f; image[1] = 'E'; image[2] = 'L'; image[3] = 'F'; image[4] = 2; image[5] = 1;
    le64(image + 32, PHOFF);
    le16(image + 54, PHENT);
    le16(image + 56, PHNUM);
    phdr(0, 0, 0, 0, 0, HEADERS, 0);
    phdr(1, 1, load_flags, HEADERS, 0x1000, 0x100, 0x200);
    phdr(2, 1, 0, second_offset, 0x2000, 0x80, 0x80);
    phdr(3, 0, hash_flags, HASH_AT, 0x3000, HASH_BYTES, HASH_BYTES);
    for (unsigned i = 0; i < HASH_BYTES; ++i) image[HASH_AT + i] = (uint8_t)(0xa0 + i);
}

static int read_image(void *context, uint64_t offset, void *to, size_t length)
{
    (void)context;
    if (offset > IMAGE_BYTES || length > IMAGE_BYTES - offset) return -1;
    memcpy(to, image + offset, length);
    return 0;
}

static int read_failing(void *context, uint64_t offset, void *to, size_t length)
{
    (void)context; (void)offset; (void)to; (void)length;
    return -1;
}

static void test_parse(void)
{
    QcomMdtImage m;
    build(QCOM_MDT_TYPE_HASH, QCOM_MDT_RELOCATABLE, 0x220);
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtOk, "a well-formed image parses");
    check(m.elf64 == 1 && m.count == 4, "the image is ELF64 with four program headers");
    check(m.hash == 3, "the hash segment is identified");
    check(m.header_size == HEADERS, "the header segment gives the metadata header size");
    check(m.relocatable == 1, "the relocatable flag is seen");
    check(m.min_address == 0x1000u && m.max_address == 0x2080u, "the loadable extent is the two load segments");
    check(qcom_mdt_span(&m) == 0x2000u, "the span is rounded up to a page");
    check(!m.segment[2].load || m.segment[2].load == 1, "loadable segments are marked");
    check(m.segment[0].load == 0 && m.segment[3].load == 0, "the header and hash segments are not loadable");

    /* Metadata: the headers followed by the hash segment. */
    uint8_t blob[IMAGE_BYTES];
    check(qcom_mdt_metadata_size(&m) == HEADERS + HASH_BYTES, "the metadata size is headers plus hash");
    check(qcom_mdt_metadata(&m, read_image, 0, blob, 8) == QcomMdtTooSmall, "a metadata buffer that is too small is refused");
    check(qcom_mdt_metadata(&m, read_image, 0, blob, sizeof blob) == QcomMdtOk, "the metadata is built");
    check(blob[0] == 0x7f && blob[1] == 'E' && blob[2] == 'L' && blob[3] == 'F', "the metadata starts with the ELF header");
    check(memcmp(blob + HEADERS, image + HASH_AT, HASH_BYTES) == 0, "the metadata carries the hash segment");

    /* Placement of a relocatable image: by offset from the lowest segment. */
    uint64_t at = 0;
    check(qcom_mdt_place(&m, 1, 0x80000000ull, 0x4000, &at) == QcomMdtOk && at == 0, "the first segment lands at zero");
    check(qcom_mdt_place(&m, 2, 0x80000000ull, 0x4000, &at) == QcomMdtOk && at == 0x1000, "the second lands at its offset");
    check(qcom_mdt_place(&m, 0, 0x80000000ull, 0x4000, &at) == QcomMdtOutside, "a non-loadable segment cannot be placed");
    check(qcom_mdt_place(&m, 1, 0x80000000ull, 0x100, &at) == QcomMdtOutside, "a region too small for the segment is refused");

    /* The same image without the relocatable flag is placed by absolute address. */
    build(QCOM_MDT_TYPE_HASH, 0, 0x220);
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtOk && m.relocatable == 0, "a fixed image parses as fixed");
    check(qcom_mdt_place(&m, 2, 0x2000, 0x1000, &at) == QcomMdtOk && at == 0, "a fixed segment lands at its address");
    check(qcom_mdt_place(&m, 2, 0x3000, 0x1000, &at) == QcomMdtOutside, "a fixed segment below the region is refused");
}

static void test_refusals(void)
{
    QcomMdtImage m;

    build(QCOM_MDT_TYPE_HASH, 0, 0x220);
    image[1] = 'X';
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtNotElf, "a bad ELF magic is refused");
    build(QCOM_MDT_TYPE_HASH, 0, 0x220);
    image[5] = 2;
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtNotElf, "a big-endian image is refused");
    build(QCOM_MDT_TYPE_HASH, 0, 0x220);
    check(qcom_mdt_parse(&m, 16, read_image, 0) == QcomMdtTooSmall, "an image smaller than an ELF header is refused");
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_failing, 0) == QcomMdtReadError, "a failing read is reported");
    build(QCOM_MDT_TYPE_HASH, 0, 0x220);
    le16(image + 56, 0);
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtBadHeaders, "an image with no program headers is refused");
    build(QCOM_MDT_TYPE_HASH, 0, 0x220);
    le16(image + 56, QCOM_MDT_MAX_SEGMENTS + 1u);
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtTooMany, "more headers than the table holds is refused");
    build(QCOM_MDT_TYPE_HASH, 0, 0x220);
    le16(image + 54, 32);
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtBadHeaders, "a wrong program header size is refused");
    build(QCOM_MDT_TYPE_HASH, 0, 0x220);
    phdr(1, 1, 0, HEADERS, 0x1000, 0x300, 0x200);   /* filesz above memsz */
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtBadHeaders, "a segment larger on file than in memory is refused");
    build(0, 0, 0x220);                              /* no hash segment */
    phdr(3, 1, 0, HASH_AT, 0x3000, HASH_BYTES, HASH_BYTES);
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtNoHash,
          "an image without a hash segment is refused, because the secure world has nothing to authenticate");
    build(QCOM_MDT_TYPE_HASH, 0, 0x220);
    phdr(1, 1, 0, HEADERS, 0x1000, 0x100, 0);        /* no loadable segment at all */
    phdr(2, 0, 0, 0x220, 0x2000, 0x80, 0);
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtNoSegments, "an image with nothing to load is refused");
    build(QCOM_MDT_TYPE_HASH, 0, 0x220);
    phdr(1, 1, 0, HEADERS, 0x1000, 0x100, 0x200);
    check(qcom_mdt_parse(&m, IMAGE_BYTES, read_image, 0) == QcomMdtOk, "the reference image parses again");
}

/* Split images: the pieces are described by the program headers. */
static int piece_read(void *context, unsigned piece, uint64_t offset, void *to, size_t length)
{
    (void)context;
    uint8_t *out = to;
    for (size_t i = 0; i < length; ++i) out[i] = (uint8_t)(piece * 16u + offset + i);
    return 0;
}

static void test_split(void)
{
    QcomMdtSplit split;
    build(QCOM_MDT_TYPE_HASH, 0, 0x300);   /* leaves a gap between 0x220 and 0x300 */
    check(qcom_mdt_split(&split, image, HEADERS) == QcomMdtOk, "the split layout is read from the headers");
    check(split.count == 4, "every program header becomes a piece");
    check(split.piece[1].offset == HEADERS && split.piece[1].size == 0x100, "the first loadable piece is placed");
    check(split.piece[3].offset == HASH_AT && split.piece[3].size == HASH_BYTES, "the hash piece is placed");
    check(split.size == 0x380u, "the joined size is the furthest piece end, past the gap");

    uint8_t got[0x100];
    check(qcom_mdt_split_read(&split, piece_read, 0, 0x200, got, sizeof got) == QcomMdtOk, "a range across a gap reads");
    check(got[0] == (uint8_t)(1u * 16u + 0xe0u), "the first bytes come from the piece");
    check(got[0x20] == 0 && got[0xff] == 0, "the bytes in the gap read as zero");
    check(qcom_mdt_split_read(&split, piece_read, 0, split.size, got, 4) == QcomMdtOutside,
          "a read past the joined image is refused");
    check(qcom_mdt_split(&split, image, 8) == QcomMdtTooSmall, "headers shorter than an ELF header are refused");
    image[0] = 'X';
    check(qcom_mdt_split(&split, image, HEADERS) == QcomMdtNotElf, "split headers with a bad magic are refused");
}

int main(void)
{
    test_parse();
    test_refusals();
    test_split();
    printf("PASS QcomMdt: %d checks\n", checks);
    return failed ? 1 : 0;
}
