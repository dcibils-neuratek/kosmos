/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Quake III Arena, on this machine: the engine's side of the system
 * (`docs/quake3e.md` §2).
 *
 * Quake3e is fetched, held to its sum and compiled as released - nothing
 * in it is edited (`kosmos_q3.h` says how that is possible). These files
 * are the platform under it, written in place of `unix_main.c` and
 * `unix_shared.c`, and the one place the engine's vocabulary and Kosmos's
 * meet:
 *
 * - this one: the kit `quake3.lua` reaches, the engine's start and its
 *   frame, and every `Sys_*` that is time, files, printing and stopping;
 * - `q3_vk.c`: the window, and Vulkan's surface in it;
 * - `q3_input.c`: keys, characters, buttons and the captured pointer;
 * - `q3_snd.c`: the engine's mixer, into the audio server's ring;
 * - `q3_net.c`: the network as a game on one machine needs it - none.
 *
 * **None of it blocks, and Lua owns the loop**, as Quake 1's port does:
 * `q3.start` runs `Com_Init` and returns, `q3.frame` runs one `Com_Frame`
 * and returns. What upstream's `main` does in a `while (1)` is a Lua loop
 * here, which is what lets the window close and the process end with its
 * reason said.
 *
 * **Stopping is `exit`, landed.** `Sys_Quit` and `Sys_Error` end in
 * `exit`, and this libc's `exit` comes back to the `setjmp` in `l_start`
 * or `l_frame` (`runtime/include/stdlib.h`): 1 for a quit, 2 for an error,
 * whose words were printed first. An `ERR_DROP` never gets that far - the
 * engine catches it in `Com_Frame` and goes back to the menu, as it does
 * everywhere.
 */

#include "q_shared.h"
#include "qcommon.h"
#include "../ui/ui_public.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <kosmos.h>

#include "lua.h"
#include "lauxlib.h"

#include "q3_kosmos.h"

/* The ring `printf` spills into for a process without the console. */
size_t kosmos_spill_drain(char *out, size_t max);

/* `client.h`'s, which drags the renderer's headers in with it. */
qboolean CL_NoDelay(void);

/*--------------------------------------------------------------------------
 * Where the game is.
 *
 * `quake3.lua` says: the base path is the application's folder, with the
 * paks in `baseq3/` (or `demoq3/` for the demo), and the home path - the
 * config, screenshots and demos the game writes - a folder beside it.
 *------------------------------------------------------------------------*/

static char base_path[MAX_OSPATH];
static char home_path[MAX_OSPATH];

const char *Sys_DefaultBasePath(void)
{
    return base_path;
}

const char *Sys_DefaultHomePath(void)
{
    return home_path;
}

char *Sys_DefaultAppPath(void)
{
    return base_path;
}

const char *Sys_Pwd(void)
{
    return base_path;
}

const char *Sys_SteamPath(void)
{
    return "";
}

/*--------------------------------------------------------------------------
 * Time.
 *------------------------------------------------------------------------*/

/* `Sys_Microseconds` is upstream's own (`common.c`), on `gettimeofday`. */

int Sys_Milliseconds(void)
{
    return (int)(Sys_Microseconds() / 1000);
}

void Sys_Sleep(int msec)
{
    if (msec > 0) {
        usleep((useconds_t)msec * 1000u);
    }
}

qboolean Sys_RandomBytes(byte *string, int len)
{
    while (len > 0) {
        int n = len > 256 ? 256 : len;

        if (getentropy(string, (size_t)n) != 0) {
            return qfalse;
        }

        string += n;
        len -= n;
    }

    return qtrue;
}

/*--------------------------------------------------------------------------
 * Files, through the POSIX layer (`docs/w3.md`): a path here resolves in
 * this process's namespace and nowhere else.
 *------------------------------------------------------------------------*/

qboolean Sys_Mkdir(const char *path)
{
    if (mkdir(path, 0750) == 0) {
        return qtrue;
    }

    return errno == EEXIST ? qtrue : qfalse;
}

FILE *Sys_FOpen(const char *ospath, const char *mode)
{
    struct stat st;

    /* A folder opened as a file would read as nothing, and the engine
     * would take that for an empty file. */
    if (stat(ospath, &st) == 0 && S_ISDIR(st.st_mode)) {
        return NULL;
    }

    return fopen(ospath, mode);
}

qboolean Sys_ResetReadOnlyAttribute(const char *ospath)
{
    (void)ospath;
    return qfalse;
}

qboolean Sys_GetFileStats(const char *filename, fileOffset_t *size,
                          fileTime_t *mtime, fileTime_t *ctime)
{
    struct stat st;

    if (filename == NULL || stat(filename, &st) != 0) {
        *size = 0;
        *mtime = *ctime = 0;
        return qfalse;
    }

    *size = st.st_size;
    *mtime = st.st_mtime;
    *ctime = st.st_ctime;
    return qtrue;
}

/*
 * A folder's names, as `unix_shared.c` lists them: those ending in
 * `extension` (or every folder, for "/"), or matching `filter`, and with
 * `subdirs` the folders under it too, each named by its path from the top.
 */
#define LISTED_MAX 0x1000

static int list_into(const char *directory, const char *subdir,
                     const char *extension, const char *filter,
                     char **list, int count, int subdirs)
{
    char search[MAX_OSPATH * 2 + 1];
    char name[MAX_OSPATH * 2 + 1];
    qboolean folders = extension != NULL && strcmp(extension, "/") == 0;
    int ext_len = extension != NULL ? (int)strlen(extension) : 0;
    struct dirent *d;
    DIR *dir;

    if (subdir[0] != '\0') {
        Com_sprintf(search, sizeof search, "%s/%s", directory, subdir);
    } else {
        Com_sprintf(search, sizeof search, "%s", directory);
    }

    dir = opendir(search);

    if (dir == NULL) {
        return count;
    }

    while (count < LISTED_MAX - 1 && (d = readdir(dir)) != NULL) {
        struct stat st;
        char path[MAX_OSPATH * 3 + 2];
        qboolean is_dir;

        if (d->d_name[0] == '.' &&
            (d->d_name[1] == '\0' || (d->d_name[1] == '.' && d->d_name[2] == '\0'))) {
            continue;
        }

        Com_sprintf(path, sizeof path, "%s/%s", search, d->d_name);
        is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);

        if (subdir[0] != '\0') {
            Com_sprintf(name, sizeof name, "%s/%s", subdir, d->d_name);
        } else {
            Com_sprintf(name, sizeof name, "%s", d->d_name);
        }

        if (is_dir && subdirs) {
            count = list_into(directory, name, extension, filter, list, count, subdirs);

            if (count >= LISTED_MAX - 1) {
                break;
            }
        }

        if (filter != NULL) {
            if (!Com_FilterPath(filter, name)) {
                continue;
            }
        } else if (folders) {
            if (!is_dir) {
                continue;
            }
        } else {
            int len = (int)strlen(d->d_name);

            if (is_dir || len < ext_len ||
                Q_stricmp(d->d_name + len - ext_len, extension) != 0) {
                continue;
            }
        }

        list[count++] = CopyString(name);
    }

    closedir(dir);
    return count;
}

char **Sys_ListFiles(const char *directory, const char *extension,
                     const char *filter, int *numfiles, int subdirs)
{
    char *found[LISTED_MAX];
    char **list;
    int count;

    if (extension == NULL) {
        extension = "";
    }

    count = list_into(directory, "", filter != NULL ? NULL : extension,
                      filter, found, 0, subdirs || filter != NULL);
    *numfiles = count;

    if (count == 0) {
        return NULL;
    }

    list = Z_Malloc((count + 1) * sizeof *list);
    memcpy(list, found, count * sizeof *list);
    list[count] = NULL;

    return list;
}

void Sys_FreeFileList(char **list)
{
    int i;

    if (list == NULL) {
        return;
    }

    for (i = 0; list[i] != NULL; i++) {
        Z_Free(list[i]);
    }

    Z_Free(list);
}

/*
 * **The game's code is in this image** (`docs/quake3e.md` Q3): ioquake3's
 * game, cgame and q3_ui, compiled native and linked in, each made one
 * object whose two doors are renamed for it (the Makefile's `Q3G_MODULE`).
 * Nothing here is linked dynamically, so a "library" is one of those three:
 * the engine asks for `qagamex86_64.so` in each of its game folders in turn
 * (`FS_LoadLibrary`), and the first ask is answered from the table - which
 * folder it named does not matter, since the code is the same wherever the
 * game's data is.
 *
 * Why not the QVMs: the Quake III demo's (1999) speak a user interface of
 * version 3 where every 1.32 engine expects 6, and they are interpreted;
 * these are the same game, compiled, and as fast as the engine.
 */
typedef void (*q3_door)(void);

void q3_game_vmMain(void);
void q3_game_dllEntry(dllSyscall_t engine);
void q3_cgame_vmMain(void);
void q3_cgame_dllEntry(dllSyscall_t engine);
void q3_ui_vmMain(void);
void q3_ui_dllEntry(dllSyscall_t engine);

/*
 * **The demo needs no key.** id's 1.32 q3_ui knew it was running the demo
 * (four arenas, one special) and never asked for a CD key; ioquake3's took
 * that test out (`ui_gameinfo.c`, `uis.demoversion = qfalse`), so with the
 * demo's paks it opens on a key the demo never had. So the user interface
 * is handed the engine's calls through this one, which answers the key's
 * question yes while the game is the demo (`fs_basegame demoq3`) - what
 * the demo's own code did - and passes every other call, and every call
 * for the full game, to the engine as it is.
 */
static dllSyscall_t ui_engine;

static intptr_t QDECL ui_calls(intptr_t call, ...)
{
    intptr_t args[16];
    va_list ap;

    if (call == UI_VERIFY_CDKEY && Q_stricmp(Cvar_VariableString("fs_basegame"), BASEDEMO) == 0) {
        return qtrue;
    }

    va_start(ap, call);
    for (int i = 1; i < 16; i++) {
        args[i] = va_arg(ap, intptr_t);
    }
    va_end(ap);

    return ui_engine(call, args[1], args[2], args[3], args[4], args[5], args[6], args[7],
                     args[8], args[9], args[10], args[11], args[12], args[13], args[14],
                     args[15]);
}

static void ui_dll_entry(dllSyscall_t engine)
{
    ui_engine = engine;
    q3_ui_dllEntry(ui_calls);
}

static const struct q3_module {
    const char *file;
    q3_door vm_main;
    q3_door dll_entry;
} MODULES[] = {
    { "qagame" ARCH_STRING DLL_EXT, q3_game_vmMain,  (q3_door)q3_game_dllEntry },
    { "cgame"  ARCH_STRING DLL_EXT, q3_cgame_vmMain, (q3_door)q3_cgame_dllEntry },
    { "ui"     ARCH_STRING DLL_EXT, q3_ui_vmMain,    (q3_door)ui_dll_entry },
};

void *Sys_LoadLibrary(const char *name)
{
    const char *base = strrchr(name, '/');

    base = base ? base + 1 : name;

    for (size_t i = 0; i < sizeof MODULES / sizeof MODULES[0]; i++) {
        if (strcmp(base, MODULES[i].file) == 0) {
            return (void *)&MODULES[i];
        }
    }

    return NULL;
}

void Sys_UnloadLibrary(void *handle)
{
    (void)handle;
}

void *Sys_LoadFunction(void *handle, const char *name)
{
    const struct q3_module *module = handle;

    if (module == NULL) {
        return NULL;
    }
    if (strcmp(name, "vmMain") == 0) {
        return (void *)module->vm_main;
    }
    if (strcmp(name, "dllEntry") == 0) {
        return (void *)module->dll_entry;
    }

    return NULL;
}

int Sys_LoadFunctionErrors(void)
{
    return 0;
}

/* `files.c`'s `\video-pipe`: no program to pipe to (`kosmos_q3.h`). */
FILE *popen(const char *command, const char *mode)
{
    (void)command;
    (void)mode;
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE *stream)
{
    (void)stream;
    return -1;
}

/* `l_precomp.c`'s `__DATE__` and `__TIME__`, in `asctime`'s shape. */
char *ctime(const time_t *t)
{
    static char out[32];
    struct tm *tm = localtime(t);

    if (tm == NULL || strftime(out, sizeof out, "%a %b %d %H:%M:%S %Y\n", tm) == 0) {
        strcpy(out, "Thu Jan 01 00:00:00 1970\n");
    }

    return out;
}

/*--------------------------------------------------------------------------
 * Printing and stopping.
 *------------------------------------------------------------------------*/

/* Into the spill ring, which `q3.log` hands to Lua and Lua prints. */
void Sys_Print(const char *msg)
{
    fputs(msg, stdout);
}

void QDECL Sys_SetStatus(const char *format, ...)
{
    (void)format;
}

void Sys_ShowConsole(int level, qboolean quitOnClose)
{
    (void)level;
    (void)quitOnClose;
}

void Sys_SetErrorText(const char *text)
{
    (void)text;
}

char *Sys_ConsoleInput(void)
{
    return NULL;
}

void Sys_Init(void)
{
    Cvar_Set("arch", OS_STRING " " ARCH_STRING);
}

qboolean Sys_LowPhysicalMemory(void)
{
    return qfalse;
}

void Sys_BeginProfiling(void)
{
}

void NORETURN Sys_Quit(void)
{
    /* `Com_Quit_f` has shut the common half down already. */
    CL_Shutdown("", qtrue);
    q3_window_close();
    exit(0);
}

void NORETURN FORMAT_PRINTF(1, 2) QDECL Sys_Error(const char *format, ...)
{
    va_list ap;
    char text[1024];

    va_start(ap, format);
    Q_vsnprintf(text, sizeof text, format, ap);
    va_end(ap);

    /* What stopped it, before anything that might stop it again. */
    printf("Sys_Error: %s\n", text);

    CL_Shutdown(text, qtrue);
    q3_window_close();
    exit(1);
}

/*--------------------------------------------------------------------------
 * The engine's own stack.
 *
 * Quake 1's renderer ran off a process's 256 KB on its first frame
 * (`quake_kosmos.c`), and Quake III's botlib and renderer have frames as
 * large. So the engine runs on four megabytes of ordinary pages, entered
 * through `kosmos_call_on_stack`, with the page under them left unmapped
 * for good - an overflow faults there rather than writing into whatever
 * was mapped before.
 *------------------------------------------------------------------------*/

#define ENGINE_STACK_PAGES 1024

static void *engine_stack_top;

static int make_engine_stack(void)
{
    long base;

    if (engine_stack_top != NULL) {
        return 0;
    }

    base = kosmos_map(ENGINE_STACK_PAGES + 1);

    if (base < 0 || kosmos_unmap((unsigned long)base, 1) < 0) {
        return -1;
    }

    engine_stack_top = (void *)(uintptr_t)((unsigned long)base
                       + (ENGINE_STACK_PAGES + 1) * KOSMOS_PAGE_SIZE);

    return 0;
}

static char *command_line;

static void engine_init(void *unused)
{
    (void)unused;

    Com_Init(command_line);
}

static void engine_frame(void *unused)
{
    (void)unused;

    IN_Frame();
    Com_Frame(CL_NoDelay());
}

/*--------------------------------------------------------------------------
 * The Lua side of it.
 *------------------------------------------------------------------------*/

static int running;

/*
 * q3.start(base, home, command line) - where the game's files are, where it
 * may write, and what `quake3 +set ...` would have said.
 *
 * Returns true, or false and why, the words being in the log.
 */
static int l_start(lua_State *L)
{
    const char *base = luaL_checkstring(L, 1);
    const char *home = luaL_checkstring(L, 2);
    const char *line = luaL_optstring(L, 3, "");
    char con_title[MAX_CVAR_VALUE_STRING];
    int x, y;

    if (running) {
        return luaL_error(L, "Quake III is already running in this process");
    }

    Q_strncpyz(base_path, base, sizeof base_path);
    Q_strncpyz(home_path, home, sizeof home_path);
    Sys_Mkdir(home_path);

    if (make_engine_stack() != 0) {
        return luaL_error(L, "no room for the engine's %d KB stack",
                          ENGINE_STACK_PAGES * 4);
    }

    free(command_line);
    command_line = strdup(line);

    if (command_line == NULL) {
        return luaL_error(L, "no memory for the command line");
    }

    Com_EarlyParseCmdLine(command_line, con_title, sizeof con_title, &x, &y);

    kosmos_exit_arm();

    if (setjmp(kosmos_exit_to) != 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "Quake III stopped during startup");
        return 2;
    }

    kosmos_call_on_stack(engine_init, NULL, engine_stack_top);

    kosmos_exit_disarm();
    running = 1;

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * q3.frame() - one `Com_Frame`: true while the game runs, and false and
 * why once `exit` has landed - "quit" for the menu's quit, anything else
 * for an error, whose words are in the log.
 */
static int l_frame(lua_State *L)
{
    if (!running) {
        return luaL_error(L, "Quake III has not been started");
    }

    kosmos_exit_arm();

    switch (setjmp(kosmos_exit_to)) {
    case 0:
        break;

    case 1:
        running = 0;
        lua_pushboolean(L, 0);
        lua_pushstring(L, "quit");
        return 2;

    default:
        running = 0;
        lua_pushboolean(L, 0);
        lua_pushstring(L, "stopped with an error, above");
        return 2;
    }

    kosmos_call_on_stack(engine_frame, NULL, engine_stack_top);

    kosmos_exit_disarm();

    lua_pushboolean(L, 1);
    return 1;
}

/* q3.log() - whatever the engine printed since the last call, or nil. */
static int l_log(lua_State *L)
{
    char buf[2048];
    size_t n = kosmos_spill_drain(buf, sizeof buf);

    if (n == 0) {
        return 0;
    }

    lua_pushlstring(L, buf, n);
    return 1;
}

static const luaL_Reg q3_lib[] = {
    { "sound", q3_lua_sound },
    { "start", l_start },
    { "frame", l_frame },
    { "log",   l_log },
    { NULL, NULL },
};

/*
 * The table `quake3.lua` reaches as `use("quake3.elf")`: the image it runs
 * in, by its file, as Doom's and Quake 1's are (`sys_user.c`'s `own`).
 */
void kosmos_quake3_kit(lua_State *L)
{
    luaL_newlib(L, q3_lib);
}
