/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Temporary weak stubs for GopFramebuffer's direct-DPU scanout path.
 * fuxi still uses the older NativeUfs backend, which does not expose
 * native_ufs_dpu_map/native_ufs_dpu_state yet. These keep the link working
 * and keep direct scanout disabled until the fuxi UFS backend implements it.
 */
#include <stdint.h>
#include <stddef.h>

__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count) {
    (void)slot;
    (void)pages;
    (void)count;
    return 0;
}

__attribute__((weak)) volatile uint32_t native_ufs_dpu_state = 0;
