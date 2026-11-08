/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Speaker topology assembled from explicit inputs, and the consistency checks that keep a wrong mapping from
 * reaching the DSP.
 *
 * The board's amplifier count, which slots each one carries, the interface it hangs on and the sample format all
 * arrive as inputs. Nothing here invents a rate, a slot mask or a module id: the builder fills the AudioReach
 * structures the layers below take, and refuses a mapping that cannot be right - overlapping slots, a channel
 * count that disagrees with the masks, a width the endpoint cannot take, two modules sharing an instance. */
#ifndef QCOM_AUDIOTOPOLOGY_H
#define QCOM_AUDIOTOPOLOGY_H

#include <stdint.h>

#include "QcomAudioReach.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_TOPOLOGY_MAX_MODULES 4u

typedef struct {
    /* graph identity, from the board */
    uint32_t graph_id, container_id, container_position, container_stack_bytes;
    uint32_t performance, direction, scenario;
    /* the modules that make the path: the stream side first, then the endpoint */
    uint32_t stream_instance, stream_module;
    uint32_t endpoint_instance, endpoint_module;
    /* the endpoint's interface */
    uint32_t interface_type, interface_index, secondary, line, ws_source;
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

/* The assembled topology: the caller owns it, and the graph points into this same structure, so nothing is
 * static and nothing is thread local. */
typedef struct {
    QcomArModule modules[QCOM_TOPOLOGY_MAX_MODULES];
    QcomArLink links[QCOM_TOPOLOGY_MAX_MODULES];
    QcomArGraph graph;
    QcomArEndpoint endpoint;
    QcomArFormat format;
} QcomAudioTopology;

/* Fills the topology from the inputs: the stream module, the endpoint module, the link between them, and the
 * endpoint and format the layers below take. */
int qcom_audio_topology_build(const QcomAudioTopologyInputs *in, QcomAudioTopology *out);

/* Which amplifier owns a slot, 0 for none: a slot may carry only one amplifier. */
int qcom_audio_topology_slot_owner(const QcomAudioTopologyInputs *in, uint32_t slot);

#ifdef __cplusplus
}
#endif

#endif
