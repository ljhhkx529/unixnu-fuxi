/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Fuzz the parsers that will face real firmware and DSP bytes.
 *
 * Everything here decodes a byte stream it did not produce: the firmware container, the GPR packet, the APM answer and
 * the GLINK wire. A decoder that reads one byte past a short packet is a fault that only appears on the device, with a
 * firmware build nobody can reproduce locally - so it is worth looking for here, where ASan and UBSan turn the read
 * into a report and the seed is fixed so any finding reproduces.
 *
 * Two generators: pure noise, which finds the length checks, and a well-formed header with a noisy body, which gets past
 * the first checks and reaches the fields behind them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../QcomAudioGraph.h"
#include "../QcomGlink.h"
#include "../QcomMdt.h"
#include "../ShengGpr.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static uint32_t seed = 0x1234567u;

static uint32_t next_random(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* The container's reader over a plain buffer: the parser must never ask past the end, and if it did, the read hook
 * would be the one to notice. */
typedef struct {
    const uint8_t *bytes;
    uint64_t size;
    uint64_t requests;
    int overran;
} Buffer;

static int read_buffer(void *context, uint64_t offset, void *to, size_t length)
{
    Buffer *b = context;
    ++b->requests;
    if (offset > b->size || length > b->size - offset) {b->overran = 1; return -1;}
    memcpy(to, b->bytes + offset, length);
    return 0;
}

#define BUFFER_BYTES 1024u
static uint8_t buffer[BUFFER_BYTES];

static void fill_noise(uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; ++i) buffer[i] = (uint8_t)next_random();
}

/* A GPR header that passes the first checks, with a random payload behind it. */
static void fill_gpr_header(uint32_t payload_bytes)
{
    fill_noise(24u + payload_bytes);
    const uint32_t size = 24u + payload_bytes;
    const uint32_t word = (GPR_VERSION & 0xfu) | (GPR_HEADER_BYTES / 4) << 4 | size << 8;
    buffer[0] = (uint8_t)word; buffer[1] = (uint8_t)(word >> 8);
    buffer[2] = (uint8_t)(word >> 16); buffer[3] = (uint8_t)(word >> 24);
    buffer[4] = (uint8_t)(GPR_DOMAIN_ADSP | GPR_DOMAIN_APPS << 8);
    buffer[16] = (uint8_t)(GPR_OP_BASIC_RESULT);          /* a plausible opcode */
    buffer[20] = (uint8_t)(QCOM_AR_OP_WRITE_DONE & 0xffu);
    buffer[21] = (uint8_t)((QCOM_AR_OP_WRITE_DONE >> 8) & 0xffu);
}

static void fuzz_mdt(uint32_t iterations)
{
    for (uint32_t i = 0; i < iterations; ++i) {
        Buffer b = {buffer, 0, 0, 0};
        b.size = 1u + next_random() % BUFFER_BYTES;
        if (i % 2u) {
            /* A plausible ELF header, so the parser gets past the magic and into the program headers. */
            fill_noise((uint32_t)b.size);
            if (b.size >= 64u) {
                buffer[0] = 0x7f; buffer[1] = 'E'; buffer[2] = 'L'; buffer[3] = 'F';
                buffer[4] = 2; buffer[5] = 1;
            }
        } else {
            fill_noise((uint32_t)b.size);
        }
        QcomMdtImage image;
        const int rc = qcom_mdt_parse(&image, b.size, read_buffer, &b);
        check(!b.overran, "the container parser never reads past the buffer");
        if (rc == QcomMdtOk) {
            const uint64_t span = qcom_mdt_span(&image);
            check(span <= 0x10000000ull, "a parsed image has a plausible span");
            for (unsigned s = 0; s < image.count; ++s) {
                uint64_t at = 0;
                (void)qcom_mdt_place(&image, s, 0, span ? span : 1u, &at);
                check(!b.overran, "placing a segment reads only what the image declares");
            }
        }
    }
}

static void fuzz_gpr(uint32_t iterations)
{
    for (uint32_t i = 0; i < iterations; ++i) {
        const uint32_t length = next_random() % BUFFER_BYTES;
        if (i % 2u) fill_gpr_header(length > 24u ? length - 24u : 0u);
        else fill_noise(length);

        GprPacket packet;
        const int rc = gpr_decode(buffer, length, &packet);
        if (rc == GprOk) {
            check(packet.size <= length, "a decoded packet does not claim more bytes than it was given");
            check(packet.header_bytes >= GPR_HEADER_BYTES, "and its header is at least the fixed one");
            check(packet.payload_bytes <= length - GPR_HEADER_BYTES, "and its payload fits inside it");
        }

        QcomArAnswer answer;
        const int kind = qcom_ar_parse(buffer, length, &answer);
        check(kind <= QcomArAnswerReadDone && kind >= QcomArAnswerBad, "the answer parser returns one of its kinds");
    }
}

static void fuzz_glink(uint32_t iterations)
{
    static uint32_t h_rx, t_rx, h_tx, t_tx;
    static uint8_t rx[512], tx[512];
    for (uint32_t i = 0; i < iterations; ++i) {
        h_rx = t_rx = h_tx = t_tx = 0;
        const QcomGlinkFifo in = {&h_rx, &t_rx, rx, sizeof rx};
        const QcomGlinkFifo out = {&h_tx, &t_tx, tx, sizeof tx};
        QcomGlink link;
        qcom_glink_init(&link, out, in, 0, 0);

        /* Random bytes straight into the receive FIFO: the wire is what an attacker or a confused DSP controls. */
        const uint32_t bytes = next_random() % (sizeof rx - 8u);
        for (uint32_t k = 0; k < bytes; ++k) rx[k] = (uint8_t)next_random();
        h_rx = bytes;
        const int rc = qcom_glink_poll(&link);
        check(rc == QcomGlinkOk || rc == QcomGlinkCorrupt || rc == QcomGlinkAgain,
              "the link classifies nonsense instead of failing silently");
    }
}

int main(int argc, char **argv)
{
    const uint32_t iterations = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 10) : 4000u;
    fuzz_mdt(iterations);
    fuzz_gpr(iterations);
    fuzz_glink(iterations);
    printf("PASS QcomAudioFuzz: %d checks over %u iterations per parser\n", checks, iterations);
    return failed ? 1 : 0;
}
