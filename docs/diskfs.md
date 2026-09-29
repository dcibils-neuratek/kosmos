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

1. **`kfs.c`: the format and its operations in C**, a core with no Lua in it
   (`user/servers/kfs/`), reading and writing through a disk it is handed as
   four functions - the same seam `sys.disk_read` is now, so the stick and
   the block cache sit behind it unchanged. **Held on the Mac by the same
   87 checks**: a host Lua module over `kfs.c` with `kfs.lua`'s interface,
   and `test_kfs.lua` run against it (`KFS_IMPL=c`) as well as against the
   Lua; and each writes images the other reads. Attributes stay the bytes
   `sys.pack` makes, opaque to the core.
2. **The host tool on the C core**: `tools/kfs.lua` through that module, so
   the QEMU disk and the stick's `/Home` are made by the code the machine
   runs. Then `kfs.lua` has no user outside the disk server.
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
   if nothing else uses it.

Each step ends with `make test` green, the checks it added and their
controls, and the documents saying what it became. Step 1 is the largest and
the one everything rests on; it is also the one that can be finished and
held entirely on the Mac.

## What is Diego's to decide, if anything

Nothing yet: the direction was his ("yes, now"), and every step keeps the
format the disk already has, so a `/Home` made before is read after. One
question for step 3, to be put to him then with measurements: whether the
index keeps being built at mount - a scan of every file's attributes, which
on a large `/Home` is the slow part of a first query - or is dropped for a
scan per query.
