/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The window server (`docs/astra-display.md` D3b-2): the process that holds
 * the screen and composes the desktop, in C.
 *
 * **Started by init at boot**, as every C server is (Diego, 10 October:
 * "Let's go with option a"), with the screen and the console's endpoint, and
 * it outlives a window manager that ends or crashes. The window manager - and
 * only the system's, which init lends this door to - describes the desktop;
 * this composes it with the Graphics Kit's own code (`draw.c`,
 * `scene_core.c`) and decides nothing a person would choose.
 *
 * What it is handed, each once, as a region (`windowproto.h`): the back
 * buffer, composed into; the region the scene is written in, every pass;
 * and every picture the scene names - a window's buffers, the window
 * manager's own wallpaper, stamp, menu bars, volume display, pointer and a
 * drag's label - by a slot the window manager chose. `WS_OP_COMPOSE` then
 * reads the scene, composes each damaged rectangle into the back buffer and
 * puts it on the screen.
 *
 * **The window manager is not trusted to be right**, only to be the one
 * holding the door: every count in the scene is held to what its region
 * holds, every slot to the pictures there are, every picture to the region
 * it came in, and a source rectangle to its picture - so a scene written
 * wrong composes wrong, and nothing here reads or writes past what it was
 * given.
 *
 * **It holds the screen for as long as the window manager is there**: it
 * takes it from the console when handed the back buffer, and gives it
 * back when the endpoint the manager greeted with names nothing - the
 * manager ended, or crashed - letting go of everything it was handed.
 *
 * Built in steps, the desktop working after each (`docs/astra-display.md`):
 * the door and a greeting; composing; the console's screen handed over;
 * then the window manager's own composing gone.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "gpuproto.h"
#include "windowproto.h"
#include "init/say.h"
#include "kits/gfx/surface.h"
#include "kits/gfx/draw_primitives.h"
#include "kits/gfx/scene_core.h"

static long console = -1;
static bool have_screen;
static bool screen_taken;           /* from the console, for the manager */

/* The window manager's own endpoint, handed over with its greeting: while
 * it names something the manager is there, and once it does not - the
 * manager ended or crashed - the screen goes back to the console. */
static long manager = -1;
static struct screen_info screen;
static struct surface screen_surface;

/* A region handed over: its capability, where it is mapped, how long. */
struct held {
    long cap;
    uintptr_t at;
    size_t bytes;
};

static struct held back_region = { -1, 0, 0 };
static struct surface back;
static struct held scene_region = { -1, 0, 0 };

/* The pictures, by slot: each its region and a surface over it. */
struct picture {
    struct held region;
    struct surface surface;
    uint32_t gpu_id;                /* its buffer id in the GPU's table, 0 not yet, ~0 never (6b) */
};

static struct picture *pictures;
static unsigned npictures;
static uint32_t nheld;              /* of them, holding a region */

static struct scene *sc;


/* A region that came with a message, mapped; false when none came or it
 * would not map, and then nothing is held. */
static bool take(long cap, struct held *out)
{
    long at, pages;

    if (cap < 0) {
        return false;
    }

    pages = kosmos_mem_size(cap);
    at = pages > 0 ? kosmos_mem_map(cap) : -1;

    if (at < 0) {
        (void)kosmos_cap_drop(cap);
        return false;
    }

    out->cap = cap;
    out->at = (uintptr_t)at;
    out->bytes = (size_t)pages * KOSMOS_PAGE_SIZE;
    return true;
}

/* A region let go: out of the share window, and its capability. */
static void let_go(struct held *h)
{
    if (h->cap >= 0) {
        (void)kosmos_share_unmap(h->at, (h->bytes + KOSMOS_PAGE_SIZE - 1) / KOSMOS_PAGE_SIZE);
        (void)kosmos_cap_drop(h->cap);
    }

    h->cap = -1;
    h->at = 0;
    h->bytes = 0;
}

/*
 * Whether `width` by `height` at `pitch`, from `offset`, lies inside a
 * region of `bytes` - with every product checked before it is trusted,
 * since each number came from another process.
 */
static bool fits(uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch,
                 size_t bytes)
{
    uint64_t rows;

    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        return false;
    }

    if ((uint64_t)pitch < (uint64_t)width * 4u || pitch % 4u != 0) {
        return false;
    }

    rows = (uint64_t)pitch * height;
    return offset <= bytes && rows <= (uint64_t)bytes - offset;
}

static void surface_over(struct surface *s, uintptr_t at, uint32_t width, uint32_t height,
                         uint32_t pitch)
{
    memset(s, 0, sizeof(*s));
    s->pixels = (uint32_t *)at;
    s->width = width;
    s->height = height;
    s->pitch = pitch;
}

static void picture_gpu_forget(unsigned slot);

static void forget(unsigned slot)
{
    if (slot < npictures) {
        picture_gpu_forget(slot);

        if (pictures[slot].region.cap >= 0) {
            nheld--;
        }

        let_go(&pictures[slot].region);
        memset(&pictures[slot].surface, 0, sizeof(pictures[slot].surface));
    }
}

/* Room for slot `slot`, the table doubling; false when there is no memory. */
static bool room_for(unsigned slot)
{
    unsigned want = npictures ? npictures : 16, k;
    struct picture *grown;

    if (slot < npictures) {
        return true;
    }

    while (want <= slot) {
        want *= 2;
    }

    grown = realloc(pictures, (size_t)want * sizeof(*grown));

    if (grown == NULL) {
        return false;
    }

    for (k = npictures; k < want; k++) {
        memset(&grown[k], 0, sizeof(grown[k]));
        grown[k].region.cap = -1;
    }

    pictures = grown;
    npictures = want;
    return true;
}

/* The picture in `slot`, or NULL: none, or one never handed over. */
static struct surface *picture(int32_t slot)
{
    if (slot < 0 || (uint32_t)slot >= npictures || pictures[slot].region.cap < 0) {
        return NULL;
    }

    return &pictures[slot].surface;
}

static long clamp(long v, long most)
{
    return v < 0 ? 0 : (v > most ? most : v);
}

/*
 * The scene in its region, read into `sc`: false when it says more than the
 * region holds. Each field is read once, into the scene, so what composes
 * is what was checked even if the region changes underneath.
 */
static bool read_scene(const struct ws_rect **rects, uint32_t *nrect)
{
    const struct ws_scene_head *head = (const struct ws_scene_head *)scene_region.at;
    struct ws_scene_head h;
    const struct ws_scene_window *win;
    const struct ws_scene_menu *menu;
    uint64_t need;
    int k;

    if (scene_region.bytes < sizeof(h)) {
        return false;
    }

    memcpy(&h, head, sizeof(h));

    if (h.magic != WS_SCENE_MAGIC || h.nwin > 65536u || h.nmenu > 65536u || h.nrect > 65536u) {
        return false;
    }

    need = sizeof(h) + (uint64_t)h.nwin * sizeof(*win) + (uint64_t)h.nmenu * sizeof(*menu)
           + (uint64_t)h.nrect * sizeof(struct ws_rect);

    if (need > scene_region.bytes) {
        return false;
    }

    if (!scene_set_windows(sc, (int)h.nwin) || !scene_set_menus(sc, (int)h.nmenu)) {
        return false;
    }

    sc->corner = h.corner;
    sc->shadow = h.shadow;
    sc->grip = h.grip;
    sc->edge_light = h.edge_light;
    sc->edge_dark = h.edge_dark;
    sc->box = h.box;
    sc->box_w = h.box_w;
    sc->run = h.run;
    sc->d = h.d;
    sc->track = h.track;
    sc->line_soft = h.line_soft;
    sc->outline = h.outline;

    for (k = 0; k < SC_SLOTS; k++) {
        sc->fill[k] = h.fill[k];
        sc->ring[k] = h.ring[k];
        sc->mark[k] = h.mark[k];
        sc->held_fill[k] = h.held_fill[k];
    }

    sc->desktop = h.desktop;
    sc->wallpaper = picture(h.wallpaper);
    sc->wx = h.wx;
    sc->wy = h.wy;
    sc->ww = h.ww;
    sc->wh = h.wh;
    sc->stamp = picture(h.stamp);
    sc->sx = h.sx;
    sc->sy = h.sy;

    sc->outline_on = h.outline_on != 0;
    sc->ox = h.ox;
    sc->oy = h.oy;
    sc->ow = h.ow;
    sc->oh = h.oh;
    sc->outline_colour = h.outline_colour;
    sc->osd = picture(h.osd);
    sc->osd_x = h.osd_x;
    sc->osd_y = h.osd_y;
    sc->osd_alpha = h.osd_alpha > 255u ? 255u : h.osd_alpha;
    sc->cursor = picture(h.cursor);
    sc->cursor_x = h.cursor_x;
    sc->cursor_y = h.cursor_y;
    sc->badge = picture(h.badge);
    sc->badge_x = h.badge_x;
    sc->badge_y = h.badge_y;
    sc->panel = picture(h.panel);
    sc->panel_x = h.panel_x;
    sc->panel_y = h.panel_y;

    win = (const struct ws_scene_window *)(head + 1);
    menu = (const struct ws_scene_menu *)(win + h.nwin);

    for (k = 0; k < (int)h.nwin; k++) {
        struct ws_scene_window in;
        struct sc_window *w = &sc->win[k];

        memcpy(&in, &win[k], sizeof(in));
        memset(w, 0, sizeof(*w));
        w->x = in.x;
        w->y = in.y;
        w->w = in.w < 0 ? 0 : in.w;
        w->h = in.h < 0 ? 0 : in.h;
        w->flags = in.flags;
        w->picture = picture(in.picture);
        w->bar = picture(in.bar);
        w->bar_h = clamp(in.bar_h, w->bar ? (long)w->bar->height : 0);
        w->bx = in.bx;
        w->by = in.by;
        w->held = in.held >= 0 && in.held < SC_SLOTS ? in.held : -1;
        w->picture_slot = in.picture;
        w->bar_slot = in.bar;

        /* What is stretched from is never more than the picture holds. */
        w->src_w = w->picture ? clamp(in.src_w, (long)w->picture->width) : 0;
        w->src_h = w->picture ? clamp(in.src_h, (long)w->picture->height) : 0;
    }

    for (k = 0; k < (int)h.nmenu; k++) {
        struct ws_scene_menu in;
        struct sc_menu *m = &sc->menu[k];

        memcpy(&in, &menu[k], sizeof(in));
        m->x = in.x;
        m->y = in.y;
        m->w = in.w < 0 ? 0 : in.w;
        m->h = in.h < 0 ? 0 : in.h;
        m->picture = picture(in.picture);
        m->picture_slot = in.picture;
    }

    *rects = (const struct ws_rect *)(menu + h.nmenu);
    *nrect = h.nrect;
    return true;
}

/* The scene composed into the back buffer, and with `show`, the screen. */
/*--------------------------------------------------------------------------
 * **Page flips** (`docs/m700-2d.md` step 4b), where the GPU's driver has
 * buffers to give: the screen is two buffers, the firmware's (buffer 0)
 * and one the driver made, and the display shows one while the other is
 * brought up to date from the back buffer and then flipped to, at the
 * display's next vertical blank. No frame is seen half drawn.
 *
 * The buffer not shown is two frames old when its turn comes: it lacks
 * what was copied into the other one last time (`given`) and what has
 * been composed since (`owed`), so both are copied - the back buffer
 * always holds the whole frame. A flip is answered at once, and the buffer
 * it left is not touched until the driver says the display has left it;
 * until then what is composed is owed, and the receive loop looks again a
 * tick later. Where there is no such driver - QEMU, any machine without an
 * Intel GPU - the back buffer is copied to the screen as before.
 *------------------------------------------------------------------------*/

#define DAMAGE_MAX 64u

struct damage {
    unsigned n;
    bool whole;                     /* more than fit: the whole screen */
    struct { long x, y, w, h; } r[DAMAGE_MAX];
};

static long gpu = -1;               /* the GPU's driver's door, if init gave one */
static bool flipping;
static struct held flip_region = { -1, 0, 0 };
static struct surface buffers[2];   /* 0: the firmware's; 1: the driver's */
static uint32_t buffer_id[2];
static unsigned shown;              /* which of the two the display shows */
static bool flip_pending;           /* a flip asked for, not yet seen taken */
static struct damage owed, given;
static unsigned long copy_ticks;    /* since the last reply, for `copy_us` */

/*
 * **The copy on the blitter** (`m700-2d.md` step 6): the back buffer
 * handed to the GPU once (`back_id`), and the copy into the hidden screen
 * given to the blit engine rather than done here. A frame is then three
 * stages, each looked at a tick later when it is not done: the blit given,
 * its fence reached, the flip taken. Anything the driver refuses but
 * "busy" puts the copying back here, said once.
 */
static bool blitting;               /* the copy is the blitter's */
static bool blit_composing;         /* and composing's opaque parts (6b) */
static uint32_t back_id;
static bool blit_pending;           /* given, its fence not yet reached */
static uint32_t blit_fence;
static unsigned blit_target;        /* the buffer it copies into */
static struct damage sent;          /* what that blit copies: `given` once it is shown */
static unsigned checks_left = 20;   /* frames compared pixel by pixel, from the first */
static uint64_t checked_px, wrong_px;

static void damage_add(struct damage *d, long x, long y, long w, long h)
{
    if (d->whole) {
        return;
    }

    if (d->n == DAMAGE_MAX) {
        d->whole = true;
        return;
    }

    d->r[d->n].x = x;
    d->r[d->n].y = y;
    d->r[d->n].w = w;
    d->r[d->n].h = h;
    d->n++;
}

/* One request to the GPU's driver, of any shape, a capability beside it
 * when `pass` is one; the reply's own beside it into `cap`. */
static uint32_t gpu_send(const void *req, size_t bytes, long pass, struct gpu_reply *rep, long *cap)
{
    static struct message msg, got;

    if (gpu < 0) {
        return GPU_ERR_NO_DEVICE;
    }

    memset(&msg, 0, sizeof msg);
    msg.length = (uint32_t)bytes;
    msg.cap_plus_one = pass >= 0 ? (uint32_t)pass + 1u : 0u;
    memcpy(msg.data, req, bytes);

    if (kosmos_call(gpu, &msg, &got) != 0 || got.length < sizeof *rep) {
        return GPU_ERR_NO_DEVICE;
    }

    memcpy(rep, got.data, sizeof *rep);

    if (cap != NULL) {
        *cap = got.cap_plus_one > 0 ? (long)got.cap_plus_one - 1 : -1;
    } else if (got.cap_plus_one > 0) {
        (void)kosmos_cap_drop((long)got.cap_plus_one - 1);
    }

    return rep->error;
}

static uint32_t gpu_ask(uint32_t op, int32_t x, int32_t y, struct gpu_reply *rep, long *cap)
{
    struct gpu_request rq;

    memset(&rq, 0, sizeof rq);
    rq.op = op;
    rq.x = x;
    rq.y = y;
    return gpu_send(&rq, sizeof rq, -1, rep, cap);
}

/* A second screen from the GPU's driver, once the screen is taken. */
static void flips_start(void)
{
    struct gpu_reply rep;
    struct say_line line;
    long cap = -1;

    if (flipping || gpu < 0 || !have_screen) {
        return;
    }

    /* The second screen is made once and kept: a window manager started
     * again flips with the same one rather than leaving another 4.6 MB a
     * million pixels behind each time. */
    if (flip_region.cap < 0) {
        uint32_t error = gpu_ask(GPU_OP_BUFFER_NEW, (int32_t)screen.width, (int32_t)screen.height, &rep, &cap);

        if (error != GPU_OK) {
            if (cap >= 0) {
                (void)kosmos_cap_drop(cap);
            }

            /* No such driver here - QEMU - is the usual answer, and quiet:
             * copying, as before. Anything else is said. */
            if (error != GPU_ERR_NO_DEVICE) {
                say_begin(&line);
                say_text(&line, "windowserver: no page flips, the GPU's driver said ");
                say_dec(&line, error);
                say_send(console, &line);
            }

            return;
        }

        if (!take(cap, &flip_region)
            || flip_region.bytes < (size_t)rep.buffer.pitch * screen.height) {
            let_go(&flip_region);
            return;
        }

        buffer_id[1] = rep.buffer.id;
        surface_over(&buffers[1], flip_region.at, screen.width, screen.height, rep.buffer.pitch);
    }

    buffer_id[0] = 0;
    buffers[0] = screen_surface;
    shown = 0;
    flip_pending = false;
    owed.n = given.n = 0;
    owed.whole = false;
    given.whole = true;             /* the new buffer holds nothing yet */
    flipping = true;

    /* The back buffer handed to the GPU, so the blitter can copy from it. */
    {
        struct gpu_request rq;
        struct gpu_reply got;

        memset(&rq, 0, sizeof rq);
        rq.op = GPU_OP_BUFFER_IMPORT;
        rq.x = (int32_t)back.width;
        rq.y = (int32_t)back.height;
        rq.pitch = back.pitch;
        rq.offset = 0;
        blitting = gpu_send(&rq, sizeof rq, back_region.cap, &got, NULL) == GPU_OK;
        back_id = got.buffer.id;
        blit_pending = false;
    }

    say_begin(&line);
    say_text(&line, "windowserver: page flips, between the firmware's screen and buffer ");
    say_dec(&line, buffer_id[1]);
    /*
     * **Composing's opaque parts stay the processor's for now** (6b,
     * `testing.md` 18.669): on the M700 they worked - 0 pixels different -
     * and took the processor's composing from 0.80 ms a frame to 0.43, but
     * the blitter could not keep up with them and the copy to the screen
     * behind them, and the screen got 42 frames a second rather than 57.
     * Diego: "it has some choppy moments". Responsiveness first: off, until
     * the blitter copies faster (XY_FAST_COPY_BLT, next).
     */
    blit_composing = false;
    say_text(&line, blitting ? "; the copy into them on the blitter" : "; the copy into them here");
    say_send(console, &line);
}

static void copy_damage(struct surface *to, const struct damage *d)
{
    unsigned k;

    if (d->whole) {
        blit_on(to, &back, 0, 0, (long)screen.width, (long)screen.height, 0, 0);
        return;
    }

    for (k = 0; k < d->n; k++) {
        blit_on(to, &back, d->r[k].x, d->r[k].y, d->r[k].w, d->r[k].h, d->r[k].x, d->r[k].y);
    }
}

/*
 * What is owed, put on the screen if the display has left the other buffer:
 * copied into it, and flipped to. Returns whether something is still owed.
 */
/* The rectangles of `d`, or the whole screen, into a blit's list. */
static unsigned blit_rects(struct gpu_blit *b, const struct damage *d)
{
    unsigned k;

    if (d->whole || b->n + d->n > GPU_BLIT_RECTS) {
        b->n = 1;
        b->rects[0].x = 0;
        b->rects[0].y = 0;
        b->rects[0].w = (int32_t)screen.width;
        b->rects[0].h = (int32_t)screen.height;
        b->rects[0].sx = 0;
        b->rects[0].sy = 0;
        return 1;
    }

    for (k = 0; k < d->n && b->n < GPU_BLIT_RECTS; k++) {
        b->rects[b->n].x = (int32_t)d->r[k].x;
        b->rects[b->n].y = (int32_t)d->r[k].y;
        b->rects[b->n].w = (int32_t)d->r[k].w;
        b->rects[b->n].h = (int32_t)d->r[k].h;
        b->rects[b->n].sx = (int32_t)d->r[k].x;
        b->rects[b->n].sy = (int32_t)d->r[k].y;
        b->n++;
    }

    return b->n;
}

/*
 * The first frames the blitter copies, compared with the back buffer they
 * came from, pixel by pixel - the plan's "compared pixel for pixel with
 * what the processor draws" - and said once, after twenty.
 */
static void check_blit(const struct gpu_blit *b, const struct surface *to, size_t to_bytes)
{
    unsigned k;

    if (checks_left == 0) {
        return;
    }

    for (k = 0; k < b->n; k++) {
        long x0 = b->rects[k].x, y0 = b->rects[k].y, x, y;

        for (y = y0 < 0 ? 0 : y0; y < y0 + b->rects[k].h && y < (long)screen.height; y++) {
            /* A surface's `pitch` is bytes and its `pixels` 32-bit words, so
             * a row is found through a byte pointer - added to `pixels`
             * itself it was four rows' worth too far, and the first check
             * read past its buffer and took the window server down
             * (`testing.md` 18.667). */
            const uint32_t *want = (const uint32_t *)((const uint8_t *)back.pixels + (size_t)y * back.pitch);
            const volatile uint32_t *got =
                (const volatile uint32_t *)((const uint8_t *)to->pixels + (size_t)y * to->pitch);

            /* Every row held to the regions it is read from: a window
             * server that faults takes the desktop with it (18.667). */
            if ((size_t)y * to->pitch + (size_t)screen.width * 4u > to_bytes
                || (size_t)y * back.pitch + (size_t)screen.width * 4u > back_region.bytes) {
                struct say_line line;

                say_begin(&line);
                say_text(&line, "windowserver: a checked row past its region: y ");
                say_dec(&line, (unsigned long)y);
                say_text(&line, ", to ");
                say_hex(&line, (unsigned long)(uintptr_t)to->pixels, 12);
                say_text(&line, " pitch ");
                say_dec(&line, to->pitch);
                say_text(&line, " of ");
                say_dec(&line, (unsigned long)to_bytes);
                say_text(&line, ", back pitch ");
                say_dec(&line, back.pitch);
                say_text(&line, " of ");
                say_dec(&line, (unsigned long)back_region.bytes);
                say_send(console, &line);
                checks_left = 1;
                break;
            }

            for (x = x0 < 0 ? 0 : x0; x < x0 + b->rects[k].w && x < (long)screen.width; x++) {
                checked_px++;
                wrong_px += (got[x] & 0x00ffffffu) != (want[x] & 0x00ffffffu);
            }
        }
    }

    if (--checks_left == 0) {
        struct say_line line;

        say_begin(&line);
        say_text(&line, "windowserver: the blitter's copies checked, 20 frames, ");
        say_dec(&line, (unsigned long)checked_px);
        say_text(&line, " pixels, ");
        say_dec(&line, (unsigned long)wrong_px);
        say_text(&line, " different");
        say_send(console, &line);
    }
}

/*
 * What is owed, put on the screen if the display has left the other buffer:
 * copied into it - by the blitter, or here - and flipped to. Returns whether
 * something is still to be done, which the receive loop looks at again a
 * tick later.
 */
static bool present(void)
{
    struct gpu_reply rep;
    unsigned hidden;
    unsigned long t0;

    if (!flipping || (owed.n == 0 && !owed.whole && !blit_pending)) {
        return false;
    }

    if (flip_pending) {
        if (gpu_ask(GPU_OP_FLIPPED, 0, 0, &rep, NULL) != GPU_OK) {
            return true;
        }

        if (rep.flip.shown != buffer_id[shown]) {
            return true;            /* still on the other one: a tick later */
        }

        flip_pending = false;
    }

    /* A blit given: flipped to once its fence is reached. */
    if (blit_pending) {
        if (gpu_ask(GPU_OP_FENCE, 0, 0, &rep, NULL) != GPU_OK
            || (int32_t)(rep.engine.value - blit_fence) < 0) {
            return true;
        }

        blit_pending = false;

        if (gpu_ask(GPU_OP_FLIP, (int32_t)buffer_id[blit_target], 0, &rep, NULL) != GPU_OK) {
            return true;
        }

        shown = blit_target;
        flip_pending = true;
        given = sent;
        return owed.n > 0 || owed.whole;
    }

    hidden = 1u - shown;

    if (blitting) {
        static struct gpu_blit b;

        memset(&b, 0, sizeof b);
        b.op = GPU_OP_BLIT;
        b.src = back_id;
        b.dst = buffer_id[hidden];
        b.n = 0;
        (void)blit_rects(&b, &given);
        (void)blit_rects(&b, &owed);

        uint32_t error = gpu_send(&b, 16 + b.n * 24u, -1, &rep, NULL);


        if (error == GPU_OK) {
            blit_pending = true;
            blit_fence = rep.engine.value;
            blit_target = hidden;
            sent = owed;
            owed.n = 0;
            owed.whole = false;

            /* The first frames: the blit waited for here, so its pixels
             * can be compared before anything else touches them. */
            if (checks_left > 0) {
                for (unsigned i = 0; i < 250; i++) {
                    if (gpu_ask(GPU_OP_FENCE, 0, 0, &rep, NULL) == GPU_OK
                        && (int32_t)(rep.engine.value - blit_fence) >= 0) {
                        check_blit(&b, &buffers[hidden],
                                   hidden == 0 ? (size_t)screen.pitch * screen.height
                                               : flip_region.bytes);
                        break;
                    }

                    kosmos_sleep(1);
                }
            }

            return true;
        }

        if (error != GPU_ERR_BUSY) {
            struct say_line line;

            blitting = false;
            say_begin(&line);
            say_text(&line, "windowserver: the blitter refused a copy (");
            say_dec(&line, error);
            say_text(&line, "); copying here from now on");
            say_send(console, &line);
        }

        return true;
    }

    t0 = kosmos_ticks();
    copy_damage(&buffers[hidden], &given);
    copy_damage(&buffers[hidden], &owed);

#if defined(__x86_64__)
    /* Write-combining stores drained before the display is pointed at
     * them: `sfence` orders them ahead of the flip's register write. */
    __asm__ volatile("sfence" ::: "memory");
#endif

    copy_ticks += kosmos_ticks() - t0;

    if (gpu_ask(GPU_OP_FLIP, (int32_t)buffer_id[hidden], 0, &rep, NULL) != GPU_OK) {
        return true;
    }

    shown = hidden;
    flip_pending = true;
    given = owed;
    owed.n = 0;
    owed.whole = false;
    return false;
}

/*
 * Whether a frame can go to the screen now: no flip on its way, or the one
 * that was has been taken. While one is still on its way, composing now
 * would only make a frame that waits - and the next would replace it.
 */
static bool ready_for_frame(void)
{
    struct gpu_reply rep;

    if (!flipping || (!flip_pending && !blit_pending)) {
        return true;
    }

    /* A copy on the blitter still running, or its flip still to be asked:
     * the frame it carries is not on the screen yet. */
    if (blit_pending) {
        (void)present();

        if (blit_pending) {
            return false;
        }
    }

    if (!flip_pending) {
        return true;
    }

    if (gpu_ask(GPU_OP_FLIPPED, 0, 0, &rep, NULL) != GPU_OK) {
        return true;
    }

    if (rep.flip.shown != buffer_id[shown]) {
        return false;
    }

    flip_pending = false;
    return true;
}

/* Back to the firmware's screen, for the console, and the flips ended. */
static void flips_stop(void)
{
    struct gpu_reply rep;

    if (!flipping) {
        return;
    }

    if (shown != 0) {
        (void)gpu_ask(GPU_OP_FLIP, 0, 0, &rep, NULL);
    }

    /* The back buffer the GPU was given goes with the manager: forgotten
     * once the blitter has stopped reading it, a second at most. */
    if (blitting) {
        for (unsigned i = 0; i < 250; i++) {
            if (gpu_ask(GPU_OP_BUFFER_FORGET, (int32_t)back_id, 0, &rep, NULL) != GPU_ERR_BUSY) {
                break;
            }

            kosmos_sleep(1);
        }

        blitting = false;
    }

    blit_composing = false;
    blit_pending = false;
    flipping = false;
    flip_pending = false;
    shown = 0;
}

static unsigned long counter_hz;     /* the counter's ticks a second */

/* Counter ticks to microseconds, held to a word. */
static uint32_t in_us(unsigned long ticks)
{
    unsigned long us = counter_hz ? ticks / (counter_hz / 1000000ul ? counter_hz / 1000000ul : 1) : 0;

    return us > 0xffffffffu ? 0xffffffffu : (uint32_t)us;
}

/*--------------------------------------------------------------------------
 * **The opaque parts of composing on the blitter** (`m700-2d.md` step 6b).
 *
 * A pixel's final value is a plain copy of a window's picture when that
 * window is opaque, unscaled, not stale and has no bar there, the pixel is
 * inside its corners, its lights and its grip, and nothing is above it -
 * no window, no window's shadow, no menu, panel, level bar, badge, pointer
 * or outline. Those pieces of each damaged rectangle are copied by the
 * blitter, straight from the window's buffer into the back buffer, while
 * the processor composes the rest of the rectangle as before; the two never
 * touch the same pixel, so they run at the same time. The first frames are
 * composed again here into a scratch buffer and compared.
 *------------------------------------------------------------------------*/

#define PIECES_MAX   64
#define PIECE_LEAST  4096           /* pixels: a smaller piece is cheaper here */

struct piece {
    long x, y, w, h;
    int  win;                       /* the window it is copied from */
};

static unsigned compose_checks = 20;
static uint64_t compose_checked, compose_wrong;
static struct surface scratch;      /* for the check, then freed */

/* A picture in the GPU's table: imported the first time, its id after. */
static uint32_t picture_gpu(int slot)
{
    struct picture *p;
    struct gpu_request rq;
    struct gpu_reply got;

    if (slot < 0 || (unsigned)slot >= npictures || pictures[slot].region.cap < 0) {
        return 0;
    }

    p = &pictures[slot];

    if (p->gpu_id == 0) {
        memset(&rq, 0, sizeof rq);
        rq.op = GPU_OP_BUFFER_IMPORT;
        rq.x = (int32_t)p->surface.width;
        rq.y = (int32_t)p->surface.height;
        rq.pitch = p->surface.pitch;
        rq.offset = (uint32_t)((uintptr_t)p->surface.pixels - p->region.at);
        p->gpu_id = gpu_send(&rq, sizeof rq, p->region.cap, &got, NULL) == GPU_OK
                  ? got.buffer.id : ~0u;
    }

    return p->gpu_id == ~0u ? 0 : p->gpu_id;
}

/* A picture let go: out of the GPU's table first, once the blitter has
 * stopped reading it - a second at most. */
static void picture_gpu_forget(unsigned slot)
{
    struct gpu_reply rep;
    unsigned i;

    if (slot >= npictures || pictures[slot].gpu_id == 0) {
        return;
    }

    if (pictures[slot].gpu_id != ~0u) {
        for (i = 0; i < 250; i++) {
            if (gpu_ask(GPU_OP_BUFFER_FORGET, (int32_t)pictures[slot].gpu_id, 0, &rep, NULL)
                != GPU_ERR_BUSY) {
                break;
            }

            kosmos_sleep(1);
        }
    }

    pictures[slot].gpu_id = 0;
}

/* `list` less `b`: each rectangle cut into up to four around it. False
 * when that would not fit. */
static bool cut(struct piece *list, int *n, int max, long bx, long by, long bw, long bh)
{
    static struct piece out[PIECES_MAX * 8];
    int k, m = 0;

    if (bw <= 0 || bh <= 0) {
        return true;
    }

    for (k = 0; k < *n; k++) {
        struct piece p = list[k];

        /* Four more at most from this one: never past `out`, nor `max`. */
        if (m + 4 > (int)(sizeof out / sizeof out[0])) {
            return false;
        }

        long x0 = bx > p.x ? bx : p.x, y0 = by > p.y ? by : p.y;
        long x1 = bx + bw < p.x + p.w ? bx + bw : p.x + p.w;
        long y1 = by + bh < p.y + p.h ? by + bh : p.y + p.h;

        if (x1 <= x0 || y1 <= y0) {
            out[m++] = p;
            continue;
        }

        if (y0 > p.y) out[m++] = (struct piece){ p.x, p.y, p.w, y0 - p.y, p.win };
        if (y1 < p.y + p.h) out[m++] = (struct piece){ p.x, y1, p.w, p.y + p.h - y1, p.win };
        if (x0 > p.x) out[m++] = (struct piece){ p.x, y0, x0 - p.x, y1 - y0, p.win };
        if (x1 < p.x + p.w) out[m++] = (struct piece){ x1, y0, p.x + p.w - x1, y1 - y0, p.win };
    }

    if (m > max) {
        return false;
    }

    memcpy(list, out, (size_t)m * sizeof out[0]);
    *n = m;
    return true;
}

/* Everything that draws over `k`'s picture, cut out of `list`. */
static bool uncovered(struct piece *list, int *n, int k)
{
    const struct sc_window *w = &sc->win[k];
    long s = sc->shadow;
    int j;

    if ((w->flags & SC_LIGHTS) && !cut(list, n, PIECES_MAX, w->bx, w->by, sc->run, sc->box)) {
        return false;
    }

    if ((w->flags & SC_GRIP)
        && !cut(list, n, PIECES_MAX, w->x + w->w - sc->grip, w->y + w->h - sc->grip, sc->grip, sc->grip)) {
        return false;
    }

    for (j = k + 1; j < sc->nwin; j++) {
        const struct sc_window *a = &sc->win[j];

        if (a->flags & SC_HIDDEN) {
            continue;
        }

        if ((a->flags & SC_SHADOW) && s > 0) {
            if (!cut(list, n, PIECES_MAX, a->x - s, a->y - s + s / 3, a->w + 2 * s, a->h + 2 * s)) {
                return false;
            }
        }

        if (!cut(list, n, PIECES_MAX, a->x, a->y, a->w, a->h)) {
            return false;
        }
    }

    for (j = 0; j < sc->nmenu; j++) {
        if (!cut(list, n, PIECES_MAX, sc->menu[j].x, sc->menu[j].y, sc->menu[j].w, sc->menu[j].h)) {
            return false;
        }
    }

    return (sc->osd == NULL || sc->osd_alpha == 0
            || cut(list, n, PIECES_MAX, sc->osd_x, sc->osd_y, (long)sc->osd->width, (long)sc->osd->height))
        && (sc->panel == NULL
            || cut(list, n, PIECES_MAX, sc->panel_x, sc->panel_y, (long)sc->panel->width, (long)sc->panel->height))
        && (sc->cursor == NULL
            || cut(list, n, PIECES_MAX, sc->cursor_x, sc->cursor_y, (long)sc->cursor->width, (long)sc->cursor->height))
        && (sc->badge == NULL
            || cut(list, n, PIECES_MAX, sc->badge_x, sc->badge_y, (long)sc->badge->width, (long)sc->badge->height))
        && (!sc->outline_on || cut(list, n, PIECES_MAX, sc->ox, sc->oy, sc->ow, sc->oh));
}

/*
 * The pieces of `r` the blitter can copy, into `out`, at most `max`; each
 * window's from the top down, so a lower window's pieces are cut by the
 * ones above it through `uncovered`.
 */
static int opaque_pieces(long rx, long ry, long rw, long rh, struct piece *out, int max)
{
    int k, total = 0;

    for (k = sc->nwin - 1; k >= 0 && total < max; k--) {
        const struct sc_window *w = &sc->win[k];
        struct piece list[PIECES_MAX];
        long c = (w->flags & SC_ROUNDED) ? sc->corner : 0;
        long x0, y0, x1, y1;
        int n = 1, p;

        if ((w->flags & (SC_HIDDEN | SC_BLEND | SC_STALE)) || w->picture == NULL || w->bar != NULL
            || w->src_w != w->w || w->src_h != w->h || picture_gpu(w->picture_slot) == 0) {
            continue;
        }

        /* Inside its corners: a band of the corner's size off every side. */
        x0 = w->x + c > rx ? w->x + c : rx;
        y0 = w->y + c > ry ? w->y + c : ry;
        x1 = w->x + w->w - c < rx + rw ? w->x + w->w - c : rx + rw;
        y1 = w->y + w->h - c < ry + rh ? w->y + w->h - c : ry + rh;

        if (x1 <= x0 || y1 <= y0) {
            continue;
        }

        list[0] = (struct piece){ x0, y0, x1 - x0, y1 - y0, k };

        if (!uncovered(list, &n, k)) {
            continue;
        }

        for (p = 0; p < n && total < max; p++) {
            if (list[p].w * list[p].h >= PIECE_LEAST) {
                out[total++] = list[p];
            }
        }
    }

    return total;
}

/* The blitter's pieces of one rectangle given, a blit for each window;
 * the fence of the last. False when it would not take them all. */
static bool blit_pieces(const struct piece *pc, int n, uint32_t *fence)
{
    static struct gpu_blit b;
    struct gpu_reply rep;
    int i = 0;

    while (i < n) {
        int win = pc[i].win;
        const struct sc_window *w = &sc->win[win];

        memset(&b, 0, sizeof b);
        b.op = GPU_OP_BLIT;
        b.src = picture_gpu(w->picture_slot);
        b.dst = back_id;

        while (i < n && pc[i].win == win && b.n < GPU_BLIT_RECTS) {
            b.rects[b.n].x = (int32_t)pc[i].x;
            b.rects[b.n].y = (int32_t)pc[i].y;
            b.rects[b.n].w = (int32_t)pc[i].w;
            b.rects[b.n].h = (int32_t)pc[i].h;
            b.rects[b.n].sx = (int32_t)(pc[i].x - w->x);
            b.rects[b.n].sy = (int32_t)(pc[i].y - w->y);
            b.n++;
            i++;
        }

        if (gpu_send(&b, 16 + b.n * 24u, -1, &rep, NULL) != GPU_OK) {
            return false;
        }

        *fence = rep.engine.value;
    }

    return true;
}

/* The fence reached, looked for at once and then a tick at a time. */
static bool fence_wait(uint32_t fence)
{
    struct gpu_reply rep;
    unsigned i;

    for (i = 0; i < 2000 + 250; i++) {
        if (gpu_ask(GPU_OP_FENCE, 0, 0, &rep, NULL) == GPU_OK
            && (int32_t)(rep.engine.value - fence) >= 0) {
            return true;
        }

        if (i >= 2000) {
            kosmos_sleep(1);
        }
    }

    return false;
}

/* The blitter's pieces composed again here, into the scratch buffer, and
 * compared with what it copied. Said once, after twenty frames. */
static void check_pieces(const struct piece *pc, int n)
{
    struct surface *was = sc->back;
    int i;

    if (compose_checks == 0 || n == 0) {
        return;
    }

    if (scratch.pixels == NULL) {
        scratch = back;
        scratch.pixels = malloc((size_t)back.pitch * back.height);

        if (scratch.pixels == NULL) {
            compose_checks = 0;
            return;
        }
    }

    sc->back = &scratch;

    for (i = 0; i < n; i++) {
        long x, y;

        (void)scene_compose(sc, pc[i].x, pc[i].y, pc[i].w, pc[i].h);

        for (y = pc[i].y; y < pc[i].y + pc[i].h; y++) {
            const uint32_t *want = (const uint32_t *)((const uint8_t *)scratch.pixels + (size_t)y * scratch.pitch);
            const uint32_t *got = (const uint32_t *)((const uint8_t *)back.pixels + (size_t)y * back.pitch);

            for (x = pc[i].x; x < pc[i].x + pc[i].w; x++) {
                compose_checked++;
                compose_wrong += (want[x] & 0x00ffffffu) != (got[x] & 0x00ffffffu);
            }
        }
    }

    sc->back = was;

    if (--compose_checks == 0) {
        struct say_line line;

        free(scratch.pixels);
        scratch.pixels = NULL;
        say_begin(&line);
        say_text(&line, "windowserver: the blitter's composing checked, 20 frames, ");
        say_dec(&line, (unsigned long)compose_checked);
        say_text(&line, " pixels, ");
        say_dec(&line, (unsigned long)compose_wrong);
        say_text(&line, " different");
        say_send(console, &line);
    }
}

/*
 * One damaged rectangle composed: its opaque pieces given to the blitter,
 * the rest composed here around them. The pixels drawn, or -1 out of room.
 * `*fence` is the last fence given, untouched when nothing was.
 */
static long compose_rect(long x, long y, long w, long h, uint32_t *fence, bool *gave)
{
    static struct piece pc[PIECES_MAX];
    static struct piece rest[PIECES_MAX * 2];
    int n = 0, nrest = 1, i;
    long drawn = 0, got;

    if (blit_composing) {
        n = opaque_pieces(x, y, w, h, pc, PIECES_MAX);
    }

    if (n > 0 && blit_pieces(pc, n, fence)) {
        *gave = true;
        rest[0] = (struct piece){ x, y, w, h, -1 };

        for (i = 0; i < n; i++) {
            if (!cut(rest, &nrest, PIECES_MAX * 2, pc[i].x, pc[i].y, pc[i].w, pc[i].h)) {
                nrest = -1;
                break;
            }
        }

        /* Too many pieces left to compose: all of it here, the blitter's
         * copies the same pixels the processor writes - harmless. */
        if (nrest < 0) {
            return scene_compose(sc, x, y, w, h);
        }

        for (i = 0; i < nrest; i++) {
            got = scene_compose(sc, rest[i].x, rest[i].y, rest[i].w, rest[i].h);

            if (got < 0) {
                return -1;
            }

            drawn += got;
        }

        for (i = 0; i < n; i++) {
            drawn += pc[i].w * pc[i].h;
        }

        return drawn;
    }

    return scene_compose(sc, x, y, w, h);
}

static uint32_t compose(bool show, uint32_t *error, struct ws_reply *rep)
{
    const struct ws_rect *rects;
    uint32_t nrect, k;
    uint64_t drawn = 0;
    unsigned long composing = 0, copying = 0, t0, t1;
    uint32_t fence = 0;
    bool gave = false;
    static struct piece checked[PIECES_MAX];
    int nchecked = 0;

    if (back_region.cap < 0 || scene_region.cap < 0 || sc == NULL) {
        *error = WS_ERR_NO_SCENE;
        return 0;
    }

    if (!read_scene(&rects, &nrect)) {
        *error = WS_ERR_BAD_SCENE;
        return 0;
    }

    for (k = 0; k < nrect; k++) {
        struct ws_rect r;
        long x, y, w, h, got;

        memcpy(&r, &rects[k], sizeof(r));
        x = r.x;
        y = r.y;
        w = r.w;
        h = r.h;

        /* On the back buffer, or nothing: a rectangle off it draws nothing. */
        if (!clip(&back, &x, &y, &w, &h, NULL, NULL)) {
            continue;
        }

        t0 = kosmos_ticks();
        got = compose_rect(x, y, w, h, &fence, &gave);
        t1 = kosmos_ticks();

        /* For the check: this rectangle's pieces, worked out again. */
        if (gave && compose_checks > 0 && nchecked < PIECES_MAX) {
            nchecked += opaque_pieces(x, y, w, h, checked + nchecked, PIECES_MAX - nchecked);
        }
        composing += t1 - t0;

        if (got < 0) {
            *error = WS_ERR_NO_ROOM;
            return (uint32_t)drawn;
        }

        drawn += (uint64_t)got;

        if (show && flipping) {
            damage_add(&owed, x, y, w, h);
        } else if (show && have_screen) {
            blit_on(&screen_surface, &back, x, y, w, h, x, y);
            (void)kosmos_screen_flush((unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h);
            copying += kosmos_ticks() - t1;
        }
    }

    /*
     * **Not waited for** (`testing.md` 18.669): the copy to the screen is
     * behind these in the ring, and the next frame is not composed until
     * that copy's fence is reached (`ready_for_frame`) - so nothing here
     * reads what the blitter is still writing, but VNC's mirror, which may
     * be a frame behind. Waiting cost more than the blitter saved: 3.41 ms
     * a frame against 0.80. Only the checked frames wait, to be compared.
     */
    if (gave && compose_checks > 0) {
        t0 = kosmos_ticks();

        if (fence_wait(fence)) {
            check_pieces(checked, nchecked);
        }

        composing += kosmos_ticks() - t0;
    }

    if (show && flipping) {
        (void)present();
    }

    copying += copy_ticks;
    copy_ticks = 0;
    rep->compose_us = in_us(composing);
    rep->copy_us = in_us(copying);
    return drawn > 0xffffffffu ? 0xffffffffu : (uint32_t)drawn;
}

static void answer(const struct message *in, uint64_t sender)
{
    struct message out;
    struct ws_reply *rep = (struct ws_reply *)(void *)out.data;
    struct ws_request req;
    long cap = in->cap_plus_one > 0 ? (long)in->cap_plus_one - 1 : -1;

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);
    memset(&req, 0, sizeof(req));

    /* The greeting of step one was two words; every request since is the
     * whole shape, and the shorter one is read as far as it goes. */
    if (in->length < 8) {
        rep->error = WS_ERR_BAD_OP;

        if (cap >= 0) {
            (void)kosmos_cap_drop(cap);
        }

        (void)kosmos_reply(sender, &out);
        return;
    }

    memcpy(&req, in->data, in->length < sizeof(req) ? in->length : sizeof(req));

    switch (req.op) {
    case WS_OP_HELLO:
        if (!have_screen) {
            rep->error = WS_ERR_NO_SCREEN;
            break;
        }

        if (cap >= 0) {
            if (manager >= 0) {
                (void)kosmos_cap_drop(manager);
            }

            manager = cap;
            cap = -1;
        }

        rep->width = (uint32_t)screen.width;
        rep->height = (uint32_t)screen.height;
        break;

    case WS_OP_BACK: {
        struct held got;

        if (!take(cap, &got)) {
            rep->error = WS_ERR_NO_REGION;
            break;
        }

        cap = -1;

        if (!fits(0, req.width, req.height, req.pitch, got.bytes)) {
            let_go(&got);
            rep->error = WS_ERR_TOO_SMALL;
            break;
        }

        let_go(&back_region);
        back_region = got;
        surface_over(&back, got.at, req.width, req.height, req.pitch);

        if (sc == NULL) {
            sc = malloc(sizeof(*sc));

            if (sc == NULL) {
                rep->error = WS_ERR_NO_ROOM;
                break;
            }

            scene_init(sc, &back);
        }

        sc->back = &back;

        /*
         * **The screen taken from the console** (step 3): it stops printing
         * on it, and prints again when the manager goes (`manager_gone`).
         * The window manager does not take it itself when it has a window
         * server.
         */
        if (have_screen && !screen_taken) {
            screen_taken = kosmos_screen_take(1) >= 0;
        }

        if (screen_taken) {
            flips_start();
        }

        break;
    }

    case WS_OP_SCENE: {
        struct held got;

        if (!take(cap, &got)) {
            rep->error = WS_ERR_NO_REGION;
            break;
        }

        cap = -1;
        let_go(&scene_region);
        scene_region = got;
        break;
    }

    case WS_OP_PICTURE: {
        struct held got;

        if (req.slot >= WS_SLOTS_MAX) {
            rep->error = WS_ERR_BAD_OP;
            break;
        }

        if (!take(cap, &got)) {
            rep->error = WS_ERR_NO_REGION;
            break;
        }

        cap = -1;

        if (!fits(req.offset, req.width, req.height, req.pitch, got.bytes)) {
            let_go(&got);
            rep->error = WS_ERR_TOO_SMALL;
            break;
        }

        if (!room_for(req.slot)) {
            let_go(&got);
            rep->error = WS_ERR_NO_ROOM;
            break;
        }

        forget(req.slot);
        pictures[req.slot].region = got;
        nheld++;
        surface_over(&pictures[req.slot].surface, got.at + (uintptr_t)req.offset, req.width,
                     req.height, req.pitch);
        break;
    }

    case WS_OP_FORGET:
        forget(req.slot);
        break;

    case WS_OP_COMPOSE: {
        uint32_t error = WS_OK;

        /* Not now: the scene is not read and the caller keeps its damage. */
        if ((req.flags & WS_IF_READY) != 0 && !ready_for_frame()) {
            rep->height = 1;
            rep->width = nheld;
            break;
        }

        rep->drawn = compose((req.flags & WS_SHOW) != 0, &error, rep);
        rep->error = error;
        rep->width = nheld;
        break;
    }

    default:
        rep->error = WS_ERR_BAD_OP;
        break;
    }

    /* A region nobody asked for, or one refused above, is not kept. */
    if (cap >= 0) {
        (void)kosmos_cap_drop(cap);
    }

    (void)kosmos_reply(sender, &out);
}

/*
 * **The window manager gone**: everything it handed over let go, the
 * display's pointer hidden, and the screen given back to the console,
 * which clears it and prints from the top. A window manager started again
 * greets and hands everything over afresh.
 */
static void manager_gone(void)
{
    unsigned k;

    (void)kosmos_cap_drop(manager);
    manager = -1;

    for (k = 0; k < npictures; k++) {
        forget(k);
    }

    let_go(&scene_region);
    let_go(&back_region);

    if (sc != NULL) {
        scene_free(sc);
        free(sc);
        sc = NULL;
    }

    flips_stop();

    if (screen_taken) {
        struct gpu_reply rep;

        (void)kosmos_cursor_hide();
        (void)gpu_ask(GPU_OP_CURSOR_HIDE, 0, 0, &rep, NULL);
        (void)kosmos_screen_take(0);
        screen_taken = false;
    }

    {
        struct say_line line;

        say_begin(&line);
        say_text(&line, "windowserver: the window manager is gone; the screen is the console's");
        say_send(console, &line);
    }
}

void windowserver_server(long endpoint, long console_cap, long gpu_cap)
{
    struct schedinfo info;
    unsigned long watch_ticks = 50;     /* scheduler ticks: a fifth of a second at 250 */
    console = console_cap;
    gpu = gpu_cap;
    memset(&screen, 0, sizeof(screen));
    have_screen = kosmos_screen(&screen) >= 0;

    if (have_screen) {
        surface_over(&screen_surface, (uintptr_t)screen.address, screen.width, screen.height,
                     screen.pitch);
    }

    {
        struct say_line line;

        say_begin(&line);

        if (have_screen) {
            say_text(&line, "windowserver: the screen, ");
            say_dec(&line, (unsigned long)screen.width);
            say_text(&line, "x");
            say_dec(&line, (unsigned long)screen.height);
        } else {
            say_text(&line, "windowserver: no screen on this machine");
        }

        say_send(console, &line);
    }

    /* How long to wait for a request before looking at the manager: a
     * fifth of a second, however fast the scheduler ticks. */
    if (kosmos_sched_info(&info) == 0 && info.tick_hz >= 5) {
        watch_ticks = info.tick_hz / 5;
    }

    {
        static struct sysinfo machine;  /* large: not on the stack */

        if (kosmos_sysinfo(&machine) == 0) {
            counter_hz = machine.counter_hz;
        }
    }

    for (;;) {
        struct message msg;
        uint64_t sender = 0;
        /* A tick when a frame is owed to a display still on its way to the
         * last one; a fifth of a second while a manager is to be watched. */
        unsigned long wait = (flipping && (owed.n > 0 || owed.whole || blit_pending)) ? 1ul
                           : manager >= 0 ? watch_ticks : 0;
        long got = kosmos_receive(endpoint, &msg, &sender, 0, wait);

        if (got == 0) {
            answer(&msg, sender);
        } else if (got != -7) {
            return;
        }

        (void)present();

        if (manager >= 0 && kosmos_cap_check(manager) < 0) {
            manager_gone();
        }
    }
}
