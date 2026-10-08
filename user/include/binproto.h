/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_BINPROTO_H
#define KOSMOS_BINPROTO_H

#include <stdint.h>

/*
 * What you may ask the /bin server, written down.
 *
 * The third protocol here to be a definition, and the first that has to
 * carry something whose length it does not know: a program's source.
 *
 * **Which raises the rule and does not break it.** `CLAUDE.md` says a stream
 * never travels as a message payload, and the test is the *rate* - if it
 * recurs because the hardware says so, the bytes go in a region. A program's
 * source is not that: it is read once, when somebody launches the thing, and
 * then not again. Nobody's clock is waiting.
 *
 * So this chunks, as the Lua original did, but the chunk is now as large as
 * a message will hold instead of a round kilobyte - which halves the round
 * trips for no work. A twenty-kilobyte program was twenty exchanges and is
 * now eleven.
 *
 * **`read_into` remains the better answer for anything large**, and it
 * already exists: the caller provides a region, the server fills it, and one
 * exchange moves the lot. That is the path a disk file takes. It is not used
 * here because /bin is small, in the image, and read at launch.
 *
 * `data` carries both payloads and `count`/`length` say which: a `list`
 * packs fixed-width names into it, a `read` puts raw source there. One
 * buffer rather than two, because a reply that could hold both at once is a
 * reply with a case nobody has thought about.
 */

#define BIN_OP_LIST     1u
#define BIN_OP_READ     2u
#define BIN_OP_GETATTR  3u

#define BIN_OK              0u
#define BIN_ERR_NO_PROGRAM  1u
#define BIN_ERR_BAD_OP      3u

/*
 * A name, padded. **Sixty-four, because a file name has to be able to be.**
 *
 * Twenty-four was generous for `/bin`, where every entry is one file with
 * a flat name - `tracker.lua` is twelve. It stopped being enough when the
 * library store started carrying a *tree*: Lite XL's Lua was 78 files with
 * paths like `litexl/core/commands/findreplace.lua`, which is thirty-six,
 * until it left the tree on 26 September. The solar system and Cafesa3D's
 * translators are trees now.
 *
 * The failure was not a truncated name, which would have been the good
 * outcome. `string.pack` in `init.lua` refused the field outright - "bad
 * argument #4 to 'pack'" from a line that has nothing to do with names -
 * so the first two files read and the third produced an error about
 * packing. Worth the sentence, because the next person to see it will be
 * looking at the wrong file.
 *
 * Forty-eight answered that, and sixty-four answers a rule rather than a
 * tree: a name anywhere in Kosmos may be sixty-four characters, and a store
 * that held fewer would be the one place that broke it.
 *
 * The assert below still holds at 64: 1632 / 64 is 25 names to a listing
 * reply, against the sixteen it asks for - 1792 and 28 until a title took
 * thirty-two bytes of it (3 October 2026), and 1760 and 27 until `needs`
 * took sixteen words rather than eight (8 October).
 */
#define BIN_NAME_MAX    64u
#define BIN_WORD_MAX    16u       /* a kind, a section, one `needs` word */
#define BIN_NEEDS_MAX   16u       /* authorities one program may declare */
#define BIN_ICON_MAX    32u       /* `Misc_Deskbar_Group` is eighteen */
#define BIN_OPENS_MAX   40u       /* `kosmos: opens`, a line of types */
#define BIN_TITLE_MAX   32u       /* `kosmos: name`, "About This Machine" */
#define BIN_CHUNK     1632u       /* source bytes, or 25 names, per reply */

struct bin_request {
    uint32_t op;
    uint32_t offset;              /* into the source for read, into the
                                     store for list - both continue from
                                     where the last reply stopped */
    char     name[BIN_NAME_MAX];
};

struct bin_reply {
    uint32_t error;
    uint32_t count;               /* names in `data`, for list */
    uint32_t size;                /* the program's whole length */
    uint32_t length;              /* bytes of `data` that are source */
    uint32_t more;                /* 1 when another chunk follows */
    uint32_t windowed;            /* 1 when it draws a window */

    /* Declared in the program's opening comment block and worked out when
     * the store loads, because it is a property of source that cannot
     * change while the system runs - /bin is in the image. */
    char     kind[BIN_WORD_MAX];
    char     section[BIN_WORD_MAX];
    char     needs[BIN_NEEDS_MAX][BIN_WORD_MAX];

    /* The picture the Deskbar draws beside it - `kosmos: icon App_Tracker`,
     * an asset's name without its `.png`. Wider than a word because Haiku's
     * names are, and blank when a program declares none. */
    char     icon[BIN_ICON_MAX];

    /* What it opens - `kosmos: opens png jpg jpeg`, the types a file of
     * which it is started with (`roadmap.md` 6z) - as the line says it,
     * words apart. Blank when it declares none. Programs as well as
     * applications: `play` opens a film in a window of its own. */
    char     opens[BIN_OPENS_MAX];

    /* Its name for a person - `kosmos: name Process Viewer` - which the
     * Deskbar's menu, the launcher and the dock show (Diego, 3 October 2026:
     * "it needs real app names not just the file name"). The file's own
     * name stays what everything is found by. Blank when none is declared,
     * and whoever shows it falls back to the file's. */
    char     title[BIN_TITLE_MAX];

    uint8_t  data[BIN_CHUNK];
};

_Static_assert(sizeof(struct bin_reply) <= 2048,
               "a /bin reply must fit in one message - lower BIN_CHUNK");
/*
 * How many names a listing carries per reply, which is *not* how many
 * programs there are.
 *
 * This used to read `>= 64` with the message "a list must hold every
 * program in the image", and it could never have checked that: the number
 * of programs is not something a static assert can see. It guarded the
 * chunk shrinking while the image grew past it, which is what happened -
 * 82 programs, 74 names to a reply, and the Deskbar quietly short of four
 * applications. A listing pages now, and this only has to be enough for
 * paging to be worth doing rather than a name at a time.
 */
_Static_assert(BIN_CHUNK / BIN_NAME_MAX >= 16,
               "a listing reply should carry a useful number of names");

#endif /* KOSMOS_BINPROTO_H */
