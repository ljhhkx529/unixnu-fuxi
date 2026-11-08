/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* One audio session: the ADSP comes up, the silent data path follows, and a restart goes back through both in the
 * right order.
 *
 * The order is the point. A restart mutes the amplifiers first, then moves to a new generation, then resets the
 * bring-up and the data path; the path's transactions carry that generation, so an answer from before the
 * restart cannot be mistaken for one after it. The ring, when there is one, is emptied as part of the same move.
 *
 * The session owns no hardware and no board value: the ADSP and path configurations are copies of what the
 * caller supplied, and the amplifiers are muted through a hook. */
#ifndef QCOM_AUDIOSESSION_H
#define QCOM_AUDIOSESSION_H

#include <stdint.h>

#include "QcomAdsp.h"
#include "QcomAudioPath.h"
#include "QcomAudioRing.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    QcomSessionIdle = 0,
    QcomSessionBringUp = 1,
    QcomSessionRunning = 2,
    QcomSessionFailed = -1,
};

enum {
    QcomSessionOk = 0,
    QcomSessionAgain = 1,
    QcomSessionBadConfig = -1,
    QcomSessionMuteFailed = -2,   /* the amplifiers could not be muted, so the restart did not happen */
    QcomSessionBringUpFailed = -3,
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
    /* the amplifier safety net, called before every restart */
    int (*mute_amplifiers)(void *context);
    void *context;
    int state, last_error;
    uint32_t generation, steps, recoveries, mutes, failures;
} QcomAudioSession;

void qcom_audio_session_init(QcomAudioSession *s, const QcomAdspConfig *adsp_config,
                             const QcomAdspHooks *adsp_hooks, const QcomAudioPathConfig *path_config,
                             QcomGlink *link, QcomGlinkChannel *channel, QcomQmp *mailbox, QcomSmp2p *smp2p,
                             const QcomMdtImage *image, QcomAudioRing *ring,
                             int (*mute_amplifiers)(void *context), void *context);

/* Brings the session up and then keeps it up: QcomSessionOk when running, QcomSessionAgain while a layer waits. */
int qcom_audio_session_step(QcomAudioSession *s);
/* Mutes, moves to a new generation and rebuilds both layers. QcomSessionMuteFailed leaves the session failed and
 * does not pretend the restart happened. */
int qcom_audio_session_restart(QcomAudioSession *s);

#ifdef __cplusplus
}
#endif

#endif
