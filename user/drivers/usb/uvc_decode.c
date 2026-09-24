/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A USB Video Class camera's bytes, decoded. `uvc_decode.h` says what and
 * why; the section numbers are USB Video Class 1.1's.
 */

#include "uvc_decode.h"

#include <string.h>

/* Descriptor types (USB 2.0 9.4, UVC 1.1 A.4). */
#define DESC_CONFIGURATION  0x02
#define DESC_INTERFACE      0x04
#define DESC_ENDPOINT       0x05
#define DESC_CS_INTERFACE   0x24

/* The video class and its two interface subclasses (UVC 1.1 A.1, A.2). */
#define CLASS_VIDEO         0x0E
#define SUBCLASS_CONTROL    0x01
#define SUBCLASS_STREAMING  0x02

/* Class-specific descriptor subtypes (UVC 1.1 A.5, A.6). */
#define VC_HEADER               0x01
#define VS_FORMAT_UNCOMPRESSED  0x04
#define VS_FRAME_UNCOMPRESSED   0x05
#define VS_FORMAT_MJPEG         0x06
#define VS_FRAME_MJPEG          0x07

/*
 * YUY2's GUID, 32595559-0000-0010-8000-00AA00389B71, as it sits in the
 * descriptor - the first three fields little-endian, which puts the four
 * letters in reading order (UVC Payload Uncompressed 1.1, 2.2).
 */
static const uint8_t YUY2[16] = {
    'Y', 'U', 'Y', '2', 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/*
 * A frame descriptor, uncompressed or MJPEG - the two lay out the same as
 * far as this reads (UVC 1.1 Payload Uncompressed 3.1.2, Payload MJPEG
 * 3.1.2): the index at 3, the size at 5 and 7, the largest frame at 17, the
 * default interval at 21, and at 25 how the intervals are given - zero for a
 * continuous range from 26, or a count of discrete ones from 26.
 */
static void add_frame(struct uvc_camera *out, const uint8_t *d,
                      unsigned length, uint8_t format, uint8_t pixels)
{
    struct uvc_frame *f;
    unsigned kinds, k;

    if (length < 26) {
        return;
    }

    if (out->nframes >= UVC_FRAMES_MAX) {
        out->frames_dropped++;
        return;
    }

    f = &out->frames[out->nframes++];
    f->format    = format;
    f->frame     = d[3];
    f->pixels    = pixels;
    f->width     = le16(d + 5);
    f->height    = le16(d + 7);
    f->max_bytes = le32(d + 17);
    f->interval  = le32(d + 21);
    f->fastest   = f->interval;

    kinds = d[25];

    if (kinds == 0) {
        /* Continuous: minimum, maximum, step. The minimum is the fastest. */
        if (length >= 38 && le32(d + 26) != 0) {
            f->fastest = le32(d + 26);
        }
        return;
    }

    for (k = 0; k < kinds && 26 + 4 * k + 4 <= length; k++) {
        uint32_t t = le32(d + 26 + 4 * k);

        if (t != 0 && t < f->fastest) {
            f->fastest = t;
        }
    }
}

void uvc_decode_config(const uint8_t *bytes, unsigned length,
                       struct uvc_camera *out)
{
    unsigned at = 0;

    /* What the walk is inside: which interface, which setting of it. */
    bool have_control = false, have_streaming = false;
    bool in_control = false, in_streaming = false;
    uint8_t alternate = 0;

    /* The format the frame descriptors that follow belong to. */
    uint8_t format = 0, pixels = UVC_PIXELS_OTHER;
    bool in_format = false;

    memset(out, 0, sizeof *out);

    if (bytes == NULL) {
        out->malformed = true;
        return;
    }

    while (at + 2 <= length) {
        const uint8_t *d = bytes + at;
        unsigned len = d[0];

        if (len < 2 || len > length - at) {
            out->malformed = true;
            break;
        }

        switch (d[1]) {
        case DESC_CONFIGURATION:
            if (len >= 9) {
                out->configuration = d[5];
            }
            break;

        case DESC_INTERFACE:
            if (len < 9) {
                in_control = in_streaming = false;
                break;
            }

            in_control = in_streaming = false;

            if (d[5] == CLASS_VIDEO && d[6] == SUBCLASS_CONTROL
                && (!have_control || d[2] == out->control)) {
                have_control = true;
                out->control = d[2];
                in_control = true;
            } else if (d[5] == CLASS_VIDEO && d[6] == SUBCLASS_STREAMING
                       && (!have_streaming || d[2] == out->streaming)) {
                /* The first streaming interface only: a camera with two
                 * (a still-image one, say) streams its video on the first. */
                have_streaming = true;
                out->streaming = d[2];
                in_streaming = true;
                alternate = d[3];
            }
            break;

        case DESC_CS_INTERFACE:
            if (len < 3) {
                break;
            }

            if (in_control && d[2] == VC_HEADER && len >= 5) {
                out->uvc = le16(d + 3);
            } else if (in_streaming) {
                switch (d[2]) {
                case VS_FORMAT_UNCOMPRESSED:
                    in_format = (len >= 21);
                    if (in_format) {
                        format = d[3];
                        pixels = memcmp(d + 5, YUY2, 16) == 0
                               ? UVC_PIXELS_YUY2 : UVC_PIXELS_OTHER;
                    }
                    break;

                case VS_FORMAT_MJPEG:
                    in_format = (len >= 4);
                    if (in_format) {
                        format = d[3];
                        pixels = UVC_PIXELS_MJPEG;
                    }
                    break;

                case VS_FRAME_UNCOMPRESSED:
                case VS_FRAME_MJPEG:
                    if (in_format) {
                        add_frame(out, d, len, format, pixels);
                    }
                    break;

                default:
                    /*
                     * A format this does not read - frame-based H.264, say,
                     * whose frame descriptors are laid out differently - so
                     * the frames after it are not taken for the last one's.
                     */
                    if (d[2] >= VS_FORMAT_UNCOMPRESSED) {
                        in_format = false;
                    }
                    break;
                }
            }
            break;

        case DESC_ENDPOINT:
            if (in_streaming && len >= 7 && (d[2] & 0x80) != 0) {
                uint16_t mps = le16(d + 4);
                unsigned type = d[3] & 3;

                if (type == 1) {        /* isochronous */
                    struct uvc_alt *a;

                    if (out->nalts >= UVC_ALTS_MAX) {
                        out->alts_dropped++;
                        break;
                    }

                    a = &out->alts[out->nalts++];
                    a->alternate = alternate;
                    a->endpoint  = d[2] & 0x0f;
                    a->packet    = mps & 0x7ff;
                    a->extra     = (mps >> 11) & 3;
                    a->interval  = d[6];
                    a->bytes     = (uint32_t)a->packet * (1u + a->extra);
                } else if (type == 2 && alternate == 0) {   /* bulk */
                    out->bulk = true;
                    out->bulk_endpoint = d[2] & 0x0f;
                    out->bulk_packet = mps & 0x7ff;
                }
            }
            break;

        default:
            break;
        }

        at += len;
    }

    /* A byte left over is a descriptor cut short before its type. */
    if (at < length && !out->malformed) {
        out->malformed = true;
    }

    out->ok = have_control && have_streaming;
}

/* Whether a size is in the format asked for; 0 is either one this reads. */
static bool wanted(unsigned pixels, const struct uvc_frame *f)
{
    if (pixels == 0) {
        return f->pixels == UVC_PIXELS_YUY2 || f->pixels == UVC_PIXELS_MJPEG;
    }

    return f->pixels == pixels;
}

/* Of two sizes alike in every other way, YUY2's: no decoder between the
 * camera and the screen. */
static bool preferred(const struct uvc_frame *a, const struct uvc_frame *b)
{
    return a->pixels == UVC_PIXELS_YUY2 && b->pixels != UVC_PIXELS_YUY2;
}

int uvc_find_frame(const struct uvc_camera *c, unsigned pixels,
                   unsigned width, unsigned height)
{
    unsigned long area = 0;
    int best = -1;
    unsigned i, n = c->nframes < UVC_FRAMES_MAX ? c->nframes : UVC_FRAMES_MAX;

    for (i = 0; i < n; i++) {
        const struct uvc_frame *f = &c->frames[i];

        if (wanted(pixels, f) && f->width == width && f->height == height
            && (best < 0 || preferred(f, &c->frames[best]))) {
            best = (int)i;
        }
    }

    if (best >= 0) {
        return best;
    }

    for (i = 0; i < n; i++) {
        const struct uvc_frame *f = &c->frames[i];
        unsigned long a = (unsigned long)f->width * f->height;

        if (!wanted(pixels, f) || f->width > width || f->height > height) {
            continue;
        }

        if (a > area || (a == area && best >= 0
                         && preferred(f, &c->frames[best]))) {
            area = a;
            best = (int)i;
        }
    }

    return best;
}

unsigned uvc_probe_length(uint16_t uvc)
{
    if (uvc >= 0x0150) {
        return 48;
    }

    if (uvc >= 0x0110) {
        return 34;
    }

    return 26;
}

/*
 * The fields at their offsets (UVC 1.1 Table 4-47): bmHint at 0, the
 * format and frame at 2 and 3, the interval at 4, the largest frame at 18,
 * the largest payload at 22 - where UVC 1.0 ends - and the clock at 26.
 */
void uvc_encode_probe(const struct uvc_probe *p, uint8_t *out,
                      unsigned length)
{
    if (length > UVC_PROBE_MAX) {
        length = UVC_PROBE_MAX;
    }

    memset(out, 0, length);

    if (length < 26) {
        return;
    }

    put16(out + 0, p->hint);
    out[2] = p->format;
    out[3] = p->frame;
    put32(out + 4, p->interval);
    put32(out + 18, p->max_frame);
    put32(out + 22, p->max_payload);

    if (length >= 30) {
        put32(out + 26, p->clock);
    }
}

bool uvc_decode_probe(const uint8_t *bytes, unsigned length,
                      struct uvc_probe *p)
{
    if (bytes == NULL || length < 26) {
        return false;
    }

    p->hint        = le16(bytes + 0);
    p->format      = bytes[2];
    p->frame       = bytes[3];
    p->interval    = le32(bytes + 4);
    p->max_frame   = le32(bytes + 18);
    p->max_payload = le32(bytes + 22);
    p->clock       = length >= 30 ? le32(bytes + 26) : 0;

    return true;
}

int uvc_pick_alternate(const struct uvc_camera *c, uint32_t payload)
{
    int best = -1;
    unsigned i;

    if (c->bulk) {
        return -1;
    }

    for (i = 0; i < c->nalts && i < UVC_ALTS_MAX; i++) {
        const struct uvc_alt *a = &c->alts[i];

        if (a->bytes >= payload
            && (best < 0 || a->bytes < c->alts[best].bytes)) {
            best = (int)i;
        }
    }

    return best;
}

void uvc_assembly_init(struct uvc_assembly *a, uint8_t *frame,
                       uint32_t capacity, uint32_t expect)
{
    memset(a, 0, sizeof *a);
    a->frame = frame;
    a->capacity = capacity;
    a->expect = expect;
    a->fid = -1;
}

void uvc_next(struct uvc_assembly *a)
{
    a->length = 0;
    a->broken = false;
    a->fid = -1;
}

/* A frame that has ended: whether it is one to keep. */
static bool keepable(const struct uvc_assembly *a)
{
    return a->length > 0 && !a->broken
        && (a->expect == 0 || a->length == a->expect);
}

/* A frame that has ended and is not kept. */
static void give_up(struct uvc_assembly *a)
{
    if (a->length > 0 || a->broken) {
        a->dropped++;
    }

    a->length = 0;
    a->broken = false;
}

enum uvc_step uvc_payload(struct uvc_assembly *a, const uint8_t *bytes,
                          unsigned length)
{
    unsigned header, data;
    uint8_t flags;
    int fid;

    if (bytes == NULL || length < 2) {
        return UVC_SKIPPED;             /* an interval with nothing in it */
    }

    header = bytes[0];
    flags = bytes[1];

    /* A header longer than the payload, or shorter than itself: something
     * other than a payload, and the frame it would have been part of has a
     * hole in it now. */
    if (header < 2 || header > length) {
        if (a->length > 0) {
            a->broken = true;
        }
        return UVC_SKIPPED;
    }

    fid = flags & 1;

    /* Joined mid-frame: wait for a boundary before keeping anything. */
    if (!a->synced) {
        if (a->fid >= 0 && fid != a->fid) {
            a->synced = true;           /* and this payload starts a frame */
        } else {
            a->fid = (flags & 2) ? -1 : fid;
            a->synced = (flags & 2) != 0;
            return UVC_SKIPPED;
        }
    }

    /* FID changed with a frame in hand: that frame ended without its EOF. */
    if (a->fid >= 0 && fid != a->fid) {
        if (keepable(a)) {
            a->whole++;
            return UVC_BEFORE;
        }

        give_up(a);
    }

    a->fid = fid;

    if (flags & 0x40) {
        a->broken = true;               /* ERR: the camera says so itself */
    }

    data = length - header;

    if (data > 0) {
        if (data > a->capacity - a->length) {
            a->broken = true;           /* more than fits: not a frame of ours */
        } else if (!a->broken) {
            memcpy(a->frame + a->length, bytes + header, data);
            a->length += data;
        }
    }

    if (flags & 2) {                    /* EOF */
        if (keepable(a)) {
            a->whole++;
            return UVC_WHOLE;
        }

        give_up(a);
        a->fid = -1;
    }

    return UVC_TAKEN;
}
