/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Fixed, CDC-driven audio bring-up operations.
 *
 * This file deliberately has no boot argument, delayed thread, mapping or
 * dereference path. The diagnostic transport calls one fixed operation at a
 * time from its bounded worker. The descriptor is the sole retained resource.
 */
#include <IOKit/IOMemoryDescriptor.h>
#include "FuxiAudioDescriptor.h"

namespace {

FuxiAudioDescriptorState gState;

void *createDescriptor(void *, uint64_t physical, uint64_t length)
{
    return IOMemoryDescriptor::withPhysicalAddress(
        static_cast<mach_vm_address_t>(physical), static_cast<mach_vm_size_t>(length), kIODirectionIn);
}

void releaseDescriptor(void *, void *opaque)
{
    static_cast<IOMemoryDescriptor *>(opaque)->release();
}

const FuxiAudioDescriptorBackend gBackend = {createDescriptor, releaseDescriptor, nullptr};

}  // namespace

extern "C" int fuxi_audio_diag_run(unsigned step)
{
    return fuxi_audio_descriptor_run(&gState, &gBackend, step);
}

extern "C" unsigned fuxi_audio_diag_resources(void)
{
    return fuxi_audio_descriptor_resources(&gState);
}

/* Definition retained for the H0 reachability gate. Nothing calls it, and it
 * intentionally cannot start work from the boot path. */
extern "C" void native_audio_probe(void)
{
}
