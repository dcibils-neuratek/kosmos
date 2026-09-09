/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where this board's pixels come from: the loader first, then ramfb.
 *
 * **This is the difference between a screen on a laptop and no screen at
 * all.** `ramfb` is QEMU's: the guest allocates memory and tells the
 * hypervisor to scan it out. A ThinkPad has nothing of the kind. What it
 * has is firmware that set a mode before anything of ours ran - and a
 * loader that can be asked to pass the address on, which is what the video
 * request in `boot/x86_64/start.S` asks for.
 *
 * The order is the point. A machine booted under QEMU with `-device ramfb`
 * and no loader framebuffer takes the second branch and nothing changes; a
 * laptop takes the first and gets a picture. Neither has to know the other
 * exists, and asking costs nothing on a loader that will not answer -
 * `MB_FLAG_FRAMEBUFFER` stays clear and this falls through.
 *
 * **What is checked, and why each one.** A framebuffer that is not RGB is a
 * palette or EGA text, neither of which anything above `struct fb` can
 * draw into. A depth that is not 32 would mean every pixel write above here
 * is the wrong width - the whole system treats a pixel as one 32-bit word.
 * And a zero address is a loader that set the flag and filled in nothing,
 * which has happened to other people and is cheaper to check than to
 * debug on a machine with no serial port.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"
#include "mmu.h"
#include "pc.h"
#include "ramfb.h"

static bool from_loader(struct fb *out)
{
    uint64_t addr;
    uint32_t pitch, width, height;
    uintptr_t mapped;

    /*
     * Asked of `pc_capture_memory`'s copy rather than of the multiboot
     * structure, which by now is under the page allocator. `pc.h` has the
     * account; it is the trap that ate the command line once already.
     */
    if (!pc_loader_framebuffer(&addr, &pitch, &width, &height)) {
        return false;
    }

    /*
     * **Mapped, because it is not in RAM and nothing else was going to map
     * it.**
     *
     * ramfb's pixels are memory the guest allocated, so they are inside the
     * identity map and a pointer to them simply works - which is why this
     * line did not exist and why nothing missed it. A firmware framebuffer
     * is somewhere else entirely: booted through GRUB under UEFI this
     * machine reported 0x80000000, two gigabytes up, where there is no RAM
     * and no mapping.
     *
     * The failure was worth the trip. The first pixel written faulted, the
     * fault handler tried to say so, the console lock was already held by
     * the write that faulted - and the machine printed `spinlock: console
     * held by 0, wanted by 0` for ever. On a laptop that is a dead black
     * screen, and the deadlock means it is dead in a way that cannot even
     * reach a serial port.
     *
     * Uncached, which is what `mmu_map_device` gives and is not what a
     * framebuffer wants: write-combining is, and it needs the PAT set up.
     * That is a real piece of work and this is the first boot; the desktop
     * will say plainly whether it is worth doing.
     */
    mapped = mmu_map_device((uintptr_t)addr, (size_t)pitch * height);

    if (mapped == 0) {
        return false;           /* the device window is full */
    }

    out->pixels = (volatile uint32_t *)mapped;
    out->width = width;
    out->height = height;
    out->pitch = pitch;

    return true;
}

static const char *source = "none";

bool hal_fb_init(struct fb *out)
{
    if (from_loader(out)) {
        source = "the loader's, from the multiboot video request";
        return true;
    }

    if (ramfb_init(out)) {
        source = "ramfb, which is QEMU's and has no equivalent on hardware";
        return true;
    }

    return false;
}

/*
 * Which of the two answered, in the words the boot log uses.
 *
 * **On a machine with no serial port this is the only way to know**, and
 * the two failures it distinguishes look identical from the outside: a
 * laptop that fell back to ramfb has no screen because there is no ramfb,
 * and a laptop whose loader ignored the video request has no screen for a
 * different reason and a different fix. One line here is the difference
 * between knowing which and guessing.
 */
const char *hal_fb_describe(void)
{
    return source;
}
