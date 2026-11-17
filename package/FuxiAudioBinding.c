/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FuxiAudioBinding.h"

#include <string.h>

/* Produced from facts/audio/binding.toml by gen-fuxi-audio-binding.py. */
#include "FuxiAudioBindingData.inc"

const FuxiAudioBinding *fuxi_audio_binding(void)
{
    return &gFuxiAudioBinding;
}

const FuxiAudioFieldInfo *fuxi_audio_binding_find(const char *path)
{
    if (!path) return 0;
    for (uint32_t i = 0; i < gFuxiAudioBinding.field_count; ++i)
        if (!strcmp(gFuxiAudioBinding.fields[i].path, path)) return &gFuxiAudioBinding.fields[i];
    return 0;
}
