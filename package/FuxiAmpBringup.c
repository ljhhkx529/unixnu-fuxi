/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The pure part of the amplifier bring-up plan.  See FuxiAmpBringup.h for
 * where the facts come from. */
#include "FuxiAmpBringup.h"

/* The branch halt registers of the five QUP wrapper 2 branches that carry the
 * amplifier's I2C transfer (Linux gcc-sm8550.c, gcc_qupv3_wrap2_*_clk).  The
 * order matches the vote bits in FuxiAmpBringup.h: core, s_ahb, m_ahb,
 * core_2x, s0. */
const uint32_t fuxi_amp_halt_registers[FUXI_AMP_HALTS_COUNT] = {
    0x233d4, 0x233d0, 0x233cc, 0x23004, 0x1e004,
};

/* The hardware clock gating bit of each of those branches, in the same
 * order.  Linux's gcc-sm8550.c gives the two AHB branches an hwcg
 * register equal to their halt register with hwcg_bit 1, and gives the
 * other three no hwcg register at all (so their bit 1 keeps its usual
 * meaning and must not be read as hwcg).  clk-branch.c's
 * clk_branch_in_hwcg_mode makes clk_branch_wait skip the halt check for
 * the branches in this mode. */
const uint8_t fuxi_amp_halt_hwcg[FUXI_AMP_HALTS_COUNT] = {
    0, 1, 1, 0, 0,
};

/* The Linux names of those branches, in the same order. */
const char *const fuxi_amp_halt_names[FUXI_AMP_HALTS_COUNT] = {
    "core", "s_ahb", "m_ahb", "core_2x", "s0",
};

unsigned fuxi_amp_dfs_level(const uint64_t *level_hz, unsigned count, uint64_t target)
{
    if (!level_hz) return 0xffu;
    for (unsigned level = 0; level < count; ++level)
        if (level_hz[level] == target) return level;
    return 0xffu;
}
