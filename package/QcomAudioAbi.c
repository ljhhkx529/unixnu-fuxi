/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAudioAbi.h"

uint64_t qcom_audio_abi_ticks_per_frame(uint32_t host_clock_hz, uint32_t rate)
{
    if (!rate) return 0;
    /* host_clock_hz / rate in 32.32: the integer part on the left, the fraction from the remainder. */
    const uint64_t whole = host_clock_hz / rate;
    const uint64_t remainder = host_clock_hz % rate;
    return (whole << 32) | ((remainder << 32) / rate);
}

int qcom_audio_abi_init(QcomAudioControl *control, const QcomAudioAbiConfig *config)
{
    if (!control || !config) return QcomAudioAbiBadArgument;
    if (!config->rate || !config->out_channels || !config->period_frames || !config->ring_frames ||
        !config->host_clock_hz)
        return QcomAudioAbiBadFormat;
    if (config->ring_frames % config->period_frames) return QcomAudioAbiBadRing;
    if (config->lead_frames >= config->ring_frames) return QcomAudioAbiBadRing;

    for (uint32_t i = 0; i < sizeof *control; ++i) ((uint8_t *)control)[i] = 0;
    control->magic = QCOM_AUDIO_MAGIC;
    control->version = QCOM_AUDIO_VERSION;
    control->rate = config->rate;
    control->out_channels = config->out_channels;
    control->in_channels = config->in_channels;
    control->period_frames = config->period_frames;
    control->ring_frames = config->ring_frames;
    control->lead_frames = config->lead_frames;
    control->ticks_per_frame_q32 = qcom_audio_abi_ticks_per_frame(config->host_clock_hz, config->rate);
    control->out_anchor_host = config->anchor_host;
    /* The capture timeline starts at the same instant as playback: one timeline for the whole boot. */
    control->in_anchor_host = config->anchor_host;
    control->state = QcomAudioOutputReady | (config->in_channels ? QcomAudioInputReady : 0u);
    return QcomAudioAbiOk;
}

int qcom_audio_abi_validate(const void *page, size_t length)
{
    if (!page) return QcomAudioAbiBadArgument;
    if (length < sizeof(QcomAudioControl)) return QcomAudioAbiBadRing;
    const QcomAudioControl *c = page;
    if (c->magic != QCOM_AUDIO_MAGIC) return QcomAudioAbiBadMagic;
    if (c->version != QCOM_AUDIO_VERSION) return QcomAudioAbiBadVersion;
    if (!c->rate || !c->out_channels || !c->period_frames || !c->ring_frames || !c->ticks_per_frame_q32)
        return QcomAudioAbiBadFormat;
    if (c->ring_frames % c->period_frames) return QcomAudioAbiBadRing;
    if (c->lead_frames >= c->ring_frames) return QcomAudioAbiBadRing;
    return QcomAudioAbiOk;
}

int qcom_audio_abi_export(const QcomAudioControl *control, void *page, size_t capacity)
{
    if (!control || !page) return QcomAudioAbiBadArgument;
    if (capacity < QCOM_AUDIO_PAGE_BYTES) return QcomAudioAbiBadRing;
    uint8_t *out = page;
    for (uint32_t i = 0; i < sizeof *control; ++i) out[i] = ((const uint8_t *)control)[i];
    /* The rest of the page is the diagnostics and the trace: zeroed here so a reader never sees stale memory. */
    for (uint32_t i = (uint32_t)sizeof *control; i < QCOM_AUDIO_PAGE_BYTES; ++i) out[i] = 0;
    return QcomAudioAbiOk;
}
