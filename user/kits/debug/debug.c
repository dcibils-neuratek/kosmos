/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Debug Kit** (`docs/debug.md`): an address in an image to the
 * function it is in.
 *
 * ELF64, little-endian, read by offsets rather than through structs, so
 * the same file builds freestanding and on the Mac and no padding or
 * alignment is assumed. The layout is the System V gABI's: the header's
 * `e_shoff` (0x28), `e_shentsize` (0x3A), `e_shnum` (0x3C); a section
 * header of 64 bytes with `sh_type` at 4, `sh_offset` at 24, `sh_size` at
 * 32, `sh_link` at 40 and `sh_entsize` at 56; a symbol of 24 bytes with
 * `st_name` at 0, `st_info` at 4 and `st_value` at 8, `st_size` at 16.
 *
 * `.symtab` is read a megabyte at a time and only its functions are kept -
 * 252,000 of them for JetSki, 7.1 MB of symbols read and 4 MB kept - then
 * sorted by address. `.strtab` (28 MB for JetSki) is never read whole: a
 * name is read at its offset when one is asked for.
 */

#include "debug.h"

#include <stdlib.h>
#include <string.h>

#define SHT_SYMTAB  2u
#define STT_FUNC    2u
#define SYM_BYTES   24u
#define SHDR_BYTES  64u
#define CHUNK       (1024u * 1024u)
#define NAME_CHUNK  256u

struct fn {
    uint64_t addr;
    uint64_t size;
    uint32_t name;          /* offset into .strtab */
};

struct debug_image {
    debug_read_fn read;
    void *ctx;
    uint64_t strtab;        /* where .strtab starts in the file */
    uint64_t strtab_size;
    struct fn *fns;
    size_t count;
};

static uint16_t u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static uint64_t u64(const unsigned char *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }

static int read_all(struct debug_image *im, uint64_t at, void *into, size_t n)
{
    return im->read(im->ctx, at, into, n) == (long)n ? 0 : -1;
}

/* By address; at one address the sized function first, so an alias with
 * no size never hides the one that says how long it is. */
static int by_addr(const void *a, const void *b)
{
    const struct fn *x = a, *y = b;
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1;
    if ((x->size != 0) != (y->size != 0))
        return x->size ? -1 : 1;
    return 0;
}

static struct debug_image *fail(struct debug_image *im, const char **why,
                                const char *what)
{
    if (why)
        *why = what;
    debug_close(im);
    return NULL;
}

struct debug_image *debug_open(debug_read_fn read, void *ctx, const char **why)
{
    unsigned char head[64], sh[SHDR_BYTES], *chunk = NULL;
    struct debug_image *im = calloc(1, sizeof *im);
    uint64_t shoff, symoff = 0, symsize = 0, done;
    unsigned shentsize, shnum, i, link = 0;
    size_t room = 0;

    if (!im)
        return fail(im, why, "out of memory");
    im->read = read;
    im->ctx = ctx;
    if (read_all(im, 0, head, sizeof head) ||
        memcmp(head, "\177ELF", 4) || head[4] != 2 || head[5] != 1)
        return fail(im, why, "not a 64-bit little-endian ELF image");
    shoff = u64(head + 0x28);
    shentsize = u16(head + 0x3A);
    shnum = u16(head + 0x3C);
    if (shentsize < SHDR_BYTES || shnum == 0)
        return fail(im, why, "no section headers");

    for (i = 0; i < shnum; i++) {
        if (read_all(im, shoff + (uint64_t)i * shentsize, sh, sizeof sh))
            return fail(im, why, "section headers cut short");
        if (u32(sh + 4) == SHT_SYMTAB && u64(sh + 56) == SYM_BYTES) {
            symoff = u64(sh + 24);
            symsize = u64(sh + 32);
            link = u32(sh + 40);
            break;
        }
    }
    if (!symsize)
        return fail(im, why, "no symbol table");
    if (link >= shnum ||
        read_all(im, shoff + (uint64_t)link * shentsize, sh, sizeof sh))
        return fail(im, why, "the symbol table's names are missing");
    im->strtab = u64(sh + 24);
    im->strtab_size = u64(sh + 32);

    chunk = malloc(CHUNK);
    if (!chunk)
        return fail(im, why, "out of memory");
    symsize -= symsize % SYM_BYTES;
    for (done = 0; done < symsize;) {
        size_t n = symsize - done < CHUNK ? (size_t)(symsize - done)
                                          : CHUNK - CHUNK % SYM_BYTES;
        size_t at;
        if (read_all(im, symoff + done, chunk, n)) {
            free(chunk);
            return fail(im, why, "the symbol table is cut short");
        }
        for (at = 0; at < n; at += SYM_BYTES) {
            const unsigned char *s = chunk + at;
            uint64_t value = u64(s + 8);
            if ((s[4] & 0xf) != STT_FUNC || value == 0 ||
                u32(s) >= im->strtab_size)
                continue;
            if (im->count == room) {
                size_t more = room ? room * 2 : 4096;
                struct fn *grown = realloc(im->fns, more * sizeof *grown);
                if (!grown) {
                    free(chunk);
                    return fail(im, why, "out of memory");
                }
                im->fns = grown;
                room = more;
            }
            im->fns[im->count].addr = value;
            im->fns[im->count].size = u64(s + 16);
            im->fns[im->count].name = u32(s);
            im->count++;
        }
        done += n;
    }
    free(chunk);
    if (!im->count)
        return fail(im, why, "no functions in the symbol table");
#ifndef DEBUG_NO_SORT   /* the host test's control: unsorted, it must fail */
    qsort(im->fns, im->count, sizeof *im->fns, by_addr);
#else
    (void)by_addr;
#endif
    return im;
}

/* The function holding `addr`: the last that starts at or before it and
 * whose size reaches past it. A few are looked at going back, since one
 * function can sit inside another's span (a local label given a size);
 * one with no size is taken only when it is the nearest. */
static const struct fn *holding(const struct debug_image *im, uint64_t addr)
{
    size_t lo = 0, hi = im->count, i, back;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (im->fns[mid].addr <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    for (i = lo, back = 0; i > 0 && back < 8; i--, back++) {
        const struct fn *f = &im->fns[i - 1];
        if (f->size && addr < f->addr + f->size)
            return f;
    }
    return im->fns[lo - 1].size ? NULL : &im->fns[lo - 1];
}

int debug_name(struct debug_image *im, uint64_t addr, char *name, size_t room,
               uint64_t *offset)
{
    const struct fn *f;
    size_t got = 0;

    if (!im || !name || room == 0)
        return -1;
    name[0] = 0;
    f = holding(im, addr);
    if (!f)
        return -1;
    if (offset)
        *offset = addr - f->addr;
    while (got + 1 < room) {
        size_t want = room - 1 - got < NAME_CHUNK ? room - 1 - got : NAME_CHUNK;
        uint64_t at = (uint64_t)f->name + got;
        long n;
        char *end;
        if (at >= im->strtab_size)
            break;
        if (want > im->strtab_size - at)
            want = (size_t)(im->strtab_size - at);
        n = im->read(im->ctx, im->strtab + at, name + got, want);
        if (n <= 0)
            break;
        end = memchr(name + got, 0, (size_t)n);
        if (end)
            return 0;
        got += (size_t)n;
    }
    name[got] = 0;
    return 0;
}

size_t debug_functions(const struct debug_image *im)
{
    return im ? im->count : 0;
}

void debug_close(struct debug_image *im)
{
    if (!im)
        return;
    free(im->fns);
    free(im);
}
