/*
 * The boot, narrated. See boot.h.
 */

#include "boot.h"
#include "console.h"

static unsigned done;

void boot_stage(const char *what)
{
    done++;

    /* The number in a dimmer colour than the name, so the sequence reads as
     * a list rather than as a wall. On the serial side the colour is
     * ignored and the brackets do the same job. */
    console_colour(0xff58a6ff);
    kputc('[');
    kputu(done);
    kputc('/');
    kputu(BOOT_STAGES);
    kputs("] ");

    console_colour(0xffc9d1d9);
    kputs(what);
    kputc('\n');

    console_progress(done, BOOT_STAGES);
}

void boot_detail(const char *text)
{
    console_colour(0xff8b949e);
    kputs("       ");
    kputs(text);
    kputc('\n');
    console_colour(0xffc9d1d9);
}

unsigned boot_stages_done(void)
{
    return done;
}
