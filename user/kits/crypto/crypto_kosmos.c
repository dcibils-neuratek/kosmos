/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Crypto Kit, as Lua sees it: `use("/Kosmos/Kits/crypto")`.
 *
 * **Encryption is C, all of it** - Diego, 29 September: "shouldnt we write
 * all encryption in c ... it should be super fast". A cipher is a loop over
 * bits and bytes, which is the shape `CLAUDE.md` puts in C, and it is the
 * one kind of code where a mistake is silent: so the primitives live in
 * `crypto.c`, each held to its specification's vectors by `test_crypto`,
 * and a program in Lua is handed the operation and never the arithmetic.
 *
 * Three functions so far, each because a program asks for it: `des` for
 * `vncd`'s password check, `random` for whatever needs randomness - VNC's
 * challenge first, TLS next - and `sha256`, for a file checked whole
 * without its bytes becoming a Lua string (sharing's step N3 checks a 64 MB
 * file read from a share with it). The rest of `crypto.c` joins this table
 * when a program needs it, not before.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "crypto.h"
#include "kosmos.h"

/*
 * `crypto.des(key, data)` - `data` under DES, eight bytes at a time, each
 * block alone (ECB), which is what VNC Authentication asks of it: sixteen
 * bytes of challenge under an eight-byte key. The key is exactly eight
 * bytes and the data a whole number of blocks, or it is an error rather
 * than something padded silently.
 */
static int l_des(lua_State *L)
{
    size_t key_bytes, bytes, at;
    const char *key = luaL_checklstring(L, 1, &key_bytes);
    const char *data = luaL_checklstring(L, 2, &bytes);
    luaL_Buffer b;
    char *out;

    if (key_bytes != 8) {
        return luaL_error(L, "crypto.des: a key is eight bytes, not %d",
                          (int)key_bytes);
    }

    if (bytes == 0 || bytes % 8 != 0 || bytes > 4096) {
        return luaL_error(L, "crypto.des: whole blocks of eight bytes, up to "
                          "4096, and this is %d", (int)bytes);
    }

    out = luaL_buffinitsize(L, &b, bytes);

    for (at = 0; at < bytes; at += 8) {
        des_encrypt((const uint8_t *)key, (const uint8_t *)data + at,
                    (uint8_t *)out + at);
    }

    luaL_pushresultsize(&b, bytes);
    return 1;
}

/*
 * `crypto.random(n)` - `n` random bytes, 1 to 65536, from this process's
 * generator (`crypto.c`), seeded from the hardware the first time it is
 * asked and mixed with fresh bytes from it after each megabyte. An error,
 * never weaker bytes, on a machine with no source: randomness that is
 * quietly not random is the one failure that looks like success.
 */
static struct drbg generator;
static int seeded;
static size_t since_seed;

static int reseed(void)
{
    uint8_t fresh[32];

    if (kosmos_entropy(fresh, sizeof(fresh)) != (long)sizeof(fresh)) {
        return 0;
    }

    if (seeded) {
        drbg_reseed(&generator, fresh);
    } else {
        drbg_seed(&generator, fresh);
        seeded = 1;
    }

    memset(fresh, 0, sizeof(fresh));
    since_seed = 0;
    return 1;
}

static int l_random(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    luaL_Buffer b;
    char *out;

    if (n < 1 || n > 65536) {
        return luaL_error(L, "crypto.random: 1 to 65536 bytes, not %d", (int)n);
    }

    if ((!seeded || since_seed >= (1u << 20)) && !reseed()) {
        return luaL_error(L, "crypto.random: this machine has no source of "
                          "randomness the kernel will hand out");
    }

    out = luaL_buffinitsize(L, &b, (size_t)n);
    drbg_generate(&generator, (uint8_t *)out, (size_t)n);
    since_seed += (size_t)n;
    luaL_pushresultsize(&b, (size_t)n);
    return 1;
}

/*
 * `crypto.sha256(data)` or `crypto.sha256(at, bytes)` - SHA-256 (FIPS
 * 180-4) as 64 hexadecimal digits: of a string, or of `bytes` bytes at `at`
 * in a region this process has mapped (`regions.make`'s `at`), as
 * `compress.crc32` takes one. The second is the one that matters: a file
 * read into a region is checked where it lies, and a file of megabytes
 * never passes through the heap to be hashed.
 */
static int l_sha256(lua_State *L)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[32];
    char out[64];
    unsigned i;

    if (lua_type(L, 1) == LUA_TSTRING) {
        size_t bytes;
        const char *data = lua_tolstring(L, 1, &bytes);

        sha256(data, bytes, digest);
    } else {
        uintptr_t at = (uintptr_t)luaL_checkinteger(L, 1);
        lua_Integer bytes = luaL_checkinteger(L, 2);

        if (bytes < 0 || (at == 0 && bytes > 0)) {
            return luaL_error(L, "crypto.sha256: a string, or a mapped region "
                              "and how many of its bytes");
        }

        sha256((const void *)at, (size_t)bytes, digest);
    }

    for (i = 0; i < 32; i++) {
        out[2 * i]     = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }

    lua_pushlstring(L, out, sizeof(out));
    return 1;
}

void kosmos_crypto_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "des",    l_des },
        { "random", l_random },
        { "sha256", l_sha256 },
        { NULL, NULL }
    };

    luaL_newlib(L, api);
}
