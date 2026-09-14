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

static const char *source = "none";

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
     * **Write-combining rather than uncached.** Uncached is what a device
     * register wants, where the write is the command; a framebuffer is
     * eight megabytes a compositor rewrites sixty times a second, and
     * making every store its own bus transaction is the difference between
     * a desktop and a slideshow. `mmu.h`'s `MAP_FRAMEBUFFER` has the whole
     * argument, including why this is a correctness question and not only a
     * speed one.
     */
    mapped = mmu_map_framebuffer((uintptr_t)addr, (size_t)pitch * height);

    if (mapped == 0) {
        return false;           /* the device window is full */
    }

    out->pixels = (volatile uint32_t *)mapped;

    /* Where the display controller reads from, which is not where this
     * kernel writes to. See `struct fb`. */
    out->phys = (uintptr_t)addr;

    out->width = width;
    out->height = height;
    out->pitch = pitch;

    /*
     * Said here rather than by the caller, because there are three of them
     * now - `hal_fb_init`, `hal_fb_remap`, and whichever runs first - and
     * only this function knows which memory type the mapping got. It said
     * the wrong thing for exactly one revision, on the path that matters:
     * the early screen means `hal_fb_init` is never called on a machine
     * with a firmware framebuffer, so the sentence the boot log printed was
     * the one set before any mapping had happened.
     */
    /*
     * **"write-combining" here is a claim about two mappings, and it used
     * to be a claim about one.**
     *
     * The compositor draws through its own mapping of these same pages, and
     * that one was write-back for as long as this line has existed - so on
     * the first machine with a real cache the log said write-combining, and
     * was right, about the mapping the desktop does not use. What it
     * described was quick; what the person was looking at was not.
     *
     * It says both now because `MAP_USER_FB` is asserted against
     * `MAP_FRAMEBUFFER` at compile time, so there is one memory type to
     * report rather than two that were never compared.
     */
    source = mmu_write_combining()
           ? "the loader's, from the multiboot video request, "
             "write-combining for the kernel and the compositor alike"
           : "the loader's, from the multiboot video request, uncached "
             "because this processor has no PAT";

    return true;
}

bool hal_fb_init(struct fb *out)
{
    if (from_loader(out)) {
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

/*
 * The loader's framebuffer, at its own address.
 *
 * **`boot/x86_64/start.S` identity maps the first four gigabytes, and a
 * screen below that line needs nothing more.** A screen above it has the
 * mapping added to those same tables by `mmu_boot_map_high`, and that is not
 * a hypothetical: the ThinkPad's firmware puts its screen at 0x4000000000.
 *
 * This function used to refuse it, on the reasoning that a multiboot loader
 * hands over a 32-bit pointer and so everything it describes is below the
 * line. The pointer is 32 bits; the framebuffer's address inside the
 * structure is 64, and the one machine the early screen was written for used
 * them. Every boot of it was dark from the loader's last line to stage six,
 * and nothing said so, because under OVMF the screen is at 0x80000000 and
 * this path never ran.
 *
 * A screen across the line is still refused: none has been reported, and it
 * would need both kinds of mapping at once.
 *
 * ramfb is deliberately not tried. It needs fw_cfg, a DMA setup and memory
 * the guest allocates, and the allocator does not exist at this point in
 * the boot - which is the whole reason this is a separate question.
 */
#define BOOT_IDENTITY_END   0x100000000ULL

bool hal_fb_early(struct fb *out)
{
    uint64_t addr, bytes;
    uint32_t pitch, width, height;

    if (!pc_loader_framebuffer(&addr, &pitch, &width, &height)) {
        return false;
    }

    bytes = (uint64_t)pitch * height;

    if (addr >= BOOT_IDENTITY_END) {
        /* Added uncached, for the reason below; `mmu.c` has the rest. */
        if (!mmu_boot_map_high((uintptr_t)addr, (size_t)bytes)) {
            return false;
        }
    } else if (addr + bytes > BOOT_IDENTITY_END) {
        return false;           /* across the line, which nothing has done */
    } else {
        /*
         * **And not write-back**, which is what `start.S` left it as.
         *
         * The boot page tables map the first four gigabytes with plain
         * present-and-writable 2 MB entries, and plain means write-back
         * cached - the one memory type MMIO may not have. Under QEMU it makes
         * no difference, because TCG models no cache and every store lands
         * at once; on a machine with a real one the boot log would sit in
         * cache and reach the panel when a line happened to be evicted, which
         * is exactly the failure this early screen exists to prevent.
         */
        mmu_boot_uncached((uintptr_t)addr, (size_t)bytes);
    }

    out->pixels = (volatile uint32_t *)(uintptr_t)addr;

    /* Identical before `mmu_init`, because the boot page tables identity
     * map it - from `start.S` below four gigabytes, and above from the
     * entries added a few lines up. */
    out->phys = (uintptr_t)addr;

    out->width = width;
    out->height = height;
    out->pitch = pitch;

    /*
     * Uncached, not write-combining: this runs before `mmu_init` has
     * programmed the PAT, and the boot page tables name a memory type with
     * different bits at a 2 MB entry. A few kilobytes of boot log is not
     * worth a second encoding to get wrong.
     */
    source = "the loader's, from the multiboot video request, uncached "
             "until the page tables are the kernel's";

    return true;
}

/*
 * `from_loader` again, which is the whole of it: that function maps the
 * firmware's framebuffer into the device window and answers where it
 * landed, and calling it a second time simply maps it a second time.
 *
 * Deliberately not `hal_fb_init`. That would try ramfb when the loader had
 * nothing, and a board reaching here has already been told the loader did.
 */
bool hal_fb_remap(struct fb *out)
{
    return from_loader(out);
}
