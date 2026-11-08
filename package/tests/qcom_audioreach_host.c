/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the board-independent AudioReach payload builder and answer parser.
 *
 * The point of these checks is that the builder carries no board values: every id in the output comes from the
 * topology passed in, and two different topologies produce two different packets with the same code. */
#include <stdio.h>
#include <string.h>

#include "../QcomAudioReach.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Finds a little-endian word at any four-byte-aligned offset. */
static int has_word(const uint8_t *b, uint32_t n, uint32_t value)
{
    for (uint32_t i = 0; i + 4u <= n; i += 4u) if (le32(b + i) == value) return 1;
    return 0;
}

static int has_word_any(const uint8_t *b, uint32_t n, uint32_t value)
{
    for (uint32_t i = 0; i + 4u <= n; ++i) if (le32(b + i) == value) return 1;
    return 0;
}

/* A single-subgraph graph, as the topology input the plan requires. */
static const QcomArModule modules_a[] = {
    {0x101u, QCOM_AR_MOD_SHARED_MEMORY, 0, 1},
    {0x102u, QCOM_AR_MOD_PCM_CONVERT, 1, 1},
    {0x103u, QCOM_AR_MOD_I2S_SINK, 1, 0},
};
static const QcomArLink links_a[] = {
    {0x101u, 1, 0x102u, 2},
    {0x102u, 1, 0x103u, 2},
};
static const QcomArGraph graph_a = {
    0x11110001u, 2, QCOM_AR_DIR_RX, QCOM_AR_SCENARIO_PLAYBACK,
    0x22220001u, QCOM_AR_POSITION_STREAM, 8192, QCOM_GPR_DOMAIN_ADSP,
    modules_a, 3, links_a, 2,
};

/* The same shape with every board value different, to show nothing is baked in. */
static const QcomArModule modules_b[] = {
    {0x201u, QCOM_AR_MOD_SHARED_MEMORY, 0, 1},
    {0x202u, QCOM_AR_MOD_I2S_SINK, 1, 0},
};
static const QcomArLink links_b[] = {{0x201u, 1, 0x202u, 2}};
static const QcomArGraph graph_b = {
    0x33330001u, 1, QCOM_AR_DIR_TX, QCOM_AR_SCENARIO_RECORD,
    0x44440001u, QCOM_AR_POSITION_STREAM, 4096, QCOM_GPR_DOMAIN_ADSP,
    modules_b, 2, links_b, 1,
};

static const QcomArFormat format_pcm = {
    QCOM_AR_MEDIA_PCM, 16, 2, QCOM_AR_INTERLEAVED, 4,
};

int main(void)
{
    uint8_t a[512], b[512], again[512], exact[512], tight[512], small[64];
    uint32_t na, nb;

    /* Graph open: two topologies, nothing shared but the protocol. */
    na = qcom_ar_open_graph(a, sizeof a, &graph_a);
    nb = qcom_ar_open_graph(b, sizeof b, &graph_b);
    check(na > 0 && nb > 0, "graph open builds");
    check(na % 8u == 0u && nb % 8u == 0u, "graph open is eight-byte aligned");
    check(le32(a + 12) == na - 16u, "graph open payload size matches the packet");
    check(has_word(a, na, graph_a.graph_id), "graph open carries its graph id");
    check(has_word(a, na, graph_a.container_id), "graph open carries its container id");
    for (unsigned i = 0; i < graph_a.module_count; ++i) {
        check(has_word(a, na, modules_a[i].instance), "graph open carries every module instance");
        check(has_word(a, na, modules_a[i].module), "graph open carries every module type");
    }
    for (unsigned i = 0; i < graph_a.link_count; ++i) {
        check(has_word(a, na, links_a[i].from_instance) && has_word(a, na, links_a[i].to_instance),
              "graph open carries every link");
    }
    check(na != nb || memcmp(a, b, na) != 0, "two topologies produce two packets");
    check(has_word(b, nb, graph_b.graph_id) && has_word(b, nb, graph_b.container_id),
          "the second topology carries its own ids");
    /* The board values sheng bakes in must not appear anywhere in our output. */
    check(!has_word_any(a, na, 0x4005u) && !has_word_any(a, na, 0x4007u), "no sheng graph or endpoint ids");
    check(!has_word_any(a, na, 0x30cu) && !has_word_any(a, na, 0x400u), "no sheng clock or interface constants");
    check(!has_word_any(b, nb, 0x4005u) && !has_word_any(b, nb, 0x4007u), "no sheng constants in the other topology");

    /* Determinism: the same input twice is the same bytes. */
    check(qcom_ar_open_graph(again, sizeof again, &graph_a) == na && memcmp(a, again, na) == 0,
          "graph open is deterministic");

    /* An exactly sized buffer fits, and one that is eight bytes short is refused rather than truncated. */
    check(qcom_ar_open_graph(exact, na, &graph_a) == na, "graph open fits an exactly sized buffer");
    check(qcom_ar_open_graph(tight, na - 8u, &graph_a) == 0, "graph open refuses a buffer eight bytes short");
    check(qcom_ar_open_graph(small, sizeof small, &graph_a) == 0, "graph open refuses a small buffer");

    /* Out-of-range topology inputs are refused. */
    QcomArGraph too_many = graph_a;
    QcomArModule nine[QCOM_AR_MAX_MODULES + 1];
    for (unsigned i = 0; i < QCOM_AR_MAX_MODULES + 1; ++i) nine[i] = modules_a[0];
    too_many.modules = nine; too_many.module_count = QCOM_AR_MAX_MODULES + 1;
    check(qcom_ar_open_graph(a, sizeof a, &too_many) == 0, "more modules than the cap is refused");
    QcomArGraph no_modules = graph_a; no_modules.module_count = 0;
    check(qcom_ar_open_graph(a, sizeof a, &no_modules) == 0, "an empty graph is refused");
    QcomArGraph no_link_array = graph_a; no_link_array.links = 0;
    check(qcom_ar_open_graph(a, sizeof a, &no_link_array) == 0, "links without an array are refused");

    /* Graph start and stop: only the subgraph id travels. */
    na = qcom_ar_graph_command(a, sizeof a, QCOM_AR_APM_INSTANCE, graph_a.graph_id);
    check(na > 0 && na % 8u == 0u, "graph command builds and is aligned");
    check(has_word(a, na, graph_a.graph_id), "graph command carries the subgraph id");
    check(has_word(a, na, QCOM_AR_PARAM_SG_LIST), "graph command carries the subgraph list parameter");
    check(qcom_ar_graph_command(small, 4, QCOM_AR_APM_INSTANCE, graph_a.graph_id) == 0,
          "graph command refuses a short buffer");

    /* I2S endpoint: interface values and format all come from the caller. */
    QcomArEndpoint i2s = {0x103u, 0x103u, QCOM_AR_INTERFACE_I2S, 0x11u, 1u, 3u, 2u, 0};
    na = qcom_ar_configure_endpoint(a, sizeof a, &i2s, &format_pcm);
    check(na > 0 && na % 8u == 0u, "I2S endpoint builds and is aligned");
    check(has_word(a, na, 0x11u), "I2S endpoint carries its interface index");
    check(has_word(a, na, format_pcm.bit_width) && has_word(a, na, format_pcm.channels),
          "I2S endpoint carries the format");
    check(has_word_any(a, na, (3u << 0) | (2u << 16)), "I2S endpoint carries the line and word-select source");
    check(has_word(a, na, format_pcm.bytes_per_frame), "I2S endpoint carries the frame size factor");

    /* Codec DMA endpoint: the interface index and the slot mask. */
    QcomArEndpoint dma = {0x104u, 0x104u, QCOM_AR_INTERFACE_CODEC_DMA, 0x55u, 0, 0, 0, 0x0ff0u};
    na = qcom_ar_configure_endpoint(a, sizeof a, &dma, &format_pcm);
    check(na > 0 && na % 8u == 0u, "codec DMA endpoint builds and is aligned");
    check(has_word(a, na, 0x55u), "codec DMA endpoint carries its interface index");
    check(has_word(a, na, 0x0ff0u), "codec DMA endpoint carries its slot mask");

    QcomArEndpoint bad_kind = dma; bad_kind.interface_type = 9u;
    check(qcom_ar_configure_endpoint(a, sizeof a, &bad_kind, &format_pcm) == 0,
          "an unknown interface kind is refused");

    /* Stream configuration: media format and packing. */
    na = qcom_ar_configure_stream(a, sizeof a, 0x102u, &format_pcm);
    check(na > 0 && na % 8u == 0u, "stream configuration builds and is aligned");
    check(has_word(a, na, 0x102u), "stream configuration carries its module instance");
    check(has_word(a, na, QCOM_AR_MEDIA_PCM), "stream configuration carries the media id");
    check(has_word(a, na, QCOM_AR_INTERLEAVED), "stream configuration carries the packing");
    QcomArFormat no_channels = format_pcm; no_channels.channels = 0;
    check(qcom_ar_configure_stream(a, sizeof a, 0x102u, &no_channels) == 0, "zero channels is refused");
    QcomArFormat narrow = format_pcm; narrow.bit_width = 4;
    check(qcom_ar_configure_stream(a, sizeof a, 0x102u, &narrow) == 0, "a width below eight bits is refused");

    /* Clock vote: id and rate are the board's. */
    na = qcom_ar_prm_clock(a, sizeof a, QCOM_AR_APM_INSTANCE, 0x77u, 19200000u);
    check(na > 0 && na % 8u == 0u, "clock vote builds and is aligned");
    check(has_word(a, na, 0x77u) && has_word(a, na, 19200000u), "clock vote carries the id and the rate");

    /* Buffer mapping: address, rounded length, and the handle on the way back. */
    na = qcom_ar_map(a, sizeof a, 0x101u, 0x8f000000ull, 0x3000u);
    check(na > 0 && na % 8u == 0u, "map builds and is aligned");
    check(has_word(a, na, 0x8f000000u) && has_word(a, na, 0u), "map carries the low and high address words");
    check(has_word(a, na, 0x3000u), "map carries the rounded length");
    check(qcom_ar_map(a, sizeof a, 0x101u, 0x8f000000ull, 0) == 0, "a zero-length map is refused");
    na = qcom_ar_unmap(a, sizeof a, 0x101u, 0x1234u);
    check(na > 0 && na % 8u == 0u && has_word(a, na, 0x1234u), "unmap carries the handle");

    /* Answers: each opcode has its own shape, and a short payload is refused. */
    uint8_t payload[16] = {0};
    QcomGprPacket packet;
    QcomArAnswer answer;
    memset(&packet, 0, sizeof packet);
    packet.opcode = QCOM_GPR_OP_BASIC_RESULT;
    payload[0] = 0x00; payload[1] = 0x10; payload[2] = 0x00; payload[3] = 0x01;   /* AR_OP_GRAPH_OPEN */
    payload[7] = 0x00;                                                              /* status 0 */
    packet.payload = payload; packet.payload_bytes = 8;
    check(qcom_ar_parse(&packet, &answer) == QcomArAnswerBasicResult, "basic result parses");
    check(answer.result_opcode == QCOM_AR_OP_GRAPH_OPEN && answer.status == 0,
          "basic result carries the command and its status");

    packet.opcode = QCOM_AR_OP_MEM_MAP_DONE;
    payload[0] = 0xcd; payload[1] = 0xab; payload[2] = 0; payload[3] = 0;
    packet.payload_bytes = 4;
    check(qcom_ar_parse(&packet, &answer) == QcomArAnswerMapped && answer.handle == 0xabcdu,
          "map done parses and carries the handle");

    packet.opcode = QCOM_GPR_OP_APM_SPF_STATE;
    memset(payload, 0, sizeof payload);
    payload[0] = 1;
    check(qcom_ar_parse(&packet, &answer) == QcomArAnswerFrameworkState && answer.status == 1,
          "framework state parses");

    packet.opcode = QCOM_GPR_OP_BASIC_RESULT; packet.payload_bytes = 4;
    check(qcom_ar_parse(&packet, &answer) == QcomArAnswerBad, "a short basic result is refused");
    packet.opcode = 0x03000000u; packet.payload_bytes = 8;
    check(qcom_ar_parse(&packet, &answer) == QcomArAnswerUnknown, "an unmodelled opcode is reported, not guessed");

    printf("PASS QcomAudioReach: %d checks\n", checks);
    return failed ? 1 : 0;
}
