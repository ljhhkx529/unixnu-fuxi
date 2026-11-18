/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The FAT volume's directory listing, on a volume built in memory.
 *
 * The firmware partition is the one place the modem's file names can be proven, and that proof needs a
 * listing of a directory. So the listing is exercised here on a FAT32 volume made by the test itself: a
 * root directory with a subdirectory, a file with a long name, a file with only a short name, a deleted
 * entry and a volume label. The test then reads the listing back and opens the files by the names it gave.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../package/FatVolume.h"

#define SECTOR 512
#define CLUSTER_SECTORS 1
#define CLUSTER_BYTES (SECTOR * CLUSTER_SECTORS)
#define RESERVED_SECTORS 32
#define FAT_SECTORS 520
#define TOTAL_SECTORS 66672
#define TOTAL_BYTES ((size_t)SECTOR * TOTAL_SECTORS)

/* The data area starts after the reserved sectors and the two FATs. */
#define DATA_SECTOR (RESERVED_SECTORS + 2 * FAT_SECTORS)
#define ROOT_CLUSTER 2
#define IMAGE_CLUSTER 4

static uint8_t gImage[TOTAL_BYTES];

static int image_read(void *context, uint64_t offset, void *to, size_t length)
{
    (void)context;
    if (offset + length > TOTAL_BYTES) return 1;
    memcpy(to, gImage + offset, length);
    return 0;
}

static void put16(uint8_t *at, uint16_t value) {at[0] = (uint8_t)value; at[1] = (uint8_t)(value >> 8);}
static void put32(uint8_t *at, uint32_t value) {put16(at, (uint16_t)value); put16(at + 2, (uint16_t)(value >> 16));}

/* One 32-byte directory entry. */
static uint8_t *entry(uint8_t *at)
{
    memset(at, 0, 32);
    return at;
}

/* The short name, in the 11 bytes of the entry. The unused bytes are padded with spaces, which is what
 * the format stores and what the reader compares against. */
static void short_of(uint8_t *at, const char *name)
{
    memset(at, ' ', 11);
    for (unsigned i = 0; i < 8 && name[i] && name[i] != '.'; ++i) at[i] = (uint8_t)name[i];
    const char *dot = strchr(name, '.');
    if (dot) for (unsigned i = 0; i < 3 && dot[1 + i]; ++i) at[8 + i] = (uint8_t)dot[1 + i];
}

/* The checksum the long-name entries carry, so the entry they belong to is accepted. */
static uint8_t short_sum(const uint8_t *d)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; ++i) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + d[i]);
    return sum;
}

/* A long-name entry: the sequence number, the checksum of the short entry it belongs to, and the text
 * spread over the 13 slots the format fixes. */
static uint8_t *long_entry(uint8_t *at, unsigned sequence, const char *text, uint8_t sum)
{
    memset(at, 0, 32);
    at[0] = (uint8_t)(sequence | (sequence == 1 ? 0x40 : 0));
    at[11] = 0x0f;
    at[13] = sum;
    static const uint8_t slots[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (unsigned i = 0; i < 13; ++i) {
        const size_t at_index = (sequence - 1) * 13 + i;
        const uint16_t character = at_index < strlen(text) ? (uint8_t)text[at_index] : 0;
        put16(at + slots[i], character);
    }
    return at;
}

static void build_image(void)
{
    memset(gImage, 0, sizeof gImage);

    /* The boot sector: FAT32, one sector per cluster, 512-byte sectors. */
    uint8_t *boot = gImage;
    boot[0] = 0xeb; boot[1] = 0x58; boot[2] = 0x90;
    put16(boot + 11, SECTOR);
    boot[13] = CLUSTER_SECTORS;
    put16(boot + 14, RESERVED_SECTORS);
    boot[16] = 2;                 /* two FATs */
    put16(boot + 17, 0);          /* the root is a cluster chain, not a fixed area */
    put32(boot + 32, TOTAL_SECTORS);
    put32(boot + 36, FAT_SECTORS);
    put32(boot + 44, ROOT_CLUSTER);
    boot[510] = 0x55; boot[511] = 0xaa;

    /* The two FATs. The root's chain ends at its own cluster; the subdirectory's does the same. */
    for (unsigned copy = 0; copy < 2; ++copy) {
        uint8_t *fat = gImage + (size_t)(RESERVED_SECTORS + copy * FAT_SECTORS) * SECTOR;
        put32(fat + ROOT_CLUSTER * 4, 0x0fffffff);
        put32(fat + (ROOT_CLUSTER + 1) * 4, 0x0fffffff);
        put32(fat + IMAGE_CLUSTER * 4, 0x0fffffff);
        put32(fat + (IMAGE_CLUSTER + 1) * 4, 0x0fffffff);
    }

    /* The root directory, in its own cluster. */
    uint8_t *root = gImage + (size_t)DATA_SECTOR * SECTOR + (ROOT_CLUSTER - 2) * CLUSTER_BYTES;

    uint8_t *volume = entry(root);
    short_of(volume, "NO NAME");
    volume[11] = 0x08;   /* the volume label: not listed */

    uint8_t *deleted = entry(root + 32);
    short_of(deleted, "GONE");
    deleted[0] = 0xe5;   /* deleted: not listed */

    /* The long-name entry comes before the short entry it belongs to: that is the order the format
     * stores them in. */
    uint8_t *short_name = entry(root + 96);
    short_of(short_name, "MODEM~1MDT");
    short_name[11] = 0x20;   /* an archive */
    const uint8_t sum = short_sum(short_name);
    long_entry(root + 64, 1, "modem_dtb.mdt", sum);
    put16(short_name + 20, 0);
    put16(short_name + 26, 0);
    put32(short_name + 28, 1234);

    uint8_t *plain = entry(root + 128);
    short_of(plain, "MODEM.MDT");
    plain[11] = 0x20;
    put32(plain + 28, 4321);

    uint8_t *subdir = entry(root + 160);
    short_of(subdir, "IMAGE");
    subdir[11] = 0x10;   /* a directory */
    put16(subdir + 26, IMAGE_CLUSTER);
    put32(subdir + 28, 0);

    /* The subdirectory, in its own cluster. */
    uint8_t *image = gImage + (size_t)DATA_SECTOR * SECTOR + (IMAGE_CLUSTER - 2) * CLUSTER_BYTES;
    uint8_t *piece = entry(image);
    short_of(piece, "MODEM.B00");
    piece[11] = 0x20;
    put32(piece + 28, 8192);
}

/* The listing, as the names and the kinds the caller gets. */
static int listing(FatVolume *v, const FatFile *dir, FatEntry *entries, int max)
{
    int count = 0;
    for (unsigned index = 0; index < 64; ++index) {
        FatEntry entry;
        if (fat_list(v, dir, index, &entry)) return -1;
        if (!entry.name[0]) break;
        if (count < max) entries[count] = entry;
        ++count;
    }
    return count;
}

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {printf("FAIL: %s\n", what); ++failures;}
}

int main(void)
{
    build_image();

    FatVolume volume;
    check(fat_mount(&volume, TOTAL_BYTES, image_read, NULL) == FatOk, "the volume mounts");
    check(volume.fat32, "the volume is FAT32");

    FatFile root;
    check(fat_open(&volume, "/", &root) == FatOk, "the root opens");
    check(root.directory, "the root is a directory");

    FatEntry entries[16];
    const int count = listing(&volume, &root, entries, 16);
    /* The volume label and the deleted entry are not listed; the long name, the plain one and the
     * subdirectory are. */
    check(count == 3, "the root lists three entries");

    int found_long = 0, found_plain = 0, found_dir = 0;
    for (int i = 0; i < count && i >= 0; ++i) {
        if (!strcmp(entries[i].name, "modem_dtb.mdt")) {
            found_long = 1;
            check(entries[i].size == 1234, "the long-named entry has its size");
            check(!entries[i].directory, "the long-named entry is a file");
        }
        if (!strcmp(entries[i].name, "MODEM.MDT")) {
            found_plain = 1;
            check(entries[i].size == 4321, "the short-named entry has its size");
        }
        if (!strcmp(entries[i].name, "IMAGE")) {
            found_dir = 1;
            check(entries[i].directory, "the subdirectory is a directory");
        }
    }
    check(found_long, "the long name is listed");
    check(found_plain, "the short name is listed");
    check(found_dir, "the subdirectory is listed");

    /* The subdirectory lists its own file. */
    FatFile image_dir;
    check(fat_open(&volume, "/IMAGE", &image_dir) == FatOk, "the subdirectory opens");
    const int sub_count = listing(&volume, &image_dir, entries, 16);
    check(sub_count == 1, "the subdirectory lists one entry");
    if (sub_count == 1) {
        check(!strcmp(entries[0].name, "MODEM.B00"), "the subdirectory names its file");
        check(entries[0].size == 8192, "the subdirectory entry has its size");
    }

    /* A file is not a directory, so listing it is refused. */
    FatFile file;
    check(fat_open(&volume, "/IMAGE/MODEM.B00", &file) == FatOk, "the file opens");
    check(fat_list(&volume, &file, 0, entries) == FatNotDirectory, "listing a file is refused");

    /* The listing can be walked again from the start: it does not disturb the volume. */
    const int again = listing(&volume, &root, entries, 16);
    check(again == count, "the listing repeats");

    if (failures) {printf("FAIL fat volume: %d case(s)\n", failures); return 1;}
    printf("PASS fat volume: the listing shows the long name, the short name and the subdirectory\n");
    return 0;
}
