/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The amplifier bring-up plan for the fuxi board: which window to map, which
 * pins to set, which clock votes to write and which register writes the QUP2
 * SE0 engine needs before it can carry an I2C transfer.
 *
 * Everything here is pure arithmetic over the board's facts, so the plan can
 * be checked on the host before a single register is touched.  The probe
 * (FuxiAdspProbe.cpp) does the mapping and the writes; this file decides
 * what to write.
 *
 * Where the numbers come from:
 *   GCC     the fuxi device tree (clock-controller@100000) and Linux's
 *           gcc-sm8550.c for the branch and vote register names.
 *   QUP2    the fuxi device tree (qupv3_2_geni_se@8c0000, i2c@880000 with
 *           reg 0x880000 0x4000) and i2c-qcom-geni.c for the clock values.
 *   TLMM    the fuxi device tree (pinctrl@f000000) and pinctrl-sm8550.c for
 *           the register layout.
 *   The amplifier pins and its I2C address: the fuxi device tree
 *           (cs35l41@40, cs35l41@42 and the pinctrl nodes they use).
 *
 * The amplifier's own register values (the CS35L41 program) are the ones
 * Kerfand's UniXNU-Fuxi-Arm64e-iOS17 uses for the same board and the same
 * part (BSD-2-Clause-Patent).  The clock values here are not: Kerfand's
 * GeniI2c.c assumes the boot loader left the serial engine configured
 * and never touches the clock controller, so the votes and the DFS level
 * below are taken from Linux's gcc-sm8550.c for this SoC instead.  The
 * bring-up stops when the clocks do not come up rather than proceeding. */
#ifndef FUXI_AMP_BRINGUP_H
#define FUXI_AMP_BRINGUP_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The stages of the amplifier bring-up.  Each stage does one kind of thing,
 * so a run that stops in one of them says which. */
#define FUXI_AMP_STAGE_NONE 0u
#define FUXI_AMP_STAGE_MAP 1u        /* the windows are mapped */
#define FUXI_AMP_STAGE_GCC_READ 2u   /* the GCC state was read */
#define FUXI_AMP_STAGE_GCC_VOTE 3u   /* the QUP votes were written */
#define FUXI_AMP_STAGE_PINS 4u       /* the I2C pins and the resets were driven */
#define FUXI_AMP_STAGE_BUS 5u        /* the SE0 engine was configured for I2C */
#define FUXI_AMP_STAGE_IDENTITY 6u   /* the amplifiers answered */
#define FUXI_AMP_STAGE_DONE 7u

/* The QUP2 SE0 engine: the fuxi tree gives the I2C node reg 0x880000 with a
 * 0x4000 window.  The 16 KiB matters: the engine's interrupt and clock
 * registers reach 0xe30, and a smaller window turns those writes into an
 * abort. */
#define FUXI_AMP_I2C_BASE 0x00880000ULL
#define FUXI_AMP_I2C_SIZE 0x4000ULL

/* The clock controller: the fuxi tree gives clock-controller@100000. */
#define FUXI_AMP_GCC_BASE 0x00100000ULL
/* The pages the bring-up touches: the GPLL0 mode, the DFS clock sources of
 * the two QUP branches, the branch halt registers and the vote registers. */
#define FUXI_AMP_GCC_PAGE_LOW 0x00000ULL
#define FUXI_AMP_GCC_PAGE_18 0x18000ULL
#define FUXI_AMP_GCC_PAGE_1E 0x1E000ULL
#define FUXI_AMP_GCC_PAGE_23 0x23000ULL
#define FUXI_AMP_GCC_PAGE_52 0x52000ULL
#define FUXI_AMP_GCC_PAGE_SIZE 0x1000ULL

/* The GCC registers the bring-up uses (Linux gcc-sm8550.c). */
#define FUXI_AMP_GPLL0_MODE 0x00000u      /* bit 31: the PLL is locked */
#define FUXI_AMP_GPLL0_ENABLE 0x52018u    /* bit 0 */
#define FUXI_AMP_VOTE1 0x52008u
#define FUXI_AMP_VOTE2 0x52010u
/* The vote bits.  The fuxi tree gives the amplifier's I2C node the clock
 * <&gcc GCC_QUPV3_WRAP2_S0_CLK> (index 0x6f), so the branches that carry
 * the transfer are the QUP wrapper 2 ones, and Linux's gcc-sm8550.c puts
 * all five of them in the second vote register:
 *   0x52010 bit 0  gcc_qupv3_wrap2_core_clk
 *   0x52010 bit 1  gcc_qupv3_wrap_2_s_ahb_clk
 *   0x52010 bit 2  gcc_qupv3_wrap_2_m_ahb_clk
 *   0x52010 bit 3  gcc_qupv3_wrap2_core_2x_clk
 *   0x52010 bit 4  gcc_qupv3_wrap2_s0_clk
 * The first vote register holds the wrapper 1 branches (bits 18..21 are
 * wrapper 1's core and AHB clocks), which the touch driver already votes
 * for; they belong to a different QUP block and are not written here. */
#define FUXI_AMP_VOTE2_BITS 0x1fu
/* The branch halt registers of the five branches above: a branch whose
 * clock is off reads bit 31 of its CBCR.
 *   0x233d4 gcc_qupv3_wrap2_core_clk
 *   0x233d0 gcc_qupv3_wrap_2_s_ahb_clk
 *   0x233cc gcc_qupv3_wrap_2_m_ahb_clk
 *   0x23004 gcc_qupv3_wrap2_core_2x_clk
 *   0x1e004 gcc_qupv3_wrap2_s0_clk */
#define FUXI_AMP_HALTS_COUNT 5
extern const uint32_t fuxi_amp_halt_registers[FUXI_AMP_HALTS_COUNT];
/* The DFS clock sources of the two QUP branches. */
#define FUXI_AMP_RCG_SE0 0x1E010u
/* The RCG's own command register: bit 31 the root is off, bit 0 the update
 * is pending, and at +0x14 the DFS register - bit 0 the dynamic frequency
 * scaling is enabled, bits 4:1 the level the branch runs at
 * (Linux clk-rcg2.c: SE_CMD_DFSR_OFFSET). */
#define FUXI_AMP_RCG_CMD 0x0u
#define FUXI_AMP_RCG_DFSR 0x14u
#define FUXI_AMP_RCG_ROOT_OFF (1u << 31)
#define FUXI_AMP_DFSR_ENABLED 1u
#define FUXI_AMP_DFSR_LEVEL(v) (((v) >> 1) & 0xfu)
/* The CBCR clock-off bit. */
#define FUXI_AMP_CBCR_CLK_OFF (1u << 31)

/* The TLMM: the fuxi tree's node is pinctrl@f000000, and the registers
 * start 0x100000 into it (pinctrl-sm8550.c's REG_BASE). */
#define FUXI_AMP_TLMM_BASE 0x0F100000ULL
#define FUXI_AMP_GPIO_STRIDE 0x1000ULL
/* The pins (fuxi tree: qupv3_se8_i2c_pins and cirrus_reset_default_*). */
#define FUXI_AMP_I2C_SDA_GPIO 0u
#define FUXI_AMP_I2C_SCL_GPIO 1u
#define FUXI_AMP_RESET_T_GPIO 182u   /* the T amplifier (0x42) */
#define FUXI_AMP_RESET_B_GPIO 183u   /* the B amplifier (0x40) */
#define FUXI_AMP_IRQ_T_GPIO 98u
#define FUXI_AMP_IRQ_B_GPIO 99u
/* The secondary MI2S (I2S1) pins that carry the amplifier link: the fuxi
 * tree's aux1_pcm_* nodes give gpio121 the clock, gpio123 the word select,
 * gpio122 and gpio124 the two data lines.  Linux's pinctrl-sm8550.c lists
 * the GPIO function first in every group, so the i2s1_* function is the
 * second one and its selector in the control register is 1. */
#define FUXI_AMP_MI2S_SCK_GPIO 121u
#define FUXI_AMP_MI2S_DATA0_GPIO 122u
#define FUXI_AMP_MI2S_WS_GPIO 123u
#define FUXI_AMP_MI2S_DATA1_GPIO 124u
/* The pin control register's fields (pinctrl-sm8550.c): bits 1:0 the pull
 * (1 down, 3 up), bits 5:2 the function, bits 8:6 the drive strength
 * ((mA / 2) - 1), bit 9 the output enable, bit 12 the eGPIO enable. */
#define FUXI_AMP_PULL_DOWN 1u
#define FUXI_AMP_PULL_UP 3u
#define FUXI_AMP_OUTPUT_ENABLE (1u << 9)
#define FUXI_AMP_EGPIO_ENABLE (1u << 12)
/* Bit 11 says the pin carries an eGPIO function; Linux sets bit 12 when it
 * takes ownership of such a pin (pinctrl-msm.c, msm_pinctrl_set_mux). */
#define FUXI_AMP_EGPIO_PRESENT (1u << 11)
/* The input/output register's bits: bit 0 the input level, bit 1 the output
 * level (pinctrl-msm.c's in_bit and out_bit). */
#define FUXI_AMP_IO_INPUT 1u
#define FUXI_AMP_IO_OUTPUT (1u << 1)
/* The I2C pins' function in the fuxi tree: qupv3_se8_i2c_pins uses
 * qup2_se0_l0_mirb (SDA) and qup2_se0_l1_mirb (SCL).  In Linux's
 * pinctrl-sm8550.c those two groups are the second and third entries of
 * the pingroup's function list (the first is the GPIO function), so the
 * function selector in the TLMM control register is 2 for both pins -
 * not 0 and 1, which would select the GPIO function. */
#define FUXI_AMP_I2C_FUNC 2u
/* The MI2S function selector (the I2S1 group) in the control register. */
#define FUXI_AMP_MI2S_FUNC 1u
/* The pins' drive strength: the tree says 0x02, and Linux encodes the
 * drive strength in 2 mA steps as (mA / 2) - 1, so 2 mA is the code 0. */
#define FUXI_AMP_I2C_DRIVE_MA 2u
/* The reset pins' drive strength: the tree says 0x10, which is 16 mA and
 * the code 7. */
#define FUXI_AMP_RESET_DRIVE_MA 16u
/* The MI2S pins' drive strength: the tree's aux1_pcm_*_active nodes ask for
 * 8 mA, which is the code 3. */
#define FUXI_AMP_MI2S_DRIVE_MA 8u

/* The GENI engine's clock settings for 400 kHz from 19.2 MHz
 * (i2c-qcom-geni.c): divider 2, high 5, low 11, cycle 22. */
#define FUXI_AMP_CLK_CFG (2u << 4 | 1u)
#define FUXI_AMP_SCL_COUNTERS (5u << 20 | 11u << 10 | 22u)
/* The DFS level the I2C clock comes from: 19.2 MHz. */
#define FUXI_AMP_DFS_HZ 19200000ULL

/* The pin control word for a pin: the function, the drive strength in mA and
 * the pull. */
static inline uint32_t fuxi_amp_pin_cfg(unsigned func, unsigned ma, uint32_t pull)
{
    return (pull & 3u) | (func & 15u) << 2 | ((ma / 2 - 1) & 7u) << 6;
}

/* The I2C pins' control word: the QUP2 SE0 function, 2 mA and a pull-up.
 * The tree asks for a plain pull-up, and Linux only sets the strong pull-up
 * bit when the device tree asks for 2200 uA explicitly, so the strong bit
 * is not set here. */
static inline uint32_t fuxi_amp_i2c_pin_cfg(void)
{
    return fuxi_amp_pin_cfg(FUXI_AMP_I2C_FUNC, FUXI_AMP_I2C_DRIVE_MA,
                            FUXI_AMP_PULL_UP);
}

/* The reset pins' control word: the GPIO function, 16 mA, no pull.  The
 * tree's cirrus_reset_default_* nodes ask for the output to be driven high
 * with the bias disabled. */
static inline uint32_t fuxi_amp_reset_pin_cfg(void)
{
    return fuxi_amp_pin_cfg(0u, FUXI_AMP_RESET_DRIVE_MA, 0u);
}

/* The MI2S pins' control word: the I2S1 function, 8 mA, no bias.  The
 * tree's aux1_pcm_*_active nodes ask for the drive strength without a bias,
 * and only the clock and the word select carry the output-high setting,
 * which the TLMM's direction register holds rather than the control word. */
static inline uint32_t fuxi_amp_mi2s_pin_cfg(void)
{
    return fuxi_amp_pin_cfg(FUXI_AMP_MI2S_FUNC, FUXI_AMP_MI2S_DRIVE_MA, 0u);
}

/* The address of a pin's control register and of its input/output register. */
static inline uint64_t fuxi_amp_tlmm_ctl(unsigned gpio)
{
    return FUXI_AMP_TLMM_BASE + (uint64_t)gpio * FUXI_AMP_GPIO_STRIDE;
}

static inline uint64_t fuxi_amp_tlmm_io(unsigned gpio)
{
    return fuxi_amp_tlmm_ctl(gpio) + 4;
}

/* The DFS level whose frequency is the target, or 0xff when no level
 * matches.  The caller reads each level's CFG, M and N registers and passes
 * the resulting frequencies in; this keeps the arithmetic out of the probe. */
unsigned fuxi_amp_dfs_level(const uint64_t *level_hz, unsigned count, uint64_t target);

/* Whether a branch's clock is running.  Linux's clk_branch2_check_halt
 * (clk-branch.c) says: a branch that is coming up counts as running when
 * bit 31 (CBCR_CLK_OFF) is clear, or when the FSM status field (bits 30:28)
 * reads 2 (FSM_STATUS_ON).  A branch that is off has bit 31 set and the FSM
 * field reads something else. */
static inline int fuxi_amp_branch_running(uint32_t cbcr)
{
    if (!(cbcr & FUXI_AMP_CBCR_CLK_OFF)) return 1;
    return ((cbcr >> 28) & 0x7u) == 2u;
}

/* Whether a branch runs in hardware clock gating mode.  Linux's
 * clk_branch_in_hwcg_mode (clk-branch.c) reads the branch's hwcg register
 * and tests its hwcg bit; when that bit is set, clk_branch_wait returns
 * early and never looks at the halt bit at all.  The two QUP wrapper 2 AHB
 * branches are in this mode: gcc-sm8550.c gives them hwcg_reg equal to
 * their halt register and hwcg_bit 1, so their bit 31 says nothing about
 * whether the clock is on.  The table tells which of the five branches
 * that applies to; the other three have no hwcg register at all, so for
 * them bit 1 keeps its usual meaning and must not be read as hwcg. */
extern const uint8_t fuxi_amp_halt_hwcg[FUXI_AMP_HALTS_COUNT];

/* The name of each of those branches, in the same order, for the log. */
extern const char *const fuxi_amp_halt_names[FUXI_AMP_HALTS_COUNT];

/* The branch's clock is good when it is running, or when it is gated by
 * the hardware and Linux would not have waited for it anyway. */
static inline int fuxi_amp_branch_ok(uint32_t cbcr, unsigned hwcg_bit)
{
    if (fuxi_amp_branch_running(cbcr)) return 1;
    if (!hwcg_bit) return 0;
    return (cbcr & (1u << hwcg_bit)) ? 1 : 0;
}

#ifdef __cplusplus
}
#endif
#endif
