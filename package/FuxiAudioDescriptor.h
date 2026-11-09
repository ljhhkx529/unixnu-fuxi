/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef FUXI_AUDIO_DESCRIPTOR_H
#define FUXI_AUDIO_DESCRIPTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    FuxiAudioDescriptorOk = 0,
    FuxiAudioDescriptorBadState = -1,
    FuxiAudioDescriptorCreateFailed = -2,
    FuxiAudioDescriptorRefused = -6,
    FuxiAudioDescriptorResource = 1u << 0
};

struct FuxiAudioDescriptorBackend {
    void *(*create)(void *context, uint64_t physical, uint64_t length);
    void (*release)(void *context, void *descriptor);
    void *context;
};

struct FuxiAudioDescriptorState {
    void *descriptor;
};

int fuxi_audio_descriptor_run(struct FuxiAudioDescriptorState *state,
                              const struct FuxiAudioDescriptorBackend *backend,
                              unsigned step);
unsigned fuxi_audio_descriptor_resources(const struct FuxiAudioDescriptorState *state);

#ifdef __cplusplus
}
#endif

#endif
