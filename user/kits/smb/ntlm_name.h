/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **What a server calls itself, out of NTLM's challenge** (`testing.md`
 * 18.415). Pure arithmetic over bytes, so `tools/test_ntlmname.c` holds it
 * on this Mac.
 *
 * The challenge (MS-NLMP 2.2.1.2) carries three names. **TargetName** is the
 * one libsmb2 keeps, as the domain: for a server in a workgroup it is the
 * server's NetBIOS name - Samba's peer says MACPEER - but it is a domain's
 * name where there is one, and macOS's own server, reached at 192.168.1.38,
 * said "192". **TargetInfo** holds the computer's own: MsvAvNbComputerName
 * (1), the NetBIOS name, and MsvAvDnsComputerName (3), whose first label is
 * what Finder shows - `Diegos-Mac-mini` of `Diegos-Mac-mini.local`.
 *
 * Chosen in that order - NetBIOS computer name, the DNS one's first label,
 * then TargetName - and **a name that is only digits and dots is no name**:
 * it is an address, or a piece of one, and "192" was exactly that. With
 * none left the caller names the server by its whole address.
 */
#ifndef KOSMOS_NTLM_NAME_H
#define KOSMOS_NTLM_NAME_H

#include <stddef.h>
#include <stdint.h>

#define NTLM_NAME_FOUND   1   /* `out` holds the name, in UTF-8 */
#define NTLM_NAME_NONE    0   /* a whole challenge, and no name worth having */
#define NTLM_NAME_SHORT (-1)  /* a challenge whose fields run past `len`: more */
#define NTLM_NAME_NOT   (-2)  /* not an NTLM challenge at all */

/*
 * `msg` points at "NTLMSSP\0" and `len` is how many bytes follow it,
 * counting those eight. `out` gets at most `room - 1` bytes and a NUL; a
 * name that does not fit is cut at a character, never inside one.
 */
int ntlm_challenge_name(const uint8_t *msg, size_t len, char *out, size_t room);

#endif /* KOSMOS_NTLM_NAME_H */
