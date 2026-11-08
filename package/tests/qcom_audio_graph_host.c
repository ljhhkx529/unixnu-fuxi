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

/* The same walk for the resource manager's commands, whose parameter blocks are exact-sized rather than padded to
 * eight bytes: the reference client does not pad those, so requiring alignment there would be wrong. */
static unsigned walk_exact(const uint8_t *packet, uint32_t length, Block *blocks, unsigned max)
{
    GprPacket p;
    if (gpr_decode(packet, length, &p) != GprOk || p.size != length) return 0;
    if (p.payload_bytes < 16 || le32(p.payload + 12) != p.payload_bytes - 16) return 0;
    unsigned count = 0;
    for (uint32_t at = 16; at < p.payload_bytes;) {
        if (at + 16 > p.payload_bytes || count >= max) return 0;
        const uint32_t size = le32(p.payload + at + 8);
        if (at + 16 + size > p.payload_bytes) return 0;
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

static QcomArFormat format(void)
{
    QcomArFormat f = {48000u, 16u, 2u};
    return f;
}

/* The commands that configure a path once its graph is open. */
static void test_configuration(void)
{
    const QcomArFormat f = format();
    const QcomArI2sInterface intf = {0u, 1u, 2u, 1u};
    uint8_t packet[512];

    /* The hardware endpoint: its interface, its format, and the frame-size factor. */
    uint32_t n = qcom_ar_config_hw_endpoint(packet, sizeof packet, 0x6000u, 0x21u, 1u, 0x7051u, &intf, &f);
    check(n > 0, "a hardware endpoint configuration is written");
    Block b[8];
    unsigned count = walk(packet, n, b, 8);
    check(count == 3, "it carries the interface, the format and the frame-size factor");
    check(b[0].id == QCOM_AR_PARAM_I2S_INTF && b[1].id == QCOM_AR_PARAM_HW_EP_MF && b[2].id == QCOM_AR_PARAM_FRAME_SIZE_FACTOR,
          "the blocks come in the framework's order");
    check(b[0].iid == 0x7051u && b[1].iid == 0x7051u, "every block is addressed to the endpoint instance");
    GprPacket g;
    (void)gpr_decode(packet, n, &g);
    check(le32(g.payload + b[0].offset) == intf.lpaif && le32(g.payload + b[0].offset + 4) == intf.interface_type,
          "the interface carries the port and the interface type");
    check(le32(g.payload + b[0].offset + 8) == (intf.sd_line | (intf.ws_source << 16)),
          "the data line and the word-select source follow");
    check(le32(g.payload + b[1].offset) == f.rate &&
          le32(g.payload + b[1].offset + 4) == ((uint32_t)f.bits | ((uint32_t)f.channels << 16)),
          "the format carries the rate, the width and the channels");

    /* One converter's output format. */
    n = qcom_ar_config_pcm_output(packet, sizeof packet, 0x6000u, 0x22u, 1u, 0x6003u, &f, QCOM_AR_DEINTERLEAVED_UNPACKED);
    count = walk(packet, n, b, 8);
    check(count == 1 && b[0].id == QCOM_AR_PARAM_PCM_OUTPUT && b[0].iid == 0x6003u,
          "a converter's output format is one block at the converter's instance");
    (void)gpr_decode(packet, n, &g);
    check(le32(g.payload + b[0].offset + 4) == QCOM_AR_MEDIA_PCM, "the output media is linear PCM");
    check(le32(g.payload + b[0].offset) == 1u, "the block describes one output");

    /* The shared memory endpoint's own format is sent to that endpoint, not to the APM. */
    n = qcom_ar_config_media_format(packet, sizeof packet, 0x6000u, 0x23u, 0x6001u, &f);
    count = walk(packet, n, b, 8);
    check(count == 1 && b[0].id == QCOM_AR_PARAM_MEDIA_FORMAT && b[0].iid == 0x6001u,
          "the endpoint format is addressed to the endpoint instance");
    (void)gpr_decode(packet, n, &g);
    check(g.dst_port == 0x6001u, "and sent to that endpoint's port");

    /* An empty buffer for the endpoint, and the graph commands. */
    n = qcom_ar_write(packet, sizeof packet, 0x6000u, 0x24u, 0x6001u, 0x8f000000ull, 0x77u, 480u);
    (void)gpr_decode(packet, n, &g);
    check(n > 0 && g.opcode == QCOM_AR_OP_WRITE && g.dst_port == 0x6001u, "a buffer goes to the endpoint port");
    check(le32(g.payload) == 0x8f000000u && le32(g.payload + 8) == 0x77u && le32(g.payload + 12) == 480u,
          "the buffer carries its address, its handle and its length");

    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t opcodes[3] = {QCOM_AR_OP_GRAPH_START, QCOM_AR_OP_GRAPH_STOP, QCOM_AR_OP_GRAPH_CLOSE};
        n = qcom_ar_graph_command(packet, sizeof packet, 0x6000u, 0x30u + i, 1u, opcodes[i], 0x5100u);
        count = walk(packet, n, b, 8);
        (void)gpr_decode(packet, n, &g);
        check(n > 0 && g.opcode == opcodes[i] && g.dst_port == 1u && count == 1 && b[0].id == QCOM_AR_PARAM_SG_LIST,
              "a graph command names its subgraph and goes to the APM");
        check(le32(g.payload + b[0].offset + 4) == 0x5100u, "the subgraph id comes from the caller");
    }

    /* Mapping a region: the size is rounded up to a whole page. */
    n = qcom_ar_map(packet, sizeof packet, 0x6000u, 0x40u, 1u, 0x8f000000ull, 5000u);
    (void)gpr_decode(packet, n, &g);
    check(n > 0 && g.opcode == QCOM_AR_OP_MEM_MAP, "a buffer map is written");
    check(le32(g.payload + 8) == 0x8f000000u && le32(g.payload + 16) == 8192u,
          "the map carries the address and a size rounded to whole pages");

    /* The resource manager: a clock, and a block vote. */
    n = qcom_ar_prm_clock(packet, sizeof packet, 0x6000u, 0x41u, 0x30cu, 19200000u);
    (void)gpr_decode(packet, n, &g);
    check(n > 0 && g.opcode == QCOM_AR_OP_PRM_REQUEST && g.dst_port == QCOM_AR_PRM_IID,
          "a clock request goes to the resource manager");
    count = walk_exact(packet, n, b, 8);
    /* The body is {count, clock, frequency, attribute, default root}: twenty bytes, not padded to twenty-four. */
    check(count == 1 && b[0].id == QCOM_AR_PARAM_HW_CLOCK && b[0].size == 20u,
          "the clock block is exact-sized rather than padded");
    check(walk(packet, n, b, 8) == 0, "it is deliberately not eight-byte padded");
    check(le32(g.payload + b[0].offset) == 1u && le32(g.payload + b[0].offset + 4) == 0x30cu,
          "the clock block names one clock");
    check(le32(g.payload + b[0].offset + 8) == 19200000u, "the clock block carries the frequency");

    n = qcom_ar_prm_vote(packet, sizeof packet, 0x6000u, 0x42u, 2u, 1);
    (void)gpr_decode(packet, n, &g);
    count = walk_exact(packet, n, b, 8);
    check(n > 0 && count == 1 && b[0].id == QCOM_AR_PARAM_HW_CORE && le32(g.payload + b[0].offset) == 2u,
          "a digital codec vote names its block");
    n = qcom_ar_prm_vote(packet, sizeof packet, 0x6000u, 0x43u, 2u, 0);
    (void)gpr_decode(packet, n, &g);
    check(g.opcode == QCOM_AR_OP_PRM_RELEASE, "releasing the vote is a release command");
}

/* What comes back. */
static void test_answers(void)
{
    uint8_t packet[64];
    QcomArAnswer a;

    GprPacket p;
    memset(&p, 0, sizeof p);
    p.dst_domain = GPR_DOMAIN_APPS;
    p.src_domain = GPR_DOMAIN_ADSP;
    p.src_port = 1u;
    p.token = 9u;
    p.opcode = GPR_OP_BASIC_RESULT;
    const uint8_t result[8] = {0x06, 0x10, 0, 0, 0, 0, 0, 0};
    (void)gpr_encode(packet, sizeof packet, &p, result, sizeof result);
    check(qcom_ar_parse(packet, 24u + 8u, &a) == QcomArAnswerBasicResult, "a basic result is recognised");
    check(a.status == 0 && a.result_opcode == 0x00001006u && a.token == 9u, "it carries the opcode and the status");

    p.opcode = QCOM_AR_OP_MEM_MAP_DONE;
    const uint8_t handle[4] = {0x77, 0, 0, 0};
    (void)gpr_encode(packet, sizeof packet, &p, handle, sizeof handle);
    check(qcom_ar_parse(packet, 24u + 4u, &a) == QcomArAnswerMapped && a.handle == 0x77u, "a map answer carries the handle");

    p.opcode = QCOM_AR_OP_WRITE_DONE;
    uint8_t done[16] = {0x00, 0x00, 0x00, 0x8f, 0, 0, 0, 0, 0x77, 0, 0, 0, 0, 0, 0, 0};
    (void)gpr_encode(packet, sizeof packet, &p, done, sizeof done);
    check(qcom_ar_parse(packet, 24u + 16u, &a) == QcomArAnswerWriteDone && a.address == 0x8f000000ull &&
          a.handle == 0x77u, "a write answer repeats the address and the handle");

    p.opcode = QCOM_AR_OP_READ_DONE;
    uint8_t read_done[24] = {0, 0, 0, 0, 0x00, 0x00, 0x00, 0x8f, 0, 0, 0, 0, 0x77, 0, 0, 0, 0x40, 0, 0, 0, 0x10, 0, 0, 0};
    (void)gpr_encode(packet, sizeof packet, &p, read_done, sizeof read_done);
    check(qcom_ar_parse(packet, 24u + 24u, &a) == QcomArAnswerReadDone && a.data_bytes == 0x40u && a.offset == 0x10u,
          "a read answer carries the length and the offset");

    p.opcode = 0x0fffffffu;
    const uint8_t none[4] = {0};
    (void)gpr_encode(packet, sizeof packet, &p, none, sizeof none);
    check(qcom_ar_parse(packet, 24u + 4u, &a) == QcomArAnswerUnknown, "an unknown command is reported as unknown");
    check(qcom_ar_parse(packet, 8u, &a) == QcomArAnswerBad, "a packet too short to hold a header is refused");
    check(qcom_ar_parse(0, 24u, &a) == QcomArAnswerBad, "a null packet is refused");
}

int main(void)
{
    test_device_graph();
    test_stream_graph_connection();
    test_configuration();
    test_answers();
    test_refusals();
    printf("PASS QcomAudioGraph: %d checks\n", checks);
    return failed ? 1 : 0;
}
