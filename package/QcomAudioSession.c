/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAudioSession.h"

static void rebuild(QcomAudioSession *s)
{
    qcom_adsp_init(&s->adsp, &s->adsp_config, &s->adsp_hooks, s->image, s->mailbox, s->smp2p, s->link, s->channel);
    qcom_audio_path_init(&s->path, &s->path_config, s->link, s->channel);
    qcom_audio_path_restart(&s->path, s->generation);
}

void qcom_audio_session_init(QcomAudioSession *s, const QcomAdspConfig *adsp_config,
                             const QcomAdspHooks *adsp_hooks, const QcomAudioPathConfig *path_config,
                             QcomGlink *link, QcomGlinkChannel *channel, QcomQmp *mailbox, QcomSmp2p *smp2p,
                             const QcomMdtImage *image, QcomAudioRing *ring,
                             int (*mute_amplifiers)(void *context), void *context)
{
    for (uint32_t i = 0; i < sizeof *s; ++i) ((uint8_t *)s)[i] = 0;
    if (adsp_config) s->adsp_config = *adsp_config;
    if (adsp_hooks) s->adsp_hooks = *adsp_hooks;
    if (path_config) s->path_config = *path_config;
    s->link = link;
    s->channel = channel;
    s->mailbox = mailbox;
    s->smp2p = smp2p;
    s->image = image;
    s->ring = ring;
    s->mute_amplifiers = mute_amplifiers;
    s->context = context;
    s->generation = 1;
    s->state = QcomSessionIdle;
    s->last_error = QcomSessionOk;
    rebuild(s);
}

int qcom_audio_session_step(QcomAudioSession *s)
{
    if (!s) return QcomSessionBadConfig;
    if (s->state == QcomSessionFailed) return s->last_error;
    ++s->steps;

    if (s->adsp.state != QcomAdspUp) {
        s->state = QcomSessionBringUp;
        const int rc = qcom_adsp_step(&s->adsp);
        if (rc < 0) {
            s->last_error = QcomSessionBringUpFailed;
            s->state = QcomSessionFailed;
            return s->last_error;
        }
        return QcomSessionAgain;
    }

    if (s->path.state != QcomPathRunning) {
        const int rc = qcom_audio_path_step(&s->path);
        if (rc < 0) {
            s->last_error = QcomSessionBringUpFailed;
            s->state = QcomSessionFailed;
            return s->last_error;
        }
        return QcomSessionAgain;
    }

    s->state = QcomSessionRunning;
    return QcomSessionOk;
}

int qcom_audio_session_restart(QcomAudioSession *s)
{
    if (!s) return QcomSessionBadConfig;
    /* Amplifiers first: nothing may be left unmuted across a restart. */
    if (s->mute_amplifiers) {
        if (s->mute_amplifiers(s->context) != 0) {
            ++s->failures;
            s->last_error = QcomSessionMuteFailed;
            s->state = QcomSessionFailed;
            return s->last_error;
        }
        ++s->mutes;
    }
    ++s->recoveries;
    ++s->generation;
    if (s->ring) {
        if (s->ring->config.write_index) *s->ring->config.write_index = 0;
        if (s->ring->config.read_index) *s->ring->config.read_index = 0;
    }
    rebuild(s);
    s->state = QcomSessionIdle;
    s->last_error = QcomSessionOk;
    return QcomSessionOk;
}
