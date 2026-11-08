/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the silent AudioReach data path: the real GLINK, GPR and AudioReach layers with a simulated DSP
 * on the other end of the link. The DSP answers each command it is given, and the test turns individual answers
 * off or into failures. */
#include <stdio.h>
#include <string.h>

#include "../QcomAudioPath.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

#define FIFO_BYTES 1024u
static uint32_t h_ab, t_ab, h_ba, t_ba;
/* The channels' receive storage outlives the rig: a compound literal here would dangle as soon as rig_init returned. */
static uint8_t storage_ours[4 * 512], storage_dsp[4 * 512];
static uint8_t d_ab[FIFO_BYTES], d_ba[FIFO_BYTES];

typedef struct {
    QcomGlink link;
    uint8_t outbox[256];
    QcomGlinkChannel *channel;
    int spf_ready;        /* 1 answers ready, 0 answers "not ready", 2 never answers */
    int fail_opcode;      /* answer this opcode with a non-zero status */
    int drop_opcode;      /* never answer this opcode */
    uint32_t handle;
    uint32_t commands, pending_bytes, retries;
} Dsp;

static QcomAudioPath *g_path;   /* the path under test, for our side's receive callback */

static void path_receive(void *context, QcomGlinkChannel *channel, const uint8_t *data, uint32_t length)
{
    (void)context; (void)channel;
    (void)qcom_audio_path_receive(g_path, data, length);
}

/* The DSP echoes the request's token and swaps the ports, which is what a real answer does: the GPR readiness
 * probe ignores an answer for another token, and so does the path's transaction layer. */
static void dsp_send(Dsp *d, const QcomGprPacket *request, uint32_t opcode, const uint8_t *payload, uint32_t bytes)
{
    QcomGprPacket packet;
    memset(&packet, 0, sizeof packet);
    packet.dst_domain = QCOM_GPR_DOMAIN_APPS;
    packet.src_domain = QCOM_GPR_DOMAIN_ADSP;
    packet.src_port = request->dst_port;
    packet.dst_port = request->src_port;
    packet.token = request->token;
    packet.opcode = opcode;
    uint8_t frame[QCOM_GPR_MAX_PACKET];
    const uint32_t size = qcom_gpr_encode(frame, sizeof frame, &packet, payload, bytes);
    if (!size) return;
    const int rc = qcom_glink_send(&d->link, d->channel, frame, size);
    if (rc == QcomGlinkOk) return;
    /* A real DSP waits for an intent instead of dropping the answer: keep it and try again on the next poll. */
    if (rc == QcomGlinkAgain && size <= sizeof d->outbox) {
        memcpy(d->outbox, frame, size);
        d->pending_bytes = size;
    }
}

/* Answers one command the way the DSP would. */
static void dsp_receive(void *context, QcomGlinkChannel *channel, const uint8_t *data, uint32_t length)
{
    (void)channel;
    Dsp *d = context;
    QcomGprPacket packet;
    if (qcom_gpr_decode(data, length, &packet) != QcomGprOk) return;
    ++d->commands;
    if (d->drop_opcode && (int)packet.opcode == d->drop_opcode) return;

    if (packet.opcode == QCOM_GPR_OP_APM_GET_SPF_STATE) {
        if (d->spf_ready == 2) return;
        uint8_t state[4];
        const uint32_t ready = d->spf_ready ? 1u : 0u;
        state[0] = (uint8_t)ready; state[1] = 0; state[2] = 0; state[3] = 0;
        dsp_send(d, &packet, QCOM_GPR_OP_APM_SPF_STATE, state, sizeof state);
        return;
    }
    if (packet.opcode == QCOM_AR_OP_MEM_MAP) {
        uint8_t handle[4] = {0xef, 0xbe, 0, 0};
        handle[0] = (uint8_t)(d->handle & 0xff); handle[1] = (uint8_t)(d->handle >> 8);
        dsp_send(d, &packet, QCOM_AR_OP_MEM_MAP_DONE, handle, sizeof handle);
        return;
    }
    if (packet.opcode == QCOM_AR_OP_DATA_WRITE) {
        /* The DSP echoes the buffer address it was given: that, not the token, identifies the period. */
        /* A data-write payload starts with the address itself: there is no APM command header in front of it. */
        uint8_t done[16] = {0};
        for (unsigned i = 0; i < 8 && i < packet.payload_bytes; ++i) done[i] = packet.payload[i];
        dsp_send(d, &packet, QCOM_AR_OP_DATA_WRITE_DONE, done, sizeof done);
        return;
    }
    /* A configuration command: a basic result that echoes it, unless this one is meant to fail. */
    uint8_t result[8];
    result[0] = (uint8_t)packet.opcode; result[1] = (uint8_t)(packet.opcode >> 8);
    result[2] = (uint8_t)(packet.opcode >> 16); result[3] = (uint8_t)(packet.opcode >> 24);
    const uint32_t status = (d->fail_opcode && (int)packet.opcode == d->fail_opcode) ? 5u : 0u;
    result[4] = (uint8_t)status; result[5] = 0; result[6] = 0; result[7] = 0;
    dsp_send(d, &packet, QCOM_GPR_OP_BASIC_RESULT, result, sizeof result);
}

typedef struct {
    QcomAudioPath path;
    QcomGlink ours;
    QcomGlinkChannel *channel;
    Dsp dsp;
} Rig;

static const QcomArModule modules[] = {{0x101u, QCOM_AR_MOD_SHARED_MEMORY, 0, 1}};
static const QcomArGraph graph = {0x7001u, 2, QCOM_AR_DIR_RX, QCOM_AR_SCENARIO_PLAYBACK, 0x8001u,
                                  QCOM_AR_POSITION_STREAM, 8192, QCOM_GPR_DOMAIN_ADSP, modules, 1, 0, 0};
static const QcomArEndpoint endpoint = {0x102u, 0x102u, QCOM_AR_INTERFACE_CODEC_DMA, 0x4u, 0, 0, 0, 0x3u};
static const QcomArFormat format = {QCOM_AR_MEDIA_PCM, 16, 2, QCOM_AR_INTERLEAVED, 4};

static QcomAudioPathConfig config(void)
{
    QcomAudioPathConfig c;
    memset(&c, 0, sizeof c);
    c.gpr_port = 3u;
    c.token_seed = 0x1000u;
    c.timeout_steps = 8u;
    c.graph = &graph;
    c.endpoint = &endpoint;
    c.endpoint_format = &format;
    c.stream_format = &format;
    c.stream_instance = 0x101u;
    c.buffer_address = 0x8f000000ull;
    c.buffer_bytes = 0x4000u;
    return c;
}

static void rig_init(Rig *r)
{
    const QcomGlinkFifo o_tx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
    const QcomGlinkFifo o_rx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
    const QcomGlinkFifo p_tx = {&h_ba, &t_ba, d_ba, FIFO_BYTES};
    const QcomGlinkFifo p_rx = {&h_ab, &t_ab, d_ab, FIFO_BYTES};
    qcom_glink_init(&r->ours, o_tx, o_rx, 0, 0);
    qcom_glink_init(&r->dsp.link, p_tx, p_rx, 0, 0);
    r->dsp.spf_ready = 1; r->dsp.fail_opcode = 0; r->dsp.drop_opcode = 0; r->dsp.handle = 0x1234u;
    r->dsp.commands = 0;

    const QcomGlinkClient ours_client = {0, path_receive};
    const QcomGlinkClient dsp_client = {0, dsp_receive};
    r->channel = qcom_glink_channel(&r->ours, "adsp_apps", 512, 4, storage_ours, &ours_client, 0, 1);
    r->dsp.channel = qcom_glink_channel(&r->dsp.link, "adsp_apps", 512, 4, storage_dsp, &dsp_client, &r->dsp, 0);
    const QcomAudioPathConfig c = config();
    g_path = &r->path;
    qcom_audio_path_init(&r->path, &c, &r->ours, r->channel);
}

static void dsp_flush(Dsp *d)
{
    if (!d->pending_bytes) return;
    if (qcom_glink_send(&d->link, d->channel, d->outbox, d->pending_bytes) != QcomGlinkOk) return;
    d->pending_bytes = 0;
    ++d->retries;
}

static void pump(Rig *r, int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        dsp_flush(&r->dsp);
        (void)qcom_glink_poll(&r->dsp.link);
        (void)qcom_glink_poll(&r->ours);
    }
}

static int run_until(Rig *r, int target, int steps)
{
    for (int i = 0; i < steps; ++i) {
        pump(r, 1);
        const int rc = qcom_audio_path_step(&r->path);
            fprintf(stderr, "i=%d st=%d rc=%d cmds=%u spf=%d ans=%u sent=%u out=%d pend=%u\n", i, r->path.state, rc,
                    r->dsp.commands, r->path.spf.state, r->path.spf.answers, r->path.stage_sent, r->path.outstanding,
                    r->path.tx_pending);
        if (rc < 0) return rc;
        if (r->path.state == target) return QcomPathOk;
    }
    return QcomPathAgain;
}

static void test_happy(void)
{
    Rig *r = &(Rig){0};
    rig_init(r);
    check(run_until(r, QcomPathRunning, 400) == QcomPathOk, "the path reaches running");
    check(r->path.map_handle == 0x1234u, "the map answer's handle is kept");
    check(r->dsp.commands >= 6, "the DSP saw the readiness, open, two configurations, the map and the start");

    const uint32_t before = r->path.completed;
    check(qcom_audio_path_period(&r->path, 1024) == QcomPathOk, "a period is queued");
    check(r->path.submitted == 1, "the period is counted as submitted");
    check(qcom_audio_path_period(&r->path, 1024) == QcomPathAgain, "a second period waits for the answer");
    for (int i = 0; i < 40 && r->path.completed == before; ++i) {pump(r, 1); (void)qcom_audio_path_step(&r->path);}
    check(r->path.completed == before + 1, "the DSP's data-done answer completes the period");
    check(qcom_audio_path_period(&r->path, 0) == QcomPathBadConfig, "a zero-length period is refused");
}

static void test_failures(void)
{
    /* The framework never reports ready. */
    Rig *r = &(Rig){0};
    rig_init(r);
    r->dsp.spf_ready = 0;
    check(run_until(r, QcomPathRunning, 600) == QcomPathSpfTimeout, "a framework that never reports ready times out");

    /* A configuration answer with a non-zero status. */
    r = &(Rig){0};
    rig_init(r);
    r->dsp.fail_opcode = QCOM_AR_OP_SET_CFG;
    check(run_until(r, QcomPathRunning, 40) == QcomPathCommandFailed, "a refused configuration stops the path");

    /* A command the DSP never answers. */
    r = &(Rig){0};
    rig_init(r);
    r->dsp.drop_opcode = QCOM_AR_OP_GRAPH_OPEN;
    const int rc = run_until(r, QcomPathRunning, 400);
    check(rc == QcomPathTimeout, "an unanswered command times out");
    check(r->path.timeouts >= 1, "the timeout is counted");

    /* A reply for a transaction that is not outstanding is counted, not applied. */
    r = &(Rig){0};
    rig_init(r);
    (void)run_until(r, QcomPathRunning, 400);
    uint8_t stale[64];
    QcomGprPacket packet;
    memset(&packet, 0, sizeof packet);
    packet.dst_domain = QCOM_GPR_DOMAIN_APPS;
    packet.src_domain = QCOM_GPR_DOMAIN_ADSP;
    packet.opcode = QCOM_GPR_OP_BASIC_RESULT;
    packet.token = 0xdeadbeefu;
    const uint8_t result[8] = {0};
    const uint32_t n = qcom_gpr_encode(stale, sizeof stale, &packet, result, sizeof result);
    check(qcom_audio_path_receive(&r->path, stale, n) == QcomPathOk && r->path.late == 1,
          "a reply for no outstanding transaction is counted as late");

    /* A restart generation makes the previous generation's replies stale. */
    r = &(Rig){0};
    rig_init(r);
    (void)run_until(r, QcomPathRunning, 400);
    check(qcom_audio_path_restart(&r->path, 2) == QcomPathOk && r->path.state == QcomPathIdle,
          "a restart returns the path to the start");
    const uint32_t late_before = r->path.late;
    packet.token = (1u << 24) | 7u;   /* the previous generation */
    const uint32_t n2 = qcom_gpr_encode(stale, sizeof stale, &packet, result, sizeof result);
    (void)qcom_audio_path_receive(&r->path, stale, n2);
    check(r->path.late == late_before + 1, "a reply from the previous generation is ignored");
    check(run_until(r, QcomPathRunning, 400) == QcomPathOk, "the path comes back up after a restart");

    /* Without a graph the path refuses to start. */
    Rig bare = {0};
    rig_init(&bare);
    QcomAudioPathConfig empty = config();
    empty.graph = 0;
    g_path = &bare.path;
    qcom_audio_path_init(&bare.path, &empty, &bare.ours, bare.channel);
    check(qcom_audio_path_step(&bare.path) == QcomPathBadConfig, "a path without a graph is refused");

    /* A period before the path runs is a state error, not a silent drop. */
    check(qcom_audio_path_period(&bare.path, 1024) == QcomPathWrongState, "a period before running is refused");
}

/* Several periods in the DSP's hands at once, answered out of order. */
static void test_multiple_periods(void)
{
    Rig *r = &(Rig){0};
    QcomAudioPathConfig c = config();
    c.periods_in_flight = QCOM_AUDIOPATH_MAX_PERIODS;
    rig_init(r);
    qcom_audio_path_init(&r->path, &c, &r->ours, r->channel);
    g_path = &r->path;
    check(run_until(r, QcomPathRunning, 400) == QcomPathOk, "the path runs with several periods configured");

    int queued = 0;
    for (int i = 0; i < (int)QCOM_AUDIOPATH_MAX_PERIODS; ++i)
        if (qcom_audio_path_period(&r->path, 1024) == QcomPathOk) ++queued;
    check(queued == (int)QCOM_AUDIOPATH_MAX_PERIODS, "as many periods as the window allows are queued");
    check(qcom_audio_path_in_flight(&r->path) == QCOM_AUDIOPATH_MAX_PERIODS, "all of them are in flight");
    check(qcom_audio_path_period(&r->path, 1024) == QcomPathAgain, "one more is refused while the window is full");
    check(r->path.submitted == QCOM_AUDIOPATH_MAX_PERIODS, "each queued period is counted as submitted");

    /* Pump until every period is answered; the DSP answers in whatever order the link delivers. */
    const uint32_t before = r->path.completed;
    for (int i = 0; i < 200 && r->path.completed < before + QCOM_AUDIOPATH_MAX_PERIODS; ++i) {
        pump(r, 1);
        (void)qcom_audio_path_step(&r->path);
    }
    check(r->path.completed == before + QCOM_AUDIOPATH_MAX_PERIODS, "every period is completed");
    check(qcom_audio_path_in_flight(&r->path) == 0, "the window empties as the answers arrive");

    /* An answer for a buffer that is not outstanding is late, not a completion. */
    uint8_t stale[64];
    QcomGprPacket packet;
    memset(&packet, 0, sizeof packet);
    packet.dst_domain = QCOM_GPR_DOMAIN_APPS;
    packet.src_domain = QCOM_GPR_DOMAIN_ADSP;
    packet.opcode = QCOM_AR_OP_DATA_WRITE_DONE;
    packet.token = 1u;
    uint8_t payload[16] = {0};
    payload[0] = 0x77;   /* an address no period used */
    const uint32_t n = qcom_gpr_encode(stale, sizeof stale, &packet, payload, sizeof payload);
    const uint32_t late_before = r->path.late;
    (void)qcom_audio_path_receive(&r->path, stale, n);
    check(r->path.late == late_before + 1 && r->path.completed == before + QCOM_AUDIOPATH_MAX_PERIODS,
          "an answer for nothing outstanding is late");

    /* A restart counts what was still in flight as cancelled. */
    (void)qcom_audio_path_period(&r->path, 1024);
    (void)qcom_audio_path_period(&r->path, 1024);
    check(qcom_audio_path_in_flight(&r->path) == 2, "two periods are in flight before the restart");
    const uint32_t cancelled_before = r->path.cancelled;
    (void)qcom_audio_path_restart(&r->path, 3);
    check(r->path.cancelled == cancelled_before + 2 && qcom_audio_path_in_flight(&r->path) == 0,
          "a restart cancels the periods that were in flight");
}

int main(void)
{
    test_happy();
    test_multiple_periods();
    test_failures();
    printf("PASS QcomAudioPath: %d checks\n", checks);
    return failed ? 1 : 0;
}
