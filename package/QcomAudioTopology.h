/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The board's speaker topology turned into what QcomAudioGraph opens and QcomAudioPath drives.
 *
 * The inputs are the board's facts - which modules, on which instances, on which interface, in which format, with
 * which slots per amplifier - and the output is the graph spec, the module and connection lists, the endpoint
 * interface and the two formats. Nothing here invents a rate, a slot mask or a module id, and a mapping that cannot
 * be right is refused rather than passed on. */
#ifndef QCOM_AUDIOTOPOLOGY_H
#define QCOM_AUDIOTOPOLOGY_H

#include <stdint.h>

#include "QcomAudioGraph.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_TOPOLOGY_MAX_MODULES 4u

typedef struct {
    /* where the graph lives */
    uint32_t apm_iid;
    uint32_t subgraph_id, container_id, container_position, container_stack_bytes, container_domain;
    uint32_t performance, direction, scenario;
    /* the modules that make the path: the stream side first, then the endpoint */
    uint32_t stream_instance, stream_module;
    uint32_t endpoint_instance, endpoint_module;
    uint32_t shmem_instance, shmem_module;
    /* the endpoint's interface */
    uint32_t interface_index, interface_word, line, ws_source;
    /* the format both modules carry */
    uint32_t media, sample_rate, bit_width, channels, interleaving, bytes_per_frame;
    /* which slots carry audio, and how many slots the frame has */
    uint32_t slot_mask_a, slot_mask_b, slot_count;
} QcomAudioTopologyInputs;

enum {
    QcomTopologyOk = 0,
    QcomTopologyBadInput = -1,
    QcomTopologyBadChannels = -2,
    QcomTopologyBadWidth = -3,
    QcomTopologySlotOverlap = -4,
    QcomTopologySlotMismatch = -5,
    QcomTopologyBadInterface = -6,
    QcomTopologyDuplicateInstance = -7,
    QcomTopologyBadRate = -8,
    QcomTopologySlotRange = -9,
};

typedef struct {
    QcomArGraphSpec spec;
    QcomArGraphModule modules[QCOM_TOPOLOGY_MAX_MODULES];
    QcomArGraphLink links[QCOM_TOPOLOGY_MAX_MODULES];
    uint32_t module_count, link_count;
    QcomArI2sInterface endpoint_interface;
    QcomArFormat endpoint_format, stream_format;
    uint32_t endpoint_instance, stream_instance, shmem_instance;
} QcomAudioTopology;

/* Fills the topology: shmem -> converter -> endpoint, joined by two connections, with the endpoint's interface and
 * the format the caller supplied. */
int qcom_audio_topology_build(const QcomAudioTopologyInputs *in, QcomAudioTopology *out);

/* Which amplifier owns a slot, 0 for none: a slot may carry only one amplifier. */
int qcom_audio_topology_slot_owner(const QcomAudioTopologyInputs *in, uint32_t slot);

#ifdef __cplusplus
}
#endif

#endif
