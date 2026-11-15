<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# The Debug Kit - a function's name from an address

Written 15 November 2026, before it is built. It is the last part of
*Seeing what goes wrong* (`roadmap.md`, JetSki): a page that stopped
already says what fault it was, where, and on which thread
(`testing.md` 18.825). What it cannot say yet is **which function** it was
in. On The Verge that function was
`JSC::Parser<...>::parseMemberExpression`, and that took a Mac, the
build's own image and `addr2line`. The tab should say it.

## What it does

- **An address becomes a function's name and an offset into it**:
  `0x43131296` becomes `JSC::Parser<JSC::Lexer<char16_t>>::parseMemberExpression
  <JSC::ASTBuilder>(JSC::ASTBuilder&) + 0x...`, demangled where the name
  is C++.
- **From any Kosmos image that keeps its symbols.** Every image the build
  links does: an installed application is stripped of its debug
  information (`--strip-debug`) and keeps `.symtab`, which is what this
  reads. It does not need DWARF, so it gives no file or line. Lines would
  need `.debug_line`, which the stick does not carry, and are left for
  later.
- **Asked by whoever wants a name**, not only JetSki. The first user is
  JetSki's *What happened*. Then come the system log's fault lines, read
  in Log View, and a crash report for any application. Those last two are
  why this is a kit and not part of JetSki (the premise: *does another
  application want this?*).

## How it is built

**C, in `user/kits/debug/`, one door, `debug.h`**, reached from Lua as
`use("/Kosmos/Kits/debug")`. It is a loop over a few hundred thousand
records and the bytes of a file, so it belongs in C.

```c
struct debug_image *debug_open(const char *path);   /* NULL: no symbols */
int  debug_name(struct debug_image *, uint64_t addr,
                char *name, size_t room, uint64_t *offset); /* 0: found */
void debug_close(struct debug_image *);
```

- **Opening reads only what it must.** It reads the ELF header and the
  section headers, then `.symtab` whole: 7.1 MB for JetSki. From it, it
  keeps the functions as `{address, size, name offset}`, which is 252,000
  of them and 4 MB for JetSki, sorted by address. It does not read
  `.strtab` (28 MB for JetSki). A name is read from the file at its offset
  when it is asked for, a few hundred bytes. The file goes through the
  Storage Kit like any other read.
- **A lookup is a binary search**, then one read for the name. When two
  functions share an address (an alias, or identical code folded
  together), the sized one wins.
- **Demangling is `__cxa_demangle`, when the caller links it.** A C++
  image (JetSki) has libc++abi already. A Lua application's runtime does
  not, and gets the mangled name. The kit takes the function through a
  weak reference, so no second demangler is written.
- **Nothing is kept between crashes.** The table lives while the caller
  holds the image open. JetSki opens it when a tab's page stops and closes
  it when the page is shown. On a desktop with no crashes, nothing is
  read.
- **The address is the image's own.** Kosmos images are linked at a fixed
  address, with no relocation at load time (`.text` at `0x40000000`).
  So the instruction a death records (`struct process_death`'s `pc`) is
  the address to look up, as it is. When images move, the kit takes the
  load base as well.

**Which image.** A web process runs JetSki's own image in another role
(`ProcessLauncherKosmos.cpp`), so JetSki looks up a web process's fault in
the file it was itself started from, `/Home/Apps/JetSki/jetski.elf`.
Anyone else asks by path.

**The busiest path** is the first lookup after a crash: about 7 MB read
and a sort of 252,000 records. On the M700 that is expected to be well
under a second, and it happens once, after the page is already gone. The
crash page is drawn first with the address and fills in the name when it
arrives. It never waits.

## How it is tested

The host test (`tools/test_debug.c`) builds the kit on the Mac and looks
up known addresses in the build's own `jetski.elf`, held to what
`x86_64-elf-nm -C` says. The control is a lookup with the sort left out,
which must fail. In the guest, `x86-jetski`'s stopped-page check also
reads the function's name on *What happened*. The process the test kills
is waiting in the event loop, so the name it checks is that loop's
function.
