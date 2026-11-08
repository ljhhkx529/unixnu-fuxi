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
#define QCOM_AR_PARAM_SG_LIST 0x08001005u
#define QCOM_AR_PARAM_PCM_OUTPUT 0x08001008u
#define QCOM_AR_PARAM_MEDIA_FORMAT 0x0800100Cu
#define QCOM_AR_PARAM_HW_EP_MF 0x08001017u
#define QCOM_AR_PARAM_FRAME_SIZE_FACTOR 0x08001018u
#define QCOM_AR_PARAM_I2S_INTF 0x08001019u
#define QCOM_AR_PARAM_HW_CLOCK 0x0800102Cu
#define QCOM_AR_PARAM_HW_CORE 0x08001032u
#define QCOM_AR_OP_GRAPH_OPEN 0x01001000u
#define QCOM_AR_OP_GRAPH_PREPARE 0x01001001u
#define QCOM_AR_OP_GRAPH_START 0x01001002u
#define QCOM_AR_OP_GRAPH_STOP 0x01001003u
#define QCOM_AR_OP_GRAPH_CLOSE 0x01001004u
#define QCOM_AR_OP_SET_CFG 0x01001006u
#define QCOM_AR_OP_MEM_MAP 0x0100100Cu
#define QCOM_AR_OP_MEM_UNMAP 0x0100100Du
#define QCOM_AR_OP_MEM_MAP_DONE 0x02001001u
#define QCOM_AR_OP_WRITE 0x0400100Au
#define QCOM_AR_OP_WRITE_DONE 0x05001004u
#define QCOM_AR_OP_READ 0x0400100Bu
#define QCOM_AR_OP_READ_DONE 0x05001005u
#define QCOM_AR_OP_PRM_REQUEST 0x0100100Fu
#define QCOM_AR_OP_PRM_RELEASE 0x01001010u
#define QCOM_AR_OP_PRM_REQUEST_DONE 0x02001002u
#define QCOM_AR_OP_PRM_RELEASE_DONE 0x02001003u
#define QCOM_AR_MEDIA_PCM 0x09001000u
#define QCOM_AR_INTERLEAVED 1u
#define QCOM_AR_DEINTERLEAVED_UNPACKED 3u
#define QCOM_AR_PRM_IID 2u
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

/* The sample format, and the interface an endpoint sits on, both from the board. */
typedef struct {
    uint32_t rate;
    uint16_t bits;
    uint16_t channels;
} QcomArFormat;

typedef struct {
    uint32_t lpaif, interface_type, sd_line, ws_source;
} QcomArI2sInterface;

/* What a received packet turned out to be, and the fields that carry its answer. */
enum {
    QcomArAnswerBad = -1,
    QcomArAnswerUnknown = 0,
    QcomArAnswerBasicResult = 1,
    QcomArAnswerPrmDone = 2,
    QcomArAnswerFrameworkState = 3,
    QcomArAnswerMapped = 4,
    QcomArAnswerWriteDone = 5,
    QcomArAnswerReadDone = 6,
};

typedef struct {
    uint32_t opcode, token, src_port;
    uint32_t result_opcode, status;
    uint32_t handle;
    uint64_t address;
    uint32_t data_bytes, offset;
} QcomArAnswer;

/* Writes one graph-open packet into out; returns its size, or 0 when it does not fit. */
uint32_t qcom_ar_graph_open(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token,
                            const QcomArGraphSpec *spec, const QcomArGraphModule *modules, uint32_t module_count,
                            const QcomArGraphLink *links, uint32_t link_count);

/* A hardware endpoint's format and interface: the SET_CFG that configures where the graph's audio leaves the DSP. */
uint32_t qcom_ar_config_hw_endpoint(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t apm_iid,
                                    uint32_t endpoint_iid, const QcomArI2sInterface *intf, const QcomArFormat *format);
/* One converter's output format inside a stream graph. */
uint32_t qcom_ar_config_pcm_output(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t apm_iid,
                                   uint32_t instance, const QcomArFormat *format, uint32_t interleaving);
/* The shared memory endpoint's own media format, addressed to that endpoint rather than to the APM. */
uint32_t qcom_ar_config_media_format(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t instance,
                                     const QcomArFormat *format);
/* Prepare, start, stop or close one subgraph. */
uint32_t qcom_ar_graph_command(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t apm_iid,
                               uint32_t opcode, uint32_t subgraph_id);
/* Maps one physically contiguous region, rounded up to 4 KiB; the answer carries the handle. */
uint32_t qcom_ar_map(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t apm_iid,
                     uint64_t address, uint32_t bytes);
uint32_t qcom_ar_unmap(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t apm_iid,
                       uint32_t handle);
/* One buffer for a shared memory endpoint; the answer repeats the address. */
uint32_t qcom_ar_write(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t shmem_iid,
                       uint64_t address, uint32_t handle, uint32_t bytes);
uint32_t qcom_ar_read(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t shmem_iid,
                      uint64_t address, uint32_t handle, uint32_t bytes);
/* The DSP's resource manager: a clock at hz (zero releases it), and a hardware block vote. */
uint32_t qcom_ar_prm_clock(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t clock_id,
                           uint32_t hz);
uint32_t qcom_ar_prm_vote(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t block, int enable);
/* What arrived: the kind of answer and the fields that carry it. */
int qcom_ar_parse(const void *packet, uint32_t length, QcomArAnswer *answer);

#ifdef __cplusplus
}
#endif

#endif
