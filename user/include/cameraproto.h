/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_CAMERAPROTO_H
#define KOSMOS_CAMERAPROTO_H

/*
 * **`/dev/camera`**: what a program says to the camera's driver, and the
 * region the pictures come back in (`roadmap.md` 6d, `usb.md` §11 8d).
 *
 * Control by message, data by shared memory (`CLAUDE.md`). A frame is the
 * canonical stream - it recurs because the camera says so, thirty times a
 * second - so no picture ever travels as a payload. The messages are three:
 * which cameras there are and what each can send; open one, at one size,
 * into a region the program made and hands over; and close it. The frames
 * are in that region.
 *
 * **A declared shape**, as `audioproto.h` is: a request cannot say what this
 * has no field for, and the driver never composes a sentence - an error is a
 * number, and the words belong to whoever shows it to a person.
 *
 * **The region is a triple buffer, not a queue.** A camera is a picture of
 * *now*: a window that falls behind wants the newest frame, never the one it
 * missed. So the driver writes whole frames into slots and publishes the
 * newest (`latest`, then `sequence`); the program says which slot it is
 * reading (`reading`) and re-checks `latest` before it trusts it; and the
 * driver never writes into the slot that is `latest` or `reading`. Three
 * slots is the least that always leaves the driver one to write.
 *
 * **A program that stops reading is gone.** It advances `taken` whenever it
 * takes a frame; a stream whose `taken` has not moved for
 * `CAMERA_LEASE_SECONDS` is closed by the driver, so a Camera window that
 * died without closing does not keep the camera "in use" for ever. A program
 * that wants the camera back opens it again.
 */

#include <stdint.h>

#define CAMERA_OP_LIST      1u  /* camera N: its name and its sizes */
#define CAMERA_OP_OPEN      2u  /* a region travels with this one */
#define CAMERA_OP_CLOSE     3u

/* Errors are numbers. The words belong to whoever shows them. */
#define CAMERA_OK           0u
#define CAMERA_ERR_NONE     1u  /* no camera with that number */
#define CAMERA_ERR_SIZE     2u  /* not a size it offers */
#define CAMERA_ERR_REGION   3u  /* no region, or one too small */
#define CAMERA_ERR_IN_USE   4u  /* another program has it */
#define CAMERA_ERR_STREAM   5u  /* the camera would not stream */
#define CAMERA_ERR_REQUEST  6u  /* not a request this understands */

/* What a picture's bytes are: `enum uvc_pixels`'s numbers. */
#define CAMERA_PIXELS_YUY2  1u
#define CAMERA_PIXELS_MJPEG 2u

#define CAMERA_NAME_MAX     40u
#define CAMERA_SIZES_MAX    48u

/* One size a camera can send. */
struct camera_size {
    uint16_t width;
    uint16_t height;
    uint8_t  pixels;            /* CAMERA_PIXELS_* */
    uint8_t  fps;               /* at its default interval */
    uint16_t reserved;
    uint32_t slot_bytes;        /* the most one frame of it takes */
};

struct camera_request {
    uint32_t op;
    uint32_t camera;            /* LIST, OPEN: which, from 0 */
    uint32_t size;              /* OPEN: an index into LIST's sizes */
    uint32_t handle;            /* CLOSE: what OPEN answered */
};

struct camera_reply {
    uint32_t error;             /* CAMERA_OK, or why not */
    uint32_t cameras;           /* LIST: how many there are */
    char     name[CAMERA_NAME_MAX];
    uint32_t sizes;             /* LIST: how many of `size` are filled */
    struct camera_size size[CAMERA_SIZES_MAX];

    /*
     * OPEN: the stream's handle, which CLOSE must quote - so a window
     * whose stream the driver already ended (its lease ran out, the camera
     * was pulled) cannot close another window's that opened since.
     */
    uint32_t handle;
};

_Static_assert(sizeof(struct camera_request) == 16,
               "the camera's request is sixteen bytes on both sides");
_Static_assert(sizeof(struct camera_size) == 12,
               "a camera size is twelve bytes on both sides");
_Static_assert(sizeof(struct camera_reply) <= 2048,
               "a camera reply must fit in one message");

/*
 * **A full fence, not `RING_BARRIER`** (`tcpring.h`). A ring's producer
 * publishes after its writes and its consumer reads after it looks, and on
 * x86 the order of stores and the order of loads come free, so a compiler
 * barrier is enough there. This handshake is the other kind: the program
 * *stores* `reading` and then *loads* `latest`, and the driver stores
 * `latest` and then loads `reading` - each needs its store seen before its
 * load, and x86 lets a later load pass an earlier store. `mfence` there, and
 * `dmb ish` on AArch64, which orders everything anyway.
 */
#if defined(__x86_64__)
#define CAMERA_FENCE()  __asm__ volatile("mfence" ::: "memory")
#else
#define CAMERA_FENCE()  __asm__ volatile("dmb ish" ::: "memory")
#endif

/*
 * **The region**: this header, then `slots` frames of `slot_bytes` each
 * from `CAMERA_RING_DATA`. The program makes it - `CAMERA_RING_DATA +
 * CAMERA_SLOTS * slot_bytes` at least, from LIST - and the driver fills the
 * header in when it opens.
 */
#define CAMERA_RING_MAGIC   0x4d41434bu     /* 'KCAM' */
#define CAMERA_SLOTS        3u
#define CAMERA_RING_DATA    4096u           /* frames start a page in */
#define CAMERA_NOT_READING  0xFFFFFFFFu
#define CAMERA_LEASE_SECONDS 3u

struct camera_ring {
    /* Written by the driver when it opens, then only read. */
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t pixels;            /* CAMERA_PIXELS_* */
    uint32_t slots;             /* CAMERA_SLOTS */
    uint32_t slot_bytes;
    uint32_t interval;          /* in 100 ns, what the camera agreed */
    uint32_t reserved;

    /* Written by the driver: the newest whole frame, then its number. */
    volatile uint32_t latest;
    volatile uint32_t sequence;     /* 0 until the first frame */
    volatile uint32_t stopped;      /* 1 once the driver has closed it */
    uint32_t reserved2;
    uint32_t length[CAMERA_SLOTS];  /* each slot's frame, in bytes */
    uint32_t reserved3;

    /* Written by the program. */
    volatile uint32_t reading;      /* a slot, or CAMERA_NOT_READING */
    volatile uint32_t taken;        /* the sequence last taken: the lease */
};

#endif /* KOSMOS_CAMERAPROTO_H */
