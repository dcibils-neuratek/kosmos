/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_SERVERS_FDT_H
#define USER_SERVERS_FDT_H

/*
 * **A flattened device tree, written** - the description of a machine an
 * ARM operating system is handed at boot, in the format the Devicetree
 * Specification (v0.4, chapter 5) gives: a header, an empty memory
 * reservation block, a structure block of nodes and properties, and a block
 * of property names. Big-endian throughout.
 *
 * Written for `vmm.c`, which describes each guest's small machine this way
 * (`docs/virtualization.md` V3). Nothing here reads one.
 *
 * Usage: `fdt_begin` over a buffer, nodes opened and closed around their
 * properties, `fdt_finish` - which answers the tree's size, or 0 if the
 * buffer was too small for it.
 */

#include <stddef.h>
#include <stdint.h>

struct fdt {
    uint8_t *buf;
    size_t   cap;
    size_t   structure;             /* bytes of the structure block so far */
    char     names[1024];           /* the strings block, built beside it */
    size_t   names_len;
    int      full;                  /* a write did not fit */
};

void   fdt_begin(struct fdt *f, void *buf, size_t cap);
void   fdt_node(struct fdt *f, const char *name);
void   fdt_end_node(struct fdt *f);
void   fdt_prop(struct fdt *f, const char *name, const void *data, size_t len);
void   fdt_prop_str(struct fdt *f, const char *name, const char *value);
void   fdt_prop_u32(struct fdt *f, const char *name, uint32_t value);
void   fdt_prop_cells(struct fdt *f, const char *name, const uint32_t *cells, size_t n);
void   fdt_prop_empty(struct fdt *f, const char *name);
size_t fdt_finish(struct fdt *f);

#endif /* USER_SERVERS_FDT_H */
