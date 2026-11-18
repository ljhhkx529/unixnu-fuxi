/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The modem's own secure-world and doorbell constants: the PAS ids of the modem image and its device tree blob,
 * the IPCC client the modem's doorbells arrive on, and the remoteproc's own registers.
 *
 * These live apart from QcomScm.h on purpose: that file is an unchanged copy from UniXNU-sheng and stays that
 * way, so anything the modem needs on top of it goes here.
 *
 * The IPCC client comes from Linux include/dt-bindings/mailbox/qcom-ipcc.h: IPCC_CLIENT_MPSS is 2, and the
 * modem's glink-edge and its SMP2P both ring on that client with IPCC_MPROC_SIGNAL_GLINK_QMP and
 * IPCC_MPROC_SIGNAL_SMP2P.
 *
 * The remoteproc register block at 0x04080000 is where the modem's own status lives: the same address the
 * board's device tree gives the remoteproc-mss node, and the base Linux's qcom_q6v5 uses for its "ready"
 * handshake with the secure world. */
#ifndef FUXI_MODEM_SCM_H
#define FUXI_MODEM_SCM_H

#include <stdint.h>

#include "QcomScm.h"

/* The PAS ids of the modem's two images live in FuxiModemPas.h, which owns them and the step order. */

/* The IPCC client the modem's doorbells arrive on. QcomScm.h has AOP (0) and LPASS (3) but not MPSS. */
#define FUXI_IPCC_CLIENT_MPSS 2u

/* The remoteproc register block of the modem. */
#define FUXI_MODEM_REMOTEPROC_BASE 0x04080000ull
#define FUXI_MODEM_REMOTEPROC_SIZE 0x10000ull

#endif
