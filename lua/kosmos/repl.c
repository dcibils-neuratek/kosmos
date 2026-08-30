/*
 * The REPL.
 *
 * M2's definition of done: a `>` prompt over serial where `2+2` returns `4`.
 *
 * It is also the first thing in Kosmos that a person talks to, and from M5
 * it becomes the shell: the way the system is inspected and modified while
 * running, which `design.md` §9.1 calls the Lisp Machine property. So the
 * line editing here is not decoration, it is the beginning of that.
 *
 * What it deliberately is not: a terminal emulator. Arrow keys, history and
 * completion need to know about escape sequences, and the terminal that
 * parses those arrives at M6 alongside the app server. Until then this
 * understands backspace and nothing else, which is honest for a thing
 * talking to a raw serial line.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos_lua.h"
#include "console.h"
#include "hal.h"

/* Not LINE_MAX: newlib's <limits.h> already defines that, and GCC's own
 * limits.h reaches it through syslimits.h even under -ffreestanding. */
#define REPL_LINE_MAX   512

static char line[REPL_LINE_MAX];
static size_t length;

/*
 * Reading a line without blocking.
 *
 * `hal_getchar` returns immediately either way, and the loop that calls this
 * spends the gaps in `wfi`. That matters more than it looks: a REPL that
 * spins on the UART pins the core, and from M3 there is a scheduler that
 * would have nothing left to give anyone else.
 *
 * Returns true when a complete line is ready in `line`.
 */
static bool poll_line(void)
{
    int c;

    while ((c = hal_getchar()) != HAL_NO_INPUT) {
        if (c == '\r' || c == '\n') {
            kputc('\n');
            line[length] = '\0';
            return true;
        }

        if (c == 0x7f || c == '\b') {
            if (length > 0) {
                length--;
                /* Back up, overwrite with a space, back up again. Three
                 * characters, because a serial terminal does not erase
                 * anything just because the cursor moved over it. */
                kputs("\b \b");
            }
            continue;
        }

        /* Anything that is not printable is dropped rather than echoed. An
         * escape sequence from an arrow key would otherwise arrive as three
         * bytes of noise in the middle of the expression. */
        if (c < 0x20 || c > 0x7e) {
            continue;
        }

        if (length + 1 < sizeof(line)) {
            line[length++] = (char)c;
            kputc((char)c);
        }
    }

    return false;
}

/*
 * Prints whatever the chunk left on the stack, and clears it.
 *
 * Uses Lua's own tostring rather than lua_tostring, so a table with a
 * __tostring metamethod prints the way its author intended. That is the
 * behaviour every Lua REPL has and the one that makes an object inspectable
 * from the prompt, which is the entire point of having one.
 */
static void print_results(lua_State *L, int base)
{
    int n = lua_gettop(L) - base;
    int i;

    if (n <= 0) {
        return;
    }

    for (i = 1; i <= n; i++) {
        size_t len;
        const char *s;

        if (i > 1) {
            kputs("\t");
        }

        luaL_tolstring(L, base + i, &len);
        s = lua_tolstring(L, -1, &len);
        kosmos_lua_write(s, len);
        lua_pop(L, 1);
    }

    kputc('\n');
    lua_settop(L, base);
}

static void report_error(lua_State *L)
{
    const char *msg = lua_tostring(L, -1);

    kputs("error: ");
    kputs((msg != NULL) ? msg : "(error object is not a string)");
    kputc('\n');
    lua_pop(L, 1);
}

/*
 * Tries the line as an expression first, then as a statement.
 *
 * Typing `2+2` at a prompt is not valid Lua: it is an expression, and a
 * chunk is a sequence of statements. Every Lua REPL solves this by wrapping
 * the line in `return (...)` and falling back to the raw line if that does
 * not compile. Without it, M2's own definition of done would be a syntax
 * error.
 */
static int load_line(lua_State *L, const char *src)
{
    char wrapped[REPL_LINE_MAX + 16];
    int status;

    snprintf(wrapped, sizeof(wrapped), "return %s", src);

    /* Mode "t" and never "bt". design.md 5.3: the undump loader validates
     * almost nothing, and this is a prompt anyone can type into. */
    status = luaL_loadbufferx(L, wrapped, strlen(wrapped), "=stdin", "t");

    if (status == LUA_OK) {
        return status;
    }

    /* Not an expression. Drop the error from the attempt and try it as
     * written, so the real syntax error is the one that gets reported. */
    lua_pop(L, 1);

    return luaL_loadbufferx(L, src, strlen(src), "=stdin", "t");
}

void repl_run(lua_State *L)
{
    kputs("\nKosmos Lua REPL. There is no way out, and nothing to go back to.\n");
    kputs("> ");

    for (;;) {
        int base;
        int status;

        if (!poll_line()) {
            /*
             * Nothing typed yet. Park the core until something happens
             * rather than spinning on the UART: at 100 Hz the timer alone
             * wakes this often enough to feel instant, and it leaves the
             * machine idle instead of pinned.
             */
            __asm__ volatile("wfi");
            continue;
        }

        if (length == 0) {
            kputs("> ");
            continue;
        }

        base = lua_gettop(L);
        status = load_line(L, line);

        if (status == LUA_OK) {
            status = lua_pcall(L, 0, LUA_MULTRET, 0);
        }

        if (status == LUA_OK) {
            print_results(L, base);
        } else {
            report_error(L);
        }

        length = 0;
        kputs("> ");
    }
}
