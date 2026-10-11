/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A name inside an SMB share, as a path on the disk (`smbd.c`, `docs/sharing.md`
 * N12b): the share's folder, then the name with its backslashes turned to
 * slashes. Refused: a part that is "." or "..", which could climb out of
 * the share; a ":", which names a stream this disk does not keep; and a
 * path too long to ask for. Apart from the server so the Mac can hold it to
 * its cases (`tools/test_smbpath.c`) - a client like Samba's cleans ".."
 * out of a name before sending it, so nothing over the wire in a test
 * reaches this refusal, and a client that does not clean is exactly the
 * one it is for.
 */

#include <string.h>

#include "smbpath.h"

bool smb_share_path(const char *root, const char *name, char *out, size_t room)
{
    size_t at = strlen(root), part = 0;
    const char *c;

    if (at >= room) {
        return false;
    }

    memcpy(out, root, at);

    for (c = name != NULL ? name : ""; ; c++) {
        bool end = *c == '\0' || *c == '\\' || *c == '/';

        if (end) {
            const char *start = c - part;

            if ((part == 1 && start[0] == '.') || (part == 2 && start[0] == '.' && start[1] == '.')) {
                return false;
            }

            if (part > 0) {
                if (at + 1 + part >= room) {
                    return false;
                }
                out[at++] = '/';
                memcpy(out + at, start, part);
                at += part;
            }

            part = 0;

            if (*c == '\0') {
                break;
            }
            continue;
        }

        if (*c == ':') {
            return false;
        }

        part++;
    }

    out[at] = '\0';
    return true;
}

