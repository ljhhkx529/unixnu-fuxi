/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The host check of the GLINK layer's handling of a channel the remote opens and nobody here serves.
 *
 * The modem opens SSM_RTR_MODEM_APPS, the edge it uses to ask for a subsystem restart, and it is not a
 * channel this port serves. On 2026-11-20 the port counted that open as unknown and answered nothing,
 * and the modem said nothing more: it waited for the acknowledgement and gave up on the edge.
 *
 * Upstream answers it. Linux drivers/rpmsg/qcom_glink_native.c allocates a channel for the name in
 * qcom_glink_rx_open and sends the OPEN_ACK, and qcom_glink_create_remote then sends its own OPEN and
 * waits for the acknowledgement in turn.
 *
 * So this drives the real package/QcomGlink.c over a pair of in-memory FIFOs and checks:
 *
 *   an OPEN for a name no channel serves is answered with an OPEN_ACK
 *   the OPEN_ACK carries the remote's own channel id
 *   the port sends its own OPEN afterwards, and the channel comes up
 *   the channel carries no intents, so nothing is advertised for it
 *   an OPEN for a name a channel does serve still behaves as before
 */
#include <stdio.h>
#include <string.h>

#include "QcomGlink.h"

static int failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

#define FIFO_BYTES 4096

/* The two FIFOs, as the real one has them: the indices live in the SMEM, the data in memory. */
static uint8_t tx_data[FIFO_BYTES];
static uint8_t rx_data[FIFO_BYTES];
static uint32_t tx_head, tx_tail, rx_head, rx_tail;

/* What the port wrote into the transmit FIFO, read back as GLINK messages. */
typedef struct {
    uint16_t cmd, param1;
    uint32_t param2;
    char name[QCOM_GLINK_NAME];
} seen_message;

static unsigned seen_count;
static seen_message seen[32];

static void record(const uint8_t *bytes, uint32_t count)
{
    uint32_t at = 0;
    while (at + 8 <= count) {
        seen_message *m = &seen[seen_count < 32 ? seen_count++ : 31];
        m->cmd = (uint16_t)(bytes[at] | bytes[at + 1] << 8);
        m->param1 = (uint16_t)(bytes[at + 2] | bytes[at + 3] << 8);
        m->param2 = (uint32_t)bytes[at + 4] | (uint32_t)bytes[at + 5] << 8 | (uint32_t)bytes[at + 6] << 16 |
            (uint32_t)bytes[at + 7] << 24;
        m->name[0] = 0;
        if (m->cmd == QCOM_GLINK_OPEN) {
            const uint32_t n = m->param2 & 0xffff;
            for (uint32_t i = 0; i < n && i + 8 < count && i < QCOM_GLINK_NAME - 1; ++i) m->name[i] = bytes[at + 8 + i];
            at += 8 + n;
        } else {
            at += 8;
        }
        at = (at + 7u) & ~7u;
    }
}

/* The port kicked us: read what it wrote into the transmit FIFO. */
static void on_kick(void *context)
{
    (void)context;
    const uint32_t head = tx_head, tail = tx_tail;
    if (head >= tail) {
        record(tx_data + tail, head - tail);
    } else {
        record(tx_data + tail, FIFO_BYTES - tail);
        record(tx_data, head);
    }
    tx_tail = head;
}

/* What the port saw on the channel, and the channel's state. */
static unsigned receive_calls;
static unsigned state_calls;
static int last_up;
static QcomGlinkChannel *the_channel;

static void on_state(void *context, QcomGlinkChannel *channel, int up)
{
    (void)context;
    state_calls++;
    last_up = up;
    the_channel = channel;
}

/* The channel with a name, whether it came up or not: the port's own table. */
static QcomGlinkChannel *find_channel(QcomGlink *g, const char *name)
{
    for (unsigned i = 0; i < QCOM_GLINK_MAX_CHANNELS; ++i)
        if (g->channel[i].used && strcmp(g->channel[i].name, name) == 0) return &g->channel[i];
    return 0;
}

static void on_receive(void *context, QcomGlinkChannel *channel, const uint8_t *data, uint32_t length)
{
    (void)context;
    (void)channel;
    (void)data;
    (void)length;
    receive_calls++;
}

/* Writes a message into the receive FIFO, as the remote would. */
static void remote_write(const uint8_t *bytes, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        rx_data[rx_head] = bytes[i];
        if (++rx_head == FIFO_BYTES) rx_head = 0;
    }
}

static void remote_open(const char *name, uint16_t rcid)
{
    uint8_t m[8 + QCOM_GLINK_NAME];
    const uint32_t n = (uint32_t)strlen(name) + 1;
    memset(m, 0, sizeof m);
    m[0] = QCOM_GLINK_OPEN;
    m[2] = (uint8_t)rcid;
    m[3] = (uint8_t)(rcid >> 8);
    m[4] = (uint8_t)n;
    memcpy(m + 8, name, n);
    /* The message is padded to eight bytes, and the padding is part of it: the port advances its tail by the
     * padded length, so the remote has to write it. */
    remote_write(m, (8 + n + 7u) & ~7u);
}

static void remote_version(void)
{
    uint8_t m[8];
    memset(m, 0, sizeof m);
    m[0] = QCOM_GLINK_VERSION;
    m[2] = 1; /* the version */
    m[4] = 1; /* the features */
    remote_write(m, sizeof m);
}

static void setup(QcomGlink *g)
{
    memset(g, 0, sizeof *g);
    tx_head = tx_tail = rx_head = rx_tail = 0;
    seen_count = 0;
    receive_calls = 0;
    state_calls = 0;
    last_up = 0;
    the_channel = 0;
    const QcomGlinkFifo tx = {&tx_head, &tx_tail, tx_data, FIFO_BYTES};
    const QcomGlinkFifo rx = {&rx_head, &rx_tail, rx_data, FIFO_BYTES};
    qcom_glink_init(g, tx, rx, on_kick, 0);
}

/* Finds the message of a kind in what the port wrote. */
static const seen_message *find(uint16_t cmd, uint16_t param1)
{
    for (unsigned i = 0; i < seen_count; ++i)
        if (seen[i].cmd == cmd && (param1 == 0xffff || seen[i].param1 == param1)) return &seen[i];
    return 0;
}

static void test_unknown_open_is_answered(void)
{
    QcomGlink g;
    setup(&g);

    /* The remote speaks first: the version handshake, then it opens a channel nobody here serves. */
    remote_version();
    qcom_glink_poll(&g);
    remote_open("SSM_RTR_MODEM_APPS", 7);

    const int handled = qcom_glink_poll(&g);
    CHECK(handled > 0, "the open was handled at all");
    CHECK(g.unknown_opens == 1, "the open was counted as one nobody serves, not %u", g.unknown_opens);

    /* The port must answer it: the OPEN_ACK carries the remote's own channel id. */
    const seen_message *ack = find(QCOM_GLINK_OPEN_ACK, 7);
    CHECK(ack != 0, "the open was acknowledged");
    if (ack) CHECK(ack->param1 == 7, "the acknowledgement carries the remote's channel id 7, not %u", ack->param1);

    /* And the port sends its own OPEN, as upstream's qcom_glink_create_remote does. */
    const seen_message *open = find(QCOM_GLINK_OPEN, 0xffff);
    CHECK(open != 0, "the port sent its own open");
    if (open) {
        CHECK(strcmp(open->name, "SSM_RTR_MODEM_APPS") == 0, "the port's open names the same channel, not %s",
              open->name);
    }

    /* The remote acknowledges the port's open, and the channel comes up. */
    uint8_t ackback[8];
    memset(ackback, 0, sizeof ackback);
    ackback[0] = QCOM_GLINK_OPEN_ACK;
    ackback[2] = (uint8_t)(the_channel ? the_channel->lcid : 1);
    remote_write(ackback, sizeof ackback);
    qcom_glink_poll(&g);

    /* The channel nobody serves has no client, so no state is reported for it: there is nobody to tell.
     * What has to be true is that the channel exists and is up. */
    the_channel = find_channel(&g, "SSM_RTR_MODEM_APPS");
    CHECK(the_channel != 0, "the channel exists");
    if (the_channel) {
        CHECK(the_channel->up == 1, "the channel came up, not %u", the_channel->up);
        CHECK(the_channel->remote_open == 1, "the remote's open was recorded");
        CHECK(the_channel->open_acked == 1, "the port's own open was acknowledged");
        /* The channel nobody serves carries no intents, so nothing is advertised for it. */
        CHECK(the_channel->intent_count == 0, "the channel has no intents, not %u", the_channel->intent_count);
        CHECK(the_channel->local[0].advertised == 0, "nothing was advertised on the channel");
    }
    CHECK(state_calls == 0, "no state was reported for a channel nobody serves, but %u came", state_calls);
    /* And no data is ever delivered on it: nobody here serves it. */
    CHECK(receive_calls == 0, "no data was delivered, but %u calls came", receive_calls);
}

static void test_known_open_still_works(void)
{
    QcomGlink g;
    setup(&g);

    static uint8_t storage[1024];
    const QcomGlinkClient client = {on_state, on_receive};
    QcomGlinkChannel *c = qcom_glink_channel(&g, "IPCRTR", 1024, 1, storage, &client, 0, 0);
    CHECK(c != 0, "the channel was made");
    if (!c) return;

    remote_version();
    qcom_glink_poll(&g);
    remote_open("IPCRTR", 9);
    qcom_glink_poll(&g);

    const seen_message *ack = find(QCOM_GLINK_OPEN_ACK, 9);
    CHECK(ack != 0, "the known channel's open was acknowledged");
    if (ack) CHECK(ack->param1 == 9, "the acknowledgement carries the remote's channel id 9, not %u", ack->param1);
    CHECK(g.unknown_opens == 0, "a known channel is not counted as unknown");

    /* The channel comes up and advertises its intent, as before. */
    uint8_t ackback[8];
    memset(ackback, 0, sizeof ackback);
    ackback[0] = QCOM_GLINK_OPEN_ACK;
    ackback[2] = (uint8_t)c->lcid;
    remote_write(ackback, sizeof ackback);
    qcom_glink_poll(&g);
    CHECK(c->up == 1, "the known channel came up, not %u", c->up);
    CHECK(c->local[0].advertised == 1, "the known channel advertised its intent");
}

static void test_unknown_open_keeps_the_others(void)
{
    QcomGlink g;
    setup(&g);

    static uint8_t storage[1024];
    const QcomGlinkClient client = {on_state, on_receive};
    QcomGlinkChannel *c = qcom_glink_channel(&g, "IPCRTR", 1024, 1, storage, &client, 0, 0);
    CHECK(c != 0, "the channel was made");
    if (!c) return;

    remote_version();
    qcom_glink_poll(&g);
    remote_open("SSM_RTR_MODEM_APPS", 7);
    qcom_glink_poll(&g);
    remote_open("IPCRTR", 9);
    qcom_glink_poll(&g);

    CHECK(g.unknown_opens == 1, "only the unknown one was counted, not %u", g.unknown_opens);
    const seen_message *ack = find(QCOM_GLINK_OPEN_ACK, 9);
    CHECK(ack != 0, "the known channel's open was still acknowledged");
}

int main(void)
{
    test_unknown_open_is_answered();
    test_known_open_still_works();
    test_unknown_open_keeps_the_others();

    if (failures) {
        printf("FAIL fuxi_glink_open_host: %d checks failed\n", failures);
        return 1;
    }
    printf("PASS fuxi_glink_open_host\n");
    return 0;
}
