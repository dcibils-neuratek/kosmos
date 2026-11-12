/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **W5c's SQLite, in `/Home`** (`docs/w5.md`): the two things SQLite's own
 * programs do not ask, beside `fuzzcheck` and `speedtest1`, which do the
 * rest (`sqltest_kit.c`).
 *
 * - **The lock.** Kosmos keeps no locks on files (`sys/file.h`, `fcntl.h`),
 *   so SQLite's default here is `unix-dotfile`: a connection that writes
 *   makes `<database>.lock`, a directory, and the server makes it or
 *   refuses it atomically. Two connections, the second refused while the
 *   first holds the database and let in after.
 * - **A reboot.** Rows written in one boot and read in the next, with their
 *   fingerprint said both times.
 *
 * Ported code's test: it reaches `/Home` through the POSIX layer, which
 * is how SQLite reaches it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sqlite3.h"
#include "sqltest.h"

static void sayf(sqltest_say_fn say, void *ctx, const char *format, const char *a, int b)
{
    char line[512];

    snprintf(line, sizeof(line), format, a, b);
    say(line, ctx);
}

static int exec(sqlite3 *db, const char *sql)
{
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

void sqltest_locks(const char *path, sqltest_say_fn say, void *ctx)
{
    sqlite3 *a = NULL, *b = NULL;
    int made, held, refused, after;
    char lock[512];

    remove(path);
    snprintf(lock, sizeof(lock), "%s.lock", path);

    made = sqlite3_open(path, &a) == SQLITE_OK
        && exec(a, "CREATE TABLE t(x); INSERT INTO t VALUES(1)") == SQLITE_OK;
    if (!made || sqlite3_open(path, &b) != SQLITE_OK) {
        sayf(say, ctx, "sql-locks: %s: not opened (%d)", path, sqlite3_errcode(a));
        sqlite3_close(a);
        sqlite3_close(b);
        return;
    }

    /* A holds the database; B, asked not to wait, is refused. */
    held = exec(a, "BEGIN EXCLUSIVE; INSERT INTO t VALUES(2)") == SQLITE_OK;
    FILE *f = fopen(lock, "r");
    int lock_there = f != NULL;                 /* a directory opens to read */
    if (f)
        fclose(f);
    refused = exec(b, "INSERT INTO t VALUES(3)");

    /* A lets go; B is let in, and sees what A committed. */
    exec(a, "COMMIT");
    after = exec(b, "INSERT INTO t VALUES(3)");

    sqlite3_stmt *st;
    int rows = -1;
    if (sqlite3_prepare_v2(b, "SELECT count(*) FROM t", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW)
        rows = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    char line[512];
    snprintf(line, sizeof(line),
             "sql-locks: vfs %s; held %s, lock %s, second %s, then %s, rows %d",
             sqlite3_vfs_find(NULL)->zName, held ? "yes" : "no",
             lock_there ? "made" : "absent",
             refused == SQLITE_BUSY ? "busy" : sqlite3_errstr(refused),
             after == SQLITE_OK ? "in" : sqlite3_errstr(after), rows);
    say(line, ctx);
    sqlite3_close(a);
    sqlite3_close(b);
}

/* FNV-1a over every row, in order. */
static uint64_t fingerprint(sqlite3 *db, int *rows)
{
    sqlite3_stmt *st;
    uint64_t h = 0xcbf29ce484222325ull;

    *rows = 0;
    if (sqlite3_prepare_v2(db, "SELECT k, v FROM kept ORDER BY k", -1, &st, NULL) != SQLITE_OK)
        return 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 1);
        int64_t k = sqlite3_column_int64(st, 0);

        for (unsigned i = 0; i < sizeof(k); i++) {
            h ^= (unsigned char)(k >> (8 * i));
            h *= 0x100000001b3ull;
        }
        for (; v && *v; v++) {
            h ^= *v;
            h *= 0x100000001b3ull;
        }
        (*rows)++;
    }
    sqlite3_finalize(st);
    return h;
}

void sqltest_keep(const char *path, int write, sqltest_say_fn say, void *ctx)
{
    sqlite3 *db = NULL;
    int rows;
    char line[512];

    if (write) {
        remove(path);
        if (sqlite3_open(path, &db) != SQLITE_OK
            || exec(db, "CREATE TABLE kept(k INTEGER PRIMARY KEY, v TEXT);"
                        "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n"
                        " WHERE i < 5000)"
                        " INSERT INTO kept SELECT i, printf('row %d of five thousand, %x', i,"
                        " i*2654435761 % 4294967296) FROM n") != SQLITE_OK) {
            snprintf(line, sizeof(line), "sql-keep: not written: %s", sqlite3_errmsg(db));
            say(line, ctx);
            sqlite3_close(db);
            return;
        }
    } else if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        snprintf(line, sizeof(line), "sql-keep: not opened: %s", sqlite3_errmsg(db));
        say(line, ctx);
        sqlite3_close(db);
        return;
    }

    uint64_t h = fingerprint(db, &rows);
    sqlite3_stmt *st;
    const char *check = "?";
    char ok[64] = "";
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        snprintf(ok, sizeof(ok), "%s", (const char *)sqlite3_column_text(st, 0));
        check = ok;
    }
    sqlite3_finalize(st);
    snprintf(line, sizeof(line), "sql-keep: %d rows, %016llx, integrity %s", rows,
             (unsigned long long)h, check);
    say(line, ctx);
    sqlite3_close(db);
}

/* SQLite's own account of the last thing that went wrong - which call,
 * on what - for `sqltest_exec` to say. */
static char logged[256];

static void log_line(void *unused, int code, const char *message)
{
    (void)unused;
    snprintf(logged, sizeof(logged), "%d: %s", code, message);
}

void sqltest_exec(const char *path, const char *sql, sqltest_say_fn say, void *ctx)
{
    sqlite3 *db = NULL;
    char *message = NULL;
    char line[768];

    logged[0] = 0;                      /* SQLite's log: `sqltest_fresh` */
    int rc = sqlite3_open(path, &db);

    if (rc == SQLITE_OK) {
        rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    }
    if (rc == SQLITE_OK) {
        snprintf(line, sizeof(line), "sql-exec: ok");
    } else {
        snprintf(line, sizeof(line), "sql-exec: %s (extended code %d, errno %d; %s)",
                 message ? message : sqlite3_errmsg(db), sqlite3_extended_errcode(db),
                 sqlite3_system_errno(db), logged[0] ? logged : "nothing logged");
    }
    sqlite3_free(message);
    sqlite3_close(db);
    say(line, ctx);
}

/*
 * **A fresh SQLite, as a process of its own would have it** (`docs/w5.md`
 * W5c). On this Mac `fuzzcheck` and `speedtest1` are two processes; here
 * they are two calls in one, sharing one library, and what one configures
 * outlives its `main`: `fuzzcheck` gives SQLite a heap it allocated
 * (`SQLITE_CONFIG_HEAP`, `memsys5`) and frees, and `speedtest1` then
 * allocated from it (`memsys5Unlink`, a page that was gone). So before each
 * program the library is shut down and every setting either program
 * changes goes back to SQLite's own default: the allocator, saved the
 * first time before anything changed it; no page cache; the lookaside
 * (`SQLITE_DEFAULT_LOOKASIDE`); serialised threads, as a
 * `SQLITE_THREADSAFE=1` build starts; memory statistics on; URIs off.
 */
void sqltest_fresh(int temporary)
{
    static sqlite3_mem_methods saved;
    static int have;

    sqlite3_shutdown();
    if (!have) {
        sqlite3_config(SQLITE_CONFIG_GETMALLOC, &saved);
        have = 1;
    }
    sqlite3_config(SQLITE_CONFIG_MALLOC, &saved);
    sqlite3_config(SQLITE_CONFIG_PAGECACHE, (void *)0, 0, 0);
    sqlite3_config(SQLITE_CONFIG_LOOKASIDE, 1200, 40);
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);
    sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 1);
    sqlite3_config(SQLITE_CONFIG_URI, 0);
    sqlite3_config(SQLITE_CONFIG_LOG, log_line, (void *)0);

    /*
     * **Temporary files in `/Temporary`.** SQLite looks for a place in
     * `SQLITE_TMPDIR` and `TMPDIR`, then `/var/tmp`, `/usr/tmp` and `/tmp`,
     * then the working directory; Kosmos has no environment (`getenv`,
     * `misc_user.c`) and none of those paths, and a program's working
     * directory is `/`, which is no mount. So `VACUUM` of anything too big
     * for memory failed with "disk I/O error". `sqlite3_temp_directory` is
     * SQLite's own door for this, and asked of first; set here last, since
     * `sqlite3_mprintf` starts the library, and from the allocator just put
     * back - the one before may be a heap that is gone. The last one is
     * left, a few bytes from a heap that may no longer be there. Not for
     * `fuzzcheck`, which will not start while SQLite holds any memory at
     * all, and whose databases are in memory.
     */
    sqlite3_temp_directory = temporary ? sqlite3_mprintf("%s", "/Temporary") : (char *)0;
}

#ifdef SQLTEST_HOST
static void print_line(const char *line, void *ctx)
{
    (void)ctx;
    puts(line);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: sqltest locks|write|read PATH\n");
        return 2;
    }
    if (!strcmp(argv[1], "locks"))
        sqltest_locks(argv[2], print_line, NULL);
    else
        sqltest_keep(argv[2], !strcmp(argv[1], "write"), print_line, NULL);
    return 0;
}
#endif
