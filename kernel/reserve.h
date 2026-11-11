/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Addresses reserved, then protected and released (`docs/w2.md` part 2,
 * `reserve.c`).
 */
#ifndef KOSMOS_RESERVE_H
#define KOSMOS_RESERVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct process;

/* A run of reserved pages with one access, in its process's list. */
struct reserve_span {
    bool                 in_use;
    uintptr_t            va;
    size_t               pages;
    unsigned             access;    /* RESERVE_NONE ... RESERVE_RX */
    struct reserve_span *next;      /* the owner's next, by address */
};

void reserve_init(void);

/* SYS_RESERVE, SYS_PROTECT, SYS_RELEASE: `syscall.h` has what each says. */
long reserve_create(struct process *p, size_t pages);
long reserve_protect(struct process *p, uintptr_t va, size_t pages,
                     unsigned long access);
long reserve_release(struct process *p, uintptr_t va, size_t pages);

/* A first touch of a reserved page whose access allows one: a zeroed page
 * mapped with that access, and true. False for a page with no access, or
 * not reserved, and the fault is what it looks like. */
bool reserve_fill(struct process *p, uintptr_t va);

/* What a reserved address's access is, for a fault's report: "no access",
 * "read only", "read and write", "read and execute"; NULL if the address
 * is not reserved. */
const char *reserve_describe(struct process *p, uintptr_t va);

/* Every committed page given back and every span with them, as the
 * process ends - before its tables are freed. */
void reserve_teardown(struct process *p);

#endif
