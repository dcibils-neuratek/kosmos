# `diskfs` in C, and speaking a declared shape

Written on 29 September 2026, before any of it is built, as `threads.md` was.
Diego the same day: "`diskfs` to a declared protocol and C" - yes, now -
and "push it and go on with diskfs".

## Why, in one paragraph

`CLAUDE.md` said it would move "when Disk Benchmark says that is where the
time is". It says so now. After the byte path went to C and the disk server
learned to keep its small reads (`testing.md` 18.270, 18.271), a random 4 KB
read of `/Home` under QEMU is 297 to 329 us, and a request that touches no
disk at all is 137: **about 45% is the round trip** - the namespace, the IPC
and a Lua table marshalled through `sys.pack` on both sides - **and 38% is the
path walked twice in Lua**, each directory's entries unpacked into tables each
time (18.273). The bytes are no longer the cost. What is left is the server
being a Lua process that speaks tables, which is what every other server here
stopped being, one at a time.

## What there is

- `user/lib/kfs.lua`, 2060 lines: the format - a superblock, a bitmap,
  128-byte inodes of up to twelve extents, directories as ordinary files,
  attributes in a block of their own, a journal of 256 blocks whose commit
  is checksummed with FNV-1a - and every operation on it: mount, mkfs,
  recover, begin / commit / rollback, find and the walk with names folded
  (`design.md` 8.3c), read a range into a region, write a file from one,
  store, mkdir, unlink, rename, attributes, stamps.
- The disk server, `diskfs_main` and `diskfs_handlers` in `init.lua`, about
  a thousand lines: nine requests (`list`, `read`, `write`, `delete`,
  `rename`, `mkdir`, `getattr`, `setattr`, `query`), three names that are not
  files (`.super`, `.device`, `.format`), the attribute index `query`
  answers from, the stick's partition behind the USB driver (`stick_home`),
  the block cache and the device's counters.
- `tools/test_kfs.lua`, 87 checks on the Mac - the journal's power-loss
  instants among them, chosen rather than hit by luck - run twice, the second
  time through the block cache. And `tools/kfs.lua`, the host tool that makes
  the QEMU disk and the stick's `/Home` with the same `kfs.lua`.

## The line this has to respect

- **A server receives exactly what it expects** (`CLAUDE.md`): the requests
  become a declared shape, `user/include/diskproto.h`, as `/Devices/audio`
  has, and a malformed one cannot be expressed.
- **The host testing goes with it rather than being given up** (`CLAUDE.md`,
  on this very move): the C core compiles on the Mac, and the same 87 checks
  hold it.
- **One implementation of the format.** Two - a C one in the machine and a
  Lua one in the host tool - would be two readings of one layout to keep in
  agreement, which is how a filesystem corrupts something that was fine.
- **Policy in C costs the shape of its bug.** The index is exact-match
  equality on attributes and small; it moves too, and scans rather than
  indexes if that is what keeps it small.

## The steps, each lived with before the next

1. **`kfs.c`: the format and its operations in C** - **done 29 September**
   (`testing.md` 18.279). `user/servers/kfs.c` and `kfs.h`, beside
   `fat_decode.c` and for its reason: no system calls, no Lua and no
   allocator, so the same file compiles on the Mac and on both boards. It
   reads and writes through a disk it is handed as **two functions, not the
   four this said**: `kfs.lua` has four because a Lua program cannot hold a
   pointer to a region, and in C a region is mapped memory, so a read into
   one is a read into a pointer - the whole blocks go from the disk to where
   they belong, and only a block entered or left part way passes through a
   block of the core's own. Everything it works in is a `struct kfs` its
   owner provides, about two megabytes: the transaction's 254 blocks and a
   directory being edited, up to a megabyte of it. Attributes stay the bytes
   `sys.pack` makes, opaque to the core.

   **Held on the Mac by the same 87 checks** - the host's Lua with the core
   in it as `require "kfsc"` (`build/host/kfs-lua` then, `build/host/lua`
   itself since step 2), answering as `kfs.lua` does, and
   `test_kfs.lua` run against it with `KFS_IMPL=c`, with and without the
   block cache. One check differs and says why: a window of a file is one
   disk call from the Lua and at most three from the C, its run and its two
   part-blocks. **And held to the Lua block for block** by
   `tools/test_kfs_cross.lua`: 293 operations run four ways over a fresh
   disk - all by the Lua, all by the C, taking turns, at random - must leave
   the same disk and come out the same way, and each must read the same
   tree off it.

   **What writing it found.** Two holes in `kfs.lua`'s `rename`, both fixed
   in it the same day, since it is what the machine runs: a directory moved
   into itself through a path in another case - `/Home/a` to `/HOME/a/b/a`
   went past a check of the paths as typed, since the server puts a path in
   the disk's spelling only while it keeps an index, and took `/Home/a` and
   everything in it out of reach - and a rename to `..`, a name no path can
   reach. And a branch that can never run: `write_file` joins a new run onto
   the last extent when it follows it, and a run always ends at a block that
   is not free, so none ever does - left over from blocks taken one at a
   time; the C does without it.

   **Where the C is stricter**, each only where the Lua would fail anyway or
   do harm: a name is checked before anything is written rather than at the
   directory's write; a transaction that would change more blocks than the
   journal holds is refused at the write that would not fit rather than at
   the commit; and recovering is refused while a transaction is open, which
   nothing does.
2. **The host tool on the C core** - **done 29 September** (`testing.md`
   18.280). `tools/kfs.lua` runs on `require "kfsc"`, so the QEMU disk, every
   suite's disk and the stick's `/Home` are made by the C the disk server is
   moving to. **The host has one Lua**: `build/host/lua` is
   `tools/host_lua.c` - upstream's library, a `main` that runs a script as
   `lua.c` does, collector and all, and the core - rather than upstream's
   `lua` beside a second binary that every caller of the tool would have had
   to be told about; nothing here ever gave the host's Lua an option or a
   prompt. `KFS_IMPL=lua` makes a disk with `kfs.lua` for as long as it is
   there, and `tools/test_kfs_tool.py` runs every command the tool has both
   ways over 5 MB of real files and holds them to the same images and the
   same words. **The Lua's only user now is the disk server**, and step 4
   takes `KFS_IMPL=lua` and the comparison with it.
3. **The disk server in C** (`user/servers/diskfs.c`), on `kfs.c`, speaking
   `diskproto.h`: every request above, the three names, the stick's
   partition, the block cache (`blockcache.lua`'s rules, in C) and the
   counters; attributes read with a small C reader of `sys.pack`'s format
   (a type byte, then its payload - flat tables of names and values), which
   is what `query` needs. The namespace's side for a disk mount packs the
   struct in C, as `con.wait` does for the console, so no table is made per
   request on either side. Measured with Disk Benchmark before and after,
   and the random read's parts measured again (18.273).
4. **`kfs.lua` and `diskfs_handlers` removed**, and `blockcache.lua` with them
   if nothing else uses it. And **the drive server's own reading of the
   format** goes too: `drives_decode.c` recognises a Kosmos volume and counts
   its free blocks with a `struct kfs_super` and checks of its own
   (`kfs_super_from`, `kfs_free_in`), a second reading of the superblock and
   the bitmap written to `kfs.lua`'s layout; it should ask `kfs.h`, and the
   two headers cannot both be included in one file until it does.

Each step ends with `make test` green, the checks it added and their
controls, and the documents saying what it became. Step 1 is the largest and
the one everything rests on; it is also the one that can be finished and
held entirely on the Mac.

## Step 3, drawn before it is built

**The disk server in C, `user/servers/diskfs.c`, on `kfs.c`**, answering what
`diskfs_handlers` answers today - the same operations, the same answers -
and speaking a declared shape, `user/include/diskproto.h`. In three parts,
each gated and lived with:

**3a. The server, and the namespace's side in Lua** - **done 29
September** (`testing.md` 18.282): the whole gate passed with it, and
`arm-diskwire` holds its wire. The namespace speaks it
with `string.pack`, as it speaks `/Temporary` (`ram_request`) and `/Drives`:
the pattern this system has for a declared protocol, and nothing new to get
right in the same step as everything else.

- **Its own header, with `ramproto.h`'s conventions** - `drivesproto.h`'s
  reason, the other way round: `/Temporary` has `watch`, which the disk does
  not, and the disk has regions, `.super`, `.device` and `.format`, which
  `/Temporary` does not. A listing a page at a time at an offset, a read of
  bytes at an offset with `more`, an error as a number the namespace puts
  into words. Paths of 512 bytes rather than 256: a disk holds folders deep
  enough for a whole path to pass 256 with names of 64.
- **The operations**: `list`, `read` - into the region handed with it, or
  inline a page at a time - `write` - from a region, or inline - `delete`,
  `rename`, `mkdir`, `getattr`, `setattr`, `query`; and `.super`, `.device`
  and `.format` as they are read and written now.
- **Values stay the namespace's.** `fs.write(path, table)` is stored as bytes
  with a mark in front, and read back as the table: the server does that
  today (`TABLE_MARK`), and in the protocol it moves to the namespace, which
  already packs and unpacks for `/Temporary`. The server stores bytes.
- **Attributes are `sys.pack`'s bytes on the wire as on the disk**, since
  what a launcher says - a program's path, its arguments - is longer than
  `/Temporary`'s forty-eight characters. `getattr` answers the node's facts
  in fields - kind, size, the stamp, the date, its extents - and its stored
  attributes as those bytes, paged if they are long; `setattr` sends the
  changes the same way. The server has to read them to merge them and to
  answer a query, so it has a small reader and writer of that format for
  flat tables of names to strings, numbers and booleans - what attributes
  are - and refuses anything else.
- **A query scans what it is asked about**: the folder named and what is
  under it, each file's facts and attributes against the terms. No index,
  and so **no path put in the disk's spelling first** - which is the second
  walk of every request while an index exists, 38% of a random read
  (18.273). Whether that holds up is part 3b's to measure.
- **And what is around it now**: the stick's partition behind the USB driver
  (`blockproto.h`, in C), the block cache (`blockcache.lua`'s rules), the
  device's counters behind `.device`, a write's date from `/Devices/clock`,
  a blank disk formatting itself once, and a replay said out loud.
- **Held by what holds the disk now** - `arm-queries`, `arm-interchange`,
  `x86-disk`, `run_power`, the USB suites' `/Home`, Disk Benchmark's suite -
  unchanged, since they speak through the namespace; and a check of the
  wire itself: a request of every shape a hostile caller can send refused
  without the server's state changing.

**3b. Measured.** Disk Benchmark before and after, the random read's parts
again (18.273), and queries timed on a `/Home` of two thousand files - which
is the question below, answered by numbers rather than by a guess.

**3c. The namespace's side in C**, as `con.wait` did for the console, if 3b
shows the Lua client allocating on a path that is felt: one reused table
and no string a request, rather than `string.pack` and five tables.

## What is Diego's to decide, if anything

Nothing yet: the direction was his ("yes, now"), and every step keeps the
format the disk already has, so a `/Home` made before is read after. One
question for step 3, to be put to him with measurements (3b): **whether a
query scans the folder it is asked about, or an index is kept.** Today the
index is built on the first query - every file's attributes, the whole disk
- and from then on every request is spelled the disk's way first, because
the index is keyed by path. The recommendation is the scan: queries come
from `find`, Tracker's search and the query suite, never from anything on a
frame's path, and dropping the index drops the second walk from every
request that is. 3a builds the scan, since it is the simpler of the two and
the one that can be measured; an index is added if the numbers ask for it.
