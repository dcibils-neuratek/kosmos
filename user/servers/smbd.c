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
 * **N12a, as built**: signing in, and nothing past it. One account, whose
 * NT hash is all a sign-in needs (NTLMv2 is computed from the hash, never
 * the password); a TREE_CONNECT is refused as a share that does not exist,
 * which is what tells a right password from a wrong one - "no such share"
 * after a sign-in, "logon failure" without. Folders of `/Home` as shares
 * are N12b; the account from the Servers window, its hash in the keyring,
 * N12c.
 *
 * **Started only when asked**: the boot's `opt/kosmos/smbd=PORT ACCOUNT
 * HASH` - a port, an account, and the account's NT hash as 32 hex digits -
 * which a test machine's command line carries. A machine not told to share
 * shares nothing. The hash on a command line is for a test account only;
 * the Servers window keeps the real one in the keyring (N12c).
 *
 * It is handed, at boot: the stack's capability, as a client of it, and the
 * console's, so what it does is said in the log.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "kosmos.h"
#include "init/say.h"

#include "smb_kit.h"

static long console = -1;

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
 * No shares yet (N12b): every one asked for is one that does not exist -
 * answered as Windows and Samba answer it, "bad network name", by this
 * handler queuing the reply itself (a handler's `> 0`). Left to libsmb2, a
 * refusal is "not implemented", which a client reads as a broken server.
 */
static int tree_connect(struct smb2_server *server, struct smb2_context *smb2,
                        struct smb2_tree_connect_request *req,
                        struct smb2_tree_connect_reply *rep)
{
    struct smb2_error_reply err;
    struct smb2_pdu *pdu;

    (void)server;
    (void)req;
    (void)rep;
    said("asked for a share; none are offered yet", NULL, NULL);

    memset(&err, 0, sizeof(err));
    pdu = smb2_cmd_error_reply_async(smb2, &err, SMB2_TREE_CONNECT,
                                     SMB2_STATUS_BAD_NETWORK_NAME, NULL, NULL);

    if (pdu == NULL) {
        return -1;
    }

    smb2_set_pdu_message_id(smb2, pdu, smb2->message_id);
    smb2_queue_pdu(smb2, pdu);
    return 1;
}

static int gone(struct smb2_server *server, struct smb2_context *smb2)
{
    (void)server;
    (void)smb2;
    said("a connection ended", NULL, NULL);
    return 0;
}

static struct smb2_server_request_handlers handlers = {
    .destruction_event  = gone,
    .authorize_user     = authorize,
    .session_established = signed_in,
    .logoff_cmd         = logoff,
    .tree_connect_cmd   = tree_connect,
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

    if (sscanf(text, "%u %63s %39s", &port, name, hash) != 3 || port == 0 || port > 65535
        || strlen(hash) != 32) {
        *why = "opt/kosmos/smbd is PORT ACCOUNT and 32 hex digits";
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

void smbd_server(long net_cap, long console_cap)
{
    const char *why = NULL;
    char port[8];

    console = console_cap;
    smb_kit_start(net_cap);

    memset(&server, 0, sizeof(server));

    if (!configure(&why)) {
        said(why, NULL, NULL);
        return;
    }

    server.handlers = &handlers;
    server.signing_enabled = 1;
    server.allow_anonymous = 0;
    snprintf(server.hostname, sizeof server.hostname, "KOSMOS");
    snprintf(port, sizeof port, "%u", (unsigned)server.port);

    for (;;) {
        int err;

        said("sharing on port ", port, ", SMB 2 and 3, signing in only (N12a)");
        err = smb2_serve_port(&server, 16, arrived, NULL);

        {
            char code[16];

            snprintf(code, sizeof code, "%d", err);
            said("the server loop ended (", code, "); again in a second");
        }

        kosmos_sleep(250);
    }
}
