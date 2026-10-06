/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The SMB Kit, from the side of the process that speaks SMB
 * (`docs/sharing.md`, *The SMB Kit*): libsmb2 as vendored, its platform
 * (`port/`), its socket on the network stack's ring (`smb_transport.c`) and
 * its cryptography the Crypto Kit's (`smb_crypto.c`).
 *
 * **C linked into the processes that speak SMB, and no Lua door.** smbfs is
 * the first (`user/servers/smbfs.c`), smbd the second when it comes; an
 * application never reaches this, because it reaches a share through the
 * namespace.
 *
 * Including this brings libsmb2's whole interface, public and private, in
 * the order its own files include it - after the platform's `config.h`,
 * since what a `struct smb2_context` holds depends on what that says.
 */
#ifndef KOSMOS_SMB_KIT_H
#define KOSMOS_SMB_KIT_H

#include "config.h"

#include <stdbool.h>
#include <stdint.h>

#include "compat.h"
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#include "libsmb2-private.h"

/* The stack every connection is opened through: a capability to
 * `/Network`'s server, which the process was handed. Before any connect. */
void smb_kit_start(long net_cap);

/*
 * What is known of one connection, for the process that owns it to say why
 * it ended and to wait on it.
 *
 * `handle` is the stack's name for it, for `NET_OP_POLL`. `taken` is that
 * the stack has sent bytes of ours, which it can only do once the far end
 * took the connection: a connection that closed with nothing taken was
 * refused or never answered, and one that closed after is a server that
 * hung up. `received` counts what arrived. `room` is the space in the ring
 * going out, so an owner knows whether waiting for room makes sense.
 */
struct smb_link {
    uint64_t handle;
    bool     taken;
    bool     closed;
    uint64_t received;
    uint32_t room;
};

bool smb_kit_link(int fd, struct smb_link *out);

/* The stack's refusal of the last connection asked for (`NET_ERR_*`), or
 * `NET_OK`: what a `connect` that failed at once was told. */
uint32_t smb_kit_last_refusal(void);

#endif /* KOSMOS_SMB_KIT_H */
