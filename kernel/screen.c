/*
 * The display the board found, remembered for whoever is granted it.
 *
 * See screen.h for why this is not simply a second call to hal_fb_init.
 */

#include <stdbool.h>
#include <string.h>

#include "screen.h"

/*
 * Written once at boot, before there is a second thread, and read-only
 * afterwards. That is what makes a file-scope variable acceptable here where
 * `CLAUDE.md` otherwise forbids loose mutable globals: nothing mutates it
 * after the machine is running, so there is nothing for a second core to
 * race against.
 */
static struct fb display;
static bool      present;

void screen_init(const struct fb *fb)
{
    display = *fb;
    present = true;
}

bool screen_get(struct fb *out)
{
    if (!present) {
        return false;
    }

    *out = display;
    return true;
}
