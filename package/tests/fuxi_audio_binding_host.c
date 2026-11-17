/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../FuxiAudioBinding.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

int main(void)
{
    const FuxiAudioBinding *b = fuxi_audio_binding();
    CHECK(b != 0);
    CHECK(b->adsp.carveout_base == 0x9ea00000ull);
    CHECK(b->adsp.carveout_size == 0x04680000ull);
    CHECK(!strcmp(b->adsp.state_entry, "master-kernel"));
    CHECK(!strcmp(b->adsp.ready_entry, "slave-kernel"));
    CHECK(b->mi2s.port_id == 0x12u && b->mi2s.data_out_gpio == 124u);
    CHECK(b->i2c8.mmio_base == 0x00880000ull && b->i2c8.frequency_hz == 400000u);
    CHECK(b->amplifiers[0].i2c_address == 0x40u && b->amplifiers[1].i2c_address == 0x42u);
    CHECK(b->amplifiers[0].reset_gpio != b->amplifiers[1].reset_gpio);
    CHECK(b->field_count == b->state_count[0] + b->state_count[1] + b->state_count[2]);
    const FuxiAudioFieldInfo *predicted = fuxi_audio_binding_find("adsp.carveout.carveout_base");
    CHECK(predicted && predicted->state == FuxiAudioUnverified && !strcmp(predicted->evidence, "sheng.sm8550"));
    const FuxiAudioFieldInfo *choice = fuxi_audio_binding_find("adsp.timeouts.ready_timeout");
    CHECK(choice && choice->state == FuxiAudioChosen);
    CHECK(fuxi_audio_binding_find("does.not.exist") == 0);
    if (failures) return 1;
    printf("PASS FuxiAudioBinding: %d checks, %u fields (%u unverified, %u chosen, %u verified)\n",
           checks, b->field_count, b->state_count[0], b->state_count[1], b->state_count[2]);
    return 0;
}
