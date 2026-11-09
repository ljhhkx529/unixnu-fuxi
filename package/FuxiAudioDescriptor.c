/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FuxiAudioDescriptor.h"

#define FUXI_SMEM_BASE UINT64_C(0x81d00000)
#define FUXI_SMEM_SIZE UINT64_C(0x200000)

int fuxi_audio_descriptor_run(struct FuxiAudioDescriptorState *state,
                              const struct FuxiAudioDescriptorBackend *backend,
                              unsigned step)
{
    void *descriptor;
    if (!state || !backend || !backend->create || !backend->release)
        return FuxiAudioDescriptorBadState;
    if (step == 4u) {
        if (state->descriptor) return FuxiAudioDescriptorBadState;
        descriptor = backend->create(backend->context, FUXI_SMEM_BASE, FUXI_SMEM_SIZE);
        if (!descriptor) return FuxiAudioDescriptorCreateFailed;
        state->descriptor = descriptor;
        return FuxiAudioDescriptorOk;
    }
    if (step == 5u) {
        descriptor = state->descriptor;
        if (!descriptor) return FuxiAudioDescriptorBadState;
        /* Clear ownership first. A release fault cannot advertise a reusable
         * resource or let a later request release the same object twice. */
        state->descriptor = 0;
        backend->release(backend->context, descriptor);
        return FuxiAudioDescriptorOk;
    }
    return FuxiAudioDescriptorRefused;
}

unsigned fuxi_audio_descriptor_resources(const struct FuxiAudioDescriptorState *state)
{
    return state && state->descriptor ? FuxiAudioDescriptorResource : 0u;
}
