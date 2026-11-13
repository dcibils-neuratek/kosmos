/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A ported program's `main`, run inside this process**
 * (`posix_program.h` has what and why). Written for SQLite's test
 * programs, whose kit had it first, and moved here when libxml2's and
 * libpsl's needed the same three things - so there is one copy.
 */

#include <errno.h>
#include <posix_program.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "kosmos.h"

static jmp_buf leaving;
static int     left_with;
static int     running;

/* The stack, mapped once and kept: a test kit runs one program after
 * another, and a map's addresses are not given again (`malloc.c`). A
 * larger one is mapped when a later program asks for more. */
static unsigned long stack_base;        /* the guard page */
static unsigned long stack_pages;       /* above it */

struct run {
    int  (*entry)(int, char **);
    int    argc;
    char **argv;
    int    code;
};

/* The program, or the code it left with - on its own stack, so the jump
 * back lands inside it. */
static void call(void *arg)
{
    struct run *r = arg;

    if (setjmp(leaving) != 0) {
        r->code = left_with;
        return;
    }

    r->code = r->entry(r->argc, r->argv);
}

int posix_program_run(int (*entry)(int, char **), int argc, char **argv,
                      unsigned long pages)
{
    struct run r = { entry, argc, argv, 0 };

    if (running) {
        errno = EBUSY;
        return -1;
    }

    if (pages > stack_pages) {
        long base = kosmos_map(pages + 1);

        if (base < 0 || kosmos_unmap((unsigned long)base, 1) < 0) {
            errno = ENOMEM;
            return -1;
        }

        if (stack_pages != 0) {
            kosmos_unmap(stack_base + KOSMOS_PAGE_SIZE, stack_pages);
        }

        stack_base = (unsigned long)base;
        stack_pages = pages;
    }

    running = 1;
    kosmos_call_on_stack(call, &r, (void *)(uintptr_t)(stack_base
                         + (stack_pages + 1) * KOSMOS_PAGE_SIZE));
    running = 0;

    fflush(stdout);
    fflush(stderr);
    return r.code;
}

_Noreturn void posix_program_exit(int code)
{
    if (!running) {
        exit(code);
    }

    left_with = code;
    longjmp(leaving, 1);
}

/* What the program prints, into a file for the length of the run: a test
 * program says more than the spill ring keeps (64 KB), and curl's says
 * what it fetched on `stdout`. Both kits that run one ask for it here. */
int posix_program_run_to(int (*entry)(int, char **), int argc, char **argv,
                         unsigned long pages, const char *path)
{
    FILE *out, *was_out = stdout, *was_err = stderr;
    int code, saved;

    if ((out = fopen(path, "w")) == NULL) {
        return -1;
    }

    stdout = out;
    stderr = out;
    code = posix_program_run(entry, argc, argv, pages);
    saved = errno;
    fflush(out);
    stdout = was_out;
    stderr = was_err;
    fclose(out);
    errno = saved;
    return code;
}
