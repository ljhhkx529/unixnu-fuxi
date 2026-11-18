/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FuxiRpmhCorner.h"

uint32_t fuxi_rpmh_highest_corner(const QcomRpmhResource *resource)
{
    if (!resource || !resource->level_count) return 0;
    return resource->level_count - 1;
}

uint32_t fuxi_rpmh_enable_corner(const QcomRpmhResource *resource)
{
    if (!resource || !resource->level_count) return 0;
    /* The Retention level keeps its place in the index space, so the search starts past it
     * rather than skipping it, and the first corner with a non-zero level is the enable one. */
    for (uint32_t i = 0; i < resource->level_count; ++i) {
        if (resource->levels[i] == FUXI_RPMH_RETENTION_LEVEL) continue;
        if (resource->levels[i]) return i;
    }
    return 0;
}
