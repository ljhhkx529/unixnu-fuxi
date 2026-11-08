/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Cs35Run.h"

void cs35_run_start(Cs35Run *r, const Cs35Step *steps, unsigned count)
{
    r->steps = steps; r->count = count; r->index = 0; r->phase = 0; r->read_count = 0; r->pending = 0;
    for (unsigned i = 0; i < 4; ++i) r->reads[i] = 0;
}

static void be32(uint8_t *b, uint32_t v) {b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;}

int cs35_run_next(Cs35Run *r, Cs35Action *a)
{
    a->length = a->read_length = a->delay_ms = 0;
    if (r->index >= r->count) return Cs35Finished;
    const Cs35Step *s = &r->steps[r->index];
    if (r->phase == 4) {   /* the write half of a read-modify-write */
        be32(a->bytes, s->reg); be32(a->bytes + 4, r->pending); a->length = 8;
        r->phase = 2;
        return Cs35Transfer;
    }
    switch (s->op) {
    case Cs35DelayMs:
        a->delay_ms = s->value;
        r->index++;
        return Cs35Wait;
    case Cs35Write:
        be32(a->bytes, s->reg); be32(a->bytes + 4, s->value); a->length = 8;
        r->phase = 2;
        return Cs35Transfer;
    case Cs35Update:
    case Cs35Read:
        be32(a->bytes, s->reg); a->length = 4; a->read_length = 4;
        r->phase = s->op == Cs35Update ? 1 : 3;
        return Cs35Transfer;
    default:
        r->index = r->count;
        return Cs35Finished;
    }
}

void cs35_run_result(Cs35Run *r, uint32_t value)
{
    if (r->index >= r->count) return;
    const Cs35Step *s = &r->steps[r->index];
    if (r->phase == 1) {
        /* The read half of a read-modify-write: the write half is the next transfer, unless the bits are already set. */
        r->pending = (value & ~s->mask) | s->value;
        if (r->pending == value) {r->phase = 0; r->index++;} else r->phase = 4;
        return;
    }
    if (r->phase == 3 && r->read_count < 4) r->reads[r->read_count++] = value;
    r->phase = 0;
    r->index++;
}
