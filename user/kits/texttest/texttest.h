/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* W5b's text test (`texttest.c`): HarfBuzz given every case in
 * `FOLDER/cases.txt` and FreeType every font in `FOLDER/fonts.txt`, one
 * line said for each test file and each font. The number of cases run,
 * or -1. */
#ifndef TEXTTEST_H
#define TEXTTEST_H

typedef void (*texttest_say_fn)(const char *line, void *ctx);

int texttest_run(const char *folder, texttest_say_fn say, void *ctx);

#endif
