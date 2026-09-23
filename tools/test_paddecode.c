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

#include "../user/drivers/usb/pad_decode.h"

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

    /*
     * **An Xbox One or Series pad**, GIP (`pad_decode.h`): each button of
     * both bytes, the triggers' ten bits, the sticks, the Xbox button kept
     * across input reports, what asks for an acknowledgement, and what the
     * host says first.
     */
    {
        static const struct { uint8_t b4, b5; int bit; const char *what; } one[] = {
            { 0x04, 0, PAD_START,  "Menu, byte 4 bit 2" },
            { 0x08, 0, PAD_SELECT, "View, byte 4 bit 3" },
            { 0x10, 0, PAD_SOUTH,  "A, byte 4 bit 4" },
            { 0x20, 0, PAD_EAST,   "B, byte 4 bit 5" },
            { 0x40, 0, PAD_WEST,   "X, byte 4 bit 6" },
            { 0x80, 0, PAD_NORTH,  "Y, byte 4 bit 7" },
            { 0, 0x01, PAD_UP,     "D-pad up, byte 5 bit 0" },
            { 0, 0x02, PAD_DOWN,   "D-pad down, byte 5 bit 1" },
            { 0, 0x04, PAD_LEFT,   "D-pad left, byte 5 bit 2" },
            { 0, 0x08, PAD_RIGHT,  "D-pad right, byte 5 bit 3" },
            { 0, 0x10, PAD_TL,     "LB, byte 5 bit 4" },
            { 0, 0x20, PAD_TR,     "RB, byte 5 bit 5" },
            { 0, 0x40, PAD_THUMBL, "the left stick's click, byte 5 bit 6" },
            { 0, 0x80, PAD_THUMBR, "the right stick's click, byte 5 bit 7" },
        };
        uint8_t m[18], out[16], seq = 0;
        bool ack = true;
        unsigned n, k, lengths[PAD_XONE_STEPS];

        for (i = 0; i < sizeof(one) / sizeof(one[0]); i++) {
            memset(m, 0, sizeof(m));
            m[0] = 0x20;
            m[3] = 0x0e;
            m[4] = one[i].b4;
            m[5] = one[i].b5;
            memset(&s, 0, sizeof(s));
            snprintf(line, sizeof(line), "%s was not read as itself alone",
                     one[i].what);
            check(pad_decode_xone(m, 18, &s, &ack, &seq) == PAD_XONE_INPUT
                  && s.buttons == (1u << one[i].bit) && !ack, line);
        }

        memset(m, 0, sizeof(m));
        m[0] = 0x20;
        m[4] = 0x03;                        /* the sync bit and its neighbour */
        memset(&s, 0, sizeof(s));
        check(pad_decode_xone(m, 18, &s, &ack, &seq) == PAD_XONE_INPUT
              && s.buttons == 0,
              "byte 4's bits 0 and 1 were read as buttons");

        /* Triggers of ten bits, sticks signed; the left stick up and the
         * right trigger all the way in. */
        m[6] = 0x00; m[7] = 0x02;            /* left trigger 512 */
        m[8] = 0xff; m[9] = 0x03;            /* right trigger 1023 */
        m[10] = 0x00; m[11] = 0x80;          /* left X -32768 */
        m[12] = 0xff; m[13] = 0x7f;          /* left Y 32767, up */
        check(pad_decode_xone(m, 18, &s, &ack, &seq) == PAD_XONE_INPUT
              && s.lt == 128 && s.rt == 255 && s.lx == -32768 && s.ly == 32767
              && pad_pressed(&s, 0) == ((1u << PAD_UP) | (1u << PAD_LEFT)
                                        | (1u << PAD_TL2) | (1u << PAD_TR2)),
              "an Xbox One's triggers and left stick were not read as the "
              "360's are - ten bits halved twice, up positive");

        /* The Xbox button: its own message, kept by the input after it, and
         * an acknowledgement asked for with options 30h. */
        memset(&s, 0, sizeof(s));
        {
            uint8_t guide[6] = { 0x07, 0x30, 0x2a, 0x02, 0x01, 0x5b };

            check(pad_decode_xone(guide, 6, &s, &ack, &seq) == PAD_XONE_GUIDE
                  && (s.buttons & (1u << PAD_MODE)) && ack && seq == 0x2a,
                  "the Xbox button's message was not the button down with an "
                  "acknowledgement asked for, sequence 2Ah");

            memset(m, 0, sizeof(m));
            m[0] = 0x20;
            m[4] = 0x10;                     /* A, and the Xbox button still */
            check(pad_decode_xone(m, 18, &s, &ack, &seq) == PAD_XONE_INPUT
                  && s.buttons == ((1u << PAD_MODE) | (1u << PAD_SOUTH)),
                  "an input report let go of the Xbox button, which only its "
                  "own message changes");

            guide[1] = 0x20;
            guide[4] = 0x00;
            check(pad_decode_xone(guide, 6, &s, &ack, &seq) == PAD_XONE_GUIDE
                  && !(s.buttons & (1u << PAD_MODE)) && !ack,
                  "the Xbox button let go, with no acknowledgement asked for, "
                  "was not read so");
        }

        {
            uint8_t announce[8] = { 0x02, 0x20, 0x01, 0x1c };

            check(pad_decode_xone(announce, 8, &s, &ack, &seq)
                  == PAD_XONE_ANNOUNCE, "the pad's announcement was missed");
            check(pad_decode_xone(m, 17, &s, &ack, &seq) == PAD_XONE_OTHER,
                  "an input report of 17 bytes was read");
        }

        /* What the host says first: every pad, and the One S and Elite 2. */
        for (k = 0; k < PAD_XONE_STEPS; k++) {
            lengths[k] = pad_xone_init(0x045e, 0x0b12, k, (uint8_t)k, out);
        }
        check(lengths[0] == 5 && lengths[1] == 0 && lengths[2] == 0
              && lengths[3] == 0 && lengths[4] == 7 && lengths[5] == 6,
              "a Series pad (045e:0b12) was not sent power on, the light and "
              "authenticated - and only those");

        n = pad_xone_init(0x045e, 0x0b12, 0, 7, out);
        check(n == 5 && out[0] == 0x05 && out[1] == 0x20 && out[2] == 7
              && out[3] == 0x01 && out[4] == 0x00,
              "power on was not 05 20 <seq> 01 00");

        n = pad_xone_init(0x045e, 0x02ea, 1, 1, out);
        check(n == 5 && out[3] == 0x0f && out[4] == 0x06
              && pad_xone_init(0x045e, 0x0b00, 3, 3, out) == 6 && out[0] == 0x4d
              && out[2] == 3,
              "the One S's and the Elite 2's own start-up messages were not "
              "theirs");
        check(pad_xone_init(0x045e, 0x0b12, PAD_XONE_STEPS, 0, out) == 0,
              "a step past the last gave a message");

        n = pad_xone_ack(0x2a, out);
        check(n == 13 && out[0] == 0x01 && out[1] == 0x20 && out[2] == 0x2a
              && out[3] == 0x09 && out[5] == 0x07 && out[6] == 0x20
              && out[7] == 0x02 && out[12] == 0x00,
              "the acknowledgement was not xpad's thirteen bytes with the "
              "pad's sequence");
    }

    if (fails) {
        printf("FAIL: %d of %d checks on Xbox 360 and Xbox One controllers' "
               "reports\n",
               fails, fails + checks);
        return 1;
    }

    printf("PASS: %d checks on Xbox 360 and Xbox One controllers' reports (every "
           "button by itself, the sticks and triggers signed and "
           "little-endian, the stick as the D-pad with its hysteresis, the "
           "triggers as buttons, and reports that are not input "
           "refused; and an Xbox One's messages - its buttons, its Xbox "
           "button kept across input, what asks for an acknowledgement, the "
           "start-up for each pad and the acknowledgement's bytes).\n",
           checks);
    return 0;
}
