/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomHalAbi.h"

int qcom_hal_abi_init(QcomHalControlBlock *block, const QcomHalConfig *config)
{
    if (!block || !config) return QcomHalBadArgument;
    if (!config->output_ring_address || !config->output_ring_bytes) return QcomHalBadRing;
    if (!config->output_frames || !config->output_period_frames) return QcomHalBadRing;
    if (config->output_frames % config->output_period_frames) return QcomHalBadRing;
    if (!config->output_periods_in_flight ||
        config->output_periods_in_flight > config->output_frames / config->output_period_frames) return QcomHalBadRing;
    if (!config->channels || !config->bytes_per_sample || !config->sample_rate || !config->host_clock_hz)
        return QcomHalBadRing;
    if ((uint64_t)config->output_frames * config->channels * config->bytes_per_sample != config->output_ring_bytes)
        return QcomHalBadRing;

    for (uint32_t i = 0; i < sizeof *block; ++i) ((uint8_t *)block)[i] = 0;
    block->magic = QCOM_HAL_MAGIC;
    block->version = QCOM_HAL_VERSION;
    block->struct_bytes = (uint32_t)sizeof *block;
    block->flags = QCOM_HAL_FLAG_OUTPUT;
    block->output_ring_address = config->output_ring_address;
    block->output_ring_bytes = config->output_ring_bytes;
    block->output_frames = config->output_frames;
    block->output_period_frames = config->output_period_frames;
    block->output_periods_in_flight = config->output_periods_in_flight;
    block->channels = config->channels;
    block->bytes_per_sample = config->bytes_per_sample;
    block->sample_rate = config->sample_rate;
    block->host_clock_hz = config->host_clock_hz;
    block->zero_timestamp_ticks = config->zero_timestamp_ticks;
    return QcomHalOk;
}

int qcom_hal_abi_validate(const void *bytes, size_t length)
{
    if (!bytes) return QcomHalBadArgument;
    if (length < QCOM_HAL_BLOCK_BYTES) return QcomHalBadSize;
    const QcomHalControlBlock *block = bytes;
    if (block->magic != QCOM_HAL_MAGIC) return QcomHalBadMagic;
    if (block->version != QCOM_HAL_VERSION) return QcomHalBadVersion;
    /* The size the producer compiled must be the size this side reads, or the offsets below mean something else. */
    if (block->struct_bytes != QCOM_HAL_BLOCK_BYTES) return QcomHalBadSize;
    if (block->flags & QCOM_HAL_FLAG_INPUT) return QcomHalUnsupported;   /* output only in this version */
    if (!(block->flags & QCOM_HAL_FLAG_OUTPUT)) return QcomHalUnsupported;
    if (!block->output_ring_address || !block->output_ring_bytes) return QcomHalBadRing;
    if (!block->output_frames || !block->output_period_frames) return QcomHalBadRing;
    if (block->output_frames % block->output_period_frames) return QcomHalBadRing;
    if (!block->output_periods_in_flight ||
        block->output_periods_in_flight > block->output_frames / block->output_period_frames) return QcomHalBadRing;
    if (!block->channels || !block->bytes_per_sample || !block->sample_rate || !block->host_clock_hz)
        return QcomHalBadRing;
    if ((uint64_t)block->output_frames * block->channels * block->bytes_per_sample != block->output_ring_bytes)
        return QcomHalBadRing;
    return QcomHalOk;
}

int qcom_hal_abi_export(const QcomHalControlBlock *block, void *bytes, size_t capacity)
{
    if (!block || !bytes) return QcomHalBadArgument;
    if (capacity < QCOM_HAL_BLOCK_BYTES) return QcomHalBadSize;
    for (uint32_t i = 0; i < QCOM_HAL_BLOCK_BYTES; ++i) ((uint8_t *)bytes)[i] = ((const uint8_t *)block)[i];
    return QcomHalOk;
}
