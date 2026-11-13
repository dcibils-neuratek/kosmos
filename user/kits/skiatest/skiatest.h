/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* W5e's Skia test (`skiatest.cpp`): every scene drawn by Skia into a
 * surface of its own, and a line said for each - its pixels' fingerprint
 * and how many it painted - with the fonts in `FOLDER/fonts/` and the
 * pictures in `FOLDER/pictures/`. The number of scenes drawn, or -1. */
#ifndef SKIATEST_H
#define SKIATEST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*skiatest_say_fn)(const char *line, void *ctx);

int skiatest_run(const char *folder, skiatest_say_fn say, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
