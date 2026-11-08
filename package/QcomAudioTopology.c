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
    if (!in->graph_id || !in->container_id || !in->stream_module || !in->endpoint_module) return QcomTopologyBadInput;
    if (in->stream_instance == in->endpoint_instance) return QcomTopologyDuplicateInstance;
    if (!in->channels || in->channels > 8u) return QcomTopologyBadChannels;
    if (in->bit_width != 16u && in->bit_width != 24u && in->bit_width != 32u) return QcomTopologyBadWidth;
    if (!in->sample_rate || !in->bytes_per_frame) return QcomTopologyBadRate;
    if (in->interface_type != QCOM_AR_INTERFACE_I2S && in->interface_type != QCOM_AR_INTERFACE_CODEC_DMA)
        return QcomTopologyBadInterface;

    /* Slot masks: inside the frame, never overlapping, and exactly as many slots as channels. */
    if (!in->slot_count || in->slot_count > 32u) return QcomTopologySlotRange;
    const uint32_t frame = in->slot_count >= 32u ? 0xffffffffu : ((1u << in->slot_count) - 1u);
    if ((in->slot_mask_a | in->slot_mask_b) & ~frame) return QcomTopologySlotRange;
    if (in->slot_mask_a & in->slot_mask_b) return QcomTopologySlotOverlap;
    if (popcount(in->slot_mask_a | in->slot_mask_b) != in->channels) return QcomTopologySlotMismatch;

    for (uint32_t i = 0; i < sizeof *out; ++i) ((uint8_t *)out)[i] = 0;
    out->modules[0] = (QcomArModule){in->stream_instance, in->stream_module, 0u, 1u};
    out->modules[1] = (QcomArModule){in->endpoint_instance, in->endpoint_module, 1u, 0u};
    out->links[0] = (QcomArLink){in->stream_instance, 1u, in->endpoint_instance, 2u};
    out->graph = (QcomArGraph){
        in->graph_id, in->performance, in->direction, in->scenario,
        in->container_id, in->container_position, in->container_stack_bytes, QCOM_GPR_DOMAIN_ADSP,
        out->modules, 2u, out->links, 1u,
    };
    out->endpoint = (QcomArEndpoint){
        in->endpoint_module, in->endpoint_instance, in->interface_type, in->interface_index,
        in->secondary, in->line, in->ws_source, in->slot_mask_a | in->slot_mask_b,
    };
    out->format = (QcomArFormat){in->media, in->bit_width, in->channels, in->interleaving, in->bytes_per_frame};
    return QcomTopologyOk;
}
