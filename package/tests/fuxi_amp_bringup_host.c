// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the amplifier bring-up plan's pure part (package/FuxiAmpBringup.c): the QUP2 clock vote
// interpretation, the branch state rule and the pin and clock words the probe writes.
//
// The rule that matters here is the one Linux uses in clk-branch.c: a branch that is gated by the hardware is
// never waited for, so its halt bit says nothing. Getting that wrong makes a working clock look broken.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../FuxiAmpBringup.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

/* The CBCR bit fields of clk-branch.h. */
#define CBCR_CLK_OFF (1u << 31)
#define CBCR_NOC_FSM_STATUS (7u << 28)
#define FSM_STATUS_ON (2u << 28)

int main(void)
{
    /* The five branches, their halt registers and their hardware gating bits. */
    CHECK(FUXI_AMP_HALTS_COUNT == 5);
    CHECK(fuxi_amp_halt_registers[0] == 0x233d4);   /* core */
    CHECK(fuxi_amp_halt_registers[1] == 0x233d0);   /* s_ahb */
    CHECK(fuxi_amp_halt_registers[2] == 0x233cc);   /* m_ahb */
    CHECK(fuxi_amp_halt_registers[3] == 0x23004);   /* core_2x */
    CHECK(fuxi_amp_halt_registers[4] == 0x1e004);   /* s0 */
    /* The two AHB branches are hardware gated; the other three are not. */
    CHECK(fuxi_amp_halt_hwcg[0] == 0);
    CHECK(fuxi_amp_halt_hwcg[1] == 1);
    CHECK(fuxi_amp_halt_hwcg[2] == 1);
    CHECK(fuxi_amp_halt_hwcg[3] == 0);
    CHECK(fuxi_amp_halt_hwcg[4] == 0);
    /* The names are for the log only, but they must line up with the registers. */
    CHECK(!strcmp(fuxi_amp_halt_names[0], "core"));
    CHECK(!strcmp(fuxi_amp_halt_names[4], "s0"));

    /* A branch that is on: bit 31 clear. */
    CHECK(fuxi_amp_branch_running(0x00004000u));
    CHECK(fuxi_amp_branch_ok(0x00004000u, 0));
    /* A branch that is off: bit 31 set and the FSM field is not "on". */
    CHECK(!fuxi_amp_branch_running(CBCR_CLK_OFF | 0x2u));
    CHECK(!fuxi_amp_branch_ok(CBCR_CLK_OFF | 0x2u, 0));
    /* A branch that is coming up: bit 31 set but the FSM field reads "on". */
    CHECK(fuxi_amp_branch_running(CBCR_CLK_OFF | FSM_STATUS_ON));
    CHECK(fuxi_amp_branch_ok(CBCR_CLK_OFF | FSM_STATUS_ON, 0));

    /* The case the device actually showed: the two AHB branches read
     * 0x80000002 and 0x00000000.  Linux never waits for a branch in
     * hardware gating mode, so both are good.  Without that rule the
     * bring-up reports a clock that is actually running as broken. */
    CHECK(fuxi_amp_branch_ok(0x80000002u, 1));
    CHECK(fuxi_amp_branch_ok(0x00000000u, 1));
    /* The same value on a branch that is not hardware gated is a clock that
     * is off, and the hwcg bit must not be borrowed for it. */
    CHECK(!fuxi_amp_branch_ok(0x80000002u, 0));

    /* The vote bits: all five QUP wrapper 2 branches live in the second
     * vote register, and the first one belongs to wrapper 1. */
    CHECK(FUXI_AMP_VOTE2_BITS == 0x1fu);
    CHECK(FUXI_AMP_VOTE1 == 0x52008u);
    CHECK(FUXI_AMP_VOTE2 == 0x52010u);

    /* The pin words.  The I2C pins take the QUP2 SE0 function (2), 2 mA and
     * a pull-up; the reset pins take the GPIO function, 16 mA and no pull. */
    const uint32_t i2c = fuxi_amp_i2c_pin_cfg();
    CHECK((i2c & 3u) == FUXI_AMP_PULL_UP);
    CHECK(((i2c >> 2) & 0xfu) == FUXI_AMP_I2C_FUNC);
    CHECK(((i2c >> 6) & 7u) == 0u);           /* 2 mA is the code 0 */
    const uint32_t rst = fuxi_amp_reset_pin_cfg();
    CHECK((rst & 3u) == 0u);                  /* no pull */
    CHECK(((rst >> 2) & 0xfu) == 0u);         /* the GPIO function */
    CHECK(((rst >> 6) & 7u) == 7u);           /* 16 mA is the code 7 */

    /* The pin addresses: the TLMM starts 0x100000 into the fuxi node
 * (pinctrl@f000000) and each pin is 0x1000 apart. */
    CHECK(FUXI_AMP_TLMM_BASE == 0x0f100000ull);
    CHECK(fuxi_amp_tlmm_ctl(0) == 0x0f100000ull);
    CHECK(fuxi_amp_tlmm_ctl(182) == 0x0f1b6000ull);
    CHECK(fuxi_amp_tlmm_io(182) == fuxi_amp_tlmm_ctl(182) + 4u);

    /* The engine's clock words: 19.2 MHz in, a divider of 2 and the
     * counters 5 / 11 / 22 for 400 kHz (Linux i2c-qcom-geni.c). */
    CHECK(FUXI_AMP_CLK_CFG == (2u << 4 | 1u));
    CHECK(FUXI_AMP_SCL_COUNTERS == (5u << 20 | 11u << 10 | 22u));
    CHECK(FUXI_AMP_DFS_HZ == 19200000ull);

    /* The DFS level the I2C clock comes from. */
    const uint64_t levels[4] = {9600000ull, 19200000ull, 32000000ull, 64000000ull};
    CHECK(fuxi_amp_dfs_level(levels, 4, 19200000ull) == 1u);
    CHECK(fuxi_amp_dfs_level(levels, 4, 10000000ull) == 0xffu);
    CHECK(fuxi_amp_dfs_level(NULL, 4, 19200000ull) == 0xffu);
    CHECK(fuxi_amp_dfs_level(levels, 0, 19200000ull) == 0xffu);

    printf("PASS fuxi_amp_bringup_host %u checks\n", checks);
    return 0;
}
