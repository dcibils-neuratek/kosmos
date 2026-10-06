/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The keyring: the passwords Kosmos keeps, sealed on the disk, and the
 * doors that decide who may ask what (`docs/keyring.md`, step K4).
 *
 * **Doors, never names.** Two endpoints, each handed to one process:
 *
 *   - **`smb`**, to smbfs: list, get, put and forget entries of kind SMB,
 *     and no other kind;
 *   - **`manage`**, to Passwords and the `keyring` program: every entry
 *     without its secret, forget, edit, the keyring's state - and `REVEAL`,
 *     the one operation that hands a secret to anybody but smbfs, for the
 *     Show button (Diego's decision 5).
 *
 * The kernel has no wait on two endpoints, so the main thread receives on
 * `smb` and a thread receives on `manage` and calls `smb` with the request
 * stamped, in the message's `tag`, by a word from the kernel's entropy that
 * nobody outside this process knows - `diskfs.c`'s second door and `smbfs.c`'s
 * waiters, the same shape. One thread holds every entry.
 *
 * **The file** is `keyfile.c`'s, sealed whole with AES-256-CCM, at
 * `/Keyring/keyring`, and the key is `/Keyring/machine-key` - 32 bytes from
 * the kernel's entropy, made the first time (decision 3: on the disk). Both
 * are reached through the disk server's second door, which reaches
 * `/Keyring` and nothing else, and which no program holds (K3).
 *
 * **Every change is written before it is answered**, and undone in memory
 * when the write fails, so what a caller is told is what is on the disk.
 * **A file that does not open is never overwritten**: it is renamed
 * `keyring.unopened-<date>`, an empty keyring begins, and the log says so
 * (*When the file does not open*).
 *
 * **Secrets are zeroed** wherever they passed: the request and the reply
 * once answered, the region a file was sealed or opened in, and an entry
 * forgotten.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "diskproto.h"
#include "keyproto.h"

#include "../init/say.h"
#include "clock_epoch.h"
#include "keyfile.h"

void keyring_server(long smb_door, long manage_door, long disk, long devices,
                    long console);

static const char KEY_PATH[] = "/Keyring/machine-key";
static const char FILE_PATH[] = "/Keyring/keyring";

/* How many entries a file may hold: CCM with a 12-byte nonce counts its
 * plaintext in three bytes, so sixteen megabytes - about twenty thousand -
 * and the format, not a number chosen here, is the ceiling. */
#define RECORDS_MOST  ((16u * 1024u * 1024u - KEYFILE_COUNTS_BYTES) / sizeof(struct key_record))

static long console = -1;
static long devices = -1;
static long disk = -1;
static long smb = -1;
static long manage = -1;

static uint8_t key[KEYFILE_KEY_BYTES];
static bool have_key;
static uint32_t file_state = KEY_FILE_NEW;
static bool disk_ok;

static struct key_record *records;
static size_t count, room;
static uint32_t next_id = 1;

static uint64_t door_token;
static bool door_open;

/*
 * ------------------------------------------------------------------------
 * Saying things.
 * ------------------------------------------------------------------------
 */

static void tell(const char *a, unsigned long n, bool with_n, const char *b)
{
    struct say_line line;

    say_begin(&line);
    say_text(&line, "keyring: ");
    say_text(&line, a);
    if (with_n) {
        say_dec(&line, n);
    }
    if (b != NULL) {
        say_text(&line, b);
    }
    say_send(console, &line);
}

static void put_text(char *field, size_t size, const char *text)
{
    size_t n = strlen(text);

    if (n >= size) {
        n = size - 1;
    }

    memcpy(field, text, n);
    field[n] = '\0';
}

static uint64_t now(void)
{
    return clock_epoch(devices);
}

/*
 * ------------------------------------------------------------------------
 * The disk, through its second door: `diskproto.h` as a client.
 * ------------------------------------------------------------------------
 */

static struct message dmsg, drep;

static uint32_t disk_ask(uint32_t op, const char *path, uint32_t flags,
                         uint64_t offset, uint64_t bytes, const void *data,
                         uint32_t length, long region)
{
    struct disk_request *rq = (struct disk_request *)dmsg.data;
    const struct disk_reply *rp = (const struct disk_reply *)drep.data;

    if (disk < 0) {
        return DISK_ERR_NO_DISK;
    }

    memset(&dmsg, 0, sizeof dmsg);
    memset(&drep, 0, sizeof drep);
    dmsg.length = sizeof *rq;
    dmsg.cap_plus_one = region >= 0 ? (uint32_t)region + 1 : 0;
    rq->op = op;
    rq->flags = flags;
    rq->offset = offset;
    rq->bytes = bytes;
    rq->length = length;
    put_text(rq->path, sizeof rq->path, path);
    if (length > 0) {
        memcpy(rq->u.data, data, length);
    }

    if (kosmos_call(disk, &dmsg, &drep) != 0 || drep.length < sizeof *rp) {
        memset(&dmsg, 0, sizeof dmsg);
        return DISK_ERR_NO_DISK;
    }

    memset(rq->u.data, 0, sizeof rq->u.data);
    return rp->error;
}

static const struct disk_reply *disk_said(void)
{
    return (const struct disk_reply *)drep.data;
}

/* A region of `bytes`, mapped: its capability and where it is. */
struct region {
    long cap;
    uint8_t *at;
    unsigned long pages;
};

static bool region_make(struct region *r, size_t bytes)
{
    long at;

    r->pages = (bytes + 4095u) / 4096u;
    if (r->pages == 0) {
        r->pages = 1;
    }

    r->cap = kosmos_mem_create(r->pages);
    if (r->cap < 0) {
        return false;
    }

    at = kosmos_mem_map(r->cap);
    if (at < 0) {
        kosmos_cap_drop(r->cap);
        return false;
    }

    r->at = (uint8_t *)(uintptr_t)at;
    return true;
}

/* Zeroed before it goes: it held a sealed file or an opened one. */
static void region_free(struct region *r)
{
    memset(r->at, 0, r->pages * 4096u);
    kosmos_share_unmap((unsigned long)(uintptr_t)r->at, r->pages);
    kosmos_cap_drop(r->cap);
}

/* A file's size, or -1 when there is none. */
static long file_size(const char *path)
{
    if (disk_ask(DISK_OP_GETATTR, path, 0, 0, 0, NULL, 0, -1) != 0
        || disk_said()->node.kind != DISK_KIND_FILE) {
        return -1;
    }

    return (long)disk_said()->node.size;
}

/* A whole file into a region made for it: one read, which `diskfs.c`
 * answers with all of it or an error. */
static bool file_read(const char *path, struct region *r, size_t *bytes)
{
    long size = file_size(path);

    if (size < 0 || !region_make(r, (size_t)size)) {
        return false;
    }

    if (disk_ask(DISK_OP_READ, path, DISK_REGION, 0, (uint64_t)size, NULL, 0, r->cap) != 0
        || disk_said()->bytes != (uint64_t)size) {
        region_free(r);
        return false;
    }

    *bytes = (size_t)size;
    return true;
}

/*
 * ------------------------------------------------------------------------
 * The key, and the file.
 * ------------------------------------------------------------------------
 */

static void set_aside(const char *path, const char *why)
{
    char to[64];
    struct say_line line;
    const char *name = strrchr(path, '/') + 1;
    uint64_t when = now();
    size_t n;

    put_text(to, sizeof to, name);
    n = strlen(to);
    put_text(to + n, sizeof to - n, ".unopened-");
    n = strlen(to);

    /* The date, or the counter when there is no clock: a name nothing else has. */
    {
        char digits[24];
        int d = 0;
        uint64_t v = when ? when : kosmos_ticks();

        do {
            digits[d++] = (char)('0' + v % 10);
            v /= 10;
        } while (v > 0 && d < (int)sizeof digits);

        while (d > 0 && n + 1 < sizeof to) {
            to[n++] = digits[--d];
        }
        to[n] = '\0';
    }

    if (disk_ask(DISK_OP_RENAME, path, 0, 0, 0, to, (uint32_t)strlen(to), -1) == 0) {
        say_begin(&line);
        say_text(&line, "keyring: ");
        say_text(&line, path);
        say_text(&line, " ");
        say_text(&line, why);
        say_text(&line, "; kept aside as /Keyring/");
        say_text(&line, to);
        say_text(&line, ", and an empty one begun - nothing is lost by trying it again later");
        say_send(console, &line);
    } else {
        tell(why, 0, false, "; and it could not be set aside, so nothing will be written");
        disk_ok = false;
    }
}

static bool make_key(void)
{
    if (kosmos_entropy(key, sizeof key) != (long)sizeof key) {
        tell("no entropy for a key", 0, false, NULL);
        return false;
    }

    if (disk_ask(DISK_OP_WRITE, KEY_PATH, 0, 0, 0, key, sizeof key, -1) != 0) {
        memset(key, 0, sizeof key);
        tell("the key could not be written", 0, false, NULL);
        return false;
    }

    tell("a key made for this machine, at ", 0, false, KEY_PATH);
    return true;
}

static bool load_key(void)
{
    long size = file_size(KEY_PATH);

    if (size < 0) {
        return make_key();
    }

    if (size != (long)sizeof key
        || disk_ask(DISK_OP_READ, KEY_PATH, 0, 0, sizeof key, NULL, 0, -1) != 0
        || disk_said()->length != sizeof key) {
        /* A key that is not one: kept, and another made, so the file it
         * sealed will be kept aside too rather than overwritten. */
        set_aside(KEY_PATH, "is not a key");
        return make_key();
    }

    memcpy(key, disk_said()->u.data, sizeof key);
    memset(drep.data, 0, sizeof drep.data);
    return true;
}

static bool grow(size_t want)
{
    struct key_record *more;
    size_t to = room ? room : 16;

    if (want <= room) {
        return true;
    }

    if (want > RECORDS_MOST) {
        return false;
    }

    while (to < want) {
        to *= 2;
    }

    if (to > RECORDS_MOST) {
        to = RECORDS_MOST;
    }

    more = realloc(records, to * sizeof *more);
    if (more == NULL) {
        return false;
    }

    memset(more + room, 0, (to - room) * sizeof *more);
    records = more;
    room = to;
    return true;
}

/* Every entry, sealed, written whole: the disk's journal makes the old file
 * or the new one, never half of each. */
static bool save(void)
{
    struct region r;
    uint8_t nonce[KEYFILE_NONCE_BYTES];
    size_t bytes = keyfile_bytes(count);
    long sealed;
    uint32_t e;

    if (!disk_ok || !have_key) {
        return false;
    }

    if (kosmos_entropy(nonce, sizeof nonce) != (long)sizeof nonce
        || !region_make(&r, bytes)) {
        return false;
    }

    sealed = keyfile_seal(key, nonce, records, count, next_id, r.at, r.pages * 4096u);
    e = sealed < 0 ? DISK_ERR_NO_DISK
                   : disk_ask(DISK_OP_WRITE, FILE_PATH, DISK_REGION, 0,
                              (uint64_t)sealed, NULL, 0, r.cap);
    region_free(&r);
    return e == 0;
}

static void open_file(void)
{
    struct region r;
    size_t bytes;
    long holds;
    int answer;

    if (file_size(FILE_PATH) < 0) {
        file_state = KEY_FILE_NEW;
        if (!save()) {
            tell("an empty keyring could not be written", 0, false, NULL);
        }
        return;
    }

    if (!file_read(FILE_PATH, &r, &bytes)) {
        tell("the keyring could not be read; it is left as it is", 0, false, NULL);
        disk_ok = false;
        return;
    }

    holds = keyfile_count(bytes);
    if (holds < 0 || !grow((size_t)holds)) {
        region_free(&r);
        set_aside(FILE_PATH, "is not a keyring this reads");
        file_state = KEY_FILE_SET_ASIDE;
        (void)save();
        return;
    }

    answer = keyfile_open(key, r.at, bytes, records, room, &count, &next_id);
    region_free(&r);

    if (answer == KEYFILE_OK) {
        file_state = KEY_FILE_OPENED;
        tell("opened, ", count, true, count == 1 ? " entry" : " entries");
        return;
    }

    set_aside(FILE_PATH, answer == KEYFILE_OTHER_KEY ? "was sealed by another key"
                       : answer == KEYFILE_ALTERED  ? "did not open with this machine's key"
                       : "is not a keyring this reads");
    file_state = KEY_FILE_SET_ASIDE;
    count = 0;
    next_id = 1;
    (void)save();
}

/*
 * ------------------------------------------------------------------------
 * The operations.
 * ------------------------------------------------------------------------
 */

static bool ended(const char *s, size_t room_bytes)
{
    return memchr(s, 0, room_bytes) != NULL;
}

static bool request_well_formed(const struct key_request *rq)
{
    const struct key_entry *e = &rq->entry;

    return ended(e->used_by_name, sizeof e->used_by_name)
        && ended(e->service, sizeof e->service)
        && ended(e->account, sizeof e->account)
        && ended(e->title, sizeof e->title)
        && ended(e->notes, sizeof e->notes)
        && ended(e->shares, sizeof e->shares);
}

static struct key_record *by_id(uint32_t id)
{
    for (size_t i = 0; i < count; i++) {
        if (records[i].entry.id == id) {
            return &records[i];
        }
    }

    return NULL;
}

static struct key_record *by_name(uint16_t kind, const char *service, const char *account)
{
    for (size_t i = 0; i < count; i++) {
        if (records[i].entry.kind == kind
            && strcmp(records[i].entry.service, service) == 0
            && strcmp(records[i].entry.account, account) == 0) {
            return &records[i];
        }
    }

    return NULL;
}

static bool sees(bool managing, uint16_t kind)
{
    return managing || kind == KEY_KIND_SMB;
}

static void op_list(const struct key_request *rq, struct key_reply *rp, bool managing)
{
    const struct key_record *best = NULL;

    for (size_t i = 0; i < count; i++) {
        const struct key_record *r = &records[i];

        if (r->entry.id > rq->entry.id && sees(managing, r->entry.kind)
            && (best == NULL || r->entry.id < best->entry.id)) {
            best = r;
        }
    }

    if (best == NULL) {
        rp->error = KEY_ERR_NONE;
        return;
    }

    rp->entry = best->entry;
    rp->count = (uint32_t)count;
}

static void op_get(const struct key_request *rq, struct key_reply *rp)
{
    struct key_record *r = by_name(KEY_KIND_SMB, rq->entry.service, rq->entry.account);
    struct sender_info who;

    if (r == NULL) {
        rp->error = KEY_ERR_NONE;
        return;
    }

    r->entry.used_unix = now();
    r->entry.uses++;
    if (kosmos_sender(&who) == 0) {
        r->entry.used_by = who.id;
        memcpy(r->entry.used_by_name, who.name, sizeof r->entry.used_by_name - 1);
        r->entry.used_by_name[sizeof r->entry.used_by_name - 1] = '\0';
    }
    (void)save();       /* a use not dated on the disk is not worth refusing for */

    rp->entry = r->entry;
    rp->secret_bytes = r->secret_bytes;
    memcpy(rp->secret, r->secret, r->secret_bytes);
}

static void op_put(const struct key_request *rq, struct key_reply *rp)
{
    struct key_record *r;
    struct key_record was;
    bool fresh;

    if (rq->secret_bytes > KEY_SECRET_MAX) {
        rp->error = KEY_ERR_TOO_LONG;
        return;
    }

    if (rq->entry.service[0] == '\0' || rq->entry.account[0] == '\0') {
        rp->error = KEY_ERR_BAD_OP;
        return;
    }

    r = by_name(KEY_KIND_SMB, rq->entry.service, rq->entry.account);
    fresh = r == NULL;

    /* No secret: what is kept about an entry there is - its shares, its
     * title - and the password it has, kept (smbfs, a share connected on a
     * session signed into from the keyring, holds no password to send). */
    if (fresh && rq->secret_bytes == 0) {
        rp->error = KEY_ERR_NONE;
        return;
    }

    if (fresh) {
        if (!grow(count + 1)) {
            rp->error = KEY_ERR_FULL;
            return;
        }

        r = &records[count++];
        memset(r, 0, sizeof *r);
        r->entry.id = next_id++;
        r->entry.kind = KEY_KIND_SMB;
        r->entry.created_unix = now();
        put_text(r->entry.service, sizeof r->entry.service, rq->entry.service);
        put_text(r->entry.account, sizeof r->entry.account, rq->entry.account);
    }

    was = *r;
    r->entry.modified_unix = now();
    r->entry.flags = rq->entry.flags & KEY_AT_START;
    if (rq->entry.title[0] != '\0') {
        put_text(r->entry.title, sizeof r->entry.title, rq->entry.title);
    }
    memcpy(r->entry.shares, rq->entry.shares, sizeof r->entry.shares);
    r->entry.shares[sizeof r->entry.shares - 1] = '\0';
    r->entry.shares[sizeof r->entry.shares - 2] = '\0';
    if (rq->secret_bytes > 0) {
        memset(r->secret, 0, sizeof r->secret);
        memcpy(r->secret, rq->secret, rq->secret_bytes);
        r->secret_bytes = rq->secret_bytes;
    }

    if (!save()) {
        if (fresh) {
            memset(r, 0, sizeof *r);
            count--;
            next_id--;
        } else {
            *r = was;
        }
        memset(&was, 0, sizeof was);
        rp->error = KEY_ERR_DISK;
        return;
    }

    memset(&was, 0, sizeof was);
    rp->entry = r->entry;
}

static void op_forget(const struct key_request *rq, struct key_reply *rp, bool managing)
{
    struct key_record *r = by_id(rq->entry.id);
    struct key_record was;
    size_t at;

    if (r == NULL || !sees(managing, r->entry.kind)) {
        rp->error = KEY_ERR_NONE;
        return;
    }

    at = (size_t)(r - records);
    was = *r;
    memmove(r, r + 1, (count - at - 1) * sizeof *r);
    count--;
    memset(&records[count], 0, sizeof records[count]);

    if (!save()) {
        memmove(&records[at + 1], &records[at], (count - at) * sizeof *records);
        records[at] = was;
        count++;
        rp->error = KEY_ERR_DISK;
    }

    memset(&was, 0, sizeof was);
}

static void op_edit(const struct key_request *rq, struct key_reply *rp)
{
    struct key_record *r = by_id(rq->entry.id);
    struct key_entry was;

    if (r == NULL) {
        rp->error = KEY_ERR_NONE;
        return;
    }

    was = r->entry;
    put_text(r->entry.title, sizeof r->entry.title, rq->entry.title);
    put_text(r->entry.notes, sizeof r->entry.notes, rq->entry.notes);
    r->entry.flags = rq->entry.flags & KEY_AT_START;
    r->entry.modified_unix = now();

    if (!save()) {
        r->entry = was;
        rp->error = KEY_ERR_DISK;
        return;
    }

    rp->entry = r->entry;
}

static void op_reveal(const struct key_request *rq, struct key_reply *rp)
{
    struct key_record *r = by_id(rq->entry.id);

    if (r == NULL) {
        rp->error = KEY_ERR_NONE;
        return;
    }

    rp->entry = r->entry;
    rp->secret_bytes = r->secret_bytes;
    memcpy(rp->secret, r->secret, r->secret_bytes);
}

static void answer(struct message *msg, uint64_t sender)
{
    static struct message out;
    struct key_request *rq = (struct key_request *)msg->data;
    struct key_reply *rp = (struct key_reply *)out.data;
    bool managing = door_open && msg->tag == door_token;

    memset(&out, 0, sizeof out);
    out.length = sizeof *rp;
    rp->file = file_state;
    rp->count = (uint32_t)count;

    if (msg->cap_plus_one) {
        kosmos_cap_drop((long)msg->cap_plus_one - 1);
    }

    if (msg->length != sizeof *rq || !request_well_formed(rq)) {
        rp->error = KEY_ERR_BAD_OP;
    } else if (!have_key && rq->op != KEY_OP_STATE) {
        rp->error = KEY_ERR_SEALED;
    } else {
        switch (rq->op) {
        case KEY_OP_LIST:
            op_list(rq, rp, managing);
            break;
        case KEY_OP_FORGET:
            op_forget(rq, rp, managing);
            break;
        case KEY_OP_GET:
            if (managing) rp->error = KEY_ERR_NOT_THIS_DOOR;
            else op_get(rq, rp);
            break;
        case KEY_OP_PUT:
            if (managing) rp->error = KEY_ERR_NOT_THIS_DOOR;
            else op_put(rq, rp);
            break;
        case KEY_OP_EDIT:
            if (!managing) rp->error = KEY_ERR_NOT_THIS_DOOR;
            else op_edit(rq, rp);
            break;
        case KEY_OP_STATE:
            if (!managing) rp->error = KEY_ERR_NOT_THIS_DOOR;
            break;
        case KEY_OP_REVEAL:
            if (!managing) rp->error = KEY_ERR_NOT_THIS_DOOR;
            else op_reveal(rq, rp);
            break;
        default:
            rp->error = KEY_ERR_BAD_OP;
            break;
        }
    }

    rp->count = (uint32_t)count;
    memset(msg->data, 0, sizeof msg->data);
    (void)kosmos_reply(sender, &out);
    memset(out.data, 0, sizeof out.data);
}

/*
 * ------------------------------------------------------------------------
 * The doors, and the loop.
 * ------------------------------------------------------------------------
 */

/* The `manage` door: forwarded to `smb`, stamped, and the answer handed back. */
static void manage_door_main(unsigned long unused)
{
    static struct message in, back;

    (void)unused;

    for (;;) {
        uint64_t caller = 0;

        if (kosmos_receive(manage, &in, &caller, 0, 0) != 0) {
            return;
        }

        if (in.cap_plus_one) {
            kosmos_cap_drop((long)in.cap_plus_one - 1);
            in.cap_plus_one = 0;
        }

        in.tag = door_token;

        if (kosmos_call(smb, &in, &back) != 0) {
            memset(&back, 0, sizeof back);
            back.length = sizeof(struct key_reply);
            ((struct key_reply *)back.data)->error = KEY_ERR_BAD_OP;
        }

        memset(in.data, 0, sizeof in.data);
        (void)kosmos_reply(caller, &back);
        memset(back.data, 0, sizeof back.data);
    }
}

void keyring_server(long smb_door, long manage_door, long disk_door, long devices_ep,
                    long console_ep)
{
    smb = smb_door;
    manage = manage_door;
    disk = disk_door;
    devices = devices_ep;
    console = console_ep;

    /* The folder, on a disk made before there was a keyring: made here, the
     * one place it is made (`testing.md` 18.418). */
    disk_ok = disk >= 0;
    if (disk_ok) {
        uint32_t e = disk_ask(DISK_OP_MKDIR, "/Keyring", 0, 0, 0, NULL, 0, -1);

        if (e != 0 && (disk_ask(DISK_OP_GETATTR, "/Keyring", 0, 0, 0, NULL, 0, -1) != 0
                       || disk_said()->node.kind != DISK_KIND_DIR)) {
            tell("there is no disk to keep it on; it holds nothing for now", 0, false, NULL);
            disk_ok = false;
        }
    }

    have_key = disk_ok && load_key();
    if (have_key) {
        open_file();
    }

    while (door_token == 0
           && kosmos_entropy(&door_token, sizeof door_token) == (long)sizeof door_token) {
    }

    if (manage >= 0 && door_token != 0 && kosmos_thread_start(manage_door_main, 0) >= 0) {
        door_open = true;
    } else if (manage >= 0) {
        tell("the manage door would not open", 0, false, NULL);
    }

    for (;;) {
        static struct message msg;
        uint64_t sender = 0;

        if (kosmos_receive(smb, &msg, &sender, 0, 0) != 0) {
            return;
        }

        answer(&msg, sender);
    }
}
