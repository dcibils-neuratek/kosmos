/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * PNG and JPEG, decoded by plain C with no Lua anywhere near them.
 *
 * `gfx.png` and `gfx.jpeg` were each one function that read its arguments
 * off a Lua stack, decoded, and pushed a surface - so the only way to decode
 * a picture was to be a Lua state. Flight's terrain builder is a pthread
 * with no state of its own (`docs/flight.md` F4d, F5): it has a tile's bytes
 * and wants its pixels, and the answer must not be a second decoder beside
 * the first. So the decoders are here, and `png.c` and `jpeg.c` are their
 * Lua doors and nothing else - one decoder each, two ways in.
 *
 * **Safe on any thread.** Nothing here keeps state between calls: every
 * buffer a decode uses is its own, from `malloc` (under a lock since
 * `threads.md` step 7), and the inflater's workspace is one of its own
 * rather than the process's shared `kosmos_inflater()`. Two threads may
 * decode at once. The one thing shared is stb_image's failure sentence,
 * which `stb_impl.c` keeps in a global for want of thread locals: two
 * JPEGs refused at the same instant can swap sentences, never pixels.
 *
 * **The pixels are 0xAARRGGBB in a 32-bit word**, which is what every gfx
 * surface holds, straight alpha (not premultiplied), opaque where the file
 * has no alpha.
 */

#ifndef KOSMOS_PICTURE_DECODE_H
#define KOSMOS_PICTURE_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* What a decode came to. */
enum {
    GFX_PICTURE_OK       =  0,
    GFX_PICTURE_BAD      = -1,  /* this format, and refused: `why` says why */
    GFX_PICTURE_NOT_MINE = -2   /* not this format at all - try another     */
};

/*
 * The plain form: `*pixels` is `*w` by `*h` words in rows of exactly `*w`
 * (a pitch of `*w * 4` bytes), from `malloc`, given back with
 * `gfx_picture_free`. On failure nothing is allocated, `*pixels` is NULL
 * and `*why` is a sentence - a string constant, never to be freed.
 */
int gfx_png_decode(const uint8_t *bytes, size_t n, uint32_t **pixels,
                   unsigned *w, unsigned *h, const char **why);
int gfx_jpeg_decode(const uint8_t *bytes, size_t n, uint32_t **pixels,
                    unsigned *w, unsigned *h, const char **why);
void gfx_picture_free(uint32_t *pixels);

/*
 * The form that decodes into somebody else's memory, for a caller whose
 * rows have a pitch of their own: a surface's, 64-byte aligned, which is
 * what `gfx.png` wants so that the picture lands in the surface without a
 * second copy.
 *
 * `room` is asked once, after the file has been read and found good, for
 * `w` by `h` pixels; it answers with the first row and that memory's pitch
 * in bytes (at least `w * 4`), or NULL when there is no room - which is the
 * decode's failure, "no room for the picture". Once `room` has answered
 * nothing can fail, so a caller never has memory to hand back from a
 * refused decode. Bytes between `w * 4` and the pitch are not written.
 */
typedef uint32_t *(*gfx_picture_room)(void *user, unsigned w, unsigned h,
                                      unsigned *pitch);

int gfx_png_decode_into(const uint8_t *bytes, size_t n,
                        gfx_picture_room room, void *user,
                        unsigned *w, unsigned *h, const char **why);
int gfx_jpeg_decode_into(const uint8_t *bytes, size_t n,
                         gfx_picture_room room, void *user,
                         unsigned *w, unsigned *h, const char **why);

#endif
