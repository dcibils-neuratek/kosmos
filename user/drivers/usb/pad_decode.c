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

/*--------------------------------------------------------------------------
 * Xbox One and Series: GIP.
 *------------------------------------------------------------------------*/

#define GIP_CMD_ACK          0x01u
#define GIP_CMD_ANNOUNCE     0x02u
#define GIP_CMD_VIRTUAL_KEY  0x07u
#define GIP_CMD_INPUT        0x20u
#define GIP_OPT_ACK          0x10u
#define GIP_OPT_INTERNAL     0x20u

static const int8_t one_byte4[8] = {
    -1, -1, PAD_START, PAD_SELECT,
    PAD_SOUTH, PAD_EAST, PAD_WEST, PAD_NORTH,     /* A, B, X, Y */
};

static const int8_t one_byte5[8] = {
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_TL, PAD_TR, PAD_THUMBL, PAD_THUMBR,
};

enum pad_xone_kind pad_decode_xone(const uint8_t *m, unsigned length,
                                   struct pad_state *s, bool *ack,
                                   uint8_t *seq)
{
    *ack = false;

    if (m == 0 || length < 4u) {
        return PAD_XONE_OTHER;
    }

    *seq = m[2];

    if (m[0] == GIP_CMD_ANNOUNCE) {
        return PAD_XONE_ANNOUNCE;
    }

    if (m[0] == GIP_CMD_VIRTUAL_KEY && length >= 5u) {
        *ack = m[1] == (GIP_OPT_ACK | GIP_OPT_INTERNAL);

        if ((m[4] & 0x03u) != 0) {
            s->buttons |= 1u << PAD_MODE;
        } else {
            s->buttons &= ~(1u << PAD_MODE);
        }

        return PAD_XONE_GUIDE;
    }

    if (m[0] == GIP_CMD_INPUT && length >= 18u) {
        uint32_t buttons = s->buttons & (1u << PAD_MODE);
        unsigned bit;

        for (bit = 0; bit < 8u; bit++) {
            if ((m[4] & (1u << bit)) != 0 && one_byte4[bit] >= 0) {
                buttons |= 1u << one_byte4[bit];
            }

            if ((m[5] & (1u << bit)) != 0) {
                buttons |= 1u << one_byte5[bit];
            }
        }

        s->buttons = buttons;

        /* Ten bits, 0 to 1023, kept as the 360's eight so one threshold
         * serves both. */
        s->lt = (uint8_t)((uint16_t)word_at(m, 6) >> 2);
        s->rt = (uint8_t)((uint16_t)word_at(m, 8) >> 2);
        s->lx = word_at(m, 10);
        s->ly = word_at(m, 12);
        s->rx = word_at(m, 14);
        s->ry = word_at(m, 16);
        return PAD_XONE_INPUT;
    }

    return PAD_XONE_OTHER;
}

/*
 * `xpad`'s `xboxone_init_packets`, the ones for Microsoft's pads and every
 * pad: a vendor and product of 0 is every pad. The sequence byte, the
 * third, is written over with the count.
 */
static const struct {
    uint16_t vendor, product;
    uint8_t  length;
    uint8_t  bytes[8];
} one_init[PAD_XONE_STEPS] = {
    { 0x0000, 0x0000, 5, { 0x05, 0x20, 0x00, 0x01, 0x00 } },       /* power on */
    { 0x045e, 0x02ea, 5, { 0x05, 0x20, 0x00, 0x0f, 0x06 } },       /* One S */
    { 0x045e, 0x0b00, 5, { 0x05, 0x20, 0x00, 0x0f, 0x06 } },       /* Elite 2 */
    { 0x045e, 0x0b00, 6, { 0x4d, 0x10, 0x01, 0x02, 0x07, 0x00 } }, /* its input */
    { 0x0000, 0x0000, 7, { 0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 } }, /* light */
    { 0x0000, 0x0000, 6, { 0x06, 0x20, 0x00, 0x02, 0x01, 0x00 } }, /* auth done */
};

unsigned pad_xone_init(uint16_t vendor, uint16_t product, unsigned step,
                       uint8_t seq, uint8_t *out)
{
    unsigned i;

    if (step >= PAD_XONE_STEPS) {
        return 0;
    }

    if (one_init[step].vendor != 0
        && (one_init[step].vendor != vendor
            || one_init[step].product != product)) {
        return 0;
    }

    for (i = 0; i < one_init[step].length; i++) {
        out[i] = one_init[step].bytes[i];
    }

    out[2] = seq;
    return one_init[step].length;
}

/* `xpadone_ack_mode_report`'s thirteen bytes, with the pad's sequence. */
unsigned pad_xone_ack(uint8_t seq, uint8_t *out)
{
    static const uint8_t ack[13] = {
        GIP_CMD_ACK, GIP_OPT_INTERNAL, 0x00, 0x09,
        0x00, GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00,
    };
    unsigned i;

    for (i = 0; i < sizeof(ack); i++) {
        out[i] = ack[i];
    }

    out[2] = seq;
    return sizeof(ack);
}
