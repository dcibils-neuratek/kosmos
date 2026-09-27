/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A program's ELF, read into Kosmos's own image form. `elfimage.h` says
 * what for; `docs/elf.md` says why the reading is here and not in the
 * kernel.
 *
 * **Everything in the file is a claim.** The file came off a disk, perhaps a
 * stick somebody else wrote, so every offset and size is checked against the
 * file before it is used, and every sum is written so that it cannot wrap: a
 * size near 2^64 added to an offset is a small number, and a small number
 * passes a bounds check it should fail.
 *
 * **What makes it a Kosmos program**, beyond being an ELF for this machine:
 * its first segment is the code, at the base, read and execute; the rest is
 * data, read and write, never both; the code starts with Kosmos's header,
 * whose `rx_bytes` is where the kernel will stop mapping code - so the
 * code must end before it and the data begin after it, or the permissions
 * the ELF states and the ones the kernel gives would disagree; and it starts
 * at the base plus sixteen, where the kernel starts every image.
 */

#include <string.h>

#include "elfimage.h"

#define EHDR_SIZE      64u
#define PHDR_SIZE      56u
#define MOST_PHDRS     16u
#define PAGE           4096u

#define PT_LOAD        1u
#define PT_DYNAMIC     2u
#define PT_INTERP      3u
#define PT_TLS         7u

#define PF_X           1u
#define PF_W           2u
#define PF_R           4u

#define KOSMOS_MAGIC   0x534f4d534f4bull    /* "KOSMOS", as the header has it */
#define KOSMOS_HEADER  16u

/* Little-endian, whatever the machine reading it is. */
static uint64_t le(const unsigned char *p, unsigned bytes)
{
    uint64_t v = 0;
    unsigned i;

    for (i = bytes; i > 0; i--) {
        v = (v << 8) | p[i - 1];
    }

    return v;
}

static const char *machine_name(unsigned m)
{
    switch (m) {
    case ELFIMAGE_AARCH64: return "an AArch64 processor";
    case ELFIMAGE_X86_64:  return "an x86-64 processor";
    case 40:               return "a 32-bit ARM processor";
    case 3:                return "a 32-bit x86 processor";
    case 243:              return "a RISC-V processor";
    default:               return "another processor";
    }
}

/* One loadable segment, as the file states it. */
struct seg {
    uint64_t offset, vaddr, filesz, memsz;
    unsigned flags;
};

/*
 * Everything checked, the loadable segments gathered, and the image's
 * length. NULL, or the sentence.
 */
static const char *read_file(const unsigned char *f, size_t n,
                             const struct elfimage_want *want,
                             struct seg *segs, unsigned *nsegs, size_t *length)
{
    uint64_t phoff, entry, rx_bytes, end = 0, prev_end = 0;
    unsigned phnum, phentsize, i, loads = 0;
    static char said[96];

    if (f == NULL || n < EHDR_SIZE) {
        return "shorter than an ELF header";
    }

    if (f[0] != 0x7f || f[1] != 'E' || f[2] != 'L' || f[3] != 'F') {
        return "not an ELF file";
    }

    if (f[4] != 2) {
        return "not a 64-bit ELF";
    }

    if (f[5] != 1) {
        return "not little-endian";
    }

    if (f[6] != 1 || le(f + 20, 4) != 1) {
        return "an ELF version this does not know";
    }

    if (le(f + 16, 2) == 3) {
        return "position-independent, which Kosmos does not load - a program is "
               "linked at the base, as the system's own image is";
    }

    if (le(f + 16, 2) != 2) {
        return "not an executable - an object file or a library";
    }

    if (le(f + 18, 2) != want->machine) {
        /* A static buffer: the sentence names the processor it was built
         * for, and the caller copies it before asking again. */
        strcpy(said, "built for ");
        strcat(said, machine_name((unsigned)le(f + 18, 2)));
        strcat(said, ", not this one");
        return said;
    }

    entry = le(f + 24, 8);
    phoff = le(f + 32, 8);
    phentsize = (unsigned)le(f + 54, 2);
    phnum = (unsigned)le(f + 56, 2);

    if (phentsize != PHDR_SIZE) {
        return "program headers of a size this does not know";
    }

    if (phnum == 0) {
        return "no program headers - nothing to load";
    }

    if (phnum > MOST_PHDRS) {
        return "more program headers than a Kosmos program has";
    }

    if (phoff > n || (uint64_t)phnum * PHDR_SIZE > n - phoff) {
        return "its program headers run past the end of the file";
    }

    for (i = 0; i < phnum; i++) {
        const unsigned char *ph = f + phoff + (uint64_t)i * PHDR_SIZE;
        unsigned type = (unsigned)le(ph, 4);
        struct seg s;

        if (type == PT_INTERP || type == PT_DYNAMIC) {
            return "it asks for a dynamic linker, and Kosmos has none: a "
                   "program carries what it uses";
        }

        if (type == PT_TLS) {
            return "it has thread-local storage in its file, which Kosmos "
                   "does not set up from one";
        }

        if (type != PT_LOAD) {
            continue;                 /* a note, the stack's flags: nothing to load */
        }

        s.flags  = (unsigned)le(ph + 4, 4);
        s.offset = le(ph + 8, 8);
        s.vaddr  = le(ph + 16, 8);
        s.filesz = le(ph + 32, 8);
        s.memsz  = le(ph + 40, 8);

        if (s.filesz > s.memsz) {
            return "a segment with more bytes in the file than in memory";
        }

        if (s.offset > n || s.filesz > n - s.offset) {
            return "a segment runs past the end of the file";
        }

        if (s.vaddr < want->base) {
            return "a segment below where a Kosmos image begins";
        }

        if (s.memsz > want->most || s.vaddr - want->base > want->most - s.memsz) {
            return "longer than an image may be - it would run into its heap";
        }

        if ((s.flags & PF_W) != 0 && (s.flags & PF_X) != 0) {
            return "a segment that is writable and executable at once";
        }

        if ((s.flags & PF_R) == 0) {
            return "a segment that cannot be read";
        }

        if (loads > 0 && s.vaddr < prev_end) {
            return "segments out of order, or overlapping";
        }

        if (loads == 0) {
            if (s.vaddr != want->base) {
                return "its first segment is not where a Kosmos image begins";
            }

            if ((s.flags & PF_X) == 0) {
                return "its first segment is not code";
            }
        } else if ((s.flags & PF_X) != 0) {
            return "code after its data - a Kosmos image is its code, then its data";
        }

        if (loads == MOST_PHDRS) {
            return "more program headers than a Kosmos program has";
        }

        segs[loads++] = s;
        prev_end = s.vaddr + s.memsz;

        if (prev_end - want->base > end) {
            end = prev_end - want->base;
        }
    }

    if (loads == 0) {
        return "nothing in it to load";
    }

    /* Kosmos's header, at the front of the code. */
    if (segs[0].filesz < KOSMOS_HEADER) {
        return "no room for a Kosmos header at its start";
    }

    if (le(f + segs[0].offset, 8) != KOSMOS_MAGIC) {
        return "no Kosmos header at its start - it was not built for Kosmos";
    }

    rx_bytes = le(f + segs[0].offset + 8, 8);

    if (rx_bytes == 0 || (rx_bytes % PAGE) != 0) {
        return "its header's code size is not a whole number of pages";
    }

    if (segs[0].memsz > rx_bytes) {
        return "its code runs past where its header says code ends";
    }

    for (i = 1; i < loads; i++) {
        if (segs[i].vaddr - want->base < rx_bytes) {
            return "its data begins inside what its header calls code";
        }
    }

    if (end < rx_bytes) {
        return "its header says there is more code than the file has";
    }

    if (entry != want->base + KOSMOS_HEADER) {
        return "it starts somewhere other than where a Kosmos image starts";
    }

    *nsegs = loads;
    *length = (size_t)end;
    return NULL;
}

const char *elfimage_size(const unsigned char *file, size_t file_len,
                          const struct elfimage_want *want, size_t *length)
{
    struct seg segs[MOST_PHDRS];
    unsigned nsegs;

    return read_file(file, file_len, want, segs, &nsegs, length);
}

const char *elfimage_write(const unsigned char *file, size_t file_len,
                           const struct elfimage_want *want,
                           unsigned char *out, size_t room, size_t *length)
{
    struct seg segs[MOST_PHDRS];
    unsigned nsegs, i;
    const char *why = read_file(file, file_len, want, segs, &nsegs, length);

    if (why != NULL) {
        return why;
    }

    if (out == NULL || *length > room) {
        return "no room for the image it describes";
    }

    /* The gaps between segments and a segment's bytes past its file part are
     * zeroes, as `objcopy` writes them - and as the kernel expects them. */
    memset(out, 0, *length);

    for (i = 0; i < nsegs; i++) {
        memcpy(out + (segs[i].vaddr - want->base), file + segs[i].offset,
               (size_t)segs[i].filesz);
    }

    return NULL;
}
