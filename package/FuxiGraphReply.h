/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Matching one bounded APM command reply.
 *
 * This is deliberately separate from the kernel glue so the receive path can
 * be host-tested with fixed packets.  A reply matches only when the channel,
 * GPR decode, source/destination domains, reply destination port, token,
 * GPR opcode and BASIC_RESULT request opcode all agree.  Anything else is
 * ignored and keeps the wait active; a timeout/cancel is the only other way
 * to leave the waiting state. */
#ifndef FUXI_GRAPH_REPLY_H
#define FUXI_GRAPH_REPLY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t source_port;      /* the local GPR port the reply must be addressed to */
    uint32_t token;            /* the request token */
    uint32_t request_opcode;   /* GRAPH_OPEN, SET_CFG, GRAPH_CLOSE, etc. */
    uint64_t address;          /* the buffer a write answer names, when match_address is set */
    uint8_t match_address;     /* whether the address has to agree */
} FuxiGraphExpect;

typedef struct {
    uint32_t opcode;
    uint32_t result_opcode;
    uint32_t status;
    uint32_t token;
    uint32_t src_port;
    uint32_t dst_port;
    uint32_t length;
    uint32_t handle;            /* nonzero only for MEM_MAP_DONE */
    uint64_t address;           /* the buffer a write answer names */
} FuxiGraphReply;

typedef struct {
    uint8_t active;
    uint8_t kind;              /* owner-defined transaction kind, for diagnostics */
    FuxiGraphExpect expect;
} FuxiGraphWait;

/* Starts one wait.  kind is copied for diagnostics only. */
void fuxi_graph_wait_begin(FuxiGraphWait *wait, const FuxiGraphExpect *expect, uint8_t kind);

/* Clears the wait after timeout, send failure, channel down, or owner stop. */
void fuxi_graph_wait_cancel(FuxiGraphWait *wait);

/* Returns 1 when the wait expired and was cleared, 0 when it is still active
 * or already complete. */
int fuxi_graph_wait_timeout(FuxiGraphWait *wait, uint64_t deadline_ms, uint64_t now_ms);

/* Feeds one received packet.  Returns 1 only for an exact match, which also
 * clears active.  Returns 0 for irrelevant, malformed, wrong-channel, unknown
 * or late packets; in those cases active is unchanged. */
int fuxi_graph_wait_packet(FuxiGraphWait *wait, const uint8_t *data, uint32_t length,
                           int channel_match, FuxiGraphReply *reply);

#ifdef __cplusplus
}
#endif

#endif
