/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the CS35L41 safe bring-up: a register-level device model behind the two hooks, and every way the
 * sequence must refuse to reach an unmuted amplifier. */
#include <stdio.h>
#include <string.h>

#include "../CirrusCs35l41.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

/* Register addresses are the test's; the state machine never knows them. */
enum { R_ID = 0x01, R_REV = 0x02, R_BOOT = 0x03, R_TRIM = 0x04, R_BOOST_PEAK = 0x10, R_BOOST_CAP = 0x11,
       R_ASP = 0x12, R_PLL_CTRL = 0x20, R_PLL_LOCK = 0x21, R_MUTE = 0x30, R_FAULT = 0x40, R_COUNT = 0x48 };

typedef struct {
    uint32_t reg[R_COUNT];
    uint32_t boot_delay;      /* reads of the boot register that still answer zero */
    uint32_t lock_delay;      /* reads of the lock register after the PLL write that still answer zero */
    uint32_t lock_reads;
    uint32_t pll_written;
    uint32_t sticky;          /* a fault that survives its own clear */
    uint32_t corrupt_reg;     /* writes here read back changed */
    uint32_t fail_reg;        /* writes here fail */
    uint32_t resets;          /* reset assertions seen */
    uint32_t writes;
} Device;

static int dev_read(void *context, uint32_t reg, uint32_t *value)
{
    Device *d = context;
    if (reg >= R_COUNT || !value) return 1;
    if (reg == R_BOOT) {
        if (d->boot_delay) {--d->boot_delay; *value = 0; return 0;}
        *value = d->reg[R_BOOT];
        return 0;
    }
    if (reg == R_PLL_LOCK) {
        if (!d->pll_written) {*value = 0; return 0;}
        if (d->lock_delay) {--d->lock_delay; *value = 0; return 0;}
        *value = d->reg[R_PLL_LOCK];
        return 0;
    }
    if (reg == R_FAULT && d->sticky) {*value = d->sticky; return 0;}
    *value = d->reg[reg];
    return 0;
}

static int dev_write(void *context, uint32_t reg, uint32_t value)
{
    Device *d = context;
    if (reg >= R_COUNT) return 1;
    if (reg == d->fail_reg) return 1;
    ++d->writes;
    if (reg == R_PLL_CTRL) d->pll_written = 1;
    if (reg == R_FAULT) {d->reg[reg] = d->sticky ? d->sticky : 0; return 0;}
    d->reg[reg] = (reg == d->corrupt_reg) ? value ^ 1u : value;
    return 0;
}

static void dev_reset(void *context, int asserted)
{
    Device *d = context;
    if (asserted) ++d->resets;
}

static Cs35l41Binding binding(void)
{
    Cs35l41Binding b;
    memset(&b, 0, sizeof b);
    b.reg_chip_id = R_ID; b.reg_revision = R_REV; b.reg_boot_done = R_BOOT; b.reg_otp_trim = R_TRIM;
    b.reg_boost_peak = R_BOOST_PEAK; b.reg_boost_cap = R_BOOST_CAP; b.reg_asp_ctrl = R_ASP;
    b.reg_pll_ctrl = R_PLL_CTRL; b.reg_pll_lock = R_PLL_LOCK;
    b.reg_mute = R_MUTE; b.reg_fault_sticky = R_FAULT; b.reg_fault_mask = R_FAULT;
    b.chip_id_value = 0x35a41u;
    b.revision_mask = 0xffu; b.revision_value = 0xb0u;
    b.pll_lock_value = 1u;
    b.boost_peak_min = 1000u; b.boost_peak_max = 4500u; b.boost_cap_min = 10u;
    b.poll_limit = 8u;
    b.boost_peak_value = 4000u; b.boost_cap_value = 15u; b.asp_ctrl_value = 0x1234u; b.pll_ctrl_value = 0x0001u;
    b.mute_asserted = 1u; b.mute_released = 0u; b.fault_clear = 0xffffffffu;
    b.boot_done_value = 1u;
    return b;
}

static void device_init(Device *d)
{
    memset(d, 0, sizeof *d);
    d->reg[R_ID] = 0x35a41u;
    d->reg[R_REV] = 0xb0u;
    d->reg[R_BOOT] = 1u;
    d->reg[R_PLL_LOCK] = 1u;
    d->reg[R_MUTE] = 1u;
}

static int run(Cs35l41 *a, int steps)
{
    int rc = Cs35l41Again;
    for (int i = 0; i < steps; ++i) {
        rc = cs35l41_step(a);
        if (rc < 0) return rc;
        if (a->state == Cs35l41MutedReady) return Cs35l41Ok;
    }
    return rc;
}

static void start(Cs35l41 *a, Device *d, const Cs35l41Binding *b)
{
    const Cs35l41Hooks hooks = {dev_read, dev_write, dev_reset, d};
    cs35l41_init(a, b, &hooks);
}

static void test_happy(void)
{
    Device d; Cs35l41 a; const Cs35l41Binding b = binding();
    device_init(&d); d.boot_delay = 2; d.lock_delay = 2;
    start(&a, &d, &b);
    check(run(&a, 40) == Cs35l41Ok, "the bring-up reaches muted ready");
    check(a.state == Cs35l41MutedReady, "the terminal state is muted ready");
    check(a.chip_id == b.chip_id_value && a.revision == 0xb0u, "the chip id and revision are kept");
    check(a.verified && a.configured && a.locked, "the amplifier is verified, configured and locked");
    check(d.reg[R_MUTE] == b.mute_asserted, "the amplifier is muted before it is ready");
    check(d.reg[R_BOOST_PEAK] == b.boost_peak_value && d.reg[R_ASP] == b.asp_ctrl_value, "the configuration was written");
    check(d.resets == 1, "reset was asserted once");

    check(cs35l41_unmute(&a) == Cs35l41Ok && a.state == Cs35l41Unmuted, "a ready amplifier unmutes on request");
    check(d.reg[R_MUTE] == b.mute_released, "unmuting clears the mute register");
    check(cs35l41_mute(&a) == Cs35l41Ok && a.state == Cs35l41MutedReady, "muting returns to muted ready");
    check(d.reg[R_MUTE] == b.mute_asserted, "muting sets the mute register");

    (void)cs35l41_unmute(&a);
    check(cs35l41_shutdown(&a) == Cs35l41Ok && a.state == Cs35l41Off, "shutdown returns to off");
    check(d.reg[R_MUTE] == b.mute_asserted && d.resets == 2, "shutdown mutes first, then asserts reset");
    check(!a.verified, "shutdown clears the verified flag");
}

static void test_refusals(void)
{
    Device d; Cs35l41 a; const Cs35l41Binding b = binding();

    /* Unmuting before the bring-up ran. */
    device_init(&d); start(&a, &d, &b);
    check(cs35l41_unmute(&a) == Cs35l41NotReady, "an amplifier that is not ready cannot unmute");

    /* A wrong chip identity stops the sequence before anything is written. */
    device_init(&d); d.reg[R_ID] = 0x35a43u; start(&a, &d, &b);
    check(run(&a, 40) == Cs35l41WrongChip, "a wrong chip id stops the bring-up");
    check(d.reg[R_BOOST_PEAK] == 0, "nothing is configured on a wrong chip id");

    /* A revision outside the accepted mask. */
    device_init(&d); d.reg[R_REV] = 0x20u; start(&a, &d, &b);
    check(run(&a, 40) == Cs35l41WrongRevision, "a revision outside the mask stops the bring-up");

    /* The boot handshake never completes. */
    device_init(&d); d.boot_delay = 1000; start(&a, &d, &b);
    check(run(&a, 60) == Cs35l41BootTimeout, "a boot handshake that never completes times out");

    /* A write that does not read back what was written. */
    device_init(&d); d.corrupt_reg = R_BOOST_CAP; start(&a, &d, &b);
    check(run(&a, 40) == Cs35l41Readback, "a write that does not read back stops the bring-up");

    /* A write that fails outright. */
    device_init(&d); d.fail_reg = R_ASP; start(&a, &d, &b);
    check(run(&a, 40) == Cs35l41NoAck, "a failing write stops the bring-up");

    /* A boost profile outside the limits, refused before any write. */
    device_init(&d);
    Cs35l41Binding hot = b; hot.boost_peak_value = 6000u;
    start(&a, &d, &hot);
    check(run(&a, 40) == Cs35l41BoostOutOfRange, "a boost profile above the maximum is refused");
    check(d.reg[R_BOOST_PEAK] == 0 && d.writes == 0, "an out-of-range profile never reaches the part");

    Cs35l41Binding thin = b; thin.boost_cap_value = 5u;
    device_init(&d); start(&a, &d, &thin);
    check(run(&a, 40) == Cs35l41BoostOutOfRange, "a boost capacitance below the minimum is refused");

    /* The PLL never locks. */
    device_init(&d); d.lock_delay = 1000; start(&a, &d, &b);
    check(run(&a, 60) == Cs35l41PllTimeout, "a PLL that never locks times out");

    /* A sticky fault at the start. */
    device_init(&d); d.sticky = Cs35l41FaultShort; start(&a, &d, &b);
    check(run(&a, 40) == Cs35l41FaultPresent, "a sticky fault stops the bring-up");

    /* A fault that appears before unmuting keeps the amplifier muted. */
    device_init(&d); start(&a, &d, &b);
    (void)run(&a, 40);
    d.sticky = Cs35l41FaultThermal;
    check(cs35l41_unmute(&a) == Cs35l41FaultPresent && a.state == Cs35l41Failed,
          "a fault before unmuting keeps the amplifier muted");
    check(d.reg[R_MUTE] == b.mute_asserted, "the fault leaves the mute register asserted");
}

/* The plan asks for 100 reset/identify/configure/mute cycles with a consistent identity. */
static void test_cycles(void)
{
    const Cs35l41Binding b = binding();
    int reached = 0;
    uint32_t id = 0;
    for (int i = 0; i < 100; ++i) {
        Device d; Cs35l41 a;
        device_init(&d);
        d.boot_delay = (uint32_t)(i % 3);
        start(&a, &d, &b);
        if (run(&a, 40) == Cs35l41Ok && a.state == Cs35l41MutedReady) ++reached;
        if (i == 0) id = a.chip_id;
        else if (a.chip_id != id) reached = -1000;
        (void)cs35l41_shutdown(&a);
    }
    check(reached == 100, "a hundred reset/identify/configure/mute cycles all reach muted ready");
}

int main(void)
{
    test_happy();
    test_refusals();
    test_cycles();
    printf("PASS CirrusCs35l41: %d checks\n", checks);
    return failed ? 1 : 0;
}
