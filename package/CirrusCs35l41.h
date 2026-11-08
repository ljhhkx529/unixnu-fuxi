/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* One CS35L41 amplifier: the programs that identify, configure and mute it, run by the step runner, with the safety
 * condition the plan puts around them.
 *
 * The mechanism is sheng's - a program is a step table, the runner turns each step into one bus action, and nothing
 * waits - but the tables are built from the binding rather than copied from the CS35L43 board code: the register
 * addresses, the expected device id and revision, the slot this amplifier plays, the boost and interrupt values are
 * all facts about this board's part.
 *
 * The gate is what the plan adds. The amplifier may only be unmuted once its identity was read back and matched, its
 * interrupt status was read and found free of the error bits, and its PLL reports locked. Anything else leaves it
 * muted and the reason in last_error. */
#ifndef CIRRUS_CS35L41_H
#define CIRRUS_CS35L41_H

#include <stdint.h>

#include "Cs35Run.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CS35L41_MAX_STEPS 24u

/* The part's registers and the values this board programs into them. */
typedef struct {
    uint32_t reg_device_id, reg_revision, reg_global_enables, reg_block_enables, reg_asp_enables, reg_asp_ctrl;
    uint32_t reg_boost_peak, reg_boost_cap, reg_pll_ctrl, reg_mute, reg_irq1_eint1, reg_irq1_eint2;
    uint32_t reg_irq1_mask1;
    uint32_t device_id_value, revision_value;
    uint32_t enable_value, asp_slot_value, boost_peak_value, boost_cap_value, pll_value, irq_mask_value;
    uint32_t mute_on, mute_off;
    uint32_t clock_settle_ms;
} Cs35l41Binding;

/* One wire primitive: write length bytes, and when read_length is four, read four bytes back. */
typedef struct {
    int (*transfer)(void *context, const uint8_t *bytes, uint32_t length, uint32_t read_length, uint32_t *value);
    void *context;
} Cs35l41Hooks;

/* The interrupt bits, as the binding's masks use them. */
#define CS35L41_EINT2_PLL_LOCK 0x00000001u
#define CS35L41_EINT1_ERRORS 0x1c400000u   /* DC watchdog, amplifier, temperature, boost short */

enum {
    Cs35l41Idle = 0,
    Cs35l41Init = 1,
    Cs35l41MutedReady = 2,
    Cs35l41Unmuted = 3,
    Cs35l41Failed = -1,
};

enum {
    Cs35l41Ok = 0,
    Cs35l41Again = 1,
    Cs35l41BadBinding = -1,
    Cs35l41NoAck = -2,
    Cs35l41WrongChip = -3,
    Cs35l41WrongRevision = -4,
    Cs35l41FaultPresent = -5,
    Cs35l41PllUnlocked = -6,
    Cs35l41NotReady = -7,
};

typedef struct {
    Cs35l41Binding binding;
    Cs35l41Hooks hooks;
    Cs35Step program[CS35L41_MAX_STEPS];
    unsigned steps_in_program;
    Cs35Run run;
    int state, last_error;
    uint32_t device_id, revision, eint1, eint2;
    uint8_t verified;
    uint32_t steps, transfers, delays;
} Cs35l41;

/* Builds the programs from the binding; each returns the step count. */
unsigned cs35l41_init_program(const Cs35l41Binding *b, Cs35Step *out);
unsigned cs35l41_mute_program(const Cs35l41Binding *b, Cs35Step *out, int mute);
unsigned cs35l41_shutdown_program(const Cs35l41Binding *b, Cs35Step *out);

void cs35l41_init(Cs35l41 *amp, const Cs35l41Binding *binding, const Cs35l41Hooks *hooks);
/* Runs the identification, configuration and mute program one transfer at a time; Cs35l41Again while it waits for
 * a delay, Cs35l41Ok once the amplifier is muted and its identity, interrupts and PLL have been checked. */
int cs35l41_step(Cs35l41 *amp);
/* Unmutes only from a verified, muted, fault-free amplifier. */
int cs35l41_unmute(Cs35l41 *amp);
int cs35l41_mute(Cs35l41 *amp);
int cs35l41_shutdown(Cs35l41 *amp);

#ifdef __cplusplus
}
#endif

#endif
