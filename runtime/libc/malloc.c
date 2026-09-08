/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A segregated-fit heap with boundary coalescing.
 *
 * Two structures over the same blocks, and keeping them straight is the
 * whole of this file:
 *
 *   - **the physical list**, every block in address order, which is what
 *     lets `free` look at its neighbours and merge them. Unchanged.
 *   - **the bins**, the free blocks only, threaded by size class, which is
 *     what lets `malloc` find one without looking at the rest.
 *
 * The bin links live *inside the free block's own payload*, so they cost
 * nothing: a free block's payload is unused by definition, and the minimum
 * payload is sixteen bytes, which is exactly two pointers.
 *
 * **It was a first-fit scan over the physical list**, which meant every
 * allocation walked every block, free *or allocated*. That is fine on an
 * empty heap and quadratic in aggregate on a full one - a process holding
 * ten thousand live objects walked ten thousand headers to allocate one
 * more. The comment here used to say the scan was "known and not fixed yet,
 * because nothing has measured them", which was true for as long as nobody
 * built the benchmark; `alloc_table` is that benchmark, and it is the number
 * this change is answerable to.
 *
 * What has *not* changed, because it is what the design depends on: free
 * coalesces in both directions, so Lua's collector can get memory back
 * rather than facing a heap of small adjacent holes.
 *
 * The header is still 32 bytes, which is a lot for the many small
 * allocations Lua makes. That one is measured but not fixed: it needs the
 * physical `prev` to go and come back as a footer on free blocks, and it
 * touches every pointer computation here. Worth doing when a number asks
 * for it, and not before.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <panic.h>
#include <setjmp.h>

#ifdef KOSMOS_USER
#include <kosmos.h>
#endif

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

/*
 * Where a free block's bin links live: in its payload, which nobody is
 * using. This is why the minimum payload is ALIGNMENT and not less.
 */
struct link {
    struct block *next;
    struct block *prev;
};

#define LINK(b)     ((struct link *)PAYLOAD(b))

/*
 * The size classes.
 *
 * Exact bins up to 512 bytes, one per 16-byte step, because that is where
 * almost everything Lua allocates lands and an exact bin means the first
 * block in it always fits. Powers of two above that, where the sizes are
 * too spread out for one bin each and a short scan within the bin is
 * cheaper than a bin per size.
 */
#define SMALL_MAX       512u
#define BINS_SMALL      (SMALL_MAX / ALIGNMENT)     /* 32: 16, 32 ... 512 */
#define BINS_LARGE      24u                         /* up to 2^32 */
#define BINS            (BINS_SMALL + BINS_LARGE)

static struct block *bins[BINS];
static struct block *first;
static size_t total;
static size_t used;

static unsigned bin_of(size_t size)
{
    unsigned k;

    if (size <= SMALL_MAX) {
        return (unsigned)(size / ALIGNMENT) - 1u;
    }

    /* floor(log2(size)); 512 is 2^9, so that class starts the large bins. */
    k = BINS_SMALL + (unsigned)(63 - __builtin_clzll((unsigned long long)size))
        - 9u;

    return (k < BINS) ? k : BINS - 1u;
}

static void bin_insert(struct block *b)
{
    unsigned k = bin_of(b->size);

    LINK(b)->next = bins[k];
    LINK(b)->prev = NULL;

    if (bins[k] != NULL) {
        LINK(bins[k])->prev = b;
    }

    bins[k] = b;
}

static void bin_remove(struct block *b)
{
    struct block *n = LINK(b)->next;
    struct block *p = LINK(b)->prev;

    if (p != NULL) {
        LINK(p)->next = n;
    } else {
        bins[bin_of(b->size)] = n;
    }

    if (n != NULL) {
        LINK(n)->prev = p;
    }
}

void heap_init(void *base, size_t size)
{
    unsigned k;

    if (size <= HEADER) {
        panic("heap_init: the region is smaller than one header");
    }

    for (k = 0; k < BINS; k++) {
        bins[k] = NULL;
    }

    first = base;
    first->size = ALIGN_UP(size - HEADER) - ALIGNMENT;
    first->prev = NULL;
    first->next = NULL;
    first->free = 1;

    total = first->size;
    used  = 0;

    bin_insert(first);
}


size_t heap_used(void) { return used; }
size_t heap_size(void) { return total; }

/*
 * Cuts a block in two when the tail is big enough to be worth having.
 *
 * `b` must already be out of its bin - it is about to change size, and a
 * block in the wrong bin is a block `malloc` will hand out too small.
 */
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

    bin_insert(tail);
}

/*
 * How much to ask the kernel for when the heap runs out.
 *
 * A page at a time would be correct and would mean a syscall, a fresh
 * arena and a block that cannot merge with anything, over and over. This is
 * 256 KB, which is small against the 48 MB a process may map and large
 * enough that a program growing steadily asks rarely.
 */
#define GROW_PAGES  64u

/*
 * More heap, from the kernel, as a new arena.
 *
 * **The arenas are not adjacent and must never be merged.** `sys_map` bumps
 * a cursor and never reuses an address, so what comes back is somewhere
 * else entirely; two blocks in different arenas can be neighbours in a bin
 * and are never neighbours in memory. What keeps that safe is that each
 * arena is its own physical list with NULL at both ends, so `free` looking
 * at `prev` and `next` can only ever find a block in the same arena.
 *
 * This is what the fixed 2 MB heap used to be instead of. `USER_HEAP_PAGES`
 * is a compile-time answer to a runtime question, which is why `make DOOM=1`
 * exists at all - Doom wanted a 5 MB zone and the only way to give it one
 * was to rebuild the system with a different `-D`. A program that needs
 * memory can now ask for it, and one that does not never pays for it.
 */
#ifdef KOSMOS_USER

static bool grow(size_t want)
{
    size_t need = want + HEADER + ALIGNMENT;
    size_t pages = (need + 4095u) / 4096u;
    struct block *b;
    long at;

    if (pages < GROW_PAGES) {
        pages = GROW_PAGES;
    }

    at = kosmos_map(pages);

    if (at < 0) {
        return false;
    }

    /* Page-aligned from the kernel, so the payload sixteen bytes in is
     * aligned too - the one property this cast depends on. */
    b = (struct block *)(uintptr_t)at;

    b->size = pages * 4096u - HEADER;
    b->prev = NULL;
    b->next = NULL;
    b->free = 1;

    total += b->size;
    bin_insert(b);

    return true;
}

#else

/*
 * And at EL1, where there is nothing to ask.
 *
 * The kernel's test image links this file for its guest-side C tests, and
 * `kosmos_map` is a *syscall* - the kernel does not make one to itself. So
 * there the heap stays what it was handed, which is what it has always been.
 *
 * Worth being explicit that this is not a hole in `CLAUDE.md`'s "no dynamic
 * allocator in the kernel": that rule is about the kernel's own memory, and
 * the kernel proper links `string.c` and not this file. What compiles here
 * is a userland allocator that a test image happens to carry.
 */
static bool grow(size_t want)
{
    (void)want;
    return false;
}

#endif

void *malloc(size_t n)
{
    size_t want;
    unsigned k;

    if (n == 0) {
        /* Returning NULL would be legal, and it would also be
         * indistinguishable from failure at every call site. */
        n = 1;
    }

    want = ALIGN_UP(n);

    /*
     * The bin the size belongs to, then upwards. An exact small bin's first
     * block always fits; a large bin holds a range, so the size is still
     * checked. Either way this walks free blocks only, and usually one.
     */
    for (;;) {
        for (k = bin_of(want); k < BINS; k++) {
            struct block *b;

            for (b = bins[k]; b != NULL; b = LINK(b)->next) {
                if (b->size >= want) {
                    bin_remove(b);
                    split(b, want);
                    b->free = 0;
                    used += b->size;
                    return PAYLOAD(b);
                }
            }
        }

        /* Nothing fits. Ask for more and look once more; if the kernel says
         * no, so does this. */
        if (!grow(want)) {
            return NULL;
        }
    }
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

/*
 * Absorbs the following block unconditionally.
 *
 * The caller checks that there is one and that it is free, and takes it out
 * of its bin first. Split that way round because both callers already know
 * the answer and the bin has to be left correct either way.
 */
static void absorb(struct block *b)
{
    struct block *n = b->next;

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

    /*
     * Both directions, every time. Coalescing forward only leaves the heap
     * looking full of small holes that are physically adjacent.
     *
     * Each neighbour comes out of its bin before it is merged, because
     * merging changes a size and a bin is indexed by size. The block that
     * survives goes into its new bin once, at the end.
     */
    if (b->next != NULL && b->next->free) {
        bin_remove(b->next);
        absorb(b);
    }

    if (b->prev != NULL && b->prev->free) {
        struct block *prev = b->prev;

        bin_remove(prev);
        absorb(prev);           /* takes in `b` */
        b = prev;
    }

    bin_insert(b);
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
        bin_remove(b->next);
        absorb(b);
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

/*
 * Somewhere to land, for a program that thinks it owns the process.
 *
 * `exit()` used to panic, and the comment was right: there is nothing to
 * exit *to*, because this process is not a Unix program with a `main` that
 * somebody called. It is a Lua application that happens to have some C in
 * it.
 *
 * That is exactly the case a vendored port breaks. Doom calls `I_Error`
 * when it does not like something, `I_Error` prints and calls `exit`, and
 * with a panic on the end of that the process dies with the explanation
 * still in a buffer nobody read. What that looks like from outside is a
 * black window and total silence, which is the least useful failure a
 * system can have.
 *
 * So a caller may arm a landing place. `exit` jumps back to it and the
 * caller carries on - drains the message, closes the window, tells the
 * person what happened. Nothing is unwound: no destructors run and no
 * memory is freed, and that is fine here because the whole heap goes when
 * the process does.
 *
 * Unarmed, it still panics, because then the old comment is true again.
 */
static jmp_buf exit_to;
static int     exit_armed;

int kosmos_exit_arm(void)
{
    int landed;

    exit_armed = 1;
    landed = setjmp(exit_to);

    if (landed != 0) {
        exit_armed = 0;
    }

    return landed;
}

void kosmos_exit_disarm(void)
{
    exit_armed = 0;
}

void exit(int status)
{
    if (exit_armed) {
        exit_armed = 0;

        /* 1 when the status was 0, because longjmp(0) is defined to arrive
         * as 1 and the caller has to be able to tell a landing from the
         * first pass. The status itself is not carried: nothing here reads
         * it, and inventing an encoding for it would be inventing a
         * requirement. */
        longjmp(exit_to, (status == 0) ? 1 : 2);
    }

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
