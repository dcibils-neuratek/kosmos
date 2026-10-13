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
 *   data register is a byte in the console this process keeps, which the
 *   window and `sandbox` read from it (`vmmproto.h`), and what they type
 *   comes back to its receiver;
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
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "vmmproto.h"
#include "appproto.h"
#include "fdt.h"
#include "vdev.h"
#include "vmmfs.h"
#include "vmmnet.h"
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

static long guest_ram = -1;
static uint8_t *ram;
static struct vmm_setup setup;

/* Whether it has a network device: outgoing, or ports carried in. */
static bool networked(void)
{
    for (unsigned i = 0; i < VMM_PORTS; i++) {
        if (setup.ports[i].from != 0) {
            return true;
        }
    }

    return setup.network != VMM_NET_NONE;
}
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

/* Allocated when the monitor starts, not in the system image's static data,
 * which every process carries. */
#define GICD_BYTES      0x10000u
#define GICR_BYTES      0x20000u

static uint8_t *gicd;
static uint8_t *gicr;
#if defined(__aarch64__)
static uint64_t gic_enabled;            /* a bit an interrupt */

/* The PL011's interrupt state: what is masked in, and what is raised. */
static uint32_t uart_imsc;
#endif

/*
 * What was typed and the guest has not read yet: a ring this process's
 * serving thread fills from `VMM_OP_KEYS` and the PL011's data register,
 * on the vCPU's thread, empties. One writes `rx_head`, the other `rx_tail`,
 * and nobody takes a lock - the discipline every ring here keeps.
 */
static uint8_t rx[256];
static unsigned rx_head, rx_tail;

/* The x86-64 machine's too, from X2. */
__attribute__((unused)) static bool rx_empty(void)
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

/* The x86-64 machine's too, from X2. */
__attribute__((unused)) static int rx_take(void)
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

static char *con;                           /* allocated at the start, as gicd is */
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

/* What the serving thread and the vCPU's thread tell each other. */
static volatile bool stop_asked;            /* VMM_OP_STOP while it runs */
static volatile bool ended;                 /* the guest is gone */
static volatile int exit_code = -1;         /* its command's, by its end line */
static volatile uint64_t guest_ticks, exits, started;

/* The guest's serial output, a line at a time. */
static char out[257];
static size_t out_len;

/*
 * A line, or what there is of one, into the console's ring - unless it is
 * the guest's end line: `end_token`, a space and its command's exit code,
 * which is the monitor's to read and not anybody's to see.
 */
static void flush_out(void)
{
    size_t token = strlen(setup.end_token);

    if (out_len == 0) {
        return;
    }

    if (token > 0 && out_len > token && memcmp(out, setup.end_token, token) == 0
        && out[token] == ' ') {
        int code = 0;
        size_t i = token + 1;

        while (i < out_len && out[i] >= '0' && out[i] <= '9') {
            code = code * 10 + (out[i++] - '0');
        }

        exit_code = code;
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

/* The x86-64 machine's too, from X2. */
__attribute__((unused)) static void guest_byte(char c)
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
        flush_out();
    }
}

/* A line of the monitor's own, into the console's ring. */
static void note(const char *text)
{
    flush_out();
    ring_put("vmm: ", 5);
    ring_put(text, strlen(text));
    ring_put("\n", 1);
}

/* The x86-64 machine's too, from X2. */
__attribute__((unused)) static void tell(const char *what, unsigned long a, unsigned long b)
{
    struct say_line line;

    say_begin(&line);
    say_text(&line, what);
    say_hex(&line, a, 0);
    say_text(&line, " ");
    say_hex(&line, b, 0);
    note(line.text);
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

    /* Its folders: a virtio-mmio device each, 0x200 apart, edge-triggered
     * from SPI 16 - QEMU `virt`'s own arrangement. */
    for (unsigned i = 0; i < VMM_SHARES; i++) {
        char node[32] = "virtio_mmio@a000000";
        uint64_t at = VMM_SHARE_BASE + 0x200ul * i;
        static const char hex[] = "0123456789abcdef";

        if (setup.shares[i].root[0] == '\0') {
            continue;
        }

        for (unsigned k = 0; k < 7; k++) {
            node[12 + k] = hex[(at >> (4 * (6 - k))) & 15];
        }

        fdt_node(&f, node);
        fdt_prop_str(&f, "compatible", "virtio,mmio");
        {
            uint32_t reg[4] = { 0, (uint32_t)at, 0, 0x200 };
            uint32_t irq[3] = { 0, VMM_SHARE_IRQ - 32 + i, 1 };

            fdt_prop_cells(&f, "reg", reg, 4);
            fdt_prop_cells(&f, "interrupts", irq, 3);
        }
        fdt_prop_empty(&f, "dma-coherent");
        fdt_end_node(&f);
    }

    /* Its network: one more virtio-mmio device, after its folders. */
    if (networked()) {
        uint32_t reg[4] = { 0, (uint32_t)VMM_NET_BASE, 0, 0x200 };
        uint32_t irq[3] = { 0, VMM_NET_IRQ - 32, 1 };

        fdt_node(&f, "virtio_mmio@a000800");
        fdt_prop_str(&f, "compatible", "virtio,mmio");
        fdt_prop_cells(&f, "reg", reg, 4);
        fdt_prop_cells(&f, "interrupts", irq, 3);
        fdt_prop_empty(&f, "dma-coherent");
        fdt_end_node(&f);
    }

    fdt_end_node(&f);
    return fdt_finish(&f);
}

#if defined(__aarch64__)
/*
 * **The PL011**, as far as a console needs it: a byte written to its data
 * register is a byte in the console's ring, a byte read from it is the
 * next one typed, and the transmitter is always empty. Its interrupt is
 * raised while the transmitter is empty and the guest asked to hear that
 * (TXIM) - which is how Linux's driver finishes a write - and while
 * something typed is waiting and it asked to hear that (RXIM, RTIM).
 */
#define UART_RXIS       (1u << 4)
#define UART_TXIS       (1u << 5)
#define UART_RTIS       (1u << 6)

static uint32_t uart_raw(void)
{
    return UART_TXIS | (rx_empty() ? 0 : UART_RXIS | UART_RTIS);
}

static bool uart_raised(void)
{
    return (uart_raw() & uart_imsc) != 0;
}

static uint64_t uart_read(uint64_t offset)
{
    static const uint8_t ids[8] = { 0x11, 0x10, 0x14, 0x00, 0x0d, 0xf0, 0x05, 0xb1 };

    switch (offset) {
    case 0x000: {                           /* DR: the next byte typed */
        int c = rx_take();

        return c < 0 ? 0 : (uint64_t)c;
    }
    case 0x018:                             /* FR: transmit empty; RXFE */
        return 0x80 | (rx_empty() ? 0x10 : 0);
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
    } else if (at >= VMM_NET_BASE && at < VMM_NET_BASE + 0x200) {
        uint64_t v = value;

        if (!vmmnet_mmio(at - VMM_NET_BASE, size, write, &v)) {
            return false;
        }

        got = v;
    } else if (at >= VMM_SHARE_BASE && at < VMM_SHARE_BASE + 0x200ul * VMM_SHARES) {
        uint64_t v = value;
        unsigned i = (unsigned)((at - VMM_SHARE_BASE) / 0x200);

        if (!vmmfs_mmio(i, (at - VMM_SHARE_BASE) % 0x200, size, write, &v)) {
            return false;
        }

        got = v;
    } else if (at >= VMM_GICD_BASE && at + size <= VMM_GICD_BASE + GICD_BYTES) {
        if (write) {
            gicd_write(at - VMM_GICD_BASE, size, value);
        } else {
            got = gicd_read(at - VMM_GICD_BASE, size);
        }
    } else if (at >= VMM_GICR_BASE && at + size <= VMM_GICR_BASE + GICR_BYTES) {
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

    vmmnet_pump();

    if (vmmnet_raised()) {
        offer(r, VMM_NET_IRQ);
        any = true;
    }

    for (unsigned i = 0; i < VMM_SHARES; i++) {
        if (vmmfs_raised(i)) {
            offer(r, VMM_SHARE_IRQ + i);
            any = true;
        }
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
    uint64_t now = kosmos_ticks(), ticks = vmmnet_busy() ? 1 : 2;

    /* A prompt has no newline after it: what the guest said goes in the
     * ring now, before it is waited on. */
    flush_out();

    if (!rx_empty() || deliver((struct vcpu_regs *)r)
        || (r->lr[0] | r->lr[1] | r->lr[2] | r->lr[3]) >> 62) {
        return;
    }

    /*
     * Until its timer is due, and two scheduler ticks at the most - 8 ms -
     * which is how soon a key typed is seen: keys arrive on the other
     * thread, which has no way to wake this one but to be looked at.
     */
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
        note("the guest switched itself off");
        return false;
    default:
        r->x[0] = PSCI_NOT_SUPPORTED;
        return true;
    }
}

#endif /* __aarch64__ */

/* The guest has ended - by itself, or because it was asked to. */
static void finish(void)
{
    flush_out();
    ended = true;
    kosmos_thread_exit(0);
}

#if defined(__x86_64__)
/*
 * **On x86-64 the kernel enters a guest (X1) and this half is not written
 * yet**: a 64-bit direct boot, a 16550 on ports, the PIC and the PIT, and
 * MMIO decoded from the instruction - `docs/sandboxes.md` X2. Until then
 * the guest is made, so the kernel's half is exercised, and stopped.
 */
static void run(unsigned long unused)
{
    (void)unused;
    note("x86-64 guests are not run by this monitor yet (docs/sandboxes.md, X2)");
    finish();
}
#else
/* The vCPU's thread: run, answer, run again, until the guest stops. */
static void run(unsigned long unused)
{
    struct vcpu_regs r;
    uint64_t dtb_at = setup.ram_bytes - DTB_SPACE;
    uint64_t before;
    long why;

    (void)unused;
    memset(&r, 0, sizeof(r));
    r.pc = VMM_RAM_BASE + setup.kernel_offset;
    r.pstate = 0x3c5;                       /* EL1h, D A I F masked */
    r.x[0] = VMM_RAM_BASE + dtb_at;         /* the device tree, as Linux wants */

    for (;;) {
        if (stop_asked) {
            note("stopped, as asked");
            finish();
        }

        deliver(&r);
        before = kosmos_ticks();
        why = kosmos_vcpu_run(vcpu, &r);
        guest_ticks += kosmos_ticks() - before;
        exits++;

        if (why == VCPU_EXIT_HOST) {
            continue;
        }

        if (why != VCPU_EXIT_TRAP) {
            tell("the vCPU would not run: ", (unsigned long)why, 0);
            finish();
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
                finish();
            }
            break;
        case EC_SMC:
            r.pc += 4;
            if (!psci(&r)) {
                finish();
            }
            break;
        case EC_DABT_LOW:
            if (!emulate(&r)) {
                tell("the guest touched what is not there, at ", r.ipa | (r.address & 0xfff), r.pc);
                finish();
            }
            break;
        default:
            tell("the guest stopped: syndrome and pc ", r.syndrome, r.pc);
            finish();
        }
    }
}

#endif

/* What a request is answered with. */
static void answer(const struct vmm_request *req, struct vmm_reply *rep)
{
    size_t got = 0, i;

    memset(rep, 0, sizeof(*rep));
    rep->state = ended ? VMM_ENDED : VMM_RUNNING;
    rep->exit_code = exit_code;
    rep->ram_bytes = setup.ram_bytes;
    rep->up_counter_ticks = kosmos_ticks() - started;
    rep->guest_counter_ticks = guest_ticks;
    rep->exits = exits;
    vmmnet_counted(&rep->net_in, &rep->net_out);
    memcpy(rep->name, setup.name, VMM_NAME_MAX);

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
        stop_asked = true;
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
    memcpy(name + 8, setup.name, strlen(setup.name) < APP_NAME_MAX - 9
                                 ? strlen(setup.name) : APP_NAME_MAX - 9);

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

void vmm_server(long running_cap, long ram_cap, long setup_cap, long more1, long more2)
{
    long home_cap = -1, net_cap = -1;
    long at, guest, thread, endpoint;
    const struct vmm_setup *s;
    unsigned long linger = 0;
    size_t tree;

    guest_ram = ram_cap;
    started = kosmos_ticks();
    gicd = calloc(1, GICD_BYTES);
    gicr = calloc(1, GICR_BYTES);
    con = malloc(CON_RING);

    if (gicd == NULL || gicr == NULL || con == NULL) {
        kosmos_exit(VMM_NO_GUEST);
    }

    {
        struct sysinfo info;

        memset(&info, 0, sizeof(info));

        if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0 && info.tick_hz != 0) {
            counter_hz = info.counter_hz;
            tick_hz = info.tick_hz;
        }
    }

    if ((at = kosmos_mem_map(setup_cap)) < 0) {
        kosmos_exit(VMM_NO_SETUP);
    }

    s = (const struct vmm_setup *)at;
    setup = *s;
    setup.cmdline[VMM_CMDLINE_MAX - 1] = '\0';
    setup.name[VMM_NAME_MAX - 1] = '\0';
    setup.end_token[VMM_TOKEN_MAX - 1] = '\0';

    if (setup.magic != VMM_SETUP_MAGIC || setup.version != VMM_SETUP_VERSION
        || setup.name[0] == '\0'
        || setup.ram_bytes != (uint64_t)kosmos_mem_size(ram_cap) * 4096u
        || setup.ram_bytes < 4 * DTB_SPACE
        || (setup.kernel_offset & (DTB_SPACE - 1)) != 0
        || setup.kernel_offset + setup.kernel_bytes > setup.ram_bytes - DTB_SPACE
        || setup.initrd_offset + setup.initrd_bytes > setup.ram_bytes - DTB_SPACE) {
        kosmos_exit(VMM_BAD_SETUP);
    }

    if ((at = kosmos_mem_map(ram_cap)) < 0) {
        kosmos_exit(VMM_NO_RAM);
    }

    ram = (uint8_t *)at;

    /* What it was handed after its first three: `/Home` if it has folders,
     * then `/Network` if it has the network (`fs.vm_start`). */
    {
        bool folders = false;

        for (unsigned i = 0; i < VMM_SHARES; i++) {
            folders = folders || setup.shares[i].root[0] != '\0';
        }

        home_cap = folders ? more1 : -1;
        net_cap = networked() ? (folders ? more2 : more1) : -1;

        if (networked()) {
            uint16_t from[VMM_PORTS], to[VMM_PORTS];

            if (net_cap < 0) {
                kosmos_exit(VMM_BAD_SHARE);
            }

            for (unsigned i = 0; i < VMM_PORTS; i++) {
                from[i] = setup.ports[i].from;
                to[i] = setup.ports[i].to;
            }

            unsigned refused = vmmnet_start(net_cap, (uint32_t)tick_hz,
                                            setup.network == VMM_NET_OUT, from, to, VMM_PORTS);

            for (unsigned i = 0; i < VMM_PORTS; i++) {
                if ((refused >> i & 1u) != 0) {
                    struct say_line line;

                    say_begin(&line);
                    say_text(&line, "Kosmos would not answer on port ");
                    say_dec(&line, from[i]);
                    say_text(&line, ", for the guest's ");
                    say_dec(&line, to[i]);
                    note(line.text);
                }
            }
        }
    }

    /* Its folders, served from `/Home` - which it was handed only if it has
     * any (`fs.vm_start`). */
    vdev_memory(ram, setup.ram_bytes, VMM_RAM_BASE);
    vmmfs_start(home_cap);

    for (unsigned i = 0; i < VMM_SHARES; i++) {
        char tag[8] = "share0";

        setup.shares[i].root[sizeof(setup.shares[i].root) - 1] = '\0';

        if (setup.shares[i].root[0] == '\0') {
            continue;
        }

        tag[5] = (char)('0' + i);

        if (home_cap < 0 || !vmmfs_add(i, tag, setup.shares[i].root, setup.shares[i].read_only != 0)) {
            kosmos_exit(VMM_BAD_SHARE);
        }
    }

    tree = write_tree(ram + setup.ram_bytes - DTB_SPACE, DTB_SPACE);

    if (tree == 0) {
        kosmos_exit(VMM_NO_TREE);
    }

    if ((guest = kosmos_guest_create()) < 0) {
        kosmos_exit(guest == SYS_ERR_NO_DEVICE ? VMM_NO_EL2 : VMM_NO_GUEST);
    }

    if (kosmos_guest_map(guest, ram_cap, VMM_RAM_BASE) != 0
        || (vcpu = kosmos_vcpu_create(guest)) < 0) {
        kosmos_exit(VMM_REFUSED);
    }

    if ((endpoint = register_name(running_cap)) < 0) {
        kosmos_exit(VMM_NO_NAME);
    }

    /*
     * **On core zero**, which is the one core every way of starting this
     * machine leaves at EL2: QEMU's PSCI starts the others at EL1 (`hyp.S`,
     * `hyp_present`). A board's firmware that starts every core at EL2 would
     * let it run anywhere.
     */
    thread = kosmos_thread_start_on(run, 0, 0);

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
        long got = kosmos_receive(endpoint, &msg, &sender, 0, tick_hz / 4);

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
        if (ended && (stop_asked || ++linger > 40)) {
            break;                          /* ten seconds, nobody asking */
        }
    }

    kosmos_exit(exit_code < 0 ? 0 : exit_code);
}
