/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A USB Video Class camera's bytes, decoded on the host.
 *
 * QEMU has no camera, so the descriptors `uvc_decode_config` ever reads in a
 * machine come from a real one - Diego's C920 through QEMU on the Mac, or the
 * ThinkPad's own. So the camera's bytes are here (`tools/uvc_c920.h`), and
 * what a real camera never sends as well:
 *
 *   - the C920: its interfaces, UVC 1.0, 35 sizes in YUY2 and MJPEG, and
 *     eleven isochronous settings from 192 to 3,072 bytes a microframe -
 *     the same eleven libusb listed on the Mac;
 *   - a size chosen, a setting chosen, and the probe block both ways;
 *   - the configuration cut short at every one of its 2,427 lengths, each
 *     copy ending on the last byte before a page that is not mapped, so a
 *     read one byte past the end is a crash rather than a pass;
 *   - a camera that streams on bulk, and a device that is not a camera;
 *   - payloads put together into frames: joined mid-frame, whole at EOF,
 *     whole when FID changes without one, a frame short a payload, one with
 *     the error bit, one too large, and a header longer than its payload.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../user/drivers/usb/uvc_decode.h"
#include "uvc_c920.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  not ok: %s\n", what);
    }
}

/* A payload: a two-byte header with these flags, then `n` bytes of `fill`. */
static unsigned payload(uint8_t *p, uint8_t flags, unsigned n, uint8_t fill)
{
    p[0] = 2;
    p[1] = flags;
    memset(p + 2, fill, n);
    return 2 + n;
}

#define FID 0x01
#define EOF_ 0x02
#define ERR 0x40

static void test_c920(void)
{
    static const uint32_t bytes[11] = {
        192, 384, 512, 640, 800, 944, 1280, 1600, 1984, 2688, 3072,
    };
    struct uvc_camera c;
    unsigned i;
    int f;

    uvc_decode_config(C920_CONFIG, sizeof C920_CONFIG, &c);

    check(c.ok, "the C920 is a camera");
    check(!c.malformed, "the C920's configuration is well formed");
    check(c.configuration == 1, "the C920's configuration is 1");
    check(c.control == 0 && c.streaming == 1,
          "the C920 controls on interface 0 and streams on 1");
    check(c.uvc == 0x0100, "the C920 is UVC 1.0");
    check(!c.bulk, "the C920 streams isochronously");
    check(c.nframes == 35 && c.frames_dropped == 0,
          "the C920 offers 35 sizes, all kept");
    check(c.nalts == 11 && c.alts_dropped == 0,
          "the C920 has eleven streaming settings");

    for (i = 0; i < 11 && i < c.nalts; i++) {
        char what[96];

        snprintf(what, sizeof what, "setting %u carries %u bytes on endpoint 1",
                 i + 1, (unsigned)bytes[i]);
        check(c.alts[i].alternate == i + 1 && c.alts[i].endpoint == 1
              && c.alts[i].bytes == bytes[i] && c.alts[i].interval == 1, what);
    }

    check(c.alts[9].packet == 896 && c.alts[9].extra == 2,
          "setting 10 is three transactions of 896 a microframe");

    /* Its first size, and the two ends of what it offers. */
    check(c.frames[0].format == 1 && c.frames[0].frame == 1
          && c.frames[0].pixels == UVC_PIXELS_YUY2
          && c.frames[0].width == 640 && c.frames[0].height == 480
          && c.frames[0].max_bytes == 614400
          && c.frames[0].interval == 333333,
          "YUY2 640x480 is format 1 frame 1, 614400 bytes, 30 a second");

    f = uvc_find_frame(&c, UVC_PIXELS_YUY2, 1920, 1080);
    check(f >= 0 && c.frames[f].frame == 17 && c.frames[f].interval == 2000000,
          "YUY2 1920x1080 is frame 17, 5 a second");

    f = uvc_find_frame(&c, UVC_PIXELS_MJPEG, 1920, 1080);
    check(f >= 0 && c.frames[f].format == 2 && c.frames[f].frame == 17
          && c.frames[f].interval == 333333,
          "MJPEG 1920x1080 is format 2 frame 17, 30 a second");

    /* Choosing. */
    f = uvc_find_frame(&c, 0, 640, 480);
    check(f == 0, "either format at 640x480 is YUY2's");

    f = uvc_find_frame(&c, UVC_PIXELS_MJPEG, 640, 480);
    check(f >= 0 && c.frames[f].pixels == UVC_PIXELS_MJPEG
          && c.frames[f].width == 640, "MJPEG at 640x480 when asked for");

    f = uvc_find_frame(&c, UVC_PIXELS_YUY2, 1000, 700);
    check(f >= 0 && c.frames[f].width == 800 && c.frames[f].height == 600,
          "1000x700 is the largest inside it, 800x600");

    check(uvc_find_frame(&c, 0, 100, 50) == -1, "nothing fits in 100x50");

    check(uvc_pick_alternate(&c, 3072) == 10, "3072 bytes is setting 11");
    check(uvc_pick_alternate(&c, 1) == 0, "one byte is setting 1");
    check(uvc_pick_alternate(&c, 945) == 6,
          "945 bytes is setting 7, the least of those that carry it");
    check(uvc_pick_alternate(&c, 3073) == -1, "no setting carries 3073");

    check(C920_DEVICE[0] == 18 && C920_DEVICE[8] == 0x6d && C920_DEVICE[9] == 0x04
          && C920_DEVICE[10] == 0xe5 && C920_DEVICE[11] == 0x08,
          "the fixture is 046d:08e5");
}

static void test_probe(void)
{
    struct uvc_probe p = { 0 }, q = { 0 };
    uint8_t b[UVC_PROBE_MAX];

    check(uvc_probe_length(0x0100) == 26 && uvc_probe_length(0x0110) == 34
          && uvc_probe_length(0x0150) == 48,
          "the probe is 26, 34 or 48 bytes by version");

    p.hint = 1;
    p.format = 1;
    p.frame = 1;
    p.interval = 333333;
    p.max_frame = 614400;
    p.max_payload = 3072;
    p.clock = 30000000;

    memset(b, 0xee, sizeof b);
    uvc_encode_probe(&p, b, 26);
    check(b[0] == 1 && b[1] == 0 && b[2] == 1 && b[3] == 1
          && b[4] == 0x15 && b[5] == 0x16 && b[6] == 0x05 && b[7] == 0x00,
          "bmHint, the format, the frame and 333333 at their offsets");
    check(b[8] == 0 && b[17] == 0, "what the struct does not say is zero");
    check(b[26] == 0xee, "a 26-byte probe writes 26 bytes");

    check(uvc_decode_probe(b, 26, &q) && q.format == 1 && q.frame == 1
          && q.interval == 333333 && q.max_frame == 614400
          && q.max_payload == 3072 && q.clock == 0,
          "a 26-byte probe decodes, with no clock");

    uvc_encode_probe(&p, b, 34);
    check(uvc_decode_probe(b, 34, &q) && q.clock == 30000000,
          "a 34-byte probe carries the clock");

    memset(&q, 0x5a, sizeof q);
    check(!uvc_decode_probe(b, 25, &q) && q.format == 0x5a,
          "25 bytes is refused, and nothing written");
}

/*
 * **Bytes that end on the edge of a page nobody may read.** The address
 * sanitizer was the first choice for this and does not start on this Mac -
 * it hangs in its own initialiser before `main` - so the test makes the
 * check itself: the last byte handed over is the last byte before a page
 * mapped with no access, and a read one past it is a fault.
 */
static uint8_t *guarded;
static size_t guarded_room;

static uint8_t *on_the_edge(const uint8_t *bytes, unsigned n)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);

    if (guarded == NULL) {
        guarded_room = ((sizeof C920_CONFIG + page - 1) / page) * page;
        guarded = mmap(NULL, guarded_room + page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);

        if (guarded == MAP_FAILED
            || mprotect(guarded + guarded_room, page, PROT_NONE) != 0) {
            printf("not ok - no guard page to test against\n");
            exit(1);
        }
    }

    memcpy(guarded + guarded_room - n, bytes, n);
    return guarded + guarded_room - n;
}

static void test_cut_short(void)
{
    unsigned n, boundaries = 0;
    struct uvc_camera c;

    /* Every length from nothing to the whole. At a descriptor's end the
     * walk is clean; anywhere else the last one runs past and is refused. */
    for (n = 0; n <= sizeof C920_CONFIG; n++) {
        unsigned at = 0;
        int boundary = 0;
        uint8_t *copy = on_the_edge(C920_CONFIG, n);

        while (at < n) {
            at += C920_CONFIG[at];
        }

        boundary = (at == n);
        boundaries += boundary;
        uvc_decode_config(copy, n, &c);

        if (boundary == c.malformed) {
            char what[80];

            snprintf(what, sizeof what, "cut at %u: malformed is %d", n,
                     c.malformed);
            check(0, what);
        }
    }

    check(1, "every length of the C920's configuration, read in bounds");
    check(boundaries > 50, "and many of them at a descriptor's end");

    {
        uint8_t zero[9 + 2] = { 9, 2, 11, 0, 1, 1, 0, 0x80, 50, 0, 4 };

        uvc_decode_config(zero, sizeof zero, &c);
        check(c.malformed && !c.ok, "a descriptor of length zero stops it");
    }
}

static void test_bulk_and_not_a_camera(void)
{
    /* The least a bulk camera is: a VideoControl interface with its header,
     * and a streaming interface whose setting 0 has a bulk IN endpoint, one
     * MJPEG format and one 320x240 frame. */
    static const uint8_t bulk[] = {
        9, 0x02, 0, 0, 2, 1, 0, 0x80, 50,
        9, 0x04, 0, 0, 0, 0x0e, 0x01, 0, 0,
        13, 0x24, 0x01, 0x10, 0x01, 0, 0, 0, 0, 0, 0, 0, 0,
        9, 0x04, 1, 0, 1, 0x0e, 0x02, 0, 0,
        11, 0x24, 0x06, 1, 1, 0, 1, 0, 0, 0, 0,
        30, 0x24, 0x07, 1, 0, 0x40, 0x01, 0xf0, 0x00,
            0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x58, 0x02, 0x00,
            0x15, 0x16, 0x05, 0x00, 1, 0x15, 0x16, 0x05, 0x00,
        7, 0x05, 0x82, 0x02, 0x00, 0x02, 0,
    };
    static const uint8_t keyboard[] = {
        9, 0x02, 34, 0, 1, 1, 0, 0xa0, 50,
        9, 0x04, 0, 0, 1, 0x03, 0x01, 0x01, 0,
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, 63, 0,
        7, 0x05, 0x81, 0x03, 8, 0, 10,
    };
    struct uvc_camera c;

    uvc_decode_config(bulk, sizeof bulk, &c);
    check(c.ok && !c.malformed, "a bulk camera is a camera");
    check(c.uvc == 0x0110, "it is UVC 1.1");
    check(c.bulk && c.bulk_endpoint == 2 && c.bulk_packet == 512,
          "it streams on bulk IN 2, 512 bytes a packet");
    check(c.nalts == 0 && uvc_pick_alternate(&c, 1) == -1,
          "and has no isochronous setting to pick");
    check(c.nframes == 1 && c.frames[0].pixels == UVC_PIXELS_MJPEG
          && c.frames[0].width == 320 && c.frames[0].height == 240,
          "its one size is MJPEG 320x240");

    uvc_decode_config(keyboard, sizeof keyboard, &c);
    check(!c.ok && !c.malformed, "a keyboard is not a camera");
}

static void test_assembly(void)
{
    uint8_t frame[16], p[64], expect[16];
    struct uvc_assembly a;
    unsigned n;

    uvc_assembly_init(&a, frame, sizeof frame, 16);

    /* Joined in the middle of a frame: nothing kept until its EOF. */
    n = payload(p, 0, 6, 0x11);
    check(uvc_payload(&a, p, n) == UVC_SKIPPED, "mid-frame is skipped");
    n = payload(p, EOF_, 4, 0x11);
    check(uvc_payload(&a, p, n) == UVC_SKIPPED && a.length == 0,
          "and so is the end of that frame");

    /* A whole frame, ended by EOF. */
    n = payload(p, FID, 8, 0x22);
    check(uvc_payload(&a, p, n) == UVC_TAKEN && a.length == 8,
          "the next frame's first half is taken");
    n = payload(p, FID | EOF_, 8, 0x33);
    check(uvc_payload(&a, p, n) == UVC_WHOLE && a.length == 16,
          "and with its second half and EOF it is whole");
    memset(expect, 0x22, 8);
    memset(expect + 8, 0x33, 8);
    check(memcmp(frame, expect, 16) == 0, "the frame is the two halves, in order");
    uvc_next(&a);

    /* A frame a byte short: dropped, not shown. */
    n = payload(p, 0, 8, 0x44);
    uvc_payload(&a, p, n);
    n = payload(p, EOF_, 7, 0x44);
    check(uvc_payload(&a, p, n) == UVC_TAKEN && a.dropped == 1
          && a.length == 0, "a frame of 15 bytes, not 16, is dropped");

    /* A frame whose EOF was lost: whole when FID changes. */
    n = payload(p, FID, 16, 0x55);
    check(uvc_payload(&a, p, n) == UVC_TAKEN && a.length == 16,
          "16 bytes with no EOF");
    n = payload(p, 0, 8, 0x66);
    check(uvc_payload(&a, p, n) == UVC_BEFORE && a.length == 16,
          "FID changing says the frame so far is whole");
    uvc_next(&a);
    check(uvc_payload(&a, p, n) == UVC_TAKEN && a.length == 8
          && frame[0] == 0x66, "and the same payload then begins the next");

    /* The error bit. */
    n = payload(p, ERR, 8, 0x77);
    uvc_payload(&a, p, n);
    n = payload(p, EOF_, 0, 0);
    check(uvc_payload(&a, p, n) == UVC_TAKEN && a.dropped == 2,
          "a frame with the error bit is dropped");

    /* Too much. */
    n = payload(p, FID, 20, 0x88);
    uvc_payload(&a, p, n);
    n = payload(p, FID | EOF_, 0, 0);
    check(uvc_payload(&a, p, n) == UVC_TAKEN && a.dropped == 3,
          "20 bytes in a 16-byte frame is dropped");

    /* A header longer than its payload, and payloads with nothing in them. */
    n = payload(p, 0, 8, 0x99);
    uvc_payload(&a, p, n);
    p[0] = 40;
    check(uvc_payload(&a, p, 10) == UVC_SKIPPED && a.broken,
          "a header longer than the payload breaks the frame");
    check(uvc_payload(&a, p, 1) == UVC_SKIPPED
          && uvc_payload(&a, NULL, 0) == UVC_SKIPPED,
          "a payload of one byte, or none, is nothing");

    check(a.whole == 2, "two frames came out whole");

    /* MJPEG: no fixed size, so any whole frame is kept. */
    uvc_assembly_init(&a, frame, sizeof frame, 0);
    n = payload(p, EOF_, 0, 0);
    uvc_payload(&a, p, n);
    n = payload(p, FID | EOF_, 5, 0xaa);
    check(uvc_payload(&a, p, n) == UVC_WHOLE && a.length == 5,
          "an MJPEG frame of five bytes is whole");
}

int main(void)
{
    test_c920();
    test_probe();
    test_cut_short();
    test_bulk_and_not_a_camera();
    test_assembly();

    if (fails) {
        printf("FAIL: %d of %d checks on USB Video Class decoding\n",
               fails, checks + fails);
        return 1;
    }

    printf("PASS: %d checks on USB Video Class decoding (Diego's C920 from "
           "its own bytes, every length of them cut short, a bulk camera, "
           "probe and commit, and frames put together from payloads)\n",
           checks);
    return 0;
}
