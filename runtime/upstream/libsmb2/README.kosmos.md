# libsmb2, as vendored

`master` of `github.com/sahlberg/libsmb2` at commit
`51c5910da240a3b60bc4ad1b472185c2815628d4` (3 October 2026), **byte for
byte as released**: `https://github.com/sahlberg/libsmb2/archive/51c5910da240a3b60bc4ad1b472185c2815628d4.tar.gz`,
465,992 bytes, SHA-256
`ab5398da368940bf78efb3e1949668d4f05d727d3a044db77aaeec5c3fcef671`, fetched
on 5 October 2026. By Ronnie Sahlberg and its contributors, LGPL 2.1 or
later - `COPYING` and `LICENCE-LGPL-2.1.txt` beside this.

## Why this is here

Network file sharing, SMB 2 and 3, both ways (`docs/sharing.md`, agreed by
Diego on 5 October 2026): the client and, later, the server framework,
rather than SMB written again from Microsoft's specification. It has met
real servers' quirks, and the same code serves both directions.

## What is done with it

**Nothing in this directory is changed**, as with every vendored tree here:
what the port needs is a build step and a platform of Kosmos's own beside
the library. Today it is built only for this Mac (`make
build/host/libsmb2/smb2-ls-async`, with `tools/libsmb2_mac_config.h`), and
`tools/test_smbpeer.py` holds it to Samba run as the user at SMB 2.0.2, 3.0,
3.1.1 signed and 3.1.1 sealed (step N0). The machine's build - its socket
on the network stack's ring, its randomness from the kernel, its
cryptography the Crypto Kit's - is step N2.
