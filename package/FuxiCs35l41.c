/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The fuxi CS35L41 program tables.  Derived from Kerfand's
 * UniXNU-Fuxi-Arm64e-iOS17 (device/package/FuxiCs35l41.c, BSD-2-Clause-Patent,
 * 2026-11-13), which derived them from the Linux mainline driver.  The step
 * runner is the local one (Cs35Run.c), which has the same opcodes and the
 * same layout, so only the tables are carried over.
 *
 * One difference from the reference: the local runner applies the update
 * value as it stands, so every read-modify-write value here is already
 * masked with the bits it touches. */
#include "FuxiCs35l41.h"

typedef struct {Cs35Step *p; unsigned n;} P;

static void w(P *p, uint32_t reg, uint32_t value)
{
    if (p->n < FUXI_CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35Write, reg, 0, value};
}
static void u(P *p, uint32_t reg, uint32_t mask, uint32_t value)
{
    if (p->n < FUXI_CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35Update, reg, mask, value & mask};
}
static void r(P *p, uint32_t reg)
{
    if (p->n < FUXI_CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35Read, reg, 0, 0};
}
static void d(P *p, uint32_t ms)
{
    if (p->n < FUXI_CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35DelayMs, 0, 0, ms};
}
static void unlock(P *p) {w(p, CS35_TEST_KEY, 0x55u); w(p, CS35_TEST_KEY, 0xaau);}
static void lock(P *p) {w(p, CS35_TEST_KEY, 0xccu); w(p, CS35_TEST_KEY, 0x33u);}

unsigned fuxi_cs35_init_program(Cs35Step *out, unsigned slot)
{
    P p = {out, 0};
    /* The software reset and the OTP boot.  The runner cannot poll, so this
 * is one 20 ms wait; the two status words are read for the log. */
    w(&p, CS35_SFT_RESET, 0x5A000000u);
    d(&p, 20);
    r(&p, CS35_DEVID);
    r(&p, CS35_REVID);
    r(&p, CS35_IRQ1_STATUS4);
    r(&p, CS35_IRQ1_STATUS3);
    /* The rev B0/B2 errata patch, then the DSP core held off. */
    unlock(&p);
    w(&p, CS35_VIMON_SPKMON_RESYNC, 0);
    w(&p, 0x00004310u, 0);
    w(&p, CS35_VPVBST_FS_SEL, 0);
    w(&p, CS35_BSTCVRT_DCM_CTRL, 0x00000051u);
    w(&p, CS35_DSP1_YM_ACCEL_PL0_PRI, 0);
    w(&p, CS35_DSP1_XM_ACCEL_PL0_PRI, 0);
    w(&p, CS35_PWR_CTRL2, 0);
    w(&p, CS35_AMP_GAIN_CTRL, 0);
    w(&p, CS35_ASP_TX3_SRC, 0);
    w(&p, CS35_ASP_TX4_SRC, 0);
    w(&p, CS35_DSP1_CCM_CORE_CTRL, 0);
    lock(&p);
    /* The interrupts stay masked: the owner reads the status registers. */
    w(&p, CS35_IRQ1_MASK1, 0xffffffffu);
    w(&p, CS35_IRQ1_MASK2, 0xffffffffu);
    w(&p, CS35_IRQ1_MASK3, 0xffffffffu);
    w(&p, CS35_IRQ1_MASK4, 0xffffffffu);
    /* The boost converter: 1000 nH, 15 uF, 4000 mA peak.  K1 0x24, K2 0x24,
     * slope 0x75, LBST 0, IPK 0x40, and the boost enabled (BST_EN 2). */
    u(&p, CS35_BSTCVRT_COEFF, 0xffffu, 0x24u << 8 | 0x24u);
    u(&p, CS35_BSTCVRT_SLOPE_LBST, 0xff03u, 0x75u << 8);
    u(&p, CS35_BSTCVRT_PEAK_CUR, 0x7fu, (4000u - 1600u) / 50u + 0x10u);
    u(&p, CS35_PWR_CTRL2, 0x30u, 2u << 4);
    /* The board's values: SDOUT high impedance (cirrus,asp-sdout-hiz 3) and
 * GPIO2 as an output with source 4 (cirrus,gpio-config2). */
    u(&p, CS35_SP_HIZ_CTRL, 0x3u, 0x3u);
    u(&p, CS35_GPIO_PAD_CONTROL, 0x07000000u, 4u << 24);
    /* The format (I2S, clock and frame from the SoC) and the word length
 * (16 bit, 48 kHz). */
    u(&p, CS35_SP_FORMAT, 0xff000000u | 0x0755u, 16u << 24 | 2u << 8);
    u(&p, CS35_SP_RX_WL, 0x3fu, 16u);
    u(&p, CS35_GLOBAL_CLK_CTRL, 0x1fu, 0x03u);
    /* The PLL locks to the 1.536 MHz SCLK (reference 0, frequency code 0x1B),
 * with the FS monitor at 52 / 84. */
    u(&p, CS35_PLL_CLK_CTRL, 0x800u, 0x800u);
    u(&p, CS35_PLL_CLK_CTRL, 0x7e0u, 0x1bu << 5);
    u(&p, CS35_PLL_CLK_CTRL, 0x10u, 0);
    u(&p, CS35_PLL_CLK_CTRL, 0x7u, 0);
    u(&p, CS35_PLL_CLK_CTRL, 0x800u, 0);
    u(&p, CS35_PLL_CLK_CTRL, 0x10u, 0x10u);
    w(&p, CS35_TST_FS_MON0, 52u | (84u << 12));
    /* The PCM source ASPRX1, this amplifier's slot, the analog gain and the
     * digital volume at 0 dB. */
    u(&p, CS35_DAC_PCM1_SRC, 0x7fu, 0x08u);
    u(&p, CS35_SP_FRAME_RX_SLOT, 0x3fu, slot);
    u(&p, CS35_AMP_GAIN_CTRL, 0x3e0u, CS35_GAIN_INDEX << 5);
    u(&p, CS35_AMP_DIG_VOL_CTRL, 0x7ffu << 3, 0);
    return p.n;
}

unsigned fuxi_cs35_power_up_program(Cs35Step *out)
{
    P p = {out, 0};
    u(&p, CS35_SP_ENABLES, 1u << 16, 1u << 16);
    unlock(&p);
    w(&p, 0x00002084u, 0x002F1AA0u);
    lock(&p);
    u(&p, CS35_PWR_CTRL1, 1u, 1u);
    d(&p, 5);
    r(&p, CS35_IRQ1_STATUS1);
    w(&p, CS35_IRQ1_STATUS1, CS35_PUP_DONE);
    u(&p, CS35_PWR_CTRL2, 1u, 1u);
    return p.n;
}

unsigned fuxi_cs35_power_down_program(Cs35Step *out)
{
    P p = {out, 0};
    u(&p, CS35_PWR_CTRL2, 1u, 0);
    u(&p, CS35_PWR_CTRL1, 1u, 0);
    d(&p, 5);
    r(&p, CS35_IRQ1_STATUS1);
    w(&p, CS35_IRQ1_STATUS1, CS35_PDN_DONE);
    unlock(&p);
    w(&p, 0x00002084u, 0x002F1AA3u);
    lock(&p);
    u(&p, CS35_SP_ENABLES, 1u << 16, 0);
    return p.n;
}

unsigned fuxi_cs35_status_program(Cs35Step *out)
{
    P p = {out, 0};
    r(&p, CS35_IRQ1_STATUS1);
    r(&p, CS35_IRQ1_STATUS3);
    w(&p, CS35_IRQ1_STATUS1, 0xffffffffu);
    w(&p, CS35_IRQ1_STATUS3, 0xffffffffu);
    return p.n;
}

unsigned fuxi_cs35_mute_program(Cs35Step *out)
{
    P p = {out, 0};
    /* The digital volume to the part's mute code and the analog gain to
     * zero.  Both fields are read-modify-write, so the parts of the
     * registers this port does not own keep their values. */
    u(&p, CS35_AMP_DIG_VOL_CTRL, CS35_AMP_PCM_VOL_MASK, CS35_AMP_PCM_VOL_MUTE << CS35_AMP_PCM_VOL_SHIFT);
    u(&p, CS35_AMP_GAIN_CTRL, 0x3e0u, 0u);
    return p.n;
}

unsigned fuxi_cs35_unmute_program(Cs35Step *out, unsigned gain_index)
{
    P p = {out, 0};
    /* The digital volume back to 0 dB (the field counts down into the
     * attenuation, so 0 is the loud end) and the analog gain to the
     * given index. */
    u(&p, CS35_AMP_DIG_VOL_CTRL, CS35_AMP_PCM_VOL_MASK, 0u);
    u(&p, CS35_AMP_GAIN_CTRL, 0x3e0u, (gain_index & 0x1fu) << 5);
    return p.n;
}
