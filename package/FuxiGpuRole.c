/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * The GPU role the A740 user client asks for, which this port does not assign.
 *
 * Adreno740's user client reads the role to choose between the foreground and the background priority
 * the render server set on a connection, and it reports the role in its own priority log. iPadOS
 * assigns one through PRIO_DARWIN_GPU; no code here does, so the call answers 0, which is neither
 * background nor utility, and every client keeps the foreground value.
 *
 * It lives in the device repository rather than in osfmk/kern/task.c so the kernel source keeps only
 * the two constants and the declaration (patches/xnu/fuxi-gpu-role.patch).
 */
#include <kern/task.h>
#include <vm/vm_page.h>

int
task_get_gpu_role(task_t task)
{
	(void)task;
	return 0;
}

/* The user mappings of a physical page, which Adreno740 asks for when it frees an owned buffer object:
 * it counts how many of the object's first pages a task still maps and records the last one. This port
 * publishes no user mapping of GPU memory, so the answer is zero mappings, and the driver takes its
 * unmapped path. Adreno740 declares the symbol weak_import, but a kernel link resolves it statically,
 * so it has to exist. */
unsigned int
native_pmap_user_mappings(ppnum_t pn, void **pmap, uint64_t *va, char *name)
{
	(void)pn;
	(void)pmap;
	(void)va;
	(void)name;
	return 0;
}

/* The pool size the GPU driver reports for its own diagnostics. Adreno740 defines it when its memory
 * pool is built; this board's probe reads it from the power path before that, so a weak zero lets the
 * link close and the diagnostic row read 0 instead of a pool size. */
__attribute__((weak)) unsigned int native_adreno_pool_mib = 0;
