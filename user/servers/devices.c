/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /dev: what hardware this turned out to be.
 *
 * A description rather than a device: the nodes here are read, printed and
 * looked at, and nothing is on a deadline. **So this one is not in C for
 * timing** - it is in C because `CLAUDE.md` says the layer decides, and a
 * server is a system component that receives exactly what it expects rather
 * than whatever somebody put in a table.
 *
 * `/dev/console` and `/dev/audio` are *not* served from
 * here. They are mounted over this prefix by whoever owns them and longest
 * prefix wins, so a read of `/dev/console` goes to the console server and
 * means "give me a line". Listing a name this server does not answer for
 * would be a lie and an expensive one: the first version of the Lua original
 * listed `console`, the `devices` command dutifully read every name it was
 * given, and the console answered by swallowing the next thing typed at the
 * prompt.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "devproto.h"

/* MIDR_EL1 implementer, [31:24]. The list is short because it is the list of
 * people who have ever shipped an AArch64 core, not a lookup table. */
static const char *implementer(unsigned code)
{
    switch (code) {
    case 0x41: return "ARM";        case 0x42: return "Broadcom";
    case 0x43: return "Cavium";     case 0x46: return "Fujitsu";
    case 0x48: return "HiSilicon";  case 0x4e: return "NVIDIA";
    case 0x50: return "APM";        case 0x51: return "Qualcomm";
    case 0x61: return "Apple";      case 0x6d: return "Microsoft";
    case 0xc0: return "Ampere";     default:   return NULL;
    }
}

/* MIDR_EL1 part number, [15:4], and only meaningful when the implementer is
 * ARM - everyone else numbers their own parts. */
static const char *arm_part(unsigned code)
{
    switch (code) {
    case 0xb76: return "ARM1176JZF-S"; case 0xc07: return "Cortex-A7";
    case 0xc08: return "Cortex-A8";    case 0xc09: return "Cortex-A9";
    case 0xd03: return "Cortex-A53";   case 0xd05: return "Cortex-A55";
    case 0xd07: return "Cortex-A57";   case 0xd08: return "Cortex-A72";
    case 0xd09: return "Cortex-A73";   case 0xd0a: return "Cortex-A75";
    case 0xd0b: return "Cortex-A76";   case 0xd0c: return "Neoverse-N1";
    case 0xd0d: return "Cortex-A77";   case 0xd41: return "Cortex-A78";
    default:    return NULL;
    }
}

/* ID_AA64MMFR0_EL1.PARANGE [3:0] is a table, not a formula. */
static unsigned pa_bits(unsigned parange)
{
    static const unsigned bits[] = { 32, 36, 40, 42, 44, 48, 52, 56 };

    return (parange < sizeof(bits) / sizeof(bits[0])) ? bits[parange] : 0;
}

/*
 * Appending a field, and the only place a name or a value is copied.
 *
 * `strncpy` is not used and its absence is deliberate: it does not
 * terminate when the source fills the buffer, which is exactly the case
 * this hits with a long device name. The fields are memset to zero by the
 * caller, so copying one byte less than the array always leaves a
 * terminator behind.
 */
static void put(struct dev_reply *r, const char *name, uint64_t number,
                const char *text)
{
    struct dev_field *f;
    size_t n;

    if (r->count >= DEV_FIELDS) {
        return;                     /* silently dropped would be a lie; see
                                     * the assert in devproto.h - this is the
                                     * runtime half of the same guard */
    }

    f = &r->field[r->count++];
    memset(f, 0, sizeof(*f));

    n = strlen(name);
    memcpy(f->name, name, (n < DEV_NAME_MAX) ? n : DEV_NAME_MAX - 1);

    if (text != NULL) {
        n = strlen(text);
        memcpy(f->text, text, (n < DEV_TEXT_MAX) ? n : DEV_TEXT_MAX - 1);
        f->kind = DEV_KIND_TEXT;
    } else {
        f->number = number;
        f->kind = DEV_KIND_NUMBER;
    }
}

static void put_num(struct dev_reply *r, const char *name, uint64_t v)
{
    put(r, name, v, NULL);
}

static void put_text(struct dev_reply *r, const char *name, const char *v)
{
    put(r, name, 0, v);
}

/* "0x41" and "part 0xd08" and "r0p3", built without a printf. */
static const char *hex(char *buf, const char *prefix, uint64_t v, unsigned digits)
{
    static const char D[] = "0123456789abcdef";
    char *p = buf;
    unsigned i;

    while (*prefix != '\0') {
        *p++ = *prefix++;
    }

    *p++ = '0';
    *p++ = 'x';

    for (i = digits; i > 0; i--) {
        *p++ = D[(v >> ((i - 1) * 4)) & 0xf];
    }

    *p = '\0';
    return buf;
}

static void node_cpu(const struct sysinfo *i, struct dev_reply *r)
{
    unsigned impl = (unsigned)((i->midr >> 24) & 0xff);
    unsigned part = (unsigned)((i->midr >> 4) & 0xfff);
    const char *name = implementer(impl);
    const char *pname = (impl == 0x41) ? arm_part(part) : NULL;
    char buf[24], buf2[24], rev[12];

    put_text(r, "implementer", name ? name : hex(buf, "", impl, 2));
    put_text(r, "part", pname ? pname : hex(buf2, "part ", part, 3));

    /* "r<major>p<minor>", from MIDR variant [23:20] and revision [3:0]. */
    rev[0] = 'r';
    rev[1] = (char)('0' + (unsigned)((i->midr >> 20) & 0xf));
    rev[2] = 'p';
    rev[3] = (char)('0' + (unsigned)(i->midr & 0xf));
    rev[4] = '\0';
    put_text(r, "revision", rev);

    put_num(r, "midr", i->midr);
    put_num(r, "cores", i->cpus);

    /* CTR_EL0 DminLine [19:16] is log2 of the line in *words*, not bytes. */
    put_num(r, "cache_line", 4u << ((i->ctr >> 16) & 0xf));
    put_num(r, "pa_bits", pa_bits((unsigned)(i->mmfr0 & 0xf)));
    put_num(r, "counter_hz", i->counter_hz);

    /* ID_AA64ISAR0_EL1: AES [7:4], SHA1 [11:8], SHA2 [15:12], CRC32 [19:16],
     * atomics [23:20]. Non-zero means present. */
    put_num(r, "aes",     ((i->isar0 >> 4)  & 0xf) != 0);
    put_num(r, "sha1",    ((i->isar0 >> 8)  & 0xf) != 0);
    put_num(r, "sha2",    ((i->isar0 >> 12) & 0xf) != 0);
    put_num(r, "crc32",   ((i->isar0 >> 16) & 0xf) != 0);
    put_num(r, "atomics", ((i->isar0 >> 20) & 0xf) != 0);

    /* ID_AA64PFR0_EL1: FP [19:16], AdvSIMD [23:20]. 0xf means absent. */
    put_num(r, "fp",   ((i->pfr0 >> 16) & 0xf) != 0xf);
    put_num(r, "simd", ((i->pfr0 >> 20) & 0xf) != 0xf);
    put_num(r, "el", i->current_el);
}

static void node_memory(const struct sysinfo *i, struct dev_reply *r)
{
    uint64_t mb = 1024u * 1024u;

    put_num(r, "total_mb", ((uint64_t)i->pages_total * i->page_size) / mb);
    put_num(r, "free_mb",  ((uint64_t)i->pages_free  * i->page_size) / mb);
    put_num(r, "pages_total", i->pages_total);
    put_num(r, "pages_free", i->pages_free);
    put_num(r, "page_size", i->page_size);
    put_num(r, "base", i->ram_base);
}

static void node_kernel(const struct sysinfo *i, struct dev_reply *r)
{
    put_num(r, "idle_ticks", i->idle_ticks);
    put_num(r, "busy_ticks", i->busy_ticks);
    put_num(r, "threads", i->threads_used);
    put_num(r, "threads_max", i->threads_total);
    put_num(r, "processes", i->processes_used);
    put_num(r, "processes_max", i->processes_total);
    put_num(r, "endpoints", i->endpoints_used);
    put_num(r, "endpoints_max", i->endpoints_total);
    put_num(r, "spaces", i->spaces_used);
    put_num(r, "spaces_max", i->spaces_total);
    put_num(r, "tick_hz", i->tick_hz);
}

/*
 * The order `list` returns, so a listing is reproducible.
 *
 * `console` is deliberately absent - see the note at the top of this file.
 */
static bool node_read(const char *want, const struct sysinfo *i,
                      struct dev_reply *r)
{
    if (strcmp(want, "cpu") == 0)    { node_cpu(i, r);    return true; }
    if (strcmp(want, "memory") == 0) { node_memory(i, r); return true; }
    if (strcmp(want, "kernel") == 0) { node_kernel(i, r); return true; }

    if (strcmp(want, "timer") == 0) {
        put_num(r, "hz", i->tick_hz);
        put_num(r, "counter_hz", i->counter_hz);
        return true;
    }

    /*
     * The wall clock, in seconds since 1970.
     *
     * **Restored, having been dropped when this server moved from Lua to C.**
     * The Lua one built a `clock` node out of `sysinfo.epoch` and nothing
     * here replaced it, so `/dev/clock` stopped existing and every clock in
     * the system said "no clock --:--" - the Deskbar's, the `datetime`
     * program's, and the topbar's.
     *
     * Only the epoch, and deliberately: `clock.lua` turns it into a civil
     * date, applies the time zone, and names the day. That is a table lookup
     * and a division, which belongs above this layer - the same division
     * `sysinfo` draws when it hands back a raw MIDR instead of "Cortex-A72".
     *
     * `utc` says what the number is against, so a caller does not have to
     * assume. There is no zone here: the board keeps UTC and a preference
     * turns it into local time.
     */
    if (strcmp(want, "clock") == 0) {
        put_num(r, "epoch", i->epoch);
        put_num(r, "utc", 1);
        return true;
    }

    if (strcmp(want, "screen") == 0 && i->screen_width > 0) {
        put_num(r, "width", i->screen_width);
        put_num(r, "height", i->screen_height);
        put_num(r, "pitch", i->screen_pitch);
        return true;
    }

    if (strcmp(want, "keyboard") == 0 && i->has_keyboard != 0) {
        put_text(r, "transport", "virtio-input over virtio-mmio");
        return true;
    }

    return false;
}

static void node_list(const struct sysinfo *i, struct dev_reply *r)
{
    put_text(r, "cpu", "");
    put_text(r, "memory", "");
    put_text(r, "kernel", "");
    put_text(r, "timer", "");
    put_text(r, "clock", "");

    /* Listed only when there is one: a board with no screen should not offer
     * a node that answers nothing. */
    if (i->screen_width > 0) { put_text(r, "screen", ""); }
    if (i->has_keyboard != 0) { put_text(r, "keyboard", ""); }
}

static void answer(const struct message *in, uint64_t sender)
{
    struct message out;
    struct dev_reply *rep = (struct dev_reply *)(void *)out.data;
    const struct dev_request *req =
        (const struct dev_request *)(const void *)in->data;
    struct sysinfo info;
    char want[DEV_NAME_MAX];

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);

    if (in->length < sizeof(*req)) {
        rep->error = DEV_ERR_BAD_OP;
        (void)kosmos_reply(sender, &out);
        return;
    }

    memset(&info, 0, sizeof(info));

    if (kosmos_sysinfo(&info) != 0) {
        rep->error = DEV_ERR_NO_KERNEL;
        (void)kosmos_reply(sender, &out);
        return;
    }

    switch (req->op) {
    case DEV_OP_LIST:
        node_list(&info, rep);
        break;

    case DEV_OP_READ:
        /* The mount prefix is stripped before this arrives; a leading slash
         * is not, so "/cpu" and "cpu" both mean the same node. */
        memcpy(want, req->name, DEV_NAME_MAX);
        want[DEV_NAME_MAX - 1] = '\0';

        if (!node_read((want[0] == '/') ? want + 1 : want, &info, rep)) {
            rep->error = DEV_ERR_NO_NODE;
        }
        break;

    case DEV_OP_GETATTR:
        put_text(rep, "kind", "device");
        break;

    default:
        rep->error = DEV_ERR_BAD_OP;
        break;
    }

    (void)kosmos_reply(sender, &out);
}

void devices_server(long endpoint)
{
    for (;;) {
        struct message msg;
        uint64_t sender = 0;

        /*
         * Blocking, with no deadline, and that is right here. Nothing in
         * `/dev` happens on its own - every field is computed from `sysinfo`
         * when somebody asks - so there is nothing to wake up for and an
         * idle machine should be idle.
         */
        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            return;                 /* the endpoint went away */
        }

        answer(&msg, sender);
    }
}
