/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* One AudioReach subgraph, opened the way sheng opens it.
 *
 * The mechanism is sheng's ShengAudioReach: the GPR header is written in place and its size patched at the end, the
 * APM command header is in-band with its payload size patched in the same way, and every parameter block is padded to
 * eight bytes with its body size recorded. Those are protocol shapes, so they are copied rather than re-derived.
 *
 * What is not copied is the board: the APM instance, the subgraph and container identity, the modules with their
 * instance ids, ports and connections, the container's position and domain, and the stack size all arrive in
 * QcomArGraphSpec. sheng's own subgraph numbers (0x4005, 0x4007, the TX3 interface) are sheng's values, not facts
 * about this board, so this file holds none of them.
 *
 * The parameter and property ids below are the framework's, and match sheng's. */
#ifndef QCOM_AUDIOGRAPH_H
#define QCOM_AUDIOGRAPH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The framework's parameter and property ids, unchanged from sheng. */
#define QCOM_AR_PARAM_SG_CONFIG 0x08001001u
#define QCOM_AR_PARAM_CONTAINER_CONFIG 0x08001000u
#define QCOM_AR_PARAM_MODULE_LIST 0x08001002u
#define QCOM_AR_PARAM_MODULE_PROP 0x08001003u
#define QCOM_AR_PARAM_MODULE_CONN 0x08001004u
#define QCOM_AR_PROP_SG_PERF 0x0800100Eu
#define QCOM_AR_PROP_SG_DIRECTION 0x0800100Fu
#define QCOM_AR_PROP_SG_SCENARIO 0x08001010u
#define QCOM_AR_PROP_CONT_CAPS 0x08001011u
#define QCOM_AR_PROP_CONT_POS 0x08001012u
#define QCOM_AR_PROP_CONT_STACK 0x08001013u
#define QCOM_AR_PROP_CONT_DOMAIN 0x08001014u
#define QCOM_AR_PROP_PORT_INFO 0x08001015u
#define QCOM_AR_CONTAINER_GC 0x0B001001u
#define QCOM_AR_OP_GRAPH_OPEN 0x01001000u
#define QCOM_AR_OUT_PORT 1u
#define QCOM_AR_IN_PORT 2u

/* Where the subgraph lives and what it is for, from the board's topology. */
typedef struct {
    uint32_t apm_iid;
    uint32_t subgraph_id, performance, direction, scenario;
    uint32_t container_id, container_position, container_stack_bytes, container_domain;
} QcomArGraphSpec;

typedef struct {
    uint32_t module, instance, inputs, outputs;
} QcomArGraphModule;

typedef struct {
    uint32_t from_instance, to_instance;
} QcomArGraphLink;

/* Writes one graph-open packet into out; returns its size, or 0 when it does not fit. */
uint32_t qcom_ar_graph_open(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token,
                            const QcomArGraphSpec *spec, const QcomArGraphModule *modules, uint32_t module_count,
                            const QcomArGraphLink *links, uint32_t link_count);

#ifdef __cplusplus
}
#endif

#endif
