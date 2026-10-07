/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_KIT_H
#define KOSMOS_KIT_H

/*
 * What a project's C includes (`docs/tinycc.md`, C6): Lua's API, the one
 * door to a surface's pixels, and `KOSMOS_KIT(name)` - the function the
 * image's runtime calls when the project's Lua asks `use("name.elf")`.
 *
 *   #include "kosmos_kit.h"
 *
 *   static int l_answer(lua_State *L) { lua_pushinteger(L, 42); return 1; }
 *
 *   KOSMOS_KIT(myapp)
 *   {
 *       lua_newtable(L);
 *       lua_pushcfunction(L, l_answer);
 *       lua_setfield(L, -2, "answer");
 *   }
 *
 * The name is the image's - `build/myapp.elf` - and the function leaves one
 * value on the stack: what `use` answers.
 */

#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

/* A surface's pixels, and its width, height and pitch (in bytes - almost
 * never width times four): a Lua error for anything that is not a live
 * surface. `gfx.c`'s, the one file that knows a surface's shape. */
uint32_t *kosmos_surface_pixels(struct lua_State *L, int index, unsigned *width,
                                unsigned *height, unsigned *pitch);

#define KOSMOS_KIT(name) \
    const char kosmos_project_name[] = #name; \
    void kosmos_project_kit(lua_State *L)

#endif /* KOSMOS_KIT_H */
