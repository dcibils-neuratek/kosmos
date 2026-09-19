/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_BACKLIGHTPROTO_H
#define KOSMOS_BACKLIGHTPROTO_H

#include <stdint.h>

/*
 * What you may ask the backlight driver, at `/dev/backlight`.
 *
 * A declared shape, as `audioproto.h` is and for its reasons: two fixed
 * fields each way, and nothing a caller can send that the driver has to
 * think about. **Levels are 0 to 256**, the scale every gain in the audio
 * server uses, so the window manager's two level keys are the same
 * arithmetic.
 *
 * **The driver keeps a floor**, `BACKLIGHT_LEVEL_FLOOR`, and a `set` below
 * it is raised to it. A black panel on a machine whose only way back is a
 * key the person cannot see is the one level nobody should be able to ask
 * for - not a program, and not a finger held on F5 too long.
 */

#define BACKLIGHT_OP_GET   1u
#define BACKLIGHT_OP_SET   2u

#define BACKLIGHT_OK              0u
#define BACKLIGHT_ERR_NO_DEVICE   1u    /* no backlight this driver knows */
#define BACKLIGHT_ERR_BAD_OP      2u
#define BACKLIGHT_ERR_UNCHANGED   3u    /* written, and read back different */

#define BACKLIGHT_LEVEL_FULL   256u
#define BACKLIGHT_LEVEL_FLOOR   16u     /* a sixteenth: dim, never black */

struct backlight_request {
    uint32_t op;
    uint32_t level;             /* set: 0..256, raised to the floor */
};

struct backlight_reply {
    uint32_t error;             /* BACKLIGHT_OK, or why not */
    uint32_t level;             /* as it now is, read from the controller */
};

_Static_assert(sizeof(struct backlight_request) == 8,
               "the backlight request is two words, and backlight.lua packs "
               "it as \"<I4I4\"");
_Static_assert(sizeof(struct backlight_reply) == 8,
               "the backlight reply is two words, and backlight.lua unpacks "
               "it as \"<I4I4\"");

#endif /* KOSMOS_BACKLIGHTPROTO_H */
