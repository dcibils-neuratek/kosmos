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

/* AudioControl's descriptors: what kind of device, its unit IDs. */
static void control(const uint8_t *d, unsigned len, uint8_t version,
                    struct uac_device *out)
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
            }
        }
        break;
    case AC_FEATURE_UNIT:
        if (len >= 4 && out->feature == 0) {
            out->feature = d[3];
        }
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
    unsigned at = 0, len;
    uint8_t class_code = 0, subclass = 0, protocol = 0;
    bool seen_control = false;
    const uint8_t *d;

    memset(out, 0, sizeof(*out));
    memset(&p, 0, sizeof(p));

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
                control(d, len, protocol == PROTOCOL_UAC2 ? 2 : 1, out);
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

int uac_pick(const struct uac_device *d, bool in, uint32_t mix_rate,
             uint32_t *rate)
{
    int best = -1;
    unsigned i, channels = 0, bits = 0;
    unsigned best_channels = 0, best_bits = 0;
    uint64_t score, best_score = 0;
    uint32_t r, best_rate = 0;
    const struct uac_alt *a;

    for (i = 0; i < d->nalts; i++) {
        a = &d->alts[i];

        if (a->in != in) {
            continue;
        }

        channels = a->channels == 2 ? 0 : a->channels > 2 ? 1 : 2;
        bits = a->bits >= 16 ? a->bits - 16u : 100u + (16u - a->bits);
        score = rate_for(a, mix_rate, &r);

        if (best < 0 || channels < best_channels
            || (channels == best_channels && bits < best_bits)
            || (channels == best_channels && bits == best_bits
                && score < best_score)) {
            best = (int)i;
            best_channels = channels;
            best_bits = bits;
            best_score = score;
            best_rate = r;
        }
    }

    if (rate != 0) {
        *rate = best < 0 ? 0 : best_rate;
    }

    return best;
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
