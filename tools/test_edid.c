/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The EDID kit held to bytes, on the Mac (`docs/displaymodes.md` D2).
 *
 * First QEMU's own EDID, as QEMU 11.1.1's generator makes it - `qemu-edid`
 * with -x 1920 -y 1080, -x 3440 -y 1440 -X 3440 -Y 1440, and -x 5120
 * -y 2160 -X 5120 -Y 2160, whose 5120 is wider than a detailed timing can
 * say and so goes in a DisplayID extension. Every expectation for those
 * was read off the bytes by hand, not off the decoder. Then blocks built
 * here for each path QEMU does not take: a 1.4 monitor with a CTA-861
 * extension and a DisplayID 2.0 one, a 1.3 analogue monitor whose standard
 * timings fall to GTF, and the ways an EDID is wrong - each decoded, or
 * refused by name.
 *
 * Its control: `EDIDTEST_BROKEN=1` moves one byte in each fixture, its
 * block's sum kept right, and the run must fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/kits/display/edid.h"

static int checks, failed, broken;
static struct edid_info info;           /* 2 KB: not on the stack */

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        failed++;
        printf("FAIL: %s\n", what);
    }
}

/* The last byte of a block made to bring its sum to zero. */
static void sum_block(uint8_t *block)
{
    unsigned sum = 0, i;

    for (i = 0; i < 127; i++) {
        sum += block[i];
    }

    block[127] = (uint8_t)(0x100u - (sum & 0xFFu));
}

/* The control: one byte moved, the block's sum kept. */
static void maybe_break(uint8_t *edid, size_t at)
{
    if (broken) {
        edid[at]++;
        sum_block(&edid[at / 128u * 128u]);
    }
}

/* The mode of that size at that refresh, to the millihertz, or NULL. */
static const struct edid_mode *find(unsigned w, unsigned h, unsigned mhz)
{
    unsigned i;

    for (i = 0; i < info.nmodes; i++) {
        if (info.modes[i].width == w && info.modes[i].height == h &&
            info.modes[i].refresh_mhz == mhz) {
            return &info.modes[i];
        }
    }

    return NULL;
}

static int is(const struct edid_mode *m, unsigned clock, unsigned ht, unsigned vt,
              unsigned source, unsigned flags)
{
    return m != NULL && m->clock_khz == clock && m->htotal == ht && m->vtotal == vt &&
           m->source == source && m->flags == flags;
}

static void dump(void)
{
    unsigned i;

    for (i = 0; i < info.nmodes; i++) {
        const struct edid_mode *m = &info.modes[i];

        printf("      %2u %5u x %-5u %7u mHz %8u kHz  %u/%u  %s flags %#x vic %u\n", i,
               m->width, m->height, m->refresh_mhz, m->clock_khz, m->htotal, m->vtotal,
               edid_source_name(m->source), m->flags, m->vic);
    }
}

static const uint8_t qemu_1920[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x49, 0x14, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00,
    0x2a, 0x18, 0x01, 0x04, 0xa5, 0x30, 0x1b, 0x78, 0x06, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x21, 0x08, 0x00, 0xe1, 0xc0, 0xd1, 0xc0, 0xd1, 0x00, 0xa9, 0x40, 0xb3, 0x00,
    0x95, 0x00, 0x81, 0x80, 0x81, 0x40, 0xd2, 0x54, 0x80, 0xa0, 0x72, 0x38, 0x25, 0x40, 0xe0, 0x39,
    0x55, 0x40, 0xe7, 0x12, 0x11, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0xf7, 0x00, 0x0a, 0x00, 0x40,
    0x82, 0x00, 0x28, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32,
    0x7d, 0x1e, 0xa0, 0xff, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc,
    0x00, 0x51, 0x45, 0x4d, 0x55, 0x20, 0x4d, 0x6f, 0x6e, 0x69, 0x74, 0x6f, 0x72, 0x0a, 0x01, 0xb0,
    0x02, 0x03, 0x0b, 0x00, 0x46, 0x7d, 0x65, 0x60, 0x59, 0x1f, 0x61, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2f,
};

static const uint8_t qemu_3440[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x49, 0x14, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00,
    0x2a, 0x18, 0x01, 0x04, 0xa5, 0x57, 0x24, 0x78, 0x06, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x21, 0x08, 0x00, 0xe1, 0xc0, 0xd1, 0xc0, 0xd1, 0x00, 0xa9, 0x40, 0xb3, 0x00,
    0x95, 0x00, 0x81, 0x80, 0x81, 0x40, 0xb8, 0xca, 0x70, 0xb4, 0xd4, 0xa0, 0x32, 0x50, 0x5c, 0x67,
    0x77, 0xc0, 0x69, 0x6d, 0x31, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0xf7, 0x00, 0x0a, 0x00, 0x40,
    0x82, 0x00, 0x28, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32,
    0x7d, 0x1e, 0xa0, 0xff, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc,
    0x00, 0x51, 0x45, 0x4d, 0x55, 0x20, 0x4d, 0x6f, 0x6e, 0x69, 0x74, 0x6f, 0x72, 0x0a, 0x01, 0xf0,
    0x02, 0x03, 0x07, 0x00, 0x42, 0x59, 0x1f, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xda,
};

static const uint8_t qemu_5120[384] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x49, 0x14, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00,
    0x2a, 0x18, 0x01, 0x04, 0xa5, 0x82, 0x36, 0x78, 0x06, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x21, 0x08, 0x00, 0xe1, 0xc0, 0xd1, 0xc0, 0xd1, 0x00, 0xa9, 0x40, 0xb3, 0x00,
    0x95, 0x00, 0x81, 0x80, 0x81, 0x40, 0x00, 0x00, 0x00, 0xf7, 0x00, 0x0a, 0x00, 0x40, 0x82, 0x00,
    0x28, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0x7d, 0x1e,
    0xa0, 0xff, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x51,
    0x45, 0x4d, 0x55, 0x20, 0x4d, 0x6f, 0x6e, 0x69, 0x74, 0x6f, 0x72, 0x0a, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x57,
    0x02, 0x03, 0x0b, 0x00, 0x46, 0x7d, 0x65, 0x60, 0x59, 0x1f, 0x61, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2f,
    0x70, 0x13, 0x17, 0x03, 0x00, 0x03, 0x00, 0x14, 0x96, 0xc4, 0x01, 0x88, 0xff, 0x13, 0xff, 0x06,
    0xff, 0x04, 0x98, 0x00, 0x6f, 0x08, 0x4a, 0x00, 0x09, 0x00, 0x09, 0x00, 0x54, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
};

/* What all three QEMU EDIDs share: the base block's identity, its range
 * descriptor, its eight standard timings and its established ones. */
static void qemu_common(const char *which)
{
    char what[96];

#define CHECK(ok, text) do { snprintf(what, sizeof what, "%s: %s", which, text); check(ok, what); } while (0)
    CHECK(strcmp(info.maker, "RHT") == 0, "maker RHT from 49 14");
    CHECK(info.product == 0x1234 && info.serial == 0, "product 1234, no serial");
    CHECK(strcmp(info.name, "QEMU Monitor") == 0 && info.serial_text[0] == 0, "named QEMU Monitor");
    CHECK(info.week == 42 && info.year == 2014, "week 42 of 2014");
    CHECK(info.version == 1 && info.revision == 4, "EDID 1.4");
    CHECK(info.digital && info.bpc == 8 && info.interface == 5, "digital, 8 bits, DisplayPort (A5)");
    CHECK(info.range.present && info.range.kind == 1 && info.range.min_vhz == 50 &&
          info.range.max_vhz == 125 && info.range.min_hkhz == 30 && info.range.max_hkhz == 160 &&
          info.range.max_clock_khz == 2550000 && !info.range.reduced_blanking,
          "range: limits only, 50-125 Hz, 30-160 kHz, 2550 MHz");
    CHECK(info.cta_blocks == 1, "one CTA block");

    /* Standard timings e1c0 d1c0 d100 a940 b300 9500 8180 8140: the
     * monitor does not say it takes reduced blanking (its range is not
     * CVT's), so DMT's ordinary modes - and 2048 x 1152's DMT entry, which
     * is not shaped as reduced blanking, is ordinary to DRM too. */
    CHECK(is(find(2048, 1152, 60000), 162000, 2250, 1200, EDID_FROM_STANDARD,
             EDID_MODE_HSYNC_POS | EDID_MODE_VSYNC_POS), "standard 2048 x 1152 at 60, DMT 0x54");
    CHECK(is(find(1920, 1080, 60000), 148500, 2200, 1125, EDID_FROM_STANDARD, 0),
          "standard 1920 x 1080 at 60, DMT 0x52");
    CHECK(is(find(1920, 1200, 59885), 193250, 2592, 1245, EDID_FROM_STANDARD, EDID_MODE_VSYNC_POS),
          "standard 1920 x 1200 at 60, DMT 0x45 and not the RB 0x44");
    CHECK(find(1600, 1200, 60000) && find(1680, 1050, 59954) && find(1440, 900, 59887) &&
          find(1280, 1024, 60020) && find(1280, 960, 60000), "the other five standard timings");

    /* Established III (f7, from byte 6: 00 40 82 00 28 20): 1280 x 768,
     * 1360 x 768, 1400 x 1050, 1792 x 1344, 1856 x 1392, 1920 x 1440. */
    CHECK(is(find(1280, 768, 59870), 79500, 1664, 798, EDID_FROM_ESTABLISHED_III, EDID_MODE_VSYNC_POS),
          "established III 1280 x 768 at 60");
    CHECK(find(1360, 768, 60015) && find(1400, 1050, 59978) && find(1792, 1344, 60000) &&
          find(1856, 1392, 59995) && find(1920, 1440, 60000), "established III's other five");

    /* Established timings 21 08 00: 800 x 600 at 60, 640 x 480 at 60,
     * 1024 x 768 at 60. */
    CHECK(is(find(800, 600, 60317), 40000, 1056, 628, EDID_FROM_ESTABLISHED,
             EDID_MODE_HSYNC_POS | EDID_MODE_VSYNC_POS) &&
          is(find(640, 480, 59940), 25175, 800, 525, EDID_FROM_ESTABLISHED, 0) &&
          is(find(1024, 768, 60004), 65000, 1344, 806, EDID_FROM_ESTABLISHED, 0),
          "established 800 x 600, 640 x 480, 1024 x 768");
#undef CHECK
}

static void test_qemu(void)
{
    static uint8_t b[384];
    const struct edid_mode *m;
    int s;

    /* 1920 x 1080: the detailed timing d2 54 80 a0 72 38 25 40 e0 39 55 40
     * e7 12 11 00 00 18 is 217.14 MHz, 1920 + 672, 1080 + 37, sync 480+57
     * and 5+5, 487 x 274 mm, both syncs negative: 75 Hz - QEMU's default. */
    memcpy(b, qemu_1920, sizeof qemu_1920);
    maybe_break(b, 0x38);
    s = edid_decode(b, sizeof qemu_1920, &info);
    check(s == EDID_OK, "QEMU 1920: decoded");
    qemu_common("QEMU 1920");
    m = &info.modes[0];
    check(info.nmodes == 24 && m->width == 1920 && m->height == 1080 && m->refresh_mhz == 74998 &&
          m->clock_khz == 217140 && m->hsync_start == 2400 && m->hsync_end == 2457 &&
          m->htotal == 2592 && m->vsync_start == 1085 && m->vsync_end == 1090 && m->vtotal == 1117 &&
          m->source == EDID_FROM_DETAILED && m->flags == EDID_MODE_PREFERRED,
          "QEMU 1920: the first of 24 is the preferred detailed timing, 1920 x 1080 at 74.998");
    check(info.width_mm == 487 && info.height_mm == 274, "QEMU 1920: 487 x 274 mm, from the timing");
    check(edid_diagonal_decinches(&info) == 220, "QEMU 1920: 22.0 inches");
    check(edid_preferred(&info) == m, "QEMU 1920: the preferred mode is it");

    /* Its CTA block, 02 03 0b 00, video data 46: 7d 65 60 59 1f 61 - codes
     * 125, 101, 96, 89, 31 and 97, none of them native. */
    check(is(find(5120, 2160, 50000), 742500, 6600, 2250, EDID_FROM_CTA_VIC,
             EDID_MODE_HSYNC_POS | EDID_MODE_VSYNC_POS) && find(5120, 2160, 50000)->vic == 125,
          "QEMU 1920: VIC 125, 5120 x 2160 at 50");
    check(find(4096, 2160, 50000) && find(4096, 2160, 50000)->vic == 101 &&
          find(3840, 2160, 50000) && find(3840, 2160, 50000)->vic == 96 &&
          find(2560, 1080, 50000) && find(2560, 1080, 50000)->vic == 89 &&
          find(1920, 1080, 50000) && find(1920, 1080, 50000)->vic == 31 &&
          find(3840, 2160, 60000) && find(3840, 2160, 60000)->vic == 97,
          "QEMU 1920: VICs 101, 96, 89, 31, 97");
    check(info.problems == 0, "QEMU 1920: nothing wrong");

    if (failed) {
        dump();
    }

    /* 3440 x 1440: b8 ca 70 b4 d4 a0 32 50 5c 67 77 c0 69 6d 31 00 00 18 is
     * 518.96 MHz, 3440 + 1204, 1440 + 50: 75 Hz again; 873 x 365 mm. */
    memcpy(b, qemu_3440, sizeof qemu_3440);
    maybe_break(b, 0x3a);
    s = edid_decode(b, sizeof qemu_3440, &info);
    check(s == EDID_OK, "QEMU 3440: decoded");
    qemu_common("QEMU 3440");
    m = &info.modes[0];
    check(info.nmodes == 20 && m->width == 3440 && m->height == 1440 && m->refresh_mhz == 74999 &&
          m->clock_khz == 518960 && m->htotal == 4644 && m->vtotal == 1490 &&
          m->flags == EDID_MODE_PREFERRED, "QEMU 3440: 3440 x 1440 at 74.999, preferred, of 20");
    check(info.width_mm == 873 && info.height_mm == 365 && edid_diagonal_decinches(&info) == 373,
          "QEMU 3440: 873 x 365 mm, 37.3 inches");
    check(find(2560, 1080, 50000) && find(1920, 1080, 50000) && !find(5120, 2160, 50000),
          "QEMU 3440: its CTA block names VICs 89 and 31 only");

    /* 5120 x 2160: no detailed timing in the base block - its first
     * descriptor is established III - and a DisplayID 1.3 extension, 70 13
     * 17 03 00, with one Type I block (03 00 14): 96 c4 01 is 115 862 + 1
     * tens of kHz, 1158.63 MHz; options 88, preferred; 5120 + 1792,
     * 2160 + 75, sync 1280+153 and 10+10. */
    memcpy(b, qemu_5120, sizeof qemu_5120);
    maybe_break(b, 0x10c);
    s = edid_decode(b, sizeof qemu_5120, &info);
    check(s == EDID_OK, "QEMU 5120: decoded");
    qemu_common("QEMU 5120");
    check(info.extensions == 2 && info.displayid_blocks == 1, "QEMU 5120: two extensions, one DisplayID");
    m = &info.modes[0];
    check(m->width == 5120 && m->height == 2160 && m->clock_khz == 1158630 && m->refresh_mhz == 75000 &&
          m->hsync_start == 6400 && m->hsync_end == 6553 && m->htotal == 6912 &&
          m->vsync_start == 2170 && m->vsync_end == 2180 && m->vtotal == 2235 &&
          m->source == EDID_FROM_DISPLAYID && m->flags == EDID_MODE_PREFERRED,
          "QEMU 5120: DisplayID's 5120 x 2160 at 75.000, first and preferred");
    check(info.width_mm == 1300 && info.height_mm == 540, "QEMU 5120: size from the block's 130 x 54 cm");
    check(info.nmodes == 24 && info.problems == 0, "QEMU 5120: 24 modes, nothing wrong");

    if (failed) {
        dump();
    }
}

/*
 * The builders: every field placed where `edid.h` says the decoder will
 * look for it. The detailed timing's packing is the one QEMU's fixtures
 * above hold the decoder to from the other side.
 */
static void put_dtd(uint8_t *d, unsigned clock_khz, unsigned ha, unsigned hss, unsigned hse,
                    unsigned ht, unsigned va, unsigned vss, unsigned vse, unsigned vt,
                    unsigned wmm, unsigned hmm, unsigned misc)
{
    unsigned c = clock_khz / 10u, hb = ht - ha, vb = vt - va;
    unsigned hso = hss - ha, hsw = hse - hss, vso = vss - va, vsw = vse - vss;

    d[0] = (uint8_t)c;
    d[1] = (uint8_t)(c >> 8);
    d[2] = (uint8_t)ha;
    d[3] = (uint8_t)hb;
    d[4] = (uint8_t)(((ha >> 8) << 4) | (hb >> 8));
    d[5] = (uint8_t)va;
    d[6] = (uint8_t)vb;
    d[7] = (uint8_t)(((va >> 8) << 4) | (vb >> 8));
    d[8] = (uint8_t)hso;
    d[9] = (uint8_t)hsw;
    d[10] = (uint8_t)(((vso & 0xFu) << 4) | (vsw & 0xFu));
    d[11] = (uint8_t)(((hso >> 8) << 6) | ((hsw >> 8) << 4) | ((vso >> 4) << 2) | (vsw >> 4));
    d[12] = (uint8_t)wmm;
    d[13] = (uint8_t)hmm;
    d[14] = (uint8_t)(((wmm >> 8) << 4) | (hmm >> 8));
    d[15] = 0;
    d[16] = 0;
    d[17] = (uint8_t)misc;
}

static void put_text(uint8_t *d, unsigned tag, const char *s)
{
    unsigned i, n = (unsigned)strlen(s);

    memset(d, 0, 18);
    d[3] = (uint8_t)tag;

    for (i = 0; i < 13; i++) {
        d[5 + i] = i < n ? (uint8_t)s[i] : i == n ? 0x0A : 0x20;
    }
}

static void put_base(uint8_t *b, unsigned revision, unsigned input, unsigned features, unsigned extensions)
{
    static const uint8_t header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    unsigned i;

    memset(b, 0, 128);
    memcpy(b, header, 8);
    b[8] = 0x2D;                        /* "KOS": 11 << 10 | 15 << 5 | 19 = 0x2DF3 */
    b[9] = 0xF3;
    b[10] = 0x34;                       /* product 0x3434 */
    b[11] = 0x34;
    b[12] = 0x04;                       /* serial 0x01020304 */
    b[13] = 0x03;
    b[14] = 0x02;
    b[15] = 0x01;
    b[16] = 12;
    b[17] = 2026 - 1990;
    b[18] = 1;
    b[19] = (uint8_t)revision;
    b[20] = (uint8_t)input;
    b[21] = 80;                         /* cm */
    b[22] = 34;
    b[24] = (uint8_t)features;

    for (i = 0; i < 8; i++) {           /* every standard timing unused */
        b[38 + i * 2] = 0x01;
        b[39 + i * 2] = 0x01;
    }

    b[126] = (uint8_t)extensions;
}

/* A DisplayID timing (`struct displayid_detailed_timings_1`): every field
 * one less than it says; the sync offsets' bit 15 their polarity. */
static void put_did(uint8_t *t, unsigned clock, unsigned options, unsigned ha, unsigned hb,
                    unsigned hso, unsigned hsw, bool hpos, unsigned va, unsigned vb,
                    unsigned vso, unsigned vsw, bool vpos)
{
    unsigned v[8] = { ha - 1, hb - 1, (hso - 1) | (hpos ? 0x8000u : 0), hsw - 1,
                      va - 1, vb - 1, (vso - 1) | (vpos ? 0x8000u : 0), vsw - 1 };
    unsigned i;

    clock -= 1;
    t[0] = (uint8_t)clock;
    t[1] = (uint8_t)(clock >> 8);
    t[2] = (uint8_t)(clock >> 16);
    t[3] = (uint8_t)options;

    for (i = 0; i < 8; i++) {
        t[4 + i * 2] = (uint8_t)v[i];
        t[5 + i * 2] = (uint8_t)(v[i] >> 8);
    }
}

/* A DisplayID section's own sum: bytes 1 to `5 + bytes`. */
static void sum_displayid(uint8_t *x)
{
    unsigned sum = 0, i, end = 5u + x[2];

    for (i = 1; i < end; i++) {
        sum += x[i];
    }

    x[end] = (uint8_t)(0x100u - (sum & 0xFFu));
}

/*
 * **A 34-inch ultrawide that can go to 144 Hz**, the kind of monitor the
 * M700 has, built whole: EDID 1.4, digital, 8 bits, DisplayPort.
 *
 * - Its detailed timing: 3440 x 1440 at 60 by CVT-RB (worked by hand in
 *   `test_intelgpu_display.c`): 319.75 MHz, 3600 x 1481 - 59 973 mHz.
 * - A name, a range that says CVT with reduced blanking, a serial.
 * - Standard timings: 1920 x 1200 at 60, which with reduced blanking
 *   taken is DMT's 0x44, 154 MHz, 2080 x 1235 - 59 950 mHz; 1920 x 1080
 *   at 75, which DMT does not have, by CVT; and 1360 x 765 at 60, DRM's
 *   "HDTV hack" to 1366 x 768.
 * - Established: 640 x 480 at 60, and byte 37's bit 7, 1152 x 864 at 75.
 * - A CTA block and a DisplayID 2.0 block, below.
 */
static size_t build_ultrawide(uint8_t *e)
{
    uint8_t *b = e, *x = e + 128, *y = e + 256;
    unsigned i;

    put_base(b, 4, 0xA5, 0x06, 2);
    put_dtd(&b[54], 319750, 3440, 3488, 3520, 3600, 1440, 1443, 1453, 1481, 800, 335,
            0x18 | 0x02);                           /* +H -V */
    put_text(&b[72], 0xFC, "Kosmos UW34");
    memset(&b[90], 0, 18);
    b[90 + 3] = 0xFD;
    b[90 + 5] = 48;                                 /* min V */
    b[90 + 6] = 144;
    b[90 + 7] = 30;                                 /* min H, kHz */
    b[90 + 8] = 230;
    b[90 + 9] = 80;                                 /* 800 MHz */
    b[90 + 10] = 0x04;                              /* CVT */
    b[90 + 11] = 0x11;                              /* CVT 1.1 */
    b[90 + 12] = 1u << 2;                           /* 0.25 MHz less: 799.75 */
    b[90 + 15] = 0x18;                              /* standard and reduced blanking */
    b[90 + 17] = 60;
    put_text(&b[108], 0xFF, "KS-0001");

    b[38] = (1920 - 248) / 8;                       /* 1920 x 1200 (16:10, 0) at 60 */
    b[39] = (0u << 6) | 0u;
    b[40] = (1920 - 248) / 8;                       /* 1920 x 1080 (16:9, 3) at 75 */
    b[41] = (3u << 6) | 15u;
    b[42] = (1360 - 248) / 8;                       /* 1360 x 765 at 60 */
    b[43] = (3u << 6) | 0u;
    b[35] = 1u << 5;                                /* 640 x 480 at 60 */
    b[37] = 0x80;                                   /* 1152 x 870 at 75 */
    sum_block(b);

    /* CTA-861, revision 3: underscan, audio, YCbCr, one native timing. */
    memset(x, 0, 128);
    x[0] = 0x02;
    x[1] = 3;
    x[3] = 0xF1;
    i = 4;
    x[i++] = (2u << 5) | 5u;                        /* video: 5 codes */
    x[i++] = 0x90;                                  /* VIC 16, native */
    x[i++] = 0x04;                                  /* VIC 4 */
    x[i++] = 0x05;                                  /* VIC 5, 1080i */
    x[i++] = 0x80;                                  /* 128: reserved */
    x[i++] = 0xC1;                                  /* VIC 193, whole */
    x[i++] = (3u << 5) | 7u;                        /* HDMI: 03 0C 00, 1.0.0.0, 300 MHz */
    x[i++] = 0x03; x[i++] = 0x0C; x[i++] = 0x00;
    x[i++] = 0x10; x[i++] = 0x00; x[i++] = 0x00; x[i++] = 60;
    x[i++] = (3u << 5) | 7u;                        /* HDMI Forum: D8 5D C4, 600 MHz */
    x[i++] = 0xD8; x[i++] = 0x5D; x[i++] = 0xC4;
    x[i++] = 0x01; x[i++] = 120; x[i++] = 0x00; x[i++] = 0x00;
    x[2] = (uint8_t)i;                              /* the timings from here */
    /* 1080p60 as CTA writes it; 1080i60 as a field, 540 lines, which the
     * decoder must double; and a stereo 720p, which it must refuse. */
    put_dtd(&x[i], 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 0, 0, 0x1E);
    put_dtd(&x[i + 18], 74250, 1920, 2008, 2052, 2200, 540, 542, 547, 562, 0, 0, 0x9E);
    put_dtd(&x[i + 36], 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, 0, 0, 0x3E);
    sum_block(x);

    /*
     * DisplayID 2.0: a Type VII timing, 3440 x 1440 at 144 by CVT-RB - the
     * line period (1/144 s - 460 us) / 1440 = 4.503 us, a blank of 102.2
     * + 1 = 103 lines, 1543 in all; 144 x 1543 x 3600 = 799.89 MHz, down
     * to 799.75 - 143 975 mHz, preferred - in kHz; and a Type I, 120 Hz:
     * 5.467 us, 84.1 + 1 = 85 lines, 1525; 658.8 MHz, down to 658.75 -
     * 119 991 mHz - in tens of kHz.
     */
    memset(y, 0, 128);
    y[0] = 0x70;
    y[1] = 0x20;
    y[2] = 46;
    y[3] = 0x04;                                    /* desktop productivity */
    y[5] = 0x22;
    y[7] = 20;
    put_did(&y[8], 799750, 0x80, 3440, 160, 48, 32, true, 1440, 103, 3, 10, false);
    y[28] = 0x03;
    y[30] = 20;
    put_did(&y[31], 65875, 0x00, 3440, 160, 48, 32, true, 1440, 85, 3, 10, false);
    sum_displayid(y);
    sum_block(y);
    return 384;
}

static void test_ultrawide(void)
{
    static uint8_t e[384];
    const struct edid_mode *m;
    size_t n = build_ultrawide(e);
    int s;

    maybe_break(e, 256 + 8);                        /* the 144 Hz clock */
    s = edid_decode(e, n, &info);
    check(s == EDID_OK, "ultrawide: decoded");
    check(strcmp(info.maker, "KOS") == 0 && info.product == 0x3434 && info.serial == 0x01020304,
          "ultrawide: KOS, 3434, serial 01020304");
    check(strcmp(info.name, "Kosmos UW34") == 0 && strcmp(info.serial_text, "KS-0001") == 0,
          "ultrawide: its name and serial text, the padding dropped");
    check(info.week == 12 && info.year == 2026, "ultrawide: week 12 of 2026");
    check(info.width_mm == 800 && info.height_mm == 335 && edid_diagonal_decinches(&info) == 341,
          "ultrawide: 800 x 335 mm from the preferred timing, not 80 x 34 cm; 34.1 inches");
    check(info.range.present && info.range.kind == 4 && info.range.min_vhz == 48 &&
          info.range.max_vhz == 144 && info.range.max_hkhz == 230 &&
          info.range.max_clock_khz == 799750 && info.range.reduced_blanking,
          "ultrawide: range CVT, 48-144 Hz, 799.75 MHz, reduced blanking");
    check(info.cta_blocks == 1 && info.displayid_blocks == 1, "ultrawide: a CTA and a DisplayID block");
    check(info.hdmi && info.hdmi_max_tmds_khz == 300000, "ultrawide: HDMI VSDB, 300 MHz");
    check(info.hdmi_forum && info.hdmi_forum_max_tmds_khz == 600000, "ultrawide: HDMI Forum, 600 MHz");
    check(info.problems == (EDID_PROBLEM_UNKNOWN_VIC | EDID_PROBLEM_BAD_DETAILED),
          "ultrawide: code 128 unknown and the stereo timing refused, and nothing else");

    m = &info.modes[0];
    check(info.nmodes == 12 && is(m, 319750, 3600, 1481, EDID_FROM_DETAILED,
                                  EDID_MODE_PREFERRED | EDID_MODE_HSYNC_POS) &&
          m->refresh_mhz == 59973 && m->hsync_start == 3488 && m->vsync_end == 1453,
          "ultrawide: the first of 12 is 3440 x 1440 at 59.973, preferred");
    check(edid_preferred(&info) == m, "ultrawide: the base block's timing is the preferred one");
    check(is(find(3440, 1440, 143975), 799750, 3600, 1543, EDID_FROM_DISPLAYID,
             EDID_MODE_PREFERRED | EDID_MODE_HSYNC_POS) && info.modes[1].refresh_mhz == 143975,
          "ultrawide: Type VII 144 Hz in kHz, second, marked preferred by its own bit");
    check(is(find(3440, 1440, 119991), 658750, 3600, 1525, EDID_FROM_DISPLAYID, EDID_MODE_HSYNC_POS),
          "ultrawide: Type I 120 Hz in tens of kHz");

    m = find(1920, 1080, 60000);
    check(is(m, 148500, 2200, 1125, EDID_FROM_CTA_DETAILED,
             EDID_MODE_NATIVE | EDID_MODE_HSYNC_POS | EDID_MODE_VSYNC_POS),
          "ultrawide: CTA's 1080p60 kept once, marked native by VIC 16 (0x90)");
    m = find(1920, 1080, 60000 + 0);
    m = NULL;

    {
        unsigned i;

        for (i = 0; i < info.nmodes; i++) {
            if (info.modes[i].flags & EDID_MODE_INTERLACED) {
                m = &info.modes[i];
            }
        }
    }

    check(m && m->width == 1920 && m->height == 1080 && m->vtotal == 1125 && m->vsync_start == 1084 &&
          m->vsync_end == 1094 && m->refresh_mhz == 60000 && m->source == EDID_FROM_CTA_DETAILED,
          "ultrawide: 1080i written as 540-line fields doubled to frames, 60 fields a second, kept over VIC 5");
    check(is(find(1280, 720, 60000), 74250, 1650, 750, EDID_FROM_CTA_VIC,
             EDID_MODE_HSYNC_POS | EDID_MODE_VSYNC_POS) && find(1280, 720, 60000)->vic == 4,
          "ultrawide: VIC 4, 720p60 - the stereo DTD of the same mode was refused");
    check(is(find(5120, 2160, 120000), 1485000, 5500, 2250, EDID_FROM_CTA_VIC,
             EDID_MODE_HSYNC_POS | EDID_MODE_VSYNC_POS) && find(5120, 2160, 120000)->vic == 193,
          "ultrawide: VIC 193 (0xC1) read whole, 5120 x 2160 at 120");

    check(is(find(1920, 1200, 59950), 154000, 2080, 1235, EDID_FROM_STANDARD, EDID_MODE_HSYNC_POS),
          "ultrawide: 1920 x 1200 at 60 is DMT's reduced blanking, since the range takes it");
    /* 1920 x 1080 at 75 by CVT, worked by hand: the line period (1/75 s -
     * 550 us) / (1080 + 3) = 11.803 us; 550 us of sync and porch is 46.6
     * + 1 = 47 lines, 1130 in all; the blanking 30% - 300 x 11.803 / 1000
     * = 26.46%, 690.8 pixels, 688 to the 16: 2608 in all; the clock
     * 2608 / 11.803 us = 220.96 MHz, down to 220.75 - VESA's figure; the
     * sync 2064-2264 and 1083-1088. 220 750 000 / (2608 x 1130) = 74.906 Hz. */
    m = find(1920, 1080, 74906);
    check(is(m, 220750, 2608, 1130, EDID_FROM_STANDARD, EDID_MODE_VSYNC_POS | EDID_MODE_COMPUTED) &&
          m->hsync_start == 2064 && m->hsync_end == 2264 && m->vsync_start == 1083 && m->vsync_end == 1088,
          "ultrawide: 1920 x 1080 at 75, not in DMT, by CVT: 220.75 MHz");
    /* 1366 x 768: CVT's 1360 x 768 - 20.903 us, 27 + 3 + 768 = 798 lines,
     * 23.73% is 416 pixels, 1776; 84.96 MHz down to 84.75 - with 1366
     * shown and the sync one left. */
    m = find(1366, 768, 59799);
    check(is(m, 84750, 1776, 798, EDID_FROM_STANDARD, EDID_MODE_VSYNC_POS | EDID_MODE_COMPUTED) &&
          m->hsync_start == 1431 && m->hsync_end == 1567,
          "ultrawide: 1360 x 765 at 60 is DRM's 1366 x 768");
    check(is(find(640, 480, 59940), 25175, 800, 525, EDID_FROM_ESTABLISHED, 0) &&
          is(find(1152, 864, 75000), 108000, 1600, 900, EDID_FROM_ESTABLISHED,
             EDID_MODE_HSYNC_POS | EDID_MODE_VSYNC_POS),
          "ultrawide: established 640 x 480, and byte 37's 1152 x 864 at 75");

    if (failed) {
        dump();
    }
}

/*
 * **An analogue monitor of 2003**: EDID 1.3, the preferred-timing bit
 * clear, a GTF range. Its standard timings fall to GTF where DMT has
 * nothing: 1152 x 864 at 60, by `drm_gtf_mode`'s integers - the line rate
 * (865 x 2 x 1000 x 60) / ((1 000 000 - 550 x 60) / 500) = 53 671 Hz,
 * sync and porch 30 lines, 895 in all; the duty cycle 30% - 300 / 53.671
 * = 24.411%, 372 pixels, 368 to the 16: 1520; 1520 x 53.671 = 81.579 MHz,
 * 59 967 mHz.
 */
static size_t build_analogue(uint8_t *b, unsigned revision)
{
    put_base(b, revision, 0x0E, 0x00, 0);
    put_dtd(&b[54], 108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, 0, 0, 0x1E);
    put_text(&b[72], 0xFF, "ABC123");
    memset(&b[90], 0, 18);
    b[90 + 3] = 0xFD;
    b[90 + 5] = 50;
    b[90 + 6] = 75;
    b[90 + 7] = 30;
    b[90 + 8] = 82;
    b[90 + 9] = 14;
    b[90 + 10] = 0x00;                              /* GTF */
    memset(&b[108], 0, 18);
    b[108 + 3] = 0x10;                              /* dummy */
    b[38] = (1152 - 248) / 8;                       /* 1152 x 864 (4:3, 1) at 60 */
    b[39] = (1u << 6) | 0u;
    b[40] = (1280 - 248) / 8;                       /* 1280, aspect 0, at 60 */
    b[41] = (0u << 6) | 0u;
    sum_block(b);
    return 128;
}

static void test_analogue(void)
{
    static uint8_t b[128];
    const struct edid_mode *m;
    int s;

    build_analogue(b, 3);
    maybe_break(b, 39);                             /* the refresh: 61 */
    s = edid_decode(b, 128, &info);
    check(s == EDID_OK, "analogue: decoded");
    check(!info.digital && info.bpc == 0 && info.interface == 0, "analogue: not digital, no depth");
    check(info.range.present && info.range.kind == 0 && info.range.max_clock_khz == 140000 &&
          !info.range.reduced_blanking, "analogue: a GTF range to 140 MHz");
    check(info.width_mm == 800 && info.height_mm == 340, "analogue: no size in the timing, so 80 x 34 cm");
    check(info.name[0] == 0 && strcmp(info.serial_text, "ABC123") == 0, "analogue: a serial, no name");
    check(info.nmodes == 3 && info.modes[0].flags == (EDID_MODE_HSYNC_POS | EDID_MODE_VSYNC_POS) &&
          info.modes[0].width == 1280 && info.modes[0].refresh_mhz == 60020,
          "analogue: 1.3 with the feature bit clear has no preferred timing");
    check(edid_preferred(&info) == &info.modes[0], "analogue: so the first stands for it");
    m = find(1152, 864, 59967);
    check(is(m, 81579, 1520, 895, EDID_FROM_STANDARD, EDID_MODE_VSYNC_POS | EDID_MODE_COMPUTED) &&
          m->hsync_start == 1216 && m->hsync_end == 1336 && m->vsync_start == 865 && m->vsync_end == 868,
          "analogue: 1152 x 864 at 60 by GTF, 81.579 MHz");
    check(is(find(1280, 800, 59810), 83500, 1680, 831, EDID_FROM_STANDARD, EDID_MODE_VSYNC_POS),
          "analogue: aspect 0 at 1.3 is 16:10 - DMT 0x1c, not the RB 0x1b an analogue monitor is not given");

    if (failed) {
        dump();
    }

    /* At 1.2 aspect 0 is 1:1, and 1280 x 1280 is nowhere but GTF. */
    build_analogue(b, 2);
    s = edid_decode(b, 128, &info);
    check(s == EDID_OK && info.nmodes == 3 && info.modes[2].width == 1280 && info.modes[2].height == 1280 &&
          (info.modes[2].flags & EDID_MODE_COMPUTED), "analogue 1.2: aspect 0 is square");
    check(info.modes[0].flags == (EDID_MODE_HSYNC_POS | EDID_MODE_VSYNC_POS),
          "analogue 1.2: no preferred timing either");
}

/* The ways an EDID is wrong. */
static void test_wrong(void)
{
    static uint8_t e[384 + 128];
    size_t n;
    int s;

    n = build_ultrawide(e);
    check(edid_decode(e, 100, &info) == EDID_ERR_SHORT && info.nmodes == 0, "100 bytes: refused, short");
    check(strcmp(edid_status_name(EDID_ERR_SHORT), "shorter than a block") == 0, "...by name");

    e[0] = 0x01;
    sum_block(e);
    check(edid_decode(e, n, &info) == EDID_ERR_HEADER, "a header byte wrong: refused");

    n = build_ultrawide(e);
    e[20] ^= 0x10;
    check(edid_decode(e, n, &info) == EDID_ERR_CHECKSUM && info.nmodes == 0 && info.maker[0] == 0,
          "a byte changed and the sum not: refused, and nothing said");
    check(strcmp(edid_status_name(EDID_ERR_CHECKSUM), "the base block's checksum is wrong") == 0, "...by name");

    n = build_ultrawide(e);
    e[18] = 2;
    sum_block(e);
    check(edid_decode(e, n, &info) == EDID_ERR_VERSION, "EDID 2: refused");

    /* The count says three; the bytes hold two. */
    n = build_ultrawide(e);
    e[126] = 3;
    sum_block(e);
    s = edid_decode(e, n, &info);
    check(s == EDID_OK && (info.problems & EDID_PROBLEM_EXT_MISSING) && info.extensions == 3 &&
          info.cta_blocks == 1 && info.displayid_blocks == 1 && info.nmodes == 12,
          "an extension count past the bytes: the two there decoded, and said");

    /* The base block alone, its count still two. */
    n = build_ultrawide(e);
    s = edid_decode(e, 128, &info);
    check(s == EDID_OK && (info.problems & EDID_PROBLEM_EXT_MISSING) && info.cta_blocks == 0 &&
          info.nmodes == 6, "the base block alone: its own six modes");

    /* A CTA block's sum wrong: decoded, as DRM does, and said. */
    n = build_ultrawide(e);
    e[128 + 100] ^= 1;
    s = edid_decode(e, n, &info);
    check(s == EDID_OK && (info.problems & EDID_PROBLEM_EXT_CHECKSUM) && info.hdmi && info.nmodes == 12,
          "a CTA checksum wrong: still decoded");

    /* A DisplayID block's sum wrong: left out. */
    n = build_ultrawide(e);
    e[256 + 100] ^= 1;
    s = edid_decode(e, n, &info);
    check(s == EDID_OK && (info.problems & EDID_PROBLEM_EXT_CHECKSUM) && !find(3440, 1440, 143975) &&
          info.nmodes == 10, "a DisplayID block's checksum wrong: its timings left out");

    /* A DisplayID section's own sum wrong, the block's right. */
    n = build_ultrawide(e);
    e[256 + 51] ^= 1;
    sum_block(&e[256]);
    s = edid_decode(e, n, &info);
    check(s == EDID_OK && (info.problems & EDID_PROBLEM_DISPLAYID) && !find(3440, 1440, 119991),
          "a DisplayID section's checksum wrong: left out, and said");

    /* A DisplayID section longer than its block. */
    n = build_ultrawide(e);
    e[256 + 2] = 125;
    sum_block(&e[256]);
    s = edid_decode(e, n, &info);
    check(s == EDID_OK && (info.problems & EDID_PROBLEM_DISPLAYID) && !find(3440, 1440, 143975),
          "a DisplayID section past its block: left out");

    /* A CTA data block running past the timings' offset. */
    n = build_ultrawide(e);
    e[128 + 10] = (3u << 5) | 31u;
    sum_block(&e[128]);
    s = edid_decode(e, n, &info);
    check(s == EDID_OK && (info.problems & EDID_PROBLEM_CTA) && !info.hdmi,
          "a CTA data block past its collection: the rest of the collection left");

    /* An extension this kit does not read. */
    n = build_ultrawide(e);
    memset(&e[384], 0, 128);
    e[384] = 0xF0;                                  /* a block map */
    sum_block(&e[384]);
    e[126] = 3;
    sum_block(e);
    s = edid_decode(e, n + 128, &info);
    check(s == EDID_OK && info.problems == (EDID_PROBLEM_UNKNOWN_VIC | EDID_PROBLEM_BAD_DETAILED |
                                            EDID_PROBLEM_EXT_UNKNOWN) && info.nmodes == 12,
          "an unknown extension: passed over, and said");
}

/* More modes than the list holds: fourteen DisplayID blocks of five. */
static void test_too_many(void)
{
    static uint8_t e[15 * 128];
    unsigned k, j, w = 640;
    int s;

    put_base(e, 4, 0xA5, 0x06, 14);
    sum_block(e);

    for (k = 1; k <= 14; k++) {
        uint8_t *y = &e[k * 128];

        memset(y, 0, 128);
        y[0] = 0x70;
        y[1] = 0x13;
        y[2] = 3 + 100;
        y[5] = 0x03;
        y[7] = 100;

        for (j = 0; j < 5; j++, w += 8) {
            put_did(&y[8 + j * 20], 14850, 0, w, 160, 48, 32, true, 480, 30, 3, 4, false);
        }

        sum_displayid(y);
        sum_block(y);
    }

    s = edid_decode(e, sizeof e, &info);
    check(s == EDID_OK && info.nmodes == EDID_MODES_MAX && (info.problems & EDID_PROBLEM_TOO_MANY_MODES) &&
          info.modes[63].width == 640 + 63 * 8 && info.displayid_blocks == 14,
          "70 modes: the first 64 kept, and said");
}

int main(void)
{
    broken = getenv("EDIDTEST_BROKEN") != NULL;

    test_qemu();
    test_ultrawide();
    test_analogue();
    test_wrong();
    test_too_many();

    printf("%s: %d checks on edid\n", failed ? "FAIL" : "PASS", checks);
    return failed != 0;
}
