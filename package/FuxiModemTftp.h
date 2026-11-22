/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TFTP: the modem's carrier configuration, fetched over QRTR. The board's half of the service.
 *
 * The modem reads its carrier configuration over a TFTP service, and stays offline without it: the
 * reference says so in one line (hakios.mpss.c, tftp: "the modem reads its carrier configuration here and
 * stays offline without it"), and the device agrees — on 2026-11-22 the modem's operating mode was 5,
 * PERSIST, and it had no registration and no signal.
 *
 * This is the second of the two gates the modem's stack has, and they are separate: RMTFS decides whether
 * the modem announces its stack at all (that one is done), and TFTP decides whether it goes from offline to
 * online. The two were once mistaken for one, and the mistake cost a round.
 *
 * The service is number 4096. It is published at two versions, 1 and 3, because the modem looks up the
 * version its own firmware expects, and the reference publishes both.
 *
 * The protocol half is in the component's own header (drivers/QcomBaseband/platform/fuxi/fuxi_tftp.h),
 * which this one is included by: the two halves have to agree on the paths and on the adapter's contract.
 *
 * Nothing here writes anything to the modem's storage. A write request from the modem is acknowledged and
 * taken into memory, for the run, so the modem can read it back; the carrier configuration is the radio's
 * own, and rewriting it on the radio's storage is not this port's to do. The reference keeps a written
 * file in memory as well (hakios.mpss.c:4214-4236).
 */
#ifndef FUXI_MODEM_TFTP_H
#define FUXI_MODEM_TFTP_H

#include <stdint.h>

/* The prefixes the modem asks its files under, and what each of them maps to in the modem's own volume.
 *
 * The modem asks for two kinds of path, and the difference is what the path means:
 *
 *   "/readonly/..."   a file the modem wants to read. The reference maps it to the read-only modem FAT
 *                    partition (hakios.mpss.c, tftp_file_of:4264-4281), where "/readonly/firmware/image/X"
 *                    is "image/X". That mapping is kept here.
 *
 *   "/readwrite/..."  a file the modem wants to write. The reference takes it into memory and keeps it
 *                    for the run (hakios.mpss.c:4214-4236), and serves it back if the modem reads it
 *                    again. The modem does exactly that here: it writes "/readwrite/mcfg.tmp" and then
 *                    reads it back, and it writes "/readwrite/server_check.txt" to see whether the host
 *                    took it.
 *
 *   "/only/..."       a file the modem wants to read or write, with no other meaning attached.
 *
 * The volume itself is the modem's own firmware partition: the port already mounts it, read only, for the
 * modem's images and for its service registry's tables (package/FuxiAdspFirmware.c).
 *
 * The read prefix is the one the reference maps, and it is the only one that names a file on the volume.
 * The write prefix is served from memory and never reaches the volume: nothing on the modem's storage is
 * written, and the write that the modem makes of its own carrier configuration is a write into a buffer
 * that this port holds for the run. That is the reference's own behaviour, and it is the reason the modem
 * can hand over a file it has generated and then read it back. */
#define FUXI_TFTP_PREFIX_COUNT 4u

static const char *const fuxi_tftp_prefixes[FUXI_TFTP_PREFIX_COUNT] = {
    "/readonly/firmware/image/",
    "/readonly/vendor/firmware_mnt/image/",
    "/readonly/vendor/firmware/",
    "/readonly/firmware/",
};

/* What the board's adapter has to provide for the protocol to work.
 *
 * The open callback opens the file a path names. The path it is given is the modem's own path, with the
 * prefix still on it, so the adapter decides what it serves; the protocol half does not know which
 * prefixes map where. It returns zero when the file is there, and the handle it fills in is what the
 * read and the size callbacks are given.
 *
 * The read callback moves one range of the file out: the offset and the length in, the bytes out. It
 * returns zero when it did.
 *
 * The size callback answers the file's own size in bytes, which is what bounds the transfer.
 *
 * The close callback releases the file.
 *
 * The send callback puts one packet on the wire. It is given the port the answer goes from, the node and
 * the port it goes to, and the bytes. It returns zero when the packet went out. */
typedef int (*fuxi_tftp_open_fn)(void *context, const char *path, uint32_t path_length, void **handle);
typedef int (*fuxi_tftp_read_fn)(void *context, void *handle, uint64_t offset, void *to, uint32_t length);
typedef uint64_t (*fuxi_tftp_size_fn)(void *context, void *handle);
typedef void (*fuxi_tftp_close_fn)(void *context, void *handle);
typedef int (*fuxi_tftp_send_fn)(void *context, uint32_t src_port, uint32_t dst_node, uint32_t dst_port,
                                 const uint8_t *body, uint32_t length);
/* What a file the modem wrote is kept under, so the modem can read it back. The adapter holds it: the
   protocol half hands it each block as the block arrives, and the adapter appends. The name is the one the
   modem used, so the read that follows the write finds the same file.
 *
 * The store never reaches the modem's storage. It is memory, for the run, and it goes away when the port
   does. */
typedef int (*fuxi_tftp_store_fn)(void *context, const char *name, uint32_t name_length, const void *data,
                                  uint32_t length);

struct fuxi_tftp_ops {
    fuxi_tftp_open_fn open;
    fuxi_tftp_read_fn read;
    fuxi_tftp_size_fn size;
    fuxi_tftp_close_fn close;
    fuxi_tftp_send_fn send;
    fuxi_tftp_store_fn store;
};

#endif
