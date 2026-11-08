/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The shared block between the kernel audio service and the user-space HAL.
 *
 * This is the shape, not the policy: one control block describing the output ring, its format and the timeline
 * anchor, plus the two indices and the counters the HAL reports back. Every board value in it comes from the
 * configuration; the only constants are the magic and the version, which are what the two sides agree on before
 * they interpret a single word.
 *
 * The field offsets are asserted at compile time, so a reordering or an inserted field breaks the build instead
 * of silently changing what the other side reads.
 *
 * The first version publishes stereo output only: the input ring fields exist so the block's size does not change
 * when capture arrives, and qcom_hal_abi_validate() refuses a block that claims an input ring while the version
 * says output only. */
#ifndef QCOM_HAL_ABI_H
#define QCOM_HAL_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_HAL_MAGIC 0x48414c31u   /* "HAL1" */
#define QCOM_HAL_VERSION 1u
#define QCOM_HAL_BLOCK_BYTES 104u
#define QCOM_HAL_FLAG_OUTPUT 1u << 0
#define QCOM_HAL_FLAG_INPUT 1u << 1

typedef struct {
    uint32_t magic;                  /* must be QCOM_HAL_MAGIC */
    uint32_t version;                /* must be QCOM_HAL_VERSION */
    uint32_t struct_bytes;           /* sizeof of this structure as the producer compiled it */
    uint32_t flags;                  /* which directions are published */

    uint64_t output_ring_address;
    uint32_t output_ring_bytes;
    uint32_t output_frames;
    uint32_t output_period_frames;
    uint32_t output_periods_in_flight;
    uint32_t channels;
    uint32_t bytes_per_sample;
    uint32_t sample_rate;
    uint32_t host_clock_hz;
    uint64_t zero_timestamp_ticks;   /* the timeline anchor the HAL and the service share */

    volatile uint32_t output_write_index;   /* the service advances this */
    volatile uint32_t output_read_index;    /* the HAL advances this */
    uint32_t underruns;                     /* the HAL counts these */
    uint32_t late;
    uint32_t silent;
    uint32_t overruns;

    uint64_t input_ring_address;            /* reserved for the version that publishes capture */
    uint32_t input_ring_bytes;
    uint32_t reserved;
} QcomHalControlBlock;

/* The offsets the two sides rely on; any change is a new version, not a rearrangement. */
_Static_assert(offsetof(QcomHalControlBlock, magic) == 0, "magic must be first");
_Static_assert(offsetof(QcomHalControlBlock, version) == 4, "version must follow magic");
_Static_assert(offsetof(QcomHalControlBlock, struct_bytes) == 8, "the size field must be third");
_Static_assert(offsetof(QcomHalControlBlock, output_ring_address) == 16, "the ring address must be aligned");
_Static_assert(offsetof(QcomHalControlBlock, zero_timestamp_ticks) == 56, "the timeline anchor is pinned");
_Static_assert(offsetof(QcomHalControlBlock, output_write_index) == 64, "the indices own one cache line");
_Static_assert(offsetof(QcomHalControlBlock, input_ring_address) == 88, "the reserved tail is pinned");
_Static_assert(sizeof(QcomHalControlBlock) == QCOM_HAL_BLOCK_BYTES, "the block size is part of the ABI");

typedef struct {
    uint64_t output_ring_address;
    uint32_t output_ring_bytes;
    uint32_t output_frames, output_period_frames, output_periods_in_flight;
    uint32_t channels, bytes_per_sample, sample_rate, host_clock_hz;
    uint64_t zero_timestamp_ticks;
} QcomHalConfig;

enum {
    QcomHalOk = 0,
    QcomHalBadArgument = -1,
    QcomHalBadMagic = -2,
    QcomHalBadVersion = -3,
    QcomHalBadSize = -4,
    QcomHalBadRing = -5,
    QcomHalUnsupported = -6,
};

/* Fills the block from the configuration and clears both indices and every counter. */
int qcom_hal_abi_init(QcomHalControlBlock *block, const QcomHalConfig *config);
/* Reads a block from a byte buffer the other side produced and checks it before anything is interpreted. */
int qcom_hal_abi_validate(const void *bytes, size_t length);
/* Copies the block out for the other side; the caller's buffer must hold exactly the block. */
int qcom_hal_abi_export(const QcomHalControlBlock *block, void *bytes, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
