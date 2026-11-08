/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the graph-open command: the parameter block layout sheng's builder produces, checked block by block
 * the way the DSP reads it, with every board value coming from the caller. */
#include <stdio.h>
#include <string.h>

#include "../QcomAudioGraph.h"
#include "../ShengGpr.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

typedef struct {uint32_t iid, id, size, offset;} Block;

/* Walks the in-band parameter blocks of one command the way sheng's own test does: each must be eight-byte sized,
 * must stay inside the payload, and its body must not be padded with a non-zero tail field. */
static unsigned walk(const uint8_t *packet, uint32_t length, Block *blocks, unsigned max)
{
    GprPacket p;
    if (gpr_decode(packet, length, &p) != GprOk) return 0;
    if (p.size != length) return 0;
    if (p.payload_bytes < 16 || le32(p.payload + 12) != p.payload_bytes - 16) return 0;
    unsigned count = 0;
    for (uint32_t at = 16; at < p.payload_bytes;) {
        if (at + 16 > p.payload_bytes || count >= max) return 0;
        const uint32_t size = le32(p.payload + at + 8);
        if (size % 8 || at + 16 + size > p.payload_bytes) return 0;
        if (le32(p.payload + at + 12) != 0) return 0;
        blocks[count].iid = le32(p.payload + at);
        blocks[count].id = le32(p.payload + at + 4);
        blocks[count].size = size;
        blocks[count].offset = at + 16;
        ++count;
        at += 16 + size;
    }
    return count;
}

/* Finds a property inside a parameter block. A subgraph or container block starts with the element count and then
 * {id, property_count}, so the properties begin twelve bytes in; the module-property block has no such header. */
static int property(const uint8_t *payload, const Block *b, uint32_t skip, uint32_t id, uint32_t *value, uint32_t *size)
{
    uint32_t at = b->offset + skip;
    const uint32_t end = b->offset + b->size;
    while (at + 8 <= end) {
        const uint32_t prop = le32(payload + at), bytes = le32(payload + at + 4);
        if (prop == id) {
            if (value) *value = le32(payload + at + 8);
            if (size) *size = bytes;
            return 1;
        }
        at += 8 + ((bytes + 3u) & ~3u);
    }
    return 0;
}

static QcomArGraphSpec speaker_spec(void)
{
    QcomArGraphSpec s;
    memset(&s, 0, sizeof s);
    s.apm_iid = 1u;
    s.subgraph_id = 0x5100u;        /* this board's, not sheng's 0x4005 */
    s.performance = 2u;
    s.direction = 1u;
    s.scenario = 1u;
    s.container_id = 0x9001u;
    s.container_position = 4u;
    s.container_stack_bytes = 8192u;
    s.container_domain = 2u;
    return s;
}

static void test_device_graph(void)
{
    /* A device graph: one module that owns the hardware endpoint, no links. */
    const QcomArGraphSpec spec = speaker_spec();
    const QcomArGraphModule module = {0x0700100Au, 0x7051u, 1u, 0u};
    uint8_t packet[512];
    const uint32_t n = qcom_ar_graph_open(packet, sizeof packet, 0x6000u, 0x11u, &spec, &module, 1u, 0, 0u);
    check(n > 0, "a device graph is written");
    check(le32(packet) >> 8 == n, "the header records the packet size");

    Block b[8];
    const unsigned count = walk(packet, n, b, 8);
    {
        GprPacket dbg;
        const int rc = gpr_decode(packet, n, &dbg);
        fprintf(stderr, "n=%u rc=%d size=%u payload=%u hdr=%u first_word=0x%08x cmd_size=%u\n", n, rc, dbg.size,
                dbg.payload_bytes, dbg.header_bytes, le32(packet), dbg.payload_bytes >= 16 ? le32(dbg.payload + 12) : 0);
        fprintf(stderr, "count=%u ids:", count);
        for (unsigned i = 0; i < count; ++i) fprintf(stderr, " %x/%u", b[i].id, b[i].size);
        fprintf(stderr, "\n");
    }
    /* sheng always writes the connection block, with a count of zero when there is nothing to connect. */
    check(count == 5, "a graph without links still carries the connection block");
    check(b[0].id == QCOM_AR_PARAM_SG_CONFIG && b[1].id == QCOM_AR_PARAM_CONTAINER_CONFIG &&
          b[2].id == QCOM_AR_PARAM_MODULE_LIST && b[3].id == QCOM_AR_PARAM_MODULE_PROP,
          "the blocks come in the framework's order");
    check(b[0].iid == spec.apm_iid && b[3].iid == spec.apm_iid, "every block is addressed to the APM instance");

    GprPacket p;
    (void)gpr_decode(packet, n, &p);
    uint32_t value = 0;
    check(property(p.payload, &b[0], 12u, QCOM_AR_PROP_SG_PERF, &value, 0) && value == spec.performance,
          "the subgraph carries its performance");
    check(property(p.payload, &b[0], 12u, QCOM_AR_PROP_SG_DIRECTION, &value, 0) && value == spec.direction,
          "the subgraph carries its direction");
    check(property(p.payload, &b[0], 12u, QCOM_AR_PROP_SG_SCENARIO, &value, 0) && value == spec.scenario,
          "the subgraph carries its scenario");
    check(property(p.payload, &b[1], 12u, QCOM_AR_PROP_CONT_POS, &value, 0) && value == spec.container_position,
          "the container carries its position");
    check(property(p.payload, &b[1], 12u, QCOM_AR_PROP_CONT_STACK, &value, 0) && value == spec.container_stack_bytes,
          "the container carries its stack");
    check(property(p.payload, &b[1], 12u, QCOM_AR_PROP_CONT_DOMAIN, &value, 0) && value == spec.container_domain,
          "the container carries its domain");
    uint32_t caps_size = 0;
    check(property(p.payload, &b[1], 12u, QCOM_AR_PROP_CONT_CAPS, &value, &caps_size) && value == 1u && caps_size == 8u,
          "the container is a global container");
    check(le32(p.payload + b[0].offset + 4) == spec.subgraph_id, "the subgraph id comes from the caller");
    check(le32(p.payload + b[2].offset + 12) == 1u, "the module list has one module");
    check(le32(p.payload + b[2].offset + 16) == module.module && le32(p.payload + b[2].offset + 20) == module.instance,
          "the module and its instance are listed");
    check(le32(p.payload + b[2].offset) == 1u && le32(p.payload + b[2].offset + 4) == spec.subgraph_id &&
          le32(p.payload + b[2].offset + 8) == spec.container_id,
          "the module list names the subgraph and the container it belongs to");
    /* The module-property block is {count} then, per module, {instance, property_count}, then the properties. */
    check(property(p.payload, &b[3], 12u, QCOM_AR_PROP_PORT_INFO, &value, 0), "the module has port information");
    uint32_t ports = 0;
    check(property(p.payload, &b[3], 12u, QCOM_AR_PROP_PORT_INFO, &value, &ports) && ports == 8u,
          "the port information declares eight bytes, one input and one output");
    check(b[4].id == QCOM_AR_PARAM_MODULE_CONN && le32(p.payload + b[4].offset) == 0u,
          "a device graph's connection block says there are none");
}

static void test_stream_graph_connection(void)
{
    /* A stream graph: three modules, and the last link reaches into the device graph's instance. That link is how the
     * stream graph joins the endpoint it plays through, which is the mechanism sheng uses. */
    const QcomArGraphSpec spec = speaker_spec();
    const QcomArGraphModule modules[3] = {
        {0x07001000u, 0x6001u, 0u, 1u},
        {0x07001005u, 0x6002u, 1u, 1u},
        {0x07001003u, 0x6003u, 1u, 1u},
    };
    const QcomArGraphLink links[3] = {{0x6001u, 0x6002u}, {0x6002u, 0x6003u}, {0x6003u, 0x7051u}};
    uint8_t packet[512];
    const uint32_t n = qcom_ar_graph_open(packet, sizeof packet, 0x6000u, 0x12u, &spec, modules, 3u, links, 3u);
    check(n > 0, "a stream graph with a connection is written");

    Block b[8];
    const unsigned count = walk(packet, n, b, 8);
    check(count == 5, "a graph with links adds the connection block");
    check(b[4].id == QCOM_AR_PARAM_MODULE_CONN, "the connection block comes last");

    GprPacket p;
    (void)gpr_decode(packet, n, &p);
    check(le32(p.payload + b[4].offset) == 3u, "three connections are recorded");
    const uint32_t at = b[4].offset + 4;
    check(le32(p.payload + at) == 0x6001u && le32(p.payload + at + 4) == QCOM_AR_OUT_PORT &&
          le32(p.payload + at + 8) == 0x6002u && le32(p.payload + at + 12) == QCOM_AR_IN_PORT,
          "a connection joins an output port to an input port");
    /* Each connection is four words: from, its output port, to, its input port. */
    const uint32_t last = at + 16u * 2u;
    check(le32(p.payload + last + 8) == 0x7051u,
          "the last connection reaches into the device graph's endpoint instance");
}

static void test_refusals(void)
{
    const QcomArGraphSpec spec = speaker_spec();
    const QcomArGraphModule module = {0x0700100Au, 0x7051u, 1u, 0u};
    uint8_t small[64];
    check(qcom_ar_graph_open(small, sizeof small, 0x6000u, 1u, &spec, &module, 1u, 0, 0u) == 0,
          "a packet that does not fit is refused rather than truncated");
    uint8_t packet[512];
    check(qcom_ar_graph_open(packet, sizeof packet, 0x6000u, 1u, &spec, 0, 0u, 0, 0u) == 0,
          "a graph without modules is refused");
    check(qcom_ar_graph_open(0, sizeof packet, 0x6000u, 1u, &spec, &module, 1u, 0, 0u) == 0,
          "a null buffer is refused");
    /* The intent bounds the graph: seven modules fit, eight do not, and the builder refuses rather than truncating. */
    QcomArGraphModule many[8];
    for (unsigned i = 0; i < 8; ++i) many[i] = (QcomArGraphModule){0x07001000u + i, 0x6100u + i, 1u, 1u};
    const uint32_t seven = qcom_ar_graph_open(packet, sizeof packet, 0x6000u, 1u, &spec, many, 7u, 0, 0u);
    check(seven > 0 && seven <= GPR_MAX_PACKET, "seven modules stay inside one GLINK intent");
    check(qcom_ar_graph_open(packet, sizeof packet, 0x6000u, 1u, &spec, many, 8u, 0, 0u) == 0,
          "eight modules exceed the intent and are refused");
}

int main(void)
{
    test_device_graph();
    test_stream_graph_connection();
    test_refusals();
    printf("PASS QcomAudioGraph: %d checks\n", checks);
    return failed ? 1 : 0;
}
