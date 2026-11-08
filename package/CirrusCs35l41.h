/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Cirrus CS35L41 safe bring-up: reset, boot, identification, fault screening, configuration, PLL lock, then mute.
 *
 * The state machine owns the order and the conditions; it owns no register address. The binding supplies the
 * register map, the values it expects to read back, the revision it accepts, the boost limits and the poll
 * budget; the hooks supply the two wires (read/write) and the reset line. That keeps every fuxi value in one
 * place that is a fact, not code, and lets the host model drive a device model instead of hardware.
 *
 * Nothing here unmutes by itself: MUTED_READY is the terminal state of a bring-up, and unmuting is a separate
 * request that is refused unless the amplifier is verified, configured, locked and free of faults. */
#ifndef CIRRUS_CS35L41_H
#define CIRRUS_CS35L41_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The register map and the limits, supplied by the board binding. */
typedef struct {
    uint32_t reg_chip_id, reg_revision, reg_boot_done, reg_otp_trim;
    uint32_t reg_boost_peak, reg_boost_cap, reg_asp_ctrl, reg_pll_ctrl, reg_pll_lock;
    uint32_t reg_mute, reg_fault_sticky, reg_fault_mask;
    uint32_t chip_id_value;
    uint32_t revision_mask, revision_value;
    uint32_t pll_lock_value;
    uint32_t boost_peak_min, boost_peak_max, boost_cap_min;
    uint32_t poll_limit;   /* steps allowed for the boot handshake and for the PLL lock */
    /* The values to program, from the board profile: nothing here is a default. */
    uint32_t boost_peak_value, boost_cap_value, asp_ctrl_value, pll_ctrl_value;
    uint32_t mute_asserted, mute_released, fault_clear;
    uint32_t boot_done_value;
} Cs35l41Binding;

typedef struct {
    int (*read)(void *context, uint32_t reg, uint32_t *value);
    int (*write)(void *context, uint32_t reg, uint32_t value);
    void (*reset)(void *context, int asserted);
    void *context;
} Cs35l41Hooks;

enum {
    Cs35l41Off = 0,
    Cs35l41Reset = 1,        /* reset asserted */
    Cs35l41Boot = 2,         /* waiting for the boot handshake */
    Cs35l41Identify = 3,     /* chip id and revision must match before anything is written */
    Cs35l41Faults = 4,       /* sticky faults read and cleared */
    Cs35l41Configure = 5,    /* boost and ASP limits, each write read back */
    Cs35l41Pll = 6,          /* waiting for the PLL to lock */
    Cs35l41MutedReady = 7,
    Cs35l41Unmuted = 8,
    Cs35l41Failed = -1,
};

enum {
    Cs35l41Ok = 0,
    Cs35l41Again = 1,
    Cs35l41BadBinding = -1,
    Cs35l41NoAck = -2,
    Cs35l41WrongChip = -3,
    Cs35l41WrongRevision = -4,
    Cs35l41BootTimeout = -5,
    Cs35l41FaultPresent = -6,
    Cs35l41Readback = -7,
    Cs35l41PllTimeout = -8,
    Cs35l41NotReady = -9,
    Cs35l41BoostOutOfRange = -10,
};

/* Fault bits, in the mask the binding's fault registers use. */
enum {
    Cs35l41FaultBoost = 1u << 0,
    Cs35l41FaultPll = 1u << 1,
    Cs35l41FaultThermal = 1u << 2,
    Cs35l41FaultShort = 1u << 3,
    Cs35l41FaultBrownout = 1u << 4,
};

typedef struct {
    Cs35l41Binding binding;
    Cs35l41Hooks hooks;
    int state, last_error;
    uint32_t steps, waits, chip_id, revision, faults;
    uint8_t verified, configured, locked;
} Cs35l41;

void cs35l41_init(Cs35l41 *amp, const Cs35l41Binding *binding, const Cs35l41Hooks *hooks);
/* Advances the bring-up. Cs35l41Ok when it moved, Cs35l41Again while it waits, negative on failure. */
int cs35l41_step(Cs35l41 *amp);
/* Unmutes only from MutedReady with a verified, configured, locked amplifier and no fault in the mask. */
int cs35l41_unmute(Cs35l41 *amp);
/* Mutes and returns to MutedReady; used by the caller on any fault. */
int cs35l41_mute(Cs35l41 *amp);
/* Mutes, then asserts reset and returns to Off. */
int cs35l41_shutdown(Cs35l41 *amp);
/* Reads the sticky faults and classifies them. */
int cs35l41_faults(Cs35l41 *amp, uint32_t *mask);

#ifdef __cplusplus
}
#endif

#endif
