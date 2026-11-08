/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAudioTopology.h"

static uint32_t popcount(uint32_t v)
{
    uint32_t n = 0;
    while (v) {v &= v - 1u; ++n;}
    return n;
}

int qcom_audio_topology_slot_owner(const QcomAudioTopologyInputs *in, uint32_t slot)
{
    if (!in || slot >= 32u) return 0;
    const uint32_t bit = 1u << slot;
    if (in->slot_mask_a & bit) return 1;
    if (in->slot_mask_b & bit) return 2;
    return 0;
}

int qcom_audio_topology_build(const QcomAudioTopologyInputs *in, QcomAudioTopology *out)
{
    if (!in || !out) return QcomTopologyBadInput;
    if (!in->subgraph_id || !in->container_id || !in->stream_module || !in->endpoint_module) return QcomTopologyBadInput;
    if (in->stream_instance == in->endpoint_instance) return QcomTopologyDuplicateInstance;
    if (!in->channels || in->channels > 8u) return QcomTopologyBadChannels;
    if (in->bit_width != 16u && in->bit_width != 24u && in->bit_width != 32u) return QcomTopologyBadWidth;
    if (!in->sample_rate || !in->bytes_per_frame) return QcomTopologyBadRate;
    /* The interface word is the framework's: LPAIF for an I2S endpoint, LPAIF_RXTX for a codec DMA one. Anything
     * else is a board fact that has not been established, so it is refused rather than passed to the DSP. */
    if (in->interface_index != QCOM_AR_LPAIF && in->interface_index != QCOM_AR_LPAIF_RXTX)
        return QcomTopologyBadInterface;
    if (in->line > 31u) return QcomTopologyBadInterface;

    /* Slot masks: inside the frame, never overlapping, and exactly as many slots as channels. */
    if (!in->slot_count || in->slot_count > 32u) return QcomTopologySlotRange;
    const uint32_t frame = in->slot_count >= 32u ? 0xffffffffu : ((1u << in->slot_count) - 1u);
    if ((in->slot_mask_a | in->slot_mask_b) & ~frame) return QcomTopologySlotRange;
    if (in->slot_mask_a & in->slot_mask_b) return QcomTopologySlotOverlap;
    if (popcount(in->slot_mask_a | in->slot_mask_b) != in->channels) return QcomTopologySlotMismatch;

    for (uint32_t i = 0; i < sizeof *out; ++i) ((uint8_t *)out)[i] = 0;
    out->spec = (QcomArGraphSpec){
        in->apm_iid, in->subgraph_id, in->performance, in->direction, in->scenario,
        in->container_id, in->container_position, in->container_stack_bytes, in->container_domain,
    };
    /* The stream side takes audio from shared memory and hands it to the endpoint; when they share an instance the
     * pair collapses into one module. */
    uint32_t count = 0;
    if (in->shmem_module && in->shmem_instance != in->stream_instance) {
        out->modules[count++] = (QcomArGraphModule){in->shmem_module, in->shmem_instance, 0u, 1u};
    }
    out->modules[count++] = (QcomArGraphModule){in->stream_module, in->stream_instance, 1u, 1u};
    out->modules[count++] = (QcomArGraphModule){in->endpoint_module, in->endpoint_instance, 1u, 0u};
    out->module_count = count;

    uint32_t links = 0;
    if (count == 3u) out->links[links++] = (QcomArGraphLink){in->shmem_instance, in->stream_instance};
    out->links[links++] = (QcomArGraphLink){in->stream_instance, in->endpoint_instance};
    out->link_count = links;

    out->endpoint_interface = (QcomArI2sInterface){in->interface_index, in->interface_word, in->line, in->ws_source};
    out->endpoint_format = (QcomArFormat){in->sample_rate, (uint16_t)in->bit_width, (uint16_t)in->channels};
    out->stream_format = out->endpoint_format;
    out->endpoint_instance = in->endpoint_instance;
    out->stream_instance = in->stream_instance;
    out->shmem_instance = in->shmem_instance;
    return QcomTopologyOk;
}
