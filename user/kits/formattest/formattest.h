/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* W5f's format test (`formattest.cpp`): every file `FOLDER/manifest.txt`
 * names, asked of Brotli, woff2 or libavif on dav1d, one line said for
 * each. 0, or -1 when there is no manifest. */
#ifndef FORMATTEST_H
#define FORMATTEST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*formattest_say_fn)(const char *line, void *ctx);

int formattest_run(const char *folder, formattest_say_fn say, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
