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

static void forget(unsigned slot)
{
    if (slot < npictures) {
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

static uint32_t gpu_ask(uint32_t op, int32_t x, int32_t y, struct gpu_reply *rep, long *cap)
{
    static struct message msg, got;
    struct gpu_request rq;

    if (gpu < 0) {
        return GPU_ERR_NO_DEVICE;
    }

    memset(&rq, 0, sizeof rq);
    rq.op = op;
    rq.x = x;
    rq.y = y;
    memset(&msg, 0, sizeof msg);
    msg.length = (uint32_t)sizeof rq;
    memcpy(msg.data, &rq, sizeof rq);

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

/* A second screen from the GPU's driver, once the screen is taken. */
static void flips_start(void)
{
    struct gpu_reply rep;
    struct say_line line;
    long cap = -1;

    if (flipping || gpu < 0 || !have_screen) {
        return;
    }

    {
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
    }

    if (!take(cap, &flip_region)
        || flip_region.bytes < (size_t)rep.buffer.pitch * screen.height) {
        let_go(&flip_region);
        return;
    }

    buffer_id[0] = 0;
    buffer_id[1] = rep.buffer.id;
    buffers[0] = screen_surface;
    surface_over(&buffers[1], flip_region.at, screen.width, screen.height, rep.buffer.pitch);
    shown = 0;
    flip_pending = false;
    owed.n = given.n = 0;
    owed.whole = false;
    given.whole = true;             /* the new buffer holds nothing yet */
    flipping = true;

    say_begin(&line);
    say_text(&line, "windowserver: page flips, between the firmware's screen and buffer ");
    say_dec(&line, rep.buffer.id);
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
static bool present(void)
{
    struct gpu_reply rep;
    unsigned hidden;
    unsigned long t0;

    if (!flipping || (owed.n == 0 && !owed.whole)) {
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

    hidden = 1u - shown;
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

    if (!flipping || !flip_pending) {
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

static uint32_t compose(bool show, uint32_t *error, struct ws_reply *rep)
{
    const struct ws_rect *rects;
    uint32_t nrect, k;
    uint64_t drawn = 0;
    unsigned long composing = 0, copying = 0, t0, t1;

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
        got = scene_compose(sc, x, y, w, h);
        t1 = kosmos_ticks();
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
        unsigned long wait = (flipping && (owed.n > 0 || owed.whole)) ? 1ul
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
