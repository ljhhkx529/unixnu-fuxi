/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAudioCapture.h"

static uint32_t isqrt64(uint64_t v)
{
    uint64_t bit = (uint64_t)1 << 62;
    uint64_t root = 0;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= root + bit) {v -= root + bit; root = (root >> 1) + bit;}
        else root >>= 1;
        bit >>= 2;
    }
    return (uint32_t)(root > 0xffffffffu ? 0xffffffffu : root);
}

int qcom_audio_capture_init(QcomAudioCapture *c, const QcomAudioCaptureConfig *config)
{
    if (!c || !config) return QcomCaptureBadConfig;
    if (config->bit_width != 16u && config->bit_width != 32u) return QcomCaptureBadWidth;
    if (!config->channels || config->channels > 8u) return QcomCaptureBadConfig;
    if (!config->period_frames || !config->sample_rate) return QcomCaptureBadConfig;
    for (uint32_t i = 0; i < sizeof *c; ++i) ((uint8_t *)c)[i] = 0;
    c->config = *config;
    return QcomCaptureOk;
}

int qcom_audio_capture_period(QcomAudioCapture *c, uint32_t index, uint32_t offset, uint32_t bytes,
                              const uint8_t *samples, uint32_t capacity)
{
    if (!c || !samples) return QcomCaptureBadConfig;
    const uint32_t bytes_per_sample = c->config.bit_width / 8u;
    const uint32_t frame_bytes = bytes_per_sample * c->config.channels;
    const uint32_t period_bytes = frame_bytes * c->config.period_frames;
    /* The descriptor has to name a whole period inside the buffer we were given. */
    if (bytes != period_bytes || offset > capacity || bytes > capacity - offset) {
        ++c->malformed;
        return QcomCaptureDescriptor;
    }
    /* Periods arrive in order; one that does not is late rather than silently stitched in. */
    if (index != c->next_index) {
        ++c->late;
        return QcomCaptureOutOfOrder;
    }
    c->last_offset = offset;
    c->last_bytes = bytes;
    ++c->next_index;
    ++c->periods;

    const uint32_t count = c->config.period_frames * c->config.channels;
    const int32_t full = c->config.bit_width == 16u ? 32767 : 0x7fffffff;
    const uint8_t *at = samples + offset;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t value;
        if (c->config.bit_width == 16u) {
            const uint8_t *s = at + 2u * i;
            value = (int16_t)(uint16_t)(s[0] | (uint16_t)s[1] << 8);
        } else {
            const uint8_t *s = at + 4u * i;
            value = (int32_t)((uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24);
        }
        if (value) ++c->nonzero;
        const uint32_t magnitude = (uint32_t)(value < 0 ? -(int64_t)value : value);
        /* At or beyond full scale is clipping, whichever side of zero it is on. */
        if (magnitude >= (uint32_t)full) ++c->clipped;
        if (magnitude > c->peak) c->peak = magnitude;
        c->sum_squares += (uint64_t)((int64_t)value * (int64_t)value);
        c->sum_signed += value;
        ++c->samples;
    }
    return QcomCaptureOk;
}

uint32_t qcom_audio_capture_rms(const QcomAudioCapture *c)
{
    if (!c || !c->samples) return 0;
    return isqrt64(c->sum_squares / c->samples);
}

int32_t qcom_audio_capture_dc_bias(const QcomAudioCapture *c)
{
    if (!c || !c->samples) return 0;
    const int64_t mean = c->sum_signed / (int64_t)c->samples;
    return (int32_t)mean;
}

uint32_t qcom_audio_capture_nonzero_per_mille(const QcomAudioCapture *c)
{
    if (!c || !c->samples) return 0;
    return (uint32_t)((uint64_t)c->nonzero * 1000u / c->samples);
}

int qcom_audio_capture_complete(const QcomAudioCapture *c)
{
    if (!c || !c->config.expected_periods) return 0;
    return c->periods >= c->config.expected_periods && !c->late && !c->malformed && !c->clipped;
}
