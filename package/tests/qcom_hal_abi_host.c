/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the shared HAL block: the shape the kernel service and the user-space HAL agree on, and every
 * way a block from the other side must be refused before a single field is interpreted. */
#include <stdio.h>
#include <string.h>

#include "../QcomHalAbi.h"

static int checks, failed;

static void check(int condition, const char *what)
{
    ++checks;
    if (!condition) {++failed; fprintf(stderr, "FAIL %s\n", what);}
}

static QcomHalConfig config(void)
{
    QcomHalConfig c;
    memset(&c, 0, sizeof c);
    c.output_ring_address = 0x8f000000ull;
    /* 136 periods of 480 frames. The plan's 65536-frame ring does not divide by 480 - 65536 / 480 is 136.5 - so a
     * board that wants both has to pick one; the validator refuses the pair rather than rounding it silently. */
    c.output_frames = 480u * 136u;
    c.output_period_frames = 480u;
    c.output_periods_in_flight = 4u;
    c.channels = 2u;
    c.bytes_per_sample = 2u;
    c.sample_rate = 48000u;
    c.host_clock_hz = 19200000u;
    c.zero_timestamp_ticks = 0x1234u;
    c.output_ring_bytes = c.output_frames * c.channels * c.bytes_per_sample;
    return c;
}

static void test_shape(void)
{
    QcomHalControlBlock block;
    const QcomHalConfig c = config();
    check(qcom_hal_abi_init(&block, &c) == QcomHalOk, "a consistent configuration fills the block");
    check(block.magic == QCOM_HAL_MAGIC && block.version == QCOM_HAL_VERSION, "the block carries the magic and version");
    check(block.struct_bytes == sizeof(QcomHalControlBlock), "the block records its own size");
    check(block.flags == QCOM_HAL_FLAG_OUTPUT, "this version publishes output only");
    check(block.output_write_index == 0 && block.output_read_index == 0, "both indices start at zero");
    check(block.underruns == 0 && block.late == 0 && block.silent == 0 && block.overruns == 0,
          "the counters start at zero");
    check(block.zero_timestamp_ticks == c.zero_timestamp_ticks, "the timeline anchor is carried");
    check(qcom_hal_abi_validate(&block, sizeof block) == QcomHalOk, "the block it just filled validates");

    /* Across a byte boundary, as the other side would see it. */
    uint8_t bytes[QCOM_HAL_BLOCK_BYTES + 8];
    memset(bytes, 0xa5, sizeof bytes);
    check(qcom_hal_abi_export(&block, bytes, sizeof bytes) == QcomHalOk, "the block exports into a byte buffer");
    check(qcom_hal_abi_validate(bytes, sizeof bytes) == QcomHalOk, "the exported block validates where it lands");
    check(((QcomHalControlBlock *)bytes)->output_frames == c.output_frames, "the exported block has the same values");
    check(bytes[QCOM_HAL_BLOCK_BYTES] == 0xa5, "the export writes exactly the block and not a byte more");

    /* A live index survives the copy, which is what the two sides rely on. */
    ((QcomHalControlBlock *)bytes)->output_write_index = 480u;
    check(qcom_hal_abi_validate(bytes, sizeof bytes) == QcomHalOk &&
          ((QcomHalControlBlock *)bytes)->output_write_index == 480u,
          "a live index travels with the block");
}

static void test_init_refusals(void)
{
    QcomHalControlBlock block;
    QcomHalConfig c = config();

    c.output_ring_address = 0;
    check(qcom_hal_abi_init(&block, &c) == QcomHalBadRing, "a ring without an address is refused");
    c = config(); c.output_frames = 0;
    check(qcom_hal_abi_init(&block, &c) == QcomHalBadRing, "a ring without frames is refused");
    c = config(); c.output_period_frames = 0;
    check(qcom_hal_abi_init(&block, &c) == QcomHalBadRing, "a ring without a period is refused");
    c = config(); c.output_frames = 1000u;   /* not a multiple of the period */
    check(qcom_hal_abi_init(&block, &c) == QcomHalBadRing, "a ring whose size is not whole periods is refused");
    c = config(); c.output_frames = 65536u; c.output_ring_bytes = 65536u * c.channels * c.bytes_per_sample;
    check(qcom_hal_abi_init(&block, &c) == QcomHalBadRing,
          "the plan's 65536-frame ring with a 480-frame period is refused: 480 does not divide 65536");
    c = config(); c.output_periods_in_flight = 0;
    check(qcom_hal_abi_init(&block, &c) == QcomHalBadRing, "a ring with no periods in flight is refused");
    c = config(); c.output_periods_in_flight = 200u;
    check(qcom_hal_abi_init(&block, &c) == QcomHalBadRing, "more periods in flight than the ring holds is refused");
    c = config(); c.channels = 0;
    check(qcom_hal_abi_init(&block, &c) == QcomHalBadRing, "a block without channels is refused");
    c = config(); c.host_clock_hz = 0;
    check(qcom_hal_abi_init(&block, &c) == QcomHalBadRing, "a block without a host clock is refused");
    c = config(); c.output_ring_bytes = 8u;   /* disagrees with frames, channels and width */
    check(qcom_hal_abi_init(&block, &c) == QcomHalBadRing, "a ring size that disagrees with the format is refused");
    check(qcom_hal_abi_init(0, &c) == QcomHalBadArgument, "a null block is refused");
}

static void test_validate_refusals(void)
{
    QcomHalControlBlock block;
    const QcomHalConfig c = config();
    (void)qcom_hal_abi_init(&block, &c);

    check(qcom_hal_abi_validate(0, sizeof block) == QcomHalBadArgument, "a null block is refused");
    check(qcom_hal_abi_validate(&block, sizeof block - 1u) == QcomHalBadSize, "a buffer shorter than the block is refused");

    QcomHalControlBlock bad = block;
    bad.magic ^= 1u;
    check(qcom_hal_abi_validate(&bad, sizeof bad) == QcomHalBadMagic, "a wrong magic is refused before anything else");
    bad = block; bad.version = QCOM_HAL_VERSION + 1u;
    check(qcom_hal_abi_validate(&bad, sizeof bad) == QcomHalBadVersion, "a version this side does not read is refused");
    bad = block; bad.struct_bytes = QCOM_HAL_BLOCK_BYTES + 8u;
    check(qcom_hal_abi_validate(&bad, sizeof bad) == QcomHalBadSize,
          "a producer that compiled a different size is refused");
    bad = block; bad.flags = QCOM_HAL_FLAG_OUTPUT | QCOM_HAL_FLAG_INPUT;
    check(qcom_hal_abi_validate(&bad, sizeof bad) == QcomHalUnsupported,
          "an input ring is refused while this version publishes output only");
    bad = block; bad.flags = 0;
    check(qcom_hal_abi_validate(&bad, sizeof bad) == QcomHalUnsupported, "a block publishing nothing is refused");
    bad = block; bad.output_frames = 1000u;
    check(qcom_hal_abi_validate(&bad, sizeof bad) == QcomHalBadRing, "a ring that is not whole periods is refused");
    bad = block; bad.output_ring_bytes = 8u;
    check(qcom_hal_abi_validate(&bad, sizeof bad) == QcomHalBadRing, "a ring size that disagrees with the format is refused");
    bad = block; bad.sample_rate = 0;
    check(qcom_hal_abi_validate(&bad, sizeof bad) == QcomHalBadRing, "a block without a rate is refused");

    uint8_t small[QCOM_HAL_BLOCK_BYTES - 1u];
    check(qcom_hal_abi_export(&block, small, sizeof small) == QcomHalBadSize, "an export into a short buffer is refused");
}

int main(void)
{
    test_shape();
    test_init_refusals();
    test_validate_refusals();
    printf("PASS QcomHalAbi: %d checks\n", checks);
    return failed ? 1 : 0;
}
