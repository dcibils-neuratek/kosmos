/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * lieff's `minimp4`, compiled - with its `malloc`, `realloc` and `free` the
 * recording's arena (`record_core.c`), since a process's heap is 2 MB and a
 * recording's tables grow with it. The names are defined before the header
 * is included, after the C library's own declarations, so the vendored file
 * is compiled as released.
 */

#include <stddef.h>
#include <stdlib.h>

#include "record_config.h"

void *kosmos_record_malloc(size_t n);
void *kosmos_record_realloc(void *p, size_t n);
void  kosmos_record_free(void *p);

#define malloc  kosmos_record_malloc
#define realloc kosmos_record_realloc
#define free    kosmos_record_free

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
