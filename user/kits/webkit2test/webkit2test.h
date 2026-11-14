/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **W7a's WebKit2 test** (`docs/w7.md`, `tools/run_webkit2.py`): a UI
 * process with no window, which starts a web process and a network
 * process, loads a page in them and paints the picture it is sent.
 */

#ifndef KOSMOS_WEBKIT2TEST_H
#define KOSMOS_WEBKIT2TEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*webkit2test_say_fn)(const char *line, void *ctx);

enum webkit2test_mode {
    WEBKIT2TEST_LOAD = 0,         /* load, wait for the picture, paint it */
    WEBKIT2TEST_KILL = 1,         /* the web process ended once it committed */
    WEBKIT2TEST_URL = 2,          /* `html` is an address, loaded by the network process */
};

/* The page `html` - or, in WEBKIT2TEST_URL, the page at that address - loaded and its picture painted into the pixels, within
 * `ms` milliseconds; what happened said a line at a time. 0 when it was
 * painted. */
int webkit2test_load(const char *html, uint32_t *pixels, unsigned width, unsigned height,
                     unsigned pitch, unsigned ms, int mode, webkit2test_say_fn say, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
