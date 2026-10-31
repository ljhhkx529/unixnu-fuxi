// SPDX-License-Identifier: BSD-2-Clause-Patent
// Contact tracking for FTM5 events: the controller reports per-contact enter, motion and leave events with its own
// tracking id (0..9), so the host only keeps a slot per id. Pure: no allocation, no I/O.
#ifndef FTS_TOUCH_CORE_H
#define FTS_TOUCH_CORE_H
#include <stdint.h>
#include "FtsProtocol.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FTS_SLOTS FTS_TOUCH_ID_MAX

struct fts_point {
    int id, x, y;          // controller units (fuxi: 0..10800 x 0..24000, ten times the panel pixels)
    int major, minor;
    uint8_t type;
};

struct fts_touch_core {
    int super_resolution;  // DT fts,support-super-resolution (fuxi 1)
    int x_max, y_max;      // DT fts,x-max / fts,y-max: coordinates are clamped to these
    uint16_t down;         // bit n: slot n is touching
    struct fts_point slot[FTS_SLOTS];
    // Counters for the diagnostic page.
    uint32_t enters, motions, leaves, hovers, invalid, status, user, errors, readies, unknown;
    uint8_t last_error[2];         // ev[1], ev[2] of the last error event
    uint8_t last_status[3];        // ev[1..3] of the last status event
    int reset_needed;              // a watchdog or ESD error, or an unrequested ready event: the chip must be reset or re-armed
};

void fts_touch_init(struct fts_touch_core *t, int super_resolution, int x_max, int y_max);
// Lifts every contact (the driver's release_all_touches).
void fts_touch_release_all(struct fts_touch_core *t);
// Applies one event. Returns 1 when the set of touching contacts or a position changed, 0 otherwise.
int fts_touch_apply(struct fts_touch_core *t, const uint8_t ev[FTS_EVENT_SIZE]);
// Applies count events (FTS_EVENT_SIZE bytes each) and stops at a NOEVENT entry, as the interrupt handler does.
int fts_touch_apply_events(struct fts_touch_core *t, const uint8_t *events, unsigned count);
// Copies the touching contacts in id order; returns how many.
unsigned fts_touch_contacts(const struct fts_touch_core *t, struct fts_point *out, unsigned capacity);

// Decodes the position, size, id and type of a touch event.
void fts_decode_point(const uint8_t ev[FTS_EVENT_SIZE], int super_resolution, struct fts_point *p);

// Board transform from controller units to the units a consumer expects: optional swap and mirror, then a linear
// scale from 0..from_max to 0..to_max on each axis. Integer arithmetic, rounding down.
#define FTS_MAP_SWAP_XY 1u
#define FTS_MAP_MIRROR_X 2u
#define FTS_MAP_MIRROR_Y 4u
struct fts_map {
    int from_x_max, from_y_max, to_x_max, to_y_max;
    unsigned flags;
};
void fts_map_point(const struct fts_map *m, int x, int y, int *out_x, int *out_y);

#ifdef __cplusplus
}
#endif
#endif
