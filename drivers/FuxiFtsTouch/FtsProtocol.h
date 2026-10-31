// SPDX-License-Identifier: BSD-2-Clause-Patent
// ST FingerTip (FTM5, chip id 36 40) host protocol over an abstract full-duplex SPI transport.
// Protocol facts follow a study of the Xiaomi fts_spi driver that the fuxi kernel loads (LineageOS
// android_kernel_xiaomi_sm8550 drivers/input/touchscreen/fts_spi, built with SALIXP, SPI4_WIRE, no I2C_INTERFACE
// and support-super-resolution). No code is copied from it. No board addresses, clocks or pins here.
#ifndef FTS_PROTOCOL_H
#define FTS_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// ---- Operation codes (SPI form) ----
#define FTS_OP_HW_REG_W 0xFA        // ftsHardware.h FTS_CMD_HW_REG_W
#define FTS_OP_HW_REG_R 0xFB        // FTS_CMD_HW_REG_R for SPI (0xFA is the I2C form)
#define FTS_OP_FIFO_READ 0x87       // FIFO_CMD_READALL and FIFO_CMD_READONE for SPI (0x86 for I2C)
#define FTS_OP_SCAN_MODE 0xA0       // ftsSoftware.h FTS_CMD_SCAN_MODE: A0 <mode> <settings>
#define FTS_HW_ADDR_BYTES 4         // ADDR_SIZE_HW_REG = BITS_32, sent big-endian
#define FTS_DUMMY_BYTES 1           // DUMMY_HW_REG / DUMMY_FIFO: the first byte clocked in after the command is discarded

// ---- Hardware registers (through FA / FB) ----
#define FTS_ADDR_CHIP_ID 0x20000000u    // ADDR_DCHIP_ID: two bytes, 36 40
#define FTS_ADDR_FW_VER 0x20000004u
#define FTS_ADDR_SYSTEM_RESET 0x20000024u
#define FTS_SYSTEM_RESET_VALUE 0x81     // soft reset, used only without a reset GPIO
#define FTS_ADDR_ICR 0x2000002Du        // interface control; SPI4_MASK selects 4-wire SPI
#define FTS_ICR_SPI4 0x02
#define FTS_CHIP_ID0 0x36               // DCHIP_ID_0 (SALIXP)
#define FTS_CHIP_ID1 0x40               // DCHIP_ID_1; the firmware header target of st_fts_m3.ftb is 0x4036

// ---- Scan mode ----
#define FTS_SCAN_ACTIVE 0x00
#define FTS_SCAN_SENSE_OFF 0x00         // settings byte of A0 00 00
#define FTS_SCAN_SENSE_ON 0x01          // settings byte of A0 00 01 (fts_init_sensing and the resume path)

// ---- FIFO and events ----
#define FTS_EVENT_SIZE 8
#define FTS_FIFO_DEPTH 32
#define FTS_EVT_REMAINING_POS 7         // EVENTS_REMAINING_POS: low five bits count the events still queued
#define FTS_EVT_REMAINING_MASK 0x1F
#define FTS_EVT_NOEVENT 0x00
#define FTS_EVT_CONTROLLER_READY 0x03   // whole byte: the chip finished a reset
// The interrupt handler dispatches on the high nibble of byte 0 (fts_event_handler: eventId = data[0] >> 4); the low
// bits of byte 0 also carry the top bits of the contact major axis, so 0x13 and 0x17 are both an enter event.
#define FTS_CLASS_ENTER 0x1
#define FTS_CLASS_MOTION 0x2
#define FTS_CLASS_LEAVE 0x3
#define FTS_CLASS_STATUS 0x4
#define FTS_CLASS_USER 0x5
#define FTS_CLASS_DEBUG 0xE
#define FTS_CLASS_ERROR 0xF
#define FTS_ERROR_WATCHDOG 0x06         // EVT_TYPE_ERROR_WATCHDOG: the driver resets the chip
#define FTS_ERROR_ESD 0xF0              // EVT_TYPE_ERROR_ESD: the driver power-cycles and resets the chip
#define FTS_TOUCH_INVALID 0x00
#define FTS_TOUCH_FINGER 0x01
#define FTS_TOUCH_GLOVE 0x02
#define FTS_TOUCH_STYLUS 0x03
#define FTS_TOUCH_PALM 0x04
#define FTS_TOUCH_HOVER 0x05
#define FTS_TOUCH_ID_MAX 10             // TOUCH_ID_MAX

// ---- Timing ----
#define FTS_RESET_LOW_MS 10             // fts_system_reset: reset low for 10 ms, then high
#define FTS_POLL_STEP_MS 2              // TIMEOUT_RESOLUTION
#define FTS_POLL_TIMEOUT_MS 100         // GENERAL_TIMEOUT
#define FTS_RESET_RETRIES 3             // RETRY_SYSTEM_RESET
#define FTS_SENSE_OFF_WAIT_MS 50        // WAIT_AFTER_SENSEOFF
#define FTS_ID_RETRIES 3                // fts_init: chip id read three times, 10 ms apart, after a failed reset

// ---- Buffers ----
// Largest transfer: the command byte, the dummy byte and a full FIFO.
#define FTS_XFER_MAX (1 + FTS_DUMMY_BYTES + FTS_EVENT_SIZE * FTS_FIFO_DEPTH)

#define FTS_OK 0
#define FTS_ERR_BUS (-5)
#define FTS_ERR_PARAM (-22)
#define FTS_ERR_CHIP (-19)
#define FTS_ERR_TIMEOUT (-110)

// Transport: full-duplex SPI mode 0, one transfer with chip select held for its whole length. rx receives len bytes.
// reset drives the reset line (0 low, 1 high); NULL means the board has no reset GPIO and the soft reset register is used.
struct fts_bus {
    void *ctx;
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*delay_ms)(void *ctx, unsigned ms);
    void (*reset)(void *ctx, int level);
};

struct fts_chip {
    const struct fts_bus *bus;
    uint8_t *tx, *rx;                 // caller supplied, FTS_XFER_MAX bytes each (DMA-safe if the transport needs it)
    uint8_t id[2];
    uint8_t last_event[FTS_EVENT_SIZE];   // last event read while polling, for diagnostics
    unsigned polls;                   // FIFO reads spent in the last wait
    unsigned reset_attempts;          // attempts used by the last fts_reset
    unsigned errors_seen;             // error events met while polling
    int spi4_fallback;                // 1: the reset gave no ready event and the ICR fallback ran
};

static inline unsigned fts_event_class(uint8_t id) {return id >> 4;}
static inline int fts_is_touch_event(uint8_t id) {
    const unsigned c = fts_event_class(id);
    return id != FTS_EVT_NOEVENT && (c == FTS_CLASS_ENTER || c == FTS_CLASS_MOTION || c == FTS_CLASS_LEAVE);
}

// Command frames. Each returns the number of bytes written to out.
size_t fts_frame_hw_write(uint8_t *out, uint32_t addr, const uint8_t *data, size_t len);
size_t fts_frame_hw_read(uint8_t *out, uint32_t addr);

int fts_hw_write(struct fts_chip *c, uint32_t addr, const uint8_t *data, size_t len);
int fts_hw_read(struct fts_chip *c, uint32_t addr, uint8_t *out, size_t len);
int fts_read_chip_id(struct fts_chip *c);
// Reads count (1..FTS_FIFO_DEPTH) events in one transfer into out (count * 8 bytes).
int fts_read_fifo(struct fts_chip *c, uint8_t *out, unsigned count);
// Reads the head event and then the rest of the queue it announces; *count is the number of events in out
// (out holds FTS_FIFO_DEPTH * 8 bytes). A queue that starts with NOEVENT gives *count = 0.
int fts_read_events(struct fts_chip *c, uint8_t *out, unsigned *count);
// Polls the FIFO every FTS_POLL_STEP_MS until an event whose first byte is id arrives.
int fts_wait_event(struct fts_chip *c, uint8_t id, unsigned timeout_ms);
// fts_system_reset: pulse the reset line (or write the soft reset), then wait for CONTROLLER_READY; up to three attempts.
// Without a ready event it falls back as fts_init does: select 4-wire SPI in the ICR and read the chip id, so a chip
// without firmware is told from a missing one (FTS_ERR_TIMEOUT with id 36 40, FTS_ERR_CHIP without).
int fts_reset(struct fts_chip *c);
int fts_set_scan(struct fts_chip *c, uint8_t mode, uint8_t settings);
// fts_init_sensing with FTS_FOD_AREA_REPORT: sense off, wait 50 ms, sense on.
int fts_start_sensing(struct fts_chip *c);

#ifdef __cplusplus
}
#endif
#endif
