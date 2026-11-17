/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Typed, generated-at-build-time view of facts/audio/binding.toml.
 *
 * Protocol code consumes this object instead of carrying sheng board constants.
 * Every generated value also has a metadata row, so device diagnostics can say
 * whether a value was measured, selected by design, or is still a prediction. */
#ifndef FUXI_AUDIO_BINDING_H
#define FUXI_AUDIO_BINDING_H

#include <stdint.h>

#include "CirrusCs35l41.h"
#include "QcomAdsp.h"

#ifdef __cplusplus
extern "C" {
#endif

enum FuxiAudioBindingState {
    FuxiAudioUnverified = 0,
    FuxiAudioChosen = 1,
    FuxiAudioVerified = 2,
};

typedef struct {
    const char *path;
    const char *evidence;
    uint8_t state;
} FuxiAudioFieldInfo;

typedef struct {
    uint32_t port_id;
    uint32_t sck_gpio, ws_gpio, data_out_gpio, data_in_gpio;
} FuxiMi2sBinding;

typedef struct {
    uint64_t mmio_base;
    uint32_t frequency_hz;
    uint32_t sda_gpio, scl_gpio;
} FuxiI2cBinding;

typedef struct {
    uint32_t channel;
    uint32_t i2c_address;
    uint32_t reset_gpio, irq_gpio;
} FuxiAmpEndpoint;

/* The platform windows and routing numbers the ADSP owner glue needs. All are
 * board values; the runtime consumes them instead of carrying a second copy. */
typedef struct {
    uint64_t smem_base, smem_size;
    uint64_t tcsr_mutex;
    uint64_t ipcc_base;
    uint32_t ipcc_spi;
    uint64_t aoss_base;
    uint32_t aoss_size;
    uint64_t rsc_base, rsc_size;
    uint32_t rsc_tcs_offset, rsc_spi;
    uint64_t cmd_db_base, cmd_db_size;
    uint32_t remote_pid;
    uint32_t smp2p_in_item, smp2p_out_item, smp2p_crash_item;
    uint32_t glink_desc_item, glink_tx_item, glink_rx_item;
    uint32_t glink_fifo_bytes;
    uint32_t pas_adsp, pas_dtb;
    uint64_t dtb_carveout_base, dtb_carveout_size;
    const char *glink_channel;
    uint32_t glink_intent_size, glink_intent_count;
    uint32_t gpr_port;
} FuxiAdspPlatformBinding;

typedef struct {
    QcomAdspConfig adsp;
    FuxiAdspPlatformBinding platform;
    FuxiMi2sBinding mi2s;
    FuxiI2cBinding i2c8;
    Cs35l41Binding cs35l41;
    FuxiAmpEndpoint amplifiers[2]; /* B then T, matching binding.toml. */
    const FuxiAudioFieldInfo *fields;
    uint32_t field_count;
    uint32_t state_count[3];
} FuxiAudioBinding;

const FuxiAudioBinding *fuxi_audio_binding(void);
const FuxiAudioFieldInfo *fuxi_audio_binding_find(const char *path);

#ifdef __cplusplus
}
#endif
#endif
