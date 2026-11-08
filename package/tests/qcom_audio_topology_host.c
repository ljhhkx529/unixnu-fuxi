/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the speaker topology: a valid mapping assembled from inputs, every inconsistent mapping refused,
 * and the assembled structures fed to the AudioReach builders to show they are usable as they are. */
#include <stdio.h>
#include <string.h>

#include "../QcomAudioTopology.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int has_word(const uint8_t *b, uint32_t n, uint32_t value)
{
    for (uint32_t i = 0; i + 4u <= n; i += 4u) if (le32(b + i) == value) return 1;
    return 0;
}

static QcomAudioTopologyInputs stereo(void)
{
    QcomAudioTopologyInputs in;
    memset(&in, 0, sizeof in);
    in.graph_id = 0x7001u;
    in.container_id = 0x8001u;
    in.container_position = QCOM_AR_POSITION_STREAM;
    in.container_stack_bytes = 8192u;
    in.performance = 2u;
    in.direction = QCOM_AR_DIR_TX;
    in.scenario = QCOM_AR_SCENARIO_PLAYBACK;
    in.stream_instance = 0x101u;
    in.stream_module = QCOM_AR_MOD_PCM_CONVERT;
    in.endpoint_instance = 0x102u;
    in.endpoint_module = QCOM_AR_MOD_I2S_SINK;
    in.interface_type = QCOM_AR_INTERFACE_CODEC_DMA;
    in.interface_index = 4u;
    in.media = QCOM_AR_MEDIA_PCM;
    in.sample_rate = 48000u;
    in.bit_width = 16u;
    in.channels = 2u;
    in.interleaving = QCOM_AR_INTERLEAVED;
    in.bytes_per_frame = 4u;
    in.slot_mask_a = 1u << 0;
    in.slot_mask_b = 1u << 1;
    in.slot_count = 4u;
    return in;
}

static void test_valid(void)
{
    const QcomAudioTopologyInputs in = stereo();
    QcomAudioTopology topo;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyOk, "a consistent stereo mapping assembles");

    check(topo.graph.graph_id == in.graph_id && topo.graph.container_id == in.container_id,
          "the graph carries its identity");
    check(topo.graph.module_count == 2 && topo.graph.link_count == 1, "the graph has the two modules and one link");
    check(topo.graph.modules == topo.modules && topo.graph.links == topo.links,
          "the graph points into the caller's structure, not at anything static");
    check(topo.modules[0].instance == in.stream_instance && topo.modules[0].module == in.stream_module &&
          topo.modules[0].inputs == 0 && topo.modules[0].outputs == 1, "the stream module is the source");
    check(topo.modules[1].instance == in.endpoint_instance && topo.modules[1].inputs == 1 &&
          topo.modules[1].outputs == 0, "the endpoint module is the sink");
    check(topo.links[0].from_instance == in.stream_instance && topo.links[0].to_instance == in.endpoint_instance,
          "the link runs from the stream to the endpoint");
    check(topo.endpoint.module == in.endpoint_module && topo.endpoint.instance == in.endpoint_instance,
          "the endpoint names its module");
    check(topo.endpoint.interface_index == in.interface_index && topo.endpoint.slot_mask == 0x3u,
          "the endpoint carries its interface and the union of both slot masks");
    check(topo.format.bit_width == 16u && topo.format.channels == 2u && topo.format.bytes_per_frame == 4u,
          "the format carries the caller's values");

    check(qcom_audio_topology_slot_owner(&in, 0) == 1, "slot zero belongs to the first amplifier");
    check(qcom_audio_topology_slot_owner(&in, 1) == 2, "slot one belongs to the second");
    check(qcom_audio_topology_slot_owner(&in, 2) == 0, "an empty slot belongs to nobody");
    check(qcom_audio_topology_slot_owner(&in, 32) == 0, "a slot beyond the frame belongs to nobody");

    /* The assembled structures are what the layers below take. */
    uint8_t packet[512];
    const uint32_t graph_bytes = qcom_ar_open_graph(packet, sizeof packet, &topo.graph);
    check(graph_bytes > 0 && has_word(packet, graph_bytes, in.graph_id), "the AudioReach builder takes the graph as it is");
    check(has_word(packet, graph_bytes, in.endpoint_instance), "the open command names both modules");
    const uint32_t ep_bytes = qcom_ar_configure_endpoint(packet, sizeof packet, &topo.endpoint, &topo.format);
    check(ep_bytes > 0 && has_word(packet, ep_bytes, 0x3u), "the endpoint command carries the slot mask");
}

static void test_refusals(void)
{
    QcomAudioTopology topo;

    QcomAudioTopologyInputs in = stereo();
    in.endpoint_instance = in.stream_instance;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyDuplicateInstance, "two modules on one instance is refused");

    in = stereo(); in.channels = 0;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadChannels, "zero channels is refused");
    in = stereo(); in.channels = 9;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadChannels, "nine channels is refused");

    in = stereo(); in.bit_width = 20;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadWidth, "a width the endpoint cannot take is refused");

    in = stereo(); in.sample_rate = 0;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadRate, "a zero rate is refused");
    in = stereo(); in.bytes_per_frame = 0;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadRate, "a zero frame size is refused");

    in = stereo(); in.interface_type = 9;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadInterface, "an unknown interface is refused");

    in = stereo(); in.graph_id = 0;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadInput, "a graph without an id is refused");
    in = stereo(); in.endpoint_module = 0;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadInput, "an endpoint without a module is refused");

    in = stereo(); in.slot_count = 0;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologySlotRange, "a frame without slots is refused");
    in = stereo(); in.slot_mask_b = 1u << 5;   /* beyond a four-slot frame */
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologySlotRange, "a slot beyond the frame is refused");

    in = stereo(); in.slot_mask_b = in.slot_mask_a;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologySlotOverlap, "two amplifiers on one slot is refused");

    in = stereo(); in.slot_mask_b = 0;   /* one slot, two channels */
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologySlotMismatch,
          "a channel count that disagrees with the masks is refused");

    in = stereo(); in.channels = 4; in.slot_mask_a = 0x3u; in.slot_mask_b = 0xcu; in.slot_count = 8;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyOk && topo.endpoint.slot_mask == 0xfu,
          "four channels over four slots assemble");
}

int main(void)
{
    test_valid();
    test_refusals();
    printf("PASS QcomAudioTopology: %d checks\n", checks);
    return failed ? 1 : 0;
}
