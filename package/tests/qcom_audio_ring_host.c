/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the shared output ring: producer and consumer indices over one buffer, the period accounting the
 * acceptance record needs, and the classification of a period that finished late or short. */
#include <stdio.h>
#include <string.h>

#include "../QcomAudioRing.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

#define FRAMES 1920u
#define PERIOD 480u
#define CHANNELS 2u
#define BYTES 2u

static uint8_t buffer[FRAMES * CHANNELS * BYTES];
static uint32_t write_index, read_index;

static QcomAudioRingConfig config(void)
{
    QcomAudioRingConfig c;
    memset(&c, 0, sizeof c);
    c.data = buffer;
    c.write_index = &write_index;
    c.read_index = &read_index;
    c.frames = FRAMES;
    c.period_frames = PERIOD;
    c.periods_in_flight = 4;
    c.channels = CHANNELS;
    c.bytes_per_sample = BYTES;
    c.sample_rate = 48000u;
    c.host_clock_hz = 19200000u;
    return c;
}

static void test_config(void)
{
    QcomAudioRing r;
    const QcomAudioRingConfig good = config();
    check(qcom_audio_ring_init(&r, &good) == QcomRingOk, "a consistent ring initialises");
    check(qcom_audio_ring_level(&r) == 0, "a fresh ring is empty");
    check(qcom_audio_ring_room(&r) == FRAMES - 1u, "a fresh ring can take all but one frame");
    check(r.period_deadline_ticks == 192000u, "one period is 192000 host ticks at 48 kHz on a 19.2 MHz clock");

    QcomAudioRingConfig bad = good; bad.frames = 2000u;   /* not a multiple of the period */
    check(qcom_audio_ring_init(&r, &bad) == QcomRingBadConfig, "a ring whose size is not a whole number of periods is refused");
    bad = good; bad.period_frames = 0;
    check(qcom_audio_ring_init(&r, &bad) == QcomRingBadConfig, "a zero period is refused");
    bad = good; bad.sample_rate = 0;
    check(qcom_audio_ring_init(&r, &bad) == QcomRingBadConfig, "a zero rate is refused");
    bad = good; bad.host_clock_hz = 0;
    check(qcom_audio_ring_init(&r, &bad) == QcomRingBadConfig, "a zero host clock is refused");
    bad = good; bad.channels = 0;
    check(qcom_audio_ring_init(&r, &bad) == QcomRingBadConfig, "a ring without channels is refused");
    bad = good; bad.data = 0;
    check(qcom_audio_ring_init(&r, &bad) == QcomRingBadConfig, "a ring without a buffer is refused");
    bad = good; bad.periods_in_flight = 0;
    check(qcom_audio_ring_init(&r, &bad) == QcomRingTooManyPeriods, "a ring with no periods in flight is refused");
    bad = good; bad.periods_in_flight = 5;
    check(qcom_audio_ring_init(&r, &bad) == QcomRingTooManyPeriods, "more periods in flight than the ring holds is refused");
}

static void test_transfer(void)
{
    QcomAudioRing r;
    const QcomAudioRingConfig good = config();
    (void)qcom_audio_ring_init(&r, &good);

    static uint8_t samples[PERIOD * CHANNELS * BYTES];
    for (uint32_t i = 0; i < sizeof samples; ++i) samples[i] = (uint8_t)(i & 0xff);

    check(qcom_audio_ring_write(&r, samples, PERIOD) == PERIOD, "one period of audio is written");
    check(qcom_audio_ring_level(&r) == PERIOD, "the level follows the producer");
    check(buffer[0] == samples[0] && buffer[sizeof samples - 1] == samples[sizeof samples - 1],
          "the audio lands in the buffer unchanged");

    check(qcom_audio_ring_complete(&r, PERIOD, 192000u, 0u) == QcomRingOk, "the consumer completes a period");
    check(r.completed == 1 && r.late == 0 && r.underruns == 0, "a period that finished on time is neither late nor short");
    check(qcom_audio_ring_level(&r) == 0, "the level drops as the consumer reads");

    /* Fill the ring and then overrun it. */
    const uint32_t room = qcom_audio_ring_room(&r);
    static uint8_t big[FRAMES * CHANNELS * BYTES];
    check(qcom_audio_ring_write(&r, big, room) == room, "the ring takes everything it has room for");
    check(qcom_audio_ring_room(&r) == 0, "a full ring has no room");
    check(qcom_audio_ring_write(&r, big, PERIOD) == 0 && r.overruns == 1, "writing into a full ring is an overrun, not a wrap");

    /* A consumer that outruns the producer is an underrun, and the shortfall is recorded. */
    QcomAudioRing r2;
    (void)qcom_audio_ring_init(&r2, &good);
    (void)qcom_audio_ring_write(&r2, samples, 100u);
    check(qcom_audio_ring_complete(&r2, PERIOD, 192000u, 0u) == QcomRingOk, "a short period still completes");
    check(r2.underruns == 1 && r2.last_underrun_frames == PERIOD - 100u, "the missing frames are counted");
    check(qcom_audio_ring_level(&r2) == 0, "the underrun leaves nothing unread");

    /* Silence: the silent-data-path period, counted separately from real audio. */
    QcomAudioRing r3;
    (void)qcom_audio_ring_init(&r3, &good);
    memset(buffer, 0xff, sizeof buffer);
    check(qcom_audio_ring_write_silence(&r3, PERIOD) == PERIOD, "a silent period is written");
    check(r3.silent == 1, "the silent period is counted");
    check(buffer[0] == 0 && buffer[PERIOD * CHANNELS * BYTES - 1] == 0, "a silent period really is silence");

    /* Late: the same period, finishing after its deadline. */
    QcomAudioRing r4;
    (void)qcom_audio_ring_init(&r4, &good);
    (void)qcom_audio_ring_write(&r4, samples, PERIOD);
    (void)qcom_audio_ring_complete(&r4, PERIOD, 192000u, 0u);
    check(r4.late == 0, "finishing exactly on the deadline is not late");
    (void)qcom_audio_ring_write(&r4, samples, PERIOD);
    (void)qcom_audio_ring_complete(&r4, PERIOD, 192000u + 192000u + 5u, 192000u);
    check(r4.late == 1 && r4.worst_late_ticks == 5u, "finishing after the deadline is late and the delay is kept");

    /* Wrap: write across the end of the ring and read it back. */
    QcomAudioRing r5;
    (void)qcom_audio_ring_init(&r5, &good);
    const uint32_t nearly = FRAMES - 1u - PERIOD;
    (void)qcom_audio_ring_write(&r5, big, nearly);
    (void)qcom_audio_ring_complete(&r5, nearly, 0u, 0u);
    (void)qcom_audio_ring_write(&r5, samples, PERIOD);
    check(qcom_audio_ring_level(&r5) == PERIOD, "audio written past the end of the ring is kept");
    const uint32_t at = *r5.config.write_index - PERIOD;
    check(buffer[(at % FRAMES) * CHANNELS * BYTES] == samples[0], "the wrapped write starts where the index says");
}

int main(void)
{
    test_config();
    test_transfer();
    printf("PASS QcomAudioRing: %d checks\n", checks);
    return failed ? 1 : 0;
}
