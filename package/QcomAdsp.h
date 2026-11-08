/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* ADSP bring-up order, as a state machine that never waits.
 *
 * This layer owns the sequence and the conditions, not the hardware: the carveout, the authentication call,
 * the memory setup and every SMP2P entry name come from the caller, and the two steps that touch registers or
 * the secure world are owner hooks. Each step does a bounded amount of work and returns; the owner calls it
 * again on its next event, so a step can also mean "the DSP has not answered yet".
 *
 * The image, the mailbox, the SMP2P pair and the link are the board-independent layers beneath it, so the same
 * state machine runs in the host tests with those layers over memory. */
#ifndef QCOM_ADSP_H
#define QCOM_ADSP_H

#include <stdint.h>

#include "QcomGlink.h"
#include "QcomMdt.h"
#include "QcomQmp.h"
#include "QcomSmp2p.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    QcomAdspIdle = 0,
    QcomAdspPlacement = 1,  /* every loadable segment must land inside the carveout */
    QcomAdspAuth = 2,       /* the owner authenticates and resets the image */
    QcomAdspMemory = 3,     /* the owner sets the carveout up from the span */
    QcomAdspMailbox = 4,    /* the QMP handshake, then the load-state request */
    QcomAdspReady = 5,      /* waiting for the ready bits the DSP sets over SMP2P */
    QcomAdspLink = 6,       /* waiting for GLINK and the channel */
    QcomAdspUp = 7,
    QcomAdspFailed = -1,
};

enum {
    QcomAdspOk = 0,
    QcomAdspAgain = 1,        /* not this step: call again on the next event */
    QcomAdspBadImage = -1,    /* a segment does not fit the carveout, or the span is larger than it */
    QcomAdspBadConfig = -2,   /* a required name or hook is missing */
    QcomAdspAuthFailed = -3,
    QcomAdspMemoryFailed = -4,
    QcomAdspMailboxFailed = -5,
    QcomAdspReadyTimeout = -6,
    QcomAdspLinkFailed = -7,
    QcomAdspFatal = -8,       /* the DSP reported a fatal condition before it came up */
    QcomAdspWrongState = -9,
};

typedef struct {
    uint64_t carveout_base, carveout_size;
    const char *state_entry;          /* the outbound SMP2P entry that names our boot state */
    const char *ready_entry;          /* the inbound SMP2P entry holding the DSP's ready bits */
    const char *fatal_entry;          /* the inbound SMP2P entry that carries a fatal bit */
    uint32_t ready_value, fatal_value;
    const char *load_state_message;   /* the text the AOP takes to start the DSP */
    uint32_t mailbox_timeout, ready_timeout, link_timeout;  /* in steps; zero means the default */
} QcomAdspConfig;

/* Hooks that need hardware or the secure world. QcomAdspOk: done; QcomAdspAgain: call again; anything else
 * fails the bring-up with the matching reason. */
typedef struct {
    int (*auth)(void *context, const QcomMdtImage *image);
    int (*memory)(void *context, uint64_t base, uint64_t span);
    void *context;
} QcomAdspHooks;

typedef struct {
    QcomAdspConfig config;
    QcomAdspHooks hooks;
    const QcomMdtImage *image;
    QcomQmp *mailbox;
    QcomSmp2p *smp2p;      /* the SMP2P pair: the DSP's entries are read from it every step */
    QcomGlink *link;
    QcomGlinkChannel *channel;
    int state, last_error;
    uint32_t steps, waits, ready_entries, fatal_entries;
    uint64_t segment_offset[QCOM_MDT_MAX_SEGMENTS];
    uint8_t segment_placed[QCOM_MDT_MAX_SEGMENTS];
    uint64_t span;
} QcomAdsp;

void qcom_adsp_init(QcomAdsp *a, const QcomAdspConfig *config, const QcomAdspHooks *hooks,
                    const QcomMdtImage *image, QcomQmp *mailbox, QcomSmp2p *state, QcomGlink *link,
                    QcomGlinkChannel *channel);

/* One step. QcomAdspOk when it moved (or finished), QcomAdspAgain when it is waiting for something, and a
 * negative reason when the bring-up failed; the state then stays QcomAdspFailed. */
int qcom_adsp_step(QcomAdsp *a);

/* Where a loadable segment goes inside the carveout, once placement has run. */
int qcom_adsp_segment(const QcomAdsp *a, unsigned index, uint64_t *offset);

#ifdef __cplusplus
}
#endif

#endif
