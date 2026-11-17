/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host checks for the GRAPH_OPEN/GRAPH_CLOSE reply matcher. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FuxiGraphReply.h"
#include "ShengGpr.h"
#include "QcomAudioGraph.h"

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

static uint32_t reply(uint8_t *out, uint32_t capacity, uint32_t token, uint32_t result_opcode,
                      uint32_t status, uint32_t dst_port, uint32_t src_domain, uint32_t opcode,
                      uint32_t payload_bytes)
{
    uint8_t payload[16] = {};
    payload[0] = (uint8_t)result_opcode;
    payload[1] = (uint8_t)(result_opcode >> 8);
    payload[2] = (uint8_t)(result_opcode >> 16);
    payload[3] = (uint8_t)(result_opcode >> 24);
    payload[4] = (uint8_t)status;
    payload[5] = (uint8_t)(status >> 8);
    payload[6] = (uint8_t)(status >> 16);
    payload[7] = (uint8_t)(status >> 24);
    const GprPacket p = {
        .dst_domain = GPR_DOMAIN_APPS, .src_domain = src_domain,
        .src_port = GPR_PORT_APM, .dst_port = dst_port,
        .token = token, .opcode = opcode,
    };
    return gpr_encode(out, capacity, &p, payload, payload_bytes);
}

static FuxiGraphWait open_wait(uint32_t token)
{
    FuxiGraphWait wait;
    const FuxiGraphExpect expect = {GPR_PORT_APM, token, 0x01001000u, 0ull, 0u};
    fuxi_graph_wait_begin(&wait, &expect, 1u);
    return wait;
}

static FuxiGraphWait close_wait(uint32_t token)
{
    FuxiGraphWait wait;
    const FuxiGraphExpect expect = {GPR_PORT_APM, token, 0x01001004u, 0ull, 0u};
    fuxi_graph_wait_begin(&wait, &expect, 2u);
    return wait;
}

/* A shared-memory write answers with WRITE_DONE and a 16 byte payload
 * {address low, address high, handle, status}. */
static uint32_t write_done_reply(uint8_t *out, uint32_t capacity, uint32_t token,
                                 uint64_t address, uint32_t handle, uint32_t status)
{
    uint8_t payload[16] = {};
    payload[0] = (uint8_t)address;
    payload[1] = (uint8_t)(address >> 8);
    payload[2] = (uint8_t)(address >> 16);
    payload[3] = (uint8_t)(address >> 24);
    payload[4] = (uint8_t)(address >> 32);
    payload[5] = (uint8_t)(address >> 40);
    payload[6] = (uint8_t)(address >> 48);
    payload[7] = (uint8_t)(address >> 56);
    payload[8] = (uint8_t)handle;
    payload[9] = (uint8_t)(handle >> 8);
    payload[10] = (uint8_t)(handle >> 16);
    payload[11] = (uint8_t)(handle >> 24);
    payload[12] = (uint8_t)status;
    payload[13] = (uint8_t)(status >> 8);
    payload[14] = (uint8_t)(status >> 16);
    payload[15] = (uint8_t)(status >> 24);
    const GprPacket p = {
        .dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP,
        .src_port = GPR_PORT_APM, .dst_port = GPR_PORT_APM,
        .token = token, .opcode = QCOM_AR_OP_WRITE_DONE,
    };
    return gpr_encode(out, capacity, &p, payload, sizeof payload);
}

int main(void)
{
    uint8_t packet[128];
    FuxiGraphReply answer;

    /* OPEN success: exact BASIC_RESULT {GRAPH_OPEN, 0}. */
    FuxiGraphWait wait = open_wait(0x1101u);
    uint32_t n = reply(packet, sizeof packet, 0x1101u, 0x01001000u, 0, GPR_PORT_APM,
                       GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(n > 0, "open success packet builds");
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 1, "open success matches");
    check(wait.active == 0, "open success clears wait");
    check(answer.status == 0 && answer.result_opcode == 0x01001000u, "open success fields");

    /* OPEN rejection is a match, but status 1 must stay a failure for the caller. */
    wait = open_wait(0x1102u);
    n = reply(packet, sizeof packet, 0x1102u, 0x01001000u, 1, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 1, "open rejection matches");
    check(answer.status == 1, "open rejection status preserved");
    check(wait.active == 0, "open rejection clears wait");

    /* CLOSE success, and CLOSE rejection. */
    wait = close_wait(0x1103u);
    n = reply(packet, sizeof packet, 0x1103u, 0x01001004u, 0, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 1, "close success matches");
    check(answer.result_opcode == 0x01001004u && answer.status == 0, "close success fields");

    wait = close_wait(0x1104u);
    n = reply(packet, sizeof packet, 0x1104u, 0x01001004u, 1, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 1, "close rejection matches");
    check(answer.status == 1, "close rejection status preserved");

    /* A map is usable only after the dedicated reply supplies a nonzero handle. */
    const FuxiGraphExpect map_expect = {GPR_PORT_APM, 0x1110u, QCOM_AR_OP_MEM_MAP, 0ull, 0u};
    fuxi_graph_wait_begin(&wait, &map_expect, 8u);
    n = reply(packet, sizeof packet, 0x1110u, QCOM_AR_OP_MEM_MAP, 0u, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0,
          "map acceptance waits for handle");
    check(wait.active == 1, "map acceptance keeps wait");
    n = reply(packet, sizeof packet, 0x1110u, 0u, 0u, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, QCOM_AR_OP_MEM_MAP_DONE, 4u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "zero map handle rejected");
    check(wait.active == 1, "zero map handle keeps wait");
    n = reply(packet, sizeof packet, 0x1110u, 0x1234u, 0u, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, QCOM_AR_OP_MEM_MAP_DONE, 4u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 1, "nonzero map handle matches");
    check(answer.handle == 0x1234u && answer.result_opcode == QCOM_AR_OP_MEM_MAP,
          "map handle and request opcode recorded");

    /* Wrong token, wrong result opcode, wrong destination port, wrong domain,
     * unknown reply type and malformed packets are ignored and keep waiting. */
    wait = open_wait(0x1105u);
    n = reply(packet, sizeof packet, 0x9999u, 0x01001000u, 0, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "wrong token ignored");
    check(wait.active == 1, "wrong token keeps wait");

    n = reply(packet, sizeof packet, 0x1105u, 0x01001004u, 0, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "wrong result opcode ignored");
    check(wait.active == 1, "wrong result opcode keeps wait");

    n = reply(packet, sizeof packet, 0x1105u, 0x01001000u, 0, 0x2001u,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "wrong port ignored");
    check(wait.active == 1, "wrong port keeps wait");

    n = reply(packet, sizeof packet, 0x1105u, 0x01001000u, 0, GPR_PORT_APM,
              GPR_DOMAIN_APPS, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "wrong source domain ignored");
    check(wait.active == 1, "wrong source domain keeps wait");

    n = reply(packet, sizeof packet, 0x1105u, 0x01001000u, 0, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_APM_SPF_STATE, 4u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "unknown type ignored");
    check(wait.active == 1, "unknown type keeps wait");

    n = reply(packet, sizeof packet, 0x1105u, 0x01001000u, 0, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 4u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "short result ignored");
    check(wait.active == 1, "short result keeps wait");

    check(fuxi_graph_wait_packet(&wait, packet, 3u, 1, &answer) == 0, "short packet ignored");
    check(wait.active == 1, "short packet keeps wait");

    /* Wrong channel and an asynchronous unrelated packet do not consume the wait. */
    wait = open_wait(0x1106u);
    n = reply(packet, sizeof packet, 0x1106u, 0x01001000u, 0, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 0, &answer) == 0, "wrong channel ignored");
    check(wait.active == 1, "wrong channel keeps wait");
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 1, "matching channel then matches");

    /* A late reply from an old token cannot match a new wait. */
    wait = open_wait(0x1108u);
    n = reply(packet, sizeof packet, 0x1107u, 0x01001000u, 0, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "late old token ignored");
    check(wait.active == 1, "late old token keeps wait");
    n = reply(packet, sizeof packet, 0x1108u, 0x01001000u, 0, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 1, "current token matches");

    /* Timeout clears the wait; cancel and send failure clear it too. */
    wait = open_wait(0x1109u);
    check(fuxi_graph_wait_timeout(&wait, 100u, 99u) == 0, "timeout before deadline");
    check(wait.active == 1, "timeout before deadline keeps wait");
    check(fuxi_graph_wait_timeout(&wait, 100u, 100u) == 1, "timeout at deadline");
    check(wait.active == 0, "timeout clears wait");

    wait = open_wait(0x110au);
    fuxi_graph_wait_cancel(&wait);
    check(wait.active == 0, "cancel clears wait");
    n = reply(packet, sizeof packet, 0x110au, 0x01001000u, 0, GPR_PORT_APM,
              GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
    check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "cancelled wait ignores reply");

    /* A shared-memory write answers with WRITE_DONE and names the buffer it
     * finished; status lives in the last word of the 16 byte payload. */
    {
        const uint64_t period_dsp = 0x0000000100012340ull;
        FuxiGraphExpect write_expect = {GPR_PORT_APM, 0x1120u, QCOM_AR_OP_WRITE,
                                        period_dsp, 1u};
        fuxi_graph_wait_begin(&wait, &write_expect, 8u);
        n = write_done_reply(packet, sizeof packet, 0x1120u, period_dsp, 0x00abcdefu, 0u);
        check(n > 0, "write done packet builds");
        check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 1, "write done matches");
        check(wait.active == 0, "write done clears wait");
        check(answer.result_opcode == QCOM_AR_OP_WRITE, "write done records request opcode");
        check(answer.address == period_dsp, "write done records the buffer address");
        check(answer.handle == 0x00abcdefu, "write done records the handle");
        check(answer.status == 0u, "write done status is the last payload word");

        /* A different buffer is not the answer the caller is waiting for. */
        fuxi_graph_wait_begin(&wait, &write_expect, 8u);
        n = write_done_reply(packet, sizeof packet, 0x1120u, period_dsp + 0x2000ull, 0x00abcdefu, 0u);
        check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "mismatched address ignored");
        check(wait.active == 1, "mismatched address keeps wait");

        /* A nonzero status is still a completed write; the caller inspects it. */
        fuxi_graph_wait_begin(&wait, &write_expect, 8u);
        n = write_done_reply(packet, sizeof packet, 0x1120u, period_dsp, 0x00abcdefu, 7u);
        check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 1, "write done with status matches");
        check(answer.status == 7u, "write done nonzero status preserved");

        /* A short WRITE_DONE payload is malformed and keeps waiting. */
        fuxi_graph_wait_begin(&wait, &write_expect, 8u);
        n = reply(packet, sizeof packet, 0x1120u, 0u, 0u, GPR_PORT_APM,
                  GPR_DOMAIN_ADSP, QCOM_AR_OP_WRITE_DONE, 8u);
        check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0, "short write done ignored");
        check(wait.active == 1, "short write done keeps wait");

        /* A BASIC_RESULT for the write request is not a write answer. */
        fuxi_graph_wait_begin(&wait, &write_expect, 8u);
        n = reply(packet, sizeof packet, 0x1120u, QCOM_AR_OP_WRITE, 0u, GPR_PORT_APM,
                  GPR_DOMAIN_ADSP, GPR_OP_BASIC_RESULT, 8u);
        check(fuxi_graph_wait_packet(&wait, packet, n, 1, &answer) == 0,
              "basic result is not a write answer");
        check(wait.active == 1, "basic result keeps write wait");
    }

    if (failures) {
        printf("FAIL fuxi graph reply: %d failures\n", failures);
        return 1;
    }
    printf("PASS fuxi graph reply: 45 checks\n");
    return 0;
}
