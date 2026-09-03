/*
 * A first-fit heap with boundary coalescing.
 *
 * Deliberately the simplest thing that is correct. Every block is on one
 * doubly-linked list in address order, so freeing is a look at the two
 * neighbours and merging whichever are free. No size classes, no bins, no
 * arenas.
 *
 * The overhead is 32 bytes a block, which is a lot for Lua's many small
 * allocations, and the scan is linear. Both are known and neither is fixed
 * yet, because nothing has measured them. `roadmap.md` M4 adds "allocating
 * and freeing a table" to the benchmark set, and that is the number that
 * decides whether this needs to become a real allocator or stays as it is.
 *
 * What it must not do is fragment badly enough that Lua's GC cannot get
 * memory back, so free coalesces in both directions from the start.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <panic.h>

/*
 * 16 bytes, because that is what a double and a long double want on
 * AArch64 and Lua puts both inside its values. Returning an 8-aligned
 * pointer works until something takes a 16-byte load across it.
 */
#define ALIGNMENT       16
#define ALIGN_UP(n)     (((n) + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1))

struct block {
    size_t        size;     /* payload bytes, always a multiple of ALIGNMENT */
    struct block *prev;     /* the block physically before this one */
    struct block *next;     /* the block physically after this one */
    size_t        free;     /* a word rather than a bit, to keep the header
                             * 32 bytes and therefore 16-byte aligned */
};

#define HEADER      ALIGN_UP(sizeof(struct block))
#define PAYLOAD(b)  ((void *)((char *)(b) + HEADER))
#define BLOCK(p)    ((struct block *)((char *)(p) - HEADER))

static struct block *first;
static size_t total;
static size_t used;

void heap_init(void *base, size_t size)
{
    if (size <= HEADER) {
        panic("heap_init: the region is smaller than one header");
    }

    first = base;
    first->size = ALIGN_UP(size - HEADER) - ALIGNMENT;
    first->prev = NULL;
    first->next = NULL;
    first->free = 1;

    total = first->size;
    used  = 0;
}

size_t heap_used(void) { return used; }
size_t heap_size(void) { return total; }

/* Cuts a block in two when the tail is big enough to be worth having. */
static void split(struct block *b, size_t want)
{
    struct block *tail;

    if (b->size < want + HEADER + ALIGNMENT) {
        return;     /* the remainder could not hold a header and a payload */
    }

    tail = (struct block *)((char *)PAYLOAD(b) + want);
    tail->size = b->size - want - HEADER;
    tail->free = 1;
    tail->prev = b;
    tail->next = b->next;

    if (b->next != NULL) {
        b->next->prev = tail;
    }

    b->next = tail;
    b->size = want;
}

void *malloc(size_t n)
{
    struct block *b;
    size_t want;

    if (n == 0) {
        /* Returning NULL would be legal, and it would also be
         * indistinguishable from failure at every call site. */
        n = 1;
    }

    want = ALIGN_UP(n);

    for (b = first; b != NULL; b = b->next) {
        if (b->free && b->size >= want) {
            split(b, want);
            b->free = 0;
            used += b->size;
            return PAYLOAD(b);
        }
    }

    return NULL;
}

void *calloc(size_t count, size_t size)
{
    size_t n;
    void *p;

    /* The overflow check is the only reason calloc is not malloc plus
     * memset at the call site. */
    if (count != 0 && size > (size_t)-1 / count) {
        return NULL;
    }

    n = count * size;
    p = malloc(n);

    if (p != NULL) {
        memset(p, 0, n);
    }

    return p;
}

/* Absorbs the following block if it is free. */
static void merge_forward(struct block *b)
{
    struct block *n = b->next;

    if (n == NULL || !n->free) {
        return;
    }

    b->size += HEADER + n->size;
    b->next = n->next;

    if (n->next != NULL) {
        n->next->prev = b;
    }
}

void free(void *p)
{
    struct block *b;

    if (p == NULL) {
        return;
    }

    b = BLOCK(p);

    if (b->free) {
        panic("free: double free");
    }

    used -= b->size;
    b->free = 1;

    /* Both directions, every time. Coalescing forward only leaves the heap
     * looking full of small holes that are physically adjacent. */
    merge_forward(b);

    if (b->prev != NULL && b->prev->free) {
        merge_forward(b->prev);
    }
}

void *realloc(void *p, size_t n)
{
    struct block *b;
    void *fresh;
    size_t copy;

    if (p == NULL) {
        return malloc(n);
    }

    if (n == 0) {
        free(p);
        return NULL;
    }

    b = BLOCK(p);

    if (b->size >= ALIGN_UP(n)) {
        /* Growing into the block it already has. Not split back down: the
         * tail would usually be too small to reuse and splitting on every
         * shrink is how a heap gets fragmented. */
        return p;
    }

    /* Try to grow in place by swallowing a free neighbour, which is the
     * common case for a string being appended to. */
    if (b->next != NULL && b->next->free
        && b->size + HEADER + b->next->size >= ALIGN_UP(n)) {
        used -= b->size;
        merge_forward(b);
        split(b, ALIGN_UP(n));
        used += b->size;
        return p;
    }

    fresh = malloc(n);
    if (fresh == NULL) {
        return NULL;    /* the original is untouched, as the standard requires */
    }

    copy = (b->size < n) ? b->size : n;
    memcpy(fresh, p, copy);
    free(p);
    return fresh;
}

void abort(void)
{
    panic("abort() was called");
}

void exit(int status)
{
    (void)status;
    panic("exit() was called: there is nothing to exit to");
}

/*
 * A copy of a string on the heap.
 *
 * Here rather than in `string.c` with its siblings because it allocates,
 * and `string.c` is linked into the kernel, which has no allocator and must
 * not have one. The link error that put it here was the rule enforcing
 * itself, which is the best kind.
 *
 * Returns NULL when there is no room. That is the one thing about `strdup`
 * worth care: it allocates and almost nobody checks. Doom does not check
 * either, which is Doom's problem rather than a reason to hand back
 * something that is not a string.
 */
char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *out = malloc(n);

    if (out != NULL) {
        memcpy(out, s, n);
    }

    return out;
}
