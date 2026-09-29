/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Mac's Lua - `build/host/lua` - with the filesystem's C core in it
 * (`docs/diskfs.md`). `build/host/lua script [args]` runs a script as
 * upstream's `lua.c` runs one, and `require "kfsc"` answers
 * `user/servers/kfs.c` dressed as `user/lib/kfs.lua`: the same functions,
 * taking and answering the same tables, so a script written against one runs
 * against the other. `tools/kfs.lua` makes every disk the machine is given
 * with it, and `tools/test_kfs.lua` asks it its 87 questions with
 * `KFS_IMPL=c`.
 *
 * **A script and its arguments, and nothing else.** No prompt, no `-e`, no
 * reading a script from its input: nothing here ever asked upstream's for
 * those, and a host Lua that is also the filesystem is one binary rather
 * than two to keep straight. It was two in step 1 - this, as `kfs-lua`,
 * beside upstream's - and every script that runs `tools/kfs.lua` would have
 * had to be told which.
 *
 * **The disk is the script's `sys`**, as it is `kfs.lua`'s: every block the
 * core reads or writes goes through `sys.disk_read` and `sys.disk_write`,
 * looked up at each call, so a test's stand-ins - and the block cache
 * wrapped around them - see exactly what they see from the Lua.
 *
 * **Regions are where this has to translate.** The core reads a file into
 * memory and writes one from memory; a region, on the machine, is memory
 * mapped into the disk server, and the whole blocks of a read go straight
 * into it. A test's region is a Lua string behind `sys.region_read` and
 * `sys.region_write`. So a read into a region is made into a buffer here,
 * and every call the core makes into that buffer is sent to
 * `sys.disk_read_into` for the same place in the region - which is what the
 * test counts - and read back, so the buffer holds what the region does;
 * the parts the core cut from a block of its own are written to the region
 * after. A write from a region is the same the other way.
 *
 * **Not the machine's.** Nothing here runs on Kosmos: the disk server calls
 * `kfs.c` itself (step 3). This is how the Mac asks it questions.
 */

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/servers/diskcache.h"
#include "../user/servers/kfs.h"
#include "../user/servers/packflat.h"
#include "serialize.h"

static struct kfs *K;

/* Where the module's own table is kept, so `mkfs` can read its `LAYOUT`. */
#define KFSC_MODULE "kosmos.kfsc"

/* What the disk callbacks need, set by whichever function called the core. */
static struct {
    lua_State *L;
    char why[200];

    /* A read into a region: this buffer is the region from `at`. */
    uint8_t *into;
    size_t into_len;
    lua_Integer into_cap, into_at;

    /* A write from a region: these bytes are the region from `at`. */
    const uint8_t *from;
    size_t from_len;
    lua_Integer from_cap, from_at;

    lua_Integer most;           /* blocks a call, once `sys.disk` says */
} H;

/*
 * `sys.name(...)` with `n` arguments already pushed, protected. True with
 * its two answers on the stack; false with why in `H.why`.
 */
static int call_sys(lua_State *L, const char *name, int n)
{
    lua_getglobal(L, "sys");

    if (!lua_istable(L, -1)) {
        lua_pop(L, 1 + n);
        snprintf(H.why, sizeof H.why, "no sys");
        return 0;
    }

    lua_getfield(L, -1, name);
    lua_remove(L, -2);
    lua_insert(L, -1 - n);

    if (lua_pcall(L, n, 2, 0) != LUA_OK) {
        snprintf(H.why, sizeof H.why, "sys.%s: %s", name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }

    if (lua_isnil(L, -2)) {
        snprintf(H.why, sizeof H.why, "%s", lua_isstring(L, -1)
                 ? lua_tostring(L, -1) : "the disk refused");
        lua_pop(L, 2);
        return 0;
    }

    return 1;
}

static int host_read(void *ctx, uint32_t block, uint32_t count, void *to)
{
    lua_State *L = H.L;
    lua_Integer sector = (lua_Integer)block * KFS_PER_BLOCK;
    size_t bytes = (size_t)count * KFS_BLOCK;
    uint8_t *place = to;

    (void)ctx;

    if (H.into != NULL && place >= H.into && place + bytes <= H.into + H.into_len) {
        lua_Integer at = H.into_at + (lua_Integer)(place - H.into);

        lua_pushinteger(L, sector);
        lua_pushinteger(L, (lua_Integer)bytes);
        lua_pushinteger(L, H.into_cap);
        lua_pushinteger(L, at);

        if (!call_sys(L, "disk_read_into", 4)) {
            return -1;
        }

        if (lua_tointeger(L, -2) != (lua_Integer)bytes) {
            lua_pop(L, 2);
            snprintf(H.why, sizeof H.why, "a short read into a region");
            return -1;
        }

        lua_pop(L, 2);
        lua_pushinteger(L, H.into_cap);
        lua_pushinteger(L, at);
        lua_pushinteger(L, (lua_Integer)bytes);

        if (!call_sys(L, "region_read", 3)) {
            return -1;
        }
    } else {
        lua_pushinteger(L, sector);
        lua_pushinteger(L, (lua_Integer)bytes);

        if (!call_sys(L, "disk_read", 2)) {
            return -1;
        }
    }

    {
        size_t got;
        const char *s = lua_tolstring(L, -2, &got);

        if (s == NULL || got != bytes) {
            lua_pop(L, 2);
            snprintf(H.why, sizeof H.why, "a short read");
            return -1;
        }

        memcpy(place, s, bytes);
    }

    lua_pop(L, 2);
    return 0;
}

static int host_write(void *ctx, uint32_t block, uint32_t count, const void *from)
{
    lua_State *L = H.L;
    lua_Integer sector = (lua_Integer)block * KFS_PER_BLOCK;
    size_t bytes = (size_t)count * KFS_BLOCK;
    const uint8_t *bytes_at = from;

    (void)ctx;

    if (H.from != NULL && bytes_at >= H.from && bytes_at + bytes <= H.from + H.from_len) {
        lua_pushinteger(L, sector);
        lua_pushinteger(L, H.from_cap);
        lua_pushinteger(L, H.from_at + (lua_Integer)(bytes_at - H.from));
        lua_pushinteger(L, (lua_Integer)bytes);

        if (!call_sys(L, "disk_write_from", 4)) {
            return -1;
        }
    } else {
        lua_pushinteger(L, sector);
        lua_pushlstring(L, from, bytes);

        if (!call_sys(L, "disk_write", 2)) {
            return -1;
        }
    }

    lua_pop(L, 2);
    return 0;
}

/*
 * The most one disk call moves: `kfs.lua`'s `blocks_a_call`, asked of
 * `sys.disk()` once it answers, and a block at a time where there is none.
 */
static void enter(lua_State *L)
{
    H.L = L;
    H.why[0] = 0;

    if (H.most == 0) {
        lua_getglobal(L, "sys");

        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "disk");

            if (!lua_isfunction(L, -1)) {
                H.most = 1;
            } else if (lua_pcall(L, 0, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                lua_getfield(L, -1, "most");
                H.most = lua_tointeger(L, -1) / KFS_BLOCK;

                if (H.most < 1) {
                    H.most = 1;
                }

                lua_pop(L, 1);
            }

            lua_pop(L, 1);
        }

        lua_pop(L, 1);
    }

    K->disk.most = H.most > 0 ? (uint32_t)H.most : 1;
}

/* A status as `kfs.lua` answers one: true, or nil and why. */
static int answer(lua_State *L, int status)
{
    if (status == KFS_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }

    lua_pushnil(L);

    if (status == KFS_E_DISK && H.why[0] != 0) {
        lua_pushstring(L, H.why);
    } else {
        lua_pushstring(L, kfs_why(status));
    }

    return 2;
}

static uint64_t field_u64(lua_State *L, int idx, const char *name)
{
    uint64_t v;

    lua_getfield(L, idx, name);
    v = (uint64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
}

static void get_sb(lua_State *L, int idx, struct kfs_super *sb)
{
    luaL_checktype(L, idx, LUA_TTABLE);
    sb->magic = (uint32_t)field_u64(L, idx, "magic");
    sb->version = (uint32_t)field_u64(L, idx, "version");
    sb->block_size = (uint32_t)field_u64(L, idx, "block_size");
    sb->blocks = (uint32_t)field_u64(L, idx, "blocks");
    sb->bitmap_at = (uint32_t)field_u64(L, idx, "bitmap_at");
    sb->bitmap_blocks = (uint32_t)field_u64(L, idx, "bitmap_blocks");
    sb->inodes_at = (uint32_t)field_u64(L, idx, "inodes_at");
    sb->inode_count = (uint32_t)field_u64(L, idx, "inode_count");
    sb->journal_at = (uint32_t)field_u64(L, idx, "journal_at");
    sb->data_at = (uint32_t)field_u64(L, idx, "data_at");
    sb->created = field_u64(L, idx, "created");
}

static void set_int(lua_State *L, const char *name, uint64_t v)
{
    lua_pushinteger(L, (lua_Integer)v);
    lua_setfield(L, -2, name);
}

static void push_sb(lua_State *L, const struct kfs_super *sb)
{
    lua_createtable(L, 0, 11);
    set_int(L, "magic", sb->magic);
    set_int(L, "version", sb->version);
    set_int(L, "block_size", sb->block_size);
    set_int(L, "blocks", sb->blocks);
    set_int(L, "bitmap_at", sb->bitmap_at);
    set_int(L, "bitmap_blocks", sb->bitmap_blocks);
    set_int(L, "inodes_at", sb->inodes_at);
    set_int(L, "inode_count", sb->inode_count);
    set_int(L, "journal_at", sb->journal_at);
    set_int(L, "data_at", sb->data_at);
    set_int(L, "created", sb->created);
}

static void get_node(lua_State *L, int idx, struct kfs_inode *node)
{
    lua_Integer n;

    luaL_checktype(L, idx, LUA_TTABLE);
    memset(node, 0, sizeof *node);
    node->kind = (uint32_t)field_u64(L, idx, "kind");
    node->links = (uint32_t)field_u64(L, idx, "links");
    node->size = field_u64(L, idx, "size");
    node->mtime = field_u64(L, idx, "mtime");
    node->attrs = (uint32_t)field_u64(L, idx, "attrs");

    lua_getfield(L, idx, "extents");

    if (lua_istable(L, -1)) {
        n = luaL_len(L, -1);
        luaL_argcheck(L, n <= KFS_EXTENTS, idx, "more extents than fit in an inode");

        for (lua_Integer i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            node->extent[i - 1].start = (uint32_t)field_u64(L, -1, "start");
            node->extent[i - 1].count = (uint32_t)field_u64(L, -1, "count");
            lua_pop(L, 1);
        }

        node->extents = (uint32_t)n;
    }

    lua_pop(L, 1);
}

/* A node's fields, into the table at the top of the stack. */
static void fill_node(lua_State *L, const struct kfs_inode *node)
{
    set_int(L, "kind", node->kind);
    set_int(L, "links", node->links);
    set_int(L, "size", node->size);
    set_int(L, "mtime", node->mtime);
    set_int(L, "attrs", node->attrs);
    lua_createtable(L, (int)node->extents, 0);

    for (uint32_t i = 0; i < node->extents; i++) {
        lua_createtable(L, 0, 2);
        set_int(L, "start", node->extent[i].start);
        set_int(L, "count", node->extent[i].count);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    lua_setfield(L, -2, "extents");
}

static void push_node(lua_State *L, const struct kfs_inode *node)
{
    lua_createtable(L, 0, 6);
    fill_node(L, node);
}

/* The table at `idx` made to say what `node` says, as `kfs.lua` changes it. */
static void update_node(lua_State *L, int idx, const struct kfs_inode *node)
{
    lua_pushvalue(L, idx);
    fill_node(L, node);
    lua_pop(L, 1);
}

static uint64_t opt_time(lua_State *L, int idx)
{
    return lua_isnoneornil(L, idx) ? KFS_NO_TIME : (uint64_t)luaL_checkinteger(L, idx);
}

/*
 * ------------------------------------------------------------------------
 * The functions, in `kfs.lua`'s order.
 * ------------------------------------------------------------------------
 */

static int l_stamp(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)kfs_stamp((uint64_t)luaL_checkinteger(L, 1),
                                              (uint32_t)luaL_optinteger(L, 2, 0)));
    return 1;
}

static int l_modified(lua_State *L)
{
    uint64_t mtime = 0, epoch;

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "mtime");

        if (lua_isinteger(L, -1)) {
            mtime = (uint64_t)lua_tointeger(L, -1);
        }

        lua_pop(L, 1);
    }

    if (!kfs_modified(mtime, &epoch)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, (lua_Integer)epoch);
    return 1;
}

static int l_read_block(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    int r;

    enter(L);
    r = kfs_read_block(K, (uint32_t)n, K->part);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    lua_pushlstring(L, (const char *)K->part, KFS_BLOCK);
    return 1;
}

static int l_write_block(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    size_t len;
    const char *bytes = luaL_checklstring(L, 2, &len);

    enter(L);

    if (len > KFS_BLOCK) {
        return answer(L, KFS_E_BLOCK_LONG);
    }

    return answer(L, kfs_write_block(K, (uint32_t)n, bytes, (uint32_t)len));
}

static int l_begin(lua_State *L)
{
    return answer(L, kfs_begin(K));
}

static int l_rollback(lua_State *L)
{
    (void)L;
    kfs_rollback(K);
    return 0;
}

static int l_commit(lua_State *L)
{
    struct kfs_super sb;
    const char *stop = luaL_optstring(L, 2, "");

    get_sb(L, 1, &sb);
    enter(L);
    return answer(L, kfs_commit(K, &sb, strcmp(stop, "after-commit") == 0));
}

static int l_recover(lua_State *L)
{
    struct kfs_super sb;
    uint32_t replayed;
    int r;

    get_sb(L, 1, &sb);
    enter(L);
    r = kfs_recover(K, &sb, &replayed);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    lua_pushinteger(L, replayed);
    return 1;
}

static int l_alloc_run(lua_State *L)
{
    struct kfs_super sb;
    uint32_t start, got;
    int r;

    get_sb(L, 1, &sb);
    enter(L);
    r = kfs_alloc_run(K, &sb, (uint32_t)luaL_checkinteger(L, 2), &start, &got);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    lua_pushinteger(L, start);
    lua_pushinteger(L, got);
    return 2;
}

static int l_alloc_block(lua_State *L)
{
    struct kfs_super sb;
    uint32_t start, got;
    int r;

    get_sb(L, 1, &sb);
    enter(L);
    r = kfs_alloc_run(K, &sb, 1, &start, &got);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    lua_pushinteger(L, start);
    return 1;
}

static int l_free_block(lua_State *L)
{
    struct kfs_super sb;

    get_sb(L, 1, &sb);
    enter(L);
    return answer(L, kfs_free_run(K, &sb, (uint32_t)luaL_checkinteger(L, 2), 1));
}

static int l_free_blocks(lua_State *L)
{
    struct kfs_super sb;
    uint64_t n;
    int r;

    get_sb(L, 1, &sb);
    enter(L);
    r = kfs_free_blocks(K, &sb, &n);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int l_read_inode(lua_State *L)
{
    struct kfs_super sb;
    struct kfs_inode node;
    lua_Integer n;
    int r;

    get_sb(L, 1, &sb);
    n = luaL_checkinteger(L, 2);
    enter(L);

    if (n < 0) {
        return answer(L, KFS_E_NO_INODE);
    }

    r = kfs_read_inode(K, &sb, (uint32_t)n, &node);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    push_node(L, &node);
    return 1;
}

static int l_write_inode(lua_State *L)
{
    struct kfs_super sb;
    struct kfs_inode node;
    lua_Integer n;

    get_sb(L, 1, &sb);
    n = luaL_checkinteger(L, 2);
    get_node(L, 3, &node);
    enter(L);

    if (n < 0) {
        return answer(L, KFS_E_NO_INODE);
    }

    return answer(L, kfs_write_inode(K, &sb, (uint32_t)n, &node));
}

static int l_alloc_inode(lua_State *L)
{
    struct kfs_super sb;
    uint32_t n;
    int r;

    get_sb(L, 1, &sb);
    enter(L);
    r = kfs_alloc_inode(K, &sb, &n);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    lua_pushinteger(L, n);
    return 1;
}

/* `want` bytes from `offset`, cut to the file: how many that is. */
static uint64_t clipped(const struct kfs_inode *node, uint64_t offset, uint64_t want)
{
    if (offset >= node->size) {
        return 0;
    }

    return want < node->size - offset ? want : node->size - offset;
}

static int l_read_range(lua_State *L)
{
    struct kfs_super sb;
    struct kfs_inode node;
    uint64_t offset, want, placed;
    uint8_t *buf;
    int r;

    get_sb(L, 1, &sb);
    get_node(L, 2, &node);
    offset = (uint64_t)luaL_checkinteger(L, 3);
    want = clipped(&node, offset, (uint64_t)luaL_checkinteger(L, 4));
    enter(L);

    buf = malloc(want > 0 ? want : 1);

    if (buf == NULL) {
        return luaL_error(L, "out of memory");
    }

    r = kfs_read_range(K, &sb, &node, offset, want, buf, &placed);

    if (r == KFS_OK) {
        lua_pushlstring(L, (const char *)buf, placed);
    }

    free(buf);
    return r == KFS_OK ? 1 : answer(L, r);
}

static int l_read_file(lua_State *L)
{
    lua_settop(L, 2);
    lua_pushinteger(L, 0);
    lua_getfield(L, 2, "size");
    return l_read_range(L);
}

static int l_read_range_into(lua_State *L)
{
    struct kfs_super sb;
    struct kfs_inode node;
    uint64_t offset, want, placed;
    int r;

    get_sb(L, 1, &sb);
    get_node(L, 2, &node);
    offset = (uint64_t)luaL_checkinteger(L, 3);
    want = clipped(&node, offset, (uint64_t)luaL_checkinteger(L, 4));
    enter(L);

    if (want == 0) {
        lua_pushinteger(L, 0);
        return 1;
    }

    H.into = malloc(want);

    if (H.into == NULL) {
        return luaL_error(L, "out of memory");
    }

    H.into_len = want;
    H.into_cap = luaL_checkinteger(L, 5);
    H.into_at = luaL_checkinteger(L, 6);
    r = kfs_read_range(K, &sb, &node, offset, want, H.into, &placed);

    /* What the core cut from its own blocks, to the region with the rest. */
    if (r == KFS_OK) {
        lua_pushinteger(L, H.into_cap);
        lua_pushinteger(L, H.into_at);
        lua_pushlstring(L, (const char *)H.into, placed);

        if (!call_sys(L, "region_write", 3)) {
            r = KFS_E_DISK;
        } else {
            lua_pop(L, 2);
        }
    }

    free(H.into);
    H.into = NULL;

    if (r != KFS_OK) {
        return answer(L, r);
    }

    lua_pushinteger(L, (lua_Integer)placed);
    return 1;
}

/*
 * Where a file's bytes come from (`kfs.lua`'s `source_of`): a string, a
 * region, or a reader. Left on the stack as one string, with the region's
 * place remembered so its whole blocks are written from it; a reader is
 * read 64 KB at a time, as `kfs.lua` reads one.
 */
static int push_source(lua_State *L, int idx, const uint8_t **bytes, size_t *size)
{
    lua_Integer n;

    H.from = NULL;

    if (lua_type(L, idx) == LUA_TSTRING) {
        lua_pushvalue(L, idx);
        *bytes = (const uint8_t *)lua_tolstring(L, -1, size);
        return KFS_OK;
    }

    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "size");
    n = lua_isnumber(L, -1) ? (lua_Integer)lua_tonumber(L, -1) : 0;
    lua_pop(L, 1);

    if (n < 0) {
        n = 0;
    }

    lua_getfield(L, idx, "region");

    if (!lua_isnil(L, -1)) {
        lua_Integer cap = lua_tointeger(L, -1);
        lua_Integer at;

        lua_pop(L, 1);
        lua_getfield(L, idx, "at");
        at = lua_isnumber(L, -1) ? (lua_Integer)lua_tonumber(L, -1) : 0;
        lua_pop(L, 1);

        if (n == 0) {
            lua_pushliteral(L, "");
        } else {
            lua_pushinteger(L, cap);
            lua_pushinteger(L, at);
            lua_pushinteger(L, n);

            if (!call_sys(L, "region_read", 3)) {
                return KFS_E_DISK;
            }

            lua_pop(L, 1);
        }

        *bytes = (const uint8_t *)lua_tolstring(L, -1, size);
        H.from = *bytes;
        H.from_len = *size;
        H.from_cap = cap;
        H.from_at = at;
        return KFS_OK;
    }

    lua_pop(L, 1);

    {
        luaL_Buffer b;
        lua_Integer done = 0;

        luaL_buffinit(L, &b);

        while (done < n) {
            lua_Integer want = n - done < 65536 ? n - done : 65536;
            size_t got;

            lua_getfield(L, idx, "read");
            lua_pushinteger(L, done);
            lua_pushinteger(L, want);
            lua_call(L, 2, 1);

            if (lua_tolstring(L, -1, &got) == NULL || (lua_Integer)got != want) {
                lua_pop(L, 1);
                luaL_pushresult(&b);
                lua_pop(L, 1);
                snprintf(H.why, sizeof H.why, "the file's bytes came up short");
                return KFS_E_DISK;
            }

            luaL_addvalue(&b);
            done += want;
        }

        luaL_pushresult(&b);
        *bytes = (const uint8_t *)lua_tolstring(L, -1, size);
    }

    return KFS_OK;
}

static int l_write_file(lua_State *L)
{
    struct kfs_super sb;
    struct kfs_inode node;
    const uint8_t *bytes;
    size_t size;
    int r;

    get_sb(L, 1, &sb);
    get_node(L, 3, &node);
    enter(L);
    r = push_source(L, 4, &bytes, &size);

    if (r == KFS_OK) {
        r = kfs_write_file(K, &sb, (uint32_t)luaL_checkinteger(L, 2), &node, bytes, size);
        update_node(L, 3, &node);
    }

    H.from = NULL;
    return answer(L, r);
}

static void push_entries(lua_State *L)
{
    uint32_t pos = 0, inode, n;
    const char *name;
    lua_Integer i = 0;

    lua_newtable(L);

    while (kfs_dir_next(K, &pos, &inode, &name, &n)) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, inode);
        lua_setfield(L, -2, "inode");
        lua_pushlstring(L, name, n);
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, ++i);
    }
}

static int l_read_dir(lua_State *L)
{
    struct kfs_super sb;
    struct kfs_inode node;
    int r;

    get_sb(L, 1, &sb);
    get_node(L, 2, &node);
    enter(L);
    r = kfs_open_dir(K, &sb, &node);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    push_entries(L);
    return 1;
}

static int l_same_name(lua_State *L)
{
    size_t a_len, b_len;
    const char *a = luaL_checklstring(L, 1, &a_len);
    const char *b = luaL_checklstring(L, 2, &b_len);

    lua_pushboolean(L, kfs_same_name(a, a_len, b, b_len));
    return 1;
}

/* Attributes: the core keeps `sys.pack`'s bytes; this packs and unpacks. */
static int l_read_attrs(lua_State *L)
{
    struct kfs_super sb;
    struct kfs_inode node;
    uint32_t len;
    int r;

    get_sb(L, 1, &sb);

    if (!lua_istable(L, 2)) {
        lua_newtable(L);
        return 1;
    }

    get_node(L, 2, &node);
    enter(L);
    r = kfs_read_attrs(K, &sb, &node, K->map, &len);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    if (len == 0) {
        lua_newtable(L);
        return 1;
    }

    lua_pushlstring(L, (const char *)K->map, len);

    if (!call_sys(L, "unpack", 1) || !lua_istable(L, -2)) {
        lua_pushnil(L);
        lua_pushfstring(L, "the attributes did not unpack: %s", H.why);
        return 2;
    }

    lua_pop(L, 1);
    return 1;
}

static int l_write_attrs(lua_State *L)
{
    struct kfs_super sb;
    struct kfs_inode node;
    const char *bytes = "";
    size_t len = 0;
    int r;

    get_sb(L, 1, &sb);
    get_node(L, 3, &node);
    luaL_checktype(L, 4, LUA_TTABLE);
    enter(L);

    lua_pushnil(L);

    if (lua_next(L, 4) != 0) {
        lua_pop(L, 2);
        lua_pushvalue(L, 4);

        if (!call_sys(L, "pack", 1)) {
            lua_pushnil(L);
            lua_pushstring(L, H.why);
            return 2;
        }

        lua_pop(L, 1);
        bytes = lua_tolstring(L, -1, &len);
    }

    if (len > KFS_BLOCK - 4) {
        return answer(L, KFS_E_ATTRS_BIG);
    }

    r = kfs_write_attrs(K, &sb, (uint32_t)luaL_checkinteger(L, 2), &node, bytes,
                        (uint32_t)len);
    update_node(L, 3, &node);
    return answer(L, r);
}

static int l_find(lua_State *L)
{
    struct kfs_super sb;
    struct kfs_inode node;
    uint32_t number;
    size_t len;
    const char *path;
    int r;

    get_sb(L, 1, &sb);
    path = luaL_optlstring(L, 2, "", &len);
    enter(L);
    r = kfs_find(K, &sb, path, len, &number, &node);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    lua_pushinteger(L, number);
    push_node(L, &node);
    return 2;
}

static int l_parent_of(lua_State *L)
{
    struct kfs_super sb;
    struct kfs_inode node;
    uint32_t number;
    size_t len, name_len;
    const char *path, *name;
    int r;

    get_sb(L, 1, &sb);
    path = luaL_optlstring(L, 2, "", &len);
    enter(L);
    r = kfs_parent_of(K, &sb, path, len, &number, &node, &name, &name_len);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    lua_pushinteger(L, number);
    push_node(L, &node);
    lua_pushlstring(L, name, name_len);
    return 3;
}

static int l_spelled(lua_State *L)
{
    struct kfs_super sb;
    size_t len, n;
    const char *path;
    char *out;

    get_sb(L, 1, &sb);
    path = luaL_checklstring(L, 2, &len);
    enter(L);
    out = malloc(len + 2);

    if (out == NULL) {
        return luaL_error(L, "out of memory");
    }

    n = kfs_spelled(K, &sb, path, len, lua_toboolean(L, 3), out);
    lua_pushlstring(L, out, n);
    free(out);
    return 1;
}

static int l_list(lua_State *L)
{
    struct kfs_super sb;
    size_t len;
    const char *path;
    int r;

    get_sb(L, 1, &sb);
    path = luaL_optlstring(L, 2, "", &len);
    enter(L);
    r = kfs_list(K, &sb, path, len);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    /* Names only, sorted as `kfs.lua` sorts them. */
    {
        uint32_t pos = 0, inode, n;
        const char *name;
        lua_Integer i = 0;

        lua_newtable(L);

        while (kfs_dir_next(K, &pos, &inode, &name, &n)) {
            lua_pushlstring(L, name, n);
            lua_rawseti(L, -2, ++i);
        }

        lua_getglobal(L, "table");
        lua_getfield(L, -1, "sort");
        lua_remove(L, -2);
        lua_pushvalue(L, -2);
        lua_call(L, 1, 0);
    }

    return 1;
}

static int l_mkdir(lua_State *L)
{
    struct kfs_super sb;
    size_t len;
    const char *path;

    get_sb(L, 1, &sb);
    path = luaL_checklstring(L, 2, &len);
    enter(L);
    return answer(L, kfs_mkdir(K, &sb, path, len, opt_time(L, 3)));
}

static int l_store(lua_State *L)
{
    struct kfs_super sb;
    const uint8_t *bytes;
    size_t len, size;
    const char *path;
    uint32_t number;
    int r;

    get_sb(L, 1, &sb);
    path = luaL_checklstring(L, 2, &len);
    enter(L);
    r = push_source(L, 3, &bytes, &size);

    if (r == KFS_OK) {
        r = kfs_store(K, &sb, path, len, bytes, size, opt_time(L, 4), &number);
    }

    H.from = NULL;

    if (r != KFS_OK) {
        return answer(L, r);
    }

    lua_pushinteger(L, number);
    return 1;
}

static int l_rename(lua_State *L)
{
    struct kfs_super sb;
    size_t len, to_len;
    const char *path, *to;

    get_sb(L, 1, &sb);
    path = luaL_checklstring(L, 2, &len);

    if (lua_type(L, 3) != LUA_TSTRING) {
        return answer(L, KFS_E_NAME_EMPTY);
    }

    to = lua_tolstring(L, 3, &to_len);
    enter(L);
    return answer(L, kfs_rename(K, &sb, path, len, to, to_len));
}

static int l_unlink(lua_State *L)
{
    struct kfs_super sb;
    size_t len;
    const char *path;

    get_sb(L, 1, &sb);
    path = luaL_checklstring(L, 2, &len);
    enter(L);
    return answer(L, kfs_unlink(K, &sb, path, len));
}

/*
 * The folders a disk is made with: the module's `LAYOUT`, which a script may
 * replace as it could `kfs.lua`'s - `tools/kfs.lua` does, for `KFS_LAYOUT`.
 * Each name stays on the stack while the core uses it.
 */
#define LAYOUT_MOST 16

static int l_mkfs(lua_State *L)
{
    struct kfs_super sb;
    lua_Integer sectors = luaL_checkinteger(L, 1);
    const char *layout[LAYOUT_MOST + 1];
    const char *const *given = NULL;
    int r;

    lua_getfield(L, LUA_REGISTRYINDEX, KFSC_MODULE);
    lua_getfield(L, -1, "LAYOUT");

    if (lua_istable(L, -1)) {
        lua_Integer n = luaL_len(L, -1);

        luaL_argcheck(L, n <= LAYOUT_MOST, 1, "more folders in LAYOUT than a disk is made with");
        luaL_checkstack(L, (int)n, "the layout's names");

        for (lua_Integer i = 1; i <= n; i++) {
            lua_rawgeti(L, -1 - (int)(i - 1), i);
            layout[i - 1] = luaL_checkstring(L, -1);
        }

        layout[n] = NULL;
        given = layout;
    }

    enter(L);
    r = kfs_mkfs(K, sectors > 0 ? (uint64_t)sectors : 0, opt_time(L, 2), given, &sb);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    push_sb(L, &sb);
    return 1;
}

static int l_mount(lua_State *L)
{
    struct kfs_super sb;
    int r;

    enter(L);
    r = kfs_mount(K, &sb);

    if (r != KFS_OK) {
        return answer(L, r);
    }

    push_sb(L, &sb);
    return 1;
}

/*
 * The disk the core is handed: the script's `sys`, or the disk server's cache
 * in C over it - `user/servers/diskcache.c`, as the machine will run the two
 * together (`docs/diskfs.md` step 3). `test_kfs.lua` asks for the cache with
 * `KFS_CACHE=1`, and clears it where it changes the disk behind the core.
 */
static const struct kfs_disk host_disk = { NULL, host_read, host_write, 1 };
static struct diskcache *C;

static int l_use_cache(lua_State *L)
{
    lua_Integer most = luaL_checkinteger(L, 1), small = luaL_checkinteger(L, 2);

    if (C == NULL && (C = malloc(sizeof *C)) == NULL) {
        return luaL_error(L, "out of memory for the cache");
    }

    diskcache_init(C, &host_disk, (uint32_t)most, (uint32_t)small);
    K->disk = diskcache_disk(C);
    return 0;
}

static int l_cache_clear(lua_State *L)
{
    (void)L;

    if (C != NULL) {
        diskcache_clear(C);
    }

    return 0;
}

static int luaopen_kfsc(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "stamp",           l_stamp },
        { "modified",        l_modified },
        { "read_block",      l_read_block },
        { "write_block",     l_write_block },
        { "begin",           l_begin },
        { "rollback",        l_rollback },
        { "commit",          l_commit },
        { "recover",         l_recover },
        { "alloc_run",       l_alloc_run },
        { "alloc_block",     l_alloc_block },
        { "free_block",      l_free_block },
        { "free_blocks",     l_free_blocks },
        { "read_inode",      l_read_inode },
        { "write_inode",     l_write_inode },
        { "alloc_inode",     l_alloc_inode },
        { "read_range",      l_read_range },
        { "read_range_into", l_read_range_into },
        { "read_file",       l_read_file },
        { "write_file",      l_write_file },
        { "read_dir",        l_read_dir },
        { "same_name",       l_same_name },
        { "read_attrs",      l_read_attrs },
        { "write_attrs",     l_write_attrs },
        { "find",            l_find },
        { "parent_of",       l_parent_of },
        { "spelled",         l_spelled },
        { "list",            l_list },
        { "mkdir",           l_mkdir },
        { "store",           l_store },
        { "rename",          l_rename },
        { "unlink",          l_unlink },
        { "mkfs",            l_mkfs },
        { "mount",           l_mount },
        { "use_cache",       l_use_cache },
        { "cache_clear",     l_cache_clear },
        { NULL, NULL },
    };
    static const struct { const char *name; lua_Integer value; } constants[] = {
        { "MAGIC", KFS_MAGIC },         { "VERSION", KFS_VERSION },
        { "BLOCK", KFS_BLOCK },         { "SECTOR", KFS_SECTOR },
        { "PER_BLOCK", KFS_PER_BLOCK }, { "INODE_SIZE", KFS_INODE_SIZE },
        { "EXTENTS", KFS_EXTENTS },     { "ROOT_INODE", KFS_ROOT_INODE },
        { "JOURNAL_BLOCKS", KFS_JOURNAL_BLOCKS },
        { "KIND_FREE", KFS_KIND_FREE }, { "KIND_FILE", KFS_KIND_FILE },
        { "KIND_DIR", KFS_KIND_DIR },   { "DATED", (lua_Integer)KFS_DATED },
        { "J_MAGIC", KFS_J_MAGIC },     { "J_EMPTY", KFS_J_EMPTY },
        { "J_COMMITTED", KFS_J_COMMITTED },
    };

    if (K == NULL) {
        K = malloc(sizeof *K);

        if (K == NULL) {
            return luaL_error(L, "out of memory for the filesystem");
        }

        kfs_init(K, &host_disk);
    }

    luaL_newlib(L, fns);

    for (size_t i = 0; i < sizeof constants / sizeof constants[0]; i++) {
        lua_pushinteger(L, constants[i].value);
        lua_setfield(L, -2, constants[i].name);
    }

    lua_pushliteral(L, "<I4I4I4I4I8");
    lua_setfield(L, -2, "J_HEADER");

    lua_createtable(L, 1, 0);

    for (int i = 0; kfs_layout[i] != NULL; i++) {
        lua_pushstring(L, kfs_layout[i]);
        lua_rawseti(L, -2, i + 1);
    }

    lua_setfield(L, -2, "LAYOUT");

    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, KFSC_MODULE);
    return 1;
}

/*
 * ------------------------------------------------------------------------
 * `require "kpack"`: the machine's serialiser - `lua/kosmos/serialize.c`,
 * which is `sys.pack` and `sys.unpack` there - over a string, so a script
 * here makes and reads the bytes the machine does rather than a stand-in's.
 * ------------------------------------------------------------------------
 */

static int l_kpack_pack(lua_State *L)
{
    static unsigned char buf[64 * 1024];
    size_t len;
    int r;

    luaL_checkany(L, 1);
    r = serialize_pack_into(L, 1, buf, sizeof buf, &len);

    if (r != SERIALIZE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, serialize_error(r));
        return 2;
    }

    lua_pushlstring(L, (const char *)buf, len);
    return 1;
}

static int l_kpack_unpack(lua_State *L)
{
    size_t len;
    const char *bytes = luaL_checklstring(L, 1, &len);
    int r = serialize_unpack_from(L, (const unsigned char *)bytes, len);

    if (r != SERIALIZE_OK) {
        lua_pushnil(L);
        lua_pushstring(L, serialize_error(r));
        return 2;
    }

    return 1;
}

static int luaopen_kpack(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "pack", l_kpack_pack }, { "unpack", l_kpack_unpack }, { NULL, NULL },
    };

    luaL_newlib(L, fns);
    return 1;
}

/*
 * `require "packflat"`: `user/servers/packflat.c`, for
 * `tools/test_packflat.lua` to hold to the serialiser above. Each call reads
 * the bytes it is given, does one thing, and answers bytes, text or why not.
 */

static int packflat_fail(lua_State *L, int r)
{
    lua_pushnil(L);
    lua_pushstring(L, r == PACKFLAT_E_MALFORMED ? "malformed"
                      : r == PACKFLAT_E_NOT_FLAT ? "not flat"
                      : r == PACKFLAT_E_TOO_MANY ? "too many"
                      : r == PACKFLAT_E_TOO_BIG ? "too big" : "failed");
    return 2;
}

static struct packflat *read_flat(lua_State *L, int *r)
{
    static struct packflat t;
    size_t len;
    const char *bytes = luaL_checklstring(L, 1, &len);

    *r = packflat_read(bytes, len, &t);
    return &t;
}

static int push_written(lua_State *L, const struct packflat *t)
{
    static unsigned char out[4092];
    size_t len;
    int r = packflat_write(t, out, sizeof out, &len);

    if (r != PACKFLAT_OK) {
        return packflat_fail(L, r);
    }

    lua_pushlstring(L, (const char *)out, len);
    return 1;
}

/* packflat.rewrite(bytes): read and written again, as a block holds them. */
static int l_packflat_rewrite(lua_State *L)
{
    int r;
    struct packflat *t = read_flat(L, &r);

    return r != PACKFLAT_OK ? packflat_fail(L, r) : push_written(L, t);
}

/* packflat.set(bytes, name, value): `value` a string, number or boolean, or
 * nil to take `name` out. */
static int l_packflat_set(lua_State *L)
{
    int r;
    struct packflat *t = read_flat(L, &r);
    size_t nlen, vlen;
    const char *name = luaL_checklstring(L, 2, &nlen);
    struct packflat_value v;

    if (r != PACKFLAT_OK) {
        return packflat_fail(L, r);
    }

    memset(&v, 0, sizeof v);

    switch (lua_type(L, 3)) {
    case LUA_TNONE:
    case LUA_TNIL:
        r = packflat_set(t, name, nlen, NULL);
        break;

    case LUA_TBOOLEAN:
        v.type = lua_toboolean(L, 3) ? PACKFLAT_TRUE : PACKFLAT_FALSE;
        r = packflat_set(t, name, nlen, &v);
        break;

    case LUA_TNUMBER:
        if (lua_isinteger(L, 3)) {
            v = packflat_int((int64_t)lua_tointeger(L, 3));
        } else {
            double d = (double)lua_tonumber(L, 3);

            v.type = PACKFLAT_FLOAT;
            memcpy(&v.bits, &d, sizeof d);
        }

        r = packflat_set(t, name, nlen, &v);
        break;

    default: {
        const char *text = luaL_checklstring(L, 3, &vlen);

        v = packflat_string(text, vlen);
        r = packflat_set(t, name, nlen, &v);
        break;
    }
    }

    return r != PACKFLAT_OK ? packflat_fail(L, r) : push_written(L, t);
}

/* packflat.text(bytes, name): the value named, as `tostring` writes it. */
static int l_packflat_text(lua_State *L)
{
    int r;
    struct packflat *t = read_flat(L, &r);
    size_t nlen;
    const char *name = luaL_checklstring(L, 2, &nlen);
    const struct packflat_value *v;
    char text[4096];

    if (r != PACKFLAT_OK) {
        return packflat_fail(L, r);
    }

    v = packflat_get(t, name, nlen);

    if (v == NULL) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, text, packflat_text(v, text, sizeof text));
    return 1;
}

static int luaopen_packflat(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "rewrite", l_packflat_rewrite }, { "set", l_packflat_set },
        { "text", l_packflat_text }, { NULL, NULL },
    };

    luaL_newlib(L, fns);
    return 1;
}

/*
 * ------------------------------------------------------------------------
 * The interpreter: `lua.c`'s job, for a script and its arguments, and
 * nothing it does for a person at a prompt.
 * ------------------------------------------------------------------------
 */

static int traceback(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);

    luaL_traceback(L, L, msg != NULL ? msg : "(an error that is not a string)", 1);
    return 1;
}

int main(int argc, char **argv)
{
    lua_State *L;
    int base;

    if (argc < 2) {
        fprintf(stderr, "usage: %s script [args]\n", argv[0]);
        return 2;
    }

    L = luaL_newstate();

    if (L == NULL) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }

    /* Upstream's collector, as `lua.c` sets it: stopped while the libraries
     * load, then generational - so no script here collects differently for
     * the change of binary. `LUA_INIT` is the one thing of `lua.c`'s left
     * out, and nothing here sets it. */
    lua_gc(L, LUA_GCSTOP);
    luaL_openlibs(L);
    lua_gc(L, LUA_GCRESTART);
    lua_gc(L, LUA_GCGEN, 0, 0);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    lua_pushcfunction(L, luaopen_kfsc);
    lua_setfield(L, -2, "kfsc");
    lua_pushcfunction(L, luaopen_kpack);
    lua_setfield(L, -2, "kpack");
    lua_pushcfunction(L, luaopen_packflat);
    lua_setfield(L, -2, "packflat");
    lua_pop(L, 1);

    lua_createtable(L, argc - 2, 2);

    for (int i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }

    lua_setglobal(L, "arg");

    lua_pushcfunction(L, traceback);
    base = lua_gettop(L);

    if (luaL_loadfile(L, argv[1]) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    /* Room for them first: Lua promises twenty slots and no more, and
     * `test_filetypes.lua` is given a hundred and fifty names. */
    luaL_checkstack(L, argc, "too many arguments to the script");

    for (int i = 2; i < argc; i++) {
        lua_pushstring(L, argv[i]);
    }

    if (lua_pcall(L, argc - 2, 0, base) != LUA_OK) {
        fprintf(stderr, "%s: %s\n", argv[1], lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    lua_close(L);
    return 0;
}
