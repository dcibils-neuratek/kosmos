/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* The Media Kit's test (`mediatest.c`, `docs/media.md` M1): every file
 * `FOLDER/manifest.txt` names, opened, decoded and sought through the
 * kit, one line said for each. 0, or -1 when there is no manifest. */
#ifndef MEDIATEST_H
#define MEDIATEST_H

typedef void (*mediatest_say_fn)(const char *line, void *ctx);

int mediatest_run(const char *folder, mediatest_say_fn say, void *ctx);

#endif
