/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_SERVERS_X86DEV_H
#define USER_SERVERS_X86DEV_H

/*
 * **The PC a sandbox's guest sees, as data and arithmetic** -
 * `docs/sandboxes.md` X2, for `vmm_x86.c`.
 *
 * Everything here is pure: no system calls, no globals, the time handed in
 * as a counter and its rate - so `tools/test_x86dev.c` holds all of it to
 * what a guest expects on the Mac, since the only machine that can run it
 * for real is a PC with VT-x and QEMU's TCG emulates none.
 *
 * - **Two 8259A interrupt controllers**, master and slave, as Linux's
 *   `i8259.c` programs them; Linux runs on them with `noapic nolapic`.
 * - **An 8254 timer**: channel 0 on IRQ 0, the guest's clock tick; channel
 *   2 behind port 0x61, which Linux reads to calibrate.
 * - **A 16550A serial port** at 0x3F8 on IRQ 4: the guest's console.
 * - **CPUID, filtered**: the host's answer with what the machine does not
 *   have taken out - no APIC, no XSAVE or AVX, no VMX, one processor.
 * - **Linux's 64-bit boot** (`Documentation/arch/x86/boot.rst`): the zero
 *   page with its setup header, the e820 map and the command line; a GDT
 *   with `__BOOT_CS` and `__BOOT_DS`; identity page tables.
 * - **The guest's page tables walked**, to read an instruction at its RIP.
 * - **The `mov` an MMIO access stopped on, decoded** - which register or
 *   immediate, how wide, which way, and how long the instruction is - since
 *   an EPT violation says where and not what.
 */

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------ the 8259s */

struct x86_pic_chip {
    uint8_t irr, isr, imr, level;       /* level: lines a level source holds high */
    uint8_t base;                       /* the vector of its line 0, from ICW2 */
    uint8_t step;                       /* 0 ready; 1, 2, 3 waiting for ICW2-4 */
    bool    want_icw4, single, auto_eoi, read_isr;
    uint8_t elcr;                       /* port 0x4D0/1, kept and read back */
};

struct x86_pic {
    struct x86_pic_chip chip[2];        /* master, slave on its line 2 */
};

void    x86_pic_reset(struct x86_pic *p);
void    x86_pic_write(struct x86_pic *p, uint16_t port, uint8_t value);
uint8_t x86_pic_read(struct x86_pic *p, uint16_t port);
void    x86_pic_edge(struct x86_pic *p, unsigned irq);              /* 0-15 */
void    x86_pic_level(struct x86_pic *p, unsigned irq, bool high);
bool    x86_pic_pending(const struct x86_pic *p);                   /* one to deliver */
int     x86_pic_take(struct x86_pic *p);                            /* its vector, in service */

/* ------------------------------------------------------------- the 8254 */

#define X86_PIT_HZ      1193182u

struct x86_pit_channel {
    uint8_t  mode, access;              /* 0 to 5; 1 low, 2 high, 3 low then high */
    uint32_t reload;                    /* 1 to 65536 */
    uint64_t start;                     /* the counter when it was last loaded */
    uint64_t fired;                     /* periods already turned into edges */
    bool     loaded, write_high, read_high, latched;
    uint16_t latch, partial;
};

struct x86_pit {
    struct x86_pit_channel ch[3];
    uint64_t hz;                        /* the counter's rate */
    bool     gate2, speaker;            /* port 0x61, bits 0 and 1 */
    bool     refresh;                   /* bit 4, which toggles as it is read */
};

void     x86_pit_reset(struct x86_pit *t, uint64_t counter_hz);
void     x86_pit_write(struct x86_pit *t, uint16_t port, uint8_t value, uint64_t now);
uint8_t  x86_pit_read(struct x86_pit *t, uint16_t port, uint64_t now);
bool     x86_pit_due(struct x86_pit *t, uint64_t now);     /* channel 0 fired since asked */
uint64_t x86_pit_next(const struct x86_pit *t, uint64_t now);  /* when it next fires; ~0 never */

/* ----------------------------------------------------------- the 16550 */

struct x86_uart {
    uint8_t ier, lcr, mcr, scr, dll, dlm, fcr;
    bool    thre;                       /* the transmitter-empty interrupt, owed */
};

/* What the port reads and writes through: the monitor's console. */
struct x86_uart_io {
    bool (*ready)(void);                /* something typed */
    int  (*take)(void);                 /* the next byte typed, or -1 */
    void (*put)(char c);                /* a byte the guest wrote */
};

void    x86_uart_reset(struct x86_uart *u);
void    x86_uart_write(struct x86_uart *u, unsigned off, uint8_t value, const struct x86_uart_io *io);
uint8_t x86_uart_read(struct x86_uart *u, unsigned off, const struct x86_uart_io *io);
bool    x86_uart_raised(const struct x86_uart *u, const struct x86_uart_io *io);

/* ----------------------------------------------------------------- CPUID */

/* The host's answer to `leaf`, `sub` in r[0..3] (eax ebx ecx edx), filtered in place. */
void x86_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]);

/* ----------------------------------------------------------- the boot */

/*
 * Where the boot's own structures go in the guest's RAM, below the 640 KB a
 * PC has always kept free of anything else, and where the bzImage's
 * protected-mode kernel is loaded - its preferred address, 16 MB.
 */
#define X86_BOOT_GDT        0x1000ul
#define X86_BOOT_PARAMS     0x2000ul
#define X86_BOOT_PML4       0x3000ul        /* then a PDPT and four directories */
#define X86_BOOT_CMDLINE    0x20000ul
#define X86_BOOT_CMDLINE_MAX 2048u
#define X86_BOOT_STACK      0x8f000ul
#define X86_BOOT_KERNEL     0x1000000ul

struct x86_boot {
    uint64_t rip, rsi, rsp, cr3, gdt_base, gdt_limit;
};

/*
 * The bzImage at `image` - its first byte at `image_at` in the guest's RAM,
 * so its protected-mode kernel is at `X86_BOOT_KERNEL` - started with its
 * initramfs and command line: the zero page, the e820 map, the GDT and the
 * page tables written into `ram`. False, and `why`, when the image is not
 * one this can boot or does not fit.
 */
bool x86_boot(uint8_t *ram, uint64_t ram_bytes, uint64_t image_at, uint64_t image_bytes,
              uint64_t initrd_at, uint64_t initrd_bytes, const char *cmdline,
              struct x86_boot *out, const char **why);

/* What a bzImage's header says: where its protected-mode kernel starts in the
 * file, and how much room it needs from where it is loaded. */
bool x86_bzimage(const uint8_t *image, uint64_t bytes, uint64_t *setup_bytes, uint64_t *init_size);

/* ------------------------------------------------- the guest's addresses */

/* A guest virtual address through its page tables at `cr3`: false when unmapped. */
bool x86_walk(const uint8_t *ram, uint64_t ram_bytes, uint64_t cr3, uint64_t va, uint64_t *pa);

/* ----------------------------------------------------- an MMIO `mov` */

struct x86_mov {
    unsigned length;                    /* the whole instruction's bytes */
    unsigned size;                      /* the access: 1, 2, 4 or 8 */
    bool     write;                     /* to memory */
    int      reg;                       /* the register, 0-15, or -1 for `imm` */
    bool     high8;                     /* AH, CH, DH or BH */
    unsigned dest_size;                 /* a load's destination: 1, 2, 4 or 8 */
    uint64_t imm;
};

bool     x86_decode_mov(const uint8_t *code, unsigned avail, struct x86_mov *m);
uint64_t x86_mov_value(const struct x86_mov *m, const uint64_t gpr[16]);      /* what a store writes */
void     x86_mov_load(const struct x86_mov *m, uint64_t gpr[16], uint64_t value); /* a load's result */

#endif /* USER_SERVERS_X86DEV_H */
