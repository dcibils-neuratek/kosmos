/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Quake III's network, as a game on one machine needs it: none
 * (`docs/quake3e.md` §5), in place of `net_ip.c`.
 *
 * A local game - single player, bots, a demo - is a client and a server in
 * one process, talking through `net_chan.c`'s loopback, which never
 * reaches these functions. So every address that is not the loopback or a
 * bot is one this build cannot reach, and says so; and a packet for one is
 * dropped. This is what Quake 1's port's network table is, with Loopback
 * alone in it.
 *
 * The real `net_ip.c` comes with multiplayer (Q6), once the POSIX layer
 * has datagram sockets: it reaches for `ioctl(FIONBIO)`, `<net/if.h>`,
 * `<ifaddrs.h>` and `select`, none of which is here today.
 */

#include "q_shared.h"
#include "qcommon.h"

#include <time.h>

void NET_Init(void)
{
    Com_Printf("No network: a game on this machine only\n");
}

void NET_Shutdown(void)
{
}

/*
 * The frame's wait for the next frame (`Com_Frame`), in microseconds. A
 * networked build waits on its sockets; with none, it is only the wait -
 * which is how a frame rate is held, and which the frame loop asks for in
 * slices of `com_yieldCPU`, so the window's events are read between them.
 */
qboolean NET_Sleep(int timeout)
{
    if (timeout > 0) {
        struct timespec t = {
            .tv_sec = timeout / 1000000,
            .tv_nsec = (long)(timeout % 1000000) * 1000,
        };

        nanosleep(&t, NULL);
    }

    return qtrue;
}

void Sys_SendPacket(int length, const void *data, const netadr_t *to)
{
    (void)length;
    (void)data;
    (void)to;
}

/* `NET_StringToAdr` has already taken "localhost" and the bots. */
qboolean Sys_StringToAdr(const char *s, netadr_t *a, netadrtype_t family)
{
    (void)s;
    (void)family;
    Com_Memset(a, 0, sizeof *a);
    a->type = NA_BAD;
    return qfalse;
}

qboolean Sys_IsLANAddress(const netadr_t *adr)
{
    return adr->type == NA_LOOPBACK ? qtrue : qfalse;
}

void Sys_ShowIP(void)
{
    Com_Printf("No network: only the loopback\n");
}

qboolean NET_CompareBaseAdr(const netadr_t *a, const netadr_t *b)
{
    return a->type == b->type ? qtrue : qfalse;
}

qboolean NET_CompareAdr(const netadr_t *a, const netadr_t *b)
{
    return NET_CompareBaseAdr(a, b);
}

qboolean NET_IsLocalAddress(const netadr_t *adr)
{
    return adr->type == NA_LOOPBACK ? qtrue : qfalse;
}

const char *NET_AdrToString(const netadr_t *a)
{
    switch (a->type) {
    case NA_LOOPBACK:
        return "loopback";
    case NA_BOT:
        return "bot";
    default:
        return "unreachable";
    }
}

const char *NET_AdrToStringwPort(const netadr_t *a)
{
    return NET_AdrToString(a);
}

void NET_JoinMulticast6(void)
{
}

void NET_LeaveMulticast6(void)
{
}
