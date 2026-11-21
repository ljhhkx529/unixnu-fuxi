/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef FUXI_ADSP_FIRMWARE_H
#define FUXI_ADSP_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int fuxi_adsp_fw_open(const char *path, void **handle, uint64_t *size);
int fuxi_adsp_fw_ready(void);
int fuxi_adsp_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void fuxi_adsp_fw_close(void *handle);
/* Reads the .mdt file of a modem firmware (the header part followed by the hash segment). The secure world
 * authenticates the metadata out of this file, so it is read on its own and not out of the joined image of
 * the .bNN pieces. Returns 0 on success. */
int fuxi_adsp_fw_read_mdt(void *handle, uint64_t offset, void *to, size_t length);
/* The size of the .mdt file, 0 when the firmware has none. */
uint64_t fuxi_adsp_fw_mdt_size(void *handle);
/* Lists a directory of the device's own firmware partition, read only. path is a directory inside the
 * volume ("/" for its root, "/image" for the firmware's directory). The caller drives it: entry = 0 for
 * the first entry, and the directory ends when the call returns an entry whose name is empty. */
int fuxi_adsp_fw_list(const char *path, unsigned index, char *name, size_t name_length, uint64_t *size,
                      int *is_directory);
/* The same, but of one named slot's volume: "modem_a" or "modem_b". The volume is mounted for the
 * call and released after it, so both slots can be looked at one after the other. */
int fuxi_adsp_fw_list_slot(const char *slot, const char *path, unsigned index, char *name, size_t name_length,
                            uint64_t *size, int *is_directory);
/* The identity of one named partition: its LUN, its first block and its block count. Two slots that
 * are the same region have the same three numbers. */
int fuxi_adsp_fw_slot_info(const char *slot, uint32_t *lun, uint64_t *first, uint64_t *blocks);
/* The joined image of NAME (the .bNN split form) out of one named slot, copied out whole. The caller
 * frees it with the size it was given. When mdt is not null the slot's .mdt file is copied out as
 * well (the header part followed by the hash segment, which is what the secure world authenticates),
 * with its size in mdt_size; the caller frees it with that size. */
int fuxi_adsp_fw_read_slot(const char *slot, const char *name, uint64_t *size, void **out, void **mdt, uint64_t *mdt_size);
/* Reads one plain file of the device's own firmware partition, whole, read only. The path is inside
 * the volume, as for the listing ("image/modemr.jsn"). The bytes are copied out and the caller frees
 * them with the size it is given. Returns 0 on success. */
int fuxi_adsp_fw_read_file(const char *path, uint64_t *size, void **out);
#ifdef __cplusplus
}
#endif
#endif
