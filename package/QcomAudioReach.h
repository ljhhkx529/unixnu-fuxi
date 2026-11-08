/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* AudioReach (APM/PRM) command payload builder and answer parser.
 *
 * Board independent by construction: every graph id, module instance, interface index, clock id and slot mask
 * comes from the topology the caller passes in. The only constants are the AudioReach property ids, module type
 * ids and APM opcodes from the specification, which are the same on every board that speaks this protocol.
 *
 * The builders produce the APM command payload; QcomGpr frames it (version, domains, ports, token) and decodes
 * the reply, and qcom_ar_parse interprets that decoded packet. No GLINK, address, firmware or pin knowledge
 * lives here. */
#ifndef QCOM_AUDIOREACH_H
#define QCOM_AUDIOREACH_H

#include <stdint.h>
#include "QcomGpr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_AR_CMD_HEADER_BYTES 16u
#define QCOM_AR_PARAM_HEADER_BYTES 16u
#define QCOM_AR_MAX_MODULES 8u
#define QCOM_AR_MAX_LINKS 8u

/* APM instance the commands are addressed to, and the opcodes from the specification. */
#define QCOM_AR_APM_INSTANCE 1u
#define QCOM_AR_OP_GRAPH_OPEN 0x01001000u
#define QCOM_AR_OP_GRAPH_PREPARE 0x01001001u
#define QCOM_AR_OP_GRAPH_START 0x01001002u
#define QCOM_AR_OP_GRAPH_STOP 0x01001003u
#define QCOM_AR_OP_GRAPH_CLOSE 0x01001004u
#define QCOM_AR_OP_SET_CFG 0x01001006u
#define QCOM_AR_OP_MEM_MAP 0x0100100Cu
#define QCOM_AR_OP_MEM_UNMAP 0x0100100Du
#define QCOM_AR_OP_MEM_MAP_DONE 0x02001001u
#define QCOM_AR_OP_DATA_WRITE 0x0400100Au
#define QCOM_AR_OP_DATA_WRITE_DONE 0x05001004u
#define QCOM_AR_OP_DATA_READ 0x0400100Bu
#define QCOM_AR_OP_DATA_READ_DONE 0x05001005u
#define QCOM_AR_OP_PRM_REQUEST_DONE 0x02001002u
#define QCOM_AR_OP_PRM_RELEASE_DONE 0x02001003u

/* Property and parameter ids (specification). */
#define QCOM_AR_PARAM_SG_CONFIG 0x08001001u
#define QCOM_AR_PARAM_CONTAINER_CONFIG 0x08001000u
#define QCOM_AR_PARAM_MODULE_LIST 0x08001002u
#define QCOM_AR_PARAM_MODULE_PROP 0x08001003u
#define QCOM_AR_PARAM_MODULE_CONN 0x08001004u
#define QCOM_AR_PARAM_SG_LIST 0x08001005u
#define QCOM_AR_PARAM_MEDIA_FORMAT 0x0800100Cu
#define QCOM_AR_PARAM_HW_EP_MF 0x08001017u
#define QCOM_AR_PARAM_FRAME_SIZE_FACTOR 0x08001018u
#define QCOM_AR_PARAM_CODEC_DMA_INTF 0x08001063u
#define QCOM_AR_PARAM_PCM_OUTPUT 0x08001008u
#define QCOM_AR_PARAM_I2S_INTF 0x08001019u
#define QCOM_AR_PARAM_CLOCK_ID 0x08001027u
#define QCOM_AR_PARAM_CLOCK_FREQUENCY 0x08001028u
#define QCOM_AR_PROP_SG_PERF 0x0800100Eu
#define QCOM_AR_PROP_SG_DIRECTION 0x0800100Fu
#define QCOM_AR_PROP_SG_SCENARIO 0x08001010u
#define QCOM_AR_PROP_CONTAINER_CAPABILITY 0x08001011u
#define QCOM_AR_PROP_CONTAINER_POSITION 0x08001012u
#define QCOM_AR_PROP_CONTAINER_STACK_SIZE 0x08001013u
#define QCOM_AR_PROP_CONTAINER_DOMAIN 0x08001014u
#define QCOM_AR_PROP_MODULE_PORT_INFO 0x08001015u

/* Module type ids (specification); the instances stay the caller's. */
#define QCOM_AR_MOD_SHARED_MEMORY 0x07001001u
#define QCOM_AR_MOD_PCM_DECODE 0x0700100Cu
#define QCOM_AR_MOD_PCM_CONVERT 0x07001010u
#define QCOM_AR_MOD_I2S_SINK 0x0700101Au

/* Direction, scenario, container and media values (specification). */
#define QCOM_AR_DIR_TX 1u
#define QCOM_AR_DIR_RX 2u
#define QCOM_AR_SCENARIO_PLAYBACK 1u
#define QCOM_AR_SCENARIO_RECORD 2u
#define QCOM_AR_POSITION_STREAM 1u
#define QCOM_AR_CONTAINER_GLOBAL 0x0B001001u
#define QCOM_AR_MEDIA_PCM 0x09001000u
#define QCOM_AR_INTERLEAVED 1u
#define QCOM_AR_DEINTERLEAVED_UNPACKED 3u

typedef struct {
    uint32_t instance;   /* the id this module is known by inside the graph */
    uint32_t module;     /* the specification module type id */
    uint32_t inputs;
    uint32_t outputs;
} QcomArModule;

typedef struct {
    uint32_t from_instance;
    uint32_t from_port;
    uint32_t to_instance;
    uint32_t to_port;
} QcomArLink;

typedef struct {
    uint32_t graph_id;            /* subgraph id, the caller's */
    uint32_t performance;         /* the caller's vote */
    uint32_t direction;
    uint32_t scenario;
    uint32_t container_id;
    uint32_t container_position;
    uint32_t container_stack_bytes;
    uint32_t container_domain;    /* AudioReach domain of the container */
    const QcomArModule *modules;
    uint32_t module_count;
    const QcomArLink *links;
    uint32_t link_count;
} QcomArGraph;

typedef struct {
    uint32_t media;               /* QCOM_AR_MEDIA_PCM and friends */
    uint32_t bit_width;
    uint32_t channels;
    uint32_t interleaving;
    uint32_t bytes_per_frame;     /* frame size factor */
} QcomArFormat;

/* Which interface kind a hardware endpoint drives; the two carry different parameters. */
#define QCOM_AR_INTERFACE_I2S 1u
#define QCOM_AR_INTERFACE_CODEC_DMA 2u

/* One hardware endpoint: the module instance that drives it, and the interface plus the values that interface
 * needs. Every field is the board's; nothing here has a default. */
typedef struct {
    uint32_t module;
    uint32_t instance;
    uint32_t interface_type;      /* QCOM_AR_INTERFACE_I2S or _CODEC_DMA */
    uint32_t interface_index;     /* the interface instance: an LPAIF port or a codec DMA index */
    uint32_t secondary;           /* I2S: 0 master, 1 secondary */
    uint32_t line;                /* I2S: which data line */
    uint32_t ws_source;           /* I2S: word-select source */
    uint32_t slot_mask;           /* codec DMA: which slots carry audio */
} QcomArEndpoint;

/* The builders return the payload size, or zero when the input does not fit or is out of range. */
uint32_t qcom_ar_open_graph(uint8_t *out, uint32_t capacity, const QcomArGraph *graph);
uint32_t qcom_ar_graph_command(uint8_t *out, uint32_t capacity, uint32_t apm_instance, uint32_t graph_id);
uint32_t qcom_ar_configure_endpoint(uint8_t *out, uint32_t capacity, const QcomArEndpoint *endpoint,
                                    const QcomArFormat *format);
uint32_t qcom_ar_configure_stream(uint8_t *out, uint32_t capacity, uint32_t instance, const QcomArFormat *format);
uint32_t qcom_ar_prm_clock(uint8_t *out, uint32_t capacity, uint32_t prm_instance, uint32_t clock_id, uint32_t hz);
uint32_t qcom_ar_map(uint8_t *out, uint32_t capacity, uint32_t instance, uint64_t address, uint32_t bytes);
uint32_t qcom_ar_unmap(uint8_t *out, uint32_t capacity, uint32_t instance, uint32_t handle);

typedef struct {
    uint32_t opcode;         /* the packet's opcode */
    uint32_t result_opcode;  /* basic result: the command it answers */
    uint32_t status;
    uint32_t handle;         /* map done */
    uint64_t address;        /* data done: the buffer */
    uint32_t data_bytes;     /* data read done: bytes filled */
    uint32_t offset;
} QcomArAnswer;

enum {
    QcomArAnswerBad = -1,
    QcomArAnswerUnknown = 0,
    QcomArAnswerBasicResult = 1,
    QcomArAnswerMapped = 2,
    QcomArAnswerFrameworkState = 3,
    QcomArAnswerPrmDone = 4,
    QcomArAnswerDataDone = 5,
};

/* Interprets one decoded GPR packet. Rejects a payload that is too short for what its opcode promises. */
int qcom_ar_parse(const QcomGprPacket *packet, QcomArAnswer *answer);

#ifdef __cplusplus
}
#endif

#endif
