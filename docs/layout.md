# What lives where

Two different questions that are easy to confuse, so they get separate
halves of this document:

- **The repository** — where source code goes while you are writing it.
- **The running system** — what paths a process sees once the machine is
  up, which is *not* a picture of a disk. See §3 before assuming it is.

---

## 1. The repository

| directory | what it is | the rule for putting something here |
|---|---|---|
| `boot/` | the assembly entry point and the linker script | the first instructions the machine runs, before there is a C stack |
| `arch/aarch64/` | **which CPU you are.** Page tables, exception vector, context switch, barriers | it is different on another architecture and is *reimplemented*, not abstracted |
| `hal/qemu-virt/` | **which peripherals you have.** UART, timer, interrupt controller, framebuffer, keyboard, block device | it is different on another board behind the same interface |
| `kernel/` | Nebula: threads, address spaces, IPC, capabilities, physical memory | it must run at EL1, or it defines the isolation boundary |
| `lua/upstream/` | Lua 5.4, exactly as shipped | never edited; changes are patches applied at build time |
| `lua/kosmos/` | our additions to the interpreter: the serializer, the freestanding header | it is C that belongs to the language rather than to the system |
| `runtime/libc/` | the freestanding libc: `memcpy`, `malloc`, `snprintf`, `setjmp` | a C program needs it and it has no operating system to ask |
| `runtime/upstream/` | vendored C: `stb_truetype`, `puff` | somebody else wrote it; it keeps their licence and is not edited |
| `user/init/` | `init.lua` — init, the servers, the shell, the namespace client | it is the first process, or something the first process serves |
| `user/lib/` | libraries: `ui.lua`, `gfx.lua`, `kfs.lua`, `theme.lua`, plus `gfx.c` and `sys_user.c` | more than one program needs it |
| `user/bin/` | programs and applications, in Lua | it is something a person runs |
| `user/tests/` | guest-side tests | it can only be answered by a running machine |
| `assets/` | vendored data: fonts, images, with their licences | somebody else made it and the build converts it |
| `tools/` | host-side: test runners, converters, the image builder | it runs on your desk, not on the target |
| `bench/` | benchmarks and their baselines | it produces a number that gets compared to yesterday's |
| `docs/` | this | |
| `book/` | the book | |

Two directories look similar and are not. **`arch/` is which processor.
`hal/` is which board.** A Pi 5 and QEMU's `virt` are both AArch64 — same
`arch/`, different `hal/`. A Pi 1 is ARMv6 — different `arch/` *and*
different `hal/`.

---

## 2. The running system

Modelled on BeOS, which got this right: **a hard line between what the
system ships and what a person accumulates.** BeOS put the first under
`/boot/beos` and the second under `/boot/home`, and you could delete the
second without breaking the machine.

```
/system/            the operating system. Not written while running.
  kernel            Nebula's own image
  servers/          the system servers
  libraries/        libraries the system ships: ui, gfx, kfs, theme
  add-ons/          loaded on demand: codecs, translators, drivers
  documentation/
  settings/         system-wide configuration

/user/              what a person installed. Writable.
  applications/     graphical, marked `-- kosmos: application`
  programs/         console
  libraries/        their own Lua libraries
  add-ons/

/home/              what a person made
  settings/         preferences, per person - `.appearance` lives here
  desktop/          what is on the desktop
  people/           BeOS People files: a node with attributes and no
                    contents, which this filesystem already supports
  queries/          saved live queries, which re-evaluate when asked

/dev/               devices: cpu, memory, screen, keyboard, console, wm
/tmp/               the ramfs. Fast, and gone at the next boot.
```

Two departures from BeOS worth naming:

**No `/boot` prefix.** BeOS had one because the boot volume was one of
several mounted disks. Here the layout *is* the namespace, so a prefix
naming the volume it came from would be describing something a process
cannot see anyway.

**`bin` split into `programs` and `applications`.** BeOS separated
`apps/` from `bin/` for the same reason and it is the right distinction:
a program prints and reads lines, an application opens a window. `/bin`
today holds both and the Deskbar has to filter it by reading a manifest
line out of every file.

---

## 3. The layout is a convention, not a tree

This is where Kosmos stops resembling every system the layout is borrowed
from, and it is the most important paragraph here.

**There is no global filesystem.** Nothing walks from `/`. A process has a
namespace - a short list of what was mounted for it - and a path that is
not in that list does not exist for that process. Not "permission denied":
*no such path*.

So the layout above is what **init assembles and hands out**, and different
processes get different subsets of it:

```
   the shell                     a game started from the Deskbar
   ---------                     ------------------------------
   /system/libraries             /system/libraries
   /user/programs                /dev/wm
   /user/applications            /home/settings
   /home
   /dev
   /tmp                          (that is the whole list)
```

The game cannot open your documents. Not because it is forbidden - because
`/home` was never put in its namespace and it has no way to name it.

This is why the layout is worth agreeing on anyway: it is the *convention*
every program can rely on being handed, in the way POSIX programs rely on
`/etc` existing. It just is not enforced by a tree, and no server is
obliged to serve any part of it.

---

## 4. What is real today, and what it takes to get the rest

Most of the layout above does not exist yet, because most of the system is
still compiled into the image rather than read from a disk.

| what | today | to change it |
|---|---|---|
| programs and applications | inside the kernel image, served from `/bin` | write them to the disk at build time |
| libraries | inside the image, served from `/lib` | the same |
| the servers | functions in `init.lua`, chosen by a role number at spawn | see below |
| fonts and images | inside the image, ~700 KB of it | write them to the disk; the wallpaper case wants this first |
| `/home` | a real disk, real files, journalled | done |
| `/tmp` | the ramfs, at `/data` today | rename |

**A disk this Mac cannot mount is still a disk this Mac can write.**
`tools/kfs.lua` runs the filesystem on the development machine, over the
image file:

```
build/host/lua tools/kfs.lua create disk.img 64
build/host/lua tools/kfs.lua put    disk.img book.pdf /home/books/book.pdf
build/host/lua tools/kfs.lua ls     disk.img /home
build/host/lua tools/kfs.lua get    disk.img /home/notes.txt notes.txt
build/host/lua tools/kfs.lua rm     disk.img /home/old
```

That is the answer to the one real cost of not using FAT32. `make test`
runs `run_interchange.py`, which writes a file here, reads it inside the
machine, writes one inside the machine, and reads it back here - because a
format only one of the two can write is a format that traps everything you
make in it.

What is still missing compared to a mounted volume is Finder. That would
be a FUSE filesystem, and the way to build one without a second
implementation of the format is to embed this same `kfs.lua`.

**Writing files to the disk at build time is now cheap**, and that is new:
`build/host/lua` runs `kfs.lua` unchanged on the development machine, so
the same code that formats a disk inside the machine can populate an image
outside it. That is the step that unblocks fonts, wallpapers, a WAD, and
moving `/bin` out of the binary.

**Servers as separate files is the expensive one, and worth being clear
about.** Today every process is the *same* ELF with a different role
number - init, the shell, a server and an application are one binary
entered at different places. Making `/system/servers/disk` a real file that
is loaded and started means an ELF loader for user processes, which does
not exist.

And a constraint that matters for any plan to write servers in C:
**there is no dynamic linking.** No `dlopen`, no shared objects. A C
library is *linked into* whoever uses it, so `/system/libraries` can hold
Lua libraries - which are loaded from source at runtime - and cannot hold C
ones. C system libraries are a build-time fact, not a filesystem one,
unless somebody builds a dynamic linker.

None of that stops a C server. It means a C server is another role in the
same image until there is a loader, which is exactly what the Lua ones are
now.

---

## 5. The order this suggests

1. **Rename to the convention** - `/tmp` for the ramfs, `/home` as it is.
   Costs nothing and stops the names drifting further.
2. **A host image builder**, using `kfs.lua` on the host. Small, and it is
   the thing every later step needs.
3. **Move fonts and assets to the disk.** Reclaims most of the 19.6 MB the
   image currently costs across fourteen processes, and makes wallpapers
   possible.
4. **Move `/bin` and `/lib` to the disk**, and split them into
   `/system/...` and `/user/...`.
5. **An ELF loader**, when a server or an application should be its own
   binary. This is also what Doom needs, since Doom is not Lua.
