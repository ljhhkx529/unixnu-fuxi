/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Qualcomm Generic Packet Router packet codec and APM readiness transaction model.
 *
 * This layer has no board addresses, firmware paths, graph topology or I/O. The owner supplies a GLINK channel,
 * sends the encoded bytes, feeds received bytes back, and owns all timers and restart generations. */
#ifndef QCOM_GPR_H
#define QCOM_GPR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_GPR_HEADER_BYTES 24u
#define QCOM_GPR_VERSION 0u
#define QCOM_GPR_DOMAIN_MODEM 1u
#define QCOM_GPR_DOMAIN_ADSP 2u
#define QCOM_GPR_DOMAIN_APPS 3u
#define QCOM_GPR_PORT_APM 1u
#define QCOM_GPR_OP_BASIC_RESULT 0x02001005u
#define QCOM_GPR_OP_APM_GET_SPF_STATE 0x01001021u
#define QCOM_GPR_OP_APM_SPF_STATE 0x02001007u
#define QCOM_GPR_MAX_PACKET 512u

typedef struct {
    uint32_t version;
    uint32_t header_bytes;
    uint32_t size;
    uint32_t dst_domain;
    uint32_t src_domain;
    uint32_t src_port;
    uint32_t dst_port;
    uint32_t token;
    uint32_t opcode;
    const uint8_t *payload;
    uint32_t payload_bytes;
} QcomGprPacket;

enum {
    QcomGprOk = 0,
    QcomGprShort = -1,
    QcomGprBadVersion = -2,
    QcomGprBadHeader = -3,
    QcomGprBadSize = -4,
    QcomGprBadArgument = -5,
};

/* Writes one header and payload. Returns the packet size or zero when the input is invalid or does not fit. */
uint32_t qcom_gpr_encode(uint8_t *out, uint32_t capacity, const QcomGprPacket *packet,
                         const void *payload, uint32_t payload_bytes);

/* Decodes one packet from a possibly larger GLINK intent. The returned payload points into input. */
int qcom_gpr_decode(const uint8_t *input, uint32_t length, QcomGprPacket *packet);

/* Extracts {command opcode, status} from a basic-result response. */
int qcom_gpr_basic_result(const QcomGprPacket *packet, uint32_t *opcode, uint32_t *status);

enum {
    QcomSpfIdle,
    QcomSpfWaiting,
    QcomSpfReady,
    QcomSpfGaveUp,
};

enum {
    QcomSpfIgnored = 0,
    QcomSpfNotReady = 1,
    QcomSpfIsReady = 2,
    QcomSpfError = 3,
    QcomSpfDone = 4,
};

typedef struct {
    uint32_t port;
    uint32_t token;
    uint32_t state;
    uint32_t queries;
    uint32_t max_queries;
    uint32_t answers;
    uint32_t errors;
    uint32_t timeouts;
    uint32_t ignored;
    uint32_t last_status;
    uint32_t last_opcode;
} QcomGprSpfProbe;

/* token_seed must change across ADSP restart generations so a delayed response cannot match a new query. */
void qcom_gpr_spf_init(QcomGprSpfProbe *probe, uint32_t port, uint32_t token_seed,
                       uint32_t max_queries);

/* Returns the query size, or zero while a query is in flight or after reaching a terminal state. */
uint32_t qcom_gpr_spf_next(QcomGprSpfProbe *probe, uint8_t out[QCOM_GPR_HEADER_BYTES]);

/* Consumes one received packet. Packets for another domain, port or token are ignored. */
int qcom_gpr_spf_receive(QcomGprSpfProbe *probe, const uint8_t *data, uint32_t length);

/* Records expiry of the current answer timer. */
int qcom_gpr_spf_timeout(QcomGprSpfProbe *probe);

#ifdef __cplusplus
}
#endif

#endif
