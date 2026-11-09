/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../FuxiAudioDescriptor.h"
#include <stdio.h>
#include <stdlib.h>

static int checks;
static void check(int value, const char *message)
{
    ++checks;
    if (!value) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

struct mock {
    unsigned creates, releases, fail;
    uint64_t physical, length;
    void *released;
    struct FuxiAudioDescriptorState *state;
};

static void *create(void *context, uint64_t physical, uint64_t length)
{
    struct mock *m = context;
    ++m->creates; m->physical = physical; m->length = length;
    return m->fail ? 0 : (void *)(uintptr_t)0x1234u;
}

static void release(void *context, void *descriptor)
{
    struct mock *m = context;
    ++m->releases; m->released = descriptor;
    check(fuxi_audio_descriptor_resources(m->state) == 0,
          "resource ownership is cleared before backend release");
}

int main(void)
{
    struct FuxiAudioDescriptorState state = {0};
    struct mock m = {.state = &state};
    struct FuxiAudioDescriptorBackend backend = {create, release, &m};

    check(fuxi_audio_descriptor_resources(&state) == 0, "initial resources are empty");
    check(fuxi_audio_descriptor_run(&state, &backend, 5) == FuxiAudioDescriptorBadState,
          "release before create is refused");
    check(fuxi_audio_descriptor_run(&state, &backend, 99) == FuxiAudioDescriptorRefused,
          "unknown operation is refused");
    m.fail = 1;
    check(fuxi_audio_descriptor_run(&state, &backend, 4) == FuxiAudioDescriptorCreateFailed,
          "backend creation failure is reported");
    check(fuxi_audio_descriptor_resources(&state) == 0 && m.releases == 0,
          "failed creation leaves no resource");
    m.fail = 0;
    check(fuxi_audio_descriptor_run(&state, &backend, 4) == 0, "fixed descriptor is created");
    check(m.physical == UINT64_C(0x81d00000) && m.length == UINT64_C(0x200000),
          "only the compiled SMEM range reaches the backend");
    check(fuxi_audio_descriptor_resources(&state) == FuxiAudioDescriptorResource,
          "descriptor resource bit is published");
    check(fuxi_audio_descriptor_run(&state, &backend, 4) == FuxiAudioDescriptorBadState && m.creates == 2,
          "duplicate creation does not call the backend");
    check(fuxi_audio_descriptor_run(&state, &backend, 5) == 0, "descriptor is released");
    check(m.releases == 1 && m.released == (void *)(uintptr_t)0x1234u,
          "the exact retained descriptor is released once");
    check(fuxi_audio_descriptor_resources(&state) == 0, "resource bit returns to zero");
    check(fuxi_audio_descriptor_run(&state, &backend, 5) == FuxiAudioDescriptorBadState && m.releases == 1,
          "duplicate release has no side effect");
    printf("PASS FuxiAudioDescriptor: %d checks\n", checks);
    return 0;
}
