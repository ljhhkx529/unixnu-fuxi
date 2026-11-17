/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FuxiGraphReply.h"

#include "ShengGpr.h"
#include "QcomAudioGraph.h"

static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void fuxi_graph_wait_begin(FuxiGraphWait *wait, const FuxiGraphExpect *expect, uint8_t kind)
{
    if (!wait) return;
    for (uint32_t i = 0; i < sizeof *wait; ++i) ((uint8_t *)wait)[i] = 0;
    if (!expect) return;
    wait->active = 1;
    wait->kind = kind;
    wait->expect = *expect;
}

void fuxi_graph_wait_cancel(FuxiGraphWait *wait)
{
    if (wait) wait->active = 0;
}

int fuxi_graph_wait_timeout(FuxiGraphWait *wait, uint64_t deadline_ms, uint64_t now_ms)
{
    if (!wait || !wait->active) return 0;
    if (now_ms < deadline_ms) return 0;
    wait->active = 0;
    return 1;
}

int fuxi_graph_wait_packet(FuxiGraphWait *wait, const uint8_t *data, uint32_t length,
                           int channel_match, FuxiGraphReply *reply)
{
    if (!wait || !wait->active || !channel_match || !data) return 0;

    GprPacket packet;
    if (gpr_decode(data, length, &packet) != GprOk) return 0;
    if (packet.dst_domain != GPR_DOMAIN_APPS || packet.src_domain != GPR_DOMAIN_ADSP) return 0;
    if (packet.dst_port != wait->expect.source_port) return 0;
    if (packet.token != wait->expect.token) return 0;
    if (wait->expect.request_opcode == QCOM_AR_OP_MEM_MAP &&
        packet.opcode == QCOM_AR_OP_MEM_MAP_DONE) {
        if (packet.payload_bytes < 4) return 0;
        const uint32_t handle = word(packet.payload);
        if (!handle) return 0;
        if (reply) {
            reply->opcode = packet.opcode;
            reply->result_opcode = QCOM_AR_OP_MEM_MAP;
            reply->status = 0;
            reply->handle = handle;
            reply->token = packet.token;
            reply->src_port = packet.src_port;
            reply->dst_port = packet.dst_port;
            reply->length = length;
        }
        wait->active = 0;
        return 1;
    }
    /* A shared-memory write answers with WRITE_DONE, not BASIC_RESULT; its
     * payload is {address low, address high, handle, status}. */
    if (wait->expect.request_opcode == QCOM_AR_OP_WRITE &&
        packet.opcode == QCOM_AR_OP_WRITE_DONE) {
        if (packet.payload_bytes < 16) return 0;
        const uint64_t address = word(packet.payload) | ((uint64_t)word(packet.payload + 4) << 32);
        /* The answer names the buffer it finished.  When the caller says
         * which buffer it is waiting for, a different address is not the
         * answer it wants. */
        if (wait->expect.match_address && address != wait->expect.address) return 0;
        if (reply) {
            reply->opcode = packet.opcode;
            reply->result_opcode = QCOM_AR_OP_WRITE;
            reply->status = word(packet.payload + 12);
            reply->handle = word(packet.payload + 8);
            reply->address = address;
            reply->token = packet.token;
            reply->src_port = packet.src_port;
            reply->dst_port = packet.dst_port;
            reply->length = length;
        }
        wait->active = 0;
        return 1;
    }
    /* A successful BASIC_RESULT can precede MEM_MAP_DONE.  It contains no
     * usable handle, so continue waiting.  A rejected map is terminal. */
    if (wait->expect.request_opcode == QCOM_AR_OP_MEM_MAP &&
        packet.opcode == GPR_OP_BASIC_RESULT && packet.payload_bytes >= 8 &&
        word(packet.payload) == QCOM_AR_OP_MEM_MAP &&
        word(packet.payload + 4) == 0)
        return 0;
    if (packet.opcode != GPR_OP_BASIC_RESULT) return 0;
    /* A write only completes through WRITE_DONE; a BASIC_RESULT that merely
     * echoes the write opcode does not name a buffer and is not the answer. */
    if (wait->expect.request_opcode == QCOM_AR_OP_WRITE) return 0;
    if (packet.payload_bytes < 8) return 0;

    uint32_t result_opcode = 0;
    uint32_t status = 0;
    if (gpr_basic_result(&packet, &result_opcode, &status) != 0) return 0;
    if (result_opcode != wait->expect.request_opcode) return 0;

    if (reply) {
        reply->opcode = packet.opcode;
        reply->result_opcode = result_opcode;
        reply->status = status;
        reply->token = packet.token;
        reply->src_port = packet.src_port;
        reply->dst_port = packet.dst_port;
        reply->length = length;
    }
    wait->active = 0;
    return 1;
}
