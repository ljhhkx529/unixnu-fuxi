/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Capture period accounting: what a capture run has to report before anyone is allowed to call a microphone
 * route proven.
 *
 * The plan's acceptance for the first microphone is 200 completed periods whose returned descriptors line up,
 * with a non-zero rate, RMS, peak, clipping count and DC bias that move with the scene - quiet versus speech -
 * and no clipping. Those are all integer statistics over the samples, so they belong here rather than in a
 * logging path, and a run that is short, late or malformed has to be visible in the same record.
 *
 * Nothing here is board specific: the width, the channel count and the period length are inputs, and 24-bit
 * packed samples are refused rather than guessed at, because the packing is a board fact that has not been
 * established yet. */
#ifndef QCOM_AUDIOCAPTURE_H
#define QCOM_AUDIOCAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t channels;        /* 1 for the first mono path */
    uint32_t bit_width;       /* 16 or 32; 24-bit packed is not modelled yet */
    uint32_t period_frames;
    uint32_t sample_rate;
    uint32_t expected_periods;  /* 200 in the plan's acceptance, zero for "no expectation" */
} QcomAudioCaptureConfig;

typedef struct {
    QcomAudioCaptureConfig config;
    uint32_t periods, shorts, late, malformed;
    uint32_t samples, nonzero, clipped;
    uint32_t peak;             /* largest magnitude seen */
    uint64_t sum_squares;      /* over samples, in sample-squared units */
    int64_t sum_signed;        /* for the DC bias */
    uint32_t next_index;       /* the period index the DSP must deliver next */
    uint32_t last_offset, last_bytes;
} QcomAudioCapture;

enum {
    QcomCaptureOk = 0,
    QcomCaptureBadConfig = -1,
    QcomCaptureBadWidth = -2,
    QcomCaptureOutOfOrder = -3,
    QcomCaptureDescriptor = -4,
    QcomCaptureShort = -5,
};

int qcom_audio_capture_init(QcomAudioCapture *c, const QcomAudioCaptureConfig *config);

/* One completed period: its index, where it landed in the ring and how many bytes came back. The samples are read
 * from the caller's buffer, so the statistics are over exactly what the DSP returned. */
int qcom_audio_capture_period(QcomAudioCapture *c, uint32_t index, uint32_t offset, uint32_t bytes,
                              const uint8_t *samples, uint32_t capacity);

/* Integer RMS of everything so far, in sample units; zero before any samples. */
uint32_t qcom_audio_capture_rms(const QcomAudioCapture *c);
/* Mean of the signed samples: the DC bias a bad route shows as. */
int32_t qcom_audio_capture_dc_bias(const QcomAudioCapture *c);
/* The non-zero rate in thousandths, which avoids floating point in the acceptance record. */
uint32_t qcom_audio_capture_nonzero_per_mille(const QcomAudioCapture *c);
/* Whether the run meets the plan's acceptance shape: every expected period, none late or malformed, no clipping. */
int qcom_audio_capture_complete(const QcomAudioCapture *c);

#ifdef __cplusplus
}
#endif

#endif
