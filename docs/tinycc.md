# C inside Kosmos: TinyCC and the IDE's Lua-and-C projects, before they are built

Written on 6 October 2026 for Diego to agree, with its drawing
(`docs/tinycc.html`) and its diagram (`docs/tinycc-architecture.png`).
Nothing here is built. **The one thing that could have sunk it was tried
first**, in the scratchpad, on both processors - *What was tried* below -
and it works.

Diego, 5 and 6 October 2026: "I want just one C compiler", "Tinycc and ide
integration so I can build c programs", "Tinycc writes a program file and
kosmos starts separately", "i want to be able to do a very simple all in IDE
that uses LUA and C and compile it in and run it", "also i might want to do
a C-only app as well", "IDE should ask me what kind of app im building when
i create a new project", and "the NEW app window in kosmos ide should have
at least 1 template of each app type so the user has a simple example to
start with".

---

## What it does

### In the IDE

- **New Project...** asks what kind of application it is, each with what it
  is for, and offers at least one template of each - a small example that
  builds and runs as it is:
  - **Lua app** - a desktop application in Lua alone: a window, buttons,
    files. *Hello Window*: a window with a button that counts. Nothing to
    build: Run runs it.
  - **Lua and C app** - Lua for the window and the orchestrating, C for the
    work that wants every cycle or touches hardware. *Mandelbrot*: a Lua
    window whose pixels a C function computes into a surface, with the time
    it took beside the time Lua alone would have taken; and *Sum, both
    ways*: one loop written in Lua and in C, timed side by side.
  - **C app** - a program that is C: a computation, a tool, later a server
    or a driver. *Primes*: counts the primes under ten million and says how
    long it took. It prints; a C app with a window of its own is question 2.
  - A name and a folder - `/Home/Projects/Mandelbrot` - and **Create**.
- **The project is a folder**: its Lua, its C, and once built its image,
  `mandelbrot.elf`, beside them. The tree shows them; `.elf` is greyed - it
  is made, not written.
- **Build** (F6, and the hammer in the bar) compiles the project's C with
  TinyCC and links it with Kosmos's runtime into the project's image. **Its
  errors and warnings are the Problems the IDE already lists** - each on its
  line, a click from it, the line marked in the editor - in TinyCC's words.
  A build that succeeds says so in Output, with how long it took and how big
  the image is.
- **Run** (F5) builds first when a C file changed since the last build, then
  starts the project's Lua in its image - for a C app, the one line of Lua
  the template carries, which calls the C. Output is the program's console,
  as it is for Lua today. **A change to the Lua alone builds nothing**: the
  image is the C and the runtime, and the Lua is read when it runs.
- **Stop**, as today.

### At the prompt

- **`cc`**, the compiler as a program: `cc primes.c -o primes.elf` - the same
  build the IDE does, its diagnostics as `file:line: error: ...` - for a C
  program written in Write, in a Terminal, or over telnet.

### Not here, and why

- **A debugger.** TinyCC can write DWARF; Kosmos has nothing that reads it
  or stops a thread at a line. Its own step, later.
- **C++.** TinyCC is a C compiler.
- **Compiling Kosmos itself inside Kosmos.** The kits that want every cycle
  - GL, video, the vector loops - stay built on the Mac with GCC, which keeps
  building Kosmos. TinyCC's code is two or three times slower than GCC's -O2
  and has no vector types (`CLAUDE.md`: "SIMD ... where it pays"); an
  application's own C is still many times faster than its Lua.
- **Code written to memory and run.** Diego's decision: "Tinycc writes a
  program file and kosmos starts separately". The kernel keeps never
  allowing a process to write code and then run it; a build is a file, and
  it starts as any image does (`docs/elf.md`).

---

## What was tried, 6 October

The design rests on one claim: that TinyCC, which knows nothing of Kosmos,
can link an application's C against Kosmos's runtime - built by GCC, with a
linker script - into an image Kosmos's loader accepts and runs. So it was
tried before a word of this was written, in the scratchpad, against the
tree as it is:

1. **TinyCC built on the Mac** as cross compilers for AArch64 and x86-64 -
   `mob`, commit `43c7708b`, 3 October 2026, from repo.or.cz: 482 KB and
   463 KB.
2. **The runtime** - the 531 objects of the lean userland every
   application's image carries (`make apps`: Lua, the libc, the bindings,
   the kits) - **packed into one relocatable object** by GCC's `ld -r`:
   21 MB without debug information, 7.8 MB compressed.
3. **The loader's test kit**, `user/kits/apptest/apptest.c`, compiled by
   TinyCC: 1,801 bytes in 3 ms. TinyCC needed two headers GCC supplies to
   Kosmos's build, `limits.h` and `stdint.h`.
4. **Linked by TinyCC**: the runtime, the kit and libgcc - **no relocation
   it could not do**, in 20 to 40 ms on the Mac. Two symbols the linker
   script defines were missing (`__bss_start`, `__bss_end`), and the layout
   was TinyCC's own - four segments, the read-only data first, the entry in
   the middle - which Kosmos's loader refuses, rightly.
5. **TinyCC's layout made Kosmos's**, in fifteen lines behind one switch:
   `.text.start` first; two segments, everything read-only joined to the
   code, as `user.ld` does and as TinyCC already does for NetBSD; and the
   code a page into the file, at the base, with the ELF headers outside it.
   The 16-byte header - "KOSMOS" and how much of the image is code -
   written into the file after the link, from its own program headers.
6. **Booted, by the loader's own suite** (`tools/run_loader.py`, handed the
   folder): **14 checks passing on AArch64 and on x86-64** - a program run
   in the TinyCC-linked image beside it, 20.1 MB and 20.5 MB, its kit
   answering `use("apptest.elf").answer()` with 42, alongside Doom, Quake
   and the Super Nintendo in theirs. The image was TinyCC's: 20,117,672
   bytes, where GCC's is 20,787,176.

So the hard question has an answer, and the rest of this page is ordinary
work: where things live, and what each piece is.

---

## The architecture

### At a glance

| Piece | What it is | Supplied by |
|---|---|---|
| **New Project, Build, Run, Problems** | the IDE's | `ide.lua`, grown; New Project a dialog of its own |
| **Templates** | three project folders, read-only, copied on Create | `/Kosmos/Templates/` in the image, new |
| **The compiler** | TinyCC, compiling and linking in one process | **the C Kit**, new: `user/kits/cc/`, TinyCC vendored unmodified in `runtime/upstream/tinycc/` with a Kosmos patch applied at the build |
| **`cc`** | the compiler at the prompt | `user/bin/programs/cc.lua`, new, the C Kit's first user |
| **The runtime it links against** | the lean userland as one object | built by `make` as `runtime.o`, carried compressed (question 1) |
| **The headers** | `kosmos.h`, the libc's, Lua's, `limits.h` and `stdint.h`, TinyCC's own | carried beside the runtime, `/Kosmos/Developer/include` |
| **The header stamp** | "KOSMOS" and the code's size, after the link | the C Kit, 30 lines, from `elfimage.c`'s own reading of an ELF |
| **Writing the image** | 20 MB to the project's folder | the disk server, through a region |
| **Starting it** | `-- kosmos: image mandelbrot.elf` | the loader, as Doom's today - nothing new |
| **A kit's door** | `use("mandelbrot.elf")` | `sys_user.c`'s list, one weak entry more: `kosmos_project_kit` |

**Does something already supply it?** The running half entirely: an image
with an application's C in it, its Lua started inside it, its kit reached
by `use` - that is Doom's, Quake's and the Super Nintendo's since
`docs/elf.md` step 5. The IDE's Run, Output, Problems and their lines are
step 3 and 4 of the IDE. What is new is the compiler, where its inputs live,
and a dialog.

**Does another application want it?** The compiler, yes - so it is a kit,
the C Kit, with the IDE and `cc` its first two users; a later Kosmos Write
macro, a shader editor in Cafesa3D or a plugin host would be the next.

### The C Kit

`use("/Kosmos/Kits/cc")` gives:

```lua
local cc = use("/Kosmos/Kits/cc")
local r = cc.build{
  sources = { "/Home/Projects/Mandelbrot/fractal.c" },
  out     = "/Home/Projects/Mandelbrot/mandelbrot.elf",
  kit     = "mandelbrot",            -- what `use("mandelbrot.elf")` finds
}
-- r.ok, r.milliseconds, r.bytes, and r.problems: { { file, line, column,
-- severity = "error" | "warning", text }, ... }
```

**In C, all of it** (`CLAUDE.md`, *Language split*): TinyCC is a compiler,
and its loops are over bytes. It runs in the caller's process - a kit is
code you run - with TinyCC's own allocator on the process's heap, and it
reads sources and writes the image through the libc, which resolves paths
through the caller's namespace. Lua is handed the result and the problems,
never a byte of the image.

**TinyCC's diagnostics become the IDE's problems** through TinyCC's own
error callback (`tcc_set_error_func`), which hands the kit each message
as TinyCC composed it, with its file and line - so no text is parsed back
out of what was printed.

### The kit inside the image

An application's C is a kit like Doom's: a function the image's runtime
calls when the Lua asks for it.

```c
#include "kosmos_kit.h"          /* lua.h, lauxlib.h, and what a kit is */

static int l_fill(lua_State *L) { ... }

KOSMOS_KIT(mandelbrot)           /* `use("mandelbrot.elf")` */
{
    lua_newtable(L);
    lua_pushcfunction(L, l_fill);
    lua_setfield(L, -2, "fill");
}
```

`KOSMOS_KIT(name)` defines `kosmos_project_kit` and the kit's name, which
`sys_user.c` lists as Doom's are listed - one weak entry, so the system's
image is unchanged by it and a project's image has it. **A C app** is the
same thing with one function, `main`, and the one line of Lua its template
carries: `use("primes.elf").main(args)`.

### Where the runtime lives - question 1

TinyCC links every image against the runtime: 21 MB a processor, 7.8 MB
compressed. Something has to carry it:

- **(a) In the system's image, compressed** - 7.8 MB more in a 36.6 MB
  image - and unpacked into `/Temporary/cc/` the first time a build asks,
  21 MB of memory for as long as the machine runs. Nothing to install;
  `cc` works on every machine Kosmos boots on. **Recommended.**
- (b) On the disk, `/Home/Developer/` - installed by `make install-apps`
  and carried by a stick - nothing in the image, and a machine without it
  cannot build.
- (c) Not a second copy at all: link a project's C against the system's
  own image, already in memory, and load it beside the runtime rather than
  with one. Smallest by far; it needs the loader and the kernel to place a
  second piece of code in a process, which is a change to Nebula and its
  own design.

**And it must be the system's runtime**: an image carries the protocols
of the build that linked it (`CLAUDE.md`, *an installed application
carries its own copy of the runtime*). With (a) the runtime and the system
are one build by construction; with (b) a stale `/Home/Developer` makes
images the system refuses, which is the Doom-on-the-M700 refusal again.

### What crosses in a region, and what in a message

The sources are read and the image written through the libc, which asks
the disk server - a 20 MB image is written from a region, never as a
message (`CLAUDE.md`, *Control by message, data by shared memory*). The
problems cross to Lua as a table of a few lines each. Nothing streams.

### The busiest path: Build, and Run after it

| Step | Measured on the Mac | Expected on the machine |
|---|---|---|
| the runtime unpacked, once a boot | - | 7.8 MB inflated: well under a second on the M700 |
| compiling a project's C | 3 ms (`apptest.c`) | milliseconds; a long file, tens |
| linking against the runtime | 20-40 ms | under 0.2 s on the M700; seconds under QEMU's TCG |
| writing the 20 MB image | - | **the cost**: the disk server's write speed - a USB stick's, on the ThinkPad and the M700 |
| starting it | 0.1-0.3 s (`run_loader.py`) | as Doom starts |

**The write is the slow step**, not the compiler, and the design keeps it
rare: a build writes the image only when the C changed, and a change to
the Lua builds nothing. If it is still felt, the image can be written to
`/Temporary` and started from there - question 4.

### What is C and what is Lua

C: TinyCC, the C Kit, the header stamp. Lua: New Project, the IDE's Build
and Run, `cc` at the prompt, the templates' Lua.

### What is new code, and the order to build it in

Each step its own revision and its own permanent test.

- **C1 - TinyCC in the tree.** Vendored unmodified, its licence (LGPL 2.1)
  in `LICENSE`; the Kosmos layout as a patch applied at the build, as
  `lua/patches/` is; built for the Mac as a cross compiler. *Test*, on the
  Mac: what was tried above, permanently - the loader's test kit compiled
  and linked by TinyCC, booted by `run_loader.py`, on both processors.
- **C2 - the runtime and the headers in the image** (question 1's answer).
  *Test*: the archive's sum held to the build's; a stale one refused.
- **C3 - the C Kit**, TinyCC built for Kosmos itself. *Test*: inside the
  machine, `apptest.c` built and its image run - the whole thing, on the
  machine, without the Mac; a file with an error answered with its line.
- **C4 - `cc` at the prompt.** *Test*: `cc primes.c -o primes.elf`, then
  the program run; an error's `file:line:` exact.
- **C5 - the IDE's Build and Run**, problems at their lines. *Test*: the
  display harness opens a project, builds, marks a broken line, fixes it,
  runs.
- **C6 - New Project and the three templates.** *Test*: each template
  created, built and run in the machine, and its window or its output
  checked - a template that stops working fails the gate.

---

## What is Diego's to decide

1. **Where the runtime lives**: (a) in the system's image, compressed,
   7.8 MB more (recommended); (b) on the disk, installed; (c) not a second
   copy - a Nebula change, its own design.
2. **A C app's window.** The first version's C apps print, as the template
   does. A C app that opens a window of its own needs a door to windows
   from C, which today only Lua has: (a) a small **Window Kit in C** -
   open a window, a surface to draw into, events - which a C app and a
   future C server could use (recommended, as its own step after C6); or
   (b) every C app keeps a few lines of Lua for its window, as the Lua and
   C template does.
3. **The templates**: Hello Window (Lua), Mandelbrot and Sum both ways
   (Lua and C), Primes (C) - or others.
4. **Where a build writes its image**: beside the sources, as drawn, so a
   project folder is whole and can be copied (recommended); or in
   `/Temporary`, faster to write and gone at the next boot.
5. **The name at the prompt**: `cc`, as on every Unix, or `tcc`.

---

## Appendix: the patch that was tried

TinyCC `mob` `43c7708b`, built with `-DTCC_KOSMOS_LAYOUT`. Step C1 carries
it as a patch applied at the build; TinyCC's own files stay as shipped.
After the link, the first 16 bytes of the code - `.text.start`, placed
there by a header object linked first - are written with `0x534f4d534f4b`
and the code's size rounded up to a page, as `user/user.ld` computes it.

```diff
--- a/tccelf.c
+++ b/tccelf.c
@@ -2331,6 +2331,11 @@
             k = 0x50; /* data */
         }
         k += j;
+#ifdef TCC_KOSMOS_LAYOUT
+        /* Kosmos: the image starts with its header and _start, at the base. */
+        if (0 == strcmp(s->name, ".text.start"))
+            k = 0x101;
+#endif
         /* make our standard sections come last to have _etext/_edata correct values */
         if (s->sh_num <= bss_section->sh_num)
             ++k;
@@ -2358,7 +2363,7 @@
             ++d->shnum;
         if (k < 0x700) {
             f = s->sh_flags & (SHF_ALLOC|SHF_WRITE|SHF_EXECINSTR);
-#if TARGETOS_NetBSD || TARGETOS_FreeBSD
+#if TARGETOS_NetBSD || TARGETOS_FreeBSD || defined TCC_KOSMOS_LAYOUT
 	    /* NetBSD only supports 2 PT_LOAD sections.
 	       See: https://blog.netbsd.org/tnf/entry/the_first_report_on_lld */
 	    if ((f & SHF_WRITE) == 0)
@@ -2410,6 +2415,11 @@
 
 /* Assign sections to segments and decide how are sections laid out when loaded
    in memory. This function also fills corresponding program headers. */
+#ifdef TCC_KOSMOS_LAYOUT
+#define KOSMOS_LAYOUT_ON 1
+#else
+#define KOSMOS_LAYOUT_ON 0
+#endif
 static int layout_sections(TCCState *s1, int *sec_order, struct dyn_inf *d)
 {
     Section *s;
@@ -2465,8 +2475,14 @@
         }
     }
     base = addr;
+#ifdef TCC_KOSMOS_LAYOUT
+    /* Kosmos: the headers are in the file and not in memory; the code
+       starts a page into the file, at the base. */
+    file_offset = (file_offset + s_align - 1) & ~(s_align - 1);
+#else
     /* compute address after headers */
     addr += file_offset;
+#endif
 
     n = 0;
     for(i = 1; i < s1->nb_sections; i++) {
@@ -2512,7 +2528,7 @@
             ph->p_align = s_align;
             if (f & SHF_EXECINSTR)
                 ph->p_flags |= PF_X;
-            if (n == 0) {
+            if (n == 0 && !KOSMOS_LAYOUT_ON) {
 		/* Make the first PT_LOAD segment include the program
 		   headers itself (and the ELF header as well), it'll
 		   come out with same memory use but will make various
```
