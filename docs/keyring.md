<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# The keyring and Passwords, before they are built

Written on 6 October 2026, before any of it is built. Network file sharing's
step N8 (`sharing.md`, *The keyring - a piece of its own*) grew into this the
same day, when Diego decided what the keyring is for:

> "key kept on the machine itself in a keyring", "actually we can do a
> keyring or password manager app that holds all keys and passwords that
> kosmos uses", "that would be better so i dont have to type the password
> all the time but at the same time i have a simple keyring app that allows
> me to delete passwords i no longer want kept", "that is what modern OSs
> like MacOS" - Diego, 6 October 2026

He showed macOS's Passwords: a list of items on the left - a title, a user
name - and on the right the one chosen, with its User Name, its Password as
dots, its Website, when it was Modified and its Notes; and a lock screen,
"Passwords is Locked", asking for the user's password.

So two things are decided before this page starts, and it is built on them:

- **No password is typed at boot.** The keyring's key lives on the machine,
  and the keyring opens with it. Diego chose convenience knowingly, and this
  page says exactly what that costs (*What it protects, and what it does
  not*).
- **A Passwords application** lists what Kosmos keeps, shows each entry, and
  deletes what Diego no longer wants kept.

It is the four things `CLAUDE.md` asks of anything with a window (*An app is
designed before it is written*):

- **what a person can do** with it, below;
- **the mockup**, `docs/keyring.html` - Passwords with an entry open, the
  delete confirmation, Connect to Server remembering and remembered, and the
  lock that a later step could add;
- **the architecture**, the rest of this page: the keyring server and its
  declared shape, who may ask it for what and how that is known, where the
  secrets and the key live, smbfs as its first user, what is C and what is
  Lua, and the order it is built in;
- **the diagram**, `docs/keyring-architecture.png`, drawn from
  `docs/keyring-architecture.html`.

Nothing here is agreed until Diego says so. His questions are at the end.

---

## What it does

### Passwords, the application

**Named Passwords**, recommended over Keyring: it is the word a person looks
for, and macOS's. *The keyring* is the server underneath, as *the
compositor* is under the windows. Question 1.

- **A list of what this machine remembers**, one row an entry: its title -
  `diego-mac (SMB)`, `Office NAS` - and under it the account, `diego`, and
  the kind's picture. The newest at the top until a sort is chosen.
- **Kinds down the side**: All, Shared folders, and - when each has a user -
  Wi-Fi, Websites, Mail. A kind with nothing in it is not listed. Shared
  folders is the only kind the first version has.
- **Search**, as typed, over the title, the account, the address and the
  notes; **sort** by title, by kind, by when it was last changed, by when it
  was last used.
- **An entry, on the right**:
  - its **account** and its **address** - `smb://192.168.1.38:445`;
  - its **secret as dots**, with a sentence saying what is kept: for a share,
    "Kept as the NT hash the sign-in uses; the password itself is not kept,
    so it cannot be shown";
  - its **kind**, **created**, **modified**;
  - **where it is used**: by whom - "smbfs, the SMB client" - when it was
    last used and how many times; and for a share, which shares are opened
    with it and whether they are **connected when Kosmos starts** (a switch);
  - **notes**, Diego's own words, editable;
  - **Sign in again...**, which opens Connect to Server on that address - the
    way a changed password is changed - and **Delete...**.
- **Edit**: the title and the notes, in place. A secret is never edited
  here; it is replaced by signing in again, where whoever checks it checks
  it.
- **Delete, with a confirmation** that says what follows: "This machine will
  forget the password for diego on smb://192.168.1.38. Projects and Music
  stay open until they are disconnected; the next sign-in will ask for the
  password. This cannot be undone." Delete... is also the right-click and
  the Delete key on a row.
- **The status line** says how many entries there are, that the file is
  sealed, and where its key is: "4 entries · sealed · key kept by this
  machine".

### What the rest of Kosmos does with it

- **Connect to Server's "Remember in this machine's keyring" becomes real**,
  on by default. Ticked, the password crosses once to smbfs, which keeps its
  NT hash in the keyring **only after the server has accepted it** - a
  mistyped password is never remembered.
- **A remembered server is signed into without a password.** Once the
  server answers, Connect to Server says "Remembered: signed in as diego"
  where the name and password fields were, with *Use another account...*
  beside it; pressing a remembered server in Tracker's Network group signs
  in without a window at all.
- **Connected when Kosmos starts**, for a remembered share whose switch is
  on: smbfs asks the keyring at start and connects them, and a Mac asleep is
  the Gone away band of `sharing.html`, coming back by itself.
- **A remembered password that stops working** - changed on the Mac - is
  said ("diego-mac did not accept the remembered password") and asked for
  again; the entry is replaced only when the new one is accepted, and is
  never deleted by a refusal.

### Later, each its own step

- **Wi-Fi**, when Kosmos has a Wi-Fi driver: the network's key, kept as the
  driver needs it.
- **The browser's sign-ins**, **mail**: each a kind and a door of its own,
  designed with its user.
- **Lock with a password** (K9, if Diego wants it): the keyring sealed under
  a password too, asked once a boot, and Passwords locked until it is typed,
  as macOS's is. Only then is **showing a password** offered.
- **The key off the stick** on a PC (K8): in the machine's firmware, so a
  stick that is lost holds nothing anybody can open.

### Not here, and why

- **Showing a password, in the first version.** Nothing protects Passwords
  but the machine being yours: with no lock, Show would hand every kept
  password to whoever sits down. And a share's password is not kept to be
  shown - only its NT hash is. Show arrives with the lock (K9), and for a
  share never.
- **Adding a password by hand.** A password typed into Passwords and never
  shown is useless; one that is shown needs the lock. Entries are made by
  what signs in with them.
- **Passkeys, one-time codes, a security audit**: macOS's, and nothing here
  uses them.
- **Syncing between machines.** Each machine's keyring is its own; the
  ThinkPad and the M700 sharing a stick have one each (*Where the key
  lives*).

---

## The architecture

### At a glance

Each piece and what supplies it. Under each new one, the premise's two
questions (`CLAUDE.md`, *Kits, servers and drivers supply*): **does another
application want this?** and **does something already supply it?**

| Piece | What supplies it | |
|---|---|---|
| Passwords: the list, the detail, delete | `user/bin/apps/passwords.lua`, on `ui.lua` | new |
| Remember, and remembered | Connect to Server, `connect.lua`; Tracker's Network group, `netshares.lua` | changed |
| Reaching the keyring from Lua | the namespace, `fs.keyring_*`, through `/Devices/keyring` | changed |
| Who is handed the keyring | the launcher in `init.lua`: `kosmos: needs keyring`, honoured only for `/Kosmos/Apps` | changed |
| Keeping secrets, answering for them | **the keyring**, `user/servers/keyring.c`, a role of `init.elf` | new |
| What crosses to it | **`user/include/keyproto.h`** | new |
| The file, sealed and opened | `user/servers/keyfile.c`, compiled on the Mac too | new |
| A place no program reaches | **diskfs**: a second door, rooted at `/Keyring`; the programs' door held to `/Home` | changed |
| Sealing | the Crypto Kit: **one AEAD door**, AES-CCM over BearSSL, which the SMB Kit's sealing then uses | changed |
| The key, made | `SYS_ENTROPY`, as the Crypto Kit's generator is seeded | kept |
| Dates on an entry | `time()`, every process's (`clock_user.c`) | kept |
| Who asked, for the record | `SYS_SENDER` - a label for a person, never a decision | kept |
| The first user | smbfs: `shareproto.h`'s CONNECT and PROBE gain a word each | changed |
| Sticks' raw sectors | the USB driver's read door refuses the boot stick's Kosmos partition (K7) | changed |
| The key off the stick | the loader, a UEFI variable, one word of the boot information (K8) | later, Diego's |

**Does another application want the keyring?** Yes - that is why it is a
server and not part of sharing: Wi-Fi, the browser and mail each will, and
each will be a kind and a door (*The doors*). **Does something already
supply it?** No: nothing in Kosmos holds a secret across a restart today.
Haiku has one, `keystore_server`, which asks the person the first time an
application wants a keyring - not read for this page, and worth reading
before K4.

**Does something already supply the sealing?** Yes, twice, and that is the
defect K1 removes: BearSSL's CCM is driven over `crypto_aes_ctrcbc()` in the
SMB Kit (`smb_crypto.c`'s `ccm_start`) and again in `tools/test_crypto.c`.
The keyring would be a third. So the Crypto Kit gains the one door - seal,
open - and the SMB Kit's `aes128ccm_encrypt`/`_decrypt` become two lines
calling it.

### Why a server, in C

**The point is that the secret is not in the caller's process.** A kit runs
in its caller; a server is someone you ask, and it answers only what the
door you asked through allows. It is C by the layer rule (`CLAUDE.md`,
*Language split*): smbfs's sign-in depends on it, and so does the boot. And
it is the shape C is for here - small, bounded, fixed structs - with the
sealing done by the Crypto Kit, never by Lua (*Encryption is C, all of
it*).

### Who is asking: what `SYS_SENDER` says, and why it is not enough

The keyring must tell **smbfs** from **a program somebody downloaded**, and
the first thing to read is what the kernel says of a caller. Read in
`kernel/syscall.c` (`sys_sender`), `kernel/process.c` (`process_describe`)
and `kernel/syscall.h` (`struct sender_info`), 6 October:

```c
struct sender_info {
    uint32_t id;          /* the process; ids are never reused */
    uint32_t parent;
    char     name[16];    /* what the process called itself */
    char     from[128];   /* the file it runs, said once; empty for an image */
};
```

- **`id`** is the kernel's and cannot be forged, but it is a number for this
  boot: nothing stored can be keyed by it.
- **`name`** is whatever the process said with `SYS_SETNAME`, printable and
  nothing else. A program can call itself `smbfs`. Worse, it does not need
  to: `sys.spawn(24, caps)` starts **the real smbfs code** - role 24 of
  `init.elf` - with whatever capabilities the parent hands it, and
  `sys.spawn_image` starts any image at all, which names itself.
- **`from`** is the file, said once by the runner before the program's first
  line and refused a second time - which stops a program renaming *itself*.
  But the runner says the path it was *asked* to run, in a namespace built
  from capabilities its *parent* handed it (`init.lua`, the runner role:
  `req.bin` becomes `/Kosmos/Apps`). A program that makes an endpoint, answers
  `binproto.h` on it, spawns a runner with that endpoint as `bin` and asks
  for `/Kosmos/Apps/passwords.lua`, gets a child whose `from` reads
  `/Kosmos/Apps/passwords.lua` and whose code is its own. **Read in the
  code, not tried**: K4's suite tries it, and this paragraph changes if it
  fails. (`notifyproto.h` says "a program cannot post as another" on the
  same grounds; if this holds, that sentence is weaker than it reads -
  for the roadmap, not for this page.)

So **neither name tells smbfs from a hostile program**, and nothing in
`sender_info` could without the kernel knowing what a file is - which it
must not.

**What does tell them apart is the capability**, which is the answer Kosmos
gives everywhere else: *what you were not handed, you cannot reach.* A
message arrives on an endpoint, and only the holders of that endpoint could
have sent it. So the keyring has **a door per kind of caller**, and **which
door a message came through decides what it may ask** - never what the
message or the sender says it is.

`SYS_SENDER` is still asked on every request, for **the record**: an entry
says "last used by smbfs (process 41), today 09:12", which a person reads
and nothing decides on.

### The doors

| Door | Held by | May |
|---|---|---|
| **`smb`** | smbfs, handed by init at boot, and nothing else | list, get, put and forget entries of kind SMB - and no other kind |
| **`manage`** | Passwords; and the launchers, to pass it on | list every entry without its secret; forget any; edit a title and notes; the keyring's state |
| **the disk door** | the keyring alone - it is diskfs's, rooted at `/Keyring` | read and write the keyring's two files |

**A program holds none of them.** It cannot read a secret, list what is kept
or delete it. Later kinds are later doors: `wifi` to the Wi-Fi driver,
`web` to the browser, each held by its one user and limited to its kind.

**`smb` cannot list other kinds and `manage` cannot read a secret.** The
door that reads secrets is held by the one process that signs in with them,
and the door that sees everything sees no secret. Passwords is a list and a
delete button; that is all its door allows, so that is all a bug in it, or
a Lua library it loads, can do.

**How Passwords is handed `manage`.** A door reaches a program only from the
process that launches it (`init.lua`'s launcher: `ep, console_cap, ...,
share_cap`, then the camera and MIDI when declared). The keyring's goes the
same way, and with one rule more:

- an application declares **`-- kosmos: needs keyring`**;
- the launcher **honours the word only for a file it reads from
  `/Kosmos/Apps`** - the image, which nothing at runtime writes - and hands
  nothing to the same word in a file in `/Home`.

The word alone would not do: any file can say anything in its header, which
is how the camera is granted today and what *per-launcher permissions* on the
roadmap is for. Where the file lives is decided in the launcher's own
namespace, whose `/Kosmos/Apps` is the real one; a hostile program cannot
grant a door it never held, so faking `from` in a child of its own gains
it nothing.

**And its cost, said**: the launchers hold `manage` in order to pass it -
init, the shell, the desktop, the Deskbar, the launch pad, Tracker - so
whatever runs *inside* one of those processes holds it too. A desklet from
`/Home` running in the Deskbar would. That is a reason for desklets to run
in processes of their own when they come, not a reason against the door.

**One process, several doors.** The kernel has no wait on two endpoints, and
none is proposed (`sharing.md` says the same of smbfs). The keyring does
what smbfs's waiters do: **a thread per door** receives on it and forwards
the request as a call to the keyring's own private endpoint, stamped with
which door it came through and a token only the keyring's threads know. One
thread holds every entry; the door threads hold nothing. It is three
threads for a server asked a few times a day, and the alternative - one
door and the caller's word - is the thing this page exists to avoid.

### `keyproto.h`, a declared shape

```c
#define KEY_OP_LIST     1u  /* the entry after `id`, of the kinds this door sees */
#define KEY_OP_GET      2u  /* smb: the secret kept for (service, account) */
#define KEY_OP_PUT      3u  /* smb: a secret for (service, account), new or replacing */
#define KEY_OP_FORGET   4u  /* by id: manage, any; smb, its own kind */
#define KEY_OP_EDIT     5u  /* manage: a title, notes, the at-start switch */
#define KEY_OP_STATE    6u  /* manage: how many, sealed, where the key is */

#define KEY_KIND_SMB    1u  /* an NT hash, 16 bytes; never a password */
#define KEY_KIND_WIFI   2u  /* later */
#define KEY_KIND_WEB    3u  /* later */
#define KEY_KIND_MAIL   4u  /* later */

#define KEY_AT_START    1u  /* smb: connect its shares when Kosmos starts */

#define KEY_SERVICE_MAX 128u    /* "smb://192.168.1.38:445", as smbfs spells it */
#define KEY_ACCOUNT_MAX  64u
#define KEY_TITLE_MAX    64u
#define KEY_NOTES_MAX   256u
#define KEY_SHARES_MAX  128u    /* smb: "Projects\0Music\0" */
#define KEY_SECRET_MAX  128u    /* a Wi-Fi key is 63, an NT hash 16 */

struct key_entry {              /* what LIST answers: never the secret */
    uint32_t id;                /* from 1, never reused */
    uint16_t kind, flags;
    uint64_t created_unix, modified_unix, used_unix;  /* time(): seconds */
    uint32_t uses;
    uint32_t used_by;           /* the process, as SYS_SENDER said */
    char     used_by_name[16];  /* and its name, for a person to read */
    char     service[KEY_SERVICE_MAX];
    char     account[KEY_ACCOUNT_MAX];
    char     title[KEY_TITLE_MAX];
    char     notes[KEY_NOTES_MAX];
    char     shares[KEY_SHARES_MAX];
};
```

The request carries an operation, an id, the entry's fields and - for PUT
only - the secret and its length; the reply carries an error, one entry, and
- for GET only - the secret. Both are under a message's 2048 bytes, held to
it by `_Static_assert` as every protocol header here is. **One entry a
reply**, walked by asking for the one after the last id - `notifyproto.h`'s
`NEXT`, and for the same reason: they are rare and a walk of fifty is fifty
round trips of microseconds.

**Every field crossing is a time in seconds from `time()`, named `_unix`**,
never a counter: dates on an entry are dates a person reads, and the keyring
has the libc's clock as every process does (*Two clocks*, `CLAUDE.md`).

**Errors are numbers**: `KEY_ERR_BAD_OP`, `KEY_ERR_NOT_THIS_DOOR` (a manage
operation on `smb`, or another kind), `KEY_ERR_NONE` (nothing kept for
that), `KEY_ERR_FULL`, `KEY_ERR_DISK` (the file could not be written, and
nothing changed), `KEY_ERR_SEALED` (the file did not open - *When the file
does not open*). The sentence is Passwords' to compose.

### What smbfs keeps: the NT hash, and what that means

**smbfs keeps the NT hash and never the password**, as it does today while
connected (`sharing.md`: MD4 of the password in UTF-16, made by
`crypto_md4`, handed to libsmb2 as `ntlm:` and 32 hex digits). The keyring
keeps the same sixteen bytes.

**Said plainly: the NT hash is password-equivalent for NTLM.** Whoever has
it can sign into that server as that account without knowing the password -
"pass the hash" - so the keyring guards it exactly as it would the password.
What keeping the hash buys is narrower and real: **the password itself is
nowhere on this machine.** A Mac's File Sharing password is the Mac
account's password, and Diego's may well be used elsewhere; the hash opens
SMB on that Mac and nothing else - unless the password is weak enough that
MD4, unsalted, is cracked offline, which is a reason for a good password
and not for keeping it.

**Going further** - the keyring computing NTLMv2's answer itself, so not even
the hash leaves it - would mean the keyring speaking a piece of NTLM and
libsmb2's session key arriving from outside it. smbfs is already a C server
whose memory no program can read; the step buys little and costs a protocol.
Not proposed; question 4 says so.

### Remember, and remembered: smbfs as the first user

**Connect to Server never holds a keyring door**, and does not need one:

1. **PROBE** answers as today, and smbfs asks the keyring (`smb`, LIST) for
   an entry whose service is the address it probed. **PROBE's answer gains
   the account remembered there**, if any - one field in `share_server`'s
   record, in the 336 bytes it already is.
2. **Remembered**: Connect to Server shows "Remembered: signed in as diego"
   and sends **CONNECT with an empty password**. smbfs GETs the hash and
   signs in.
3. **Not remembered**: the name and password, as now, with **"Remember in
   this machine's keyring"** ticked by default. CONNECT gains a flag,
   `SHARE_REMEMBER`. The password crosses once, smbfs makes the hash and
   zeroes the request as it does now, signs in - and **only when SESSION_SETUP
   succeeds** PUTs the hash, the account, the server's name as the title
   ("diego-mac (SMB)"), and the share. A refused sign-in remembers nothing.
4. **Refused with a remembered hash**: smbfs says so in STATUS's `why` -
   "did not accept the remembered password" - and Connect to Server asks for
   it; the old entry stays until a new one is accepted.

**An entry is bound to its address.** A CONNECT names a server, and smbfs
asks for the entry *of that server* - there is no "sign into X with Y's
entry". So a program that asks smbfs to connect somewhere cannot point a
remembered hash at a machine of its choosing.

**At start**, smbfs LISTs its kind and connects every share of an entry
whose `KEY_AT_START` is set, as a CONNECT from nobody: answered by STATUS
like any other. A Mac asleep is *Gone away*.

### What it protects, and what it does not

The honest part, as Diego asked: he chose to type nothing at boot, and this
is what that buys and what it does not, with Kosmos as it is today - no
per-file permissions, every program handed `/Home`, every program handed
`/Devices/blocks`.

| Who | What they get | Why |
|---|---|---|
| **A program on this machine**, any - downloaded, buggy, hostile | **No secret, no list, no delete.** It can make smbfs open a share that is remembered | It holds no keyring door, and diskfs's door it holds reaches only `/Home` (K3). But `/Network` is every program's: a remembered share is as open to it as a share already connected is today |
| **Passwords, or a bug in it** | Titles, accounts, addresses; deletes | `manage` has no operation that answers a secret |
| **smbfs, or a bug in it** | Every SMB hash | It is the process that signs in with them; it is C and nothing reads its memory |
| **Somebody at the keyboard** | Everything a remembered sign-in opens, and Passwords - but no password shown | No lock: that is the convenience chosen. K9 is the answer if it ever matters |
| **A copy of `/Home`** - a backup, `~/Kosmos/home` | Nothing | The keyring is not in `/Home` |
| **The stick, lost**, before K8 | **Everything** | The key is on the same stick as the file. Encryption with the key beside the lock protects nothing from whoever holds both |
| **The stick, lost**, after K8 | Nothing | The key is in the machine's firmware, not on the stick |
| **The QEMU disk image, on the Mac** | Everything | As the stick before K8: QEMU is a machine to test on |
| **Somebody who boots their own program on the machine** | Everything, after K8 too | A program in firmware can read a firmware variable; Secure Boot is not used |
| **A fault in Nebula, the keyring or the Crypto Kit** | Everything | It always would |

**So the encryption is the protection only where the key is not beside the
file** - on a PC after K8. Until then the protection is the **doors**: a
program cannot ask the keyring, cannot reach `/Keyring` through diskfs, and
(after K7) cannot read the stick's sectors underneath. The file is sealed
from the first version anyway, so that the format never changes and the
only thing K8 moves is the key.

**Why not derive the key from the machine** - the disk's GUID, the MAC
address, the SMBIOS serial? **Because none of them is a secret.** Anything
that can read the disk reads its GUID; anything on the network reads the MAC
address. A key computed from public facts is a key written down in a
longer way. The key is 32 random bytes from `SYS_ENTROPY`, and the
question is only where it lives.

### Where the secrets live

**Outside `/Home`**, in a folder of the volume of its own: **`/Keyring`**,
beside `/Home` at the root of the disk (`kfs_layout` gains it, as it holds
`/Home`):

- `/Keyring/keyring` - the entries, sealed;
- `/Keyring/machine-key` - the key, on a machine that keeps it on the disk
  (*Where the key lives*).

**Today a program could reach it.** The namespace's `/Home` is a prefix the
caller's own process adds: `ns.mount("/Home", req.disk, "/Home", "disk")`.
The capability under it is diskfs's endpoint, and a request on it may name
any path on the volume. Nothing is outside `/Home` yet, so nothing has been
reachable that should not be.

**So diskfs gains a second door (K3)**: the door every program holds answers
only paths under `/Home` and refuses any other with `DISK_ERR_NOT_FOUND`, as
though it were not there; and a **door rooted at `/Keyring`**, made by init
and handed to the keyring alone, answers only paths under `/Keyring`. diskfs
serves two endpoints the way smbfs and the keyring do - a thread on the
second forwarding with a token. The change is worth having on its own: a
door to `/Home` should reach `/Home`.

**The whole file is sealed, not each secret.** Titles and addresses say which
servers this machine signs into, and a file sealed whole says nothing - not
even how many entries there are, beyond its size. It is small (fifty entries
is about 40 KB) and rewritten whole on every change, which diskfs's write
already does - a file replaced whole under its journal, so a power cut
leaves the old keyring or the new one and never half of each.

```
"KOSKEYR1"     8 bytes   magic
version        4         1
key place      4         0 file, 1 firmware variable (K8), 2 + password (K9)
key id         16        which machine's key sealed this, in the clear (K8)
nonce          12        random, new at every write
sealed         n         the entries, each a key_entry and its secret
tag            16        CCM's
```

The header is the AEAD's associated data, so a changed version or key place
is refused as surely as a changed byte of an entry.

### How it is sealed: AES-256-CCM, through one door

**AES-256 in CCM, BearSSL's, through the Crypto Kit**, with a 12-byte nonce
drawn from the generator at every write and a 16-byte tag.

- **CCM rather than GCM**: the kit already drives CCM - smbfs seals SMB 3
  with it - and holds it to RFC 3610's vectors (`tools/test_crypto.c`). GCM
  is in BearSSL too and would be a second AEAD wired up for one user. Speed
  does not decide it: the file is opened once a boot.
- **Rather than ChaCha20-Poly1305**, which `sharing.md` wrote for this file
  on 5 October: the kit has both halves but has never joined them, and
  BearSSL's `br_chacha20poly1305_run` would be another construction for
  one user. **That line in `sharing.md` is superseded by this one** once
  agreed (question 6).
- **256-bit**, because nothing is saved by 128 and the key lives for years.

**The door, in `crypto.h`**:

```c
/* AES-CCM (NIST SP 800-38C): `bytes` of `data` sealed in place and the tag
 * written; opened in place, 0 when the tag holds and -1 - with `data`
 * zeroed - when it does not. A key of 16 or 32 bytes. */
void crypto_aes_ccm_seal(const void *key, size_t key_bytes,
                         const uint8_t *nonce, size_t nonce_bytes,
                         const void *aad, size_t aad_bytes,
                         void *data, size_t bytes,
                         uint8_t *tag, size_t tag_bytes);
int  crypto_aes_ccm_open(...same...);
```

Held to RFC 3610 and to Wycheproof's AES-CCM vectors for 256-bit keys
before anything uses it (K1); then `smb_crypto.c`'s two functions call it
and lose `ccm_start`.

### Where the key lives

**On a machine with nothing else - QEMU's `virt` board, booted with
`-kernel`, and every machine until K8**: `/Keyring/machine-key`, 32 random
bytes written by the keyring the first time it starts, behind the same door
as the file. **Programs cannot read it** (K3, and on a stick K7); **the disk
can**, and so can whoever has it.

**On a PC booted by Kosmos's loader - the M700, the ThinkPad, and x86 under
OVMF - after K8**: in **a UEFI variable** in the machine's own flash,
non-volatile and readable **only while the firmware's boot services run**:

1. The loader, which runs in boot services, reads `KosmosKeyring` - or,
   the first time, draws 32 bytes from the firmware's RNG protocol or
   RDRAND and makes it - and puts it in the boot information it already
   hands the kernel (`boot/efi/mbi.c`).
2. The kernel keeps it in one place and answers **`SYS_BOOT_SECRET` once**,
   to the first process that asks - init, before it has started anything -
   and zeroes its copy. After `ExitBootServices` nothing on the machine can
   read the variable again: Kosmos calls no runtime services, and the
   variable is not marked for them.
3. init hands the key to the keyring in its first message and keeps none.

**The kernel's part is carrying 32 bytes from the loader to init**, as it
carries the command line - it does not know what they are for. It is still a
syscall and a field in the boot information, which is Nebula and is
Diego's call (question 3).

**Each machine has its own key, so each has its own keyring on the same
stick.** The file's header names the key that sealed it (*key id*, a hash of
the key, not the key); the keyring keeps `/Keyring/<key id>`, so the ThinkPad
and the M700, booting the same stick, each open theirs and leave the other's
alone. A stick that moves to a third machine starts an empty keyring there.

**What K8 costs**: a stick's keyring cannot be opened on another machine, by
design; a firmware reset (clearing NVRAM, a new motherboard) loses every
remembered password, and Passwords says so rather than failing silently;
and the variable is a few dozen bytes of a flash chip that has room.

### When the file does not open

A tag that does not hold - a disk fault, a key that changed after a firmware
reset, a file from another machine - is **not** a keyring to overwrite. The
keyring keeps the file as `/Keyring/keyring.unopened-<date>`, starts empty,
and says so in the log and in Passwords' status line: "The keyring kept
before 6 October could not be opened with this machine's key; it is kept
aside, and nothing is lost by trying again later." Remembered sign-ins ask
again. **Nothing is ever deleted because it did not open.**

### Locking it with a password, later (K9)

What macOS's lock screen adds, and what it would here:

- **The key, sealed under a password**: PBKDF2-HMAC-SHA256 over the
  password and a salt in the header (the kit has HMAC-SHA256; a memory-hard
  function such as Argon2id would be the kit's to gain), and the file's key
  is opened by it.
- **Asked once a boot**, by whatever first needs a secret - a window from the
  keyring's door, not from the asker - and **Passwords locked** until then
  and after a while unused, as the mockup's last state draws it.
- **What it buys**: a lost stick or machine, and somebody at the keyboard,
  get nothing. **What it costs**: a password at boot, which is exactly what
  Diego decided against - so it is an option, off, and smbfs's sign-ins at
  start wait (a held reply, never a blocked server) until it is typed.
- **Show** a password, for kinds that keep one, once unlocked recently.

### The busiest path, briefly

**There is none worth the name.** The keyring is asked when a share is
signed into - one round trip, 19 us on the M700, beside an SMB sign-in of
several round trips on the wire - and when Passwords opens: an entry a
reply, fifty round trips, about a millisecond on the M700 and some tens
under QEMU. A change seals and writes 40 KB once: tens of microseconds of
CCM on AES-NI, under 2 ms in `aes_ct64` on ARM by `sharing.md`'s measurement,
and a diskfs write. Passwords asks on opening and after a change, never on
a clock.

### What crosses in a region, and what in a message

**Everything is a message**, one-shot and small: a request for one entry, a
secret of 16 to 128 bytes once at a sign-in. Nothing recurs because a clock
came round, so *control by message, data by shared memory* has nothing to
move. A secret is zeroed in the keyring's reply buffer once sent, and in
smbfs once used, as smbfs zeroes the password today.

### What is C and what is Lua

- **C**: the keyring, `keyfile.c`, diskfs's door, smbfs's part, the Crypto
  Kit's door, the loader and the kernel's 32 bytes (K8).
- **Lua**: Passwords, Connect to Server's switch, `fs.keyring_*` in the
  namespace (packing `keyproto.h` with `string.pack`, as `fs.share_*` pack
  `shareproto.h`), and the launcher's rule.

### What is new code, and the order to build it in

**Each step its own revision and its own permanent test**, in this order;
each is lived with before the next starts.

- **K1 - one AEAD door.** `crypto_aes_ccm_seal`/`_open` in the Crypto Kit,
  16- and 32-byte keys, held to RFC 3610 and Wycheproof's 256-bit vectors in
  `tools/test_crypto.c`; `smb_crypto.c`'s `aes128ccm_*` become its callers.
  *Test*: the vectors, natively and through Rosetta; the SMB suites unchanged.
- **K2 - the file, on the Mac.** `keyproto.h`; `user/servers/keyfile.c`, the
  format above, compiled on the Mac as `kfs.c` is. *Test*:
  `tools/test_keyfile.c` - two hundred entries round trip; one flipped byte
  anywhere, a changed header, a truncated file and the wrong key are each
  refused and zero what they opened; and **the file holds no secret in any
  encoding** - not the password in UTF-8 or UTF-16, not the NT hash in bytes
  or hex, not a title.
- **K3 - diskfs's second door.** `/Keyring` made beside `/Home`; the
  programs' door held to `/Home`; a door rooted at `/Keyring`, forwarded by a
  thread with a token. *Test*: a program asks for `/Keyring/x` through its
  door and is told it does not exist; the keyring's door cannot reach
  `/Home`; every `/Home` suite unchanged (*the whole gate*: diskfs is shared).
- **K4 - the keyring.** `ROLE_KEYRING`, started by init after diskfs and
  before smbfs; the `smb` and `manage` doors and their threads; the key made
  on first start; *When the file does not open*. *Test*, a suite of its own:
  put and get through `smb`; a manage operation on `smb` refused; `smb`
  blind to another kind; `manage` never answers a secret; QEMU restarted on
  the same disk and the entry there; the file replaced by garbage and kept
  aside, not overwritten; **and the forgery**: a program spawns a runner with
  a `bin` of its own and asks for `/Kosmos/Apps/passwords.lua` - and is not
  handed `manage`.
- **K5 - smbfs remembers.** CONNECT's `SHARE_REMEMBER` and empty password;
  PROBE's remembered account; PUT after a sign-in succeeds; `KEY_AT_START`;
  Connect to Server's switch real and its remembered state. *Test*: N8's
  suite as `sharing.md` wrote it - a share remembered, the machine
  restarted, signed in without asking; a wrong password not remembered; a
  refused remembered hash asked again and not deleted; against Samba on the
  Mac (`tools/smbpeer.py`).
- **K6 - Passwords.** `passwords.lua` as drawn; `needs keyring` honoured only
  from `/Kosmos/Apps`; `/Devices/keyring` and `fs.keyring_*`. *Test*: the
  display harness lists, searches, sorts, deletes through the confirmation
  and edits a note; a file in `/Home` that declares `needs keyring` is not
  handed it. It joins the dated screenshot (`tools/run_gallery.py`).
- **K7 - programs off the boot stick's sectors.** The USB driver's read door
  refuses the partition diskfs serves - the GUID it was given at boot - so
  `/Keyring` on a stick is no more readable underneath than through diskfs.
  *Test*: the stick suite reads every other partition and is refused that
  one.
- **K8 - the key off the stick** (if Diego says yes, question 3). The loader,
  the UEFI variable, `SYS_BOOT_SECRET`, a keyring a key. *Test*: under OVMF
  with its variables file - two "machines" (two variable files) booting one
  disk each open their own keyring and not the other's; the variable file
  reset, the keyring kept aside. Then the M700, by hand, as its stick.
- **K9 - lock with a password** (later, if wanted). As above.

Then Wi-Fi, the browser and mail, each a kind and a door, each designed with
its user.

---

## What is Diego's to decide

1. **The application's name**: Passwords, or Keyring. *Recommended*:
   **Passwords** - what a person looks for, and macOS's; the keyring is the
   server under it.
2. **Who may ask: doors, not names.** `SYS_SENDER`'s name and file do not
   tell smbfs from a program that wants to be it (*Who is asking*), so the
   keyring decides by **which door** a request arrived on: `smb` only to
   smbfs, `manage` only to Passwords - granted by `needs keyring` **only for
   a file in `/Kosmos/Apps`** - and to the launchers that pass it on.
   *Recommended*: yes, knowing that whatever runs inside a launcher's process
   holds `manage` too.
3. **Where the key lives.** On the disk, behind diskfs's door, from the
   first version - protecting nothing from whoever holds the stick; and
   **on a PC, a UEFI variable** read by the loader (K8), which takes 32
   bytes through Nebula - a field in the boot information and one syscall,
   answered once. *Recommended*: the disk now, K8 after K7, on the
   machines that boot through the loader. On QEMU's `virt` board, booted
   with `-kernel`, it stays on the disk.
4. **The NT hash, not the password**, for shares - password-equivalent for
   NTLM, but the password itself nowhere here. *Recommended*: the hash, as
   smbfs keeps it today; the keyring computing NTLMv2 itself is not worth a
   protocol.
5. **No Show in the first version.** A share's password is not kept to be
   shown; other kinds show only behind a lock, if K9 is ever built.
   *Recommended*: as written.
6. **AES-256-CCM**, through one AEAD door the SMB Kit then uses too, rather
   than the ChaCha20-Poly1305 `sharing.md` named. *Recommended*: CCM; and
   `sharing.md`'s keyring section, its N8 and question 4 rewritten to point
   here once this is agreed.
7. **A remembered share is open to every program**, as a connected one is
   today: the keyring keeps the password from programs, not the share.
   *Recommended*: accept it now; a per-program `/Network` belongs to
   *per-launcher permissions*, on the roadmap.
8. **Connected when Kosmos starts**, per remembered share. *Recommended*:
   **on** when Remember is ticked - the point is not typing - with the switch
   in Connect to Server and in the entry.
9. **K9, a lock**, at all? *Recommended*: on the roadmap as an option, not
   built until a stick is somewhere Diego would mind losing it.
