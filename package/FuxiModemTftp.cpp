// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Fuxi's TFTP adapter: the modem's carrier configuration, read out of the device's own firmware partition.
 *
 * The modem reads its carrier configuration over a TFTP service and stays offline without it: the
 * reference says so in one line (hakios.mpss.c, tftp: "the modem reads its carrier configuration here and
 * stays offline without it"), and the device agrees — on 2026-11-22 the modem's operating mode was 5,
 * PERSIST, and it had no registration and no signal.
 *
 * The files themselves are already on the phone: the modem's firmware partition is a FAT volume, and the
 * port already mounts it read only for the modem's images and for the service registry's tables. This
 * adapter is that same volume, read a second time under the names the modem asks its files by.
 *
 * The names differ, and the difference is the whole mapping. The modem asks for
 * "/readonly/firmware/image/<X>", and the reference turns that into "image/<X>" in the modem's own FAT
 * partition (hakios.mpss.c, tftp_file_of:4264-4281). So:
 *
 *   /readonly/firmware/image/            -> image/
 *   /readonly/vendor/firmware_mnt/image/ -> image/
 *   /readonly/vendor/firmware/           -> image/
 *   /readonly/firmware/                  -> image/
 *
 * Every one of them lands in the same directory, because that is what the reference does: the four
 * prefixes are four spellings of one place. A path that climbs out with ".." is refused.
 *
 * Nothing here writes anything to the modem's storage. The modem's carrier configuration holds the radio's
 * own calibration and identifiers, and a write of it onto the radio's storage would be a write of the
 * radio's own configuration, which is not this port's to do. What the modem writes is taken into memory,
 * for the run, and served back when the modem reads the same name: the modem writes "/readwrite/mcfg.tmp"
 * and then reads it, and the reference keeps it in memory too (hakios.mpss.c:4214-4236).
 */
#include <IOKit/IOLib.h>
#include <sys/errno.h>
#include <string.h>

#include "FuxiModemTftp.h"
#include "FatVolume.h"

extern int printf(const char *format, ...);
/* The partition reader of the modem's firmware volume. It is read only by construction: it exposes no
 * write counterpart. */
extern "C" int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
    __attribute__((weak_import));
extern "C" int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst)
    __attribute__((weak_import));

/* The volume the modem's files are in. The two slots are the same content on this board (the port's own
 * survey: evidence/2026-11-19-modem-slot-survey.md, both slots 0xFFCFFFBB), and the modem's own firmware
 * is read from "modem_b" first, because the active slot on this phone can be B. */
struct TftpPartition {
    uint32_t lun;
    uint64_t first;
    uint64_t cached;  // block in block (UINT64_MAX: none)
    uint8_t block[4096];
};
static struct TftpPartition *gPart;
static FatVolume *gVolume;
static uint32_t gOpens, gCloses, gReads, gBytes, gRefused, gMissing;

/* Reads the volume out of the partition, the same way the firmware reader does. */
static int partitionRead(void *context, uint64_t offset, void *to, size_t length)
{
    struct TftpPartition *p = static_cast<struct TftpPartition *>(context);
    uint8_t *out = static_cast<uint8_t *>(to);
    while (length) {
        const uint64_t lba = offset / 4096;
        const size_t within = static_cast<size_t>(offset % 4096);
        if (!within && length >= 4096) {
            const uint32_t n = length / 4096 > 256 ? 256 : static_cast<uint32_t>(length / 4096);
            if (!native_ufs_part_read(p->lun, p->first + lba, n, out)) return EIO;
            out += static_cast<size_t>(n) * 4096;
            offset += static_cast<uint64_t>(n) * 4096;
            length -= static_cast<size_t>(n) * 4096;
            continue;
        }
        if (p->cached != lba) {
            p->cached = UINT64_MAX;
            if (!native_ufs_part_read(p->lun, p->first + lba, 1, p->block)) return EIO;
            p->cached = lba;
        }
        const size_t take = 4096 - within < length ? 4096 - within : length;
        memcpy(out, p->block + within, take);
        out += take;
        offset += take;
        length -= take;
    }
    return 0;
}

/* Mounts the modem's firmware volume, once. */
static int mountVolume(void)
{
    if (gVolume) return 0;
    if (!native_ufs_part_find || !native_ufs_part_read) return ENXIO;
    struct TftpPartition *p = static_cast<struct TftpPartition *>(IOMallocType(struct TftpPartition));
    if (!p) return ENOMEM;
    static const char *const names[] = {"modem_b", "modem_a"};
    int rc = ENOENT;
    for (unsigned i = 0; i < 2 && rc; ++i) {
        uint64_t blocks = 0;
        // ENXIO while the UFS driver is not ready yet: the caller tries again later
        if (!native_ufs_part_find(names[i], &p->lun, &p->first, &blocks)) {rc = ENXIO; continue;}
        p->cached = UINT64_MAX;
        FatVolume *v = static_cast<FatVolume *>(IOMallocType(FatVolume));
        if (!v) {IOFreeType(p, struct TftpPartition); return ENOMEM;}
        rc = fat_mount(v, blocks * 4096, partitionRead, p) ? EFTYPE : 0;
        if (rc) {IOFreeType(v, FatVolume); v = NULL;}
        else {gPart = p; gVolume = v; return 0;}
    }
    IOFreeType(p, struct TftpPartition);
    return rc;
}

/* Turns the modem's own path into the path in the volume.
 *
 * The four prefixes all land in "image/", which is where the modem's firmware partition keeps its
 * files. A path that climbs out with ".." is refused, and so is one that names no known prefix. */
static int mapPath(const char *path, uint32_t path_length, char *out, uint32_t out_capacity)
{
    if (!path || !out || !out_capacity) return EINVAL;
    for (unsigned i = 0; i < FUXI_TFTP_PREFIX_COUNT; ++i) {
        const char *prefix = fuxi_tftp_prefixes[i];
        const size_t prefix_length = strlen(prefix);
        if (path_length < prefix_length) continue;
        if (memcmp(path, prefix, prefix_length) != 0) continue;
        const uint32_t rest = path_length - static_cast<uint32_t>(prefix_length);
        // "image/" plus the rest, plus the terminator
        if (rest + 6u + 1u > out_capacity) return ENAMETOOLONG;
        memcpy(out, "image/", 6);
        memcpy(out + 6, path + prefix_length, rest);
        out[6 + rest] = 0;
        /* The rest must not climb out. The reference checks the same thing (hakios.mpss.c:4276-4281).
           The kernel's libc has no strstr, so the two dots are looked for by hand. */
        for (uint32_t i = 0; i + 1u < rest + 6u; ++i) {
            if (out[i] == '.' && out[i + 1] == '.') return ENOENT;
        }
        return 0;
    }
    return ENOENT;
}

/* One file the modem asked for. */
struct TftpFile {
    FatFile file;
    uint64_t size;
    /* A file the modem wrote is held in memory, not on the volume. The handle names it, and the store is
       looked up by that name on every read, so a later write that moves the store's buffer cannot leave the
       handle pointing into the old one. */
    uint8_t written;
    char written_name[256];
};

/* The files the modem wrote, kept in memory for the run.
 *
 * The reference does the same, with eight slots and a megabyte each (hakios.mpss.c:4086-4098), and the
 * modem needs it: it writes "/readwrite/mcfg.tmp" and then reads the same path back, and a server that
 * discards the write cannot answer that read.
 *
 * Nothing here reaches the modem's storage. The carrier configuration is the radio's own, and a write of
 * it is not this port's to do; the reference makes the same choice, and the modem accepts it. */
#define FUXI_TFTP_WRITTEN_MAX 8u
#define FUXI_TFTP_WRITE_MAX (1u << 20)
struct TftpWritten {
    char name[256];
    uint8_t *data;
    uint64_t size;
    uint64_t capacity;
};
static struct TftpWritten gWritten[FUXI_TFTP_WRITTEN_MAX];
static uint32_t gStored, gStoredBytes;

/* The name the modem writes under, and whether the path is one of those. */
static int writtenPrefix(const char *path, uint32_t path_length, uint32_t *rest)
{
    static const char *const prefixes[] = {"/readwrite/", "/only/"};
    for (unsigned i = 0; i < 2; ++i) {
        const size_t n = strlen(prefixes[i]);
        if (path_length < n) continue;
        if (memcmp(path, prefixes[i], n) != 0) continue;
        if (rest) *rest = path_length - static_cast<uint32_t>(n);
        return 1;
    }
    return 0;
}

/* Finds the file the modem wrote under this name, if it wrote one. */
static struct TftpWritten *writtenFind(const char *name, uint32_t name_length)
{
    for (unsigned i = 0; i < FUXI_TFTP_WRITTEN_MAX; ++i) {
        if (!gWritten[i].name[0]) continue;
        if (strlen(gWritten[i].name) != name_length) continue;
        if (memcmp(gWritten[i].name, name, name_length) != 0) continue;
        return &gWritten[i];
    }
    return NULL;
}

/* Keeps what the modem wrote, so it can read it back. */
static int tftpStore(void *context, const char *name, uint32_t name_length, const void *data, uint32_t length)
{
    (void)context;
    if (!name || !name_length || name_length >= sizeof gWritten[0].name) return EINVAL;
    if (!length) return 0;
    struct TftpWritten *w = writtenFind(name, name_length);
    if (!w) {
        for (unsigned i = 0; i < FUXI_TFTP_WRITTEN_MAX; ++i) {
            if (gWritten[i].name[0]) continue;
            w = &gWritten[i];
            memcpy(w->name, name, name_length);
            w->name[name_length] = 0;
            break;
        }
    }
    if (!w) return ENOSPC;
    if (w->size + length > FUXI_TFTP_WRITE_MAX) return EFBIG;
    if (w->size + length > w->capacity) {
        uint64_t want = w->capacity ? w->capacity : 4096;
        while (want < w->size + length) want *= 2;
        if (want > FUXI_TFTP_WRITE_MAX) want = FUXI_TFTP_WRITE_MAX;
        uint8_t *b = static_cast<uint8_t *>(IOMalloc(static_cast<size_t>(want)));
        if (!b) return ENOMEM;
        if (w->data) {
            memcpy(b, w->data, static_cast<size_t>(w->size));
            IOFree(w->data, static_cast<size_t>(w->capacity));
        }
        w->data = b;
        w->capacity = want;
    }
    memcpy(w->data + w->size, data, length);
    w->size += length;
    gStored++;
    gStoredBytes += length;
    return 0;
}

/* Opens the file the modem asks for. The path is the modem's own, with the prefix still on it. */
static int tftpOpen(void *context, const char *path, uint32_t path_length, void **handle)
{
    (void)context;
    if (!path || !handle) return EINVAL;
    *handle = nullptr;
    /* A file the modem wrote is served out of memory, under the name it wrote it. */
    uint32_t rest = 0;
    if (writtenPrefix(path, path_length, &rest)) {
        struct TftpWritten *w = writtenFind(path, path_length);
        if (!w) {
            gMissing++;
            printf("FUXI_TFTP: the modem asked for %.*s, and it wrote no such file\n", (int)path_length, path);
            return ENOENT;
        }
        TftpFile *f = static_cast<TftpFile *>(IOMallocType(TftpFile));
        if (!f) return ENOMEM;
        memset(f, 0, sizeof *f);
        f->size = w->size;
        f->written = 1;
        if (path_length < sizeof f->written_name) {
            memcpy(f->written_name, path, path_length);
            f->written_name[path_length] = 0;
        }
        *handle = f;
        gOpens++;
        printf("FUXI_TFTP: the modem asked for %.*s, and the memory has it (%llu bytes)\n", (int)path_length, path,
               static_cast<unsigned long long>(w->size));
        return 0;
    }
    const int rc = mountVolume();
    if (rc) return rc;
    char local[256];
    const int mapped = mapPath(path, path_length, local, sizeof local);
    if (mapped) {
        gRefused++;
        printf("FUXI_TFTP: the modem asked for a path that is not served: %.*s\n", (int)path_length, path);
        return mapped;
    }
    FatFile file;
    const int found = fat_open(gVolume, local, &file);
    if (found) {
        gMissing++;
        printf("FUXI_TFTP: the modem asked for %s, and the volume does not have it\n", local);
        return found == FatNotFound ? ENOENT : EIO;
    }
    if (file.directory) return EISDIR;
    TftpFile *f = static_cast<TftpFile *>(IOMallocType(TftpFile));
    if (!f) return ENOMEM;
    f->file = file;
    f->size = file.size;
    *handle = f;
    gOpens++;
    printf("FUXI_TFTP: the modem asked for %s, and the volume has it (%llu bytes)\n", local,
           static_cast<unsigned long long>(f->size));
    return 0;
}

/* Reads one range of the file out. */
static int tftpRead(void *context, void *handle, uint64_t offset, void *to, uint32_t length)
{
    (void)context;
    TftpFile *f = static_cast<TftpFile *>(handle);
    if (!f || (!to && length)) return EINVAL;
    if (offset + length > f->size) return EIO;
    if (f->written) {
        struct TftpWritten *w = writtenFind(f->written_name, (uint32_t)strlen(f->written_name));
        if (!w || offset + length > w->size) return EIO;
        memcpy(to, w->data + offset, length);
    } else if (length && fat_read(gVolume, &f->file, offset, to, length)) {
        return EIO;
    }
    gReads++;
    gBytes += length;
    return 0;
}

/* The file's own size, which is what bounds the transfer. */
static uint64_t tftpSize(void *context, void *handle)
{
    (void)context;
    TftpFile *f = static_cast<TftpFile *>(handle);
    return f ? f->size : 0;
}

/* Releases the file. */
static void tftpClose(void *context, void *handle)
{
    (void)context;
    TftpFile *f = static_cast<TftpFile *>(handle);
    if (!f) return;
    IOFreeType(f, TftpFile);
    gCloses++;
}

/* The adapter's callbacks, for the component's protocol half. */
extern "C" const struct fuxi_tftp_ops *fuxi_tftp_ops(void)
{
    static const struct fuxi_tftp_ops ops = {
        tftpOpen, tftpRead, tftpSize, tftpClose, nullptr, tftpStore,
    };
    return &ops;
}

/* The counters, for the board's report. */
extern "C" void fuxi_tftp_counters(uint32_t *out, unsigned count)
{
    if (!out || count < 6) return;
    out[0] = gOpens;
    out[1] = gCloses;
    out[2] = gReads;
    out[3] = gBytes;
    out[4] = gRefused;
    out[5] = gMissing;
    if (count >= 8) {
        out[6] = gStored;
        out[7] = gStoredBytes;
    }
}
