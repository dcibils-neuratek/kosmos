/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **W5b's text, held to HarfBuzz and to this Mac** (`docs/w5.md`):
 * HarfBuzz 15.1.0 given its own shaping suite, and FreeType 2.15.0 every
 * font that suite ships.
 *
 * - **HarfBuzz**: every case in `cases.txt` - HarfBuzz's `test/shape`,
 *   flattened by `tools/run_text.py` from the `.tests` files its Meson
 *   build names, once for each font-functions implementation a case asks
 *   for - is shaped as `hb-shape` shapes it and held to the glyphs the
 *   suite expects, exactly as `test/shape/run-tests.py` holds them: the
 *   same buffer flags, the same font set-up, the same serialisation, and
 *   when the names differ only by `gidN`, the same second look through
 *   `--glyphs --no-glyph-names`. `hb-shape` itself needs GLib, so this is
 *   the part of it the suite uses, written from `util/shape-options.hh`,
 *   `font-options.hh`, `text-options.hh` and `shape-output.hh`.
 * - **FreeType**: every font in `fonts.txt` loaded, and its first glyphs
 *   rendered hinted at 24 pixels and unhinted at 13, each bitmap and its
 *   metrics fingerprinted.
 *
 * One line a test file and one a font, each with a fingerprint of all it
 * produced, so the guest is held to the Mac byte for byte and to
 * HarfBuzz's expectations case by case. A fingerprint is FNV-1a over 64
 * bits.
 *
 * Ported code's test: it reads its fonts with `fopen`, through the POSIX
 * layer.
 */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hb.h>
#include <hb-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "texttest.h"

/* ---- what is said ---- */

static texttest_say_fn say_fn;
static void *say_ctx;

static void say(const char *format, ...)
{
    char line[2048];
    va_list ap;

    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    say_fn(line, say_ctx);
}

static uint64_t fnv_more(uint64_t h, const void *bytes, size_t length)
{
    const unsigned char *p = bytes;

    for (size_t i = 0; i < length; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

#define FNV_START 0xcbf29ce484222325ull

static unsigned char *slurp(const char *path, size_t *length)
{
    FILE *f = fopen(path, "rb");
    unsigned char *bytes = NULL;
    size_t have = 0, room = 0;

    if (!f)
        return NULL;
    for (;;) {
        if (have == room) {
            unsigned char *more;

            room = room ? room * 2 : 65536;
            more = realloc(bytes, room + 1);
            if (!more) {
                free(bytes);
                fclose(f);
                return NULL;
            }
            bytes = more;
        }
        size_t got = fread(bytes + have, 1, room - have, f);
        if (got == 0)
            break;
        have += got;
    }
    fclose(f);
    bytes[have] = 0;
    *length = have;
    return bytes;
}

/* ---- a growing string ---- */

struct text {
    char *s;
    size_t len, room;
};

static void text_add(struct text *t, const char *s, size_t n)
{
    if (t->len + n + 1 > t->room) {
        size_t room = t->room ? t->room : 256;

        while (t->len + n + 1 > room)
            room *= 2;
        char *more = realloc(t->s, room);
        if (!more)
            return;
        t->s = more;
        t->room = room;
    }
    memcpy(t->s + t->len, s, n);
    t->len += n;
    t->s[t->len] = 0;
}

static void text_clear(struct text *t)
{
    t->len = 0;
    if (t->s)
        t->s[0] = 0;
}

/* GLib's g_string_append_unichar, which `hb-shape` encodes `--unicodes`
 * with: up to six bytes, surrogates and all, since what HarfBuzz makes of
 * a malformed sequence is part of what the suite tests. */
static void text_add_unichar(struct text *t, uint32_t c)
{
    char out[6];
    int first, len;

    if (c < 0x80) {
        first = 0;
        len = 1;
    } else if (c < 0x800) {
        first = 0xc0;
        len = 2;
    } else if (c < 0x10000) {
        first = 0xe0;
        len = 3;
    } else if (c < 0x200000) {
        first = 0xf0;
        len = 4;
    } else if (c < 0x4000000) {
        first = 0xf8;
        len = 5;
    } else {
        first = 0xfc;
        len = 6;
    }
    for (int i = len - 1; i > 0; i--) {
        out[i] = (char)((c & 0x3f) | 0x80);
        c >>= 6;
    }
    out[0] = (char)(c | (uint32_t)first);
    text_add(t, out, (size_t)len);
}

/* `text-options.hh`'s encode_unicodes. */
static int encode_unicodes(const char *s, struct text *t)
{
    static const char delimiters[] = "<+-|>{},;&#\\xXuUnNiI\n\t\v\f\r ";

    while (s && *s) {
        char *p;

        while (*s && strchr(delimiters, *s))
            s++;
        if (!*s)
            break;
        unsigned long u = strtoul(s, &p, 16);
        if (s == p)
            return 0;
        text_add_unichar(t, (uint32_t)u);
        s = p;
    }
    return 1;
}

/* The next piece of `*at` up to `delim`, empty pieces skipped, as
 * `strtok` gives them - which Kosmos's libc does not have. */
static char *piece(char **at, char delim)
{
    char *p = *at, *end;

    while (p && *p == delim)
        p++;
    if (!p || !*p) {
        *at = NULL;
        return NULL;
    }
    end = strchr(p, delim);
    if (end) {
        *end = 0;
        *at = end + 1;
    } else
        *at = NULL;
    return p;
}

/* ---- one case's options, as `hb-shape` reads them ---- */

#define FONT_SIZE_UPEM 0x7FFFFFFF

struct options {
    /* shape-options.hh */
    const char *direction, *language, *script;
    int bot, eot, preserve_default_ignorables, remove_default_ignorables;
    hb_feature_t features[64];
    unsigned num_features;
    int utf8_clusters;
    hb_codepoint_t invisible_glyph, not_found_glyph, not_found_vs_glyph;
    int cluster_level;
    int normalize_glyphs, glyphs, verify, unsafe_to_concat, safe_to_insert_tatweel;
    const char *shapers[8];             /* NULL-ended; none means the default list */
    /* text-options.hh */
    int single_par;
    struct text text, before, after;
    int have_before, have_after;
    /* face-options.hh, font-options.hh */
    unsigned face_index;
    double font_size_x, font_size_y, ptem, x_embolden, y_embolden, slant;
    int x_ppem, y_ppem, embolden_in_place;
    const char *font_funcs;
    int ft_load_flags;
    unsigned named_instance;
    hb_variation_t variations[32];
    unsigned num_variations;
    /* shape-format.hh */
    int show_glyph_names, show_positions, show_advances, show_clusters;
    int show_extents, show_flags;
};

static void options_init(struct options *o)
{
    struct text text = o->text, before = o->before, after = o->after;

    memset(o, 0, sizeof(*o));
    o->text = text;
    o->before = before;
    o->after = after;
    text_clear(&o->text);
    text_clear(&o->before);
    text_clear(&o->after);
    o->not_found_vs_glyph = HB_CODEPOINT_INVALID;
    o->cluster_level = HB_BUFFER_CLUSTER_LEVEL_DEFAULT;
    o->font_size_x = o->font_size_y = FONT_SIZE_UPEM;
    o->ft_load_flags = 2;
    o->named_instance = HB_FONT_NO_VAR_NAMED_INSTANCE;
    o->show_glyph_names = o->show_positions = o->show_advances = o->show_clusters = 1;
}

/* `parse_features` and `parse_variations`: quotes stripped, then each
 * item between commas or spaces. */
static char *strip_quotes(char *s)
{
    size_t l = strlen(s);

    if (l && s[0] == s[l - 1] && (s[0] == '"' || s[0] == '\'')) {
        s[l - 1] = 0;
        s++;
    }
    return s;
}

static void parse_features(struct options *o, char *s)
{
    s = strip_quotes(s);
    o->num_features = 0;
    for (char *p = s; p && *p;) {
        char *end = strpbrk(p, ", ");

        if (o->num_features < 64 &&
            hb_feature_from_string(p, end ? (int)(end - p) : -1, &o->features[o->num_features]))
            o->num_features++;
        p = end ? end + 1 : NULL;
    }
}

static void parse_variations(struct options *o, char *s)
{
    s = strip_quotes(s);
    o->num_variations = 0;
    for (char *p = s; p && *p;) {
        char *end = strpbrk(p, ", ");

        if (o->num_variations < 32 &&
            hb_variation_from_string(p, end ? (int)(end - p) : -1,
                                     &o->variations[o->num_variations]))
            o->num_variations++;
        p = end ? end + 1 : NULL;
    }
}

static int parse_pair(const char *arg, double *x, double *y)
{
    switch (sscanf(arg, "%lf%*[ ,]%lf", x, y)) {
    case 1:
        *y = *x;
        return 1;
    case 2:
        return 1;
    default:
        return 0;
    }
}

/* GLib's G_OPTION_ARG_INT: strtol, base 0. */
static long parse_int(const char *arg)
{
    return strtol(arg, NULL, 0);
}

static const char *const takes_value[] = {
    "unicodes", "unicodes-before", "unicodes-after", "features", "variations",
    "direction", "language", "script", "cluster-level", "invisible-glyph",
    "not-found-glyph", "not-found-variation-selector-glyph", "face-index",
    "font-size", "font-ppem", "font-ptem", "font-bold", "font-grade", "font-slant",
    "font-funcs", "ft-load-flags", "named-instance", "shaper", "shapers", NULL,
};

/* The tokens of one case, `--name=value` or `--name value`. NULL on
 * success, or the option that was not understood. */
static const char *parse_options(struct options *o, char **tokens, int count)
{
    for (int i = 0; i < count; i++) {
        char *name = tokens[i], *value = NULL, *eq;
        int needs = 0;

        if (strncmp(name, "--", 2))
            return name;
        name += 2;
        if ((eq = strchr(name, '=')) != NULL) {
            *eq = 0;
            value = eq + 1;
        }
        for (int k = 0; takes_value[k]; k++)
            if (!strcmp(name, takes_value[k]))
                needs = 1;
        if (needs && !value) {
            if (i + 1 >= count)
                return name;
            value = tokens[++i];
        }

        if (!strcmp(name, "unicodes")) {
            text_clear(&o->text);
            if (!encode_unicodes(value, &o->text))
                return name;
        } else if (!strcmp(name, "unicodes-before")) {
            o->have_before = 1;
            if (!encode_unicodes(value, &o->before))
                return name;
        } else if (!strcmp(name, "unicodes-after")) {
            o->have_after = 1;
            if (!encode_unicodes(value, &o->after))
                return name;
        } else if (!strcmp(name, "shaper") || !strcmp(name, "shapers")) {
            /* `parse_shapers`: a comma-separated list. */
            unsigned n = 0;

            char *at = value;
            for (char *p; n < 7 && (p = piece(&at, ',')) != NULL;)
                o->shapers[n++] = p;
            o->shapers[n] = NULL;
        } else if (!strcmp(name, "features"))
            parse_features(o, value);
        else if (!strcmp(name, "variations"))
            parse_variations(o, value);
        else if (!strcmp(name, "direction"))
            o->direction = value;
        else if (!strcmp(name, "language"))
            o->language = value;
        else if (!strcmp(name, "script"))
            o->script = value;
        else if (!strcmp(name, "cluster-level"))
            o->cluster_level = (int)parse_int(value);
        else if (!strcmp(name, "invisible-glyph"))
            o->invisible_glyph = (hb_codepoint_t)parse_int(value);
        else if (!strcmp(name, "not-found-glyph"))
            o->not_found_glyph = (hb_codepoint_t)parse_int(value);
        else if (!strcmp(name, "not-found-variation-selector-glyph"))
            o->not_found_vs_glyph = (hb_codepoint_t)parse_int(value);
        else if (!strcmp(name, "face-index"))
            o->face_index = (unsigned)parse_int(value);
        else if (!strcmp(name, "font-size")) {
            if (!strcmp(value, "upem"))
                o->font_size_x = o->font_size_y = FONT_SIZE_UPEM;
            else if (!parse_pair(value, &o->font_size_x, &o->font_size_y))
                return name;
        } else if (!strcmp(name, "font-ppem")) {
            double x, y;

            if (!parse_pair(value, &x, &y))
                return name;
            o->x_ppem = (int)x;
            o->y_ppem = (int)y;
        } else if (!strcmp(name, "font-ptem"))
            o->ptem = strtod(value, NULL);
        else if (!strcmp(name, "font-bold") || !strcmp(name, "font-grade")) {
            o->embolden_in_place = !strcmp(name, "font-grade");
            if (!parse_pair(value, &o->x_embolden, &o->y_embolden))
                return name;
        } else if (!strcmp(name, "font-slant"))
            o->slant = strtod(value, NULL);
        else if (!strcmp(name, "font-funcs"))
            o->font_funcs = value;
        else if (!strcmp(name, "ft-load-flags"))
            o->ft_load_flags = (int)parse_int(value);
        else if (!strcmp(name, "named-instance"))
            o->named_instance = (unsigned)parse_int(value);
        else if (!strcmp(name, "bot"))
            o->bot = 1;
        else if (!strcmp(name, "eot"))
            o->eot = 1;
        else if (!strcmp(name, "preserve-default-ignorables"))
            o->preserve_default_ignorables = 1;
        else if (!strcmp(name, "remove-default-ignorables"))
            o->remove_default_ignorables = 1;
        else if (!strcmp(name, "utf8-clusters"))
            o->utf8_clusters = 1;
        else if (!strcmp(name, "normalize-glyphs"))
            o->normalize_glyphs = 1;
        else if (!strcmp(name, "glyphs"))
            o->glyphs = 1;
        else if (!strcmp(name, "verify"))
            o->verify = 1;
        else if (!strcmp(name, "unsafe-to-concat"))
            o->unsafe_to_concat = 1;
        else if (!strcmp(name, "safe-to-insert-tatweel"))
            o->safe_to_insert_tatweel = 1;
        else if (!strcmp(name, "single-par"))
            o->single_par = 1;
        else if (!strcmp(name, "no-glyph-names"))
            o->show_glyph_names = 0;
        else if (!strcmp(name, "no-positions"))
            o->show_positions = 0;
        else if (!strcmp(name, "no-advances"))
            o->show_advances = 0;
        else if (!strcmp(name, "no-clusters"))
            o->show_clusters = 0;
        else if (!strcmp(name, "ned"))
            o->show_clusters = o->show_advances = 0;
        else if (!strcmp(name, "show-extents"))
            o->show_extents = 1;
        else if (!strcmp(name, "show-flags"))
            o->show_flags = 1;
        else
            return name;
    }
    return NULL;
}

/* ---- faces, one file at a time ---- */

static char face_path[512];
static unsigned face_index_held = (unsigned)-1;
static hb_face_t *face_held;

/* `hb_face_create_from_file_or_fail_using` with the default loader, the
 * file read here rather than mapped: the same face either way. */
static hb_face_t *face_for(const char *path, unsigned index)
{
    if (face_held && index == face_index_held && !strcmp(path, face_path))
        return face_held;
    if (face_held)
        hb_face_destroy(face_held);
    face_held = NULL;

    size_t length;
    unsigned char *bytes = slurp(path, &length);
    if (!bytes)
        return NULL;
    hb_blob_t *blob = hb_blob_create_or_fail((const char *)bytes, (unsigned)length,
                                             HB_MEMORY_MODE_READONLY, bytes, free);
    if (!blob) {
        free(bytes);
        return NULL;
    }
    face_held = hb_face_create_or_fail(blob, index);
    hb_blob_destroy(blob);
    if (!face_held)
        return NULL;
    snprintf(face_path, sizeof(face_path), "%s", path);
    face_index_held = index;
    return face_held;
}

/* `font_options_t::post_parse`. */
static hb_font_t *font_for(hb_face_t *face, const struct options *o)
{
    hb_font_t *font = hb_font_create(face);
    double sx = o->font_size_x, sy = o->font_size_y;

    if (sx == FONT_SIZE_UPEM)
        sx = hb_face_get_upem(face);
    if (sy == FONT_SIZE_UPEM)
        sy = hb_face_get_upem(face);

    hb_font_set_ppem(font, (unsigned)o->x_ppem, (unsigned)o->y_ppem);
    hb_font_set_ptem(font, (float)o->ptem);
    hb_font_set_synthetic_bold(font, (float)o->x_embolden, (float)o->y_embolden,
                               o->embolden_in_place);
    hb_font_set_synthetic_slant(font, (float)o->slant);
    hb_font_set_scale(font, (int)scalbnf((float)sx, 0), (int)scalbnf((float)sy, 0));
    if (o->named_instance != HB_FONT_NO_VAR_NAMED_INSTANCE)
        hb_font_set_var_named_instance(font, o->named_instance);
    hb_font_set_variations(font, o->variations, o->num_variations);
    if (o->font_funcs && !hb_font_set_funcs_using(font, o->font_funcs)) {
        hb_font_destroy(font);
        return NULL;
    }
    hb_ft_font_set_load_flags(font, o->ft_load_flags);
    return font;
}

/* ---- shaping one line, as `hb-shape` does ---- */

static void setup_buffer(hb_buffer_t *buffer, const struct options *o)
{
    hb_buffer_set_direction(buffer, hb_direction_from_string(o->direction, -1));
    hb_buffer_set_script(buffer, hb_script_from_string(o->script, -1));
    hb_buffer_set_language(buffer, hb_language_from_string(o->language, -1));
    hb_buffer_set_flags(buffer, (hb_buffer_flags_t)(
        HB_BUFFER_FLAG_DEFAULT |
        (o->bot ? HB_BUFFER_FLAG_BOT : 0) |
        (o->eot ? HB_BUFFER_FLAG_EOT : 0) |
        (o->verify ? HB_BUFFER_FLAG_VERIFY : 0) |
        (o->unsafe_to_concat ? HB_BUFFER_FLAG_PRODUCE_UNSAFE_TO_CONCAT : 0) |
        (o->safe_to_insert_tatweel ? HB_BUFFER_FLAG_PRODUCE_SAFE_TO_INSERT_TATWEEL : 0) |
        (o->preserve_default_ignorables ? HB_BUFFER_FLAG_PRESERVE_DEFAULT_IGNORABLES : 0) |
        (o->remove_default_ignorables ? HB_BUFFER_FLAG_REMOVE_DEFAULT_IGNORABLES : 0)));
    hb_buffer_set_invisible_glyph(buffer, o->invisible_glyph);
    hb_buffer_set_not_found_glyph(buffer, o->not_found_glyph);
    hb_buffer_set_not_found_variation_selector_glyph(buffer, o->not_found_vs_glyph);
    hb_buffer_set_cluster_level(buffer, (hb_buffer_cluster_level_t)o->cluster_level);
    hb_buffer_guess_segment_properties(buffer);
}

static unsigned serialize_flags(const struct options *o)
{
    unsigned flags = HB_BUFFER_SERIALIZE_FLAG_DEFAULT;

    if (!o->show_glyph_names)
        flags |= HB_BUFFER_SERIALIZE_FLAG_NO_GLYPH_NAMES;
    if (!o->show_clusters)
        flags |= HB_BUFFER_SERIALIZE_FLAG_NO_CLUSTERS;
    if (!o->show_positions)
        flags |= HB_BUFFER_SERIALIZE_FLAG_NO_POSITIONS;
    if (!o->show_advances)
        flags |= HB_BUFFER_SERIALIZE_FLAG_NO_ADVANCES;
    if (o->show_extents)
        flags |= HB_BUFFER_SERIALIZE_FLAG_GLYPH_EXTENTS;
    if (o->show_flags)
        flags |= HB_BUFFER_SERIALIZE_FLAG_GLYPH_FLAGS;
    return flags;
}

static void serialize(hb_buffer_t *buffer, hb_font_t *font, unsigned flags, struct text *out)
{
    unsigned count = hb_buffer_get_length(buffer), start = 0;
    static char chunk[32768];

    while (start < count) {
        unsigned consumed;

        start += hb_buffer_serialize(buffer, start, count, chunk, sizeof(chunk), &consumed,
                                     font, HB_BUFFER_SERIALIZE_FORMAT_TEXT,
                                     (hb_buffer_serialize_flags_t)flags);
        if (!consumed)
            break;
        text_add(out, chunk, strlen(chunk));
    }
}

/* `shape_options_t::populate_buffer` and `shape`, for a line of glyphs:
 * the second look `run-tests.py` takes when names differ by `gidN`. */
static void shape_glyphs(hb_buffer_t *buffer, hb_font_t *font, const struct options *o,
                         const char *line, struct text *out)
{
    struct text glyphs = {0};
    size_t len = strlen(line);

    hb_buffer_clear_contents(buffer);
    setup_buffer(buffer, o);
    text_add(&glyphs, line, len);
    if (len && line[len - 1] != ']')
        text_add(&glyphs, "]", 1);
    hb_buffer_deserialize_glyphs(buffer, glyphs.s ? glyphs.s : "", -1, NULL, font,
                                 HB_BUFFER_SERIALIZE_FORMAT_TEXT);
    if (glyphs.s && !strchr(glyphs.s, '+') && !strchr(glyphs.s, '@')) {
        unsigned count;
        hb_direction_t direction = hb_buffer_get_direction(buffer);
        hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buffer, &count);
        hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, &count);

        for (unsigned i = 0; i < count; i++)
            hb_font_get_glyph_advance_for_direction(font, infos[i].codepoint, direction,
                                                    &positions[i].x_advance,
                                                    &positions[i].y_advance);
    }
    free(glyphs.s);
    /* The font is at its upem, so `shape` scales nothing. */
    serialize(buffer, font, serialize_flags(o), out);
}

/* `shape_consumer_t::consume_line` for the first line of the text, which
 * is the line `run-tests.py` reads back. */
static void shape_text(hb_buffer_t *buffer, hb_font_t *font, const struct options *o,
                       struct text *out)
{
    const char *line = o->text.s ? o->text.s : "";
    size_t line_len = o->text.len;

    if (!o->single_par) {
        const char *nl = memchr(line, '\n', line_len);

        if (nl)
            line_len = (size_t)(nl - line);
    }

    hb_buffer_clear_contents(buffer);
    if (o->have_before)
        hb_buffer_add_utf8(buffer, o->before.s ? o->before.s : "",
                           (int)strlen(o->before.s ? o->before.s : ""),
                           (unsigned)strlen(o->before.s ? o->before.s : ""), 0);
    hb_buffer_add_utf8(buffer, line, (int)line_len, 0, (int)line_len);
    if (o->have_after)
        hb_buffer_add_utf8(buffer, o->after.s ? o->after.s : "", -1, 0, 0);
    if (!o->utf8_clusters) {
        unsigned count = hb_buffer_get_length(buffer);
        hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, NULL);

        for (unsigned i = 0; i < count; i++)
            info[i].cluster = i;
    }
    setup_buffer(buffer, o);

    if (!hb_shape_full(font, buffer, o->features, o->num_features,
                       o->shapers[0] ? o->shapers : NULL)) {
        text_add(out, "error: Shaping failed.", 22);
        return;
    }
    if (o->normalize_glyphs)
        hb_buffer_normalize_glyphs(buffer);
    serialize(buffer, font, serialize_flags(o), out);
}

/* The second look, on its own font: `hb-shape FONT --glyphs
 * --no-glyph-names GLYPHS`, with every other option at its default. */
static void without_names(const char *path, const char *glyphs, struct text *out)
{
    static struct options o;
    hb_face_t *face;
    hb_font_t *font;

    options_init(&o);
    o.glyphs = 1;
    o.show_glyph_names = 0;
    face = face_for(path, 0);
    if (!face || !(font = font_for(face, &o)))
        return;
    hb_buffer_t *buffer = hb_buffer_create();
    shape_glyphs(buffer, font, &o, glyphs, out);
    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
}

/* ---- the suite ---- */

struct tally {
    char name[160];
    unsigned cases, pass, fail;
    uint64_t print;
};

static void tally_say(const struct tally *t)
{
    if (t->cases)
        say("shape: %s: %u cases, %u as expected, %u not, %016llx", t->name, t->cases,
            t->pass, t->fail, (unsigned long long)t->print);
}

/* One case: `NAME \t FONT \t OPTIONS \t EXPECTED`. 1 when it is as
 * expected, 0 when not, -1 when the line is not a case. */
static int run_case(const char *folder, char *line, struct tally *t, unsigned *failures_said)
{
    static struct options o;
    static struct text got, got_ids, want_ids;
    char *fields[4], *tokens[64], path[512], asked[1024];
    int count = 0, ntok = 0;

    for (char *p = line; count < 4; count++) {
        fields[count] = p;
        p = strchr(p, '\t');
        if (!p) {
            count++;
            break;
        }
        *p++ = 0;
    }
    if (count != 4)
        return -1;

    if (strcmp(t->name, fields[0])) {
        tally_say(t);
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", fields[0]);
        t->print = FNV_START;
    }
    snprintf(path, sizeof(path), "%s/%s", folder, fields[1]);
    snprintf(asked, sizeof(asked), "%s", fields[2]);
    char *at = fields[2];
    for (char *p; ntok < 64 && (p = piece(&at, ' ')) != NULL;)
        tokens[ntok++] = p;

    options_init(&o);
    text_clear(&got);
    text_clear(&got_ids);
    text_clear(&want_ids);
    const char *bad = parse_options(&o, tokens, ntok);
    hb_face_t *face = bad ? NULL : face_for(path, o.face_index);
    hb_font_t *font = face ? font_for(face, &o) : NULL;

    if (bad)
        text_add(&got, "error: option ", 14), text_add(&got, bad, strlen(bad));
    else if (!face)
        text_add(&got, "error: no face", 14);
    else if (!font)
        text_add(&got, "error: font funcs", 17);
    else {
        hb_buffer_t *buffer = hb_buffer_create();

        if (o.glyphs)
            shape_glyphs(buffer, font, &o, o.text.s ? o.text.s : "", &got);
        else
            shape_text(buffer, font, &o, &got);
        hb_buffer_destroy(buffer);
        hb_font_destroy(font);
    }

    const char *want = fields[3];
    const char *result = got.s ? got.s : "";
    int pass;

    if (!strcmp(want, "*"))
        pass = 1;
    else if (!strcmp(result, want))
        pass = 1;
    else if (strstr(result, "gid")) {
        without_names(path, result, &got_ids);
        without_names(path, want, &want_ids);
        pass = want_ids.len && !strcmp(got_ids.s, want_ids.s);
    } else
        pass = 0;

    t->cases++;
    if (pass)
        t->pass++;
    else {
        t->fail++;
        if ((*failures_said)++ < 40)
            say("shape-not-expected: %s: %s: got %.600s", t->name, asked, result);
    }
    t->print = fnv_more(t->print, result, strlen(result));
    t->print = fnv_more(t->print, "\n", 1);
    return pass;
}

/* ---- FreeType ---- */

static void freetype_font(FT_Library library, const char *folder, const char *name)
{
    char path[512];
    size_t length;
    unsigned char *bytes;
    FT_Face face;
    FT_Error error;
    uint64_t h = FNV_START;
    unsigned rendered = 0;

    snprintf(path, sizeof(path), "%s/%s", folder, name);
    bytes = slurp(path, &length);
    if (!bytes) {
        say("freetype: %s: not read", name);
        return;
    }
    error = FT_New_Memory_Face(library, bytes, (FT_Long)length, 0, &face);
    if (error) {
        say("freetype: %s: not a face, error %d", name, error);
        free(bytes);
        return;
    }

    static const struct { unsigned pixels; FT_Int32 flags; } passes[] = {
        { 24, FT_LOAD_DEFAULT },
        { 13, FT_LOAD_NO_HINTING },
    };
    for (unsigned k = 0; k < 2; k++) {
        if (FT_IS_SCALABLE(face))
            error = FT_Set_Pixel_Sizes(face, 0, passes[k].pixels);
        else
            error = face->num_fixed_sizes ? FT_Select_Size(face, 0) : 1;
        h = fnv_more(h, &error, sizeof(error));
        if (error)
            continue;
        FT_Long glyphs = face->num_glyphs < 48 ? face->num_glyphs : 48;
        for (FT_Long g = 0; g < glyphs; g++) {
            FT_Int32 flags = passes[k].flags | (FT_HAS_COLOR(face) ? FT_LOAD_COLOR : 0);

            error = FT_Load_Glyph(face, (FT_UInt)g, flags);
            if (!error)
                error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
            int32_t facts[8] = {
                error,
                (int32_t)face->glyph->metrics.horiAdvance, (int32_t)face->glyph->metrics.width,
                (int32_t)face->glyph->metrics.height, face->glyph->bitmap_left,
                face->glyph->bitmap_top, (int32_t)face->glyph->bitmap.width,
                (int32_t)face->glyph->bitmap.rows,
            };
            h = fnv_more(h, facts, sizeof(facts));
            if (error)
                continue;
            FT_Bitmap *b = &face->glyph->bitmap;
            unsigned row_bytes;

            switch (b->pixel_mode) {
            case FT_PIXEL_MODE_MONO: row_bytes = (b->width + 7) / 8; break;
            case FT_PIXEL_MODE_BGRA: row_bytes = b->width * 4; break;
            default:                 row_bytes = b->width; break;
            }
            for (unsigned r = 0; r < b->rows; r++) {
                int pitch = b->pitch;
                const unsigned char *row = pitch >= 0
                    ? b->buffer + (size_t)r * (size_t)pitch
                    : b->buffer + (size_t)(b->rows - 1 - r) * (size_t)(-pitch);
                h = fnv_more(h, row, row_bytes);
            }
            rendered++;
        }
    }
    say("freetype: %s: %ld glyphs, %u rendered, %016llx", name, face->num_glyphs, rendered,
        (unsigned long long)h);
    FT_Done_Face(face);
    free(bytes);
}

/* ---- the door ---- */

static char *next_line(char **at)
{
    char *line = *at, *nl;

    if (!line || !*line)
        return NULL;
    nl = strchr(line, '\n');
    if (nl) {
        *nl = 0;
        *at = nl + 1;
    } else
        *at = NULL;
    return line;
}

int texttest_run(const char *folder, texttest_say_fn fn, void *ctx)
{
    char path[512];
    size_t length;
    unsigned char *cases, *fonts;
    int count = 0;
    unsigned failures_said = 0, all = 0, pass = 0;
    static struct tally t;

    say_fn = fn;
    say_ctx = ctx;

    struct text list = {0};
    for (const char **f = hb_font_list_funcs(); *f; f++)
        text_add(&list, " ", 1), text_add(&list, *f, strlen(*f));
    text_add(&list, ";", 1);
    for (const char **f = hb_shape_list_shapers(); *f; f++)
        text_add(&list, " ", 1), text_add(&list, *f, strlen(*f));
    say("versions: harfbuzz %s, freetype %d.%d.%d; font funcs%s", hb_version_string(),
        FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH, list.s);
    free(list.s);

    snprintf(path, sizeof(path), "%s/cases.txt", folder);
    cases = slurp(path, &length);
    if (!cases) {
        say("texttest: no %s", path);
        return -1;
    }
    memset(&t, 0, sizeof(t));
    char *at = (char *)cases;
    for (char *line; (line = next_line(&at)) != NULL;) {
        int result = run_case(folder, line, &t, &failures_said);

        if (result >= 0) {
            count++;
            all++;
            pass += (unsigned)result;
        }
    }
    tally_say(&t);
    free(cases);
    if (face_held)
        hb_face_destroy(face_held);
    face_held = NULL;

    snprintf(path, sizeof(path), "%s/fonts.txt", folder);
    fonts = slurp(path, &length);
    unsigned nfonts = 0;
    if (fonts) {
        FT_Library library;

        if (FT_Init_FreeType(&library))
            say("freetype: no library");
        else {
            at = (char *)fonts;
            for (char *line; (line = next_line(&at)) != NULL; nfonts++)
                freetype_font(library, folder, line);
            FT_Done_FreeType(library);
        }
        free(fonts);
    }

    say("texttest: %u cases, %u as expected, %u not; %u fonts", all, pass, all - pass, nfonts);
    return count;
}

#ifdef TEXTTEST_HOST
/* On this Mac, the reference: the folder the guest is given, the lines
 * on standard output. */
static void print_line(const char *line, void *ctx)
{
    (void)ctx;
    puts(line);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: texttest FOLDER\n");
        return 2;
    }
    return texttest_run(argv[1], print_line, NULL) < 0;
}
#endif
