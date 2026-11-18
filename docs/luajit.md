# LuaJIT, studied

`roadmap.md` 6zo. Diego, 27 September 2026: "I want to study the
possibility of using luajit in kosmos to improve performance for Lua code".
LuaJIT was rejected in August for Lua 5.4 (`design.md` §5, the README's
decision log). This page is what was measured the night it was asked, so
that the decision - kept or changed - is made on numbers.

**The short answer: it would be fast, and it would cost the language
Kosmos is written in.** LuaJIT's compiler runs Kosmos's own libraries two
to four times faster on this Mac. It is Lua 5.1, and more than half of
Kosmos's Lua files use something 5.1 does not have - integer arithmetic
above all, which the protocols, the filesystem's dates and the counter all
depend on. And its compiler writes machine code into memory as it runs,
which Kosmos refuses by design.

---

## What was measured

**The Lua.** Kosmos's own, vendored Lua left out: `user/init`, `user/bin`
and `user/lib`, **194 files and 75,285 lines**.

**The speed.** Four of Kosmos's libraries that LuaJIT can run unchanged -
chosen because they use nothing LuaJIT lacks, which is itself a selection:
the IDE's lexer over every line of `wm.lua`, Markdown's parse and wrap of
`roadmap.md`, the editor's buffer taking 20,000 keystrokes and 200 undos,
and the planet simulation stepped 20,000 times. Each run once to warm up,
then timed as the mean of several with `os.clock`.

On this Mac (Apple Silicon, arm64), against Lua 5.4.8 built at `-O2` as
the guest's is - the build tree's host copy is `-O1`, which would have
flattered LuaJIT by a few per cent:

| Workload | Lua 5.4 | LuaJIT, interpreter | LuaJIT, compiled |
|---|---:|---:|---:|
| Lexer, `wm.lua` | 5.04 ms | 4.54 ms (1.11x) | 2.18 ms (2.3x) |
| Markdown, `roadmap.md` | 23.5 ms | 20.2 ms (1.16x) | 20.1 ms (1.17x) |
| Editor buffer, 20,000 keys | 35.6 ms | 23.6 ms (1.51x) | 14.5 ms (2.45x) |
| Planets, 20,000 steps | 130 ms | 95.6 ms (1.36x) | 30.0 ms (4.3x) |

LuaJIT 2.1 from its repository (`c6ffc14`, 8 September 2026), built on the
Mac. Mac numbers, not the Pi 5's - `CLAUDE.md` is clear that neither QEMU
nor this Mac is the target - but the ratios between two interpreters on the
same processor say what the choice is worth.

**Where the gain is**: the compiler, on numeric loops (the planets) and on
table-and-string work done in tight loops (the lexer, the buffer). The
interpreter on its own is 1.1 to 1.5 times - LuaJIT's hand-written
assembly against Lua's C, and not enough to pay for anything below.
Markdown, which is string building and pattern matching, is the same speed
either way: the time is in the string library, which is C in both.

## What LuaJIT does not have

It is Lua 5.1, with some of 5.2. Counted in Kosmos's own code, strings and
comments excluded:

| What | Uses | Files |
|---|---:|---:|
| Integer division `//` | 685 | 84 |
| Bitwise `&`, `\|` | 150 | 24 |
| Shifts `<<`, `>>` | 89 | 19 |
| `string.pack`, `unpack`, `packsize` | 94 | 11 |
| `math.tointeger`, `math.type` | 29 | 11 |
| `utf8` | 12 | 3 |

**96 of the 194 files use at least one.** Every one of those could be
rewritten - LuaJIT has a `bit` library and FFI - and a rewrite would be a
week of mechanical work and a new class of bug in each.

**The one that is not a rewrite is integers.** LuaJIT's numbers are
doubles: whole numbers are exact up to 2^53 and not past it. Kosmos
depends on 64-bit integers in places where losing the low bits is silent:

- **A file's date** (`kfs.stamp`, 27 September) is bit 62 with the seconds
  above bit 16. As a double it would not survive.
- **The counter**: at a PC's 3 GHz, `sys.ticks()` passes 2^53 after about
  34 days of uptime, and from then on every duration computed in Lua loses
  precision.
- **Every protocol** is `string.pack` over 64-bit fields.

LuaJIT's FFI has 64-bit integers as boxed values, which would mean every
place that does arithmetic on one of these changing how it does it.

## What the kernel would have to allow

LuaJIT's compiler writes machine code into memory and then runs it. Kosmos
does not allow that: the ELF loader refuses a segment writable and
executable at once (`elfimage.c`, step 2 of `elf.md`), and no process can
turn memory it wrote into memory it runs - there is no call for it. LuaJIT
can keep to "never both at once" by writing to a page and then flipping it
to executable, so what it would need is **a system call that changes a
region from writable to executable and back** - and a decision about who
may make it, since it is exactly the thing an attacker who has control of
a process wants. Without it, only the interpreter runs, which is the
1.1-1.5x column.

## Where the time goes today

`frames` on a busy desktop: **composing is 76 to 85 per cent of a pass,
and it is C** already; the Lua half - requests, layout, focus - is about a
tenth. A 2.5x faster Lua would make a busy desktop pass perhaps 6 per cent
faster. The garbage collector, which is what `CLAUDE.md` says decides a
server's language, is a separate question LuaJIT does not answer better:
its collector is incremental and not generational.

What LuaJIT would change is **applications that compute in Lua** - a game,
a simulation, the IDE's checking. For those, `CLAUDE.md`'s answer already
stands and has been measured: move the loop to C, in a kit, where the
scanner went from 538 ms to 4.7 ms.

## Recommendation

**Keep Lua 5.4.** The speed is real - up to 4x on numeric Lua - but it is
bought with the language's integers, a rewrite of half the files, and
executable memory the system is designed not to have; and the frame is
mostly C already. **Revisit it if** an application that must stay in Lua is
measured to be bound by the interpreter, and a kit cannot take its loop.

The alternatives worth more per hour, in order:

1. **A profile of any slow application first** (6l): the answer so far has
   always been one loop, and C.
2. **Lua 5.4 built with `-O2` everywhere**, which it is on the machine; the
   host tools' `-O1` only affects the build machine's tests.
3. **Allocation in the Lua on a deadline**, which `CLAUDE.md` found was
   worth four times the worst pause where the language question was worth
   a ninth.

Diego's to decide.

---

## Reconsidered, 17 November 2026 - now that a JIT runs here

**DECIDED the same day: not now.** Diego: "lets discard the luajit for
now". Kosmos keeps Lua 5.4; a measured loop still moves to C. The
generational collector below is a separate, cheap measurement and is not
discarded with it.

Diego, 17 November 2026: "does it makes sense to jave LuaJIT now that we
changed our premises and already have javascript jit?"

**In short: not yet, and the JIT premise was the smallest of the three
reasons.** The change on 14 November removed the objection that a JIT may
not exist on Nebula - JavaScriptCore compiles on the M700 today - and
LuaJIT would find most of what it needs already in the kernel. What it did
not change is the language: Kosmos's Lua has doubled since September, and
the share of it that LuaJIT cannot run has doubled with it - 189 of 332
files, above all its 64-bit integers. Nor did it change where the time
is: composing left Lua altogether in October, and the window manager's Lua
is about a tenth of a millisecond a frame on the M700 that a JIT could
save. What is felt is the collector's worst pause, and LuaJIT's collector
is older than 5.4's. The cheap, unmeasured things - 5.4's generational
mode on the frame path, then Lua 5.5 - come first. LuaJIT makes sense
when an application that has to stay in Lua is measured to be bound by the
interpreter, and not before.

### What changed

**A JIT is allowed, and Nebula has the calls.** `docs/w2.md` part 2 and
`docs/jit.md`; `kernel/reserve.c`, `kernel/syscall.c`:

- `SYS_RESERVE`, `SYS_PROTECT`, `SYS_RELEASE` (`kosmos_reserve`,
  `kosmos_protect`, `kosmos_release` in `user/include/kosmos.h`). A page
  may be `RW`, then `RX`, then `RW` again - "never writable and executable
  at once", and `RESERVE_RWX` is refused with `SYS_ERR_FLAGS`
  (`reserve_protect`). **That is exactly LuaJIT's own model**:
  `LUAJIT_SECURITY_MCODE`, on by default, keeps its machine-code area
  `MCPROT_GEN` (`RW`) while a trace is assembled and `MCPROT_RUN` (`RX`)
  while it runs, flipped with `mprotect` in `lj_mcode.c`. One function,
  `mcode_setprot`, would call `kosmos_protect` instead.
- `SYS_MEM_MAP` with `MEM_MAP_RX`: one region mapped twice, which is what
  JavaScriptCore uses (`ENABLE_SEPARATED_WX_HEAP`). **LuaJIT has no such
  mode** - `lj_mcode.c` keeps one mapping per area and changes its
  protection in place, and its assemblers write through the pointer they
  will run from. Teaching it two mappings would mean every store into code
  and every relative branch computed against a second address: a port of
  our own inside the JIT. The flip is the shape that fits.
- **The instruction cache on AArch64**: `SCTLR_EL1.UCI` is set, so EL0
  cleans and invalidates it itself, and `__clear_cache` is in the libc -
  which is what `lj_mcode_sync` calls on a GCC build. x86-64 needs nothing.
- **Pages given on touch**, so a code area costs what it holds.

**One thing is not there: where the code lands.** LuaJIT's traces branch
to the interpreter's exit handler directly, so its code must lie within
`LJ_TARGET_JUMPRANGE` of `lj_vm_exit_handler` - **plus or minus 2 GB on
x86-64, 128 MB on ARM64** (`lj_arch.h`, read from the v2.1 tree today).
`mcode_alloc` asks `mmap` for addresses near there, sixteen tries, then
gives up with `LJ_TRERR_MCODEAL`. `SYS_RESERVE` takes no address: its
window starts at `USER_VA_BASE` + 64 GB (`USER_RESERVE_VA`,
`kernel/process.h`), and the image is at the base, so **every reservation
is out of range on both machines**. The image's gigabyte has the room -
the userland image is 42 MB (`build/x86_64/kosmos.bin`) and the rest of
`USER_IMAGE_MAX` is an unmapped guard - so the answer is small: a
reservation at an address the caller names inside that window, or a
second window beside the image. Small, and a change to Nebula, so
Diego's.

**And a gap worth knowing whatever is decided**: `PROTECT ... RX` is open
to every process - no right is asked for it, no `-- kosmos: needs jit`.
JetSki's processes need it, and so would every Lua process under LuaJIT;
whether making code should be a right a process is granted, as audio's
band is, is a question of its own (below).

### What did not change

**The dialect, counted again** - the same script as September's, comments
and strings stripped, `user/init`, `user/bin`, `user/lib`; measured today:

| What LuaJIT lacks | September | Now | Files now |
|---|---:|---:|---:|
| Kosmos's own Lua | 194 files, 75,285 lines | **332 files, 153,463 lines** | |
| Integer division `//` | 685 | **1,567** | 154 |
| Bitwise `&`, `\|`, `~` | 150 | **284** | 57 |
| Shifts `<<`, `>>` | 89 | **127** | 34 |
| `string.pack`, `unpack`, `packsize` | 94 | **218** | 31 |
| `math.type`, `tointeger`, `maxinteger`, `mininteger`, `ult` | 29 | **110** | 32 |
| `utf8` | 12 | **66** | 28 |
| Files using at least one | 96 | **189 of 332** | |

`goto` (16 uses) LuaJIT has; `<const>` and `<close>` are used nowhere.
With `user/installed` and `user/tests`, 339 files and 197 of them.

The integers are still the part that is not a rewrite (above): a file's
date at bit 62, `sys.ticks()` past 2^53 after about 34 days at 3 GHz -
438 calls to it in Lua now - and every protocol a `string.pack` of 64-bit
fields. LuaJIT's answer is FFI's boxed `int64_t`, and **FFI is the other
thing it brings**: `ffi.cast` reads and writes any address in the process.
`design.md` §5.3 makes the library list part of the security model, and
FFI is the one library that list would have to keep out - the same one the
integers would need.

**The C API**, counted in Kosmos's C (`user/`, `runtime/`, `lua/kosmos/`):
67 files include Lua's headers. LuaJIT 2.1 has the 5.2 additions most of
them call (`luaL_setfuncs`, `luaL_newlib`), and not these: `lua_newuserdatauv`
43 calls, `lua_rawlen` 36, `lua_isinteger` 15, `lua_get`/`setiuservalue` 12,
`lua_geti`/`seti` 7, `lua_absindex` 5, and four more - **about 120 call
sites to rewrite**. Then the quiet ones: `luaL_checkinteger` 429 and
`lua_pushinteger` 417 exist in 5.1, and pass every value through a double,
so **846 call sites keep compiling and lose the low bits of anything above
2^53**. Nothing would say so.

**"Upstream is not modified" would hold, at a price.** LuaJIT would sit in
its own `upstream/` with Kosmos's changes as a build step, as Lua's do in
`lua/kosmos/`: `lj_mcode.c`'s allocation and protection, its `lj_alloc`
replaced by the process's allocator (`lua_newstate` with an allocator is
allowed under GC64, which ARM64 requires and x86-64 has by default), its
error unwinding without libgcc's unwinder, `buildvm` and DynASM run on the
host. And the source-only rule carries over as it is: LuaJIT's `load`
takes bytecode too, and `luaL_loadbufferx(..., "t")`
(`user/init/main.c`) refuses it the same way.

**Maintenance.** LuaJIT 2.1 has been a rolling release since 2023 - the
`v2.1` branch is the release - kept by one person, steadily, with no 5.3 or
5.4 semantics planned. Lua 5.5.0 came out on 22 December 2025 and 5.5.1 on
3 August 2026.

### Where the time is, and what a JIT would move

Measured, from `testing.md`:

- **Composing is not Lua any more.** It was 76 to 85 per cent of a busy
  pass in September, in C called from the window manager; since Astra
  D3b-2 (18.514 to 18.516) the window server composes, in C, and the
  window manager "composes nothing".
- **The window manager's own Lua**, a drag on QEMU charged per thread
  (18.344, `qemu_pcprof`, before composing left it): about 1.2 M instructions of its own a frame,
  52% of them the interpreter - **0.3 to 0.4 ms of a 16.7 ms frame on the
  M700** for all of it.
- **The collector**: the window manager's worst collecting pass 1.40 ms
  (5.75 ms before `con.wait` stopped allocating); Plasma's frame path
  1,216 bytes a frame to 0 by a declared shape (18.433). Every gain on the
  frame path since September came from allocating less, not from running
  Lua faster.

Estimated, from those and September's ratios: LuaJIT's compiler at the
lexer's and the buffer's 2.3 to 2.5x on the window manager's interpreted
half would save **about 0.1 ms a frame, well under one per cent** of the
budget. Its interpreter alone, 1.1 to 1.5x, half that. **Neither touches
the worst pause**: LuaJIT's collector is 5.1's incremental one, not
generational; the collector its author designed for 3.0 never shipped.
The one thing it does for garbage is real and narrow - *allocation
sinking* takes temporary tables out of a compiled loop - and it applies
only inside a trace, which a coroutine's `yield` (48 uses, Kosmos's IPC
among them) ends.

**Where LuaJIT would pay is still applications that compute in Lua** - the
planets at 4.3x, the IDE's lexer at 2.3x - and the answer there is
unchanged and measured: the loop goes to a kit in C, at 110x for the PDF
scanner, with a SIMD lane where it fits.

**And the cost Kosmos would feel most is not on any of those tables.**
`CLAUDE.md` keeps policy in Lua because "a Lua module cannot have a buffer
overflow": a bug in Lua is a stack trace. Under a trace compiler that
stops being wholly true - a miscompiled trace is memory corruption under
code that cannot express any - and LuaJIT has fixed such bugs for fifteen
years. JavaScriptCore earned that risk with a 3.4x on script; Lua's
measured frame-path gain is under one per cent.

### The alternatives

1. **5.4's generational collector, on the frame path - unmeasured, and an
   afternoon.** `design.md` §5.2 point 4 has said "test generational
   against incremental and measure" since August, and it was never done:
   the guest runs 5.4's default incremental mode (`lua_newstate` in
   `user/init/lua_glue.c`, and no `LUA_GCGEN` anywhere), while the host's
   Lua already runs generational (18.280). The frame path's pattern - many
   short-lived tables - is what generational is for. `gc_pause_max` and
   `frames`' worst collecting pass, before and after, under `-icount`,
   then the M700. Its known weakness is an occasional full major
   collection, which is what 5.5 changed.
2. **Lua 5.5 - the language Kosmos writes, kept, with the collector it
   wants.** Its major collections in generational mode are incremental,
   so the long pause generational mode can bring is spread; arrays are
   more compact. The dialect gap is small and counted: `global` becomes a
   reserved word (used as a name nowhere in Kosmos's Lua), a `for` loop's
   variable becomes read-only (not counted), at most 250 results to
   `lua_call`. The C API: `lua_newstate` takes a seed (one call),
   `LUA_GCINC`/`LUA_GCGEN` no longer take parameters (`LUA_GCPARAM`; the
   one `lua_gc` call, `gfx.c`'s `LUA_GCSTEP`, is unaffected). Upstream
   stays unmodified. After point 1, so the move is measured against the
   best 5.4 can do.
3. **Loops to C, as now.** Still the rule with the biggest numbers behind
   it.
4. **Ravi** - Lua 5.3 with integers and a JIT on MIR - keeps the integers
   LuaJIT loses, but is 5.3, one person's, quieter than LuaJIT, and the
   same memory-safety argument applies. **Luau**, Roblox's, has native
   code for both processors and no integers, so LuaJIT's largest problem.
   Neither is worth a port on these numbers.
5. **Lua through JavaScriptCore** - compiling Lua to JavaScript, or a Lua
   in JavaScript - is not a serious option: JavaScriptCore is part of
   JetSki's image, tens of megabytes in every process that wanted it;
   JavaScript has no 64-bit integer either outside `BigInt`; and it would
   put a browser engine under the Deskbar.

### Decisions for Diego

1. **LuaJIT: not now** - *recommended*. Kept as 6zo's answer with a
   trigger written into it: an application that must stay in Lua,
   profiled on the M700 with `profile`, spending most of its time in
   `luaV_execute` on a loop no kit can take. *Or* yes now, which is about
   three weeks: the dialect (189 files) and 120 C call sites rewritten,
   846 integer sites audited, FFI kept out or let in, LuaJIT ported
   freestanding, and Nebula's reservation at a named address.
2. **Generational collection on the frame path, measured** - recommended
   next, on its own merit and whatever is decided about LuaJIT: one
   `lua_gc(L, LUA_GCGEN, ...)` in `lua_glue.c` against today, read in
   `gc_pause_max` and the worst passes.
3. **Lua 5.5 as the next language step** - recommended after 2, as a
   piece of its own: a vendored upgrade, upstream unmodified, the gate
   for proof.
4. **Making code a right** - independent of all the above: `PROTECT RX`
   and `MEM_MAP_RX` are open to any process today. Gating them behind a
   declared `-- kosmos: needs jit`, granted to JetSki's web and JSC
   processes, would make "what you were not handed you do not have" true
   of executable memory as well. Not recommended as urgent - an attacker
   who can make those calls already runs code - but it is the one place
   the JIT premise left looser than it found it.

**What is measured and what is not.** Measured: the counts above (today,
by script), the kernel's calls and the address windows (read from
`kernel/`), LuaJIT's jump ranges and `lj_mcode.c`'s protection model (read
from its v2.1 tree), and the profile figures quoted from `testing.md`.
Estimated: the 0.1 ms a frame, which multiplies September's Mac ratios by
a QEMU instruction count; and three weeks for the move. Not measured at
all: generational mode on the guest, and Lua 5.5 anywhere here.
