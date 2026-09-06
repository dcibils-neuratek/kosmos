/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What the machine was told to do, from the command line it was started
 * with.
 *
 * The ARM board answers this out of fw_cfg - QEMU's own name-value store,
 * reached through two I/O ports, with `opt/` reserved for exactly this. Its
 * comment says "not a kernel command line: there is a device tree with
 * `/chosen/bootargs` in it and parsing one is real work for a facility
 * fw_cfg already provides".
 *
 * On a PC the trade goes the other way. fw_cfg exists here too - it is a
 * QEMU device, not an ARM one - but a multiboot loader has *already* put
 * the command line in the structure it hands over, so reading it is a
 * pointer and a scan, and reaching for fw_cfg would be work to avoid work
 * already done.
 *
 * **The names are the same on both boards and only the last component is
 * used here.** The kernel asks for `opt/kosmos/boot`, which is a fw_cfg
 * path; this looks for `boot=` in `-append`. Mapping the path to its final
 * component rather than inventing a second naming scheme is what keeps
 * `syscall.c` from having to know which machine it is on - and the boot
 * option is the one thing a person types differently per machine anyway,
 * since it goes in the QEMU command line either way.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"
#include "pc.h"

/*
 * The command line, remembered at boot.
 *
 * Written once before there is a second thread and read-only afterwards,
 * which is what makes a file-scope variable acceptable here where
 * `CLAUDE.md` otherwise forbids loose mutable globals - the same argument
 * `kernel/screen.c` makes about the display it found.
 */
/*
 * A copy, not a pointer. `pc.h` says why: the string the loader left is in
 * memory the page allocator will hand out, and by the time anything asks it
 * is whatever was written over it.
 *
 * 256 bytes because a boot option is a word or a short command line, and a
 * longer one is refused rather than truncated - a machine doing something
 * adjacent to what was asked is worse than one that says it did not
 * understand.
 */
static char cmdline[256];

void pc_capture_cmdline(void)
{
    const struct multiboot_info *info =
        (const struct multiboot_info *)(uintptr_t)pc_multiboot;
    const char *from;
    unsigned i;

    cmdline[0] = '\0';

    if (pc_multiboot == 0 || (info->flags & MB_FLAG_CMDLINE) == 0) {
        return;
    }

    from = (const char *)(uintptr_t)info->cmdline;

    for (i = 0; i + 1 < sizeof(cmdline) && from[i] != '\0'; i++) {
        cmdline[i] = from[i];
    }

    cmdline[i] = '\0';
}

/* The part of a fw_cfg-shaped name after the last slash. `opt/kosmos/boot`
 * is `boot`, and a name with no slash in it is itself. */
static const char *leaf(const char *name)
{
    const char *at = name;
    const char *last = name;

    while (*at != '\0') {
        if (*at == '/') {
            last = at + 1;
        }

        at++;
    }

    return last;
}

/* Whether `at` begins with `key` followed by '='. */
static bool matches(const char *at, const char *key)
{
    while (*key != '\0') {
        if (*at != *key) {
            return false;
        }

        at++;
        key++;
    }

    return *at == '=';
}

bool hal_boot_option(const char *name, char *out, unsigned long max)
{
    const char *key;
    const char *at;

    if (out == NULL || max == 0) {
        return false;
    }

    out[0] = '\0';

    if (cmdline[0] == '\0') {
        return false;
    }

    key = leaf(name);
    at = cmdline;

    while (*at != '\0') {
        /* Skip whatever separates one option from the next, and only look
         * for a key at the start of a word - so `noboot=1` is not `boot`. */
        while (*at == ' ' || *at == '\t') {
            at++;
        }

        if (*at == '\0') {
            break;
        }

        if (matches(at, key)) {
            unsigned long i = 0;

            while (*at != '=') {
                at++;
            }

            at++;

            /*
             * To the end of the word, and a value that does not fit is
             * refused rather than truncated: a truncated boot option is a
             * machine doing something adjacent to what was asked, which is
             * worse than one that says it was not understood.
             */
            while (at[i] != '\0' && at[i] != ' ' && at[i] != '\t') {
                if (i + 1 >= max) {
                    out[0] = '\0';
                    return false;
                }

                out[i] = at[i];
                i++;
            }

            out[i] = '\0';

            return i != 0;
        }

        while (*at != '\0' && *at != ' ' && *at != '\t') {
            at++;
        }
    }

    return false;
}
