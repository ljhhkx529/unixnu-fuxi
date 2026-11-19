/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The host check of the modem's RSC layout choice.
 *
 * It pins the one thing that decides whether a vote can complete at all: which of the RSC's two
 * register layouts the port writes to. The id register at the driver's base names the major
 * version, and on this board it reads zero. Zero would choose the version 2_7 offsets, whose
 * command and wait registers sit at different places than the version 3_0 ones, so the request
 * would be written to registers that are not the request registers and the done bit would never
 * arrive. The ADSP's bring-up proved on the device that the version 3_0 layout is the one that
 * works here, so a zero id register must not be believed.
 *
 * The real headers are checked, not a model of them: FuxiRscLayout.h is the one the probe uses.
 */
#include <stdio.h>

#include "FuxiRscLayout.h"

static int failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* A readable id register is believed, whatever it says. */
static void test_readable_id_is_believed(void)
{
    const uint32_t major3 = 3u << 16;
    const uint32_t major2 = 2u << 16;
    CHECK(fuxi_rsc_drv_id(major3) == major3, "a major version 3 id register is taken as it is");
    CHECK(fuxi_rsc_drv_id(major2) == major2, "a major version 2 id register is taken as it is");
    CHECK(fuxi_rsc_major_version(major3) == 3, "the major version is in bits 23:16");
    CHECK(fuxi_rsc_major_version(major2) == 2, "the major version is in bits 23:16");
    /* The minor version sits below and must not be mistaken for the major one. */
    CHECK(fuxi_rsc_major_version(0x03020000u) == 2, "the minor version does not reach the major one");
}

/* A zero id register is the board's own value, and it must not choose the layout. */
static void test_zero_id_forces_major_three(void)
{
    CHECK(fuxi_rsc_drv_id(0) == FUXI_RSC_MAJOR_VERSION_3,
          "a zero id register becomes the major version 3 this board has");
    CHECK(fuxi_rsc_major_version(fuxi_rsc_drv_id(0)) == 3,
          "the layout's major version is 3 after the fallback");
}

/* The two layouts differ, so the fallback is a real choice and not a no-op. */
static void test_the_two_layouts_differ(void)
{
    const QcomRscLayout v2 = qcom_rsc_layout(2u << 16);
    const QcomRscLayout v3 = qcom_rsc_layout(3u << 16);
    CHECK(v2.cmd_stride != v3.cmd_stride, "the two layouts have different command strides");
    CHECK(v2.wait_for_cmpl != v3.wait_for_cmpl, "the two layouts have different wait registers");
    CHECK(v2.control != v3.control, "the two layouts have different control registers");
    CHECK(v2.cmd_msgid != v3.cmd_msgid, "the two layouts have different message id registers");
    /* The stride between TCSs is the same in both, so the address arithmetic does not change. */
    CHECK(v2.tcs_stride == v3.tcs_stride, "the TCS stride is the same in both layouts");
}

/* The board's own layout is the version 3_0 one. */
static void test_the_board_layout_is_v3(void)
{
    const QcomRscLayout board = fuxi_rsc_layout(0);
    const QcomRscLayout v3 = qcom_rsc_layout(3u << 16);
    const QcomRscLayout v2 = qcom_rsc_layout(2u << 16);
    CHECK(board.cmd_stride == v3.cmd_stride, "the board's layout is the version 3_0 one");
    CHECK(board.cmd_stride != v2.cmd_stride, "the board's layout is not the version 2_7 one");
    CHECK(board.wait_for_cmpl == v3.wait_for_cmpl, "the board's wait register is the version 3_0 one");
    /* The version 3_0 offsets, spelled out, so a change to the table is noticed here. */
    CHECK(board.cmd_stride == 24, "the version 3_0 command stride is 24");
    CHECK(board.wait_for_cmpl == 0x20, "the version 3_0 wait register is at 0x20");
    CHECK(board.control == 0x24, "the version 3_0 control register is at 0x24");
    CHECK(board.status == 0x28, "the version 3_0 status register is at 0x28");
    CHECK(board.cmd_enable == 0x2c, "the version 3_0 command enable register is at 0x2c");
    CHECK(board.cmd_msgid == 0x34, "the version 3_0 message id register is at 0x34");
    CHECK(board.cmd_addr == 0x38, "the version 3_0 address register is at 0x38");
    CHECK(board.cmd_data == 0x3c, "the version 3_0 data register is at 0x3c");
    /* The interrupt registers are the same in both tables. */
    CHECK(board.irq_status == 0x04, "the interrupt status register is at 0x04");
    CHECK(board.irq_clear == 0x08, "the interrupt clear register is at 0x08");
}

int main(void)
{
    test_readable_id_is_believed();
    test_zero_id_forces_major_three();
    test_the_two_layouts_differ();
    test_the_board_layout_is_v3();
    if (failures) {
        printf("FAIL fuxi_rsc_layout_test: %d\n", failures);
        return 1;
    }
    printf("PASS fuxi_rsc_layout_test\n");
    return 0;
}
