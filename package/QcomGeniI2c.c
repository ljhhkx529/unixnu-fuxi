/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomGeniI2c.h"

static int fail(QcomGeniI2c *b, int reason)
{
    b->last_error = reason;
    b->state = QcomGeniFailed;
    return reason;
}

static uint32_t poll_limit(const QcomGeniI2c *b) {return b->binding.poll_limit ? b->binding.poll_limit : 64u;}

void qcom_geni_i2c_init(QcomGeniI2c *b, const QcomGeniI2cBinding *binding, const QcomGeniI2cHooks *hooks)
{
    for (uint32_t i = 0; i < sizeof *b; ++i) ((uint8_t *)b)[i] = 0;
    if (binding) b->binding = *binding;
    if (hooks) b->hooks = *hooks;
    b->state = QcomGeniIdle;
    b->last_error = QcomGeniOk;
}

uint32_t qcom_geni_i2c_value(const QcomGeniI2c *b) {return b ? b->value : 0;}

/* Packs a value into the part's width and byte order; a value that does not fit is refused rather than cut. */
static int pack_value(QcomGeniI2c *b, uint32_t value, uint8_t *out)
{
    const uint32_t bytes = b->binding.value_bytes;
    if (!bytes || bytes > 4u) return QcomGeniBadWidth;
    if (bytes < 4u && (value >> (8u * bytes)) != 0u) return QcomGeniValueTooWide;
    for (uint32_t i = 0; i < bytes; ++i) {
        const uint32_t shift = b->binding.little_endian ? 8u * i : 8u * (bytes - 1u - i);
        out[i] = (uint8_t)((value >> shift) & 0xffu);
    }
    return QcomGeniOk;
}

static uint32_t unpack_value(const QcomGeniI2c *b, const uint8_t *in)
{
    uint32_t value = 0;
    const uint32_t bytes = b->binding.value_bytes;
    for (uint32_t i = 0; i < bytes; ++i) {
        const uint32_t shift = b->binding.little_endian ? 8u * i : 8u * (bytes - 1u - i);
        value |= (uint32_t)in[i] << shift;
    }
    return value;
}

static int pack_register(QcomGeniI2c *b, uint32_t reg)
{
    const uint32_t bytes = b->binding.register_bytes;
    if (!bytes || bytes > 4u) return QcomGeniBadWidth;
    if (bytes < 4u && (reg >> (8u * bytes)) != 0u) return QcomGeniValueTooWide;
    for (uint32_t i = 0; i < bytes; ++i) {
        const uint32_t shift = b->binding.little_endian ? 8u * i : 8u * (bytes - 1u - i);
        b->packet[i] = (uint8_t)((reg >> shift) & 0xffu);
    }
    return QcomGeniOk;
}

static int start(QcomGeniI2c *b, uint32_t device, uint32_t reg, int is_read)
{
    if (!b || !b->hooks.mmio_write || !b->hooks.mmio_read || !b->hooks.fifo_write || !b->hooks.fifo_read)
        return fail(b, QcomGeniBadBinding);
    if (b->state == QcomGeniIssued || b->state == QcomGeniWaiting) return QcomGeniBusy;
    if (device > 0x7fu) return fail(b, QcomGeniBadAddress);
    if (!b->binding.register_bytes || b->binding.register_bytes > 4u) return fail(b, QcomGeniBadWidth);
    if (!b->binding.value_bytes || b->binding.value_bytes > 4u) return fail(b, QcomGeniBadWidth);
    const uint32_t packet_bytes = is_read ? b->binding.register_bytes
                                          : b->binding.register_bytes + b->binding.value_bytes;
    if (packet_bytes > b->binding.fifo_bytes || packet_bytes > sizeof b->packet) return fail(b, QcomGeniNoRoom);

    b->device = device;
    b->reg = reg;
    b->is_read = is_read ? 1u : 0u;
    b->phase = 0;
    b->state = QcomGeniIdle;
    b->last_error = QcomGeniOk;
    const int packed = pack_register(b, reg);
    if (packed != QcomGeniOk) return fail(b, packed);
    b->packet_bytes = b->binding.register_bytes;
    if (!is_read) {
        const int value = pack_value(b, b->value_pending, b->packet + b->binding.register_bytes);
        if (value != QcomGeniOk) return fail(b, value);
        b->packet_bytes = packet_bytes;
    }
    /* The command words go out first, then the FIFO, then the wait. Even for a read, this first packet is a
     * write: it carries the register address. The read itself is the second phase. */
    const uint32_t direction = 0u;
    if (b->hooks.mmio_write(b->hooks.context, b->binding.reg_cmd0, (device << 1) | direction))
        return fail(b, QcomGeniBadBinding);
    if (b->hooks.mmio_write(b->hooks.context, b->binding.reg_cmd1, packet_bytes))
        return fail(b, QcomGeniBadBinding);
    if (b->hooks.fifo_write(b->hooks.context, b->packet, b->packet_bytes)) return fail(b, QcomGeniBadBinding);
    b->state = QcomGeniIssued;
    b->waits = 0;
    if (is_read) ++b->reads; else ++b->writes;
    ++b->transfers;
    return QcomGeniOk;
}

int qcom_geni_i2c_write(QcomGeniI2c *b, uint32_t device, uint32_t reg, uint32_t value)
{
    if (!b) return QcomGeniBadBinding;
    b->value_pending = value;
    return start(b, device, reg, 0);
}

int qcom_geni_i2c_read(QcomGeniI2c *b, uint32_t device, uint32_t reg)
{
    if (!b) return QcomGeniBadBinding;
    b->value_pending = 0;
    b->value = 0;
    return start(b, device, reg, 1);
}

int qcom_geni_i2c_step(QcomGeniI2c *b)
{
    if (!b) return QcomGeniBadBinding;
    if (b->state == QcomGeniFailed) return b->last_error;
    if (b->state == QcomGeniDone) return QcomGeniOk;
    if (b->state == QcomGeniIdle) return QcomGeniWrongState;
    ++b->steps;

    if (b->state == QcomGeniIssued) {
        b->state = QcomGeniWaiting;
        return QcomGeniAgain;
    }

    uint32_t status = 0;
    if (b->hooks.mmio_read(b->hooks.context, b->binding.reg_status, &status)) return fail(b, QcomGeniBadBinding);
    if (status & b->binding.bit_arbitration) return fail(b, QcomGeniArbitrationLost);
    if (status & b->binding.bit_nack) return fail(b, QcomGeniNack);
    if (status & b->binding.bit_timeout) return fail(b, QcomGeniTimeout);
    if (!(status & b->binding.bit_done)) {
        if (++b->waits > poll_limit(b)) return fail(b, QcomGeniTimeout);
        return QcomGeniAgain;
    }
    if (b->binding.reg_status_clear) (void)b->hooks.mmio_write(b->hooks.context, b->binding.reg_status_clear, status);

    if (b->phase == 0 && b->is_read) {
        /* The register address went out as a write; now read the value back. */
        b->phase = 1;
        const uint32_t direction = 1u;
        if (b->hooks.mmio_write(b->hooks.context, b->binding.reg_cmd0, (b->device << 1) | direction))
            return fail(b, QcomGeniBadBinding);
        if (b->hooks.mmio_write(b->hooks.context, b->binding.reg_cmd1, b->binding.value_bytes))
            return fail(b, QcomGeniBadBinding);
        b->state = QcomGeniIssued;
        b->waits = 0;
        return QcomGeniAgain;
    }

    if (b->phase == 1) {
        uint8_t bytes[4] = {0};
        if (b->hooks.fifo_read(b->hooks.context, bytes, b->binding.value_bytes)) return fail(b, QcomGeniBadBinding);
        b->value = unpack_value(b, bytes);
        b->phase = 0;
    }
    b->state = QcomGeniDone;
    return QcomGeniOk;
}
