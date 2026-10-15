/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A sandbox's monitor** - `docs/virtualization.md` V3, `docs/sandboxes.md`.
 *
 * One process a guest. It is handed the guest's RAM with a kernel already in
 * it and a page saying where (`vmmproto.h`), lays out what the machine's
 * firmware would have - a device tree on ARM, Linux's boot parameters on a
 * PC - makes the guest and a vCPU through the kernel (`SYS_GUEST_*`,
 * `SYS_VCPU_*`), and runs it, answering what the guest asks of a machine
 * that is not there.
 *
 * **This file is what both machines share**: the guest's console, kept in
 * a ring and read through `VMM_OP_READ`; what is typed for it; the
 * `/Running/sandbox-<name>` it is asked through; its folders and its
 * network, which are virtio devices on either (`vdev.c`); and its start.
 * The machine is the other file - `vmm_arm.c`, a GICv3, the generic timer, a
 * PL011 and PSCI; `vmm_x86.c`, an 8259, an 8254, a 16550 and the CPUID and
 * MSRs a PC answers - through `vmm.h`.
 *
 * C, because it is on every exit of the guest's path - a server, in the
 * sense `CLAUDE.md` draws the line.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "vmmproto.h"
#include "appproto.h"
#include "vdev.h"
#include "vmmblk.h"
#include "vmmgpu.h"
#include "vmminput.h"
#include "vmmfs.h"
#include "vmmnet.h"
#include "init/say.h"
#include "vmm.h"

static long guest_ram = -1;
uint8_t *vmm_ram;
struct vmm_setup vmm_cfg;

/* Whether it has a network device: outgoing, or ports carried in. */
bool vmm_networked(void)
{
    for (unsigned i = 0; i < VMM_PORTS; i++) {
        if (vmm_cfg.ports[i].from != 0) {
            return true;
        }
    }

    return vmm_cfg.network != VMM_NET_NONE;
}
long vmm_vcpu = -1;

/* The machine's two clocks, from `sysinfo`: the counter the guest's timer
 * compares against, and the scheduler's, which a sleep is in. */
uint64_t vmm_counter_hz = 62500000, vmm_tick_hz = 250;

/*
 * What was typed and the guest has not read yet: a ring this process's
 * serving thread fills from `VMM_OP_KEYS` and the PL011's data register,
 * on the vCPU's thread, empties. One writes `rx_head`, the other `rx_tail`,
 * and nobody takes a lock - the discipline every ring here keeps.
 */
static uint8_t rx[256];
static unsigned rx_head, rx_tail;

bool vmm_rx_empty(void)
{
    return __atomic_load_n(&rx_head, __ATOMIC_ACQUIRE)
           == __atomic_load_n(&rx_tail, __ATOMIC_ACQUIRE);
}

static void rx_put(uint8_t c)
{
    unsigned head = __atomic_load_n(&rx_head, __ATOMIC_RELAXED);
    unsigned next = (head + 1) % sizeof(rx);

    if (next != __atomic_load_n(&rx_tail, __ATOMIC_ACQUIRE)) {
        rx[head] = c;
        __atomic_store_n(&rx_head, next, __ATOMIC_RELEASE);
    }
}

int vmm_rx_take(void)
{
    unsigned tail = __atomic_load_n(&rx_tail, __ATOMIC_RELAXED);
    uint8_t c;

    if (tail == __atomic_load_n(&rx_head, __ATOMIC_ACQUIRE)) {
        return -1;
    }

    c = rx[tail];
    __atomic_store_n(&rx_tail, (tail + 1) % sizeof(rx), __ATOMIC_RELEASE);
    return c;
}

/*
 * **The guest's console, kept**: its last 64 KB, and how much it has ever
 * said, so a reader asks from an offset and learns when it fell behind
 * (`VMM_OP_READ`). Written a line at a time by the vCPU's thread and read
 * by the serving one, under a lock held for a copy and nothing else.
 */
#define CON_RING        65536u

static char *con;                           /* allocated at the start, not in the image */
static uint64_t con_written;
static char con_lock;

static void ring_lock(void)
{
    while (__atomic_test_and_set(&con_lock, __ATOMIC_ACQUIRE)) {
        /* the other thread is copying a line; it is a few hundred bytes */
    }
}

static void ring_unlock(void)
{
    __atomic_clear(&con_lock, __ATOMIC_RELEASE);
}

static void ring_put(const char *text, size_t n)
{
    size_t i;

    ring_lock();

    for (i = 0; i < n; i++) {
        con[(con_written + i) % CON_RING] = text[i];
    }

    con_written += n;
    ring_unlock();
}

/* From `offset`, up to `max` bytes; answers where they began. */
static uint64_t ring_get(uint64_t offset, char *into, size_t max, size_t *got)
{
    uint64_t from, n, i;

    ring_lock();
    from = offset;

    if (con_written > CON_RING && from < con_written - CON_RING) {
        from = con_written - CON_RING;      /* fell behind: the oldest kept */
    }

    if (from > con_written) {
        from = con_written;
    }

    n = con_written - from < max ? con_written - from : max;

    for (i = 0; i < n; i++) {
        into[i] = con[(from + i) % CON_RING];
    }

    ring_unlock();
    *got = (size_t)n;
    return from;
}

/* What the serving thread and the vCPU's thread vmm_tell each other. */
volatile bool vmm_stop_asked;            /* VMM_OP_STOP while it runs */
static volatile bool ended;                 /* the guest is gone */
static volatile int exit_code = -1;         /* its command's, by its end line */
volatile uint64_t vmm_guest_ticks, vmm_exits;
static volatile uint64_t started;

/* The guest's serial output, a line at a time. */
static char out[257];
static size_t out_len;

/*
 * A line, or what there is of one, into the console's ring - unless it is
 * the guest's end line: `end_token`, a space and its command's exit code,
 * which is the monitor's to read and not anybody's to see.
 */
void vmm_flush_out(void)
{
    size_t token = strlen(vmm_cfg.end_token), at;

    if (out_len == 0) {
        return;
    }

    out[out_len] = '\0';                     /* `out` has room for it */

    /*
     * **Anywhere in the line**, not only at its start: a command whose last
     * output has no newline - `wget -qO-` of a page - leaves the end line
     * glued to it, and its exit code was lost (13 October, on the M700). What
     * came before the token is the command's, and goes in the ring.
     */
    for (at = 0; token > 0 && at + token < out_len; at++) {
        if (memcmp(out + at, vmm_cfg.end_token, token) == 0 && out[at + token] == ' ') {
            int code = 0;
            size_t i = at + token + 1;

            while (i < out_len && out[i] >= '0' && out[i] <= '9') {
                code = code * 10 + (out[i++] - '0');
            }

            if (at > 0) {
                ring_put(out, at);
                ring_put("\n", 1);
            }

            exit_code = code;
            out_len = 0;
            return;
        }
    }

    /*
     * **And the kernel's last word on a machine it cannot switch off.** A PC
     * with no ACPI has no way for Linux to power it down, so it says so - at
     * the highest priority, which `quiet` does not hide - and halts, which is
     * how the monitor knows it has ended (`vmm_x86.c`). Said every time and
     * meaning nothing to whoever ran the sandbox.
     */
    if (strstr(out, "reboot: Power off not available") != NULL) {
        out_len = 0;
        return;
    }

    ring_put(out, out_len);
    out_len = 0;
}

/*
 * **A terminal's control sequences, dropped.** Linux's programs assume a
 * VT100 on the other end of a serial port - busybox's shell asks where the
 * cursor is (ESC [ 6 n) and `ls` colours its names - and Kosmos's console
 * has no escape language by design: a colour is a field of a write
 * (`conproto.h`), not bytes inside the text. So an escape and everything to
 * the end of its sequence - a CSI's final byte, 0x40 to 0x7e - goes no
 * further than here.
 */
static enum { PLAIN, ESCAPE, CSI } out_state = PLAIN;

void vmm_guest_byte(char c)
{
    if (out_state == ESCAPE) {
        out_state = c == '[' ? CSI : PLAIN;
        return;
    }

    if (out_state == CSI) {
        if (c >= 0x40 && c <= 0x7e) {
            out_state = PLAIN;
        }

        return;
    }

    if (c == 0x1b) {
        out_state = ESCAPE;
        return;
    }

    if (c == '\r') {
        return;
    }

    out[out_len++] = c;

    if (c == '\n' || out_len == sizeof(out) - 1) {
        vmm_flush_out();
    }
}

/* The setup page, mapped: the last note goes into it until the guest runs. */
static char *setup_page;
static bool guest_running;

/* A line of the monitor's own, into the console's ring. */
void vmm_note(const char *text)
{
    if (setup_page != NULL && !guest_running) {
        size_t n = strlen(text);

        if (n > VMM_SETUP_WHY_MAX - 1) {
            n = VMM_SETUP_WHY_MAX - 1;
        }

        memcpy(setup_page + VMM_SETUP_WHY_AT, text, n);
        setup_page[VMM_SETUP_WHY_AT + n] = '\0';
    }

    vmm_flush_out();
    ring_put("vmm: ", 5);
    ring_put(text, strlen(text));
    ring_put("\n", 1);
}

void vmm_tell(const char *what, unsigned long a, unsigned long b)
{
    struct say_line line;

    say_begin(&line);
    say_text(&line, what);
    say_hex(&line, a, 0);
    say_text(&line, " ");
    say_hex(&line, b, 0);
    vmm_note(line.text);
}

/* The guest has ended - by itself, or because it was asked to. */
void vmm_finish(void)
{
    vmm_flush_out();
    ended = true;
    kosmos_thread_exit(0);

    for (;;) {
        /* the thread is gone; the kernel does not come back here */
    }
}

/* What a request is answered with. */
static void answer(const struct vmm_request *req, struct vmm_reply *rep)
{
    size_t got = 0, i;

    memset(rep, 0, sizeof(*rep));
    rep->state = ended ? VMM_ENDED : VMM_RUNNING;
    rep->exit_code = exit_code;
    rep->ram_bytes = vmm_cfg.ram_bytes;
    rep->ram_used = (uint64_t)kosmos_mem_touched(guest_ram) * 4096u;
    rep->up_counter_ticks = kosmos_ticks() - started;
    rep->guest_counter_ticks = vmm_guest_ticks;
    rep->exits = vmm_exits;
    vmmnet_counted(&rep->net_in, &rep->net_out);
    memcpy(rep->name, vmm_cfg.name, VMM_NAME_MAX);

    switch (req->op) {
    case VMM_OP_STATE:
        break;
    case VMM_OP_READ:
        rep->offset = ring_get(req->offset, rep->text, VMM_TEXT_MAX, &got);
        rep->length = (uint32_t)got;
        break;
    case VMM_OP_KEYS:
        for (i = 0; i < req->length && i < VMM_TEXT_MAX; i++) {
            uint8_t c = (uint8_t)req->text[i];

            /* Enter is a carriage return and Backspace DEL, as a serial
             * terminal sends them. */
            rx_put(c == '\n' ? '\r' : c == 8 ? 127 : c);
        }
        break;
    case VMM_OP_STOP:
        vmm_stop_asked = true;
        break;
    default:
        rep->error = VMM_ERR_BAD_OP;
        break;
    }

    rep->written = con_written;
}

/*
 * The monitor's exit codes before its guest runs, each a sentence for
 * whoever started it (`sandboxes.lua`): an error here is a number, and the
 * words are composed by whoever shows it to a person.
 */
#define VMM_NO_SETUP        11
#define VMM_BAD_SETUP       12
#define VMM_NO_RAM          13
#define VMM_NO_TREE         14
#define VMM_NO_EL2          15
#define VMM_NO_GUEST        16
#define VMM_REFUSED         17
#define VMM_NO_THREAD       18
#define VMM_NO_NAME         19
#define VMM_BAD_SHARE       20
#define VMM_BAD_DISK        21
#define VMM_BAD_SCREEN      22

/*
 * **Registered in `/Running` as `sandbox-<name>`**, with an endpoint of its
 * own: from then on the window and `sandbox` find it there and ask it, and
 * the name goes when this process does.
 */
static long register_name(long running)
{
    struct message msg, rep;
    struct app_request *req = (struct app_request *)msg.data;
    const struct app_reply *got = (const struct app_reply *)rep.data;
    long endpoint = kosmos_endpoint();
    char name[APP_NAME_MAX];

    if (endpoint < 0) {
        return -1;
    }

    memset(name, 0, sizeof(name));
    memcpy(name, "sandbox-", 8);
    memcpy(name + 8, vmm_cfg.name, strlen(vmm_cfg.name) < APP_NAME_MAX - 9
                                 ? strlen(vmm_cfg.name) : APP_NAME_MAX - 9);

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(*req);
    msg.cap_plus_one = (uint32_t)endpoint + 1;
    req->op = APP_OP_REGISTER;
    memcpy(req->name, name, APP_NAME_MAX);

    if (kosmos_call(running, &msg, &rep) < 0 || got->error != APP_OK
        || strncmp(got->name, name, APP_NAME_MAX) != 0) {
        return -1;
    }

    return endpoint;
}

void vmm_server(long running_cap, long ram_cap, long setup_cap, long more1, long more2,
                long more3, long more4)
{
    long home_cap = -1, net_cap = -1;
    long at, guest, thread, endpoint;
    const struct vmm_setup *s;
    unsigned long linger = 0;

    guest_ram = ram_cap;
    started = kosmos_ticks();
    con = malloc(CON_RING);

    if (con == NULL || !vmm_machine_start()) {
        kosmos_exit(VMM_NO_GUEST);
    }

    {
        struct sysinfo info;

        memset(&info, 0, sizeof(info));

        if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0 && info.tick_hz != 0) {
            vmm_counter_hz = info.counter_hz;
            vmm_tick_hz = info.tick_hz;
        }
    }

    if ((at = kosmos_mem_map(setup_cap)) < 0) {
        kosmos_exit(VMM_NO_SETUP);
    }

    s = (const struct vmm_setup *)at;
    setup_page = (char *)at;
    vmm_cfg = *s;
    vmm_cfg.cmdline[VMM_CMDLINE_MAX - 1] = '\0';
    vmm_cfg.name[VMM_NAME_MAX - 1] = '\0';
    vmm_cfg.end_token[VMM_TOKEN_MAX - 1] = '\0';

    if (vmm_cfg.magic != VMM_SETUP_MAGIC || vmm_cfg.version != VMM_SETUP_VERSION
        || vmm_cfg.name[0] == '\0'
        || vmm_cfg.ram_bytes != (uint64_t)kosmos_mem_size(ram_cap) * 4096u
        || !vmm_machine_setup_ok()) {
        kosmos_exit(VMM_BAD_SETUP);
    }

    if ((at = kosmos_mem_map(ram_cap)) < 0) {
        kosmos_exit(VMM_NO_RAM);
    }

    vmm_ram = (uint8_t *)at;

    /* What it was handed after its first three: `/Home` if it has folders,
     * then `/Network` if it has the network (`fs.vm_start`). */
    {
        bool folders = false;

        for (unsigned i = 0; i < VMM_SHARES; i++) {
            folders = folders || vmm_cfg.shares[i].root[0] != '\0';
        }

        home_cap = folders ? more1 : -1;
        net_cap = vmm_networked() ? (folders ? more2 : more1) : -1;

        /* And the drive its disk is on, after those (`docs/linux.md` L1). */
        vmm_cfg.disk.path[sizeof vmm_cfg.disk.path - 1] = '\0';

        {
            long more[4] = { more1, more2, more3, more4 };
            unsigned next = (folders ? 1u : 0u) + (vmm_networked() ? 1u : 0u);

            if (vmm_cfg.disk.path[0] != '\0'
                && !vmmblk_start(more[next++], vmm_cfg.disk.path, vmm_cfg.disk.read_only != 0)) {
                kosmos_exit(VMM_BAD_DISK);
            }

            /* And its screen's region, after the drive (`docs/linux.md` L2). */
            if (vmm_cfg.screen != 0 && !vmmgpu_start(more[next])) {
                kosmos_exit(VMM_BAD_SCREEN);
            }

            /* And its keyboard and pointer, with its screen (L3). */
            if (vmmgpu_on()) {
                vmminput_start(vmmgpu_page());
            }
        }

        if (vmm_networked()) {
            uint16_t from[VMM_PORTS], to[VMM_PORTS];

            if (net_cap < 0) {
                kosmos_exit(VMM_BAD_SHARE);
            }

            for (unsigned i = 0; i < VMM_PORTS; i++) {
                from[i] = vmm_cfg.ports[i].from;
                to[i] = vmm_cfg.ports[i].to;
            }

            unsigned refused = vmmnet_start(net_cap, (uint32_t)vmm_tick_hz,
                                            vmm_cfg.network == VMM_NET_OUT, from, to, VMM_PORTS);

            for (unsigned i = 0; i < VMM_PORTS; i++) {
                if ((refused >> i & 1u) != 0) {
                    struct say_line line;

                    say_begin(&line);
                    say_text(&line, "Kosmos would not answer on port ");
                    say_dec(&line, from[i]);
                    say_text(&line, ", for the guest's ");
                    say_dec(&line, to[i]);
                    vmm_note(line.text);
                }
            }
        }
    }

    /* Its folders, served from `/Home` - which it was handed only if it has
     * any (`fs.vm_start`). */
    vdev_memory(vmm_ram, vmm_cfg.ram_bytes, VMM_GUEST_RAM);
    vmmfs_start(home_cap);

    for (unsigned i = 0; i < VMM_SHARES; i++) {
        char tag[8] = "share0";

        vmm_cfg.shares[i].root[sizeof(vmm_cfg.shares[i].root) - 1] = '\0';

        if (vmm_cfg.shares[i].root[0] == '\0') {
            continue;
        }

        tag[5] = (char)('0' + i);

        if (home_cap < 0 || !vmmfs_add(i, tag, vmm_cfg.shares[i].root, vmm_cfg.shares[i].read_only != 0)) {
            kosmos_exit(VMM_BAD_SHARE);
        }
    }

    if (!vmm_machine_prepare()) {
        kosmos_exit(VMM_NO_TREE);
    }

    if ((guest = kosmos_guest_create()) < 0) {
        kosmos_exit(guest == SYS_ERR_NO_DEVICE ? VMM_NO_EL2 : VMM_NO_GUEST);
    }

    if (kosmos_guest_map(guest, ram_cap, VMM_GUEST_RAM) != 0
        || (vmm_vcpu = kosmos_vcpu_create(guest)) < 0) {
        kosmos_exit(VMM_REFUSED);
    }

    if ((endpoint = register_name(running_cap)) < 0) {
        kosmos_exit(VMM_NO_NAME);
    }

    /*
     * **On core zero**, which is the one core every way of starting the ARM
     * machine leaves at EL2: QEMU's PSCI starts the others at EL1 (`hyp.S`,
     * `hyp_present`). A board's firmware that starts every core at EL2 would
     * let it run anywhere, and so would a PC, whose every core has VT-x.
     */
    guest_running = true;

    thread = kosmos_thread_start_on(vmm_machine_run, 0, 0);

    if (thread < 0) {
        kosmos_exit(VMM_NO_THREAD);
    }

    /*
     * **Serving, on this thread**, while the vCPU's runs the guest. Once
     * the guest has ended, the monitor stays a little - ten seconds, or
     * until somebody says it may go (`VMM_OP_STOP`) - so whoever is
     * attached reads its last lines and its exit code.
     */
    for (;;) {
        struct message msg, rep;
        uint64_t sender = 0;
        long got = kosmos_receive(endpoint, &msg, &sender, 0, vmm_tick_hz / 4);

        if (got >= 0) {
            const struct vmm_request *req = (const struct vmm_request *)msg.data;
            bool going = ended && req->op == VMM_OP_STOP;

            memset(&rep, 0, sizeof(rep));
            rep.length = sizeof(struct vmm_reply);
            answer(req, (struct vmm_reply *)rep.data);
            kosmos_reply(sender, &rep);

            if (going) {
                break;
            }
        }

        /* Asked to stop by somebody not reading it: no need to stay. */
        if (ended && (vmm_stop_asked || ++linger > 40)) {
            break;                          /* ten seconds, nobody asking */
        }
    }

    kosmos_exit(exit_code < 0 ? 0 : exit_code);
}
