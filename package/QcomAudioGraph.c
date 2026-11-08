/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAudioGraph.h"

#include "ShengGpr.h"

#define PARAM_HEADER 16u
#define CMD_HEADER 16u

/* The writer sheng uses: a cursor over the caller's buffer that records when it ran out of room instead of writing
 * past the end. */
typedef struct {uint8_t *p; uint32_t n, cap; int bad;} W;

static void put8(W *w, uint32_t v) {if (w->n + 1 > w->cap) {w->bad = 1; return;} w->p[w->n++] = (uint8_t)v;}
static void put16(W *w, uint32_t v) {put8(w, v); put8(w, v >> 8);}
static void put32(W *w, uint32_t v) {put16(w, v); put16(w, v >> 16);}
static void pad_to(W *w, uint32_t end) {while (!w->bad && w->n < end) put8(w, 0);}
static uint32_t align8(uint32_t v) {return (v + 7u) & ~7u;}
static void set32(W *w, uint32_t at, uint32_t v)
{
    if (at + 4 <= w->n) {
        w->p[at] = (uint8_t)v; w->p[at + 1] = (uint8_t)(v >> 8);
        w->p[at + 2] = (uint8_t)(v >> 16); w->p[at + 3] = (uint8_t)(v >> 24);
    }
}

/* The GPR header, with the total size patched in by finish(). */
static void begin(W *w, uint8_t *out, uint32_t capacity, uint32_t port, uint32_t dst, uint32_t token, uint32_t opcode)
{
    w->p = out; w->n = 0;
    w->cap = capacity < GPR_MAX_PACKET ? capacity : GPR_MAX_PACKET;
    w->bad = 0;
    put32(w, 0);
    put32(w, GPR_DOMAIN_ADSP | GPR_DOMAIN_APPS << 8);
    put32(w, port); put32(w, dst); put32(w, token); put32(w, opcode);
}

static uint32_t finish(W *w)
{
    if (w->bad) return 0;
    set32(w, 0, (GPR_VERSION & 0xfu) | (GPR_HEADER_BYTES / 4) << 4 | w->n << 8);
    return w->n;
}

/* The APM command header: an in-band payload of the given size follows, patched in at the end. */
static uint32_t cmd_payload_start(W *w)
{
    const uint32_t at = w->n;
    put32(w, 0); put32(w, 0); put32(w, 0); put32(w, 0);
    return at;
}
static void cmd_payload_end(W *w, uint32_t at) {set32(w, at + 12, w->n - at - CMD_HEADER);}

/* One parameter block: header, body from the caller, then padding to eight bytes; the size counts body and padding. */
static uint32_t param_begin(W *w, uint32_t iid, uint32_t id)
{
    const uint32_t at = w->n;
    put32(w, iid); put32(w, id); put32(w, 0); put32(w, 0);
    return at;
}
static void param_end(W *w, uint32_t at) {pad_to(w, at + align8(w->n - at)); set32(w, at + 8, w->n - at - PARAM_HEADER);}

uint32_t qcom_ar_graph_open(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token,
                            const QcomArGraphSpec *spec, const QcomArGraphModule *modules, uint32_t module_count,
                            const QcomArGraphLink *links, uint32_t link_count)
{
    if (!out || !spec || !modules || !module_count) return 0;
    if (links && !link_count) links = 0;

    W w;
    begin(&w, out, capacity, port, spec->apm_iid, token, QCOM_AR_OP_GRAPH_OPEN);
    const uint32_t cmd = cmd_payload_start(&w);

    /* The subgraph: identity and the three properties the framework reads from it. */
    uint32_t at = param_begin(&w, spec->apm_iid, QCOM_AR_PARAM_SG_CONFIG);
    put32(&w, 1);
    put32(&w, spec->subgraph_id); put32(&w, 3);
    put32(&w, QCOM_AR_PROP_SG_PERF); put32(&w, 4); put32(&w, spec->performance);
    put32(&w, QCOM_AR_PROP_SG_DIRECTION); put32(&w, 4); put32(&w, spec->direction);
    put32(&w, QCOM_AR_PROP_SG_SCENARIO); put32(&w, 4); put32(&w, spec->scenario);
    param_end(&w, at);

    /* One container, in the domain the board puts its graphs in. */
    at = param_begin(&w, spec->apm_iid, QCOM_AR_PARAM_CONTAINER_CONFIG);
    put32(&w, 1);
    put32(&w, spec->container_id); put32(&w, 4);
    put32(&w, QCOM_AR_PROP_CONT_CAPS); put32(&w, 8); put32(&w, 1); put32(&w, QCOM_AR_CONTAINER_GC);
    put32(&w, QCOM_AR_PROP_CONT_POS); put32(&w, 4); put32(&w, spec->container_position);
    put32(&w, QCOM_AR_PROP_CONT_STACK); put32(&w, 4); put32(&w, spec->container_stack_bytes);
    put32(&w, QCOM_AR_PROP_CONT_DOMAIN); put32(&w, 4); put32(&w, spec->container_domain);
    param_end(&w, at);

    /* The modules, then their ports. */
    at = param_begin(&w, spec->apm_iid, QCOM_AR_PARAM_MODULE_LIST);
    put32(&w, 1);
    put32(&w, spec->subgraph_id); put32(&w, spec->container_id); put32(&w, module_count);
    for (uint32_t i = 0; i < module_count; ++i) {
        put32(&w, modules[i].module); put32(&w, modules[i].instance);
    }
    param_end(&w, at);

    at = param_begin(&w, spec->apm_iid, QCOM_AR_PARAM_MODULE_PROP);
    put32(&w, module_count);
    for (uint32_t i = 0; i < module_count; ++i) {
        put32(&w, modules[i].instance); put32(&w, 1);
        put32(&w, QCOM_AR_PROP_PORT_INFO); put32(&w, 8);
        put32(&w, modules[i].inputs); put32(&w, modules[i].outputs);
    }
    param_end(&w, at);

    /* The connections between them. A link into another subgraph's instance is how a stream graph joins its device
     * graph, so the same shape covers both. */
    at = param_begin(&w, spec->apm_iid, QCOM_AR_PARAM_MODULE_CONN);
    put32(&w, link_count);
    for (uint32_t i = 0; i < link_count; ++i) {
        put32(&w, links[i].from_instance); put32(&w, QCOM_AR_OUT_PORT);
        put32(&w, links[i].to_instance); put32(&w, QCOM_AR_IN_PORT);
    }
    param_end(&w, at);

    cmd_payload_end(&w, cmd);
    return finish(&w);
}

/* A hardware endpoint's format: rate, width, channels, and the one word the framework calls its "MF" flag. */
static void hw_ep_mf(W *w, uint32_t iid, const QcomArFormat *f)
{
    const uint32_t at = param_begin(w, iid, QCOM_AR_PARAM_HW_EP_MF);
    put32(w, f->rate); put16(w, f->bits); put16(w, f->channels); put32(w, 1);
    param_end(w, at);
}

static void frame_size_factor(W *w, uint32_t iid)
{
    const uint32_t at = param_begin(w, iid, QCOM_AR_PARAM_FRAME_SIZE_FACTOR);
    put32(w, 1);
    param_end(w, at);
}

/* The framework's channel map: channel i is assigned to the i+1'th, which is what a stereo pair or a mono input needs. */
static void channel_map(W *w, uint32_t channels) {for (uint32_t i = 0; i < channels; ++i) put8(w, i + 1);}

/* The media format of a converter's output. */
static void pcm_output(W *w, uint32_t iid, const QcomArFormat *f, uint32_t interleaving)
{
    const uint32_t at = param_begin(w, iid, QCOM_AR_PARAM_PCM_OUTPUT);
    put32(w, 1); put32(w, QCOM_AR_MEDIA_PCM); put32(w, (16u + f->channels + 3u) & ~3u);
    put16(w, f->bits); put16(w, 1); put16(w, f->bits); put16(w, f->bits - 1u);
    put16(w, 1); put16(w, interleaving); put16(w, 0);
    put16(w, f->channels);
    channel_map(w, f->channels);
    param_end(w, at);
}

uint32_t qcom_ar_config_hw_endpoint(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t apm_iid,
                                    uint32_t endpoint_iid, const QcomArI2sInterface *intf, const QcomArFormat *format)
{
    if (!out || !intf || !format) return 0;
    W w;
    begin(&w, out, capacity, port, apm_iid, token, QCOM_AR_OP_SET_CFG);
    const uint32_t cmd = cmd_payload_start(&w);
    uint32_t at = param_begin(&w, endpoint_iid, QCOM_AR_PARAM_I2S_INTF);
    put32(&w, intf->lpaif); put32(&w, intf->interface_type);
    put16(&w, intf->sd_line); put16(&w, intf->ws_source);
    param_end(&w, at);
    hw_ep_mf(&w, endpoint_iid, format);
    frame_size_factor(&w, endpoint_iid);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

uint32_t qcom_ar_config_pcm_output(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t apm_iid,
                                   uint32_t instance, const QcomArFormat *format, uint32_t interleaving)
{
    if (!out || !format || !instance) return 0;
    W w;
    begin(&w, out, capacity, port, apm_iid, token, QCOM_AR_OP_SET_CFG);
    const uint32_t cmd = cmd_payload_start(&w);
    pcm_output(&w, instance, format, interleaving);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

uint32_t qcom_ar_config_media_format(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t instance,
                                     const QcomArFormat *format)
{
    if (!out || !format || !instance) return 0;
    W w;
    begin(&w, out, capacity, port, instance, token, QCOM_AR_OP_SET_CFG);
    const uint32_t cmd = cmd_payload_start(&w);
    const uint32_t at = param_begin(&w, instance, QCOM_AR_PARAM_MEDIA_FORMAT);
    const uint32_t body = align8(12u + 16u + format->channels);
    put32(&w, 1); put32(&w, QCOM_AR_MEDIA_PCM); put32(&w, body + PARAM_HEADER - 12u);
    put32(&w, format->rate); put16(&w, format->bits); put16(&w, 1);
    put16(&w, format->bits); put16(&w, format->bits - 1u); put16(&w, 1);
    put16(&w, format->channels);
    channel_map(&w, format->channels);
    param_end(&w, at);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

uint32_t qcom_ar_graph_command(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t apm_iid,
                               uint32_t opcode, uint32_t subgraph_id)
{
    if (!out || !subgraph_id) return 0;
    W w;
    begin(&w, out, capacity, port, apm_iid, token, opcode);
    const uint32_t cmd = cmd_payload_start(&w);
    const uint32_t at = param_begin(&w, apm_iid, QCOM_AR_PARAM_SG_LIST);
    put32(&w, 1); put32(&w, subgraph_id);
    param_end(&w, at);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

/* The region follows the header directly and the command-header space trails the packet, as the DSP's own client builds
 * it; the size is rounded up to a whole 4 KiB page. */
uint32_t qcom_ar_map(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t apm_iid,
                     uint64_t address, uint32_t bytes)
{
    if (!out || !bytes) return 0;
    W w;
    begin(&w, out, capacity, port, apm_iid, token, QCOM_AR_OP_MEM_MAP);
    put16(&w, 3); put16(&w, 1); put32(&w, 0);
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, (bytes + 4095u) & ~4095u);
    pad_to(&w, w.n + CMD_HEADER);
    return finish(&w);
}

uint32_t qcom_ar_unmap(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t apm_iid, uint32_t handle)
{
    if (!out || !handle) return 0;
    W w;
    begin(&w, out, capacity, port, apm_iid, token, QCOM_AR_OP_MEM_UNMAP);
    put32(&w, handle);
    pad_to(&w, w.n + CMD_HEADER);
    return finish(&w);
}

uint32_t qcom_ar_write(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t shmem_iid,
                       uint64_t address, uint32_t handle, uint32_t bytes)
{
    if (!out || !shmem_iid) return 0;
    W w;
    begin(&w, out, capacity, port, shmem_iid, token, QCOM_AR_OP_WRITE);
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, handle); put32(&w, bytes);
    put32(&w, 0); put32(&w, 0); put32(&w, 0);
    put32(&w, 0); put32(&w, 0); put32(&w, 0); put32(&w, 0);
    return finish(&w);
}

uint32_t qcom_ar_read(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t shmem_iid,
                      uint64_t address, uint32_t handle, uint32_t bytes)
{
    if (!out || !shmem_iid) return 0;
    W w;
    begin(&w, out, capacity, port, shmem_iid, token, QCOM_AR_OP_READ);
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, handle); put32(&w, bytes);
    put32(&w, 0); put32(&w, 0); put32(&w, 0); put32(&w, 0);
    return finish(&w);
}

/* The resource manager's clock parameter is exact-sized rather than padded: the reference client does not pad these. */
uint32_t qcom_ar_prm_clock(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t clock_id,
                           uint32_t hz)
{
    if (!out || !clock_id) return 0;
    W w;
    begin(&w, out, capacity, port, QCOM_AR_PRM_IID, token,
          hz ? QCOM_AR_OP_PRM_REQUEST : QCOM_AR_OP_PRM_RELEASE);
    const uint32_t cmd = cmd_payload_start(&w);
    const uint32_t at = param_begin(&w, QCOM_AR_PRM_IID, QCOM_AR_PARAM_HW_CLOCK);
    put32(&w, 1); put32(&w, clock_id);
    if (hz) {put32(&w, hz); put32(&w, 1); put32(&w, 0);}   /* frequency, "couple no", default root */
    set32(&w, at + 8, w.n - at - PARAM_HEADER);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

uint32_t qcom_ar_prm_vote(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t block, int enable)
{
    if (!out || !block) return 0;
    W w;
    begin(&w, out, capacity, port, QCOM_AR_PRM_IID, token,
          enable ? QCOM_AR_OP_PRM_REQUEST : QCOM_AR_OP_PRM_RELEASE);
    const uint32_t cmd = cmd_payload_start(&w);
    const uint32_t at = param_begin(&w, QCOM_AR_PRM_IID, QCOM_AR_PARAM_HW_CORE);
    put32(&w, block);
    set32(&w, at + 8, w.n - at - PARAM_HEADER);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int qcom_ar_parse(const void *packet, uint32_t length, QcomArAnswer *answer)
{
    if (!packet || !answer) return QcomArAnswerBad;
    GprPacket p;
    for (uint32_t i = 0; i < sizeof *answer; ++i) ((uint8_t *)answer)[i] = 0;
    if (gpr_decode(packet, length, &p) != GprOk) return QcomArAnswerBad;
    answer->opcode = p.opcode;
    answer->token = p.token;
    answer->src_port = p.src_port;
    const uint8_t *d = p.payload;
    switch (p.opcode) {
    case GPR_OP_BASIC_RESULT:
        if (p.payload_bytes < 8) return QcomArAnswerBad;
        answer->result_opcode = le32(d);
        answer->status = le32(d + 4);
        return QcomArAnswerBasicResult;
    case QCOM_AR_OP_PRM_REQUEST_DONE:
    case QCOM_AR_OP_PRM_RELEASE_DONE:
        if (p.payload_bytes < 4) return QcomArAnswerBad;
        answer->status = le32(d + (p.payload_bytes >= 8 ? 4 : 0));
        return QcomArAnswerPrmDone;
    case GPR_OP_APM_SPF_STATE:
        if (p.payload_bytes < 4) return QcomArAnswerBad;
        answer->status = le32(d);
        return QcomArAnswerFrameworkState;
    case QCOM_AR_OP_MEM_MAP_DONE:
        if (p.payload_bytes < 4) return QcomArAnswerBad;
        answer->handle = le32(d);
        return QcomArAnswerMapped;
    case QCOM_AR_OP_WRITE_DONE:
        if (p.payload_bytes < 16) return QcomArAnswerBad;
        answer->address = le32(d) | (uint64_t)le32(d + 4) << 32;
        answer->handle = le32(d + 8);
        answer->status = le32(d + 12);
        return QcomArAnswerWriteDone;
    case QCOM_AR_OP_READ_DONE:
        if (p.payload_bytes < 20) return QcomArAnswerBad;
        answer->status = le32(d);
        answer->address = le32(d + 4) | (uint64_t)le32(d + 8) << 32;
        answer->handle = le32(d + 12);
        answer->data_bytes = le32(d + 16);
        answer->offset = p.payload_bytes >= 24 ? le32(d + 20) : 0;
        return QcomArAnswerReadDone;
    default:
        return QcomArAnswerUnknown;
    }
}
