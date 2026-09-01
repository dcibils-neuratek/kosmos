# The path pixels take

`design.md` §7.4 decided that the default model is **drawing commands**: the app sends `{op="rect", ...}` and the app server rasterizes. That covers buttons, lists, text and windows, which is 95% of what the system draws.

This document covers the other 5%: **apps that produce pixels**. Paint, the 3D demo, Doom, an image viewer, a video player. Commands are no use there and you have to drop down to shared memory.

It is a different path and it has to be designed separately. What follows are the six decisions that define it.

---

## 19.1 Pixels do not live in Lua tables

**This is the most important decision in the document and the most expensive one to discover late.**

A 1920×1080 image is 2 million pixels. As a Lua table, each entry is a 16-byte `TValue` plus array overhead: well over 32MB for an image that occupies 8MB as bytes. And worse than the memory: **the GC has to walk all 2 million slots on every cycle.** The pause goes to hundreds of milliseconds and the system falls apart.

No optimization saves that. It is structural.

The rule is:

> **Lua tables carry intent, never pixels.**

A `surface` is a Lua **userdata**: an opaque object holding a pointer to a flat block of bytes that the GC does not traverse, only frees. Lua has a handle and methods; the bytes belong to C.

```lua
local s = gfx.surface{ w = 800, h = 600 }
s:fill(0, 0, 800, 600, 0xff202020)   -- the pixel loop runs in C
local px = s:get(10, 10)             -- a single pixel may cross
```

This also preserves the rest of the thesis rather than breaking it: the `commit` the app sends the server is still a small Lua table, and **the surface handle travels inside that table as a capability, not as data**. The protocol is still tables.

---

## 19.2 Lua decides what to do, C does it per pixel

A Lua loop costs between 20 and 50ns per iteration. The arithmetic defines where the boundary sits:

| Operation | Pixels | In Lua |
|---|---|---|
| A 100×100 brush dab | 10,000 | ~0.4ms, acceptable |
| A line | ~1,000 | irrelevant |
| A full-screen filter | 2,000,000 | 40 to 100ms, unworkable |
| A Doom frame | 64,000 | ~3ms in VM overhead alone |

So: **the set of C primitives has to be small and composable, and all the logic stays in Lua.**

The minimum set, and it should not grow much beyond this:

```
s:fill(x, y, w, h, color)              solid fill
s:blit(src, sx, sy, w, h, dx, dy)      rectangle to rectangle copy
s:blend(src, ..., alpha)               the same with alpha
s:span(x, y, len, color)               one horizontal row
s:get(x,y) / s:set(x,y,c)              a single pixel, for one-off cases
s:map(fn)                              apply a Lua function to every pixel
```

`map` is the escape hatch: it allows any filter without adding primitives, and it is slow on purpose. If something using `map` needs to be fast, that is the signal it deserves its own C primitive, and only then does it get added.

The 3D demo's triangle rasterizer and Doom's blitter go in C for the same reason. Each is one more primitive, not an exception to the model.

---

## 19.3 Pitch and format travel with the handle

`design.md` §7.5 already warns that the pitch is almost never `width × 4` and that the Pi delivers BGRA. The consequence for this document:

> **No line of Lua may compute a pixel offset.**

Anything doing `y * width + x` is a latent bug that works under QEMU and produces the classic skewed image on the Pi. It is the kind of bug you hunt for an entire day.

So the userdata holds `w`, `h`, `pitch`, `fmt` and `bytes_per_pixel`, and address arithmetic happens **only inside the C primitives**. Color in Lua is always logical 0xAARRGGBB and conversion to the device format happens in C, once, in the final blit.

Practical corollary: **app surfaces never carry the framebuffer's format.** They are always the canonical format, with pitch aligned to a 64-byte cache line. Only the app server's backbuffer knows the device's real format. A new target changes exactly one file.

**And a pitch bug is invisible from inside the process.** This is worth writing down because it was discovered by trying it. If `row_of` in `gfx.c` steps by `width * 4` instead of by the pitch, every read and every write agrees with every other one: the surface simply has an unused gap at the end of each row, and nothing inside the guest can tell. The whole suite passed, 103 of 103, with the bug in place.

The only observer who disagrees is on the other side of the framebuffer, where the stride is 4160 and a row written 4096 bytes along lands sixteen pixels to the left. So the test for this rule cannot live in the guest at all — it is `make screenshot`, which drives the shell into drawing vertical bars through `gfx.screen()` and then asks QEMU whether they are still vertical. A rule that can only be checked from outside needs a check that lives outside.

---

## 19.4 Presentation: double buffering and an explicit commit

The system has no locks (`design.md` §6). If the app writes the surface while the compositor reads it, there is tearing and no mechanism to prevent it.

The solution that does not reintroduce locks:

**Every shared surface has two buffers and an index of which one is live.** The app always draws into the one not being displayed. When it finishes:

```lua
win:commit{ surface = s, damage = { x=10, y=20, w=100, h=80 } }
```

The `commit` is an ordinary IPC message with a small table. The server swaps the index and from that point reads the other one. The app gets the old buffer back for the next frame.

Two properties come out of this for free:

- **Damage is mandatory.** Without `damage` the server has to blit the whole surface. Making it a field of the commit rather than a separate call makes it hard to forget.
- **If the app does not commit, the server composes with the previous frame and moves on.** A stuck app never freezes the compositor, which is the BeOS property the system is chasing.

The cost is memory: a double-buffered full-screen surface is 16MB. Acceptable, and only for apps that ask for a surface.

---

## 19.5 Cache coherency before the blit

This is the gap that appears in no other document and the one that produces bugs that look random.

The surface lives in cached RAM. The framebuffer is mapped uncached. Between the two there are two moments where the cache lies:

**Before the server reads a surface the app wrote.** On a single core with a unified cache, nothing is needed. As soon as there is SMP (M6 onward, with the code already SMP-ready), the app can be on another core and its writes can live in that core's L1. That requires a `DC CVAC` over the damaged range on the app's side before the commit, or mapping the shared memory as **inner shareable** and letting hardware coherency handle it.

Inner shareable it is. A manual `DC` over megabytes per frame costs more than bus coherency, and it is a source of forgotten bugs.

**Before the display controller reads the backbuffer.** If on some target the framebuffer ends up mapped cached, a cache clean by range is needed before the flip. With an uncached framebuffer, which is the normal case, nothing is needed because the blit's writes do not go through the cache.

Operating rule: **the memory attributes of a shared surface are decided when it is created, in the kernel, and are not touched afterwards.** There is no Lua API for invalidating the cache. If one is needed, something was designed wrong.

---

## 19.6 The GC does not see surface memory

A surface `userdata` is a small object: a pointer and a few fields. Lua sees 40 bytes. Behind it there are 8MB.

Consequence: **the GC feels no memory pressure and does not collect.** The process runs out of physical pages while Lua believes the heap is empty. It is a disorienting failure mode: the system dies on OOM and `collectgarbage("count")` reports 200KB in use.

Two mechanisms, both needed:

1. **Explicit `free`.** `s:free()` releases the pages immediately. This is what normal code uses. After that the handle is invalid and using it raises a Lua error, never a segfault.
2. **A finalizer (`__gc`) as a safety net.** If nobody called `free`, the GC releases when it collects the userdata. It is the net, not the expected path.

And additionally: **when allocating a surface, tell the GC the real size** with `lua_gc(L, LUA_GCSTEP, kb)` proportional to the bytes. With that the collector runs at a rate matching real memory and `__gc` stops being theoretical.

The same applies to any userdata wrapping a large block: audio buffers, mapped files, textures.

---

## 19.7 The full path, top to bottom

Paint drawing a brush stroke:

```
1. The mouse event arrives as a Lua table          IPC, small table
2. Paint computes the stroke geometry              Lua
3. s:blend(brush, ..., alpha) per dab              C, pixel loop
4. Paint accumulates the stroke's bounding box     Lua
5. win:commit{ surface=s, damage=bbox }            IPC, small table
6. The server swaps the buffer index               C, one integer
7. The compositor composes the damage rect         C
8. A blit of the dirty rect to the framebuffer     C, uncached memory
```

Eight steps, two IPC crossings, and not one of them carries a pixel inside a Lua table. That is what has to be preserved.

**The budget:** 16.6ms per frame. The Lua steps (2 and 4) are hundreds of operations, on the order of tens of microseconds. Steps 3, 7 and 8 are the real work and they are what gets measured (`testing.md` §18.4). If step 2 or step 4 ever shows up in a profile, a pixel loop has crept into Lua.

---

## 19.8 Who frees a shared surface's pages

A region is a kernel object with a reference count, reached through a
capability like everything else. Its pages go back when the last capability
to it is dropped - not when a process that mapped it exits.

That last sentence is the whole section, and getting it wrong is the bug
this was written after. A process frees, on its way out, every page still
mapped in the window `SYS_MAP` hands out: those pages were allocated a page
at a time and the page tables are the only record of them, so walking the
range is the only way to find them. Put a shared region in that window and
the exit walk frees pages the process does not own. Two processes mapping
one region free its pages twice. `plasma` plus Control-C panicked the
machine with `pmm_free_page: double free`, and the quieter half - `SYS_UNMAP`
returning a region's pages to the allocator while the other process is still
drawing into them - had no symptom at all.

**So the rule is an address range, and not a flag on a page.** A shared
region maps at `USER_SHARE_VA`, a window nothing frees by walking. The two
pieces of code that free pages by walking a range are each bounded by the
window whose pages they own, and `as_destroy` does what it always did: tears
down the page tables and leaves the pages alone.

Two consequences worth stating, because both are easy to get backwards:

- **Both bump pointers need a ceiling.** Neither reuses an address, so a
  process that maps and unmaps for long enough walks upward until it reaches
  the next window - and then the exit walk, bounded by the pointer rather
  than by the window, frees a region's pages after all.

- **Shared pages are charged once, to whoever created the region.** Charging
  them again to everybody who maps it means a compositor and an application
  sharing one surface pay for it twice, and the second one to ask is refused
  memory that is already allocated.

---

## 19.9 What is left open

- **Undo in Paint.** Copying the whole surface per operation does not scale. The sensible approach is tile-based undo with copy-on-write, but it is not designed and it is not needed until M7.
- **Scaling and rotation.** Not in the primitive set. They get added when a case appears, not before.
- **Video.** A decoder needs a YUV to RGB path and possibly surfaces in a different format. Out of scope until there is a real case.

---

## 19.10 Build order

| What | Milestone |
|---|---|
| `gfx.surface` as a userdata, in-process, not shared | 6 — **done** |
| The C primitive set (`fill`, `blit`, `blend`, `span`, `get`/`set`) | 6 — **done** |
| Explicit `free`, `__gc`, and telling the GC the real size | 6 — **done** |
| Surface shared with the app server, double buffering and `commit` with damage | 7 |
| Inner shareable attributes in the kernel for shared memory | 7 |
| `map` with a Lua function | 7 |
| Triangle rasterizer, as one more primitive | 9 |
| Tile-based undo with copy-on-write | 7 |

## 19.11 What a crossing costs, and the rule that follows

`gfxbench` measures the primitives. Under QEMU, on the machine this was
written on:

| | |
|---|---|
| `fill`, whole surface | 214 Mpx/s |
| `blit`, whole surface | 48 Mpx/s |
| `span`, one row at a time | 80 Mpx/s |
| a disc drawn as a span per row, **from Lua** | **3 Mpx/s** |
| the same disc as one `gfx.disc` call | **103 Mpx/s** |

Thirty-three times, for the same pixels and the same arithmetic. The only
difference is that the loop stopped crossing the Lua/C boundary once per
row.

**A crossing costs about 9 microseconds here.** At the rate a `fill`
achieves, that buys about **two thousand pixels** - so:

- a 30-pixel span is 99% overhead;
- a 32x32 tile is roughly break-even;
- a whole-window blit is pure pixel work.

**The rule: a primitive should write about two thousand pixels or more, or
be called in a batch.** That is not "avoid C calls" - it is the number that
says when a call is worth making, and it is why `fill` takes a rectangle
rather than being called per row.

Three consequences, and two of them were already true by accident:

- **`ui.lua` batches drawing commands by size**, not by count
  (`BATCH_BYTES`). That is this same rule one layer up, at the IPC boundary
  instead of the Lua/C one.
- **Direct rendering is not the fast road.** It is the road for something
  that redraws wholesale. Pac-Man was direct *and* slow, because it was
  making four hundred small calls a frame; making the transport free did
  nothing about the crossings in front of it.
- **Doom is the ideal shape.** It renders a frame inside its own C loop and
  hands over one buffer: one crossing per frame, not one per shape. 320x200
  at 35 fps needs 2 Mpx/s against the 48 measured above, and the headroom is
  not the interesting part - the shape is.

The number is QEMU's, not hardware's, and `testing.md` 18.3 applies: this
detects a regression and predicts nothing about a Pi. The *ratio* between
the rows is what transfers.

---

---

## 19.12 Outline fonts, and why there are three of them

The bitmap font is still here and still the default. It is exact, it costs
nothing, every display test was written against it, and a machine that
cannot parse a font file still has text. What outlines add is a size that
is chosen rather than fixed, and edges that are not stair-shaped.

`stb_truetype` does the parsing and the rasterizing, vendored unmodified in
`runtime/upstream/stb/` under the rule `lua/upstream/` follows. It handles
TrueType and CFF, which is why `.otf` works as well as `.ttf`.

Its own header says, in capitals, that it offers no security guarantee and
should not be pointed at untrusted font files. That is worth stating
plainly rather than filing away: it is a parser of an attacker-shaped
format. What contains it today is where it runs - EL0, in whichever process
is drawing, with no authority beyond that process's own capabilities, so
the worst case is one application dying. That is the *whole* of the
mitigation, and it stops being sufficient the moment fonts arrive from
somewhere other than the build. See §19.13.

### Three faces, not one

Text appears in three places that want different things:

| role | where | what it needs |
|---|---|---|
| `ui` | titlebars, buttons, menus, the Deskbar | to match how the desk looks |
| `text` | documents, labels, anything to read | proportional, comfortable |
| `mono` | the Terminal, the editor, hex dumps | **fixed width**, or columns stop lining up |

One setting for all three could only ever be right for one of them. The
terminal's face has to be fixed-width whatever the other two are, and that
is not a preference - a proportional font in a terminal breaks alignment,
which is what a terminal is for.

The three are independent, and the check that says so is the one worth
keeping: measure the same number of characters in two different shapes.

```
ui    designer 20      iii = 21   WWW = 75
text  roboto 18        iii =  9   WWW = 42
mono  jetbrainsmono 14 iii = 18   WWW = 18
```

The proportional faces disagree with themselves; the monospace one does
not. A single-face design cannot produce that table.

### What is embedded, and what it costs

Fonts live in `assets/fonts/` as the author shipped them, with the licence
beside them - and a face whose licence is not beside it does not get
committed, which is the same rule `lua/upstream/` follows. They are
converted at build time - the same arrangement as the
BDF. Adding one is dropping a file in that directory.

That is not free, and the number is the point: measured with five faces in
the directory, the user image went from 577 KB to 1,433 KB. Every process gets a private copy of that
image, so at fourteen processes it is about **19.6 MB of RAM spent on the
same bytes fourteen times**. The fonts did not create that copy - Lua and
the libc were always being copied too - they made it expensive enough to
notice.

Two separate things follow, and they are worth not confusing:

- The **duplication** is a property of the image, not of fonts. Mapping the
  image's read-only pages shared instead of copying them fixes it for
  fonts, Lua, the libc and every program at once.
- The **parsing** is a property of fonts, and is what §19.13 is about.

Wallpapers, therefore, do not go in `assets/`. A photograph embedded in the
image is that photograph copied into every process on the machine. Pictures
come from the disk.

---

## 19.13 The font server that is not built yet

Not built, and the reasoning is recorded here so it is not re-derived.

The obvious argument for one - "so the bytes are not duplicated" - is the
weak argument, and on its own it is wrong: the bytes are in the image, and
a server that rasterizes elsewhere does not take them out of the image.
Shared read-only image pages are what fixes duplication.

The real argument is the parser. `stb_truetype` currently runs in every
process that draws text. Once a person can install a font, that is an
untrusted file being parsed in fourteen address spaces. A font server makes
it one, in a process that can die and be restarted, costing text rather
than the machine. That is a microkernel argument and it is the one to build
on.

The shape it has to take is decided by §19.11. Glyph-per-IPC is not
allowed: at ~9µs a crossing, a sixty-character line would cost 540µs, worse
than what exists now. Instead the server owns the faces, rasterizes an
atlas per (face, size), and hands the atlas out as a **shared memory
region** - the machinery the shared surfaces already use. One IPC per
(face, size), none per glyph, and one copy of each atlas on the machine.

It gets built when fonts come from the disk, because that is when the
security argument stops being hypothetical.
