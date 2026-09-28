# Programs from a file

How a program that is not in the image is started: an ELF file, read from
`/Home`, made into a process. Written before it is built, as
`threads.md` was, and agreed with Diego on 27 September 2026:
"i want to go ahead and make the elf loader so we can start shipping a
really usable system with games on /Home" (`roadmap.md` 6t). The layout it
installs into is `layout.html`.

---

## Why, in one paragraph

Every process in Kosmos is the same image entered at a different role, so a
program with C in it has to be compiled into that image. That is why Doom,
Quake and the Super Nintendo are in the MEGA build, why the system image is
a GPLv2 work whenever they are in it, and why nobody else's application
could be installed at all - Kosmos is rebuilt, not installed on. A program
loaded from a file changes that: a game is a folder in `/Home/Apps`, the
image that ships is the system and nothing else, and a person can take
their games to another machine with their home. It is the step
`roadmap.md` called the difference "between a system that is rebuilt and a
system that is used".

---

## The line this has to respect

**The kernel does not know what a file is** (CLAUDE.md). An ELF is a file
format, so reading one is not the kernel's business. What the kernel knows
is pages, address spaces, threads and capabilities - and that is exactly
what starting a program needs from it: pages put at addresses with
permissions, an entry point, and a thread. So the work divides the way the
rest of the system does:

- **A program in userland reads the file**, checks it, and lays its
  segments out in memory it owns. A malformed file is that program's
  problem and is refused there, with a sentence.
- **The kernel is handed an image in the one form it already knows** -
  Kosmos's sixteen-byte header, the code, the data - and makes a process
  of it, checking what it has always checked about that form: the magic,
  the code's size, and that code is read and execute and data read and
  write, never both (*The design* has why that form rather than a list of
  segments: it is the check the kernel already makes).

QNX puts its loader in the process manager beside the kernel; seL4 leaves
it entirely to userland. This is seL4's answer, for the same reason: the
kernel stays a thing that knows about memory, and the part that parses
bytes somebody else wrote is a process that can fail alone.

---

## What happens today

Mapped on 27 September, with every claim against the code:

- **One image, and every process runs it.** The userland is linked once
  (`user/user.ld`, at `USER_BASE` - 0x80000000 on AArch64, 0x40000000 on
  x86-64), flattened with `objcopy -O binary` into `init.bin`, and compiled
  *into the kernel* as a `const` array by `tools/bin2c.py` (`Makefile`
  2177-2186). Its first sixteen bytes are Kosmos's own header: a magic,
  `KOSMOS`, and how many bytes of it are code (`user.ld` 42-43).
- **`SYS_SPAWN` hands the child its parent's image**, always:
  `process_spawn` calls `process_create(parent->name, parent->image,
  parent->image_len, arg)` (`kernel/process.c` 1014-1029). A process is
  told what to be by one word, its *role* (`user/init/main.c` 85-97): a C
  server, or Lua with `init.lua` deciding from there.
- **`process_create` already checks that header** (`process.c` 805-825):
  the magic, and a code size that is page-aligned, not zero and inside the
  image. The code is **mapped in place**, the same physical pages in every
  process, read and execute; the data is **copied** into pages of the
  child's own (877-929). A heap at +40 MB, a stack under +46 MB, and the
  entry at base + 16, whatever the linker said.
- **A Lua program already comes from a file.** `run` spawns a *runner*
  (role 12) and sends it the path; the runner reads the source through its
  own namespace and loads it (`init.lua` 5997, 6195-6210). A Lua program
  in `/Home` runs today - which is how the IDE runs one.
- **Doom, Quake and the Super Nintendo are kits** in that one image:
  `doom.lua` does `use("/Kosmos/Kits/doom")`, and `sys.kit` finds `doom` compiled
  in under `KOSMOS_DOOM` (`sys_user.c` 2720-2760). Their Lua is a file
  already; their C is the image. *(Doom left on 28 September, step 5.)*
- **The image is checked four times a boot** against sums the build wrote
  (`kernel/main.c` 95-240) - which covers the image in the kernel, and
  nothing a disk will hold.
- **Nothing reads an ELF** - not the kernel, not the UEFI loader, which
  reads Multiboot2's header, not a tool.

So what an installed game lacks is small and exact: **an image of its own**.
Everything else - a runner, a namespace, `use` finding a kit, a window - is
what every Lua program already has.

---

## The design, proposed

### An installed program is an image of its own, next to its Lua

A game is a folder, and **everything of it is in the folder**:
`/Home/Apps/Doom/doom.lua`, the application as it is today; beside it
**`doom.elf`** - the userland linked as `init.elf` is, runtime and Lua and
the kits it uses, with Doom's engine in it; and the WAD it plays, its saves
and its settings. The Lua names its image the way it names its icon:

```lua
-- kosmos: application
-- kosmos: icon App_Doom
-- kosmos: image doom.elf
```

A program with no `image` line runs in the system's image, as every
program does now. A Lua program needs none; a program with C in it names
the image its C is in.

### The file is an ELF, and a program reads it

**ELF on the disk**, because it is what the toolchain writes: `readelf`
and `file` know it, and it has room for the symbols a crash report will
want one day. It is **read in userland**, by a small C reader in the
runtime, and turned into the one form the kernel already accepts - the
Kosmos header, the code, the data - in a memory region:

- ELF64, little-endian, this machine (`EM_AARCH64` or `EM_X86_64`), an
  executable;
- its loadable segments inside the forty megabytes an image may use, in
  order - code read and execute, then data read and write, never both;
- its entry at base + 16, where the Kosmos header says a program starts;
- every size inside the file, every offset inside the file, nothing
  overlapping.

Anything else is refused there with the sentence that says which, and the
kernel never sees it. The reader is tested on the Mac, like
`fat_decode.c`: good images, and one broken in each of those ways.

### The kernel is handed an image, and copies it

One call, **`SYS_SPAWN_IMAGE`**: `SYS_SPAWN`'s arguments and a memory
region holding an image. The kernel checks the header as it does now -
the same code - and then **copies** the code as well as the data into
pages of the child's own. The system's image can be shared because the
kernel holds the only copy; a region is memory the parent can still
write, so a shared page would be code the parent could change under the
child. A copy of twenty megabytes is a few milliseconds, once, when a game
starts; two games sharing their pages is an improvement for later, and
needs the region sealed first.

The child is otherwise every other process: the caps and powers it was
handed, a role word, a heap and a stack - so `run` starts a runner in the
game's image exactly as it starts one in the system's, and the runner
loads `doom.lua`, which reaches the engine this image carries. **Not as a
kit**: Doom's C is Doom's, and a kit is what Kosmos ships for every program
(Diego: "there is no /Kosmos/Kits/doom folder and wont be"). It names the file:
`use("doom.elf")`, decided below.

### The bytes are the ones the build wrote

The ThinkPad taught this one: a stick can hold bytes its image did not
(`boot.md`). So the build writes the image's sums beside it - per page,
as `bin2c.py` does for the system's - and the reader checks them before
anything is spawned, and names the page that differs.

---

## What does not change

- **The kernel knows no file format**: it checks the header it already
  checks, and copies pages.
- **No dynamic linking.** Each image carries the runtime and the kits it
  uses; a kit fixed in the system is fixed in a game when the game is
  built again. That is the price, and `layout.md` §4 already paid it.
- **Capabilities.** An installed program gets what its launcher hands it
  and nothing more - its own folder, read and write, where its WAD, its
  saves and its settings are; the screen, sound and
  keys through the window manager - and never `/Home/Documents` unless it
  is given it. What a person grants a program they installed is
  per-launcher permissions' ground, and is not decided here.
- **The system's image** stays in the kernel, checked four times a boot;
  the browser and Video stay in it.

---

## The steps, each checked on its own

1. **DONE on 27 September** (`testing.md` 18.221) - **`SYS_SPAWN_IMAGE`**
   in the kernel, on both machines. The kernel suite
   spawns a copy of the system's image from a region and sees it run; a
   region with a bad magic, a code size past its end, or too small is
   refused, and the parent writing the region afterwards changes nothing
   the child runs. The stale comments in `process.h` (49-50, 552-560,
   "copied rather than mapped in place") are corrected on the way.
2. **DONE on 27 September** (`testing.md` 18.223) - **The ELF reader**, in
   C, tested on the Mac: a real image read into the
   same bytes `objcopy` makes, and each malformed case refused with its
   sentence.
3. **DONE on 27 September** (`testing.md` 18.222) - **An image of its own,
   built**: a make target linking the runtime with
   one kit - a small test kit first, then Doom's - into `build/apps/`, with
   its sums.
4. **DONE on 27 September** (`testing.md` 18.224) - **`run` learns
   `-- kosmos: image`**: reads the file into a region,
   checks it, spawns the runner in it. A suite runs a program whose kit is
   only in its own image, and one whose image is broken, which is refused
   with a sentence rather than a crash.
5. **DONE for Doom on 28 September** (`testing.md` 18.249) - **Doom leaves
   the image**: `/Home/Apps/Doom` with `doom.lua` and `doom.elf` and its
   WAD in one folder, `use("doom.elf")` in place of `use("/Kosmos/Kits/doom")`,
   the stick built that way, and `KOSMOS_DOOM` out of the system's build.
   What it took, piece by piece:
   - **`doom.elf`, linked by `make apps`** beside `apptest.elf`, and held to
     the same two promises: the image has `kosmos_doom_kit`, and the
     system's image does not. The sources moved from `user/bin/apps/doom/`
     to **`user/installed/Doom/`** - what the build installs into `/Home`
     rather than serves from `/Kosmos` - and `DOOM` is no longer a build
     variable: nothing compiles Doom into a system image, so there is no
     variant to name.
   - **Against the lean userland.** Linked against a `FULL=1` one, Doom's
     image carried the wallpapers, the browser and FFmpeg too, 33 MB
     stripped; against the lean one it is 17.8 MB. Most of that is still
     the system's own files, which binfs serves from the system's image and
     no application needs a copy of - **the next thing to take out**: an
     application's image should be the runtime and its own C, a few
     megabytes.
   - **`use("doom.elf")`**: `sys.kit(name, true)` answers only a kit marked
     as the application's own (`own` in `sys_user.c`'s table), and refuses
     outside that image with "this program is not running in doom.elf";
     `kits` does not list them, since they are nobody else's to use.
   - **Found by its name**: `ns.program` looks in `/Home/Apps/<name>/` after
     `/Kosmos/Apps` and `/Kosmos/Programs`, so `doom` at the prompt and
     `wm doom` start it.
   - **Listed where applications are**: the Deskbar reads the folders in
     `/Home/Apps` as a third layer after the shipped menu and the person's
     (`deskbarmenu.installed`), each under the section its header names -
     Doom under Demos - and File types lists what they open, so a WAD
     opens in Doom. Nothing is registered, so deleting the folder takes all
     of it away.
   - **The WAD beside it**: `doom.lua` plays the `doom1.wad` in its own
     folder unless another is named. A stick's `/Home` gets `Apps/Doom`
     from the build - `doom.lua`, the stripped x86 `doom.elf`, and a
     `doom1.wad` from the top of `HOME_DIR` copied in beside them
     (`homeimage.py`'s installed pairs, which win over the folder's own at
     the same path).

   **Then Quake and the Super Nintendo**, the same way - **DONE the same
   day** (`testing.md` 18.253): `user/installed/Quake` and `SNES`,
   `quake.elf` and `snes.elf`, Quake's pak beside it in `id1/` and the
   Super Nintendo's ROMs where they were, in `/Home/roms/snes`, a person's
   own. `QUAKE` and `SNES` are gone as build variables and `MEGA=1` is
   `FULL=1`; `tools/installed.py` is the one list a stick and `make
   install-apps` read. Step 5 is done.

---

## Decided by Diego, 27 September

"ELF copied yes":

- **ELF on the disk**, read in userland into Kosmos's own image form -
  rather than Kosmos's flat image, which would need no reader but is a
  format nothing else knows.
- **Copied, not shared**, until regions can be sealed - a game started
  twice holds its code twice.
- **`-- kosmos: image doom.elf`** as the way a program names its image.

**And the same evening, what an application is**: "doom is a simple app
that happens to be a game that holds lua and binaries in the same folder";
"just hold all doom related into the doom app folder. I dont want settings
and savedata from many programs to start living in othjer places"; "WAD
files are inherent parts of the game, not savedata that you generate". So:

- **An application is one folder holding everything that is it** - its
  Lua, its ELF, what it plays, its settings, its saves. There is no
  `/Home/Games`; deleting the folder removes the application entirely, and
  copying it copies all of it.
- **An application's C is not a kit.** There is no `/Kosmos/Kits/doom` and there
  will not be: kits are what the system ships for any program to use, and
  Doom's engine belongs to Doom.
- **Self-contained, on the system's parts**: "i like the idea that most
  apps are self contained, whule using reusable components from Kosmos
  Kits, Servers, Drivers, etc". An application brings what is only its
  own and asks Kosmos for everything shared.
- **It leaves nothing anywhere else, and deleting it is removing its
  folder**: "i dont want to be like windows apps that polluted the system
  with files", and "i want to be able to delete an app and all that the app
  brought,its gone". No registry, no files in the system's folders, no
  settings or saves elsewhere; the Deskbar lists the folders in
  `/Home/Apps`, so there is nothing to unregister either.

**Kept by convention for now, and not enforced.** Every program started
today is handed the whole disk at `/Home`, read and write, so an installed
application could write anywhere in it; making that impossible would mean
handing it only its own folder, as a capability the disk's server holds.
**Diego set that aside**: "lets not do this yet, we need to use the systme
first before enforcing things that limit the usage. right now is all
experimentation". So the rule stands as where things go - an application
keeps what it brings in its folder - and the capability to one folder is
for when using the system says it is time.

What a person saves *through* an application - a scene Cafesa3D writes into
`/Home/Documents` because they chose Save there - is theirs and stays when
the application goes.

## And the answers to what was open - Diego: "1 yes, 2 yes, 3 yes"

- **An application's Lua reaches its own C by the file**: `use("doom.elf")`
  returns the table the engine in that image builds, as `use` of a kit
  does; `/Kosmos/Kits` holds only what Kosmos ships. And by the same rule an
  application's own Lua files: `use("menu.lua")` is the file beside the
  program.
- **The manifest is the program's `-- kosmos:` header**, which `binfs`, the
  Deskbar and the launcher already read, and the program is the Lua file
  named after its folder: `Doom/doom.lua`.
- **The applications Kosmos ships keep their settings in
  `/Home/Preferences`**, one entry each, since their folders are the
  system's - today they are dotfiles at the top of `/Home`.
