/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SOURCE_H
#define KOSMOS_SOURCE_H

/*
 * A file carried inside the image, as a C server sees it.
 *
 * `tools/progs2c.py` writes an array of these beside the Lua chunk it has
 * always written. The length is recorded rather than measured because a
 * program's source may legitimately contain a NUL byte and `strlen` would
 * stop at it - which would present as a program that runs perfectly up to
 * the middle.
 */
struct source_entry {
    const char   *name;
    const char   *text;
    unsigned long length;
};

#endif /* KOSMOS_SOURCE_H */
