/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The RSC's register layout, and the rule that picks it when the id register cannot be read.
 *
 * Linux's drivers/soc/qcom/rpmh-rsc.c reads the driver's id register at the driver's base and takes
 * the major version from bits 23:16. A major version of 3 or more means the version 3_0 offset
 * table, anything below it the version 2_7 table. The two tables differ: CMD_OFFSET is 24 against
 * 20, and the command and wait registers sit at different offsets. Picking the wrong one means
 * writing the request to registers that are not the request registers, and the vote never completes.
 *
 * On this board the DRV2 id register reads zero. That is a fact about the board, not a version:
 * zero would mean the version 2_7 layout, and the ADSP's bring-up proved on the device that the
 * version 3_0 layout is the one that works there ("forced v3 layout after DRV2 ID read 0; active
 * TCS 2; vote done"). The touch probe, which does not force the version, wrote its votes and never
 * saw the done bit.
 *
 * So the rule is: a readable id register is believed; a zero one is not, and SM8550's major
 * version 3 is used instead.
 */
#ifndef FUXI_RSC_LAYOUT_H
#define FUXI_RSC_LAYOUT_H
#include "QcomRpmh.h"
#ifdef __cplusplus
extern "C" {
#endif

/* SM8550's RSC major version, in the id register's own shape: the major version is bits 23:16. */
#define FUXI_RSC_MAJOR_VERSION_3 (3u << 16)

/* The id register's major version, when it is readable. */
static inline uint32_t fuxi_rsc_major_version(uint32_t drv_id)
{
    return (drv_id >> 16) & 0xffu;
}

/* The id register's value to use for the layout. A readable register is taken as it is; a zero one
 * is replaced by the major version this board's RSC actually has, because zero would otherwise
 * choose the version 2_7 offsets and the vote would never complete. */
static inline uint32_t fuxi_rsc_drv_id(uint32_t drv_id)
{
    return drv_id ? drv_id : FUXI_RSC_MAJOR_VERSION_3;
}

/* The layout for the board's RSC. */
static inline QcomRscLayout fuxi_rsc_layout(uint32_t drv_id)
{
    return qcom_rsc_layout(fuxi_rsc_drv_id(drv_id));
}

#ifdef __cplusplus
}
#endif
#endif
