# A real browser for Kosmos - the research

*Gathered 14 October 2026. **Decided the same day**, after reading it:
native applications for the work, a light browser of Kosmos's own for
reading the modern web but not for Gmail- or Docs-sized applications, and
Linux-on-Kosmos - Chromium and native Linux applications in an Alpine guest
shown in a Kosmos window - for the rest (`roadmap.md`, "The web, decided").
What follows is the research that decision was made from.* Diego, that day: "I want to start using
kosmos as a daily os", "The major problem now ia the browser", "I do Gmail,
Google Docs, Gemini, Claude, YouTube, and a lot of browsing", and "I need
to think about it with real Data pros and cons so do the research but I
will decide with what you found later". Its turn in the queue is after
everything queued that day (`roadmap.md`).

Four research passes went into this - what Kosmos offers a big C++
program today, Chromium and Gecko, WebKit and the TV engines, Ladybird and
Servo - each with sources. Claims marked *(unverified)* the researchers
could not confirm from a primary source.

## The bar

Five sites, and four of them are among the heaviest web applications that
exist:

| Site | What it demands |
|---|---|
| Gmail | a large JavaScript application; Kosmos Mail already does Gmail natively |
| Google Docs | a document drawn on a canvas; a fast, complete JavaScript; Google supports only the two newest Chrome and Firefox |
| Google Sheets | now computes in WebAssembly with garbage collection (WasmGC) |
| Gemini, Claude | long-lived streaming single-page applications |
| YouTube | Media Source Extensions (MSE) feeding H.264, VP9 or AV1 as it streams |

So JavaScript bolted onto today's NetSurf engine never opens these, and
neither does an old engine (Google refuses old browsers).

## What Kosmos has, and what every engine needs first

**No C++ program has ever run on Kosmos.** Every engine below is C++ (and
two of them also Rust), and every one assumes a POSIX-shaped process. The
gaps, found in the tree:

| Needed | Kosmos today |
|---|---|
| A C++ compiler and runtime | g++ is installed and unused; **no libstdc++ for x86-64**; the linker script discards `.eh_frame` (no exceptions); no static constructors run; no `thread_local` |
| Room for the program | **a 40 MB image ceiling**; engines are 100-250 MB of code; static linking only |
| Threads that can share | threads exist in C (`kosmos_thread_start`), but **no futex, mutex, condition variable or pthreads**; `malloc` is not thread-safe; stacks fixed at 256 KB |
| Memory as engines use it | no reserve-without-commit, no `mprotect`, no fixed-address maps, no fault delivered to the process; addresses never reused in a 4 GB window |
| A JIT | **no way to make written memory executable** (on purpose, `luajit.md`) - so every engine starts interpreter-only |
| Files and sockets from C | `fopen` reads only bytes a process handed it; no writing, no directories; TCP and DNS exist (IPv4 only), TLS 1.2 (BearSSL) |
| Text | `stb_truetype`, Latin fonts; **no FreeType, HarfBuzz, ICU, bidi or input methods** |
| Media | FFmpeg with H.264 and AAC only; **no VP9, AV1, Opus** |
| GPU | none on the M700; TinyGL in software; virgl paused |
| The machine | the M700: 8 GB, i7-6700, 4 cores / 8 threads, HD 530 not driven |

**Every engine's first step is the same: a POSIX layer inside the
browser's own process** - musl or FreeBSD's libc plus libc++, pthreads on
Kosmos threads, BSD sockets over the network stack, an `mmap` with reserve
and protect, a clang cross-compiler for Kosmos - which *Compatibility inside
a process yes* (`CLAUDE.md`) allows. Plus three kernel additions: a futex,
reserve-then-commit memory, and suspending a thread to read its registers
(JavaScript garbage collectors do this). **Estimated 6-12 months of the
work, for any engine**; it pays off beyond the browser (Music, Remote
Screen, any large C or C++ port).

## The options

### A. Chromium (Blink, V8, Skia)

| | |
|---|---|
| Runs your five sites | **Yes** - it is what Google tests against |
| Size | about 36 M lines; a clean build about an hour on 16 cores, 8-16 GB RAM, 100 GB disk |
| Porting seam | **Ozone**: a documented interface for a new windowing platform; Fuchsia, ChromeOS, TVs and cars use it |
| Without a GPU | yes: Skia draws on the CPU, and there is a software compositor |
| Without a JIT | V8 `--jitless`: about 40% slower on Speedometer, 80% on the Web Tooling Benchmark, but only about 6% on a YouTube TV app; WebAssembly runs interpreted (DrumBrake) |
| One process | exists, documented as "not safe or robust", kept for testing |
| Closest precedent | **Genode** (a microkernel with its own userland): Chromium through QtWebEngine since August 2020, about one developer, on a FreeBSD-derived libc and Qt already there; **Gmail, Docs, Maps and YouTube work**, "perfectly usable" on a PC |
| Other precedents | Fuchsia: about five years with a Google team; LG webOS and Samsung Tizen TVs: Chromium 120-132, on Linux with GLES and hardware video; OpenBSD and FreeBSD: one maintainer rebasing every six weeks |
| Estimate | the POSIX layer, then **1-2 years to "Gmail loads"**; first milestone V8's `d8` shell `--jitless`, then `content_shell` headless in one process |

**For:** the only engine certain to run all five; the cleanest seam; the
strongest precedent of exactly Kosmos's shape (Genode). **Against:** the
largest; heavy to build; Google's six-week release cycle means permanent
rebasing; software rendering at 1080p will be slow for Docs and YouTube.

### B. Gecko (Firefox)

| | |
|---|---|
| Runs your five sites | yes, when current |
| Size | about 25 M lines, 4.3 M of them Rust; a build needs 4-8 GB RAM |
| Needs beyond Chromium's | **Rust's standard library for Kosmos**; many processes by default (single-process only behind an environment variable); WebRender wants GL or its software fallback |
| BeOS precedent | **BeZilla** (2002-2007, Firefox up to 2.0.0.x): a native `widget/beos`, a BeOS gfx backend, NSPR ported, several people; **it stopped in January 2007 when Gecko forced every platform onto Cairo**, and the code was removed |
| Haiku precedent | **KENZ, a former BeZilla developer**, from May 2023: not a native port but Linux's path - Haiku's Wayland compatibility layer, GTK3, Mesa's software EGL, Rust already ported; Firefox 128 in August 2024, Firefox 155 on HaikuDepot on 1 September 2026; **about three years, one main developer, on an OS far more POSIX than Kosmos** |
| An old Firefox | builds more easily and **fails the goal**: Google supports only the newest two versions; YouTube needs MSE; Sheets needs WasmGC |

**For:** the BeOS lineage; Haiku shows it can be done. **Against:**
everything Chromium needs plus a Rust port, no Ozone-like seam, and
BeZilla's end shows how fast Gecko's internals move under a port.

### C. WebKit (Safari's engine)

| | |
|---|---|
| Runs your five sites | yes in Safari; Google supports Safari "works well"; WebKitGTK gets nags (it should present itself as Safari); YouTube depends on MSE and codecs working |
| Size | about 3.6 M lines in `Source/`; about an hour to build; 50 GB disk |
| The embedded port (WPE) | made for TVs and devices, but **requires GLib, libsoup, EGL and GStreamer** - no switch to avoid GLib |
| The shape to copy | **the Windows and PlayStation ports**, upstream and GLib-free: curl for the network, Skia on the CPU, static JavaScriptCore; PlayStation ships with **no JIT** |
| Without a JIT | Apple's Lockdown Mode turns it off: about 65% slower on Speedometer 2 |
| Processes | WebKit2 is always several processes; the single-process WebKitLegacy is Apple-only now - so Kosmos needs WebKit's IPC on endpoints and regions, which fits *control by message, data by shared memory* |
| Precedents | **Haiku**: one or two people for about fifteen years, still on Legacy, its WebKit2 crashing; **MorphOS Wayfarer**: one developer, about four years, then **frozen at 2.46** when WebKit dropped Legacy and Cairo; Redox: WebKitGTK through its POSIX library and the GTK stack, November 2025 |
| Estimate | JavaScriptCore alone with no JIT, 1-3 months; a page drawn, +3-6; the multi-process MiniBrowser, +4-8; Gmail, Docs, Claude and Gemini usable, +3-6; YouTube through MSE on the FFmpeg kit, +2-4: **about 1-2 years** after the POSIX layer |

**For:** a third the size of Chromium; a no-JIT, GLib-free port already
upstream to copy; an engine Google supports. **Against:** single
developers have only survived on WebKitLegacy, which is gone; the
compositor still wants EGL; upstream churn froze Wayfarer.

### D. Ladybird

| | |
|---|---|
| Runs your five sites | **not yet**: YouTube with MSE up to 4K (2026), Gmail works, Sheets runs with problems, ChatGPT's page fixed; **nothing published on Google Docs, Gemini or Claude** |
| State | pre-alpha; alpha planned for 2026, beta about 2027, release about 2028 *(secondary sources)*; its own team calls it unfinished; a September 2026 security review left "a considerable backlog" |
| Needs | C++23 **and Rust (mandatory since April 2026)**, 47 libraries (Skia, ICU, FFmpeg, curl, OpenSSL, Qt...), many processes with no single-process mode; JavaScript with no JIT at all (an assembly interpreter for x86-64 and ARM64) |
| Project | **no public contributions since June 2026** - a Kosmos port would be a permanent fork of a codebase merging 350-560 changes a month |
| Precedent | a Haiku port in 2023-24 built, opened a window, and crashed |
| Estimate | 12-24 months or more, and it would not open Docs, Gemini or Claude today |

**For:** born on a hobby OS (SerenityOS); no JIT needed; improving fast.
**Against:** does not meet the bar yet; Rust plus 47 libraries; a fork for
ever.

### E. Servo

| | |
|---|---|
| Runs your five sites | **no**: Gmail only behind an experimental flag (2025), no YouTube (MSE funded from June 2026), Docs open as an issue since 2016 |
| Needs | Rust's standard library, SpiderMonkey (C++), **OpenGL ES 3** (WebRender; software only through Mesa), GStreamer |
| Precedent | Redox, with Rust, a POSIX library and Mesa already there: about 1.5 years to "barely runs" (October 2025) |
| For / against | a real single-process path and an embedding API; behind on every site that matters, and needs Mesa |

### F. Cobalt - YouTube only

Google's engine for YouTube on TVs, built to be ported behind one layer
(Starboard). **Runs the YouTube TV app and nothing else** - no Gmail,
Docs, Gemini or Claude. Single process, no JIT. Needs OpenGL ES 2
(software rendering was removed in 2021). Cobalt 27 has become a Chromium
fork; Cobalt 25 is the small one. About 3-6 months once a software GLES
exists *(estimate)*. A possible quick YouTube beside a full engine.

### G. A real browser in a Linux guest

Kosmos's hypervisor already boots Alpine Linux on the M700. A stock
Firefox or Chromium in that guest runs all five sites, as Linux does. What
is missing is the guest's screen and input in a Kosmos window - Sandboxes'
step V5 (`docs/sandboxes.md`), not built. **For:** the shortest road to all
five sites, and they stay current with Linux's browsers. **Against:** a
Linux inside Kosmos rather than Kosmos's own browser; graphics through an
emulated card, no GPU; the memory of a whole Linux. Its cost is V5, which
was going to be built anyway.

### H. Remote Screen (already agreed)

A browser on the Mac shown in a Kosmos window - every site, the week it
is built, while the Mac is on. The meantime whatever is chosen.

## Side by side

| | Five sites | Size | First milestone | To "usable" after the POSIX layer | Precedent that matters |
|---|---|---|---|---|---|
| Chromium | yes | 36 M lines | `d8 --jitless` | 1-2 years | Genode, ~1 developer, works |
| Gecko | yes | 25 M (+Rust) | SpiderMonkey shell | 2-3 years | Haiku, ~3 years, 1 developer |
| WebKit | yes | 3.6 M | `jsc` no JIT, 1-3 months | 1-2 years | Wayfarer, ~4 years, froze |
| Ladybird | not yet | large (+Rust, 47 libs) | `test-js` | 1-2+ years, still short | Haiku, crashed |
| Servo | no | large (+Rust, Mesa) | mozjs shell | 1.5+ years, still short | Redox, barely runs |
| Cobalt | YouTube only | small | Starboard + software GLES | 3-6 months | many TVs |
| Linux guest | yes | (Linux's) | Sandboxes V5 | V5 | Kosmos boots Alpine today |
| Remote Screen | yes | small | agreed | weeks | - |

## Questions for Diego, when its turn comes

1. Kosmos's **own** engine (A, B, C - Chromium most certain, WebKit
   smallest), or a real browser in a **Linux guest** (G) first and an own
   engine later, or both?
2. If an own engine: Chromium (certain to open the sites, largest) or
   WebKit (a third the size, a no-JIT port to copy)?
3. The POSIX layer is common to every own-engine choice and helps
   beyond the browser - start it before the choice is made?
