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
 * **Polled, not interrupt-driven, and that is a decision rather than a
 * shortcut.** `hal.h` asks for `hal_getchar` and `hal_pointer_poll`, both of
 * which are questions rather than announcements; the virtio driver uses
 * interrupts because a virtqueue is asynchronous by construction, and this
 * one does not because a controller with two bytes of buffer is not. It also
 * means the first hardware boot needs no APIC and no interrupt routing at
 * all, which moves an entire milestone after the picture instead of before
 * it.
 *
 * **One buffer, two devices.** The keyboard and the auxiliary port share
 * port 0x60, and which one a byte came from is a bit in the status register.
 * So there is one drain, called by whichever question arrives first, and it
 * sorts bytes into two queues. Reading only when asked for a character would
 * lose mouse packets and the other way round.
 *
 * ------------------------------------------------------------------------
 * **This is not in the build yet, and here is exactly where it got to.**
 *
 * The keyboard half *works*, demonstrated rather than assumed: with the x86
 * board switched to this driver and the virtio devices detached, QEMU's
 * `sendkey p w d ret` produced `pwd`, a newline and `/` at the prompt, and
 * `sendkey up` walked the shell's history and re-ran it. So the controller
 * handshake, scancode set 1, the shared keymap and the escape sequences for
 * the arrows are all right.
 *
 * The auxiliary half sends nothing under QEMU, and the reason is not known.
 * What is known, from instrumenting the drain:
 *
 *   - the handshake succeeds - `0xA8` leaves the configuration byte at 0x40,
 *     which is translation on with both ports enabled, and `0xFF`, `0xF6`
 *     and `0xF4` are all acknowledged;
 *   - the drain does run and does see keyboard bytes, with the status
 *     register's auxiliary bit clear as it should be for those;
 *   - no auxiliary byte ever arrives, whether the movement is sent with
 *     `input-send-event` or the monitor's own `mouse_move`;
 *   - `info mice` names the PS/2 mouse as current, and `vmport=off` - the
 *     obvious suspect, since q35 carries a vmmouse - changes nothing.
 *
 * So the board stays on virtio input until that is understood, because a
 * desktop with no pointer is not a desktop and the display harness says so
 * in eleven checks. **What is not blocked is the machine this is for**: a
 * TrackPoint is a real PS/2 device rather than an emulated one, and the
 * keyboard - the half that decides whether a laptop can be typed on at all -
 * is the half that works.
 * ------------------------------------------------------------------------
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"
#include "keys.h"
#include "pc.h"

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
    case 0x1c: return 28;           /* the keypad's enter is still enter */
    default:   return 0;
    }
}

static bool shift;
static bool ctrl;
static bool caps;

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

    if (next == keyq_tail) {
        return;
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
 * Eight puts a count at about half a pixel on a 1920-wide screen, which is
 * fine for a mouse and probably slow for a TrackPoint - those send a lot of
 * counts when pushed hard, and every other system applies an acceleration
 * curve on top. There is none here yet, deliberately: a curve tuned against
 * an emulated mouse would be a curve tuned against the wrong device.
 */
#define SCALE 8

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
     * and the range is fifteen bits. Sixty-four is a feel rather than a
     * measurement, and it is the one number in this file that should be
     * decided on the machine rather than here.
     */
    int32_t nx = (int32_t)pointer_x + dx * SCALE;
    int32_t ny = (int32_t)pointer_y - dy * SCALE;   /* up is positive on PS/2 */

    if (nx < 0) { nx = 0; }
    if (ny < 0) { ny = 0; }
    if (nx > RANGE) { nx = RANGE; }
    if (ny > RANGE) { ny = RANGE; }

    pointer_x = (uint32_t)nx;
    pointer_y = (uint32_t)ny;
}

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
    if (packet_len == 0 && (b & 0x08) == 0) {
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

    if (c >= 0) {
        put_char(c);
    }
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

bool hal_keyboard_init(void)
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
     * Interrupts off and translation on.
     *
     * Off because this driver is polled - see the note at the top - and a
     * controller raising IRQ 1 into a system with no handler for it is a
     * machine that stops. Translation on because it makes the device's set 2
     * arrive as set 1, which is the set evdev's numbers came from and
     * therefore the one `hal/keys.c` already understands.
     */
    config &= ~(CFG_KBD_IRQ | CFG_AUX_IRQ | CFG_KBD_DISABLE);
    config |= CFG_TRANSLATE;

    command(CMD_WRITE_CONFIG);

    if (!write_data((uint8_t)config)) {
        return false;
    }

    command(CMD_ENABLE_KBD);

    present = true;
    return true;
}

bool hal_pointer_init(void)
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
    return true;
}

/*------------------------------------------------------------------------
 * The questions `hal.h` asks.
 *----------------------------------------------------------------------*/

int keyboard_getchar(void)
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

bool keyboard_present(void)
{
    return present;
}

bool hal_key_event(unsigned *code, bool *down)
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

bool hal_key_held(unsigned code)
{
    if (code >= 128) {
        return false;
    }

    return (held[code >> 5] & (1u << (code & 31))) != 0;
}

bool hal_pointer_poll(struct pointer_state *out)
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
bool hal_input_pending_peek(void)
{
    if (!present) {
        return false;
    }

    drain();

    return chars_head != chars_tail || keyq_head != keyq_tail || pointer_moved;
}

bool hal_input_pending(void)
{
    return hal_input_pending_peek();
}

/*
 * The interrupt half, which this driver does not use and still has to have.
 *
 * `hal/pc/pic.c` offers every line to every driver because PCI interrupts
 * are shared and the number alone does not say who raised one - so the
 * symbol has to exist even for a device that asked not to be interrupted.
 *
 * **It is a drain rather than an empty function.** IRQ 1 and 12 are masked
 * in the configuration byte above, so nothing should arrive; if something
 * does - a firmware that left them enabled, or the day this driver stops
 * polling - reading the bytes is the right answer and dropping them is not.
 * The queues are the same ones the polled path fills.
 */
void input_interrupt(unsigned line)
{
    if (line == 1 || line == 12) {
        drain();
    }
}
