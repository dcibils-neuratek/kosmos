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
| `boot/` | the assembly entry points and the linker scripts, and `efi/`: Kosmos's own UEFI loader | the first instructions the machine runs, before there is a C stack - and on a PC, the loader that puts the kernel in memory at all (`boot.md`) |
| `arch/aarch64/` | **which CPU you are.** Page tables, exception vector, context switch, barriers | it is different on another architecture and is *reimplemented*, not abstracted |
| `hal/qemu-virt/` | **which peripherals you have.** UART, timer, interrupt controller, framebuffer, keyboard, block device | it is different on another board behind the same interface |
| `kernel/` | Nebula: threads, address spaces, IPC, capabilities, physical memory, and `pool.c`, where every kernel object lives and how the pools grow | it must run at EL1, or it defines the isolation boundary |
| `lua/upstream/` | Lua 5.4, exactly as shipped | never edited; changes are patches applied at build time |
| `lua/kosmos/` | our additions to the interpreter: the serializer, the freestanding header | it is C that belongs to the language rather than to the system |
| `runtime/libc/` | the freestanding libc: `memcpy`, `malloc`, `snprintf`, `setjmp` | a C program needs it and it has no operating system to ask |
| `runtime/upstream/` | vendored C: `stb`, `puff`, `miniz`, Lua's maths from musl, TinyGL, Doom, Quake, the Super Nintendo's core and more, each with its licence | somebody else wrote it; it keeps their licence and is not edited |
| `user/init/` | `init.lua` — init, the servers, the shell, the namespace client | it is the first process, or something the first process serves |
| `user/servers/`, `user/drivers/` | C: a process that owns something and rents it out, or drives hardware | something else's correctness or timing depends on it (`CLAUDE.md`) |
| `user/kits/` | C that runs inside the caller's process, one directory per kit | a loop over bytes a Lua program asks for |
| `user/lib/` | Lua libraries: `ui.lua`, `kfs.lua`, `theme.lua`, `zip.lua` | more than one program needs it |
| `user/include/` | the protocol headers both sides of a boundary compile against | it is a declared shape crossing into a server |
| `user/bin/` | programs and applications, in Lua, in `apps/` and `programs/` | it is something a person runs |
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

Agreed with Diego on 27 September 2026 (`roadmap.md` 6s) and drawn as
`docs/layout.html`, which is the picture of this section and is kept true
as the layout changes. Modelled on BeOS, which got the idea right: **a hard
line between what the system ships and what a person has.** BeOS put the
first under `/boot/beos` and the second under `/boot/home`; here they are
`/Kosmos` and `/Home`, and `/Home` travels - copied to another Kosmos
machine, a person arrives with everything that is theirs.

**The root is seven names**, each a plain word, each with a capital:

```
/Kosmos      what the system ships - the same on every machine of a version
/Home        what a person has, and takes with them
/Devices     the hardware, as Kosmos presents it: the console, sound, the
             camera, the processors, a drive's blocks, the backlight
/Drives      other drives, by their names - a USB stick, an SD card
/Network     the network: addresses, names and connections
/Running     what is running now, by name, for whoever may ask it
/Temporary   memory, gone at the next start
```

```
/Kosmos/
  Apps/          the applications that come with Kosmos, a window each
  Programs/      the console programs, run by name at a prompt
  Libraries/     the Lua libraries: use("/Kosmos/Libraries/ui.lua")
  Kits/          the C kits a Lua program calls into: use("/Kosmos/Kits/pdf")
  Themes/        the looks, a file each
  Deskbar/       the Deskbar's menu as it ships, laid out from each
                 application's header (roadmap.md 6zd)

/Home/
  Apps/          applications a person installed, each one folder holding
                 everything that is it - Doom's holds doom.lua, doom.elf and
                 its WAD - once a program can be loaded from a file (6t)
  Development/   projects: what the IDE opens
  Preferences/   a person's choices - the look, the keyboard, what starts at
                 login, what opens what (6z); the rest of the dotfiles at the
                 top of /Home move here (6s d)
  Documents/  Photos/  Movies/  Music/  Captures/
                 places in Tracker's sidebar, and where the applications
                 that make or open each keep them (6w)
  Desktop/       what is on the desktop, the Trash among it
  Deskbar/       the Deskbar's menu, only what the person made (6zd)
  Places/        the sidebar's shortcuts a person made
  Themes/        looks a person added
```

Names are found whatever their case and keep the case they were given:
`/home` finds `/Home`. What `layout.html` draws beyond this -
`/Kosmos/Servers`, `Drivers`, `Fonts`, `Settings`, `Logs` - is the plan,
and waits on what it needs: a server or a driver as a file needs the ELF
loader, and fonts as files need the image to stop carrying them.

---

## 3. The layout is a convention, not a tree

This is where Kosmos stops resembling every system the layout is borrowed
from, and it is the most important paragraph here.

**There is no global filesystem.** Nothing walks from `/`. A process has a
namespace - a short list of what was mounted for it - and a path that is
not in that list does not exist for that process. Not "permission denied":
*no such path*.

So the layout above is what **init assembles and hands out**, and different
processes get different subsets of it - `layout.html` draws three:

```
   a Terminal                    Doom, installed, after the loader
   ----------                    --------------------------------
   /Kosmos              read     /Kosmos/Libraries       read
   /Home                write    /Home/Apps/Doom         write
   /Devices                      /Devices/Audio
   /Drives, /Temporary  write    /Running/wm
   /Network, /Running            (that is the whole list)
```

Doom cannot open your photographs. Not because it is forbidden - because
they were never put in its namespace and it has no way to name them.

This is why the layout is worth agreeing on anyway: it is the *convention*
every program can rely on being handed, in the way POSIX programs rely on
`/etc` existing. It just is not enforced by a tree, and no server is
obliged to serve any part of it.

---

## 4. What is real today, and what it takes to get the rest

The root is real: all seven names, and `/Kosmos`'s six folders. What is
not yet is mostly what waits on programs being files rather than parts of
the image.

| what | today | what is left |
|---|---|---|
| applications and programs | inside the image, served by `binfs` as `/Kosmos/Apps` and `/Kosmos/Programs` | installed ones in `/Home/Apps`, once a program is loaded from a file (6t) |
| the Deskbar's menu | `/Kosmos/Deskbar`, a view of the same store, merged with `/Home/Deskbar` (6zd) | done |
| libraries and kits | `/Kosmos/Libraries` served from the image, `/Kosmos/Kits` answered in-process | done |
| the looks | `/Kosmos/Themes`, a file each; a person's in `/Home/Themes` | done |
| the servers and drivers | one C file each in `user/servers/` and `user/drivers/`, roles of the one image | files of their own, after the loader |
| `/Home` | the disk Kosmos started from, journalled - or memory, on a machine with none | done |
| a person's preferences | dotfiles at the top of `/Home`: `.appearance`, `.tracker`, `.terminal` and the rest; what opens what already in `/Home/Preferences` | into `/Home/Preferences` (6s d) |
| a person's places | Home, Desktop, and what they pin | Documents, Photos, Movies, Captures and Music (6w) |
| `/Temporary` | the ramfs | done |
| fonts and pictures | inside the image | files, when the image stops carrying them |

**A disk this Mac cannot mount is still a disk this Mac can write.**
`tools/kfs.lua` runs the filesystem on the development machine, over the
image file:

```
build/host/lua tools/kfs.lua create disk.img 64
build/host/lua tools/kfs.lua put    disk.img book.pdf /Home/books/book.pdf
build/host/lua tools/kfs.lua ls     disk.img /Home
build/host/lua tools/kfs.lua get    disk.img /Home/notes.txt notes.txt
build/host/lua tools/kfs.lua rm     disk.img /Home/old
```

That is the answer to the one real cost of not using FAT32. `make test`
runs `run_interchange.py`, which writes a file here, reads it inside the
machine, writes one inside the machine, and reads it back here - and, since
28 September, a zip each way, read by Python's `zipfile` on this side -
because a format only one of the two can write is a format that traps
everything you make in it.

**Programs as files is the step the rest waits on.** Every process used to
be the *same* image with a different role number. The ELF loader (6t) makes
a process from an image of its own: the kernel copies it into pages it
checks, and the ELF is read in userland (`docs/elf.md`). Doom, Quake and the
Super Nintendo leaving the image for `/Home/Apps` is its next step.

And a constraint that stays: **there is no dynamic linking.** No `dlopen`,
no shared objects. A C library is *linked into* whoever uses it, which is
why a kit is compiled into every process and an application's own C is in
its own ELF, beside its Lua.

---

## 5. The order this suggests

What is left, in the order `roadmap.md` has it:

1. **A person's preferences into `/Home/Preferences`** (6s d) - the
   dotfiles at the top of `/Home`, moved once on a home that has them.
2. **The places** (6w) - Documents, Photos, Movies, Captures, Music - and
   the applications that make or open each keeping them there.
3. **Doom into `/Home/Apps/Doom`** - the loader's step 5: its Lua, its ELF
   and its WAD in one folder; then Quake and the Super Nintendo.
4. **Fonts and pictures as files**, and `/Kosmos/Settings` and `Logs`.
