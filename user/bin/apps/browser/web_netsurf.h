/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What NetSurf's layout stands on, when the browser around it is Kosmos's
 * (`web_netsurf.c`, `roadmap.md` 6zz j).
 */

#ifndef WEB_NETSURF_H
#define WEB_NETSURF_H

#include "lua.h"

/* Runs the work NetSurf scheduled, to the end - a box tree built in the
 * slices NetSurf builds it in, one after another, with nothing between. */
void web_netsurf_run(void);

/* `web.join(base, href)` -> the address `href` names on the page at `base`,
 * by NetSurf's own URL code; nil and why when either is not one. */
int web_netsurf_join(lua_State *L);

#endif
