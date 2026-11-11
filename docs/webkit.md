<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# JetSki - WebKit on Kosmos, a browser as current as Safari

Diego, 11 November 2026: "I need a new web browser that is current and
usable like safari or chrome", "We need to port either one to kosmos and
have a proper browser". Asked which, Chromium or WebKit: **WebKit**, and
**design then build**. `docs/browser-engine.md` is the research it was
chosen from; this is the design, written before any of it is built
(`CLAUDE.md`, *How to work here*). **Its name is JetSki** - Diego, 11
November: "the browser should be called \"JetSki\" because us super fast!".
Its window is `docs/webkit.html`, its
diagram `docs/webkit-architecture.png`, drawn from
`docs/webkit-architecture.html`.

**What it replaces.** On 14 October the web was decided as a light browser
of Kosmos's own, for reading, and Linux on Kosmos for the giant web
applications. The light browser is replaced by this. Linux on Kosmos stays,
for Linux's applications, and Epiphany there is the browser to use until
this one opens the five sites.

## What a person can do with it

The bar is the one `browser-engine.md` set, from what Diego uses: **Gmail,
Google Docs and Sheets, Gemini, Claude, YouTube, and a lot of browsing.**

- **Open any site** by its address or a search typed into one field.
- **Tabs**: opened, closed, dragged into order, reopened after closing;
  Super with a number goes to one, as the old browser's did.
- **Back, forward, reload, stop**; a page's title and icon in its tab.
- **Sign in and stay signed in**: cookies and site storage kept in
  `/Home/Settings/JetSki/`, per site, between runs; passwords offered from
  the keyring server, never stored by the browser itself.
- **Watch video**: YouTube and any `<video>`, sound through the Playback
  Kit's audio path, full screen.
- **Downloads** to `/Home/Downloads`, with their progress in a list and a
  notification when one finishes.
- **Find in page, zoom, print to PDF** (through the PDF Kit, as Write
  prints).
- **Bookmarks and history**, searched from the address field.
- **Open a link from anywhere**: Mail, Write, the cheat sheet. Each asks
  the window manager to launch `/Kosmos/Apps/browser.lua` with the address
  (`mail.lua` 2367); that one door is kept, and opens JetSki instead.
- **Copy and paste**, text and pictures, with the rest of Kosmos.
- **A page that hangs does not hang the browser**: each tab's page runs in
  a process of its own, and a crashed one shows a page saying so with
  Reload.

Not in the first version: extensions, a developer console beyond a log,
WebGL and WebGPU (no GPU driven yet), syncing with another browser.

## The shape: WebKit's own ports, and which one to follow

WebKit is built for a *port*, a set of choices in
`Source/cmake/Options<Port>.cmake`. Read on 11 November, on WebKit's `main`:

| | PlayStation | Windows | **Kosmos** |
|---|---|---|---|
| Drawing | Cairo (`USE_CAIRO ON`) | **Skia** (`USE_SKIA` on by default; WebKit carries Skia in `Source/ThirdParty/skia`) | **Skia**, on the processor |
| Network | **curl** (`USE_CURL ON`) | **curl** | **curl** |
| JavaScript JIT | **off** (`ENABLE_JIT`, `DFG`, `FTL` off) | on | **off** |
| GPU process | off | on | **off** |
| WebKitLegacy (one process) | off | off | off - it no longer exists outside Apple's |
| GLib | none | none | **none** |
| Libraries linked | object libraries, JavaScriptCore static, WebKit shared | object libraries, JavaScriptCore shared | **everything static, one image** |
| Required | ICU, FreeType, HarfBuzz, Fontconfig, PNG, JPEG, WebP, curl, OpenSSL, zlib, libxml2, SQLite, libpsl, EGL, GLES2 | ICU, HarfBuzz, PNG, JPEG, WebP, curl, OpenSSL, SQLite, libxml2 | as Windows, plus FreeType for faces; **no EGL or GLES** |

So **Kosmos takes Windows' drawing and network with PlayStation's
choices about the JIT and the GPU.** The PlayStation shows a JIT-less WebKit
ships; Windows shows Skia and curl without GLib; neither needs anything
Kosmos refuses at the system level.

**Without a JIT is not without speed.** JavaScriptCore's first tier, the
LLInt, is an interpreter written in WebKit's `offlineasm` and assembled
when WebKit is built - code like any other, mapped read and execute, never
written while it runs. So Kosmos's rule that written memory never becomes
code (`luajit.md`) holds as it is. Apple's Lockdown Mode measures what it
costs: about 65% slower on Speedometer 2. A JIT is a later decision of
Diego's, with what it would ask of Nebula in front of him.

## What it is made of

The premise (`CLAUDE.md`, *kits, servers and drivers supply*) asked of
every piece: **does another application want this?**

| Piece | Who supplies it | Language | Shared with |
|---|---|---|---|
| The window, tabs, address field, menus | **JetSki, the application**, Lua on the kit (`ui.lua`) | Lua | - |
| WebKit's UI process: pages, history, sessions | `jetski.elf`, installed in `/Home/Apps/JetSki` as Doom is | C++ | - |
| Each page's web process | the same image, spawned with a role word (`SYS_SPAWN` hands a child its parent's image) | C++ | - |
| The network process | the same image, another role | C++ | - |
| **C++ itself** - libc++, libc++abi, unwinding, static constructors, `thread_local` | **new: the C++ runtime**, in `runtime/` beside the libc | C, C++ | every C++ port after it (llama.cpp, a game) |
| **POSIX inside a process** - pthreads, `mmap`, files, sockets, time | **new: the POSIX layer**, `runtime/posix/` | C | any large port; it is *compatibility inside a process*, `design.md` §17 |
| WebKit's IPC (`IPC::Connection`) | **new**: on endpoints for messages and regions for its shared memory | C++ | - |
| Fonts: faces and shaping | **new: the Text Kit** - FreeType and HarfBuzz behind one door | C | **Write, Mail, Present** - every application drawing text in a script other than Latin |
| Unicode: breaking, bidi, collation | ICU, vendored as shipped | C++ | the Text Kit |
| Pictures: PNG, JPEG, WebP, GIF, AVIF | **new**: libpng, libjpeg-turbo and libwebp, vendored. Kosmos decodes pictures with `stb_image` today (`gfx`); WebKit's decoders want these, so **the image kits move onto them** rather than keep a second decoder of each format | C | Photos, Write, Tracker's previews |
| TLS | **OpenSSL, the system's one TLS** (decided; the TLS Kit moves onto it) | C | every program that speaks TLS |
| Video and sound | **the FFmpeg kit and the Playback Kit**, behind a `MediaPlayerPrivate` of Kosmos's own; VP9, AV1 (dav1d) and Opus added to the kit's decoders | C | Video, Music |
| Storage: cookies, site data | SQLite, vendored, in `/Home` through the namespace | C | any application that wants a database |
| Passwords | the keyring server | C | Mail, Servers |
| Downloads, notifications | `/Home/Downloads`, the notification server | - | - |

**What travels in a region and what in a message** (*Control by message,
data by shared memory*):

- A page's **pixels**: the web process paints tiles with Skia into a region
  it shares with the UI process; a message says which tiles changed. The
  UI process hands the window its surface as every application does, and
  the compositor composes it. A frame never travels as a payload.
- A **video frame**: decoded by the FFmpeg kit into a region, and painted
  as a tile - the same path as the Video application.
- **Network bodies**: the network process writes a response's bytes into a
  region shared with the web process that asked; messages carry headers and
  "N more bytes".
- **IPC messages** themselves - a click, a scroll, "load this URL",
  "these tiles changed" - are messages on endpoints. WebKit's IPC encodes
  its own messages, so a large one (a page's whole cookie jar) goes into a
  region and the message names it, as `MSG_BYTES` asks.

**What is C, C++ and Lua.** *Language split* says C is what runs on behalf
of another process and Lua what runs for a person. WebKit is a third case,
and is written down as one rather than drifted into: **C++ in one image
only, the browser's**, because the engine is C++ and rewriting it is not an
option. Nothing outside `jetski.elf` is C++, and no server becomes C++.
The window around it is Lua, as every application's is. The runtime and the
POSIX layer are C (libc++ excepted), so other ports can use them.

## The busiest path: scrolling a heavy page

A wheel turn on a long page with pictures (W - the diagram's path):

1. **W1** The window manager hands JetSki's window the wheel; the Lua
   window passes it to the UI process as a message - no Lua on the frame
   after this.
2. **W2** The UI process scrolls what it already has (WebKit's scrolling
   tree, on its own thread) - the screen moves at once, from tiles painted
   before.
3. **W3** It asks the web process for tiles newly in view.
4. **W4** The web process lays out if it must and paints those tiles with
   Skia into the shared region.
5. **W5** "These tiles changed"; the window's surface is updated and the
   compositor composes it - 42 ns a pixel, already measured.

Where the time will go, from what is known before it is measured: **Skia's
painting on the processor** at 1920x1080 (WebKit's ports without a GPU live
there), **JavaScript without a JIT** on a script-heavy page, and **layout**.
Measured from W6 with the browser's own timings and `frames`, on the M700,
never under TCG.

## What Kosmos is missing, and the steps

Each step leaves its tests, held to the gate's time.

**W1 - C++ runs on Kosmos.** libc++ and libc++abi built for Kosmos's
userland with clang (Homebrew's clang 23 is on this Mac), and LLVM's
libunwind. `user.ld` stops discarding `.eh_frame` for this image, and
`.init_array` is run before `main`. `thread_local` on the thread pointer -
`FS` on x86-64, `TPIDR_EL0` on ARM - set per thread by the kernel. **And the
heap placed after the image** (already on the roadmap): it sits at a fixed
+42 MB today, and WebKit's image will be several times that. *Test*: a C++
program in the gate that throws across frames, runs a static constructor,
and counts a `thread_local` on two threads.

**W2 - three things from Nebula**, each a kernel change and therefore
Diego's to agree in detail when it is designed:
- **a futex** - wait on a word in a process's own memory until it changes,
  wake *n*. Every lock and condition variable stands on it, and today a
  waiter sleeps for a tick (the malloc lock, 0.11.363).
- **memory reserved, then committed and protected** - JavaScriptCore and
  bmalloc reserve gigabytes of addresses and commit pages as they need
  them; and a page made read only or no access (guard pages). Never
  writable and executable at once.
- **a thread suspended and its registers read** - JavaScriptCore's
  collector scans every thread's stack and registers for pointers.

**W3 - the POSIX layer, inside the process.** pthreads (threads, mutexes,
condition variables, keys) on Kosmos threads and the futex; `mmap`,
`munmap`, `mprotect` on W2; `open`, `read`, `write`, `stat`, directories
through the process's namespace - `/Home` and `/Temporary` paths, nothing
global; BSD sockets for curl over the Network Kit; `clock_gettime`,
`getenv`, `getpid`. **No `fork`, `exec`, `signal` or `ioctl`**: where WebKit
asks for them its Kosmos files say what to do instead. *Test*: a suite of
small C programs, each a POSIX call against its expected answer.

**W4 - JavaScriptCore.** `jsc`, the shell, built for Kosmos with the LLInt
and no JIT, running a script; then test262's language tests, counted, and
the count kept as a regression. The first time anything of WebKit runs.

**W5 - the libraries.** ICU, FreeType, HarfBuzz (as the Text Kit), libpng,
libjpeg-turbo, libwebp, SQLite, libxml2 (or expat, already here, where
WebKit allows), zlib (or `miniz`, already here), libpsl, curl - each vendored as shipped under
`runtime/upstream/` with its licence, each with a host test where it has
one. Skia from WebKit's own copy.

**W6 - WebCore draws a page**, headless: a page from a file laid out and
painted into a region, the picture compared with one Safari's engine draws
on this Mac. WebKit's MiniBrowser is the reference.

**W7 - WebKit2 and the window.** `IPC::Connection` on endpoints and
regions, the process launcher on `SYS_SPAWN` with a role, the three
processes; then JetSki's window, `docs/webkit.html`, with keys, the
pointer, the wheel, the clipboard and text input.

**W8 - the sites.** Networking through curl and TLS; cookies and storage;
then the five: Gmail, Docs and Sheets, Claude and Gemini, YouTube through
Media Source Extensions on the FFmpeg kit - each a check run against the
real site on the M700, outside the gate, and a page of our own inside it.

How long, honestly: `browser-engine.md` put the layer at 6-12 months and
the engine at 1-2 years after it, for one person. Every step above is
usable on its own - W1 and W3 help every later port - and none waits on
the next to be worth having.

## Fast: what makes it so, and where the GPU comes in

Diego, agreeing the design on 11 November: "make sure is blazing fast,
optimized and renders on the gpu when needed for extra speed". So speed is
part of the design, not a later pass, and every item here is measured on
the M700 before and after (`CLAUDE.md`: QEMU numbers are not performance
numbers):

- **Built for speed.** WebKit and its libraries at `-O2` with link-time
  optimisation over the one static image; no debug assertions in the build
  a person uses. Skia, libjpeg-turbo, libwebp, dav1d and FFmpeg keep their
  hand-written SIMD, chosen at run time by CPUID - AVX2 on the M700, AVX on
  the Mac Pro 2013, NEON on ARM - so one image runs on both machines at
  each one's best.
- **Every core paints.** A page's tiles are painted by several threads at
  once, one tile each, as WebKit's Linux ports do with Skia on the
  processor - four cores on the M700 rather than one. Scrolling never waits
  for painting (the busiest path, above): the UI process moves tiles it
  already has.
- **Nothing copied that can be shared.** Tiles are painted straight into
  the region the window's surface is composed from; a video frame is
  decoded into a region and drawn from there; a response body is written
  once, into the region the web process reads.
- **JavaScript.** The LLInt with no JIT, as agreed. If the measurements
  say JavaScript is where a site's time goes, a JIT is the next
  conversation, with what it would ask of Nebula: memory written and then
  made executable, never both at once.
- **The GPU, where it pays.** Kosmos drives the M700's GPU through its own
  kernels (the Compute Kit, `kc`), and has no OpenGL or Vulkan driver yet.
  So, in order:
  1. **compositing on the GPU**: WebKit's layers - a scrolled frame, a
     transformed or translucent element, a video - blended and scaled into
     the window's surface by Compute Kit kernels rather than the processor,
     whenever a frame has more than one layer moving;
  2. **pictures scaled and decoded** there - large images drawn smaller,
     JPEG's colour conversion - where measuring shows the processor busy
     with them;
  3. **video decoded in hardware**, when the M700's video engine is driven
     (`m700-gpu.md`), through the FFmpeg kit's door, so the browser gains it
     without changing;
  4. **Skia's own GPU backend**, and WebGL, once Kosmos has a Vulkan or
     OpenGL driver (`roadmap.md` 4h) - at that point painting itself moves
     to the GPU.
  Each falls back to the processor where there is no GPU - QEMU, the Mac
  Pro's FirePros - and is chosen by measuring, not assumed.

## Decided by Diego, 11 November 2026

1. **The design agreed**: "yes go ahead".
2. **Tabs in the band**, as Safari and Chrome put them: the band holds the
   tabs, the lights and a menu button; the menus are that button's, and the
   screen's menu bar under a desktop that has one (Pewter).
3. **OpenSSL as the system's one TLS.** WebKit and curl use it; the TLS
   Kit moves onto it, so Kosmos keeps one TLS library rather than two.
   Its own step, after W5: the TLS Kit's callers held to the same tests
   before and after.
4. **Fast, and on the GPU when it pays** (*Fast*, above).
5. **Its name is JetSki**, "because [it] is super fast".

## Still open

1. **Disk.** WebKit's build wants tens of GB and this Mac has under 10
   free; W1-W4 fit, W5 onward does not. Space made here, or the build on
   an external drive - before W5.
2. **The ARM build.** Tests are x86-64 only for now; WebKit builds for both
   from the first step, and is tested on ARM when ARM's tests resume.
