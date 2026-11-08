/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* GENI I2C: one register read or write at a time, packed to the widths the part actually uses.
 *
 * This is the transport the amplifiers sit on. It owns the sequence - pack the bytes, write the command words,
 * load the FIFO, wait for the controller's status, classify it - and owns no address: the controller's register
 * map, its status bits, its FIFO depth, the register and value widths and the wire byte order all arrive in the
 * binding, because they are board facts that have not been read back yet.
 *
 * A read is two transfers on the wire: the register address goes out as a write, then the value comes back. Both
 * halves are bounded, and a controller that never reports done is a timeout rather than a hang.
 *
 * NACK, arbitration loss and timeout are distinct results: an amplifier that is not there and one that is busy
 * are different problems, and the bring-up has to say which it saw. */
#ifndef QCOM_GENI_I2C_H
#define QCOM_GENI_I2C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* the controller's registers, from the binding */
    uint32_t reg_cmd0, reg_cmd1, reg_status, reg_status_clear, reg_tx_fifo, reg_rx_fifo;
    /* the status bits, from the binding */
    uint32_t bit_done, bit_nack, bit_timeout, bit_arbitration;
    /* the part's addressing, from the binding */
    uint32_t fifo_bytes, register_bytes, value_bytes, little_endian;
    uint32_t poll_limit;
} QcomGeniI2cBinding;

typedef struct {
    int (*mmio_write)(void *context, uint32_t reg, uint32_t value);
    int (*mmio_read)(void *context, uint32_t reg, uint32_t *value);
    int (*fifo_write)(void *context, const uint8_t *bytes, uint32_t length);
    int (*fifo_read)(void *context, uint8_t *bytes, uint32_t length);
    void *context;
} QcomGeniI2cHooks;

enum {
    QcomGeniIdle = 0,
    QcomGeniIssued = 1,      /* command words and FIFO written, waiting for the controller */
    QcomGeniWaiting = 2,
    QcomGeniDone = 3,
    QcomGeniFailed = -1,
};

enum {
    QcomGeniOk = 0,
    QcomGeniAgain = 1,
    QcomGeniBadBinding = -1,
    QcomGeniBadAddress = -2,
    QcomGeniBadWidth = -3,
    QcomGeniValueTooWide = -4,
    QcomGeniNoRoom = -5,
    QcomGeniNack = -6,
    QcomGeniTimeout = -7,
    QcomGeniArbitrationLost = -8,
    QcomGeniBusy = -9,
    QcomGeniWrongState = -10,
};

typedef struct {
    QcomGeniI2cBinding binding;
    QcomGeniI2cHooks hooks;
    int state, last_error, phase;   /* phase 0: the register address, phase 1: the value */
    uint32_t device, reg, value, value_pending, is_read;
    uint8_t packet[8];
    uint32_t packet_bytes;
    uint32_t steps, waits, transfers, reads, writes;
} QcomGeniI2c;

void qcom_geni_i2c_init(QcomGeniI2c *bus, const QcomGeniI2cBinding *binding, const QcomGeniI2cHooks *hooks);

/* Starts a register write. QcomGeniBusy while another transfer is in flight. */
int qcom_geni_i2c_write(QcomGeniI2c *bus, uint32_t device, uint32_t reg, uint32_t value);
/* Starts a register read; the value is available once qcom_geni_i2c_step() reports QcomGeniOk. */
int qcom_geni_i2c_read(QcomGeniI2c *bus, uint32_t device, uint32_t reg);
/* Advances the transfer: QcomGeniOk when it completed, QcomGeniAgain while it waits. */
int qcom_geni_i2c_step(QcomGeniI2c *bus);
/* The value the last read produced. */
uint32_t qcom_geni_i2c_value(const QcomGeniI2c *bus);

#ifdef __cplusplus
}
#endif

#endif
