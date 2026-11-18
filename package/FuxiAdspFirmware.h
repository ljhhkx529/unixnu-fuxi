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
/* Lists a directory of the device's own firmware partition, read only. path is a directory inside the
 * volume ("/" for its root, "/image" for the firmware's directory). The caller drives it: entry = 0 for
 * the first entry, and the directory ends when the call returns an entry whose name is empty. */
int fuxi_adsp_fw_list(const char *path, unsigned index, char *name, size_t name_length, uint64_t *size,
                      int *is_directory);
#ifdef __cplusplus
}
#endif
#endif
