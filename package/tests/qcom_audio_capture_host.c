/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of capture accounting: the statistics the plan's microphone acceptance asks for, over the period
 * payloads a DSP would return, and every way a run must be reported as short, late or malformed instead of good. */
#include <stdio.h>
#include <string.h>

#include "../QcomAudioCapture.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

#define PERIOD_FRAMES 480u
#define CHANNELS 1u

static QcomAudioCaptureConfig config(void)
{
    QcomAudioCaptureConfig c;
    memset(&c, 0, sizeof c);
    c.channels = CHANNELS;
    c.bit_width = 16u;
    c.period_frames = PERIOD_FRAMES;
    c.sample_rate = 48000u;
    c.expected_periods = 200u;
    return c;
}

static void put16(uint8_t *at, int32_t value)
{
    const uint16_t raw = (uint16_t)(int16_t)value;
    at[0] = (uint8_t)raw;
    at[1] = (uint8_t)(raw >> 8);
}

/* A period of a square wave: +amplitude for the first half, -amplitude for the second. */
static void fill_square(uint8_t *buffer, int32_t amplitude)
{
    for (uint32_t i = 0; i < PERIOD_FRAMES; ++i)
        put16(buffer + 2u * i, i < PERIOD_FRAMES / 2u ? amplitude : -amplitude);
}

static void test_statistics(void)
{
    QcomAudioCapture c;
    const QcomAudioCaptureConfig cfg = config();
    check(qcom_audio_capture_init(&c, &cfg) == QcomCaptureOk, "a mono 16-bit capture initialises");
    check(qcom_audio_capture_rms(&c) == 0 && qcom_audio_capture_dc_bias(&c) == 0, "an empty run has no statistics");
    check(qcom_audio_capture_nonzero_per_mille(&c) == 0, "an empty run has a zero non-zero rate");
    check(qcom_audio_capture_complete(&c) == 0, "an empty run is not complete");

    static uint8_t buffer[PERIOD_FRAMES * 2u];
    fill_square(buffer, 8000);
    check(qcom_audio_capture_period(&c, 0, 0, sizeof buffer, buffer, sizeof buffer) == QcomCaptureOk,
          "a period is accounted");
    check(c.periods == 1 && c.next_index == 1, "the period count and the expectation advance");
    check(c.samples == PERIOD_FRAMES, "every sample is counted");
    check(c.nonzero == PERIOD_FRAMES, "a square wave has no zero samples");
    check(c.peak == 8000, "the peak is the amplitude");
    check(qcom_audio_capture_dc_bias(&c) == 0, "a symmetric square wave has no DC bias");
    check(qcom_audio_capture_rms(&c) == 8000, "the RMS of a square wave is its amplitude");
    check(c.clipped == 0, "a quiet period does not clip");
    check(qcom_audio_capture_nonzero_per_mille(&c) == 1000, "the non-zero rate is full");

    /* Silence is a legitimate result and must look different from a bad route. */
    QcomAudioCapture silence;
    (void)qcom_audio_capture_init(&silence, &cfg);
    static uint8_t zeros[PERIOD_FRAMES * 2u];
    memset(zeros, 0, sizeof zeros);
    (void)qcom_audio_capture_period(&silence, 0, 0, sizeof zeros, zeros, sizeof zeros);
    check(silence.nonzero == 0 && qcom_audio_capture_nonzero_per_mille(&silence) == 0, "silence has a zero non-zero rate");
    check(qcom_audio_capture_rms(&silence) == 0 && silence.peak == 0, "silence has no level");

    /* Clipping is counted, and it alone makes a run incomplete. */
    QcomAudioCapture loud;
    (void)qcom_audio_capture_init(&loud, &cfg);
    fill_square(buffer, 32767);
    (void)qcom_audio_capture_period(&loud, 0, 0, sizeof buffer, buffer, sizeof buffer);
    check(loud.clipped == PERIOD_FRAMES && loud.peak == 32767, "a full-scale period is counted as clipping");
    check(qcom_audio_capture_complete(&loud) == 0, "a clipped run is not complete");

    /* A DC offset shows up as bias. */
    QcomAudioCapture biased;
    (void)qcom_audio_capture_init(&biased, &cfg);
    for (uint32_t i = 0; i < PERIOD_FRAMES; ++i) put16(buffer + 2u * i, 1000);
    (void)qcom_audio_capture_period(&biased, 0, 0, sizeof buffer, buffer, sizeof buffer);
    check(qcom_audio_capture_dc_bias(&biased) == 1000, "a constant offset is the DC bias");
}

static void test_descriptors(void)
{
    QcomAudioCapture c;
    const QcomAudioCaptureConfig cfg = config();
    static uint8_t buffer[PERIOD_FRAMES * 2u];
    fill_square(buffer, 1000);

    (void)qcom_audio_capture_init(&c, &cfg);
    check(qcom_audio_capture_period(&c, 1, 0, sizeof buffer, buffer, sizeof buffer) == QcomCaptureOutOfOrder,
          "a period that skips an index is late, not stitched in");
    check(c.late == 1 && c.periods == 0, "the late period is counted and not accounted");

    check(qcom_audio_capture_period(&c, 0, 0, sizeof buffer - 2u, buffer, sizeof buffer) == QcomCaptureDescriptor,
          "a period that is not whole is refused");
    check(c.malformed == 1 && c.periods == 0, "the malformed period is counted and not accounted");

    check(qcom_audio_capture_period(&c, 0, 8u, sizeof buffer, buffer, sizeof buffer) == QcomCaptureDescriptor,
          "a period that would run off the end of the buffer is refused");
    check(qcom_audio_capture_period(&c, 0, 0, sizeof buffer, buffer, sizeof buffer) == QcomCaptureOk,
          "the same period at the right offset is accepted");

    /* The plan's acceptance shape: 200 periods, all present, none late or clipped. */
    QcomAudioCapture run;
    (void)qcom_audio_capture_init(&run, &cfg);
    for (uint32_t i = 0; i < 200u; ++i)
        (void)qcom_audio_capture_period(&run, i, 0, sizeof buffer, buffer, sizeof buffer);
    check(run.periods == 200, "two hundred periods are accounted");
    check(qcom_audio_capture_complete(&run) == 1, "a clean two-hundred-period run is complete");

    QcomAudioCapture short_run;
    (void)qcom_audio_capture_init(&short_run, &cfg);
    for (uint32_t i = 0; i < 199u; ++i)
        (void)qcom_audio_capture_period(&short_run, i, 0, sizeof buffer, buffer, sizeof buffer);
    check(qcom_audio_capture_complete(&short_run) == 0, "a run one period short is not complete");

    /* 24-bit packed is refused rather than guessed at. */
    QcomAudioCaptureConfig packed = cfg;
    packed.bit_width = 24u;
    check(qcom_audio_capture_init(&c, &packed) == QcomCaptureBadWidth, "24-bit packed capture is refused until its packing is known");
    packed = cfg; packed.channels = 0;
    check(qcom_audio_capture_init(&c, &packed) == QcomCaptureBadConfig, "a capture without channels is refused");
    packed = cfg; packed.period_frames = 0;
    check(qcom_audio_capture_init(&c, &packed) == QcomCaptureBadConfig, "a capture without a period is refused");
}

int main(void)
{
    test_statistics();
    test_descriptors();
    printf("PASS QcomAudioCapture: %d checks\n", checks);
    return failed ? 1 : 0;
}
