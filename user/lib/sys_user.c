/*
 * `sys`, from inside a process.
 *
 * The only one there is. The kernel had a copy of this table once, over
 * direct calls rather than syscalls, and servers were kernel threads; both
 * are gone, and the names that survived the move are the ones that could be
 * expressed as a syscall. That the two were interchangeable for a milestone
 * is what let the servers walk out one at a time instead of all at once.
 *
 * The inspection half did not survive, and deliberately. `sys.threads` and
 * `sys.memory` read kernel state, and a process has no business reading it
 * directly: at M5 that is `/proc`, reached through the namespace like any
 * other resource, and reached only by a process that was handed it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"
#include "serialize.h"

/* The two definitions of struct message are one contract written twice, so
 * the agreement is checked rather than assumed. A silent disagreement would
 * be read as a disagreement about the contents. */
_Static_assert(sizeof(struct message) == 8 + 4 + MSG_BYTES + 4,
               "struct message must match kernel/ipc.h");

static const char *ipc_error(long status)
{
    switch (status) {
    case -1:   return "no such capability";
    case -2:   return "the endpoint was destroyed";
    case -3:   return "that thread is not waiting for a reply";
    case -4:   return "out of endpoints or capability slots";
    case -101: return "a pointer this process may not use";
    case -102: return "this process does not hold that device";
    case -104: return "there is nothing to wait for";
    case -105: return "the machine is out of memory or processes";
    case -106: return "this process holds too many capabilities";
    default:   return "unknown error";
    }
}

static int fail(lua_State *L, long status)
{
    lua_pushnil(L);
    lua_pushstring(L, ipc_error(status));
    return 2;
}

/*
 * A message carries a Lua value, and this is the only place that knows how.
 *
 * `design.md` §1: the protocol between servers *is* the data model of the
 * userland language. A caller passes a table and a server receives a table;
 * neither writes marshalling, because there is none to write.
 */
static void push_message(lua_State *L, const struct message *m)
{
    int rc = serialize_unpack(L, m);

    if (rc != SERIALIZE_OK) {
        /* Malformed bytes are an error rather than a half-built table. The
         * unpacker leaves the stack as it found it, so raising here is
         * safe. */
        luaL_error(L, "%s", serialize_error(rc));
    }
}

/*
 * A capability travels alongside the value, never inside it. See the kernel's
 * copy: an index means something only in the table it came from.
 */
static void take_message(lua_State *L, int index, struct message *m,
                         int cap_arg)
{
    int rc;
    long c = (long)luaL_optinteger(L, cap_arg, -1);

    m->tag = 0;
    m->length = 0;
    m->cap_plus_one = (c >= 0) ? (uint32_t)(c + 1) : 0u;

    /* A tag, if the value is a table with one. `design.md` §14 wants every
     * message to say what it is; the kernel only insists the field exists,
     * and what goes in it is between the two ends. */
    if (lua_istable(L, index)) {
        lua_getfield(L, index, "tag");
        m->tag = (uint64_t)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
    }

    rc = serialize_pack(L, index, m);

    if (rc != SERIALIZE_OK) {
        luaL_error(L, "%s", serialize_error(rc));
    }
}

static int l_write(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);

    lua_pushinteger(L, (lua_Integer)kosmos_write(s, len));
    return 1;
}

static int l_getchar(lua_State *L)
{
    long c = kosmos_getchar();

    if (c < 0) {
        lua_pushnil(L);         /* nothing waiting, which is not an error */
        return 1;
    }

    lua_pushinteger(L, (lua_Integer)c);
    return 1;
}

/*
 * `sys.key_event()` - the next key transition, or nil.
 *
 * Returns the board's keycode and whether it went down. Two values, because
 * they are two facts.
 *
 * The other half of `sys.getchar`, and needed because a character cannot
 * say that a key is *still* held: a stream of them is a stream of meanings,
 * and holding a direction in a game is a question about the key itself.
 */
/*
 * `sys.power("off")` or `sys.power("restart")`.
 *
 * Words rather than numbers, because these are the two things in this
 * system that cannot be undone by trying again, and `sys.power(1)` is one
 * typo away from `sys.power(0)`.
 */
/*
 * `sys.sound(pcm)` - one period of samples, as a string.
 *
 * A Lua string because that is what a Lua caller has, and because a period
 * is a kilobyte: the cost of it being a string rather than a region is one
 * copy of 1024 bytes every five milliseconds, which is not the thing that
 * will be too slow here. When something wants to hand over more than that
 * at a time - a whole track from a decoder - the answer is a region and a
 * different call, not this one made cleverer.
 *
 * Returns true when it was taken, false when the device is still busy with
 * what it has. False is the ordinary case for a caller that is ahead, and
 * is not an error.
 */
static int l_sound(lua_State *L)
{
    size_t len;
    const char *pcm = luaL_checklstring(L, 1, &len);
    long status;

    /*
     * The size is not checked here.
     *
     * A period's length is the *board's* fact - `hal.h` fixes it and the
     * kernel enforces it - and repeating the number in userland would be
     * two copies of one thing that agree until somebody changes a driver.
     * `sys.info().audio_period` is how a caller learns it; this just passes
     * the bytes down and reports what came back.
     */
    status = kosmos_snd_write(pcm, (unsigned long)len);

    if (status == SYS_ERR_DENIED) {
        return luaL_error(L, "this process may not play sound");
    }

    if (status == SYS_ERR_FAULT) {
        return luaL_error(L, "%d bytes is not one period", (int)len);
    }

    lua_pushboolean(L, status == 0);

    return 1;
}

/*
 * `sys.sound_queued()` - periods the device has not finished with.
 *
 * The deadline as a number, which `roadmap.md` M11a promises instead of a
 * bound. Zero means it has run dry and the next sound has a click in it.
 */
/*
 * `sys.mix(streams)` - sum several streams into one period and play it.
 *
 * `streams` is an array of `{ pcm = <string>, left = 0..256, right = 0..256 }`
 * and this is the only place in the system where more than one sound
 * exists at once. It returns whether the device took the result, and writes
 * a `peak` back into each entry.
 *
 *--------------------------------------------------------------------------
 * Why the mixing is here and not in Lua.
 *
 * It is a loop over samples - 512 of them per period, per stream - and
 * `gfx.md` 19.2's arithmetic applies unchanged: twenty to fifty nanoseconds
 * an iteration in Lua, against a period that has to be ready every 5.8
 * milliseconds. Four streams would be two thousand iterations a period and
 * most of the budget spent on the interpreter.
 *
 * The other reason is garbage. A Lua mixer builds a new 1 KB string per
 * period, which at 172 periods a second is 176 KB a second of allocation on
 * the one path in this system with a hard deadline - and `CLAUDE.md` is
 * explicit that what decides a server is `gc_pause_max`, not throughput.
 * This allocates nothing: the accumulator is static and the result goes
 * straight to the device.
 *
 *--------------------------------------------------------------------------
 * Gain is an integer, 256 for unity.
 *
 * Not a float, and not because floats are unavailable - this is EL0 and may
 * use them. Because `(sample * gain) >> 8` is exact, and a volume control
 * that is exact at unity is a volume control that cannot quietly attenuate
 * a stream nobody asked it to touch.
 *
 * The peak is measured *before* gain, which is the question the meter is
 * actually answering: which application is sending audio. A muted stream
 * that is still playing should show it, or the meter has become a second
 * volume display.
 */
static int l_mix(lua_State *L)
{
    static int32_t acc[HAL_SND_PERIOD_BYTES_MAX / 2];
    unsigned frames_seen = 0;
    unsigned samples = 0;
    lua_Integer n, i;
    unsigned k;

    luaL_checktype(L, 1, LUA_TTABLE);
    n = (lua_Integer)lua_rawlen(L, 1);

    memset(acc, 0, sizeof(acc));

    for (i = 1; i <= n; i++) {
        const int16_t *in;
        size_t len = 0;
        long left = 256, right = 256;
        int32_t peak = 0;

        lua_rawgeti(L, 1, i);

        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "pcm");
        in = (const int16_t *)lua_tolstring(L, -1, &len);

        lua_getfield(L, -3 + 1, "left");
        if (lua_isnumber(L, -1)) { left = (long)lua_tointeger(L, -1); }
        lua_pop(L, 1);

        lua_getfield(L, -2, "right");
        if (lua_isnumber(L, -1)) { right = (long)lua_tointeger(L, -1); }
        lua_pop(L, 1);

        if (in != NULL && len >= 4) {
            unsigned count = (unsigned)(len / 2);

            if (count > sizeof(acc) / sizeof(acc[0])) {
                count = (unsigned)(sizeof(acc) / sizeof(acc[0]));
            }

            if (count > samples) {
                samples = count;
            }

            for (k = 0; k < count; k++) {
                int32_t v = in[k];
                int32_t mag = (v < 0) ? -v : v;

                if (mag > peak) {
                    peak = mag;
                }

                /* Even samples are the left channel, odd the right: that is
                 * what interleaved stereo means and it is the only place
                 * the balance can be applied. */
                acc[k] += (v * (int32_t)((k & 1u) ? right : left)) >> 8;
            }

            frames_seen++;
        }

        lua_pop(L, 1);                  /* the pcm string */

        lua_pushinteger(L, (lua_Integer)peak);
        lua_setfield(L, -2, "peak");

        lua_pop(L, 1);                  /* the entry */
    }

    if (frames_seen == 0 || samples == 0) {
        lua_pushboolean(L, 0);

        return 1;
    }

    /*
     * Clipped rather than wrapped.
     *
     * Two streams at full scale sum past what sixteen bits hold, and the
     * difference between the two answers is the difference between a loud
     * moment and a bang: wrapping turns a peak into the opposite sign,
     * which is the worst noise a mixer can make.
     */
    {
        static int16_t out[HAL_SND_PERIOD_BYTES_MAX / 2];

        for (k = 0; k < samples; k++) {
            int32_t v = acc[k];

            if (v > 32767)  { v = 32767; }
            if (v < -32768) { v = -32768; }

            out[k] = (int16_t)v;
        }

        lua_pushboolean(L,
            kosmos_snd_write(out, (unsigned long)samples * 2) == 0);
    }

    return 1;
}

static int l_sound_queued(lua_State *L)
{
    long n = kosmos_snd_queued();

    if (n < 0) {
        return 0;
    }

    lua_pushinteger(L, (lua_Integer)n);

    return 1;
}

static int l_power(lua_State *L)
{
    const char *what = luaL_checkstring(L, 1);
    unsigned long which;

    if (strcmp(what, "off") == 0) {
        which = 0;
    } else if (strcmp(what, "restart") == 0) {
        which = 1;
    } else {
        return luaL_error(L, "power: \"off\" or \"restart\", not %s", what);
    }

    /* Only reached if the firmware refused; otherwise the machine is gone. */
    lua_pushnil(L);
    lua_pushstring(L, (kosmos_power(which) == SYS_ERR_DENIED)
                      ? "this process may not power the machine"
                      : "the firmware would not");

    return 2;
}

static int l_key_event(lua_State *L)
{
    unsigned code = 0, down = 0;
    long status = kosmos_key_event(&code, &down);

    if (status != 0) {
        return 0;
    }

    lua_pushinteger(L, (lua_Integer)code);
    lua_pushboolean(L, down != 0);

    return 2;
}

static int l_spawn(lua_State *L)
{
    unsigned long arg = (unsigned long)luaL_checkinteger(L, 1);
    unsigned long flags = (unsigned long)luaL_optinteger(L, 3, 0);
    int caps[16];
    unsigned long n = 0;
    long id;

    /* Capabilities as an array, in the order the child will see them. */
    if (!lua_isnoneornil(L, 2)) {
        lua_Integer count;

        luaL_checktype(L, 2, LUA_TTABLE);
        count = (lua_Integer)lua_rawlen(L, 2);

        if (count > 16) {
            lua_pushnil(L);
            lua_pushstring(L, "too many capabilities");
            return 2;
        }

        for (n = 0; n < (unsigned long)count; n++) {
            lua_rawgeti(L, 2, (lua_Integer)(n + 1));
            caps[n] = (int)luaL_checkinteger(L, -1);
            lua_pop(L, 1);
        }
    }

    id = kosmos_spawn(arg, caps, n, flags);

    if (id < 0) {
        lua_pushnil(L);
        lua_pushstring(L, (id == -102) ? "this process does not hold that device"
                                       : "could not spawn");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int l_wait(lua_State *L)
{
    uint64_t id = 0;
    int nonblocking = lua_toboolean(L, 1);
    long code = kosmos_wait(&id, nonblocking);

    if (code == -106) {
        lua_pushnil(L);
        lua_pushstring(L, "no child ready");
        return 2;
    }

    if (code < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no children");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)id);
    lua_pushinteger(L, (lua_Integer)code);
    return 2;
}

static int l_exit(lua_State *L)
{
    kosmos_exit((int)luaL_optinteger(L, 1, 0));
    return 0;
}

static int l_yield(lua_State *L)
{
    (void)L;
    kosmos_yield();
    return 0;
}

/*
 * How long something took, in counter ticks.
 *
 * A Lua integer is 64 bits, so the counter fits without losing a bit, and
 * the difference between two of these is the only thing anybody should use
 * it for. It is not a date: `design.md` §4.4 makes that a capability.
 */
static int l_ticks(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)kosmos_ticks());
    return 1;
}

static int l_endpoint(lua_State *L)
{
    long cap = kosmos_endpoint();

    if (cap < 0) {
        return fail(L, cap);
    }

    lua_pushinteger(L, (lua_Integer)cap);
    return 1;
}

static int l_destroy(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    long status = kosmos_endpoint_destroy(cap);

    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_call(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct message msg;
    struct message reply;
    long status;

    take_message(L, 2, &msg, 3);

    status = kosmos_call(cap, &msg, &reply);
    if (status != 0) {
        return fail(L, status);
    }

    push_message(L, &reply);
    lua_pushinteger(L, (lua_Integer)((reply.cap_plus_one == 0)
                                     ? -1 : (long)reply.cap_plus_one - 1));
    return 2;
}

static int l_receive(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    struct message msg;
    uint64_t sender = 0;
    int nonblocking = lua_toboolean(L, 2);
    long status = kosmos_receive(cap, &msg, &sender, nonblocking);

    if (status != 0) {
        return fail(L, status);
    }

    push_message(L, &msg);

    /*
     * The sender comes back as a Lua integer here rather than as light
     * userdata, because that is what the kernel handed over: a raw pointer.
     * The kernel's own copy could hide it behind userdata; across the
     * boundary there is nothing to hide it in.
     *
     * That is the leak already recorded against sys_receive: a process
     * learns where a struct thread lives. It becomes a capability index at
     * M5, and then this is an index like any other.
     */
    lua_pushinteger(L, (lua_Integer)sender);
    lua_pushinteger(L, (lua_Integer)((msg.cap_plus_one == 0)
                                     ? -1 : (long)msg.cap_plus_one - 1));
    return 3;
}

static int l_reply(lua_State *L)
{
    uint64_t sender = (uint64_t)luaL_checkinteger(L, 1);
    struct message msg;
    long status;

    take_message(L, 2, &msg, 3);

    status = kosmos_reply(sender, &msg);
    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * A value, as bytes, without sending it anywhere.
 *
 * The same serialiser a message uses, on its own. Two reasons it is exposed
 * rather than staying an internal detail of `sys.call`:
 *
 *   - a value has to be storable and not only sendable. At M8 `fs.write`
 *     with a table is this operation with a different destination, and it
 *     should not have to invent a second encoding to get there.
 *   - it is what a benchmark can hold still. The cost of the serialiser is
 *     `design.md` §1's thesis priced in ticks, and pricing it through a
 *     round trip would be measuring the IPC path instead.
 *
 * Bounded by a message, because a message is the buffer it writes into. A
 * value larger than one is refused rather than truncated.
 */
static int l_pack(lua_State *L)
{
    struct message m;
    int rc;

    luaL_checkany(L, 1);

    m.tag = 0;
    m.cap_plus_one = 0;
    m.length = 0;

    rc = serialize_pack(L, 1, &m);

    if (rc != SERIALIZE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, serialize_error(rc));
        return 2;
    }

    lua_pushlstring(L, (const char *)m.data, m.length);
    return 1;
}

static int l_unpack(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    struct message m;
    int rc;

    if (len > MSG_BYTES) {
        lua_pushnil(L);
        lua_pushstring(L, "longer than a message");
        return 2;
    }

    /*
     * Into a message rather than read in place, because serialize_unpack
     * takes one. The copy is what the caller would pay anyway if this had
     * come off a wire, and it keeps one definition of the reader.
     */
    m.tag = 0;
    m.cap_plus_one = 0;
    m.length = (uint32_t)len;
    memcpy(m.data, s, len);

    rc = serialize_unpack(L, &m);

    if (rc != SERIALIZE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, serialize_error(rc));
        return 2;
    }

    return 1;
}

/*
 * The machine, as a table.
 *
 * Straight out of the syscall with no interpretation: raw ID registers, pool
 * counts, device geometry. What any of it *means* is decided in Lua, in
 * `init.lua`'s device server, because decoding a MIDR is a table lookup and
 * tables belong up here - a processor this kernel has never heard of gets
 * described properly without the kernel changing.
 */
static int l_info(lua_State *L)
{
    struct sysinfo info;

    if (kosmos_sysinfo(&info) < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "the kernel refused to describe itself");
        return 2;
    }

    lua_createtable(L, 0, 24);

#define SET(name, value) \
    do { lua_pushinteger(L, (lua_Integer)(value)); \
         lua_setfield(L, -2, name); } while (0)

    SET("midr",             info.midr);
    SET("mpidr",            info.mpidr);
    SET("ctr",              info.ctr);
    SET("pfr0",             info.pfr0);
    SET("isar0",            info.isar0);
    SET("mmfr0",            info.mmfr0);
    SET("counter_hz",       info.counter_hz);

    SET("ram_base",         info.ram_base);
    SET("ram_size",         info.ram_size);
    SET("pages_total",      info.pages_total);
    SET("pages_free",       info.pages_free);
    SET("page_size",        info.page_size);

    SET("threads_used",     info.threads_used);
    SET("threads_total",    info.threads_total);
    SET("processes_used",   info.processes_used);
    SET("processes_held",   info.processes_held);
    SET("processes_total",  info.processes_total);
    SET("endpoints_used",   info.endpoints_used);
    SET("endpoints_total",  info.endpoints_total);
    SET("regions_used",     info.regions_used);
    SET("regions_total",    info.regions_total);
    SET("spaces_used",      info.spaces_used);
    SET("spaces_total",     info.spaces_total);

    SET("screen_width",     info.screen_width);
    SET("screen_height",    info.screen_height);
    SET("screen_pitch",     info.screen_pitch);
    SET("has_keyboard",     info.has_keyboard);

    SET("idle_ticks",       info.idle_ticks);
    SET("busy_ticks",       info.busy_ticks);
    SET("epoch",            info.epoch);

    /* What one period is, so a caller can size its buffer without carrying
     * a copy of a number the board owns. Zero when there is no device. */
    SET("audio_rate",       info.audio_rate);
    SET("audio_channels",   info.audio_channels);
    SET("audio_period",     info.audio_period);
    SET("audio_periods",    info.audio_periods);
    SET("cpus",             info.cpus);
    SET("tick_hz",          info.tick_hz);
    SET("current_el",       info.current_el);

#undef SET

    return 1;
}

static int l_setname(lua_State *L)
{
    size_t len;
    const char *name = luaL_checklstring(L, 1, &len);

    lua_pushboolean(L, kosmos_setname(name, len) == 0);
    return 1;
}

/*
 * Every process, as a list of tables.
 *
 * Raw again: an id, a name the process chose, a state number, ticks that
 * only rise. What a name means and which layer it belongs to is decided by
 * whoever asked - the kernel has no opinion about whether something is a
 * server or an app, and should not acquire one.
 */
static int l_processes(lua_State *L)
{
    struct proc_info table[32];
    long n = kosmos_proctable(table, 32);
    long i;

    if (n < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "the kernel would not say");
        return 2;
    }

    lua_createtable(L, (int)n, 0);

    for (i = 0; i < n; i++) {
        lua_createtable(L, 0, 9);

#define SETI(k, v) do { lua_pushinteger(L, (lua_Integer)(v)); \
                        lua_setfield(L, -2, k); } while (0)
        SETI("id",        table[i].id);
        SETI("state",     table[i].state);
        SETI("exit_code", table[i].exit_code);
        SETI("ticks",     table[i].ticks);
        SETI("pages",     table[i].pages);
        SETI("held",      table[i].held);
        SETI("caps",      table[i].caps);
        SETI("owns",      table[i].owns);
        SETI("priority",  table[i].priority);
#undef SETI

        lua_pushboolean(L, table[i].exited != 0);
        lua_setfield(L, -2, "exited");

        lua_pushstring(L, table[i].name);
        lua_setfield(L, -2, "name");

        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }

    return 1;
}

/*
 * The programs carried in this image, as Lua source for a chunk that
 * returns a table of name to source.
 *
 * **Not a syscall.** It is data in the image, reached through this table
 * because that is where a process looks for things it did not bring with
 * it. Only the /bin server calls it, which is deliberate: the string is a
 * few kilobytes and every state that asked for it would keep a copy.
 *
 * There is no disk. Until M8 this is where a program lives.
 */
struct kosmos_asset {
    const char          *name;
    const unsigned char *bytes;
    size_t               length;
};

extern const struct kosmos_asset assets_table[];

extern const char kosmos_name[];
extern const char kernel_name[];
extern const char kosmos_version[];
extern const char kosmos_build[];
extern const char kosmos_date[];
extern const char kosmos_platform[];

extern const char programs_lua[];
extern const char libraries_lua[];

/*
 * `sys.asset("name.png")` - a file carried inside the image.
 *
 * Only small ones. This was going to be how every picture arrived and it is
 * not: a photograph compiled into the kernel is one the page tables have to
 * be arranged around, and this system found that out by panicking in
 * `mmu_init` when a megabyte of JPEG-sized PNG showed up and pushed the
 * stack guards out of the page-mapped region.
 *
 * What is left is the test pattern, fifteen hundred bytes, so the decoder
 * has something to decode before there is a filesystem to read from.
 */
static int l_asset(lua_State *L)
{
    const char *want = luaL_optstring(L, 1, NULL);
    int i;

    if (want == NULL) {
        lua_newtable(L);

        for (i = 0; assets_table[i].name != NULL; i++) {
            lua_pushstring(L, assets_table[i].name);
            lua_rawseti(L, -2, i + 1);
        }

        return 1;
    }

    for (i = 0; assets_table[i].name != NULL; i++) {
        if (strcmp(assets_table[i].name, want) == 0) {
            lua_pushlstring(L, (const char *)assets_table[i].bytes,
                            assets_table[i].length);
            return 1;
        }
    }

    lua_pushnil(L);
    lua_pushfstring(L, "no asset called %s", want);
    return 2;
}

/*
 * `sys.log([bytes])` - what this machine has printed, most recent last.
 *
 * The kernel keeps a ring of everything that went through `kputc`, which is
 * its own output *and* every process's, because a process prints by asking
 * the console server and the console server calls `sys.write`. One place,
 * in order, which is what the serial line has and the screen does not.
 */
/*
 * `sys.disk()` - what block device there is, if any.
 * `sys.disk_read(sector, bytes)` - those bytes, as a string.
 * `sys.disk_write(sector, string)` - it, at that sector.
 *
 * Bytes as a Lua string rather than a surface or a userdata, because a
 * filesystem block is kilobytes and its contents are structure, not pixels
 * - `string.unpack` is exactly the right tool for reading a superblock and
 * is already here. The rule in `gfx.md` is about pixel *loops*; there is no
 * loop here, and a 4 KB string on a 2 MB heap is nothing.
 *
 * A large *file* is a different question and gets a different answer - a
 * mapped region, see design.md 8.4 - because that is megabytes.
 */
/* One filesystem block per call, which is what the kernel's bounce buffer
 * holds. Named here so the two sides cannot drift apart silently. */
#define DISK_MAX_READ  4096

static int l_disk(lua_State *L)
{
    struct diskinfo info;

    if (kosmos_disk_info(&info) != 0 || info.sectors == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "there is no disk");
        return 2;
    }

    lua_newtable(L);

    lua_pushinteger(L, (lua_Integer)info.sectors);
    lua_setfield(L, -2, "sectors");
    lua_pushinteger(L, (lua_Integer)info.sector_size);
    lua_setfield(L, -2, "sector_size");
    lua_pushinteger(L, (lua_Integer)(info.sectors * info.sector_size));
    lua_setfield(L, -2, "bytes");

    return 1;
}

static int l_disk_read(lua_State *L)
{
    lua_Integer sector = luaL_checkinteger(L, 1);
    lua_Integer bytes  = luaL_checkinteger(L, 2);
    luaL_Buffer b;
    char *out;
    long got;

    if (bytes <= 0 || bytes > DISK_MAX_READ) {
        return fail(L, SYS_ERR_FAULT);
    }

    out = luaL_buffinitsize(L, &b, (size_t)bytes);
    got = kosmos_disk_read((unsigned long)sector, out, (unsigned long)bytes);

    if (got < 0) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        return fail(L, got);
    }

    luaL_pushresultsize(&b, (size_t)got);
    return 1;
}

static int l_disk_write(lua_State *L)
{
    lua_Integer sector = luaL_checkinteger(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    long wrote;

    if (len == 0 || len > DISK_MAX_READ) {
        return fail(L, SYS_ERR_FAULT);
    }

    wrote = kosmos_disk_write((unsigned long)sector, data,
                              (unsigned long)len);

    if (wrote < 0) {
        return fail(L, wrote);
    }

    lua_pushinteger(L, wrote);
    return 1;
}

/*
 * `sys.memory(pages)` - a region two processes can share.
 * `sys.memory_map(cap)` - it, in this process's address space.
 * `sys.memory_size(cap)` - how many pages it is.
 *
 * The capability is an ordinary one: `sys.call(ep, msg, cap)` sends it and
 * the far side receives its own index for the same pages. What it maps to
 * is an address, which is deliberately not something Lua can do anything
 * with - `gfx.wrap` turns it into a surface, and every pixel that touches
 * it does so from C.
 */
static int l_memory(lua_State *L)
{
    long cap = kosmos_mem_create((unsigned long)luaL_checkinteger(L, 1));

    if (cap < 0) {
        return fail(L, cap);
    }

    lua_pushinteger(L, (lua_Integer)cap);
    return 1;
}

static int l_memory_map(lua_State *L)
{
    long at = kosmos_mem_map((long)luaL_checkinteger(L, 1));

    if (at < 0) {
        return fail(L, at);
    }

    lua_pushinteger(L, (lua_Integer)at);
    return 1;
}

/*
 * Moving bytes in and out of a shared region.
 *
 * This is the buffer half of `read(fd, buf, n)`, and it exists because a
 * server cannot do what a Unix kernel does. `read` on Unix copies into the
 * caller's buffer because the kernel can reach into the caller's address
 * space. A server here is another process at EL0 and cannot, so the buffer
 * is shared pages both sides hold a capability to, and these two functions
 * are how bytes cross between such a region and a Lua string.
 *
 * The mapping is remembered rather than repeated. `sys.memory_map` bumps
 * the process's share window every time it is called - it does not hand
 * back the same address twice - so mapping a region on each access would
 * walk through four gigabytes of address space and then stop working.
 * Mapped once here, on first use, and kept.
 */
#define MAPPED_MAX 256

static struct {
    long      cap;
    uintptr_t at;
    size_t    bytes;
} mapped[MAPPED_MAX];

static unsigned mapped_count;

static bool region_of(long cap, uintptr_t *at, size_t *bytes)
{
    unsigned i;
    long pages;
    long address;

    for (i = 0; i < mapped_count; i++) {
        if (mapped[i].cap == cap) {
            *at = mapped[i].at;
            *bytes = mapped[i].bytes;
            return true;
        }
    }

    if (mapped_count == MAPPED_MAX) {
        return false;
    }

    pages = kosmos_mem_size(cap);

    if (pages <= 0) {
        return false;
    }

    address = kosmos_mem_map(cap);

    if (address < 0) {
        return false;
    }

    mapped[mapped_count].cap = cap;
    mapped[mapped_count].at = (uintptr_t)address;
    mapped[mapped_count].bytes = (size_t)pages * 4096u;

    *at = mapped[mapped_count].at;
    *bytes = mapped[mapped_count].bytes;
    mapped_count++;

    return true;
}

/*
 * `sys.release(cap)` - a capability this process holds, given back.
 *
 * A server that is handed a buffer calls this when it has filled it. Not
 * doing so is not a leak that grows slowly: the table is sixteen deep, so
 * it is sixteen requests and then nothing works.
 */
/*
 * `sys.scheduler()` - how the machine is scheduled right now.
 *
 * Returns a table: which policy is installed, what else there is, the
 * quantum in ticks and in milliseconds, and how many priority bands exist.
 * The millisecond figure is computed here from the tick rate the kernel
 * reports rather than assumed, because the only way to get a quantum below
 * ten milliseconds is to change that rate.
 */
static int l_scheduler(lua_State *L)
{
    struct schedinfo info;
    long status = kosmos_sched_info(&info);
    unsigned i;

    if (status != 0) {
        return fail(L, status);
    }

    lua_newtable(L);

    lua_pushinteger(L, (lua_Integer)info.policy + 1);   /* Lua counts from 1 */
    lua_setfield(L, -2, "policy");

    lua_pushinteger(L, (lua_Integer)info.quantum);
    lua_setfield(L, -2, "quantum");

    lua_pushnumber(L, (lua_Number)info.quantum * 1000.0 / (lua_Number)info.tick_hz);
    lua_setfield(L, -2, "quantum_ms");

    lua_pushinteger(L, (lua_Integer)info.tick_hz);
    lua_setfield(L, -2, "tick_hz");

    lua_pushinteger(L, (lua_Integer)info.priorities);
    lua_setfield(L, -2, "bands");

    lua_newtable(L);

    for (i = 0; i < info.policies && i < SCHED_POLICY_MAX; i++) {
        lua_pushstring(L, info.name[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    lua_setfield(L, -2, "policies");

    return 1;
}

/* `sys.set_quantum(ticks)` - how long a turn lasts. */
static int l_set_quantum(lua_State *L)
{
    long ticks = (long)luaL_checkinteger(L, 1);
    long status = kosmos_sched_set(SCHED_SET_QUANTUM, ticks);

    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * `sys.set_policy(n)` - which scheduler runs the machine, changed underneath
 * it. The runnable threads are moved across rather than dropped; see
 * `sched_switch_to`.
 */
static int l_set_policy(lua_State *L)
{
    long which = (long)luaL_checkinteger(L, 1) - 1;   /* Lua counts from 1 */
    long status = kosmos_sched_set(SCHED_SET_POLICY, which);

    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_release(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    long status;
    unsigned i;

    /*
     * The mapping goes first, and forgetting it is not optional.
     *
     * `region_of` caches by capability *index*, which was safe for exactly
     * as long as an index was never reused - which was until this function
     * existed. The moment a server gives a slot back, the next region to
     * arrive takes that number, and a cache that still holds the old
     * address hands the caller a different region entirely. The server then
     * writes its data into somebody else's pages, reads it back, and
     * reports success; the process that actually owned the buffer sees
     * zeroes. That cost an evening.
     *
     * Unmapped before the capability is dropped, because after the drop
     * this process may have no right to name those pages at all - and
     * because dropping first would leave a window where the pages could be
     * freed underneath a mapping this process still has.
     */
    for (i = 0; i < mapped_count; i++) {
        if (mapped[i].cap == cap) {
            kosmos_share_unmap(mapped[i].at, mapped[i].bytes / 4096u);

            mapped[i] = mapped[mapped_count - 1];
            mapped_count--;
            break;
        }
    }

    status = kosmos_cap_drop(cap);

    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* `sys.region_write(cap, offset, data)` - bytes into the region. */
static int l_region_write(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    lua_Integer offset = luaL_checkinteger(L, 2);
    size_t len;
    const char *data = luaL_checklstring(L, 3, &len);
    uintptr_t at;
    size_t bytes;

    if (!region_of(cap, &at, &bytes)) {
        lua_pushnil(L);
        lua_pushstring(L, "that is not a region this process can map");
        return 2;
    }

    /* Bounds first, and against the region's real size rather than against
     * what the caller believes it is. This is the one place a mistake
     * writes over memory another process is reading. */
    if (offset < 0 || (size_t)offset > bytes || len > bytes - (size_t)offset) {
        lua_pushnil(L);
        lua_pushstring(L, "that would write past the end of the region");
        return 2;
    }

    memcpy((void *)(at + (uintptr_t)offset), data, len);

    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}

/* `sys.region_read(cap, offset, bytes)` - and back out again. */
static int l_region_read(lua_State *L)
{
    long cap = (long)luaL_checkinteger(L, 1);
    lua_Integer offset = luaL_checkinteger(L, 2);
    lua_Integer want = luaL_checkinteger(L, 3);
    uintptr_t at;
    size_t bytes;

    if (!region_of(cap, &at, &bytes)) {
        lua_pushnil(L);
        lua_pushstring(L, "that is not a region this process can map");
        return 2;
    }

    if (offset < 0 || want < 0 || (size_t)offset > bytes
        || (size_t)want > bytes - (size_t)offset) {
        lua_pushnil(L);
        lua_pushstring(L, "that would read past the end of the region");
        return 2;
    }

    lua_pushlstring(L, (const char *)(at + (uintptr_t)offset), (size_t)want);
    return 1;
}

static int l_memory_size(lua_State *L)
{
    long pages = kosmos_mem_size((long)luaL_checkinteger(L, 1));

    if (pages < 0) {
        return fail(L, pages);
    }

    lua_pushinteger(L, (lua_Integer)pages);
    return 1;
}

/*
 * `sys.boot(name)` - a string the machine was started with, or nil.
 *
 *   qemu ... -fw_cfg name=opt/kosmos/boot,string=wm
 *   sys.boot("opt/kosmos/boot")   -->  "wm"
 *
 * How a machine is told what to do without being rebuilt.
 */
static int l_boot_option(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    char value[128];
    long n = kosmos_boot_option(name, value, sizeof(value));

    if (n <= 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, value, (size_t)n);
    return 1;
}

static int l_log(lua_State *L)
{
    luaL_Buffer b;
    long want = (long)luaL_optinteger(L, 1, 8192);
    char *space;
    long n;

    if (want > 16384) { want = 16384; }
    if (want < 0)     { want = 0; }

    /*
     * The Lua heap, not a static buffer.
     *
     * A `static char[8192]` here lives in the image, and the image is not
     * writable as far as the kernel is concerned - `process_may_write`
     * refuses it - so the syscall returned a fault, `sys.log` raised, and
     * the application died leaving its window behind, which the compositor
     * owns and kept drawing. A window with nothing in it and no error
     * anywhere.
     *
     * `luaL_Buffer` puts it on the heap, which is writable and which the
     * collector already accounts for.
     */
    space = luaL_buffinitsize(L, &b, (size_t)want);
    n = kosmos_log(space, (unsigned long)want);

    if (n < 0) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        return fail(L, n);
    }

    luaL_pushresultsize(&b, (size_t)n);
    return 1;
}

static int l_build(lua_State *L)
{
    lua_createtable(L, 0, 5);

#define PUTS(k, v) do { lua_pushstring(L, (v)); \
                        lua_setfield(L, -2, (k)); } while (0)

    PUTS("name", kosmos_name);

    /* The kernel's own name, separately. Kosmos is the operating system -
     * servers, a desktop, a userland - and Nebula is the microkernel under
     * it. They were one name for a long time, which made "the kernel does
     * not know what a file is" harder to say than it needed to be. */
    PUTS("kernel", kernel_name);
    PUTS("version", kosmos_version);
    PUTS("build", kosmos_build);
    PUTS("date", kosmos_date);
    PUTS("platform", kosmos_platform);

#undef PUTS

    return 1;
}

/*
 * `sys.wait_input(ticks)` - scheduler ticks, not the counter.
 *
 * The one place in this binding where the unit is not the one `sys.ticks()`
 * hands out, because it is the only clock that can interrupt a sleep.
 */
static int l_wait_input(lua_State *L)
{
    long status = kosmos_wait_input((unsigned long)luaL_checkinteger(L, 1));

    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_kill(lua_State *L)
{
    long status = kosmos_kill((unsigned long)luaL_checkinteger(L, 1));

    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * `sys.screen()` - is there a screen, and how big.
 *
 * The counterpart of `sys.disk()`, and it exists for the same reason: init
 * has to decide whether to ask for a grant, and asking for one the machine
 * cannot give is refused - which killed the whole boot the first time it
 * happened with the disk. A machine with no display is a supported way to
 * run, so this is how a caller finds out before asking.
 *
 * It answers about *this* process, because that is the only thing the
 * kernel will tell anyone: the screen is held, not observed.
 */
/*
 * `sys.fnv1a(bytes, [seed])` - a checksum over a string, in C.
 *
 * Here for one reason, and the number is the reason. The filesystem's
 * journal checksums every block of a transaction, and written in Lua that
 * loop cost more than everything else the journal does put together:
 * creating a file went from 21 to 14 a second when the journal landed, and
 * 20.5 of those 21 came back the moment the checksum was stubbed out. The
 * double write a journal exists to do costs about two percent. Hashing
 * four kilobytes a byte at a time through the interpreter cost thirty.
 *
 * This is the rule in CLAUDE.md working exactly as written: a loop over
 * bytes goes to C, *after* a measurement says so and not before. It is the
 * same argument as the pixel loop, one layer down - Lua decides what to
 * checksum and when, and the walk over the bytes happens here.
 *
 * FNV-1a, 32-bit. Not cryptographic and not trying to be: what it has to
 * catch is half a block arriving, not somebody forging one.
 */
static int l_fnv1a(lua_State *L)
{
    size_t len;
    const char *bytes = luaL_checklstring(L, 1, &len);
    uint32_t h = (uint32_t)luaL_optinteger(L, 2, 0x811c9dc5u);
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= (uint32_t)(unsigned char)bytes[i];
        h *= 16777619u;
    }

    lua_pushinteger(L, (lua_Integer)h);
    return 1;
}

static int l_screen_info(lua_State *L)
{
    struct screen_info info;

    if (kosmos_screen(&info) < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "there is no screen");
        return 2;
    }

    lua_newtable(L);

    lua_pushinteger(L, (lua_Integer)info.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, (lua_Integer)info.height);
    lua_setfield(L, -2, "height");

    return 1;
}

static int l_screen_take(lua_State *L)
{
    long status = kosmos_screen_take(lua_toboolean(L, 1));

    if (status != 0) {
        return fail(L, status);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_pointer(lua_State *L)
{
    struct pointer_info info;
    long status = kosmos_pointer(&info);

    if (status != 0) {
        return fail(L, status);
    }

#define PUT(name, value) do {                       \
        lua_pushinteger(L, (lua_Integer)(value));   \
        lua_setfield(L, -2, (name));                \
    } while (0)

    lua_createtable(L, 0, 8);
    PUT("x", info.x);
    PUT("y", info.y);
    PUT("min_x", info.min_x);
    PUT("max_x", info.max_x);
    PUT("min_y", info.min_y);
    PUT("max_y", info.max_y);
    PUT("buttons", info.buttons);

#undef PUT

    lua_pushboolean(L, info.moved != 0);
    lua_setfield(L, -2, "moved");

    return 1;
}

static int l_programs(lua_State *L)
{
    lua_pushstring(L, programs_lua);
    return 1;
}

static int l_libraries(lua_State *L)
{
    lua_pushstring(L, libraries_lua);
    return 1;
}

/*
 * The kits, each in its own file, each building its own table.
 *
 * `sys.kit(name)` is the door and it is deliberately dull: `use` turns
 * `/kits/pdf` into a call to it, so a kit is reached the way a library is,
 * through the namespace, and nothing has to know which of the two it got.
 */
void kosmos_compress_kit(lua_State *L);
void kosmos_pdf_kit(lua_State *L);

static const struct {
    const char *name;
    void      (*build)(lua_State *L);
} kits[] = {
    { "compress", kosmos_compress_kit },
    { "pdf",      kosmos_pdf_kit },
    { NULL, NULL }
};

static int l_kit(lua_State *L)
{
    const char *want = luaL_checkstring(L, 1);
    unsigned i;

    for (i = 0; kits[i].name != NULL; i++) {
        if (strcmp(kits[i].name, want) == 0) {
            kits[i].build(L);
            return 1;
        }
    }

    lua_pushnil(L);
    lua_pushfstring(L, "there is no kit called %s", want);
    return 2;
}

/* Every kit's name, so `kits` can list what a machine has. */
static int l_kit_names(lua_State *L)
{
    unsigned i;

    lua_newtable(L);

    for (i = 0; kits[i].name != NULL; i++) {
        lua_pushstring(L, kits[i].name);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    return 1;
}

static const luaL_Reg sys_functions[] = {
    { "write",    l_write },
    { "key_event",  l_key_event },
    { "power",       l_power },
    { "sound",       l_sound },
    { "sound_queued", l_sound_queued },
    { "mix",         l_mix },
    { "getchar",  l_getchar },
    { "spawn",    l_spawn },
    { "wait",     l_wait },
    { "exit",     l_exit },
    { "yield",    l_yield },
    { "ticks",    l_ticks },
    { "info",     l_info },
    { "name",     l_setname },
    { "processes", l_processes },
    { "pointer",  l_pointer },
    { "asset",    l_asset },
    { "memory",      l_memory },
    { "memory_map",  l_memory_map },
    { "memory_size", l_memory_size },
    { "region_write", l_region_write },
    { "region_read",  l_region_read },
    { "disk",        l_disk },
    { "disk_read",   l_disk_read },
    { "disk_write",  l_disk_write },
    { "boot",     l_boot_option },
    { "log",      l_log },
    { "build",    l_build },
    { "wait_input", l_wait_input },
    { "kill",     l_kill },
    { "fnv1a",       l_fnv1a },
    { "screen",      l_screen_info },
    { "screen_take", l_screen_take },
    { "programs", l_programs },
    { "libraries", l_libraries },
    { "endpoint", l_endpoint },
    { "destroy",  l_destroy },
    { "release",  l_release },
    { "scheduler",   l_scheduler },
    { "set_quantum", l_set_quantum },
    { "set_policy",  l_set_policy },
    { "kit",       l_kit },
    { "kit_names", l_kit_names },
    { "call",     l_call },
    { "receive",  l_receive },
    { "reply",    l_reply },
    { "pack",     l_pack },
    { "unpack",   l_unpack },
    { NULL, NULL }
};

int luaopen_sys(lua_State *L)
{
    luaL_newlib(L, sys_functions);
    return 1;
}
