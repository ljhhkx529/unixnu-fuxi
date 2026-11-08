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
