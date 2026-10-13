/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A sandbox's monitor** - `docs/virtualization.md` V3.
 *
 * One process a guest. It is handed the guest's RAM with a kernel already in
 * it and a page saying where (`vmmproto.h`), describes a small machine in a
 * device tree at the top of that RAM, makes the guest and a vCPU through the
 * kernel (`SYS_GUEST_*`, `SYS_VCPU_*`), and runs it - answering what the
 * guest asks of a machine that is not there:
 *
 * - **a PL011 serial port** at `VMM_UART_BASE`, which nothing maps, so every
 *   touch of it stops the guest with a stage-2 fault that says the register,
 *   the size and whether it was a write (ESR's ISV): a byte written to its
 *   data register is a byte on this process's console;
 * - **PSCI**, by `hvc`, the firmware calls Linux makes - its version, and
 *   switching the machine off;
 * - and everything else is said and stops the guest, so what V3 has left to
 *   build is found by booting rather than by guessing.
 *
 * - **a GICv3**: its distributor and its one redistributor as register
 *   files here, which say what is enabled and how it is configured - and
 *   delivery through the processor's own virtual CPU interface: an
 *   interrupt the guest should take is written into a list register, and
 *   the guest acknowledges and ends it without a word to this process;
 * - **its timer**, the core's virtual one, which is the guest's while it
 *   runs: when it is due the timer's interrupt is injected, and when the
 *   guest waits (`wfi`, trapped) this process sleeps until it is.
 *
 * C, because it is on every exit of the guest's path - a server, in the
 * sense `CLAUDE.md` draws the line.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "vmmproto.h"
#include "fdt.h"
#include "init/say.h"

#define DTB_SPACE       0x200000ul          /* the top 2 MB of RAM */

#define EC_WFX          0x01u
#define EC_HVC          0x16u
#define EC_SYSREG       0x18u
#define EC_SMC          0x17u
#define EC_DABT_LOW     0x24u

#define PSCI_VERSION        0x84000000u
#define PSCI_MIGRATE_INFO   0x84000006u
#define PSCI_SYSTEM_OFF     0x84000008u
#define PSCI_SYSTEM_RESET   0x84000009u
#define PSCI_FEATURES       0x8400000au
#define PSCI_NOT_SUPPORTED  ((uint64_t)-1)

static long console = -1;
static long guest_ram = -1;
static uint8_t *ram;
static struct vmm_setup setup;
static long vcpu = -1;

/* The machine's two clocks, from `sysinfo`: the counter the guest's timer
 * compares against, and the scheduler's, which a sleep is in. */
static uint64_t counter_hz = 62500000, tick_hz = 250;

/*
 * **The GIC's registers, as files of bytes** - the distributor's 64 KB and
 * the redistributor's two 64 KB frames - which a guest's reads and writes
 * land in, with the few registers that are not plain storage handled
 * beside them. 64 interrupts: the 32 private ones (the timer's is 27) and
 * 32 shared (the serial port's is 33).
 */
#define GIC_IRQS        64u
#define IRQ_TIMER       27u
#define IRQ_UART        33u

static uint8_t gicd[0x10000];
static uint8_t gicr[0x20000];
static uint64_t gic_enabled;            /* a bit an interrupt */

/* The PL011's interrupt state: what is masked in, and what is raised. */
static uint32_t uart_imsc;

/* The guest's serial output, a line at a time. */
static char out[257];
static size_t out_len;

static void flush_out(void)
{
    if (out_len == 0) {
        return;
    }

    out[out_len] = '\0';
    say(console, out);
    out_len = 0;
}

static void guest_byte(char c)
{
    if (c == '\r') {
        return;
    }

    out[out_len++] = c;

    if (c == '\n' || out_len == sizeof(out) - 1) {
        flush_out();
    }
}

static void tell(const char *what, unsigned long a, unsigned long b)
{
    struct say_line line;

    flush_out();
    say_begin(&line);
    say_text(&line, "vmm: ");
    say_text(&line, what);
    say_hex(&line, a, 0);
    say_text(&line, " ");
    say_hex(&line, b, 0);
    say_send(console, &line);
}

/*
 * The machine, described: one processor started by PSCI over `hvc`, the
 * RAM, ARM's generic timer, a GICv3 and the PL011 - and the command line,
 * the initramfs if there is one, and where the console is.
 */
static size_t write_tree(uint8_t *at, size_t cap)
{
    struct fdt f;
    uint64_t ram_end = VMM_RAM_BASE + setup.ram_bytes;
    uint32_t cells[12];

    fdt_begin(&f, at, cap);
    fdt_node(&f, "");
    fdt_prop_u32(&f, "#address-cells", 2);
    fdt_prop_u32(&f, "#size-cells", 2);
    fdt_prop_str(&f, "compatible", "linux,dummy-virt");
    fdt_prop_u32(&f, "interrupt-parent", 1);

    fdt_node(&f, "chosen");
    fdt_prop_str(&f, "bootargs", setup.cmdline);
    fdt_prop_str(&f, "stdout-path", "/pl011@9000000");

    if (setup.initrd_bytes != 0) {
        uint64_t start = VMM_RAM_BASE + setup.initrd_offset;
        uint64_t end = start + setup.initrd_bytes;

        cells[0] = (uint32_t)(start >> 32); cells[1] = (uint32_t)start;
        fdt_prop_cells(&f, "linux,initrd-start", cells, 2);
        cells[0] = (uint32_t)(end >> 32); cells[1] = (uint32_t)end;
        fdt_prop_cells(&f, "linux,initrd-end", cells, 2);
    }

    fdt_end_node(&f);

    fdt_node(&f, "memory@40000000");
    fdt_prop_str(&f, "device_type", "memory");
    cells[0] = (uint32_t)(VMM_RAM_BASE >> 32); cells[1] = (uint32_t)VMM_RAM_BASE;
    cells[2] = (uint32_t)(setup.ram_bytes >> 32); cells[3] = (uint32_t)setup.ram_bytes;
    fdt_prop_cells(&f, "reg", cells, 4);
    fdt_end_node(&f);
    (void)ram_end;

    fdt_node(&f, "cpus");
    fdt_prop_u32(&f, "#address-cells", 1);
    fdt_prop_u32(&f, "#size-cells", 0);
    fdt_node(&f, "cpu@0");
    fdt_prop_str(&f, "device_type", "cpu");
    fdt_prop_str(&f, "compatible", "arm,armv8");
    fdt_prop_u32(&f, "reg", 0);
    fdt_prop_str(&f, "enable-method", "psci");
    fdt_end_node(&f);
    fdt_end_node(&f);

    fdt_node(&f, "psci");
    fdt_prop(&f, "compatible", "arm,psci-1.0\0arm,psci-0.2", 26);
    fdt_prop_str(&f, "method", "hvc");
    fdt_end_node(&f);

    /* The four timer interrupts as PPIs: secure, non-secure physical,
     * virtual, hypervisor - 13, 14, 11, 10, level-triggered low. */
    fdt_node(&f, "timer");
    fdt_prop_str(&f, "compatible", "arm,armv8-timer");
    {
        uint32_t ppis[12] = { 1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4 };

        fdt_prop_cells(&f, "interrupts", ppis, 12);
    }
    fdt_prop_empty(&f, "always-on");
    fdt_end_node(&f);

    fdt_node(&f, "intc@8000000");
    fdt_prop_str(&f, "compatible", "arm,gic-v3");
    fdt_prop_u32(&f, "#interrupt-cells", 3);
    fdt_prop_empty(&f, "interrupt-controller");
    fdt_prop_u32(&f, "phandle", 1);
    {
        uint32_t reg[8] = { 0, (uint32_t)VMM_GICD_BASE, 0, 0x10000,
                            0, (uint32_t)VMM_GICR_BASE, 0, 0x20000 };

        fdt_prop_cells(&f, "reg", reg, 8);
    }
    fdt_end_node(&f);

    fdt_node(&f, "apb-pclk");
    fdt_prop_str(&f, "compatible", "fixed-clock");
    fdt_prop_u32(&f, "#clock-cells", 0);
    fdt_prop_u32(&f, "clock-frequency", 24000000);
    fdt_prop_u32(&f, "phandle", 2);
    fdt_end_node(&f);

    fdt_node(&f, "pl011@9000000");
    fdt_prop(&f, "compatible", "arm,pl011\0arm,primecell", 24);
    {
        uint32_t reg[4] = { 0, (uint32_t)VMM_UART_BASE, 0, 0x1000 };
        uint32_t irq[3] = { 0, 1, 4 };
        uint32_t clocks[2] = { 2, 2 };

        fdt_prop_cells(&f, "reg", reg, 4);
        fdt_prop_cells(&f, "interrupts", irq, 3);
        fdt_prop_cells(&f, "clocks", clocks, 2);
    }
    fdt_prop(&f, "clock-names", "uartclk\0apb_pclk", 17);
    fdt_end_node(&f);

    fdt_end_node(&f);
    return fdt_finish(&f);
}

/*
 * **The PL011**, as far as a console needs it: a byte written to its data
 * register is a byte on this process's console, the transmitter is always
 * empty and the receiver always so (input is V3c), and its interrupt is
 * raised while the transmitter is empty and the guest asked to hear that
 * (TXIM) - which is how Linux's driver finishes a write.
 */
#define UART_TXIS       (1u << 5)

static uint32_t uart_raw(void)
{
    return UART_TXIS;                       /* RIS: the transmitter is empty */
}

static bool uart_raised(void)
{
    return (uart_raw() & uart_imsc) != 0;
}

static uint64_t uart_read(uint64_t offset)
{
    static const uint8_t ids[8] = { 0x11, 0x10, 0x14, 0x00, 0x0d, 0xf0, 0x05, 0xb1 };

    switch (offset) {
    case 0x018: return 0x90;                /* FR: transmit empty, nothing in */
    case 0x038: return uart_imsc;           /* IMSC */
    case 0x03c: return uart_raw();          /* RIS */
    case 0x040: return uart_raw() & uart_imsc;  /* MIS */
    default:
        break;
    }

    if (offset >= 0xfe0 && offset < 0x1000) {
        return ids[(offset - 0xfe0) / 4];   /* the PrimeCell's identity */
    }

    return 0;
}

static void uart_write(uint64_t offset, uint64_t value)
{
    if (offset == 0x000) {
        guest_byte((char)value);
    } else if (offset == 0x038) {
        uart_imsc = (uint32_t)value & 0x7ff;
    }
}

/* A little-endian value of `size` bytes at `p`, and the other way. */
static uint64_t get_le(const uint8_t *p, unsigned size)
{
    uint64_t v = 0;

    while (size-- > 0) {
        v = (v << 8) | p[size];
    }

    return v;
}

static void put_le(uint8_t *p, unsigned size, uint64_t v)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/*
 * Set-enable and clear-enable, for the distributor's shared interrupts and
 * the redistributor's private ones alike: `first` is the interrupt the
 * word's bit 0 names. A read of either says what is enabled.
 */
static uint64_t enables(unsigned first)
{
    return first < GIC_IRQS ? (uint32_t)(gic_enabled >> first) : 0;
}

static void enable(unsigned first, uint64_t bits, bool on)
{
    if (first >= GIC_IRQS) {
        return;
    }

    if (on) {
        gic_enabled |= (uint64_t)(uint32_t)bits << first;
    } else {
        gic_enabled &= ~((uint64_t)(uint32_t)bits << first);
    }
}

static uint64_t gicd_read(uint64_t off, unsigned size)
{
    if (off == 0x0004) {
        return (9u << 19) | 1u;             /* TYPER: 10 ID bits, 64 lines */
    }

    if (off == 0x0008) {
        return 0x43b;                       /* IIDR: Arm */
    }

    if (off == 0xffe8) {
        return 0x3b;                        /* PIDR2: GICv3 */
    }

    if ((off >= 0x100 && off < 0x180) || (off >= 0x180 && off < 0x200)) {
        return enables((unsigned)((off & 0x7f) * 8));
    }

    return get_le(gicd + off, size);
}

static void gicd_write(uint64_t off, unsigned size, uint64_t v)
{
    if (off >= 0x100 && off < 0x180) {
        enable((unsigned)((off - 0x100) * 8), v, true);
    } else if (off >= 0x180 && off < 0x200) {
        enable((unsigned)((off - 0x180) * 8), v, false);
    } else if (off == 0x0000) {
        put_le(gicd, 4, v & 0x37);          /* CTLR; RWP reads 0 */
    } else if (off >= 0x0004 && off < 0x0010) {
        /* TYPER, IIDR, TYPER2: read only */
    } else {
        put_le(gicd + off, size, v);
    }
}

static uint64_t gicr_read(uint64_t off, unsigned size)
{
    switch (off) {
    case 0x0004: return 0x43b;              /* IIDR */
    case 0x0008: return size == 8 ? 0x10 : 0x10;    /* TYPER: the last, CPU 0 */
    case 0x000c: return 0;                  /* TYPER's high word: affinity 0 */
    case 0x0014: return get_le(gicr + 0x14, 4) & ~4u;   /* WAKER: awake */
    case 0xffe8: return 0x3b;               /* PIDR2: GICv3 */
    default:
        break;
    }

    if (off == 0x10100 || off == 0x10180) {
        return enables(0);                  /* ISENABLER0, ICENABLER0 */
    }

    return get_le(gicr + off, size);
}

static void gicr_write(uint64_t off, unsigned size, uint64_t v)
{
    if (off == 0x10100) {
        enable(0, v, true);
    } else if (off == 0x10180) {
        enable(0, v, false);
    } else if (off >= 0x0004 && off < 0x0010) {
        /* IIDR, TYPER: read only */
    } else {
        put_le(gicr + off, size, v);
    }
}

/*
 * A load or a store to something nothing maps. ESR's ISS says, when ISV is
 * set, which register (SRT), how wide (SAS) and which way (WnR); the page
 * comes from HPFAR and the offset in it from FAR. Answers false for an
 * access it cannot describe or a place nothing is.
 */
static bool emulate(struct vcpu_regs *r)
{
    uint64_t iss = r->syndrome & 0x1ffffff;
    uint64_t at = r->ipa | (r->address & 0xfff);
    unsigned reg = (unsigned)(iss >> 16) & 31;
    unsigned size = 1u << ((iss >> 22) & 3);
    bool write = ((iss >> 6) & 1) != 0;
    uint64_t value = (write && reg != 31) ? r->x[reg] : 0, got = 0;

    if (((iss >> 24) & 1) == 0) {
        return false;                       /* no syndrome to go by */
    }

    if (size < 8) {
        value &= (1ul << (8 * size)) - 1;
    }

    if (at >= VMM_UART_BASE && at < VMM_UART_BASE + 0x1000) {
        if (write) {
            uart_write(at - VMM_UART_BASE, value);
        } else {
            got = uart_read(at - VMM_UART_BASE);
        }
    } else if (at >= VMM_GICD_BASE && at + size <= VMM_GICD_BASE + sizeof(gicd)) {
        if (write) {
            gicd_write(at - VMM_GICD_BASE, size, value);
        } else {
            got = gicd_read(at - VMM_GICD_BASE, size);
        }
    } else if (at >= VMM_GICR_BASE && at + size <= VMM_GICR_BASE + sizeof(gicr)) {
        if (write) {
            gicr_write(at - VMM_GICR_BASE, size, value);
        } else {
            got = gicr_read(at - VMM_GICR_BASE, size);
        }
    } else {
        return false;
    }

    if (!write && reg != 31) {
        r->x[reg] = got;
    }

    r->pc += 4;
    return true;
}

/* An interrupt's priority, as the guest set it in its IPRIORITYR byte. */
static unsigned priority_of(unsigned irq)
{
    return irq < 32 ? gicr[0x10400 + irq] : gicd[0x400 + irq];
}

/* Whether the guest's timer is due: enabled, not masked, compare passed. */
static bool timer_due(const struct vcpu_regs *r)
{
    return (r->timer_ctl & 1) != 0 && (r->timer_ctl & 2) == 0
           && kosmos_ticks() >= r->timer_cval;
}

/*
 * **What the guest should take next, into the list registers.** An
 * interrupt that is raised, enabled, and in none of them already goes into
 * a free one, pending, group 1, at the priority the guest gave it. Both
 * sources are levels: they are offered again once the guest has ended them
 * (its list register reads free) and only if they are still raised.
 */
static bool in_a_list(const struct vcpu_regs *r, unsigned irq)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        if ((r->lr[i] >> 62) != 0 && (uint32_t)r->lr[i] == irq) {
            return true;
        }
    }

    return false;
}

static void offer(struct vcpu_regs *r, unsigned irq)
{
    unsigned i;

    if (((gic_enabled >> irq) & 1) == 0 || in_a_list(r, irq)) {
        return;
    }

    for (i = 0; i < 4; i++) {
        if ((r->lr[i] >> 62) == 0) {
            r->lr[i] = (1ul << 62)              /* pending */
                       | (1ul << 60)            /* group 1 */
                       | ((uint64_t)priority_of(irq) << 48)
                       | irq;
            return;
        }
    }
}

static bool deliver(struct vcpu_regs *r)
{
    bool any = false;

    if (timer_due(r)) {
        offer(r, IRQ_TIMER);
        any = true;
    }

    if (uart_raised()) {
        offer(r, IRQ_UART);
        any = true;
    }

    return any;
}

/*
 * **The guest is waiting for an interrupt** (`wfi`, trapped by HCR.TWI).
 * If one is already due it simply goes on; otherwise this sleeps until its
 * timer is, a scheduler tick at the least and a tenth of a second at the
 * most, so a guest with nothing to do costs this machine nothing.
 */
static void wait_for_guest(const struct vcpu_regs *r)
{
    uint64_t now = kosmos_ticks(), ticks = tick_hz / 10;

    if (deliver((struct vcpu_regs *)r) || (r->lr[0] | r->lr[1] | r->lr[2] | r->lr[3]) >> 62) {
        return;
    }

    if ((r->timer_ctl & 1) != 0 && (r->timer_ctl & 2) == 0 && r->timer_cval > now) {
        uint64_t wait = (r->timer_cval - now) * tick_hz / counter_hz;

        ticks = wait < ticks ? wait : ticks;
    }

    kosmos_sleep(ticks > 0 ? (unsigned long)ticks : 1);
}

/* A PSCI call, in x0, answered in x0. False: the guest asked to stop. */
static bool psci(struct vcpu_regs *r)
{
    switch ((uint32_t)r->x[0]) {
    case PSCI_VERSION:
        r->x[0] = 0x10000;                  /* 1.0 */
        return true;
    case PSCI_MIGRATE_INFO:
        r->x[0] = 2;                        /* nothing to migrate */
        return true;
    case PSCI_FEATURES:
        r->x[0] = (uint32_t)r->x[1] == PSCI_VERSION ? 0 : PSCI_NOT_SUPPORTED;
        return true;
    case PSCI_SYSTEM_OFF:
    case PSCI_SYSTEM_RESET:
        flush_out();
        say(console, "vmm: the guest switched itself off\n");
        return false;
    default:
        r->x[0] = PSCI_NOT_SUPPORTED;
        return true;
    }
}

/* The vCPU's thread: run, answer, run again, until the guest stops. */
static void run(unsigned long unused)
{
    struct vcpu_regs r;
    uint64_t dtb_at = setup.ram_bytes - DTB_SPACE;
    long why;

    (void)unused;
    memset(&r, 0, sizeof(r));
    r.pc = VMM_RAM_BASE + setup.kernel_offset;
    r.pstate = 0x3c5;                       /* EL1h, D A I F masked */
    r.x[0] = VMM_RAM_BASE + dtb_at;         /* the device tree, as Linux wants */

    for (;;) {
        deliver(&r);
        why = kosmos_vcpu_run(vcpu, &r);

        if (why == VCPU_EXIT_HOST) {
            continue;
        }

        if (why != VCPU_EXIT_TRAP) {
            tell("the vCPU would not run: ", (unsigned long)why, 0);
            kosmos_thread_exit(3);
        }

        switch ((unsigned)(r.syndrome >> 26)) {
        case EC_WFX:
            r.pc += 4;
            wait_for_guest(&r);
            break;
        case EC_SYSREG:
            /* A GIC system register the virtual interface leaves to EL2 -
             * a software-generated interrupt, which one processor never
             * needs. Done as nothing. */
            r.pc += 4;
            break;
        case EC_HVC:
            if (!psci(&r)) {
                kosmos_thread_exit(0);
            }
            break;
        case EC_SMC:
            r.pc += 4;
            if (!psci(&r)) {
                kosmos_thread_exit(0);
            }
            break;
        case EC_DABT_LOW:
            if (!emulate(&r)) {
                tell("the guest touched what is not there, at ", r.ipa | (r.address & 0xfff), r.pc);
                kosmos_thread_exit(2);
            }
            break;
        default:
            tell("the guest stopped: syndrome and pc ", r.syndrome, r.pc);
            kosmos_thread_exit(2);
        }
    }
}

void vmm_server(long console_cap, long ram_cap, long setup_cap)
{
    long at, guest, thread;
    const struct vmm_setup *s;
    size_t tree;

    console = console_cap;
    guest_ram = ram_cap;

    {
        struct sysinfo info;

        memset(&info, 0, sizeof(info));

        if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0 && info.tick_hz != 0) {
            counter_hz = info.counter_hz;
            tick_hz = info.tick_hz;
        }
    }

    if ((at = kosmos_mem_map(setup_cap)) < 0) {
        say(console, "vmm: no setup page\n");
        kosmos_exit(1);
    }

    s = (const struct vmm_setup *)at;
    setup = *s;
    setup.cmdline[VMM_CMDLINE_MAX - 1] = '\0';

    if (setup.magic != VMM_SETUP_MAGIC || setup.version != VMM_SETUP_VERSION
        || setup.ram_bytes != (uint64_t)kosmos_mem_size(ram_cap) * 4096u
        || setup.ram_bytes < 4 * DTB_SPACE
        || (setup.kernel_offset & (DTB_SPACE - 1)) != 0
        || setup.kernel_offset + setup.kernel_bytes > setup.ram_bytes - DTB_SPACE
        || setup.initrd_offset + setup.initrd_bytes > setup.ram_bytes - DTB_SPACE) {
        say(console, "vmm: the setup does not describe this RAM\n");
        kosmos_exit(1);
    }

    if ((at = kosmos_mem_map(ram_cap)) < 0) {
        say(console, "vmm: could not map the guest's RAM\n");
        kosmos_exit(1);
    }

    ram = (uint8_t *)at;
    tree = write_tree(ram + setup.ram_bytes - DTB_SPACE, DTB_SPACE);

    if (tree == 0) {
        say(console, "vmm: the device tree did not fit\n");
        kosmos_exit(1);
    }

    if ((guest = kosmos_guest_create()) < 0) {
        say(console, guest == SYS_ERR_NO_DEVICE
                     ? "vmm: this machine cannot run a guest - it was not started at EL2\n"
                     : "vmm: no guest\n");
        kosmos_exit(1);
    }

    if (kosmos_guest_map(guest, ram_cap, VMM_RAM_BASE) != 0
        || (vcpu = kosmos_vcpu_create(guest)) < 0) {
        say(console, "vmm: the guest's RAM or its vCPU was refused\n");
        kosmos_exit(1);
    }

    tell("starting, RAM and kernel bytes ", (unsigned long)setup.ram_bytes,
         (unsigned long)setup.kernel_bytes);

    /*
     * **On core zero**, which is the one core every way of starting this
     * machine leaves at EL2: QEMU's PSCI starts the others at EL1 (`hyp.S`,
     * `hyp_present`). A board's firmware that starts every core at EL2 would
     * let it run anywhere.
     */
    thread = kosmos_thread_start_on(run, 0, 0);

    if (thread < 0) {
        say(console, "vmm: no thread on core zero\n");
        kosmos_exit(1);
    }

    kosmos_exit((int)kosmos_thread_wait((unsigned long)thread));
}
