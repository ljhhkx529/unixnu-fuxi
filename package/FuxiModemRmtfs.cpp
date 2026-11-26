/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Fuxi's RMTFS adapter: the four files the modem asks for, and the buffer it moves their sectors through.
 *
 * The modem keeps four files on the phone's storage that it cannot reach itself: the two modem state
 * partitions and the two calibration ones. It asks the application processor for them over QRTR, on
 * service 14, and this file is the board's half of that service.
 *
 * Two things are here, and neither of them writes the storage:
 *
 *   the four files, read once into memory of ours when the bring-up begins. The partition reader
 *   exposes no write counterpart at all, so there is no path from here to the storage's bytes.
 *
 *   the buffer, a fixed physical window that both the application processor and the modem can reach.
 *   The modem's requests carry addresses of 32 bits, so it has to be below 4 GiB, and the kernel's free
 *   memory there is not to be had in one piece of 2.5 MB. So it is the first 2.5 MB of the memory the
 *   device tree reserves for the compute DSP (reserved-memory cdsp_region@9c900000, 32 MB, no-map):
 *   nothing in this port or in uefi-fuxi starts the compute DSP, so the region is unused, and the
 *   kernel's allocator cannot reach it either, because DarwinBoot hands the kernel only the
 *   conventional memory and the reserved region never is.
 *
 * The reference port for the same modem in XNU takes the same window out of the compute DSP's memory on
 * its own board (hakios.mpss.c, rmtfs_buffer_make: RMTFS_BUFFER_PA_DEFAULT 0x9c700000, the same size).
 *
 * A write request from the modem changes the memory copy and nothing else. The four files hold the
 * IMEI and the radio calibration, so a bad write to them bricks the radio, and the write is refused
 * at the last moment: it goes into the copy, and the copy is not what the modem will find next boot.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <string.h>

#include "FuxiModemRmtfs.h"

extern int printf(const char *format, ...);
/* The board's assign for the RMTFS buffer, in FuxiModemProbe.cpp. */
extern "C" int fuxi_modem_rmtfs_assign(uint64_t address, uint64_t size, unsigned owners);
/* The partition reader. It is read only by construction: it exposes no write counterpart. */
extern "C" int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
    __attribute__((weak_import));
extern "C" int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst)
    __attribute__((weak_import));

namespace {

/* The four files, as the modem names them, and the partitions behind them. The paths and the partition
   names are the board's own facts, and they are in the header the component's protocol half reads
   too, so the two halves cannot disagree about which file is which.

   The partition names are the GPT's: the partition reader matches them case-insensitively
   (native_ufs_part_find, nativeUfsGptName). */
struct File {
    const char *path;
    const char *partition;
    uint32_t lun;
    uint64_t first;
    uint64_t blocks;
    uint8_t *data;
    uint64_t size;
    bool present;
};

File gFiles[FUXI_RMTFS_FILES];
/* Which files the modem has written since the last flush. The writes go into the copy in memory only, so this is
   what carries them to the Data volume: without it a reboot hands the modem the partitions as they were flashed,
   which is the one place this differs from Linux, where rmtfs writes the partitions themselves. */
static uint8_t gDirty[FUXI_RMTFS_FILES];
static uint32_t gPersisted, gPersistFailed, gPersistError;

/* The buffer: its mapping and its address. The address is what the modem is told, and the mapping is
   what this side reads and writes it through. */
IOMemoryMap *gBufferMap;
volatile uint8_t *gBuffer;
uint64_t gBufferPhys;
uint32_t gBufferSize;
bool gShared;

/* How much of the buffer is ours to answer with. */
uint32_t gBufferGiven;

/* The counters, so a run's log says what the modem asked for. */
uint32_t gOpens, gCloses, gReads, gWrites, gSectorsRead, gSectorsWritten, gRefused;

/* The window, from the board's header: the region the buffer must not touch. */
bool forbidden(uint64_t base)
{
    for (unsigned i = 0; i < FUXI_RMTFS_FORBIDDEN_COUNT; ++i) {
        const struct fuxi_rmtfs_range range = fuxi_rmtfs_forbidden(i);
        if (fuxi_rmtfs_overlaps(base, FUXI_RMTFS_BUFFER_SIZE, range.base, range.size)) {
            printf("FUXI_RMTFS: the buffer at %llx overlaps %s\n", static_cast<unsigned long long>(base),
                   range.name);
            return true;
        }
    }
    return false;
}

/* Reads a partition whole into memory of ours.
 *
 * The partition's own blocks are 4096 bytes, and the protocol's sectors are 512, so a sector never
 * straddles a block and the copy is sector aligned. */
bool loadFile(unsigned index)
{
    File &file = gFiles[index];
    const char *const path = fuxi_rmtfs_files[index].path;
    const char *const partition = fuxi_rmtfs_files[index].partition;
    uint64_t blocks = 0;
    if (!native_ufs_part_find || !native_ufs_part_read) {
        printf("FUXI_RMTFS: the partition reader is not there\n");
        return false;
    }
    if (!native_ufs_part_find(partition, &file.lun, &file.first, &blocks)) {
        printf("FUXI_RMTFS: the partition %s is not on this board\n", partition);
        return false;
    }
    if (!blocks || blocks > (64u << 20) / 4096u) {
        printf("FUXI_RMTFS: the partition %s has %llu blocks\n", partition,
               static_cast<unsigned long long>(blocks));
        return false;
    }
    file.blocks = blocks;
    const uint64_t bytes = blocks * 4096u;
    file.data = static_cast<uint8_t *>(IOMallocData(bytes));
    if (file.data) memset(file.data, 0, bytes);
    if (!file.data) {
        printf("FUXI_RMTFS: no memory for %s (%llu bytes)\n", partition,
               static_cast<unsigned long long>(bytes));
        return false;
    }
    file.size = bytes;
    /* The whole partition, in blocks of 256 at a time: the reader's own limit is its data buffer. */
    uint64_t at = 0;
    while (at < blocks) {
        const uint32_t n = (blocks - at) > 256u ? 256u : static_cast<uint32_t>(blocks - at);
        if (!native_ufs_part_read(file.lun, file.first + at, n, file.data + at * 4096u)) {
            printf("FUXI_RMTFS: the partition %s: read error at block %llu\n", partition,
                   static_cast<unsigned long long>(at));
            IOFreeData(file.data, bytes);
            file.data = nullptr;
            file.size = 0;
            return false;
        }
        at += n;
    }
    file.present = true;
    printf("FUXI_RMTFS: the partition %s (%s) is %llu bytes, read into memory of ours\n", partition, path,
           static_cast<unsigned long long>(bytes));
    {
        /* Read-only diagnostic: the first bytes and how much of the partition is not zero. An empty
           (zeroed) fsg or modemst means the modem has no factory/provisioning data to rebuild its EFS
           from, which is the first thing to rule out when the card reaches the modem but its USIM
           application never leaves DETECTED. Nothing is written. */
        uint32_t nonzero = 0;
        for (uint64_t i = 0; i < bytes; ++i) nonzero += file.data[i] != 0 ? 1u : 0u;
        printf("FUXI_RMTFS: %s first 16: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, nonzero %u of %llu\n",
               partition, file.data[0], file.data[1], file.data[2], file.data[3],
               file.data[4], file.data[5], file.data[6], file.data[7],
               file.data[8], file.data[9], file.data[10], file.data[11],
               file.data[12], file.data[13], file.data[14], file.data[15],
               nonzero, static_cast<unsigned long long>(bytes));
    }
    return true;
}

/* The buffer, mapped once.
 *
 * The mapping is write-combine, not uncached: the port found the uncached mapping (kIOMapInhibitCache)
 * to hang the modem's thread on 2026-11-20, and the write-combine mapping is what the low metadata
 * window uses and what works. */
bool mapBuffer()
{
    if (gBuffer) return true;
    const uint64_t base = FUXI_RMTFS_BUFFER_BASE;
    if (forbidden(base)) return false;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, FUXI_RMTFS_BUFFER_SIZE);
    if (!memory) {
        printf("FUXI_RMTFS: the buffer at %llx could not be described\n", static_cast<unsigned long long>(base));
        return false;
    }
    gBufferMap = memory->map(kIOMapAnywhere | kIOMapWriteCombineCache);
    memory->release();
    if (!gBufferMap) {
        printf("FUXI_RMTFS: the buffer at %llx could not be mapped\n", static_cast<unsigned long long>(base));
        return false;
    }
    gBuffer = reinterpret_cast<volatile uint8_t *>(gBufferMap->getVirtualAddress());
    gBufferPhys = base;
    gBufferSize = FUXI_RMTFS_BUFFER_SIZE;
    /* The whole window is zeroed before anything is put in it, so a byte the modem reads that nobody
       wrote is zero and not whatever was there before. */
    for (uint64_t i = 0; i < gBufferSize; ++i) gBuffer[i] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    printf("FUXI_RMTFS: the buffer is %llx of %u bytes, mapped write-combine\n",
           static_cast<unsigned long long>(gBufferPhys), gBufferSize);
    return true;
}

/* ---- the adapter's callbacks ---- */

int rmtfsOpen(void *context, const uint8_t *path, uint32_t path_length)
{
    (void)context;
    if (!path) return -1;
    for (unsigned i = 0; i < FUXI_RMTFS_FILES; ++i) {
        const uint32_t length = static_cast<uint32_t>(strlen(fuxi_rmtfs_files[i].path));
        if (length != path_length || memcmp(fuxi_rmtfs_files[i].path, path, length)) continue;
        if (!gFiles[i].present || !gFiles[i].data) return -1;
        gOpens++;
        return 0;
    }
    gRefused++;
    return -1;
}

int rmtfsRead(void *context, uint32_t number, uint32_t sector, uint32_t buffer_phys)
{
    (void)context;
    if (!number || number > FUXI_RMTFS_FILES) return -1;
    const File &file = gFiles[number - 1];
    if (!file.present || !file.data || !gBuffer) return -1;
    const uint64_t at = static_cast<uint64_t>(sector) * FUXI_RMTFS_SECTOR_SIZE;
    if (at + FUXI_RMTFS_SECTOR_SIZE > file.size) return -1;
    if (buffer_phys < gBufferPhys) return -1;
    const uint64_t window = static_cast<uint64_t>(buffer_phys) - gBufferPhys;
    if (window + FUXI_RMTFS_SECTOR_SIZE > gBufferSize) return -1;
    for (uint32_t i = 0; i < FUXI_RMTFS_SECTOR_SIZE; ++i) gBuffer[window + i] = file.data[at + i];
    __asm__ volatile("dsb sy" ::: "memory");
    gReads++;
    gSectorsRead++;
    return 0;
}

int rmtfsWrite(void *context, uint32_t number, uint32_t sector, uint32_t buffer_phys)
{
    (void)context;
    if (!number || number > FUXI_RMTFS_FILES) return -1;
    const File &file = gFiles[number - 1];
    if (!file.present || !file.data || !gBuffer) return -1;
    const uint64_t at = static_cast<uint64_t>(sector) * FUXI_RMTFS_SECTOR_SIZE;
    if (at + FUXI_RMTFS_SECTOR_SIZE > file.size) return -1;
    if (buffer_phys < gBufferPhys) return -1;
    const uint64_t window = static_cast<uint64_t>(buffer_phys) - gBufferPhys;
    if (window + FUXI_RMTFS_SECTOR_SIZE > gBufferSize) return -1;
    /* The write goes into the copy in memory and no further: the storage is not written, so the
       calibration and the IMEI cannot be damaged by the modem. */
    for (uint32_t i = 0; i < FUXI_RMTFS_SECTOR_SIZE; ++i) file.data[at + i] = gBuffer[window + i];
    gDirty[number - 1] = 1;
    gWrites++;
    gSectorsWritten++;
    return 0;
}

int rmtfsClose(void *context, uint32_t number)
{
    (void)context;
    if (!number || number > FUXI_RMTFS_FILES) return -1;
    gCloses++;
    return 0;
}

uint64_t rmtfsBuffer(void *context, uint32_t *size)
{
    (void)context;
    if (!gBuffer || !gShared) return 0;
    if (size) *size = gBufferGiven;
    return gBufferPhys;
}

uint64_t rmtfsFileSize(void *context, uint32_t number)
{
    (void)context;
    if (!number || number > FUXI_RMTFS_FILES) return 0;
    return gFiles[number - 1].size;
}

}  // namespace

/* ---- the adapter's own entry points ---- */

/* The four files, read only: which of them this board carries, on which LUN, and how big they are.
 *
 * This is the inventory the RMTFS service starts from, and it reads nothing but the partition table:
 * no partition is read into memory, no register is touched and no modem is started, so it can be run
 * on a device whose radio policy forbids anything else. */
extern "C" int fuxi_rmtfs_inventory(void)
{
    printf("FUXI_RMTFS: the four partitions\n");
    if (!native_ufs_part_find) {
        printf("FUXI_RMTFS: the partition reader is not there\n");
        return -1;
    }
    unsigned found = 0;
    for (unsigned i = 0; i < FUXI_RMTFS_FILES; ++i) {
        uint32_t lun = 0;
        uint64_t first = 0, blocks = 0;
        if (!native_ufs_part_find(fuxi_rmtfs_files[i].partition, &lun, &first, &blocks)) {
            printf("FUXI_RMTFS: %s: the partition %s is not on this board\n", fuxi_rmtfs_files[i].path,
                   fuxi_rmtfs_files[i].partition);
            continue;
        }
        ++found;
        printf("FUXI_RMTFS: %s: the partition %s is on lun %u at block %llu, %llu blocks of %llu bytes\n",
               fuxi_rmtfs_files[i].path, fuxi_rmtfs_files[i].partition, lun,
               static_cast<unsigned long long>(first), static_cast<unsigned long long>(blocks),
               static_cast<unsigned long long>(blocks * 4096u));
    }
    printf("FUXI_RMTFS: %u of %u partitions are there\n", found, FUXI_RMTFS_FILES);
    return found == FUXI_RMTFS_FILES ? 0 : -2;
}

/* The four files and the buffer, before the modem is started.
 *
 * The partitions are read first, so a board that does not carry them is known before anything is
 * handed to the secure world, and the buffer is mapped next, so the memory is there before the
 * assign that gives the modem its share of it. */
extern "C" int fuxi_rmtfs_prepare(void)
{
    printf("FUXI_RMTFS: the four files\n");
    unsigned loaded = 0;
    for (unsigned i = 0; i < FUXI_RMTFS_FILES; ++i) {
        if (loadFile(i)) ++loaded;
    }
    printf("FUXI_RMTFS: %u of %u files are there\n", loaded, FUXI_RMTFS_FILES);
    if (loaded != FUXI_RMTFS_FILES) {
        /* A file that is missing is a fact about the board, and it is reported rather than guessed
           around: the modem asks for all four, and one that cannot be answered is answered with a
           failure. */
        printf("FUXI_RMTFS: not every file is there\n");
    }
    if (!mapBuffer()) return -12;
    return 0;
}

/* The buffer, handed to the modem by the secure world. The assign is the same call the modem's own
 * region is assigned with, and it names the owners the reference names minus the one this board does
 * not have: the hypervisor's own and the modem's MSA (hakios.mpss.c, rmtfs_share: HLOS, MSS_MSA and
 * NAV, all read and write; the reference has a boot option for a board that cannot take the third,
 * `option("modem_no_nav") ? 2 : 3`).
 *
 * The secure world refused the third owner on 2026-11-22, and accepted the two at once, and the modem
 * read and wrote its files through the buffer afterwards. So the two are what this board names, and
 * the call is made once: the shape is not a guess any more.
 *
 * The buffer's address is the compute DSP's reserved region, the window the port took first, and the
 * secure world accepts it there: the refusal on 2026-11-22 was about the owners, not about the memory. */
extern "C" int fuxi_rmtfs_share(void)
{
    if (!gBuffer) return -5;
    const int rc = fuxi_modem_rmtfs_assign(gBufferPhys, gBufferSize, FUXI_RMTFS_OWNERS);
    if (rc) {
        printf("FUXI_RMTFS: the buffer was not shared (%d)\n", rc);
        return rc;
    }
    gShared = true;
    /* The modem is told how much of the buffer it may have. The whole window is ours, and the size
       the modem asks for is checked against it (hakios.mpss.c: `le32(value) > RMTFS_BUFFER_SIZE`). */
    gBufferGiven = gBufferSize;
    printf("FUXI_RMTFS: the modem may use the buffer: %llx of %u bytes\n",
           static_cast<unsigned long long>(gBufferPhys), gBufferGiven);
    return 0;
}

/* The adapter's callbacks, for the component's protocol half. */

/* The boot manifest hash the port publishes on IORegistry /chosen during its SystemOS mount. The FDT does not
 * carry it (setProperty goes to the IORegistry), so the firmware writer asks here. */
extern "C" int fuxi_boot_manifest_hash(unsigned char *out, unsigned size)
{
    if (!out || size < 48) return 0;
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) return 0;
    OSData *value = OSDynamicCast(OSData, chosen->getProperty("boot-manifest-hash"));
    const bool ok = value && value->getLength() == 48;
    if (ok) memcpy(out, value->getBytesNoCopy(), 48);
    chosen->release();
    return ok ? 1 : 0;
}

extern "C" const struct fuxi_rmtfs_ops *fuxi_rmtfs_ops(void)
{
    static const struct fuxi_rmtfs_ops ops = {
        rmtfsOpen, rmtfsRead, rmtfsWrite, rmtfsClose, rmtfsBuffer, rmtfsFileSize,
    };
    return &ops;
}

/* Writes the files the modem has changed to the Data volume, one call at a time. The parent directory is the one
 * the boot already uses (/private/var/db), because creating one needs a process and this thread has none. A
 * failure is counted and not fatal: the copy in memory is what serves the modem for the rest of the boot. */
extern "C" void fuxi_rmtfs_flush(void)
{
    for (unsigned i = 0; i < FUXI_RMTFS_FILES; ++i) {
        File &file = gFiles[i];
        if (!gDirty[i] || !file.present || !file.data || !file.size) continue;
        gDirty[i] = 0;
        char path[96];
        snprintf(path, sizeof path, "/private/var/db/coachz-rmtfs-%s", fuxi_rmtfs_files[i].partition);
        vfs_context_t ctx = vfs_context_current();
        vnode_t vp = NULLVP;
        int error = vnode_open(path, O_CREAT | O_TRUNC | FWRITE | O_NOFOLLOW, 0644, 0, &vp, ctx);
        if (error) {
            gPersistFailed++;
            gPersistError = error;
            printf("FUXI_RMTFS: %s: the copy could not be written to the Data volume (%d)\n",
                   fuxi_rmtfs_files[i].partition, error);
            continue;
        }
        uint64_t at = 0;
        while (at < file.size) {
            const uint32_t piece = (file.size - at) > (1u << 20) ? (1u << 20) : static_cast<uint32_t>(file.size - at);
            int resid = 0;
            const int failed = vn_rdwr(UIO_WRITE, vp, reinterpret_cast<caddr_t>(file.data + at), piece, at,
                                       UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid, NULL);
            if (failed || resid) { gPersistFailed++; gPersistError = failed ? failed : EIO; break; }
            at += piece;
        }
        vnode_close(vp, FWRITE, ctx);
        if (at == file.size) {
            gPersisted++;
            printf("FUXI_RMTFS: %s: %llu bytes of the modem's writes saved to %s\n", fuxi_rmtfs_files[i].partition,
                   static_cast<unsigned long long>(at), path);
        } else {
            printf("FUXI_RMTFS: %s: only %llu of %llu bytes were written (%d)\n", fuxi_rmtfs_files[i].partition,
                   static_cast<unsigned long long>(at), static_cast<unsigned long long>(file.size), gPersistError);
        }
    }
}
/* The counters, for the board's report. */
extern "C" void fuxi_rmtfs_counters(uint32_t *out, unsigned count)
{
    if (!out || count < 7) return;
    out[0] = gOpens;
    out[1] = gCloses;
    out[2] = gReads;
    out[3] = gWrites;
    out[4] = gSectorsRead;
    out[5] = gSectorsWritten;
    out[6] = gRefused;
}
