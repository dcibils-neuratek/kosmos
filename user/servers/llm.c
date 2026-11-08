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

void llm_server(long running_cap, long models_disk_ep, long console_ep);

#define MODELS_ROOT   "/Home/Models"
#define PIECE_BYTES   (4u * 1024u * 1024u)  /* a model read this much at a time */
#define DEFAULT_TOKENS 128u
#define WAITERS       16u                   /* WAITs held at once; more answered now */
#define ALLOCS        64u                   /* the kit's tables, a region each */

_Static_assert(sizeof(struct llm_request) == 1624, "llm_request is 1624 bytes");
_Static_assert(sizeof(struct llm_reply) == 336, "llm_reply is 336 bytes");

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
    int32_t *prompt;
    uint32_t prompt_n, prompt_at;       /* tokens, and how many are read */
    uint32_t max_tokens, flags, made;
    int32_t  next;                      /* the token to feed next, or -1 */
    unsigned long asked, began_answer, first_at;
    uint64_t prompt_us, answer_us, first_us;
    uint64_t answers;
} ans = { .region = { .cap = -1 } };

static struct block prompt_room = { .cap = -1 };

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
        say(console, " is loaded\n");
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
                 MODELS_ROOT);
        return LLM_ERR_NO_FILE;
    }

    /*
     * The folder, made where it is not - a disk that has never held a model
     * - here rather than at start: the disk is asked only when somebody
     * wants a model, so a server that asks for nothing never waits on it.
     */
    (void)diskask(&disk, DISK_OP_MKDIR, "/Home", 0, 0, 0, -1, NULL, 0);
    (void)diskask(&disk, DISK_OP_MKDIR, MODELS_ROOT, 0, 0, 0, -1, NULL, 0);
    snprintf(path, sizeof path, "%s/%s", MODELS_ROOT, name);

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
            snprintf(model.why, sizeof model.why, "no %s in %s", name, MODELS_ROOT);
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
        snprintf(path, sizeof path, "%s/%s", MODELS_ROOT, model.name);
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

    load_done(LLM_OK);
}

/* ------------------------------------------------------------------------
 * Answering: the prompt read a token at a time, then the answer made one.
 * --------------------------------------------------------------------- */

static void answer_end(void)
{
    if (!ans.on) return;

    ans.on = false;

    if (ans.began_answer != 0) ans.answer_us = micros(kosmos_ticks() - ans.began_answer);

    llm_answer_publish(&ans.head->ended, 1);
    model.state = LLM_STATE_READY;
    ans.answers++;
    wake_waiters(true);
}

static uint32_t ask_begin(const struct llm_request *rq, long *cap)
{
    uint32_t length = rq->length < LLM_TEXT_MAX ? rq->length : LLM_TEXT_MAX;
    int n;

    if (model.state == LLM_STATE_EMPTY) return LLM_ERR_NO_MODEL;

    if (model.state != LLM_STATE_READY) return LLM_ERR_BUSY;

    if (prompt_room.cap < 0 && !region(&prompt_room, (LLM_TEXT_MAX + 1u) * sizeof(int32_t))) {
        return LLM_ERR_NO_MEMORY;
    }

    ans.prompt = (int32_t *)(uintptr_t)prompt_room.at;
    n = llm_tokenize(&model.m, rq->text, length, 1, ans.prompt, (int)LLM_TEXT_MAX + 1);

    if (n <= 0 || (uint32_t)n >= model.m.n_ctx) return LLM_ERR_TOO_LONG;

    /* A new region each time: the last asker may still be reading its own. */
    region_drop(&ans.region);

    if (!region(&ans.region, LLM_ANSWER_REGION)) return LLM_ERR_NO_MEMORY;

    ans.head = (struct llm_answer *)(uintptr_t)ans.region.at;
    ans.data = (char *)(uintptr_t)ans.region.at + LLM_ANSWER_DATA;
    ans.head->magic = LLM_ANSWER_MAGIC;
    ans.head->bytes = LLM_ANSWER_BYTES;

    ans.on = true;
    ans.prompt_n = (uint32_t)n;
    ans.prompt_at = 0;
    ans.max_tokens = rq->max_tokens == 0 ? DEFAULT_TOKENS
                     : rq->max_tokens > LLM_ANSWER_TOKENS_MAX ? LLM_ANSWER_TOKENS_MAX
                     : rq->max_tokens;
    ans.flags = rq->flags;
    ans.made = 0;
    ans.next = -1;
    ans.asked = kosmos_ticks();
    ans.began_answer = 0;
    ans.prompt_us = ans.answer_us = ans.first_us = 0;
    llm_reset(&model.m);
    model.state = LLM_STATE_ANSWERING;
    *cap = ans.region.cap;
    return LLM_OK;
}

/* Whether the bytes from `from` on hold a blank line, counting the byte
 * before them: a token may end one line and the next begin the blank. */
static bool blank_line(uint32_t from)
{
    uint32_t w = ans.head->written;

    for (uint32_t i = from > 0 ? from - 1 : 0; i + 1 < w; i++) {
        if (ans.data[i] == '\n' && ans.data[i + 1] == '\n') return true;
    }

    return false;
}

/* One token: of the prompt, or of the answer. */
static void ask_step(void)
{
    int32_t token;
    uint32_t last = 0;

    if (ans.prompt_at < ans.prompt_n) {
        if (llm_step(&model.m, ans.prompt[ans.prompt_at++]) != 0) {
            answer_end();
            return;
        }

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

        last = n > 0 ? (uint32_t)n : 0;
        uint32_t w = ans.head->written;

        if (n < 0 || (uint32_t)n > LLM_ANSWER_BYTES - w) {
            answer_end();
            return;
        }

        memcpy(ans.data + w, text, (size_t)n);
        ans.made++;
        ans.head->tokens = ans.made;
        llm_answer_publish(&ans.head->written, w + (uint32_t)n);
    }

    if (ans.made == 1) ans.first_us = micros(kosmos_ticks() - ans.asked);

    ans.answer_us = micros(kosmos_ticks() - ans.began_answer);
    wake_waiters(false);

    if ((ans.flags & LLM_STOP_AT_BLANK_LINE) && blank_line(ans.head->written - last)) {
        answer_end();
        return;
    }

    if (ans.made >= ans.max_tokens || llm_step(&model.m, token) != 0) {
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

static void answer(const struct message *in, uint64_t sender)
{
    struct llm_request rq;
    long cap = -1;
    uint32_t e;

    memset(&rq, 0, sizeof rq);

    if (in->length != sizeof rq) {
        send(sender, LLM_ERR_BAD_OP, -1);
        return;
    }

    memcpy(&rq, in->data, sizeof rq);
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

        model.loader = sender;           /* answered when it is in */
        return;

    case LLM_OP_ASK:
        e = ask_begin(&rq, &cap);
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

    default:
        send(sender, LLM_ERR_BAD_OP, -1);
        return;
    }
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

void llm_server(long running_cap, long models_disk_ep, long console_ep)
{
    static struct message in;
    long endpoint;

    console = console_ep;
    disk.ep = models_disk_ep;

    {
        struct sysinfo info;

        memset(&info, 0, sizeof info);

        if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0) counter_hz = info.counter_hz;
    }

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
