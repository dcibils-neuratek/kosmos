/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The keyboard controller a PC has had since 1984, and the reason a ThinkPad
 * is the right first machine.
 *
 * `docs/targets.md` §6 ends on a question it says nothing else should be
 * planned around - *is the internal keyboard i8042 or USB?* - because the
 * answer moves the first useful milestone from about 2800 lines to about
 * 9000. On a laptop whose keyboard is USB there is **no input at all** until
 * a whole xHCI stack works.
 *
 * A ThinkPad answers it the good way. The keyboard reaches the system as
 * PS/2 through the embedded controller, and the TrackPoint arrives on the
 * same controller's *auxiliary* port - so this one file is a keyboard **and
 * a pointer**, which is the whole of what a desktop needs from a person.
 *
 * **Interrupts and questions both, and it was questions alone until the
 * first real machine.** `hal.h` asks for `hal_getchar` and
 * `hal_pointer_poll`, so this driver read the controller only when asked -
 * fine for a keyboard, and a poor fit for a pointer: the controller holds
 * *one* byte, a TrackPoint sends three a report a hundred times a second,
 * and a look once a scheduler tick leaves four milliseconds between reads.
 * IRQ 1 and 12 now drain it as bytes arrive; the questions still drain too,
 * which costs a port read and keeps the device working if a line is ever
 * masked.
 *
 * Both paths run with interrupts off - a syscall enters with IF clear and
 * nothing on its way re-enables it, and an interrupt gate clears it - which
 * kept them apart while this board scheduled on one core. **It does not any
 * more**, so every way in takes `i8042_lock`: the window manager asking for
 * the pointer on one core while IRQ 12 drains on another would otherwise put
 * two readers on one byte stream and cut packets apart.
 *
 * **One buffer, two devices.** The keyboard and the auxiliary port share
 * port 0x60, and which one a byte came from is a bit in the status register.
 * So there is one drain, called by whichever question arrives first, and it
 * sorts bytes into two queues. Reading only when asked for a character would
 * lose mouse packets and the other way round.
 *
 * ------------------------------------------------------------------------
 * **The auxiliary half sent nothing under QEMU for months, and now it
 * does.** The keyboard half was demonstrated early - `sendkey p w d ret`
 * produced `pwd` at the prompt - while no auxiliary byte ever arrived, and
 * five causes were ruled out one at a time: the handshake was acknowledged,
 * the drain ran and saw keyboard bytes, `info mice` named the PS/2 mouse,
 * `vmport=off` changed nothing, and neither did arming the interrupt bits in
 * the configuration byte.
 *
 * What changed is what the first real machine forced, all at once: the
 * configuration byte no longer carries `CFG_AUX_DISABLE`, both lines are
 * armed *and* unmasked with a handler draining them, and the framing check
 * is strict. With those, `mouse_move` from QEMU's monitor arrives as packets
 * and a click on the Deskbar opens its menu. Which of them QEMU needed has
 * not been isolated, and this says so rather than guessing.
 *
 * What it buys is the reason to write it down: the pointer driver a laptop
 * runs is exercised on every gate - `tools/run_x86.py` boots with no tablet
 * and clicks through it - instead of only on a laptop.
 * ------------------------------------------------------------------------
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"
#include "console.h"
#include "keys.h"
#include "i8042.h"
#include "pc.h"
#include "spinlock.h"

#define DATA        0x60
#define STATUS      0x64
#define COMMAND     0x64

/* Status register. */
#define ST_OUTPUT   0x01            /* a byte is waiting to be read */
#define ST_INPUT    0x02            /* the controller is still busy */
#define ST_AUX      0x20            /* that byte came from the aux port */

/* Controller commands. */
#define CMD_READ_CONFIG   0x20
#define CMD_WRITE_CONFIG  0x60
#define CMD_DISABLE_AUX   0xa7
#define CMD_ENABLE_AUX    0xa8
#define CMD_TEST_AUX      0xa9
#define CMD_SELF_TEST     0xaa
#define CMD_DISABLE_KBD   0xad
#define CMD_ENABLE_KBD    0xae
#define CMD_TO_AUX        0xd4      /* the next byte goes to the aux device */

/* Configuration byte. */
#define CFG_KBD_IRQ       0x01
#define CFG_AUX_IRQ       0x02
#define CFG_KBD_DISABLE   0x10
#define CFG_AUX_DISABLE   0x20
#define CFG_TRANSLATE     0x40      /* hand over set 1 whatever the device sends */

/* Device commands, which go to the device rather than the controller. */
#define DEV_RESET         0xff
#define DEV_DEFAULTS      0xf6
#define DEV_ENABLE        0xf4
#define DEV_ACK           0xfa

/*
 * How long to wait for the controller, in reads of the status port.
 *
 * A number rather than a clock, because this runs before the timer does -
 * the console is brought up before `hal_timer_init` and a keyboard that
 * needed a working timer to initialise could not report that the timer had
 * failed. Generous: a real controller answers in microseconds, and a machine
 * that has none must not hang here.
 */
#define PATIENCE    100000u

static bool present;
static bool aux_present;

/* Every way into the queues below and into the controller's byte stream. */
static struct spinlock i8042_lock = SPINLOCK("i8042");

/*------------------------------------------------------------------------
 * Talking to the controller.
 *----------------------------------------------------------------------*/

static bool wait_writable(void)
{
    unsigned n;

    for (n = 0; n < PATIENCE; n++) {
        if ((pc_in8(STATUS) & ST_INPUT) == 0) {
            return true;
        }
    }

    return false;
}

static bool wait_readable(void)
{
    unsigned n;

    for (n = 0; n < PATIENCE; n++) {
        if ((pc_in8(STATUS) & ST_OUTPUT) != 0) {
            return true;
        }
    }

    return false;
}

static void command(uint8_t c)
{
    if (wait_writable()) {
        pc_out8(COMMAND, c);
    }
}

static bool write_data(uint8_t c)
{
    if (!wait_writable()) {
        return false;
    }

    pc_out8(DATA, c);
    return true;
}

static int read_data(void)
{
    if (!wait_readable()) {
        return -1;
    }

    return (int)pc_in8(DATA);
}

/*------------------------------------------------------------------------
 * What the keyboard said.
 *----------------------------------------------------------------------*/

/*
 * Set 1 is the XT scancode set, and evdev's key codes were taken from it -
 * which is why `hal/keys.c` needs no table of its own for this driver
 * through the whole typing block. `A` is 30 here and `KEY_A` is 30 there
 * because the original PC keyboard sent 0x1E for A.
 *
 * Above 83 the two diverge, and the extended keys - the ones that arrive
 * behind an 0xE0 prefix - never agreed at all. Those are the table below,
 * and it is short because the only ones that matter are the ones a person
 * presses in a shell.
 */
static unsigned extended_code(uint8_t scan)
{
    switch (scan) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x4b: return KEY_LEFT;
    case 0x4d: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4f: return KEY_END;
    case 0x49: return KEY_PAGEUP;
    case 0x51: return KEY_PAGEDOWN;
    case 0x53: return KEY_DELETE;
    case 0x1d: return KEY_RIGHTCTRL;
    /* Windows on this keyboard, Command on an Apple one. See `keys.h`. */
    case 0x5b: return KEY_LEFTMETA;
    case 0x5c: return KEY_RIGHTMETA;
    case 0x1c: return 28;           /* the keypad's enter is still enter */
    default:   return 0;
    }
}

static bool shift;
static bool ctrl;
static bool caps;

/*
 * Super, and whether anything was pressed while it was held.
 *
 * The second is what separates *tapping* the key - which opens the menu -
 * from holding it to make a combination. Without it, Win+Q would open the
 * menu as well as closing the window, because the release still happens.
 */
static bool super;
static bool super_used;

/*
 * Characters waiting to come out of `keyboard_getchar`.
 *
 * One key can be several characters: an arrow becomes the escape sequence a
 * terminal would have sent, which is the same choice `hal/virtio/input.c`
 * makes and for the same reason - one input language above this layer.
 */
static char     pending[8];
static unsigned pending_len;
static unsigned pending_at;

static void queue_sequence(const char *s)
{
    unsigned n = 0;

    while (s[n] != '\0' && n < sizeof(pending)) {
        pending[n] = s[n];
        n++;
    }

    pending_len = n;
    pending_at = 0;
}

/* Characters, for the console. A ring, so a burst is not lost. */
#define CHARS 32

static unsigned char chars[CHARS];
static unsigned chars_head, chars_tail;

static void put_char(int c)
{
    unsigned next = (chars_head + 1) % CHARS;

    if (next == chars_tail) {
        return;                     /* full: the oldest keystroke wins */
    }

    chars[chars_head] = (unsigned char)c;
    chars_head = next;
}

/*
 * Key transitions, for whoever wants keys rather than characters.
 *
 * The window manager holds Control-W and wants to know a key went down
 * rather than what letter it was, so this is the same queue the virtio
 * driver keeps and it means the same thing.
 */
#define KEYQ 32

static struct { uint8_t code; uint8_t down; } keyq[KEYQ];
static unsigned keyq_head, keyq_tail;
static uint32_t held[4];

static void key_transition(unsigned code, bool down)
{
    unsigned next = (keyq_head + 1) % KEYQ;

    if (code < 128) {
        if (down) {
            held[code >> 5] |= (1u << (code & 31));
        } else {
            held[code >> 5] &= ~(1u << (code & 31));
        }
    }

    /*
     * **Full means drop the oldest, and it used to mean drop the newest.**
     *
     * A queue of key transitions is a queue of things that already
     * happened; when it overflows the interesting end is the recent one.
     *
     * Either way a full queue stays full until somebody collects it, and
     * `input_pending` says "there is input" for as long as it does. Nothing
     * collects key *events* at a shell prompt - they exist for the desktop -
     * so on the first real machine `keys=1` held from the first keystroke
     * on. The console server throws them away now when no client has asked
     * for any; `events_wanted` there has the rest.
     */
    if (next == keyq_tail) {
        keyq_tail = (keyq_tail + 1) % KEYQ;
    }

    keyq[keyq_head].code = (uint8_t)code;
    keyq[keyq_head].down = (uint8_t)(down ? 1 : 0);
    keyq_head = next;
}

/*------------------------------------------------------------------------
 * Where the pointer is.
 *
 * **A TrackPoint is relative and `hal_pointer_poll` is absolute**, and that
 * is the one real difference from the virtio tablet this replaces. A tablet
 * reports where it is; a mouse reports how far it moved. So the position
 * lives here, and the range below is this driver's own invention rather than
 * something the hardware said.
 *
 * `hal.h` asks for the device's own units with the range beside them, and
 * that rule is what makes this honest: the window manager scales whatever
 * range it is given, so a made-up one is fine as long as it is *reported*
 * rather than assumed. 32767 is the tablet's, which means the desktop cannot
 * tell the two apart - which is the point.
 *----------------------------------------------------------------------*/

#define RANGE 32767

/*
 * Units per count, and the one number here that should be decided on the
 * machine rather than in this file.
 *
 * Eight put a count at about half a pixel on a 1920-wide screen, and the
 * paragraph that used to be here said it was "fine for a mouse and probably
 * slow for a TrackPoint". It was right, and the first machine to run this
 * measured it: the range is 32767 across 1920 pixels, so seventeen units to
 * the pixel, and a count moved 0.47 of one. The pointer worked and crawled.
 *
 * Thirty-two is about 1.9 pixels a count there, which is the speed a
 * TrackPoint wants. What decides it is the ratio of this to `RANGE`: the
 * window manager maps the whole range onto the screen, so the driver's
 * limits and the screen's edges are reached together and there is no
 * saturation to worry about - crossing the screen is a fixed number of
 * counts however big the panel is.
 *
 * **It is still one number for two devices, and that is the next thing.**
 * A TrackPoint is a strain gauge and a touchpad is a surface; they want
 * different speeds and both want a curve. `pointer` at the prompt sets this
 * one, through the function below, and nothing here can yet tell a
 * TrackPoint's packet from a touchpad's to give them two.
 */
static unsigned pointer_scale = 32;

/*
 * Read and set from userland through `hal_pointer_speed`.
 *
 * Zero asks without changing anything, which is what lets one program both
 * report and set. Clamped rather than validated: a speed of nought is a
 * pointer that cannot move and a huge one is a pointer that crosses the
 * screen on one count, and neither is worth a error path in a device
 * setting somebody is fiddling with to find what they like.
 */
static unsigned pointer_speed_unlocked(unsigned scale)
{
    if (scale > 0) {
        pointer_scale = scale > 512 ? 512 : scale;
    }

    return pointer_scale;
}

static uint32_t pointer_x = RANGE / 2;
static uint32_t pointer_y = RANGE / 2;
static uint32_t buttons;
static bool     pointer_moved;

/* The three bytes of a standard PS/2 packet, as they arrive. */
static uint8_t packet[3];
static unsigned packet_len;

static void move_by(int dx, int dy)
{
    /*
     * Scaled, because a TrackPoint reports a handful of counts per report
     * and the range is fifteen bits. By how much is `pointer_scale` above,
     * which the machine decides rather than this file.
     */
    int32_t nx = (int32_t)pointer_x + dx * (int32_t)pointer_scale;
    int32_t ny = (int32_t)pointer_y - dy * (int32_t)pointer_scale;  /* up is + */

    if (nx < 0) { nx = 0; }
    if (ny < 0) { ny = 0; }
    if (nx > RANGE) { nx = RANGE; }
    if (ny > RANGE) { ny = RANGE; }

    pointer_x = (uint32_t)nx;
    pointer_y = (uint32_t)ny;
}

/*
 * Button changes as this driver decoded them, bounded: the driver's end of a
 * click, where `wm` logs the other end.
 *
 * On the first real machine a press reached the application and its release
 * never did, and the two logs together are what placed the loss: the driver
 * decoded both, the window manager saw both, and the release was dropped
 * after that, from a window's event queue full of keys typed by a serial
 * port that was not there. `hal/pc/uart.c` has that story.
 */
static unsigned button_changes;

/*
 * Bytes thrown away while looking for the start of a packet - the evidence
 * for a pointer that jumps.
 *
 * A stream that loses a byte decodes the next packet wrongly: a movement
 * byte read as the header and the following header read as a delta, which
 * with the sign bits of a byte that was never a header is a jump of
 * hundreds of pixels and a button nobody pressed. The check below then
 * discards whatever cannot be a header until the stream lines up again. So
 * a count that climbs while the pointer moves is a stream losing bytes, and
 * one that stays at zero says the jumps come from somewhere else.
 *
 * The first twenty one at a time, because how far apart they are is the
 * question; after that at each power of ten, so a stream that is badly
 * wrong says so without filling the ring.
 */
static unsigned long resync_dropped;

static void aux_byte(uint8_t b)
{
    int dx, dy;


    /*
     * Bit 3 of the first byte is always set, and that is the only way to
     * find the start of a packet on a stream that may have been joined
     * halfway. A byte that fails it is dropped rather than shifted in,
     * because a mis-framed packet moves the pointer somewhere nobody asked
     * for and the next one would be wrong too.
     */
    /*
     * **Bit 3 set *and* both overflow bits clear**, and the second half of
     * that is what this was missing.
     *
     * Byte 0 of a PS/2 packet is buttons in 0-2, an always-one bit in 3, the
     * X and Y signs in 4 and 5, and the X and Y *overflow* flags in 6 and 7.
     * Overflow means the device moved further than a byte can say, which a
     * hand on a TrackPoint does not do - so on a real stream those two bits
     * are zero, and requiring them to be zero is what tells a first byte
     * from a byte that merely looks like one.
     *
     * Testing bit 3 alone accepts almost every *negative* delta: -3 is 0xfd,
     * -5 is 0xfb, -2 is 0xfe, and all of them have bit 3 set. Measured on
     * the first real machine, that is exactly what happened - the log is
     * full of `packet0 fd`, `packet0 fb`, `packet0 fe`, which are movement
     * bytes decoded as headers. The low bits of a delta then read as button
     * state, so the pointer reported presses nobody made and releases that
     * never came, and the stream never resynchronised because the rule that
     * was supposed to resynchronise it matched the wrong bytes.
     */
    if (packet_len == 0 && ((b & 0x08) == 0 || (b & 0xc0) != 0)) {
        resync_dropped++;

        if (resync_dropped <= 20 || resync_dropped == 100
            || resync_dropped == 1000 || resync_dropped == 10000) {
            kputs("i8042 resync: dropped byte ");
            kputx(b, 2);
            kputs(", ");
            kputu(resync_dropped);
            kputs(" so far\n");
        }

        return;
    }

    packet[packet_len++] = b;

    if (packet_len < 3) {
        return;
    }

    packet_len = 0;

    /* Overflow means the device moved further than a byte can say. The
     * movement is unusable; the buttons in the same packet are not. */
    dx = (packet[0] & 0x40) ? 0 : (int)packet[1] - ((packet[0] & 0x10) ? 256 : 0);
    dy = (packet[0] & 0x80) ? 0 : (int)packet[2] - ((packet[0] & 0x20) ? 256 : 0);

    if (dx != 0 || dy != 0) {
        move_by(dx, dy);
        pointer_moved = true;
    }

    {
        uint32_t was = buttons;

        buttons = (uint32_t)(packet[0] & 0x01)          /* left  */
                | (uint32_t)((packet[0] & 0x02) >> 0);  /* right */

        if (buttons != was) {
            pointer_moved = true;

            /*
             * Both ends of a click, because a click that half-arrives has to
             * be traced to the layer that lost it - see `button_changes`.
             */
            if (button_changes < 20) {
                button_changes++;
                kputs("i8042 buttons ");
                kputx(buttons, 2);
                kputs(" from packet0 ");
                kputx(packet[0], 2);
                kputc('\n');
            }
        }
    }
}

/*------------------------------------------------------------------------
 * The drain, which is where both devices are actually read.
 *----------------------------------------------------------------------*/

static void kbd_byte(uint8_t b)
{
    static bool escaped;
    unsigned code;
    bool down;
    const char *sequence;
    int c;

    if (b == 0xe0) {
        escaped = true;
        return;
    }

    down = (b & 0x80) == 0;
    b &= 0x7f;

    if (escaped) {
        escaped = false;
        code = extended_code(b);

        if (code == 0) {
            return;
        }
    } else {
        code = b;
    }

    key_transition(code, down);

    switch (code) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT: shift = down; return;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:  ctrl = down;  return;
    case KEY_CAPSLOCK:   if (down) { caps = !caps; } return;

    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        if (down) {
            super = true;
            super_used = false;
        } else {
            /* Tapped, so it meant itself: the menu. Held and used, so the
             * combination has already been sent and this release says
             * nothing. */
            if (super && !super_used) {
                queue_sequence(hal_key_super(0));

                for (c = 0; c < (int)pending_len; c++) {
                    put_char((unsigned char)pending[c]);
                }

                pending_len = 0;
            }

            super = false;
        }
        return;

    default: break;
    }

    if (!down) {
        return;
    }

    sequence = hal_key_sequence(code);

    if (sequence != NULL) {
        unsigned i;

        queue_sequence(sequence);

        for (i = 0; i < pending_len; i++) {
            put_char((unsigned char)pending[i]);
        }

        pending_len = 0;
        return;
    }

    c = hal_key_char(code, shift, ctrl, caps);

    if (c < 0) {
        return;
    }

    /*
     * Held with Super, so it is a command rather than a character. The
     * whole sequence goes out in place of the letter, and the key is marked
     * as used so the eventual release does not also open the menu.
     */
    if (super) {
        unsigned i;

        super_used = true;
        queue_sequence(hal_key_super(c));

        for (i = 0; i < pending_len; i++) {
            put_char((unsigned char)pending[i]);
        }

        pending_len = 0;
        return;
    }

    put_char(c);
}

static void drain(void)
{
    unsigned n;

    if (!present) {
        return;
    }

    /*
     * Bounded, because this is called from `hal_getchar` and a controller
     * that answered for ever would be a machine that never got back to the
     * shell. Thirty-two bytes is ten keystrokes or ten mouse packets, which
     * is more than arrives between two looks.
     */
    for (n = 0; n < 32; n++) {
        uint8_t status = pc_in8(STATUS);
        uint8_t b;

        if ((status & ST_OUTPUT) == 0) {
            return;
        }

        b = pc_in8(DATA);

        if ((status & ST_AUX) != 0) {
            if (aux_present) {
                aux_byte(b);
            }
        } else {
            kbd_byte(b);
        }
    }
}

/*------------------------------------------------------------------------
 * Bringing it up.
 *----------------------------------------------------------------------*/

static bool aux_command(uint8_t c)
{
    command(CMD_TO_AUX);

    if (!write_data(c)) {
        return false;
    }

    return read_data() == DEV_ACK;
}

static bool keyboard_init_unlocked(void)
{
    int config;

    /*
     * Is there a controller at all? A machine with none reads 0xFF from
     * every port, and every bit of the status register set includes the one
     * that says "still busy" - so the wait below would time out and this
     * would answer false, which is what `hal.h` asks for. Checking first
     * makes that a decision rather than a timeout.
     */
    if (pc_in8(STATUS) == 0xff) {
        return false;
    }

    command(CMD_DISABLE_KBD);
    command(CMD_DISABLE_AUX);

    /* Whatever was in the buffer belongs to whoever ran before us. */
    while ((pc_in8(STATUS) & ST_OUTPUT) != 0) {
        (void)pc_in8(DATA);
    }

    command(CMD_READ_CONFIG);
    config = read_data();

    if (config < 0) {
        return false;
    }

    /*
     * Translation on, both ports enabled, both interrupts armed.
     *
     * Translation because it makes the device's set 2 arrive as set 1, which
     * is the set evdev's numbers came from and therefore the one
     * `hal/keys.c` already understands.
     */
    /*
     * **`CFG_AUX_DISABLE` too, and it was not cleared here before.**
     *
     * `CMD_ENABLE_AUX` clears it in the controller, and `i8042_pointer_init`
     * sends that - so in principle the order is fine. In practice this
     * writes the whole config byte with bit 5 still set, and a controller
     * that honours the byte over the command is a controller whose
     * auxiliary device never says anything. It is one bit, it is what the
     * datasheet describes, and the machine it matters on reports exactly
     * that symptom.
     */
    /*
     * **Interrupts on, and this driver used to turn them off.**
     *
     * The reason it did was true when it was written: a controller raising
     * IRQ 1 into a system with no handler is a machine that stops. There is a handler now - `i8042_interrupt` drains
     * on lines 1 and 12, and both `pic.c` and `apic.c` route them here - so
     * the reason expired and the cost did not.
     *
     * **The cost is lost bytes.** The i8042 has a *one-byte* output buffer.
     * Polling it once per scheduler tick leaves four milliseconds between
     * looks, and a mouse reporting a hundred times a second puts three bytes
     * through that buffer in about one. Bytes are dropped, a dropped byte
     * shifts every packet after it, and the framing check above then has to
     * recover a stream that is permanently one byte out.
     *
     * The polled drains stay exactly where they were. They cost a port read
     * when there is nothing to fetch and they are what keeps this working if
     * a line is ever masked, so this is belt and braces rather than a change
     * of mechanism.
     */
    config &= ~(CFG_KBD_DISABLE | CFG_AUX_DISABLE);
    config |= CFG_KBD_IRQ | CFG_AUX_IRQ;
    config |= CFG_TRANSLATE;

    command(CMD_WRITE_CONFIG);

    if (!write_data((uint8_t)config)) {
        return false;
    }

    command(CMD_ENABLE_KBD);

    /* The keyboard's line. The auxiliary port's is opened by
     * `i8042_pointer_init`, which is the only thing that knows whether
     * there is a device on it. */
    pc_irq_unmask(1);

    present = true;
    return true;
}

static bool pointer_init_unlocked(void)
{
    if (!present) {
        return false;
    }

    command(CMD_ENABLE_AUX);


    /*
     * Reset, then defaults, then enable reporting - and the reset is what
     * separates a machine with a TrackPoint from one without. A device that
     * is there answers 0xFA, then 0xAA (self test passed) and its id; one
     * that is not leaves the port silent and `read_data` times out.
     */
    if (!aux_command(DEV_RESET)) {
        return false;
    }

    (void)read_data();              /* 0xAA, self test */
    (void)read_data();              /* the device id */

    if (!aux_command(DEV_DEFAULTS)) {
        return false;
    }

    if (!aux_command(DEV_ENABLE)) {
        return false;
    }

    aux_present = true;

    /* IRQ 12 is the auxiliary port's, on the slave controller - `pic.c`
     * opens the cascade for anything above seven. */
    pc_irq_unmask(12);

    return true;
}

/*------------------------------------------------------------------------
 * The questions `hal.h` asks.
 *----------------------------------------------------------------------*/

static int getchar_unlocked(void)
{
    drain();

    if (chars_head == chars_tail) {
        return HAL_NO_INPUT;
    }

    {
        int c = (int)chars[chars_tail];

        chars_tail = (chars_tail + 1) % CHARS;
        return c;
    }
}

bool i8042_present(void)
{
    return present;
}

static bool key_event_unlocked(unsigned *code, bool *down)
{
    drain();

    if (keyq_head == keyq_tail) {
        return false;
    }

    *code = keyq[keyq_tail].code;
    *down = keyq[keyq_tail].down != 0;
    keyq_tail = (keyq_tail + 1) % KEYQ;

    return true;
}

bool i8042_key_held(unsigned code)
{
    if (code >= 128) {
        return false;
    }

    return (held[code >> 5] & (1u << (code & 31))) != 0;
}

static bool pointer_poll_unlocked(struct pointer_state *out)
{
    if (!aux_present) {
        return false;
    }

    drain();

    out->x = pointer_x;
    out->y = pointer_y;
    out->min_x = 0;
    out->max_x = RANGE;
    out->min_y = 0;
    out->max_y = RANGE;
    out->buttons = buttons;
    out->moved = pointer_moved ? 1u : 0u;

    pointer_moved = false;

    return true;
}

/*
 * Whether anything has arrived, which is what the desktop asks before it
 * decides to sleep.
 *
 * `_peek` does not consume and the other does not either - on a polled
 * controller there is nothing to consume, because the bytes are still in the
 * chip until somebody reads them. The virtio driver's pair differ because
 * one clears an interrupt flag; here they are the same question.
 */
static bool pending_unlocked(void)
{
    if (!present) {
        return false;
    }

    drain();

    return chars_head != chars_tail || keyq_head != keyq_tail
        || pointer_moved;
}

bool i8042_input_pending(void)
{
    return i8042_input_pending_peek();
}

/*
 * The interrupt half, and since the first real machine the half that
 * matters.
 *
 * `hal/pc/pic.c` offers every line to every driver because PCI interrupts
 * are shared and the number alone does not say who raised one, so this
 * answers only its own: 1 for the keyboard, 12 for the auxiliary port. A
 * byte is drained as soon as the controller has it, into the same queues
 * the questions above read.
 *
 * Interrupts are off in here and off in every syscall that drains, which on
 * one core was enough to keep the two apart. On several it is the lock, and
 * the note at the top says why.
 */
static void interrupt_unlocked(unsigned line)
{
    if (line == 1 || line == 12) {
        drain();
    }
}

/*
 * The ways in, each taking the lock around the unlocked version above.
 * `drain` prints on a resync and for the click probe, so the console's lock
 * nests inside this one - and never the other way round: the only path into
 * this file from a system call, `SYS_GETCHAR`, holds no lock.
 */
int i8042_getchar(void)
{
    unsigned long flags = spin_lock(&i8042_lock);
    int c = getchar_unlocked();

    spin_unlock(&i8042_lock, flags);
    return c;
}

bool i8042_key_event(unsigned *code, bool *down)
{
    unsigned long flags = spin_lock(&i8042_lock);
    bool got = key_event_unlocked(code, down);

    spin_unlock(&i8042_lock, flags);
    return got;
}

bool i8042_pointer_poll(struct pointer_state *out)
{
    unsigned long flags = spin_lock(&i8042_lock);
    bool got = pointer_poll_unlocked(out);

    spin_unlock(&i8042_lock, flags);
    return got;
}

bool i8042_input_pending_peek(void)
{
    unsigned long flags = spin_lock(&i8042_lock);
    bool pending = pending_unlocked();

    spin_unlock(&i8042_lock, flags);
    return pending;
}

void i8042_interrupt(unsigned line)
{
    unsigned long flags = spin_lock(&i8042_lock);

    interrupt_unlocked(line);
    spin_unlock(&i8042_lock, flags);
}

unsigned i8042_pointer_speed(unsigned scale)
{
    unsigned long flags = spin_lock(&i8042_lock);
    unsigned speed = pointer_speed_unlocked(scale);

    spin_unlock(&i8042_lock, flags);
    return speed;
}

/*
 * **Once, and after that a question.** `SYS_SYSINFO` asks
 * `hal_keyboard_init` whether there is a keyboard every time anybody asks
 * for system information - Processes, Monitor and the top bar, several
 * times a second between them - and its comment called that idempotent.
 * On this chip it was not: every call disabled both ports, read and threw
 * away whatever was waiting, and rewrote the configuration byte.
 *
 * So each refresh dropped whatever the mouse was half-way through sending,
 * and the driver resynchronised by discarding bytes until a packet lined up
 * again - a pointer that jumps, on the laptop, whenever those windows were
 * open. And with threads spread across cores it was worse: the sequence ran
 * on one core while IRQ 12 drained the same port on another, the drain took
 * the configuration byte the sequence was waiting for, and a garbage
 * configuration went back - after which the mouse said nothing at all.
 *
 * Both now answer from what they found the first time, under the lock.
 */
bool i8042_keyboard_init(void)
{
    unsigned long flags = spin_lock(&i8042_lock);
    bool ok = present || keyboard_init_unlocked();

    spin_unlock(&i8042_lock, flags);
    return ok;
}

bool i8042_pointer_init(void)
{
    unsigned long flags = spin_lock(&i8042_lock);
    bool ok = aux_present || pointer_init_unlocked();

    spin_unlock(&i8042_lock, flags);
    return ok;
}
