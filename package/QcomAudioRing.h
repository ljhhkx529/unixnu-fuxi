/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The shared output ring between the audio service and the DSP, and the period bookkeeping around it.
 *
 * Board independent: the ring's frame capacity, one period's frames, how many periods may be in flight, the
 * sample format and the host clock's frequency all arrive in the configuration. Nothing here knows a sample
 * rate, a ring size or a timer frequency of any particular board.
 *
 * The indices are owned by the two sides (the producer writes, the consumer reads) and live in shared memory,
 * so this file only moves them; the accounting - periods completed, periods that finished late against the
 * period deadline, underruns where the consumer outran the producer, and periods deliberately filled with
 * silence - is what the acceptance record needs. */
#ifndef QCOM_AUDIORING_H
#define QCOM_AUDIORING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;                    /* the shared buffer, frames * channels * bytes_per_sample */
    volatile uint32_t *write_index;   /* the producer's index, in frames, wrapping at frames */
    volatile uint32_t *read_index;    /* the consumer's index, in frames */
    uint32_t frames, period_frames;
    uint32_t periods_in_flight;
    uint32_t channels, bytes_per_sample;
    uint32_t sample_rate, host_clock_hz;
} QcomAudioRingConfig;

enum {
    QcomRingOk = 0,
    QcomRingBadConfig = -1,
    QcomRingFull = -2,
    QcomRingEmpty = -3,
    QcomRingTooManyPeriods = -4,
};

typedef struct {
    QcomAudioRingConfig config;
    uint32_t periods, completed, underruns, late, silent, overruns;
    uint32_t last_underrun_frames, worst_late_ticks;
    uint64_t period_deadline_ticks;   /* how long one period of audio lasts, in host ticks */
    uint32_t cursor_period_start;     /* the frame the current period started at */
} QcomAudioRing;

/* Frames of audio between the two indices; also the check the caller uses before consuming. */
uint32_t qcom_audio_ring_level(const QcomAudioRing *r);
/* Frames that can still be written without overwriting unread audio. */
uint32_t qcom_audio_ring_room(const QcomAudioRing *r);
/* The duration of one period in host ticks: host_clock_hz * period_frames / sample_rate. */
uint64_t qcom_audio_ring_period_ticks(const QcomAudioRingConfig *config);

int qcom_audio_ring_init(QcomAudioRing *r, const QcomAudioRingConfig *config);
/* Pushes frames of audio. Returns how many the ring took; a short count is an overrun, and nothing is lost
 * silently: the caller sees the number. */
uint32_t qcom_audio_ring_write(QcomAudioRing *r, const uint8_t *samples, uint32_t frames);
/* Fills one period with silence - the silent-data-path case - and counts it. */
uint32_t qcom_audio_ring_write_silence(QcomAudioRing *r, uint32_t frames);
/* The consumer finished a period at host tick now. Counts the period, and classifies it: a period whose
 * completion is later than its deadline is late, and one the consumer could not fill is an underrun. */
int qcom_audio_ring_complete(QcomAudioRing *r, uint32_t frames, uint64_t now_ticks, uint64_t period_start_ticks);

#ifdef __cplusplus
}
#endif

#endif
