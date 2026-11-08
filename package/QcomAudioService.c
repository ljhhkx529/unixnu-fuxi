/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAudioService.h"

static void rebuild(QcomAudioService *s)
{
    qcom_adsp_init(&s->adsp, &s->adsp_config, &s->adsp_hooks, s->image, s->mailbox, s->smp2p, s->link, s->channel);
    qcom_audio_path_init(&s->path, &s->path_config, s->link, s->channel);
    qcom_audio_path_restart(&s->path, s->generation);
}

void qcom_audio_service_init(QcomAudioService *s, const QcomAdspConfig *adsp_config,
                             const QcomAdspHooks *adsp_hooks, const QcomAudioPathConfig *path_config,
                             QcomGlink *link, QcomGlinkChannel *channel, QcomQmp *mailbox, QcomSmp2p *smp2p,
                             const QcomMdtImage *image, QcomAudioRing *ring, QcomAudioControl *control,
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
    s->control = control;
    s->mute_amplifiers = mute_amplifiers;
    s->context = context;
    s->generation = 1;
    s->state = QcomServiceIdle;
    s->last_error = QcomServiceOk;
    rebuild(s);
}

int qcom_audio_service_step(QcomAudioService *s)
{
    if (!s) return QcomServiceBadConfig;
    if (s->state == QcomServiceFailed) return s->last_error;
    ++s->steps;

    if (s->adsp.state != QcomAdspUp) {
        s->state = QcomServiceBringUp;
        const int rc = qcom_adsp_step(&s->adsp);
        if (rc < 0) {
            s->last_error = QcomServiceBringUpFailed;
            s->state = QcomServiceFailed;
            return s->last_error;
        }
        return QcomServiceAgain;
    }

    if (s->path.state != QcomPathRunning) {
        const int rc = qcom_audio_path_step(&s->path);
        if (rc < 0) {
            s->last_error = QcomServiceBringUpFailed;
            s->state = QcomServiceFailed;
            return s->last_error;
        }
        return QcomServiceAgain;
    }

    s->state = QcomServiceRunning;
    return QcomServiceOk;
}

int qcom_audio_service_restart(QcomAudioService *s)
{
    if (!s) return QcomServiceBadConfig;
    /* Amplifiers first: nothing may be left unmuted across a restart. */
    if (s->mute_amplifiers) {
        if (s->mute_amplifiers(s->context) != 0) {
            ++s->failures;
            s->last_error = QcomServiceMuteFailed;
            s->state = QcomServiceFailed;
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
    s->state = QcomServiceIdle;
    s->last_error = QcomServiceOk;
    return QcomServiceOk;
}

/* What has been handed to the DSP, published where the plug-in can read it. The sequence counters around the anchors
 * are the plug-in's to bump; the service only keeps the progress, the late and hole counts and the state bits. */
static void publish(QcomAudioService *s)
{
    if (!s->control) return;
    s->control->out_frames = s->path.completed;
    s->control->out_late = s->path.late + s->path.timeouts;
    s->control->out_holes = s->path.cancelled;
    s->control->in_frames = s->capture.periods;
    s->control->in_late = s->capture.late + s->capture.malformed;
    /* The running bit follows the data path, which is what actually plays: the session's own state can be Running
     * while the path has been stopped. */
    if (s->path.state == QcomPathRunning) s->control->state |= QcomAudioOutputRunning;
    else s->control->state &= ~(uint32_t)QcomAudioOutputRunning;
}

uint32_t qcom_audio_service_cycle(QcomAudioService *s)
{
    if (!s || !s->ring) return 0;
    ++s->cycles;
    if (s->path.state != QcomPathRunning) {++s->starved; publish(s); return 0;}

    /* Take one period out of the ring if the plug-in has written one; a ring that is short is a starve and is counted
     * rather than sounding short without a trace. */
    const uint32_t frames = s->ring->config.period_frames;
    if (qcom_audio_ring_level(s->ring) < frames) {++s->starved; publish(s); return 0;}
    const int rc = qcom_audio_path_period(&s->path, frames * s->ring->config.channels * s->ring->config.bytes_per_sample);
    if (rc != QcomPathOk) {++s->starved; publish(s); return 0;}
    (void)qcom_audio_ring_complete(s->ring, frames, 0, 0);
    ++s->periods_taken;
    publish(s);
    return frames;
}

int qcom_audio_service_start_output(QcomAudioService *s, uint64_t anchor_host)
{
    if (!s) return QcomServiceBadConfig;
    if (s->control) {
        /* The anchor is written with the sequence counter odd, so the plug-in can tell a half-written anchor. */
        ++s->control->out_seq;
        s->control->out_anchor_host = anchor_host;
        s->control->out_frames = 0;
        s->control->out_late = 0;
        s->control->out_holes = 0;
        ++s->control->out_seq;
        s->control->state |= QcomAudioOutputReady;
    }
    if (s->ring) {
        if (s->ring->config.write_index) *s->ring->config.write_index = 0;
        if (s->ring->config.read_index) *s->ring->config.read_index = 0;
    }
    return QcomServiceOk;
}

int qcom_audio_service_stop_output(QcomAudioService *s)
{
    if (!s) return QcomServiceBadConfig;
    if (s->control) s->control->state &= ~(uint32_t)(QcomAudioOutputRunning | QcomAudioOutputReady);
    /* The periods in flight are cancelled through the path's own restart, so nothing is left half-written. */
    const uint32_t generation = s->generation;
    (void)qcom_audio_path_restart(&s->path, generation + 1u);
    s->generation = generation + 1u;
    publish(s);
    return QcomServiceOk;
}

int qcom_audio_service_start_input(QcomAudioService *s, uint64_t anchor_host, const QcomAudioCaptureConfig *config)
{
    if (!s) return QcomServiceBadConfig;
    if (config) s->capture_config = *config;
    if (!s->capture_config.period_frames || !s->capture_config.sample_rate || !s->capture_config.channels)
        return QcomServiceBadConfig;
    if (qcom_audio_capture_init(&s->capture, &s->capture_config) != QcomCaptureOk) return QcomServiceBadConfig;
    if (s->control) {
        ++s->control->in_seq;
        s->control->in_anchor_host = anchor_host;
        s->control->in_frames = 0;
        s->control->in_late = 0;
        ++s->control->in_seq;
        s->control->state |= QcomAudioInputReady | QcomAudioInputRunning;
    }
    return QcomServiceOk;
}

int qcom_audio_service_capture(QcomAudioService *s, uint32_t index, uint32_t offset, uint32_t bytes,
                               const uint8_t *samples, uint32_t capacity)
{
    if (!s) return QcomServiceBadConfig;
    const int rc = qcom_audio_capture_period(&s->capture, index, offset, bytes, samples, capacity);
    publish(s);
    return rc;
}

int qcom_audio_service_stop_input(QcomAudioService *s)
{
    if (!s) return QcomServiceBadConfig;
    if (s->control) s->control->state &= ~(uint32_t)(QcomAudioInputReady | QcomAudioInputRunning);
    /* The run stays as it was so the caller can still read its statistics after stopping it. */
    publish(s);
    return QcomServiceOk;
}
