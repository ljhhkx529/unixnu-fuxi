// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../QcomGpr.h"

static unsigned checks;
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
    ++checks; \
} while (0)

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t reply(uint8_t *out, const uint8_t *query, uint32_t opcode,
                      const uint32_t *words, unsigned count)
{
    QcomGprPacket request;
    QcomGprPacket response = {0};
    CHECK(qcom_gpr_decode(query, QCOM_GPR_HEADER_BYTES, &request) == QcomGprOk);
    response.dst_domain = request.src_domain;
    response.src_domain = request.dst_domain;
    response.src_port = request.dst_port;
    response.dst_port = request.src_port;
    response.token = request.token;
    response.opcode = opcode;
    return qcom_gpr_encode(out, QCOM_GPR_MAX_PACKET, &response, words, 4u * count);
}

int main(void)
{
    uint8_t buffer[QCOM_GPR_MAX_PACKET] = {0};
    uint8_t query[QCOM_GPR_HEADER_BYTES] = {0};
    uint8_t answer[QCOM_GPR_MAX_PACKET] = {0};
    QcomGprPacket packet = {0};
    QcomGprPacket decoded;

    packet.dst_domain = QCOM_GPR_DOMAIN_ADSP;
    packet.src_domain = QCOM_GPR_DOMAIN_APPS;
    packet.src_port = 1;
    packet.dst_port = QCOM_GPR_PORT_APM;
    packet.token = 0x1234;
    packet.opcode = QCOM_GPR_OP_APM_GET_SPF_STATE;
    const uint8_t expected[QCOM_GPR_HEADER_BYTES] = {
        0x60, 0x18, 0, 0, 0x02, 0x03, 0, 0, 1, 0, 0, 0,
        1, 0, 0, 0, 0x34, 0x12, 0, 0, 0x21, 0x10, 0x00, 0x01,
    };
    CHECK(qcom_gpr_encode(buffer, sizeof(buffer), &packet, NULL, 0) == 24);
    CHECK(memcmp(buffer, expected, sizeof(expected)) == 0);

    const uint32_t words[3] = {7, 8, 9};
    CHECK(qcom_gpr_encode(buffer, sizeof(buffer), &packet, words, sizeof(words)) == 36);
    CHECK(get32(buffer) == (0x60u | 36u << 8));
    CHECK(qcom_gpr_decode(buffer, 36, &decoded) == QcomGprOk);
    CHECK(decoded.size == 36 && decoded.header_bytes == 24 && decoded.payload_bytes == 12);
    CHECK(get32(decoded.payload + 8) == 9);
    CHECK(decoded.dst_domain == 2 && decoded.src_domain == 3 && decoded.src_port == 1 &&
          decoded.dst_port == 1 && decoded.token == 0x1234 &&
          decoded.opcode == QCOM_GPR_OP_APM_GET_SPF_STATE);
    CHECK(qcom_gpr_decode(buffer, sizeof(buffer), &decoded) == QcomGprOk);

    CHECK(qcom_gpr_encode(NULL, sizeof(buffer), &packet, words, sizeof(words)) == 0);
    CHECK(qcom_gpr_encode(buffer, sizeof(buffer), NULL, words, sizeof(words)) == 0);
    CHECK(qcom_gpr_encode(buffer, sizeof(buffer), &packet, NULL, 4) == 0);
    CHECK(qcom_gpr_encode(buffer, 35, &packet, words, sizeof(words)) == 0);
    packet.dst_domain = 0x100;
    CHECK(qcom_gpr_encode(buffer, sizeof(buffer), &packet, NULL, 0) == 0);
    packet.dst_domain = QCOM_GPR_DOMAIN_ADSP;
    CHECK(qcom_gpr_decode(NULL, 36, &decoded) == QcomGprBadArgument);
    CHECK(qcom_gpr_decode(buffer, 36, NULL) == QcomGprBadArgument);

    CHECK(qcom_gpr_encode(buffer, sizeof(buffer), &packet, words, sizeof(words)) == 36);
    CHECK(qcom_gpr_decode(buffer, 23, &decoded) == QcomGprShort);
    CHECK(qcom_gpr_decode(buffer, 35, &decoded) == QcomGprBadSize);
    buffer[0] = 0x61;
    CHECK(qcom_gpr_decode(buffer, 36, &decoded) == QcomGprBadVersion);
    buffer[0] = 0x50;
    CHECK(qcom_gpr_decode(buffer, 36, &decoded) == QcomGprBadHeader);
    buffer[0] = 0x70;
    CHECK(qcom_gpr_decode(buffer, 36, &decoded) == QcomGprOk);
    CHECK(decoded.header_bytes == 28 && decoded.payload_bytes == 8 && get32(decoded.payload) == 8);
    buffer[0] = 0x60;
    buffer[1] = 0x10;
    CHECK(qcom_gpr_decode(buffer, 36, &decoded) == QcomGprBadHeader);

    CHECK(qcom_gpr_encode(buffer, sizeof(buffer), &packet, words, sizeof(words)) == 36);
    put32(buffer, 513u << 8 | 0x60u);
    CHECK(qcom_gpr_decode(buffer, sizeof(buffer), &decoded) == QcomGprBadSize);

    uint32_t result_opcode = 0;
    uint32_t result_status = 0;
    const uint32_t basic[2] = {QCOM_GPR_OP_APM_GET_SPF_STATE, 0x0b};
    packet = (QcomGprPacket){0};
    packet.opcode = QCOM_GPR_OP_BASIC_RESULT;
    CHECK(qcom_gpr_encode(buffer, sizeof(buffer), &packet, basic, sizeof(basic)) == 32);
    CHECK(qcom_gpr_decode(buffer, 32, &decoded) == QcomGprOk);
    CHECK(qcom_gpr_basic_result(&decoded, &result_opcode, &result_status) == 0);
    CHECK(result_opcode == QCOM_GPR_OP_APM_GET_SPF_STATE && result_status == 0x0b);
    CHECK(qcom_gpr_basic_result(NULL, &result_opcode, &result_status) == -1);

    QcomGprSpfProbe probe;
    qcom_gpr_spf_init(&probe, QCOM_GPR_PORT_APM, 0, 5);
    CHECK(qcom_gpr_spf_next(&probe, query) == QCOM_GPR_HEADER_BYTES);
    CHECK(get32(query + 20) == QCOM_GPR_OP_APM_GET_SPF_STATE && get32(query + 16) == 1);
    CHECK(probe.state == QcomSpfWaiting && qcom_gpr_spf_next(&probe, query) == 0);

    uint32_t zero = 0;
    uint32_t one = 1;
    uint32_t size = reply(answer, query, QCOM_GPR_OP_APM_SPF_STATE, &zero, 1);
    CHECK(qcom_gpr_spf_receive(&probe, answer, size) == QcomSpfNotReady);
    CHECK(probe.state == QcomSpfIdle && probe.last_status == 0);
    CHECK(qcom_gpr_spf_receive(&probe, answer, size) == QcomSpfIgnored);
    CHECK(qcom_gpr_spf_next(&probe, query) == QCOM_GPR_HEADER_BYTES && get32(query + 16) == 2);

    uint8_t stale[QCOM_GPR_MAX_PACKET];
    memcpy(stale, answer, size);
    CHECK(qcom_gpr_spf_receive(&probe, stale, size) == QcomSpfIgnored);
    size = reply(answer, query, QCOM_GPR_OP_APM_SPF_STATE, &one, 1);
    answer[12] = 9;
    CHECK(qcom_gpr_spf_receive(&probe, answer, size) == QcomSpfIgnored);
    answer[12] = 1;
    answer[0x04] = QCOM_GPR_DOMAIN_ADSP;
    CHECK(qcom_gpr_spf_receive(&probe, answer, size) == QcomSpfIgnored);
    answer[0x04] = QCOM_GPR_DOMAIN_APPS;
    answer[0x08] = 2;
    CHECK(qcom_gpr_spf_receive(&probe, answer, size) == QcomSpfIgnored);
    answer[0x08] = QCOM_GPR_PORT_APM;
    CHECK(qcom_gpr_spf_receive(&probe, answer, 10) == QcomSpfIgnored);
    CHECK(probe.ignored == 6);
    CHECK(qcom_gpr_spf_timeout(&probe) == QcomSpfIgnored);
    CHECK(probe.timeouts == 1 && probe.state == QcomSpfIdle);

    CHECK(qcom_gpr_spf_next(&probe, query) == QCOM_GPR_HEADER_BYTES && get32(query + 16) == 3);
    size = reply(answer, query, QCOM_GPR_OP_BASIC_RESULT, basic, 2);
    CHECK(qcom_gpr_spf_receive(&probe, answer, size) == QcomSpfError);
    CHECK(probe.errors == 1 && probe.last_status == 0x0b && probe.state == QcomSpfIdle);
    CHECK(qcom_gpr_spf_next(&probe, query) == QCOM_GPR_HEADER_BYTES);
    size = reply(answer, query, QCOM_GPR_OP_APM_SPF_STATE, &one, 1);
    CHECK(qcom_gpr_spf_receive(&probe, answer, size) == QcomSpfIsReady);
    CHECK(probe.state == QcomSpfReady && probe.answers == 2);
    CHECK(qcom_gpr_spf_timeout(&probe) == QcomSpfIgnored);

    qcom_gpr_spf_init(&probe, 2, 0, 2);
    CHECK(qcom_gpr_spf_next(&probe, query) == QCOM_GPR_HEADER_BYTES);
    CHECK(qcom_gpr_spf_timeout(&probe) == QcomSpfIgnored);
    CHECK(qcom_gpr_spf_next(&probe, query) == QCOM_GPR_HEADER_BYTES);
    CHECK(qcom_gpr_spf_timeout(&probe) == QcomSpfDone);
    CHECK(probe.state == QcomSpfGaveUp && qcom_gpr_spf_next(&probe, query) == 0);

    qcom_gpr_spf_init(&probe, 1, 0, 1);
    CHECK(qcom_gpr_spf_next(&probe, query) == QCOM_GPR_HEADER_BYTES);
    size = reply(answer, query, QCOM_GPR_OP_APM_SPF_STATE, &zero, 1);
    CHECK(qcom_gpr_spf_receive(&probe, answer, size) == QcomSpfDone);
    CHECK(probe.state == QcomSpfGaveUp);

    qcom_gpr_spf_init(&probe, 1, 0, 0);
    CHECK(qcom_gpr_spf_next(&probe, query) == 0);
    CHECK(qcom_gpr_spf_receive(NULL, answer, size) == QcomSpfIgnored);

    qcom_gpr_spf_init(&probe, 1, 0x10000, 1);
    CHECK(qcom_gpr_spf_next(&probe, query) == QCOM_GPR_HEADER_BYTES);
    CHECK(get32(query + 16) == 0x10001);
    size = reply(answer, query, QCOM_GPR_OP_APM_SPF_STATE, &one, 1);
    put32(answer + 16, 1);
    CHECK(qcom_gpr_spf_receive(&probe, answer, size) == QcomSpfIgnored);
    CHECK(probe.state == QcomSpfWaiting);

    printf("PASS QcomGpr: %u checks\n", checks);
    return 0;
}
