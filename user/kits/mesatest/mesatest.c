/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Mesa on Kosmos** (`docs/gpu3d.md` G1): a kit in no image but
 * `mesatest.elf`, reached as `use("mesatest.elf")`. `run(a, b, c)` is
 * Mesa's sentence, said by Mesa's own compiler:
 *
 * - a compute shader built with NIR's builder: `(a + b) * c`, written to
 *   memory - three constants the caller chose, so nothing here knows the
 *   answer;
 * - NIR's constant folding and algebraic passes run on it, and what is
 *   left stored is read back as one constant, `fold=`;
 * - the shader printed into a stream in memory (`open_memstream`, the
 *   POSIX layer's), its first line - `shader: MESA_SHADER_COMPUTE` - and
 *   how long it was, `print=`;
 * - and serialized into a blob and read back (`util`'s), the copy printed
 *   again and compared with the first, `blob=`.
 *
 * What it says is a line, and what is wrong is said in the line, so a
 * failure names the step.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_serialize.h"
#include "util/blob.h"
#include "util/ralloc.h"

static const nir_shader_compiler_options options = { 0 };

/* The shader printed into memory: its text, the caller's to free. */
static char *printed(nir_shader *s, size_t *length)
{
    char *text = NULL;
    FILE *f = open_memstream(&text, length);

    if (f == NULL) {
        return NULL;
    }

    nir_print_shader(s, f);
    fclose(f);
    return text;
}

/* What the shader stores, if it is one constant: 1 and `*value`. */
static int stored_constant(nir_shader *s, double *value)
{
    nir_function_impl *impl = nir_shader_get_entrypoint(s);

    nir_foreach_block(block, impl) {
        nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *store;
            nir_instr *from;

            if (instr->type != nir_instr_type_intrinsic) {
                continue;
            }

            store = nir_instr_as_intrinsic(instr);
            if (store->intrinsic != nir_intrinsic_store_global) {
                continue;
            }

            from = nir_def_instr(store->src[0].ssa);
            if (from->type != nir_instr_type_load_const) {
                return 0;
            }

            *value = nir_const_value_as_float(nir_instr_as_load_const(from)->value[0], 32);
            return 1;
        }
    }

    return 0;
}

static void say(char *out, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void say(char *out, size_t size, const char *fmt, ...)
{
    size_t at = strlen(out);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(out + at, size - at, fmt, ap);
    va_end(ap);
}

static void run(double a, double b, double c, char *out, size_t size)
{
    nir_builder builder;
    nir_shader *s, *copy;
    struct blob blob;
    struct blob_reader reader;
    char *text, *again, *newline;
    size_t length = 0, length_again = 0;
    double folded;
    bool progress;
    int rounds = 0;

    out[0] = '\0';
    say(out, size, "mesa");

    glsl_type_singleton_init_or_ref();

    builder = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "kosmos");
    s = builder.shader;
    nir_store_global(&builder,
                     nir_fmul(&builder,
                              nir_fadd(&builder, nir_imm_float(&builder, (float)a),
                                       nir_imm_float(&builder, (float)b)),
                              nir_imm_float(&builder, (float)c)),
                     nir_imm_int64(&builder, 0x1000));
    nir_validate_shader(s, "as built");

    do {
        progress = false;
        progress |= nir_opt_constant_folding(s);
        progress |= nir_opt_algebraic(s);
        progress |= nir_opt_dce(s);
        rounds++;
    } while (progress && rounds < 16);
    nir_validate_shader(s, "after the passes");

    if (stored_constant(s, &folded)) {
        say(out, size, " fold=%.4f/%d-rounds", folded, rounds);
    } else {
        say(out, size, " fold=not-a-constant");
    }

    text = printed(s, &length);
    if (text == NULL) {
        say(out, size, " print=no-memstream");
        goto done;
    }

    newline = strchr(text, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    say(out, size, " print=\"%s\"/%s", text,
        length > 100 ? "long" : "short");
    if (newline != NULL) {
        *newline = '\n';
    }

    blob_init(&blob);
    nir_serialize(&blob, s, false);
    blob_reader_init(&reader, blob.data, blob.size);
    copy = nir_deserialize(NULL, &options, &reader);

    if (copy == NULL || reader.overrun) {
        say(out, size, " blob=unread");
    } else {
        again = printed(copy, &length_again);
        say(out, size, " blob=%s",
            again != NULL && length_again == length && memcmp(again, text, length) == 0
                ? "same" : "different");
        free(again);
        ralloc_free(copy);
    }

    blob_finish(&blob);
    free(text);

done:
    ralloc_free(s);
    glsl_type_singleton_decref();
}

static int l_run(lua_State *L)
{
    static char said[1024];

    run(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3),
        said, sizeof(said));
    lua_pushstring(L, said);
    return 1;
}

void kosmos_mesatest_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_run);
    lua_setfield(L, -2, "run");
}
