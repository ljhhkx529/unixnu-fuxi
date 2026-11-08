/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "CirrusCs35l41.h"

static int fail(Cs35l41 *a, int reason)
{
    a->last_error = reason;
    a->state = Cs35l41Failed;
    return reason;
}

unsigned cs35l41_init_program(const Cs35l41Binding *b, Cs35Step *out)
{
    if (!b || !out) return 0;
    unsigned n = 0;
    const Cs35Step steps[] = {
        {Cs35Read, b->reg_device_id, 0, 0},
        {Cs35Read, b->reg_revision, 0, 0},
        {Cs35Write, b->reg_global_enables, 0, b->enable_value},
        {Cs35Update, b->reg_block_enables, b->enable_value, b->enable_value},
        {Cs35Write, b->reg_asp_enables, 0, b->asp_slot_value},
        {Cs35Write, b->reg_boost_peak, 0, b->boost_peak_value},
        {Cs35Write, b->reg_boost_cap, 0, b->boost_cap_value},
        {Cs35Write, b->reg_pll_ctrl, 0, b->pll_value},
        {Cs35DelayMs, 0, 0, b->clock_settle_ms},
        {Cs35Read, b->reg_irq1_eint1, 0, 0},
        {Cs35Read, b->reg_irq1_eint2, 0, 0},
        {Cs35Write, b->reg_irq1_mask1, 0, b->irq_mask_value},
        {Cs35Write, b->reg_mute, 0, b->mute_on},
    };
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0] && n < CS35L41_MAX_STEPS; ++i) out[n++] = steps[i];
    return n;
}

unsigned cs35l41_mute_program(const Cs35l41Binding *b, Cs35Step *out, int mute)
{
    if (!b || !out) return 0;
    out[0] = (Cs35Step){Cs35Write, b->reg_mute, 0, mute ? b->mute_on : b->mute_off};
    return 1;
}

unsigned cs35l41_shutdown_program(const Cs35l41Binding *b, Cs35Step *out)
{
    if (!b || !out) return 0;
    out[0] = (Cs35Step){Cs35Write, b->reg_mute, 0, b->mute_on};
    out[1] = (Cs35Step){Cs35Write, b->reg_global_enables, 0, 0};
    return 2;
}

void cs35l41_init(Cs35l41 *a, const Cs35l41Binding *binding, const Cs35l41Hooks *hooks)
{
    for (uint32_t i = 0; i < sizeof *a; ++i) ((uint8_t *)a)[i] = 0;
    if (binding) a->binding = *binding;
    if (hooks) a->hooks = *hooks;
    a->state = Cs35l41Idle;
    a->last_error = Cs35l41Ok;
}

/* Runs a short program to completion; only the mute and shutdown programs go through here, so they carry no delay. */
static int run_short(Cs35l41 *a, const Cs35Step *steps, unsigned count)
{
    cs35_run_start(&a->run, steps, count);
    for (unsigned guard = 0; guard < 8u; ++guard) {
        Cs35Action action;
        const int what = cs35_run_next(&a->run, &action);
        if (what == Cs35Finished) return Cs35l41Ok;
        if (what == Cs35Wait) {++a->delays; continue;}
        uint32_t value = 0;
        if (a->hooks.transfer(a->hooks.context, action.bytes, action.length, action.read_length, &value))
            return fail(a, Cs35l41NoAck);
        ++a->transfers;
        cs35_run_result(&a->run, value);
    }
    return fail(a, Cs35l41NoAck);
}

static int check_identity(Cs35l41 *a)
{
    a->device_id = a->run.reads[0];
    a->revision = a->run.reads[1];
    a->eint1 = a->run.reads[2];
    a->eint2 = a->run.reads[3];
    if (a->device_id != a->binding.device_id_value) return fail(a, Cs35l41WrongChip);
    if (a->revision != a->binding.revision_value) return fail(a, Cs35l41WrongRevision);
    if (a->eint1 & CS35L41_EINT1_ERRORS) return fail(a, Cs35l41FaultPresent);
    if (!(a->eint2 & CS35L41_EINT2_PLL_LOCK)) return fail(a, Cs35l41PllUnlocked);
    a->verified = 1;
    a->state = Cs35l41MutedReady;
    return Cs35l41Ok;
}

int cs35l41_step(Cs35l41 *a)
{
    if (!a) return Cs35l41BadBinding;
    if (a->state == Cs35l41Failed) return a->last_error;
    if (a->state == Cs35l41MutedReady || a->state == Cs35l41Unmuted) return Cs35l41Ok;
    if (!a->hooks.transfer) return fail(a, Cs35l41BadBinding);

    if (a->state == Cs35l41Idle) {
        a->steps_in_program = cs35l41_init_program(&a->binding, a->program);
        if (!a->steps_in_program) return fail(a, Cs35l41BadBinding);
        cs35_run_start(&a->run, a->program, a->steps_in_program);
        a->state = Cs35l41Init;
    }

    Cs35Action action;
    const int what = cs35_run_next(&a->run, &action);
    if (what == Cs35Finished) return check_identity(a);
    if (what == Cs35Wait) {++a->delays; return Cs35l41Again;}
    uint32_t value = 0;
    if (a->hooks.transfer(a->hooks.context, action.bytes, action.length, action.read_length, &value))
        return fail(a, Cs35l41NoAck);
    ++a->transfers;
    ++a->steps;
    cs35_run_result(&a->run, value);
    return Cs35l41Ok;
}

int cs35l41_unmute(Cs35l41 *a)
{
    if (!a) return Cs35l41BadBinding;
    if (a->state != Cs35l41MutedReady || !a->verified) return fail(a, Cs35l41NotReady);
    if (a->eint1 & CS35L41_EINT1_ERRORS) return fail(a, Cs35l41FaultPresent);
    if (!(a->eint2 & CS35L41_EINT2_PLL_LOCK)) return fail(a, Cs35l41PllUnlocked);
    Cs35Step steps[2];
    const unsigned n = cs35l41_mute_program(&a->binding, steps, 0);
    if (run_short(a, steps, n) != Cs35l41Ok) return a->last_error;
    a->state = Cs35l41Unmuted;
    return Cs35l41Ok;
}

int cs35l41_mute(Cs35l41 *a)
{
    if (!a) return Cs35l41BadBinding;
    if (a->state != Cs35l41Unmuted && a->state != Cs35l41MutedReady) return fail(a, Cs35l41NotReady);
    Cs35Step steps[2];
    const unsigned n = cs35l41_mute_program(&a->binding, steps, 1);
    if (run_short(a, steps, n) != Cs35l41Ok) return a->last_error;
    a->state = Cs35l41MutedReady;
    return Cs35l41Ok;
}

int cs35l41_shutdown(Cs35l41 *a)
{
    if (!a) return Cs35l41BadBinding;
    Cs35Step steps[4];
    const unsigned n = cs35l41_shutdown_program(&a->binding, steps);
    if (run_short(a, steps, n) != Cs35l41Ok) return a->last_error;
    a->state = Cs35l41Idle;
    a->verified = 0;
    a->last_error = Cs35l41Ok;
    return Cs35l41Ok;
}
