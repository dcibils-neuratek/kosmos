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
 * One function so far, because one program asks for one: `vncd`'s password
 * check. The rest of `crypto.c` - SHA-256, HMAC, ChaCha20, Poly1305,
 * X25519 - joins this table when a program needs it, not before.
 */

#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "crypto.h"

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

void kosmos_crypto_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "des", l_des },
        { NULL, NULL }
    };

    luaL_newlib(L, api);
}
