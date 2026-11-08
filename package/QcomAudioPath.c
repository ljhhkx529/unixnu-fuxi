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

/* The generation sits above the counter, so a reply from before a restart can never match. */
static uint32_t next_token(QcomAudioPath *p)
{
    p->token = (p->generation << 24) | (++p->counter & 0x00ffffffu);
    return p->token;
}

/* Hands the packet already sitting in p->tx to GLINK. A send that cannot go out yet keeps the bytes, so the next step
 * retries them instead of building a new command with a new token. */
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

/* Records what a builder produced in p->tx and tries to send it. */
static int built(QcomAudioPath *p, int rc, uint32_t bytes, uint32_t opcode)
{
    if (rc < 0) return fail(p, rc);
    if (!bytes) return fail(p, QcomPathBadConfig);
    p->tx_bytes = bytes;
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
        (void)gpr_spf_receive(&p->spf, data, length);
        return QcomPathOk;
    }
    QcomArAnswer answer;
    const int kind = qcom_ar_parse(data, length, &answer);
    if (kind == QcomArAnswerBad) {++p->unknown; return QcomPathOk;}
    if (answer.src_port != GPR_PORT_APM && kind != QcomArAnswerWriteDone) {++p->unknown; return QcomPathOk;}

    /* A data-done answer belongs to whichever period carries that address; the commands keep a single outstanding
     * transaction, so a data answer is matched before the token gate. */
    if (kind == QcomArAnswerWriteDone) {
        for (uint32_t i = 0; i < QCOM_AUDIOPATH_MAX_PERIODS; ++i) {
            if (!p->period[i].active || p->period[i].address != answer.address) continue;
            p->period[i].active = 0;
            ++p->completed;
            return QcomPathOk;
        }
        ++p->late;
        return QcomPathOk;
    }
    /* One command transaction is outstanding: a reply for anything else belongs to an older one. */
    if (!p->outstanding || answer.token != p->token) {++p->late; return QcomPathOk;}

    switch (kind) {
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

uint32_t qcom_audio_path_in_flight(const QcomAudioPath *p)
{
    uint32_t n = 0;
    if (!p) return 0;
    for (uint32_t i = 0; i < QCOM_AUDIOPATH_MAX_PERIODS; ++i) if (p->period[i].active) ++n;
    return n;
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
        if (!p->config.graph || !p->config.modules || !p->config.module_count || !p->config.endpoint_instance ||
            !p->config.endpoint_interface || !p->config.endpoint_format || !p->config.stream_format ||
            !p->config.shmem_instance || !p->config.buffer_bytes || !p->link || !p->channel)
            return fail(p, QcomPathBadConfig);
        gpr_spf_init(&p->spf, p->config.gpr_port, DEFAULT_SPF_QUERIES);
        p->state = QcomPathSpf;
        return QcomPathOk;
    case QcomPathSpf: {
        if (qcom_glink_poll(p->link) == QcomGlinkCorrupt) return fail(p, QcomPathSpfError);
        if (p->spf.state == SpfReady) {p->state = QcomPathOpen; return QcomPathOk;}
        if (p->spf.state == SpfGaveUp) return fail(p, QcomPathSpfTimeout);
        if (!p->tx_pending) {
            uint8_t query[GPR_HEADER_BYTES];
            const uint32_t n = gpr_spf_next(&p->spf, query);
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
    case QcomPathStopping: {
        if (qcom_glink_poll(p->link) == QcomGlinkCorrupt) return fail(p, QcomPathCommandFailed);
        if (p->tx_pending) return try_send(p);
        if (p->outstanding) {
            if (++p->waits > budget) {++p->timeouts; return fail(p, QcomPathTimeout);}
            return QcomPathAgain;
        }
        if (p->stage_sent) {
            /* One teardown step answered; on to the next. */
            p->stage_sent = 0;
            p->waits = 0;
            ++p->stopping;
        }
        const uint32_t token = next_token(p);
        if (p->stopping == 1u) {
            const uint32_t n = qcom_ar_graph_command(p->tx, sizeof p->tx, p->config.gpr_port, token,
                                                     p->config.apm_iid, QCOM_AR_OP_GRAPH_STOP,
                                                     p->config.graph->subgraph_id);
            return built(p, n ? 0 : QcomPathBadConfig, n, QCOM_AR_OP_GRAPH_STOP);
        }
        if (p->stopping == 2u) {
            const uint32_t n = qcom_ar_graph_command(p->tx, sizeof p->tx, p->config.gpr_port, token,
                                                     p->config.apm_iid, QCOM_AR_OP_GRAPH_CLOSE,
                                                     p->config.graph->subgraph_id);
            return built(p, n ? 0 : QcomPathBadConfig, n, QCOM_AR_OP_GRAPH_CLOSE);
        }
        if (p->stopping == 3u && p->map_handle) {
            const uint32_t n = qcom_ar_unmap(p->tx, sizeof p->tx, p->config.gpr_port, token, p->config.apm_iid,
                                             p->map_handle);
            return built(p, n ? 0 : QcomPathBadConfig, n, QCOM_AR_OP_MEM_UNMAP);
        }
        /* Teardown over: nothing is left mapped, and the periods that were in flight are cancelled. */
        p->cancelled += qcom_audio_path_in_flight(p);
        for (uint32_t i = 0; i < QCOM_AUDIOPATH_MAX_PERIODS; ++i) p->period[i].active = 0;
        p->map_handle = 0;
        p->cursor = 0;
        p->stopping = 0;
        p->state = QcomPathIdle;
        return QcomPathOk;
    }
    case QcomPathOpen:
    case QcomPathEndpoint:
    case QcomPathStream:
    case QcomPathMap:
    case QcomPathStart: {
        if (qcom_glink_poll(p->link) == QcomGlinkCorrupt) return fail(p, QcomPathCommandFailed);
        if (p->tx_pending) return try_send(p);
        if (p->outstanding) {
            if (++p->waits > budget) {++p->timeouts; return fail(p, QcomPathTimeout);}
            return QcomPathAgain;
        }
        if (p->stage_sent) {
            p->stage_sent = 0;
            p->waits = 0;
            const int next = next_stage(p->state);
            if (next == QcomPathRunning && !p->map_handle) return fail(p, QcomPathMapFailed);
            p->state = next;
            return QcomPathOk;
        }

        const uint32_t token = next_token(p);
        switch (p->state) {
        case QcomPathOpen: {
            const uint32_t n = qcom_ar_graph_open(p->tx, sizeof p->tx, p->config.gpr_port, token, p->config.graph,
                                                  p->config.modules, p->config.module_count, p->config.links,
                                                  p->config.link_count);
            return built(p, n ? 0 : QcomPathBadConfig, n, QCOM_AR_OP_GRAPH_OPEN);
        }
        case QcomPathEndpoint: {
            const uint32_t n = qcom_ar_config_hw_endpoint(p->tx, sizeof p->tx, p->config.gpr_port, token,
                                                          p->config.apm_iid, p->config.endpoint_instance,
                                                          p->config.endpoint_interface, p->config.endpoint_format);
            return built(p, n ? 0 : QcomPathBadConfig, n, QCOM_AR_OP_SET_CFG);
        }
        case QcomPathStream: {
            const uint32_t n = qcom_ar_config_pcm_output(p->tx, sizeof p->tx, p->config.gpr_port, token,
                                                         p->config.apm_iid, p->config.stream_instance,
                                                         p->config.stream_format, QCOM_AR_INTERLEAVED);
            return built(p, n ? 0 : QcomPathBadConfig, n, QCOM_AR_OP_SET_CFG);
        }
        case QcomPathMap: {
            const uint32_t n = qcom_ar_map(p->tx, sizeof p->tx, p->config.gpr_port, token, p->config.apm_iid,
                                           p->config.buffer_address, p->config.buffer_bytes);
            return built(p, n ? 0 : QcomPathBadConfig, n, QCOM_AR_OP_MEM_MAP);
        }
        default: {
            const uint32_t n = qcom_ar_graph_command(p->tx, sizeof p->tx, p->config.gpr_port, token,
                                                     p->config.apm_iid, QCOM_AR_OP_GRAPH_START,
                                                     p->config.graph->subgraph_id);
            return built(p, n ? 0 : QcomPathBadConfig, n, QCOM_AR_OP_GRAPH_START);
        }
        }
    }
    default:
        return fail(p, QcomPathWrongState);
    }
}

int qcom_audio_path_stop(QcomAudioPath *p)
{
    if (!p) return QcomPathBadConfig;
    if (p->state != QcomPathRunning) return QcomPathWrongState;
    if (p->tx_pending || p->outstanding) return QcomPathAgain;
    p->stopping = 1;
    p->stage_sent = 0;
    p->waits = 0;
    p->state = QcomPathStopping;
    return QcomPathOk;
}

int qcom_audio_path_period(QcomAudioPath *p, uint32_t bytes)
{
    if (!p || p->state != QcomPathRunning) return QcomPathWrongState;
    if (!bytes || bytes > p->config.buffer_bytes) return QcomPathBadConfig;
    if (p->outstanding || p->tx_pending) return QcomPathAgain;
    if (qcom_audio_path_in_flight(p) >= p->periods_in_flight) return QcomPathAgain;

    /* Each period starts where the previous one ended and wraps, so two periods never carry the same address: the
     * address is what the DSP's answer is matched against. */
    const uint64_t offset = p->cursor;
    p->cursor = (uint32_t)((p->cursor + bytes) % p->config.buffer_bytes);
    const uint32_t token = next_token(p);
    const uint32_t n = qcom_ar_write(p->tx, sizeof p->tx, p->config.gpr_port, token, p->config.shmem_instance,
                                     p->config.buffer_address + offset, p->map_handle, bytes);
    if (!n) return fail(p, QcomPathBadConfig);
    const int rc = built(p, 0, n, QCOM_AR_OP_WRITE);
    if (rc != QcomPathOk) return rc;
    for (uint32_t i = 0; i < QCOM_AUDIOPATH_MAX_PERIODS; ++i) {
        if (p->period[i].active) continue;
        p->period[i].address = p->config.buffer_address + offset;
        p->period[i].bytes = bytes;
        p->period[i].active = 1;
        ++p->submitted;
        break;
    }
    /* A period's identity is its address, not its token, so the command slot is free again at once. */
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
    p->tx_pending = 0;
    p->stage_sent = 0;
    p->waits = 0;
    p->map_handle = 0;
    p->cursor = 0;
    p->state = QcomPathIdle;
    p->last_error = QcomPathOk;
    return QcomPathOk;
}
