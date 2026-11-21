/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* RMTFS: the modem's remote file system. The shared facts of it, and the board's adapter's contract.
 *
 * The modem keeps four files on the phone's storage that it cannot reach itself: the two modem state
 * partitions and the two calibration ones. It asks the application processor for them over QRTR, on
 * service 14, and the application processor moves the sectors for it through a buffer both sides can
 * reach.
 *
 * The buffer is a fixed physical window. The modem's requests carry addresses of 32 bits, so it has to
 * be below 4 GiB, and the kernel's free memory there is not to be had in one piece of 2.5 MB. So it is
 * the first 2.5 MB of the memory the device tree reserves for the compute DSP
 * (reserved-memory cdsp_region@9c900000, 32 MB, no-map): nothing in this port or in uefi-fuxi starts
 * the compute DSP, so the region is unused, and the kernel's allocator cannot reach it either, because
 * DarwinBoot hands the kernel only the conventional memory, and the reserved region never is.
 *
 * The reference port for the same modem in XNU takes the same window out of the compute DSP's memory on
 * its own board (hakios.mpss.c, rmtfs_buffer_make: RMTFS_BUFFER_PA_DEFAULT 0x9c700000, the same size).
 *
 * The buffer is handed to three owners with assign_mem, and not to one: the hypervisor's own owner, the
 * modem's MSA and the navigation processor (hakios.mpss.c, rmtfs_share: the destinations are HLOS,
 * MSS_MSA and NAV, all with read and write).
 *
 * The four files hold the IMEI and the radio calibration, so nothing here ever writes them. The
 * partitions are read once into memory, and a write request from the modem changes the memory copy and
 * nothing else: the partition reader exposes no write counterpart at all.
 *
 * The protocol half is in the component's own header (drivers/QcomBaseband/platform/fuxi/fuxi_rmtfs.h),
 * which this one is included by: the two halves have to agree on the four paths and on the adapter's
 * contract.
 */
#ifndef FUXI_MODEM_RMTFS_H
#define FUXI_MODEM_RMTFS_H

#include <stdint.h>

/* The buffer: the first 2.5 MB of the compute DSP's reserved memory. The size is the one the device
   tree's own node names (qcom,rmtfs_sharedmem@0: reg = <0x00 0x280000>), and it is the size the
   reference uses (hakios.mpss.c: RMTFS_BUFFER_SIZE 0x280000). */
#define FUXI_RMTFS_BUFFER_BASE 0x9c900000ull
#define FUXI_RMTFS_BUFFER_SIZE 0x280000ull

/* The four files the modem asks for, and the partition behind each one. The paths and the order are
   the reference's (hakios.mpss.c, rmtfs_files). The file's number in the answers is its place here
   plus one, and the partition's name is the one the GPT carries. */
struct fuxi_rmtfs_file {
    const char *path;       /* the path the modem asks for */
    const char *partition;  /* the GPT partition behind it */
};

static const struct fuxi_rmtfs_file fuxi_rmtfs_files[] = {
    {"/boot/modem_fs1", "modemst1"},
    {"/boot/modem_fs2", "modemst2"},
    {"/boot/modem_fsc", "fsc"},
    {"/boot/modem_fsg", "fsg"},
};
#define FUXI_RMTFS_FILES ((uint32_t)(sizeof fuxi_rmtfs_files / sizeof fuxi_rmtfs_files[0]))

/* The sector size the protocol moves data in. The file's offset is the sector number times this, and
   the partition's own blocks are 4096 bytes, which is a whole number of sectors, so a sector never
   straddles a block. */
#define FUXI_RMTFS_SECTOR_SIZE 512u

/* What the board's adapter has to provide for the protocol to work.
 *
 * The open callback answers the file a path names. It returns zero when the file is there, and
 * anything else when it is not, and the request is then answered with a failure.
 *
 * The read and write callbacks move one sector between the file and the buffer. The buffer's address
 * is absolute, the same address the modem was told about, so a write goes to the bytes the modem is
 * looking at and not to a copy somewhere else. The callback is handed the physical address of the
 * byte, and turns it into the buffer's own mapping.
 *
 * The close callback releases a file.
 *
 * The buffer callback answers the buffer's physical address and its size. The address is what
 * ALLOC_BUFF hands the modem, and the size is the bound every RW_IOVEC address is checked against.
 * It answers zero when the buffer is not there to be shared, which is what refuses the request.
 *
 * The size callback answers a file's own size in bytes, which bounds the sectors that may be read
 * from it. */
typedef int (*fuxi_rmtfs_open_fn)(void *context, const uint8_t *path, uint32_t path_length);
typedef int (*fuxi_rmtfs_io_fn)(void *context, uint32_t number, uint32_t sector, uint32_t buffer_phys);
typedef int (*fuxi_rmtfs_close_fn)(void *context, uint32_t number);
typedef uint64_t (*fuxi_rmtfs_buffer_fn)(void *context, uint32_t *size);
typedef uint64_t (*fuxi_rmtfs_file_size_fn)(void *context, uint32_t number);

struct fuxi_rmtfs_ops {
    fuxi_rmtfs_open_fn open;
    fuxi_rmtfs_io_fn read;
    fuxi_rmtfs_io_fn write;
    fuxi_rmtfs_close_fn close;
    fuxi_rmtfs_buffer_fn buffer;
    fuxi_rmtfs_file_size_fn file_size;
};

/* The regions the buffer must not touch. Each one is a region the device tree reserves or the port
   itself maps, and the modem's buffer landing in any of them would corrupt it or the firmware.
   The buffer's own region, the compute DSP's memory, is not in the list: that is where it goes.

   The first two are the ones the window sits between: the compute video processor's region ends
   exactly where the window starts, and the modem's own memory ends well before it. The rest are the
   modem's carveouts and the windows the port maps itself. */
struct fuxi_rmtfs_range {
    uint64_t base;
    uint64_t size;
    const char *name;
};

#define FUXI_RMTFS_FORBIDDEN_COUNT 9u

static inline struct fuxi_rmtfs_range fuxi_rmtfs_forbidden(unsigned index)
{
    static const struct fuxi_rmtfs_range ranges[FUXI_RMTFS_FORBIDDEN_COUNT] = {
        {0x9c200000ull, 0x700000ull, "the compute video processor's region"},
        {0x9b000000ull, 0x80000ull, "the modem's device tree blob"},
        {0x9b080000ull, 0x10000ull, "the IPA's firmware"},
        {0x9b090000ull, 0xa000ull, "the IPA's GSI"},
        {0x9b09a000ull, 0x2000ull, "the GPU's microcode"},
        {0x8a800000ull, 0x10800000ull, "the modem's own memory"},
        {0xd4d00000ull, 0x3300000ull, "the DSM region the assign hands over"},
        {0x81d00000ull, 0x200000ull, "the shared memory window"},
        {0x00408000ull, 0x10000ull, "the IPCC window"},
    };
    return ranges[index];
}

/* Whether two ranges overlap. */
static inline int fuxi_rmtfs_overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return (a < b + blen) && (b < a + alen);
}

#endif
