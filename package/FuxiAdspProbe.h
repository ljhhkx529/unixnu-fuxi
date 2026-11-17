/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef FUXI_ADSP_PROBE_H
#define FUXI_ADSP_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Board-owned operations extracted from Sheng's AdspProbe. Each operation is
 * installed independently so bringing up one transport never implies that the
 * next register or secure-world operation is safe to execute. */
typedef struct {
    int (*firmware_source)(void *context);
    int (*map_carveout)(void *context, uint64_t base, uint64_t size);
    int (*pas_auth_reset)(void *context);
    int (*smp2p_setup)(void *context);
    int (*ipcc_attach)(void *context);
    int (*qmp_load_state)(void *context, const char *message);
    int (*glink_setup)(void *context);
    void (*rollback)(void *context, uint32_t completed_mask);
    void *context;
} FuxiAdspPlatformHooks;

enum {
    FuxiAdspHookFirmware = 1u << 0,
    FuxiAdspHookCarveout = 1u << 1,
    FuxiAdspHookPas = 1u << 2,
    FuxiAdspHookSmp2p = 1u << 3,
    FuxiAdspHookIpcc = 1u << 4,
    FuxiAdspHookQmp = 1u << 5,
    FuxiAdspHookGlink = 1u << 6,
    FuxiAdspHookAll = 0x7fu,
};

/* Stable CDC-readable diagnostics.  Word 0 is the ABI (currently 2):
 * 1 mode, 2 status, 3..6 binding counts, 7 installed hooks, 8 completed hooks,
 * 9 stage, 10 last error, 11 failed stage (negative), 12..15 DTB/ADSP KiB and
 * segment counts, 16/17 last SCM x0/x1, 18 SCM op, 19 PAS id, 20 SMP2P inbound
 * bits, 21 IPCC interrupts, 22 GLINK flags/errors, 23 RX, 24 TX, 25 unknown
 * opens, 26/27 last GPR opcode/status, 28 SPF state/counters, 29 QMP state,
 * 30 image/limit placement, 31 ready/fatal/handover flags.  32..39 hold
 * the metadata hand-off; 40..43 the preserved SCM result and resume count;
 * 44..47 SMP2P presence/index/RSC/crash word; 48..55 segment/auth evidence;
 * 56..63 crash size/text and the QMP/SMP2P order counters. */
extern volatile uint32_t native_fuxi_adsp_stats[128];

int fuxi_adsp_install_platform(const FuxiAdspPlatformHooks *hooks);
/* Opens and parses the two signed images from modem_a/modem_b without mapping,
 * authenticating or starting either PAS. Used by mode 1 inventory. */
int fuxi_adsp_inventory_firmware(void);
void native_fuxi_adsp_probe_mode(unsigned mode);
void native_fuxi_adsp_probe(void);

#ifdef __cplusplus
}
#endif
#endif
