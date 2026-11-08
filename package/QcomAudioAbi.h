/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The page the audio service and the CoreAudio plug-in share, in sheng's shape.
 *
 * Three memory types: the control page, the playback ring and the capture ring. Rings hold 16-bit little-endian
 * interleaved samples of ring_frames frames each; sample time t lives at frame t % ring_frames. Sample time zero of a
 * direction is the frame the DSP plays (or captures) at host time anchor of that direction, and one frame lasts
 * ticks_per_frame_q32 / 2^32 host ticks. The anchors are set once when the service starts, so one timeline holds for
 * the whole boot; the service clears each playback period once it has taken it. The service consumes playback frames
 * lead_frames ahead of the play position and fills capture frames lead_frames behind it, so the plug-in writes at least
 * lead_frames ahead and reads at least lead_frames behind.
 *
 * The identity is fuxi's own - magic, version and the reserve layout - while the structure, the method numbers and the
 * state bits are sheng's, because they are what the CoreAudio side speaks.
 *
 * The page is one 4 KiB block: the control page first, then the diagnostic block sheng keeps at 2048 and the property
 * trace at 2560. The two later blocks are reserved here; their contents are diagnostic and arrive when the plug-in
 * that fills them does. */
#ifndef QCOM_AUDIOABI_H
#define QCOM_AUDIOABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_AUDIO_CLASS "FuxiAudio"
#define QCOM_AUDIO_RATE 48000u
#define QCOM_AUDIO_OUT_CHANNELS 2u
#define QCOM_AUDIO_IN_CHANNELS 1u
#define QCOM_AUDIO_PERIOD_FRAMES 480u
#define QCOM_AUDIO_RING_FRAMES 65536u
#define QCOM_AUDIO_MAGIC 0x46555841u   /* 'FUXA' */
#define QCOM_AUDIO_VERSION 1u
#define QCOM_AUDIO_PAGE_BYTES 4096u
#define QCOM_AUDIO_DIAG_OFFSET 2048u
#define QCOM_AUDIO_DIAG_SLOTS 32u
#define QCOM_AUDIO_TRACE_OFFSET 2560u
#define QCOM_AUDIO_TRACE_SLOTS 128u

enum {QcomAudioMemoryControl = 0, QcomAudioMemoryOutput = 1, QcomAudioMemoryInput = 2};
enum {QcomAudioStartOutput = 0, QcomAudioStopOutput = 1, QcomAudioStartInput = 2, QcomAudioStopInput = 3};
/* Scalar input: the address of CoreAudio's host clock constant in the caller; the service sets it to this board's
 * timer frequency. */
enum {QcomAudioSetHostClock = 4};
enum {
    QcomAudioOutputRunning = 1u << 0,
    QcomAudioInputRunning = 1u << 1,
    QcomAudioOutputReady = 1u << 2,
    QcomAudioInputReady = 1u << 3,
};

typedef struct {
    uint32_t magic, version;
    uint32_t rate, out_channels, in_channels, period_frames, ring_frames, lead_frames;
    volatile uint32_t state;
    uint32_t reserved0;
    uint64_t ticks_per_frame_q32;
    volatile uint64_t out_anchor_host, in_anchor_host;
    volatile uint64_t out_frames, in_frames;
    volatile uint32_t out_late, in_late;
    volatile uint32_t out_seq, in_seq;
    volatile uint64_t out_written, in_written;
    volatile uint64_t out_written_host;
    volatile uint32_t out_holes;
} QcomAudioControl;

_Static_assert(offsetof(QcomAudioControl, magic) == 0, "the magic is first");
_Static_assert(offsetof(QcomAudioControl, ticks_per_frame_q32) == 40, "the timeline follows the format");
_Static_assert(offsetof(QcomAudioControl, out_anchor_host) == 48, "the anchors are pinned");
_Static_assert(offsetof(QcomAudioControl, out_seq) == 88, "the sequence counters are pinned");
_Static_assert(sizeof(QcomAudioControl) <= QCOM_AUDIO_DIAG_OFFSET, "the control page fits before the diagnostics");

/* The second half of the page: what the plug-in recorded, so the kernel log names what the audio server asked for
 * without the unified log. The fields are sheng's, because the plug-in that fills them is. */
typedef struct {
    volatile uint32_t count, loads;
    volatile uint32_t selector[QCOM_AUDIO_DIAG_SLOTS], where[QCOM_AUDIO_DIAG_SLOTS];
    volatile uint32_t volume_milli, volume_sets;
    volatile uint32_t mix_peak;
    volatile uint32_t in_misses;
    volatile int32_t ahead_min, ahead_max, behind_min, behind_max;
    volatile uint32_t io_frames, writes;
    volatile uint32_t overlaps, overlap_max;
    volatile uint32_t frames_total, gaps, gap_frames;
    volatile uint32_t interval[6];
    volatile uint64_t last_end, last_host;
    volatile int32_t skew_min, skew_max;
    volatile int32_t io_pri, io_base, io_policy, io_tc, io_pri_min;
    volatile uint32_t io_period, io_computation, io_constraint;
    volatile uint32_t host_clock;
    volatile int32_t host_clock_result;
} QcomAudioDiag;

/* After it, every distinct property access in the order the audio server made it: selector, then object << 24 |
 * scope's first byte << 16 | kind (1 set, 0 get) | 2 if it failed. */
typedef struct {
    volatile uint32_t count;
    volatile uint32_t entry[QCOM_AUDIO_TRACE_SLOTS][2];
} QcomAudioTrace;

_Static_assert(sizeof(QcomAudioDiag) <= QCOM_AUDIO_TRACE_OFFSET - QCOM_AUDIO_DIAG_OFFSET,
               "the diagnostic block fits between its offset and the trace's");
_Static_assert(sizeof(QcomAudioTrace) <= QCOM_AUDIO_PAGE_BYTES - QCOM_AUDIO_TRACE_OFFSET,
               "the trace fits in the rest of the page");

/* The board's numbers, and the host clock they are expressed in. */
typedef struct {
    uint32_t rate, out_channels, in_channels;
    uint32_t period_frames, ring_frames, lead_frames;
    uint32_t host_clock_hz;
    uint64_t anchor_host;
} QcomAudioAbiConfig;

enum {
    QcomAudioAbiOk = 0,
    QcomAudioAbiBadArgument = -1,
    QcomAudioAbiBadMagic = -2,
    QcomAudioAbiBadVersion = -3,
    QcomAudioAbiBadFormat = -4,
    QcomAudioAbiBadRing = -5,
};

/* Host ticks per frame, 32.32 fixed point: host_clock_hz / rate, with the fraction kept. */
uint64_t qcom_audio_abi_ticks_per_frame(uint32_t host_clock_hz, uint32_t rate);

/* Fills the control page and clears the counters and both rings' progress. */
int qcom_audio_abi_init(QcomAudioControl *control, const QcomAudioAbiConfig *config);
/* Checks a page the other side produced before a field is interpreted. */
int qcom_audio_abi_validate(const void *page, size_t length);
/* Copies the control page out; the caller's buffer must hold the whole page. */
int qcom_audio_abi_export(const QcomAudioControl *control, void *page, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
