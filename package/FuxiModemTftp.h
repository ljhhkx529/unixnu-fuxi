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
 * Nothing here writes anything. A write request from the modem is acknowledged and discarded, which is
 * the reference's own behaviour (umi_modem_mhi.inc:787-796): the carrier configuration is the radio's
 * own, and rewriting it is not this port's to do.
 */
#ifndef FUXI_MODEM_TFTP_H
#define FUXI_MODEM_TFTP_H

#include <stdint.h>

/* The prefixes the modem asks its files under, and what each of them maps to in the modem's own volume.
 *
 * The reference serves these four (hakios.mpss.c, tftp_file_of:4264-4281) and maps every one of them to
 * "image/<the rest>" in the modem's FAT partition. The rest of the path is checked for "..", so a request
 * cannot climb out of the directory it names, and that check belongs to the adapter because the adapter is
 * the one that turns a path into a file.
 *
 * The volume itself is the modem's own firmware partition: the port already mounts it, read only, for the
 * modem's images and for its service registry's tables (package/FuxiAdspFirmware.c). */
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

struct fuxi_tftp_ops {
    fuxi_tftp_open_fn open;
    fuxi_tftp_read_fn read;
    fuxi_tftp_size_fn size;
    fuxi_tftp_close_fn close;
    fuxi_tftp_send_fn send;
};

#endif
