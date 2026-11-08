/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomAudioRing.h"

static uint32_t wrap(const QcomAudioRing *r, uint32_t frames)
{
    return r->config.frames ? frames % r->config.frames : 0;
}

uint64_t qcom_audio_ring_period_ticks(const QcomAudioRingConfig *config)
{
    if (!config || !config->sample_rate) return 0;
    return (uint64_t)config->host_clock_hz * config->period_frames / config->sample_rate;
}

uint32_t qcom_audio_ring_level(const QcomAudioRing *r)
{
    if (!r || !r->config.data || !r->config.write_index || !r->config.read_index) return 0;
    const uint32_t write = *r->config.write_index % (r->config.frames ? r->config.frames : 1);
    const uint32_t read = *r->config.read_index % (r->config.frames ? r->config.frames : 1);
    return write >= read ? write - read : r->config.frames - read + write;
}

uint32_t qcom_audio_ring_room(const QcomAudioRing *r)
{
    if (!r || !r->config.frames) return 0;
    const uint32_t level = qcom_audio_ring_level(r);
    /* One frame is always left free, which is what keeps a full ring distinguishable from an empty one. */
    return r->config.frames > level + 1u ? r->config.frames - level - 1u : 0;
}

int qcom_audio_ring_init(QcomAudioRing *r, const QcomAudioRingConfig *config)
{
    if (!r || !config) return QcomRingBadConfig;
    if (!config->data || !config->write_index || !config->read_index) return QcomRingBadConfig;
    if (!config->frames || !config->period_frames || !config->sample_rate || !config->host_clock_hz)
        return QcomRingBadConfig;
    if (!config->channels || !config->bytes_per_sample) return QcomRingBadConfig;
    if (config->frames % config->period_frames) return QcomRingBadConfig;
    if (!config->periods_in_flight || config->periods_in_flight > config->frames / config->period_frames)
        return QcomRingTooManyPeriods;

    for (uint32_t i = 0; i < sizeof *r; ++i) ((uint8_t *)r)[i] = 0;
    r->config = *config;
    r->period_deadline_ticks = qcom_audio_ring_period_ticks(config);
    *config->write_index = 0;
    *config->read_index = 0;
    return QcomRingOk;
}

uint32_t qcom_audio_ring_write(QcomAudioRing *r, const uint8_t *samples, uint32_t frames)
{
    if (!r || !r->config.data) return 0;
    const uint32_t room = qcom_audio_ring_room(r);
    uint32_t take = frames < room ? frames : room;
    if (take < frames) ++r->overruns;
    const uint32_t frame_bytes = r->config.channels * r->config.bytes_per_sample;
    uint32_t at = *r->config.write_index % r->config.frames;
    for (uint32_t i = 0; i < take; ++i) {
        if (samples) {
            for (uint32_t b = 0; b < frame_bytes; ++b)
                r->config.data[at * frame_bytes + b] = samples[i * frame_bytes + b];
        } else {
            for (uint32_t b = 0; b < frame_bytes; ++b) r->config.data[at * frame_bytes + b] = 0;
        }
        if (++at == r->config.frames) at = 0;
    }
    __asm__ volatile("" ::: "memory");
    *r->config.write_index = wrap(r, *r->config.write_index + take);
    return take;
}

uint32_t qcom_audio_ring_write_silence(QcomAudioRing *r, uint32_t frames)
{
    const uint32_t wrote = qcom_audio_ring_write(r, 0, frames);
    if (wrote) ++r->silent;
    return wrote;
}

int qcom_audio_ring_complete(QcomAudioRing *r, uint32_t frames, uint64_t now_ticks, uint64_t period_start_ticks)
{
    if (!r) return QcomRingBadConfig;
    const uint32_t level = qcom_audio_ring_level(r);
    uint32_t take = frames;
    if (take > level) {
        /* The consumer asked for more than the producer has written: the rest of the period is silence, and the
         * period is counted as an underrun rather than quietly sounding short. */
        ++r->underruns;
        r->last_underrun_frames = take - level;
        take = level;
    }
    __asm__ volatile("" ::: "memory");
    *r->config.read_index = wrap(r, *r->config.read_index + take);
    ++r->completed;
    ++r->periods;

    /* A period that finishes later than one period's worth of host ticks after it started is late; the worst
     * lateness is kept so a run that is only slightly behind is still visible. */
    const uint64_t spent = now_ticks - period_start_ticks;
    if (r->period_deadline_ticks && spent > r->period_deadline_ticks) {
        const uint64_t behind = spent - r->period_deadline_ticks;
        ++r->late;
        if (behind > r->worst_late_ticks) r->worst_late_ticks = (uint32_t)(behind > 0xffffffffu ? 0xffffffffu : behind);
    }
    return QcomRingOk;
}
