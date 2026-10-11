/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * smbd: the server side of sharing - this machine's folders, to the Mac's
 * Finder and to Windows, over SMB 2 and 3 (`docs/sharing.md`, *`smbd`, the
 * server side*, and N12).
 *
 * **A server in C**, a role of `init.elf`: what runs on behalf of another
 * machine does not get a collector (`CLAUDE.md`, *Language split*). It
 * runs libsmb2's own server - `smb2_serve_port`, its NEGOTIATE, its NTLMSSP
 * sign-in, its loop - on the SMB Kit, whose socket gained a server's half
 * for it (`smb_transport.c`: `bind`, `listen`, `accept`, `poll`, `select`
 * over the stack's `NET_OP_LISTEN`, `ACCEPT` and `POLL`). libsmb2 is not
 * touched: what is Kosmos's is the socket under it and the handlers over it.
 *
 * **N12a**: signing in. One account, whose NT hash is all a sign-in needs
 * (NTLMv2 is computed from the hash, never the password).
 *
 * **N12b, as built**: folders of `/Home` as shares, **read only** - a
 * client connects to one, lists its folders, reads its files' facts and
 * fetches their bytes, and anything that would change a byte is refused as
 * "access denied". The files come from `diskfs` through the door every
 * program reaches `/Home` by, as `diskproto.h` asks them, so a share can
 * never show more than `/Home` - and a name that climbs ("..") or names a
 * stream (":") is refused here before it is asked. The account and the
 * shares from the Servers window, the hash in the keyring, are N12c.
 *
 * **Started only when asked**: the boot's `opt/kosmos/smbd=PORT,ACCOUNT,
 * HASH` - a port, an account, and the account's NT hash as 32 hex digits,
 * by commas or spaces -
 * and `opt/kosmos/smbd_shares=NAME:/Home/FOLDER,...`, which a test
 * machine's command line carries. A machine not told to share shares
 * nothing. The hash on a command line is for a test account only; the
 * Servers window keeps the real one in the keyring (N12c).
 *
 * It is handed, at boot: the stack's capability, as a client of it; the
 * console's, so what it does is said in the log; and the disk's main door,
 * which reaches `/Home` and nothing else.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "kosmos.h"
#include "diskproto.h"
#include "init/say.h"
#include "smbpath.h"

#include "smb_kit.h"

static long console = -1;
static long disk = -1;

/* The one account N12a knows: its name, and "ntlm:" and its NT hash. */
static char account[64];
static char hashed[5 + 32 + 1];

static void said(const char *a, const char *b, const char *c)
{
    struct say_line line;

    say_begin(&line);
    say_text(&line, "smbd: ");
    say_text(&line, a);
    if (b != NULL) say_text(&line, b);
    if (c != NULL) say_text(&line, c);
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

/*
 * ------------------------------------------------------------------------
 * The disk, through the door to `/Home`: `diskproto.h` as a client.
 * ------------------------------------------------------------------------
 */

static struct message dmsg, drep;

static uint32_t disk_ask(uint32_t op, const char *path, uint32_t flags,
                         uint64_t offset, uint64_t bytes, long region)
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
    put_text(rq->path, sizeof rq->path, path);

    if (kosmos_call(disk, &dmsg, &drep) != 0 || drep.length < sizeof *rp) {
        return DISK_ERR_NO_DISK;
    }

    return rp->error;
}

static const struct disk_reply *disk_said(void)
{
    return (const struct disk_reply *)drep.data;
}

/*
 * One region for every read, made once at the size of the largest read
 * this server offers: the bytes go from the disk into it and from it into
 * the reply, never through a message.
 */
#define READ_MOST  0x100000u

static long read_cap = -1;
static uint8_t *read_at;

static bool read_region(void)
{
    long at;

    if (read_cap >= 0) {
        return true;
    }

    read_cap = kosmos_mem_create(READ_MOST / 4096u);
    if (read_cap < 0) {
        return false;
    }

    at = kosmos_mem_map(read_cap);
    if (at < 0) {
        kosmos_cap_drop(read_cap);
        read_cap = -1;
        return false;
    }

    read_at = (uint8_t *)(uintptr_t)at;
    return true;
}

/* What a path is: a file or a folder, its size and when it was written. */
struct facts {
    bool dir;
    uint64_t size;
    uint64_t written;           /* seconds since 1970, or 0 when undated */
};

static bool facts_of(const char *path, struct facts *f)
{
    const struct disk_node *node;

    if (disk_ask(DISK_OP_GETATTR, path, 0, 0, 0, -1) != DISK_OK) {
        return false;
    }

    node = &disk_said()->node;

    if (node->kind != DISK_KIND_FILE && node->kind != DISK_KIND_DIR) {
        return false;
    }

    f->dir = node->kind == DISK_KIND_DIR;
    f->size = f->dir ? 0 : node->size;
    f->written = node->dated ? node->modified : 0;
    return true;
}

/*
 * ------------------------------------------------------------------------
 * Shares, and which connection has which open.
 * ------------------------------------------------------------------------
 */

struct share {
    char name[64];
    char path[DISK_PATH_MAX];   /* "/Home/Public" */
};

static struct share *shares;
static size_t share_count;

/* A tree id is a share's place plus this, so none is ever 0. */
#define TREE_BASE  0x100u

/* A connection's trees: which shares it has connected, by tree id. */
struct link {
    struct smb2_context *smb2;
    uint32_t tree;
};

static struct link *links;
static size_t link_count, link_room;

static bool grow(void **list, size_t *room, size_t used, size_t each)
{
    void *bigger;
    size_t want;

    if (used < *room) {
        return true;
    }

    want = *room == 0 ? 16 : *room * 2;
    bigger = realloc(*list, want * each);

    if (bigger == NULL) {
        return false;
    }

    *list = bigger;
    *room = want;
    return true;
}

/* The share this request's tree is, when this connection connected it. */
static const struct share *share_here(struct smb2_context *smb2, uint32_t *tree)
{
    uint32_t id = smb2->hdr.sync.tree_id;
    size_t i;

    for (i = 0; i < link_count; i++) {
        if (links[i].smb2 == smb2 && links[i].tree == id) {
            if (tree != NULL) {
                *tree = id;
            }
            return &shares[id - TREE_BASE];
        }
    }

    return NULL;
}

/*
 * An open file or folder. Its SMB file id is its place in this table and
 * a generation, so a handle closed and its slot reused is not the old
 * handle again; and it belongs to one connection and one tree, so another
 * connection cannot use a file id it guessed.
 */
struct handle {
    bool used;
    uint32_t generation;
    struct smb2_context *smb2;
    uint32_t tree;
    char path[DISK_PATH_MAX];
    struct facts facts;

    /* A folder's names, read when it is first listed. */
    char *names;
    size_t names_bytes;
    uint32_t name_count;
    uint32_t cursor;            /* the next entry: 0 ".", 1 "..", then names */
    bool listed;
};

static struct handle *handles;
static size_t handle_count, handle_room;

static void forget(struct handle *h)
{
    free(h->names);
    h->names = NULL;
    h->used = false;
}

static struct handle *handle_new(struct smb2_context *smb2, uint32_t tree, smb2_file_id id)
{
    size_t i;
    struct handle *h = NULL;
    uint32_t place;

    for (i = 0; i < handle_count; i++) {
        if (!handles[i].used) {
            h = &handles[i];
            break;
        }
    }

    if (h == NULL) {
        if (!grow((void **)&handles, &handle_room, handle_count, sizeof *handles)) {
            return NULL;
        }
        h = &handles[handle_count++];
        memset(h, 0, sizeof *h);
    }

    place = (uint32_t)(h - handles);
    h->used = true;
    h->generation++;
    h->smb2 = smb2;
    h->tree = tree;
    h->names = NULL;
    h->names_bytes = 0;
    h->name_count = 0;
    h->cursor = 0;
    h->listed = false;

    memset(id, 0, SMB2_FD_SIZE);
    memcpy(id, &place, 4);
    memcpy(id + 4, &h->generation, 4);
    memcpy(id + 8, &place, 4);
    memcpy(id + 12, &h->generation, 4);
    return h;
}

static struct handle *handle_of(struct smb2_context *smb2, const smb2_file_id id)
{
    uint32_t place, generation, tree;
    struct handle *h;

    if (share_here(smb2, &tree) == NULL) {
        return NULL;
    }

    memcpy(&place, id, 4);
    memcpy(&generation, id + 4, 4);

    if (place >= handle_count) {
        return NULL;
    }

    h = &handles[place];

    if (!h->used || h->generation != generation || h->smb2 != smb2 || h->tree != tree) {
        return NULL;
    }

    return h;
}

/*
 * ------------------------------------------------------------------------
 * Answers.
 * ------------------------------------------------------------------------
 */

/* A refusal with its own status, queued here (a handler's `> 0`): left
 * to libsmb2, every refusal is "not implemented". */
static int refuse(struct smb2_context *smb2, uint16_t command, uint32_t status)
{
    struct smb2_error_reply err;
    struct smb2_pdu *pdu;

    memset(&err, 0, sizeof(err));
    pdu = smb2_cmd_error_reply_async(smb2, &err, command, status, NULL, NULL);

    if (pdu == NULL) {
        return -1;
    }

    smb2_set_pdu_message_id(smb2, pdu, smb2->message_id);
    smb2_queue_pdu(smb2, pdu);
    return 1;
}

/* Seconds since 1970 as Windows counts time: tenths of a microsecond
 * since 1601. */
static uint64_t filetime(uint64_t seconds)
{
    return (seconds + 11644473600ull) * 10000000ull;
}

static uint32_t attributes_of(const struct facts *f)
{
    return f->dir ? SMB2_FILE_ATTRIBUTE_DIRECTORY : SMB2_FILE_ATTRIBUTE_NORMAL;
}

static uint64_t allocated(const struct facts *f)
{
    return (f->size + 4095u) & ~(uint64_t)4095u;
}

static void timeval_of(struct smb2_timeval *tv, uint64_t seconds)
{
    tv->tv_sec = (time_t)seconds;
    tv->tv_usec = 0;
}

/*
 * ------------------------------------------------------------------------
 * Signing in (N12a).
 * ------------------------------------------------------------------------
 */

/*
 * **Who may sign in**: the account and nothing else, its password the NT
 * hash libsmb2 checks the client's answer against. Another name is refused
 * here, before any answer is computed; a wrong password is refused by the
 * check, as "logon failure".
 */
static int authorize(struct smb2_server *server, struct smb2_context *smb2,
                     const char *user, const char *domain, const char *workstation)
{
    (void)server;
    (void)domain;

    if (user == NULL || account[0] == '\0' || strcasecmp(user, account) != 0) {
        said("refused ", user != NULL ? user : "a guest",
             ", who is not this machine's account");
        return -1;
    }

    smb2_set_user(smb2, user);
    smb2_set_password(smb2, hashed);
    said("asked by ", user, workstation != NULL && workstation[0] != '\0'
                            ? " - checking its answer" : "");
    return 0;
}

static int signed_in(struct smb2_server *server, struct smb2_context *smb2)
{
    char dialect[16];

    (void)server;
    snprintf(dialect, sizeof dialect, "%04x", (unsigned)smb2_get_dialect(smb2));
    said("signed in, SMB dialect ", dialect, NULL);
    return 0;
}

static int logoff(struct smb2_server *server, struct smb2_context *smb2)
{
    (void)server;
    (void)smb2;
    said("signed out", NULL, NULL);
    return 0;
}

/*
 * ------------------------------------------------------------------------
 * Shares (N12b).
 * ------------------------------------------------------------------------
 */

/* Read only: read data, attributes and extended attributes, list and
 * traverse, read the security descriptor, wait on it. */
#define READ_ACCESS  0x001200a9u

/*
 * A share, by the last part of `\\machine\Share`: one of the shares by
 * name, any case. The pipe share `IPC$` is not one yet - listing shares is
 * how the Finder asks for it (N12d) - so it is, like a name not shared,
 * "bad network name".
 */
static int tree_connect(struct smb2_server *server, struct smb2_context *smb2,
                        struct smb2_tree_connect_request *req,
                        struct smb2_tree_connect_reply *rep)
{
    struct smb2_tree_connect_reply ok;
    struct smb2_pdu *pdu;
    const char *full, *name;
    size_t i;

    (void)server;
    (void)rep;

    full = req->path != NULL && req->path_length > 0
           ? smb2_utf16_to_utf8(req->path, req->path_length / 2) : NULL;
    name = full != NULL ? strrchr(full, '\\') : NULL;
    name = name != NULL ? name + 1 : full;

    for (i = 0; name != NULL && i < share_count; i++) {
        if (strcasecmp(name, shares[i].name) == 0) {
            break;
        }
    }

    if (name == NULL || i == share_count) {
        said("asked for a share that is not offered: ", name != NULL ? name : "?", NULL);
        free((void *)full);
        return refuse(smb2, SMB2_TREE_CONNECT, SMB2_STATUS_BAD_NETWORK_NAME);
    }

    free((void *)full);

    if (!grow((void **)&links, &link_room, link_count, sizeof *links)) {
        return refuse(smb2, SMB2_TREE_CONNECT, SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }

    memset(&ok, 0, sizeof ok);
    ok.share_type = SMB2_SHARE_TYPE_DISK;
    ok.maximal_access = READ_ACCESS;

    pdu = smb2_cmd_tree_connect_reply_async(smb2, &ok, TREE_BASE + (uint32_t)i, NULL, NULL);
    if (pdu == NULL) {
        return -1;
    }

    links[link_count].smb2 = smb2;
    links[link_count].tree = TREE_BASE + (uint32_t)i;
    link_count++;

    smb2_set_pdu_message_id(smb2, pdu, smb2->message_id);
    smb2_queue_pdu(smb2, pdu);
    said("opened the share ", shares[i].name, ", read only");
    return 1;
}

static int tree_disconnect(struct smb2_server *server, struct smb2_context *smb2,
                           const uint32_t tree_id)
{
    size_t i;
    uint32_t tree = smb2->hdr.sync.tree_id;

    (void)server;
    (void)tree_id;

    for (i = 0; i < handle_count; i++) {
        if (handles[i].used && handles[i].smb2 == smb2 && handles[i].tree == tree) {
            forget(&handles[i]);
        }
    }

    for (i = 0; i < link_count; i++) {
        if (links[i].smb2 == smb2 && links[i].tree == tree) {
            links[i] = links[--link_count];
            break;
        }
    }

    return 0;
}

/* What asks to change something: writing, appending, attributes, deleting,
 * the security descriptor, or everything. */
#define WRITE_ACCESS  (0x00000002u | 0x00000004u | 0x00000010u | 0x00000100u \
                       | 0x00010000u | 0x00040000u | 0x00080000u               \
                       | SMB2_GENERIC_WRITE | SMB2_GENERIC_ALL)

#define FILE_OPENED  1u

static int create(struct smb2_server *server, struct smb2_context *smb2,
                  struct smb2_create_request *req, struct smb2_create_reply *rep)
{
    const struct share *s;
    struct handle *h;
    struct facts f;
    char path[DISK_PATH_MAX];
    uint32_t tree, status = 0;

    (void)server;
    s = share_here(smb2, &tree);

    if (s == NULL) {
        status = SMB2_STATUS_NETWORK_NAME_DELETED;
    } else if (!smb_share_path(s->path, req->name, path, sizeof path)) {
        status = SMB2_STATUS_OBJECT_NAME_INVALID;
    } else if ((req->desired_access & WRITE_ACCESS) != 0
               || (req->create_options & SMB2_FILE_DELETE_ON_CLOSE) != 0
               || (req->create_disposition != SMB2_FILE_OPEN
                   && req->create_disposition != SMB2_FILE_OPEN_IF)) {
        status = SMB2_STATUS_ACCESS_DENIED;
    } else if (!facts_of(path, &f)) {
        status = SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
    } else if (f.dir && (req->create_options & SMB2_FILE_NON_DIRECTORY_FILE)) {
        status = SMB2_STATUS_FILE_IS_A_DIRECTORY;
    } else if (!f.dir && (req->create_options & SMB2_FILE_DIRECTORY_FILE)) {
        status = SMB2_STATUS_NOT_A_DIRECTORY;
    }

    if (status == 0) {
        h = handle_new(smb2, tree, rep->file_id);
        if (h == NULL) {
            status = SMB2_STATUS_INSUFFICIENT_RESOURCES;
        } else {
            put_text(h->path, sizeof h->path, path);
            h->facts = f;
        }
    }

    if (status != 0) {
        /* libsmb2 frees the name only with a reply of its own making. */
        if (req->name != NULL) {
            smb2_free_data(smb2, (void *)req->name);
            req->name = NULL;
        }
        return refuse(smb2, SMB2_CREATE, status);
    }

    rep->oplock_level = 0;
    rep->create_action = FILE_OPENED;
    rep->creation_time = rep->last_access_time = filetime(f.written);
    rep->last_write_time = rep->change_time = filetime(f.written);
    rep->allocation_size = allocated(&f);
    rep->end_of_file = f.size;
    rep->file_attributes = attributes_of(&f);
    return 0;
}

#define CLOSE_POSTQUERY  0x0001u

static int close_handle(struct smb2_server *server, struct smb2_context *smb2,
                        struct smb2_close_request *req, struct smb2_close_reply *rep)
{
    struct handle *h = handle_of(smb2, req->file_id);

    (void)server;

    if (h == NULL) {
        return refuse(smb2, SMB2_CLOSE, SMB2_STATUS_FILE_CLOSED);
    }

    if (req->flags & CLOSE_POSTQUERY) {
        rep->flags = CLOSE_POSTQUERY;
        rep->creation_time = rep->last_access_time = filetime(h->facts.written);
        rep->last_write_time = rep->change_time = filetime(h->facts.written);
        rep->allocation_size = allocated(&h->facts);
        rep->end_of_file = h->facts.size;
        rep->file_attributes = attributes_of(&h->facts);
    }

    forget(h);
    return 0;
}

/* A folder's names, every page of them, into the handle. */
static bool list_folder(struct handle *h)
{
    uint64_t offset = 0;

    free(h->names);
    h->names = NULL;
    h->names_bytes = 0;
    h->name_count = 0;

    for (;;) {
        const struct disk_reply *rp;
        char *bigger;

        if (disk_ask(DISK_OP_LIST, h->path, 0, offset, 0, -1) != DISK_OK) {
            return false;
        }

        rp = disk_said();
        bigger = realloc(h->names, h->names_bytes + rp->length + 1);

        if (bigger == NULL) {
            return false;
        }

        h->names = bigger;
        memcpy(h->names + h->names_bytes, rp->u.data, rp->length);
        h->names_bytes += rp->length;
        h->name_count += rp->count;

        if (!rp->more) {
            break;
        }

        offset = rp->offset;
    }

    h->listed = true;
    return true;
}

static const char *name_at(const struct handle *h, uint32_t n)
{
    const char *c = h->names;

    while (n-- > 0) {
        c += strlen(c) + 1;
    }

    return c;
}

/* A pattern as SMB gives one - "*", a name, or "*.txt" - matched without
 * regard to case. */
static bool matches(const char *pattern, const char *name)
{
    if (*pattern == '\0') {
        return *name == '\0';
    }

    if (*pattern == '*') {
        for (;;) {
            if (matches(pattern + 1, name)) {
                return true;
            }
            if (*name == '\0') {
                return false;
            }
            name++;
        }
    }

    if (*name == '\0') {
        return false;
    }

    if (*pattern != '?') {
        char a = *pattern, b = *name;

        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');

        if (a != b) {
            return false;
        }
    }

    return matches(pattern + 1, name + 1);
}

/*
 * **A listing is encoded here, not by libsmb2.** Its encoder writes each
 * entry's NextEntryOffset as the entry's place from the start of the
 * buffer, where MS-FSCC 2.4 has it from the start of *this* entry, and pads
 * entries to four bytes where they are aligned to eight - so a client reads
 * the first two entries and then garbage (smbclient: "."  "..", then
 * NT_STATUS_INFO_LENGTH_MISMATCH). libsmb2 is not modified; its
 * passthrough mode sends a buffer as it is given, and is switched on for
 * the one reply and off again.
 */
static uint8_t *listing;
static size_t listing_room;

static bool room_for(size_t bytes)
{
    while (listing_room < bytes) {
        if (!grow((void **)&listing, &listing_room, listing_room, 1)) {
            return false;
        }
    }

    return true;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* The fixed part of an entry, before its name: FileIdBothDirectoryInformation
 * (MS-FSCC 2.4.17) carries a short name, FileIdFullDirectoryInformation
 * (2.4.18) does not. */
#define BOTH_FIXED  104u
#define FULL_FIXED   80u

static int query_directory(struct smb2_server *server, struct smb2_context *smb2,
                           struct smb2_query_directory_request *req,
                           struct smb2_query_directory_reply *rep)
{
    struct handle *h = handle_of(smb2, req->file_id);
    const char *pattern = req->name != NULL && req->name[0] != '\0' ? req->name : "*";
    bool both = req->file_information_class == SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION;
    uint32_t fixed = both ? BOTH_FIXED : FULL_FIXED;
    uint32_t used = 0, last = 0, end = 0, count = 0, total;
    struct smb2_query_directory_reply out;
    struct smb2_pdu *pdu;

    (void)server;
    (void)rep;

    if (h == NULL) {
        return refuse(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_FILE_CLOSED);
    }

    if (!h->facts.dir) {
        return refuse(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_INVALID_PARAMETER);
    }

    if (!both && req->file_information_class != SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION) {
        return refuse(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_INVALID_INFO_CLASS);
    }

    if (!h->listed || (req->flags & (SMB2_RESTART_SCANS | SMB2_REOPEN))) {
        if (!list_folder(h)) {
            return refuse(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_OBJECT_NAME_NOT_FOUND);
        }
        h->cursor = 0;
    }

    total = h->name_count + 2;

    while (h->cursor < total) {
        const char *name;
        struct facts f;
        struct smb2_utf16 *wide;
        uint32_t bytes, wire;
        uint8_t *e;

        if (h->cursor < 2) {
            name = h->cursor == 0 ? "." : "..";
            f = h->facts;
            f.dir = true;
            f.size = 0;
        } else {
            char path[DISK_PATH_MAX];
            size_t at = strlen(h->path);

            name = name_at(h, h->cursor - 2);

            if (!matches(pattern, name) || at + 1 + strlen(name) >= sizeof path) {
                h->cursor++;
                continue;
            }

            memcpy(path, h->path, at);
            path[at] = '/';
            strcpy(path + at + 1, name);

            if (!facts_of(path, &f)) {
                h->cursor++;
                continue;
            }
        }

        if (h->cursor < 2 && !matches(pattern, name)) {
            h->cursor++;
            continue;
        }

        wide = smb2_utf8_to_utf16(name);
        if (wide == NULL) {
            h->cursor++;
            continue;
        }

        bytes = 2u * (uint32_t)wide->len;
        wire = PAD_TO_64BIT(fixed + bytes);

        /* What does not fit waits for the next query; the first always
         * goes, since a client offers room for at least one. */
        if ((count > 0 && used + fixed + bytes > req->output_buffer_length)
            || !room_for(used + wire)) {
            free(wide);
            break;
        }

        if (count > 0) {
            put32(listing + last, used - last);
        }

        e = listing + used;
        memset(e, 0, wire);
        put64(e + 8, filetime(f.written));
        put64(e + 16, filetime(f.written));
        put64(e + 24, filetime(f.written));
        put64(e + 32, filetime(f.written));
        put64(e + 40, f.size);
        put64(e + 48, allocated(&f));
        put32(e + 56, attributes_of(&f));
        put32(e + 60, bytes);
        memcpy(e + fixed, wide->val, bytes);
        free(wide);

        last = used;
        end = used + fixed + bytes;
        used += wire;
        count++;
        h->cursor++;

        if (req->flags & SMB2_RETURN_SINGLE_ENTRY) {
            break;
        }
    }

    if (count == 0) {
        return refuse(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_NO_MORE_FILES);
    }

    /* The last entry ends at its name, not at its padding. */
    memset(&out, 0, sizeof out);
    out.output_buffer = listing;
    out.output_buffer_length = end;

    smb2->passthrough = 1;
    pdu = smb2_cmd_query_directory_reply_async(smb2, req, &out, NULL, NULL);
    smb2->passthrough = 0;

    if (pdu == NULL) {
        return -1;
    }

    smb2_set_pdu_message_id(smb2, pdu, smb2->message_id);
    smb2_queue_pdu(smb2, pdu);
    return 1;
}

/* The answer to a query of facts, kept until libsmb2 has encoded it. */
static union {
    struct smb2_file_basic_info basic;
    struct smb2_file_standard_info standard;
    struct smb2_file_all_info all;
    struct smb2_file_network_open_info network_open;
    struct smb2_file_attribute_tag_info tag;
    struct smb2_file_fs_volume_info volume;
    struct smb2_file_fs_size_info size;
    struct smb2_file_fs_full_size_info full_size;
    struct smb2_file_fs_device_info device;
    struct smb2_file_fs_attribute_info attribute;
    struct smb2_file_fs_sector_size_info sector;
} info;

static void basic_of(struct smb2_file_basic_info *b, const struct facts *f)
{
    timeval_of(&b->creation_time, f->written);
    timeval_of(&b->last_access_time, f->written);
    timeval_of(&b->last_write_time, f->written);
    timeval_of(&b->change_time, f->written);
    b->file_attributes = attributes_of(f);
}

static void standard_of(struct smb2_file_standard_info *s, const struct facts *f)
{
    s->allocation_size = allocated(f);
    s->end_of_file = f->size;
    s->number_of_links = 1;
    s->directory = f->dir ? 1 : 0;
}

/* The disk's size, as the folder's volume: total and free, in blocks. */
static void space(uint64_t *units, uint64_t *available, uint32_t *sectors_per_unit)
{
    const struct disk_super *sb;

    *units = 0;
    *available = 0;
    *sectors_per_unit = 8;

    if (disk_ask(DISK_OP_SUPER, "/Home", 0, 0, 0, -1) != DISK_OK) {
        return;
    }

    sb = &disk_said()->u.super;
    if (sb->block_size >= 512) {
        *sectors_per_unit = sb->block_size / 512;
    }
    *units = sb->blocks;
    *available = sb->free_known ? sb->free_blocks : 0;
}

#define FS_CASE_PRESERVED   0x00000002u
#define FS_UNICODE_ON_DISK  0x00000004u
#define FS_READ_ONLY        0x00080000u

static int query_info(struct smb2_server *server, struct smb2_context *smb2,
                      struct smb2_query_info_request *req,
                      struct smb2_query_info_reply *rep)
{
    struct handle *h = handle_of(smb2, req->file_id);
    const struct share *s = share_here(smb2, NULL);
    uint32_t length = 0;

    (void)server;

    if (h == NULL || s == NULL) {
        return refuse(smb2, SMB2_QUERY_INFO, SMB2_STATUS_FILE_CLOSED);
    }

    memset(&info, 0, sizeof info);

    if (req->info_type == SMB2_0_INFO_FILE) {
        switch (req->file_info_class) {
        case SMB2_FILE_BASIC_INFORMATION:
            basic_of(&info.basic, &h->facts);
            length = sizeof info.basic;
            break;
        case SMB2_FILE_STANDARD_INFORMATION:
            standard_of(&info.standard, &h->facts);
            length = sizeof info.standard;
            break;
        case SMB2_FILE_ALL_INFORMATION:
            basic_of(&info.all.basic, &h->facts);
            standard_of(&info.all.standard, &h->facts);
            info.all.access_flags = READ_ACCESS;
            length = sizeof info.all;
            break;
        case SMB2_FILE_NETWORK_OPEN_INFORMATION:
            timeval_of(&info.network_open.creation_time, h->facts.written);
            timeval_of(&info.network_open.last_access_time, h->facts.written);
            timeval_of(&info.network_open.last_write_time, h->facts.written);
            timeval_of(&info.network_open.change_time, h->facts.written);
            info.network_open.allocation_size = allocated(&h->facts);
            info.network_open.end_of_file = h->facts.size;
            info.network_open.file_attributes = attributes_of(&h->facts);
            length = sizeof info.network_open;
            break;
        case SMB2_FILE_ATTRIBUTE_TAG_INFORMATION:
            info.tag.file_attributes = attributes_of(&h->facts);
            length = sizeof info.tag;
            break;
        default:
            break;
        }
    } else if (req->info_type == SMB2_0_INFO_FILESYSTEM) {
        switch (req->file_info_class) {
        case SMB2_FILE_FS_VOLUME_INFORMATION:
            info.volume.volume_serial_number = 0x4b4f534du;
            info.volume.volume_label = (const uint8_t *)s->name;
            info.volume.volume_label_length = (uint32_t)strlen(s->name) * 2;
            length = sizeof info.volume;
            break;
        case SMB2_FILE_FS_SIZE_INFORMATION:
            space(&info.size.total_allocation_units, &info.size.available_allocation_units,
                  &info.size.sectors_per_allocation_unit);
            info.size.bytes_per_sector = 512;
            length = sizeof info.size;
            break;
        case SMB2_FILE_FS_FULL_SIZE_INFORMATION:
            space(&info.full_size.total_allocation_units,
                  &info.full_size.caller_available_allocation_units,
                  &info.full_size.sectors_per_allocation_unit);
            info.full_size.actual_available_allocation_units =
                info.full_size.caller_available_allocation_units;
            info.full_size.bytes_per_sector = 512;
            length = sizeof info.full_size;
            break;
        case SMB2_FILE_FS_DEVICE_INFORMATION:
            info.device.device_type = FILE_DEVICE_DISK;
            length = sizeof info.device;
            break;
        case SMB2_FILE_FS_ATTRIBUTE_INFORMATION:
            info.attribute.filesystem_attributes = FS_CASE_PRESERVED | FS_UNICODE_ON_DISK
                                                   | FS_READ_ONLY;
            info.attribute.maximum_component_name_length = 255;
            info.attribute.filesystem_name = (const uint8_t *)"kfs";
            info.attribute.filesystem_name_length = 6;
            length = sizeof info.attribute;
            break;
        case SMB2_FILE_FS_SECTOR_SIZE_INFORMATION:
            info.sector.logical_bytes_per_sector = 512;
            info.sector.physical_bytes_per_sector_for_atomicity = 512;
            info.sector.physical_bytes_per_sector_for_performance = 512;
            info.sector.file_system_effective_physical_bytes_per_sector_for_atomicity = 512;
            length = sizeof info.sector;
            break;
        default:
            break;
        }
    }

    if (length == 0) {
        return refuse(smb2, SMB2_QUERY_INFO, SMB2_STATUS_NOT_SUPPORTED);
    }

    rep->output_buffer = &info;
    rep->output_buffer_length = length;
    return 0;
}

/*
 * A file's bytes: from the disk into the read region, and from it into the
 * reply, which libsmb2 frees once sent. A read at or past the end is
 * "end of file", as a client expects to be told.
 */
static int read_file(struct smb2_server *server, struct smb2_context *smb2,
                     struct smb2_read_request *req, struct smb2_read_reply *rep)
{
    struct handle *h = handle_of(smb2, req->file_id);
    uint64_t want;
    uint8_t *data;

    (void)server;

    if (h == NULL) {
        return refuse(smb2, SMB2_READ, SMB2_STATUS_FILE_CLOSED);
    }

    if (h->facts.dir) {
        return refuse(smb2, SMB2_READ, SMB2_STATUS_INVALID_DEVICE_REQUEST);
    }

    want = req->length < READ_MOST ? req->length : READ_MOST;

    if (req->offset >= h->facts.size || want == 0) {
        return refuse(smb2, SMB2_READ, SMB2_STATUS_END_OF_FILE);
    }

    if (!read_region()
        || disk_ask(DISK_OP_READ, h->path, DISK_REGION, req->offset, want, read_cap) != DISK_OK) {
        return refuse(smb2, SMB2_READ, SMB2_STATUS_UNEXPECTED_IO_ERROR);
    }

    want = disk_said()->bytes;

    if (want == 0) {
        return refuse(smb2, SMB2_READ, SMB2_STATUS_END_OF_FILE);
    }

    data = malloc(want);
    if (data == NULL) {
        return refuse(smb2, SMB2_READ, SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }

    memcpy(data, read_at, want);
    rep->data = data;
    rep->data_length = (uint32_t)want;
    rep->data_remaining = 0;
    return 0;
}

/* Everything that would change a byte: a read-only share's answer. */
static int write_file(struct smb2_server *server, struct smb2_context *smb2,
                      struct smb2_write_request *req, struct smb2_write_reply *rep)
{
    (void)server;
    (void)req;
    (void)rep;
    return refuse(smb2, SMB2_WRITE, SMB2_STATUS_ACCESS_DENIED);
}

static int set_info(struct smb2_server *server, struct smb2_context *smb2,
                    struct smb2_set_info_request *req)
{
    (void)server;
    (void)req;
    return refuse(smb2, SMB2_SET_INFO, SMB2_STATUS_ACCESS_DENIED);
}

static int refuse_ioctl(struct smb2_server *server, struct smb2_context *smb2,
                 struct smb2_ioctl_request *req, struct smb2_ioctl_reply *rep)
{
    (void)server;
    (void)req;
    (void)rep;
    return refuse(smb2, SMB2_IOCTL, SMB2_STATUS_NOT_SUPPORTED);
}

static int change_notify(struct smb2_server *server, struct smb2_context *smb2,
                         struct smb2_change_notify_request *req,
                         struct smb2_change_notify_reply *rep)
{
    (void)server;
    (void)req;
    (void)rep;
    return refuse(smb2, SMB2_CHANGE_NOTIFY, SMB2_STATUS_NOT_SUPPORTED);
}

static int nothing_to_flush(struct smb2_server *server, struct smb2_context *smb2,
                            struct smb2_flush_request *req)
{
    (void)server;
    (void)smb2;
    (void)req;
    return 0;
}

static int echo(struct smb2_server *server, struct smb2_context *smb2)
{
    (void)server;
    (void)smb2;
    return 0;
}

/* A connection gone: its files closed and its shares let go. */
static int gone(struct smb2_server *server, struct smb2_context *smb2)
{
    size_t i;

    (void)server;

    for (i = 0; i < handle_count; i++) {
        if (handles[i].used && handles[i].smb2 == smb2) {
            forget(&handles[i]);
        }
    }

    for (i = 0; i < link_count; ) {
        if (links[i].smb2 == smb2) {
            links[i] = links[--link_count];
        } else {
            i++;
        }
    }

    said("a connection ended", NULL, NULL);
    return 0;
}

static struct smb2_server_request_handlers handlers = {
    .destruction_event   = gone,
    .authorize_user      = authorize,
    .session_established = signed_in,
    .logoff_cmd          = logoff,
    .tree_connect_cmd    = tree_connect,
    .tree_disconnect_cmd = tree_disconnect,
    .create_cmd          = create,
    .close_cmd           = close_handle,
    .flush_cmd           = nothing_to_flush,
    .read_cmd            = read_file,
    .write_cmd           = write_file,
    .ioctl_cmd           = refuse_ioctl,
    .echo_cmd            = echo,
    .query_directory_cmd = query_directory,
    .change_notify_cmd   = change_notify,
    .query_info_cmd      = query_info,
    .set_info_cmd        = set_info,
};

static struct smb2_server server;

static void arrived(struct smb2_context *smb2, void *data)
{
    (void)data;
    smb2_set_version(smb2, SMB2_VERSION_ANY);
    said("somebody connected", NULL, NULL);
}

/* `PORT ACCOUNT HASH` from the boot, or false and why. */
static bool configure(const char **why)
{
    char text[160];
    char name[64], hash[40];
    unsigned port = 0;
    long n = kosmos_boot_option("opt/kosmos/smbd", text, sizeof text - 1);
    int i;

    if (n <= 0) {
        *why = "not asked to share (opt/kosmos/smbd)";
        return false;
    }

    text[n] = '\0';

    /* Commas as well as spaces: a machine's command line splits its words
     * at spaces, so a value there has none (`hal/pc/boot_option.c`). */
    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] == ',') {
            text[i] = ' ';
        }
    }

    if (sscanf(text, "%u %63s %39s", &port, name, hash) != 3 || port == 0 || port > 65535
        || strlen(hash) != 32) {
        *why = "opt/kosmos/smbd is PORT,ACCOUNT,32 hex digits";
        return false;
    }

    for (i = 0; i < 32; i++) {
        char c = hash[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            *why = "the hash is not 32 hex digits";
            return false;
        }
    }

    server.port = (uint16_t)port;
    snprintf(account, sizeof account, "%s", name);
    snprintf(hashed, sizeof hashed, "ntlm:%s", hash);
    return true;
}

/*
 * The shares, from `opt/kosmos/smbd_shares=NAME:/Home/FOLDER,...`: each a
 * name and a folder of `/Home` - `/Home` itself, or under it. A share
 * outside `/Home` is not one this door could reach, so it is refused here
 * with a sentence rather than at every request.
 */
static void shares_from_boot(void)
{
    char text[1024];
    char *item, *rest;
    long n = kosmos_boot_option("opt/kosmos/smbd_shares", text, sizeof text - 1);
    size_t room = 0;

    if (n <= 0) {
        said("no folders are shared (opt/kosmos/smbd_shares)", NULL, NULL);
        return;
    }

    text[n] = '\0';

    for (item = text; item != NULL && *item != '\0'; item = rest) {
        char *colon;
        struct share *s;

        rest = strchr(item, ',');
        if (rest != NULL) {
            *rest++ = '\0';
        }

        colon = strchr(item, ':');
        if (colon == NULL || colon == item) {
            said("a share is NAME:/Home/FOLDER, not ", item, NULL);
            continue;
        }
        *colon = '\0';

        if (strncmp(colon + 1, "/Home", 5) != 0
            || (colon[6] != '\0' && colon[6] != '/')
            || strstr(colon + 1, "/..") != NULL || strstr(colon + 1, "/./") != NULL) {
            said("refused the share ", item, ": its folder is not in /Home");
            continue;
        }

        if (!grow((void **)&shares, &room, share_count, sizeof *shares)) {
            return;
        }

        s = &shares[share_count++];
        put_text(s->name, sizeof s->name, item);
        put_text(s->path, sizeof s->path, colon + 1);

        while (strlen(s->path) > 5 && s->path[strlen(s->path) - 1] == '/') {
            s->path[strlen(s->path) - 1] = '\0';
        }

        said("sharing ", s->name, " - read only");
    }
}

void smbd_server(long net_cap, long console_cap, long disk_cap)
{
    const char *why = NULL;
    char port[8];

    console = console_cap;
    disk = disk_cap;
    smb_kit_start(net_cap);

    memset(&server, 0, sizeof(server));

    if (!configure(&why)) {
        said(why, NULL, NULL);
        return;
    }

    shares_from_boot();

    server.handlers = &handlers;
    server.signing_enabled = 1;
    server.allow_anonymous = 0;
    server.max_transact_size = READ_MOST;
    server.max_read_size = READ_MOST;
    server.max_write_size = READ_MOST;
    snprintf(server.hostname, sizeof server.hostname, "KOSMOS");
    snprintf(port, sizeof port, "%u", (unsigned)server.port);

    for (;;) {
        int err;

        said("sharing on port ", port, ", SMB 2 and 3");
        err = smb2_serve_port(&server, 16, arrived, NULL);

        {
            char code[16];

            snprintf(code, sizeof code, "%d", err);
            said("the server loop ended (", code, "); again in a second");
        }

        kosmos_sleep(250);
    }
}
