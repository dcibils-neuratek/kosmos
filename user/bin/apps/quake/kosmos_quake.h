/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Included ahead of every Quake source file, by `QUAKE_CFLAGS`.
 *
 * For the POSIX names the engine calls that the SDL shim has no business
 * answering. The first: `console.c` deletes `qconsole.log` with `unlink` when
 * started with `-condebug`. Kosmos's `unistd.h` is empty on purpose -
 * `CLAUDE.md` puts `unistd.h` on the list of what a POSIX personality is made
 * of - so the call goes to ISO C's `remove` instead, which does the same job,
 * is declared in `stdio.h`, and which this libc answers by refusing: there is
 * no tree to delete from. No POSIX name is added to Kosmos to get there.
 */
#ifndef KOSMOS_QUAKE_H
#define KOSMOS_QUAKE_H

#define unlink(path) remove(path)

/*
 * And the log `-condebug` appends to, which `Con_DebugLog` writes with POSIX's
 * `open`, `write` and `close` on a file descriptor, and does not check. There
 * are no file descriptors here - `CLAUDE.md` names global file descriptors
 * as part of a POSIX personality - so opening fails, and writing and closing
 * do nothing. The console still prints; the log file is not written. The
 * arguments that are not used vanish with the call, which is also what takes
 * the undefined `O_WRONLY | O_CREAT | O_APPEND` with them.
 *
 * Safe as function-like macros because those three lines are the only calls
 * to these names anywhere in the engine this build compiles, and no header in
 * it declares anything called `open`, `write` or `close`.
 */
#define open(path, flags, mode) (-1)
#define write(fd, buf, count)   (-1)
#define close(fd)               (-1)

/*
 * And the names id Software used in both of its engines.
 *
 * Doom and Quake come from the same hands and share a vocabulary: a zone
 * allocator called `Z_Malloc`, a renderer started by `R_Init`, a `deathmatch`
 * and a `gammatable`. Each engine links alone, and `make MEGA=1` - Doom and
 * Quake in one image, with no dynamic linking to keep them apart - stopped
 * on these twenty-four, each defined twice.
 *
 * Quake's copies are renamed here rather than either tree being edited, the
 * way `unlink` is pointed at `remove` above: every Quake file and
 * `quake_kosmos.c` see these macros, so each name is renamed wherever Quake
 * defines or uses it, and nowhere else. Always, not only in a MEGA image, so
 * the Quake `make quake-check` boots is built the way MEGA's is. A name added
 * to either engine that collides again stops the MEGA link, by name.
 */
#define M_Init          quake_M_Init
#define R_DrawSprite    quake_R_DrawSprite
#define R_Init          quake_R_Init
#define R_InitTextures  quake_R_InitTextures
#define R_SetupFrame    quake_R_SetupFrame
#define S_Init          quake_S_Init
#define S_Shutdown      quake_S_Shutdown
#define S_StartSound    quake_S_StartSound
#define S_StopSound     quake_S_StopSound
#define V_Init          quake_V_Init
#define WritePCXfile    quake_WritePCXfile
#define Z_CheckHeap     quake_Z_CheckHeap
#define Z_ClearZone     quake_Z_ClearZone
#define Z_Free          quake_Z_Free
#define Z_Malloc        quake_Z_Malloc

#define deathmatch      quake_deathmatch
#define gammatable      quake_gammatable
#define mainzone        quake_mainzone
#define nomonsters      quake_nomonsters
#define onground        quake_onground
#define precache        quake_precache
#define snd_channels    quake_snd_channels
#define startepisode    quake_startepisode
#define timelimit       quake_timelimit

#endif
