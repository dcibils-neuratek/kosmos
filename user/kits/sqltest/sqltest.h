/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* W5c's SQLite test (`sqltest.c`): the dotfile lock held between two
 * connections, and a database kept across a reboot. Each says one line. */
#ifndef SQLTEST_H
#define SQLTEST_H

typedef void (*sqltest_say_fn)(const char *line, void *ctx);

/* Two connections to `path`: the second is refused while the first holds
 * it, and let in once it lets go. */
void sqltest_locks(const char *path, sqltest_say_fn say, void *ctx);

/* `write`: a database at `path`, made afresh with rows whose fingerprint
 * is said; `read`: the same fingerprint, from what is there. */
void sqltest_keep(const char *path, int write, sqltest_say_fn say, void *ctx);

/* `sql` run on the database at `path`: "ok", or SQLite's message with its
 * extended code and the `errno` behind it. */
void sqltest_exec(const char *path, const char *sql, sqltest_say_fn say, void *ctx);

/* SQLite shut down and set as SQLite sets itself, before each program:
 * one library is shared by what would be two processes elsewhere. With
 * `temporary`, its temporary files go in `/Temporary`. */
void sqltest_fresh(int temporary);

/* SQLite's own programs, their `main`s renamed as they are built. */
int sqltest_fuzzcheck_main(int argc, char **argv);
int sqltest_speedtest1_main(int argc, char **argv);

#endif
