/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_ELFIMAGE_H
#define KOSMOS_ELFIMAGE_H

/*
 * A program's ELF, read into Kosmos's own image form (`docs/elf.md`).
 *
 * The kernel knows no file format. What it accepts from `SYS_SPAWN_IMAGE`
 * is an image: Kosmos's sixteen-byte header, the code, the data - the same
 * bytes `objcopy -O binary` makes of the system's own ELF. This turns an
 * ELF into those bytes, and refuses anything that is not a Kosmos program
 * with a sentence saying which rule it broke, so a broken file is refused
 * here, in the process that read it, and the kernel never sees it.
 *
 * Plain C over buffers, so the Mac runs it too (`tools/test_elfimage.c`).
 */

#include <stddef.h>
#include <stdint.h>

#define ELFIMAGE_AARCH64  183u      /* EM_AARCH64 */
#define ELFIMAGE_X86_64    62u      /* EM_X86_64 */

/* What an image on this machine must be. */
struct elfimage_want {
    unsigned machine;               /* this machine's processor */
    uint64_t base;                  /* USER_BASE: where every image begins */
    uint64_t most;                  /* how long one may be: 40 MB */
};

/*
 * **Read in two parts**, for a launcher that does not hold the whole file:
 * a program's image may be tens of megabytes, and it is read a window at a
 * time into the image where each segment belongs (`init.lua`, `launch`).
 *
 * `elfimage_plan` reads the file's first bytes - its header and program
 * headers, which a Kosmos program has at offset 64 - and says where each
 * segment goes and where Kosmos's header is in the file. `elfimage_head`
 * checks those sixteen bytes against the plan once they have been read.
 * Everything `elfimage_write` refuses, the two refuse between them.
 */
struct elfimage_plan {
    size_t   length;                /* the image's bytes */
    uint64_t head;                  /* where Kosmos's header is, in the file */
    unsigned count;                 /* segments */
    struct {
        uint64_t offset;            /* in the file */
        uint64_t at;                /* in the image */
        uint64_t size;              /* bytes to copy; the rest of it is zero */
    } segment[16];
    uint64_t code_end;              /* where the code ends, in the image */
    uint64_t data_start;            /* where the first data begins, or length */
};

const char *elfimage_plan(const unsigned char *start, size_t start_len,
                          uint64_t file_len, const struct elfimage_want *want,
                          struct elfimage_plan *plan);

const char *elfimage_head(const unsigned char head[16],
                          const struct elfimage_plan *plan);

/* How long the image the file describes is, into `*length`; NULL, or why
 * the file is not a Kosmos program. */
const char *elfimage_size(const unsigned char *file, size_t file_len,
                          const struct elfimage_want *want, size_t *length);

/* The image itself, into `out`, which has `room` bytes; NULL, or why not.
 * Checks everything `elfimage_size` checks, so it can be called alone. */
const char *elfimage_write(const unsigned char *file, size_t file_len,
                           const struct elfimage_want *want,
                           unsigned char *out, size_t room, size_t *length);

#endif
