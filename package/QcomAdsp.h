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

/* The bring-up as one ladder of stages, numbered as sheng numbers them so a log from either board reads the same.
 * The board owns the stages that touch hardware - mapping the windows, surveying SMEM, attaching the doorbells, voting
 * the rails, loading the DTB and the image - and reports them with qcom_adsp_stage(); this layer owns the stages it can
 * judge itself: the image is placed, the mailbox carried the request, the DSP reported ready, the link agreed a version,
 * the channel came up. A failure records the stage it failed in as a negative number, as sheng's stats[0] does.
 *
 * The ladder only rises, and the stages this layer reports are the ones it judges itself: the image is placed, the
 * mailbox carried the load-state request, the DSP reported ready, the link agreed, the channel came up. The stages in
 * between - surveying SMEM, the doorbells, the rails, the DTB, the load and the start - are the board's and it reports
 * them itself, in its own order, which is why this layer never moves the ladder back. */
enum {
    QcomAdspStMapped = 1,
    QcomAdspStSurveyed = 2,
    QcomAdspStFirmware = 3,
    QcomAdspStSmp2p = 4,
    QcomAdspStDoorbells = 5,
    QcomAdspStLoadState = 6,
    QcomAdspStRails = 7,
    QcomAdspStDtb = 8,
    QcomAdspStLoaded = 9,
    QcomAdspStStarted = 10,
    QcomAdspStReady = 11,
    QcomAdspStGlink = 12,
    QcomAdspStChannel = 13,
};

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
    uint32_t stage;        /* how far the ladder got; on failure, the stage it failed in */
    int32_t failed_stage;  /* negative when the bring-up stopped, zero while it has not */
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
/* Reports a stage the board performed itself; a stage behind the current one is ignored, so the ladder only rises. */
void qcom_adsp_stage(QcomAdsp *a, uint32_t stage);

/* Where a loadable segment goes inside the carveout, once placement has run. */
int qcom_adsp_segment(const QcomAdsp *a, unsigned index, uint64_t *offset);

#ifdef __cplusplus
}
#endif

#endif
