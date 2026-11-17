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
#ifdef __cplusplus
}
#endif
#endif
