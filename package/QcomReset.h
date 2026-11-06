/* SPDX-License-Identifier: MIT */
/*
 * Qualcomm reset layer for fuxi.
 *
 * One entry point for every reset mode, so a control channel never touches a register: the CDC
 * console (or UART, or anything else added later) asks for a mode and this layer decides which
 * backend commits it. The two backends known to work on this board are a plain PSCI SYSTEM_RESET
 * and a PMIC SDAM reboot-reason write followed by the same reset, which is what makes ABL land in
 * fastboot instead of booting the payload again.
 *
 * Nothing here allocates, sleeps or logs from the commit path: the panic caller runs with the
 * console already torn down.
 */
#ifndef QCOM_RESET_H
#define QCOM_RESET_H

#include <stdint.h>

typedef enum {
    QCOM_RESET_NORMAL = 0,     /* PSCI SYSTEM_RESET: back to whatever the boot partition holds */
    QCOM_RESET_BOOTLOADER,     /* reboot reason = bootloader, then reset: ABL enters fastboot */
    QCOM_RESET_RECOVERY,       /* not implemented on this board yet */
    QCOM_RESET_EDL,            /* not implemented on this board yet */
} qcom_reset_mode_t;

typedef enum {
    QCOM_RESET_OK = 0,         /* request accepted and committed; this does not return */
    QCOM_RESET_UNSUPPORTED,    /* no backend for that mode on this board */
    QCOM_RESET_NOT_READY,      /* backend prerequisites missing (mappings, arbiter, halt hook) */
    QCOM_RESET_BUSY,           /* another request is already committing */
    QCOM_RESET_FAILED,         /* the backend ran and the SoC stayed up */
} qcom_reset_result_t;

/* Map the reset arbiter and resolve the SDAM APID. Idempotent, cheap, safe to call at init time;
 * must run in normal context because it maps device memory. Returns 0 when the bootloader backend
 * is usable. */
int qcom_reset_prepare_bootloader(void);

/* 1 when qcom_reset_prepare_bootloader() succeeded, so a caller can report why it cannot offer the
 * fastboot mode without attempting a reset. */
int qcom_reset_bootloader_ready(void);

/* Route a request to its backend. Only NORMAL and BOOTLOADER are wired; the other modes return
 * QCOM_RESET_UNSUPPORTED without touching hardware. */
qcom_reset_result_t qcom_reset_request(qcom_reset_mode_t mode);

/* Commit the bootloader backend directly: set the reboot reason, verify it, then reset. Used by the
 * panic path, which has already waited for its logs to drain. 0 only if the SoC refused to reset. */
int qcom_reset_to_bootloader(void);

#endif /* QCOM_RESET_H */
