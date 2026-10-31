// SPDX-License-Identifier: BSD-2-Clause-Patent
// FTM5 host protocol; see FtsProtocol.h for the sources of the facts. No allocation and no libc: callers are kernel code.
#include "FtsProtocol.h"

static void put_addr(uint8_t *out, uint32_t addr) {
    for (unsigned i = 0; i < FTS_HW_ADDR_BYTES; ++i) out[i] = (uint8_t)(addr >> (8 * (FTS_HW_ADDR_BYTES - 1 - i)));
}

size_t fts_frame_hw_write(uint8_t *out, uint32_t addr, const uint8_t *data, size_t len) {
    out[0] = FTS_OP_HW_REG_W;
    put_addr(out + 1, addr);
    for (size_t i = 0; i < len; ++i) out[1 + FTS_HW_ADDR_BYTES + i] = data[i];
    return 1 + FTS_HW_ADDR_BYTES + len;
}

size_t fts_frame_hw_read(uint8_t *out, uint32_t addr) {
    out[0] = FTS_OP_HW_REG_R;
    put_addr(out + 1, addr);
    return 1 + FTS_HW_ADDR_BYTES;
}

static int xfer(struct fts_chip *c, size_t len) {
    if (!c || !c->bus || !c->bus->xfer || !c->tx || !c->rx || len > FTS_XFER_MAX) return FTS_ERR_PARAM;
    return c->bus->xfer(c->bus->ctx, c->tx, c->rx, len) ? FTS_ERR_BUS : FTS_OK;
}

static void wait_ms(struct fts_chip *c, unsigned ms) {
    if (c->bus->delay_ms) c->bus->delay_ms(c->bus->ctx, ms);
}

// A read is one full-duplex transfer: the command goes out while the first bytes clocked in are ignored, then one
// dummy byte, then the data. The transmit side is zero after the command.
static int read_after(struct fts_chip *c, size_t cmd_len, uint8_t *out, size_t len) {
    const size_t total = cmd_len + FTS_DUMMY_BYTES + len;
    if (total > FTS_XFER_MAX) return FTS_ERR_PARAM;
    for (size_t i = cmd_len; i < total; ++i) c->tx[i] = 0;
    int r = xfer(c, total);
    if (r) return r;
    for (size_t i = 0; i < len; ++i) out[i] = c->rx[cmd_len + FTS_DUMMY_BYTES + i];
    return FTS_OK;
}

int fts_hw_write(struct fts_chip *c, uint32_t addr, const uint8_t *data, size_t len) {
    if (!c || !c->tx || 1 + FTS_HW_ADDR_BYTES + len > FTS_XFER_MAX) return FTS_ERR_PARAM;
    return xfer(c, fts_frame_hw_write(c->tx, addr, data, len));
}

int fts_hw_read(struct fts_chip *c, uint32_t addr, uint8_t *out, size_t len) {
    if (!c || !c->tx) return FTS_ERR_PARAM;
    return read_after(c, fts_frame_hw_read(c->tx, addr), out, len);
}

int fts_read_chip_id(struct fts_chip *c) {
    int r = fts_hw_read(c, FTS_ADDR_CHIP_ID, c->id, 2);
    if (r) return r;
    return c->id[0] == FTS_CHIP_ID0 && c->id[1] == FTS_CHIP_ID1 ? FTS_OK : FTS_ERR_CHIP;
}

int fts_read_fifo(struct fts_chip *c, uint8_t *out, unsigned count) {
    if (!c || !c->tx || !count || count > FTS_FIFO_DEPTH) return FTS_ERR_PARAM;
    c->tx[0] = FTS_OP_FIFO_READ;
    return read_after(c, 1, out, (size_t)count * FTS_EVENT_SIZE);
}

int fts_read_events(struct fts_chip *c, uint8_t *out, unsigned *count) {
    *count = 0;
    int r = fts_read_fifo(c, out, 1);
    if (r) return r;
    if (out[0] == FTS_EVT_NOEVENT) return FTS_OK;
    unsigned more = out[FTS_EVT_REMAINING_POS] & FTS_EVT_REMAINING_MASK;
    if (more > FTS_FIFO_DEPTH - 1) more = FTS_FIFO_DEPTH - 1;
    if (more) {
        r = fts_read_fifo(c, out + FTS_EVENT_SIZE, more);
        if (r) {*count = 1; return r;}
    }
    *count = 1 + more;
    return FTS_OK;
}

int fts_wait_event(struct fts_chip *c, uint8_t id, unsigned timeout_ms) {
    uint8_t ev[FTS_EVENT_SIZE];
    const unsigned limit = timeout_ms / FTS_POLL_STEP_MS ? timeout_ms / FTS_POLL_STEP_MS : 1;
    c->polls = 0;
    while (c->polls < limit) {
        int r = fts_read_fifo(c, ev, 1);
        if (r) return r;
        ++c->polls;
        for (unsigned i = 0; i < FTS_EVENT_SIZE; ++i) c->last_event[i] = ev[i];
        if (fts_event_class(ev[0]) == FTS_CLASS_ERROR) ++c->errors_seen;
        if (ev[0] == id) return FTS_OK;
        wait_ms(c, FTS_POLL_STEP_MS);
    }
    return FTS_ERR_TIMEOUT;
}

int fts_reset(struct fts_chip *c) {
    if (!c || !c->bus) return FTS_ERR_PARAM;
    int r = FTS_ERR_TIMEOUT;
    c->spi4_fallback = 0;
    for (c->reset_attempts = 1; c->reset_attempts <= FTS_RESET_RETRIES; ++c->reset_attempts) {
        if (c->bus->reset) {
            c->bus->reset(c->bus->ctx, 0);
            wait_ms(c, FTS_RESET_LOW_MS);
            c->bus->reset(c->bus->ctx, 1);
        } else {
            const uint8_t v = FTS_SYSTEM_RESET_VALUE;
            r = fts_hw_write(c, FTS_ADDR_SYSTEM_RESET, &v, 1);
            if (r) continue;
        }
        r = fts_wait_event(c, FTS_EVT_CONTROLLER_READY, FTS_POLL_TIMEOUT_MS);
        if (!r) return FTS_OK;
        if (r == FTS_ERR_BUS || r == FTS_ERR_PARAM) return r;
    }
    c->reset_attempts = FTS_RESET_RETRIES;
    c->spi4_fallback = 1;
    const uint8_t icr = FTS_ICR_SPI4;
    int w = fts_hw_write(c, FTS_ADDR_ICR, &icr, 1);
    if (w) return w;
    wait_ms(c, 1);
    for (unsigned i = 0; i < FTS_ID_RETRIES; ++i) {
        wait_ms(c, 10);
        w = fts_read_chip_id(c);
        if (w != FTS_ERR_CHIP) break;
    }
    if (w == FTS_ERR_BUS) return w;
    return w ? FTS_ERR_CHIP : FTS_ERR_TIMEOUT;   // with the id: the chip answers but its firmware never reported ready
}

int fts_set_scan(struct fts_chip *c, uint8_t mode, uint8_t settings) {
    if (!c || !c->tx) return FTS_ERR_PARAM;
    c->tx[0] = FTS_OP_SCAN_MODE;
    c->tx[1] = mode;
    c->tx[2] = settings;
    return xfer(c, 3);
}

int fts_start_sensing(struct fts_chip *c) {
    int r = fts_set_scan(c, FTS_SCAN_ACTIVE, FTS_SCAN_SENSE_OFF);
    if (r) return r;
    wait_ms(c, FTS_SENSE_OFF_WAIT_MS);
    return fts_set_scan(c, FTS_SCAN_ACTIVE, FTS_SCAN_SENSE_ON);
}
