/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The PC a sandbox's guest sees - `x86dev.h` says what and why. Each part
 * is written from the datasheet's behaviour as Linux relies on it, and the
 * Linux file that relies on it is named beside it, since that is what a
 * difference will show up in.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "x86dev.h"

/* ================================================================ 8259A */

/*
 * Intel's 8259A datasheet: ICW1 to ICW4 on initialisation, then OCW1 (the
 * mask) on the odd port and OCW2 and OCW3 on the even one. Linux's
 * `arch/x86/kernel/i8259.c` writes ICW1 0x11, ICW2 0x30 and 0x38, ICW3 4 and
 * 2, ICW4 1; probes the controller by masking every line and reading the
 * mask back; acknowledges with a specific EOI; and reads the in-service
 * register (OCW3 0x0B) to tell a real interrupt from a spurious one.
 *
 * Fixed priority, line 0 highest, which is all ICW4 1 and Linux's OCW2s
 * ask for. A line held by a level source stays requested while it is held
 * and is offered again after its end of interrupt: the 16550 and virtio
 * hold theirs until the guest has seen to them.
 */
void x86_pic_reset(struct x86_pic *p)
{
    memset(p, 0, sizeof(*p));
    p->chip[0].imr = p->chip[1].imr = 0xff;
    p->chip[0].base = 0x08;
    p->chip[1].base = 0x70;
}

static struct x86_pic_chip *chip_of(struct x86_pic *p, uint16_t port)
{
    return (port & 0x80) ? &p->chip[1] : &p->chip[0];
}

/* The highest-priority line asked for and not masked, or -1. */
static int requested(const struct x86_pic_chip *c, uint8_t extra)
{
    uint8_t asked = (uint8_t)((c->irr | c->level | extra) & ~c->imr);
    int i;

    for (i = 0; i < 8; i++) {
        if ((c->isr >> i) & 1) {
            return -1;                  /* it, or something above it, in service */
        }

        if ((asked >> i) & 1) {
            return i;
        }
    }

    return -1;
}

/* The slave asks on the master's line 2 while it has one to give. */
static uint8_t cascade(const struct x86_pic *p)
{
    return requested(&p->chip[1], 0) >= 0 ? (uint8_t)(1u << 2) : 0;
}

void x86_pic_write(struct x86_pic *p, uint16_t port, uint8_t v)
{
    struct x86_pic_chip *c;

    if (port == 0x4d0 || port == 0x4d1) {
        p->chip[port & 1].elcr = v;
        return;
    }

    c = chip_of(p, port);

    if ((port & 1) == 0) {
        if (v & 0x10) {                                 /* ICW1 */
            c->irr = c->isr = c->imr = 0;
            c->want_icw4 = (v & 1) != 0;
            c->single = (v & 2) != 0;
            c->auto_eoi = false;
            c->read_isr = false;
            c->step = 1;
            return;
        }

        if (v & 0x08) {                                 /* OCW3 */
            if ((v & 0x02) != 0) {
                c->read_isr = (v & 0x01) != 0;
            }

            return;
        }

        /* OCW2: an end of interrupt, non-specific or for line `v & 7`. */
        switch (v >> 5) {
        case 1: case 5: {                               /* non-specific */
            int i;

            for (i = 0; i < 8; i++) {
                if ((c->isr >> i) & 1) {
                    c->isr &= (uint8_t)~(1u << i);
                    break;
                }
            }
            break;
        }
        case 3: case 7:                                 /* specific */
            c->isr &= (uint8_t)~(1u << (v & 7));
            break;
        default:
            break;                                      /* priorities: fixed here */
        }

        return;
    }

    switch (c->step) {
    case 1:                                             /* ICW2 */
        c->base = v & 0xf8;
        c->step = c->single ? (c->want_icw4 ? 3 : 0) : 2;
        return;
    case 2:                                             /* ICW3, the cascade */
        c->step = c->want_icw4 ? 3 : 0;
        return;
    case 3:                                             /* ICW4 */
        c->auto_eoi = (v & 2) != 0;
        c->step = 0;
        return;
    default:
        c->imr = v;                                     /* OCW1 */
        return;
    }
}

uint8_t x86_pic_read(struct x86_pic *p, uint16_t port)
{
    struct x86_pic_chip *c;

    if (port == 0x4d0 || port == 0x4d1) {
        return p->chip[port & 1].elcr;
    }

    c = chip_of(p, port);

    if (port & 1) {
        return c->imr;
    }

    return c->read_isr ? c->isr : (uint8_t)(c->irr | c->level | (c == &p->chip[0] ? cascade(p) : 0));
}

void x86_pic_edge(struct x86_pic *p, unsigned irq)
{
    if (irq < 16) {
        p->chip[irq / 8].irr |= (uint8_t)(1u << (irq % 8));
    }
}

void x86_pic_level(struct x86_pic *p, unsigned irq, bool high)
{
    struct x86_pic_chip *c;

    if (irq >= 16) {
        return;
    }

    c = &p->chip[irq / 8];

    if (high) {
        c->level |= (uint8_t)(1u << (irq % 8));
    } else {
        c->level &= (uint8_t)~(1u << (irq % 8));
    }
}

bool x86_pic_pending(const struct x86_pic *p)
{
    return requested(&p->chip[0], cascade(p)) >= 0;
}

int x86_pic_take(struct x86_pic *p)
{
    struct x86_pic_chip *m = &p->chip[0], *s = &p->chip[1];
    int line = requested(m, cascade(p)), sline;

    if (line < 0) {
        return -1;
    }

    if (line != 2 || (cascade(p) == 0)) {
        m->irr &= (uint8_t)~(1u << line);

        if (!m->auto_eoi) {
            m->isr |= (uint8_t)(1u << line);
        }

        return m->base + line;
    }

    sline = requested(s, 0);
    s->irr &= (uint8_t)~(1u << sline);

    if (!s->auto_eoi) {
        s->isr |= (uint8_t)(1u << sline);
    }

    if (!m->auto_eoi) {
        m->isr |= 1u << 2;
    }

    return s->base + sline;
}

/* ================================================================= 8254 */

/*
 * Intel's 8254 datasheet: a control word at 0x43, then each channel's count
 * at 0x40 to 0x42, a byte or two; a count latched to be read as it was.
 * Linux's `i8253.c` drives channel 0 in mode 2, periodic, or mode 0, one
 * shot, for its clock tick; channel 2 is gated by port 0x61's bit 0 and its
 * output read back at bit 5, which is how `tsc.c` calibrates the TSC when
 * CPUID has not already said its frequency.
 *
 * Kept as a start time and a reload rather than counted down: the count at
 * any moment is arithmetic on the counter, so nothing has to run between
 * the guest's looks at it.
 */
void x86_pit_reset(struct x86_pit *t, uint64_t counter_hz)
{
    memset(t, 0, sizeof(*t));
    t->hz = counter_hz != 0 ? counter_hz : 1;
}

/* PIT ticks between two counter values, without overflowing the product. */
static uint64_t pit_ticks(const struct x86_pit *t, uint64_t from, uint64_t now)
{
    uint64_t d = now > from ? now - from : 0;

    return d / t->hz * X86_PIT_HZ + d % t->hz * X86_PIT_HZ / t->hz;
}

/* The counter after `ticks` of the PIT, from `from`, rounded up. */
static uint64_t counter_after(const struct x86_pit *t, uint64_t from, uint64_t ticks)
{
    return from + ticks / X86_PIT_HZ * t->hz + (ticks % X86_PIT_HZ * t->hz + X86_PIT_HZ - 1) / X86_PIT_HZ;
}

static bool periodic(const struct x86_pit_channel *c)
{
    return c->mode == 2 || c->mode == 3;
}

/* Ticks counted so far; a channel 2 whose gate is low does not count. */
static uint64_t counted(const struct x86_pit *t, unsigned n, uint64_t now)
{
    if (n == 2 && !t->gate2) {
        return 0;
    }

    return pit_ticks(t, t->ch[n].start, now);
}

static uint16_t count_now(const struct x86_pit *t, unsigned n, uint64_t now)
{
    const struct x86_pit_channel *c = &t->ch[n];
    uint64_t e = counted(t, n, now);

    if (!c->loaded) {
        return 0;
    }

    if (periodic(c)) {
        return (uint16_t)(c->reload - e % c->reload);
    }

    return (uint16_t)((c->reload - e) & 0xffff);       /* down through 0, and on */
}

static bool out2(const struct x86_pit *t, uint64_t now)
{
    const struct x86_pit_channel *c = &t->ch[2];
    uint64_t e = counted(t, 2, now);

    if (!c->loaded) {
        return true;
    }

    if (c->mode == 3) {
        return e % c->reload < c->reload / 2;
    }

    if (c->mode == 2) {
        return true;
    }

    return e >= c->reload;
}

static void load(struct x86_pit_channel *c, uint32_t value, uint64_t now)
{
    c->reload = value == 0 ? 65536u : value;
    c->start = now;
    c->fired = 0;
    c->loaded = true;
}

void x86_pit_write(struct x86_pit *t, uint16_t port, uint8_t v, uint64_t now)
{
    struct x86_pit_channel *c;

    if (port == 0x61) {
        bool rising = (v & 1) && !t->gate2;

        t->gate2 = (v & 1) != 0;
        t->speaker = (v & 2) != 0;

        if (rising) {
            t->ch[2].start = now;
            t->ch[2].fired = 0;
        }

        return;
    }

    if (port == 0x43) {
        unsigned n = v >> 6;

        if (n == 3) {                                   /* read-back: latch counts */
            unsigned k;

            for (k = 0; k < 3; k++) {
                if ((v & 0x20) == 0 && (v >> (k + 1)) & 1) {
                    t->ch[k].latched = true;
                    t->ch[k].latch = count_now(t, k, now);
                    t->ch[k].read_high = false;
                }
            }

            return;
        }

        c = &t->ch[n];

        if (((v >> 4) & 3) == 0) {                      /* counter latch */
            c->latched = true;
            c->latch = count_now(t, n, now);
            c->read_high = false;
            return;
        }

        c->access = (v >> 4) & 3;
        c->mode = (v >> 1) & 7;

        if (c->mode > 5) {
            c->mode -= 4;                               /* 6 and 7 are 2 and 3 */
        }

        c->loaded = false;
        c->write_high = c->read_high = c->latched = false;
        return;
    }

    if (port < 0x40 || port > 0x42) {
        return;
    }

    c = &t->ch[port - 0x40];

    switch (c->access) {
    case 1:
        load(c, v, now);
        break;
    case 2:
        load(c, (uint32_t)v << 8, now);
        break;
    default:
        if (!c->write_high) {
            c->partial = v;
            c->write_high = true;
        } else {
            load(c, c->partial | (uint32_t)v << 8, now);
            c->write_high = false;
        }
        break;
    }
}

uint8_t x86_pit_read(struct x86_pit *t, uint16_t port, uint64_t now)
{
    struct x86_pit_channel *c;
    uint16_t value;
    uint8_t byte;

    if (port == 0x61) {
        t->refresh = !t->refresh;
        return (uint8_t)((t->gate2 ? 1 : 0) | (t->speaker ? 2 : 0)
                         | (t->refresh ? 0x10 : 0) | (out2(t, now) ? 0x20 : 0));
    }

    if (port < 0x40 || port > 0x42) {
        return 0xff;
    }

    c = &t->ch[port - 0x40];
    value = c->latched ? c->latch : count_now(t, port - 0x40, now);

    if (c->access == 1) {
        byte = (uint8_t)value;
        c->latched = false;
    } else if (c->access == 2) {
        byte = (uint8_t)(value >> 8);
        c->latched = false;
    } else if (!c->read_high) {
        byte = (uint8_t)value;
        c->read_high = true;
    } else {
        byte = (uint8_t)(value >> 8);
        c->read_high = false;
        c->latched = false;
    }

    return byte;
}

bool x86_pit_due(struct x86_pit *t, uint64_t now)
{
    struct x86_pit_channel *c = &t->ch[0];
    uint64_t total;

    if (!c->loaded) {
        return false;
    }

    total = periodic(c) ? counted(t, 0, now) / c->reload
                        : (counted(t, 0, now) >= c->reload ? 1 : 0);

    if (total > c->fired) {
        c->fired = total;
        return true;
    }

    return false;
}

uint64_t x86_pit_next(const struct x86_pit *t, uint64_t now)
{
    const struct x86_pit_channel *c = &t->ch[0];

    (void)now;

    if (!c->loaded || (!periodic(c) && c->fired > 0)) {
        return ~0ull;
    }

    return counter_after(t, c->start, (uint64_t)c->reload * (periodic(c) ? c->fired + 1 : 1));
}

/* ================================================================ 16550A */

/*
 * National's PC16550D datasheet, as Linux's `8250_port.c` finds and uses
 * it: the scratch register kept, FIFOs said in the IIR's top bits once
 * enabled, a modem loopback whose status lines follow the control lines -
 * which is how it is told from an 8250 - and the transmitter always empty,
 * since a byte written is a byte in the console at once. Its interrupt
 * goes out only with OUT2 set in the modem control register, which is how a
 * PC has always gated a serial port's line.
 */
#define LCR_DLAB        0x80
#define MCR_OUT2        0x08
#define MCR_LOOP        0x10
#define IER_RDI         0x01
#define IER_THRI        0x02

void x86_uart_reset(struct x86_uart *u)
{
    memset(u, 0, sizeof(*u));
}

static uint8_t iir_of(const struct x86_uart *u, const struct x86_uart_io *io)
{
    uint8_t fifo = (u->fcr & 1) ? 0xc0 : 0;

    if ((u->ier & IER_RDI) && !(u->mcr & MCR_LOOP) && io->ready()) {
        return fifo | 0x04;
    }

    if ((u->ier & IER_THRI) && u->thre) {
        return fifo | 0x02;
    }

    return fifo | 0x01;
}

void x86_uart_write(struct x86_uart *u, unsigned off, uint8_t v, const struct x86_uart_io *io)
{
    switch (off) {
    case 0:
        if (u->lcr & LCR_DLAB) {
            u->dll = v;
        } else {
            if (!(u->mcr & MCR_LOOP)) {
                io->put((char)v);
            }

            u->thre = true;
        }
        break;
    case 1:
        if (u->lcr & LCR_DLAB) {
            u->dlm = v;
        } else {
            bool was = (u->ier & IER_THRI) != 0;

            u->ier = v & 0x0f;

            if ((u->ier & IER_THRI) && !was) {
                u->thre = true;             /* it is empty, and now it is asked */
            }
        }
        break;
    case 2: u->fcr = v; break;
    case 3: u->lcr = v; break;
    case 4: u->mcr = v & 0x1f; break;
    case 7: u->scr = v; break;
    default: break;
    }
}

uint8_t x86_uart_read(struct x86_uart *u, unsigned off, const struct x86_uart_io *io)
{
    switch (off) {
    case 0:
        if (u->lcr & LCR_DLAB) {
            return u->dll;
        } else {
            int c = (u->mcr & MCR_LOOP) ? -1 : io->take();

            return c < 0 ? 0 : (uint8_t)c;
        }
    case 1:
        return (u->lcr & LCR_DLAB) ? u->dlm : u->ier;
    case 2: {
        uint8_t iir = iir_of(u, io);

        if ((iir & 0x0f) == 0x02) {
            u->thre = false;                /* read, so said */
        }

        return iir;
    }
    case 3: return u->lcr;
    case 4: return u->mcr;
    case 5:
        return (uint8_t)(0x60 | (!(u->mcr & MCR_LOOP) && io->ready() ? 1 : 0));
    case 6:
        if (u->mcr & MCR_LOOP) {
            return (uint8_t)((u->mcr & 1) << 5 | (u->mcr & 2) << 3
                             | (u->mcr & 4) << 4 | (u->mcr & 8) << 4);
        }

        return 0xb0;                        /* DCD, DSR, CTS */
    case 7: return u->scr;
    default: return 0xff;
    }
}

bool x86_uart_raised(const struct x86_uart *u, const struct x86_uart_io *io)
{
    return (u->mcr & MCR_OUT2) && (iir_of(u, io) & 1) == 0;
}

/* ================================================================= CPUID */

/*
 * **What the guest is told it has**: the processor's own answer less what
 * this machine does not give it. No local APIC or x2APIC, so Linux uses the
 * 8259 and the 8254; no XSAVE, AVX or AVX-512, whose state the monitor
 * does not swap (it swaps the FXSAVE area); no VMX, SMX, MONITOR or
 * RDTSCP, PCID or INVPCID, which the VMCS does not enable; no
 * speculation-control MSRs; and one processor, with no topology to report.
 * The hypervisor bit set, and nobody's signature behind it. What is left
 * is the instruction set Linux needs and the TSC's frequency (leaves 0x15
 * and 0x16), so it need not calibrate.
 */
#define BIT(n)  (1u << (n))

void x86_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    (void)sub;

    switch (leaf) {
    case 0:
        if (r[0] > 0x16) {
            r[0] = 0x16;
        }
        return;
    case 1:
        r[1] &= 0x0000ffffu;                            /* APIC id 0, one logical processor */
        r[1] |= 1u << 16;
        r[2] &= ~(BIT(3) | BIT(5) | BIT(6) | BIT(7) | BIT(8) | BIT(15) | BIT(17)
                  | BIT(18) | BIT(21) | BIT(24) | BIT(26) | BIT(27) | BIT(28));
        r[2] |= BIT(31);                                /* a hypervisor */
        r[3] &= ~(BIT(7) | BIT(9) | BIT(12) | BIT(14) | BIT(18) | BIT(21) | BIT(22)
                  | BIT(27) | BIT(28) | BIT(29) | BIT(31));
        return;
    case 2:
    case 0x15:
    case 0x16:
        return;
    case 4:
        r[0] &= 0x00003fffu;                            /* one core, one thread a cache */
        if ((r[0] & 0x1f) == 0) {
            r[1] = r[2] = r[3] = 0;
        }
        return;
    case 7:
        if (sub != 0) {
            r[0] = r[1] = r[2] = r[3] = 0;
            return;
        }

        r[0] = 0;
        r[1] &= BIT(0) | BIT(3) | BIT(7) | BIT(8) | BIT(9) | BIT(18) | BIT(19)
                | BIT(20) | BIT(23);                    /* FSGSBASE BMI SMEP BMI2 ERMS RDSEED ADX SMAP CLFLUSHOPT */
        r[2] = 0;
        r[3] = 0;
        return;
    case 0x80000000u:
        if (r[0] > 0x80000008u) {
            r[0] = 0x80000008u;
        }
        return;
    case 0x80000001u:
        r[2] &= BIT(0) | BIT(5) | BIT(8);               /* LAHF, ABM, PREFETCHW */
        r[3] &= BIT(11) | BIT(20) | BIT(26) | BIT(29);  /* SYSCALL, NX, 1 GB pages, long mode */
        return;
    case 0x80000002u: case 0x80000003u: case 0x80000004u:
    case 0x80000006u:
        return;                                         /* its name, its caches */
    case 0x80000007u:
        r[0] = r[1] = r[2] = 0;
        r[3] &= BIT(8);                                 /* the TSC is invariant */
        return;
    case 0x80000008u:
        r[2] = 0;                                       /* address sizes, no cores */
        return;
    default:
        r[0] = r[1] = r[2] = r[3] = 0;
        return;
    }
}

/* ============================================================ the boot */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16; }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/*
 * The setup header's offsets, `Documentation/arch/x86/boot.rst`: setup_sects
 * 0x1f1, the header's magic "HdrS" at 0x202 and its version at 0x206,
 * loadflags 0x211, xloadflags 0x236 (bit 0, a 64-bit entry at 0x200 of the
 * protected-mode kernel), init_size 0x260; and the length of the header,
 * 0x202 plus the byte at 0x201.
 */
bool x86_bzimage(const uint8_t *image, uint64_t bytes, uint64_t *setup_bytes, uint64_t *init_size)
{
    unsigned sects;

    if (bytes < 0x1000 || le32(image + 0x202) != 0x53726448u        /* "HdrS" */
        || le16(image + 0x206) < 0x020c
        || (le16(image + 0x236) & 1) == 0 || (image[0x211] & 1) == 0) {
        return false;
    }

    sects = image[0x1f1] != 0 ? image[0x1f1] : 4;
    *setup_bytes = (uint64_t)(sects + 1) * 512u;
    *init_size = le32(image + 0x260);
    return *setup_bytes < bytes;
}

bool x86_boot(uint8_t *ram, uint64_t ram_bytes, uint64_t image_at, uint64_t image_bytes,
              uint64_t initrd_at, uint64_t initrd_bytes, const char *cmdline,
              struct x86_boot *out, const char **why)
{
    const uint8_t *image = ram + image_at;
    uint8_t *bp = ram + X86_BOOT_PARAMS;
    uint64_t setup_bytes, init_size, *table;
    unsigned header_end, i, j;
    size_t n;

    if (image_at + image_bytes > ram_bytes || ram_bytes > 0xc0000000ul) {
        *why = "the kernel is not inside the RAM, or the RAM is over 3 GB";
        return false;
    }

    if (!x86_bzimage(image, image_bytes, &setup_bytes, &init_size)) {
        *why = "the kernel is not a bzImage with a 64-bit entry";
        return false;
    }

    if (image_at + setup_bytes != X86_BOOT_KERNEL) {
        *why = "the kernel's protected-mode part is not at 16 MB";
        return false;
    }

    if (X86_BOOT_KERNEL + init_size > ram_bytes
        || (initrd_bytes != 0 && (initrd_at < X86_BOOT_KERNEL + init_size
                                  || initrd_at + initrd_bytes > ram_bytes))) {
        *why = "the RAM is not room for the kernel to unpack itself and the initramfs";
        return false;
    }

    /* The zero page, with the kernel's setup header copied into it. */
    memset(bp, 0, 4096);
    header_end = 0x202u + image[0x201];
    memcpy(bp + 0x1f1, image + 0x1f1, header_end - 0x1f1);
    put16(bp + 0x1fa, 0xffff);                          /* vid_mode: normal */
    bp[0x210] = 0xff;                                   /* type_of_loader: undefined */
    put32(bp + 0x218, (uint32_t)initrd_at);
    put32(bp + 0x21c, (uint32_t)initrd_bytes);
    put32(bp + 0x0c0, (uint32_t)(initrd_at >> 32));
    put32(bp + 0x0c4, (uint32_t)(initrd_bytes >> 32));
    put32(bp + 0x228, (uint32_t)X86_BOOT_CMDLINE);

    if (initrd_bytes == 0) {
        put32(bp + 0x218, 0);
    }

    /* The command line, cut at what the kernel reads. */
    n = strlen(cmdline);

    if (n > X86_BOOT_CMDLINE_MAX - 1) {
        n = X86_BOOT_CMDLINE_MAX - 1;
    }

    memcpy(ram + X86_BOOT_CMDLINE, cmdline, n);
    ram[X86_BOOT_CMDLINE + n] = 0;

    /* The e820 map: the first 636 KB, the old hole to 1 MB, and the rest. */
    {
        static const uint64_t low = 0x9f000ul;
        uint8_t *e = bp + 0x2d0;

        put64(e, 0);              put64(e + 8, low);                   put32(e + 16, 1);
        put64(e + 20, low);       put64(e + 28, 0x100000ul - low);     put32(e + 36, 2);
        put64(e + 40, 0x100000ul); put64(e + 48, ram_bytes - 0x100000ul); put32(e + 56, 1);
        bp[0x1e8] = 3;
    }

    /* `__BOOT_CS` 0x10, 64-bit code; `__BOOT_DS` 0x18, flat data. */
    table = (uint64_t *)(ram + X86_BOOT_GDT);
    table[0] = table[1] = 0;
    table[2] = 0x00af9a000000ffffull;
    table[3] = 0x00cf92000000ffffull;

    /* The first 4 GB, identity, in 2 MB pages. */
    table = (uint64_t *)(ram + X86_BOOT_PML4);
    memset(table, 0, 6 * 4096);
    table[0] = (X86_BOOT_PML4 + 0x1000) | 3;

    for (i = 0; i < 4; i++) {
        uint64_t *pdpt = (uint64_t *)(ram + X86_BOOT_PML4 + 0x1000);
        uint64_t *pd = (uint64_t *)(ram + X86_BOOT_PML4 + 0x2000 + 0x1000 * i);

        pdpt[i] = (X86_BOOT_PML4 + 0x2000 + 0x1000 * i) | 3;

        for (j = 0; j < 512; j++) {
            pd[j] = ((uint64_t)(i * 512 + j) << 21) | 0x83;
        }
    }

    out->rip = X86_BOOT_KERNEL + 0x200;
    out->rsi = X86_BOOT_PARAMS;
    out->rsp = X86_BOOT_STACK;
    out->cr3 = X86_BOOT_PML4;
    out->gdt_base = X86_BOOT_GDT;
    out->gdt_limit = 4 * 8 - 1;
    return true;
}

/* ========================================================= the page walk */

bool x86_walk(const uint8_t *ram, uint64_t ram_bytes, uint64_t cr3, uint64_t va, uint64_t *pa)
{
    uint64_t table = cr3 & 0x000ffffffffff000ull;
    int level;

    for (level = 3; level >= 0; level--) {
        unsigned index = (unsigned)(va >> (12 + 9 * level)) & 511;
        uint64_t entry;

        if (table + 4096 > ram_bytes) {
            return false;
        }

        memcpy(&entry, ram + table + 8 * index, 8);

        if ((entry & 1) == 0) {
            return false;
        }

        if (level == 0 || ((level == 1 || level == 2) && (entry & 0x80))) {
            uint64_t span = 1ull << (12 + 9 * level);

            *pa = (entry & 0x000ffffffffff000ull & ~(span - 1)) | (va & (span - 1));
            return true;
        }

        table = entry & 0x000ffffffffff000ull;
    }

    return false;
}

/* ========================================================== a `mov` */

/*
 * The forms a compiler makes of Linux's `readb`, `readw`, `readl`,
 * `writeb` and the rest (`asm/io.h`, which are `mov` with a memory operand
 * and nothing else): 88, 89, 8A, 8B, C6 /0, C7 /0, and 0F B6 and B7, the
 * zero-extending loads - with the operand-size and REX prefixes, a ModRM, a
 * SIB and a displacement, which say nothing about the address (the exit
 * already has it) but everything about the instruction's length. Intel SDM
 * vol. 2, chapter 2.
 */
bool x86_decode_mov(const uint8_t *code, unsigned avail, struct x86_mov *m)
{
    unsigned i = 0, op, mod, rm;
    bool size16 = false;
    uint8_t rex = 0, modrm;

    memset(m, 0, sizeof(*m));

    for (;;) {
        if (i >= avail || i >= 14) {
            return false;
        }

        switch (code[i]) {
        case 0x66: size16 = true; i++; continue;
        case 0x67: case 0x2e: case 0x3e: case 0x26: case 0x36:
        case 0x64: case 0x65: case 0xf0: case 0xf2: case 0xf3:
            i++;
            continue;
        default:
            break;
        }

        break;
    }

    if (code[i] >= 0x40 && code[i] <= 0x4f) {
        rex = code[i++];
    }

    if (i >= avail) {
        return false;
    }

    op = code[i++];

    if (op == 0x0f) {
        if (i >= avail) {
            return false;
        }

        op = 0x0f00 | code[i++];
    }

    {
        unsigned wide = (rex & 8) ? 8 : size16 ? 2 : 4;

        switch (op) {
        case 0x88: m->write = true; m->size = 1; break;
        case 0x89: m->write = true; m->size = wide; break;
        case 0x8a: m->size = 1; m->dest_size = 1; break;
        case 0x8b: m->size = wide; m->dest_size = wide; break;
        case 0xc6: m->write = true; m->size = 1; m->reg = -1; break;
        case 0xc7: m->write = true; m->size = wide; m->reg = -1; break;
        case 0x0fb6: m->size = 1; m->dest_size = wide; break;
        case 0x0fb7: m->size = 2; m->dest_size = wide; break;
        default: return false;
        }
    }

    if (i >= avail) {
        return false;
    }

    modrm = code[i++];
    mod = modrm >> 6;
    rm = modrm & 7;

    if (mod == 3) {
        return false;                       /* a register, not memory */
    }

    if (m->reg != -1) {
        m->reg = (int)(((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0));

        /* AH, CH, DH and BH, where there is no REX to say SPL and the rest. */
        if (m->size == 1 && (op == 0x88 || op == 0x8a) && rex == 0 && m->reg >= 4) {
            m->high8 = true;
            m->reg -= 4;
        }
    } else if (((modrm >> 3) & 7) != 0) {
        return false;                       /* C6 and C7 are only /0 */
    }

    if (rm == 4) {
        if (i >= avail) {
            return false;
        }

        if ((code[i++] & 7) == 5 && mod == 0) {
            i += 4;                         /* a SIB with no base: a disp32 */
        }
    } else if (rm == 5 && mod == 0) {
        i += 4;                             /* RIP-relative */
    }

    i += mod == 1 ? 1 : mod == 2 ? 4 : 0;

    if (m->reg == -1) {
        unsigned bytes = m->size == 1 ? 1 : m->size == 2 ? 2 : 4;
        uint64_t imm = 0;
        unsigned k;

        if (i + bytes > avail) {
            return false;
        }

        for (k = 0; k < bytes; k++) {
            imm |= (uint64_t)code[i + k] << (8 * k);
        }

        if (m->size == 8 && (imm & 0x80000000ull)) {
            imm |= 0xffffffff00000000ull;   /* imm32, sign-extended */
        }

        m->imm = imm;
        i += bytes;
    }

    if (i > avail) {
        return false;
    }

    m->length = i;
    return true;
}

static uint64_t mask_of(unsigned size)
{
    return size >= 8 ? ~0ull : (1ull << (8 * size)) - 1;
}

uint64_t x86_mov_value(const struct x86_mov *m, const uint64_t gpr[16])
{
    if (m->reg < 0) {
        return m->imm & mask_of(m->size);
    }

    if (m->high8) {
        return (gpr[m->reg] >> 8) & 0xff;
    }

    return gpr[m->reg] & mask_of(m->size);
}

void x86_mov_load(const struct x86_mov *m, uint64_t gpr[16], uint64_t value)
{
    uint64_t v = value & mask_of(m->size);

    if (m->reg < 0) {
        return;
    }

    if (m->high8) {
        gpr[m->reg] = (gpr[m->reg] & ~0xff00ull) | v << 8;
    } else if (m->dest_size >= 4) {
        gpr[m->reg] = v;                    /* a 32-bit destination clears the top too */
    } else {
        gpr[m->reg] = (gpr[m->reg] & ~mask_of(m->dest_size)) | v;
    }
}
