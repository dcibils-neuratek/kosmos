/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The ARM machine a sandbox's monitor answers for** - `vmm.h`, and
 * `docs/virtualization.md` V3. Described to the guest in a device tree at
 * the top of its RAM:
 *
 * - **a PL011 serial port** at `VMM_UART_BASE`, which nothing maps, so every
 *   touch of it stops the guest with a stage-2 fault that says the register,
 *   the size and whether it was a write (ESR's ISV): a byte written to its
 *   data register is a byte in the console the monitor keeps, and what is
 *   typed comes back to its receiver;
 * - **PSCI**, by `hvc`, the firmware calls Linux makes - its version, and
 *   switching the machine off;
 * - **a GICv3**: its distributor and its one redistributor as register
 *   files here, which say what is enabled and how it is configured - and
 *   delivery through the processor's own virtual CPU interface: an
 *   interrupt the guest should take is written into a list register, and
 *   the guest acknowledges and ends it without a word to this process;
 * - **its timer**, the core's virtual one, which is the guest's while it
 *   runs: when it is due the timer's interrupt is injected, and when the
 *   guest waits (`wfi`, trapped) this process sleeps until it is;
 * - and everything else is said and stops the guest.
 */

#if defined(__aarch64__)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "vmmproto.h"
#include "fdt.h"
#include "vdev.h"
#include "vmmfs.h"
#include "vmmnet.h"
#include "vmm.h"

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
static uint64_t gic_enabled;            /* a bit an interrupt */

/* The PL011's interrupt state: what is masked in, and what is raised. */
static uint32_t uart_imsc;

/*
 * The machine, described: one processor started by PSCI over `hvc`, the
 * RAM, ARM's generic timer, a GICv3 and the PL011 - and the command line,
 * the initramfs if there is one, and where the console is.
 */
static size_t write_tree(uint8_t *at, size_t cap)
{
    struct fdt f;
    uint64_t ram_end = VMM_RAM_BASE + vmm_cfg.ram_bytes;
    uint32_t cells[12];

    fdt_begin(&f, at, cap);
    fdt_node(&f, "");
    fdt_prop_u32(&f, "#address-cells", 2);
    fdt_prop_u32(&f, "#size-cells", 2);
    fdt_prop_str(&f, "compatible", "linux,dummy-virt");
    fdt_prop_u32(&f, "interrupt-parent", 1);

    fdt_node(&f, "chosen");
    fdt_prop_str(&f, "bootargs", vmm_cfg.cmdline);
    fdt_prop_str(&f, "stdout-path", "/pl011@9000000");

    if (vmm_cfg.initrd_bytes != 0) {
        uint64_t start = VMM_RAM_BASE + vmm_cfg.initrd_offset;
        uint64_t end = start + vmm_cfg.initrd_bytes;

        cells[0] = (uint32_t)(start >> 32); cells[1] = (uint32_t)start;
        fdt_prop_cells(&f, "linux,initrd-start", cells, 2);
        cells[0] = (uint32_t)(end >> 32); cells[1] = (uint32_t)end;
        fdt_prop_cells(&f, "linux,initrd-end", cells, 2);
    }

    fdt_end_node(&f);

    fdt_node(&f, "memory@40000000");
    fdt_prop_str(&f, "device_type", "memory");
    cells[0] = (uint32_t)(VMM_RAM_BASE >> 32); cells[1] = (uint32_t)VMM_RAM_BASE;
    cells[2] = (uint32_t)(vmm_cfg.ram_bytes >> 32); cells[3] = (uint32_t)vmm_cfg.ram_bytes;
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

        if (vmm_cfg.shares[i].root[0] == '\0') {
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
    if (vmm_networked()) {
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
    return UART_TXIS | (vmm_rx_empty() ? 0 : UART_RXIS | UART_RTIS);
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
        int c = vmm_rx_take();

        return c < 0 ? 0 : (uint64_t)c;
    }
    case 0x018:                             /* FR: transmit empty; RXFE */
        return 0x80 | (vmm_rx_empty() ? 0x10 : 0);
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
        vmm_guest_byte((char)value);
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
    uint64_t now = kosmos_ticks(), ticks = 1;

    /* A prompt has no newline after it: what the guest said goes in the
     * ring now, before it is waited on. */
    vmm_flush_out();

    if (!vmm_rx_empty() || deliver((struct vcpu_regs *)r)
        || (r->lr[0] | r->lr[1] | r->lr[2] | r->lr[3]) >> 62) {
        return;
    }

    /*
     * Until its timer is due, and one scheduler tick at the most - 4 ms -
     * which is how soon a key typed is seen: keys arrive on the other
     * thread, which has no way to wake this one but to be looked at. It was
     * two, and a key could wait 8 ms before the guest saw it (13 October).
     */
    if ((r->timer_ctl & 1) != 0 && (r->timer_ctl & 2) == 0 && r->timer_cval > now) {
        uint64_t wait = (r->timer_cval - now) * vmm_tick_hz / vmm_counter_hz;

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
        vmm_note("the guest switched itself off");
        return false;
    default:
        r->x[0] = PSCI_NOT_SUPPORTED;
        return true;
    }
}


/* The vCPU's thread: run, answer, run again, until the guest stops. */
void vmm_machine_run(unsigned long unused)
{
    struct vcpu_regs r;
    uint64_t dtb_at = vmm_cfg.ram_bytes - DTB_SPACE;
    uint64_t before;
    long why;

    (void)unused;
    memset(&r, 0, sizeof(r));
    r.pc = VMM_RAM_BASE + vmm_cfg.kernel_offset;
    r.pstate = 0x3c5;                       /* EL1h, D A I F masked */
    r.x[0] = VMM_RAM_BASE + dtb_at;         /* the device tree, as Linux wants */

    for (;;) {
        if (vmm_stop_asked) {
            vmm_note("stopped, as asked");
            vmm_finish();
        }

        deliver(&r);
        before = kosmos_ticks();
        why = kosmos_vcpu_run(vmm_vcpu, &r);
        vmm_guest_ticks += kosmos_ticks() - before;
        vmm_exits++;

        if (why == VCPU_EXIT_HOST) {
            continue;
        }

        if (why != VCPU_EXIT_TRAP) {
            vmm_tell("the vCPU would not run: ", (unsigned long)why, 0);
            vmm_finish();
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
                vmm_finish();
            }
            break;
        case EC_SMC:
            r.pc += 4;
            if (!psci(&r)) {
                vmm_finish();
            }
            break;
        case EC_DABT_LOW:
            if (!emulate(&r)) {
                vmm_tell("the guest touched what is not there, at ", r.ipa | (r.address & 0xfff), r.pc);
                vmm_finish();
            }
            break;
        default:
            vmm_tell("the guest stopped: syndrome and pc ", r.syndrome, r.pc);
            vmm_finish();
        }
    }
}

bool vmm_machine_start(void)
{
    gicd = calloc(1, GICD_BYTES);
    gicr = calloc(1, GICR_BYTES);
    return gicd != NULL && gicr != NULL;
}

/* An arm64 Image 2 MB aligned, and the device tree's 2 MB at the top. */
bool vmm_machine_setup_ok(void)
{
    return vmm_cfg.ram_bytes >= 4 * DTB_SPACE
        && (vmm_cfg.kernel_offset & (DTB_SPACE - 1)) == 0
        && vmm_cfg.kernel_offset + vmm_cfg.kernel_bytes <= vmm_cfg.ram_bytes - DTB_SPACE
        && vmm_cfg.initrd_offset + vmm_cfg.initrd_bytes <= vmm_cfg.ram_bytes - DTB_SPACE;
}

bool vmm_machine_prepare(void)
{
    return write_tree(vmm_ram + vmm_cfg.ram_bytes - DTB_SPACE, DTB_SPACE) != 0;
}

#endif /* __aarch64__ */
