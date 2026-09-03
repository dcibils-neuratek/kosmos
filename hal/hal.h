#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What the board provides. One implementation per board, under hal/<board>/.
 *
 * `arch/` is "which CPU are you" and is reimplemented per architecture.
 * `hal/` is "which peripheral do you have" and is one interface with several
 * implementations, because pushing out a character is pushing out a
 * character on any board.
 *
 * Deliberately only what has been needed. Each of these arrived with the
 * milestone that needed it - the interrupt controller and the timer at M1,
 * the display at M6 - and none of it was written ahead of a caller.
 *
 * It still takes its real shape at M2's second half, once there is a second
 * target to compare against. An interface written against a single target is
 * that target's shape wearing generic names, and everything below is written
 * knowing that.
 */

/* The minimum required to have output. Called before anything else. */
void hal_early_init(void);

/* One character out the serial port. Blocks until there is room. */
void hal_putchar(char c);

/* One character in, or HAL_NO_INPUT when none is waiting. Never blocks:
 * a REPL has to stay responsive to a timer tick while it waits, and from M6
 * input has to keep flowing while an application is busy drawing. */
#define HAL_NO_INPUT    (-1)

int hal_getchar(void);

/* Where usable RAM is. One contiguous range is enough for every target so
 * far; a board with holes in its map would need a list, and that is the
 * moment to change this, not before. */
struct memrange {
    unsigned long base;
    unsigned long size;
};

void hal_ram_range(struct memrange *out);

/* The interrupt controller. hal_irq_handle() is called from the IRQ vector:
 * it acknowledges, services and signals end-of-interrupt. */
void hal_irq_init(void);
void hal_irq_handle(void);

/* The tick source. hal_timer_init needs hal_irq_init first. */
void hal_timer_init(unsigned hz);
unsigned long hal_ticks(void);

/*
 * How many deadlines have been missed: the timer came due again before the
 * previous interrupt was serviced, so the tick for it never happened.
 *
 * It means the system fell behind, and it is the difference between a tick
 * count and a clock. Under load the two diverge, and anything measuring
 * elapsed time from ticks is measuring something else. The counter is the
 * clock; ticks are a heartbeat.
 */
unsigned long hal_ticks_missed(void);

/*
 * The display.
 *
 * A linear framebuffer and nothing else. What is above this line is the app
 * server's problem, and `gfx.md` is emphatic that it stays that way: the
 * kernel does not know what a window is, and the board does not know what a
 * pixel means beyond its format.
 *
 * `pitch` is bytes per row and is **not** width * 4. Under QEMU this board
 * chooses the stride itself and deliberately pads it, because a display that
 * hands back exactly width * 4 lets every address calculation in the system
 * be written wrong and still work - right up until the first real board,
 * where the pitch is whatever the firmware felt like. `gfx.md` §19.3 puts
 * this at the top of its list of traps.
 *
 * One format, XRGB8888: a uint32_t per pixel, 0x00RRGGBB. Not negotiable
 * from up here. When a board arrives that cannot do it, that is the moment
 * the interface grows a format field, and not before.
 */
struct fb {
    volatile uint32_t *pixels;
    unsigned width;
    unsigned height;
    unsigned pitch;             /* bytes per row; never assume width * 4 */
};

/*
 * Brings the display up and describes it. False when there is none, which
 * is not an error: a serial-only boot is a legitimate way to run, and the
 * system has to keep working without a screen.
 *
 * The board decides where the pixels live, because that is the one thing the
 * two targets disagree about. Under QEMU the guest points ramfb at memory it
 * chose; on the Pi the firmware answers the mailbox with an address it chose.
 * A caller that supplied the memory would be right on one board and wrong on
 * the other.
 */
bool hal_fb_init(struct fb *out);

/*
 * Brings up a keyboard, if the board has one. False is not an error: input
 * arriving over the serial line is how this system ran until M6 and is how
 * it runs on a board with a cable and no keyboard.
 *
 * There is deliberately no hal_keyboard_getchar. A keyboard is a source of
 * characters and `hal_getchar` is where characters come from, so the board
 * answers from whichever of its sources has one. Nothing above the HAL
 * changes because a keyboard exists.
 */
bool hal_keyboard_init(void);

/*
 * Where the pointer is, in the device's own units.
 *
 * Undecoded on purpose. An absolute pointing device reports in a range of
 * its own choosing - QEMU's tablet is 0 to 32767 on both axes whatever the
 * display happens to be - and the range travels with the position so that
 * whoever knows how big the screen is can do the scaling. The same division
 * `hal_ram_range` and `sysinfo` draw: this layer says what the hardware
 * said, and what it means belongs further up.
 */
struct pointer_state {
    uint32_t x, y;
    uint32_t min_x, max_x;
    uint32_t min_y, max_y;
    uint32_t buttons;               /* bit 0 left, bit 1 right */
    uint32_t moved;                 /* something happened since the last look */
};

/*
 * Brings up a pointing device, if the board has one. False is not an error,
 * exactly as with the keyboard: a machine with a serial cable and no mouse
 * is a legitimate way to run this system and always will be.
 *
 * There is no `hal_pointer_getchar` equivalent - no merging of sources -
 * because unlike characters, a position has only one place it can come from.
 * A second pointing device would be a second thing to choose between, and
 * that choice does not exist until there is a board with two.
 */
bool hal_pointer_init(void);

/*
 * The current position and buttons. False when there is no pointer at all;
 * `moved` distinguishes "nothing has happened" from "it is still there".
 */
bool hal_pointer_poll(struct pointer_state *out);

/*
 * Has an input device raised an interrupt since this was last asked?
 *
 * Not "what happened" - the events are in the device's own queue and are
 * read by whoever wants them, in a thread, in its own time. This is only the
 * fact that there is something, which is what a sleeper needs in order to
 * stop sleeping. Reading it clears it.
 */
bool hal_input_pending(void);

/* The same question without clearing it, for the interrupt path - which
 * wakes the sleeper but must leave the fact for the sleeper to read. */
bool hal_input_pending_peek(void);

/*
 * A block device.
 *
 * The one piece of hardware the filesystem needs. Sectors are 512 bytes
 * because that is the unit virtio counts in, whatever the underlying device
 * reports - the filesystem's own block size is a separate and larger
 * number, and conflating the two is how a driver ends up reading the wrong
 * place on a disk that calls its blocks 4096.
 *
 * Synchronous, and the byte count must be a whole number of sectors. They
 * return false for a device that is not there, for a request past the end of
 * it, and for an error from it. The caller cannot tell those apart and does
 * not need to: all three mean the bytes are not there.
 */
#define HAL_BLK_SECTOR  512u

struct blkdev {
    uint64_t sectors;               /* how many, of HAL_BLK_SECTOR each */
    uint32_t sector_size;
};

bool hal_blk_init(struct blkdev *out);      /* M8; false when there is none */
bool hal_blk_read(uint64_t sector, void *buf, uint32_t bytes);
bool hal_blk_write(uint64_t sector, const void *buf, uint32_t bytes);

/*
 * Is this key down right now?
 *
 * `code` is the keycode the board's keyboard uses, which on this one is
 * Linux's `input-event-codes.h` numbering because that is what virtio-input
 * speaks. Undecoded, like `hal_pointer_poll`'s device units and `sysinfo`'s
 * raw ID registers: this layer says what the hardware said.
 *
 * **This is a departure from what `CLAUDE.md` says about keyboards**, and
 * worth stating rather than sliding past. The rule was that a keyboard is a
 * source of characters and `hal_getchar` is where characters come from, so
 * there was deliberately no second keyboard entry point. That reasoning is
 * still right for characters and it cannot answer this question: "W is
 * still held" is not a character, and no stream of characters expresses it.
 * A key that repeats is not the same as a key that is down - the repeat
 * rate is a setting, and a game walks at whatever rate the frame runs at.
 *
 * The cost is one entry point and a bitmap the driver already had the
 * events for. What it buys is holding a key, which is the whole of moving
 * in a game and half of a modifier in a shortcut.
 */
bool hal_key_held(unsigned code);

/*
 * The next key transition, oldest first, or false when there are none.
 *
 * The companion to `hal_key_held`, and both are needed. The bitmap answers
 * "is it down now", which is what a game asks once a frame; it cannot
 * answer "it was pressed", because a press and its release inside one frame
 * leave the bitmap as they found it. A key you tap would never appear to
 * have been held.
 *
 * `code` is the board's own numbering, undecoded - Linux's
 * `input-event-codes.h` here, because that is what virtio-input speaks.
 * Turning it into a character is `hal_getchar`'s job and turning it into
 * something an application means is the window manager's.
 */
bool hal_key_event(unsigned *code, bool *down);

/*
 * Seconds since 1970, or 0 when this board has no clock.
 *
 * Read-only, and deliberately the whole of it. Setting the time is a
 * different operation with a different question behind it - what is
 * authoritative, this machine or the network - and there is no network.
 *
 * A number, not a date. Decoding it into a year and a month is arithmetic
 * with no hardware in it, so it happens above this layer, in Lua, for the
 * same reason `hal_pointer_poll` reports the device's own units and lets
 * the window manager scale them: this layer says what the hardware said.
 */
unsigned long hal_rtc_seconds(void);

/*
 * A string the firmware was asked to carry, or false when there is none.
 *
 * QEMU takes `-fw_cfg name=opt/kosmos/boot,string=wm`, which is how a
 * machine is told what to do without rebuilding it. `opt/` is the namespace
 * QEMU reserves for exactly this, so nothing here collides with a name QEMU
 * defines itself.
 *
 * Not a kernel command line. There is a device tree with `/chosen/bootargs`
 * in it and parsing one is real work for a facility fw_cfg already
 * provides - and fw_cfg is here because `hal_fb_init` needed it.
 */
bool hal_boot_option(const char *name, char *out, unsigned long max);

#endif /* HAL_H */
