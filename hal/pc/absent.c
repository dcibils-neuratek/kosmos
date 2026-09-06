/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The devices this board does not have yet.
 *
 * **Every one of these is inside the HAL contract rather than around it.**
 * `hal.h` says so at each of them: "false when there is none, which is not
 * an error - a serial-only boot is a legitimate way to run, and the system
 * has to keep working without a screen", and the same sentence for the
 * keyboard, the pointer, the disk, the card and the sound device. That path
 * is not hypothetical on ARM either: `tools/run_headless.py` exists because
 * a machine with nothing plugged into it booted to a prompt that never
 * appeared, and it now boots to one on every run of `make test`.
 *
 * So a PC that says no to all of them is a machine this system already
 * knows how to be. What it is not is a machine worth using, and that is the
 * point of this file existing as one file: **it shrinks.** Each driver that
 * arrives takes its functions out of here and into `hal/pc/<device>.c`,
 * next to its ARM counterpart in `hal/qemu-virt/`, and what is left is the
 * honest list of what is still missing. A stub per file would have hidden
 * that behind a constant file count.
 *
 * Nothing here pretends. There is no framebuffer that draws nowhere and no
 * disk that swallows writes: a caller is told no, once, and takes the
 * branch it already has.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"

/* ---- The display ------------------------------------------------- */

/*
 * QEMU offers this machine three ways to have one - ramfb, which the ARM
 * board uses and which needs fw_cfg; virtio-gpu, which needs an explicit
 * flush and is what will finally grow `hal_fb_flush`; and the VGA and VESA
 * modes a PC has had since 1987. Which of those to take is the next real
 * decision on this board and `docs/hal.md` will record it.
 */
bool hal_fb_init(struct fb *out)
{
    (void)out;

    return false;
}

/* ---- Keyboard and pointer ----------------------------------------- */

/*
 * A PS/2 keyboard is two I/O ports and IRQ 1, and is the smallest of these
 * by a wide margin - which is why it is likely to be first. `hal.h` is
 * deliberate that there is no `hal_keyboard_getchar`: a keyboard is a
 * source of characters and `hal_getchar` is where characters come from, so
 * this board answers from the UART today and from either tomorrow, with
 * nothing above the HAL changing.
 */
bool hal_keyboard_init(void)
{
    return false;
}

bool hal_key_event(unsigned *code, bool *down)
{
    (void)code;
    (void)down;

    return false;
}

bool hal_input_pending(void)
{
    return false;
}

/*
 * The same question asked from inside an interrupt handler, where it must
 * not consume anything - the kernel is deciding whether to wake a sleeper,
 * not reading a key. There is nothing to peek at on this board yet.
 */
bool hal_input_pending_peek(void)
{
    return false;
}

bool hal_key_held(unsigned code)
{
    (void)code;

    return false;
}

bool hal_pointer_init(void)
{
    return false;
}

bool hal_pointer_poll(struct pointer_state *out)
{
    (void)out;

    return false;
}

/* ---- The disk ------------------------------------------------------ */

/*
 * virtio-blk is the same device QEMU gives the ARM board, so
 * `hal/qemu-virt/blk.c` and the virtio queue machinery beside it should
 * move across nearly whole. What does not move is how the device is
 * *found*: there it is an MMIO window the device tree describes, and here
 * it is behind PCI configuration space. That difference is the shape of
 * every remaining driver on this board.
 */
bool hal_blk_init(struct blkdev *out)
{
    (void)out;

    return false;
}

bool hal_blk_read(uint64_t sector, void *buf, uint32_t bytes)
{
    (void)sector;
    (void)buf;
    (void)bytes;

    return false;
}

bool hal_blk_write(uint64_t sector, const void *buf, uint32_t bytes)
{
    (void)sector;
    (void)buf;
    (void)bytes;

    return false;
}

/* ---- The network --------------------------------------------------- */

bool hal_net_init(struct netdev *out)
{
    (void)out;

    return false;
}

bool hal_net_present(void)
{
    return false;
}

bool hal_net_info(struct netdev *out)
{
    (void)out;

    return false;
}

bool hal_net_send(const void *frame, unsigned bytes)
{
    (void)frame;
    (void)bytes;

    return false;
}

int hal_net_recv(void *frame, unsigned max)
{
    (void)frame;
    (void)max;

    return 0;
}

/* ---- Sound --------------------------------------------------------- */

/*
 * The last of these to matter and the one with the deadline on it.
 * `CLAUDE.md` records what the audio path cost to get right on the other
 * board - a period every 5.8 ms, a shared ring rather than a message, and
 * a server in C because a collector pause is longer than the deadline. All
 * of that is above this line and none of it has to be learned twice.
 */
bool hal_snd_init(void)
{
    return false;
}

bool hal_snd_present(void)
{
    return false;
}

bool hal_snd_write(const void *pcm, unsigned bytes)
{
    (void)pcm;
    (void)bytes;

    return false;
}

unsigned hal_snd_queued(void)
{
    return 0;
}

unsigned hal_snd_wakes(void)
{
    return 0;
}

unsigned hal_snd_dry(void)
{
    return 0;
}

unsigned hal_snd_floor(void)
{
    return 0;
}

/*
 * Whether the device is asking for a period, which the interrupt path asks
 * on every tick. A board with no sound device never wants one, and the
 * process that would have been woken does not exist.
 */
bool hal_snd_wants(void)
{
    return false;
}

const char *hal_input_describe(void)
{
    return "No PCI bus walked yet; nothing here but the serial line.";
}
