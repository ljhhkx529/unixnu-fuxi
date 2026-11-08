/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomGpr.h"

#include <stddef.h>

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void clear_bytes(void *value, uint32_t bytes)
{
    uint8_t *p = value;
    for (uint32_t i = 0; i < bytes; ++i)
        p[i] = 0;
}

uint32_t qcom_gpr_encode(uint8_t *out, uint32_t capacity, const QcomGprPacket *packet,
                         const void *payload, uint32_t payload_bytes)
{
    if (!out || !packet || (payload_bytes && !payload) ||
        payload_bytes > QCOM_GPR_MAX_PACKET - QCOM_GPR_HEADER_BYTES)
        return 0;
    if (packet->dst_domain > 0xffu || packet->src_domain > 0xffu)
        return 0;

    const uint32_t size = QCOM_GPR_HEADER_BYTES + payload_bytes;
    if (size > capacity)
        return 0;

    /* Word 0: version 3:0, header words 7:4, packet bytes 31:8.
     * Word 1: destination domain 7:0, source domain 15:8. */
    put32(out, (QCOM_GPR_VERSION & 0xfu) | (QCOM_GPR_HEADER_BYTES / 4u) << 4 |
               size << 8);
    put32(out + 4, packet->dst_domain | packet->src_domain << 8);
    put32(out + 8, packet->src_port);
    put32(out + 12, packet->dst_port);
    put32(out + 16, packet->token);
    put32(out + 20, packet->opcode);
    for (uint32_t i = 0; i < payload_bytes; ++i)
        out[QCOM_GPR_HEADER_BYTES + i] = ((const uint8_t *)payload)[i];
    return size;
}

int qcom_gpr_decode(const uint8_t *input, uint32_t length, QcomGprPacket *packet)
{
    if (!input || !packet)
        return QcomGprBadArgument;
    clear_bytes(packet, sizeof(*packet));
    if (length < QCOM_GPR_HEADER_BYTES)
        return QcomGprShort;

    const uint32_t word0 = get32(input);
    const uint32_t word1 = get32(input + 4);
    packet->version = word0 & 0xfu;
    packet->header_bytes = ((word0 >> 4) & 0xfu) * 4u;
    packet->size = word0 >> 8;
    if (packet->version != QCOM_GPR_VERSION)
        return QcomGprBadVersion;
    if (packet->header_bytes < QCOM_GPR_HEADER_BYTES || packet->header_bytes > packet->size)
        return QcomGprBadHeader;
    if (packet->size > length || packet->size > QCOM_GPR_MAX_PACKET)
        return QcomGprBadSize;

    packet->dst_domain = word1 & 0xffu;
    packet->src_domain = (word1 >> 8) & 0xffu;
    packet->src_port = get32(input + 8);
    packet->dst_port = get32(input + 12);
    packet->token = get32(input + 16);
    packet->opcode = get32(input + 20);
    packet->payload = input + packet->header_bytes;
    packet->payload_bytes = packet->size - packet->header_bytes;
    return QcomGprOk;
}

int qcom_gpr_basic_result(const QcomGprPacket *packet, uint32_t *opcode, uint32_t *status)
{
    if (!packet || !opcode || !status || packet->opcode != QCOM_GPR_OP_BASIC_RESULT ||
        packet->payload_bytes < 8 || !packet->payload)
        return -1;
    *opcode = get32(packet->payload);
    *status = get32(packet->payload + 4);
    return 0;
}

void qcom_gpr_spf_init(QcomGprSpfProbe *probe, uint32_t port, uint32_t token_seed,
                       uint32_t max_queries)
{
    if (!probe)
        return;
    clear_bytes(probe, sizeof(*probe));
    probe->port = port;
    probe->token = token_seed;
    probe->max_queries = max_queries;
    probe->state = QcomSpfIdle;
}

uint32_t qcom_gpr_spf_next(QcomGprSpfProbe *probe, uint8_t out[QCOM_GPR_HEADER_BYTES])
{
    if (!probe || !out || probe->state != QcomSpfIdle ||
        probe->queries >= probe->max_queries)
        return 0;

    QcomGprPacket packet;
    clear_bytes(&packet, sizeof(packet));
    packet.dst_domain = QCOM_GPR_DOMAIN_ADSP;
    packet.src_domain = QCOM_GPR_DOMAIN_APPS;
    packet.src_port = probe->port;
    packet.dst_port = QCOM_GPR_PORT_APM;
    packet.token = ++probe->token;
    packet.opcode = QCOM_GPR_OP_APM_GET_SPF_STATE;
    const uint32_t size = qcom_gpr_encode(out, QCOM_GPR_HEADER_BYTES, &packet, NULL, 0);
    if (!size)
        return 0;
    probe->queries++;
    probe->state = QcomSpfWaiting;
    return size;
}

static int retry_or_stop(QcomGprSpfProbe *probe)
{
    if (probe->queries >= probe->max_queries) {
        probe->state = QcomSpfGaveUp;
        return QcomSpfDone;
    }
    probe->state = QcomSpfIdle;
    return QcomSpfNotReady;
}

int qcom_gpr_spf_receive(QcomGprSpfProbe *probe, const uint8_t *data, uint32_t length)
{
    if (!probe)
        return QcomSpfIgnored;

    QcomGprPacket packet;
    if (qcom_gpr_decode(data, length, &packet) != QcomGprOk ||
        packet.dst_domain != QCOM_GPR_DOMAIN_APPS ||
        packet.src_domain != QCOM_GPR_DOMAIN_ADSP ||
        packet.src_port != QCOM_GPR_PORT_APM || packet.dst_port != probe->port) {
        probe->ignored++;
        return QcomSpfIgnored;
    }

    probe->last_opcode = packet.opcode;
    if (probe->state != QcomSpfWaiting || packet.token != probe->token) {
        probe->ignored++;
        return QcomSpfIgnored;
    }

    uint32_t opcode = 0;
    uint32_t status = 0;
    if (packet.opcode == QCOM_GPR_OP_APM_SPF_STATE && packet.payload_bytes >= 4) {
        probe->answers++;
        probe->last_status = get32(packet.payload);
        if (probe->last_status == 1) {
            probe->state = QcomSpfReady;
            return QcomSpfIsReady;
        }
        return retry_or_stop(probe);
    }
    if (!qcom_gpr_basic_result(&packet, &opcode, &status) &&
        opcode == QCOM_GPR_OP_APM_GET_SPF_STATE) {
        probe->errors++;
        probe->last_status = status;
        (void)retry_or_stop(probe);
        return QcomSpfError;
    }

    probe->ignored++;
    return QcomSpfIgnored;
}

int qcom_gpr_spf_timeout(QcomGprSpfProbe *probe)
{
    if (!probe || probe->state != QcomSpfWaiting)
        return QcomSpfIgnored;
    probe->timeouts++;
    return retry_or_stop(probe) == QcomSpfDone ? QcomSpfDone : QcomSpfIgnored;
}
