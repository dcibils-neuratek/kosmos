#include "console.h"
#include "hal.h"

void kputs(const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            hal_putchar('\r');
        }
        hal_putchar(*s);
    }
}

void kputu(unsigned long v)
{
    /* 20 digits is the widest a 64-bit unsigned gets, plus room to spare.
     * The buffer is filled backwards and then walked out forwards. */
    char buf[24];
    unsigned i = 0;

    if (v == 0) {
        hal_putchar('0');
        return;
    }

    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (i > 0) {
        hal_putchar(buf[--i]);
    }
}

void kputx(unsigned long v, unsigned digits)
{
    static const char digit[] = "0123456789abcdef";

    if (digits > 16) {
        digits = 16;
    }

    /* Top nibble first, so no buffer is needed. */
    while (digits > 0) {
        digits--;
        hal_putchar(digit[(v >> (digits * 4)) & 0xf]);
    }
}
