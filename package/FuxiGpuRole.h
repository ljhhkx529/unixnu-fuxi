/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * The GPU role the A740 user client reads, declared for the archive that is compiled before the kernel.
 *
 * patches/xnu/fuxi-gpu-role.patch adds PRIO_DARWIN_GPU_BACKGROUND/UTILITY to sys/resource_private.h and
 * task_get_gpu_role() to osfmk/kern/task.h and task.c. The archives build before the XNU pass, and this
 * tree's export headers still lack the declaration when they run, so the names are restated here and
 * force-included by build-adreno740-kits.py. There is no role assignment on this port, so the kernel
 * function returns 0 and every client takes the priority the render server set for the foreground.
 */
#pragma once
#include <kern/task.h>
#include <sys/resource_private.h>

#ifndef PRIO_DARWIN_GPU_BACKGROUND
#define PRIO_DARWIN_GPU_BACKGROUND 8
#endif
#ifndef PRIO_DARWIN_GPU_UTILITY
#define PRIO_DARWIN_GPU_UTILITY 9
#endif

#ifdef __cplusplus
extern "C" {
#endif
int task_get_gpu_role(task_t task);
#ifdef __cplusplus
}
#endif
