/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A kit's way to the program that called it**: the program's own
 * `use` and `fs`, found where a library would find them, so a kit in C
 * reaches what its caller can and nothing more. Written for the Window Kit
 * and shared with the Compute Kit, so there is one copy of each.
 */

#ifndef KOSMOS_KITENV_H
#define KOSMOS_KITENV_H

#include <stddef.h>

#include "lua.h"

/*
 * The program's own `name` - `use`, `fs` - on the stack, or 0. A program's
 * world is its environment, not Lua's globals (`init.lua`, `env.use`): the
 * kit looks at the `_ENV` of the nearest Lua function that called into
 * this C, which is the program's.
 */
int kit_program_has(lua_State *L, const char *name);

/*
 * `use(path)`, loaded once and kept in the registry at `*ref`: the
 * library's table on the stack, or 0 with why in `why`.
 */
int kit_library(lua_State *L, int *ref, const char *path, char *why, size_t why_bytes);

/* `fs.capability(path)`: the index this process holds for `path`, or -1. */
long kit_capability(lua_State *L, const char *path);

#endif
