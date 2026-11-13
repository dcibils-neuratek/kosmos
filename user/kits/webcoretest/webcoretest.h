/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* W6b's WebCore test (`webcoretest.cpp`): each page in `FOLDER/pages/`
 * loaded into a WebCore Page with no window, laid out at 800 by 600 with
 * the fonts in `FOLDER/fonts/`, and its render tree said a line at a time
 * - the text WebKit's LayoutTests are held to. The number of pages, or -1. */
#ifndef WEBCORETEST_H
#define WEBCORETEST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*webcoretest_say_fn)(const char *line, void *ctx);

int webcoretest_run(const char *folder, const char *const *pages, int count,
                    webcoretest_say_fn say, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
