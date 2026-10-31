// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of FtsTouchCore.c: event classes, slots, clamping, error recovery flags, board transform.
#include "../FtsTouchCore.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void ev(uint8_t out[8], uint8_t id, int touch_id, int type, int x, int y) {
    out[0] = id;
    out[1] = (uint8_t)(touch_id << 4 | type);
    out[2] = (uint8_t)x; out[3] = (uint8_t)(x >> 8);
    out[4] = (uint8_t)y; out[5] = (uint8_t)(y >> 8);
    out[6] = 0x34; out[7] = 0x80;
}

int main(void) {
    struct fts_touch_core t;
    struct fts_point pts[FTS_SLOTS];
    uint8_t e[8];
    fts_touch_init(&t, 1, 10800, 24000);

    // Decoding (super resolution): the same bytes as the board test, id 2, finger, x 6700, y 20492.
    const uint8_t raw[8] = {0x13, 0x21, 0x2C, 0x1A, 0x0C, 0x50, 0x34, 0x82};
    struct fts_point p;
    fts_decode_point(raw, 1, &p);
    CHECK(p.id == 2 && p.type == FTS_TOUCH_FINGER && p.x == 6700 && p.y == 20492);
    CHECK(p.major == 3 && p.minor == (((0x82 & 0xC0) >> 2) | 4));
    fts_decode_point(raw, 0, &p);
    CHECK(p.x == ((0x1A & 0x0F) << 8 | 0x2C) && p.y == (0x0C << 4 | (0x1A & 0xF0) >> 4));

    // Enter with the major axis top bits in byte 0 (0x17) is still an enter event.
    CHECK(fts_is_touch_event(0x17) && fts_is_touch_event(0x2F) && fts_is_touch_event(0x33));
    CHECK(!fts_is_touch_event(0x03) && !fts_is_touch_event(0x00) && !fts_is_touch_event(0x43));
    ev(e, 0x17, 3, FTS_TOUCH_FINGER, 1000, 2000);
    CHECK(fts_touch_apply(&t, e) == 1 && t.enters == 1);
    CHECK(fts_touch_contacts(&t, pts, FTS_SLOTS) == 1 && pts[0].id == 3 && pts[0].x == 1000 && pts[0].y == 2000);
    CHECK(pts[0].major == (((0x17 & 0x0C) << 2) | 3));

    // Motion: moved changes, the same position does not.
    ev(e, 0x23, 3, FTS_TOUCH_FINGER, 1010, 2000);
    CHECK(fts_touch_apply(&t, e) == 1);
    CHECK(fts_touch_apply(&t, e) == 0 && t.motions == 2);

    // A second finger and a palm: ordered by id.
    ev(e, 0x13, 1, FTS_TOUCH_PALM, 5000, 6000);
    CHECK(fts_touch_apply(&t, e) == 1);
    CHECK(fts_touch_contacts(&t, pts, FTS_SLOTS) == 2 && pts[0].id == 1 && pts[1].id == 3);
    CHECK(fts_touch_contacts(&t, pts, 1) == 1);

    // Clamping to the axis maxima.
    ev(e, 0x23, 1, FTS_TOUCH_FINGER, 11000, 30000);
    fts_touch_apply(&t, e);
    fts_touch_contacts(&t, pts, FTS_SLOTS);
    CHECK(pts[0].x == 10800 && pts[0].y == 24000);

    // Hover lifts, leave lifts, an id above 9 and an invalid type are ignored.
    ev(e, 0x23, 1, FTS_TOUCH_HOVER, 5000, 6000);
    CHECK(fts_touch_apply(&t, e) == 1 && t.hovers == 1);
    ev(e, 0x33, 3, 0, 0, 0);
    CHECK(fts_touch_apply(&t, e) == 1 && t.down == 0);
    CHECK(fts_touch_apply(&t, e) == 0);
    ev(e, 0x13, 12, FTS_TOUCH_FINGER, 1, 1);
    CHECK(fts_touch_apply(&t, e) == 0 && t.invalid == 1);
    ev(e, 0x13, 2, FTS_TOUCH_INVALID, 1, 1);
    CHECK(fts_touch_apply(&t, e) == 0 && t.invalid == 2 && t.down == 0);

    // Status and user events are counted only.
    const uint8_t status[8] = {0x43, 0x01, 0xA0, 0x00, 0, 0, 0, 0};
    CHECK(fts_touch_apply(&t, status) == 0 && t.status == 1 && t.last_status[0] == 0x01 && t.last_status[1] == 0xA0);

    // Watchdog error: contacts lifted and a reset requested; a CRC error is only recorded.
    ev(e, 0x13, 0, FTS_TOUCH_FINGER, 10, 10);
    fts_touch_apply(&t, e);
    const uint8_t crc[8] = {0xF3, 0x20, 0, 0, 0, 0, 0, 0};
    CHECK(fts_touch_apply(&t, crc) == 0 && !t.reset_needed && t.down == 1);
    const uint8_t wd[8] = {0xF3, 0x06, 0, 0, 0, 0, 0, 0};
    CHECK(fts_touch_apply(&t, wd) == 1 && t.reset_needed && t.down == 0 && t.last_error[0] == 0x06);

    // An unrequested ready event also requests re-arming.
    fts_touch_init(&t, 1, 10800, 24000);
    const uint8_t ready[8] = {0x03, 0, 0, 0, 0, 0, 0, 0};
    CHECK(fts_touch_apply(&t, ready) == 0 && t.reset_needed && t.readies == 1);

    // A batch stops at the first NOEVENT entry.
    fts_touch_init(&t, 1, 10800, 24000);
    uint8_t batch[4 * 8];
    ev(batch, 0x13, 0, FTS_TOUCH_FINGER, 100, 100);
    memset(batch + 8, 0, 8);
    ev(batch + 16, 0x13, 1, FTS_TOUCH_FINGER, 200, 200);
    ev(batch + 24, 0x13, 2, FTS_TOUCH_FINGER, 300, 300);
    CHECK(fts_touch_apply_events(&t, batch, 4) == 1 && t.down == 1);

    // Board transform: fuxi 0..10800 x 0..24000 to the 30480 x 20320 range the HID source scales from.
    const struct fts_map id = {10800, 24000, 30480, 20320, 0};
    int x, y;
    fts_map_point(&id, 0, 0, &x, &y);
    CHECK(x == 0 && y == 0);
    fts_map_point(&id, 10800, 24000, &x, &y);
    CHECK(x == 30480 && y == 20320);
    fts_map_point(&id, 5400, 12000, &x, &y);
    CHECK(x == 15240 && y == 10160);
    fts_map_point(&id, -5, 99999, &x, &y);
    CHECK(x == 0 && y == 20320);
    const struct fts_map mir = {10800, 24000, 30480, 20320, FTS_MAP_MIRROR_X | FTS_MAP_MIRROR_Y};
    fts_map_point(&mir, 0, 24000, &x, &y);
    CHECK(x == 30480 && y == 0);
    const struct fts_map sw = {10800, 24000, 24000, 10800, FTS_MAP_SWAP_XY};
    fts_map_point(&sw, 100, 200, &x, &y);
    CHECK(x == 200 && y == 100);

    printf(failures ? "touch-core: %d failed\n" : "touch-core: all passed\n", failures);
    return failures != 0;
}
