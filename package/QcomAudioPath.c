/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAudioPath.h"

#define DEFAULT_TIMEOUT_STEPS 24u
#define DEFAULT_SPF_QUERIES 24u

static int fail(QcomAudioPath *p, int reason)
{
    p->last_error = reason;
    p->state = QcomPathFailed;
    return reason;
}

void qcom_audio_path_init(QcomAudioPath *p, const QcomAudioPathConfig *config, QcomGlink *link,
                          QcomGlinkChannel *channel)
{
    for (uint32_t i = 0; i < sizeof *p; ++i) ((uint8_t *)p)[i] = 0;
    if (config) p->config = *config;
    p->link = link;
    p->channel = channel;
    p->state = QcomPathIdle;
    p->last_error = QcomPathOk;
    p->generation = 1;
    p->periods_in_flight = p->config.periods_in_flight;
    if (!p->periods_in_flight) p->periods_in_flight = 1;
    if (p->periods_in_flight > QCOM_AUDIOPATH_MAX_PERIODS) p->periods_in_flight = QCOM_AUDIOPATH_MAX_PERIODS;
}

/* The stage that follows a finished one. */
static int next_stage(int state)
{
    switch (state) {
    case QcomPathOpen: return QcomPathEndpoint;
    case QcomPathEndpoint: return QcomPathStream;
    case QcomPathStream: return QcomPathMap;
    case QcomPathMap: return QcomPathStart;
    case QcomPathStart: return QcomPathRunning;
    default: return state;
    }
}

/* The generation sits above the counter, so a reply from before a restart can never match. The full value is
 * what the reply is compared against, so p->token holds the token itself and p->counter counts the commands. */
static uint32_t next_token(QcomAudioPath *p)
{
    p->token = (p->generation << 24) | (++p->counter & 0x00ffffffu);
    return p->token;
}

/* Hands the frame already sitting in p->tx to GLINK. A send that cannot go out yet (no remote intent, no room)
 * leaves the frame in place, so the next step retries the same bytes instead of a new token: the command is
 * never lost between encoding and sending. */
static int try_send(QcomAudioPath *p)
{
    const int rc = qcom_glink_send(p->link, p->channel, p->tx, p->tx_bytes);
    if (rc == QcomGlinkOk) {
        p->tx_pending = 0;
        p->outstanding = 1;
        p->stage_sent = 1;
        p->waits = 0;
        return QcomPathOk;
    }
    if (rc == QcomGlinkAgain) return QcomPathAgain;
    if (rc == QcomGlinkNotOpen) return fail(p, QcomPathWrongState);
    return fail(p, QcomPathBadConfig);
}

/* Frames one APM payload with QcomGpr into p->tx and tries to send it. The payload always lives in another buffer
 * (p->apm or the caller's), never in p->tx: encoding a packet over its own payload would overwrite it. */
static int submit(QcomAudioPath *p, uint32_t opcode, const uint8_t *payload, uint32_t bytes)
{
    QcomGprPacket packet;
    for (uint32_t i = 0; i < sizeof packet; ++i) ((uint8_t *)&packet)[i] = 0;
    packet.dst_domain = QCOM_GPR_DOMAIN_ADSP;
    packet.src_domain = QCOM_GPR_DOMAIN_APPS;
    packet.src_port = p->config.gpr_port;
    packet.dst_port = QCOM_GPR_PORT_APM;
    packet.token = next_token(p);
    packet.opcode = opcode;
    const uint32_t size = qcom_gpr_encode(p->tx, sizeof p->tx, &packet, payload, bytes);
    if (!size) return fail(p, QcomPathBadConfig);
    p->tx_bytes = size;
    p->tx_pending = 1;
    p->outstanding_opcode = opcode;
    p->stage_sent = 0;
    return try_send(p);
}

int qcom_audio_path_receive(QcomAudioPath *p, const uint8_t *data, uint32_t length)
{
    if (!p || !data) return QcomPathBadConfig;
    /* The framework's readiness answers belong to the probe, which tracks its own token sequence. */
    if (p->state == QcomPathSpf) {
        (void)qcom_gpr_spf_receive(&p->spf, data, length);
        return QcomPathOk;
    }
    QcomGprPacket packet;
    if (qcom_gpr_decode(data, length, &packet) != QcomGprOk) {++p->unknown; return QcomPathOk;}
    if (packet.dst_domain != QCOM_GPR_DOMAIN_APPS) {++p->unknown; return QcomPathOk;}
    QcomArAnswer answer;
    if (qcom_ar_parse(&packet, &answer) == QcomArAnswerBad) {++p->unknown; return QcomPathOk;}
    /* A data-done answer belongs to whichever period carries that address; the commands keep a single
     * outstanding transaction, so a data answer is matched before the token gate. */
    if (packet.opcode == QCOM_AR_OP_DATA_WRITE_DONE) {
        QcomArAnswer data;
        if (qcom_ar_parse(&packet, &data) != QcomArAnswerDataDone) {++p->unknown; return QcomPathOk;}
        for (uint32_t i = 0; i < QCOM_AUDIOPATH_MAX_PERIODS; ++i) {
            if (!p->period[i].active || p->period[i].address != data.address) continue;
            p->period[i].active = 0;
            ++p->completed;
            return QcomPathOk;
        }
        ++p->late;
        return QcomPathOk;
    }
    /* One command transaction is outstanding: a reply for anything else belongs to an older one. */
    if (!p->outstanding || packet.token != p->token) {++p->late; return QcomPathOk;}

    switch (qcom_ar_parse(&packet, &answer)) {
    case QcomArAnswerBasicResult:
        if (answer.status) return fail(p, QcomPathCommandFailed);
        if (answer.result_opcode != p->outstanding_opcode) {++p->late; return QcomPathOk;}
        p->outstanding = 0;
        return QcomPathOk;
    case QcomArAnswerMapped:
        p->map_handle = answer.handle;
        p->outstanding = 0;
        return QcomPathOk;
    default:
        ++p->unknown;
        return QcomPathOk;
    }
}

int qcom_audio_path_step(QcomAudioPath *p)
{
    if (!p) return QcomPathBadConfig;
    if (p->state == QcomPathFailed) return p->last_error;
    if (p->state == QcomPathRunning) return QcomPathOk;
    ++p->steps;
    const uint32_t budget = p->config.timeout_steps ? p->config.timeout_steps : DEFAULT_TIMEOUT_STEPS;

    switch (p->state) {
    case QcomPathIdle:
        if (!p->config.graph || !p->config.endpoint || !p->config.endpoint_format ||
            !p->config.stream_format || !p->config.buffer_bytes || !p->link || !p->channel)
            return fail(p, QcomPathBadConfig);
        qcom_gpr_spf_init(&p->spf, p->config.gpr_port, p->config.token_seed + p->generation,
                          DEFAULT_SPF_QUERIES);
        p->state = QcomPathSpf;
        return QcomPathOk;
    case QcomPathSpf: {
        if (qcom_glink_poll(p->link) == QcomGlinkCorrupt) return fail(p, QcomPathSpfError);
        const int spf = p->spf.state;
        if (spf == QcomSpfReady) {p->state = QcomPathOpen; return QcomPathOk;}
        if (spf == QcomSpfGaveUp) return fail(p, QcomPathSpfTimeout);
        /* The probe consumes a query when it hands one out, so the frame is kept until GLINK takes it. */
        if (!p->tx_pending) {
            uint8_t query[QCOM_GPR_HEADER_BYTES];
            const uint32_t n = qcom_gpr_spf_next(&p->spf, query);
            if (n) {
                for (uint32_t i = 0; i < n; ++i) p->tx[i] = query[i];
                p->tx_bytes = n;
                p->tx_pending = 1;
            }
        }
        if (p->tx_pending) {
            const int sent = qcom_glink_send(p->link, p->channel, p->tx, p->tx_bytes);
            if (sent == QcomGlinkOk) {p->tx_pending = 0; p->waits = 0; return QcomPathOk;}
            if (sent != QcomGlinkAgain) return fail(p, QcomPathSpfError);
        }
        if (++p->waits > budget * 4u) return fail(p, QcomPathSpfTimeout);
        return QcomPathAgain;
    }
    case QcomPathOpen:
    case QcomPathEndpoint:
    case QcomPathStream:
    case QcomPathMap:
    case QcomPathStart: {
        if (qcom_glink_poll(p->link) == QcomGlinkCorrupt) return fail(p, QcomPathCommandFailed);
        if (p->outstanding) {
            if (++p->waits > budget) {++p->timeouts; return fail(p, QcomPathTimeout);}
            return QcomPathAgain;
        }
        if (p->tx_pending) return try_send(p);
        if (p->stage_sent) {
            /* The reply arrived: on to the next stage. */
            p->stage_sent = 0;
            p->waits = 0;
            const int next = next_stage(p->state);
            if (next == QcomPathRunning && !p->map_handle) return fail(p, QcomPathMapFailed);
            p->state = next;
            return QcomPathOk;
        }
        uint32_t bytes = 0;
        uint32_t opcode = QCOM_AR_OP_SET_CFG;
        switch (p->state) {
        case QcomPathOpen:
            opcode = QCOM_AR_OP_GRAPH_OPEN;
            bytes = qcom_ar_open_graph(p->apm, sizeof p->apm, p->config.graph);
            break;
        case QcomPathEndpoint:
            bytes = qcom_ar_configure_endpoint(p->apm, sizeof p->apm, p->config.endpoint,
                                               p->config.endpoint_format);
            break;
        case QcomPathStream:
            bytes = qcom_ar_configure_stream(p->apm, sizeof p->apm, p->config.stream_instance,
                                             p->config.stream_format);
            break;
        case QcomPathMap:
            opcode = QCOM_AR_OP_MEM_MAP;
            bytes = qcom_ar_map(p->apm, sizeof p->apm, p->config.stream_instance, p->config.buffer_address,
                                p->config.buffer_bytes);
            break;
        default:
            opcode = QCOM_AR_OP_GRAPH_START;
            bytes = qcom_ar_graph_command(p->apm, sizeof p->apm, QCOM_AR_APM_INSTANCE,
                                          p->config.graph->graph_id);
            break;
        }
        if (!bytes) return fail(p, QcomPathBadConfig);
        return submit(p, opcode, p->apm, bytes);
    }
    default:
        return fail(p, QcomPathWrongState);
    }
}

uint32_t qcom_audio_path_in_flight(const QcomAudioPath *p)
{
    uint32_t n = 0;
    if (!p) return 0;
    for (uint32_t i = 0; i < QCOM_AUDIOPATH_MAX_PERIODS; ++i) if (p->period[i].active) ++n;
    return n;
}

int qcom_audio_path_period(QcomAudioPath *p, uint32_t bytes)
{
    if (!p || p->state != QcomPathRunning) return QcomPathWrongState;
    if (!bytes || bytes > p->config.buffer_bytes) return QcomPathBadConfig;
    /* A command still on its way owns the transmit buffer, and the DSP only owes a bounded number of periods. */
    if (p->outstanding || p->tx_pending) return QcomPathAgain;
    if (qcom_audio_path_in_flight(p) >= p->periods_in_flight) return QcomPathAgain;
    /* Each period starts where the previous one ended, wrapping at the end of the mapped buffer, so two periods
     * never carry the same address: the address is what the DSP's answer is matched against. */
    const uint64_t offset = p->cursor;
    p->cursor = (uint32_t)((p->cursor + bytes) % p->config.buffer_bytes);
    const uint32_t payload = bytes;
    uint8_t frame[32];
    frame[0] = (uint8_t)((p->config.buffer_address + offset) & 0xff);
    for (unsigned i = 1; i < 8; ++i) frame[i] = (uint8_t)(((p->config.buffer_address + offset) >> (8 * i)) & 0xff);
    for (unsigned i = 0; i < 4; ++i) frame[8 + i] = (uint8_t)((p->map_handle >> (8 * i)) & 0xff);
    for (unsigned i = 0; i < 4; ++i) frame[12 + i] = (uint8_t)((payload >> (8 * i)) & 0xff);
    const int rc = submit(p, QCOM_AR_OP_DATA_WRITE, frame, sizeof frame);
    if (rc != QcomPathOk) return rc;
    for (uint32_t i = 0; i < QCOM_AUDIOPATH_MAX_PERIODS; ++i) {
        if (p->period[i].active) continue;
        p->period[i].address = p->config.buffer_address + offset;
        p->period[i].bytes = bytes;
        p->period[i].active = 1;
        ++p->submitted;
        break;
    }
    /* The frame's token is not the period's identity: the DSP echoes the address, which is. */
    p->outstanding = 0;
    p->tx_pending = 0;
    p->stage_sent = 0;
    return QcomPathOk;
}

int qcom_audio_path_restart(QcomAudioPath *p, uint32_t generation)
{
    if (!p) return QcomPathBadConfig;
    if (!generation) return QcomPathBadConfig;
    p->cancelled += qcom_audio_path_in_flight(p);
    for (uint32_t i = 0; i < QCOM_AUDIOPATH_MAX_PERIODS; ++i) p->period[i].active = 0;
    p->generation = generation;
    p->outstanding = 0;
    p->waits = 0;
    p->map_handle = 0;
    p->state = QcomPathIdle;
    p->last_error = QcomPathOk;
    return QcomPathOk;
}
