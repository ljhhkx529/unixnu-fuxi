/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The CS35 family's step runner, copied from sheng's ShengCs35l43 unchanged in mechanism: a program is a list of
 * steps and the runner turns each one into a single bus action, never waiting. A register write is one eight-byte
 * transfer (register then value); a read is a four-byte transfer with a four-byte read behind it; a read-modify-write
 * is the read, then - only when the bits actually change - the write. Registers and values are 32-bit big-endian on
 * the wire.
 *
 * The register addresses live in whoever builds the program, not here: the runner knows bytes, not parts. */
#ifndef CS35_RUN_H
#define CS35_RUN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {Cs35Write = 1, Cs35Update = 2, Cs35Read = 3, Cs35DelayMs = 4};
typedef struct {uint8_t op; uint32_t reg, mask, value;} Cs35Step;

typedef struct {
    const Cs35Step *steps;
    unsigned count, index;
    uint8_t phase;                   /* 0 next step, 1 read of a read-modify-write, 2 write in flight, 3 read in flight,
                                        4 the write of a read-modify-write is next */
    uint32_t pending;
    uint32_t reads[4];
    unsigned read_count;
} Cs35Run;

enum {Cs35Transfer = 1, Cs35Wait = 2, Cs35Finished = 3};
typedef struct {
    uint8_t bytes[8];
    uint32_t length;                 /* bytes to write */
    uint32_t read_length;            /* 0 or 4: then a read follows the write */
    uint32_t delay_ms;               /* Cs35Wait */
} Cs35Action;

void cs35_run_start(Cs35Run *r, const Cs35Step *steps, unsigned count);
/* What to do next: Cs35Transfer (perform a, then call cs35_run_result), Cs35Wait (wait a.delay_ms, then call
 * cs35_run_next again) or Cs35Finished. */
int cs35_run_next(Cs35Run *r, Cs35Action *a);
/* The transfer finished; value: the 32-bit value read, when one was read. */
void cs35_run_result(Cs35Run *r, uint32_t value);
/* A value read on the wire (first byte in bits 7:0) as the register value. */
static inline uint32_t cs35_wire_value(uint32_t rx)
{
    return (rx >> 24) | ((rx >> 8) & 0xff00u) | ((rx << 8) & 0xff0000u) | (rx << 24);
}

#ifdef __cplusplus
}
#endif

#endif
