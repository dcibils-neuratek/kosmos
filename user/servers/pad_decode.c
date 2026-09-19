/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A game controller's report, decoded. `pad_decode.h` has the layout and
 * where it came from.
 */

#include "pad_decode.h"

#define X360_TYPE_INPUT   0x00u
#define X360_LENGTH       0x14u

const uint16_t pad_codes[PAD_BITS] = {
    [PAD_SOUTH]  = 0x130, [PAD_EAST]   = 0x131,
    [PAD_WEST]   = 0x134, [PAD_NORTH]  = 0x133,
    [PAD_TL]     = 0x136, [PAD_TR]     = 0x137,
    [PAD_TL2]    = 0x138, [PAD_TR2]    = 0x139,
    [PAD_SELECT] = 0x13a, [PAD_START]  = 0x13b,
    [PAD_MODE]   = 0x13c, [PAD_THUMBL] = 0x13d, [PAD_THUMBR] = 0x13e,
    [PAD_UP]     = 0x220, [PAD_DOWN]   = 0x221,
    [PAD_LEFT]   = 0x222, [PAD_RIGHT]  = 0x223,
};

const char *const pad_names[PAD_BITS] = {
    [PAD_SOUTH]  = "south", [PAD_EAST]   = "east",
    [PAD_WEST]   = "west",  [PAD_NORTH]  = "north",
    [PAD_TL]     = "L",     [PAD_TR]     = "R",
    [PAD_TL2]    = "left trigger", [PAD_TR2] = "right trigger",
    [PAD_SELECT] = "select", [PAD_START] = "start",
    [PAD_MODE]   = "home",  [PAD_THUMBL] = "left stick",
    [PAD_THUMBR] = "right stick",
    [PAD_UP]     = "up",    [PAD_DOWN]   = "down",
    [PAD_LEFT]   = "left",  [PAD_RIGHT]  = "right",
};

/* Byte 2 and byte 3, bit by bit, as the report lays them out. */
static const int8_t byte2[8] = {
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_START, PAD_SELECT, PAD_THUMBL, PAD_THUMBR,
};

static const int8_t byte3[8] = {
    PAD_TL, PAD_TR, PAD_MODE, -1,
    PAD_SOUTH, PAD_EAST, PAD_WEST, PAD_NORTH,     /* A, B, X, Y */
};

static int16_t word_at(const uint8_t *r, unsigned at)
{
    return (int16_t)(uint16_t)(r[at] | (unsigned)r[at + 1u] << 8);
}

bool pad_decode_x360(const uint8_t *report, unsigned length,
                     struct pad_state *out)
{
    unsigned bit;
    uint32_t buttons = 0;

    if (report == 0 || length < X360_LENGTH
        || report[0] != X360_TYPE_INPUT || report[1] != X360_LENGTH) {
        return false;
    }

    for (bit = 0; bit < 8u; bit++) {
        if ((report[2] & (1u << bit)) != 0) {
            buttons |= 1u << byte2[bit];
        }

        if ((report[3] & (1u << bit)) != 0 && byte3[bit] >= 0) {
            buttons |= 1u << byte3[bit];
        }
    }

    out->buttons = buttons;
    out->lt = report[4];
    out->rt = report[5];
    out->lx = word_at(report, 6);
    out->ly = word_at(report, 8);
    out->rx = word_at(report, 10);
    out->ry = word_at(report, 12);
    return true;
}

/* Past the threshold, or held and not yet back inside the release. */
static bool held(int value, bool was)
{
    return value > PAD_STICK_PRESS || (was && value > PAD_STICK_RELEASE);
}

uint32_t pad_pressed(const struct pad_state *s, uint32_t before)
{
    uint32_t on = s->buttons;

#define WAS(b) ((before & (1u << (b))) != 0)

    if (held(s->ly, WAS(PAD_UP)))        on |= 1u << PAD_UP;
    if (held(-(int)s->ly, WAS(PAD_DOWN))) on |= 1u << PAD_DOWN;
    if (held(s->lx, WAS(PAD_RIGHT)))     on |= 1u << PAD_RIGHT;
    if (held(-(int)s->lx, WAS(PAD_LEFT))) on |= 1u << PAD_LEFT;

#undef WAS

    if (s->lt >= PAD_TRIGGER_PRESS) on |= 1u << PAD_TL2;
    if (s->rt >= PAD_TRIGGER_PRESS) on |= 1u << PAD_TR2;

    return on;
}
