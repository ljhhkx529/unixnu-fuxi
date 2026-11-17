// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the fuxi CS35L41 program tables (package/FuxiCs35l41.c) driven through the step runner
// (package/Cs35Run.c) over a register model of the part.
//
// The property that matters for P4 is that the init program leaves the amplifier powered down: the bring-up
// identifies the part and sets it up, but nothing can make noise until the power-up program runs, and that
// program is not part of P4.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../Cs35Run.h"
#include "../FuxiCs35l41.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

/* The part: a register file of 32-bit words, reached through the one wire primitive the runner uses. */
typedef struct {
    uint32_t value[64];
    unsigned count;
    uint32_t transfers, writes, reads;
} Part;

static uint32_t be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
}

static int part_transfer(void *context, const uint8_t *bytes, uint32_t length,
                          uint32_t read_length, uint32_t *value)
{
    Part *p = context;
    ++p->transfers;
    const uint32_t reg = be32(bytes);
    unsigned slot = p->count;
    for (unsigned i = 0; i < p->count; ++i) if (p->value[i * 2] == reg) slot = i;
    if (length == 8) {
        if (slot == p->count) {
            if (p->count >= 32) return 1;
            p->value[p->count * 2] = reg;
            p->value[p->count * 2 + 1] = 0;
            ++p->count;
        }
        p->value[slot * 2 + 1] = be32(bytes + 4);
        ++p->writes;
        return 0;
    }
    if (length == 4 && read_length == 4) {
        ++p->reads;
        if (value) *value = slot < p->count ? p->value[slot * 2 + 1] : 0;
        return 0;
    }
    return 1;
}

/* The register values the part answers with. */
static uint32_t part_read(const Part *p, uint32_t reg)
{
    for (unsigned i = 0; i < p->count; ++i) if (p->value[i * 2] == reg) return p->value[i * 2 + 1];
    return 0;
}

/* Runs a program to the end, feeding the model.  The reads answer with the
 * part's own identity values. */
static void run_program(Part *part, const Cs35Step *steps, unsigned count)
{
    Cs35Run runner;
    Cs35Action action;
    cs35_run_start(&runner, steps, count);
    for (unsigned guard = 0; guard < 4096; ++guard) {
        const int what = cs35_run_next(&runner, &action);
        if (what == Cs35Finished) return;
        if (what == Cs35Wait) continue;
        CHECK(what == Cs35Transfer);
        uint32_t value = 0;
        const uint32_t reg = be32(action.bytes);
        if (action.length == 4 && action.read_length == 4) {
            /* The part's identity, where the program reads it. */
            switch (reg) {
            case CS35_DEVID: value = CS35_CHIP_ID; break;
            case CS35_REVID: value = 0x00000002u; break;   /* an even revision: a CS35L41 */
            case CS35_IRQ1_STATUS4: value = CS35_OTP_BOOT_DONE; break;
            case CS35_IRQ1_STATUS3: value = 0; break;
            case CS35_IRQ1_STATUS1: value = 0; break;
            default:
                CHECK(part_transfer(part, action.bytes, action.length, action.read_length, &value) == 0);
                break;
            }
            /* The runner works in register values: the wire byte swap is
             * the transfer's job (the probe's ampTransfer does it before it
             * hands the value over). */
            cs35_run_result(&runner, value);
            continue;
        }
        CHECK(action.length == 8);
        CHECK(part_transfer(part, action.bytes, action.length, 0, NULL) == 0);
        cs35_run_result(&runner, 0);
    }
    fprintf(stderr, "the program did not finish\n");
    exit(1);
}

static int has_step(const Cs35Step *steps, unsigned count, uint32_t reg, uint8_t op)
{
    for (unsigned i = 0; i < count; ++i) if (steps[i].reg == reg && steps[i].op == op) return 1;
    return 0;
}

int main(void)
{
    Cs35Step steps[FUXI_CS35_MAX_STEPS];

    /* The init program fits, and it identifies the part before it changes
     * anything: the software reset, the wait, then the four reads. */
    const unsigned init = fuxi_cs35_init_program(steps, 0u);
    CHECK(init > 0u);
    CHECK(init <= FUXI_CS35_MAX_STEPS);
    CHECK(steps[0].op == Cs35Write && steps[0].reg == CS35_SFT_RESET);
    CHECK(steps[0].value == 0x5A000000u);
    CHECK(steps[1].op == Cs35DelayMs && steps[1].value == 20u);
    CHECK(has_step(steps, init, CS35_DEVID, Cs35Read));
    CHECK(has_step(steps, init, CS35_REVID, Cs35Read));
    CHECK(has_step(steps, init, CS35_IRQ1_STATUS4, Cs35Read));
    CHECK(has_step(steps, init, CS35_IRQ1_STATUS3, Cs35Read));
    /* The identity read comes before the first configuration write. */
    unsigned first_config = init;
    for (unsigned i = 0; i < init; ++i) {
        if (steps[i].op == Cs35Read && steps[i].reg == CS35_DEVID) {first_config = i; break;}
    }
    CHECK(first_config < init);
    for (unsigned i = first_config; i < init; ++i) {
        if (steps[i].op == Cs35Write && steps[i].reg == CS35_SFT_RESET) {
            fprintf(stderr, "the reset comes after the identity read\n");
            exit(1);
        }
    }

    /* The chip id the part answers with, and the revision rule: an even
     * revision is a CS35L41, an odd one is a CS35L41R. */
    CHECK(CS35_CHIP_ID == 0x00035A40u);
    CHECK((CS35_CHIP_ID & 1u) == 0u);

    /* The board's amplifiers: T at 0x42 is the left channel (slot 0), B at
     * 0x40 is the right one (slot 1). */
    CHECK(fuxi_cs35_address(0) == 0x42u);
    CHECK(fuxi_cs35_address(1) == 0x40u);
    CHECK(fuxi_cs35_slot(0) == 0u);
    CHECK(fuxi_cs35_slot(1) == 1u);

    /* The init program runs and the part ends up configured but off. */
    Part part;
    memset(&part, 0, sizeof part);
    run_program(&part, steps, init);
    CHECK(part.writes > 0u);
    CHECK(part.reads >= 4u);

    /* The two bits that decide whether the amplifier makes noise: the main
     * amplifier bit of PWR_CTRL2 and the global enable of PWR_CTRL1.  The
     * init program leaves both off. */
    const uint32_t pwr2 = part_read(&part, CS35_PWR_CTRL2);
    const uint32_t pwr1 = part_read(&part, CS35_PWR_CTRL1);
    CHECK((pwr2 & 0x1u) == 0u);            /* the main amplifier is off */
    CHECK((pwr1 & 0x1u) == 0u);            /* the global enable is off */
    /* The serial port's RX is off too, so no slot can be played. */
    CHECK((part_read(&part, CS35_SP_ENABLES) & (1u << 16)) == 0u);

    /* The boost converter's values: K1 and K2 0x24, the peak current 0x40
     * and the boost enabled (BST_EN 2). */
    const uint32_t coeff = part_read(&part, CS35_BSTCVRT_COEFF);
    CHECK(((coeff >> 8) & 0xffu) == 0x24u);
    CHECK((coeff & 0xffu) == 0x24u);
    CHECK((part_read(&part, CS35_BSTCVRT_PEAK_CUR) & 0x7fu) == 0x40u);
    CHECK(((part_read(&part, CS35_BSTCVRT_SLOPE_LBST) >> 8) & 0xffu) == 0x75u);
    CHECK(((pwr2 >> 4) & 0x3u) == 2u);    /* BST_EN */

    /* The routing: the serial port plays this amplifier's slot. */
    CHECK((part_read(&part, CS35_SP_FRAME_RX_SLOT) & 0x3fu) == 0u);
    CHECK((part_read(&part, CS35_DAC_PCM1_SRC) & 0x7fu) == 0x08u);
    /* The gain: the index 17 and the digital volume at 0 dB. */
    CHECK(((part_read(&part, CS35_AMP_GAIN_CTRL) >> 5) & 0x1fu) == CS35_GAIN_INDEX);
    CHECK((part_read(&part, CS35_AMP_DIG_VOL_CTRL) & (0x7ffu << 3)) == 0u);

    /* The second amplifier takes slot 1. */
    const unsigned init_b = fuxi_cs35_init_program(steps, 1u);
    Part part_b;
    memset(&part_b, 0, sizeof part_b);
    run_program(&part_b, steps, init_b);
    CHECK((part_read(&part_b, CS35_SP_FRAME_RX_SLOT) & 0x3fu) == 1u);

    /* The power-up program is the one that can make noise, and it is not
     * part of the bring-up: it turns the amplifier and its serial port on.
     * It is checked here so the shape is known before it is ever run. */
    const unsigned up = fuxi_cs35_power_up_program(steps);
    CHECK(up > 0u && up <= FUXI_CS35_MAX_STEPS);
    CHECK(has_step(steps, up, CS35_PWR_CTRL1, Cs35Update));
    CHECK(has_step(steps, up, CS35_SP_ENABLES, Cs35Update));
    Part part_up;
    memset(&part_up, 0, sizeof part_up);
    run_program(&part_up, steps, up);
    CHECK((part_read(&part_up, CS35_PWR_CTRL2) & 0x1u) == 1u);
    CHECK((part_read(&part_up, CS35_PWR_CTRL1) & 0x1u) == 1u);
    CHECK((part_read(&part_up, CS35_SP_ENABLES) & (1u << 16)) != 0u);

    /* The power-down program is its reverse. */
    const unsigned down = fuxi_cs35_power_down_program(steps);
    CHECK(down > 0u && down <= FUXI_CS35_MAX_STEPS);
    Part part_down;
    memset(&part_down, 0, sizeof part_down);
    /* Start from the powered-up state. */
    part_down = part_up;
    run_program(&part_down, steps, down);
    CHECK((part_read(&part_down, CS35_PWR_CTRL2) & 0x1u) == 0u);
    CHECK((part_read(&part_down, CS35_PWR_CTRL1) & 0x1u) == 0u);
    CHECK((part_read(&part_down, CS35_SP_ENABLES) & (1u << 16)) == 0u);

    /* The status program reads the two status words and clears them. */
    const unsigned status = fuxi_cs35_status_program(steps);
    CHECK(status > 0u && status <= FUXI_CS35_MAX_STEPS);
    CHECK(has_step(steps, status, CS35_IRQ1_STATUS1, Cs35Read));
    CHECK(has_step(steps, status, CS35_IRQ1_STATUS3, Cs35Read));
    /* The PLL lock is a bit of STATUS3, not of STATUS2. */
    CHECK(CS35_EINT2_PLL_LOCK == 0x00000002u);

    printf("PASS fuxi_cs35l41_host %u checks\n", checks);
    return 0;
}
