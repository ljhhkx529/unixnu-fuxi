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
 * Nothing here writes anything. The modem's carrier configuration holds the radio's own calibration and
 * identifiers, and a write of it would be a write of the radio's own configuration, which is not this
 * port's to do. The protocol half acknowledges a write and discards it.
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
};

/* Opens the file the modem asks for. The path is the modem's own, with the prefix still on it. */
static int tftpOpen(void *context, const char *path, uint32_t path_length, void **handle)
{
    (void)context;
    if (!path || !handle) return EINVAL;
    *handle = nullptr;
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
    if (length && fat_read(gVolume, &f->file, offset, to, length)) return EIO;
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
        tftpOpen, tftpRead, tftpSize, tftpClose, nullptr,
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
}
