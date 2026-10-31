// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of FtsProtocol.c against a model of the controller's SPI side: framing, dummy byte, FIFO draining, reset
// and its fallback, sensing commands.
#include "../FtsProtocol.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ---- controller model ----
static struct {
    uint8_t id[2];
    uint8_t fifo[64][8];
    unsigned head, tail;
    int reset_level, resets, ready_after_polls, ready_pending, polls_since_reset, firmware;
    uint8_t icr, soft_reset;
    unsigned delays_ms, xfers;
    uint8_t log[16][8];
    size_t log_len[16];
    unsigned log_n;
    int fail_xfer;
} m;

static void push(const uint8_t ev[8]) {memcpy(m.fifo[m.tail++ % 64], ev, 8);}
static void model_reset(void) {
    memset(&m, 0, sizeof m);
    m.id[0] = 0x36; m.id[1] = 0x40;
    m.reset_level = 1;
    m.firmware = 1;
    m.ready_after_polls = 3;
}
static void chip_reset(void) {
    m.head = m.tail = 0;
    ++m.resets;
    m.ready_pending = m.firmware;
    m.polls_since_reset = 0;
}

static uint32_t be32(const uint8_t *p) {return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];}

static int bus_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)ctx;
    ++m.xfers;
    if (m.fail_xfer) return -1;
    if (m.log_n < 16) {m.log_len[m.log_n] = len; memcpy(m.log[m.log_n++], tx, len < 8 ? len : 8);}
    memset(rx, 0xEE, len);                      // bytes clocked in during the command are junk
    switch (tx[0]) {
    case 0xFA: {
        uint32_t a = be32(tx + 1);
        if (a == 0x2000002Du) m.icr = tx[5];
        if (a == 0x20000024u && tx[5] == 0x81) {m.soft_reset = 1; chip_reset();}
        return 0;
    }
    case 0xFB: {
        uint32_t a = be32(tx + 1);
        rx[5] = 0xDD;                           // dummy
        for (size_t i = 6; i < len; ++i) rx[i] = (a == 0x20000000u && i - 6 < 2) ? m.id[i - 6] : 0;
        return 0;
    }
    case 0x87: {
        rx[1] = 0xDD;
        for (size_t i = 2; i + 8 <= len; i += 8) {
            if (m.ready_pending && ++m.polls_since_reset >= m.ready_after_polls) {
                static const uint8_t ready[8] = {0x03, 0, 0, 0, 0, 0, 0, 0};
                memcpy(rx + i, ready, 8);
                m.ready_pending = 0;
            } else if (m.head != m.tail) {
                memcpy(rx + i, m.fifo[m.head++ % 64], 8);
            } else {
                memset(rx + i, 0, 8);
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}
static void bus_delay(void *ctx, unsigned ms) {(void)ctx; m.delays_ms += ms;}
static void bus_reset(void *ctx, int level) {
    (void)ctx;
    if (m.reset_level == 0 && level == 1) chip_reset();
    m.reset_level = level;
}

int main(void) {
    static uint8_t tx[FTS_XFER_MAX], rx[FTS_XFER_MAX];
    struct fts_bus bus = {NULL, bus_xfer, bus_delay, bus_reset};
    struct fts_chip c;
    uint8_t buf[16];

    // Frames: FA 20 00 00 24 81 and FB 20 00 00 00.
    uint8_t v = 0x81;
    size_t n = fts_frame_hw_write(buf, FTS_ADDR_SYSTEM_RESET, &v, 1);
    const uint8_t want_w[] = {0xFA, 0x20, 0x00, 0x00, 0x24, 0x81};
    CHECK(n == sizeof want_w && memcmp(buf, want_w, n) == 0);
    n = fts_frame_hw_read(buf, FTS_ADDR_CHIP_ID);
    const uint8_t want_r[] = {0xFB, 0x20, 0x00, 0x00, 0x00};
    CHECK(n == sizeof want_r && memcmp(buf, want_r, n) == 0);

    // Chip id: one transfer of 5 + 1 + 2 bytes, the dummy byte skipped.
    model_reset();
    c = (struct fts_chip){&bus, tx, rx, {0}, {0}, 0, 0, 0, 0};
    CHECK(fts_read_chip_id(&c) == FTS_OK && c.id[0] == 0x36 && c.id[1] == 0x40);
    CHECK(m.log_n == 1 && m.log_len[0] == 8);
    m.id[1] = 0x41;
    CHECK(fts_read_chip_id(&c) == FTS_ERR_CHIP);

    // Reset with a GPIO: one pulse, ready after three polls, 10 ms low plus two poll waits.
    model_reset();
    CHECK(fts_reset(&c) == FTS_OK);
    CHECK(m.resets == 1 && c.reset_attempts == 1 && c.polls == 3 && !c.spi4_fallback);
    CHECK(m.delays_ms == FTS_RESET_LOW_MS + 2 * FTS_POLL_STEP_MS);
    CHECK(c.last_event[0] == FTS_EVT_CONTROLLER_READY);

    // Soft reset without a reset GPIO writes FA 20 00 00 24 81.
    model_reset();
    struct fts_bus soft = {NULL, bus_xfer, bus_delay, NULL};
    c.bus = &soft;
    CHECK(fts_reset(&c) == FTS_OK && m.soft_reset == 1);
    CHECK(m.log_len[0] == 6 && memcmp(m.log[0], want_w, 6) == 0);
    c.bus = &bus;

    // No firmware: three attempts of 50 polls, then 4-wire SPI selected and the id read: chip present, not ready.
    model_reset();
    m.firmware = 0;
    CHECK(fts_reset(&c) == FTS_ERR_TIMEOUT);
    CHECK(m.resets == 3 && c.spi4_fallback && m.icr == FTS_ICR_SPI4 && c.polls == FTS_POLL_TIMEOUT_MS / FTS_POLL_STEP_MS);
    // No chip at all (id reads 00 00).
    model_reset();
    m.firmware = 0; m.id[0] = m.id[1] = 0;
    CHECK(fts_reset(&c) == FTS_ERR_CHIP);
    // A bus failure stops at once.
    model_reset();
    m.fail_xfer = 1;
    CHECK(fts_reset(&c) == FTS_ERR_BUS && m.xfers == 1);

    // Sensing: A0 00 00, 50 ms, A0 00 01.
    model_reset();
    CHECK(fts_start_sensing(&c) == FTS_OK);
    CHECK(m.log_n == 2 && m.log_len[0] == 3 && m.log_len[1] == 3);
    CHECK(m.log[0][0] == 0xA0 && m.log[0][1] == 0x00 && m.log[0][2] == 0x00);
    CHECK(m.log[1][0] == 0xA0 && m.log[1][1] == 0x00 && m.log[1][2] == 0x01);
    CHECK(m.delays_ms == FTS_SENSE_OFF_WAIT_MS);

    // FIFO: the head event announces two more, read in one second transfer of 1 + 1 + 16 bytes.
    model_reset();
    m.ready_pending = 0;
    static uint8_t events[FTS_FIFO_DEPTH * FTS_EVENT_SIZE];
    unsigned count = 99;
    CHECK(fts_read_events(&c, events, &count) == FTS_OK && count == 0);
    const uint8_t e1[8] = {0x13, 0x01, 0x10, 0x27, 0x20, 0x4e, 0x33, 0x02};
    const uint8_t e2[8] = {0x23, 0x01, 0x11, 0x27, 0x21, 0x4e, 0x33, 0x01};
    const uint8_t e3[8] = {0x33, 0x01, 0x11, 0x27, 0x21, 0x4e, 0x00, 0x00};
    push(e1); push(e2); push(e3);
    m.log_n = 0;
    CHECK(fts_read_events(&c, events, &count) == FTS_OK && count == 3);
    CHECK(memcmp(events, e1, 8) == 0 && memcmp(events + 8, e2, 8) == 0 && memcmp(events + 16, e3, 8) == 0);
    CHECK(m.log_n == 2 && m.log_len[0] == 10 && m.log_len[1] == 18 && m.log[1][0] == 0x87);
    // A remaining count above 31 is clamped to the FIFO depth.
    const uint8_t big[8] = {0x13, 0x01, 0, 0, 0, 0, 0, 0x1F};
    push(big);
    CHECK(fts_read_events(&c, events, &count) == FTS_OK && count == FTS_FIFO_DEPTH);

    // A wait that never sees its event gives up after FTS_POLL_TIMEOUT_MS / FTS_POLL_STEP_MS reads.
    model_reset();
    m.ready_pending = 0;
    const uint8_t err[8] = {0xF3, 0x06, 0, 0, 0, 0, 0, 0};
    push(err);
    CHECK(fts_wait_event(&c, 0x43, FTS_POLL_TIMEOUT_MS) == FTS_ERR_TIMEOUT && c.polls == 50 && c.errors_seen == 1);

    // Parameter checks.
    CHECK(fts_read_fifo(&c, events, 0) == FTS_ERR_PARAM && fts_read_fifo(&c, events, FTS_FIFO_DEPTH + 1) == FTS_ERR_PARAM);
    CHECK(fts_hw_write(&c, 0, events, FTS_XFER_MAX) == FTS_ERR_PARAM);

    printf(failures ? "fts-protocol: %d failed\n" : "fts-protocol: all passed\n", failures);
    return failures != 0;
}
