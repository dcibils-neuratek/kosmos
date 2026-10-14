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
 * **N12b**: folders of `/Home` as shares, **read only** - a client
 * connects to one, lists its folders, reads its files' facts and fetches
 * their bytes, and anything that would change a byte is refused as "access
 * denied". The files come from `diskfs` through the door every program
 * reaches `/Home` by, as `diskproto.h` asks them, so a share can never show
 * more than `/Home` - and a name that climbs ("..") or names a stream (":")
 * is refused before it is asked (`smbpath.c`).
 *
 * **N12c, as built: told through a door of its own** (`smbdproto.h`).
 * init starts it at every boot, and it holds no port until the Servers
 * window or the `sharing` program says START - with the shares they set
 * one at a time and the account they named. The account's password crosses
 * once, in ACCOUNT, and only its NT hash is kept, in the keyring under
 * KEY_KIND_SHARING through the keyring's `sharing` door. Stopped, it waits
 * on its door; serving, it reads the door between libsmb2's passes.
 *
 * It is handed, at boot: the stack's capability, as a client of it; the
 * console's, so what it does is said in the log; the disk's main door,
 * which reaches `/Home` and nothing else; its own door; and the keyring's
 * `sharing` door.
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
#include "diskask.h"
#include "keyproto.h"
#include "smbdproto.h"
#include "mdnsproto.h"
#include "init/say.h"
#include "smbpath.h"
#include "smbrpc.h"

#include "smb_kit.h"
#include "kits/network/netclient.h"
#include "md4.h"

static long console = -1;
static uint32_t connections;        /* clients connected now */
static bool local_only;             /* This network only, as START said */
static struct smb2_server server;   /* the one this serves; its name for the share list */

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

static struct diskask disk = { .ep = -1 };

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

    if (diskask(&disk, DISK_OP_GETATTR, path, 0, 0, 0, -1, NULL, 0) != DISK_OK) {
        return false;
    }

    node = &diskask_said(&disk)->node;

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
    bool writable;              /* N14: marked so; read only otherwise */
};

static struct share *shares;
static size_t share_count;

/* A tree id is a share's place plus this, so none is ever 0; IPC$'s is
 * below them all (N12d). */
#define TREE_BASE  0x100u
#define IPC_TREE   0x0ffu

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
/* Whether this request's tree is one this connection connected: a share,
 * or IPC$. */
static bool linked(struct smb2_context *smb2, uint32_t *tree)
{
    uint32_t id = smb2->hdr.sync.tree_id;
    size_t i;

    for (i = 0; i < link_count; i++) {
        if (links[i].smb2 == smb2 && links[i].tree == id) {
            if (tree != NULL) {
                *tree = id;
            }
            return true;
        }
    }

    return false;
}

/* The share this request's tree is, when this connection connected it -
 * none for IPC$, which is pipes rather than a folder. */
static const struct share *share_here(struct smb2_context *smb2, uint32_t *tree)
{
    uint32_t id;

    if (!linked(smb2, &id) || id == IPC_TREE || id - TREE_BASE >= share_count) {
        return NULL;
    }

    if (tree != NULL) {
        *tree = id;
    }

    return &shares[id - TREE_BASE];
}

static bool ipc_here(struct smb2_context *smb2)
{
    uint32_t id;

    return linked(smb2, &id) && id == IPC_TREE;
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

    /* N14: opened to change it, in a writable share; and to be deleted
     * when it is closed. */
    bool writable;
    bool delete_on_close;

    /* A folder's names, read when it is first listed. */
    char *names;
    size_t names_bytes;
    uint32_t name_count;
    uint32_t cursor;            /* the next entry: 0 ".", 1 "..", then names */
    bool listed;

    /* IPC$'s srvsvc pipe (N12d): what was written and not yet a whole
     * request, and the answers not yet read. */
    bool pipe;
    int rpc;                    /* SMB_RPC_SRVSVC or SMB_RPC_LSARPC */
    uint8_t *in, *out;
    size_t in_len, out_len;
};

static struct handle *handles;
static size_t handle_count, handle_room;

static void forget(struct handle *h)
{
    free(h->names);
    free(h->in);
    free(h->out);
    h->names = NULL;
    h->in = h->out = NULL;
    h->in_len = h->out_len = 0;
    h->pipe = false;
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
    h->pipe = false;
    h->in = h->out = NULL;
    h->in_len = h->out_len = 0;

    memset(id, 0, SMB2_FD_SIZE);
    memcpy(id, &place, 4);
    memcpy(id + 4, &h->generation, 4);
    memcpy(id + 8, &place, 4);
    memcpy(id + 12, &h->generation, 4);
    return h;
}

static bool last_opened(struct smb2_context *smb2, smb2_file_id out);
static void none_opened(struct smb2_context *smb2);

static struct handle *handle_of(struct smb2_context *smb2, const smb2_file_id given)
{
    uint32_t place, generation, tree;
    struct handle *h;
    const uint8_t *id = given;
    smb2_file_id last;
    static const smb2_file_id RELATED = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    if (!linked(smb2, &tree)) {
        return NULL;
    }

    /*
     * **A compound's "the file just opened"** (MS-SMB2 3.3.5.2.7.2): the
     * Mac's Finder sends CREATE, QUERY_INFO and CLOSE in one, the last two
     * naming the file as all ones - meaning the one the CREATE before them
     * opened. libsmb2 hands them over as they are, so this keeps it.
     */
    if (memcmp(given, RELATED, sizeof RELATED) == 0) {
        if (!last_opened(smb2, last)) {
            return NULL;
        }

        id = last;
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
 * Who is connected (N12c-3): for the window's Connected now, its
 * Disconnect, and This network only.
 * ------------------------------------------------------------------------
 */

struct visitor {
    bool used;
    uint32_t id;                    /* from 1, never reused while serving */
    struct smb2_context *smb2;
    uint8_t address[4];
    uint64_t since;                 /* counter ticks */
    char account[64];
    char share[64];
    smb2_file_id last;              /* the last file it opened, for a compound */
    bool has_last;
};

/*
 * **In slabs that never move and are never freed**, linked as they are made:
 * the door's thread walks them to answer CONNECTION at once while the
 * serving thread fills them, so a slab is linked only once it is whole
 * (a release store; the walk an acquire load), and a visitor's words read
 * half-written say a moment's state, asked again half a second later.
 */
#define VISITORS_A_SLAB 16

struct visitor_slab {
    struct visitor v[VISITORS_A_SLAB];
    struct visitor_slab *next;
};

static struct visitor_slab first_slab;
static uint32_t next_visitor = 1;

static struct visitor_slab *slab_after(struct visitor_slab *slab)
{
    return __atomic_load_n(&slab->next, __ATOMIC_ACQUIRE);
}

static struct visitor *visitor_of(struct smb2_context *smb2)
{
    struct visitor_slab *slab;
    size_t i;

    for (slab = &first_slab; slab != NULL; slab = slab_after(slab)) {
        for (i = 0; i < VISITORS_A_SLAB; i++) {
            if (slab->v[i].used && slab->v[i].smb2 == smb2) {
                return &slab->v[i];
            }
        }
    }

    return NULL;
}

/* A place for one more: an unused one, or a new slab linked at the end. */
static struct visitor *visitor_new(void)
{
    struct visitor_slab *slab, *last = NULL, *made;
    size_t i;

    for (slab = &first_slab; slab != NULL; last = slab, slab = slab_after(slab)) {
        for (i = 0; i < VISITORS_A_SLAB; i++) {
            if (!slab->v[i].used) {
                return &slab->v[i];
            }
        }
    }

    made = calloc(1, sizeof *made);
    if (made == NULL) {
        return NULL;
    }

    __atomic_store_n(&last->next, made, __ATOMIC_RELEASE);
    return &made->v[0];
}

static void visitors_gone(void)
{
    struct visitor_slab *slab;
    size_t i;

    for (slab = &first_slab; slab != NULL; slab = slab_after(slab)) {
        for (i = 0; i < VISITORS_A_SLAB; i++) {
            slab->v[i].used = false;
        }
    }
}

/* A CREATE that failed: what follows it in a compound has no file. */
static void none_opened(struct smb2_context *smb2)
{
    struct visitor *v = visitor_of(smb2);

    if (v != NULL) {
        v->has_last = false;
    }
}

static bool last_opened(struct smb2_context *smb2, smb2_file_id out)
{
    struct visitor *v = visitor_of(smb2);

    if (v == NULL || !v->has_last) {
        return false;
    }

    memcpy(out, v->last, SMB2_FD_SIZE);
    return true;
}

static void address_text(const uint8_t a[4], char *out, size_t size)
{
    snprintf(out, size, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

/*
 * ------------------------------------------------------------------------
 * Answers.
 * ------------------------------------------------------------------------
 */

/* A refusal with its own status, queued here (a handler's `> 0`): left
 * to libsmb2, every refusal is "not implemented". */
/*
 * **Every refusal said**, but the ones that are a conversation's normal end
 * - no more names, the end of a file, an empty pipe: what a client asks
 * that this does not answer is how the next thing to build is found (N12d,
 * the Mac's Finder). `refusing` names what was asked, when a caller knows.
 */
static char refusing[160];

static const char *command_name(uint16_t c)
{
    static const char *const names[] = {
        "NEGOTIATE", "SESSION_SETUP", "LOGOFF", "TREE_CONNECT", "TREE_DISCONNECT",
        "CREATE", "CLOSE", "FLUSH", "READ", "WRITE", "LOCK", "IOCTL", "CANCEL",
        "ECHO", "QUERY_DIRECTORY", "CHANGE_NOTIFY", "QUERY_INFO", "SET_INFO",
        "OPLOCK_BREAK" };

    return c < sizeof names / sizeof names[0] ? names[c] : "a command";
}

static int refuse(struct smb2_context *smb2, uint16_t command, uint32_t status)
{
    struct smb2_error_reply err;
    struct smb2_pdu *pdu;

    if (status != SMB2_STATUS_NO_MORE_FILES && status != SMB2_STATUS_END_OF_FILE
        && status != SMB2_STATUS_PIPE_EMPTY) {
        char what[200];

        snprintf(what, sizeof what, "%s with %08x%s%s", command_name(command),
                 (unsigned)status, refusing[0] ? ": " : "", refusing);
        said("refused ", what, NULL);
    }

    refusing[0] = '\0';

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

    /* This network only: an address outside this machine's own subnet is
     * refused before its answer is checked, as a logon failure. */
    if (local_only) {
        struct visitor *v = visitor_of(smb2);
        uint8_t here[4], mask[4];
        int i;

        if (v == NULL || net_client_address(here, mask) != NET_OK) {
            said("refused a visitor whose network is not known", NULL, NULL);
            return -1;
        }

        for (i = 0; i < 4; i++) {
            if ((v->address[i] & mask[i]) != (here[i] & mask[i])) {
                char from[16];

                address_text(v->address, from, sizeof from);
                said("refused ", from, ", which is not on this network");
                return -1;
            }
        }
    }

    {
        struct visitor *v = visitor_of(smb2);

        if (v != NULL) {
            put_text(v->account, sizeof v->account, user);
        }
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
/* Everything a file can be asked: what a writable share answers (N14). */
#define FULL_ACCESS  0x001f01ffu

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

    /* IPC$: the pipe share every client asks for after signing in, to ask
     * by DCE/RPC what is shared (N12d). */
    if (name != NULL && strcasecmp(name, "IPC$") == 0) {
        free((void *)full);

        if (!grow((void **)&links, &link_room, link_count, sizeof *links)) {
            return refuse(smb2, SMB2_TREE_CONNECT, SMB2_STATUS_INSUFFICIENT_RESOURCES);
        }

        memset(&ok, 0, sizeof ok);
        ok.share_type = SMB2_SHARE_TYPE_PIPE;
        ok.maximal_access = 0x0012019fu;        /* read and write a pipe */

        pdu = smb2_cmd_tree_connect_reply_async(smb2, &ok, IPC_TREE, NULL, NULL);
        if (pdu == NULL) {
            return -1;
        }

        links[link_count].smb2 = smb2;
        links[link_count].tree = IPC_TREE;
        link_count++;
        smb2_set_pdu_message_id(smb2, pdu, smb2->message_id);
        smb2_queue_pdu(smb2, pdu);
        return 1;
    }

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
    ok.maximal_access = shares[i].writable ? FULL_ACCESS : READ_ACCESS;

    pdu = smb2_cmd_tree_connect_reply_async(smb2, &ok, TREE_BASE + (uint32_t)i, NULL, NULL);
    if (pdu == NULL) {
        return -1;
    }

    links[link_count].smb2 = smb2;
    links[link_count].tree = TREE_BASE + (uint32_t)i;
    link_count++;

    smb2_set_pdu_message_id(smb2, pdu, smb2->message_id);
    smb2_queue_pdu(smb2, pdu);
    said("opened the share ", shares[i].name,
         shares[i].writable ? ", read and write" : ", read only");

    {
        struct visitor *v = visitor_of(smb2);

        if (v != NULL) {
            put_text(v->share, sizeof v->share, shares[i].name);
        }
    }

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

#define FILE_SUPERSEDED   0u
#define FILE_OPENED       1u
#define FILE_CREATED      2u
#define FILE_OVERWRITTEN  3u

/*
 * What the disk said, as the status an SMB client expects (N14): the
 * filesystem's own refusals arrive as DISK_ERR_KFS plus `-KFS_E_*`
 * (`diskproto.h`, `kfs.h`).
 */
#define KFS_REFUSED(n)  (DISK_ERR_KFS + (n))

static uint32_t status_of(uint32_t e)
{
    switch (e) {
    case DISK_OK:                       return 0;
    case KFS_REFUSED(7):                /* no such file */
    case KFS_REFUSED(8):                return SMB2_STATUS_OBJECT_PATH_NOT_FOUND;
    case KFS_REFUSED(9):                return SMB2_STATUS_FILE_IS_A_DIRECTORY;
    case KFS_REFUSED(10):               return SMB2_STATUS_OBJECT_NAME_COLLISION;
    case KFS_REFUSED(11):               return SMB2_STATUS_DIRECTORY_NOT_EMPTY;
    case KFS_REFUSED(12):
    case KFS_REFUSED(13):               return SMB2_STATUS_DISK_FULL;
    case KFS_REFUSED(20):
    case KFS_REFUSED(21):
    case KFS_REFUSED(18):               return SMB2_STATUS_OBJECT_NAME_INVALID;
    case DISK_ERR_RESERVED:             return SMB2_STATUS_ACCESS_DENIED;
    default:                            return SMB2_STATUS_UNSUCCESSFUL;
    }
}

/*
 * ------------------------------------------------------------------------
 * IPC$'s srvsvc pipe (N12d): the share list, answered by `smbrpc.c`.
 * ------------------------------------------------------------------------
 */

/* Requests written into the pipe, answered into its output as each is
 * whole: the shares as they are now, IPC$ last. */
static bool pipe_in(struct handle *h, const uint8_t *data, size_t len)
{
    struct smb_rpc_share *list;
    struct smb_rpc_server srv;
    int unknown = -1;
    uint8_t *bigger;
    size_t i, used;

    bigger = realloc(h->in, h->in_len + len);
    if (bigger == NULL) {
        return false;
    }

    h->in = bigger;
    memcpy(h->in + h->in_len, data, len);
    h->in_len += len;

    list = calloc(share_count + 1, sizeof *list);
    if (list == NULL) {
        return false;
    }

    for (i = 0; i < share_count; i++) {
        list[i].name = shares[i].name;
        list[i].type = SMB_RPC_DISK;
        list[i].comment = "";
    }

    list[share_count].name = "IPC$";
    list[share_count].type = SMB_RPC_IPC;
    list[share_count].comment = "IPC Service";
    srv.pipe = h->rpc;
    srv.hostname = server.hostname;
    srv.shares = list;
    srv.count = share_count + 1;
    srv.user = NULL;
    srv.unknown = &unknown;

    {
        struct visitor *v = visitor_of(h->smb2);

        srv.user = v != NULL ? v->account : "";
    }

    for (;;) {
        size_t room = h->out_len + 65536;

        bigger = realloc(h->out, room);
        if (bigger == NULL) {
            free(list);
            return false;
        }
        h->out = bigger;

        if (smb_rpc_answer(&srv, h->in, h->in_len, &used, h->out, room, &h->out_len) != 0) {
            said("a request on the srvsvc pipe it could not answer", NULL, NULL);
            h->in_len = 0;
            free(list);
            return false;
        }

        if (unknown >= 0) {
            char what[48];

            snprintf(what, sizeof what, "%d on %s", unknown,
                     h->rpc == SMB_RPC_LSARPC ? "lsarpc" : "srvsvc");
            said("answered with a fault: operation ", what, NULL);
            unknown = -1;
        }

        if (used == 0) {
            break;
        }

        memmove(h->in, h->in + used, h->in_len - used);
        h->in_len -= used;
    }

    free(list);
    return true;
}

/* What the pipe has to say, up to `room`, taken from it. */
static size_t pipe_out(struct handle *h, uint8_t *to, size_t room)
{
    size_t n = h->out_len < room ? h->out_len : room;

    memcpy(to, h->out, n);
    memmove(h->out, h->out + n, h->out_len - n);
    h->out_len -= n;
    return n;
}

/* CREATE on IPC$: `srvsvc` and `lsarpc`, and no other pipe. */
static int create_pipe(struct smb2_context *smb2, struct smb2_create_request *req,
                       struct smb2_create_reply *rep)
{
    const char *name = req->name != NULL ? req->name : "";
    struct handle *h;
    uint32_t tree;

    while (*name == '\\') {
        name++;
    }

    if ((strcasecmp(name, "srvsvc") != 0 && strcasecmp(name, "lsarpc") != 0)
        || !linked(smb2, &tree)
        || (h = handle_new(smb2, tree, rep->file_id)) == NULL) {
        snprintf(refusing, sizeof refusing, "the pipe \"%s\" on IPC$", name);
        none_opened(smb2);
        if (req->name != NULL) {
            smb2_free_data(smb2, (void *)req->name);
            req->name = NULL;
        }
        return refuse(smb2, SMB2_CREATE, SMB2_STATUS_OBJECT_NAME_NOT_FOUND);
    }

    h->pipe = true;
    h->rpc = strcasecmp(name, "lsarpc") == 0 ? SMB_RPC_LSARPC : SMB_RPC_SRVSVC;

    {
        struct visitor *v = visitor_of(smb2);

        if (v != NULL) {
            memcpy(v->last, rep->file_id, sizeof v->last);
            v->has_last = true;
        }
    }
    put_text(h->path, sizeof h->path,
             h->rpc == SMB_RPC_LSARPC ? "\\PIPE\\lsarpc" : "\\PIPE\\srvsvc");
    memset(&h->facts, 0, sizeof h->facts);
    rep->create_action = FILE_OPENED;
    rep->file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
    said(h->rpc == SMB_RPC_LSARPC ? "opened the lsarpc pipe, for who signed in"
                                  : "opened the srvsvc pipe, for the share list", NULL, NULL);
    return 0;
}

static int create(struct smb2_server *server, struct smb2_context *smb2,
                  struct smb2_create_request *req, struct smb2_create_reply *rep)
{
    const struct share *s;
    struct handle *h;
    struct facts f;
    char path[DISK_PATH_MAX];
    uint32_t tree, status = 0, action = FILE_OPENED;
    bool writable = false;

    (void)server;

    if (ipc_here(smb2)) {
        return create_pipe(smb2, req, rep);
    }

    s = share_here(smb2, &tree);

    if (s == NULL) {
        status = SMB2_STATUS_NETWORK_NAME_DELETED;
    } else if (!smb_share_path(s->path, req->name, path, sizeof path)) {
        status = SMB2_STATUS_OBJECT_NAME_INVALID;
    } else {
        /*
         * N14: what a writable share allows - a file or a folder made, a
         * file emptied (overwrite, supersede), opened to be written or
         * deleted - and a share marked read only refuses, as it always did.
         */
        uint32_t disp = req->create_disposition;
        bool wants = (req->desired_access & WRITE_ACCESS) != 0
                     || (req->create_options & SMB2_FILE_DELETE_ON_CLOSE) != 0
                     || (disp != SMB2_FILE_OPEN && disp != SMB2_FILE_OPEN_IF);
        bool dir = (req->create_options & SMB2_FILE_DIRECTORY_FILE) != 0;
        bool here = facts_of(path, &f);

        if (wants && !s->writable) {
            status = here || disp == SMB2_FILE_OPEN_IF || disp == SMB2_FILE_OPEN
                     ? (here ? SMB2_STATUS_ACCESS_DENIED : SMB2_STATUS_OBJECT_NAME_NOT_FOUND)
                     : SMB2_STATUS_ACCESS_DENIED;
        } else if (here && f.dir && (req->create_options & SMB2_FILE_NON_DIRECTORY_FILE)) {
            status = SMB2_STATUS_FILE_IS_A_DIRECTORY;
        } else if (here && !f.dir && dir) {
            status = SMB2_STATUS_NOT_A_DIRECTORY;
        } else if (here && disp == SMB2_FILE_CREATE) {
            status = SMB2_STATUS_OBJECT_NAME_COLLISION;
        } else if (!here && (disp == SMB2_FILE_OPEN || disp == SMB2_FILE_OVERWRITE)) {
            status = SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
        } else if (!here) {
            /* Made: a folder, or an empty file. */
            status = status_of(dir ? diskask(&disk, DISK_OP_MKDIR, path, 0, 0, 0, -1, NULL, 0)
                                   : diskask(&disk, DISK_OP_WRITE, path, 0, 0, 0, -1, NULL, 0));

            if (status == 0 && !facts_of(path, &f)) {
                status = SMB2_STATUS_UNSUCCESSFUL;
            }

            action = FILE_CREATED;
        } else if (!f.dir && (disp == SMB2_FILE_SUPERSEDE || disp == SMB2_FILE_OVERWRITE
                              || disp == SMB2_FILE_OVERWRITE_IF)) {
            /* Emptied, to be written again. */
            status = status_of(diskask(&disk, DISK_OP_WRITE, path, 0, 0, 0, -1, NULL, 0));

            if (status == 0 && !facts_of(path, &f)) {
                status = SMB2_STATUS_UNSUCCESSFUL;
            }

            action = disp == SMB2_FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
        }

        writable = wants && s->writable;
    }

    if (status == 0) {
        h = handle_new(smb2, tree, rep->file_id);
        if (h == NULL) {
            status = SMB2_STATUS_INSUFFICIENT_RESOURCES;
        } else {
            struct visitor *v = visitor_of(smb2);

            put_text(h->path, sizeof h->path, path);
            h->facts = f;
            h->writable = writable;
            h->delete_on_close = (req->create_options & SMB2_FILE_DELETE_ON_CLOSE) != 0;

            if (v != NULL) {
                memcpy(v->last, rep->file_id, sizeof v->last);
                v->has_last = true;
            }
        }
    }

    if (status != 0) {
        none_opened(smb2);
        snprintf(refusing, sizeof refusing, "\"%s\" access %08x disposition %u options %08x",
                 req->name != NULL ? req->name : "", (unsigned)req->desired_access,
                 (unsigned)req->create_disposition, (unsigned)req->create_options);
        /* libsmb2 frees the name only with a reply of its own making. */
        if (req->name != NULL) {
            smb2_free_data(smb2, (void *)req->name);
            req->name = NULL;
        }
        return refuse(smb2, SMB2_CREATE, status);
    }

    rep->oplock_level = 0;
    rep->create_action = action;
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

    /* N14: a file or folder marked to go, gone; one written, as it is now. */
    if (h->delete_on_close) {
        if (diskask(&disk, DISK_OP_DELETE, h->path, 0, 0, 0, -1, NULL, 0) == DISK_OK) {
            said("deleted ", h->path, NULL);
        }
    } else if (h->writable) {
        (void)facts_of(h->path, &h->facts);
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

        if (diskask(&disk, DISK_OP_LIST, h->path, 0, offset, 0, -1, NULL, 0) != DISK_OK) {
            return false;
        }

        rp = diskask_said(&disk);
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

    {
        int was = smb2->passthrough;

        smb2->passthrough = 1;
        pdu = smb2_cmd_query_directory_reply_async(smb2, req, &out, NULL, NULL);
        smb2->passthrough = was;
    }

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

    if (diskask(&disk, DISK_OP_SUPER, "/Home", 0, 0, 0, -1, NULL, 0) != DISK_OK) {
        return;
    }

    sb = &diskask_said(&disk)->u.super;
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

    /* A file in a share, or the pipe, whose facts are a file's with none. */
    if (h == NULL || (s == NULL && !h->pipe)) {
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
            info.volume.volume_label = (const uint8_t *)(s != NULL ? s->name : "IPC$");
            info.volume.volume_label_length = (uint32_t)strlen((const char *)
                                                               info.volume.volume_label) * 2;
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
        snprintf(refusing, sizeof refusing, "info type %u class %u",
                 (unsigned)req->info_type, (unsigned)req->file_info_class);
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

    /* The pipe: its answers, as much as asked; nothing yet, "empty". */
    if (h->pipe) {
        size_t n = h->out_len < req->length ? h->out_len : req->length;

        if (n == 0) {
            return refuse(smb2, SMB2_READ, SMB2_STATUS_PIPE_EMPTY);
        }

        data = malloc(n);
        if (data == NULL) {
            return refuse(smb2, SMB2_READ, SMB2_STATUS_INSUFFICIENT_RESOURCES);
        }

        rep->data_length = (uint32_t)pipe_out(h, data, n);
        rep->data = data;
        rep->data_remaining = 0;
        return 0;
    }

    if (h->facts.dir) {
        return refuse(smb2, SMB2_READ, SMB2_STATUS_INVALID_DEVICE_REQUEST);
    }

    want = req->length < READ_MOST ? req->length : READ_MOST;

    if (req->offset >= h->facts.size || want == 0) {
        return refuse(smb2, SMB2_READ, SMB2_STATUS_END_OF_FILE);
    }

    if (!read_region()
        || diskask(&disk, DISK_OP_READ, h->path, DISK_REGION, req->offset, want, read_cap, NULL, 0) != DISK_OK) {
        return refuse(smb2, SMB2_READ, SMB2_STATUS_UNEXPECTED_IO_ERROR);
    }

    want = diskask_said(&disk)->bytes;

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
    struct handle *h = handle_of(smb2, req->file_id);

    (void)server;

    /* The pipe takes a request; a file in a share takes nothing. */
    if (h != NULL && h->pipe) {
        if (req->buf == NULL || !pipe_in(h, req->buf, req->length)) {
            return refuse(smb2, SMB2_WRITE, SMB2_STATUS_INVALID_PARAMETER);
        }

        rep->count = req->length;
        rep->remaining = 0;
        return 0;
    }

    /*
     * N14: a file opened to be written, in a writable share - its bytes
     * through the region into `DISK_OP_WRITE_AT`, one transaction a piece,
     * so a copy of any size takes this one region and no more.
     */
    if (h != NULL && h->writable && !h->facts.dir) {
        uint32_t status;

        if (req->buf == NULL || req->length > READ_MOST) {
            return refuse(smb2, SMB2_WRITE, SMB2_STATUS_INVALID_PARAMETER);
        }

        if (!read_region()) {
            return refuse(smb2, SMB2_WRITE, SMB2_STATUS_INSUFFICIENT_RESOURCES);
        }

        memcpy(read_at, req->buf, req->length);
        status = status_of(diskask(&disk, DISK_OP_WRITE_AT, h->path, DISK_REGION, req->offset, req->length, read_cap, NULL, 0));

        if (status != 0) {
            return refuse(smb2, SMB2_WRITE, status);
        }

        if (req->offset + req->length > h->facts.size) {
            h->facts.size = req->offset + req->length;
        }

        rep->count = req->length;
        rep->remaining = 0;
        return 0;
    }

    return refuse(smb2, SMB2_WRITE, SMB2_STATUS_ACCESS_DENIED);
}

/* A disk request with a second path, `u.to`: a rename's destination. */
static uint32_t disk_rename(const char *from, const char *to)
{
    size_t n = strlen(to);

    if (n >= DISK_PATH_MAX) {
        return DISK_ERR_BAD_OP;
    }

    return diskask(&disk, DISK_OP_RENAME, from, 0, 0, 0, -1, to, (uint32_t)n);
}

/*
 * N14: what a client changes about an open file - its name (MS-FSCC
 * 2.4.37), that it is to go when closed (2.4.11), where it ends (2.4.13).
 * Its dates and its allocation are taken and let be: the disk keeps when a
 * file was written itself, and has nothing to reserve ahead.
 */
static int set_info(struct smb2_server *server, struct smb2_context *smb2,
                    struct smb2_set_info_request *req)
{
    struct handle *h = handle_of(smb2, req->file_id);
    const uint8_t *in = req->input_data;
    uint32_t len = req->buffer_length, tree, status = 0;

    (void)server;

    if (h == NULL || h->pipe) {
        return refuse(smb2, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
    }

    if (!h->writable || req->info_type != SMB2_0_INFO_FILE || in == NULL) {
        return refuse(smb2, SMB2_SET_INFO, SMB2_STATUS_ACCESS_DENIED);
    }

    switch (req->file_info_class) {
    case SMB2_FILE_BASIC_INFORMATION:
    case SMB2_FILE_ALLOCATION_INFORMATION:
        return 0;

    case SMB2_FILE_END_OF_FILE_INFORMATION: {
        uint64_t end;

        if (len < 8 || h->facts.dir) {
            return refuse(smb2, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
        }

        memcpy(&end, in, 8);            /* little-endian, as this machine is */
        status = status_of(diskask(&disk, DISK_OP_WRITE_AT, h->path, DISK_CUT, end, 0, -1, NULL, 0));

        if (status == 0) {
            h->facts.size = end;
        }

        break;
    }

    case SMB2_FILE_DISPOSITION_INFORMATION:
        if (len < 1) {
            return refuse(smb2, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
        }

        /* A folder with something in it is not let go, said now rather
         * than at the close. */
        if (in[0] != 0 && h->facts.dir && (!list_folder(h) || h->name_count > 0)) {
            status = SMB2_STATUS_DIRECTORY_NOT_EMPTY;
            break;
        }

        h->delete_on_close = in[0] != 0;
        break;

    case SMB2_FILE_RENAME_INFORMATION: {
        uint32_t name_bytes;
        const struct share *s = share_here(smb2, &tree);
        const char *name;
        char to[DISK_PATH_MAX];
        struct facts there;

        if (len < 20 || s == NULL) {
            return refuse(smb2, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
        }

        memcpy(&name_bytes, in + 16, 4);

        if (name_bytes == 0 || name_bytes % 2 != 0 || 20u + name_bytes > len) {
            return refuse(smb2, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
        }

        name = smb2_utf16_to_utf8((const uint16_t *)(const void *)(in + 20), name_bytes / 2);

        if (name == NULL || !smb_share_path(s->path, name, to, sizeof to)) {
            free((void *)name);
            status = SMB2_STATUS_OBJECT_NAME_INVALID;
            break;
        }

        free((void *)name);

        if (facts_of(to, &there) && strcasecmp(to, h->path) != 0) {
            if (in[0] == 0 || there.dir) {
                status = SMB2_STATUS_OBJECT_NAME_COLLISION;
                break;
            }

            status = status_of(diskask(&disk, DISK_OP_DELETE, to, 0, 0, 0, -1, NULL, 0));

            if (status != 0) {
                break;
            }
        }

        status = status_of(disk_rename(h->path, to));

        if (status == 0) {
            said("renamed to ", to, NULL);
            put_text(h->path, sizeof h->path, to);
        }

        break;
    }

    default:
        snprintf(refusing, sizeof refusing, "set info class %u", (unsigned)req->file_info_class);
        return refuse(smb2, SMB2_SET_INFO, SMB2_STATUS_NOT_SUPPORTED);
    }

    return status != 0 ? refuse(smb2, SMB2_SET_INFO, status) : 0;
}

/* A pipe's answer for TRANSCEIVE, kept until libsmb2 has copied it. */
static uint8_t transceived[65536];

static int refuse_ioctl(struct smb2_server *server, struct smb2_context *smb2,
                 struct smb2_ioctl_request *req, struct smb2_ioctl_reply *rep)
{
    struct handle *h;

    (void)server;

    /* TRANSCEIVE: a request written and its answer read, in one. */
    if (req->ctl_code == SMB2_FSCTL_PIPE_TRANSCEIVE) {
        h = handle_of(smb2, req->file_id);

        if (h == NULL || !h->pipe || req->input == NULL
            || !pipe_in(h, req->input, req->input_count)) {
            return refuse(smb2, SMB2_IOCTL, SMB2_STATUS_INVALID_PARAMETER);
        }

        rep->output_count = (uint32_t)pipe_out(h, transceived,
                                               req->max_output_response < sizeof transceived
                                               ? req->max_output_response
                                               : sizeof transceived);
        rep->output = transceived;
        return 0;
    }

    /* No DFS here: "not found", as a server without it says. */
    if (req->ctl_code == 0x00060194u || req->ctl_code == 0x000601b0u) {
        return refuse(smb2, SMB2_IOCTL, SMB2_STATUS_NOT_FOUND);
    }

    snprintf(refusing, sizeof refusing, "control code %08x", (unsigned)req->ctl_code);
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

    if (connections > 0) {
        connections--;
    }

    {
        struct visitor *v = visitor_of(smb2);

        if (v != NULL) {
            v->used = false;
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

/*
 * ------------------------------------------------------------------------
 * The door (`smbdproto.h`, N12c): started, stopped and told what to share.
 * ------------------------------------------------------------------------
 */

static long door = -1;
static long keyring = -1;

static bool wanted;                 /* asked to serve, and not since to stop */
static bool account_pending;        /* PATIENT: the keyring could not answer for it yet */
static int listen_error;            /* what the last try at the port said */
static struct smbd_start asked;     /* the last START's port, account and name */

/* Every share and every handle let go: after the loop, or the shares reset. */
static void forget_everything(void)
{
    size_t i;

    for (i = 0; i < handle_count; i++) {
        if (handles[i].used) {
            forget(&handles[i]);
        }
    }

    link_count = 0;
}

static bool ended(const char *field, size_t size)
{
    return memchr(field, '\0', size) != NULL;
}

/* The keyring, through its `sharing` door: this machine's account and its
 * NT hash, kept under a kind no other door sees. */
static struct message kmsg, krep;

#define SERVICE "this machine"

static uint32_t keyring_ask(uint32_t op, const char *name, const uint8_t *secret,
                            uint32_t bytes)
{
    struct key_request *rq = (struct key_request *)kmsg.data;
    const struct key_reply *rp = (const struct key_reply *)krep.data;
    uint32_t error;

    if (keyring < 0) {
        return KEY_ERR_SEALED;
    }

    memset(&kmsg, 0, sizeof kmsg);
    memset(&krep, 0, sizeof krep);
    kmsg.length = sizeof *rq;
    rq->op = op;
    rq->entry.kind = KEY_KIND_SHARING;
    put_text(rq->entry.service, sizeof rq->entry.service, SERVICE);
    put_text(rq->entry.account, sizeof rq->entry.account, name);
    put_text(rq->entry.title, sizeof rq->entry.title, "Sharing this machine's folders");

    if (secret != NULL) {
        memcpy(rq->secret, secret, bytes);
        rq->secret_bytes = bytes;
    }

    if (kosmos_call(keyring, &kmsg, &krep) != 0 || krep.length < sizeof *rp) {
        error = KEY_ERR_SEALED;
    } else {
        error = rp->error;
    }

    memset(kmsg.data, 0, sizeof kmsg.data);
    return error;
}

/* The account's hash, from the keyring into `hashed` as libsmb2 takes it. */
static bool account_from_keyring(const char *name)
{
    const struct key_reply *rp = (const struct key_reply *)krep.data;
    static const char hex[] = "0123456789abcdef";
    int i;

    if (name[0] == '\0' || keyring_ask(KEY_OP_GET, name, NULL, 0) != 0
        || rp->secret_bytes != 16) {
        memset(krep.data, 0, sizeof krep.data);
        return false;
    }

    put_text(account, sizeof account, name);
    memcpy(hashed, "ntlm:", 5);

    for (i = 0; i < 16; i++) {
        hashed[5 + 2 * i] = hex[rp->secret[i] >> 4];
        hashed[6 + 2 * i] = hex[rp->secret[i] & 15];
    }

    hashed[37] = '\0';
    memset(krep.data, 0, sizeof krep.data);
    return true;
}

/* ACCOUNT: the password's NT hash - MD4 of its UTF-16 - kept, the password
 * itself never. */
static uint32_t set_account(const struct smbd_account *a)
{
    char password[SMBD_PASSWORD_MAX + 1];
    struct smb2_utf16 *wide;
    MD4_CTX ctx;
    uint8_t hash[16];
    uint32_t error;

    if (!ended(a->name, sizeof a->name) || a->name[0] == '\0') {
        return SMBD_ERR_NO_ACCOUNT;
    }

    /* No password: whether one is kept for this name, for the window. */
    if (a->password_bytes == 0) {
        const struct key_reply *rp = (const struct key_reply *)krep.data;
        bool kept = keyring_ask(KEY_OP_GET, a->name, NULL, 0) == 0 && rp->secret_bytes == 16;

        memset(krep.data, 0, sizeof krep.data);
        return kept ? 0 : SMBD_ERR_NO_ACCOUNT;
    }

    if (a->password_bytes > SMBD_PASSWORD_MAX) {
        return SMBD_ERR_PASSWORD;
    }

    memcpy(password, a->password, a->password_bytes);
    password[a->password_bytes] = '\0';
    wide = smb2_utf8_to_utf16(password);
    memset(password, 0, sizeof password);

    if (wide == NULL) {
        return SMBD_ERR_PASSWORD;
    }

    MD4Init(&ctx);
    MD4Update(&ctx, (unsigned char *)wide->val, (unsigned)wide->len * 2u);
    MD4Final(hash, &ctx);
    memset(wide->val, 0, (size_t)wide->len * 2u);
    free(wide);

    error = keyring_ask(KEY_OP_PUT, a->name, hash, sizeof hash);
    memset(hash, 0, sizeof hash);

    if (error != 0) {
        return SMBD_ERR_KEYRING;
    }

    said("kept the account ", a->name, "'s password, as its NT hash");

    /* Serving as it already: the new password from the next sign-in. */
    if (wanted && strcasecmp(a->name, account) == 0) {
        account_from_keyring(a->name);
    }

    return 0;
}

/* SHARE: a name nobody has taken, and a folder of `/Home` that exists. */
static size_t share_room;

static uint32_t add_share(const struct smbd_share *sh)
{
    struct facts f;
    struct share *s;
    size_t i, n;

    if (!ended(sh->name, sizeof sh->name) || !ended(sh->path, sizeof sh->path)
        || sh->name[0] == '\0' || strpbrk(sh->name, "\\/:*?\"<>|") != NULL
        || strncmp(sh->path, "/Home", 5) != 0
        || (sh->path[5] != '\0' && sh->path[5] != '/')
        || strstr(sh->path, "/..") != NULL || strstr(sh->path, "/./") != NULL) {
        return SMBD_ERR_SHARE;
    }

    for (i = 0; i < share_count; i++) {
        if (strcasecmp(shares[i].name, sh->name) == 0) {
            return SMBD_ERR_SHARE;
        }
    }

    if (!facts_of(sh->path, &f) || !f.dir) {
        return SMBD_ERR_NO_FOLDER;
    }

    if (!grow((void **)&shares, &share_room, share_count, sizeof *shares)) {
        return SMBD_ERR_SHARE;
    }

    s = &shares[share_count++];
    put_text(s->name, sizeof s->name, sh->name);
    put_text(s->path, sizeof s->path, sh->path);
    s->writable = (sh->flags & SMBD_SHARE_WRITE) != 0;

    while ((n = strlen(s->path)) > 5 && s->path[n - 1] == '/') {
        s->path[n - 1] = '\0';
    }

    said("sharing ", s->name, " - read only");
    return 0;
}

/* The connection after `after`, by id, as the window lists them. */
static uint64_t counter_hz;

static uint32_t next_connection(uint32_t after, struct smbd_connection *out)
{
    const struct visitor *best = NULL;
    struct visitor_slab *slab;
    size_t i;

    for (slab = &first_slab; slab != NULL; slab = slab_after(slab)) {
        for (i = 0; i < VISITORS_A_SLAB; i++) {
            const struct visitor *v = &slab->v[i];

            if (__atomic_load_n(&v->used, __ATOMIC_ACQUIRE) && v->id > after
                && (best == NULL || v->id < best->id)) {
                best = v;
            }
        }
    }

    if (best == NULL) {
        return SMBD_ERR_NONE;
    }

    if (counter_hz == 0) {
        struct sysinfo info;

        counter_hz = kosmos_sysinfo(&info) == 0 && info.counter_hz != 0 ? info.counter_hz
                                                                       : 62500000ull;
    }

    out->id = best->id;
    memcpy(out->address, best->address, 4);
    out->for_ms = (kosmos_ticks() - best->since) * 1000u / counter_hz;
    put_text(out->account, sizeof out->account, best->account);
    put_text(out->share, sizeof out->share, best->share);
    return 0;
}

/* Disconnect: its socket closed; libsmb2's next pass lets it go, and `gone`
 * forgets it. */
static uint32_t disconnect(uint32_t id)
{
    struct visitor_slab *slab;
    size_t i;

    for (slab = &first_slab; slab != NULL; slab = slab_after(slab)) {
      for (i = 0; i < VISITORS_A_SLAB; i++) {
        struct visitor *v = &slab->v[i];

        if (v->used && v->id == id) {
            char from[16];

            address_text(v->address, from, sizeof from);
            said("disconnected ", from, NULL);
            smb2_close_context(v->smb2);
            return 0;
        }
      }
    }

    return SMBD_ERR_NONE;
}

static uint32_t start(const struct smbd_start *st)
{
    if (wanted) {
        return SMBD_ERR_SERVING;
    }

    if (!ended(st->account, sizeof st->account) || !ended(st->hostname, sizeof st->hostname)) {
        return SMBD_ERR_BAD_OP;
    }

    if (st->flags & SMBD_ENCRYPT) {
        return SMBD_ERR_LATER;
    }

    if (share_count == 0) {
        return SMBD_ERR_NO_SHARES;
    }

    account_pending = false;

    if (!account_from_keyring(st->account)) {
        if ((st->flags & SMBD_PATIENT) && st->account[0] != '\0') {
            /* At boot `/Home`, where the keyring keeps its file, may not be
             * there yet: asked again each second until it answers. */
            account_pending = true;
            said("waiting for the keyring to hold ", st->account, "'s password");
        } else {
            return st->account[0] == '\0' || keyring >= 0 ? SMBD_ERR_NO_ACCOUNT
                                                          : SMBD_ERR_KEYRING;
        }
    }

    asked = *st;
    local_only = (st->flags & SMBD_LOCAL_ONLY) != 0;
    wanted = true;
    listen_error = 0;
    return 0;
}

/* How it is, as STATE says it - read by the door's thread as the serving
 * thread writes it; a word read half-changed says a moment's state, and is
 * asked again half a second later. */
static void state_into(struct smbd_reply *rp)
{
    rp->state = !wanted ? SMBD_STOPPED : server.listener_ready ? SMBD_SERVING
                                                               : SMBD_WAITING;
    rp->port = wanted ? asked.port : 0;
    rp->flags = wanted ? asked.flags : 0;
    rp->shares = (uint32_t)share_count;
    rp->connections = connections;
    rp->listen_error = listen_error;
    put_text(rp->account, sizeof rp->account, wanted ? account : "");
    put_text(rp->hostname, sizeof rp->hostname, wanted ? asked.hostname : "");
}

static void answer(struct message *msg, uint64_t sender)
{
    static struct message out;
    const struct smbd_request *rq = (const struct smbd_request *)msg->data;
    struct smbd_reply *rp = (struct smbd_reply *)out.data;

    memset(&out, 0, sizeof out);
    out.length = sizeof *rp;

    if (msg->cap_plus_one) {
        kosmos_cap_drop((long)msg->cap_plus_one - 1);
    }

    if (msg->length != sizeof *rq) {
        rp->error = SMBD_ERR_BAD_OP;
    } else {
        switch (rq->op) {
        case SMBD_OP_STATE:
            break;
        case SMBD_OP_START:
            rp->error = start(&rq->u.start);
            break;
        case SMBD_OP_STOP:
            if (wanted) {
                said("asked to stop sharing", NULL, NULL);
            }
            wanted = false;
            server.stop_requested = 1;
            break;
        case SMBD_OP_SHARES_CLEAR:
            forget_everything();
            share_count = 0;
            break;
        case SMBD_OP_SHARE:
            rp->error = add_share(&rq->u.share);
            break;
        case SMBD_OP_ACCOUNT:
            rp->error = set_account(&rq->u.account);
            break;
        case SMBD_OP_CONNECTION:
            rp->error = next_connection(rq->u.after, &rp->connection);
            break;
        case SMBD_OP_DISCONNECT:
            rp->error = disconnect(rq->u.after);
            break;
        default:
            rp->error = SMBD_ERR_BAD_OP;
            break;
        }
    }

    /* The password, if it came, gone from this process's copy of it. */
    memset(msg->data, 0, sizeof msg->data);

    state_into(rp);
    (void)kosmos_reply(sender, &out);
}

/*
 * **The door's own thread**: STATE answered here at once; everything else
 * handed to the serving thread through `inner`, a private endpoint, and its
 * answer handed back - so what changes shares and handles is one thread.
 */
static long inner = -1;

static void door_main(unsigned long unused)
{
    static struct message in, back, out;
    uint64_t sender;

    (void)unused;

    for (;;) {
        const struct smbd_request *rq = (const struct smbd_request *)in.data;

        if (kosmos_receive(door, &in, &sender, 0, 0) != 0) {
            continue;
        }

        if (in.cap_plus_one) {
            kosmos_cap_drop((long)in.cap_plus_one - 1);
            in.cap_plus_one = 0;
        }

        if (in.length == sizeof *rq
            && (rq->op == SMBD_OP_STATE || rq->op == SMBD_OP_CONNECTION)) {
            struct smbd_reply *rp = (struct smbd_reply *)out.data;

            memset(&out, 0, sizeof out);
            out.length = sizeof *rp;

            if (rq->op == SMBD_OP_CONNECTION) {
                rp->error = next_connection(rq->u.after, &rp->connection);
            }

            state_into(rp);
            (void)kosmos_reply(sender, &out);
            continue;
        }

        memset(&back, 0, sizeof back);
        if (kosmos_call(inner, &in, &back) != 0) {
            struct smbd_reply *rp = (struct smbd_reply *)back.data;

            memset(&back, 0, sizeof back);
            back.length = sizeof *rp;
            rp->error = SMBD_ERR_BAD_OP;
        }

        memset(in.data, 0, sizeof in.data);
        (void)kosmos_reply(sender, &back);
    }
}

/*
 * **Between libsmb2's passes, the door** - at most 100 ms apart, its
 * `select`'s bound - so the one thread that holds every share and handle
 * is the one that changes them, and nothing takes a lock.
 */
static void door_between_passes(struct smb2_server *srv, fd_set *rfds, fd_set *wfds)
{
    static struct message msg;
    uint64_t sender;

    (void)srv;
    (void)rfds;
    (void)wfds;

    while (kosmos_receive(inner, &msg, &sender, 1, 0) == 0) {
        answer(&msg, sender);
    }
}

static void arrived(struct smb2_context *smb2, void *data)
{
    struct visitor *v = visitor_new();
    char from[16] = "somebody";

    (void)data;
    smb2_set_version(smb2, SMB2_VERSION_ANY);

    /*
     * **Passthrough, for every connection** (N12d): without it libsmb2
     * drops an IOCTL's input and refuses to encode its output for any
     * control code but VALIDATE_NEGOTIATE_INFO - so a pipe's TRANSCEIVE,
     * which is how a client asks IPC$ for the share list, could not be
     * read or answered, and the connection was closed. Every other place
     * passthrough changes is a client's path (a request encoded, a reply
     * read) or something smbd refuses or encodes itself.
     */
    smb2_set_passthrough(smb2, 1);
    connections++;

    if (v != NULL) {
        memset(v->account, 0, sizeof v->account);
        v->has_last = false;
        memset(v->share, 0, sizeof v->share);
        v->id = next_visitor++;
        v->smb2 = smb2;
        v->since = kosmos_ticks();

        if (smb_kosmos_peer(smb2_get_fd(smb2), v->address)) {
            address_text(v->address, from, sizeof from);
        }

        /* Last, so a walk never sees it used and half made. */
        __atomic_store_n(&v->used, true, __ATOMIC_RELEASE);
    }

    said(from, " connected", NULL);
}

/* Ticks of the scheduler's clock in a second, for the wait between tries
 * at a port that would not open. */
#define A_SECOND  250u

/*
 * **This machine's sharing, said on the network** (N13): `mdnsd` is asked
 * to answer for `_smb._tcp` under the name on the network while this
 * serves, and to say it gone when it stops - so the Mac's Finder lists the
 * machine in its Locations without an address typed. Told, never waited
 * on: `mdnsd` answers at once.
 */
static long mdns = -1;

static void discovery(uint32_t op)
{
    struct mdns_request req;
    struct message out, back;

    if (mdns < 0) {
        return;
    }

    memset(&req, 0, sizeof(req));
    req.op = op;
    req.index = server.port;
    memcpy(req.name, "_smb._tcp", 10);
    snprintf(req.instance, sizeof req.instance, "%s", server.hostname);

    memset(&out, 0, sizeof(out));
    out.length = sizeof(req);
    memcpy(out.data, &req, sizeof(req));
    (void)kosmos_call(mdns, &out, &back);
}

void smbd_server(long net_cap, long console_cap, long disk_cap, long door_cap,
                 long keyring_cap, long mdns_cap)
{
    static struct message msg;
    uint64_t sender;

    console = console_cap;
    disk.ep = disk_cap;
    door = door_cap;
    keyring = keyring_cap;
    mdns = mdns_cap;
    smb_kit_start(net_cap);

    memset(&server, 0, sizeof(server));
    server.handlers = &handlers;
    server.signing_enabled = 1;
    server.allow_anonymous = 0;
    server.max_transact_size = READ_MOST;
    server.max_read_size = READ_MOST;
    server.max_write_size = READ_MOST;
    server.extra_service = door_between_passes;

    inner = kosmos_endpoint();

    if (door < 0 || inner < 0 || kosmos_thread_start(door_main, 0) < 0) {
        said("no door to be told what to share; sharing nothing", NULL, NULL);
        return;
    }

    for (;;) {
        char port[8], code[16];

        if (!wanted) {
            if (kosmos_receive(inner, &msg, &sender, 0, 0) == 0) {
                answer(&msg, sender);
            }
            continue;
        }

        if (account_pending) {
            if (account_from_keyring(asked.account)) {
                account_pending = false;
                said("the keyring holds ", asked.account, "'s password now");
            } else {
                if (kosmos_receive(inner, &msg, &sender, 0, A_SECOND) == 0) {
                    answer(&msg, sender);
                }
                continue;
            }
        }

        server.port = asked.port;
        server.stop_requested = 0;
        snprintf(server.hostname, sizeof server.hostname, "%s",
                 asked.hostname[0] != '\0' ? asked.hostname : "KOSMOS");
        snprintf(port, sizeof port, "%u", (unsigned)server.port);
        said("sharing on port ", port, ", SMB 2 and 3");

        discovery(MDNS_OP_PUBLISH);
        listen_error = smb2_serve_port(&server, 16, arrived, NULL);
        discovery(MDNS_OP_UNPUBLISH);
        forget_everything();
        connections = 0;
        visitors_gone();

        if (!wanted) {
            said("stopped sharing", NULL, NULL);
            continue;
        }

        /* It would not listen - no address yet, or the port taken: said,
         * and tried again in a second, answering the door meanwhile. */
        snprintf(code, sizeof code, " (%d)", listen_error);
        said("could not serve on port ", port, code);

        if (kosmos_receive(inner, &msg, &sender, 0, A_SECOND) == 0) {
            answer(&msg, sender);
        }
    }
}
