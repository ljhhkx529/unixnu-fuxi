/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the speaker topology: a valid mapping turned into the graph spec, the module and connection lists and
 * the endpoint interface that QcomAudioGraph opens, every inconsistent mapping refused, and the result fed to the
 * builder to show it is usable as it stands. */
#include <stdio.h>
#include <string.h>

#include "../QcomAudioTopology.h"
#include "../ShengGpr.h"

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
    in.apm_iid = 1u;
    in.subgraph_id = 0x5100u;
    in.container_id = 0x9001u;
    in.container_position = QCOM_AR_POS_STREAM;
    in.container_stack_bytes = 8192u;
    in.container_domain = GPR_DOMAIN_ADSP;
    in.performance = QCOM_AR_PERF_LOW_LATENCY;
    in.direction = QCOM_AR_DIR_TX;
    in.scenario = QCOM_AR_SCENARIO_PLAYBACK;
    in.shmem_instance = 0x6001u;
    in.shmem_module = QCOM_AR_MOD_WR_SHMEM;
    in.stream_instance = 0x6003u;
    in.stream_module = QCOM_AR_MOD_PCM_CNV;
    in.endpoint_instance = 0x6051u;
    in.endpoint_module = QCOM_AR_MOD_I2S_SINK;
    in.interface_index = QCOM_AR_LPAIF;
    in.interface_word = QCOM_AR_I2S_SECONDARY;
    in.line = QCOM_AR_I2S_SD1;
    in.ws_source = QCOM_AR_WS_INTERNAL;
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

    check(topo.spec.apm_iid == in.apm_iid && topo.spec.subgraph_id == in.subgraph_id &&
          topo.spec.container_id == in.container_id, "the spec carries the graph's identity");
    check(topo.spec.container_position == in.container_position &&
          topo.spec.container_stack_bytes == in.container_stack_bytes &&
          topo.spec.container_domain == in.container_domain, "the spec carries the container's placement");
    check(topo.spec.performance == in.performance && topo.spec.direction == in.direction &&
          topo.spec.scenario == in.scenario, "the spec carries the subgraph's properties");

    check(topo.module_count == 3, "the path is three modules");
    check(topo.modules[0].module == QCOM_AR_MOD_WR_SHMEM && topo.modules[0].inputs == 0 &&
          topo.modules[0].outputs == 1, "the shared memory source comes first");
    check(topo.modules[1].module == QCOM_AR_MOD_PCM_CNV && topo.modules[1].inputs == 1 &&
          topo.modules[1].outputs == 1, "the converter sits in the middle");
    check(topo.modules[2].module == QCOM_AR_MOD_I2S_SINK && topo.modules[2].outputs == 0,
          "the endpoint is the sink");
    check(topo.link_count == 2, "two connections join them");
    check(topo.links[0].from_instance == in.shmem_instance && topo.links[0].to_instance == in.stream_instance,
          "the first connection runs from shared memory to the converter");
    check(topo.links[1].from_instance == in.stream_instance && topo.links[1].to_instance == in.endpoint_instance,
          "the second runs from the converter to the endpoint");

    check(topo.endpoint_interface.lpaif == in.interface_index &&
          topo.endpoint_interface.interface_type == in.interface_word &&
          topo.endpoint_interface.sd_line == in.line && topo.endpoint_interface.ws_source == in.ws_source,
          "the endpoint interface comes from the caller");
    check(topo.endpoint_format.rate == in.sample_rate && topo.endpoint_format.bits == in.bit_width &&
          topo.endpoint_format.channels == in.channels, "the format comes from the caller");
    check(topo.endpoint_instance == in.endpoint_instance && topo.stream_instance == in.stream_instance &&
          topo.shmem_instance == in.shmem_instance, "the instances are carried through");

    check(qcom_audio_topology_slot_owner(&in, 0) == 1, "slot zero belongs to the first amplifier");
    check(qcom_audio_topology_slot_owner(&in, 1) == 2, "slot one belongs to the second");
    check(qcom_audio_topology_slot_owner(&in, 2) == 0, "an empty slot belongs to nobody");
    check(qcom_audio_topology_slot_owner(&in, 32) == 0, "a slot beyond the frame belongs to nobody");

    /* What the topology produced is what the builder takes. */
    uint8_t packet[512];
    const uint32_t bytes = qcom_ar_graph_open(packet, sizeof packet, 0x6000u, 0x5au, &topo.spec, topo.modules,
                                              topo.module_count, topo.links, topo.link_count);
    check(bytes > 0 && bytes <= GPR_MAX_PACKET, "the builder takes the topology as it stands");
    check(has_word(packet, bytes, in.subgraph_id), "the packet names the subgraph");
    check(has_word(packet, bytes, in.endpoint_instance) && has_word(packet, bytes, in.stream_instance),
          "the packet names both instances");
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

    in = stereo(); in.interface_index = 7u;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadInterface, "an unknown interface word is refused");
    in = stereo(); in.line = 40u;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadInterface, "a data line beyond the interface is refused");

    in = stereo(); in.subgraph_id = 0;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadInput, "a graph without an id is refused");
    in = stereo(); in.endpoint_module = 0;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyBadInput, "an endpoint without a module is refused");

    in = stereo(); in.slot_count = 0;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologySlotRange, "a frame without slots is refused");
    in = stereo(); in.slot_mask_b = 1u << 5;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologySlotRange, "a slot beyond the frame is refused");

    in = stereo(); in.slot_mask_b = in.slot_mask_a;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologySlotOverlap, "two amplifiers on one slot is refused");

    in = stereo(); in.slot_mask_b = 0;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologySlotMismatch,
          "a channel count that disagrees with the masks is refused");

    /* Without a shared memory module the path is two modules and one connection, which a capture path needs. */
    in = stereo(); in.shmem_module = 0; in.channels = 2u;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyOk && topo.module_count == 2 && topo.link_count == 1,
          "a path without a shared memory module collapses to two modules");

    in = stereo(); in.channels = 4; in.slot_mask_a = 0x3u; in.slot_mask_b = 0xcu; in.slot_count = 8;
    check(qcom_audio_topology_build(&in, &topo) == QcomTopologyOk, "four channels over four slots assemble");
}

int main(void)
{
    test_valid();
    test_refusals();
    printf("PASS QcomAudioTopology: %d checks\n", checks);
    return failed ? 1 : 0;
}
