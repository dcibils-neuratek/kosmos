/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The POSIX layer's own checks** (`docs/w3.md`), in an image of their own,
 * `posixtest.elf`, as `cxxtest.elf` holds C++'s. Written the way a port
 * would write them - `<pthread.h>`, nothing of Kosmos's - so what is held
 * is what WebKit and curl will see.
 *
 * `kosmos_posixtest_run` writes one sentence: every check's result in
 * order when all of them held, or the first that did not and why.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* The one Kosmos call here: how many threads the kernel has handed out,
 * to see that detached threads are collected. */
long kosmos_posixtest_fresh_index(void);

/* And whether the process table names process `id` `name`. */
int kosmos_posixtest_named(long id, const char *name);

#define WORKERS     8
#define DETACHED    1000
#define COUNT_EACH  20000

static pthread_mutex_t counted_lock = PTHREAD_MUTEX_INITIALIZER;
static long counted;

static void *counts(void *arg)
{
    int i;

    (void)arg;

    for (i = 0; i < COUNT_EACH; i++) {
        pthread_mutex_lock(&counted_lock);
        counted++;
        pthread_mutex_unlock(&counted_lock);
    }

    return NULL;
}

/* A token handed round a ring of threads under one mutex and condition. */
#define RING       4
#define RING_PASSES 1000

static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_turn = PTHREAD_COND_INITIALIZER;
static int ring_holder;
static int ring_passes;

static void *passes(void *arg)
{
    int me = (int)(intptr_t)arg;

    pthread_mutex_lock(&ring_lock);

    for (;;) {
        while (ring_holder != me && ring_passes < RING_PASSES) {
            pthread_cond_wait(&ring_turn, &ring_lock);
        }

        if (ring_passes >= RING_PASSES) {
            break;
        }

        ring_passes++;
        ring_holder = (me + 1) % RING;
        pthread_cond_broadcast(&ring_turn);
    }

    pthread_mutex_unlock(&ring_lock);
    return NULL;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static volatile int once_ran;

static void run_once(void)
{
    once_ran++;
    sched_yield();                  /* the others arrive while it runs */
}

static void *calls_once(void *arg)
{
    (void)arg;
    pthread_once(&once, run_once);
    return NULL;
}

static pthread_key_t key;
static volatile int destroyed;

static void destroy(void *value)
{
    if (value == (void *)&destroyed) {
        __atomic_add_fetch(&destroyed, 1, __ATOMIC_RELAXED);
    }
}

static void *sets_a_key(void *arg)
{
    (void)arg;
    pthread_setspecific(key, (void *)&destroyed);
    return pthread_getspecific(key) == &destroyed ? (void *)1 : NULL;
}

static pthread_mutex_t recursive;

static void *tries(void *arg)
{
    (void)arg;
    return pthread_mutex_trylock(&recursive) == EBUSY ? (void *)1 : NULL;
}

/* Whether this thread's own local is inside the stack it is told it has. */
static int my_stack_holds_me(void)
{
    pthread_attr_t a;
    void *low;
    size_t size;
    char here;

    if (pthread_getattr_np(pthread_self(), &a) != 0
        || pthread_attr_getstack(&a, &low, &size) != 0) {
        return 0;
    }

    return (uintptr_t)&here >= (uintptr_t)low && (uintptr_t)&here < (uintptr_t)low + size;
}

static void *checks_stack(void *arg)
{
    (void)arg;
    return my_stack_holds_me() ? (void *)1 : NULL;
}

static void *ends_at_once(void *arg)
{
    return arg;
}

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static volatile int inside, most_inside, writer_alone = 1, rw_done;

static void *reads(void *arg)
{
    int i;

    (void)arg;

    for (i = 0; i < 200; i++) {
        int n;

        pthread_rwlock_rdlock(&rw);
        n = __atomic_add_fetch(&inside, 1, __ATOMIC_ACQ_REL);

        if (n > most_inside) {
            most_inside = n;
        }

        sched_yield();
        __atomic_sub_fetch(&inside, 1, __ATOMIC_ACQ_REL);
        pthread_rwlock_unlock(&rw);
    }

    __atomic_add_fetch(&rw_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *writes(void *arg)
{
    int i;

    (void)arg;

    for (i = 0; i < 100; i++) {
        pthread_rwlock_wrlock(&rw);

        if (__atomic_load_n(&inside, __ATOMIC_ACQUIRE) != 0) {
            writer_alone = 0;
        }

        sched_yield();
        pthread_rwlock_unlock(&rw);
    }

    __atomic_add_fetch(&rw_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* A pause, on a timed wait nobody signals: `nanosleep` is W3c's. */
static void pause_ms(int ms)
{
    pthread_condattr_t ca;
    pthread_cond_t c;
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    struct timespec until;

    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&c, &ca);
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_nsec += (long)ms * 1000000L;
    until.tv_sec += until.tv_nsec / 1000000000L;
    until.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&m);
    (void)pthread_cond_timedwait(&c, &m, &until);
    pthread_mutex_unlock(&m);
}

#define SAY(...) do { snprintf(out, size, __VA_ARGS__); return 1; } while (0)

/*
 * **Memory** (`docs/w3.md` W3b): what bmalloc and JavaScriptCore do with
 * it - a large range reserved with no access, pages in it given access as
 * they are used, given back with their access kept, and pieces unmapped.
 */
#define GB4 (4UL * 1024 * 1024 * 1024)

static int memory_checks(char *out, unsigned long size)
{
    unsigned char *big, *page, *small, *f;
    long ps = sysconf(_SC_PAGESIZE);
    unsigned long i;

    /* 11: the page size, and 4 GB with no access - addresses, not pages */
    if (ps != 4096 || getpagesize() != 4096) {
        SAY("posix: the page size is %ld", ps);
    }

    big = mmap(NULL, GB4, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    if (big == MAP_FAILED) {
        SAY("posix: 4 GB with no access was refused (%d)", errno);
    }

    /* 12: three pages in the middle made writable, and used */
    page = big + GB4 / 2;

    if (mprotect(page, 3 * 4096, PROT_READ | PROT_WRITE) != 0) {
        SAY("posix: mprotect to read and write failed (%d)", errno);
    }

    for (i = 0; i < 3 * 4096; i++) {
        if (page[i] != 0) {
            SAY("posix: a page made writable did not read zero");
        }

        page[i] = (unsigned char)(i * 7);
    }

    /* 13: read only, and still what was written */
    if (mprotect(page, 4096, PROT_READ) != 0 || page[1] != 7 || page[4095] != (unsigned char)(4095 * 7)) {
        SAY("posix: mprotect to read only lost what the page held (%d)", errno);
    }

    /* 14: MADV_DONTNEED - the second page reads zero, and is writable still */
    if (madvise(page + 4096, 4096, MADV_DONTNEED) != 0) {
        SAY("posix: madvise(MADV_DONTNEED) failed (%d)", errno);
    }

    if (page[4096] != 0 || page[8191] != 0) {
        SAY("posix: a page given back with MADV_DONTNEED did not read zero");
    }

    page[4096] = 0x42;

    if (page[4096] != 0x42 || page[2 * 4096 + 1] != (unsigned char)((2 * 4096 + 1) * 7)) {
        SAY("posix: after MADV_DONTNEED the pages beside it were not as they were");
    }

    /* 15: the middle page unmapped, and the two around it still there */
    if (munmap(page + 4096, 4096) != 0) {
        SAY("posix: munmap of a middle page failed (%d)", errno);
    }

    if (page[1] != 7 || page[2 * 4096 + 1] != (unsigned char)((2 * 4096 + 1) * 7)) {
        SAY("posix: the pages around an unmapped one lost what they held");
    }

    page[2 * 4096] = 9;

    /* 16: the refusals - writable and executable, a file, the unmapped
     * middle protected, an address that was never this process's */
    if (mmap(NULL, 4096, PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED
        || errno != EACCES) {
        SAY("posix: writable and executable at once was not refused with EACCES");
    }

    if (mmap(NULL, 4096, PROT_READ, MAP_SHARED, 3, 0) != MAP_FAILED || errno != ENODEV) {
        SAY("posix: a file shared was not refused with ENODEV");
    }

    if (mprotect(page, 3 * 4096, PROT_READ) == 0 || errno != ENOMEM) {
        SAY("posix: mprotect across an unmapped page was not refused with ENOMEM");
    }

    /* ...and what is not one: no bytes protected or advised, as on Linux,
     * which JavaScriptCore's resizable buffers ask for (`testing.md` 18.780) */
    if (mprotect(page, 0, PROT_NONE) != 0 || madvise(page, 0, MADV_DONTNEED) != 0) {
        SAY("posix: mprotect or madvise of no bytes was refused (%d)", errno);
    }

    if (munmap((void *)(uintptr_t)0x1000, 4096) == 0 || errno != EINVAL) {
        SAY("posix: munmap of an address never mapped was not refused with EINVAL");
    }

    /* 17: MAP_FIXED over what the process has - zeroed, and the new access */
    f = mmap(page + 2 * 4096, 4096, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    if (f != page + 2 * 4096 || f[0] != 0) {
        SAY("posix: MAP_FIXED over a page of its own did not give it new and zeroed");
    }

    f[0] = 1;

    /* 18: an ordinary mapping, zeroed and writable throughout */
    small = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (small == MAP_FAILED) {
        SAY("posix: 64 KB read and write was refused (%d)", errno);
    }

    for (i = 0; i < 65536; i += 512) {
        if (small[i] != 0) {
            SAY("posix: a new mapping did not read zero");
        }

        small[i] = 1;
    }

    if (munmap(small, 65536) != 0 || munmap(big, GB4) != 0) {
        SAY("posix: munmap of a whole mapping failed (%d)", errno);
    }

    snprintf(out, size, "memory: 4 GB reserved, protected, discarded, unmapped "
             "in the middle, fixed, refused");
    return 0;
}

/*
 * **Time and the process's own facts** (`docs/w3.md` W3c).
 */
static long long ms_between(const struct timespec *a, const struct timespec *b)
{
    return ((long long)(b->tv_sec - a->tv_sec) * 1000000000LL + (b->tv_nsec - a->tv_nsec))
           / 1000000LL;
}

static int process_checks(char *out, unsigned long size)
{
    struct timespec start, end, bad = { 0, 1000000000L };
    struct timeval tv;
    struct utsname u;
    unsigned char a[32], b[32];
    long long took;
    long online, conf;

    /* 19: a 20 ms sleep is at least that, and not ten times it */
    clock_gettime(CLOCK_MONOTONIC, &start);
    {
        struct timespec t = { 0, 20000000L };

        if (nanosleep(&t, NULL) != 0) {
            SAY("posix: nanosleep of 20 ms failed (%d)", errno);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    took = ms_between(&start, &end);

    if (took < 20 || took > 2000) {
        SAY("posix: a 20 ms nanosleep took %lld ms", took);
    }

    if (nanosleep(&bad, NULL) == 0 || errno != EINVAL) {
        SAY("posix: nanosleep of a billion nanoseconds was not refused with EINVAL");
    }

    /* 20: usleep, 5 ms */
    clock_gettime(CLOCK_MONOTONIC, &start);
    usleep(5000);
    clock_gettime(CLOCK_MONOTONIC, &end);

    if (ms_between(&start, &end) < 5) {
        SAY("posix: a 5 ms usleep took %lld ms", ms_between(&start, &end));
    }

    /* 21: gettimeofday is the realtime clock */
    clock_gettime(CLOCK_REALTIME, &start);

    if (gettimeofday(&tv, NULL) != 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000
        || tv.tv_sec < start.tv_sec || tv.tv_sec > start.tv_sec + 2) {
        SAY("posix: gettimeofday said %ld s against the clock's %ld s",
            (long)tv.tv_sec, (long)start.tv_sec);
    }

    /* 22: getpid is the id the process table shows for this process */
    if (getpid() <= 0 || !kosmos_posixtest_named(getpid(), "posixtest")) {
        SAY("posix: getpid said %d, which the process table does not call posixtest",
            (int)getpid());
    }

    if (getuid() != 0 || geteuid() != 0 || getgid() != 0 || getegid() != 0) {
        SAY("posix: somebody other than 0");
    }

    /* 23: the processors */
    online = sysconf(_SC_NPROCESSORS_ONLN);
    conf = sysconf(_SC_NPROCESSORS_CONF);

    if (online < 1 || conf < online) {
        SAY("posix: sysconf counted %ld processors online of %ld", online, conf);
    }

    /* 24: uname */
    if (uname(&u) != 0 || strcmp(u.sysname, "Kosmos") != 0 || strchr(u.release, '.') == NULL
#if defined(__x86_64__)
        || strcmp(u.machine, "x86_64") != 0
#else
        || strcmp(u.machine, "aarch64") != 0
#endif
        ) {
        SAY("posix: uname said %s %s on %s", u.sysname, u.release, u.machine);
    }

    /* 25: random bytes, twice different, and getentropy's limit */
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);

    if (getrandom(a, sizeof a, 0) != (ssize_t)sizeof a || getentropy(b, sizeof b) != 0
        || memcmp(a, b, sizeof a) == 0) {
        SAY("posix: getrandom and getentropy gave the same 32 bytes");
    }

    if (getentropy(b, 257) == 0 || errno != EIO) {
        SAY("posix: getentropy of 257 bytes was not refused with EIO");
    }

    /* 26: there is no environment */
    if (getenv("HOME") != NULL || getenv("PATH") != NULL) {
        SAY("posix: an environment, where there is none");
    }

    snprintf(out, size, "process: slept, timed, pid in the table, %ld processors, %s %s, "
             "random, no environment", online, u.sysname, u.machine);
    return 0;
}


/*
 * **Files** (`docs/w3.md` W3d2): descriptors on the Storage Kit, in one
 * folder of the disk and one of memory, as a port uses them.
 */
#define FSAY(...) do { snprintf(out, size, __VA_ARGS__); return 1; } while (0)

static int files_in(const char *dir, char *out, unsigned long size)
{
    char path[PATH_MAX], sub[PATH_MAX], moved[PATH_MAX], got[64];
    struct stat st;
    int fd, fd2, d;
    ssize_t n = 0;

    snprintf(path, sizeof path, "%s/px-file", dir);
    snprintf(sub, sizeof sub, "%s/px-dir", dir);
    snprintf(moved, sizeof moved, "%s/px-dir/px-file", dir);
    unlink(moved);
    rmdir(sub);
    unlink(path);

    if (open(path, O_RDONLY) >= 0 || errno != ENOENT) {
        FSAY("files: %s: a file not there opened, or not ENOENT (%d)", dir, errno);
    }

    if ((fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644)) < 3) {
        FSAY("files: %s: O_CREAT | O_EXCL gave %d (%d)", dir, fd, errno);
    }

    if (open(path, O_RDWR | O_CREAT | O_EXCL, 0644) >= 0 || errno != EEXIST) {
        FSAY("files: %s: a second O_EXCL was not EEXIST (%d)", dir, errno);
    }

    /* Written in two pieces, the offset following. */
    if (write(fd, "hello", 5) != 5 || write(fd, " world", 6) != 6
        || lseek(fd, 0, SEEK_CUR) != 11) {
        FSAY("files: %s: two writes left the offset at %ld", dir, (long)lseek(fd, 0, SEEK_CUR));
    }

    if (lseek(fd, 0, SEEK_SET) != 0 || (n = read(fd, got, sizeof got)) != 11
        || memcmp(got, "hello world", 11) != 0 || read(fd, got, sizeof got) != 0) {
        FSAY("files: %s: read back %ld bytes", dir, (long)n);
    }

    /* At a place, the offset untouched. */
    if (pwrite(fd, "J", 1, 6) != 1 || pread(fd, got, 1, 6) != 1 || got[0] != 'J'
        || lseek(fd, 0, SEEK_CUR) != 11) {
        FSAY("files: %s: pwrite and pread at 6", dir);
    }

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 11) {
        FSAY("files: %s: fstat said mode %o, %ld bytes", dir, st.st_mode, (long)st.st_size);
    }

    {
        struct stat by_name;

        if (stat(path, &by_name) != 0 || by_name.st_ino != st.st_ino || by_name.st_size != 11) {
            FSAY("files: %s: stat and fstat disagree", dir);
        }
    }

    if (ftruncate(fd, 5) != 0 || fstat(fd, &st) != 0 || st.st_size != 5) {
        FSAY("files: %s: ftruncate to 5 left %ld bytes", dir, (long)st.st_size);
    }

    /* Appending through a second descriptor, at the end as it is. */
    if ((fd2 = open(path, O_WRONLY | O_APPEND)) < 0 || write(fd2, "!", 1) != 1
        || pread(fd, got, sizeof got, 0) != 6 || memcmp(got, "hello!", 6) != 0) {
        FSAY("files: %s: O_APPEND did not write at the end", dir);
    }

    if (lseek(fd, 0, SEEK_END) != 6) {
        FSAY("files: %s: SEEK_END was not 6", dir);
    }

    /* Mapped privately (`docs/w5.md` W5e): the file's six bytes, the rest
     * of the page zero, and a write to the page reaching only the page. */
    {
        unsigned char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        unsigned char *r = mmap(NULL, 2 * 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        int zero = 1;

        if (m == MAP_FAILED || memcmp(m, "hello!", 6) != 0) {
            FSAY("files: %s: a file mapped privately did not hold its bytes (%d)", dir, errno);
        } else {
            for (unsigned i = 6; i < 4096; i++) {
                zero &= m[i] == 0;
            }
            m[0] = 'J';
            if (!zero || pread(fd, got, 6, 0) != 6 || memcmp(got, "hello!", 6) != 0) {
                FSAY("files: %s: past a mapped file's end was not zero, or a write reached it", dir);
            }
            munmap(m, 4096);
        }

        if (r == MAP_FAILED || r[0] != 'h' || r[4096] != 0
            || mprotect(r, 4096, PROT_READ) != 0) {
            FSAY("files: %s: a file mapped read only, two pages (%d)", dir, errno);
        } else {
            munmap(r, 2 * 4096);
        }

        if (mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd2, 0) != MAP_FAILED || errno != EBADF) {
            FSAY("files: %s: a write-only descriptor mapped, or not EBADF (%d)", dir, errno);
        }
    }

    if (read(fd2, got, 1) >= 0 || errno != EBADF) {
        FSAY("files: %s: a read of a write-only descriptor was not EBADF", dir);
    }

    close(fd2);

    /* dup shares the offset; dup2 puts it where it is asked. */
    if ((d = dup(fd)) < 3 || lseek(d, 2, SEEK_SET) != 2 || lseek(fd, 0, SEEK_CUR) != 2) {
        FSAY("files: %s: dup did not share the offset", dir);
    }

    close(d);

    if (dup2(fd, 77) != 77 || pread(77, got, 2, 0) != 2 || memcmp(got, "he", 2) != 0
        || close(77) != 0) {
        FSAY("files: %s: dup2 to 77", dir);
    }

    if ((fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDWR || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0
        || fcntl(fd, F_GETFD) != FD_CLOEXEC) {
        FSAY("files: %s: fcntl's flags", dir);
    }

    if (close(fd) != 0 || read(fd, got, 1) >= 0 || errno != EBADF || close(fd) == 0) {
        FSAY("files: %s: a closed descriptor still answered", dir);
    }

    if ((fd = open(path, O_WRONLY | O_TRUNC)) < 0 || fstat(fd, &st) != 0 || st.st_size != 0
        || write(fd, "x", 1) != 1 || close(fd) != 0) {
        FSAY("files: %s: O_TRUNC left %ld bytes", dir, (long)st.st_size);
    }

    if ((fd = open(path, O_RDONLY)) < 0 || write(fd, "y", 1) >= 0 || errno != EBADF) {
        FSAY("files: %s: a write to a read-only descriptor was not EBADF", dir);
    }

    close(fd);

    /* Folders. */
    if (mkdir(sub, 0755) != 0 || mkdir(sub, 0755) == 0 || errno != EEXIST) {
        FSAY("files: %s: mkdir twice (%d)", dir, errno);
    }

    if (open(sub, O_WRONLY) >= 0 || errno != EISDIR) {
        FSAY("files: %s: a folder opened for writing", dir);
    }

    if (open(path, O_RDONLY | O_DIRECTORY) >= 0 || errno != ENOTDIR) {
        FSAY("files: %s: a file opened as a folder", dir);
    }

    if (rename(path, moved) != 0 || access(path, F_OK) == 0 || access(moved, R_OK | W_OK) != 0) {
        FSAY("files: %s: rename into the folder (%d)", dir, errno);
    }

    {
        DIR *folder = opendir(sub);
        struct dirent *e;
        int names = 0, mine = 0;

        if (folder == NULL) {
            FSAY("files: %s: opendir (%d)", dir, errno);
        }

        while ((e = readdir(folder)) != NULL) {
            names++;
            mine += strcmp(e->d_name, "px-file") == 0;
        }

        closedir(folder);

        if (names != 1 || mine != 1) {
            FSAY("files: %s: readdir gave %d names", dir, names);
        }
    }

    if (rmdir(sub) == 0 || errno != ENOTEMPTY) {
        FSAY("files: %s: rmdir of a folder with a file in it (%d)", dir, errno);
    }

    if (unlink(sub) == 0 || errno != EISDIR || rmdir(moved) == 0 || errno != ENOTDIR) {
        FSAY("files: %s: unlink of a folder, or rmdir of a file", dir);
    }

    if (unlink(moved) != 0 || rmdir(sub) != 0 || stat(sub, &st) == 0 || errno != ENOENT) {
        FSAY("files: %s: the file and the folder were not removed", dir);
    }

    /* Unlinked while open, as SQLite's temporary files are (`docs/w5.md`
     * W5c): still read, written, measured and cut through the descriptor,
     * and gone once it is closed. */
    {
        static char block[65536];
        int fdu;

        for (unsigned i = 0; i < sizeof block; i++) {
            block[i] = (char)(i * 7);
        }
        if ((fdu = open(path, O_RDWR | O_CREAT | O_EXCL, 0600)) < 3 || unlink(path) != 0) {
            FSAY("files: %s: open then unlink (%d)", dir, errno);
        }
        if (write(fdu, block, sizeof block) != (ssize_t)sizeof block
            || pwrite(fdu, "Z", 1, 70000) != 1) {
            FSAY("files: %s: a write after unlink (%d)", dir, errno);
        }
        if (fstat(fdu, &st) != 0 || st.st_size != 70001) {
            FSAY("files: %s: fstat after unlink: %ld bytes (%d)", dir, (long)st.st_size, errno);
        }
        if (pread(fdu, got, 8, 65528) != 8 || memcmp(got, block + 65528, 8) != 0
            || ftruncate(fdu, 4096) != 0 || fstat(fdu, &st) != 0 || st.st_size != 4096
            || fsync(fdu) != 0) {
            FSAY("files: %s: read, cut or flushed after unlink (%d)", dir, errno);
        }
        /* Record locks are refused, as `flock` is - nothing keeps one
         * between processes (`fcntl.h`) - and `fchown`, like `fchmod`,
         * changes nothing and says yes. */
        {
            struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };

            if (fcntl(fdu, F_SETLK, &fl) != -1 || errno != ENOTSUP
                || fcntl(fdu, F_GETLK, &fl) != -1 || errno != ENOTSUP
                || fcntl(-1, F_SETLK, &fl) != -1 || errno != EBADF
                || fchown(fdu, 0, 0) != 0) {
                FSAY("files: %s: record locks or fchown (%d)", dir, errno);
            }
        }
        if (close(fdu) != 0 || stat(path, &st) == 0 || errno != ENOENT) {
            FSAY("files: %s: an unlinked file outlived its last close", dir);
        }
    }

    if (fsync(1) != 0) {
        FSAY("files: fsync of the console");
    }

    return 0;
}

#define FILE_THREADS 4

static void *uses_files(void *arg)
{
    intptr_t me = (intptr_t)arg;
    char path[64], mine[300], back[300];
    int round, fd;

    snprintf(path, sizeof path, "/Temporary/px-thread-%d", (int)me);

    for (round = 0; round < 20; round++) {
        memset(mine, (int)('a' + me + round), sizeof mine);

        if ((fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0
            || write(fd, mine, sizeof mine) != (ssize_t)sizeof mine
            || lseek(fd, 0, SEEK_SET) != 0
            || read(fd, back, sizeof back) != (ssize_t)sizeof back
            || memcmp(mine, back, sizeof mine) != 0 || close(fd) != 0) {
            return (void *)(intptr_t)(round + 1);
        }
    }

    unlink(path);
    return NULL;
}

static int file_checks(char *out, unsigned long size)
{
    char where[PATH_MAX], *made;
    int fds[200], i;
    struct stat st;

    if (files_in("/Temporary", out, size) != 0 || files_in("/Home/apps/apptest", out, size) != 0) {
        return 1;
    }

    /* Read only where it is. */
    if (access("/Kosmos/Programs", R_OK) != 0 || access("/Kosmos/Programs", W_OK) == 0
        || errno != EROFS) {
        FSAY("files: /Kosmos was not read only (%d)", errno);
    }

    /* Where this process is, and paths made whole. */
    if (getcwd(where, sizeof where) == NULL || where[0] != '/'
        || getcwd(where, 1) != NULL || errno != ERANGE) {
        FSAY("files: getcwd");
    }

    if ((made = realpath("/Temporary/../Temporary/./", NULL)) == NULL
        || strcmp(made, "/Temporary") != 0) {
        FSAY("files: realpath made \"%s\"", made != NULL ? made : "(nothing)");
    }

    free(made);

    if (realpath("/Temporary/px-none", where) != NULL || errno != ENOENT) {
        FSAY("files: realpath of nothing");
    }

    if (open("/Nowhere/x", O_RDONLY) >= 0 || errno != ENOENT) {
        FSAY("files: a path under no mount opened");
    }

    /* The console. */
    if (write(1, "", 0) != 0 || read(0, where, 1) != 0 || fstat(2, &st) != 0
        || lseek(1, 0, SEEK_SET) >= 0 || errno != ESPIPE) {
        FSAY("files: the console's descriptors");
    }

    /* Two hundred open at once, the table grown, each its own. */
    {
        int fd = open("/Temporary/px-many", O_RDWR | O_CREAT, 0644);

        if (fd < 0) {
            FSAY("files: px-many (%d)", errno);
        }

        close(fd);
    }

    for (i = 0; i < 200; i++) {
        if ((fds[i] = open("/Temporary/px-many", O_RDONLY)) < 3
            || (i > 0 && fds[i] == fds[i - 1])) {
            FSAY("files: the %dth descriptor was %d (%d)", i + 1, fds[i], errno);
        }
    }

    for (i = 0; i < 200; i++) {
        if (close(fds[i]) != 0) {
            FSAY("files: closing descriptor %d", fds[i]);
        }
    }

    unlink("/Temporary/px-many");

    {
        pthread_t t[FILE_THREADS];
        void *got;
        intptr_t k;

        for (k = 0; k < FILE_THREADS; k++) {
            pthread_create(&t[k], NULL, uses_files, (void *)k);
        }

        for (k = 0; k < FILE_THREADS; k++) {
            pthread_join(t[k], &got);

            if (got != NULL) {
                FSAY("files: thread %ld's file was not as written, round %ld", (long)k,
                     (long)(intptr_t)got);
            }
        }
    }

    snprintf(out, size, "files: made, excl, read and written, positioned, appended, cut, "
             "dup shared, flags, closed, folders listed and removed, read only, cwd, "
             "realpath, console, 200 open, %d threads", FILE_THREADS);
    return 0;
}


/*
 * **Streams** (`docs/w3.md` W3d3): `stdio` over descriptors.
 */
static int stdio_checks(char *out, unsigned long size)
{
    const char *path = "/Temporary/ps-file";
    char line[128], *got = NULL;
    size_t room = 0;
    unsigned char *big, *back;
    struct stat st;
    FILE *f;
    int i, a = 0, b = 0, lines = 0;

    unlink(path);

    if (fopen("/Temporary/ps-none", "r") != NULL || errno != ENOENT) {
        FSAY("stdio: a file not there opened (%d)", errno);
    }

    /* A thousand lines, several buffers' worth, and back by line. */
    if ((f = fopen(path, "w")) == NULL) {
        FSAY("stdio: fopen for writing (%d)", errno);
    }

    for (i = 0; i < 1000; i++) {
        if (fprintf(f, "line %d of %s\n", i, "a thousand") <= 0) {
            FSAY("stdio: fprintf of line %d", i);
        }
    }

    if (fclose(f) != 0) {
        FSAY("stdio: fclose after writing");
    }

    if ((f = fopen(path, "r")) == NULL) {
        FSAY("stdio: fopen for reading (%d)", errno);
    }

    while (fgets(line, sizeof line, f) != NULL) {
        char want[64];

        snprintf(want, sizeof want, "line %d of a thousand\n", lines);

        if (strcmp(line, want) != 0) {
            FSAY("stdio: line %d read as \"%s\"", lines, line);
        }

        lines++;
    }

    if (lines != 1000 || !feof(f) || ferror(f)) {
        FSAY("stdio: %d lines read back", lines);
    }

    /* Positioned, scanned, given back. */
    if (fseek(f, 5, SEEK_SET) != 0 || ftell(f) != 5 || fscanf(f, "%d of", &a) != 1 || a != 0) {
        FSAY("stdio: fscanf at 5 said %d", a);
    }

    if (fseek(f, -9, SEEK_END) != 0 || fgets(line, sizeof line, f) == NULL
        || strcmp(line, "thousand\n") != 0) {
        FSAY("stdio: fseek from the end read \"%s\"", line);
    }

    rewind(f);

    if (getc(f) != 'l' || ungetc('L', f) != 'L' || getc(f) != 'L' || getc(f) != 'i'
        || ftell(f) != 2) {
        FSAY("stdio: ungetc");
    }

    rewind(f);

    for (i = 0; getline(&got, &room, f) > 0; i++) {
        if (i == 999 && strcmp(got, "line 999 of a thousand\n") != 0) {
            FSAY("stdio: getline's last line \"%s\"", got);
        }
    }

    free(got);
    fclose(f);

    if (i != 1000) {
        FSAY("stdio: getline gave %d lines", i);
    }

    /* Changed in place, and added to. */
    if ((f = fopen(path, "r+")) == NULL || fseek(f, 5, SEEK_SET) != 0
        || fwrite("X", 1, 1, f) != 1 || ftell(f) != 6 || fseek(f, 0, SEEK_SET) != 0
        || fgets(line, sizeof line, f) == NULL || strcmp(line, "line X of a thousand\n") != 0
        || fclose(f) != 0) {
        FSAY("stdio: r+ changed line 0 to \"%s\"", line);
    }

    if ((f = fopen(path, "a")) == NULL || fputs("the end\n", f) != 0 || fclose(f) != 0
        || stat(path, &st) != 0) {
        FSAY("stdio: appending");
    }

    if ((f = fopen(path, "r")) == NULL || fseek(f, -8, SEEK_END) != 0
        || fgets(line, sizeof line, f) == NULL || strcmp(line, "the end\n") != 0) {
        FSAY("stdio: the appended line read \"%s\"", line);
    }

    fclose(f);

    if (fopen(path, "wx") != NULL || errno != EEXIST) {
        FSAY("stdio: \"wx\" over a file (%d)", errno);
    }

    /* Ten thousand bytes in one call each way. */
    big = malloc(10000);
    back = malloc(10000);

    for (i = 0; i < 10000; i++) {
        big[i] = (unsigned char)(i * 13 + 1);
    }

    if ((f = fopen(path, "w+")) == NULL || fwrite(big, 1, 10000, f) != 10000
        || fseek(f, 0, SEEK_SET) != 0 || fread(back, 1, 10000, f) != 10000
        || memcmp(big, back, 10000) != 0 || fread(back, 1, 1, f) != 0 || !feof(f)) {
        FSAY("stdio: ten thousand bytes in one call");
    }

    fclose(f);
    free(big);
    free(back);

    /* A descriptor's own stream. */
    {
        int fd = open(path, O_RDONLY);

        if (fd < 0 || (f = fdopen(fd, "r")) == NULL || fileno(f) != fd
            || fscanf(f, "%d%d", &a, &b) == EOF) {
            FSAY("stdio: fdopen");
        }

        fclose(f);

        if (close(fd) == 0) {
            FSAY("stdio: fclose left fdopen's descriptor open");
        }
    }

    /* **A scan that reaches the end says so** (`docs/w5.md` W5c): records
     * read with `fscanf` until it stops, and then `feof` tells a file that
     * ended from one that did not parse - libxml2's `runtest` asks exactly
     * that, and was told "invalid format" at the end of every file. */
    {
        static const char *const files[] = { "1 a\n2 b\n\n", "1 a\n2 b\n2" };
        char word[8];

        for (int i = 0; i < 2; i++) {
            int fd = open("/Temporary/posixtest-scan", O_RDWR | O_CREAT | O_TRUNC, 0644);
            int records = 0;

            if (fd < 0 || write(fd, files[i], strlen(files[i])) != (ssize_t)strlen(files[i])
                || lseek(fd, 0, SEEK_SET) != 0 || (f = fdopen(fd, "r")) == NULL) {
                FSAY("stdio: the scanned file");
                break;
            }

            while (fscanf(f, "%d %7s", &a, word) == 2) {
                records++;
            }

            /* The first ends cleanly after two records; the second has a
             * number with no word after it, and also reaches the end. */
            if (records != 2 || !feof(f)) {
                FSAY("stdio: fscanf to the end of file %d: %d records, feof %d",
                     i, records, feof(f));
            }

            fclose(f);
        }

        unlink("/Temporary/posixtest-scan");
    }

    /* A temporary file, gone when closed. */
    {
        DIR *d;
        struct dirent *e;
        int left = 0;

        if ((f = tmpfile()) == NULL || fputs("scratch", f) != 0 || fseek(f, 0, SEEK_SET) != 0
            || fgets(line, sizeof line, f) == NULL || strcmp(line, "scratch") != 0
            || fclose(f) != 0) {
            FSAY("stdio: tmpfile");
        }

        if ((d = opendir("/Temporary")) != NULL) {
            while ((e = readdir(d)) != NULL) {
                left += strncmp(e->d_name, "tmp-", 4) == 0;
            }

            closedir(d);
        }

        if (left != 0) {
            FSAY("stdio: %d temporary files left", left);
        }
    }

    /* What a kit provided by name, still opened where no file is. */
    if (kosmos_provide("ps-given.txt", "given\n", 6) != 0
        || (f = fopen("/Temporary/ps-given.txt", "r")) == NULL
        || fgets(line, sizeof line, f) == NULL || strcmp(line, "given\n") != 0
        || fileno(f) != -1 || fclose(f) != 0) {
        FSAY("stdio: a provided file");
    }

    if (fileno(stdout) != 1 || fileno(stderr) != 2 || fprintf(stdout, "%s", "") != 0) {
        FSAY("stdio: the console's streams");
    }

    unlink(path);
    snprintf(out, size, "stdio: written and read by line, a thousand of them, positioned, "
             "scanned, given back, getline, r+, appended, wx refused, ten thousand bytes, "
             "fdopen, tmpfile gone, a provided file, the console");
    return 0;
}

int kosmos_posixtest_run(char *out, unsigned long size)
{
    pthread_t t[WORKERS];
    void *got;
    int i, ok;

    /* 1: eight threads counting under one mutex */
    for (i = 0; i < WORKERS; i++) {
        if (pthread_create(&t[i], NULL, counts, NULL) != 0) {
            SAY("posix: create failed at %d", i);
        }
    }

    for (i = 0; i < WORKERS; i++) {
        pthread_join(t[i], NULL);
    }

    if (counted != (long)WORKERS * COUNT_EACH) {
        SAY("posix: counted %ld of %ld", counted, (long)WORKERS * COUNT_EACH);
    }

    /* 2: a token round a ring of four, a thousand times */
    for (i = 0; i < RING; i++) {
        pthread_create(&t[i], NULL, passes, (void *)(intptr_t)i);
    }

    /* Watched rather than joined at once: a wake lost leaves the ring
     * asleep for good, and that is to be a sentence, not a hang. */
    {
        int seen = -1, still = 0;

        while (still < 100) {               /* five seconds without a pass */
            int now;

            pthread_mutex_lock(&ring_lock);
            now = ring_passes;
            pthread_mutex_unlock(&ring_lock);

            if (now >= RING_PASSES) {
                break;
            }

            still = now == seen ? still + 1 : 0;
            seen = now;
            pause_ms(50);
        }

        if (still >= 100) {
            SAY("posix: the ring stopped at %d of %d passes - a wake was lost",
                seen, RING_PASSES);
        }
    }

    for (i = 0; i < RING; i++) {
        pthread_join(t[i], NULL);
    }

    if (ring_passes != RING_PASSES) {
        SAY("posix: the ring passed %d of %d", ring_passes, RING_PASSES);
    }

    /* 3: a timed wait nobody signals, on the monotonic clock */
    {
        pthread_condattr_t ca;
        pthread_cond_t c;
        pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
        struct timespec start, until, end;
        long long waited;
        int r;

        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&c, &ca);
        clock_gettime(CLOCK_MONOTONIC, &start);
        until = start;
        until.tv_nsec += 30000000;          /* 30 ms */

        if (until.tv_nsec >= 1000000000) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000;
        }

        pthread_mutex_lock(&m);
        r = pthread_cond_timedwait(&c, &m, &until);
        pthread_mutex_unlock(&m);
        clock_gettime(CLOCK_MONOTONIC, &end);
        waited = (long long)(end.tv_sec - start.tv_sec) * 1000000000LL
                 + (end.tv_nsec - start.tv_nsec);

        if (r != ETIMEDOUT || waited < 30000000LL) {
            SAY("posix: the timed wait said %d after %lld us", r, waited / 1000);
        }
    }

    /* 4: once, under eight at once */
    for (i = 0; i < WORKERS; i++) {
        pthread_create(&t[i], NULL, calls_once, NULL);
    }

    for (i = 0; i < WORKERS; i++) {
        pthread_join(t[i], NULL);
    }

    if (once_ran != 1) {
        SAY("posix: once ran %d times", once_ran);
    }

    /* 5: a key's destructor, once for each thread that set it */
    pthread_key_create(&key, destroy);

    for (i = 0, ok = 0; i < 4; i++) {
        pthread_create(&t[i], NULL, sets_a_key, NULL);
    }

    for (i = 0; i < 4; i++) {
        pthread_join(t[i], &got);
        ok += got != NULL;
    }

    if (ok != 4 || destroyed != 4 || pthread_getspecific(key) != NULL) {
        SAY("posix: keys held %d, destroyed %d", ok, destroyed);
    }

    /* 6: a recursive mutex, held twice and refused to another */
    {
        pthread_mutexattr_t ma;

        pthread_mutexattr_init(&ma);
        pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&recursive, &ma);
        pthread_mutex_lock(&recursive);
        pthread_mutex_lock(&recursive);
        pthread_create(&t[0], NULL, tries, NULL);
        pthread_join(t[0], &got);
        pthread_mutex_unlock(&recursive);
        ok = got != NULL && pthread_mutex_trylock(&recursive) == 0;
        pthread_mutex_unlock(&recursive);
        pthread_mutex_unlock(&recursive);

        if (!ok || pthread_mutex_unlock(&recursive) != EPERM) {
            SAY("posix: the recursive mutex did not hold");
        }
    }

    /* 7: every thread's stack where it is said to be */
    pthread_create(&t[0], NULL, checks_stack, NULL);
    pthread_join(t[0], &got);

    if (!my_stack_holds_me() || got == NULL) {
        SAY("posix: a stack is not where it is said to be (first %d, another %d)",
            my_stack_holds_me(), got != NULL);
    }

    /* 8: detached threads collected - their indices come back. A
     * thousand, each ending at once, and then the heap used: a record
     * written after its thread was collected corrupts malloc's links, and
     * the churn is where that shows (`testing.md` 18.769). */
    for (i = 0; i < DETACHED; i++) {
        pthread_attr_t a;
        pthread_t d;

        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);

        /* EAGAIN while ended threads wait for the reaper is the kernel's
         * ceiling, and a refusal a port must expect; it has to pass once
         * they are collected. On x86 under QEMU, whose processors take
         * turns, nine hundred were ahead of it. */
        {
            int r, tries = 0;

            while ((r = pthread_create(&d, &a, ends_at_once, NULL)) == EAGAIN && tries < 200) {
                tries++;
                pause_ms(10);
            }

            if (r != 0) {
                SAY("posix: detached thread %d not made (%d, after %d waits)", i, r, tries);
            }
        }
    }

    {
        long fresh = -1;

        for (i = 0; i < 20; i++) {
            fresh = kosmos_posixtest_fresh_index();

            if (fresh >= 0 && fresh < 16) {
                break;
            }

            pause_ms(50);
        }

        if (fresh < 0 || fresh >= 16) {
            SAY("posix: detached threads not collected (a new thread is %ld)", fresh);
        }
    }

    {
        void *held[64];
        int j;

        for (i = 0; i < 64; i++) {
            for (j = 0; j < 64; j++) {
                held[j] = malloc(32 + (size_t)((i * 64 + j) % 7) * 48);

                if (held[j] == NULL) {
                    SAY("posix: the heap refused after the detached threads");
                }

                memset(held[j], j, 32);
            }

            for (j = 63; j >= 0; j--) {
                free(held[j]);
            }
        }
    }

    /* 9: readers together, a writer alone */
    for (i = 0; i < 3; i++) {
        pthread_create(&t[i], NULL, reads, NULL);
    }

    pthread_create(&t[3], NULL, writes, NULL);

    /* Watched as the ring is: the rwlock sleeps on a condition too. */
    for (i = 0; i < 400 && __atomic_load_n(&rw_done, __ATOMIC_ACQUIRE) < 4; i++) {
        pause_ms(50);
    }

    if (__atomic_load_n(&rw_done, __ATOMIC_ACQUIRE) < 4) {
        SAY("posix: the rwlock's threads stopped, %d of 4 done - a wake was lost",
            __atomic_load_n(&rw_done, __ATOMIC_ACQUIRE));
    }

    for (i = 0; i < 4; i++) {
        pthread_join(t[i], NULL);
    }

    if (!writer_alone) {
        SAY("posix: a writer was not alone");
    }

    /* 10: what a thread ends with, given to its joiner */
    pthread_create(&t[0], NULL, ends_at_once, (void *)(intptr_t)42);
    pthread_join(t[0], &got);

    if ((intptr_t)got != 42) {
        SAY("posix: joined %ld", (long)(intptr_t)got);
    }

    {
        char memory[160], process[160], files[240], streams[240];

        if (memory_checks(memory, sizeof memory) != 0) {
            snprintf(out, size, "%s", memory);
            return 1;
        }

        if (process_checks(process, sizeof process) != 0) {
            snprintf(out, size, "%s", process);
            return 1;
        }

        if (file_checks(files, sizeof files) != 0) {
            snprintf(out, size, "%s", files);
            return 1;
        }

        if (stdio_checks(streams, sizeof streams) != 0) {
            snprintf(out, size, "%s", streams);
            return 1;
        }

        snprintf(out, size, "posix: counted %ld, ring %d, timed out, once %d, keys %d, "
                 "recursive, stacks, reaped, rwlock %s, joined %ld; %s; %s; %s; %s",
                 counted, ring_passes, once_ran, destroyed,
                 most_inside > 1 ? "shared" : "one at a time", (long)(intptr_t)got,
                 memory, process, files, streams);
    }

    return 0;
}
