/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The corner the modem's RPMh proxy power domains are voted to, and the rule that picks it.
 *
 * Linux's drivers/pmdomain/qcom/rpmhpd.c decides the corner in two steps, and this is that
 * decision with nothing else in it:
 *
 *   - Linux's sm8550_mpss_resource carries no proxy_pd_performance_states, so
 *     qcom_pas_pds_enable asks for INT_MAX.
 *   - rpmhpd_set_performance_state walks the level list for the first entry whose level is at
 *     least the requested one. A request above every level lands on the last entry, because the
 *     loop ends with i == level_count and the code steps i back by one.
 *   - rpmhpd_power_on votes max(pd->corner, pd->enable_corner). The enable corner is the first
 *     corner with a non-zero level, skipping Retention, so it never exceeds the highest corner.
 *   - Whether the RPMh state has been synchronised makes no difference: the unsynced branch of
 *     rpmhpd_aggregate_corner uses level_count - 1, and the synced branch is handed the same
 *     corner.
 *
 * So the vote is the highest corner, and the port's vote of the last index is what Linux does.
 *
 * The level list is the command database's aux data for the rail, indexed by position: index 0
 * is Retention. Retention keeps its position in the index space; only its level value is left
 * out of the enable corner's search. The corner on the wire is the index, not the level value.
 */
#ifndef FUXI_RPMH_CORNER_H
#define FUXI_RPMH_CORNER_H
#include "QcomRpmh.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The Retention level, RPMH_REGULATOR_LEVEL_RETENTION in Linux's
 * include/dt-bindings/power/qcom,rpmhpd.h. The one level the enable corner's search skips. */
#define FUXI_RPMH_RETENTION_LEVEL 16u

/* The corner the modem's domains are voted to: the last index of the level list.
 *
 * Linux asks for INT_MAX and gets the highest corner; the port asks for the same thing by
 * asking for the level index of a request above every level. */
uint32_t fuxi_rpmh_highest_corner(const QcomRpmhResource *resource);

/* The corner Linux's enable corner is: the first index with a non-zero level, skipping
 * Retention. It never exceeds the highest corner, which is why the highest corner is the one
 * that is voted. */
uint32_t fuxi_rpmh_enable_corner(const QcomRpmhResource *resource);

#ifdef __cplusplus
}
#endif

#endif
