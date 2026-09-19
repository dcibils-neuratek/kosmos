/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * An Xbox 360 controller's reports, decoded on the host.
 *
 * QEMU has no game controller, so the only reports `pad_decode_x360` ever
 * gets in a machine come from a real pad - the SN30 Pro USB, passed through
 * to QEMU on the Mac, or the ThinkPad's port. Every button of both bytes is
 * here by itself, the sticks both ways, the triggers, the stick's threshold
 * and the hysteresis that keeps a resting stick from chattering, and the
 * reports that are not input.
 */

#include <stdio.h>
#include <string.h>

#include "../user/servers/pad_decode.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

static void report(uint8_t *r, uint8_t b2, uint8_t b3, uint8_t lt, uint8_t rt,
                   int lx, int ly)
{
    memset(r, 0, 20);
    r[0] = 0x00;
    r[1] = 0x14;
    r[2] = b2;
    r[3] = b3;
    r[4] = lt;
    r[5] = rt;
    r[6] = (uint8_t)(lx & 0xff);
    r[7] = (uint8_t)((lx >> 8) & 0xff);
    r[8] = (uint8_t)(ly & 0xff);
    r[9] = (uint8_t)((ly >> 8) & 0xff);
}

static uint32_t only(uint8_t b2, uint8_t b3)
{
    uint8_t r[20];
    struct pad_state s;

    report(r, b2, b3, 0, 0, 0, 0);
    return pad_decode_x360(r, 20, &s) ? s.buttons : 0xFFFFFFFFu;
}

int main(void)
{
    uint8_t r[20];
    struct pad_state s;
    uint32_t on;
    static const struct { uint8_t b2, b3; int bit; const char *what; } each[] = {
        { 0x01, 0, PAD_UP,     "D-pad up, byte 2 bit 0" },
        { 0x02, 0, PAD_DOWN,   "D-pad down, bit 1" },
        { 0x04, 0, PAD_LEFT,   "D-pad left, bit 2" },
        { 0x08, 0, PAD_RIGHT,  "D-pad right, bit 3" },
        { 0x10, 0, PAD_START,  "Start, bit 4" },
        { 0x20, 0, PAD_SELECT, "Back, bit 5" },
        { 0x40, 0, PAD_THUMBL, "the left stick's click, bit 6" },
        { 0x80, 0, PAD_THUMBR, "the right stick's click, bit 7" },
        { 0, 0x01, PAD_TL,     "LB, byte 3 bit 0" },
        { 0, 0x02, PAD_TR,     "RB, bit 1" },
        { 0, 0x04, PAD_MODE,   "the Xbox button, bit 2" },
        { 0, 0x10, PAD_SOUTH,  "A, bit 4, the bottom one" },
        { 0, 0x20, PAD_EAST,   "B, bit 5, the right one" },
        { 0, 0x40, PAD_WEST,   "X, bit 6, the left one" },
        { 0, 0x80, PAD_NORTH,  "Y, bit 7, the top one" },
    };
    unsigned i;
    char line[96];

    for (i = 0; i < sizeof(each) / sizeof(each[0]); i++) {
        snprintf(line, sizeof(line), "%s was not read as itself alone",
                 each[i].what);
        check(only(each[i].b2, each[i].b3) == (1u << each[i].bit), line);
    }

    check(only(0, 0x08) == 0, "byte 3's unused bit 3 was read as a button");

    check(pad_codes[PAD_SOUTH] == 0x130 && pad_codes[PAD_EAST] == 0x131
          && pad_codes[PAD_NORTH] == 0x133 && pad_codes[PAD_WEST] == 0x134
          && pad_codes[PAD_START] == 0x13b && pad_codes[PAD_UP] == 0x220
          && pad_codes[PAD_RIGHT] == 0x223,
          "the evdev codes are not input-event-codes.h's");

    /* The sticks and triggers, little-endian and signed. */
    report(r, 0, 0, 200, 7, -32768, 32767);
    r[10] = 0x34; r[11] = 0x12;          /* right X 0x1234 */
    r[12] = 0xFE; r[13] = 0xFF;          /* right Y -2 */
    check(pad_decode_x360(r, 20, &s) && s.lx == -32768 && s.ly == 32767
          && s.rx == 0x1234 && s.ry == -2 && s.lt == 200 && s.rt == 7,
          "the sticks or the triggers were not read signed, little-endian, "
          "from bytes 4 to 13");

    /* The left stick is the D-pad past half-way; the triggers past half. */
    on = pad_pressed(&s, 0);
    check(on == ((1u << PAD_UP) | (1u << PAD_LEFT) | (1u << PAD_TL2)),
          "a stick up and to the left with the left trigger in did not "
          "press up, left and the left trigger - and only those");

    report(r, 0, 0, 0, 0, 16000, -16000);
    pad_decode_x360(r, 20, &s);
    check(pad_pressed(&s, 0) == 0,
          "a stick just short of half-way pressed the D-pad");
    check(pad_pressed(&s, (1u << PAD_RIGHT) | (1u << PAD_DOWN))
          == ((1u << PAD_RIGHT) | (1u << PAD_DOWN)),
          "a stick held right and down, eased back to 16000, let go - it "
          "should hold until inside the release");

    report(r, 0, 0, 0, 0, 12000, -12000);
    pad_decode_x360(r, 20, &s);
    check(pad_pressed(&s, (1u << PAD_RIGHT) | (1u << PAD_DOWN)) == 0,
          "a stick back inside the release still held the D-pad");

    /* The D-pad and the stick together: one direction, not two. */
    report(r, 0x01, 0, 0, 0, 0, 30000);
    pad_decode_x360(r, 20, &s);
    check(pad_pressed(&s, 0) == (1u << PAD_UP),
          "the D-pad and the stick both up were not one up");

    /* Not input reports. */
    memset(&s, 0x5A, sizeof(s));
    report(r, 0xFF, 0xFF, 0, 0, 0, 0);
    r[0] = 0x01;                          /* the LED's status message */
    check(!pad_decode_x360(r, 20, &s) && s.buttons == 0x5A5A5A5Au,
          "a report of type 01h was read as input");
    report(r, 0xFF, 0xFF, 0, 0, 0, 0);
    r[1] = 0x03;
    check(!pad_decode_x360(r, 20, &s), "a length other than 14h was read");
    report(r, 0xFF, 0xFF, 0, 0, 0, 0);
    check(!pad_decode_x360(r, 19, &s), "a report of 19 bytes was read");
    check(!pad_decode_x360(0, 20, &s), "no report at all was read");

    if (fails) {
        printf("FAIL: %d of %d checks on an Xbox 360 controller's reports\n",
               fails, fails + checks);
        return 1;
    }

    printf("PASS: %d checks on an Xbox 360 controller's reports (every "
           "button by itself, the sticks and triggers signed and "
           "little-endian, the stick as the D-pad with its hysteresis, the "
           "triggers as buttons, and reports that are not input "
           "refused).\n", checks);
    return 0;
}
