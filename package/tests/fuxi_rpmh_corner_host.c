/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The RPMh corner the modem's proxy power domains are voted to.
 *
 * The rule comes from Linux's drivers/pmdomain/qcom/rpmhpd.c and drivers/soc/qcom/rpmh.c, and
 * the map records it in PORTING_MAP.md. The test pins it, so the port's vote cannot drift from
 * what Linux does without the test failing.
 */
#include <stdio.h>
#include <string.h>
#include "../FuxiRpmhCorner.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {
        ++failed;
        printf("FAIL %s\n", what);
    }
}

int main(void)
{
    /* The levels of a rail, as the command database's aux data carries them: Retention first,
     * then the working levels. cx.lvl on this board has ten of them, mss.lvl seven. */
    const uint16_t cx_levels[10] = {16, 48, 128, 256, 384, 464, 480, 512, 576, 640};
    const uint16_t mss_levels[7] = {16, 48, 128, 256, 384, 464, 512};
    QcomRpmhResource cx = {0x30000u, {0}, 10};
    QcomRpmhResource mss = {0x30060u, {0}, 7};
    memcpy(cx.levels, cx_levels, sizeof cx_levels);
    memcpy(mss.levels, mss_levels, sizeof mss_levels);

    /* The corner that goes on the wire is the highest one. Linux asks for INT_MAX, because
     * sm8550_mpss_resource carries no proxy_pd_performance_states, and
     * rpmhpd_set_performance_state steps back one entry from the end when the request is above
     * every level. */
    check(fuxi_rpmh_highest_corner(&cx) == 9, "cx.lvl's highest corner is the last index");
    check(fuxi_rpmh_highest_corner(&mss) == 6, "mss.lvl's highest corner is the last index");

    /* The enable corner is the first corner with a non-zero level, with Retention skipped.
     * Retention keeps its place in the index space, so the enable corner is index 1 here, not
     * index 0. */
    check(fuxi_rpmh_enable_corner(&cx) == 1, "cx.lvl's enable corner is past Retention");
    check(fuxi_rpmh_enable_corner(&mss) == 1, "mss.lvl's enable corner is past Retention");

    /* The enable corner never exceeds the highest corner, which is why the highest corner is the
     * one that is voted: rpmhpd_power_on votes max(pd->corner, pd->enable_corner). */
    check(fuxi_rpmh_enable_corner(&cx) <= fuxi_rpmh_highest_corner(&cx),
          "the enable corner does not exceed the highest corner");
    check(fuxi_rpmh_enable_corner(&mss) <= fuxi_rpmh_highest_corner(&mss),
          "the enable corner does not exceed the highest corner");

    /* A rail whose first level is not Retention has its enable corner at index 0. */
    QcomRpmhResource plain = {0x30010u, {0}, 3};
    const uint16_t plain_levels[3] = {64, 256, 384};
    memcpy(plain.levels, plain_levels, sizeof plain_levels);
    check(fuxi_rpmh_enable_corner(&plain) == 0, "a rail without Retention enables at its first corner");

    /* A rail that is only Retention has no working corner, so the search falls back to index 0
     * rather than running past the list. */
    QcomRpmhResource retention = {0x30010u, {0}, 1};
    const uint16_t retention_levels[1] = {16};
    memcpy(retention.levels, retention_levels, sizeof retention_levels);
    check(fuxi_rpmh_enable_corner(&retention) == 0, "a rail with only Retention falls back to its first corner");

    /* A rail without levels has no corner at all. */
    QcomRpmhResource empty = {0x30010u, {0}, 0};
    check(fuxi_rpmh_highest_corner(&empty) == 0, "a rail without levels has no highest corner");
    check(fuxi_rpmh_enable_corner(&empty) == 0, "a rail without levels has no enable corner");
    check(fuxi_rpmh_highest_corner(NULL) == 0, "a missing rail has no highest corner");
    check(fuxi_rpmh_enable_corner(NULL) == 0, "a missing rail has no enable corner");

    /* The port's own vote is the highest index, and it agrees with what Linux asks for. The
     * level index of a request above every level is the highest corner. */
    uint32_t index = 0xffffffffu;
    check(qcom_rpmh_level_index(&cx, 0xffffffffu, &index) == QcomRpmhOk && index == 9,
          "the port's vote for cx.lvl is the highest corner");
    index = 0xffffffffu;
    check(qcom_rpmh_level_index(&mss, 0xffffffffu, &index) == QcomRpmhOk && index == 6,
          "the port's vote for mss.lvl is the highest corner");

    printf("PASS fuxi_rpmh_corner_test: %d checks\n", checks);
    return failed ? 1 : 0;
}
