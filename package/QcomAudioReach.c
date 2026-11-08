/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAudioReach.h"

/* Little-endian writer over a caller buffer. Setting bad marks the writer unusable, and every builder then
 * returns zero rather than a truncated command. */
typedef struct {uint8_t *p; uint32_t n, cap; int bad;} W;

static void put8(W *w, uint32_t v) {if (w->n + 1u > w->cap) {w->bad = 1; return;} w->p[w->n++] = (uint8_t)v;}
static void put16(W *w, uint32_t v) {put8(w, v); put8(w, v >> 8);}
static void put32(W *w, uint32_t v) {put16(w, v); put16(w, v >> 16);}
static void pad_to(W *w, uint32_t end) {while (!w->bad && w->n < end) put8(w, 0);}
static uint32_t align8(uint32_t v) {return (v + 7u) & ~7u;}

static void set32(W *w, uint32_t at, uint32_t v)
{
    if (at + 4u > w->n) return;
    w->p[at] = (uint8_t)v; w->p[at + 1u] = (uint8_t)(v >> 8);
    w->p[at + 2u] = (uint8_t)(v >> 16); w->p[at + 3u] = (uint8_t)(v >> 24);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void init(W *w, uint8_t *out, uint32_t capacity)
{
    w->p = out; w->n = 0; w->cap = capacity; w->bad = (out == 0 || capacity < QCOM_AR_CMD_HEADER_BYTES);
}

/* The APM command header: four words, the last of which carries the payload size once it is known. */
static uint32_t payload_start(W *w)
{
    const uint32_t at = w->n;
    put32(w, 0); put32(w, 0); put32(w, 0); put32(w, 0);
    return at;
}

static void payload_end(W *w, uint32_t at) {set32(w, at + 12u, w->n - at - QCOM_AR_CMD_HEADER_BYTES);}

/* One parameter block: header, body, then padding to eight bytes; the size counts body and padding. */
static uint32_t param_start(W *w, uint32_t instance, uint32_t id)
{
    const uint32_t at = w->n;
    put32(w, instance); put32(w, id); put32(w, 0); put32(w, 0);
    return at;
}

static void param_end(W *w, uint32_t at)
{
    pad_to(w, at + align8(w->n - at));
    set32(w, at + 8u, w->n - at - QCOM_AR_PARAM_HEADER_BYTES);
}

/* The interleaved channel map both the PCM output and the shared memory endpoint carry: front-left, front-right, ... */
static void channel_map(W *w, uint32_t channels)
{
    for (uint32_t i = 0; i < channels; ++i) put8(w, i + 1u);
}

/* The PCM output format of a decoder, converter or encoder. */
static void pcm_output(W *w, uint32_t instance, const QcomArFormat *f)
{
    const uint32_t at = param_start(w, instance, QCOM_AR_PARAM_PCM_OUTPUT);
    put32(w, 1); put32(w, f->media);
    put32(w, (16u + f->channels + 3u) & ~3u);
    put16(w, f->bit_width); put16(w, 1); put16(w, f->bit_width); put16(w, f->bit_width - 1u);
    put16(w, 1); put16(w, f->interleaving); put16(w, 0); put16(w, f->channels);
    channel_map(w, f->channels);
    param_end(w, at);
}

/* The hardware endpoint's format: rate, width and channel count. */
static void hardware_media_format(W *w, uint32_t instance, const QcomArFormat *f)
{
    const uint32_t at = param_start(w, instance, QCOM_AR_PARAM_HW_EP_MF);
    put32(w, 1);
    put32(w, f->bit_width); put16(w, f->channels); put16(w, 0);
    param_end(w, at);
}

uint32_t qcom_ar_open_graph(uint8_t *out, uint32_t capacity, const QcomArGraph *g)
{
    if (!g || !g->modules || !g->module_count) return 0;
    if (g->module_count > QCOM_AR_MAX_MODULES || g->link_count > QCOM_AR_MAX_LINKS) return 0;
    if (g->link_count && !g->links) return 0;

    W w;
    init(&w, out, capacity);
    const uint32_t cmd = payload_start(&w);

    uint32_t at = param_start(&w, QCOM_AR_APM_INSTANCE, QCOM_AR_PARAM_SG_CONFIG);
    put32(&w, 1); put32(&w, g->graph_id); put32(&w, 3);
    put32(&w, QCOM_AR_PROP_SG_PERF); put32(&w, 4); put32(&w, g->performance);
    put32(&w, QCOM_AR_PROP_SG_DIRECTION); put32(&w, 4); put32(&w, g->direction);
    put32(&w, QCOM_AR_PROP_SG_SCENARIO); put32(&w, 4); put32(&w, g->scenario);
    param_end(&w, at);

    at = param_start(&w, QCOM_AR_APM_INSTANCE, QCOM_AR_PARAM_CONTAINER_CONFIG);
    put32(&w, 1); put32(&w, g->container_id); put32(&w, 4);
    put32(&w, QCOM_AR_PROP_CONTAINER_CAPABILITY); put32(&w, 8); put32(&w, 1); put32(&w, QCOM_AR_CONTAINER_GLOBAL);
    put32(&w, QCOM_AR_PROP_CONTAINER_POSITION); put32(&w, 4); put32(&w, g->container_position);
    put32(&w, QCOM_AR_PROP_CONTAINER_STACK_SIZE); put32(&w, 4); put32(&w, g->container_stack_bytes);
    put32(&w, QCOM_AR_PROP_CONTAINER_DOMAIN); put32(&w, 4); put32(&w, g->container_domain);
    param_end(&w, at);

    at = param_start(&w, QCOM_AR_APM_INSTANCE, QCOM_AR_PARAM_MODULE_LIST);
    put32(&w, 1); put32(&w, g->graph_id); put32(&w, g->container_id); put32(&w, g->module_count);
    for (uint32_t i = 0; i < g->module_count; ++i) {put32(&w, g->modules[i].module); put32(&w, g->modules[i].instance);}
    param_end(&w, at);

    at = param_start(&w, QCOM_AR_APM_INSTANCE, QCOM_AR_PARAM_MODULE_PROP);
    put32(&w, g->module_count);
    for (uint32_t i = 0; i < g->module_count; ++i) {
        put32(&w, g->modules[i].instance); put32(&w, 1); put32(&w, QCOM_AR_PROP_MODULE_PORT_INFO); put32(&w, 8);
        put32(&w, g->modules[i].inputs); put32(&w, g->modules[i].outputs);
    }
    param_end(&w, at);

    at = param_start(&w, QCOM_AR_APM_INSTANCE, QCOM_AR_PARAM_MODULE_CONN);
    put32(&w, g->link_count);
    for (uint32_t i = 0; i < g->link_count; ++i) {
        put32(&w, g->links[i].from_instance); put32(&w, g->links[i].from_port);
        put32(&w, g->links[i].to_instance); put32(&w, g->links[i].to_port);
    }
    param_end(&w, at);

    payload_end(&w, cmd);
    return w.bad ? 0 : w.n;
}

uint32_t qcom_ar_graph_command(uint8_t *out, uint32_t capacity, uint32_t apm_instance, uint32_t graph_id)
{
    W w;
    init(&w, out, capacity);
    const uint32_t cmd = payload_start(&w);
    const uint32_t at = param_start(&w, apm_instance, QCOM_AR_PARAM_SG_LIST);
    put32(&w, 1); put32(&w, graph_id);
    param_end(&w, at);
    payload_end(&w, cmd);
    return w.bad ? 0 : w.n;
}

uint32_t qcom_ar_configure_endpoint(uint8_t *out, uint32_t capacity, const QcomArEndpoint *ep,
                                    const QcomArFormat *f)
{
    if (!ep || !f) return 0;
    W w;
    init(&w, out, capacity);
    const uint32_t cmd = payload_start(&w);

    if (ep->interface_type == QCOM_AR_INTERFACE_I2S) {
        const uint32_t at = param_start(&w, ep->instance, QCOM_AR_PARAM_I2S_INTF);
        put32(&w, ep->interface_index); put32(&w, ep->secondary);
        put16(&w, ep->line); put16(&w, ep->ws_source);
        param_end(&w, at);
    } else if (ep->interface_type == QCOM_AR_INTERFACE_CODEC_DMA) {
        const uint32_t at = param_start(&w, ep->instance, QCOM_AR_PARAM_CODEC_DMA_INTF);
        put32(&w, ep->interface_index); put32(&w, ep->slot_mask); put32(&w, 0);
        param_end(&w, at);
    } else {
        return 0;
    }

    hardware_media_format(&w, ep->instance, f);

    const uint32_t at = param_start(&w, ep->instance, QCOM_AR_PARAM_FRAME_SIZE_FACTOR);
    put32(&w, f->bytes_per_frame);
    param_end(&w, at);

    payload_end(&w, cmd);
    return w.bad ? 0 : w.n;
}

uint32_t qcom_ar_configure_stream(uint8_t *out, uint32_t capacity, uint32_t instance, const QcomArFormat *f)
{
    if (!f || !f->channels || f->bit_width < 8u) return 0;
    W w;
    init(&w, out, capacity);
    const uint32_t cmd = payload_start(&w);

    const uint32_t at = param_start(&w, instance, QCOM_AR_PARAM_MEDIA_FORMAT);
    put32(&w, 1); put32(&w, f->media);
    put16(&w, f->bit_width); put16(&w, 1); put16(&w, f->bit_width); put16(&w, f->bit_width - 1u);
    put16(&w, 1); put16(&w, f->channels);
    channel_map(&w, f->channels);
    param_end(&w, at);

    pcm_output(&w, instance, f);
    payload_end(&w, cmd);
    return w.bad ? 0 : w.n;
}

uint32_t qcom_ar_prm_clock(uint8_t *out, uint32_t capacity, uint32_t prm_instance, uint32_t clock_id, uint32_t hz)
{
    W w;
    init(&w, out, capacity);
    const uint32_t cmd = payload_start(&w);

    uint32_t at = param_start(&w, prm_instance, QCOM_AR_PARAM_CLOCK_ID);
    put32(&w, clock_id);
    param_end(&w, at);

    at = param_start(&w, prm_instance, QCOM_AR_PARAM_CLOCK_FREQUENCY);
    put32(&w, hz);
    param_end(&w, at);

    payload_end(&w, cmd);
    return w.bad ? 0 : w.n;
}

uint32_t qcom_ar_map(uint8_t *out, uint32_t capacity, uint32_t instance, uint64_t address, uint32_t bytes)
{
    if (!bytes) return 0;
    W w;
    init(&w, out, capacity);
    const uint32_t cmd = payload_start(&w);

    /* One region in the shared pool: pool id 3, one region, no flags, then address and the 4 KiB-rounded length. */
    put32(&w, 3); put32(&w, 1); put32(&w, 0);
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32));
    put32(&w, (bytes + 4095u) & ~4095u);
    (void)instance;

    /* This command carries no parameter header: the region follows the command header directly, as the DSP's own
     * client builds it. The payload is padded to eight bytes; the trailing header space the DSP's client leaves
     * belongs to the GPR framing, which QcomGpr supplies. */
    pad_to(&w, align8(w.n));
    payload_end(&w, cmd);
    return w.bad ? 0 : w.n;
}

uint32_t qcom_ar_unmap(uint8_t *out, uint32_t capacity, uint32_t instance, uint32_t handle)
{
    W w;
    init(&w, out, capacity);
    const uint32_t cmd = payload_start(&w);
    put32(&w, handle);
    (void)instance;
    pad_to(&w, align8(w.n));
    payload_end(&w, cmd);
    return w.bad ? 0 : w.n;
}

int qcom_ar_parse(const GprPacket *packet, QcomArAnswer *answer)
{
    if (!packet || !answer) return QcomArAnswerBad;
    for (uint32_t i = 0; i < sizeof *answer; ++i) ((uint8_t *)answer)[i] = 0;
    answer->opcode = packet->opcode;
    const uint8_t *d = packet->payload;
    const uint32_t n = packet->payload_bytes;

    switch (packet->opcode) {
    case GPR_OP_BASIC_RESULT:
        if (n < 8u) return QcomArAnswerBad;
        answer->result_opcode = le32(d); answer->status = le32(d + 4);
        return QcomArAnswerBasicResult;
    case QCOM_AR_OP_MEM_MAP_DONE:
        if (n < 4u) return QcomArAnswerBad;
        answer->handle = le32(d);
        return QcomArAnswerMapped;
    case QCOM_AR_OP_PRM_REQUEST_DONE:
    case QCOM_AR_OP_PRM_RELEASE_DONE:
        if (n < 4u) return QcomArAnswerBad;
        answer->status = le32(d + (n >= 8u ? 4u : 0u));
        return QcomArAnswerPrmDone;
    case GPR_OP_APM_SPF_STATE:
        if (n < 4u) return QcomArAnswerBad;
        answer->status = le32(d);
        return QcomArAnswerFrameworkState;
    case QCOM_AR_OP_DATA_WRITE_DONE:
    case QCOM_AR_OP_DATA_READ_DONE:
        if (n < 8u) return QcomArAnswerBad;
        answer->address = (uint64_t)le32(d) | (uint64_t)le32(d + 4) << 32;
        if (n >= 16u) {answer->data_bytes = le32(d + 8); answer->offset = le32(d + 12);}
        return QcomArAnswerDataDone;
    default:
        return QcomArAnswerUnknown;
    }
}
