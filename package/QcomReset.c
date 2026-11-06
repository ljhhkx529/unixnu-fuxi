/* SPDX-License-Identifier: MIT */
/*
 * Qualcomm reset backends for fuxi.
 *
 * NORMAL reuses the platform's halt hook, which is the path that already resets this board.
 * BOOTLOADER writes the PMIC SDAM reboot-reason cell through the Boot Reset Arbiter (the cell ABL
 * reads before it picks a boot target) and then resets through the same firmware call. The register
 * facts and the arbiter's version-7 layout live in BootResetCore.h, which is pure pointer code so it
 * can be driven against a model host-side.
 *
 * Both paths end in PSCI SYSTEM_RESET. The SMC arguments are the ones this board already takes for a
 * warm reset; there is no return value to inspect, so a return means the SoC refused to reset.
 */
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <arm/machine_routines.h>

#include "QcomReset.h"
#include "BootResetCore.h"

/* SM8550 devicetree: the APSS side of the reset arbiter and its two channel windows. */
#define QCOM_RESET_CORE_ADDR   0x0c400000u
#define QCOM_RESET_CORE_BYTES  0x3000u
#define QCOM_RESET_CNFG_ADDR   0x0c42d000u
#define QCOM_RESET_CNFG_BYTES  0x4000u
#define QCOM_RESET_OBS_ADDR    0x0c440000u
#define QCOM_RESET_CHN_ADDR    0x0c500000u
#define QCOM_RESET_PAGE        0x1000u

/* Linker-checked against the boot arg: the CDC/panic policy waits 30 seconds before asking. */
#define QCOM_RESET_PANIC_ARG   "fuxi-panic-fastboot"

static struct br_arbiter g_arbiter;
static volatile uint32_t *g_observer;
static volatile uint32_t *g_channel;
static int g_apid = -1;

enum { QCOM_STATE_IDLE = 0, QCOM_STATE_REQUESTED = 1, QCOM_STATE_COMMITTING = 2, QCOM_STATE_FAILED = 3 };
static volatile unsigned int g_state = QCOM_STATE_IDLE;

/* PSCI SYSTEM_RESET. w0 = 0x84000009; the other registers are zero as the calling convention wants. */
static void qcom_reset_firmware(void)
{
    __asm__ volatile(
        "movz w0, #9\n"
        "movk w0, #0x8400, lsl #16\n"
        "mov x1, xzr\n"
        "mov x2, xzr\n"
        "mov x3, xzr\n"
        "dsb sy\n"
        "smc #0\n"
        :
        :
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
          "x13", "x14", "x15", "x16", "x17", "memory", "cc");
}

int qcom_reset_bootloader_ready(void)
{
    return g_apid >= 0;
}

int qcom_reset_prepare_bootloader(void)
{
    struct br_found found;
    vm_offset_t core, cnfg, obs, chn;
    unsigned int offset;

    if (g_apid >= 0) return 0;
    if (g_state != QCOM_STATE_IDLE) return 1;

    core = ml_io_map(QCOM_RESET_CORE_ADDR, QCOM_RESET_CORE_BYTES);
    cnfg = ml_io_map(QCOM_RESET_CNFG_ADDR, QCOM_RESET_CNFG_BYTES);
    if (!core || !cnfg) {
        kprintf("QcomReset: reset arbiter mapping failed (core=%d cnfg=%d)\n", core != 0, cnfg != 0);
        return 2;
    }
    g_arbiter.core = (volatile uint32_t *)core;
    g_arbiter.cnfg = (volatile uint32_t *)cnfg;
    g_arbiter.ee = 0;
    if (br_find(&g_arbiter, &found)) return 3;
    if (found.owner != 0) {
        kprintf("QcomReset: SDAM APID %d belongs to EE %d, not APSS\n", found.apid, found.owner);
        return 4;
    }

    offset = BR_OBS_APID_STRIDE * (unsigned int)found.apid;
    obs = ml_io_map(QCOM_RESET_OBS_ADDR + (offset & ~(QCOM_RESET_PAGE - 1u)), QCOM_RESET_PAGE);
    chn = ml_io_map(QCOM_RESET_CHN_ADDR + BR_CHNL_APID_STRIDE * (unsigned int)found.apid, QCOM_RESET_PAGE);
    if (!obs || !chn) {
        kprintf("QcomReset: reset channel mapping failed (obs=%d chn=%d)\n", obs != 0, chn != 0);
        return 5;
    }
    g_observer = (volatile uint32_t *)(obs + (offset & (QCOM_RESET_PAGE - 1u)));
    g_channel = (volatile uint32_t *)chn;
    g_apid = found.apid;
    kprintf("QcomReset: arbiter v%x apids=%u apid=%d ready\n", found.version, found.apids, found.apid);
    return 0;
}

int qcom_reset_to_bootloader(void)
{
    uint8_t before = 0, after = 0;
    int result;

    if (g_apid < 0) return QCOM_RESET_NOT_READY;
    result = br_set_bootloader(g_observer, g_channel, &g_arbiter, 0, &before, &after);
    if (result) {
        kprintf("QcomReset: reboot reason write failed (%d), reason %02x\n", result, before);
        return 1;
    }
    qcom_reset_firmware();
    /* The reason cell now says bootloader, so a natural reboot lands in fastboot even if the SMC was
     * ignored; the request is not lost, but the caller still learns that this attempt did not reset. */
    return 2;
}

qcom_reset_result_t qcom_reset_request(qcom_reset_mode_t mode)
{
    switch (mode) {
    case QCOM_RESET_BOOTLOADER:
        if (g_apid < 0 && qcom_reset_prepare_bootloader()) return QCOM_RESET_NOT_READY;
        if (__atomic_exchange_n(&g_state, QCOM_STATE_COMMITTING, __ATOMIC_ACQ_REL) != QCOM_STATE_IDLE) {
            __atomic_store_n(&g_state, QCOM_STATE_COMMITTING, __ATOMIC_RELEASE);
            return QCOM_RESET_BUSY;
        }
        (void)qcom_reset_to_bootloader();
        __atomic_store_n(&g_state, QCOM_STATE_FAILED, __ATOMIC_RELEASE);
        return QCOM_RESET_FAILED;

    case QCOM_RESET_NORMAL:
        if (__atomic_exchange_n(&g_state, QCOM_STATE_COMMITTING, __ATOMIC_ACQ_REL) != QCOM_STATE_IDLE) {
            __atomic_store_n(&g_state, QCOM_STATE_COMMITTING, __ATOMIC_RELEASE);
            return QCOM_RESET_BUSY;
        }
        /* Not PE_halt_restart: that path unmounts the root volume first, which never completes on this
         * UFS driver. The firmware reset is the same one the bootloader mode ends with. */
        qcom_reset_firmware();
        __atomic_store_n(&g_state, QCOM_STATE_FAILED, __ATOMIC_RELEASE);
        return QCOM_RESET_FAILED;

    case QCOM_RESET_RECOVERY:
    case QCOM_RESET_EDL:
    default:
        return QCOM_RESET_UNSUPPORTED;
    }
}
