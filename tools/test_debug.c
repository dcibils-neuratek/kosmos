/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Debug Kit, on the Mac (`user/kits/debug/debug.c`, `docs/debug.md`).
 *
 * Given an image and the functions `readelf -sW` lists in it - one line
 * each, `address size name` - every thousandth is looked up at the middle
 * of its span, and the kit must name it: the same start (the address less
 * the offset it reports), and a name the list gives that start (several
 * may share one, when the linker folded identical code). Then an address
 * below every function must find none, and a file that is not an image
 * must be refused.
 *
 * The control is the kit built with `-DDEBUG_NO_SORT`, its table left in
 * the file's order, which must fail.
 *
 * Usage: test_debug IMAGE FUNCTIONS
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../user/kits/debug/debug.h"

static int checks, failed;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failed++;
        printf("not ok - %s\n", what);
    }
}

static long file_read(void *ctx, uint64_t offset, void *into, size_t length)
{
    FILE *f = ctx;
    if (fseeko(f, (off_t)offset, SEEK_SET))
        return -1;
    return (long)fread(into, 1, length, f);
}

struct fn { uint64_t addr, size; char *name; };

static int by_addr(const void *a, const void *b)
{
    const struct fn *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

int main(int argc, char **argv)
{
    FILE *image, *list;
    struct debug_image *dbg;
    const char *why = NULL;
    struct fn *fns = NULL;
    size_t count = 0, room = 0, i, looked = 0, wrong = 0;
    char line[8192], name[4096];
    uint64_t offset;
    clock_t began;

    if (argc != 3) {
        fprintf(stderr, "usage: test_debug IMAGE FUNCTIONS\n");
        return 2;
    }
    image = fopen(argv[1], "rb");
    list = fopen(argv[2], "r");
    if (!image || !list) {
        fprintf(stderr, "test_debug: cannot open %s\n", image ? argv[2] : argv[1]);
        return 2;
    }
    while (fgets(line, sizeof line, list)) {
        char *end, *nm;
        uint64_t addr = strtoull(line, &end, 16), size = strtoull(end, &end, 0);
        while (*end == ' ')
            end++;
        nm = end;
        end[strcspn(end, "\n")] = 0;
        if (!addr || !*nm)
            continue;
        if (count == room) {
            room = room ? room * 2 : 4096;
            fns = realloc(fns, room * sizeof *fns);
        }
        fns[count].addr = addr;
        fns[count].size = size;
        fns[count].name = strdup(nm);
        count++;
    }
    qsort(fns, count, sizeof *fns, by_addr);

    began = clock();
    dbg = debug_open(file_read, image, &why);
    printf("test_debug: %s, %zu functions, opened in %.0f ms\n", argv[1],
           debug_functions(dbg), 1000.0 * (double)(clock() - began) / CLOCKS_PER_SEC);
    check(dbg != NULL, why ? why : "the image opened");
    if (!dbg)
        goto out;
    check(debug_functions(dbg) >= count / 2, "the kit kept the image's functions");

    for (i = 0; i < count; i += 1000) {
        const struct fn *f = &fns[i];
        uint64_t probe = f->addr + f->size / 2, start;
        size_t j;
        int named = 0;
        if (!f->size)
            continue;
        looked++;
        if (debug_name(dbg, probe, name, sizeof name, &offset)) {
            wrong++;
            printf("not ok - %#llx (%s) found nothing\n",
                   (unsigned long long)probe, f->name);
            continue;
        }
        start = probe - offset;
        /* An inner function that starts later and still holds the probe
         * is the right answer too: it is the nearer one. */
        for (j = 0; j < count && !named; j++)
            named = fns[j].addr == start && !strcmp(fns[j].name, name) &&
                    start >= f->addr && probe < start + (fns[j].size ? fns[j].size : 1);
        if (!named) {
            wrong++;
            if (wrong <= 5)
                printf("not ok - %#llx is in %s, the kit said %s + %#llx\n",
                       (unsigned long long)probe, f->name, name,
                       (unsigned long long)offset);
        }
    }
    printf("test_debug: %zu functions looked up, %zu named wrong\n", looked, wrong);
    check(looked > 0 && wrong == 0, "every function looked up was named");

    check(debug_name(dbg, fns[0].addr - 1, name, sizeof name, &offset) != 0,
          "an address below every function finds none");
    check(debug_name(dbg, fns[0].addr, name, 8, &offset) == 0 &&
          strlen(name) <= 7, "a name cut to the room given, and ended");
    debug_close(dbg);

    {
        FILE *list_again = fopen(argv[2], "rb");
        check(debug_open(file_read, list_again, &why) == NULL,
              "a file that is not an image refused");
        fclose(list_again);
    }
out:
    printf("test_debug: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
