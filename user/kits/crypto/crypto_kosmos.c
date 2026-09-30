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
 * Two functions so far, each because a program asks for it: `des` for
 * `vncd`'s password check, and `random` for whatever needs randomness -
 * VNC's challenge first, TLS next. The rest of `crypto.c` joins this table
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

void kosmos_crypto_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "des",    l_des },
        { "random", l_random },
        { NULL, NULL }
    };

    luaL_newlib(L, api);
}
