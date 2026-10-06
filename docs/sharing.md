<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Sharing files over the network, before it is built

Written on 5 October 2026, before any of it is built. Diego, the same day:
"Add to the roadmap the need for a network file sharing server and client",
"Perhaps SMB? Samba in Linux for example", and of the drawing, "it great how
it looks". It is the four things `CLAUDE.md` asks of anything with a window
(*An app is designed before it is written*; `design.md` 9.7):

- **what a person can do** with it, below;
- **the mockup**, `docs/sharing.html` - a share open in Tracker, Connect to
  Server, a server seen and not signed into, a share gone away, and File
  sharing in the Servers window - drawn and agreed on 5 October;
- **the architecture**, the rest of this page: every piece and what supplies
  it, what crosses in a region and what in a message, what is C and what is
  Lua, the busiest paths with where their time goes, how it is tested, and
  the order it is built in;
- **the diagram**, `docs/sharing-architecture.png`, drawn from
  `docs/sharing-architecture.html`.

**Diego's answers to the mockup's six questions**, 5 October: "yes to all".
So: a share appears at `/Network/<server>/<share>`; a remembered password
lives in a **keyring**, a piece of its own; **read-only first**, read-write
after; **guests off** on both sides; a **Modified** column for shares in
Tracker; and sharing is set up in the **Servers** window. What is still his
to decide is collected at the end.

---

## What it does

### In the first version: the client, read-only, by address

- **Connect to Server**, from Tracker's Network group or its menu: an
  address typed - `192.168.1.38`, `diego-mac` if the router's DNS knows it,
  `smb://192.168.1.38/Projects`, a port after a colon for a test peer - and
  the recent ones. **The server answers before the password is asked for**,
  so a wrong address is said at once; then a name and a password, and the
  share. A password once accepted with Remember ticked is kept in the
  keyring, and that server is signed into with nothing typed (K5).
- **A share is a folder**, at `/Network/diego-mac/Projects`: listed, sorted,
  opened, copied from, by Tracker, the Open window and every program, as
  `/Home` is - with **nothing new in any of them to read it**. `ls`, `cat`,
  `cp`, the video player and Music read a share because they read paths.
- **What a share shows that `/Home` does not**: the globe on the trail,
  "over the network" beside it, the status line naming the server, the
  dialect, whether it is signed and encrypted and how fast files are
  arriving; and the **Modified** column, because a share's server stamps real
  dates (`disk_node.dated`).
- **Read only, said as such.** A write, a delete, a rename or a new folder
  under `/Network` is refused with a sentence - "diego-mac's Projects is open
  read only" - rather than attempted.
- **Gone away.** A server that stops answering leaves its folder as it was
  last listed, greyed, with when it was last heard and when the next try
  is; nothing spins and no window stops. It comes back by itself.
- **SMB 2.0.2, 2.1, 3.0, 3.0.2 and 3.1.1**, signed when the server asks and
  sealed (encrypted) when it insists; sign-in by NTLMv2. **No SMB 1, no
  guests, no Kerberos.**

### Then, each its own step

- **Remembered passwords**, in the keyring - **built** as `docs/keyring.md`'s
  K1 to K5 (6 October), which N8 became.
- **Read-write**: a file written, deleted, renamed, a folder made, a copy
  into a share from Tracker (N10).
- **Servers seen on the network**, by mDNS: the Network group lists
  `diego-mac`, `nas-salon` and the rest without anything typed, and
  `diego-mac.local` resolves (N11).
- **The server side, read-only**: folders of `/Home` offered to the Mac's
  Finder and Windows from the Servers window - who may sign in, which
  folders and how, who is connected now (N12).
- **Announced**: this machine in the Mac's Finder under Locations, by mDNS
  (N13).
- **The server side, read-write** (N14).

### Not here, and why

- **SMB 1**, the dialect every current system has turned off; with it,
  NetBIOS browsing.
- **Guests**, on either side - Diego's answer, and current Windows refuses
  guest sign-in by default for the same reason.
- **Kerberos and Active Directory**: there is no domain here to sign in to.
- **Samba itself**, as a server. It is hundreds of thousands of lines on
  processes, `fork`, sockets and a Unix file system, which is a POSIX
  personality (`design.md` 17) and will not be one here.
- **Watching a share for changes** (SMB's CHANGE_NOTIFY): Tracker asks on its
  own clock, as it does of `/Home`; a share that changes under a window is
  seen at the next look.

---

## The architecture

### At a glance

Each piece and what supplies it. *Kept* is used as it is; *changed* gains
something; *new* is written for this - and, where something else could
want it, written as a kit or a server of its own from the start. Under each
new piece, the premise's two questions (`CLAUDE.md`, *Kits, servers and
drivers supply*): **does another application want this?** and **does
something already supply it?**

| Piece | What supplies it | |
|---|---|---|
| Tracker: the Network group, Connect to Server, the banner, the status line, Modified | `user/bin/apps/tracker.lua` | changed |
| The Open and Save window's sidebar | `user/lib/sidebar.lua`, used by `panel.lua` | changed |
| `/Network` holding files | `user/lib/places.lua` - `NOT_FILES` loses `/Network` | changed |
| File sharing in the Servers window | `user/bin/apps/servers.lua` - a fourth `SERVERS` row | changed (N12) |
| A share's files, to every program | the namespace (`user/init/init.lua`) - `disk_request`, as `/Home` | **kept** |
| `/Network` answered by two servers | the namespace's mount of `/Network` | changed |
| Connecting, a server's state, its shares | **`user/include/shareproto.h`** and `fs.share_*` in the namespace | new |
| The client: a share as a folder | **`smbfs`**, `user/servers/smbfs.c`, a role of `init.elf` | new |
| SMB 2 and 3 on the wire | **the SMB Kit**: libsmb2, vendored, and its port | new |
| MD4, MD5, HMAC-MD5, SHA-512, AES, AES-CMAC, AES-CCM, AES-GCM, SP 800-108 | the Crypto Kit (`user/kits/crypto/`), from BearSSL where BearSSL has it | changed |
| TCP to port 445 | the network stack (`user/servers/net.c`), `NET_OP_CONNECT`, `tcpring.h` | kept |
| A name to an address | `NET_OP_RESOLVE` | kept |
| Randomness | `SYS_ENTROPY`, the Crypto Kit's generator | kept |
| Remembered passwords | **the keyring**, `user/servers/keyring.c` | new (its own design) |
| Servers seen; `.local` names; this machine announced | **`mdnsd`**, and datagrams in the stack | new (N11, N13) |
| The server side | **`smbd`**, `user/servers/smbd.c`, on the SMB Kit | new (N12) |
| A copy of a large file into `/Home` | `cp --job` and `diskproto.h`'s write in pieces | changed (not sharing's) |

### A share is a disk: `smbfs` speaks `diskproto.h`

**This is the decision the rest stands on.** `/Home` is served by a C server
speaking a declared shape - `diskproto.h`: a directory's names a page at a
time, `getattr`'s facts, a read of bytes from an offset into the caller's
region or a page of the reply, and the writes. The namespace already speaks
it (`disk_request` in `init.lua`), and Tracker, the Open window, `files.lua`,
`regions.lua` and every program reach `/Home` through it without knowing it
is a disk. **An SMB client that speaks the same protocol is reached by all of
them with no new client code at all**: the namespace mounts it with
`proto = "disk"` and every `fs.list`, `fs.getattr`, `fs.read` and
`regions.read_file` already works.

It fits closely, and where it does not the difference is small and said:

| `diskproto.h` | What `smbfs` does for it |
|---|---|
| `LIST`: names, a page at a time from `offset` | SMB QUERY_DIRECTORY (`FileIdFullDirectoryInformation`, 64 KB a response), the names sorted once and paged from smbfs's cache |
| `GETATTR`: `disk_node` | **from the same listing**: QUERY_DIRECTORY carries each entry's size, kind and dates, so a `getattr` after a `list` is answered from smbfs's memory and never reaches the network |
| `disk_node.modified`, `dated` | the entry's LastWriteTime, FILETIME to seconds since 1970, `dated = 1` - which is what Tracker's Modified column reads |
| `READ`, `DISK_REGION`: bytes from `offset` into the caller's region | SMB READ, at most the server's `MaxReadSize` each (Samba and the Mac offer at least 1 MB), several in flight inside the server's credits, **straight from the TCP ring into the caller's region** |
| `READ` without a region: a page of 1 KB | the same, into the reply |
| `WRITE`, `DELETE`, `RENAME`, `MKDIR`, `SETATTR` | refused while read-only: `DISK_ERR_READ_ONLY` (new); SMB CREATE, WRITE, SET_INFO from N10 |
| `QUERY` | refused: `DISK_ERR_BAD_OP`. A share has no Kosmos attributes to query |
| `.super`, `.device`, `.format` | `.super` answers the share's size and free space (SMB QUERY_INFO, `FileFsFullSizeInformation`); `.device` what the share has cost - bytes, requests, round trips; `.format` refused |

**What `diskproto.h` gains, and only this:**

- three errors: `DISK_ERR_READ_ONLY`; `DISK_ERR_AWAY` - "diego-mac is not
  answering", the name in `u.data` as `DISK_ERR_NO_DISK` carries its why;
  and `DISK_ERR_DENIED` - the server refused this file to this account.
  **And a fourth, as built (N4)**: `DISK_ERR_ALTERED` - an answer changed
  on the way, its signature or its seal not holding - which is not a
  server gone away and is not said as one;
- **whether an answer is as it was last heard.** A reply gains a field
  saying the listing or the facts came from smbfs's memory of a server that
  is not answering, and the counter tick it last answered at. `/Home`
  always answers fresh and sets neither. That is the whole of what Tracker
  needs to draw *Gone away*. **As built (N3)**: `heard` and `heard_ms`
  after the reply's `u`, so every field before it is where it was - and
  *how long ago*, in milliseconds, rather than a tick, for `shareproto.h`'s
  reason: a number mailed to another process says which clock it is in its
  unit, and a tick of the counter crossing a boundary is the mistake
  `CLAUDE.md`'s *Two clocks* records twice. The namespace puts it on what
  it answers - `entries.last_heard_ms` beside a listing's names,
  `attrs.last_heard_ms` among a node's facts - so a caller that wants only
  the names reads them as before, and `ls` prints "(as last heard: the
  server has not answered for 3.3 s)" under them.

The namespace's words for those errors are the one place the namespace
changes for reading: `disk_error` says "/Home did not understand that" today,
and a mount's name goes into its sentences instead. **As built (N3)**: the
server says the sentence - `u.data`, as `DISK_ERR_NO_DISK` already carried
its why - and the namespace quotes any refusal that carries words, so
"MACPEER's Projects is open read only" and "MACPEER is not answering" are
smbfs's, which knows the names, and `/Home`'s numbers are put into words as
before. "No such file" and "that is a directory" keep kfs's numbers
(`DISK_ERR_KFS` + 7, + 9), with the words beside them. `.device` gains four
fields after its eight - SMB requests sent, folder listings, the requests
they took and their time - which `/Home` leaves at nought.

**Two things a share is not, and why they do not change the decision.**
SMB is a protocol of handles - CREATE, then READ, then CLOSE - and
`diskproto.h` is a protocol of paths. smbfs keeps **a handle per file it has
been asked to read, closed when unused for a few seconds** (and at once when
the server asks for its lease back), so a program reading a film a megabyte
at a time opens it once rather than a thousand times. And SMB names are
case-insensitive and UTF-16 on the wire; smbfs converts at its edge (libsmb2
does) and the namespace sees UTF-8, as it does everywhere.

### `/Network` is already somebody's

**Found reading the system, and not in the mockup.** `/Network` is the
network stack's mount today (`init.lua`: `ns.mount("/Network", net_cap, nil,
"net")`), named so on purpose - "a *card* is a device and the stack is
someone you ask" - and twenty-eight places in the tree say `"/Network"` to
reach it: `fs.connect("/Network", ...)` in `http.lua`, `netprogram.lua`,
`telnet`, `ping`, the Servers window. `places.lua` lists it among the three
names that are not files.

Diego's answer puts shares at `/Network/diego-mac/Projects`, and the two can
both be true, because **what the stack is asked and what a share is asked
never overlap**: the stack answers `netproto.h` - connect, listen, accept,
poll, ping, resolve, info, config, DHCP - always about `/Network` itself; a
share answers `diskproto.h` and `shareproto.h`, always about a path under it.
So:

- **the mount of `/Network` carries two capabilities**: the stack's, which
  `net_at` takes for every operation of the network kit as now, and
  smbfs's, which `request` takes for every file operation. About twenty
  lines in `init.lua`, and no caller of the stack changes;
- **`/Network` becomes a folder**: `places.NOT_FILES` loses it, and Tracker
  lists its servers. Its root, listed, is smbfs's answer: the servers
  connected, the recent ones, and - after N11 - the ones seen;
- `README.md`'s decision log and the comment beside the mount say why it is
  two, in the same session the mount changes.

**As built (N3)**: the namespace's `request` sends `list`, `getattr`,
`read`, `write`, `delete`, `rename`, `mkdir`, `setattr` and `query` under a
`net` mount that has a second server to that server, in `diskproto.h`
through `disk_request` exactly as `/Home`'s - and `ns.send`'s `mkdir`,
`delete` and `rename` the same way; everything the network kit asks still
goes to the stack. `places.NOT_FILES` is `/Devices` and `/Running`, so
Tracker's root and the Open window list `Network`, under System until N6's
Network group; opening it asks smbfs, which answers from its records.

**The alternative**, said so it is weighed rather than forgotten: move the
stack to a name of its own and give smbfs the whole of `/Network`. It is
cleaner to explain - one name, one server - and costs twenty-eight call sites
and a name for the stack that is not `/Network`, which is the name its own
comment argues for. The split is recommended; it is question 1 below.

### `smbfs`, the client

**A server in C** (`CLAUDE.md`, *Language split*: what runs on behalf of
another process), a role of `init.elf` started at boot as `diskfs` and
`drives` are - `ROLE_SMBFS` - and idle until somebody connects. **One
process for every share on every server**, for `drives.c`'s reason:
`/Network` is one folder every program has from the moment it starts, and a
share connected later could never be given a mount of its own in a namespace
that already exists.

It is handed, at boot: its own endpoint; the stack's capability, as a
client of it; the console's, so what it says reaches the log; and, from N8,
the keyring's `use` door. **Not `/Devices`**, which this said it would be
for the clock NTLMv2 puts in its answer and `counter_hz`: both are every
process's already - `time()` is the userland libc's (`clock_user.c`, the
board's epoch carried by the counter) and `counter_hz` and `tick_hz` are
`kosmos_sysinfo`'s - so a capability for them would be one held for nothing
(found building N2).

**Its loop, and the one new idea in it.** smbfs has two things to wait on:
callers on its endpoint, and bytes arriving on each server's TCP ring. The
kernel has no single wait for both, and none is proposed. Instead, **a waiter
thread per server connection** (`kosmos_thread_start`, `threads.md` step 3,
done) blocks in `NET_OP_POLL` on that one connection and, when something
happens, says so as a *call to smbfs's own endpoint* - a message like any
caller's. So:

- **every piece of state is touched by one thread**, the main one: libsmb2's
  context, the caches, the held replies, `malloc`. There is no futex yet
  (`threads.md` step 5) and `malloc` has no lock (step 7), and nothing here
  needs either;
- what the waiter waits for next - reading, or reading and room to write - is
  in the main thread's answer to its last call, so it never waits on an
  interest that has gone stale. **As built (N2)**: the answer says go on or
  stop, and *what* to wait for is a word in the server's record the main
  thread writes and the waiter reads with `__atomic` loads; the waiter's
  poll has a deadline of a second, so a change made while it was parked is
  read within one. The waiter's call carries a word smbfs drew from the
  kernel's entropy when it started (`SHARE_OP_WAITER`), and a call without
  it is refused like an operation that does not exist. **And a name is
  looked up by the waiter**, before it waits on anything: `NET_OP_RESOLVE`
  parks its caller, so the main thread never asks it - libsmb2 is handed
  four numbers. While the client only reads, what goes out is
  a hundred bytes a request and the out ring never fills; the case where it
  does - a WRITE larger than the ring - is held by its own test at N10;
- the main thread is `kosmos_receive` with a timeout - the next deadline: an
  ECHO due, a retry due, a held reply's bound - which is what that timeout
  exists for. Nothing sleeps for nought.

**Nothing on the desktop waits on it.** A caller's request goes out as SMB
and **smbfs holds the caller's reply** - its sender, as `net.c` holds
callers parked in `CONNECT` - and goes on answering others; the reply goes
when the server answers. Three bounds keep a window from waiting on a Mac
that has gone to sleep:

- **A question about names** - `LIST`, `GETATTR` - is answered from smbfs's
  memory when it has an answer younger than a couple of seconds, and
  otherwise held until the server answers *or a bound passes*, after which
  it is answered from memory marked as last heard, or `DISK_ERR_AWAY` if
  there is none. **The bound is a quarter of a second to start with**, and
  N6 measures Tracker with it; it is a number in one place.
  **As built (N3), the bound is on the server's silence**, not on the
  listing's length (`SILENT_MS`, 250, beside `FRESH_MS`, 2,000): a caller
  is held while bytes keep arriving from the server, and answered from
  memory once a quarter of a second passes with nothing at all. A folder
  of 2,000 names is seven requests and, under emulation, a third of a
  second on ARM and two seconds on x86-64 of a server answering the whole
  time - which a bound on the whole would have called
  a server gone away. A server gone to sleep still stops nobody for longer
  than the bound: whoever asks after the first is answered at once, from
  memory, while the question to the server is still out. A page after the
  first of one listing is answered from the same memory whatever its age,
  so a listing never changes under the pages already read.
- **Bytes** - `READ` - wait for the server, because a program reading a file
  wants the file; SMB's own timeout ends the wait, and the reader is a
  program on its own clock, never Tracker's loop. **As built (N3)**: thirty
  seconds (`SMB_TIMEOUT_SECONDS`), armed once the share is connected, the
  loop servicing libsmb2 once a second while anything is in flight - which
  is how libsmb2's timeout runs; a held listing was seen ending at it, with
  the bound set long as a control. **As built (N5)**: a server that says
  nothing at all for ten seconds with something asked of it
  (`AWAY_SECONDS`, the bound a connect has) is away, and a READ held for
  it ends then, in words - "MACPEER is not answering" - rather than at the
  thirty seconds, which are left for the one request a server otherwise
  answering never answers. Silence, as the bound on names is: a megabyte
  arriving slowly is a server answering.
- **Connecting** - `shareproto.h`'s `PROBE` and `CONNECT` - is answered at
  once, as `NET_CONNECT_AT_ONCE` is, and the window asks `STATUS` on its own
  clock to draw "192.168.1.38 answered - diego-mac, SMB 3.1.1, signing
  required" when it is so.

**What it keeps in memory**: per server, its connection, dialect, signing
and sealing, its name (from NTLM's target information, so a server typed as
`192.168.1.38` is listed as `diego-mac`), when it last answered, and what is
owed it; per share, its tree; per folder looked at, its sorted names and each
name's facts, with when they were heard; per file being read, its handle. A
fixed ceiling on none of it: tables that grow, as `net.c`'s do (`CLAUDE.md`,
*the pools grow*), from the machine's memory.

**A server's name in `/Network` is how it calls itself**, which is what the
mockup draws. Two servers that call themselves the same are told apart by
their address after the name.

**As built (N3)**: `/Network` lists every server whose share was connected
and has not been let go - an away one too, so its folder stays as last
heard - and `/Network/<server>` its share; both from smbfs's records, never
from the network. A server is found by its name or its address, and every
name under it whatever its case, as SMB's are. **One share a server, at
N3**: a record was a connection and its one tree; `SHARE_OP_SHARES` and a
second share on the same connection arrived with the windows that choose
one (N6, *N6, as built*), and `/Network/MACPEER` lists every share
connected on its session. A folder's listing is kept - its names sorted once, each with its
size, kind and the server's date - until the server is let go; a file's
handle is opened on its first read, read through by everyone reading it,
and closed five seconds after the last (`HANDLE_IDLE_SECONDS`). A READ
asks for at most a megabyte at a time, two in flight, each straight into
the caller's region at its place; a short answer has its remainder asked
for again, there.

### `shareproto.h`: what is not a file

A few things a share is asked are not file operations, and they are a
declared shape of their own on smbfs's endpoint - **the same request
struct**, `struct disk_request`, so smbfs checks one size, with operation
numbers above `diskproto.h`'s and their own structs in `u.data`:

| Operation | What it carries and answers |
|---|---|
| `SHARE_OP_PROBE` | an address and a port; answered at once. The server is asked to NEGOTIATE and nothing more |
| `SHARE_OP_CONNECT` | an address, a share, a name and a password - or none, for the keyring's - and `SHARE_REMEMBER` in `flags`; answered at once. SESSION_SETUP and TREE_CONNECT follow |
| `SHARE_OP_REMEMBERED` | an address: the account the keyring keeps for it, or nothing (K5) |
| `SHARE_OP_STATUS` | per server: answering or since when, the dialect, signed, sealed, the name it gave, its shares, bytes arriving a second, the next try |
| `SHARE_OP_SHARES` | a server's shares, through `srvsvc`'s NetShareEnum (libsmb2's `smb2-share-enum.c`), for "choose one once it answers" - **built at N6** (below) |
| `SHARE_OP_DISCONNECT` | a server, or a share of it |

The password crosses once, in one message, as a keystroke does - a one-shot,
which the rule about streams allows. **smbfs keeps the NT hash** (MD4 of the
password, which is what NTLMv2 needs and all it needs), never the password,
and only while connected, so a server that comes back after sleep is signed
into again without asking. **From K5** a CONNECT may carry no password,
and smbfs takes the keyring's for that address and account; with Remember,
it holds the typed one until the server takes it and then puts it in the
keyring (`docs/keyring.md`). **As built (N2)**: smbfs makes the
hash with `crypto_md4` and hands libsmb2 the form it takes one in,
`ntlm:` and 32 hex digits, so libsmb2 never sees the password either; the
request it came in is zeroed once read. The `share` program reads it at the
prompt, **shown as it is typed** - the console has no way yet to read a
line without echoing it - and Connect to Server's field (N6) and the
keyring (N8) are where it stops being typed there.

**And N5's** (`testing.md` 18.411): `SHARE_OP_RETRY`, the mockup's Try
now, answered at once as CONNECT is; and in STATUS's record, in the 336
bytes it already was, `trying`, `sign_ins` - 2 and more is signed in again
- and `next_try_ms`, when an away server is tried next, with `in_state_ms`
saying since when it has been away. DISCONNECT forgets a server that is
away as it does one that is connected.

**What N2 built of it** (`user/include/shareproto.h`): PROBE, CONNECT and
DISCONNECT carry `struct share_ask` - an address with its port, a share, an
account and the password; STATUS answers `struct share_server` three to a
page - the address, the name it gave, the share and account, its state
(asking, answered, connected, refused, away), the dialect, signed, sealed,
**why in words** when refused or away, and how long it has been so, in
milliseconds, so no clock's ticks cross. Refusals of a request are
`SHARE_ERR_*`, above `diskproto.h`'s numbers, each with its sentence. **A
PROBE answers the dialect and whether signing is required, but not the
server's name**: the name is in NTLM's challenge, which only a sign-in's
first leg brings, and a probe that began a sign-in to learn a name would be
an authentication attempt nobody asked for. CONNECT has it - libsmb2 keeps
the challenge's target name as the session's domain when it was given
none, and smbfs gives none. **The bound** for a server to answer -
connect, negotiate, sign in, connect the share - is ten seconds
(`ANSWER_SECONDS`), one number; past it the server is away, "took the
connection and did not answer within 10 seconds" if it took the connection
at all. Records are kept until DISCONNECT, so STATUS can say what became of
one that failed.

The namespace gains `fs.share_probe`, `fs.share_connect`, `fs.share_status`,
`fs.share_list` and `fs.share_disconnect` - each a `string.pack` and an
unpack, as `disk_call` is - and a `share` program at the prompt uses them
before any window does.

### The SMB Kit: libsmb2, vendored

**SMB 2 and 3 is not written here from the specification.** The candidate
the roadmap named is vendored, as BearSSL was for TLS, and it was read for
this page: Ronnie Sahlberg's **libsmb2**, `github.com/sahlberg/libsmb2`.
Downloaded on 5 October into this session's scratch folder, nothing in the
tree: the tag `v6.0.0` (commit `b944462`, 11 December 2024; 256,687 bytes,
SHA-256 `c65a7a2b...`) and `master` at `51c5910` (3 October 2026; 465,992
bytes, SHA-256 `ab5398da...`), both from `github.com/sahlberg/libsmb2/
archive/`.

**What it is, read rather than remembered:**

- **Licence**: the library - `lib/` and `include/` - is **LGPL 2.1 or
  later**; the examples are BSD-2-Clause; `libdcerpc/`, a separate library
  in the same repository on `master`, is BSD-2-Clause. It is linked statically
  into `init.elf`, as everything here is; its notice is kept where it sits
  and `LICENSE` lists it, which is all the bookkeeping `CLAUDE.md` asks.
- **Size**: `master`'s `lib/` is **34,566 lines** in 53 C files (26,754
  without blank lines and comments), and 5,531 of headers. Of those, 4,853
  are its own cryptography, 1,231 Kerberos, 1,205 the synchronous API and
  1,615 compatibility shims for twenty platforms. What a Kosmos client
  keeps is about **25,700**: the protocol (`libsmb2.c` 5,442, `pdu.c`, the
  commands 9,496), framing (`socket.c` 1,681), NTLMSSP in SPNEGO in ASN.1
  (3,534), signing, sealing, Unicode, and share enumeration (988). The
  repository's `libdcerpc/` (26,911 lines) is not needed.
- **What it needs from the platform**: `malloc` and `free` (217 calls - a
  PDU is allocated and freed per request); `socket`, `connect`, `setsockopt`
  and `fcntl` to make a non-blocking TCP connection; `getaddrinfo`;
  `readv` and `writev`; `poll` only in its synchronous API; `time()` for
  NTLMv2's timestamp; and randomness. **No threads**: one context is one
  connection driven from one loop.
- **Its async model**: the caller owns the loop. `smb2_get_fd` and
  `smb2_which_events` say what to wait for - `POLLIN`, and `POLLOUT` while
  it has bytes queued; the caller waits however it likes and calls
  `smb2_service(smb2, revents)`, which reads and writes what it can without
  blocking and runs the completion callbacks. Every operation has an
  `_async` form taking a callback; the synchronous ones are a `poll` loop
  around them. **This is exactly the shape smbfs's loop wants**: the waiter
  thread's message is `revents`.
- **Dialects**: 2.0.2, 2.1, 3.0, 3.0.2 and 3.1.1, any or one pinned
  (`vers=`). **Signing**: HMAC-SHA256 for 2.x, AES-CMAC for 3.x - not 3.1.1's
  AES-GMAC. **Encryption**: SMB 3's transform with **AES-128-CCM only** - not
  GCM, not AES-256. **Checked at N4**: its 3.1.1 NEGOTIATE offers one
  cipher, `SMB2_ENCRYPTION_AES_128_CCM` (`smb2-cmd-negotiate.c`), and its
  transform writes CCM's number whatever was agreed (`smb3-seal.c`); no
  signing-algorithm context is sent, so 3.1.1 signs with CMAC too. Preauthentication integrity with SHA-512 for 3.1.1.
  Credits, compounding, `STATUS_PENDING` interim answers, and **zero-copy
  reads**: `smb2_pread_async` reads a READ's data from the socket straight
  into the caller's buffer.
- **Its own cryptography**, all in `lib/`: AES (tiny-AES, table-driven -
  not constant-time - plus an Apple CommonCrypto path on `master`), AES-128-CCM,
  MD4, MD5, HMAC-MD5, HMAC, SHA-1, SHA-224/256 and SHA-384/512 (RFC 6234's
  code), and AES-CMAC inside `smb2-signing.c`.
- **Randomness**: `master` has one door, `smb2_random_bytes`, for the
  NTLMv2 client challenge, 3.1.1's salt and the CCM nonce, using
  `arc4random_buf`, `getrandom` or `/dev/urandom` where the platform has one
  and **falling back to `random()` seeded with `time() ^ getpid()`** where it
  does not. `v6.0.0` has only the fallback. That is the door Kosmos patches.
- **A server side - it has one, which this page was told it did not.**
  Already in `v6.0.0`, the library parses and builds the server's half of every
  PDU and runs NEGOTIATE and an NTLMSSP SESSION_SETUP with a callback that
  authorises a user; the application gives it a table of handlers -
  `tree_connect_cmd`, `create_cmd`, `read_cmd`, `query_directory_cmd` and the
  rest - and answers each. `examples/smb2-server-sync.c` (497 lines)
  simulates a disk of a few files. **It is a framework, not a file server**:
  shares, files, handles, locks, directory enumeration, the Mac's AAPL
  extensions - all of it is the application's, and its own README calls the
  serving loop synchronous ("You could run an async server if you implement
  the main loop yourself"). About 1,300 lines of `libsmb2.c` on `master`,
  younger and less used than the client.
- **Who else stands on it**: VLC's SMB 2 and 3 access module, AMSMB2 under
  iOS file managers, and ports to the PlayStations, Nintendo's consoles, the
  Amiga, the ESP32 and the Pico W - which is why its platform layer is
  already a set of hooks rather than POSIX all the way down.

**Which revision**: `master` at `51c5910`, pinned by commit as the Haiku
icons and `minih264e` were, not `v6.0.0`: twenty-two months newer, with the
one randomness door, CANCEL, and `libdcerpc` moved out of the library.

**What the Kosmos port is - and it patches nothing.** This said the port
would be patches in `runtime/patches/libsmb2/`, the NetSurf arrangement.
**Built (N2), none was needed**: every file of libsmb2 includes a
`config.h` first and guards each system header with a `HAVE_`, so the
platform is a `config.h` of Kosmos's own and the headers it names, and
what leaves the build leaves it by name in the Makefile
(`LIBSMB2_LEFT_OUT`). `runtime/upstream/libsmb2/` is compiled as released,
with `-w`, and there is no `runtime/patches/libsmb2/`:

1. **A platform, `__KOSMOS__`**, in `user/kits/smb/port/config.h` and
   `smb_port.h` - not in `compat.h`, which would be an edit: its generic
   path (`t_socket` an `int`) is all Kosmos needs of it. `t_socket` is an
   index into the kit's table of connections, which grows; `socket`,
   `connect`, `readv`, `writev`, `getsockopt`, `getaddrinfo` and the rest
   are **macros onto `smb_kosmos_*`** in `smb_transport.c`: `connect` is
   `NET_OP_CONNECT` with `NET_CONNECT_AT_ONCE` and the region it hands back;
   `writev` copies the iovecs into the `out` ring and says `NET_OP_PUSH`;
   `readv` copies from the `in` ring - **into the caller's buffer, for a
   READ's data**, since that is what libsmb2's zero-copy path hands its
   socket; `getsockopt(SO_ERROR)` is "refused" when the ring closed before
   the far end acknowledged a byte; `getaddrinfo` takes four numbers and a
   port, and a name is looked up before libsmb2 sees it (by smbfs's waiter,
   above). **This is compatibility inside a process** (`design.md` 17.2):
   those names exist in the SMB Kit's build and nowhere else - no program
   anywhere is given a `socket()`. `port/sys/select.h` gives `fd_set`, which
   `libsmb2.h` names in a structure of its server's: the ARM build had been
   taking newlib's from the toolchain and the x86-64 one found none.
2. **Randomness**: `HAVE_ARC4RANDOM_BUF`, and `arc4random_buf` is the
   userland libc's over `SYS_ENTROPY` (`misc_user.c`) - the kernel's
   entropy, every call, rather than the Crypto Kit's generator seeded from
   it as this said. The `random()` fallback is then not compiled at all.
3. **Time**: `time()` is the userland libc's - the board's epoch, read once
   a minute and carried by the counter - and not `/Devices/clock`.
4. **Cryptography is the Crypto Kit's** (`CLAUDE.md`, *Encryption is C, all
   of it*): libsmb2's `aes.c`, `aes_reference.c`, `aes128ccm.c`, `md4c.c`,
   `md5.c`, `hmac-md5.c`, `hmac.c` and the SHA files leave the build, and
   `user/kits/smb/smb_crypto.c` gives the names libsmb2's other files call
   to the kit and BearSSL: one AES block is `crypto_aes_ctrcbc()`'s
   (AES-NI or constant-time `aes_ct64`), CCM is BearSSL's over it, MD4 is
   `crypto_md4`, HMAC-MD5, HMAC-SHA256 and SHA-512 are BearSSL's - each
   kept inside libsmb2's own context structures, which the compiler checks
   it fits. **And signing, since N4**: libsmb2's `smb2-signing.c` leaves the
   build too. Its AES-CMAC keyed AES again for every sixteen bytes and
   copied every message whole before signing it - 34.6 ms a megabyte on
   an ARM core where the kit's `crypto_aes_cmac`, keyed once, takes 14.3,
   and 4.6 against 0.8 with AES-NI (`testing.md` 18.409) - so
   `user/kits/smb/smb_signing.c` supplies every name that file defined
   (`smb3_aes_cmac_128`, `smb2_calc_signature`, `smb2_pdu_add_signature`,
   `smb2_pdu_check_signature`): CMAC the kit's, over the message's vectors
   where they lie, and HMAC-SHA256 BearSSL's. Where an answer's signature is
   *checked* is still libsmb2's (`socket.c`, `libsmb2.c`); this decides only
   how one is computed. `tools/test_smbsign.c` holds it to libsmb2's own
   file, compiled beside it as the reference.
5. **Out of the build**: its signing (`smb2-signing.c`, item 4, N4); Kerberos (`krb5-wrapper.c`), the synchronous API
   (`sync.c`, a `poll` loop - the two of its functions `libsmb2.c`'s own
   synchronous helper names are refusals in `smb_transport.c`),
   `compat.c` (other platforms'), and the `NTLM_USER_FILE` path:
   `getenv` is no environment at all for libsmb2, so the branch that reads
   credentials from a file is dead code the compiler drops. On the
   server's side `smb2_bind_and_listen`, `smb2_serve_port` and
   `smb2_accept_connection_async` are in `socket.c` and `libsmb2.c` with
   everything else, so they are compiled - against `bind`, `listen`,
   `accept`, `poll` and `select` that answer "not here"; nothing calls them.

**And one thing of libsmb2's found wrong** (N2): `smb2_connect_async` keeps
its caller's callback data as the context's `connect_data`, and
`smb2_destroy_context` frees whatever is there as the `struct connect_data`
its own `smb2_connect_share_async` puts there - so a caller of the raw
connect has its own data freed under it. smbfs's PROBE is such a caller,
and its record was listed empty; smbfs takes it back before a context goes
(`server_close`). Upstream's to fix.

The SMB Kit is `user/kits/smb/`: the platform, the transport over
`tcpring.h`, `smb_crypto.c`, and `smb_kit.h` for the process that speaks
SMB - and **no Lua door**. It is C linked into the processes that speak
SMB, smbfs and later smbd, and an application never reaches it: an
application reaches a share through the namespace. **It costs the image
184 KB** of code (the ARM `init.elf`'s text, 32,623,364 bytes to
32,811,556; libsmb2's objects 175 KB of it).

**Does another application want this?** Two servers do, smbfs and smbd, and
that is why the protocol is a kit rather than inside smbfs: smbd builds its
answers with the same coders the client reads them with. **Does something
already supply it?** Nothing in the tree speaks SMB. The TCP connection is
the stack's, the hashes and ciphers become the Crypto Kit's, and the
randomness is `SYS_ENTROPY`'s - which is the whole of what the port is.

**Against writing SMB 2 and 3 from MS-SMB2.** The specification is complete
and public, and a client of the commands Tracker needs - NEGOTIATE with
3.1.1's contexts, SESSION_SETUP with SPNEGO around NTLMSSP in ASN.1, TREE_CONNECT,
CREATE, QUERY_DIRECTORY, QUERY_INFO, READ, CLOSE, ECHO, signing, sealing,
the key derivations, credits and `STATUS_PENDING` - is perhaps eight to ten
thousand lines here. **What it would not have is the years against real
servers**: the Mac's server, Samba on a NAS and Windows each read the
specification a little differently, and every one of those differences is a
silent bug found only against that server - which is the class of bug this
project cannot find from the Mac without the machine in question. libsmb2
has met them. And this page found, reading it, that it already carries the
server side's coders, which a protocol written here would have to write
twice. **Vendored, then; written here only where the port is.** What is
given up is said: 25,700 lines that are not this project's, LGPL, and
libsmb2's choices - CCM and not GCM, CMAC and not GMAC - until a patch or
upstream changes them.

### What the Crypto Kit gains

Every primitive held to its own specification's vectors in
`tools/test_crypto.c`, as the kit's first ones are, **before smbfs uses any of
them** (N1):

| Primitive | For | From |
|---|---|---|
| MD4 (RFC 1320) | the NT hash, MD4 of the password in UTF-16 | new, about a hundred lines; BearSSL has none |
| MD5, HMAC-MD5 (RFC 1321, 2104) | NTLMv2's proof and its session key | BearSSL's `md5.c`, `hmac` |
| SHA-512 | 3.1.1's preauthentication hash | BearSSL's `sha2big.c` |
| AES-128, AES-256 | everything below | BearSSL's `aes_ct64` (constant-time), `aes_x86ni` on a processor with AES-NI |
| AES-CMAC (RFC 4493) | SMB 3.0 to 3.1.1's signing | new, about sixty lines over AES; BearSSL has none. **And in pieces since N4** (`crypto_cmac_init`, `_update`, `_final`), so an answer is signed where its vectors lie |
| AES-128-CCM | SMB 3's encryption, the one libsmb2 speaks | BearSSL's `ccm.c` |
| AES-128-GCM, AES-GMAC | 3.1.1's faster cipher and signing, when the kit offers them (later) | BearSSL's `gcm.c`, `ghash_pclmul` |
| SP 800-108 KDF, counter mode | SMB 3's signing and sealing keys | new, a loop over the kit's HMAC-SHA256 |

And two worked examples beyond the primitives: **NTLMv2** from MS-NLMP 4.2.4,
whose published example gives every intermediate value from the password to
the session key, and **3.1.1's key derivation** from Microsoft's published
example of a session's preauthentication hash and keys.

BearSSL is already in the tree for TLS (`runtime/upstream/bearssl/`, compiled
whole), so most of this is giving a primitive that is there a name in the
kit's header rather than writing it. The Lua door, `crypto_kosmos.c`, gains
nothing: no program in Lua needs an NT hash.

**Not vectorised yet, and measured first**: BearSSL has no AArch64 AES
instructions, so on ARM a signed or sealed byte costs `aes_ct64`'s bit-sliced
rounds. On the M700, an x86 processor with AES-NI, it should not matter.
Where N4 measures it costing, the kit gains ARMv8's AES instructions as C
with intrinsics - userland may use them; only the kernel may not.

**Measured at N4** (`testing.md` 18.409), a megabyte on this Mac's own
cores: AES-CMAC 14.3 ms on an ARM core's `aes_ct64` and 0.8 ms with
AES-NI; AES-128-CCM opening 15.0 and 0.8; HMAC-SHA256 2.1 and 3.1. **So
it costs, on ARM**: a gigabit wire brings a megabyte every 8.9 ms, and a
signed or sealed one takes 14 ms of an M4's core to check - a Pi 5's
slower - so signing and sealing, not the wire, would bound a share's speed
there, at about 70 MB/s on this Mac's core. A CBC-MAC is serial and
`aes_ct64` computes four blocks at once, so three quarters of its work is
thrown away on CMAC; ARMv8's AES instructions are the step, on the
roadmap and not taken in N4. On the M700 it does not matter, as guessed: 9%
of a core at the wire's speed.

### The keyring - a piece of its own

**Designed and built in `docs/keyring.md`**, which supersedes what this
section said on 5 October - two doors called `store` and `use`, the NT hash
kept rather than the password, ChaCha20-Poly1305, a file in `/Home`, and a
choice of where the key comes from. What was decided on 6 October and built
in K1 to K5:

- **a server in C**, `user/servers/keyring.c`, deciding by **door**: `smb`,
  held by smbfs alone, and `manage`, held by Passwords and the `keyring`
  program, which may show a password (`REVEAL`) and never put one;
- **the password itself is kept**, sealed - Diego: "i need to know the
  password at some point" - and smbfs works out the NT hash at each sign-in
  and keeps that only while connected;
- **AES-256-CCM** through the Crypto Kit's one door, the whole file sealed
  at `/Keyring/keyring`, **outside `/Home`**, behind a disk door no program
  holds; the key at `/Keyring/machine-key`, on the disk (decision 3);
- **Remember**, ticked in Connect to Server and `share connect --remember`:
  kept once the server takes the password, never before; a remembered
  server signs in with nothing typed, and connects when Kosmos starts.

### mDNS: servers seen, and this machine announced

**What the Network group's list of servers needs**, and Kosmos does not have:
multicast DNS (RFC 6762) and DNS service discovery (RFC 6763) - a question
for `_smb._tcp.local` to the group 224.0.0.251 on UDP port 5353, and the
answers: a name, a host, a port, an address. And for this machine to be in
the Mac's Finder, the other half: **answering** for `_smb._tcp` and
`_device-info._tcp` with its own name.

**Does another application want this?** Yes, several: the VNC viewer the
roadmap wants (`_rfb._tcp` lists the machines to watch), printing
(`_ipp._tcp`), speakers (`_raop._tcp`), the browser's `.local` names, and
the Servers window announcing Web, Command line and Screen as well as File
sharing. **So it is a server of its own**, `mdnsd` in C, with a declared
shape - browse a type, resolve a `.local` name, announce a service for as
long as the announcer lives - and smbfs is its first user, not its owner.

**Does something already supply it?** Half. The stack has a DNS resolver
(`net.c`, unicast, A records), so the DNS message's coding - names,
compression, records - exists once already; it moves into a file both link,
as `fat_decode.c` and `drives_decode.c` are shared, rather than being
written a second time. What does not exist is **a datagram interface**: the
stack has "UDP as far as DNS and DHCP need it, and no further", and its own
comment says "When something else wants UDP, the interface it wants will be
visible". mDNS is that something, with NTP for the clock and a stream of
sound over the network behind it. So the stack gains datagrams - a port
bound, a ring of datagrams (`udpring.h` beside `tcpring.h`), a multicast
group joined with IGMP - and that is a design of its own too, at N11.

**Typed addresses work without any of it**, which is why the client comes
first: `smb://192.168.1.38/Projects` is TCP to port 445; `diego-mac` works
through the resolver when the router's DNS knows it; only `diego-mac.local`
needs mDNS.

### `smbd`, the server side, later

**A server in C** on the SMB Kit, started and stopped from the Servers window
like `httpd`, `telnetd` and `vncd` - a fourth row in its `SERVERS` table,
settings in `/Home/Preferences/servers` through the settings kit, its status
and its log under `/Temporary/smbd` for the window to read, as
`netprogram.lua` has the others do. Being C, it is a role of `init.elf`
started through the desktop's launch rather than a Lua program; the window
does not need to know the difference.

**On libsmb2's server framework**, with its loop replaced by Kosmos's - a
listener (`NET_OP_LISTEN`, `ACCEPT`) and a waiter thread per connection, as
smbfs has - and its handlers Kosmos's own: shares are folders of `/Home`,
and **smbd reaches them as a client of the namespace** - `diskproto.h` to
`diskfs` - so it serves exactly the folders it was given and nothing else
(`CLAUDE.md`, *what you were not handed, you cannot reach*). Sign-in is
NTLMv2 against the one account the Servers window sets, its NT hash kept in
the keyring; guests off; encryption on when the window says so. The Mac's
Finder asks things Samba's handlers know and libsmb2's example does not -
the AAPL create context, `FSCTL_VALIDATE_NEGOTIATE_INFO`, the information
classes Finder queries, `srvsvc` over `IPC$` to list shares - and each is
found by testing against the Mac's own client (below), one at a time.
**Whether the framework holds up is assessed at N12**; if it does not, its
coders stay and the serving is written here.

### What crosses in a region, and what in a message

**In regions:**

- **a file's bytes from a share**: the TCP ring (`tcpring.h`, the stack's
  and smbfs's) and the **caller's own region**, mapped by smbfs for the one
  request and handed back with the reply, as `diskfs`'s `map_region` does.
  libsmb2's zero-copy READ writes the data from the ring straight into it.
  **A file's bytes never become a Lua string**: a copy is
  `regions.read_file` into a region and `regions.write_file` out of it, and
  the bytes pass from the network to `/Home` without entering an
  interpreter;
- **the bytes of every SMB message**, both ways, in the TCP ring - a
  QUERY_DIRECTORY's 64 KB of names as much as a READ's megabyte;
- **the poll list** a waiter hands the stack (`struct net_poll_entry`);
- later, **a file written to a share** from the caller's region, and smbd's
  answers to the Mac from regions `diskfs` filled.

**In messages:**

- every `diskproto.h` and `shareproto.h` request and reply, each under 2 KB:
  a page of names, a node's facts, "read a megabyte from here into the region
  I hand you", "it is done";
- the stack's operations: `CONNECT`, `PUSH`, `POLL`, `RESOLVE`;
- **a password, once**, in `SHARE_OP_CONNECT` - a one-shot, as a keystroke
  is - and later a keyring entry's name instead; the keyring's answer to
  smbfs, an NT hash of sixteen bytes;
- the waiter's "something happened on connection 3", a few dozen bytes.

**Nothing recurs here because a clock says so** except the ECHO that keeps a
connection alive, which is a dozen bytes every minute. A copy is driven by its
reader, a region at a time.

### What is C and what is Lua

- **C**: smbfs, the SMB Kit and libsmb2, the Crypto Kit's additions, the
  keyring, and later `mdnsd`, the stack's datagrams and smbd. Each runs on
  behalf of another process or is a loop over bytes - a cipher, a hash, a
  parser of a binary protocol - which is the line `CLAUDE.md` draws.
- **Lua**: Tracker's Network group, Connect to Server, the banner, the status
  line and the Modified column; `sidebar.lua` for the Open window; `places.lua`;
  the Servers window's File sharing page; the namespace's mount and its
  `fs.share_*`; the `share` program. Each decides what is drawn or what is
  asked, and none touches a file's bytes.

### The busiest paths

Two paths decide whether a share feels like a folder: **copying a 1 GB film
from the Mac's share in Tracker**, and **listing a folder of 2,000 files**.
Numbers are the repository's own where it has them, and where it has none
that is said, with the step that measures it. QEMU's are not performance
numbers and are given only where they say what a suite will see.

**Copying a 1 GB film** from `/Network/diego-mac/Projects` into `/Home` on the
M700, the Mac on the same gigabit network:

| | Where | What is known |
|---|---|---|
| 1 | Tracker starts a copy job - `cp --job`, as zip and unzip run (`start_job`) - and draws its bar | a process start |
| 2 | `cp` asks smbfs, through the namespace, to `READ` the next piece into its region | an IPC round trip: **19.0 us** on the M700 (`testing.md`, the M700 suite) |
| 3 | smbfs splits the piece into SMB READs of the server's `MaxReadSize`, several in flight within its credits, each signed | CMAC: **0.8 ms a megabyte with AES-NI** (N4, measured through Rosetta on this Mac), 9% of a core at a gigabit; 14.3 ms on an ARM core's constant-time AES |
| 4 | the Mac sends; frames into the card's ring, the stack's TCP into smbfs's ring | **receive throughput on the M700 has never been measured** |
| 5 | the waiter wakes smbfs; libsmb2 copies the data from the ring into `cp`'s region, checks the signature | one copy, ring to region |
| 6 | `cp` writes the region into `/Home` | **19.5 MB/s** - the M700's `/Home` on its stick, written (`testing.md`, the M700 suite) |

**Where the time goes, honestly:**

- **The stack's receive window is the ring**: 16 KB (`TCP_RING_BYTES`, whose
  comment says the size was never measured against throughput), with no
  window scaling. TCP moves at most a window a round trip, and the M700's
  round trip to its router is 0.55 ms - so **at most about 30 MB/s**, and 1 GB
  in no less than 36 s, before anything else costs anything. A gigabit wire
  carries about 112 MB/s - the mockup's figure - which needs a window of at
  least 62 KB at that round trip, and a ring of 256 KB with window scaling
  (RFC 7323) to keep it full. **That is the stack's work, not sharing's**, and
  it is wanted by the browser and every download as much.
- **`/Home` on the M700's stick writes at 19.5 MB/s**, which bounds the copy
  at about 55 s whatever the network does. On the kernel's disk under QEMU it
  is 301 MB/s (`testing.md` 18.283), which is QEMU.
- **Writing 1 GB into `/Home` cannot be done today at all.** `diskproto.h`'s
  WRITE replaces a file whole from one region, so `files.copy` refuses
  anything over its 1 MB window (`COPY_MAX`), and a 1 GB region is not a
  thing to ask a process for. **`/Home` needs a write in pieces** - an offset
  on `DISK_OP_WRITE`, appending - and `cp` a `--job` mode that copies a
  region at a time and reports progress for Tracker's bar. Not sharing's
  either: the Camera's long recordings and any large download need it, and
  it is not on the roadmap yet.
- **Playing the film from the share**, rather than copying it, needs none of
  that: the video player reads at offsets, which works through `READ` as it
  stands, and at a film's few megabytes a second the window above is ample.
- **Under QEMU** the one figure there is: the browser fetched a 1.4 MB page
  from this Mac over plain HTTP in 0.6 s (`testing.md` 18.299), about
  2.3 MB/s through TCG and slirp. A 64 MB file is the suite's size; a
  gigabyte is not something to copy under emulation.

**Listing a folder of 2,000 files**, opened in Tracker:

| | Where | What is known |
|---|---|---|
| 1 | Tracker: `files.entries` - `fs.list`, then `fs.getattr` for each name | the same code that lists `/Home` |
| 2 | smbfs: QUERY_DIRECTORY, 64 KB a response - an entry is 80 bytes, its name in UTF-16 and padding to eight, so about 540 names of 20 characters a response: **four round trips** to the Mac, at 0.55 ms each, plus the Mac's own time | the Mac's time: unmeasured |
| 3 | smbfs sorts the names and keeps each one's facts | microseconds of C |
| 4 | the namespace pages the names: 1 KB a page, about 48 names of 20 characters - **42 round trips** | 19.0 us each on the M700 |
| 5 | **2,000 `getattr` round trips**, each answered from smbfs's memory | 19.0 us of IPC each on the M700, plus the namespace's packing; **206 us a request under QEMU** (`testing.md` 18.283) |
| 6 | Tracker sorts and draws 2,000 rows | unmeasured |

**About 2,050 round trips on this machine's side for four on the network's.**
On the M700 that is something like 40 to 80 ms; under QEMU about 0.4 s, which
is what the suite will see. **The network is not where a listing's time
goes** - the namespace's one-`getattr`-a-name is, and `/Home` pays exactly the
same today for a folder of 2,000. The fix is shared and is not sharing's: a
`LIST` that carries each name's `disk_node` beside it, so a page of names is a
page of facts. `diskfs`, `drives` and smbfs would each answer it, and
`files.entries` would ask it. Measured at N3 before it is proposed.

**Measured at N3** (`testing.md` 18.408), under TCG against Samba on this
Mac through slirp - and not under `-icount`, which the suite's network
does not run under; QEMU's numbers, for what a suite sees and for the
proportions:

| | ARM | x86-64 |
|---|---|---|
| SMB requests for the 2,000 (CREATE, QUERY_DIRECTORY until none are left, CLOSE) | 7 | 7 |
| smbfs asking them, from the first request to the last answer | 337 ms | 2,066 ms |
| `fs.list`, the 20 pages through the namespace included | 351 ms | 2,083 ms |
| 2,000 `getattr`s, every one from smbfs's memory | 528 ms | 409 ms |

**So the guess was half right.** The network took seven requests where four
were guessed - Samba fills a reply with fewer names than 64 KB would hold.
On ARM the listing and the 2,000 `getattr`s cost about the same: the round
trips on this machine's side are 60% of a folder's time, a quarter of a
millisecond each, with nothing of the network in them. A `LIST` carrying
each name's facts would take that part away for `/Home`, `/Drives` and
shares alike - question 6 below, now with its number. On x86-64 the network
is five times the `getattr`s, and that is the next finding.

**The bytes**: the 64 MB file read whole into one region in 5.4 s on ARM,
11.9 MB/s, 64 READs of a megabyte; its SHA-256, in C over the region, 0.84 s.
**On x86-64 the machine's network receives about 0.14 MB/s under QEMU** - 4
MB through smbfs in 27.8 s, and 2 MB through `fetch` over plain HTTP in
13.2 s against 1.2 s on ARM - so it is the stack's and the emulated card's
on that board, not SMB's, and the x86-64 suite reads the file's first 4 MB
whole rather than 64. **Found**, for the roadmap beside the stack's receive
window: x86-64's receive under QEMU, ten times slower than ARM's. **Its
cause found and fixed on 6 October** (`testing.md` 18.410): the card never
interrupted - virtio-net-pci was switched to MSI-X and no queue was given
a vector - so the stack read its frames on its own tenth-of-a-second
deadline. The same 2 MB now arrive in 93 ms, and both boards' suites read
the same.

### How it is tested, under QEMU first

**The far end is Samba's `smbd`, from Homebrew, run as Diego's user on a high
port, reached from the guest at 10.0.2.2 through QEMU's user network** - as
`run_tls.py` serves HTTPS on this Mac at 10.0.2.2 today.

Why Samba, over the alternatives:

- **The Mac's own File Sharing** is the real far end and the one Diego will
  use - and it is a system setting, his to turn on, so no suite depends on
  it. It is where N9 is checked by hand, once, with him.
- **Samba is what a NAS runs**, Synology's included, and it speaks every
  dialect this page wants - 2.0.2 to 3.1.1, signing, CCM and GCM - each
  pinned by one line of its configuration (`server min protocol`, `server
  max protocol`, `server signing = mandatory`, `smb encrypt = required`), so
  each suite says exactly which conversation it held. And it brings
  `smbclient`, which the server side needs later as a client that is not
  Kosmos.
- **impacket's `smbserver`**, in a scratch virtualenv, needs no Homebrew and
  runs as a user by construction. But its release, 0.13.1, answers **SMB
  2.0.2 only** (`smb2Negotiate` sets `SMB2_DIALECT_002` and nothing else);
  3.1.1 with encryption is on its `master`, unreleased. A peer that cannot
  sign with CMAC or seal tests none of SMB 3. It is the fallback if Samba
  will not run as a user.

**It runs as a user.** QEMU's own `-netdev user,smb=` starts `smbd` as the
invoking user with a configuration it writes into a temporary folder, which
is the evidence that this works. `tools/smbpeer.py` does the same: a
configuration in the scratch folder with its own private, lock, state, cache
and pid directories; `smb ports = 4450`; **`interfaces = 127.0.0.1` and
`bind interfaces only`**, so nothing on the Mac's network can reach it; a
passdb of one account; `disable netbios`; guests refused; and shares made
fresh by the tool - a folder of known files, a folder of 2,000, a 64 MB file
of known bytes. Slirp maps the guest's 10.0.2.2 to the Mac's loopback, so the
guest types `smb://10.0.2.2:4450/Projects` and nothing is forwarded.

**What needs Diego**: installing Homebrew's `samba` (4.25.0, with thirteen
dependencies - GnuTLS, MIT Kerberos, ICU among them), whose size is said
before it is fetched and checked with `brew deps --tree` against the disk's
free space - 12 GB free on 5 October. Nothing else on the Mac
changes: no system setting, no service registered, no root.

**For the server side later**, the other direction: QEMU forwards a port of
the Mac's loopback to the guest's 445 (`hostfwd=tcp:127.0.0.1:4451-:445`),
and two clients that are not Kosmos connect - Samba's `smbclient -p 4451`,
and **the Mac's own `mount_smbfs`**, which a user may run onto a folder of
their own with no setting changed. That is Apple's SMB client - the code
behind Finder - talking to Kosmos.

**And mDNS later**: QEMU's user network carries no multicast, so discovery is
tested on a `-netdev socket,mcast=` segment - two guests, one announcing and
one browsing, or one guest and a Python peer on the Mac that speaks the
segment's Ethernet frames.

**Within the budget** (`CLAUDE.md`, five to ten minutes): an `arm-share` and an
`x86-share` suite, split in halves that run side by side if they pass three
minutes, each waiting for the thing rather than for a number of seconds, and
the host checks in `host`. **As it stands since 6 October** (`testing.md`
18.410): two suites a board - `arm-share` (N2 and N4, one machine, ten
peers) and `arm-share-2` (N3 and N5), and the same for x86-64 - 118 s of
the gate's slots where six suites had taken 313; the reads in them sized to
what they prove, and the sizes the tables were measured at `--measure`. A change to the SMB Kit or smbfs runs those; a
change to `diskproto.h`, the namespace or the stack runs the whole gate.

### The order to build it in

Each step its own revision and its own test, and the suites green before the
next. The client, read-only, first.

- **N0 - the peer and the library, on the Mac.** `tools/smbpeer.py` starts
  Samba as the user and makes its shares; libsmb2 vendored at `51c5910` into
  `runtime/upstream/libsmb2/` as released, its licence beside it and in
  `LICENSE`; built for the Mac, its own `smb2-ls-async` and `smb2-cat-async`
  list the 2,000 and read the 64 MB file against the peer at 2.0.2, 3.0,
  3.1.1 signed and 3.1.1 sealed. **Proves the peer and the library before
  either is trusted inside the machine.** Host check, in `host`.
- **N1 - the Crypto Kit's additions.** MD4, MD5, HMAC-MD5, SHA-512, AES,
  AES-CMAC, AES-CCM and the SP 800-108 KDF, each to its specification's
  vectors; NTLMv2 to MS-NLMP 4.2.4's worked
  example; 3.1.1's keys to Microsoft's published example. `tools/test_crypto.c`,
  natively and through Rosetta, as now.
- **N2 - smbfs connects.** The port (`__KOSMOS__`, the ring transport,
  randomness, time, `smb_crypto.c`), `ROLE_SMBFS` started at boot,
  `shareproto.h`'s PROBE, CONNECT, STATUS and DISCONNECT, the waiter thread,
  and the `share` program. In the machine: `share connect
  smb://10.0.2.2:4450/Projects` with the peer's account answers with the
  dialect and the server's name; a wrong password is refused in words; an
  address with nobody on it is "not answering" within its bound. **Controls**:
  a peer pinned to SMB 1 only is refused, and a peer that is stopped (SIGSTOP)
  mid-negotiation never stops `share status` answering. **Built on 5
  October** (`testing.md` 18.407): `tools/run_share.py`, `arm-share` and
  `x86-share`, 14 checks in about 18 s each, a peer of each suite's own
  (`smbpeer.py --instance --port`, beside `host`'s on 4450) - and beyond
  what was asked, 3.1.1 *signed* and a peer *sealing*, so SMB 3's keys, the
  SHA-512 preauthentication hash, CMAC and CCM over the Crypto Kit are each
  held by a real server before N4. `/Network`'s mount carries smbfs's
  capability beside the stack's from this step, for `fs.share_*`; the
  folder under it is N3's.
- **N3 - a share is a folder.** `/Network` mounted twice-over; `LIST`,
  `GETATTR` from the listing, `READ` into a region and a page, `.super`,
  `.device`; read-only refusals; `places.lua`. In the machine: the 2,000 names
  equal the peer's; the 64 MB file's SHA-256 equals the Mac's, read whole and
  read at a hundred random offsets; dates are the peer's; `ls`, `cat`, `cp`
  from a share; Tracker opens it. **Measured**: the listing's round trips and
  time, split by stage, under `-icount`. **Built on 5 October**
  (`testing.md` 18.408): `run_share.py --part 2`, `arm-share-2` and
  `x86-share-2`, 19 checks each on a peer of their own (18 s and 49 s), the
  controls the peer stopped whole and a READ asking one byte further on;
  x86-64 reads the big file's first 4 MB, its network being slow under
  QEMU (above); measured under
  TCG rather than `-icount` (above); since 6 October 16 MB whole on both
  boards in the gate and 64 MB with `--measure` (`testing.md` 18.410).
  Departures, each said where it
  belongs: the bound is on silence; `heard_ms` rather than a tick; one
  share a server and `SHARE_OP_SHARES` left for N6; Tracker held to opening
  the share by the line it prints, its picture N6's.
- **N4 - signed and sealed.** Each dialect from 2.0.2 to 3.1.1 pinned in
  turn; `server signing = mandatory`; `smb encrypt = required` (CCM). **The
  control that bites**: a proxy on the Mac between the guest and the peer
  changes one byte of one READ's data, and smbfs refuses the answer rather
  than handing over the file - signed - and the same byte under sealing is
  refused by the cipher. The cost of signing and sealing a byte measured,
  natively on this Mac's cores. **Built on 5 October** (`testing.md`
  18.409): `run_share.py --part 3`, `arm-share-3` and `x86-share-3` (since
  6 October the first part's second half, in `arm-share` and `x86-share`,
  `testing.md` 18.410), on
  nine peers at once, each pinned (`smbpeer.py --instance`), every one
  reached through `tools/smbrelay.py` - a relay that watches what crosses,
  so `share status`'s dialect, signed and sealed are held to the wire's
  bytes rather than to an expectation. The relay changes the last byte of
  a READ's answer at 2.1 (HMAC-SHA256), 3.1.1 (AES-CMAC) and 3.1.1 sealed
  (CCM), each after letting the same read pass once. Refused, in words -
  `DISK_ERR_ALTERED`, "MACPEER's answer was changed on the way - its
  signature did not match - and was refused; nothing of it was handed
  over" - and the connection ended, said in `share status`. **Found**:
  libsmb2 reads a READ's data straight into the caller's region and checks
  the signature after the last byte, so a refused answer has already
  landed; smbfs now takes back what a failed read wrote, and the suite
  holds the region to zeros. The disabled check, in a scratch build, hands
  over the file with its last byte changed - the control bites. And
  libsmb2's `smb2-signing.c` replaced through the build, measured first
  (*The SMB Kit*, item 4). GCM: libsmb2 offers none.
- **N5 - gone away, and back.** The peer stopped (SIGSTOP) with a folder open:
  `fs.list` answered from memory within its bound, marked as last heard;
  a `READ` ends in words after SMB's timeout; the peer continued (SIGCONT)
  or restarted, and smbfs signs in again by itself, from the NT hash it
  kept, and the mark goes. **Built on 6 October** (`testing.md` 18.411), in
  `run_share.py --part 2` (now `arm-share-2` and `x86-share-2`, 33 checks):
  away after ten seconds of silence with something asked, or at once when
  the connection closes - a read in flight ended in words at 10.0 s, a
  folder from memory marked, a read meanwhile refused at once; tries at 2,
  4, 8 seconds and up to a minute; continued, signed in again by itself
  1.8 s later; restarted, `share retry` (Try now) signing in at once, a new
  session and tree; `share status` saying "since ..., next try in N s";
  `share disconnect` forgetting a server away, and nothing trying it again.
  The control: no try by itself in a scratch build, and the read after the
  return fails. Departures: the ten seconds rather than SMB's thirty (*Bytes*
  above), and an ECHO once a minute when nothing else is asked, so a server
  gone to sleep is noticed by itself. **And the gate's sharing suites
  rearranged first** (18.410): N2 and N4 one machine, N3 and N5 one, both
  boards reading alike once x86-64's card interrupted.
- **N6 - the windows.** Tracker's Network group, Connect to Server as drawn,
  the trail's globe, "over the network", the status line, the banner,
  Modified where `dated` says so; the Open window's sidebar through
  `sidebar.lua`. Pressed by name in the display harness, as `arm-writeapp`
  presses Write's; the gallery's screenshot gains a share open. **Tracker
  measured opening the 2,000** - the bound of N5 held to it. **Built on 6
  October** (`testing.md` 18.412), as *N6, as built* below says.

### N6, as built

**What the windows are made of.** Tracker's Network group, trail, status
line, Modified column, gone-away band and a server's page are Tracker's
(`tracker.lua`), drawn from one library that puts smbfs's answers together -
`user/lib/netshares.lua`, held on the Mac by `tools/test_netshares.lua`
(35 checks): which servers the group lists and how (live, amber, being
asked, locked), an address read as typed (`smb://host:port/share/within`,
which the `share` program reads with it too), the trail, the status line,
the band's words and what is remembered. **Connect to Server is a window of
its own**, `user/bin/apps/connect.lua`, opened by the group's *Connect...*
and Tracker's `...` menu; the Tracker that opened it goes to the share once
smbfs lists it. Dates in words are `clock.relative` ("Today, 11:20") and
`clock.day_word` ("yesterday") - one door, `test_clock.lua`.

**Asked on the window's own clock, never in a paint or a press.** Tracker
asks `fs.share_status`, `/Network` and each server's listing - every one
answered at once from smbfs's memory - once a second while any of the
network is in view and every five seconds otherwise, and redraws only what
changed. A press (Try now, Disconnect, Forget; Connect and a share chosen in
Connect to Server) paints what it began at once and leaves the sending to
the clock. **smbfs counts the STATUS requests it answers** - in STATUS's
`size` - and Tracker says that count against its paints every five
seconds, which is how the suite holds the two apart.

**Several shares on one connection, and the list of them.** A server record
in smbfs is one connection and one session holding a tree per share: a
CONNECT to a server signed into as the same account is one TREE_CONNECT,
no password; one with no share signs in to `IPC$` alone. `SHARE_OP_SHARES`
answers what the server offers (`struct share_offered`: name, asked or
connected or refused, its kind), from NetShareEnum on `IPC$` - folders
only, a printer, a pipe and a share ending in `$` left out - and `bytes` is
1 once the server's own list has been heard. A server signed into again
(N5) connects every tree again, since a tree's number is the connection's.
**Found building it**: libsmb2 stamps a request with the tree current when
it is made, and a listing, an open that follows a link and the share list
make their next request inside their own callbacks - so with two trees a
listing's next QUERY_DIRECTORY could go to the other share. smbfs runs those
as *chains*: the tree is selected before each request it makes and put
back after, and a chain on another tree of the same server waits for the
running ones to end (`chain_take`). One-shot requests - READ, CLOSE,
`.super`'s compound - only select and put back.

**Departures from the mockup, each for a reason:**

- The probe answers no server name (*`shareproto.h`*, above), so the
  answered line says "10.0.2.2:4466 answered - SMB 3.1.1, signing
  required" where the drawing names `diego-mac`; the name appears once
  signed in.
- "Seen, not signed in" is a server **remembered** - signed into before and
  not now, or one that refused - since nothing is *seen* until mDNS (N11);
  its page says what is known (address, port, guests not tried, why it last
  refused) with Sign in... and Forget, and not "Calls itself Synology
  DS220+", which only discovery tells.
- The band says since when and when the next try is, not "Tried 3 times":
  STATUS has no count of tries.
- The status line's rate is bytes a second the share has read
  (`.device`), not which file is arriving.
- No "Drives" heading over the drives: Tracker's sidebar had none, and the
  tests click its rows by position.
- Recent servers are kept in `/Home/Preferences/sharing` by the settings kit
  - an address, a share and an account, never a password.
- File sharing in the Servers window is drawn - its switches disabled, its
  folders none - saying "Sharing this machine's folders comes in a later
  step" (N12).
- **The prompt's password is still shown as typed.** An unechoed read would
  be a change to `conproto.h` and to the three servers that implement it -
  `console.c`, the Terminal and `telnetd`'s sessions - for one program whose
  password now has a window; Connect to Server is the door where it is not
  shown, and the keyring (N8) where it is not typed.
- **N7 - on real hardware, by hand.** The M700 against Diego's Mac with File
  Sharing on - his setting, turned on by him - the 2,000 listed and the film
  played from the share; the numbers above that say "not measured" measured,
  receive throughput first.
- **N8 - the keyring** (after `docs/keyring.md` and its windows are drawn
  and agreed): `store` and `use`, ChaCha20-Poly1305 as one AEAD to RFC
  8439's vectors, the file sealed, the unlock; "Remember in
  this machine's keyring"; smbfs signs in from it. Its own suite: a secret
  stored, the machine restarted, signed in without asking; `store` cannot
  read back; the file on disk holds no password in any encoding.
- **N9 - the 1 GB copy.** Needs the stack's ring sized by measurement and
  window scaling, `diskproto.h`'s write in pieces, and `cp --job` - each its
  own step under its own owner, on the roadmap. Then the film copied on the
  M700, and its time against the wire's and the stick's.

Then, each when its time comes:

- **N10 - read-write**: WRITE from a region at offsets, CREATE, delete, rename,
  a folder, dates set; the out ring filling held by its own test; Tracker's
  drag into a share. Held by reading back on the Mac what the guest wrote.
- **N11 - discovery**: the stack's datagrams (`udpring.h`, IGMP), the DNS
  message coding shared out of `net.c`, `mdnsd` browsing; the Network group's
  servers seen; `.local` names. On a multicast segment.
- **N12 - smbd, read-only**: the Servers window's File sharing page as drawn;
  `smbclient` and the Mac's `mount_smbfs` through a forwarded port list and
  read what `/Home` holds; a wrong password and a guest refused. **Needs the
  M700's TCP send** - today one segment in flight, 1.4 MB/s
  (`testing.md`, the M700 suite) - which is the stack's item already on the
  roadmap.
- **N13 - announced**: `_smb._tcp` and `_device-info._tcp`, and the M700 in
  Finder's Locations.
- **N14 - smbd read-write.**

---

## What is Diego's to decide

1. **`/Network` shared by two servers**: the stack keeps every network
   operation on `/Network`, and smbfs answers every file under it - one
   mount, two capabilities, no caller of the stack changed. Or the stack
   moves to a name of its own. *Recommended*: the split.
2. **libsmb2, vendored from `master` at a pinned commit** rather than SMB
   written from MS-SMB2 - 25,700 lines that are not ours, LGPL, its cryptography
   replaced by the Crypto Kit's. *Recommended*: yes.
3. **Samba from Homebrew as the test peer**, run as the user on port 4450,
   bound to the loopback; a Homebrew download, its size said first. *Recommended*:
   yes, with impacket as the fallback.
4. **The keyring's key**: a keyring password asked once a boot, or a key in a
   file that protects nothing from a program reading `/Home`. *Recommended*:
   a password - decided in `docs/keyring.md`, its own step.
5. **Two things outside sharing that its busiest path needs**, for the
   roadmap: `/Home`'s write in pieces with `cp --job`, and the stack's receive
   ring sized by measurement with window scaling. *Recommended*: both on the
   roadmap now, neither before N9.
6. **A `LIST` that carries each name's facts**, for `/Home`, `/Drives` and
   shares alike, if N3's measurement says the 2,000 `getattr` round trips are
   where a listing's time is. *Recommended*: measure first. **Measured
   (N3)**: under emulation they are a little over half of a 2,000-name
   folder's time on a share - 507 ms against 334 for the listing itself,
   network and all, on ARM. *Recommended now*: yes, as its own step, for all three.
