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

## 19.8 What is left open

- **Undo in Paint.** Copying the whole surface per operation does not scale. The sensible approach is tile-based undo with copy-on-write, but it is not designed and it is not needed until M7.
- **Scaling and rotation.** Not in the primitive set. They get added when a case appears, not before.
- **Font output format in a surface.** `stb_truetype` rasterizes to 8bpp alpha; whether that is another surface type or a separate case is undecided.
- **Video.** A decoder needs a YUV to RGB path and possibly surfaces in a different format. Out of scope until there is a real case.

---

## 19.9 Build order

| What | Milestone |
|---|---|
| `gfx.surface` as a userdata, in-process, not shared | 6 |
| The C primitive set (`fill`, `blit`, `blend`, `span`, `get`/`set`) | 6 |
| Explicit `free`, `__gc`, and telling the GC the real size | 6 |
| Surface shared with the app server, double buffering and `commit` with damage | 7 |
| Inner shareable attributes in the kernel for shared memory | 7 |
| `map` with a Lua function | 7 |
| Triangle rasterizer, as one more primitive | 9 |
| Tile-based undo with copy-on-write | 7 |
