/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef Q3_KOSMOS_H
#define Q3_KOSMOS_H

/*
 * What the files of Quake III's platform say to one another
 * (`q3_kosmos.c` has the list of them).
 */

#include "lua.h"

struct kw_window;

/* q3_vk.c: the game's window, once `VKimp_Init` has opened it, or NULL. */
extern struct kw_window *q3_window;

/* q3_vk.c: closed, if it is open - on a quit and on an error. */
void q3_window_close(void);

/* q3_input.c: the window's events since the last frame, into the engine's
 * queue; and the pointer asked for or given back as the game needs it. */
void IN_Init(void);
void IN_Frame(void);
void IN_Shutdown(void);

/* q3_snd.c: q3.sound(ring, rate) - the stream `audio.open` made. */
int q3_lua_sound(lua_State *L);

#endif /* Q3_KOSMOS_H */
