/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the GENI I2C transport: a controller that records what it was given, and every status it can
 * report classified as itself rather than as a generic failure. */
#include <stdio.h>
#include <string.h>

#include "../QcomGeniI2c.h"
#include "../CirrusCs35l41.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

enum { R_CMD0 = 0x10, R_CMD1 = 0x14, R_STATUS = 0x18, R_CLEAR = 0x1c, R_TX = 0x20, R_RX = 0x24 };

typedef struct {
    uint32_t regs[0x40];
    uint8_t fifo[16];
    uint32_t fifo_bytes;
    uint8_t reply[4];
    uint32_t status;          /* what the controller reports */
    uint32_t never_done;
    uint32_t fifo_reads;
} Controller;

static int mmio_write(void *context, uint32_t reg, uint32_t value)
{
    Controller *c = context;
    if (reg >= 0x40) return 1;
    if (reg == R_CLEAR) {c->status &= ~value; return 0;}
    c->regs[reg] = value;
    /* Writing the command starts a transfer: the controller reports done once it finishes, unless this run is
     * meant to fail or to never finish. */
    if (reg == R_CMD0 && !c->never_done && !(c->status & 0xeu)) c->status |= 1u;
    return 0;
}

static int mmio_read(void *context, uint32_t reg, uint32_t *value)
{
    Controller *c = context;
    if (reg >= 0x40 || !value) return 1;
    if (reg == R_STATUS && c->never_done) {*value = 0; return 0;}   /* never reports done */
    *value = (reg == R_STATUS) ? c->status : c->regs[reg];
    return 0;
}

/* The far end of the wire: the amplifier itself. A write carries the register and the value; a read answers with
 * whatever that register holds. */
static uint32_t device[0x100];
static uint32_t device_register;
static int device_mode;   /* only the amplifier case puts a device on the far end of the wire */

static int fifo_write(void *context, const uint8_t *bytes, uint32_t length)
{
    Controller *c = context;
    if (length > sizeof c->fifo) return 1;
    memcpy(c->fifo, bytes, length);
    c->fifo_bytes = length;
    if (device_mode && length >= 2u) {
        const uint32_t reg = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8;
        if (reg >= 0x100u) return 1;
        device_register = reg;
        if (length == 6u) {
            device[reg] = (uint32_t)bytes[2] | (uint32_t)bytes[3] << 8 |
                          (uint32_t)bytes[4] << 16 | (uint32_t)bytes[5] << 24;
        }
    }
    return 0;
}

static int fifo_read(void *context, uint8_t *bytes, uint32_t length)
{
    Controller *c = context;
    if (length > sizeof c->reply) return 1;
    for (uint32_t i = 0; i < length; ++i) {
        if (device_mode) bytes[i] = (uint8_t)((device_register < 0x100u ? device[device_register] : 0u) >> (8u * i));
        else bytes[i] = c->reply[i];
    }
    ++c->fifo_reads;
    return 0;
}

static Controller controller;
static QcomGeniI2c bus;

static QcomGeniI2cBinding binding(void)
{
    QcomGeniI2cBinding b;
    memset(&b, 0, sizeof b);
    b.reg_cmd0 = R_CMD0; b.reg_cmd1 = R_CMD1; b.reg_status = R_STATUS; b.reg_status_clear = R_CLEAR;
    b.reg_tx_fifo = R_TX; b.reg_rx_fifo = R_RX;
    b.bit_done = 1u; b.bit_nack = 2u; b.bit_timeout = 4u; b.bit_arbitration = 8u;
    b.fifo_bytes = 8u;
    b.register_bytes = 2u;
    b.value_bytes = 4u;
    b.little_endian = 1u;
    b.poll_limit = 8u;
    return b;
}

static void start(void)
{
    memset(&controller, 0, sizeof controller);
    controller.status = 1u;   /* done */
    const QcomGeniI2cBinding b = binding();
    const QcomGeniI2cHooks hooks = {mmio_write, mmio_read, fifo_write, fifo_read, &controller};
    qcom_geni_i2c_init(&bus, &b, &hooks);
}

static int run(int steps)
{
    int rc = QcomGeniAgain;
    for (int i = 0; i < steps; ++i) {
        rc = qcom_geni_i2c_step(&bus);
        if (rc != QcomGeniAgain) return rc;
    }
    return rc;
}

static void test_write(void)
{
    start();
    check(qcom_geni_i2c_write(&bus, 0x40u, 0x1234u, 0xdeadbeefu) == QcomGeniOk, "a register write starts");
    check(controller.regs[R_CMD0] == (0x40u << 1), "the command word carries the device address and the direction");
    check(controller.regs[R_CMD1] == 6u, "the length is the register plus the value");
    check(controller.fifo_bytes == 6u, "six bytes reach the FIFO");
    check(controller.fifo[0] == 0x34 && controller.fifo[1] == 0x12, "the register is packed little endian");
    check(controller.fifo[2] == 0xef && controller.fifo[5] == 0xde, "the value is packed little endian");
    check(run(8) == QcomGeniOk, "the transfer completes");
    check(bus.state == QcomGeniDone && bus.writes == 1 && bus.transfers == 1, "the write is counted");

    /* Big endian on the wire reverses both fields. */
    start();
    QcomGeniI2cBinding be = binding();
    be.little_endian = 0u;
    const QcomGeniI2cHooks hooks = {mmio_write, mmio_read, fifo_write, fifo_read, &controller};
    qcom_geni_i2c_init(&bus, &be, &hooks);
    (void)qcom_geni_i2c_write(&bus, 0x40u, 0x1234u, 0xdeadbeefu);
    check(controller.fifo[0] == 0x12 && controller.fifo[1] == 0x34, "the register is packed big endian");
    check(controller.fifo[2] == 0xde && controller.fifo[5] == 0xef, "the value is packed big endian");
}

static void test_read(void)
{
    start();
    controller.reply[0] = 0x41; controller.reply[1] = 0x35; controller.reply[2] = 0x0a; controller.reply[3] = 0x00;
    check(qcom_geni_i2c_read(&bus, 0x42u, 0x0001u) == QcomGeniOk, "a register read starts");
    check(controller.regs[R_CMD0] == ((0x42u << 1) | 0u), "the register address goes out as a write first");
    check(controller.regs[R_CMD1] == 2u, "the first transfer carries the register address alone");
    check(run(12) == QcomGeniOk, "the read completes");
    check(controller.fifo_reads == 1, "the value came back through the FIFO");
    check(qcom_geni_i2c_value(&bus) == 0x000a3541u, "the value is unpacked little endian");
    check(bus.reads == 1, "the read is counted");

    start();
    controller.reply[0] = 0x0a; controller.reply[1] = 0x35; controller.reply[2] = 0x41; controller.reply[3] = 0x00;
    QcomGeniI2cBinding be = binding();
    be.little_endian = 0u;
    const QcomGeniI2cHooks hooks = {mmio_write, mmio_read, fifo_write, fifo_read, &controller};
    qcom_geni_i2c_init(&bus, &be, &hooks);
    (void)qcom_geni_i2c_read(&bus, 0x42u, 0x0001u);
    (void)run(12);
    check(qcom_geni_i2c_value(&bus) == 0x0a354100u, "the value is unpacked big endian");
}

static void test_failures(void)
{
    start();
    check(qcom_geni_i2c_write(&bus, 0x80u, 0, 0) == QcomGeniBadAddress, "a device address beyond seven bits is refused");
    check(qcom_geni_i2c_write(&bus, 0x40u, 0x10000u, 0) == QcomGeniValueTooWide, "a register that does not fit its width is refused");

    start();
    QcomGeniI2cBinding narrow = binding();
    narrow.value_bytes = 2u;
    const QcomGeniI2cHooks hooks = {mmio_write, mmio_read, fifo_write, fifo_read, &controller};
    qcom_geni_i2c_init(&bus, &narrow, &hooks);
    check(qcom_geni_i2c_write(&bus, 0x40u, 0x1u, 0x12345u) == QcomGeniValueTooWide, "a value that does not fit its width is refused");
    check(qcom_geni_i2c_write(&bus, 0x40u, 0x1u, 0x1234u) == QcomGeniOk, "a value that fits is accepted");

    start();
    QcomGeniI2cBinding tight = binding();
    tight.fifo_bytes = 4u;
    qcom_geni_i2c_init(&bus, &tight, &hooks);
    check(qcom_geni_i2c_write(&bus, 0x40u, 0x1u, 0x2u) == QcomGeniNoRoom, "a packet larger than the FIFO is refused");

    start();
    controller.status = 2u;   /* NACK */
    (void)qcom_geni_i2c_write(&bus, 0x40u, 0x1u, 0x2u);
    check(run(8) == QcomGeniNack, "a NACK is reported as a NACK");
    check(bus.state == QcomGeniFailed, "a NACK leaves the bus failed");

    start();
    controller.status = 4u;   /* timeout */
    (void)qcom_geni_i2c_write(&bus, 0x40u, 0x1u, 0x2u);
    check(run(8) == QcomGeniTimeout, "a controller timeout is reported as a timeout");

    start();
    controller.status = 8u;   /* arbitration lost */
    (void)qcom_geni_i2c_write(&bus, 0x40u, 0x1u, 0x2u);
    check(run(8) == QcomGeniArbitrationLost, "arbitration loss is reported as itself");

    start();
    controller.never_done = 1u;
    (void)qcom_geni_i2c_write(&bus, 0x40u, 0x1u, 0x2u);
    check(run(40) == QcomGeniTimeout, "a controller that never reports done times out");
    check(bus.waits > 0, "the wait was bounded rather than a hang");

    /* One transfer at a time. */
    start();
    (void)qcom_geni_i2c_write(&bus, 0x40u, 0x1u, 0x2u);
    check(qcom_geni_i2c_write(&bus, 0x40u, 0x3u, 0x4u) == QcomGeniBusy, "a second transfer while one is in flight is refused");

    /* Stepping a bus that was never given a transfer is a state error, and a failure stays a failure. */
    start();
    check(qcom_geni_i2c_step(&bus) == QcomGeniWrongState, "stepping an idle bus is a state error");
    start();
    controller.status = 2u;
    (void)qcom_geni_i2c_write(&bus, 0x40u, 0x1u, 0x2u);
    (void)run(8);
    check(qcom_geni_i2c_step(&bus) == QcomGeniNack, "a failed transfer keeps reporting its reason");
}

static QcomGeniI2c amp_bus;
static uint32_t amp_address;
static int amp_ok = 1;

static int amp_read(void *context, uint32_t reg, uint32_t *value);
static int amp_write(void *context, uint32_t reg, uint32_t value);
static void amp_reset(void *context, int asserted) {(void)context; (void)asserted;}

static int finish(void)
{
    for (int i = 0; i < 32; ++i) {
        const int rc = qcom_geni_i2c_step(&amp_bus);
        if (rc == QcomGeniAgain) continue;
        if (rc == QcomGeniOk) return 0;
        amp_ok = 0;
        return 1;
    }
    amp_ok = 0;
    return 1;
}

static int amp_read(void *context, uint32_t reg, uint32_t *value)
{
    (void)context;
    if (qcom_geni_i2c_read(&amp_bus, amp_address, reg) != QcomGeniOk) {amp_ok = 0; return 1;}
    if (finish()) return 1;
    *value = qcom_geni_i2c_value(&amp_bus);
    return 0;
}

static int amp_write(void *context, uint32_t reg, uint32_t value)
{
    (void)context;
    if (qcom_geni_i2c_write(&amp_bus, amp_address, reg, value) != QcomGeniOk) {amp_ok = 0; return 1;}
    return finish();
}

/* The amplifier's bring-up, run over the transport rather than through stubs. */
static void test_amps_over_i2c(void)
{
    memset(&controller, 0, sizeof controller);
    memset(device, 0, sizeof device);
    device_mode = 1;
    controller.status = 1u;
    device[0x01] = 0x35a41u;   /* chip id */
    device[0x02] = 0xb0u;      /* revision */
    device[0x03] = 1u;         /* boot done */
    device[0x21] = 1u;         /* PLL locked */

    const QcomGeniI2cBinding i2c = binding();
    const QcomGeniI2cHooks hooks = {mmio_write, mmio_read, fifo_write, fifo_read, &controller};
    qcom_geni_i2c_init(&amp_bus, &i2c, &hooks);

    QcomGeniI2c amp_a = amp_bus;   /* one bus per amplifier, as the board has */
    (void)amp_a;

    Cs35l41Binding b;
    memset(&b, 0, sizeof b);
    b.reg_chip_id = 0x01; b.reg_revision = 0x02; b.reg_boot_done = 0x03; b.reg_otp_trim = 0x04;
    b.reg_boost_peak = 0x10; b.reg_boost_cap = 0x11; b.reg_asp_ctrl = 0x12;
    b.reg_pll_ctrl = 0x20; b.reg_pll_lock = 0x21;
    b.reg_mute = 0x30; b.reg_fault_sticky = 0x40; b.reg_fault_mask = 0x40;
    b.chip_id_value = 0x35a41u;
    b.revision_mask = 0xffu; b.revision_value = 0xb0u;
    b.pll_lock_value = 1u;
    b.boost_peak_min = 1000u; b.boost_peak_max = 4500u; b.boost_cap_min = 10u;
    b.poll_limit = 8u;
    b.boost_peak_value = 4000u; b.boost_cap_value = 15u; b.asp_ctrl_value = 0x1234u; b.pll_ctrl_value = 1u;
    b.mute_asserted = 1u; b.mute_released = 0u; b.fault_clear = 0xffffffffu;
    b.boot_done_value = 1u;
    const Cs35l41Hooks amp_hooks = {amp_read, amp_write, amp_reset, 0};

    Cs35l41 amp;
    cs35l41_init(&amp, &b, &amp_hooks);
    amp_address = 0x40u;
    amp_ok = 1;
    /* Cs35l41Ok means "moved on", not "done": the loop has to watch the state. */
    int rc = Cs35l41Again;
    for (int i = 0; i < 60 && amp.state != Cs35l41MutedReady && amp.state != Cs35l41Failed; ++i) {
        rc = cs35l41_step(&amp);
    }
    check(amp_ok == 1, "every transfer over the wire succeeded");
    check(rc == Cs35l41Ok && amp.state == Cs35l41MutedReady, "the amplifier reaches muted ready over the I2C transport");
    check(device[0x30] == 1u, "the mute register was written through the wire");
    check(device[0x10] == 4000u && device[0x11] == 15u, "the boost registers were written through the wire");
    check(device[0x01] == 0x35a41u, "the chip id was read back through the wire");

    check(cs35l41_unmute(&amp) == Cs35l41Ok && device[0x30] == 0u, "unmuting writes the register through the wire");
    check(cs35l41_mute(&amp) == Cs35l41Ok && device[0x30] == 1u, "muting writes it back");
}

int main(void)
{
    test_write();
    test_read();
    test_failures();
    test_amps_over_i2c();
    printf("PASS QcomGeniI2c: %d checks\n", checks);
    return failed ? 1 : 0;
}
