/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where a PC's boot options come from: fw_cfg under QEMU, and the loader's
 * command line everywhere else.
 *
 * **A laptop has no fw_cfg.** Every option this system reads -
 * `opt/kosmos/boot`, `opt/kosmos/irq`, `opt/kosmos/smp` - was a `-fw_cfg`
 * argument, so on the ThinkPad not one of them could be given. GRUB's
 * `multiboot2` line carries the words after the kernel's path, and Multiboot
 * 1 under QEMU's `-kernel` carries `-append`, so both reach
 * `pc_loader_cmdline` and the same names work there:
 *
 *     multiboot2 /boot/kosmos.bin opt/kosmos/smp=1
 *
 * fw_cfg first, because under QEMU that is where a harness puts an option on
 * purpose. A word is `name=value`, words are separated by spaces, and a value
 * contains none - which every option so far satisfies.
 */

#include <stdbool.h>
#include <stddef.h>

#include "fwcfg.h"
#include "hal.h"
#include "multiboot.h"

static bool from_command_line(const char *line, const char *name,
                              char *out, unsigned long max)
{
    const char *at = line;
    size_t length = 0;

    while (name[length] != '\0') {
        length++;
    }

    while (*at != '\0') {
        size_t n = 0;

        while (*at == ' ') {
            at++;
        }

        while (n < length && at[n] == name[n]) {
            n++;
        }

        if (n == length && at[n] == '=') {
            size_t copied = 0;

            at += length + 1;

            while (at[copied] != '\0' && at[copied] != ' '
                   && copied + 1 < max) {
                out[copied] = at[copied];
                copied++;
            }

            out[copied] = '\0';
            return copied > 0;
        }

        while (*at != '\0' && *at != ' ') {
            at++;
        }
    }

    return false;
}

bool hal_boot_option(const char *name, char *out, unsigned long max)
{
    if (out == NULL || max == 0) {
        return false;
    }

    if (fwcfg_boot_option(name, out, max)) {
        return true;
    }

    out[0] = '\0';
    return from_command_line(pc_loader_cmdline(), name, out, max);
}
