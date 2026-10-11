/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The share list a client asks for over `IPC$`'s `srvsvc` pipe (`smbrpc.c`,
 * `docs/sharing.md` N12d).
 */

#ifndef KOSMOS_SMBRPC_H
#define KOSMOS_SMBRPC_H

#include <stddef.h>
#include <stdint.h>

#define SMB_RPC_DISK      0x00000000u   /* STYPE_DISKTREE */
#define SMB_RPC_IPC       0x80000003u   /* STYPE_IPC | STYPE_SPECIAL: IPC$ */

struct smb_rpc_share {
    const char *name;                   /* UTF-8 */
    uint32_t    type;
    const char *comment;
};

#define SMB_RPC_SRVSVC    0             /* the share list */
#define SMB_RPC_LSARPC    1             /* who a client signed in as */

struct smb_rpc_server {
    int         pipe;                   /* SMB_RPC_SRVSVC or SMB_RPC_LSARPC */
    const char *hostname;               /* NetBIOS's, as NetServerGetInfo says it */
    const struct smb_rpc_share *shares;
    size_t      count;
    const char *user;                   /* lsarpc: the account signed in */
    int        *unknown;                /* set to an operation not answered, when not NULL */
};

/*
 * One DCE/RPC PDU from the front of `in`: its answer appended to `out` at
 * `*out_used`, and how much of `in` it was in `*in_used` - 0 when `in` does
 * not yet hold a whole one. 0 when it was answered, -1 when it could not be:
 * not DCE/RPC 5.0, or an answer that would not fit in `out`.
 */
int smb_rpc_answer(const struct smb_rpc_server *server,
                   const uint8_t *in, size_t in_len, size_t *in_used,
                   uint8_t *out, size_t out_room, size_t *out_used);

#endif
