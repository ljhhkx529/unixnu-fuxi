/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The audio bring-up probe, one step per boot.
 *
 * The runbook in docs/fuxi-audio-probe-debug-runbook.md asks for experiments that are small enough that a failure names the
 * action that failed. The earlier design built a 1.6 KiB report and printed it in one call, which made every crash a
 * guess about which of six reads died. This one takes a step number, performs exactly one action, and prints two short
 * checkpoints around it:
 *
 *   AUDIO step=3 phase=BEGIN
 *   AUDIO step=3 phase=END rc=0 value=0x24434f54
 *
 * A missing END with the control channel still answering points at that action; a missing END with the channel gone is a
 * kernel fault inside it. Either way the next step is one action, not a rewrite.
 *
 * Steps (each needs its own boot, and nothing here writes):
 *   1  create the descriptor and release it
 *   2  create the descriptor and the mapping, report the virtual address, do not dereference
 *   3  mapping, then one 32-bit read of the TOC magic
 *   4  mapping, then one 32-bit read of the partition magic
 *   5  mapping, then one 32-bit read of the item 478 header
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

extern "C" {
int fuxi_audio_step = 0;
}

#define kSmemBase 0x81d00000ull
#define kSmemSize 0x200000u
#define kTocOffset (kSmemSize - 0x1000u)
#define kPartitionOffset 0x19f000u
#define kItem478Offset 0x19f030u

namespace {

void checkpoint(int step, const char *phase, int rc, uint32_t value, int has_value)
{
    if (has_value) IOLog("AUDIO step=%d phase=%s rc=%d value=0x%08x\n", step, phase, rc, value);
    else IOLog("AUDIO step=%d phase=%s rc=%d\n", step, phase, rc);
}

}  // namespace

/* The checkpoints have to reach the host, and the USB CDC channel only exists after userland starts, so the step runs
 * from a thread that waits for it. One boot still performs exactly one step: the thread only moves the moment, not the
 * amount of work, and a fault inside the step still costs one boot and names itself in the log. */
#define kStepDelayMs 15000u

static int gStep;

static void audioStepThread(void *, wait_result_t)
{
    const int step = gStep;
    IOSleep(kStepDelayMs);

    /* Step 1: the descriptor alone. */
    checkpoint(step, "BEGIN", 0, 0, 0);
    IOMemoryDescriptor *descriptor =
        IOMemoryDescriptor::withPhysicalAddress(kSmemBase, kSmemSize, kIODirectionIn);
    if (!descriptor) {
        checkpoint(step, "END", -1, 0, 0);
        return;
    }
    if (step == 1) {
        descriptor->release();
        checkpoint(step, "END", 0, 0, 0);
        return;
    }

    /* Steps 2 to 5: a read-only mapping, then one action. */
    IOMemoryMap *map = descriptor->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly);
    descriptor->release();
    if (!map || !map->getVirtualAddress()) {
        if (map) map->release();
        checkpoint(step, "END", -2, 0, 0);
        return;
    }
    const volatile uint32_t *base = (const volatile uint32_t *)map->getVirtualAddress();
    if (step == 2) {
        checkpoint(step, "END", 0, (uint32_t)(uintptr_t)base, 1);
        return;
    }

    const uint32_t offset = step == 3 ? kTocOffset : (step == 4 ? kPartitionOffset : kItem478Offset);
    const uint32_t value = base[offset / 4u];
    checkpoint(step, "END", 0, value, 1);
}

extern "C" void native_audio_probe(void)
{
    int step = 0;
    if (!PE_parse_boot_argn("fuxi-audio-step", &step, sizeof step) || step < 1) return;
    fuxi_audio_step = step;
    gStep = step;
    thread_t thread = nullptr;
    if (kernel_thread_start(audioStepThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("AUDIO step=%d phase=NOTHREAD\n", step);
        return;
    }
    thread_deallocate(thread);
}
