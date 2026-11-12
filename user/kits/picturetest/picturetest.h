/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* W5a's picture test (`picturetest.c`): every file `FOLDER/manifest.txt`
 * names, asked of zlib, libpng, libjpeg-turbo and libwebp, one line said
 * for each. The number of files read, or -1. */
#ifndef PICTURETEST_H
#define PICTURETEST_H

typedef void (*picturetest_say_fn)(const char *line, void *ctx);

int picturetest_run(const char *folder, picturetest_say_fn say, void *ctx);

#endif
