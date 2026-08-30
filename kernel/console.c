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
