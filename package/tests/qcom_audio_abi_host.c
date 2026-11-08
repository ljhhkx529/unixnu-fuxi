/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the shared audio page: the control page sheng's service and plug-in exchange, its timeline in 32.32
 * host ticks, and every page from the other side that must be refused before a field is read. */
#include <stdio.h>
#include <string.h>

#include "../QcomAudioAbi.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static QcomAudioAbiConfig config(void)
{
    QcomAudioAbiConfig c;
    memset(&c, 0, sizeof c);
    c.rate = 48000u;
    c.out_channels = 2u;
    c.in_channels = 1u;
    c.period_frames = 480u;
    c.ring_frames = 65536u;     /* 136 periods of 480 do not divide, so this board uses 480 * 128 */
    c.ring_frames = 480u * 128u;
    c.lead_frames = 14400u;
    c.host_clock_hz = 19200000u;
    c.anchor_host = 0x123456789ull;
    return c;
}

static void test_timeline(void)
{
    check(qcom_audio_abi_ticks_per_frame(19200000u, 48000u) == (400ull << 32), "a 19.2 MHz clock at 48 kHz is 400 ticks a frame");
    const uint64_t fractional = qcom_audio_abi_ticks_per_frame(19200000u, 44100u);
    const uint64_t whole = fractional >> 32;
    const double value = (double)fractional / 4294967296.0;
    check(whole == 435u, "the integer part of a fractional rate is the plain division");
    check(value > 435.37 && value < 435.38, "and the fraction is kept rather than truncated");
    check(qcom_audio_abi_ticks_per_frame(19200000u, 0u) == 0, "a zero rate has no timeline");
}

static void test_page(void)
{
    QcomAudioControl c;
    const QcomAudioAbiConfig cfg = config();
    check(sizeof(QcomAudioControl) <= QCOM_AUDIO_DIAG_OFFSET,
          "the control page fits before the diagnostic block");
    check(sizeof(QcomAudioDiag) <= QCOM_AUDIO_TRACE_OFFSET - QCOM_AUDIO_DIAG_OFFSET,
          "the diagnostic block fits before the trace");
    check(sizeof(QcomAudioTrace) <= QCOM_AUDIO_PAGE_BYTES - QCOM_AUDIO_TRACE_OFFSET,
          "the trace fits in the rest of the page");
    check(QCOM_AUDIO_DIAG_SLOTS == 32u && QCOM_AUDIO_TRACE_SLOTS == 128u,
          "the slot counts the plug-in writes are the ones the service reads");
    check(qcom_audio_abi_init(&c, &cfg) == QcomAudioAbiOk, "a consistent configuration fills the page");
    check(c.magic == QCOM_AUDIO_MAGIC && c.version == QCOM_AUDIO_VERSION, "the page carries the identity");
    check(c.rate == cfg.rate && c.out_channels == cfg.out_channels && c.in_channels == cfg.in_channels,
          "it carries the format");
    check(c.period_frames == cfg.period_frames && c.ring_frames == cfg.ring_frames &&
          c.lead_frames == cfg.lead_frames, "and the geometry");
    check(c.ticks_per_frame_q32 == (400ull << 32), "and the timeline");
    check(c.out_anchor_host == cfg.anchor_host && c.in_anchor_host == cfg.anchor_host,
          "both directions start on one timeline");
    check(c.state == (QcomAudioOutputReady | QcomAudioInputReady), "both directions are announced ready");
    check(c.out_frames == 0 && c.in_frames == 0 && c.out_late == 0 && c.in_late == 0,
          "the counters start at zero");
    check(c.out_seq == 0 && c.in_seq == 0, "and so do the sequence counters");
    check(qcom_audio_abi_validate(&c, sizeof c) == QcomAudioAbiOk, "the page validates");

    QcomAudioAbiConfig capture_only = cfg;
    capture_only.in_channels = 0;
    check(qcom_audio_abi_init(&c, &capture_only) == QcomAudioAbiOk && c.state == QcomAudioOutputReady,
          "a page without capture announces only the output");

    /* Across a page boundary, as the plug-in maps it. */
    static uint8_t page[QCOM_AUDIO_PAGE_BYTES];
    memset(page, 0xa5, sizeof page);
    const QcomAudioAbiConfig full = config();
    (void)qcom_audio_abi_init(&c, &full);
    check(qcom_audio_abi_export(&c, page, sizeof page) == QcomAudioAbiOk, "the page exports into the mapping");
    check(qcom_audio_abi_validate(page, sizeof page) == QcomAudioAbiOk, "and validates where it lands");
    check(((QcomAudioControl *)page)->rate == full.rate, "the exported page has the same values");
    check(page[sizeof(QcomAudioControl)] == 0 && page[QCOM_AUDIO_PAGE_BYTES - 1u] == 0,
          "the diagnostic and trace area is cleared, not left stale");
    check(qcom_audio_abi_export(&c, page, QCOM_AUDIO_PAGE_BYTES - 1u) == QcomAudioAbiBadRing,
          "an export into a short mapping is refused");
}

static void test_refusals(void)
{
    QcomAudioControl c;
    QcomAudioAbiConfig cfg = config();

    cfg.rate = 0;
    check(qcom_audio_abi_init(&c, &cfg) == QcomAudioAbiBadFormat, "a page without a rate is refused");
    cfg = config(); cfg.out_channels = 0;
    check(qcom_audio_abi_init(&c, &cfg) == QcomAudioAbiBadFormat, "a page without an output is refused");
    cfg = config(); cfg.ring_frames = 1000u;
    check(qcom_audio_abi_init(&c, &cfg) == QcomAudioAbiBadRing, "a ring that is not whole periods is refused");
    cfg = config(); cfg.lead_frames = cfg.ring_frames;
    check(qcom_audio_abi_init(&c, &cfg) == QcomAudioAbiBadRing, "a lead that reaches the ring's length is refused");
    check(qcom_audio_abi_init(0, &cfg) == QcomAudioAbiBadArgument, "a null page is refused");

    const QcomAudioAbiConfig good = config();
    (void)qcom_audio_abi_init(&c, &good);
    check(qcom_audio_abi_validate(0, sizeof c) == QcomAudioAbiBadArgument, "a null page is refused on the way in");
    check(qcom_audio_abi_validate(&c, sizeof c - 1u) == QcomAudioAbiBadRing, "a short page is refused");

    QcomAudioControl bad = c;
    bad.magic ^= 1u;
    check(qcom_audio_abi_validate(&bad, sizeof bad) == QcomAudioAbiBadMagic, "a foreign magic is refused first");
    bad = c; bad.version = QCOM_AUDIO_VERSION + 1u;
    check(qcom_audio_abi_validate(&bad, sizeof bad) == QcomAudioAbiBadVersion, "a version this side does not read is refused");
    bad = c; bad.ticks_per_frame_q32 = 0;
    check(qcom_audio_abi_validate(&bad, sizeof bad) == QcomAudioAbiBadFormat, "a page without a timeline is refused");
    bad = c; bad.ring_frames = 1000u;
    check(qcom_audio_abi_validate(&bad, sizeof bad) == QcomAudioAbiBadRing, "a ring that is not whole periods is refused");
    bad = c; bad.lead_frames = bad.ring_frames;
    check(qcom_audio_abi_validate(&bad, sizeof bad) == QcomAudioAbiBadRing, "a lead that reaches the ring's length is refused");
}

int main(void)
{
    test_timeline();
    test_page();
    test_refusals();
    printf("PASS QcomAudioAbi: %d checks\n", checks);
    return failed ? 1 : 0;
}
