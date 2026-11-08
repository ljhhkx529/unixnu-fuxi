/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the DSP client layer: channels registered by name, sends queued from anywhere and flushed from the
 * DSP thread, and callbacks that see what the other side actually sent. */
#include <stdio.h>
#include <string.h>

#include "../QcomAdspClient.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

#define FIFO_BYTES 1024u
static uint32_t h_ab, t_ab, h_ba, t_ba;
static uint8_t d_ab[FIFO_BYTES], d_ba[FIFO_BYTES];
static uint8_t storage_peer[4 * 512];

typedef struct {
    int up, ups, downs;
    uint32_t messages, bytes;
    uint8_t last[QCOM_ADSP_MAX_MESSAGE];
    uint32_t last_length;
} Sink;

static void sink_state(void *context, int up)
{
    Sink *s = context;
    s->up = up;
    if (up) ++s->ups; else ++s->downs;
}

static void sink_receive(void *context, const uint8_t *data, uint32_t length)
{
    Sink *s = context;
    ++s->messages;
    s->bytes += length;
    s->last_length = length;
    if (length <= sizeof s->last) memcpy(s->last, data, length);
}

static void sink_reset(Sink *s) {memset(s, 0, sizeof *s);}

static QcomGlink ours, peer;
static QcomGlinkChannel *peer_channel;

static void rig(void)
{
    const QcomGlinkFifo o_tx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
    const QcomGlinkFifo o_rx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
    const QcomGlinkFifo p_tx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
    const QcomGlinkFifo p_rx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
    qcom_glink_init(&ours, o_tx, o_rx, 0, 0);
    qcom_glink_init(&peer, p_tx, p_rx, 0, 0);
    static Sink peer_sink;
    sink_reset(&peer_sink);
    const QcomGlinkClient peer_client = {0, 0};
    (void)peer_client;
    peer_channel = qcom_glink_channel(&peer, "adsp_apps", 512, 4, storage_peer,
                                      &(QcomGlinkClient){0, 0}, 0, 0);
}

static void pump_both(int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        (void)qcom_glink_poll(&peer);
        (void)qcom_adsp_pump();
    }
}

int main(void)
{
    /* Before a link is bound there is nothing to register on. */
    Sink sink;
    sink_reset(&sink);
    const QcomAdspClient client = {sink_state, sink_receive};
    check(qcom_adsp_channel("adsp_apps", 512, 4, 1, &client, &sink) == 0, "a channel without a bound link is refused");

    rig();
    check(qcom_adsp_bind(&ours) == 0, "the link is bound");
    QcomAdspChannel *channel = qcom_adsp_channel("adsp_apps", 512, 4, 1, &client, &sink);
    check(channel != 0, "a channel registers by name");
    check(qcom_adsp_find("adsp_apps") == channel, "and is found again by its name");
    check(qcom_adsp_find("nothing") == 0, "an unregistered name is not found");

    /* Sends are queued before the DSP is up, and nothing goes out. */
    const uint8_t payload[6] = {1, 2, 3, 4, 5, 6};
    check(qcom_adsp_send(channel, payload, sizeof payload) == 0, "a send from any thread is queued");
    check(qcom_adsp_queued(channel) == 1 && qcom_adsp_sent(channel) == 0, "it waits for the DSP thread");
    check(qcom_adsp_pump() >= 0, "the pump runs while the channel is down");
    check(qcom_adsp_queued(channel) == 1, "and the message is still queued");

    /* Once both sides are up the queue drains. */
    pump_both(40);
    check(sink.ups == 1 && sink.up == 1, "the client is told the channel came up");
    check(qcom_adsp_queued(channel) == 0 && qcom_adsp_sent(channel) == 1, "the message went out");
    check(sink.messages == 0, "the peer has not been read yet");

    /* A full queue is refused rather than grown or dropped. */
    uint8_t big[QCOM_ADSP_MAX_MESSAGE];
    memset(big, 0x5a, sizeof big);
    for (unsigned i = 0; i < QCOM_ADSP_QUEUE; ++i)
        check(qcom_adsp_send(channel, big, sizeof big) == 0, "the queue takes what it has room for");
    check(qcom_adsp_send(channel, big, 4) == -1, "a full queue refuses the next message");
    check(qcom_adsp_send(channel, big, QCOM_ADSP_MAX_MESSAGE + 1u) == -1, "an oversized message is refused");
    check(qcom_adsp_send(channel, 0, 4) == -2, "a null message is refused");
    check(qcom_adsp_send(channel, big, 0) == -1, "an empty message is refused");

    /* A second channel takes its own slot and its own queue. */
    Sink second;
    sink_reset(&second);
    const QcomAdspClient second_client = {sink_state, sink_receive};
    QcomAdspChannel *other = qcom_adsp_channel("audio", 512, 4, 1, &second_client, &second);
    check(other != 0 && other != channel, "a second channel registers separately");
    check(qcom_adsp_queued(other) == 0, "with its own empty queue");

    /* Past the slot count there is no room. */
    (void)qcom_adsp_channel("c1", 512, 4, 1, &client, &sink);
    (void)qcom_adsp_channel("c2", 512, 4, 1, &client, &sink);
    check(qcom_adsp_channel("c3", 512, 4, 1, &client, &sink) == 0, "a fifth channel is refused");

    /* Bad registration arguments. */
    check(qcom_adsp_channel(0, 512, 4, 1, &client, &sink) == 0, "a channel without a name is refused");
    check(qcom_adsp_channel("x", 0, 4, 1, &client, &sink) == 0, "a channel without intents is refused");
    check(qcom_adsp_channel("this-name-is-far-too-long-for-glink", 512, 4, 1, &client, &sink) == 0,
          "a name that does not fit GLINK's is refused");

    printf("PASS QcomAdspClient: %d checks\n", checks);
    return failed ? 1 : 0;
}
