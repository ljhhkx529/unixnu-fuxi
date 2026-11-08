/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the amplifier: sheng's step runner over a register model of the part, the programs the binding builds,
 * and every condition that must keep the amplifier muted. */
#include <stdio.h>
#include <string.h>

#include "../CirrusCs35l41.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static uint32_t be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
}

/* The part: a register file of 32-bit words, read and written through the one wire primitive. */
typedef struct {
    uint32_t address[64];
    uint32_t value[64];
    unsigned count;
    uint32_t transfers, writes, reads;
    int broken;
} Part;

static int part_transfer(void *context, const uint8_t *bytes, uint32_t length, uint32_t read_length, uint32_t *value)
{
    Part *p = context;
    if (p->broken) return 1;
    ++p->transfers;
    const uint32_t reg = be32(bytes);
    unsigned slot = p->count;
    for (unsigned i = 0; i < p->count; ++i) if (p->address[i] == reg) slot = i;
    if (length == 8) {
        if (slot == p->count) {if (p->count < 64) {p->address[p->count] = reg; p->value[p->count] = 0; ++p->count;} else return 1;}
        p->value[slot] = be32(bytes + 4);
        ++p->writes;
        return 0;
    }
    if (length == 4 && read_length == 4) {
        ++p->reads;
        if (value) *value = slot < p->count ? p->value[slot] : 0;
        return 0;
    }
    return 1;
}

static uint32_t part_read(const Part *p, uint32_t reg)
{
    for (unsigned i = 0; i < p->count; ++i) if (p->address[i] == reg) return p->value[i];
    return 0xffffffffu;
}

static void part_write(Part *p, uint32_t reg, uint32_t value)
{
    for (unsigned i = 0; i < p->count; ++i) if (p->address[i] == reg) {p->value[i] = value; return;}
    if (p->count < 64) {p->address[p->count] = reg; p->value[p->count] = value; ++p->count;}
}

/* The board's part: addresses and values are the test's, not sheng's CS35L43 ones. */
static Cs35l41Binding binding(void)
{
    Cs35l41Binding b;
    memset(&b, 0, sizeof b);
    b.reg_device_id = 0x0000000u; b.reg_revision = 0x0000004u;
    b.reg_global_enables = 0x0002014u; b.reg_block_enables = 0x0002018u;
    b.reg_asp_enables = 0x0004800u; b.reg_asp_ctrl = 0x0004808u;
    b.reg_boost_peak = 0x0003808u; b.reg_boost_cap = 0x0003850u;
    b.reg_pll_ctrl = 0x0002C04u; b.reg_mute = 0x0006000u;
    b.reg_irq1_eint1 = 0x0010010u; b.reg_irq1_eint2 = 0x0010014u; b.reg_irq1_mask1 = 0x0010110u;
    b.device_id_value = 0x35a41u; b.revision_value = 0xb0u;
    b.enable_value = 0x00000001u; b.asp_slot_value = 0x00000002u;
    b.boost_peak_value = 4000u; b.boost_cap_value = 15u; b.pll_value = 0x00000001u;
    b.irq_mask_value = 0x00ffffffu;
    b.mute_on = 1u; b.mute_off = 0u;
    b.clock_settle_ms = 20u;
    return b;
}

static void part_init(Part *p, const Cs35l41Binding *b)
{
    memset(p, 0, sizeof *p);
    part_write(p, b->reg_device_id, b->device_id_value);
    part_write(p, b->reg_revision, b->revision_value);
    part_write(p, b->reg_irq1_eint2, CS35L41_EINT2_PLL_LOCK);
    part_write(p, b->reg_irq1_eint1, 0);
    part_write(p, b->reg_mute, b->mute_on);
}

static Cs35l41Hooks hooks_for(Part *p)
{
    Cs35l41Hooks h = {part_transfer, p};
    return h;
}

static int run_to_ready(Cs35l41 *a)
{
    int rc = Cs35l41Again;
    for (int i = 0; i < 80 && a->state != Cs35l41MutedReady && a->state != Cs35l41Failed; ++i) rc = cs35l41_step(a);
    return a->state == Cs35l41MutedReady ? Cs35l41Ok : (a->state == Cs35l41Failed ? a->last_error : rc);
}

static void test_program(void)
{
    const Cs35l41Binding b = binding();
    Cs35Step steps[CS35L41_MAX_STEPS];
    const unsigned n = cs35l41_init_program(&b, steps);
    check(n == 13u, "the identification and configuration program has thirteen steps");
    check(steps[0].op == Cs35Read && steps[0].reg == b.reg_device_id, "it reads the device id first");
    check(steps[1].op == Cs35Read && steps[1].reg == b.reg_revision, "then the revision");
    check(steps[2].op == Cs35Write && steps[2].reg == b.reg_global_enables && steps[2].value == b.enable_value,
          "then it enables the part");
    check(steps[3].op == Cs35Update && (steps[3].mask & b.enable_value) == b.enable_value,
          "the block enables are set without clearing the rest");
    check(steps[8].op == Cs35DelayMs && steps[8].value == b.clock_settle_ms, "it waits for the clock to settle");
    check(steps[9].reg == b.reg_irq1_eint1 && steps[10].reg == b.reg_irq1_eint2,
          "the interrupt status is read after the wait");
    check(steps[12].op == Cs35Write && steps[12].reg == b.reg_mute && steps[12].value == b.mute_on,
          "the last step leaves the part muted");

    Cs35Step mute[2];
    check(cs35l41_mute_program(&b, mute, 1) == 1u && mute[0].value == b.mute_on, "the mute program asserts mute");
    check(cs35l41_mute_program(&b, mute, 0) == 1u && mute[0].value == b.mute_off, "and releasing asserts the opposite");
    Cs35Step down[4];
    check(cs35l41_shutdown_program(&b, down) == 2u && down[0].value == b.mute_on && down[1].value == 0u,
          "shutdown mutes before it disables the part");
}

static void test_bring_up(void)
{
    const Cs35l41Binding b = binding();
    Part part;
    Cs35l41 amp;
    part_init(&part, &b);
    Cs35l41Hooks hooks = hooks_for(&part);
    cs35l41_init(&amp, &b, &hooks);

    check(cs35l41_unmute(&amp) == Cs35l41NotReady, "an amplifier that never came up cannot unmute");
    cs35l41_init(&amp, &b, &hooks);
    check(run_to_ready(&amp) == Cs35l41Ok, "the program brings the amplifier to muted ready");
    check(amp.state == Cs35l41MutedReady && amp.verified, "it is verified and muted");
    check(amp.device_id == b.device_id_value && amp.revision == b.revision_value, "the identity was read back");
    check(part_read(&part, b.reg_mute) == b.mute_on, "the part is muted");
    check(part_read(&part, b.reg_boost_peak) == b.boost_peak_value &&
          part_read(&part, b.reg_boost_cap) == b.boost_cap_value, "the boost limits were written");
    check(part_read(&part, b.reg_asp_enables) == b.asp_slot_value, "the slot this amplifier plays was written");
    check(amp.delays == 1u, "the settle delay was counted");

    check(cs35l41_unmute(&amp) == Cs35l41Ok && amp.state == Cs35l41Unmuted, "a verified amplifier unmutes");
    check(part_read(&part, b.reg_mute) == b.mute_off, "unmuting reaches the part");
    check(cs35l41_mute(&amp) == Cs35l41Ok && part_read(&part, b.reg_mute) == b.mute_on, "muting reaches the part again");
    check(cs35l41_shutdown(&amp) == Cs35l41Ok && amp.state == Cs35l41Idle && !amp.verified, "shutdown returns it to idle");
    check(part_read(&part, b.reg_global_enables) == 0u, "shutdown disables the part");
}

static void test_refusals(void)
{
    const Cs35l41Binding b = binding();
    Part part;
    Cs35l41 amp;

    part_init(&part, &b);
    part_write(&part, b.reg_device_id, 0x35a43u);
    Cs35l41Hooks hooks = hooks_for(&part);
    cs35l41_init(&amp, &b, &hooks);
    check(run_to_ready(&amp) == Cs35l41WrongChip, "a wrong device id stops the bring-up");
    check(part_read(&part, b.reg_boost_peak) == 4000u && amp.state == Cs35l41Failed,
          "nothing was configured on a wrong device id");

    part_init(&part, &b);
    part_write(&part, b.reg_revision, 0x20u);
    cs35l41_init(&amp, &b, &hooks);
    check(run_to_ready(&amp) == Cs35l41WrongRevision, "a revision outside the accepted one stops the bring-up");

    part_init(&part, &b);
    part_write(&part, b.reg_irq1_eint1, CS35L41_EINT1_ERRORS);
    cs35l41_init(&amp, &b, &hooks);
    check(run_to_ready(&amp) == Cs35l41FaultPresent, "an error bit stops the bring-up");

    part_init(&part, &b);
    part_write(&part, b.reg_irq1_eint2, 0u);
    cs35l41_init(&amp, &b, &hooks);
    check(run_to_ready(&amp) == Cs35l41PllUnlocked, "an unlocked PLL stops the bring-up");

    part_init(&part, &b);
    cs35l41_init(&amp, &b, &hooks);
    (void)run_to_ready(&amp);
    part_write(&part, b.reg_irq1_eint1, CS35L41_EINT1_ERRORS);
    amp.eint1 = CS35L41_EINT1_ERRORS;   /* the fault appears between the check and the request */
    check(cs35l41_unmute(&amp) == Cs35l41FaultPresent && part_read(&part, b.reg_mute) == b.mute_on,
          "a fault before unmuting keeps the part muted");

    part_init(&part, &b);
    part.broken = 1;
    cs35l41_init(&amp, &b, &hooks);
    check(run_to_ready(&amp) == Cs35l41NoAck, "a bus that does not answer stops the bring-up");
}

/* The runner on its own: a read-modify-write that changes nothing must not write. */
static void test_runner(void)
{
    Cs35Step steps[1];
    Cs35Run run;
    Cs35Action action;

    steps[0] = (Cs35Step){Cs35Update, 0x14u, 0x3u, 0x3u};
    cs35_run_start(&run, steps, 1);
    check(cs35_run_next(&run, &action) == Cs35Transfer && action.length == 4 && action.read_length == 4,
          "an update starts with a read");
    cs35_run_result(&run, 0x3u);   /* the bits are already set */
    check(cs35_run_next(&run, &action) == Cs35Finished, "when the bits are already set there is no write");

    cs35_run_start(&run, steps, 1);
    (void)cs35_run_next(&run, &action);
    cs35_run_result(&run, 0x1u);
    check(cs35_run_next(&run, &action) == Cs35Transfer && action.length == 8u,
          "when they are not, the write follows");
    check(be32(action.bytes) == 0x14u && be32(action.bytes + 4) == 0x3u, "the write carries the merged value");

    steps[0] = (Cs35Step){Cs35DelayMs, 0, 0, 7u};
    cs35_run_start(&run, steps, 1);
    check(cs35_run_next(&run, &action) == Cs35Wait && action.delay_ms == 7u, "a delay is a wait, not a transfer");
    check(cs35_run_next(&run, &action) == Cs35Finished, "and the program is over after it");

    check(cs35_wire_value(0x44332211u) == 0x11223344u, "a wire value is swapped into the register value");
}

int main(void)
{
    test_program();
    test_bring_up();
    test_refusals();
    test_runner();
    printf("PASS CirrusCs35l41: %d checks\n", checks);
    return failed ? 1 : 0;
}
