/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **`/Running/llm`: the one process that holds a language model**
 * (`docs/xavier.md` X3, `llmproto.h`). Xavier asks it, and so will anything
 * else that wants an answer from a model: there is one model in memory and
 * one attention cache, rather than a copy of either in every application -
 * the premise in `CLAUDE.md`, kits and servers supplying and applications
 * orchestrating.
 *
 * What it is made of is the system's: the LLM Kit (`user/kits/llm/`) does
 * the arithmetic, and the disk server's own door reaches `/Home/Models` and
 * nothing else (`diskfs.c`'s doors), so the server can read a model and no
 * other file on the disk.
 *
 * **It never makes a caller wait behind work.** A LOAD's reply is held while
 * the file comes in, a piece at a time; an ASK is answered at once with the
 * answer's region, and the tokens are made one at a time between messages;
 * a WAIT is held until the answer has moved. The loop receives without
 * waiting while there is work, and blocks only when there is none - `design.md`
 * §2's rule, that nothing on the desktop waits on a server.
 *
 * **The answer never travels in a message** (`CLAUDE.md`, *Control by
 * message, data by shared memory*): tokens recur at the model's rate, so
 * their text accumulates in a region (`llmring.h`) the asker maps, and WAIT
 * carries only how far it has got.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kosmos.h"
#include "appproto.h"
#include "diskproto.h"
#include "diskask.h"
#include "llmproto.h"
#include "llmring.h"
#include "../init/say.h"
#include "kits/llm/llm_core.h"

void llm_server(long running_cap, long models_disk_ep, long console_ep, long models_drive_ep);

/*
 * **Where the models are** (`docs/xavier.md`, *Where the models live*):
 * `/Models`, a drive of their own, on a machine started with one - and
 * `/Home/Models`, through the disk server's door, on a machine without, as
 * every machine with one disk is. init decides, by which it hands over.
 */
static const char *models_root = "/Home/Models";
#define PIECE_BYTES   (4u * 1024u * 1024u)  /* a model read this much at a time */
#define DEFAULT_TOKENS 128u
#define WAITERS       16u                   /* WAITs held at once; more answered now */
#define ALLOCS        64u                   /* the kit's tables, a region each */

_Static_assert(sizeof(struct llm_request) == 1688, "llm_request is 1688 bytes");
_Static_assert(sizeof(struct llm_reply) == 424, "llm_reply is 424 bytes");
_Static_assert(LLM_LOOPS_AUTO == LLM_KIT_LOOPS_AUTO && LLM_LOOPS_PLAIN == LLM_KIT_LOOPS_PLAIN
               && LLM_LOOPS_BASE == LLM_KIT_LOOPS_BASE && LLM_LOOPS_AVX2 == LLM_KIT_LOOPS_AVX2,
               "the protocol's loops are the kit's");

/* ------------------------------------------------------------------------
 * **Where the model runs** (`docs/xavier.md`, *Settings*): the server's, for
 * every application that asks, kept in a file beside the models - the one
 * folder this server can reach - and in force from the next question.
 * --------------------------------------------------------------------- */

#define SETTINGS_FILE  "llm.settings"
#define SETTINGS_MAGIC 0x534d4c4cu          /* "LLMS", then the struct */

struct settings_file {
    uint32_t magic, size;
    struct llm_settings s;
};

static struct llm_settings settings;        /* zero: Auto everywhere */
static uint32_t processors = 1, cores = 1;  /* this machine */
static uint32_t ran_cores = 1, ran_loops;   /* the last answer */

static long console = -1;
static unsigned long counter_hz = 62500000ul;
static struct diskask disk = { .ep = -1 };

static uint64_t micros(unsigned long counts)
{
    return (uint64_t)(counts / counter_hz) * 1000000ull
           + (uint64_t)(counts % counter_hz) * 1000000ull / counter_hz;
}

/* ------------------------------------------------------------------------
 * Memory: every table the kit asks for is a region of its own, so a model
 * let go gives every page back - the kit's tables, the attention cache and
 * the file - without a heap to fragment between one model and the next.
 * --------------------------------------------------------------------- */

struct block {
    long          cap;
    unsigned long at, pages;
};

static struct block blocks[ALLOCS];
static unsigned n_blocks;

static bool region(struct block *b, uint64_t bytes)
{
    long at;

    b->pages = (unsigned long)((bytes + 4095u) / 4096u);

    if (b->pages == 0) b->pages = 1;

    if ((b->cap = kosmos_mem_create(b->pages)) < 0) return false;

    if ((at = kosmos_mem_map(b->cap)) < 0) {
        kosmos_cap_drop(b->cap);
        b->cap = -1;
        return false;
    }

    b->at = (unsigned long)at;
    return true;
}

static void region_drop(struct block *b)
{
    if (b->cap >= 0) {
        kosmos_share_unmap(b->at, b->pages);
        kosmos_cap_drop(b->cap);
    }

    b->cap = -1;
}

/* The kit's allocator: zeroed pages, as a fresh region always is. */
static void *kit_alloc(void *ctx, size_t bytes)
{
    (void)ctx;

    if (n_blocks == ALLOCS || !region(&blocks[n_blocks], bytes)) return NULL;

    return (void *)(uintptr_t)blocks[n_blocks++].at;
}

/* ------------------------------------------------------------------------
 * The model, and what is being done with it.
 * --------------------------------------------------------------------- */

static struct {
    uint32_t state;
    struct llm_model m;
    struct block file;
    char     name[LLM_NAME_MAX];
    char     why[LLM_WHY_MAX];
    uint64_t size, loaded;
    unsigned long load_began;
    uint64_t load_us;
    uint64_t loader;                    /* the caller held in LOAD, or 0 */
} model = { .file = { .cap = -1 } };

static struct block piece = { .cap = -1 };

static struct {
    bool     on;                        /* an answer is being made */
    struct block region;
    struct llm_answer *head;
    char    *data;
    int32_t *prompt;                    /* the model's n_ctx, from the kit's room */
    uint32_t prompt_n, prompt_at;       /* tokens, and how many are read */
    uint32_t kept;                      /* of the conversation, already held */
    uint32_t max_tokens, flags, made;

    /*
     * The answer's bytes as made, and as shown. They part only while the
     * end of what is made could be the start of a stop text: that much is
     * held back, so text a stop would take away was never in `written`,
     * which only grows (`llmring.h`).
     */
    uint32_t made_bytes, searched;
    char     stop[LLM_STOPS][LLM_STOP_MAX];
    uint32_t stop_n[LLM_STOPS];
    int32_t  next;                      /* the token to feed next, or -1 */
    unsigned long asked, began_answer, first_at;
    uint64_t prompt_us, answer_us, first_us;
    uint64_t answers;
} ans = { .region = { .cap = -1 } };

/*
 * What the cache holds (`llm_core.h`, `struct llm_held`), from the kit's
 * own allocator, so it goes with the model; and a conversation handed in a
 * region, copied here before it is read, so the asker cannot change it
 * under the tokenizer.
 */
static struct llm_held held;
static struct block conversation = { .cap = -1 };

static uint64_t waiters[WAITERS];
static uint32_t waiting_since[WAITERS];

static void put(char *to, size_t room, const char *from, size_t n)
{
    memset(to, 0, room);
    memcpy(to, from, n < room - 1 ? n : room - 1);
}

/* Everything a reply says about the server, the model and the last answer. */
static void describe(struct llm_reply *rp)
{
    rp->state = model.state;
    put(rp->model, sizeof rp->model, model.name, strlen(model.name));
    put(rp->why, sizeof rp->why, model.why, strlen(model.why));
    rp->model_bytes = model.size;
    rp->loaded_bytes = model.loaded;
    rp->load_us = model.load_us;

    if (model.state == LLM_STATE_READY || model.state == LLM_STATE_ANSWERING) {
        put(rp->title, sizeof rp->title, model.m.name.s ? model.m.name.s : "", model.m.name.n);
        put(rp->arch, sizeof rp->arch, model.m.arch.s ? model.m.arch.s : "", model.m.arch.n);
        rp->n_layer = model.m.n_layer;
        rp->n_embd = model.m.n_embd;
        rp->n_vocab = model.m.n_vocab;
        rp->n_ctx = model.m.n_ctx;
    }

    if (model.state == LLM_STATE_READY || model.state == LLM_STATE_ANSWERING) {
        rp->held_tokens = model.m.n_past;
    }

    rp->settings = settings;
    rp->processors = processors;
    rp->cores = cores;
    rp->loops_have = llm_loops_have();
    rp->gpu_have = 0;                   /* the GPU's steps are next (X9) */
    rp->ran_cores = ran_cores;
    rp->ran_loops = ran_loops;
    rp->ran_gpu = 0;

    rp->kept_tokens = ans.kept;
    rp->prompt_tokens = ans.prompt_n;
    rp->answer_tokens = ans.made;
    rp->prompt_us = ans.prompt_us;
    rp->answer_us = ans.answer_us;
    rp->first_us = ans.first_us;
    rp->answers = ans.answers;

    if (ans.head != NULL) {
        rp->written = ans.head->written;
        rp->ended = ans.head->ended;
    }
}

static void send(uint64_t to, uint32_t error, long cap)
{
    static struct message out;
    struct llm_reply *rp = (struct llm_reply *)(void *)out.data;

    memset(&out, 0, sizeof out);
    out.length = sizeof *rp;
    out.cap_plus_one = cap >= 0 ? (uint32_t)cap + 1u : 0u;
    rp->error = error;
    describe(rp);
    (void)kosmos_reply(to, &out);
}

/* Every WAIT whose answer has moved past where it was, or ended. */
static void wake_waiters(bool all)
{
    for (unsigned i = 0; i < WAITERS; i++) {
        if (waiters[i] != 0
            && (all || ans.head == NULL || ans.head->written > waiting_since[i]
                || ans.head->ended)) {
            send(waiters[i], ans.head ? LLM_OK : LLM_ERR_NO_ANSWER, -1);
            waiters[i] = 0;
        }
    }
}

/* ------------------------------------------------------------------------
 * Loading: the file into one region, a piece at a time, then the kit.
 * --------------------------------------------------------------------- */

static void unload(void)
{
    for (unsigned i = 0; i < n_blocks; i++) region_drop(&blocks[i]);

    n_blocks = 0;
    region_drop(&model.file);
    memset(&model.m, 0, sizeof model.m);
    memset(&held, 0, sizeof held);
    ans.prompt = NULL;
    model.state = LLM_STATE_EMPTY;
    model.size = model.loaded = 0;
}

static void load_done(uint32_t error)
{
    if (error != LLM_OK) {
        unload();
    } else {
        model.state = LLM_STATE_READY;
        model.load_us = micros(kosmos_ticks() - model.load_began);
        say(console, "llm: ");
        say(console, model.name);
        say(console, model.m.loops == LLM_KIT_LOOPS_AVX2 ? " is loaded, its loops in AVX2\n"
                                                          : " is loaded\n");
    }

    if (model.loader != 0) {
        send(model.loader, error, -1);
        model.loader = 0;
    }
}

/* A name in /Home/Models: one name, never a path out of it. */
static bool good_name(const char *name)
{
    size_t n = 0;

    while (n < LLM_NAME_MAX && name[n] != '\0') n++;

    if (n == 0 || n == LLM_NAME_MAX) return false;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;

    return memchr(name, '/', n) == NULL;
}

static uint32_t load_begin(const char *name)
{
    char path[DISK_PATH_MAX];
    uint64_t size;
    uint32_t e;

    if (!good_name(name)) {
        snprintf(model.why, sizeof model.why, "a model is a file's name in %s, not a path",
                 models_root);
        return LLM_ERR_NO_FILE;
    }

    /*
     * The folder, made where it is not - a disk that has never held a model
     * - here rather than at start: the disk is asked only when somebody
     * wants a model, so a server that asks for nothing never waits on it.
     */
    if (strcmp(models_root, "/Home/Models") == 0) {
        (void)diskask(&disk, DISK_OP_MKDIR, "/Home", 0, 0, 0, -1, NULL, 0);
    }
    (void)diskask(&disk, DISK_OP_MKDIR, models_root, 0, 0, 0, -1, NULL, 0);
    snprintf(path, sizeof path, "%s/%s", models_root, name);

    /* The file is looked for before the model held is let go: a name
     * mistyped costs a refusal, not the model already loaded. */
    e = diskask(&disk, DISK_OP_GETATTR, path, 0, 0, 0, -1, NULL, 0);

    if (e != DISK_OK || diskask_said(&disk)->node.kind != DISK_KIND_FILE) {
        const struct disk_reply *rp = diskask_said(&disk);

        /* The disk's own words when it has some - a door shut, say - and
         * otherwise the file is simply not there. */
        if (e == DISK_ERR_NO_DISK && rp->length > 0 && rp->length < DISK_DATA_MAX) {
            put(model.why, sizeof model.why, rp->u.data, rp->length);
        } else {
            snprintf(model.why, sizeof model.why, "no %s in %s", name, models_root);
        }

        return LLM_ERR_NO_FILE;
    }

    size = diskask_said(&disk)->node.size;
    unload();
    put(model.name, sizeof model.name, name, strlen(name));
    model.why[0] = '\0';
    model.size = size;

    if (!region(&model.file, model.size)
        || (piece.cap < 0 && !region(&piece, PIECE_BYTES))) {
        region_drop(&model.file);
        snprintf(model.why, sizeof model.why, "no room for %llu bytes",
                 (unsigned long long)model.size);
        return LLM_ERR_NO_MEMORY;
    }

    model.state = LLM_STATE_LOADING;
    model.loaded = 0;
    model.load_began = kosmos_ticks();
    return LLM_OK;
}

/* One piece of the file; then, when it is all in, the kit's turn. */
static void load_step(void)
{
    char path[DISK_PATH_MAX];
    uint64_t want = model.size - model.loaded;
    uint32_t e;

    if (want > PIECE_BYTES) want = PIECE_BYTES;

    if (want > 0) {
        snprintf(path, sizeof path, "%s/%s", models_root, model.name);
        e = diskask(&disk, DISK_OP_READ, path, DISK_REGION, model.loaded, want, piece.cap,
                    NULL, 0);

        if (e != DISK_OK || diskask_said(&disk)->bytes != want) {
            snprintf(model.why, sizeof model.why, "the disk stopped at %llu of %llu bytes",
                     (unsigned long long)model.loaded, (unsigned long long)model.size);
            load_done(LLM_ERR_NO_FILE);
            return;
        }

        memcpy((uint8_t *)(uintptr_t)model.file.at + model.loaded,
               (const void *)(uintptr_t)piece.at, (size_t)want);
        model.loaded += want;
        return;
    }

    if (llm_open(&model.m, (const void *)(uintptr_t)model.file.at, (size_t)model.size, 0,
                 kit_alloc, NULL, model.why, sizeof model.why) != 0) {
        load_done(n_blocks == ALLOCS ? LLM_ERR_NO_MEMORY : LLM_ERR_BAD_MODEL);
        return;
    }

    /* The room a conversation needs, beside the model and gone with it: a
     * prompt of the whole cache, and the cache's tokens and text held. */
    ans.prompt = kit_alloc(NULL, ((size_t)model.m.n_ctx + 1) * sizeof(int32_t));
    held.tokens = kit_alloc(NULL, (size_t)model.m.n_ctx * sizeof(int32_t));
    held.text = kit_alloc(NULL, LLM_CONVERSATION_MAX);
    held.text_room = LLM_CONVERSATION_MAX;

    if (ans.prompt == NULL || held.tokens == NULL || held.text == NULL) {
        snprintf(model.why, sizeof model.why, "no room for a conversation beside the model");
        load_done(LLM_ERR_NO_MEMORY);
        return;
    }

    llm_held_reset(&model.m, &held);

    load_done(LLM_OK);
}

/* ------------------------------------------------------------------------
 * Every core (`docs/xavier.md`, *Making it fast*): a worker a core, for as
 * long as an answer is being made.
 *
 * The kit cuts each product's rows into one part a worker and hands them
 * here (`llm_parallel`); the server's own thread hands them out and waits.
 * A product is about two milliseconds, and there are a hundred and thirty
 * of them a token, so a part is handed over by a counter in memory rather
 * than by a message or a new thread: the server's thread adds one to
 * `gen`, each worker sees it, does its rows and adds one to `finished`.
 * Between products a worker looks again with `kosmos_yield`, which gives
 * its core to anything else that wants it - and when the answer ends the
 * workers end, so a server with nothing to answer runs nothing at all.
 * --------------------------------------------------------------------- */

#define WORKERS_MAX 64

static struct {
    llm_part_fn *fn;
    void *job;
    uint32_t gen, finished, quit;       /* touched only atomically */
    uint32_t base;                      /* `gen` when this answer's workers began */
    uint32_t n;
    long thread[WORKERS_MAX];
    uint32_t said_n;                    /* how many the log was last told */
} pool;

static void pool_worker(unsigned long part)
{
    /* **From the count as it stood when the workers were made**, never 0:
     * `gen` counts every product of every answer, so a worker of the second
     * answer that began at 0 saw a product at once - the first answer's
     * last, through a pointer to a `struct product` long gone - and wrote
     * through it (`testing.md` 18.719). `base` is set before any worker is
     * made and `gen` does not move until all of them are. */
    uint32_t seen = pool.base, g;

    for (;;) {
        while ((g = __atomic_load_n(&pool.gen, __ATOMIC_ACQUIRE)) == seen) {
            if (__atomic_load_n(&pool.quit, __ATOMIC_ACQUIRE)) kosmos_thread_exit(0);
            kosmos_yield();
        }

        seen = g;
        pool.fn(pool.job, (uint32_t)part, pool.n);
        __atomic_add_fetch(&pool.finished, 1, __ATOMIC_RELEASE);
    }
}

/* The kit's way to run a product's parts: one a worker, and wait for all. */
static void pool_run(void *ctx, llm_part_fn *fn, void *job, uint32_t parts)
{
    (void)ctx;
    (void)parts;                        /* always `pool.n`: `llm_parallel` said so */

    pool.fn = fn;
    pool.job = job;
    __atomic_store_n(&pool.finished, 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&pool.gen, 1, __ATOMIC_RELEASE);

    while (__atomic_load_n(&pool.finished, __ATOMIC_ACQUIRE) < pool.n) kosmos_yield();
}

/* A worker on each core the settings ask for, where that is more than one. */
static void pool_start(void)
{
    uint32_t want = settings.cores ? settings.cores : cores;

    if (want > processors) want = processors;
    if (want < 2) return;
    if (want > WORKERS_MAX) want = WORKERS_MAX;

    __atomic_store_n(&pool.quit, 0, __ATOMIC_RELEASE);
    pool.base = __atomic_load_n(&pool.gen, __ATOMIC_ACQUIRE);
    pool.n = 0;

    for (uint32_t i = 0; i < want; i++) {
        long t = kosmos_thread_start_on(pool_worker, i, i);

        if (t < 0) t = kosmos_thread_start(pool_worker, i);
        if (t < 0) break;               /* as many as the machine would give */
        pool.thread[pool.n++] = t;
    }

    if (pool.n < 2) {
        /* One worker is no faster than this thread: let it go. */
        __atomic_store_n(&pool.quit, 1, __ATOMIC_RELEASE);
        for (uint32_t i = 0; i < pool.n; i++) (void)kosmos_thread_wait((unsigned long)pool.thread[i]);
        pool.n = 0;
        return;
    }

    llm_parallel(&model.m, pool_run, NULL, pool.n);

    /* Said once, and again whenever the settings change how many. */
    if (pool.said_n != pool.n) {
        char line[96];

        snprintf(line, sizeof line, "llm: each product cut across %u cores\n", pool.n);
        say(console, line);
        pool.said_n = pool.n;
    }
}

static void pool_stop(void)
{
    if (pool.n == 0) return;

    llm_parallel(&model.m, NULL, NULL, 1);
    __atomic_store_n(&pool.quit, 1, __ATOMIC_RELEASE);

    for (uint32_t i = 0; i < pool.n; i++) (void)kosmos_thread_wait((unsigned long)pool.thread[i]);

    pool.n = 0;
}

/* ------------------------------------------------------------------------
 * Answering: the prompt read a batch at a time, then the answer made a
 * token at a time.
 * --------------------------------------------------------------------- */

static void answer_end(void)
{
    if (!ans.on) return;

    ans.on = false;
    pool_stop();

    if (ans.began_answer != 0) ans.answer_us = micros(kosmos_ticks() - ans.began_answer);

    /* Whatever was held back for a stop that did not come is shown now. */
    if (ans.made_bytes > ans.head->written) llm_answer_publish(&ans.head->written, ans.made_bytes);

    llm_answer_publish(&ans.head->ended, 1);
    model.state = LLM_STATE_READY;
    ans.answers++;
    wake_waiters(true);
}

/*
 * The conversation an ASK hands over in a region: `length` bytes from its
 * start, copied out and the mapping let go at once.
 */
static uint32_t take_conversation(long given, uint32_t length, const char **text)
{
    long pages, at;

    if (given < 0) return LLM_ERR_BAD_OP;

    if (length == 0 || length > LLM_CONVERSATION_MAX) return LLM_ERR_TOO_LONG;

    if ((pages = kosmos_mem_size(given)) <= 0 || (uint64_t)length > (uint64_t)pages * 4096u) {
        return LLM_ERR_TOO_LONG;
    }

    if (conversation.cap < 0 && !region(&conversation, LLM_CONVERSATION_MAX)) {
        return LLM_ERR_NO_MEMORY;
    }

    if ((at = kosmos_mem_map(given)) < 0) return LLM_ERR_BAD_OP;

    memcpy((void *)(uintptr_t)conversation.at, (const void *)(uintptr_t)at, length);
    kosmos_share_unmap((unsigned long)at, (unsigned long)pages);
    *text = (const char *)(uintptr_t)conversation.at;
    return LLM_OK;
}

static uint32_t ask_begin(const struct llm_request *rq, long given, long *cap)
{
    const char *text = rq->text;
    uint32_t length = rq->length < LLM_TEXT_MAX ? rq->length : LLM_TEXT_MAX;
    uint32_t e, kept;
    int n;

    if (model.state == LLM_STATE_EMPTY) return LLM_ERR_NO_MODEL;

    if (model.state != LLM_STATE_READY) return LLM_ERR_BUSY;

    if (rq->flags & LLM_TEXT_IN_REGION) {
        if ((e = take_conversation(given, rq->length, &text)) != LLM_OK) return e;

        length = rq->length;
    }

    /* What the cache holds of it is kept; only the rest is to be read. */
    n = llm_prompt(&model.m, &held, text, length, ans.prompt, (int)model.m.n_ctx + 1, &kept);

    if (n <= 0) return LLM_ERR_TOO_LONG;

    /* A new region each time: the last asker may still be reading its own. */
    region_drop(&ans.region);

    if (!region(&ans.region, LLM_ANSWER_REGION)) return LLM_ERR_NO_MEMORY;

    ans.head = (struct llm_answer *)(uintptr_t)ans.region.at;
    ans.data = (char *)(uintptr_t)ans.region.at + LLM_ANSWER_DATA;
    ans.head->magic = LLM_ANSWER_MAGIC;
    ans.head->bytes = LLM_ANSWER_BYTES;

    for (unsigned i = 0; i < LLM_STOPS; i++) {
        memcpy(ans.stop[i], rq->stop[i], LLM_STOP_MAX);
        ans.stop_n[i] = 0;

        while (ans.stop_n[i] < LLM_STOP_MAX && ans.stop[i][ans.stop_n[i]] != '\0') ans.stop_n[i]++;
    }

    ans.on = true;
    ans.prompt_n = (uint32_t)n;
    ans.prompt_at = 0;
    ans.kept = kept;
    ans.max_tokens = rq->max_tokens == 0 ? DEFAULT_TOKENS
                     : rq->max_tokens > LLM_ANSWER_TOKENS_MAX ? LLM_ANSWER_TOKENS_MAX
                     : rq->max_tokens;
    ans.flags = rq->flags;
    ans.made = 0;
    ans.made_bytes = ans.searched = 0;
    ans.next = -1;
    ans.asked = kosmos_ticks();
    ans.began_answer = 0;
    ans.prompt_us = ans.answer_us = ans.first_us = 0;
    model.state = LLM_STATE_ANSWERING;

    /* The settings as they stand now, for this whole answer. */
    if (llm_use_loops(&model.m, settings.loops) < 0) (void)llm_use_loops(&model.m, LLM_LOOPS_AUTO);
    ran_loops = model.m.loops;
    pool_start();
    ran_cores = pool.n ? pool.n : 1;
    *cap = ans.region.cap;
    return LLM_OK;
}

/*
 * After the answer has grown: whether a stop text has appeared in it, and
 * how far it may be shown - to where the stop begins, or short of whatever
 * at its end could still become one.
 */
static bool stopped(uint32_t *show)
{
    uint32_t made = ans.made_bytes, hold = 0;

    for (unsigned s = 0; s < LLM_STOPS; s++) {
        uint32_t L = ans.stop_n[s];

        if (L == 0) continue;

        for (uint32_t i = ans.searched >= L - 1 ? ans.searched - (L - 1) : 0; i + L <= made; i++) {
            if (memcmp(ans.data + i, ans.stop[s], L) == 0) {
                *show = i;
                return true;
            }
        }

        for (uint32_t k = L - 1 < made ? L - 1 : made; k > hold; k--) {
            if (memcmp(ans.data + made - k, ans.stop[s], k) == 0) {
                hold = k;
                break;
            }
        }
    }

    ans.searched = made;
    *show = made - hold;
    return false;
}

/* Whether the bytes from `from` on hold a blank line, counting the byte
 * before them: a token may end one line and the next begin the blank. */
static bool blank_line(uint32_t from)
{
    uint32_t w = ans.made_bytes;

    for (uint32_t i = from > 0 ? from - 1 : 0; i + 1 < w; i++) {
        if (ans.data[i] == '\n' && ans.data[i + 1] == '\n') return true;
    }

    return false;
}

/* A batch of the prompt, or one token of the answer. **The prompt a batch
 * at a time** (`docs/xavier.md`, *Making it fast*): its tokens are all
 * known, so up to `LLM_BATCH` of them go through the model together, each
 * weight read once for the batch. Between batches the loop looks for STOP
 * as it does between tokens of the answer. */
static void ask_step(void)
{
    int32_t token;
    uint32_t last = 0;

    if (ans.prompt_at < ans.prompt_n) {
        uint32_t n = ans.prompt_n - ans.prompt_at;

        if (n > LLM_BATCH) n = LLM_BATCH;

        if (llm_feed_batch(&model.m, &held, ans.prompt + ans.prompt_at, n) != 0) {
            answer_end();
            return;
        }

        ans.prompt_at += n;

        if (ans.prompt_at == ans.prompt_n) {
            ans.prompt_us = micros(kosmos_ticks() - ans.asked);
            ans.began_answer = kosmos_ticks();
            ans.next = llm_greedy(&model.m);
        }

        return;
    }

    token = ans.next;

    if (token < 0 || token == model.m.eos || ans.made >= ans.max_tokens
        || model.m.n_past >= model.m.n_ctx) {
        answer_end();
        return;
    }

    {
        char text[256];
        int n = llm_token_text(&model.m, token, text, (int)sizeof text);

        uint32_t w = ans.made_bytes, show;

        last = n > 0 ? (uint32_t)n : 0;

        if (n < 0 || (uint32_t)n > LLM_ANSWER_BYTES - w) {
            answer_end();
            return;
        }

        memcpy(ans.data + w, text, (size_t)n);
        ans.made++;
        ans.made_bytes = w + (uint32_t)n;
        ans.head->tokens = ans.made;

        if (stopped(&show)) {
            ans.made_bytes = show;      /* the stop and after it, never shown */
            answer_end();
            return;
        }

        if (show > ans.head->written) llm_answer_publish(&ans.head->written, show);
    }

    if (ans.made == 1) ans.first_us = micros(kosmos_ticks() - ans.asked);

    ans.answer_us = micros(kosmos_ticks() - ans.began_answer);
    wake_waiters(false);

    if ((ans.flags & LLM_STOP_AT_BLANK_LINE) && blank_line(ans.made_bytes - last)) {
        answer_end();
        return;
    }

    if (ans.made >= ans.max_tokens || llm_feed(&model.m, &held, token) != 0) {
        answer_end();
        return;
    }

    ans.next = llm_greedy(&model.m);
}

/* ------------------------------------------------------------------------
 * The door.
 * --------------------------------------------------------------------- */

static void hold_wait(uint64_t sender, uint32_t since)
{
    if (ans.head == NULL) {
        send(sender, LLM_ERR_NO_ANSWER, -1);
        return;
    }

    if (ans.head->written > since || ans.head->ended) {
        send(sender, LLM_OK, -1);
        return;
    }

    for (unsigned i = 0; i < WAITERS; i++) {
        if (waiters[i] == 0) {
            waiters[i] = sender;
            waiting_since[i] = since;
            return;
        }
    }

    send(sender, LLM_OK, -1);           /* full: asked again, which is a poll */
}

/*
 * **How many cores the processors are on.** A core's second processor
 * shares its arithmetic and its way to memory, so Auto is one worker a
 * core: on x86 CPUID leaf 0xB's first level, when it is the SMT level, says
 * how many processors a core has; every ARM core here has one.
 */
static uint32_t processors_a_core(void)
{
#if defined(__x86_64__)
    uint32_t a, b, c, d;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0u), "c"(0u));
    if (a < 0xbu) return 1;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0xbu), "c"(0u));
    if (((c >> 8) & 0xffu) == 1u && (b & 0xffffu) > 1u) return b & 0xffffu;
#endif
    return 1;
}

/* Whether this machine can run `s`. */
static bool settings_possible(const struct llm_settings *s)
{
    return s->cores <= processors
        && s->loops <= LLM_LOOPS_AVX2
        && (s->loops == LLM_LOOPS_AUTO || (llm_loops_have() & 1u << s->loops))
        && s->gpu == 0;                 /* no GPU yet: the switches are drawn off */
}

static void settings_path(char *path, size_t room)
{
    snprintf(path, room, "%s/%s", models_root, SETTINGS_FILE);
}

/* The file beside the models, when there is one this machine can run; Auto
 * otherwise, and a file from another machine is left as it is. */
static void settings_read(void)
{
    char path[96];
    struct settings_file f;

    settings_path(path, sizeof path);

    if (diskask(&disk, DISK_OP_READ, path, 0, 0, sizeof f, -1, NULL, 0) != 0
        || diskask_said(&disk)->length != sizeof f) {
        return;
    }

    memcpy(&f, diskask_said(&disk)->u.data, sizeof f);
    f.s.reserved = 0;

    if (f.magic == SETTINGS_MAGIC && f.size == sizeof f && settings_possible(&f.s)) settings = f.s;
}

static bool settings_write(void)
{
    char path[96];
    struct settings_file f = { .magic = SETTINGS_MAGIC, .size = sizeof f, .s = settings };

    settings_path(path, sizeof path);
    (void)diskask(&disk, DISK_OP_MKDIR, models_root, 0, 0, 0, -1, NULL, 0);
    return diskask(&disk, DISK_OP_WRITE, path, 0, 0, 0, -1, &f, sizeof f) == 0;
}

/* SETTINGS: changed when asked and possible, kept, and said back. */
static void settings_request(const struct llm_request *rq, uint64_t sender)
{
    struct llm_settings want = rq->settings;

    if (!(rq->flags & LLM_SETTINGS_CHANGE)) {
        send(sender, LLM_OK, -1);
        return;
    }

    want.reserved = 0;

    if (!settings_possible(&want)) {
        send(sender, LLM_ERR_CANNOT, -1);
        return;
    }

    settings = want;
    send(sender, settings_write() ? LLM_OK : LLM_ERR_NOT_KEPT, -1);
}

static void handle(const struct llm_request *in, long given, uint64_t sender)
{
    struct llm_request rq = *in;
    long cap = -1;
    uint32_t e;

    rq.name[LLM_NAME_MAX - 1] = '\0';

    switch (rq.op) {
    case LLM_OP_LOAD:
        if (model.state == LLM_STATE_LOADING || model.state == LLM_STATE_ANSWERING) {
            send(sender, LLM_ERR_BUSY, -1);
            return;
        }

        if ((e = load_begin(rq.name)) != LLM_OK) {
            send(sender, e, -1);
            return;
        }

        /* Answered when it is in - or now, for a window that watches
         * STATS on its own clock rather than wait. */
        if (rq.flags & LLM_LOAD_NOWAIT) {
            send(sender, LLM_OK, -1);
        } else {
            model.loader = sender;
        }

        return;

    case LLM_OP_ASK:
        e = ask_begin(&rq, given, &cap);
        send(sender, e, e == LLM_OK ? cap : -1);
        return;

    case LLM_OP_WAIT:
        hold_wait(sender, rq.since);
        return;

    case LLM_OP_STOP:
        if (ans.head == NULL) {
            send(sender, LLM_ERR_NO_ANSWER, -1);
            return;
        }

        answer_end();
        send(sender, LLM_OK, -1);
        return;

    case LLM_OP_STATS:
        send(sender, LLM_OK, -1);
        return;

    case LLM_OP_SETTINGS:
        settings_request(&rq, sender);
        return;

    default:
        send(sender, LLM_ERR_BAD_OP, -1);
        return;
    }
}

/*
 * A request, whole or refused, and whatever capability came with it let
 * go after: only an ASK with its conversation in a region has a use for
 * one, and it copies the text out before it returns.
 */
static void answer(const struct message *in, uint64_t sender)
{
    struct llm_request rq;
    long given = in->cap_plus_one != 0 ? (long)in->cap_plus_one - 1 : -1;

    if (in->length != sizeof rq) {
        send(sender, LLM_ERR_BAD_OP, -1);
    } else {
        memcpy(&rq, in->data, sizeof rq);
        handle(&rq, given, sender);
    }

    if (given >= 0) kosmos_cap_drop(given);
}

static long register_name(long running)
{
    struct message msg, rep;
    struct app_request *req = (struct app_request *)msg.data;
    const struct app_reply *got = (const struct app_reply *)rep.data;
    long endpoint = kosmos_endpoint();

    if (endpoint < 0) return -1;

    memset(&msg, 0, sizeof msg);
    msg.length = sizeof *req;
    msg.cap_plus_one = (uint32_t)endpoint + 1;
    req->op = APP_OP_REGISTER;
    memcpy(req->name, "llm", 4);

    if (kosmos_call(running, &msg, &rep) < 0 || got->error != APP_OK
        || strncmp(got->name, "llm", APP_NAME_MAX) != 0) {
        return -1;
    }

    return endpoint;
}

void llm_server(long running_cap, long models_disk_ep, long console_ep, long models_drive_ep)
{
    static struct message in;
    long endpoint;

    console = console_ep;
    disk.ep = models_disk_ep;

    if (models_drive_ep >= 0) {
        disk.ep = models_drive_ep;
        models_root = "/Models";
    }

    {
        struct sysinfo info;

        memset(&info, 0, sizeof info);

        if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0) counter_hz = info.counter_hz;
        if (info.cpus > 0) processors = info.cpus;
    }

    cores = processors / processors_a_core();
    if (cores == 0) cores = 1;
    settings_read();

    if ((endpoint = register_name(running_cap)) < 0) {
        say(console, "llm: /Running would not take the name; not serving\n");
        kosmos_exit(1);
    }

    say(console, "llm: serving /Running/llm, idle until a model is asked for\n");

    for (;;) {
        uint64_t sender = 0;
        bool busy = model.state == LLM_STATE_LOADING || ans.on;
        long r = kosmos_receive(endpoint, &in, &sender, busy ? 1 : 0, 0);

        if (r == 0 && sender != 0) answer(&in, sender);

        if (model.state == LLM_STATE_LOADING) {
            load_step();
        } else if (ans.on) {
            ask_step();
        }
    }
}
