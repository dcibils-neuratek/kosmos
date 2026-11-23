/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Playback Kit's player** (`playback.h`): the core
 * (`playback_core.h`) on a thread of its own in the audio band, an input
 * ring for the frames, a ring of commands, and what is heard published
 * back. `playback_kosmos.c` gives it a Lua face and JetSki's web process
 * calls it from C (`docs/media.md` M4).
 *
 * **What crosses between the two threads, and each one way:**
 *
 * - frames, caller to thread, in the input ring (`playback_core.h`);
 * - commands - play, pause, a seek with its new ring - in a ring of fixed
 *   records, the caller writing and the thread reading, and `applied` back
 *   so that a seek can know the old ring is no longer touched;
 * - what is heard, published after each pass under a sequence number and
 *   read by the caller as a copy.
 *
 * Everything is allocated at `open` and freed at `close`; the thread's pass
 * allocates nothing (`playback_core.h`).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audioring.h"
#include "kosmos.h"
#include "playback.h"
#include "playback_core.h"

_Static_assert(PLAYBACK_WAV == PB_WAV && PLAYBACK_MP3 == PB_MP3,
               "the door's kinds are the core's");

/*
 * **One megabyte of compressed sound**: a minute of a 128 kb/s song, half a
 * minute of most films' - so the window may be away for seconds and nothing
 * is heard (`docs/playback.md`, *The two rings*).
 */
#define INPUT_BYTES (1u << 20)

/* ------------------------------------------------------------ commands */

enum cmd_kind { CMD_PLAY, CMD_PAUSE, CMD_SEEK };

struct cmd {
    enum cmd_kind kind;
    uint32_t generation;
    struct audio_ring *ring;
};

#define CMDS 64u

/* What the thread publishes, and the window copies. */
struct heard {
    struct pb_heard core;
    uint32_t generation;
    unsigned long worst_pass;           /* counter ticks between two passes */
    bool     playing;
    char     why[96];
};

struct playback {
    struct pb core;                     /* the thread's, after `open` */
    struct pb_input *in;
    struct audio_ring *ring;            /* the thread's: where it writes */
    bool playing;                       /* the thread's */

    /* The window's. */
    uint32_t generation;
    bool ended;
    bool closed;

    struct cmd cmds[CMDS];
    uint32_t cmd_head;                  /* written by the window */
    uint32_t cmd_tail;                  /* written by the thread */
    uint32_t applied;                   /* the thread's: commands done */

    int quit;
    long thread;
    bool audio_band;

    /* The thread's, between passes. */
    unsigned long last_pass;
    unsigned long worst_pass;
    unsigned long last_pass_at;         /* read straight by `state` */

    uint32_t heard_seq;
    struct heard heard;
};

struct handle {
    struct playback *p;
};

/* ------------------------------------------------------------ the thread */

static void take_commands(struct playback *pl)
{
    uint32_t head = __atomic_load_n(&pl->cmd_head, __ATOMIC_ACQUIRE);

    while (pl->cmd_tail != head) {
        const struct cmd *c = &pl->cmds[pl->cmd_tail % CMDS];

        switch (c->kind) {
        case CMD_PLAY:
            if (!pl->playing) {
                pl->last_pass = 0;      /* away is counted while playing */
            }

            pl->playing = true;
            break;

        case CMD_PAUSE:
            pl->playing = false;
            break;

        case CMD_SEEK:
            pb_restart(&pl->core, c->generation);
            pl->ring = c->ring;
            break;
        }

        __atomic_store_n(&pl->cmd_tail, pl->cmd_tail + 1, __ATOMIC_RELEASE);
    }

    /* After the old ring was let go, and not before: a seek waits on this. */
    __atomic_store_n(&pl->applied, pl->cmd_tail, __ATOMIC_RELEASE);
}

static void publish(struct playback *pl)
{
    uint32_t seq = pl->heard_seq;

    __atomic_store_n(&pl->heard_seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    pl->heard.core = pl->core.heard;
    pl->heard.generation = pl->core.generation;
    pl->heard.worst_pass = pl->worst_pass;
    pl->heard.playing = pl->playing;

    if (pl->core.why != NULL) {
        strncpy(pl->heard.why, pl->core.why, sizeof pl->heard.why - 1);
        pl->heard.why[sizeof pl->heard.why - 1] = '\0';
    } else {
        pl->heard.why[0] = '\0';
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&pl->heard_seq, seq + 2, __ATOMIC_RELEASE);
}

static void playback_main(unsigned long arg)
{
    struct playback *pl = (struct playback *)(uintptr_t)arg;

    while (!__atomic_load_n(&pl->quit, __ATOMIC_ACQUIRE)) {
        unsigned long now = kosmos_ticks();

        /* The longest this thread has been away between two passes while
         * playing: a sleep of one tick, and whatever held it off. */
        if (pl->playing && pl->last_pass != 0 && now - pl->last_pass > pl->worst_pass) {
            pl->worst_pass = now - pl->last_pass;
        }

        pl->last_pass = now;
        __atomic_store_n(&pl->last_pass_at, now, __ATOMIC_RELAXED);

        take_commands(pl);

        if (pl->playing && pl->ring != NULL) {
            (void)pb_pass(&pl->core, pl->in, pl->ring);
        }

        publish(pl);

        /*
         * One tick, 4 ms at 250 Hz, against a film's ring of 186 ms and a
         * song's of 46: every pass fills the ring, so a pass a tick keeps it
         * full with room to spare, and the thread is asleep - which is what
         * keeps it inside the audio band's budget.
         */
        kosmos_sleep(1);
    }

    /* A thread that returned from its entry would return to nothing. */
    kosmos_thread_exit(0);
}

/* ------------------------------------------------------------ the caller's */

static bool post(struct playback *pl, enum cmd_kind kind, uint32_t generation,
                 struct audio_ring *ring)
{
    uint32_t tail = __atomic_load_n(&pl->cmd_tail, __ATOMIC_ACQUIRE);

    if (pl->cmd_head - tail >= CMDS) {
        return false;                   /* the thread is far behind */
    }

    pl->cmds[pl->cmd_head % CMDS] = (struct cmd){ kind, generation, ring };
    __atomic_store_n(&pl->cmd_head, pl->cmd_head + 1, __ATOMIC_RELEASE);
    return true;
}

static void player_free(struct playback *pl)
{
    pb_close(&pl->core);
    free(pl->in);
    free(pl);
}

struct playback *playback_open(int kind, long rate, long channels,
                               long bits, const uint8_t *config, size_t config_n,
                               const char **why)
{
    struct playback *pl = (struct playback *)calloc(1, sizeof *pl);

    if (pl == NULL) {
        *why = "no memory for a player";
        return NULL;
    }

    pl->in = (struct pb_input *)malloc(INPUT_BYTES);

    if (pl->in == NULL || !pb_input_init(pl->in, INPUT_BYTES)) {
        free(pl->in);
        free(pl);
        *why = "no memory for the player's input ring";
        return NULL;
    }

    (void)config;                       /* AAC's, before 0.12.97; see playback.h */
    (void)config_n;

    if (pb_open(&pl->core, (enum pb_kind)kind, rate, channels, bits) != 0) {
        *why = pl->core.why ? pl->core.why : "this sound cannot be played";
        player_free(pl);
        return NULL;
    }

    /*
     * In the audio band from its first instruction, if this program may put
     * a thread there (`kosmos: needs audio`); at NORMAL otherwise, as the
     * Synth Kit falls back, and the state's `audio_band` says which.
     */
    pl->thread = kosmos_thread_start_audio(playback_main, (unsigned long)(uintptr_t)pl);
    pl->audio_band = pl->thread >= 0;

    if (pl->thread < 0) {
        pl->thread = kosmos_thread_start(playback_main, (unsigned long)(uintptr_t)pl);
    }

    if (pl->thread < 0) {
        player_free(pl);
        *why = "no thread to play on";
        return NULL;
    }

    return pl;
}

bool playback_seek(struct playback *pl, struct audio_ring *r)
{
    uint32_t want;

    if (!audio_ring_valid(r) || r->period_bytes % 4u != 0
        || r->period_bytes > sizeof pl->core.period) {
        return false;
    }

    pl->generation = (pl->generation + 1) & 0xffffu;
    pl->ended = false;

    if (!post(pl, CMD_SEEK, pl->generation, r)) {
        return false;
    }

    want = pl->cmd_head;

    /* Bounded: a thread a second late is a thread that has stopped. */
    for (int i = 0; i < 250; i++) {
        if ((int32_t)(__atomic_load_n(&pl->applied, __ATOMIC_ACQUIRE) - want) >= 0) {
            return true;
        }

        kosmos_sleep(1);
    }

    return false;
}

bool playback_play(struct playback *pl)
{
    return post(pl, CMD_PLAY, 0, NULL);
}

bool playback_pause(struct playback *pl)
{
    return post(pl, CMD_PAUSE, 0, NULL);
}

size_t playback_room(const struct playback *pl)
{
    return pb_input_room(pl->in);
}

/*
 * **An MP3's or a WAV's piece is 16 KB at most.** Their bytes are a stream,
 * held in the core's carry (32 KB) until they make a frame, and an MP3 is
 * decoded only with 16 KB of it waiting (`PB_MP3_AHEAD`). A piece larger
 * than what is left beside that would never fit and never be decoded to
 * make room - the core would wait on it for ever, silently - so it is
 * refused here, loudly.
 */
#define PB_PIECE (PB_CARRY - PB_MP3_AHEAD)

size_t playback_piece(const struct playback *pl)
{
    (void)pl;
    return PB_PIECE;
}

int playback_frame(struct playback *pl, const void *bytes, size_t n, const char **why)
{
    if (pl->ended) {
        *why = "a frame after the end";
        return -1;
    }

    if (n > pl->in->size / 2) {
        *why = "a frame larger than the player takes";
        return -1;
    }

    if (n > PB_PIECE) {
        *why = "a piece larger than an MP3's or a WAV's may be";
        return -1;
    }

    return pb_input_put(pl->in, bytes, (uint32_t)n, pl->generation << 16) ? 1 : 0;
}

bool playback_ended(struct playback *pl)
{
    if (pl->ended) {
        return true;
    }

    if (!pb_input_put(pl->in, NULL, 0, PB_END | (pl->generation << 16))) {
        return false;
    }

    pl->ended = true;
    return true;
}

/*
 * Copied under the sequence number, and the longest the thread was away
 * between two passes while playing counted up to now as well, so a thread
 * held off for good is seen.
 *
 * **Yielding while the writer is half way, and never giving up on zeros**
 * (`testing.md` 18.1009). This tried a thousand times without yielding and
 * then answered with the copy it had cleared: a writer held off mid-update -
 * on a loaded machine, or by this thread itself on its core - outlasted a
 * thousand reads in microseconds, and Radio said "0 frames heard, 0 dry"
 * a second after 316,928. Now each failed try gives the core away, so the
 * writer, wherever it is, has a thousand turns of the scheduler to finish
 * a dozen stores in. Only a writer that died half way outlasts them.
 */
void playback_state(const struct playback *pl, struct playback_state *out)
{
    struct heard copy;
    uint32_t seq;

    memset(&copy, 0, sizeof copy);

    for (int tries = 0; tries < 1000; tries++) {
        if (tries > 0) {
            kosmos_yield();
        }

        seq = __atomic_load_n(&pl->heard_seq, __ATOMIC_ACQUIRE);

        if (seq & 1u) {
            continue;
        }

        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        memcpy(&copy, (const void *)&pl->heard, sizeof copy);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        if (__atomic_load_n(&pl->heard_seq, __ATOMIC_ACQUIRE) == seq) {
            break;
        }
    }

    unsigned long away = copy.worst_pass;
    unsigned long last = __atomic_load_n(&pl->last_pass_at, __ATOMIC_RELAXED);

    if (copy.playing && last != 0 && kosmos_ticks() - last > away) {
        away = kosmos_ticks() - last;
    }

    memset(out, 0, sizeof *out);
    out->periods = copy.core.periods;
    out->frames_in = copy.core.frames_in;
    out->frames_out = copy.core.frames_out;
    out->dry = copy.core.dry;
    out->refused = copy.core.refused;
    out->finished = copy.core.finished && copy.generation == pl->generation;
    out->playing = copy.playing;
    out->audio_band = pl->audio_band;
    out->worst_pass = away;
    memcpy(out->why, copy.why, sizeof out->why);
}

void playback_close(struct playback *pl)
{
    if (pl == NULL) {
        return;
    }

    __atomic_store_n(&pl->quit, 1, __ATOMIC_RELEASE);
    (void)kosmos_thread_wait((unsigned long)pl->thread);
    player_free(pl);
}
