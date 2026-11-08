/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The service side: the ADSP comes up, the silent data path follows, the shared page tells the plug-in where the
 * timeline is, and one IO cycle takes a period from the ring and hands it to the DSP. A restart goes back through
 * both layers in the order the safety rules require.
 *
 * The order is the point. A restart mutes the amplifiers first, then moves to a new generation, then resets the
 * bring-up and the data path; the path's transactions carry that generation, so an answer from before the
 * restart cannot be mistaken for one after it. The ring, when there is one, is emptied as part of the same move.
 *
 * The session owns no hardware and no board value: the ADSP and path configurations are copies of what the
 * caller supplied, and the amplifiers are muted through a hook. */
#ifndef QCOM_AUDIOSERVICE_H
#define QCOM_AUDIOSERVICE_H

#include <stdint.h>

#include "QcomAdsp.h"
#include "QcomAudioPath.h"
#include "QcomAudioAbi.h"
#include "QcomAudioRing.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    QcomServiceIdle = 0,
    QcomServiceBringUp = 1,
    QcomServiceRunning = 2,
    QcomServiceFailed = -1,
};

enum {
    QcomServiceOk = 0,
    QcomServiceAgain = 1,
    QcomServiceBadConfig = -1,
    QcomServiceMuteFailed = -2,   /* the amplifiers could not be muted, so the restart did not happen */
    QcomServiceBringUpFailed = -3,
};

typedef struct {
    /* the caller's configuration, kept so a restart can rebuild both layers from it */
    QcomAdspConfig adsp_config;
    QcomAdspHooks adsp_hooks;
    QcomAudioPathConfig path_config;
    /* what the ADSP layer talks to */
    const QcomMdtImage *image;
    QcomQmp *mailbox;
    QcomSmp2p *smp2p;
    QcomGlink *link;
    QcomGlinkChannel *channel;
    /* the layers themselves, and the ring when the board has one */
    QcomAdsp adsp;
    QcomAudioPath path;
    QcomAudioRing *ring;
    QcomAudioControl *control;   /* the page the plug-in maps; the service publishes into it */
    /* the amplifier safety net, called before every restart */
    int (*mute_amplifiers)(void *context);
    void *context;
    int state, last_error;
    uint32_t generation, steps, recoveries, mutes, failures;
    uint32_t periods_taken, cycles, starved;
} QcomAudioService;

void qcom_audio_service_init(QcomAudioService *s, const QcomAdspConfig *adsp_config,
                             const QcomAdspHooks *adsp_hooks, const QcomAudioPathConfig *path_config,
                             QcomGlink *link, QcomGlinkChannel *channel, QcomQmp *mailbox, QcomSmp2p *smp2p,
                             const QcomMdtImage *image, QcomAudioRing *ring, QcomAudioControl *control,
                             int (*mute_amplifiers)(void *context), void *context);

/* Brings the session up and then keeps it up: QcomServiceOk when running, QcomServiceAgain while a layer waits. */
int qcom_audio_service_step(QcomAudioService *s);
/* Mutes, moves to a new generation and rebuilds both layers. QcomServiceMuteFailed leaves the session failed and
 * does not pretend the restart happened. */
int qcom_audio_service_restart(QcomAudioService *s);

/* One IO cycle: takes a period out of the ring into the DSP when the path runs, publishes the counters into the page
 * and returns the frames it handed over (0 when there was nothing to hand over). */
uint32_t qcom_audio_service_cycle(QcomAudioService *s);
/* Announces the output on the shared page and clears its progress, as the service does when the plug-in starts it. */
int qcom_audio_service_start_output(QcomAudioService *s, uint64_t anchor_host);
/* Stops it: the page says so first, then the periods in flight are cancelled. */
int qcom_audio_service_stop_output(QcomAudioService *s);

#ifdef __cplusplus
}
#endif

#endif
