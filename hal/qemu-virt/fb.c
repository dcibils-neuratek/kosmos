/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where this board's pixels come from: virtio-gpu when the machine has one
 * (`roadmap.md` 4h a), and ramfb otherwise.
 *
 * A pass-through, and it exists for the same reason `input_bind.c` does -
 * the PC has two possible answers and has to choose between them, so the
 * HAL name belongs to a board rather than to a driver.
 *
 * The Pi will replace this file rather than edit one: its firmware answers
 * a mailbox with an address it chose, which is neither of the two things
 * the other boards do.
 */

#include <stdbool.h>

#include "hal.h"
#include "ramfb.h"
#include "fbpixels.h"
#include "gpu.h"

bool hal_fb_init(struct fb *out)
{
    /* A machine started with virtio-gpu has asked for it, and it is the
     * one that is shown; ramfb, when it was not. */
    return virtio_gpu_init(out) || ramfb_init(out);
}

/* virtio-gpu is the only one of the two with something to send. */
void hal_fb_flush(unsigned x, unsigned y, unsigned w, unsigned h)
{
    virtio_gpu_flush(x, y, w, h);
}

/* With where its size came from (`opt/kosmos/fb`, `roadmap.md` 6zt). */
const char *hal_fb_describe(void)
{
    if (virtio_gpu_present()) {
        switch (fb_pixels_from()) {
        case FB_SIZE_ASKED:
            return "virtio-gpu, 2D, each drawn rectangle sent to the host, at "
                   "the size opt/kosmos/fb asked";
        case FB_SIZE_REFUSED:
            return "virtio-gpu, 2D, each drawn rectangle sent to the host, at "
                   "the size it was built for: opt/kosmos/fb asked for none "
                   "it can show";
        default:
            return "virtio-gpu, 2D, each drawn rectangle sent to the host, at "
                   "the size it was built for";
        }
    }

    switch (fb_pixels_from()) {
    case FB_SIZE_ASKED:
        return "ramfb, the way the Pi's mailbox will be, at the size "
               "opt/kosmos/fb asked";
    case FB_SIZE_REFUSED:
        return "ramfb, the way the Pi's mailbox will be, at the size it was "
               "built for: opt/kosmos/fb asked for none it can show";
    default:
        return "ramfb, the way the Pi's mailbox will be, at the size it was "
               "built for";
    }
}

/*
 * Not on this board, and nothing is lost by it.
 *
 * ramfb is the guest allocating pixels and telling the hypervisor to scan
 * them out, and "the guest allocating" is exactly what does not exist
 * before `pmm_init`. The board this matters on is the one with no serial
 * port; `virt` has one, and every boot here already reads its whole log
 * over a cable.
 */
bool hal_fb_early(struct fb *out)
{
    (void)out;

    return false;
}

/* Nothing said yes to `hal_fb_early` here, so nothing can need remapping. */
bool hal_fb_remap(struct fb *out)
{
    (void)out;

    return false;
}
