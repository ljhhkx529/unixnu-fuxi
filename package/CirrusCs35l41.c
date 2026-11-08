/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "CirrusCs35l41.h"

#define DEFAULT_POLL 64u

static int fail(Cs35l41 *a, int reason)
{
    a->last_error = reason;
    a->state = Cs35l41Failed;
    return reason;
}

static uint32_t poll_limit(const Cs35l41 *a)
{
    return a->binding.poll_limit ? a->binding.poll_limit : DEFAULT_POLL;
}

/* Every configuration write is read back: an amplifier that did not take a value must not be unmuted later. */
static int write_checked(Cs35l41 *a, uint32_t reg, uint32_t value)
{
    if (!reg) return fail(a, Cs35l41BadBinding);
    uint32_t back = 0;
    if (a->hooks.write(a->hooks.context, reg, value)) return fail(a, Cs35l41NoAck);
    if (a->hooks.read(a->hooks.context, reg, &back)) return fail(a, Cs35l41NoAck);
    if (back != value) return fail(a, Cs35l41Readback);
    return Cs35l41Ok;
}

static int read_reg(Cs35l41 *a, uint32_t reg, uint32_t *value)
{
    if (!reg) return fail(a, Cs35l41BadBinding);
    if (a->hooks.read(a->hooks.context, reg, value)) return fail(a, Cs35l41NoAck);
    return Cs35l41Ok;
}

void cs35l41_init(Cs35l41 *a, const Cs35l41Binding *binding, const Cs35l41Hooks *hooks)
{
    for (uint32_t i = 0; i < sizeof *a; ++i) ((uint8_t *)a)[i] = 0;
    if (binding) a->binding = *binding;
    if (hooks) a->hooks = *hooks;
    a->state = Cs35l41Off;
    a->last_error = Cs35l41Ok;
}

int cs35l41_faults(Cs35l41 *a, uint32_t *mask)
{
    if (!a || !mask) return Cs35l41BadBinding;
    uint32_t sticky = 0;
    const int rc = read_reg(a, a->binding.reg_fault_sticky, &sticky);
    if (rc != Cs35l41Ok) return rc;
    if (sticky) {
        /* Clear what was seen, then read it again: a fault that survives its own clear is not a stale bit. */
        if (a->binding.fault_clear) {
            if (a->hooks.write(a->hooks.context, a->binding.reg_fault_sticky, a->binding.fault_clear))
                return fail(a, Cs35l41NoAck);
        }
        uint32_t again = 0;
        if (a->hooks.read(a->hooks.context, a->binding.reg_fault_sticky, &again)) return fail(a, Cs35l41NoAck);
        if (again) return fail(a, Cs35l41FaultPresent);
    }
    a->faults = sticky;
    *mask = sticky;
    return Cs35l41Ok;
}

int cs35l41_unmute(Cs35l41 *a)
{
    if (!a) return Cs35l41BadBinding;
    if (a->state != Cs35l41MutedReady || !a->verified || !a->configured || !a->locked)
        return fail(a, Cs35l41NotReady);
    uint32_t mask = 0;
    const int rc = cs35l41_faults(a, &mask);
    if (rc != Cs35l41Ok) return rc;
    if (mask) return fail(a, Cs35l41FaultPresent);
    if (write_checked(a, a->binding.reg_mute, a->binding.mute_released) != Cs35l41Ok) return a->last_error;
    a->state = Cs35l41Unmuted;
    return Cs35l41Ok;
}

int cs35l41_mute(Cs35l41 *a)
{
    if (!a) return Cs35l41BadBinding;
    if (a->state != Cs35l41Unmuted && a->state != Cs35l41MutedReady && a->state != Cs35l41Pll)
        return fail(a, Cs35l41NotReady);
    if (write_checked(a, a->binding.reg_mute, a->binding.mute_asserted) != Cs35l41Ok) return a->last_error;
    /* A fault found while muting leaves the amplifier muted and ready, which is where the caller wants it. */
    a->state = Cs35l41MutedReady;
    return Cs35l41Ok;
}

int cs35l41_shutdown(Cs35l41 *a)
{
    if (!a) return Cs35l41BadBinding;
    if (a->verified && a->state != Cs35l41Off) (void)cs35l41_mute(a);
    a->hooks.reset(a->hooks.context, 1);
    a->state = Cs35l41Off;
    a->verified = a->configured = a->locked = 0;
    a->last_error = Cs35l41Ok;
    return Cs35l41Ok;
}

int cs35l41_step(Cs35l41 *a)
{
    if (!a) return Cs35l41BadBinding;
    if (a->state == Cs35l41Failed) return a->last_error;
    if (a->state == Cs35l41MutedReady || a->state == Cs35l41Unmuted) return Cs35l41Ok;
    if (!a->hooks.read || !a->hooks.write || !a->hooks.reset) return fail(a, Cs35l41BadBinding);
    ++a->steps;

    switch (a->state) {
    case Cs35l41Off:
        a->hooks.reset(a->hooks.context, 1);
        a->waits = 0;
        a->state = Cs35l41Reset;
        return Cs35l41Ok;
    case Cs35l41Reset:
        /* Reset released; the datasheet delay belongs to the caller's step interval. */
        a->hooks.reset(a->hooks.context, 0);
        a->waits = 0;
        a->state = Cs35l41Boot;
        return Cs35l41Ok;
    case Cs35l41Boot: {
        uint32_t done = 0;
        const int rc = read_reg(a, a->binding.reg_boot_done, &done);
        if (rc != Cs35l41Ok) return rc;
        if (done == a->binding.boot_done_value) {a->state = Cs35l41Identify; return Cs35l41Ok;}
        if (++a->waits > poll_limit(a)) return fail(a, Cs35l41BootTimeout);
        return Cs35l41Again;
    }
    case Cs35l41Identify: {
        uint32_t id = 0, revision = 0;
        int rc = read_reg(a, a->binding.reg_chip_id, &id);
        if (rc != Cs35l41Ok) return rc;
        a->chip_id = id;
        if (id != a->binding.chip_id_value) return fail(a, Cs35l41WrongChip);
        rc = read_reg(a, a->binding.reg_revision, &revision);
        if (rc != Cs35l41Ok) return rc;
        a->revision = revision;
        if ((revision & a->binding.revision_mask) != a->binding.revision_value) return fail(a, Cs35l41WrongRevision);
        a->verified = 1;
        a->state = Cs35l41Faults;
        return Cs35l41Ok;
    }
    case Cs35l41Faults: {
        uint32_t mask = 0;
        const int rc = cs35l41_faults(a, &mask);
        if (rc != Cs35l41Ok) return rc;
        if (mask) return fail(a, Cs35l41FaultPresent);
        a->state = Cs35l41Configure;
        return Cs35l41Ok;
    }
    case Cs35l41Configure: {
        /* The limits are checked before anything is written, so an out-of-range profile never reaches the part. */
        if (a->binding.boost_peak_value < a->binding.boost_peak_min ||
            a->binding.boost_peak_value > a->binding.boost_peak_max ||
            a->binding.boost_cap_value < a->binding.boost_cap_min)
            return fail(a, Cs35l41BoostOutOfRange);
        if (write_checked(a, a->binding.reg_boost_peak, a->binding.boost_peak_value) != Cs35l41Ok) return a->last_error;
        if (write_checked(a, a->binding.reg_boost_cap, a->binding.boost_cap_value) != Cs35l41Ok) return a->last_error;
        if (write_checked(a, a->binding.reg_asp_ctrl, a->binding.asp_ctrl_value) != Cs35l41Ok) return a->last_error;
        a->configured = 1;
        a->state = Cs35l41Pll;
        return Cs35l41Ok;
    }
    case Cs35l41Pll: {
        if (!a->locked) {
            if (write_checked(a, a->binding.reg_pll_ctrl, a->binding.pll_ctrl_value) != Cs35l41Ok) return a->last_error;
            uint32_t lock = 0;
            const int rc = read_reg(a, a->binding.reg_pll_lock, &lock);
            if (rc != Cs35l41Ok) return rc;
            if (lock == a->binding.pll_lock_value) {a->locked = 1; a->waits = 0;}
            else if (++a->waits > poll_limit(a)) return fail(a, Cs35l41PllTimeout);
            else return Cs35l41Again;
        }
        /* Mute before the amplifier is declared ready, never after. */
        if (write_checked(a, a->binding.reg_mute, a->binding.mute_asserted) != Cs35l41Ok) return a->last_error;
        a->state = Cs35l41MutedReady;
        return Cs35l41Ok;
    }
    default:
        return fail(a, Cs35l41BadBinding);
    }
}
