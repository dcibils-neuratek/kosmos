/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A thread's own variables** - `thread_local` in C++, `_Thread_local` in
 * C - laid out as each architecture's ELF ABI expects (`docs/webkit.md`
 * W1b).
 *
 * The image carries a template, `.tdata` then `.tbss` (`user/user.ld`),
 * and every thread that wants its own copy makes one here and points its
 * thread pointer at it. The compiler reaches a variable as a fixed offset
 * from the thread pointer - a static image is the local-exec model, with
 * no lookup at all - and the two architectures put the data on opposite
 * sides of it:
 *
 * - **x86-64, variant II**: the variables *below* the pointer, ending at
 *   it, and `%fs:0` holding the pointer's own value;
 * - **AArch64, variant I**: a sixteen-byte block *at* the pointer, and the
 *   variables after it.
 *
 * `struct tls_block` (`runtime/include/tls.h`) was already both: its first
 * word is its own address, which is `%fs:0`, and it is sixteen bytes on
 * AArch64. So the block every thread has stays where `errno` is found, and
 * the copy is put beside it.
 *
 * **Why the kernel's block is not used.** The kernel makes each thread a
 * page at the bottom of its stack slot (`USER_TBLOCK`), and the page below
 * it is the top of the previous thread's stack - variant II would put the
 * variables there. So a thread that has variables of its own makes its
 * block on the heap, and an image with none never calls this and pays
 * nothing.
 *
 * In front of the block, two words: what to give back to `free` and the
 * pointer the thread had before, so `kosmos_tls_end` can put it back.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "tls.h"

/* From `user/user.ld`; `__tls_align` is a word holding the template's
 * alignment. */
extern const char __tdata_start[], __tdata_end[], __tbss_end[];
extern const uint64_t __tls_align;

struct tls_front {
    void *mem;          /* what malloc gave */
    void *before;       /* the thread pointer this replaced */
};

static size_t round_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}

size_t kosmos_tls_bytes(void)
{
    return (size_t)(__tbss_end - __tdata_start);
}

/*
 * Make this thread's copy and point the thread pointer at it. NULL when
 * the image has no variables of its own (nothing changes), or when there
 * is no memory for one.
 */
void *kosmos_tls_begin(void)
{
    size_t bytes = kosmos_tls_bytes();
    size_t init = (size_t)(__tdata_end - __tdata_start);
    /*
     * **Two alignments, and they are not the same number.** The linker
     * placed every variable by the segment's own (`align`): on x86-64 at
     * the template's size rounded to it below the pointer, on AArch64 at
     * sixteen rounded to it above. The pointer itself is put on sixteen at
     * least (`tp_align`), for the block. Rounding the offsets to sixteen
     * too put a four-byte-aligned template's copy where none of its
     * variables were - the first version of this, which read 0.
     */
    size_t align = (size_t)__tls_align;
    size_t tp_align;
    size_t front = round_up(sizeof(struct tls_front), 16);
    size_t room;
    char *mem, *data;
    uintptr_t tp;
    struct tls_block *block;
    struct tls_front *f;

    if (bytes == 0) {
        return NULL;
    }

    if (align == 0) {
        align = 1;
    }

    tp_align = align < 16 ? 16 : align;

#if defined(__x86_64__)
    /* [front][variables, ending at tp][block] */
    room = round_up(bytes, align);
    mem = malloc(front + tp_align + room + sizeof(struct tls_block));
    if (mem == NULL) {
        return NULL;
    }
    tp = round_up((uintptr_t)mem + front + room, tp_align);
    data = (char *)tp - room;
#else
    /* [front][block, 16 bytes at tp][variables] */
    room = round_up(sizeof(struct tls_block), align);
    mem = malloc(front + tp_align + room + bytes);
    if (mem == NULL) {
        return NULL;
    }
    tp = round_up((uintptr_t)mem + front, tp_align);
    data = (char *)tp + room;
#endif

    memcpy(data, __tdata_start, init);
    memset(data + init, 0, bytes - init);

    block = (struct tls_block *)tp;
#if defined(__x86_64__)
    /* The front words sit below the variables, which sit below the block. */
    f = (struct tls_front *)(data - sizeof(struct tls_front));
#else
    f = (struct tls_front *)(tp - sizeof(struct tls_front));
#endif
    f->mem = mem;
    f->before = kosmos_tls();
    block->self = block;
    block->errno_value = ((struct tls_block *)f->before)->errno_value;
    (void)kosmos_set_tls(block);
    return block;
}

/* Put the pointer back where it was and give the copy back. A thread that
 * ends without calling this leaves its copy on the heap, and only that. */
void kosmos_tls_end(void *made)
{
    struct tls_block *block = made;
    struct tls_front *f;
    size_t align = (size_t)__tls_align;

    if (block == NULL) {
        return;
    }

    if (align == 0) {
        align = 1;
    }

#if defined(__x86_64__)
    f = (struct tls_front *)((char *)block - round_up(kosmos_tls_bytes(), align)
                             - sizeof(struct tls_front));
#else
    f = (struct tls_front *)((char *)block - sizeof(struct tls_front));
#endif
    (void)kosmos_set_tls(f->before);
    free(f->mem);
}

/*
 * **The image's constructors**, in the order the linker sorted them:
 * C++'s globals with a constructor, and C's `__attribute__((constructor))`.
 * Run by `main` once the heap is ready, since a constructor may allocate.
 */
void kosmos_run_constructors(void)
{
    extern void (*const __init_array_start[])(void);
    extern void (*const __init_array_end[])(void);
    size_t i, n = (size_t)(__init_array_end - __init_array_start);

    for (i = 0; i < n; i++) {
        __init_array_start[i]();
    }
}
