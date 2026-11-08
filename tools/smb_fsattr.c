/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **What a share says its volume is**, asked as macOS asks it: CREATE on the
 * share's root, QUERY_INFO for FileFsAttributeInformation, CLOSE - and the
 * attributes printed, so `tools/run_smbd.py` can hold `smbd` to them.
 *
 *   smb_fsattr smb://USER@127.0.0.1:PORT/SHARE PASSWORD
 *   -> attributes 0x00000006 read-only no
 *
 * Why it exists: `smbd` said FILE_READ_ONLY_VOLUME of every share from N12,
 * when every share was read only, and kept saying it after N14 made some
 * writable. Samba's `smbclient`, which the suite drives, never reads the
 * flag; macOS's smbfs does, and mounted Diego's read-and-write share read
 * only (8 November 2026). Written after libsmb2's own
 * `examples/smb2-raw-fsstat-async.c`, synchronously and for one class.
 */

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"

#define FS_READ_ONLY_VOLUME 0x00080000u

struct waiting {
    int done;
    int status;
    uint32_t attributes;
    int answered;
};

static int wait_for(struct smb2_context *smb2, struct waiting *w)
{
    while (!w->done) {
        struct pollfd pfd = { .fd = smb2_get_fd(smb2), .events = smb2_which_events(smb2) };

        if (poll(&pfd, 1, 1000) < 0) {
            return -1;
        }
        if (pfd.revents != 0 && smb2_service(smb2, pfd.revents) < 0) {
            fprintf(stderr, "smb_fsattr: %s\n", smb2_get_error(smb2));
            return -1;
        }
    }

    return 0;
}

static void opened(struct smb2_context *smb2, int status, void *data, void *private_data)
{
    struct waiting *w = private_data;

    (void)smb2;
    (void)data;
    if (w->status == 0) {
        w->status = status;
    }
}

static void queried(struct smb2_context *smb2, int status, void *data, void *private_data)
{
    struct waiting *w = private_data;
    struct smb2_query_info_reply *rep = data;

    (void)smb2;
    if (w->status == 0) {
        w->status = status;
    }
    if (status == 0 && rep != NULL && rep->output_buffer != NULL) {
        const struct smb2_file_fs_attribute_info *a = rep->output_buffer;

        w->attributes = a->filesystem_attributes;
        w->answered = 1;
    }
}

static void closed(struct smb2_context *smb2, int status, void *data, void *private_data)
{
    struct waiting *w = private_data;

    (void)smb2;
    (void)data;
    (void)status;
    w->done = 1;
}

int main(int argc, char **argv)
{
    struct smb2_context *smb2;
    struct smb2_url *url;
    struct smb2_create_request cr;
    struct smb2_query_info_request qi;
    struct smb2_close_request cl;
    struct smb2_pdu *pdu, *next;
    struct waiting w = { 0 };

    if (argc != 3) {
        fprintf(stderr, "usage: smb_fsattr smb://USER@HOST:PORT/SHARE PASSWORD\n");
        return 2;
    }

    smb2 = smb2_init_context();
    if (smb2 == NULL) {
        return 1;
    }

    url = smb2_parse_url(smb2, argv[1]);
    if (url == NULL) {
        fprintf(stderr, "smb_fsattr: %s\n", smb2_get_error(smb2));
        return 1;
    }

    smb2_set_password(smb2, argv[2]);
    smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);

    if (smb2_connect_share(smb2, url->server, url->share, url->user) < 0) {
        fprintf(stderr, "smb_fsattr: %s\n", smb2_get_error(smb2));
        return 1;
    }

    memset(&cr, 0, sizeof cr);
    cr.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
    cr.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
    cr.desired_access = SMB2_FILE_READ_ATTRIBUTES;
    cr.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE;
    cr.create_disposition = SMB2_FILE_OPEN;
    cr.name = "";

    memset(&qi, 0, sizeof qi);
    qi.info_type = SMB2_0_INFO_FILESYSTEM;
    qi.file_info_class = SMB2_FILE_FS_ATTRIBUTE_INFORMATION;
    qi.output_buffer_length = 65535;
    memcpy(qi.file_id, compound_file_id, SMB2_FD_SIZE);

    memset(&cl, 0, sizeof cl);
    memcpy(cl.file_id, compound_file_id, SMB2_FD_SIZE);

    pdu = smb2_cmd_create_async(smb2, &cr, opened, &w);
    if (pdu == NULL) {
        return 1;
    }
    next = smb2_cmd_query_info_async(smb2, &qi, queried, &w);
    if (next == NULL) {
        return 1;
    }
    smb2_add_compound_pdu(smb2, pdu, next);
    next = smb2_cmd_close_async(smb2, &cl, closed, &w);
    if (next == NULL) {
        return 1;
    }
    smb2_add_compound_pdu(smb2, pdu, next);
    smb2_queue_pdu(smb2, pdu);

    if (wait_for(smb2, &w) < 0 || w.status != 0 || !w.answered) {
        fprintf(stderr, "smb_fsattr: no attributes: status %d\n", w.status);
        return 1;
    }

    printf("attributes 0x%08x read-only %s\n", (unsigned)w.attributes,
           (w.attributes & FS_READ_ONLY_VOLUME) ? "yes" : "no");

    smb2_disconnect_share(smb2);
    smb2_destroy_url(url);
    smb2_destroy_context(smb2);
    return 0;
}
