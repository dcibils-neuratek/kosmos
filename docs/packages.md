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
3. **P3 - the window**, as drawn.
4. **P4 - powers asked for**, with Automations' consent step.

Only x86-64 runs the tests for now (`CLAUDE.md`).
