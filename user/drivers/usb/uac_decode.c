/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A USB Audio Class device's descriptors read (`uac_decode.h`,
 * `docs/usbaudio.md` U1). No hardware, no system calls: bytes in, a
 * description out, held on the Mac by `tools/test_uacdecode.c`.
 */

#include "uac_decode.h"

#include <string.h>

/* `user/include/bytes.h`, by its path from here: the host test compiles this
 * file with no include path for it. USB is little-endian throughout. */
#include "../../include/bytes.h"

#define DT_CONFIGURATION    0x02u
#define DT_INTERFACE        0x04u
#define DT_ENDPOINT         0x05u
#define DT_CS_INTERFACE     0x24u
#define DT_CS_ENDPOINT      0x25u
#define EP_GENERAL          0x01u

#define CLASS_AUDIO         0x01u
#define SUB_CONTROL         0x01u
#define SUB_STREAMING       0x02u
#define PROTOCOL_UAC2       0x20u   /* IP_VERSION_02_00 */

/* AudioControl subtypes: the same numbers in both versions, for the ones
 * read here, and the clock source in UAC2 only. */
#define AC_HEADER           0x01u
#define AC_INPUT_TERMINAL   0x02u
#define AC_OUTPUT_TERMINAL  0x03u
#define AC_FEATURE_UNIT     0x06u
#define AC_CLOCK_SOURCE     0x0Au

/* AudioStreaming subtypes. */
#define AS_GENERAL          0x01u
#define AS_FORMAT_TYPE      0x02u

#define FORMAT_TYPE_I       0x01u
#define TAG_PCM             0x0001u /* UAC1 wFormatTag */
#define FORMATS_PCM         0x1u    /* UAC2 bmFormats bit 0 */

static uint32_t get_le24(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

/* A setting being read: its fields, and what has been seen of it. */
struct pending {
    bool active;
    uint8_t version;            /* the streaming interface's: 1 or 2 */
    bool endpoint, pcm, type1;
    bool after_data;            /* the last endpoint was the setting's own */
    struct uac_alt alt;
};

/* The terminal types that are the USB end of the device (0x01xx), not
 * what a person plays into or speaks at. */
static bool usb_end(uint16_t type)
{
    return (type >> 8) == 0x01u;
}

static void finish(struct pending *p, struct uac_device *out)
{
    const struct uac_alt *a = &p->alt;

    if (!p->active) {
        return;
    }

    p->active = false;

    if (!p->endpoint || !p->pcm || !p->type1 || a->channels == 0
        || a->subframe == 0 || a->subframe > 4
        || a->bits == 0 || a->bits > a->subframe * 8u) {
        out->skipped++;
        return;
    }

    if (out->nalts == UAC_ALTS_MAX) {
        out->alts_dropped++;
        return;
    }

    out->alts[out->nalts++] = *a;

    if (a->in) {
        out->records = true;
    } else {
        out->plays = true;
    }
}

static void add_rate(struct uac_alt *a, uint32_t rate, unsigned *dropped)
{
    unsigned i;

    for (i = 0; i < a->nrates; i++) {
        if (a->rates[i] == rate) {
            return;
        }
    }

    if (a->nrates == UAC_RATES_MAX) {
        if (dropped != 0) {
            (*dropped)++;
        }
        return;
    }

    a->rates[a->nrates++] = rate;
}

/* What the AudioControl walk keeps for after it: every feature unit, and
 * where the output terminal's sound comes from, so the unit on its path
 * can be found whatever order the descriptors came in. */
struct units {
    struct uac_feature fu[UAC_FEATURES_MAX];
    unsigned nfu;
    uint8_t output_source;      /* the chosen output terminal's bSourceID */
};

/* A control's place in one feature unit: the master, every logical
 * channel, or nowhere. `master` and `each` say which channels have it and
 * will take it; `some` that one does but will not (UAC2's 0b01). Every
 * logical channel or none: a volume on one channel of two would move the
 * balance. */
static uint8_t where(bool master, uint32_t each, unsigned channels)
{
    uint32_t all;

    if (master) {
        return UAC_CONTROL_MASTER;
    }

    if (channels == 0 || channels > UAC_FU_CHANNELS_MAX) {
        return UAC_CONTROL_NONE;
    }

    all = channels == 32u ? 0xFFFFFFFFu : (1u << channels) - 1u;
    return each == all ? UAC_CONTROL_CHANNELS : UAC_CONTROL_NONE;
}

/*
 * A feature unit, both versions (`uac_decode.h`, `struct uac_feature`).
 * UAC1 (1.0 4.3.2.5, Table 4-7): bUnitID 3, bSourceID 4, bControlSize 5,
 * then (ch + 1) bitmaps of that many bytes from 6 and iFeature, so bLength
 * is 7 + (ch + 1) x n; D0 mute and D1 volume are in each bitmap's first
 * byte, little-endian as everything USB is. UAC2 (2.0 4.7.2.8, Table 4-13):
 * bUnitID 3, bSourceID 4, then (ch + 1) bitmaps of four bytes from 5 and
 * iFeature, so 6 + (ch + 1) x 4; a control is a pair, D1..0 mute and D3..2
 * volume, and only 0b11 is one the host may set.
 */
static void feature_unit(const uint8_t *d, unsigned len, uint8_t version,
                         struct units *u)
{
    struct uac_feature f;
    unsigned n, ch, i;
    bool mute0, vol0;
    uint32_t mutes = 0, vols = 0;

    if (u->nfu == UAC_FEATURES_MAX) {
        return;
    }

    memset(&f, 0, sizeof(f));

    if (version == 2) {
        uint32_t bits;

        if (len < 10) {
            return;
        }

        ch = (len - 6u) / 4u - 1u;
        bits = get_le32(d + 5);
        mute0 = (bits & 0x3u) == 0x3u;
        vol0 = ((bits >> 2) & 0x3u) == 0x3u;
        f.read_only = (bits & 0x3u) == 0x1u || ((bits >> 2) & 0x3u) == 0x1u;

        for (i = 1; i <= ch && i <= UAC_FU_CHANNELS_MAX; i++) {
            bits = get_le32(d + 5 + 4 * i);
            mutes |= (bits & 0x3u) == 0x3u ? 1u << (i - 1) : 0u;
            vols |= ((bits >> 2) & 0x3u) == 0x3u ? 1u << (i - 1) : 0u;
            f.read_only = f.read_only || (bits & 0x3u) == 0x1u
                       || ((bits >> 2) & 0x3u) == 0x1u;
        }
    } else {
        if (len < 7) {
            return;
        }

        n = d[5];

        if (n == 0 || len - 7u < n) {
            return;
        }

        ch = (len - 7u) / n - 1u;
        mute0 = (d[6] & 0x01u) != 0;
        vol0 = (d[6] & 0x02u) != 0;

        for (i = 1; i <= ch && i <= UAC_FU_CHANNELS_MAX; i++) {
            uint8_t bits = d[6 + n * i];

            mutes |= (bits & 0x01u) != 0 ? 1u << (i - 1) : 0u;
            vols |= (bits & 0x02u) != 0 ? 1u << (i - 1) : 0u;
        }
    }

    f.unit = d[3];
    f.source = d[4];
    f.channels = ch > 255u ? 255u : (uint8_t)ch;
    f.volume = where(vol0, vols, ch);
    f.mute = where(mute0, mutes, ch);
    f.volume_channels = f.volume == UAC_CONTROL_CHANNELS ? vols : 0;
    f.mute_channels = f.mute == UAC_CONTROL_CHANNELS ? mutes : 0;
    u->fu[u->nfu++] = f;
}

static const struct uac_feature *find_unit(const struct units *u, uint8_t id)
{
    unsigned i;

    for (i = 0; id != 0 && i < u->nfu; i++) {
        if (u->fu[i].unit == id) {
            return &u->fu[i];
        }
    }

    return NULL;
}

/* From the output terminal upstream, through feature units only: the
 * nearest with a volume it takes, else the nearest with a mute; and where
 * there is neither, whether one had controls it would not take. At most
 * UAC_FEATURES_MAX steps, so a loop in a broken device's graph ends. */
static void volume_on_path(const struct units *u, struct uac_device *out)
{
    const struct uac_feature *f, *mute_only = NULL;
    uint8_t at = u->output_source;
    bool read_only = false;
    unsigned step;

    for (step = 0; step < UAC_FEATURES_MAX
                   && (f = find_unit(u, at)) != NULL; step++) {
        if (f->volume != UAC_CONTROL_NONE) {
            out->volume = *f;
            return;
        }

        if (f->mute != UAC_CONTROL_NONE && mute_only == NULL) {
            mute_only = f;
        }

        read_only = read_only || f->read_only;
        at = f->source;
    }

    if (mute_only != NULL) {
        out->volume = *mute_only;
    } else {
        out->volume.read_only = read_only;
    }
}

/* AudioControl's descriptors: what kind of device, its unit IDs. */
static void control(const uint8_t *d, unsigned len, uint8_t version,
                    struct uac_device *out, struct units *u)
{
    uint16_t type;

    switch (d[2]) {
    case AC_HEADER:
        if (len >= 5) {
            out->adc = get_le16(d + 3);
        }
        break;
    case AC_INPUT_TERMINAL:
        if (len >= 6) {
            type = get_le16(d + 4);
            if (!usb_end(type) && out->input_type == 0) {
                out->input_type = type;
            }
        }
        break;
    case AC_OUTPUT_TERMINAL:
        if (len >= 6) {
            type = get_le16(d + 4);
            if (!usb_end(type) && out->output_type == 0) {
                out->output_type = type;
                /* bSourceID, 7 in both (1.0 Table 4-4, 2.0 Table 4-10) */
                u->output_source = len >= 8 ? d[7] : 0;
            }
        }
        break;
    case AC_FEATURE_UNIT:
        if (len >= 4 && out->feature == 0) {
            out->feature = d[3];
        }
        feature_unit(d, len, version, u);
        break;
    case AC_CLOCK_SOURCE:
        if (version == 2 && len >= 4 && out->clock == 0) {
            out->clock = d[3];
        }
        break;
    default:
        break;
    }
}

/* AudioStreaming's: the setting's format. UAC1 2.2.5 and 4.5.2, UAC2
 * 4.9.2 and FMT20 2.3.1.6 - the same subtypes, laid out differently. */
static void streaming(const uint8_t *d, unsigned len, struct pending *p,
                      struct uac_device *out)
{
    struct uac_alt *a = &p->alt;
    unsigned n, i;

    if (d[2] == AS_GENERAL) {
        if (p->version == 1 && len >= 7) {
            a->terminal = d[3];
            p->pcm = get_le16(d + 5) == TAG_PCM;
        } else if (p->version == 2 && len >= 11) {
            a->terminal = d[3];
            p->pcm = d[5] == FORMAT_TYPE_I
                  && (get_le32(d + 6) & FORMATS_PCM) != 0;
            a->channels = d[10];
        }
        return;
    }

    if (d[2] != AS_FORMAT_TYPE) {
        return;
    }

    if (p->version == 2) {
        if (len >= 6) {
            p->type1 = d[3] == FORMAT_TYPE_I;
            a->subframe = d[4];
            a->bits = d[5];
        }
        return;
    }

    if (len < 8) {
        return;
    }

    p->type1 = d[3] == FORMAT_TYPE_I;
    a->channels = d[4];
    a->subframe = d[5];
    a->bits = d[6];
    n = d[7];

    if (n == 0) {
        if (len >= 14) {
            a->continuous = true;
            a->rates[0] = get_le24(d + 8);
            a->rates[1] = get_le24(d + 11);
            a->nrates = 2;
        }
        return;
    }

    for (i = 0; i < n && 8u + 3u * i + 3u <= len; i++) {
        add_rate(a, get_le24(d + 8 + 3 * i), &out->rates_dropped);
    }
}

/* An isochronous endpoint: the sound's, or an asynchronous output's
 * feedback. A feedback endpoint says so in its usage bits (USB 2.0 9.6.6,
 * 5:4 = 01); a UAC1 device's is an IN beside an OUT, named by the OUT's
 * bSynchAddress, with usage bits a USB 1.1 device leaves at zero. */
static void endpoint(const uint8_t *d, unsigned len, struct pending *p)
{
    struct uac_alt *a = &p->alt;
    uint8_t address = d[2], attributes = d[3];
    uint16_t size = get_le16(d + 4);
    bool in = (address & 0x80u) != 0;

    p->after_data = false;

    if (len < 7 || (attributes & 0x3u) != 0x1u) {
        return;
    }

    if (((attributes >> 4) & 0x3u) == 0x1u
        || (p->endpoint && in && !a->in)) {
        if (a->feedback == 0) {
            a->feedback = address & 0x0Fu;
            a->feedback_packet = size & 0x7FFu;
            a->feedback_interval = d[6];
        }
        return;
    }

    if (p->endpoint) {
        return;
    }

    p->endpoint = true;
    p->after_data = true;
    a->endpoint = address & 0x0Fu;
    a->in = in;
    a->sync = (attributes >> 2) & 0x3u;
    a->packet = size & 0x7FFu;
    a->extra = (size >> 11) & 0x3u;
    a->interval = d[6];
}

void uac_decode_config(const uint8_t *bytes, unsigned length,
                       struct uac_device *out)
{
    struct pending p;
    struct units u;
    unsigned at = 0, len;
    uint8_t class_code = 0, subclass = 0, protocol = 0;
    bool seen_control = false;
    const uint8_t *d;

    memset(out, 0, sizeof(*out));
    memset(&p, 0, sizeof(p));
    memset(&u, 0, sizeof(u));

    while (at < length) {
        if (length - at < 2) {
            out->malformed = true;
            break;
        }

        d = bytes + at;
        len = d[0];

        if (len < 2 || len > length - at) {
            out->malformed = true;
            break;
        }

        switch (d[1]) {
        case DT_CONFIGURATION:
            if (len >= 9) {
                out->configuration = d[5];
            }
            break;

        case DT_INTERFACE:
            finish(&p, out);

            if (len < 9) {
                class_code = 0;
                break;
            }

            class_code = d[5];
            subclass = d[6];
            protocol = d[7];

            if (class_code == CLASS_AUDIO && subclass == SUB_CONTROL
                && !seen_control) {
                seen_control = true;
                out->control = d[2];
                out->version = protocol == PROTOCOL_UAC2 ? 2 : 1;
            }

            if (class_code == CLASS_AUDIO && subclass == SUB_STREAMING
                && d[4] > 0) {
                memset(&p, 0, sizeof(p));
                p.active = true;
                p.version = protocol == PROTOCOL_UAC2 ? 2 : 1;
                p.alt.interface = d[2];
                p.alt.alternate = d[3];
            }
            break;

        case DT_CS_INTERFACE:
            if (len < 3 || class_code != CLASS_AUDIO) {
                break;
            }

            if (subclass == SUB_CONTROL) {
                control(d, len, protocol == PROTOCOL_UAC2 ? 2 : 1, out, &u);
            } else if (subclass == SUB_STREAMING && p.active) {
                streaming(d, len, &p, out);
            }
            break;

        case DT_ENDPOINT:
            if (p.active) {
                endpoint(d, len, &p);
            }
            break;

        case DT_CS_ENDPOINT:
            /* UAC1's class endpoint descriptor (4.6.1.2), after the
             * setting's own endpoint: bmAttributes bit 0 is its sampling
             * frequency control. UAC2's rate is its clock's instead. */
            if (p.active && p.after_data && p.version == 1 && len >= 7
                && d[2] == EP_GENERAL) {
                p.alt.sets_rate = (d[3] & 0x01u) != 0;
            }
            break;

        default:
            break;
        }

        at += len;
    }

    finish(&p, out);
    volume_on_path(&u, out);
    out->ok = seen_control && out->nalts > 0;
}

const char *uac_kind(const struct uac_device *d)
{
    if (d->plays && d->records) {
        return "headset";
    }

    if (d->plays) {
        if (d->output_type == 0x0302u) {
            return "headphones";
        }
        return (d->output_type >> 8) == 0x03u ? "speaker" : "audio output";
    }

    if (d->records) {
        return (d->input_type >> 8) == 0x02u ? "microphone" : "audio input";
    }

    return "audio";
}

bool uac_takes(const struct uac_alt *a, uint32_t rate)
{
    unsigned i;

    if (a->continuous) {
        return a->nrates == 2 && rate >= a->rates[0] && rate <= a->rates[1];
    }

    for (i = 0; i < a->nrates; i++) {
        if (a->rates[i] == rate) {
            return true;
        }
    }

    return false;
}

/* The rate to run `a` at for a mix at `mix`, and how good a choice it is:
 * lower is better - the mix's own, then the nearest above, then the
 * nearest below, then none known. */
static uint64_t rate_for(const struct uac_alt *a, uint32_t mix, uint32_t *r)
{
    uint32_t above = 0, below = 0, x;
    unsigned i;

    if (uac_takes(a, mix)) {
        *r = mix;
        return 0;
    }

    if (a->continuous && a->nrates == 2) {
        if (mix < a->rates[0]) {
            above = a->rates[0];
        } else {
            below = a->rates[1];
        }
    } else {
        for (i = 0; i < a->nrates; i++) {
            x = a->rates[i];
            if (x > mix && (above == 0 || x < above)) {
                above = x;
            }
            if (x < mix && x > below) {
                below = x;
            }
        }
    }

    if (above != 0) {
        *r = above;
        return (1ull << 32) | (above - mix);
    }

    if (below != 0) {
        *r = below;
        return (2ull << 32) | (mix - below);
    }

    *r = 0;
    return 3ull << 32;
}

int uac_pick_for(const struct uac_device *d, bool in, uint32_t want_rate,
                 unsigned want_bits, uint32_t *rate)
{
    int best = -1;
    unsigned i;
    uint64_t key[4], best_key[4] = { 0, 0, 0, 0 };
    uint32_t r, best_rate = 0;
    const struct uac_alt *a;

    for (i = 0; i < d->nalts; i++) {
        uint64_t score;
        unsigned k;
        bool better;

        a = &d->alts[i];

        if (a->in != in) {
            continue;
        }

        score = rate_for(a, want_rate, &r);

        /* Lower is better, in this order: two channels, then more, then
         * one; no bits short of the stream's; the stream's own rate, then
         * the nearest above, then below; the fewest bits past its; and
         * the nearest rate of its kind. */
        key[0] = a->channels == 2 ? 0 : a->channels > 2 ? 1 : 2;
        key[1] = a->bits >= want_bits ? 0 : want_bits - a->bits;
        key[2] = score >> 32;
        key[3] = ((uint64_t)(a->bits >= want_bits ? a->bits - want_bits : 0) << 32)
               | (score & 0xFFFFFFFFu);

        better = best < 0;

        for (k = 0; k < 4 && !better; k++) {
            if (key[k] != best_key[k]) {
                better = key[k] < best_key[k];
                break;
            }
        }

        if (better) {
            best = (int)i;
            memcpy(best_key, key, sizeof(key));
            best_rate = r;
        }
    }

    if (rate != 0) {
        *rate = best < 0 ? 0 : best_rate;
    }

    return best;
}

int uac_pick(const struct uac_device *d, bool in, uint32_t mix_rate,
             uint32_t *rate)
{
    return uac_pick_for(d, in, mix_rate, 16u, rate);
}

/* The rates a wide subrange is offered at: those devices are built for. */
static const uint32_t COMMON[] = {
    8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000, 88200, 96000,
    176400, 192000, 352800, 384000, 705600, 768000,
};

bool uac_rates_from_range(const uint8_t *bytes, unsigned length,
                          struct uac_alt *a)
{
    unsigned n, i, k;
    uint32_t lo, hi, step, r;

    if (length < 2) {
        return false;
    }

    n = get_le16(bytes);

    if (2u + 12u * n > length) {
        return false;
    }

    a->continuous = false;
    a->nrates = 0;

    for (i = 0; i < n; i++) {
        lo = get_le32(bytes + 2 + 12 * i);
        hi = get_le32(bytes + 6 + 12 * i);
        step = get_le32(bytes + 10 + 12 * i);

        if (lo == hi) {
            add_rate(a, lo, 0);
            continue;
        }

        for (k = 0; k < sizeof(COMMON) / sizeof(COMMON[0]); k++) {
            r = COMMON[k];
            if (r >= lo && r <= hi && (step == 0 || (r - lo) % step == 0)) {
                add_rate(a, r, 0);
            }
        }
    }

    return true;
}

/*
 * ---- The speaker's own volume (U10g, `uac_decode.h`) ----
 */

#define RT_GET              0xA1u   /* 10100001B: in, class, interface */
#define RT_SET              0x21u   /* 00100001B: out, class, interface */
#define UAC1_SET_CUR        0x01u   /* Audio 1.0 A.9, Table A-9 */
#define UAC1_GET_CUR        0x81u
#define UAC1_GET_MIN        0x82u
#define UAC1_GET_MAX        0x83u
#define UAC1_GET_RES        0x84u
#define UAC2_CUR            0x01u   /* Audio 2.0 A.14, Table A-14 */
#define UAC2_RANGE          0x02u

bool uac_fu_request(const struct uac_device *d, unsigned selector,
                    enum uac_attribute attribute, bool set, uint8_t channel,
                    struct uac_setup *out)
{
    const struct uac_feature *f = &d->volume;
    uint8_t request;
    uint16_t length;

    if (f->unit == 0 || channel > f->channels
        || (selector != UAC_FU_MUTE && selector != UAC_FU_VOLUME)
        || (set && attribute != UAC_ATTR_CUR)
        || (selector == UAC_FU_MUTE && attribute != UAC_ATTR_CUR)) {
        return false;
    }

    /* A mute's block is one byte (1.0 Table 5-16, 2.0 Layout 1); a
     * volume's two (1.0 Table 5-18, 2.0 Table 5-4), and its RANGE two and
     * six a subrange (2.0 Table 5-5). */
    length = selector == UAC_FU_MUTE ? 1u : 2u;

    if (d->version == 2) {
        switch (attribute) {
        case UAC_ATTR_CUR:
            request = UAC2_CUR;
            break;
        case UAC_ATTR_RANGE:
            request = UAC2_RANGE;
            length = (uint16_t)(2u + 6u * UAC_VOLUME_SUBRANGES_MAX);
            break;
        default:
            return false;
        }
    } else {
        switch (attribute) {
        case UAC_ATTR_CUR:
            request = set ? UAC1_SET_CUR : UAC1_GET_CUR;
            break;
        case UAC_ATTR_MIN:
            request = UAC1_GET_MIN;
            break;
        case UAC_ATTR_MAX:
            request = UAC1_GET_MAX;
            break;
        case UAC_ATTR_RES:
            request = UAC1_GET_RES;
            break;
        default:
            return false;
        }
    }

    out->request_type = set ? RT_SET : RT_GET;
    out->request = request;
    out->value = (uint16_t)(selector << 8 | channel);
    out->index = (uint16_t)(f->unit << 8 | d->control);
    out->length = length;
    return true;
}

unsigned uac_fu_channels(const struct uac_feature *f, unsigned selector,
                         uint8_t *channels, unsigned room)
{
    uint8_t place = selector == UAC_FU_VOLUME ? f->volume
                  : selector == UAC_FU_MUTE ? f->mute : UAC_CONTROL_NONE;
    uint32_t mask = selector == UAC_FU_VOLUME ? f->volume_channels
                                              : f->mute_channels;
    unsigned n = 0, ch;

    if (f->unit == 0 || place == UAC_CONTROL_NONE || room == 0) {
        return 0;
    }

    if (place == UAC_CONTROL_MASTER) {
        channels[0] = 0;
        return 1;
    }

    for (ch = 1; ch <= UAC_FU_CHANNELS_MAX && n < room; ch++) {
        if ((mask & (1u << (ch - 1))) != 0) {
            channels[n++] = (uint8_t)ch;
        }
    }

    return n;
}

void uac_volume_put(int16_t value, uint8_t out[2])
{
    uint16_t u = (uint16_t)value;

    out[0] = (uint8_t)u;
    out[1] = (uint8_t)(u >> 8);
}

int16_t uac_volume_get(const uint8_t in[2])
{
    uint16_t u = (uint16_t)(in[0] | (uint16_t)in[1] << 8);

    return (int16_t)u;          /* two's complement, as the device's */
}

/* A range made sound: MIN never 0x8000, RES at least 1. */
static bool range_from(int32_t min, int32_t max, int32_t res,
                       struct uac_volume_range *out)
{
    if (min == UAC_VOLUME_SILENCE) {
        min = UAC_VOLUME_LEAST;
    }

    if (min > max) {
        return false;
    }

    out->min = (int16_t)min;
    out->max = (int16_t)max;
    out->res = (int16_t)(res < 1 ? 1 : res);
    return true;
}

bool uac_volume_range_v1(int16_t min, int16_t max, int16_t res,
                         struct uac_volume_range *out)
{
    return range_from(min, max, res, out);
}

bool uac_volume_range_v2(const uint8_t *bytes, unsigned length,
                         struct uac_volume_range *out)
{
    struct uac_volume_range best = { 0, 0, 1 }, r;
    unsigned n, whole, i;
    int32_t widest = -1;

    if (length < 2) {
        return false;
    }

    n = get_le16(bytes);
    whole = (length - 2u) / 6u;
    n = n < whole ? n : whole;

    for (i = 0; i < n; i++) {
        const uint8_t *t = bytes + 2 + 6 * i;

        if (range_from(uac_volume_get(t), uac_volume_get(t + 2),
                       uac_volume_get(t + 4), &r)
            && (int32_t)r.max - r.min > widest) {
            widest = (int32_t)r.max - r.min;
            best = r;
        }
    }

    if (widest < 0) {
        return false;
    }

    *out = best;
    return true;
}

/*
 * **The curve**: DB_OF[level - 1] is 20 log10(level / 256) in 1/256 dB,
 * round(5120 x log10(level / 256)), for every level 1 to 256 - the
 * loudness the audio server's master gives a sample today, when it
 * multiplies by level / 256. Worked out on the host; `test_uacdecode.c`
 * holds every entry to the logarithm.
 */
static const int16_t DB_OF[256] = {
    -12330, -10789, -9887, -9248, -8751, -8346, -8003, -7706,
    -7444, -7210, -6998, -6805, -6627, -6462, -6309, -6165,
    -6030, -5903, -5783, -5669, -5560, -5457, -5358, -5264,
    -5173, -5086, -5002, -4921, -4843, -4767, -4694, -4624,
    -4555, -4489, -4425, -4362, -4301, -4242, -4184, -4128,
    -4073, -4019, -3967, -3916, -3866, -3817, -3769, -3722,
    -3676, -3631, -3587, -3544, -3502, -3460, -3420, -3379,
    -3340, -3301, -3263, -3226, -3189, -3153, -3118, -3083,
    -3048, -3014, -2981, -2948, -2915, -2883, -2852, -2821,
    -2790, -2760, -2730, -2700, -2671, -2643, -2614, -2586,
    -2559, -2531, -2505, -2478, -2452, -2426, -2400, -2374,
    -2349, -2324, -2300, -2276, -2252, -2228, -2204, -2181,
    -2158, -2135, -2113, -2090, -2068, -2046, -2024, -2003,
    -1982, -1961, -1940, -1919, -1899, -1878, -1858, -1838,
    -1818, -1799, -1779, -1760, -1741, -1722, -1703, -1685,
    -1666, -1648, -1630, -1612, -1594, -1576, -1559, -1541,
    -1524, -1507, -1490, -1473, -1456, -1439, -1423, -1406,
    -1390, -1374, -1358, -1342, -1326, -1310, -1295, -1279,
    -1264, -1249, -1234, -1218, -1203, -1189, -1174, -1159,
    -1145, -1130, -1116, -1101, -1087, -1073, -1059, -1045,
    -1031, -1017, -1004, -990, -977, -963, -950, -937,
    -923, -910, -897, -884, -871, -859, -846, -833,
    -821, -808, -796, -783, -771, -759, -746, -734,
    -722, -710, -698, -687, -675, -663, -651, -640,
    -628, -617, -605, -594, -583, -571, -560, -549,
    -538, -527, -516, -505, -494, -483, -472, -462,
    -451, -440, -430, -419, -409, -398, -388, -378,
    -368, -357, -347, -337, -327, -317, -307, -297,
    -287, -277, -267, -258, -248, -238, -228, -219,
    -209, -200, -190, -181, -171, -162, -153, -144,
    -134, -125, -116, -107, -98, -89, -80, -71,
    -62, -53, -44, -35, -26, -17, -9, 0,
};

int16_t uac_volume_unity(const struct uac_volume_range *r)
{
    return r->max < 0 ? r->max : r->min > 0 ? r->min : 0;
}

int16_t uac_volume_value(const struct uac_volume_range *r, int16_t top,
                         unsigned level)
{
    int32_t want, steps, v, ceiling;

    ceiling = top < r->max ? top : r->max;
    ceiling = ceiling < r->min ? r->min : ceiling;

    if (level >= 256u) {
        return (int16_t)ceiling;
    }

    if (level == 0) {
        return r->min;
    }

    want = (int32_t)top + DB_OF[level - 1];
    want = want < r->min ? r->min : want > ceiling ? ceiling : want;

    /* The nearest of MIN + k x RES, then down a step while above. */
    steps = (want - r->min + r->res / 2) / r->res;
    v = r->min + steps * r->res;

    while (v > ceiling && v - r->res >= r->min) {
        v -= r->res;
    }

    return (int16_t)v;
}

unsigned uac_volume_level(int16_t top, int16_t value)
{
    int32_t below, gap, best_gap = -1;
    unsigned level, best = 1;

    if (value == UAC_VOLUME_SILENCE) {
        return 0;
    }

    if (value >= top) {
        return 256;
    }

    below = (int32_t)value - top;

    for (level = 1; level <= 256u; level++) {
        gap = below - DB_OF[level - 1];
        gap = gap < 0 ? -gap : gap;

        if (best_gap < 0 || gap < best_gap) {
            best_gap = gap;
            best = level;
        }
    }

    return best;
}

void uac_volume_for(const struct uac_device *d, const struct uac_volume_range *r,
                    int16_t top, unsigned level, bool muted,
                    struct uac_volume_set *out)
{
    bool quiet = muted || level == 0;
    bool has_mute = d->volume.unit != 0 && d->volume.mute != UAC_CONTROL_NONE;

    out->mute = quiet && has_mute;
    out->value = uac_volume_value(r, top, level);
    out->silent = quiet && (has_mute || d->version == 2);

    if (quiet && !has_mute) {
        out->value = UAC_VOLUME_SILENCE;
    }
}
