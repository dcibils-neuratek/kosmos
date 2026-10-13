/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The PC a sandbox's guest sees, on the host - `user/servers/x86dev.c`,
 * `docs/sandboxes.md` X2. Only a PC with VT-x can run the monitor for real,
 * so this is where its devices are held to what Linux does with them:
 * each check below is a sequence Linux's own driver performs - `i8259.c`,
 * `i8253.c`, `8250_port.c`, the boot protocol - and what it needs back,
 * worked out from the datasheets and `Documentation/arch/x86/boot.rst`, not
 * from the code.
 *
 * Usage: test_x86dev
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x86dev.h"

static int checks, fails;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        fails++;
        printf("  %s\n", what);
    }
}

/* ------------------------------------------------------------ the 8259s */

static void test_pic(void)
{
    struct x86_pic p;

    x86_pic_reset(&p);

    /* `init_8259A`: ICW1-4 to both, then every line masked. */
    x86_pic_write(&p, 0x21, 0xff);
    x86_pic_write(&p, 0xa1, 0xff);
    x86_pic_write(&p, 0x20, 0x11);
    x86_pic_write(&p, 0x21, 0x30);
    x86_pic_write(&p, 0x21, 0x04);
    x86_pic_write(&p, 0x21, 0x01);
    x86_pic_write(&p, 0xa0, 0x11);
    x86_pic_write(&p, 0xa1, 0x38);
    x86_pic_write(&p, 0xa1, 0x02);
    x86_pic_write(&p, 0xa1, 0x01);
    x86_pic_write(&p, 0x21, 0xff);
    x86_pic_write(&p, 0xa1, 0xff);

    /* `probe_8259A`: the mask reads back, or there is no PIC. */
    check(x86_pic_read(&p, 0x21) == 0xff && x86_pic_read(&p, 0xa1) == 0xff,
          "the 8259s' masks did not read back as 0xff, so Linux would find no PIC");

    x86_pic_edge(&p, 0);
    check(!x86_pic_pending(&p), "a masked line was offered");

    /* The timer, the cascade and the serial port unmasked. */
    x86_pic_write(&p, 0x21, (uint8_t)~(1u | 4u | 16u));
    check(x86_pic_pending(&p), "the timer's edge was not offered once unmasked");
    check(x86_pic_take(&p) == 0x30, "the timer's vector is not ICW2's base, 0x30");
    x86_pic_edge(&p, 0);
    check(!x86_pic_pending(&p), "the timer was offered again while in service");

    /* OCW3 0x0b reads the in-service register: line 0. */
    x86_pic_write(&p, 0x20, 0x0b);
    check(x86_pic_read(&p, 0x20) == 0x01, "the in-service register did not say line 0");
    x86_pic_write(&p, 0x20, 0x0a);

    /* `mask_and_ack_8259A`: a specific EOI, 0x60 + line. */
    x86_pic_write(&p, 0x20, 0x60);
    check(x86_pic_pending(&p) && x86_pic_take(&p) == 0x30,
          "the timer's second edge was not offered after its EOI");
    x86_pic_write(&p, 0x20, 0x20);                      /* non-specific */

    /* A level held: offered, and offered again after its EOI while held. */
    x86_pic_level(&p, 4, true);
    check(x86_pic_take(&p) == 0x34, "the serial port's vector is not 0x34");
    check(!x86_pic_pending(&p), "a level line was offered twice in service");
    x86_pic_write(&p, 0x20, 0x64);
    check(x86_pic_take(&p) == 0x34, "a level line still held was not offered again");
    x86_pic_level(&p, 4, false);
    x86_pic_write(&p, 0x20, 0x64);
    check(!x86_pic_pending(&p), "a level line let go was still offered");

    /* Line 10 through the slave: vector 0x3a, both in service. */
    x86_pic_write(&p, 0xa1, (uint8_t)~(1u << 2));
    x86_pic_level(&p, 10, true);
    check(x86_pic_read(&p, 0x20) & 4, "the slave's line did not show on the master's line 2");
    check(x86_pic_take(&p) == 0x3a, "the slave's line 2 is not vector 0x3a");
    x86_pic_write(&p, 0xa0, 0x0b);
    check(x86_pic_read(&p, 0xa0) == 0x04, "the slave's in-service register did not say line 2");
    x86_pic_level(&p, 10, false);
    x86_pic_write(&p, 0xa0, 0x62);
    x86_pic_write(&p, 0x20, 0x62);
    check(!x86_pic_pending(&p), "after both EOIs something was still offered");

    /* A higher line goes ahead of a lower one in service. */
    x86_pic_level(&p, 4, true);
    check(x86_pic_take(&p) == 0x34, "the serial port was not offered");
    x86_pic_edge(&p, 0);
    check(x86_pic_take(&p) == 0x30, "the timer did not go ahead of the serial port in service");
    x86_pic_level(&p, 4, false);
}

/* ------------------------------------------------------------- the 8254 */

static void test_pit(void)
{
    struct x86_pit t;
    uint64_t hz = 1000000000ull, now = 5000;            /* a counter in nanoseconds */

    x86_pit_reset(&t, hz);
    check(!x86_pit_due(&t, now) && x86_pit_next(&t, now) == ~0ull,
          "an unprogrammed timer fired, or said it would");

    /* `init_pit_timer` periodic: mode 2, LATCH = 1193 for HZ 1000. */
    x86_pit_write(&t, 0x43, 0x34, now);
    x86_pit_write(&t, 0x40, 1193 & 0xff, now);
    x86_pit_write(&t, 0x40, 1193 >> 8, now);
    check(!x86_pit_due(&t, now + 999000), "the timer fired before a millisecond");
    check(x86_pit_next(&t, now) >= now + 999800 && x86_pit_next(&t, now) <= now + 1000000,
          "the timer's next tick is not a millisecond away");
    check(x86_pit_due(&t, now + 1000100), "the timer did not fire after a millisecond");
    check(!x86_pit_due(&t, now + 1500000), "the timer fired twice in one period");
    check(x86_pit_due(&t, now + 2000200), "the timer did not fire in its second period");

    /* The count, latched and read low then high, half way through. */
    x86_pit_write(&t, 0x43, 0x00, now + 2500000);
    {
        unsigned lo = x86_pit_read(&t, 0x40, now + 2900000);
        unsigned hi = x86_pit_read(&t, 0x40, now + 2900000);
        unsigned count = lo | hi << 8;

        check(count >= 590 && count <= 600, "the latched count half way through is not about 597");
    }

    /* One shot, mode 0: fires once. */
    x86_pit_write(&t, 0x43, 0x30, now);
    x86_pit_write(&t, 0x40, 0x10, now);
    x86_pit_write(&t, 0x40, 0x00, now);
    check(x86_pit_due(&t, now + 20000) && !x86_pit_due(&t, now + 90000000)
          && x86_pit_next(&t, now + 90000000) == ~0ull,
          "a one-shot timer did not fire exactly once");

    /* Channel 2 behind port 0x61, as `pit_calibrate_tsc` uses it. */
    x86_pit_write(&t, 0x61, 0x01, now);
    x86_pit_write(&t, 0x43, 0xb0, now);
    x86_pit_write(&t, 0x42, 0xff, now);
    x86_pit_write(&t, 0x42, 0x00, now);
    check((x86_pit_read(&t, 0x61, now + 1000) & 0x20) == 0,
          "channel 2's output was high before its count ran out");
    check((x86_pit_read(&t, 0x61, now + 300000) & 0x20) != 0,
          "channel 2's output was not high after its count ran out");
    check(((x86_pit_read(&t, 0x61, now) ^ x86_pit_read(&t, 0x61, now)) & 0x10) != 0,
          "port 0x61's refresh bit did not toggle");
}

/* ------------------------------------------------------------ the 16550 */

static char typed[8];
static int typed_at, typed_n;
static char said[64];
static int said_n;

static int ready(void) { return typed_at < typed_n; }
static bool ready_b(void) { return ready() != 0; }
static int take(void) { return ready() ? (unsigned char)typed[typed_at++] : -1; }
static void put(char c) { if (said_n < 63) said[said_n++] = c; }

static void test_uart(void)
{
    struct x86_uart u;
    struct x86_uart_io io = { ready_b, take, put };
    uint8_t iir1, iir;

    x86_uart_reset(&u);

    /* `autoconfig`: the scratch register, the FIFO, the loopback. */
    x86_uart_write(&u, 7, 0xa5, &io);
    check(x86_uart_read(&u, 7, &io) == 0xa5, "the scratch register did not keep 0xa5");
    x86_uart_write(&u, 2, 0x01, &io);
    check((x86_uart_read(&u, 2, &io) & 0xc0) == 0xc0, "with its FIFO on, the IIR did not say a 16550A");
    x86_uart_write(&u, 4, 0x1a, &io);
    check((x86_uart_read(&u, 6, &io) & 0xf0) == 0x90, "the loopback's status was not 0x90, so no UART");
    x86_uart_write(&u, 4, 0x00, &io);

    /* The divisor behind DLAB, and the IER not disturbed by it. */
    x86_uart_write(&u, 3, 0x80, &io);
    x86_uart_write(&u, 0, 0x01, &io);
    x86_uart_write(&u, 1, 0x00, &io);
    x86_uart_write(&u, 3, 0x03, &io);
    check(said_n == 0, "writing the divisor printed something");

    /* `serial8250_do_startup`'s THRE test: no bug to find. */
    x86_uart_write(&u, 1, 0x02, &io);
    iir1 = x86_uart_read(&u, 2, &io);
    x86_uart_write(&u, 1, 0x00, &io);
    x86_uart_write(&u, 1, 0x02, &io);
    iir = x86_uart_read(&u, 2, &io);
    x86_uart_write(&u, 1, 0x00, &io);
    check((iir1 & 0x0f) == 0x02 && (iir & 1) == 0,
          "enabling the THRE interrupt did not raise it, so Linux would call the port buggy");

    /* A byte out; its interrupt only with OUT2. */
    x86_uart_write(&u, 4, 0x0b, &io);                   /* DTR, RTS, OUT2 */
    x86_uart_write(&u, 1, 0x03, &io);
    x86_uart_write(&u, 0, 'h', &io);
    x86_uart_write(&u, 0, 'i', &io);
    check(said_n == 2 && said[0] == 'h' && said[1] == 'i', "the bytes written were not said");
    check(x86_uart_raised(&u, &io), "the transmitter-empty interrupt was not raised");
    check((x86_uart_read(&u, 2, &io) & 0x0f) == 0x02 && !x86_uart_raised(&u, &io),
          "reading the IIR did not clear the transmitter-empty interrupt");

    /* A key typed: data ready, the receive interrupt, the byte. */
    typed[0] = 'k';
    typed_n = 1;
    check((x86_uart_read(&u, 5, &io) & 0x61) == 0x61, "the LSR did not say data ready and empty");
    check((x86_uart_read(&u, 2, &io) & 0x0f) == 0x04 && x86_uart_raised(&u, &io),
          "a byte waiting did not raise the receive interrupt");
    check(x86_uart_read(&u, 0, &io) == 'k' && !x86_uart_raised(&u, &io),
          "the byte typed was not read, or its interrupt stayed");

    x86_uart_write(&u, 4, 0x03, &io);                   /* OUT2 off */
    x86_uart_write(&u, 0, '!', &io);
    check(!x86_uart_raised(&u, &io), "the interrupt went out without OUT2");
}

/* ------------------------------------------------------------- the RTC */

static void test_rtc(void)
{
    /* 13 October 2026, 15:04:05 UTC, a Tuesday: 1791903845. */
    uint64_t t = 1791903845ull;

    check(x86_rtc(t, 0x00) == 0x05 && x86_rtc(t, 0x02) == 0x04 && x86_rtc(t, 0x04) == 0x15,
          "15:04:05 was not read back in BCD");
    check(x86_rtc(t, 0x07) == 0x13 && x86_rtc(t, 0x08) == 0x10 && x86_rtc(t, 0x09) == 0x26
          && x86_rtc(t, 0x32) == 0x20, "13 October 2026 was not read back in BCD");
    check(x86_rtc(t, 0x06) == 0x03, "a Tuesday is not weekday 3");
    /* 29 February 2028, the day after 28 February. */
    check(x86_rtc(1835395200ull, 0x07) == 0x29 && x86_rtc(1835395200ull, 0x08) == 0x02,
          "29 February 2028 was not a leap day");
    check(x86_rtc(0, 0x09) == 0x70 && x86_rtc(0, 0x32) == 0x19 && x86_rtc(0, 0x07) == 0x01,
          "1 January 1970 was not the epoch");
    check((x86_rtc(t, 0x0a) & 0x80) == 0 && x86_rtc(t, 0x0b) == 0x02,
          "the RTC said an update in progress, or not BCD and 24-hour");
}

/* ----------------------------------------------------------------- CPUID */

static void test_cpuid(void)
{
    uint32_t r[4] = { 0x000906e3, 0x08100800, 0x7ffafbff, 0xbfebfbff };    /* an i7-6700's leaf 1 */
    uint32_t seven[4] = { 0, 0x029c6fbf, 0x40000000, 0xbc002e00 };

    x86_cpuid(1, 0, r);
    check(r[0] == 0x000906e3, "the processor's signature changed");
    check((r[3] & (1u << 9)) == 0, "the APIC was not hidden");
    check((r[2] & (1u << 21)) == 0 && (r[2] & (1u << 26)) == 0 && (r[2] & (1u << 28)) == 0,
          "x2APIC, XSAVE or AVX was not hidden");
    check((r[2] & (1u << 5)) == 0, "VMX was not hidden");
    check((r[2] & (1u << 31)) != 0, "the hypervisor bit was not set");
    check((r[1] >> 24) == 0 && ((r[1] >> 16) & 0xff) == 1, "not one processor, APIC id 0");
    check((r[3] & ((1u << 0) | (1u << 4) | (1u << 24) | (1u << 25) | (1u << 26)))
          == ((1u << 0) | (1u << 4) | (1u << 24) | (1u << 25) | (1u << 26)),
          "FPU, TSC, FXSR, SSE or SSE2 went with them");

    x86_cpuid(7, 0, seven);
    check((seven[1] & (1u << 5)) == 0 && (seven[1] & (1u << 10)) == 0 && seven[3] == 0,
          "AVX2, INVPCID or the speculation controls were not hidden");

    {
        uint32_t hv[4] = { 1, 2, 3, 4 };

        x86_cpuid(0x40000000u, 0, hv);
        check(hv[0] == 0 && hv[1] == 0, "a hypervisor's signature was said");
    }
}

/* ------------------------------------------------------- the boot */

#define RAM (256ul << 20)

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, unsigned v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
static unsigned get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned long long get64(const uint8_t *p) { return get32(p) | (unsigned long long)get32(p + 4) << 32; }

static void test_boot(void)
{
    uint8_t *ram = calloc(1, RAM);
    uint64_t setup_bytes = 0, init_size = 0, at, pa = 0;
    struct x86_boot b;
    const char *why = "";
    uint8_t *image, *bp;

    if (ram == NULL) {
        check(0, "no memory for the test");
        return;
    }

    /* A bzImage's header, as boot.rst lays it out: 4 setup sectors. */
    at = X86_BOOT_KERNEL - 5 * 512;
    image = ram + at;
    image[0x1f1] = 4;
    image[0x201] = 0x6a;                                /* the header runs to 0x26c */
    memcpy(image + 0x202, "HdrS", 4);
    put16(image + 0x206, 0x020f);
    image[0x211] = 0x01;                                /* LOADED_HIGH */
    put16(image + 0x236, 0x0001);                       /* a 64-bit entry */
    put32(image + 0x260, 32u << 20);                    /* init_size */
    put32(image + 0x264, 0x12345678);                   /* past the header's copy? no: inside */

    check(x86_bzimage(image, 1u << 20, &setup_bytes, &init_size)
          && setup_bytes == 2560 && init_size == (32u << 20),
          "the bzImage header was not read: 2560 bytes of setup, 32 MB to unpack");

    check(!x86_boot(ram, RAM, at + 512, 1u << 20, 0, 0, "x", &b, &why),
          "a kernel not at 16 MB was booted");
    check(!x86_boot(ram, RAM, at, 1u << 20, X86_BOOT_KERNEL + (16u << 20), 4096, "x", &b, &why),
          "an initramfs inside the kernel's room was accepted");

    check(x86_boot(ram, RAM, at, 1u << 20, 64u << 20, 1u << 20, "console=ttyS0 quiet", &b, &why),
          why);

    bp = ram + X86_BOOT_PARAMS;
    check(b.rip == X86_BOOT_KERNEL + 0x200 && b.rsi == X86_BOOT_PARAMS,
          "not entered at the 64-bit entry with the zero page in rsi");
    check(memcmp(bp + 0x202, "HdrS", 4) == 0 && get32(bp + 0x264) == 0x12345678,
          "the setup header was not copied into the zero page whole");
    check(bp[0x210] == 0xff, "the type of loader is not 0xff");
    check(get32(bp + 0x218) == (64u << 20) && get32(bp + 0x21c) == (1u << 20),
          "the initramfs is not where the zero page says");
    check(get32(bp + 0x228) == X86_BOOT_CMDLINE
          && strcmp((char *)ram + X86_BOOT_CMDLINE, "console=ttyS0 quiet") == 0,
          "the command line is not where the zero page says");
    check(bp[0x1e8] == 3 && get64(bp + 0x2d0) == 0 && get64(bp + 0x2d8) == 0x9f000
          && get32(bp + 0x2e0) == 1 && get32(bp + 0x2f4) == 2
          && get64(bp + 0x2f8) == 0x100000 && get64(bp + 0x300) == RAM - 0x100000,
          "the e820 map is not 636 KB, a hole, and the rest");
    check(get64(ram + X86_BOOT_GDT + 0x10) == 0x00af9a000000ffffull
          && get64(ram + X86_BOOT_GDT + 0x18) == 0x00cf92000000ffffull && b.gdt_limit == 31,
          "the GDT has no __BOOT_CS and __BOOT_DS at 0x10 and 0x18");

    /* Its page tables, walked: identity over the first 4 GB. */
    check(x86_walk(ram, RAM, b.cr3, 0x1000200, &pa) && pa == 0x1000200,
          "the kernel's entry did not translate to itself");
    check(x86_walk(ram, RAM, b.cr3, 0xd0000010ul, &pa) && pa == 0xd0000010ul,
          "the devices' window did not translate to itself");
    check(!x86_walk(ram, RAM, b.cr3, 0x100000000ull, &pa), "past 4 GB translated");

    /* A 4 KB page of the guest's own: 0xffff800000001000 to 0x5000000. */
    {
        uint64_t root = 0x6000000, l3 = root + 0x1000, l2 = root + 0x2000, l1 = root + 0x3000;
        uint64_t va = 0xffff800000001234ull;

        memset(ram + root, 0, 0x4000);
        memcpy(ram + root + 8 * ((va >> 39) & 511), &(uint64_t){ l3 | 3 }, 8);
        memcpy(ram + l3 + 8 * ((va >> 30) & 511), &(uint64_t){ l2 | 3 }, 8);
        memcpy(ram + l2 + 8 * ((va >> 21) & 511), &(uint64_t){ l1 | 3 }, 8);
        memcpy(ram + l1 + 8 * ((va >> 12) & 511), &(uint64_t){ 0x5000000 | 3 }, 8);
        check(x86_walk(ram, RAM, root, va, &pa) && pa == 0x5000234,
              "a 4 KB page in the kernel's half did not translate");
    }

    free(ram);
}

/* ----------------------------------------------------------- a `mov` */

struct mov_case {
    const char *what;
    uint8_t code[15];
    unsigned n, length, size, dest;
    int write, reg, high8;
    uint64_t imm;
};

static void test_decode(void)
{
    static const struct mov_case cases[] = {
        { "mov eax, [rbx]",          { 0x8b, 0x03 }, 2, 2, 4, 4, 0, 0, 0, 0 },
        { "mov [rdi], eax",          { 0x89, 0x07 }, 2, 2, 4, 0, 1, 0, 0, 0 },
        { "mov [rdi+0x10], al",      { 0x88, 0x47, 0x10 }, 3, 3, 1, 0, 1, 0, 0, 0 },
        { "mov [rdi+0x10], ax",      { 0x66, 0x89, 0x47, 0x10 }, 4, 4, 2, 0, 1, 0, 0, 0 },
        { "mov rax, [rbx+8]",        { 0x48, 0x8b, 0x43, 0x08 }, 4, 4, 8, 8, 0, 0, 0, 0 },
        { "mov [rdi+4], r9d",        { 0x44, 0x89, 0x4f, 0x04 }, 4, 4, 4, 0, 1, 9, 0, 0 },
        { "mov ah, [rdi+1]",         { 0x8a, 0x67, 0x01 }, 3, 3, 1, 1, 0, 0, 1, 0 },
        { "mov sil, [rdi]",          { 0x40, 0x8a, 0x37 }, 3, 3, 1, 1, 0, 6, 0, 0 },
        { "mov dword [rdi+0xc], 0x12345678",
                                     { 0xc7, 0x47, 0x0c, 0x78, 0x56, 0x34, 0x12 }, 7, 7, 4, 0, 1, -1, 0, 0x12345678 },
        { "mov byte [rdi], 0xff",    { 0xc6, 0x07, 0xff }, 3, 3, 1, 0, 1, -1, 0, 0xff },
        { "mov qword [rdi], -1",     { 0x48, 0xc7, 0x07, 0xff, 0xff, 0xff, 0xff }, 7, 7, 8, 0, 1, -1, 0,
                                       0xffffffffffffffffull },
        { "movzx eax, byte [rdi+4]", { 0x0f, 0xb6, 0x47, 0x04 }, 4, 4, 1, 4, 0, 0, 0, 0 },
        { "movzx eax, word [rdi]",   { 0x0f, 0xb7, 0x07 }, 3, 3, 2, 4, 0, 0, 0, 0 },
        { "mov eax, [0xd0000000]",   { 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0xd0 }, 7, 7, 4, 4, 0, 0, 0, 0 },
        { "mov eax, [rip+x]",        { 0x8b, 0x05, 1, 2, 3, 4 }, 6, 6, 4, 4, 0, 0, 0, 0 },
        { "mov eax, [rsp+0x100]",    { 0x8b, 0x84, 0x24, 0x00, 0x01, 0x00, 0x00 }, 7, 7, 4, 4, 0, 0, 0, 0 },
        { "mov r15d, [r12+rax*4]",   { 0x47, 0x8b, 0x3c, 0x84 }, 4, 4, 4, 4, 0, 15, 0, 0 },
    };
    struct x86_mov m;
    uint64_t gpr[16];
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct mov_case *c = &cases[i];
        char what[96];
        bool ok = x86_decode_mov(c->code, c->n, &m);

        snprintf(what, sizeof(what), "%s was not decoded as it is", c->what);
        check(ok && m.length == c->length && m.size == c->size && (int)m.write == c->write
              && m.reg == c->reg && (int)m.high8 == c->high8
              && (c->write || m.dest_size == c->dest) && (c->reg >= 0 || m.imm == c->imm), what);
    }

    check(!x86_decode_mov((const uint8_t[]){ 0x01, 0x07 }, 2, &m), "an add was taken for a mov");
    check(!x86_decode_mov((const uint8_t[]){ 0x8b, 0xc0 }, 2, &m), "a register-to-register mov was taken");
    check(!x86_decode_mov((const uint8_t[]){ 0xc7, 0x4f, 0x00 }, 3, &m), "C7 /1 was taken for a mov");
    check(!x86_decode_mov((const uint8_t[]){ 0x8b, 0x84, 0x24, 0x00 }, 4, &m),
          "an instruction cut short was decoded");

    /* What a load leaves in its register, and what a store takes. */
    memset(gpr, 0, sizeof(gpr));
    gpr[0] = 0xffffffff11223344ull;
    x86_decode_mov((const uint8_t[]){ 0x8a, 0x67, 0x01 }, 3, &m);            /* mov ah */
    x86_mov_load(&m, gpr, 0xab);
    check(gpr[0] == 0xffffffff1122ab44ull, "a load into AH changed more than AH");
    x86_decode_mov((const uint8_t[]){ 0x8b, 0x03 }, 2, &m);                  /* mov eax */
    x86_mov_load(&m, gpr, 0x5566778899aabbccull);
    check(gpr[0] == 0x99aabbccull, "a 32-bit load did not clear the register's top half");
    x86_decode_mov((const uint8_t[]){ 0x66, 0x8b, 0x03 }, 3, &m);            /* mov ax */
    gpr[0] = 0x1111111111111111ull;
    x86_mov_load(&m, gpr, 0xbeef);
    check(gpr[0] == 0x111111111111beefull, "a 16-bit load changed more than AX");
    x86_decode_mov((const uint8_t[]){ 0x0f, 0xb6, 0x07 }, 3, &m);            /* movzx eax, byte */
    x86_mov_load(&m, gpr, 0x1ff);
    check(gpr[0] == 0xff, "movzx did not zero-extend one byte");
    gpr[9] = 0x123456789abcdef0ull;
    x86_decode_mov((const uint8_t[]){ 0x44, 0x89, 0x4f, 0x04 }, 4, &m);      /* mov [rdi+4], r9d */
    check(x86_mov_value(&m, gpr) == 0x9abcdef0ull, "a 32-bit store did not take r9's low half");
    gpr[3] = 0x1200;
    x86_decode_mov((const uint8_t[]){ 0x88, 0x3f }, 2, &m);                  /* mov [rdi], bh */
    check(m.high8 && x86_mov_value(&m, gpr) == 0x12, "a store of BH did not take BH");
}

int main(void)
{
    test_pic();
    test_pit();
    test_uart();
    test_rtc();
    test_cpuid();
    test_boot();
    test_decode();

    if (fails != 0) {
        printf("FAIL: %d of %d checks on the sandbox's PC\n", fails, checks);
        return 1;
    }

    printf("PASS: %d checks on the sandbox's PC (the 8259s as Linux programs and probes them, "
           "levels and the cascade; the 8254's periodic and one-shot ticks and channel 2 behind "
           "port 0x61; the 16550 found, its THRE test passed, a byte out and a key in; the RTC's date; CPUID "
           "without APIC, XSAVE or VMX; Linux's 64-bit boot - zero page, e820, GDT, page tables; "
           "the guest's tables walked; the MMIO movs decoded)\n", checks);
    return 0;
}
