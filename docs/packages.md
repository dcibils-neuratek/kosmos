# Packages

Everything Kosmos can install, from a package server, and the window and the
program that install, update and remove it. Diego, 12 October 2026, after
Doom, Quake, the SNES and the developer files had been pushed to the M700
after every netboot: "perhaps add a program in kosmos that requests those
from the mac as needed?", "where we show all available packages", "and we
can install them on demand", "from the server (which is the mac now)", then
"i think app store is very apple, so we need a new name", "where we can see
all installed packages, remove them, install new ones, update packages to
new versions", "and packages might have dependencies?", "like an APT package
system?" - and, of what a package is, "a zip file containing an app with a
manifest on what the app is? its files and where are they located?".

The window is drawn in `docs/packages.html` and agreed ("Mockup is
perfect"); this is the feature set and the architecture, and
`packages-architecture.png` the picture of what it stands on. Its first
brick is built: `install` and the Mac's server (0.11.175, `testing.md`
18.570).

## What a person can do

- **See every package** the server offers: what it is, its size, its
  version, and whether it is installed here, from which version - by
  everything, installed, with updates, not installed, and by kind (games,
  emulators, developer files, data).
- **See a package's details**: what it is and who made it, what is
  installed and what is offered, its files and where they go, the powers it
  needs - sound, the screen, the network - what it depends on and what
  installed packages need it.
- **Install one**, and with it whatever it depends on that is not installed,
  after one confirmation that lists everything and the total size, and the
  powers it will be able to use.
- **Update one**, or **all** - every installed package with a newer version
  on the server, as `apt upgrade` does.
- **Remove one** - exactly the files it installed, never what a person made
  beside them - told first when another installed package needs it, and
  offered to remove both.
- **Do all of it at the prompt** with `pkg` - `pkg`, `pkg install`,
  `pkg remove`, `pkg update` - which the window and the program share.
- **Never be left with half a package**: a download that was cut short, or
  a file whose sum is wrong, is refused before anything installed is
  touched; and a package made for another build of Kosmos - other protocols
  - is refused with a sentence, rather than installed to fail at its start.

## What a package is

**A zip of its folder with a manifest inside**, `package.txt`, a line a
fact, so another machine can read and check it:

```
name        quake
title       Quake
kind        game
about       id Software's Quake, the shareware episode
version     0.11.176
protocols   7f3c9a1e0b2d4c58      # sys.protostamp of the build it is for
into        /Home/Apps/Quake
needs       audio display input
depends
file        quake.lua        13102  9b1e...   # size and SHA-256
file        quake.elf     11422864  51c0...
file        id1/pak0.pak  18689235  e3a7...
```

**Written by the build, never by hand.** An application already says what
it is in its header - `-- kosmos: name`, `icon`, `needs`, `image` - and
`tools/packages.py` writes the manifest from that, so the two cannot
disagree. A package's dependencies are one new header word, `-- kosmos:
depends quake`. The developer files, which have no header, have their facts
in `packages.py`.

**The server's index** is every manifest's facts but its files, a package a
paragraph, as APT's Packages file is - so the list costs one small request,
and a package's files are read from its own manifest once it is chosen.

## What it is made of

**The application is the thinnest layer there is** (`CLAUDE.md`): it shows
and asks; installing is a library's.

| Piece | Supplied by | New or as it is |
|---|---|---|
| The window, the list, the details, the questions | the UI kit | as it is |
| **Reading the index and manifests; installing with dependencies; removing exactly what was installed; updating** | **`packages.lua`**, a library - what `install` does today moved out of the program, so `pkg`, `install` and the window are one copy | **new**, from `install` |
| A package's download | `http.lua` | as it is |
| Unpacking it | `zip.lua` | as it is |
| **Each file's sum**, checked before anything is replaced | the Crypto Kit's SHA-256 | as it is |
| Which build this is | `sys.protostamp`, which already refuses an application built for other protocols | as it is |
| What is installed, and from which version | `/Home/Packages/<name>`: the manifest it was installed from, kept - which is also the list of files Remove takes | **new**: the build string `install` keeps now becomes the manifest |
| **Making packages and manifests** from each application's header, with sums | `tools/packages.py`, on the Mac | **grows** |
| Serving them | `tools/packages_serve.py`; later any machine | as it is |
| Asking before a package that needs powers is installed | the consent step the roadmap wants for launchers, shared with Automations | **new**, later |

**C or Lua.** Lua: a package is installed a few times a day at most, and its
byte work - unzipping, hashing - is C in the kits already.

## The busiest path

**Installing a package**: one request for the index (small), one for the
zip, written to `/Temporary`, unpacked beside the installed folder on the
same disk, every file's sum held to its manifest, then the old folder
renamed away, the new one in, the old one removed. On the M700, Doom - 10 MB
- took 1.2 seconds without the sums; SHA-256 in C at a few hundred megabytes
a second adds a fraction of that.

## Steps

1. **P1 - manifests and sums**: `tools/packages.py` writes `package.txt`
   into each zip from the header, with each file's size and SHA-256, the
   version and the protocol stamp, and builds the index from them;
   `-- kosmos: depends`. `install` refuses a sum that is wrong and a package
   for other protocols.
2. **P2 - `packages.lua` and `pkg`**: the library, with dependencies
   resolved in order, Remove by the kept manifest, Update; `pkg` and
   `install` over it; `x86-install` grown to dependencies, removal and
   refusals.
   **P1 done in 0.11.177, P2 in 0.11.178** (`testing.md` 18.572, 18.573).
3. **P3 - the window**, as drawn: it reads the index in a coroutine on its
   own clock, and runs `pkg --report` in a process of its own for every
   action, following the report - nothing in the window waits on the server.
   **Done in 0.11.179** (`testing.md` 18.574).
4. **P4 - powers asked for**, with Automations' consent step.
5. **P5 - installed applications kept current at boot** (Diego, 22
   November 2026, after Quake III would not start on the M700 because its
   copy was 0.12.66's, made for other protocols: "yes, make it update apps
   automatically at boot"). A boot whose command line names a package server
   (`opt/kosmos/packages`, as every network boot does) already runs
   `systemapps` at login: the window manager starts it, as it does `maild`,
   and it waits for an address and brings Kosmos's own packages - JetSki,
   WebKit - when the copy on the disk is another build's. **P5 is that, for
   every installed package**, not a second program beside it: each package
   `/Home/Packages` says is installed, of any kind, is installed again from
   the server when its files differ from the ones offered. Then it says
   what it brought in a notification. **"Differs" is the files, not the version**: a manifest
   carries `sums`, one hash over every file's SHA-256, and two builds whose
   files are the same are the same package. A new revision that changed
   nothing in Doom does not fetch Doom again. An application built for other
   protocols always differs, because its image carries the stamp. Nothing
   waits for it: the desktop comes up as before, and an application opened
   while it is being replaced runs the old copy or the new, never half of
   one (the swap is by renames, above). Held by `x86-install`: a package
   installed for other protocols, a boot naming the server, and the new one
   in place without anybody typing; the control is a boot that names none.

Only x86-64 runs the tests for now (`CLAUDE.md`).

## The stamp - held, to discuss

**Held on 22 November 2026** (Diego: "not sure, lets discuss this later and
hold any changes for now"). Nothing below is to be built until then.

**What it is now** (`tools/protostamp.py`). An installed application carries
its own copy of the runtime, so it speaks the message layouts of the build
that linked it. The stamp is one hash over the text of every protocol header
(`user/include/*proto.h`, `*ring.h`) and `kernel/syscall.h`. A program
whose stamp is not the system's is refused before it runs anything. That is
what stopped Doom's crash of 4 October, deep in the UI kit with no reason
given.

**What it costs.** It is far stricter than the danger. A comment changed, a
header added that the application never uses (0.12.80's `usbaudioring.h`),
or a new operation number in a server it does use: each refuses every
installed application, though none of them changes what Quake III sends or
receives.

**The options:**

- **(a) A stamp of the layouts, not the text** *(recommended)*. Computed by
  the compiler from what the bytes are: every struct's size and every
  field's offset, and every operation's number. A comment never changes it.
  A new header, a new struct or a new operation adds to it without
  changing what was there. So the application records the layout of each
  thing, and the system refuses only when something the two of them share
  is laid out differently. Automatic, with no discipline asked of anybody,
  and nearly every refusal so far would not have happened.
- **(b) A version for each protocol, bumped by hand** when a change breaks
  it, and servers that keep answering the old shape for a while: BeOS's and
  Haiku's binary compatibility. Applications survive real changes too, but
  every change asks somebody to decide whether it breaks and to keep the
  old shape working. Worth it when there are applications nobody can
  rebuild, and not before.
- **(c) The runtime from the system, not the application**: linking it at
  load time, so an application carries only its own code. Then nothing
  installed goes stale when the system changes. But Kosmos links
  everything statically by design, and this is the largest change of the
  three.

With P5, a machine that boots from the Mac rebuilds nothing by hand anyway.
(a) is what makes a stick, or an application from somebody else, keep
working across the revisions that change nothing it uses.
