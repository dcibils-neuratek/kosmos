/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_USB_UVC_DECODE_H
#define KOSMOS_DRIVERS_USB_UVC_DECODE_H

/*
 * **A USB Video Class camera, as its bytes describe it** (`roadmap.md` 6d,
 * `usb.md` §11): which interfaces are its, what pictures it can send and at
 * what sizes, which alternate settings carry them, the block that negotiates
 * a stream, and the payloads a stream arrives in, put back together into
 * frames.
 *
 * Its own file with no hardware and no system calls in it, for the reason
 * `usb_decode.c` is: the bytes are the camera's to choose, and QEMU has no
 * camera of its own - the only descriptors a machine here ever sees come from
 * a real one. So `tools/test_uvcdecode.c` holds this to Diego's C920's own
 * 2,427 bytes, read from it on the Mac, and to the malformed ones no real
 * camera sends.
 *
 * The specification is USB Video Class 1.1 (and 1.5 where they differ):
 * 3.7 and 3.9 for the descriptors, 4.3.1.1 for probe and commit, 2.4.3.3
 * for the payload header.
 */

#include <stdbool.h>
#include <stdint.h>

/* Room for this many sizes, alternate settings, in one camera. The C920 has
 * 36 sizes over its two formats and 11 settings. More are counted, not kept. */
#define UVC_FRAMES_MAX  64
#define UVC_ALTS_MAX    32

/* What a picture's bytes are. */
enum uvc_pixels {
    UVC_PIXELS_OTHER = 0,       /* uncompressed, and not a layout read here */
    UVC_PIXELS_YUY2  = 1,       /* 4:2:2, Y0 U Y1 V: two pixels in four bytes */
    UVC_PIXELS_MJPEG = 2,       /* each frame a JPEG */
};

/* One size a camera can send, in one format. */
struct uvc_frame {
    uint8_t  format;            /* bFormatIndex: what the probe names */
    uint8_t  frame;             /* bFrameIndex: likewise */
    uint8_t  pixels;            /* enum uvc_pixels */
    uint16_t width;
    uint16_t height;
    uint32_t max_bytes;         /* dwMaxVideoFrameBufferSize: the largest frame */
    uint32_t interval;          /* dwDefaultFrameInterval, in 100 ns */
    uint32_t fastest;           /* the shortest interval it offers, in 100 ns */
};

/* One alternate setting of the streaming interface, and its endpoint. */
struct uvc_alt {
    uint8_t  alternate;         /* bAlternateSetting, for SET_INTERFACE */
    uint8_t  endpoint;          /* its number, 1 to 15; the direction is IN */
    uint16_t packet;            /* wMaxPacketSize 10:0 */
    uint8_t  extra;             /* wMaxPacketSize 12:11: more transactions */
    uint8_t  interval;          /* bInterval, as the device said it */
    uint32_t bytes;             /* what it carries an interval: packet x (1 + extra) */
};

struct uvc_camera {
    bool     ok;                /* a VideoControl and a VideoStreaming interface */
    uint8_t  configuration;     /* bConfigurationValue */
    uint8_t  control;           /* the VideoControl interface */
    uint8_t  streaming;         /* the first VideoStreaming interface */
    uint16_t uvc;               /* bcdUVC: 0x0100, 0x0110 or 0x0150 */

    /*
     * A camera streams on isochronous endpoints in its alternate settings,
     * or on a bulk endpoint in setting 0 (UVC 1.1 2.4.3); `bulk` says which,
     * and then `bulk_endpoint` and `bulk_packet` are it.
     */
    bool     bulk;
    uint8_t  bulk_endpoint;
    uint16_t bulk_packet;

    unsigned nframes;
    struct uvc_frame frames[UVC_FRAMES_MAX];
    unsigned nalts;
    struct uvc_alt alts[UVC_ALTS_MAX];

    unsigned frames_dropped;    /* sizes past UVC_FRAMES_MAX */
    unsigned alts_dropped;      /* settings past UVC_ALTS_MAX */
    bool     malformed;         /* a length that walks off the end, or zero */
};

/*
 * `bytes` is what GET_DESCRIPTOR returned for the configuration and `length`
 * how many of them arrived. Nothing outside those is read, and a descriptor
 * whose length is zero or runs past the end stops the walk with `malformed`
 * set and what came before it kept.
 */
void uvc_decode_config(const uint8_t *bytes, unsigned length,
                       struct uvc_camera *out);

/*
 * The size in `c->frames` closest to `width` by `height` in a format, or
 * -1: an exact match first, and otherwise the largest that fits inside it.
 * `pixels` 0 accepts either format this reads, YUY2 before MJPEG.
 */
int uvc_find_frame(const struct uvc_camera *c, unsigned pixels,
                   unsigned width, unsigned height);

/*
 * **The Video Probe and Commit Controls** (UVC 1.1 4.3.1.1), the block a
 * host and a camera pass back and forth to agree a stream: the host says
 * what it wants, the camera answers with what it will do, and the host
 * commits the answer. 26 bytes in UVC 1.0, 34 in 1.1 and 48 in 1.5 - a
 * camera refuses a block of the wrong length, which is why its version
 * decides this.
 */
struct uvc_probe {
    uint16_t hint;              /* bmHint: bit 0, keep the frame interval */
    uint8_t  format;            /* bFormatIndex */
    uint8_t  frame;             /* bFrameIndex */
    uint32_t interval;          /* dwFrameInterval, in 100 ns */
    uint32_t max_frame;         /* dwMaxVideoFrameSize: the camera's answer */
    uint32_t max_payload;       /* dwMaxPayloadTransferSize: likewise */
    uint32_t clock;             /* dwClockFrequency, 1.1 on */
};

#define UVC_PROBE_MAX 48

/* How long the block is for a camera of this bcdUVC. */
unsigned uvc_probe_length(uint16_t uvc);

/* Into `out`, `length` bytes of it, zero where the struct says nothing. */
void uvc_encode_probe(const struct uvc_probe *p, uint8_t *out,
                      unsigned length);

/* False, and `p` untouched, for fewer than the 26 bytes UVC 1.0 has. */
bool uvc_decode_probe(const uint8_t *bytes, unsigned length,
                      struct uvc_probe *p);

/*
 * Which of `c->alts` to stream `payload` bytes an interval on - its index
 * there, and `.alternate` is what SET_INTERFACE is told: the one that
 * carries least of those that carry enough, because bandwidth is the bus's
 * and a camera that takes all of it leaves nothing for the mouse. -1 when
 * none carries enough, or the camera streams on bulk.
 */
int uvc_pick_alternate(const struct uvc_camera *c, uint32_t payload);

/*
 * **Frames out of payloads** (UVC 1.1 2.4.3.3).
 *
 * Every payload starts with a header: its length, then a byte of flags - bit
 * 0 the frame's identity (FID), which toggles from one frame to the next;
 * bit 1 the end of a frame (EOF); bit 6 an error (ERR). The picture's bytes
 * follow. A frame is whole at EOF, or - for a camera that does not send EOF
 * - when FID toggles, which is also how a lost EOF is noticed.
 *
 * The caller owns the buffer; this only says where the frame ends and
 * whether it is worth keeping. `expect` is the whole frame's size when the
 * format fixes it (YUY2), and 0 when it does not (MJPEG): a YUY2 frame of
 * any other size lost a payload on the way and is dropped, not shown with a
 * tear in it.
 */
struct uvc_assembly {
    uint8_t *frame;             /* where the picture's bytes go */
    uint32_t capacity;          /* how many fit */
    uint32_t expect;            /* a whole frame's size, or 0 */
    uint32_t length;            /* how many the frame has so far */
    int      fid;               /* the frame's FID, or -1 before the first */
    bool     broken;            /* an error, or more than `capacity` */

    /*
     * A stream is joined in the middle of a frame, so nothing is kept until
     * the first boundary - an EOF, or FID changing - has gone past. Without
     * this the first frame is the tail of one, which a YUY2 size check
     * catches and an MJPEG one would hand to the decoder as a picture.
     */
    bool     synced;

    uint32_t whole;             /* frames that came out whole */
    uint32_t dropped;           /* frames given up on */
};

enum uvc_step {
    UVC_TAKEN,      /* the payload is in the frame being put together */
    UVC_WHOLE,      /* and with it the frame is whole: `length` bytes */
    UVC_BEFORE,     /* the frame so far is whole, and this payload begins the
                       next: take the frame, `uvc_next`, then hand this same
                       payload back */
    UVC_SKIPPED,    /* nothing in it, or not a payload */
};

void uvc_assembly_init(struct uvc_assembly *a, uint8_t *frame,
                       uint32_t capacity, uint32_t expect);

enum uvc_step uvc_payload(struct uvc_assembly *a, const uint8_t *bytes,
                          unsigned length);

/* After a whole frame has been taken: an empty one, with no identity yet. */
void uvc_next(struct uvc_assembly *a);

#endif /* KOSMOS_DRIVERS_USB_UVC_DECODE_H */
