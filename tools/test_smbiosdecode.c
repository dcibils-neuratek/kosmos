/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * SMBIOS tables, asked the awkward questions on the host.
 *
 * `neofetch` on the ThinkPad said `Host  QEMU q35 x86-64`, because the
 * platform was a string the Makefile compiled in. The PC board reads
 * SMBIOS's System Information now, and QEMU can be told to put any strings
 * it likes in that table - so the ThinkPad's name is checked under emulation
 * by `run_x86.py`. What QEMU cannot be made to produce is a *bad* table, and
 * the decoder's job is mostly to refuse those without walking off the end of
 * memory at boot stage three. So they are built here, a byte at a time.
 *
 * Same split as `tools/test_apicdecode.c`, for the same reason.
 *
 * The Lenovo strings are the shape Lenovo's firmware uses - the machine type
 * as the product name and the model as the version - and stand in for the
 * T14's own, which the machine itself confirms.
 */

#include <stdio.h>
#include <string.h>

#include "../hal/pc/smbios_decode.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

/*------------------------------------------------------------------------
 * Building tables.
 *----------------------------------------------------------------------*/

struct table {
    uint8_t b[4096];
    size_t  n;
};

static void put(struct table *t, const void *bytes, size_t n)
{
    memcpy(t->b + t->n, bytes, n);
    t->n += n;
}

static void put8(struct table *t, uint8_t v)
{
    t->b[t->n++] = v;
}

/* A structure: its header, `fields` as the formatted area after the header,
 * then its strings and the NUL that ends the set. No strings is two NULs. */
static void structure(struct table *t, uint8_t type, uint8_t formatted_length,
                      const uint8_t *fields, const char *const *strings,
                      int count)
{
    int i;

    put8(t, type);
    put8(t, formatted_length);
    put8(t, type);                      /* the handle, low byte */
    put8(t, 0);

    if (formatted_length > 4) {
        put(t, fields, (size_t)formatted_length - 4);
    }

    if (count == 0) {
        put8(t, 0);
    }

    for (i = 0; i < count; i++) {
        put(t, strings[i], strlen(strings[i]) + 1);
    }

    put8(t, 0);
}

/* System Information as 2.4 lays it out: 1Bh bytes, string numbers at 04h,
 * 05h, 06h, 07h, a UUID, a wake-up type, and two more string numbers. */
static void system_info(struct table *t, uint8_t maker, uint8_t product,
                        uint8_t version, const char *const *strings, int count)
{
    uint8_t fields[0x1B - 4];

    memset(fields, 0, sizeof(fields));
    fields[0] = maker;
    fields[1] = product;
    fields[2] = version;
    fields[3] = count >= 4 ? 4 : 0;     /* serial */

    structure(t, 1, 0x1B, fields, strings, count);
}

static void end_of_table(struct table *t)
{
    structure(t, 127, 4, NULL, NULL, 0);
}

/* A BIOS Information structure first, the way every real table begins, so
 * the walk has to step over something before it finds type 1. */
static void bios_info(struct table *t)
{
    static const char *const strings[] = {
        "LENOVO", "N34ET53W (1.53 )", "07/05/2022",
    };
    uint8_t fields[0x18 - 4];

    memset(fields, 0, sizeof(fields));
    fields[0] = 1;
    fields[1] = 2;
    fields[4] = 3;

    structure(t, 0, 0x18, fields, strings, 3);
}

static void set_le(uint8_t *p, uint64_t v, int bytes)
{
    int i;

    for (i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint8_t balance(const uint8_t *p, size_t n)
{
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        sum = (uint8_t)(sum + p[i]);
    }

    return (uint8_t)(0u - sum);
}

static void entry_30(uint8_t *e, uint64_t address, uint32_t most)
{
    memset(e, 0, 0x18);
    memcpy(e, "_SM3_", 5);
    e[0x06] = 0x18;
    e[0x07] = 3;
    e[0x08] = 2;
    e[0x0A] = 1;
    set_le(e + 0x0C, most, 4);
    set_le(e + 0x10, address, 8);
    e[0x05] = balance(e, 0x18);
}

static void entry_21(uint8_t *e, uint8_t length, uint32_t address,
                     uint16_t table_length)
{
    memset(e, 0, 0x1F);
    memcpy(e, "_SM_", 4);
    e[0x05] = length;
    e[0x06] = 2;
    e[0x07] = 8;
    memcpy(e + 0x10, "_DMI_", 5);
    set_le(e + 0x16, table_length, 2);
    set_le(e + 0x18, address, 4);
    e[0x15] = balance(e + 0x10, 0x0F);  /* the inner one first: the */
    e[0x04] = balance(e, length);       /* outer one covers it */
}

int main(void)
{
    struct smbios_table where;
    struct smbios_system sys;
    uint8_t e[64];

    /* 1. The 3.0 entry point, which is what OVMF and QEMU's q35 give. */
    entry_30(e, 0x7fb3b000ull, 0x1d6);
    memset(&where, 0, sizeof(where));
    check(smbios_entry(e, sizeof(e), &where)
          && where.address == 0x7fb3b000ull && where.length == 0x1d6
          && where.major == 3 && where.minor == 2,
          "a well-formed 3.0 entry point was not read");

    e[0x12] ^= 0x01;
    check(!smbios_entry(e, sizeof(e), &where),
          "a 3.0 entry point with a byte changed was believed");

    entry_30(e, 0x100000000ull, 0x1000);
    check(smbios_entry(e, sizeof(e), &where) && where.address == 0x100000000ull,
          "a 3.0 address above 4 GB was not read whole");
    check(!smbios_entry(e, 0x17, &where),
          "a 3.0 entry point was read from fewer bytes than it has");

    entry_30(e, 0x1000, 0x1000);
    e[0x06] = 0x10;
    e[0x05] = 0;
    e[0x05] = balance(e, 0x10);
    check(!smbios_entry(e, sizeof(e), &where),
          "a 3.0 entry point claiming 16 bytes was taken");

    /* 2. The 2.1 entry point, at both lengths firmware has written. */
    entry_21(e, 0x1F, 0x000f5a60u, 0x1c3);
    check(smbios_entry(e, sizeof(e), &where)
          && where.address == 0x000f5a60u && where.length == 0x1c3
          && where.major == 2 && where.minor == 8,
          "a well-formed 2.1 entry point was not read");

    entry_21(e, 0x1E, 0x000f5a60u, 0x1c3);
    check(smbios_entry(e, sizeof(e), &where),
          "a 2.1 entry point of 1Eh bytes, as 2.1 itself printed, was refused");

    entry_21(e, 0x1F, 0x000f5a60u, 0x1c3);
    e[0x16] ^= 0x01;                    /* the DMI half changes... */
    e[0x04] = 0;
    e[0x04] = balance(e, 0x1F);         /* ...and only the outer sum is fixed */
    check(!smbios_entry(e, sizeof(e), &where),
          "a 2.1 entry point whose intermediate checksum fails was believed");

    entry_21(e, 0x1F, 0x000f5a60u, 0x1c3);
    check(!smbios_entry(e, 0x1E, &where),
          "a 2.1 entry point was read from fewer bytes than it has");

    memset(e, 0, sizeof(e));
    check(!smbios_entry(e, sizeof(e), &where), "zeroes were an entry point");

    /* 3. The ThinkPad's shape. */
    {
        static const char *const strings[] = {
            "LENOVO", "20W000T9US", "ThinkPad T14 Gen 2i", "PF2ABCDE",
        };
        struct table t = { .n = 0 };

        bios_info(&t);
        system_info(&t, 1, 2, 3, strings, 4);
        end_of_table(&t);

        memset(&sys, 0x55, sizeof(sys));
        check(smbios_system(t.b, t.n, &sys)
              && strcmp(sys.manufacturer, "LENOVO") == 0
              && strcmp(sys.product, "20W000T9US") == 0
              && strcmp(sys.version, "ThinkPad T14 Gen 2i") == 0,
              "Lenovo's manufacturer, product and version were not read");
    }

    /* 4. QEMU's, which is what `run_x86.py` sees by default. */
    {
        static const char *const strings[] = {
            "QEMU", "Standard PC (Q35 + ICH9, 2009)", "pc-q35-11.1",
        };
        struct table t = { .n = 0 };

        bios_info(&t);
        system_info(&t, 1, 2, 3, strings, 3);
        end_of_table(&t);

        check(smbios_system(t.b, t.n, &sys)
              && strcmp(sys.manufacturer, "QEMU") == 0
              && strcmp(sys.product, "Standard PC (Q35 + ICH9, 2009)") == 0
              && strcmp(sys.version, "pc-q35-11.1") == 0,
              "QEMU's System Information was not read");
    }

    /* 5. Fields that point at nothing are empty, not errors. */
    {
        static const char *const strings[] = { "ACME" };
        struct table t = { .n = 0 };

        system_info(&t, 1, 0, 9, strings, 1);
        end_of_table(&t);

        check(smbios_system(t.b, t.n, &sys)
              && strcmp(sys.manufacturer, "ACME") == 0
              && sys.product[0] == '\0' && sys.version[0] == '\0',
              "string 0 and a string past the last were not read as empty");
    }

    /* 6. A structure with no strings at all, stepped over. */
    {
        static const char *const strings[] = { "Found", "It" };
        struct table t = { .n = 0 };
        uint8_t fields[4] = { 0 };

        structure(&t, 32, 8, fields, NULL, 0);     /* System Boot, no strings */
        system_info(&t, 1, 2, 0, strings, 2);

        check(smbios_system(t.b, t.n, &sys)
              && strcmp(sys.product, "It") == 0,
              "an empty string set was not stepped over, or a table with no "
              "end-of-table structure was not read to its length");
    }

    /* 7. The end of the table is the end, whatever follows it. */
    {
        static const char *const strings[] = { "Past", "The", "End" };
        struct table t = { .n = 0 };

        bios_info(&t);
        end_of_table(&t);
        system_info(&t, 1, 2, 3, strings, 3);

        check(!smbios_system(t.b, t.n, &sys),
              "a System Information after end-of-table was believed");
    }

    /*
     * 8. A table cut short in the middle of a string set. The bytes past the
     *    cut hold a valid System Information, so a walk that read beyond
     *    `length` would find it and say so.
     */
    {
        static const char *const strings[] = { "Beyond", "The", "Length" };
        struct table t = { .n = 0 };
        size_t cut;

        bios_info(&t);
        cut = t.n - 6;                  /* inside "07/05/2022" */
        system_info(&t, 1, 2, 3, strings, 3);
        end_of_table(&t);

        check(!smbios_system(t.b, cut, &sys),
              "a string set with no end in reach was walked past the table");
    }

    /* 9. Lengths a header cannot have. */
    {
        struct table t = { .n = 0 };

        put8(&t, 0);
        put8(&t, 3);
        put8(&t, 0);
        put8(&t, 0);
        put8(&t, 0);
        put8(&t, 0);
        check(!smbios_system(t.b, t.n, &sys),
              "a structure of three bytes was walked");

        t.n = 0;
        put8(&t, 0);
        put8(&t, 0x40);                 /* longer than everything after it */
        put8(&t, 0);
        put8(&t, 0);
        put8(&t, 0);
        put8(&t, 0);
        check(!smbios_system(t.b, t.n, &sys),
              "a structure longer than the table was walked");
    }

    /* 10. What comes out is safe to print, and fits. */
    {
        char long_name[101];
        const char *strings[2];
        struct table t = { .n = 0 };

        memset(long_name, 'x', 100);
        long_name[100] = '\0';
        strings[0] = "ACME\nCORP\x7f";
        strings[1] = long_name;

        system_info(&t, 1, 2, 0, strings, 2);
        end_of_table(&t);

        check(smbios_system(t.b, t.n, &sys)
              && strcmp(sys.manufacturer, "ACME?CORP?") == 0,
              "a control character reached the strings");
        check(strlen(sys.product) == SMBIOS_TEXT - 1
              && sys.product[0] == 'x',
              "a hundred-byte product was not cut to fit");
    }

    if (fails > 0) {
        printf("FAIL: %d of %d checks on SMBIOS decoding\n", fails,
               checks + fails);
        return 1;
    }

    printf("PASS: %d checks on SMBIOS decoding, on this machine.\n", checks);
    return 0;
}
