/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The two things Chocolate Quake's engine takes from `SDL_net.h`: the shape
 * of an address, which `net.h` keeps in every server-browser entry, and a
 * socket handle's type, which `net_socket.h` keeps in every connection.
 *
 * Nothing here sends a packet. The drivers that did are left out of the
 * Kosmos build, and the only one in its table is Loopback.
 */
#ifndef KOSMOS_QUAKE_SDL_NET_H
#define KOSMOS_QUAKE_SDL_NET_H

#include "SDL_stdinc.h"

typedef struct {
    Uint32 host;
    Uint16 port;
} IPaddress;

/* Opaque, as SDL_net's is, and never opened in this build. */
typedef struct _UDPsocket *UDPsocket;

#endif
