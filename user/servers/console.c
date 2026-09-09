/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The console: the one process that may touch the keyboard and the wire.
 *
 * `sys.write`, `sys.getchar`, `sys.key_event`, `sys.pointer` and
 * `sys.wait_input` are refused to every other process, and that is what
 * makes this a server rather than a convention - a client cannot decide to
 * print directly, because the machine will not let it. Everything the system
 * says and everything a person types crosses this loop.
 *
 * **`read` does not answer when it is asked**, and that is the one real
 * difference between this and the four servers converted before it. A line
 * takes as long as somebody takes to type it.
 *
 * The Lua console blocked inside the handler and pumped its own mailbox from
 * in there - serving other callers while one waited. It worked, and it cost
 * a re-entrant server and a `sys.yield` spin, because there is no UART
 * interrupt to park on: a process waiting for a line was a runnable thread
 * going round a loop.
 *
 * This one holds the sender instead. `read` records who asked and returns to
 * the loop; the loop polls the keyboard and answers messages in the same
 * pass, and when the line is finished it replies to whoever has been
 * waiting. Nothing re-enters, and the wait costs a receive with a deadline
 * rather than a spin.
 *
 * **One reader at a time**, which is a rule and not a limitation: there is
 * one keyboard, two readers would split a line between them, and a second
 * client asking for one is a bug in that client rather than a queue to
 * manage.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "conproto.h"

/*
 * How long the loop may sleep in `receive` while a line is half-typed.
 *
 * One tick, which at 250 Hz is 4 ms. It is a poll rather than a wait because
 * there is no interrupt for the serial line - `hal_getchar` asks the device
 * - so somebody has to look. A tick is far below the interval at which a
 * person produces characters and far above the cost of looking.
 *
 * It applies only while a `read` is outstanding. With nobody waiting for a
 * line the loop blocks with no deadline at all, so an idle machine is idle.
 */
#define READ_POLL_TICKS   1UL

/*
 * What has been typed but not yet asked for.
 *
 * `poll` is the reason it exists. A program that runs for a while asks
 * whether Control-C was pressed, which means reading the keyboard, which
 * means everything else typed in the same breath would be thrown away if it
 * were not kept. The shell's next `read` starts with these.
 */
static uint8_t typed[CON_KEYS_MAX];
static unsigned n_typed;

/* The line being edited, and who is waiting for it. */
static char     line[CON_TEXT_MAX];
static unsigned line_len;
static bool     reading;
static uint64_t reader;

static uint32_t n_bytes, n_lines, n_interrupts;


/*
 * `colour` is 0xAARRGGBB and zero is the console's own, which is what every
 * write that has no opinion sends. Handed straight to the kernel rather than
 * interpreted here: this server owns the console, it does not own a palette,
 * and deciding what "red" is belongs to whoever is showing it to a person.
 */
static void put(const char *s, size_t n, unsigned long colour)
{
    (void)kosmos_write_colour(s, n, colour);
}

/*------------------------------------------------------------------------
 * What was typed before.
 *
 * A prompt without this is one you can demonstrate and not one you can work
 * at: every other verb on the machine saves you a workaround once, and this
 * saves you retyping everything you have already run.
 *
 * **A ring of whole lines, and no file.** Thirty-two of them at
 * `CON_TEXT_MAX` is 32 KB in a server that already refuses to allocate, and
 * it goes when the machine does - which is the right lifetime for it, since
 * a history that outlived the boot would be state this server has nowhere
 * to keep and no business owning.
 *
 * `recall` is where the up-arrow has walked to: 0 is the line being typed,
 * 1 is the most recent, and it is reset by anything that ends a line. That
 * is what makes up-up-down land where a person expects rather than where a
 * cursor happens to be.
 *
 * Both sources already speak the same language, which is why this is one
 * implementation rather than two: a serial terminal sends ESC [ A, and
 * `hal/virtio/input.c` maps the keyboard's own up-arrow to the same three
 * bytes on the way in.
 *----------------------------------------------------------------------*/

#define HISTORY  32u

static char     past[HISTORY][CON_TEXT_MAX];
static unsigned past_len[HISTORY];
static unsigned past_n;            /* how many are kept, up to HISTORY */
static unsigned past_next;         /* where the next one goes */
static unsigned recall;            /* 0 is the line being typed */

/*
 * The `k`th line back, newest first. NULL past the end.
 */
static const char *nth_past(unsigned k, unsigned *len)
{
    unsigned at;

    if (k == 0 || k > past_n) {
        return NULL;
    }

    at = (past_next + HISTORY - k) % HISTORY;
    *len = past_len[at];

    return past[at];
}

/*
 * Keep a line, unless it is empty or the one before it.
 *
 * Consecutive duplicates are dropped because the up-arrow is for finding
 * something, and a history of `ls` eleven times is a history you have to
 * walk past rather than one that helps.
 */
static void remember(const char *text, unsigned len)
{
    unsigned last_len = 0;
    const char *last;

    if (len == 0) {
        return;
    }

    last = nth_past(1, &last_len);

    if (last != NULL && last_len == len && memcmp(last, text, len) == 0) {
        return;
    }

    memcpy(past[past_next], text, len);
    past_len[past_next] = len;

    past_next = (past_next + 1) % HISTORY;

    if (past_n < HISTORY) {
        past_n++;
    }
}

/*
 * Put a remembered line on the screen in place of whatever is there.
 *
 * Erasing is `\b \b` per character, the same three bytes backspace already
 * uses, because that is the only way to unprint on a terminal that has no
 * idea it is one - and it is what makes this work identically on the serial
 * line and on a framebuffer that has never heard of a cursor address.
 */
static void recall_to(unsigned k)
{
    const char *text;
    unsigned len = 0, i;

    if (k > past_n) {
        return;                    /* already at the oldest; stay there */
    }

    text = nth_past(k, &len);

    for (i = 0; i < line_len; i++) {
        put("\b \b", 3, 0);
    }

    line_len = 0;
    recall = k;

    if (text == NULL) {
        return;                    /* k is 0: back to an empty line */
    }

    memcpy(line, text, len);
    line_len = len;
    put(line, len, 0);
}

static void stash(uint8_t c)
{
    if (n_typed < CON_KEYS_MAX) {
        typed[n_typed++] = c;
    }
}

/*
 * The next byte from anywhere, or -1.
 *
 * The stash first, in the order it was typed, and only then the device.
 * The other way round would deliver what somebody typed during a benchmark
 * *after* what they typed at the prompt afterwards.
 */
static int next_byte(void)
{
    long c;

    if (n_typed > 0) {
        int out = typed[0];

        memmove(typed, typed + 1, n_typed - 1);
        n_typed--;
        return out;
    }

    c = kosmos_getchar();
    return (c < 0) ? -1 : (int)(c & 0xff);
}

static void reply_with(uint64_t to, const struct con_reply *rep)
{
    struct message out;

    memset(&out, 0, sizeof(out));
    out.length = sizeof(*rep);
    memcpy(out.data, rep, sizeof(*rep));

    (void)kosmos_reply(to, &out);
}

static void fail(uint64_t to, uint32_t code)
{
    struct con_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.error = code;
    reply_with(to, &rep);
}

/*
 * Finish the line and hand it to whoever asked.
 */
static void deliver(void)
{
    struct con_reply rep;

    memset(&rep, 0, sizeof(rep));
    rep.length = line_len;
    memcpy(rep.line, line, line_len);

    remember(line, line_len);

    reading  = false;
    line_len = 0;
    recall   = 0;

    reply_with(reader, &rep);
}

/*
 * One pass of the line editor, run from the loop rather than from a handler.
 *
 * Returns having consumed everything waiting: a paste arrives as a burst,
 * and taking one byte per tick would draw it at 250 characters a second.
 */
static void edit(void)
{
    /*
     * Where an arrow key has got to. Three bytes arrive one at a time
     * through `next_byte`, so the sequence is a state rather than a peek:
     * ESC, then `[` or `O` - terminals disagree about which - then a
     * letter. Anything else abandons it, which is what makes a lone ESC
     * harmless instead of swallowing the character after it.
     */
    static unsigned esc;

    for (;;) {
        int c = next_byte();

        if (c < 0) {
            return;
        }

        if (esc == 1) {
            esc = (c == '[' || c == 'O') ? 2u : 0u;
            continue;
        }

        if (esc == 2) {
            esc = 0;

            if (c == 'A') {
                recall_to(recall + 1);
            } else if (c == 'B') {
                recall_to(recall > 0 ? recall - 1 : 0);
            }

            /* Left and right arrive here too and are ignored: this editor
             * has no cursor to move, and a line that jumped when you
             * pressed one would be worse than one that did nothing. */
            continue;
        }

        if (c == 27) {
            esc = 1;
            continue;
        }

        if (c == '\n' || c == '\r') {
            put("\n", 1, 0);
            n_lines++;
            deliver();
            return;
        }

        if (c == 3) {
            /* Control-C ends the line as an empty one. The caller sees a
             * blank line rather than an error, which is what a shell wants:
             * print a fresh prompt and carry on. */
            put("^C\n", 3, 0);
            n_interrupts++;
            line_len = 0;
            recall = 0;
            deliver();
            return;
        }

        if (c == 8 || c == 127) {
            if (line_len > 0) {
                line_len--;
                /* Back over it, paint a space, back again: the only way to
                 * unprint a character on a terminal that has no idea it is
                 * one. */
                put("\b \b", 3, 0);
            }

            continue;
        }

        /* Printable only. A stray control byte used to be echoed and put in
         * the line, where it was invisible in the picture and present in the
         * string. */
        if (c >= 32 && c < 127 && line_len < CON_TEXT_MAX) {
            char ch = (char)c;

            line[line_len++] = ch;
            put(&ch, 1, 0);
        }
    }
}

/*
 * Everything typed since anyone last asked, as bytes.
 *
 * The stash goes with them: a character typed just before the window manager
 * started would otherwise be delivered to the shell afterwards.
 */
static void drain_keys(struct con_reply *rep)
{
    for (;;) {
        int c;

        if (rep->nkeys >= CON_KEYS_MAX) {
            return;
        }

        c = next_byte();

        if (c < 0) {
            return;
        }

        if (c == 3) {
            n_interrupts++;
        }

        rep->keys[rep->nkeys++] = (uint8_t)c;
    }
}

/*
 * Whatever key transitions the device has, up to what the reply holds.
 *
 * Its own function because `wait` calls it twice - once before deciding
 * whether to sleep, and once after being woken.
 */
static void drain_key_events(struct con_reply *rep)
{
    unsigned code, down;

    while (rep->nevents < CON_EVENTS_MAX
           && kosmos_key_event(&code, &down) == 0) {
        rep->events[rep->nevents].code = code;
        rep->events[rep->nevents].down = down;
        rep->nevents++;
    }
}

static void fill_pointer(struct con_reply *rep)
{
    struct pointer_info where;

    if (kosmos_pointer(&where) == 0) {
        rep->x       = where.x;
        rep->y       = where.y;
        rep->min_x   = where.min_x;
        rep->max_x   = where.max_x;
        rep->min_y   = where.min_y;
        rep->max_y   = where.max_y;
        rep->buttons = where.buttons;
        rep->moved   = where.moved;
    }
}

static void answer(const struct message *msg, uint64_t sender)
{
    struct con_request req;
    struct con_reply rep;

    if (msg->length < sizeof(req)) {
        fail(sender, CON_ERR_BAD_OP);
        return;
    }

    memcpy(&req, msg->data, sizeof(req));
    memset(&rep, 0, sizeof(rep));

    switch (req.op) {
    case CON_OP_WRITE: {
        uint32_t n = (req.length > CON_TEXT_MAX) ? CON_TEXT_MAX : req.length;

        n_bytes += n;
        put(req.text, n, req.colour);
        break;
    }

    case CON_OP_READ:
        if (reading) {
            fail(sender, CON_ERR_BUSY);
            return;
        }

        /*
         * The one operation that does not answer here.
         *
         * `sender` is kept and the loop replies when the line is done. There
         * is deliberately no reply on this path - a reply now would be an
         * empty line, and the caller is blocked in `call` waiting for a real
         * one.
         */
        reading  = true;
        reader   = sender;
        line_len = 0;

        /* Whatever was typed ahead may already be a whole line, in which
         * case this answers immediately and never reaches the loop. */
        edit();
        return;

    case CON_OP_KEYS:
        drain_keys(&rep);
        break;

    case CON_OP_WAIT: {
        drain_keys(&rep);
        drain_key_events(&rep);

        /*
         * Sleep only when there was nothing, and only here.
         *
         * The window manager used to ask for keys, ask for the pointer, get
         * nothing, yield, and go round again - a thread that is always
         * runnable, which is a core at a hundred per cent on an idle
         * desktop. An input interrupt cuts this short, so a key is noticed
         * at interrupt speed and an idle machine is idle.
         *
         * The cost is that the console answers nobody while it sleeps,
         * bounded by what the caller asked for. That is why it is its own
         * operation rather than something `keys` started doing.
         */
        if (rep.nkeys == 0 && rep.nevents == 0) {
            (void)kosmos_wait_input(req.ticks);

            /*
             * And again, because whatever woke this is the answer.
             *
             * Filling the reply before the sleep and not after meant a key
             * that arrived during it was left for the *next* call: the
             * caller got an empty reply, came straight back, and read it
             * then. One pass late, and invisible, because the wake itself
             * is what makes the next pass happen immediately.
             */
            drain_keys(&rep);
            drain_key_events(&rep);
        }

        /*
         * The pointer last, and this is the bug that made a button stop
         * working.
         *
         * **A key is an event and a position is a state.** Keys queue, so
         * reading them a pass late loses nothing - they are all still
         * there. The pointer does not queue: `kosmos_pointer` answers where
         * it is *now*, and the window manager samples it once per pass. So
         * a reply carrying the state from before the sleep meant every
         * sample was one wake stale, and a press and its release could fall
         * entirely between two samples - the button went down on screen and
         * never came up, `on_click` never fired, and the window looked
         * frozen because nothing else made the manager take another sample.
         *
         * It survived for as long as it did because a wake makes the next
         * pass happen at once, so one stale sample was usually corrected a
         * millisecond later. What broke it was a window whose repaint takes
         * a dozen messages: the release then arrives while the manager is
         * draining those, and the next sample is the one after the pointer
         * has already stopped generating events.
         *
         * Sampled once, here, after the wait, which is the state the caller
         * asked about when it said "wait".
         */
        fill_pointer(&rep);

        break;
    }

    case CON_OP_POINTER: {
        struct pointer_info where;

        if (kosmos_pointer(&where) != 0) {
            fail(sender, CON_ERR_NO_POINTER);
            return;
        }

        rep.x       = where.x;
        rep.y       = where.y;
        rep.min_x   = where.min_x;
        rep.max_x   = where.max_x;
        rep.min_y   = where.min_y;
        rep.max_y   = where.max_y;
        rep.buttons = where.buttons;
        rep.moved   = where.moved;
        break;
    }

    case CON_OP_POLL:
        /*
         * Was Control-C pressed - and keep everything else.
         *
         * The only question a long-running program can ask, because this is
         * the only process that may look. Anything else typed goes to the
         * stash so the next `read` begins with it.
         *
         * **From the device, not from `next_byte`, and that is the whole of
         * this operation's history.** `next_byte` empties the stash before
         * it asks the hardware - which is right for `read`, where the stash
         * is input that arrived early. Here it is a loop that takes a byte
         * out of the stash and puts it straight back: one character typed
         * ahead and this never ends.
         *
         * It spun the console server for ever, so nothing on the machine
         * could print or read again, and it looked like whichever program
         * had called `interrupted` had hung. Nothing found it for months
         * because the callers were shaped so it could not happen: a status
         * bar asks between screens with the keyboard drained, and `httpd`
         * asked once per request after `accept` had blocked - so the test
         * that fetched one page and stopped never reached the second ask.
         * An event loop asking ten times a second hit it on the first pass.
         */
        for (;;) {
            long c = kosmos_getchar();

            if (c < 0) {
                break;
            }

            if ((c & 0xff) == 3) {
                rep.seen = 1;
                n_interrupts++;
            } else {
                stash((uint8_t)(c & 0xff));
            }
        }

        break;

    case CON_OP_STAT:
        rep.bytes      = n_bytes;
        rep.lines      = n_lines;
        rep.interrupts = n_interrupts;
        break;

    default:
        fail(sender, CON_ERR_BAD_OP);
        return;
    }

    reply_with(sender, &rep);
}

void console_server(long endpoint)
{
    for (;;) {
        struct message msg;
        uint64_t sender = 0;
        long got;

        /*
         * A deadline only while somebody is waiting for a line.
         *
         * With no reader there is nothing this loop does on its own, so it
         * blocks until a message arrives and the process is off the
         * runqueue entirely. With a reader it must come back to look at the
         * keyboard, because no interrupt will tell it.
         */
        got = kosmos_receive(endpoint, &msg, &sender, 0,
                             reading ? READ_POLL_TICKS : 0);

        if (got == 0) {
            answer(&msg, sender);
        }

        /* Whether or not a message came, the half-typed line gets a look.
         * A `receive` that timed out is exactly the case this is for. */
        if (reading) {
            edit();
        }
    }
}
