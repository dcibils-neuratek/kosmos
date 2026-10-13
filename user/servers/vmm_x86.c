/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The PC a sandbox's monitor answers for** - `vmm.h`, and
 * `docs/sandboxes.md` X2. The kernel enters the guest through VT-x (X1);
 * this is everything a guest then asks of a machine that is not there:
 *
 * - **Linux's 64-bit boot**, with no firmware: the bzImage's protected-mode
 *   kernel at 16 MB, the zero page, the e820 map, the command line, a GDT
 *   and identity page tables written into its RAM (`x86_boot`), and the
 *   vCPU started in long mode at the kernel's 64-bit entry;
 * - **ports**, every `in` and `out` exiting: the two 8259s, the 8254 and
 *   port 0x61, a 16550 at 0x3F8 that is the guest's console, and the rest
 *   answered as a PC with nothing there answers - all ones;
 * - **CPUID**, the processor's own answer filtered (`x86_cpuid`): no APIC,
 *   which is why Linux is told `noapic nolapic` and runs on the 8259 and the
 *   8254; and **MSRs**, every one exiting but those `syscall` and TLS use,
 *   EFER and PAT kept, the rest read as 0 and written to nothing - on the
 *   M700, Linux 6.18 asked seven others (0x8b, 0x3a, 0x140, 0xce, 0x64e,
 *   0x34, 0xc0011029) and booted on the zeros;
 * - **virtio-mmio** for its folders and its network, the same devices as on
 *   ARM (`vdev.c`), found by EPT violations: the exit says where, and the
 *   instruction the guest stopped on - read through its own page tables
 *   and decoded (`x86_decode_mov`) - says what and how wide. Announced on
 *   the command line as `virtio_mmio.device=`, since there is no device
 *   tree to put them in;
 * - **`hlt`**: with interrupts on, the guest waits and this process sleeps
 *   until its timer is due; with them off, it has stopped - which is how
 *   Linux switches a PC off that has no ACPI.
 *
 * The devices themselves are `x86dev.c`, pure and held on the Mac by
 * `tools/test_x86dev.c`; this file is the glue, and is only ever run on a
 * PC with VT-x, since QEMU's TCG has none.
 */

#if defined(__x86_64__)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "vmmproto.h"
#include "vmmfs.h"
#include "vmmnet.h"
#include "x86dev.h"
#include "vmm.h"
#include "init/say.h"

/*
 * Where its virtio devices are, above any RAM it may have (at most 3 GB):
 * its folders 0x200 apart, then its network; and their lines on the 8259s,
 * the ones a PC's ISA bus leaves free.
 */
#define X86_VIRTIO_BASE     0xd0000000ul
#define X86_NET_BASE        (X86_VIRTIO_BASE + 0x200ul * VMM_SHARES)

static const unsigned share_irq[VMM_SHARES] = { 5, 7, 9, 10 };

#define X86_NET_IRQ         11u
#define X86_UART_IRQ        4u
#define X86_UART_PORT       0x3f8u

#define EXIT_EXCEPTION      0u
#define EXIT_TRIPLE_FAULT   2u
#define EXIT_WINDOW         7u
#define EXIT_CPUID          10u
#define EXIT_HLT            12u
#define EXIT_CR             28u
#define EXIT_IO             30u
#define EXIT_RDMSR          31u
#define EXIT_WRMSR          32u
#define EXIT_EPT            48u
#define EXIT_XSETBV         55u

#define RFLAGS_IF           (1ul << 9)
#define EFER_LMA            (1ul << 10)

static struct x86_pic pic;
static struct x86_pit pit;
static struct x86_uart uart;
static struct x86_boot boot;
static uint64_t pat = 0x0007040600070406ull;    /* the reset value */
static uint8_t cmos_index;
static char line[X86_BOOT_CMDLINE_MAX];

static bool uart_ready(void) { return !vmm_rx_empty(); }
static void uart_put(char c) { vmm_guest_byte(c); }

static const struct x86_uart_io console = { uart_ready, vmm_rx_take, uart_put };

bool vmm_machine_start(void)
{
    x86_pic_reset(&pic);
    x86_uart_reset(&uart);
    return true;
}

/* The kernel inside the RAM, the initramfs too, and RAM below the devices. */
bool vmm_machine_setup_ok(void)
{
    return vmm_cfg.ram_bytes >= 64ul << 20 && vmm_cfg.ram_bytes <= 0xc0000000ul
        && vmm_cfg.kernel_offset + vmm_cfg.kernel_bytes <= vmm_cfg.ram_bytes
        && vmm_cfg.initrd_offset + vmm_cfg.initrd_bytes <= vmm_cfg.ram_bytes;
}

/* Appended to the line, `" word"`, as far as it fits. */
static void append(const char *word)
{
    size_t at = strlen(line), n = strlen(word);

    if (at + n < sizeof(line)) {
        memcpy(line + at, word, n + 1);
    }
}

static void append_hex(unsigned long v)
{
    static const char digits[] = "0123456789abcdef";
    char text[19] = "0x";
    int k, at = 2;
    bool started = false;

    for (k = 60; k >= 0; k -= 4) {
        unsigned d = (unsigned)(v >> k) & 15;

        if (d != 0 || started || k == 0) {
            text[at++] = digits[d];
            started = true;
        }
    }

    text[at] = '\0';
    append(text);
}

static void append_device(uint64_t base, unsigned irq)
{
    char n[4] = { 0 };

    append(" virtio_mmio.device=0x200@");
    append_hex(base);
    n[0] = (char)(irq >= 10 ? '1' : '0' + irq);
    n[1] = irq >= 10 ? (char)('0' + irq - 10) : '\0';
    append(":");
    append(n);
}

/*
 * **The machine, said on the command line**, after what the sandbox asked:
 * no APIC and no ACPI, since there are none; no PCI bus to look for; the
 * kernel where it was put, unmoved; the TSC as the clock, its frequency in
 * CPUID; one serial port; and each virtio device, where and on which line.
 */
bool vmm_machine_prepare(void)
{
    const char *why = "";

    x86_pit_reset(&pit, vmm_counter_hz);
    memset(line, 0, sizeof(line));
    append(vmm_cfg.cmdline);
    append(" noapic nolapic acpi=off pci=off nokaslr tsc=reliable 8250.nr_uarts=1");

    for (unsigned i = 0; i < VMM_SHARES; i++) {
        if (vmm_cfg.shares[i].root[0] != '\0') {
            append_device(X86_VIRTIO_BASE + 0x200ul * i, share_irq[i]);
        }
    }

    if (vmm_networked()) {
        append_device(X86_NET_BASE, X86_NET_IRQ);
    }

    if (!x86_boot(vmm_ram, vmm_cfg.ram_bytes, vmm_cfg.kernel_offset, vmm_cfg.kernel_bytes,
                  vmm_cfg.initrd_offset, vmm_cfg.initrd_bytes, line, &boot, &why)) {
        vmm_note(why);
        return false;
    }

    return true;
}

/* ------------------------------------------------------------- the ports */

static uint8_t cmos_read(void)
{
    /*
     * The real-time clock, as a fixed moment until the monitor reads
     * Kosmos's own: 1 January 2026, 00:00:00, BCD, 24-hour, its battery
     * good - so Linux reads a date that is a date and goes on.
     */
    switch (cmos_index & 0x7f) {
    case 0x07: return 0x01;             /* day of the month */
    case 0x08: return 0x01;             /* month */
    case 0x09: return 0x26;             /* year */
    case 0x0a: return 0x26;             /* no update in progress */
    case 0x0b: return 0x02;             /* 24-hour, BCD */
    case 0x0d: return 0x80;             /* its battery is good */
    case 0x32: return 0x20;             /* century */
    default:   return 0x00;
    }
}

static uint32_t port_in(uint16_t port, unsigned size)
{
    uint64_t now = kosmos_ticks();

    if (size == 1) {
        switch (port) {
        case 0x20: case 0x21: case 0xa0: case 0xa1: case 0x4d0: case 0x4d1:
            return x86_pic_read(&pic, port);
        case 0x40: case 0x41: case 0x42: case 0x61:
            return x86_pit_read(&pit, port, now);
        case 0x71:
            return cmos_read();
        case 0x64:
            return 0;                   /* a keyboard controller with nothing to say */
        default:
            break;
        }

        if (port >= X86_UART_PORT && port < X86_UART_PORT + 8) {
            return x86_uart_read(&uart, port - X86_UART_PORT, &console);
        }
    }

    return size == 4 ? 0xffffffffu : size == 2 ? 0xffffu : 0xffu;
}

/* False: the guest asked the machine to reset, which ends it. */
static bool port_out(uint16_t port, unsigned size, uint32_t value)
{
    uint64_t now = kosmos_ticks();

    if (size != 1) {
        return true;
    }

    switch (port) {
    case 0x20: case 0x21: case 0xa0: case 0xa1: case 0x4d0: case 0x4d1:
        x86_pic_write(&pic, port, (uint8_t)value);
        return true;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x61:
        x86_pit_write(&pit, port, (uint8_t)value, now);
        return true;
    case 0x70:
        cmos_index = (uint8_t)value;
        return true;
    case 0x64:
        return value != 0xfe;           /* the keyboard controller's reset */
    case 0xcf9:
        return (value & 4) == 0;        /* the PCI reset register */
    default:
        break;
    }

    if (port >= X86_UART_PORT && port < X86_UART_PORT + 8) {
        x86_uart_write(&uart, port - X86_UART_PORT, (uint8_t)value, &console);
    }

    return true;
}

static void io(struct vcpu_regs *r)
{
    uint64_t q = r->qualification;
    unsigned size = (unsigned)(q & 7) + 1;
    uint16_t port = (uint16_t)(q >> 16);
    uint64_t *rax = &r->gpr[VCPU_GPR_RAX];

    if (q & 0x10) {
        vmm_tell("string port I/O the monitor does not do, port and rip ", port, r->rip);
        vmm_finish();
    }

    if (q & 8) {
        uint32_t v = port_in(port, size);

        *rax = size == 4 ? v : size == 2 ? (*rax & ~0xffffull) | v : (*rax & ~0xffull) | v;
    } else if (!port_out(port, size, (uint32_t)*rax)) {
        vmm_note("the guest reset its machine");
        vmm_finish();
    }

    r->rip += r->length;
}

/* -------------------------------------------------------- CPUID and MSRs */

static void cpuid(struct vcpu_regs *r)
{
    uint32_t v[4], leaf = (uint32_t)r->gpr[VCPU_GPR_RAX], sub = (uint32_t)r->gpr[VCPU_GPR_RCX];

    __asm__ volatile("cpuid" : "=a"(v[0]), "=b"(v[1]), "=c"(v[2]), "=d"(v[3])
                     : "a"(leaf), "c"(sub));
    x86_cpuid(leaf, sub, v);
    r->gpr[VCPU_GPR_RAX] = v[0];
    r->gpr[VCPU_GPR_RBX] = v[1];
    r->gpr[VCPU_GPR_RCX] = v[2];
    r->gpr[VCPU_GPR_RDX] = v[3];
    r->rip += r->length;
}

#define MSR_TSC             0x10u
#define MSR_PAT             0x277u
#define MSR_MISC_ENABLE     0x1a0u
#define MSR_EFER            0xc0000080u

static void msr(struct vcpu_regs *r, bool write)
{
    uint32_t index = (uint32_t)r->gpr[VCPU_GPR_RCX];
    uint64_t value = (r->gpr[VCPU_GPR_RDX] & 0xffffffffu) << 32
                     | (r->gpr[VCPU_GPR_RAX] & 0xffffffffu);

    if (write) {
        switch (index) {
        case MSR_EFER: r->efer = (value & ~EFER_LMA) | (r->efer & EFER_LMA); break;
        case MSR_PAT:  pat = value; break;
        default:       break;      /* written to nothing */
        }
    } else {
        switch (index) {
        case MSR_EFER: value = r->efer; break;
        case MSR_PAT:  value = pat; break;
        case MSR_TSC:  value = kosmos_ticks(); break;
        case MSR_MISC_ENABLE: value = 1u | 1u << 11 | 1u << 12; break;  /* fast strings; no BTS, PEBS */
        default:       value = 0; break;
        }

        r->gpr[VCPU_GPR_RAX] = value & 0xffffffffu;
        r->gpr[VCPU_GPR_RDX] = value >> 32;
    }

    r->rip += r->length;
}

/* A move to CR0 or CR4 that changed what VMX keeps fixed: the guest's value kept. */
static void control_register(struct vcpu_regs *r)
{
    uint64_t q = r->qualification;
    unsigned cr = (unsigned)(q & 15), type = (unsigned)(q >> 4) & 3, reg = (unsigned)(q >> 8) & 15;

    if (type == 0 && cr == 0) {
        r->cr0 = r->gpr[reg];
    } else if (type == 0 && cr == 4) {
        r->cr4 = r->gpr[reg];
    } else if (type == 2) {
        r->cr0 &= ~8ul;                 /* CLTS */
    } else if (type == 3) {
        r->cr0 = (r->cr0 & ~0xeul) | ((q >> 16) & 0xe);   /* LMSW cannot clear PE */
    } else {
        vmm_tell("a control register the monitor does not keep, qualification and rip ", q, r->rip);
        vmm_finish();
    }

    r->rip += r->length;
}

/* ------------------------------------------------------------- MMIO */

/*
 * **An access to a virtio device**: the guest's instruction read through
 * its own page tables - up to the fifteen bytes an instruction may have,
 * stopping where they stop being mapped - decoded, and done.
 */
static void mmio(struct vcpu_regs *r)
{
    uint8_t code[15];
    unsigned avail = 0;
    struct x86_mov m;
    uint64_t gpa = r->gpa, v = 0;
    bool done;

    for (avail = 0; avail < sizeof(code); avail++) {
        uint64_t pa = r->rip + avail;

        if ((r->cr0 & 0x80000000ul) != 0
            && !x86_walk(vmm_ram, vmm_cfg.ram_bytes, r->cr3, r->rip + avail, &pa)) {
            break;
        }

        if (pa >= vmm_cfg.ram_bytes) {
            break;
        }

        code[avail] = vmm_ram[pa];
    }

    if (!x86_decode_mov(code, avail, &m)) {
        uint64_t first = 0;

        for (unsigned k = 0; k < 8 && k < avail; k++) {
            first |= (uint64_t)code[k] << (8 * k);
        }

        vmm_tell("an access the monitor cannot decode, at and its bytes ", r->rip, first);
        vmm_finish();
    }

    if (m.write) {
        v = x86_mov_value(&m, r->gpr);
    }

    if (gpa >= X86_NET_BASE && gpa < X86_NET_BASE + 0x200 && vmm_networked()) {
        done = vmmnet_mmio(gpa - X86_NET_BASE, m.size, m.write, &v);
    } else if (gpa >= X86_VIRTIO_BASE && gpa < X86_VIRTIO_BASE + 0x200ul * VMM_SHARES) {
        unsigned i = (unsigned)((gpa - X86_VIRTIO_BASE) / 0x200);

        done = vmmfs_mmio(i, (gpa - X86_VIRTIO_BASE) % 0x200, m.size, m.write, &v);
    } else {
        done = false;
    }

    if (!done) {
        vmm_tell("the guest touched what is not there, at ", gpa, r->rip);
        vmm_finish();
    }

    if (!m.write) {
        x86_mov_load(&m, r->gpr, v);
    }

    r->rip += m.length;
}

/* ------------------------------------------------------- interrupts */

/*
 * **The lines, as they are now, and the next one in**: the timer's edge if
 * it came round, the serial port's and each device's level; then, if the
 * 8259s have one to give, it goes in with the entry - when the guest can
 * take it - or the guest is asked to stop as soon as it can.
 */
static void deliver(struct vcpu_regs *r)
{
    if (x86_pit_due(&pit, kosmos_ticks())) {
        x86_pic_edge(&pic, 0);
    }

    x86_pic_level(&pic, X86_UART_IRQ, x86_uart_raised(&uart, &console));
    vmmnet_pump();
    x86_pic_level(&pic, X86_NET_IRQ, vmm_networked() && vmmnet_raised());

    for (unsigned i = 0; i < VMM_SHARES; i++) {
        x86_pic_level(&pic, share_irq[i], vmmfs_raised(i));
    }

    r->flags &= ~(uint64_t)VCPU_WANT_WINDOW;
    r->inject = 0;

    if (!x86_pic_pending(&pic)) {
        return;
    }

    if (r->interruptible) {
        r->inject = (uint64_t)(unsigned)x86_pic_take(&pic) | VCPU_INJECT;
    } else {
        r->flags |= VCPU_WANT_WINDOW;
    }
}

/*
 * **The guest is waiting** (`hlt` with interrupts on): back at once if
 * something is already there for it, otherwise asleep until its timer is
 * due - one scheduler tick at the most, 4 ms, which is how soon a key
 * typed is seen, since keys arrive on the other thread.
 */
static void wait_for_guest(struct vcpu_regs *r)
{
    uint64_t now = kosmos_ticks(), next, ticks = 1;

    vmm_flush_out();
    deliver(r);

    if (r->inject != 0 || (r->flags & VCPU_WANT_WINDOW) != 0 || !vmm_rx_empty()) {
        return;
    }

    next = x86_pit_next(&pit, now);

    if (next != ~0ull && next > now) {
        uint64_t wait = (next - now) * vmm_tick_hz / vmm_counter_hz;

        ticks = wait < ticks ? wait : ticks;
    }

    kosmos_sleep(ticks > 0 ? (unsigned long)ticks : 1);
}

/* ------------------------------------------------------------ the run */

void vmm_machine_run(unsigned long unused)
{
    struct vcpu_regs r;
    uint64_t before;
    long why;

    (void)unused;
    memset(&r, 0, sizeof(r));
    r.rip = boot.rip;
    r.gpr[6] = boot.rsi;                    /* rsi: the zero page */
    r.gpr[VCPU_GPR_RSP] = boot.rsp;
    r.rflags = 2;                           /* interrupts off, as Linux is entered */
    r.cr0 = 0x80000031ul;                   /* PG, NE, ET, PE */
    r.cr3 = boot.cr3;
    r.cr4 = 0x20;                           /* PAE */
    r.efer = 0x500;                         /* LME, LMA */
    r.gdt_base = boot.gdt_base;
    r.gdt_limit = boot.gdt_limit;
    r.flags = VCPU_SET_SYSTEM;

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
        r.flags &= ~(uint64_t)VCPU_SET_SYSTEM;

        if (why == VCPU_EXIT_HOST) {
            continue;
        }

        if (why != VCPU_EXIT_TRAP) {
            vmm_tell("the vCPU would not run: ", (unsigned long)why, 0);
            vmm_finish();
        }

        if (r.reason == 0xffffffffu || (r.reason & 0x80000000u) != 0) {
            vmm_tell("the guest could not be entered: reason and error ", r.reason, r.error);
            vmm_finish();
        }

        switch ((unsigned)(r.reason & 0xffff)) {
        case EXIT_WINDOW:
            break;                          /* it can take one now: next pass */
        case EXIT_CPUID:
            cpuid(&r);
            break;
        case EXIT_HLT:
            r.rip += r.length;

            if ((r.rflags & RFLAGS_IF) == 0) {
                vmm_note("the guest switched itself off");
                vmm_finish();
            }

            wait_for_guest(&r);
            break;
        case EXIT_IO:
            io(&r);
            break;
        case EXIT_RDMSR:
            msr(&r, false);
            break;
        case EXIT_WRMSR:
            msr(&r, true);
            break;
        case EXIT_CR:
            control_register(&r);
            break;
        case EXIT_EPT:
            mmio(&r);
            break;
        case EXIT_TRIPLE_FAULT:
            vmm_tell("the guest reset itself (a triple fault) at ", r.rip, 0);
            vmm_finish();
            break;
        case EXIT_XSETBV:
        case EXIT_EXCEPTION:
        default:
            vmm_tell("the guest stopped: exit reason and rip ", r.reason, r.rip);
            vmm_finish();
        }
    }
}

#endif /* __x86_64__ */
