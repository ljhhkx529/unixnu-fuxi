// SPDX-License-Identifier: BSD-2-Clause-Patent
// Contact tracking for FTM5 events. The handling of each event class follows the driver's handlers: finger, glove
// and palm touch, hover does not, an invalid type is ignored; watchdog and ESD errors make the driver reset the chip.
#include "FtsTouchCore.h"

void fts_decode_point(const uint8_t ev[FTS_EVENT_SIZE], int super_resolution, struct fts_point *p) {
    p->type = ev[1] & 0x0F;
    p->id = (ev[1] & 0xF0) >> 4;
    if (super_resolution) {
        p->x = (ev[3] << 8) | ev[2];
        p->y = (ev[5] << 8) | ev[4];
    } else {
        p->x = ((ev[3] & 0x0F) << 8) | ev[2];
        p->y = (ev[4] << 4) | ((ev[3] & 0xF0) >> 4);
    }
    p->major = ((ev[0] & 0x0C) << 2) | ((ev[6] & 0xF0) >> 4);
    p->minor = ((ev[7] & 0xC0) >> 2) | (ev[6] & 0x0F);
}

void fts_touch_init(struct fts_touch_core *t, int super_resolution, int x_max, int y_max) {
    uint8_t *b = (uint8_t *)t;
    for (unsigned i = 0; i < sizeof *t; ++i) b[i] = 0;
    t->super_resolution = super_resolution;
    t->x_max = x_max;
    t->y_max = y_max;
}

void fts_touch_release_all(struct fts_touch_core *t) {t->down = 0;}

static int set_slot(struct fts_touch_core *t, const struct fts_point *p) {
    struct fts_point *s = &t->slot[p->id];
    const uint16_t bit = (uint16_t)(1u << p->id);
    const int changed = !(t->down & bit) || s->x != p->x || s->y != p->y;
    *s = *p;
    t->down |= bit;
    return changed;
}

static int clear_slot(struct fts_touch_core *t, int id) {
    const uint16_t bit = (uint16_t)(1u << id);
    const int changed = (t->down & bit) != 0;
    t->down &= (uint16_t)~bit;
    return changed;
}

int fts_touch_apply(struct fts_touch_core *t, const uint8_t ev[FTS_EVENT_SIZE]) {
    if (ev[0] == FTS_EVT_NOEVENT) return 0;
    if (ev[0] == FTS_EVT_CONTROLLER_READY) {
        // Unrequested: the chip reset itself and dropped its scan mode and every contact.
        ++t->readies;
        t->reset_needed = 1;
        const int changed = t->down != 0;
        fts_touch_release_all(t);
        return changed;
    }
    switch (fts_event_class(ev[0])) {
    case FTS_CLASS_ENTER:
    case FTS_CLASS_MOTION: {
        struct fts_point p;
        fts_decode_point(ev, t->super_resolution, &p);
        if (p.id >= FTS_SLOTS) {++t->invalid; return 0;}
        if (p.x > t->x_max) p.x = t->x_max;
        if (p.y > t->y_max) p.y = t->y_max;
        if (fts_event_class(ev[0]) == FTS_CLASS_ENTER) ++t->enters; else ++t->motions;
        switch (p.type) {
        case FTS_TOUCH_FINGER: case FTS_TOUCH_GLOVE: case FTS_TOUCH_PALM: case FTS_TOUCH_STYLUS:
            return set_slot(t, &p);
        case FTS_TOUCH_HOVER:
            ++t->hovers;
            return clear_slot(t, p.id);
        default:
            ++t->invalid;
            return 0;
        }
    }
    case FTS_CLASS_LEAVE: {
        const int id = (ev[1] & 0xF0) >> 4;
        ++t->leaves;
        if (id >= FTS_SLOTS) {++t->invalid; return 0;}
        return clear_slot(t, id);
    }
    case FTS_CLASS_STATUS:
        ++t->status;
        t->last_status[0] = ev[1]; t->last_status[1] = ev[2]; t->last_status[2] = ev[3];
        return 0;
    case FTS_CLASS_USER:
        ++t->user;
        return 0;
    case FTS_CLASS_ERROR:
        ++t->errors;
        t->last_error[0] = ev[1]; t->last_error[1] = ev[2];
        if (ev[1] == FTS_ERROR_WATCHDOG || ev[1] == FTS_ERROR_ESD) {
            t->reset_needed = 1;
            const int changed = t->down != 0;
            fts_touch_release_all(t);
            return changed;
        }
        return 0;
    default:
        ++t->unknown;
        return 0;
    }
}

int fts_touch_apply_events(struct fts_touch_core *t, const uint8_t *events, unsigned count) {
    int changed = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *ev = events + (size_t)i * FTS_EVENT_SIZE;
        if (ev[0] == FTS_EVT_NOEVENT) break;
        changed |= fts_touch_apply(t, ev);
    }
    return changed;
}

unsigned fts_touch_contacts(const struct fts_touch_core *t, struct fts_point *out, unsigned capacity) {
    unsigned n = 0;
    for (int id = 0; id < FTS_SLOTS && n < capacity; ++id) {
        if (t->down & (1u << id)) out[n++] = t->slot[id];
    }
    return n;
}

static int map_axis(int v, int from_max, int to_max) {
    if (v < 0) v = 0;
    if (v > from_max) v = from_max;
    return from_max > 0 ? (int)((int64_t)v * to_max / from_max) : 0;
}

void fts_map_point(const struct fts_map *m, int x, int y, int *out_x, int *out_y) {
    int ox, oy;
    if (m->flags & FTS_MAP_SWAP_XY) {
        ox = map_axis(y, m->from_y_max, m->to_x_max);
        oy = map_axis(x, m->from_x_max, m->to_y_max);
    } else {
        ox = map_axis(x, m->from_x_max, m->to_x_max);
        oy = map_axis(y, m->from_y_max, m->to_y_max);
    }
    if (m->flags & FTS_MAP_MIRROR_X) ox = m->to_x_max - ox;
    if (m->flags & FTS_MAP_MIRROR_Y) oy = m->to_y_max - oy;
    *out_x = ox;
    *out_y = oy;
}
