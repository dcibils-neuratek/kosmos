/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_MDNSPROTO_H
#define KOSMOS_MDNSPROTO_H

#include <stdint.h>

#include "netproto.h"

/*
 * **What is on the network, by name: `/Discovery`** (`docs/sharing.md`
 * N11c) - `mdnsd`, which speaks multicast DNS (RFC 6762) and DNS service
 * discovery (RFC 6763) on the stack's port 5353 and group 224.0.0.251, and
 * keeps what the network says for as long as it says to.
 *
 * Asked by a message each, the answer in the reply; nothing a caller is
 * sent is longer than one, so a list of any length is asked for an item at
 * a time. Mounted for every program, as `/Network` is: what the local
 * network has on it is part of having the network.
 */

#define MDNS_OP_BROWSE    1u    /* `name` a service type, `_smb._tcp`: asked of the
                                   network now and again while it is browsed;
                                   -> how many instances are known (`count`) */
#define MDNS_OP_INSTANCE  2u    /* `name` the type, `index` which: -> its instance,
                                   host, port and address */
#define MDNS_OP_RESOLVE   3u    /* `name` a `.local` name: -> its address, the caller
                                   parked up to `wait_ticks` for it */
#define MDNS_OP_PUBLISH   4u    /* `name` a service type, `instance` what this machine
                                   calls itself, `index` the port: answered for
                                   and announced, as this machine's (N13) */
#define MDNS_OP_UNPUBLISH 5u    /* the same `name` and `instance`: said gone
                                   (a time to live of 0) and no longer answered */

#define MDNS_OK           0u
#define MDNS_ERR_BAD_OP   1u
#define MDNS_ERR_BAD_NAME 2u    /* not a service type, or not a `.local` name */
#define MDNS_ERR_NO_SUCH  3u    /* no instance that far along the list */
#define MDNS_ERR_TIMEOUT  4u    /* nobody answered for the name in time */
#define MDNS_ERR_NO_NETWORK 5u  /* no card, or no address yet to listen with */
#define MDNS_ERR_FULL     6u    /* no memory for another question */

#define MDNS_NAME_MAX     255u
#define MDNS_INSTANCE_MAX  63u  /* a label: RFC 6763 4.1.1 */
#define MDNS_MODEL_MAX     47u

struct mdns_request {
    uint32_t op;
    uint32_t index;                 /* instance: which; publish: the port */
    uint32_t wait_ticks;            /* scheduler ticks, as every wait */
    uint32_t reserved;
    char     name[MDNS_NAME_MAX + 1];
    char     instance[MDNS_INSTANCE_MAX + 1];   /* publish and unpublish */
};

struct mdns_reply {
    uint32_t error;
    uint32_t count;                 /* browse: instances known */
    uint16_t port;                  /* instance: its port */
    uint16_t has_address;           /* instance: whether its host's address is known */
    struct net_addr address;        /* instance and resolve */
    char     instance[MDNS_INSTANCE_MAX + 1];
    char     host[MDNS_NAME_MAX + 1];
    char     model[MDNS_MODEL_MAX + 1]; /* instance: what kind of machine, from its
                                           `_device-info._tcp` TXT - `Macmini9,1` */
};

_Static_assert(sizeof(struct mdns_request) == 336,
               "mdns.lua packs \"<I4I4I4I4c256c64\"");
_Static_assert(sizeof(struct mdns_reply) == 384,
               "mdns.lua reads \"<I4I4I2I2c4c64c256c48\"");

#endif
