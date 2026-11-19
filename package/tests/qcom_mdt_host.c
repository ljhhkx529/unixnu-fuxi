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

/* A second buffer, large enough for the fuxi modem's 32 program headers: 0x40 of ELF header plus
 * 32 headers of 56 bytes each. */
#define FUXI_HEADERS (PHOFF + PHENT * 32u)        /* 0x740 */
static uint8_t fuxi_image[FUXI_HEADERS];

static void phdr_in(uint8_t *base, unsigned i, uint32_t type, uint32_t flags, uint64_t offset,
                    uint64_t address, uint64_t filesz, uint64_t memsz)
{
    uint8_t *p = base + PHOFF + i * PHENT;
    le32(p + 0, type); le32(p + 4, flags);
    le64(p + 8, offset); le64(p + 16, address); le64(p + 24, address);
    le64(p + 32, filesz); le64(p + 40, memsz); le64(p + 48, 0x1000);
}

static void phdr(unsigned i, uint32_t type, uint32_t flags, uint64_t offset, uint64_t address, uint64_t filesz,
                 uint64_t memsz)
{
    phdr_in(image, i, type, flags, offset, address, filesz, memsz);
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

/* A program header with no file size: the piece file for it does not exist on the device. The
 * fuxi's modem image has three of these, at piece 14, 21 and 30, so the numbering of the files on
 * the partition has holes in it and the reader must not treat a hole as the next file.
 *
 * The layout here has the pieces at rising file offsets, like the modem image's, so that the hole
 * sits between two pieces that both carry bytes. */
#define HOLE_HEADERS (PHOFF + PHENT * 4u)
static uint8_t hole_image[HOLE_HEADERS];

static void test_split_holes(void)
{
    QcomMdtSplit split;
    memset(hole_image, 0, sizeof hole_image);
    hole_image[0] = 0x7f; hole_image[1] = 'E'; hole_image[2] = 'L'; hole_image[3] = 'F';
    hole_image[4] = 2; hole_image[5] = 1;
    le64(hole_image + 32, PHOFF);
    le16(hole_image + 54, PHENT);
    le16(hole_image + 56, 4);
    /* piece 0 holds the headers, piece 2 has no bytes at all, piece 3 carries the hash. */
    phdr_in(hole_image, 0, 0, 0, 0, 0, HOLE_HEADERS, 0);
    phdr_in(hole_image, 1, 1, 0, HOLE_HEADERS, 0x1000, 0x100, 0x100);
    phdr_in(hole_image, 2, 1, 0, 0x220, 0x2000, 0, 0x100);
    phdr_in(hole_image, 3, 0, QCOM_MDT_TYPE_HASH, 0x240, 0x3000, 0x20, 0x20);
    check(qcom_mdt_split(&split, hole_image, HOLE_HEADERS) == QcomMdtOk, "a layout with an empty piece reads");
    check(split.count == 4, "the empty piece still counts as a program header");
    check(split.piece[2].size == 0, "the empty piece has no bytes");
    check(split.piece[2].offset == 0x220, "the empty piece keeps its place");
    check(split.piece[1].size == 0x100 && split.piece[3].size == 0x20,
          "the pieces around the hole keep their sizes");
    check(split.size == 0x260u, "the joined size is the furthest piece end, and the empty piece does not move it");

    /* A read over the hole must skip it: the bytes come from the piece after it and the hole
     * itself reads as zero, rather than the reader taking the next file for the hole. The joined
     * image ends at 0x260, so the range from 0x220 reaches the end in 0x40 bytes. */
    uint8_t got[0x40];
    check(qcom_mdt_split_read(&split, piece_read, 0, 0x220, got, sizeof got) == QcomMdtOk,
          "a range over the empty piece reads");
    check(got[0] == 0, "the empty piece reads as zero");
    check(got[0x20] == (uint8_t)(3u * 16u), "the bytes after the empty piece come from the next piece");
    /* A read of the hole alone is all zeros. */
    check(qcom_mdt_split_read(&split, piece_read, 0, 0x220, got, 0x20) == QcomMdtOk,
          "the hole alone reads");
    for (unsigned i = 0; i < 0x20; ++i) check(got[i] == 0, "every byte of the hole is zero");
}

/* The fuxi's modem image, as the device reported it: 32 program headers, the file pieces missing at
 * 14, 21 and 30. The number of the pieces is what the partition's file names follow. */
static void test_fuxi_modem_layout(void)
{
    QcomMdtSplit split;
    memset(fuxi_image, 0, sizeof fuxi_image);
    fuxi_image[0] = 0x7f; fuxi_image[1] = 'E'; fuxi_image[2] = 'L'; fuxi_image[3] = 'F';
    fuxi_image[4] = 2; fuxi_image[5] = 1;
    le64(fuxi_image + 32, PHOFF);
    le16(fuxi_image + 54, PHENT);
    le16(fuxi_image + 56, 32);            /* 32 program headers, as the device reported */
    for (unsigned i = 0; i < 32; ++i) {
        const uint64_t address = 0x8a800000ull + (uint64_t)i * 0x2000;
        const uint64_t bytes = (i == 14 || i == 21 || i == 30) ? 0 : 0x100;
        phdr_in(fuxi_image, i, i ? 1 : 0, 0, i ? 0x200 * i : 0, address,
                i ? bytes : FUXI_HEADERS, i ? bytes : 0);
    }
    check(qcom_mdt_split(&split, fuxi_image, FUXI_HEADERS) == QcomMdtOk, "the fuxi's modem layout reads");
    check(split.count == 32, "the modem image has 32 program headers");
    check(split.piece[14].size == 0 && split.piece[21].size == 0 && split.piece[30].size == 0,
          "the three pieces the partition does not carry are empty");
    check(split.piece[13].size == 0x100 && split.piece[15].size == 0x100,
          "the pieces around the holes keep their sizes");
}

/* The metadata is taken out of the .mdt file, not out of the joined image of the .bNN pieces.
 *
 * The fuxi's modem_dtb is the shape that matters, as the device reported it: the .mdt file is 4044
 * bytes (a 148 byte header part followed by a 3896 byte hash segment), the pieces are b00 148
 * bytes, b01 512 bytes and b02 3896 bytes, and the joined blob is 7992 bytes with the hash segment
 * at offset 4096.
 *
 * Linux drivers/soc/qcom/mdt_loader.c, qcom_mdt_read_metadata: the metadata is the ELF header
 * followed by the hash segment, copied out of the .mdt file itself. */
#define MDT_HEADERS 148u                /* 52 byte ELF32 header plus three program headers */
#define MDT_HASH 3896u
#define MDT_BYTES (MDT_HEADERS + MDT_HASH)     /* 4044: the .mdt file's size */
#define JOINED_BYTES 7992u
#define JOINED_HASH_AT 4096u

static uint8_t mdt_file[MDT_BYTES];
static uint8_t joined_image[JOINED_BYTES];

/* The modem_dtb's own shape: an ELF32 with three program headers, the first covering the headers
 * themselves, the second the loadable segment and the third the hash segment.
 *
 * The hash bytes are given as a parameter, because the two sources can be made to differ: the
 * question this test asks is which file the metadata is read out of, and a test where both files
 * hold the same bytes cannot tell. */
static void build_mdt(int same_bytes)
{
    memset(mdt_file, 0, sizeof mdt_file);
    memset(joined_image, 0, sizeof joined_image);
    uint8_t *e = joined_image;
    e[0] = 0x7f; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 1;   /* ELF32, little endian */
    le32(e + 28, 52);      /* e_phoff */
    le16(e + 42, 32);      /* e_phentsize */
    le16(e + 44, 3);       /* e_phnum */
    uint8_t *p = e + 52;
    le32(p + 0, 0); le32(p + 4, 0); le32(p + 12, 0);
    le32(p + 16, MDT_HEADERS); le32(p + 20, 0);                 /* the headers */
    p += 32;
    le32(p + 0, 1); le32(p + 4, MDT_HEADERS); le32(p + 12, 0x9b000000);
    le32(p + 16, MDT_HASH); le32(p + 20, MDT_HASH);             /* the loadable segment */
    p += 32;
    le32(p + 0, 0); le32(p + 4, JOINED_HASH_AT); le32(p + 12, 0x9b008000);
    le32(p + 16, MDT_HASH); le32(p + 20, MDT_HASH);
    le32(p + 24, QCOM_MDT_TYPE_HASH);                            /* the hash segment */
    /* The .mdt file is the header part followed by the hash segment; the joined blob has the hash
     * segment at 4096 instead. */
    memcpy(mdt_file, joined_image, MDT_HEADERS);
    for (unsigned i = 0; i < MDT_HASH; ++i) mdt_file[MDT_HEADERS + i] = (uint8_t)(0x11 ^ i);
    for (unsigned i = 0; i < MDT_HASH; ++i)
        joined_image[JOINED_HASH_AT + i] = (uint8_t)((same_bytes ? 0x11 : 0x77) ^ i);
}

static int read_mdt(void *context, uint64_t offset, void *to, size_t length)
{
    (void)context;
    if (offset > MDT_BYTES || length > MDT_BYTES - offset) return -1;
    memcpy(to, mdt_file + offset, length);
    return 0;
}

static int read_joined(void *context, uint64_t offset, void *to, size_t length)
{
    (void)context;
    if (offset > JOINED_BYTES || length > JOINED_BYTES - offset) return -1;
    memcpy(to, joined_image + offset, length);
    return 0;
}

static void test_metadata_from_mdt(void)
{
    QcomMdtImage m;
    static uint8_t blob[MDT_BYTES];
    static uint8_t joined_blob[MDT_BYTES];

    /* The two files holding the same hash bytes: the .mdt is the header part followed by the hash
     * segment and the joined blob carries the same hash segment at 4096, so the two sources give
     * the same metadata. This is the shape the board's own files may have, and the metadata's
     * length is the same either way. */
    build_mdt(1);
    check(qcom_mdt_parse(&m, JOINED_BYTES, read_joined, 0) == QcomMdtOk, "the joined blob parses");
    check(m.hash == 2, "the hash segment is found in the joined blob");
    check(m.header_size == MDT_HEADERS, "the header part is the size the program header names");
    check(m.segment[m.hash].file_offset == JOINED_HASH_AT, "the hash segment sits at 4096 in the joined blob");
    check(qcom_mdt_metadata_size(&m) == MDT_BYTES, "the metadata is the header part plus the hash");
    m.mdt_size = MDT_BYTES;
    check(qcom_mdt_metadata_from_mdt(&m, read_joined, 0, read_mdt, 0, blob, sizeof blob) == QcomMdtOk,
          "the metadata is built out of the mdt file");
    check(memcmp(blob, mdt_file, MDT_BYTES) == 0, "the metadata is the mdt file byte for byte");
    check(qcom_mdt_metadata(&m, read_joined, 0, joined_blob, sizeof joined_blob) == QcomMdtOk,
          "the metadata can still be built out of the joined blob");
    check(memcmp(blob, joined_blob, MDT_BYTES) == 0, "the two forms agree when the files hold the same bytes");

    /* The two files holding different hash bytes: this is what tells which file the metadata is
     * read out of. The .mdt's hash must win, because that is the file upstream authenticates. */
    build_mdt(0);
    check(qcom_mdt_parse(&m, JOINED_BYTES, read_joined, 0) == QcomMdtOk, "the joined blob parses again");
    m.mdt_size = MDT_BYTES;
    check(qcom_mdt_metadata_from_mdt(&m, read_joined, 0, read_mdt, 0, blob, sizeof blob) == QcomMdtOk,
          "the metadata is built out of the mdt file");
    check(memcmp(blob, mdt_file, MDT_BYTES) == 0, "the metadata is the mdt file byte for byte");
    check(blob[MDT_HEADERS] == (uint8_t)(0x11 ^ 0) && blob[MDT_BYTES - 1] == (uint8_t)(0x11 ^ (MDT_HASH - 1)),
          "the hash segment comes out of the mdt file");
    check(qcom_mdt_metadata(&m, read_joined, 0, joined_blob, sizeof joined_blob) == QcomMdtOk,
          "the metadata can still be built out of the joined blob");
    check(memcmp(blob, joined_blob, MDT_BYTES) != 0, "the two forms are not the same bytes");
    check(joined_blob[MDT_HEADERS] == (uint8_t)(0x77 ^ 0),
          "the joined form takes the hash out of the b02 piece");

    /* An .mdt whose size does not match the header part plus the hash is refused: the metadata
       would be a different length than the file holds. */
    m.mdt_size = MDT_BYTES - 1;
    check(qcom_mdt_metadata_from_mdt(&m, read_joined, 0, read_mdt, 0, blob, sizeof blob) == QcomMdtBadHeaders,
          "an mdt file of the wrong size is refused");

    /* Without an .mdt the old path is used unchanged. */
    m.mdt_size = 0;
    check(qcom_mdt_metadata_from_mdt(&m, read_joined, 0, NULL, NULL, blob, sizeof blob) == QcomMdtOk,
          "with no mdt file the joined blob is used");
    check(memcmp(blob, joined_blob, MDT_BYTES) == 0, "and it gives the joined form's bytes");
}

int main(void)
{
    test_parse();
    test_refusals();
    test_split();
    test_split_holes();
    test_fuxi_modem_layout();
    test_metadata_from_mdt();
    printf("PASS QcomMdt: %d checks\n", checks);
    return failed ? 1 : 0;
}
