# Kosmos — design

The central document: what Kosmos is and **why every decision is the way it is**.

Version 0.2 — August 2026.

Companions: [ui.md](ui.md) (UI kit and window manager), [gfx.md](gfx.md) (the path pixels take), [beos.md](beos.md) (BeOS lineage), [testing.md](testing.md) (measurement and regressions), [hal.md](hal.md) (targets and HAL), [roadmap.md](roadmap.md) (milestones), [glossary.md](glossary.md) (terms), [state.md](state.md) (where you are today).

---

## 1. What Kosmos is

A microkernel desktop operating system with a Lua userland, booting on a Raspberry Pi 5.

The goal is to learn by building, and to test on real hardware a set of ideas that already exist but rarely appear together. Kosmos competes with nothing. There are no users to serve and no compatibility to maintain, and that is what makes it possible to take decisions a commercial OS cannot.

**What travels between things here is a Lua table, until the moment that is
the wrong answer — and then it is a declared struct.** That is one rule, not
a compromise between two, and the line it draws is a layer:

> The language's data model where the shape is the caller's to choose.
> A declared struct where the shape is agreed.

Inside a program, and between a program and anything whose vocabulary is
open — the window manager, application scripting, `diskfs` — a table is
exactly right. Adding a field costs nothing and nobody has to be told.

Crossing into a system server it is wrong, for two reasons that were arrived
at by building the thing and living with it rather than by reasoning:

**A server has to stay correct when its caller is wrong.** A table lets a
caller send any shape at all — an extra key, a wrong type, a nested table, a
megabyte of string — and every one is something the server must think about.
A struct means most of them cannot be expressed, so the wire refuses them and
the server never has to. This is the same argument as capabilities, one layer
up: a capability means you cannot **name** what you were not handed; a struct
means you cannot **say** what the protocol has no field for. Both replace a
server that validates with one that is correct by construction.

**A collector cannot sit where something else's timing depends on it.**
`gc_pause_max` is about 1.25 ms and arrives when the collector decides; an
audio period is 5.8 ms and a frame is 16. Not speed — structure-shaped Lua
costs about 2%, measured, which is nothing. The worst case is what decides.

Seven servers speak structs declared in `user/include/`: `/dev/audio`,
`/dev`, `/bin`, `/lib`, `/app`, `/dev/console`, `/data`. Five headers, about
3,700 lines with the servers themselves. Everything above them is still Lua
tables, and that is most of the system.

**What it costs, which is not hidden**: adding a field means editing a header
and rebuilding both sides; an error is a number, with the sentence composed
by whoever shows it to a person; and hot reload is gone entirely (§10).

This section used to end *"no marshalling, no IDL, no two worlds"*. There is
marshalling, those headers are an IDL, and there are two worlds — a small one
at the boundary and a large one above it. That is the arrangement that turned
out to work, and the sentence has been changed to say so rather than kept and
apologised for.

---

## 2. Principles

Seven non-negotiable decisions. If something collides with one of them, the feature gets cut, not the principle.

**The kernel does not know what a file is.** Threads, address spaces, IPC, capabilities. Nothing else. That is the rule. The 10k-line figure that used to sit here as though it were one is a symptom of it: if the kernel grows past about that much code — comments excluded, which `make size` counts — it is worth looking for what crept in. Finding nothing means the number was the wrong thing to look at, not that something has to leave.

**What a process has not mounted does not exist.** Not permission denied — no such path. The namespace is the mechanism by which resources are reached, not a permission layer on top of them.

**One protocol for every resource.** Sensor, process, file on the SD card, window. The client code is identical.

**Share-nothing userland.** One `lua_State` per process, no shared memory. Concurrency complexity stays confined to the kernel.

**Isolation comes from the hardware.** Lua is not sandboxed and does not need to be. A process runs at EL0 with its own address space; if it breaks its language sandbox, it breaks itself.

**The system is modified while running.** ~~A server reloads its code without losing its state or its clients.~~ **Withdrawn, September 2026** - see §10. Servers are C now and there is no dynamic linking, so none of them can be reloaded. Level 2, a supervisor restarting a server that died, is unaffected and is the architectural property.

**Compatibility inside a process, never at system level.** A libc inside an app is fine and necessary. A POSIX personality in the system is forbidden. The line is drawn in section 17.

---

## 3. Where each piece comes from

| Idea | Origin | What it solves |
|---|---|---|
| Microkernel, message passing as the only primitive | QNX / L4 | A driver blowing up does not take the system with it |
| Per-process namespaces, one protocol for resources | Plan 9 | Isolation and uniformity through the same mechanism |
| Typed attributes and live queries in the filesystem | BeOS | Structured search without an indexer on top |
| One thread per window, input at highest priority | BeOS | The feeling of fluidity |
| Live image, everything inspectable at runtime | Lisp Machines | Iteration without recompiling |
| Complexity budget as a design constraint | Oberon | Fitting in one head |
| Capabilities instead of permissions | seL4 | A process reaches exactly what you handed it |

The detail of what is inherited from BeOS, what is corrected and where Kosmos departs on purpose (including POSIX compatibility, which BeOS did have): [beos.md](beos.md).

Rejected on purpose:

- **Single-level store (AS/400).** Without CHERI-style tagged memory it is insecure. The Pi 5 is plain AArch64.
- **SOM (OS/2).** IDL hell dressed up as elegance.
- **Custom hardware (Lisp Machines).** It is what killed them.
- **Plan 9's purism of demanding you throw out everything prior.** Kosmos runs on standard hardware and uses the firmware that is already there.

---

## 4. Architecture

### 4.1 Kernel

Runs at EL1. Freestanding C11, no dynamic allocator, everything in fixed-size pools. An array of threads, an array of address spaces, an array of endpoints. This eliminates half the possible memory bugs in one stroke.

Responsibilities:

- Boot, MMU, page tables, physical page allocator
- Exception vector, GIC, ARM generic timer
- Scheduler and context switch
- Synchronous IPC
- Capability table and syscall validation
- Memory barriers, TLB, cache maintenance

It knows nothing about files, networking, graphics or Lua.

### 4.1.1 One image, and what that costs

**There is one userland binary, and a process is that binary with a
different argument.** `process_spawn` copies its parent's image; there is no
loader, no ELF parser and no relocation, because each process has its own
address space and the image is linked at a fixed address with nothing to
collide with. The boot word selects a role: below the Lua interpreter for
the C servers, and inside `init.lua` for everything else.

This is not only history. It is what makes the bootstrap possible at all -
`init` spawns `binfs`, and `binfs` cannot be loaded from `binfs`. Whatever
else changes, at least one image has to be there before there is anything to
load one from.

What it costs is that **anything the image declares, every process carries**.
A process is 5156 KB: 3044 KB of image, a 2048 KB heap and 64 KB of stacks.
Of the image, 2.5 MB is read-only - the Lua source of every program, the
fonts, the icons - and 383 KB is code.

That number was 7232 KB until `ramfs` stopped keeping its store in `.bss`.
`static struct node nodes[128]` is 2.1 MB, and with one image it was 2.1 MB
in the window manager, in Tracker, in the shell and in twelve other
processes that will never address a byte of it - thirty-four megabytes, of
which two used. `kosmos_map` at startup gives pages that belong to that one
process, and the cost is now visible where it belongs: ramfs reads 7308 KB
in the process list and everything else reads 5156.

**The rule, then: nothing in the shared image declares storage that only one
role needs.** A fixed pool is still right - that is 4.1 above, and it is
about the kernel besides - but a pool for a role belongs in the process that
plays it.

**Separate binaries per program are the expensive answer to the smaller
half.** They would need a spawn that names an image (the clean form is a
region the caller filled, so the kernel still knows nothing about names), a
build that decides each binary's subset, and the servers left in the
built-in image anyway for the bootstrap. And they would save little: every
Lua application needs Lua, `gfx`, the fonts and libc, which is most of the
2.5 MB. **Mapping the read-only half from one physical copy saves the same
2.79 MB fifteen times over with no build change and nothing observable** -
nobody can write those pages, so nobody can tell they are shared. That is
the next thing to do here, and it is worth roughly 42 MB on a desktop with
sixteen processes.

### 4.2 IPC

Synchronous, rendezvous, no buffering in the kernel. L4 style.

It is simpler to implement, faster on the hot path (a send is a direct context switch to the receiver), and it avoids the queue-and-backpressure hell that ate Mach. Asynchrony gets built on top, in userspace, with a broker, when it is needed.

The cost is that writing code over synchronous IPC normally forces you into ugly state machines. Coroutines solve that (see 4.5).

A detail that is easy to forget and ruins server restart: **when an endpoint is destroyed, the kernel has to wake everyone blocked waiting on it with an error.** Otherwise they hang forever.

### 4.3 Capabilities

Each process has an array of endpoints. Syscalls take an index into that array, never a global identifier.

A process cannot name what you did not hand it. There is no global table to enumerate, no path to guess. It is simpler to implement than Unix permissions and gives better isolation.

### 4.4 Namespaces and the protocol

The namespace server is the root of the system. It maintains, per process, the mapping of paths to server capabilities.

When a process resolves `/dev/temp`, the namespace returns the capability to the server that serves that node. From then on it talks to the server directly.

**The client never holds a direct, permanent capability to the server.** It holds a namespace node. That is what lets a server die, come back as a new process with a new endpoint, and have clients reconnect without knowing anything happened.

The protocol is typed records, not byte streams. The basic operations:

```
list(path)              -> array of names
read(path)              -> table
write(path, table)      -> ok
getattr(path)           -> attribute table
setattr(path, attrs)    -> ok
query(path, pred, cb)   -> live query handle
```

`read` returns a table, not a string. `fs.read("/dev/temp")` gives `{celsius = 47.2}`, not `"47200\n"` to be parsed. This is where BeOS typed attributes go inside the Plan 9 primitive, and that fusion is what makes Kosmos Kosmos rather than two ideas taped together.

### 4.5 Concurrency

Three layers, with different costs. Do not mix them.

**Lua coroutines.** Concurrency inside a process, no parallelism. Zero synchronization. One server handles 200 clients on one thread. Every `receive` is a yield, so you write sequential code over synchronous IPC. This is the design's main lever.

**Kernel threads.** Preemptive, own stack, scheduled by the scheduler. A process can have several. With one core it gives real concurrency without parallelism.

**SMP.** The Pi 5's four Cortex-A76s. Supported by design, off until stage 6.

Turning SMP on early kills the project. With one core the bugs are deterministic. With four you get races that appear once every thousand boots, and you are debugging over UART.

What does get done from day one, and costs nothing:

- No loose mutable globals. All state hanging off an explicit struct.
- `TPIDR_EL1` as the pointer to the per-CPU struct, from the start.
- A per-CPU runqueue in the scheduler, even with a single CPU.
- A comment at every spot where a lock will go.

When SMP arrives, the hot spots will be the page allocator (fixed with per-CPU caches), TLB shootdown (on AArch64 the broadcast is hardware, with `TLBI ... IS`, which saves considerable work compared to x86), and cross-core IPC. For the last one the known solution is affinity: servers live on one core and clients migrate toward the server.

None of this touches userland. A Lua server neither knows nor cares which core it runs on. That is the advantage of having chosen microkernel plus message passing, and it is the opposite of what BeOS did, which pushed threads with shared memory all the way into applications and paid for it with complexity in every one.

---

## 5. Lua

### 5.1 Why

30k lines of clean C89. You can read the entire interpreter, and in a learning project that is worth more than any feature. V8 is millions of lines.

It is designed to be embedded in a hostile environment: you provide the allocator, error handling assumes no OS, `lua_State` is a self-contained heap.

First-class coroutines, which is what makes synchronous IPC writable.

And tables as the only structured type, which is what allows the protocol to be the data model.

Vanilla Lua 5.4. **LuaJIT stays out**: it is stuck on 5.1, it is hostile on bare-metal AArch64 (it needs executable memory, W^X, i-cache invalidation) and it adds complexity exactly where you do not want it.

### 5.2 The GC

This is the real problem. The 5.4 GC is incremental, not realtime: steps are bounded in work, not in time. With a large, fragmented heap, one step can blow the frame budget.

Mitigations, in order of importance:

1. One `lua_State` per process, with a heap limit. ~2MB for servers. A small heap collects fast, and this comes for free from the design.
2. In the app server, run the GC by hand after the frame flip, with a calibrated `collectgarbage("step", n)`. Never let it decide during drawing.
3. Zero allocations in the compositing loop. Pre-allocate and reuse.
4. Test generational against incremental and measure. For the pattern of many short-lived message tables, generational usually wins.

### 5.3 Sandboxing

Lua is not isolated by default. `debug.getupvalue` and `debug.setmetatable` break any abstraction, and loading unverified bytecode gives direct arbitrary execution.

Kosmos does not depend on the language for isolation. Isolation is the EL0 address space plus capability validation in the kernel.

Still, two rules:

- **Loading precompiled bytecode is forbidden.** Source only, always.
- **The list of libraries available inside a process is part of the security model**, not a configuration detail.

### 5.4 No LuaRocks

Not out of dogma. Out of structural incompatibility.

C bindings load with `dlopen` and Kosmos has no dynamic linking, no ELF loader, no full libc. Pure-Lua libraries assume `io`, `os` and `require` with global filesystem paths, and none of those things exist here. And LuaRocks is a Unix app: it runs subprocesses, invokes `make`, speaks HTTPS.

What can be reused, by copying individual files:

- Pure Lua without stdlib: goes in as is (`dkjson`, `luaunit`, algorithms)
- Pure Lua with stdlib: goes in if you implement the functions it uses, which are sometimes three
- Small C bindings with no dependencies: recompilable. `lpeg` is 3000 lines and is well worth porting for the shell.

There is a deeper reason beyond the technical one: `io.open("/etc/passwd")` is semantically incoherent in Kosmos. A library doing that is assuming a model of the world the system rejects. Importing them would be POSIX coming in through the back door.

### 5.5 Our own packages

A package is a directory with a manifest declaring dependencies and capabilities. `require` resolves against the process's namespace, not a global path.

A free consequence: two processes can see different versions of the same library without conflict, because each has its own namespace. It is the problem npm and pip never solved well.

---

## 6. Language split

The rule that settles 90% of decisions: **C is what touches hardware or defines the isolation boundary. Lua is everything else.**

If a bug there can corrupt another process, it goes in C. If it can only kill its own process, it goes in Lua.

### C

The whole kernel (section 4.1). Plus the userland runtime: the interpreter, the `lua_State` allocator, syscall bindings, the minimal freestanding libc, and the table serializer for IPC.

And three things **only when measurement justifies it**: the alpha-blending blitter, the font rasterizer (`stb_truetype`, 2000 lines with no dependencies), and framebuffer drivers.

### Lua

The namespace server. The filesystem servers. The entire app server, including compositing. The shell and the REPL. Init and supervision. Every application.

### Where that C actually runs, which is not where people assume

**Almost none of it is in the kernel.** The interpreter, the serializer, the
libc, `gfx.c`, `stb_truetype` - all of that is compiled into the *user*
image, a different binary from the kernel, and it runs at EL0 with the
process's own page tables. A process is Lua source and the C that runs it,
in one address space:

```
   one process, EL0
   +--------------------------------------------------------+
   |  the program, and the libraries it loaded    Lua source |
   |  ui.lua, gfx.lua, kfs.lua, theme.lua         Lua source |
   |  - - - - - - - - - - - - - - - - - - - - - - - - - - -  |
   |  the Lua interpreter                                  C |
   |  gfx.c        fill, blit, blend, glyphs, discs        C |
   |  stb_truetype outlines                                C |
   |  serialize.c  a table to bytes and back               C |
   |  libc         memcpy, malloc, snprintf, setjmp        C |
   +--------------------------------------------------------+
```

So the split is **not** about privilege. A bug in `gfx.c` can corrupt the
process it is in and nothing else, which is the same blast radius as a bug
in the Lua above it. That is why the speed exceptions are allowed at all.

### Where the line really falls: structure or a loop over bytes

The rule at the top of this section answers "may this be C". It does not
answer "should it be", and the honest answer to that only arrives with a
measurement. There is now one worth writing down, because it is the
clearest example this system has produced.

The filesystem journal was written entirely in Lua, including a checksum
over every block of a transaction - FNV-1a, a byte at a time. Creating a
file went from 21 a second to 14 when the journal landed. Stubbing out only
the checksum brought it back to 20.5.

| | files a second |
|---|---|
| before the journal existed | 21.0 |
| with the journal, checksum in Lua | 14.4 |
| with the journal, checksum stubbed out | 20.5 |
| with the journal, checksum in C | 16.7 |

Read that table twice, because it says something surprising. **The journal's
own work - transactions, ordering, recovery, writing every block twice -
cost about two percent.** Hashing four kilobytes a byte at a time through
the interpreter cost the other thirty.

Nothing about the *structure* of a journal wanted to be C. One loop over
bytes did, and only a profile could have said which. Moving that loop to C
recovered about half of what was lost; the remaining gap is not explained
yet and is worth chasing before anything larger is concluded from this.

The line, then, is not "platform in C, applications in Lua". It is:

- **Decisions, dispatch, protocol, state, layout: Lua.** Measurably cheap,
  and where every advantage below lives.
- **Loops over many bytes, pixels or samples: C.** Measurably expensive,
  and no reload value - nobody hot-patches a checksum.

### Why the servers stayed in Lua, stated as costs

**They did not stay.** All seven moved between August and September 2026,
and §1 records what the experiment returned. The costs below were real and
were paid; they are kept because they are the honest price of the decision,
not an argument that was lost.

The proposal that arrives naturally at this point is: the window manager,
the filesystem, the namespace server are *platform*, nobody edits them,
write them in C and have them be fast. It is a serious proposal and these
are the things it would cost.

**Every server would have to marshal.** Today a server receives a Lua table
and returns a Lua table, and there is no marshalling code anywhere in the
system - not a line. A C server has to parse the serialised bytes into C
structures and rebuild them on the way out, or reimplement dynamically
typed tables in C, which is writing Lua again under worse conditions. That
is the IDL-and-generated-stubs work section 3 rejects SOM for, arriving
through the back door.

**Hot reload stopped at the boundary, and then stopped.** This argued that
`fs.reload` replaces a running server's code without losing its state or its
clients, and that the project's own definition of done - "Doom at 35fps next
to a prompt where you redefine the window manager while you play" - would
not survive a C userland. That was true and it is now moot: reload was
removed in September 2026 (§10), so this is no longer an argument for
keeping anything in Lua. The window manager's Lua half is still Lua, but for
the reasons below rather than this one.

**Portability, which is a stated goal.** The 26,000 lines of Lua userland
are recompiled for a new architecture exactly zero times, and cannot carry
an endian or alignment bug. The C has already produced one endian bug, in
the serializer, and every line of it is code that has to be got right again
per target.

The counts, measured rather than remembered — this paragraph said 16,000 and
11,000 for months, and both had drifted well past being round:

| | lines |
|---|---|
| Lua userland (`user/**.lua`, less tests) | 26,241 |
| the kernel side (`kernel/ arch/ hal/ boot/`) | 14,533 |
| userland C we wrote (`user/**.c`, `.h`) | 11,542 |
| — of which the seven servers and their headers | 3,715 |
| the libc and the Lua glue | 2,999 |

**The complexity budget.** Section 3 takes Oberon's constraint seriously.
26,000 lines of Lua is not 26,000 lines of C — and the seven servers are the
evidence for the exchange rate rather than against it: they cost 3,715 lines
of C and protocol headers to replace roughly 1,400 lines of Lua, which is the
price of a declared shape and no collector, paid deliberately.

**And the experiment loses its result** — which is what this said, and which
is no longer a reason not to, because the experiment has since run to
completion. §1 records what it returned: the thesis held everywhere the
shape is the caller's to choose, and failed at the boundary where a server
must stay correct against a caller that is wrong. The servers moved to C
knowing that. What remains an argument against a *wholesale* C userland is
the paragraph above this one and the two below it, not this.

### What would change this decision

Not an argument - a measurement. The costs above are real and so is the
speed, and the way to settle it is to build one hot server path both ways.

`kfs.store` is the right subject: it is self-contained, it is measured
already, and `tools/test_kfs.lua` tests the format on the host so a C
version can be held to exactly the same checks.

- If the C version is several times faster, the platform should follow it
  and this section is wrong.
- If it is fifteen or twenty percent, the cost was never the interpreter -
  it is the block I/O and the round trips, which are the same in any
  language - and the question is closed.

The prediction on record, so it can be wrong in public: **fifteen percent.**
A file create is about sixty milliseconds and roughly fifteen block
operations at three milliseconds each, none of which cares what language
asked for them.

### Why C and not Rust

Lua is 30k lines of C and it is the system's central piece. With a Rust kernel, that C exists anyway but inside an `unsafe extern`, with raw pointers and a raw allocator. You get all of Rust's costs and no guarantee where it matters most, because the real risk surface is the kernel↔Lua interface.

And the detail that closes it: **Lua's error handling uses `longjmp`**, which jumps up the stack without unwinding it. Rust's `Drop` implementations never run. A destructor that does not execute is exactly what Rust promises to eliminate, and here it would happen on the hot path.

Separately: this is a project for learning OS work, not for learning Rust. Fighting the borrow checker over inherently cyclic structures (every scheduler is a list of threads pointing at each other) while learning bring-up is a guarantee of dying at stage 2. And all the reference material (9P, Haiku, seL4, the Pi examples) is in C.

Rust has a place later, in userland, above the message boundary. A filesystem server in Rust speaking the protocol is perfectly reasonable at stage 8. The microkernel gives that freedom precisely because it does not force everything to share a language.

### C discipline

With no compiler looking after you, the guardrails go up from day one:

```
-std=c11 -ffreestanding -nostdlib -Wall -Wextra -Werror
-fno-common -fno-strict-aliasing
```

Plus: two functions `mmio_read32`/`mmio_write32` with the barriers inside instead of loose `volatile`; stack guard pages as soon as there is an MMU; `-fsanitize=undefined` with our own handler, which works freestanding and catches odd things; `_Static_assert` on every struct that crosses the boundary with assembly.

There will be little assembly and it comes early: entry point, exception vector, context switch, barriers. A few hundred lines of AArch64 in total.

---

## 7. Graphics

### 7.1 No GPU

The framebuffer is enough, and the numbers confirm it. 1920×1080 at 32bpp is 8.3MB per frame. At 60fps, 500 MB/s. The Pi 5 has ~17 GB/s of real LPDDR4X bandwidth. There is an order of magnitude to spare.

And in practice the whole screen is never redrawn. With damage tracking, moving a window is a couple of hundred KB.

GPU acceleration is for effects, transparency and scaling. Kosmos wants to drag windows with one frame of latency, which is a problem solved in 1997.

### 7.2 The trap that does matter

**The framebuffer is usually mapped as uncached device memory.** Writing there is 10 to 50 times slower than to normal RAM. That is the mistake that kills performance, not the absence of a GPU.

Rule: **always draw into a backbuffer in cached RAM, and do a single blit of the dirty rectangle to the framebuffer at the end.** All the blending, clipping and text happens in fast memory. The only contact with slow memory is a sequential blit, which is the case where write-combining works well.

If at some point there is a cached-mapped framebuffer, a cache clean by range goes in before the display controller reads.

### 7.3 Where fluidity is won

BeOS ran on 1998 Pentiums with no useful 2D acceleration and felt better than systems do today. That came from architectural decisions, not graphics ones:

1. **Input on a highest-priority thread, always.** Non-negotiable. If an app hangs while drawing, the window keeps moving.
2. **Double buffering with damage tracking.** Never a full redraw.
3. **One message port per window, each with its own coroutine.**
4. **Synchronize with vblank.** Without it there is tearing no matter how fast you draw.
5. **Blocking in the compositor is forbidden.** If an app did not respond, compose with what it had.

Fluidity is consistent latency, not high throughput.

### 7.4 The drawing model

Two real options, and Kosmos picks the second.

**A — the app draws into its own buffer and the server composes.** What every modern compositor does. Fast, but it needs shared memory between processes, which breaks share-nothing and adds a kernel primitive.

**B — the app sends drawing commands.** `{op="rect", x=10, y=10, w=100, h=50, color=0xff0000}`. Pure message passing. It is the X11 model and the BeOS app_server model.

B it is. The cost is smaller than it sounds: a typical window is ~50 commands per frame against the ~200KB of blit the server does anyway. The serialization overhead gets buried.

And it gives something nice for free: commands are data. They can be logged, replayed, redirected to another display, or inspected from the REPL to see what an app is drawing.

If video or a large canvas shows up, shared memory gets added as a special case. Never as the general case. That path is designed in [gfx.md](gfx.md).

### 7.5 Drivers

Under QEMU: virtio-gpu in dumb framebuffer mode. A couple of hundred lines.

On the Pi 5: the firmware leaves a configured framebuffer before the kernel starts. You ask for it over the mailbox and it returns physical address, width, height and pitch. That is the entire video driver; the VideoCore VII does the scanout to HDMI.

Two details that cost an afternoon each if you do not know them: **the pitch is almost never `width × 4`** (the firmware aligns it, and assuming otherwise gives the classic skewed image), and **the channel order on the Pi is usually BGRA**.

---

## 8. Filesystem

### 8.1 There is a real filesystem

One tree on the SD card, persistent, with real files:

```
/system    binaries and config
/lib       Lua libraries
/apps      installed apps
/home      user data
```

If two processes open the same file, it is the same file. There are no copies and no divergent views.

What does not exist is every app seeing the whole tree. The shell and the file manager do see all of it, because the launcher mounts the root for them. A normal app sees the portion it declared in its manifest.

It is the same thing a container does on Linux. The difference is that there it is opt-in and complicated, and here it is the only form that exists. There is no "no namespace" mode to fall back to.

### 8.2 Attributes and live queries

Attributes live alongside the file, typed and indexed by the filesystem server. Not inside the file, not in a sidecar.

Live queries are the part of BeOS nobody replicated and that neither macOS nor Linux has today: you register a predicate over attributes and the server sends you a message when the result set changes. No polling. If another process writes a file that matches your query, your view updates on its own.

FAT32 at first, because the Pi's firmware needs it to boot. An own filesystem with native attributes and indexes is stage 8, and that is where this part becomes real.

### 8.3 What is in memory and what is on disk

All of the above works today and none of it is persistent. The attributes are real, the index is real, and `watch` really does park a caller's reply until the result set changes. They live in the ramfs server's own tables.

The division M8 introduces is therefore narrower than it looks:

- **On disk**: the tree, the file contents, and the attributes. Everything a filesystem must not lose.
- **In memory, rebuilt at mount**: the index. It is derivable from the attributes, and derivable state that is also stored is state that can disagree with itself - which on a filesystem means a query returning a file that is not there. Rebuilding costs a scan and removes both the B+tree and the class of bug where the index and the truth drift apart.

That is a scale judgement and it is written down as one, so it can be revisited honestly: it holds while a mount scan is cheaper than the complexity it avoids, and stops holding at a file count this system is nowhere near.

### 8.4 A large file is mapped, not copied

`read` returning a string is right for a configuration file and wrong for a picture. A 936 KB PNG through `fs.read` gives `not enough memory`, because the string is accumulated on a 2 MB process heap.

So a large read returns a **memory capability** instead: the server puts the file's pages in a region, the capability travels in the reply, and the client maps it. The pages exist once and both processes address them. It is the same primitive shared surfaces use, and for the same reason - a message is 2048 bytes, and the answer to "how do I move a megabyte" is never "in smaller messages".

---

## 9. Applications

### 9.1 Lua, same as the system

There is no distinction between writing an app and modifying the system. An app and the window manager are the same class of thing, in the same language, reachable from the same REPL. That is the Lisp Machine property.

An app is a directory with `.lua` files and a manifest. No compilation, no linker, no custom executable format. You copy the folder.

### 9.2 The manifest

This is where capabilities stop being a kernel abstraction and become something visible:

```lua
return {
  name = "Notes",
  needs = { "ui", "/home/notes", "/dev/clock" },
}
```

The launcher builds the namespace with exactly that. The app reaches nothing else because nothing else exists in its world.

Even the clock is a capability. If you do not ask for it, you do not have it.

### 9.3 Example: Monitor

Everything it reads comes from servers exposing a namespace. None of it is a file on the SD card.

```lua
return {
  name = "Monitor",
  needs = { "ui", "/proc", "/dev/temp", "/dev/uptime" },
}
```

```lua
local ui = require("ui")
local fs = require("fs")

local win = ui.window{ title = "Monitor", w = 380, h = 420 }
local procs, temp, uptime = {}, 0, 0

local function refresh()
  procs = {}
  for _, name in ipairs(fs.list("/proc")) do
    procs[#procs+1] = fs.read("/proc/" .. name)
  end
  table.sort(procs, function(a, b) return a.cpu > b.cpu end)

  temp   = fs.read("/dev/temp").celsius
  uptime = fs.read("/dev/uptime").seconds
  win:invalidate()
end

win:on("draw", function(gc)
  gc:fill(0x1a1a1a)
  gc:text(16, 28, string.format("SoC %.1f C   up %ds", temp, uptime), 0xffffff)

  local y = 60
  for _, p in ipairs(procs) do
    gc:text(16,  y, p.name, 0xdddddd)
    gc:text(200, y, string.format("%.1f%%", p.cpu), 0x88ff88)
    gc:text(280, y, p.state, 0x888888)
    y = y + 20
  end
end)

win:every(1000, refresh)
refresh()
win:run()
```

`/proc` is not a special filesystem with its own rules. It is a Lua server that answers `list` and `read`, speaking the same protocol as the SD card server.

### 9.4 Example: Notes

A real filesystem plus attributes and live queries.

```lua
return {
  name = "Notes",
  needs = { "ui", "/home/notes", "/dev/clock" },
}
```

```lua
local ui = require("ui")
local fs = require("fs")

local win = ui.window{ title = "Notes", w = 640, h = 480 }
local notes, current, buffer = {}, nil, ""

local function save()
  if not current then return end
  fs.write("/home/notes/" .. current, buffer)
  fs.setattr("/home/notes/" .. current, {
    modified = fs.read("/dev/clock").epoch,
    words    = select(2, buffer:gsub("%S+", "")),
  })
end

local function open(name)
  save()
  current = name
  buffer = fs.read("/home/notes/" .. name)
  win:invalidate()
end

-- live query: the filesystem server notifies when the result changes
fs.query("/home/notes", "words > 100", function(result)
  notes = result
  win:invalidate()
end)

win:on("key", function(k)
  if k == "\b" then buffer = buffer:sub(1, -2)
  else buffer = buffer .. k end
  win:invalidate()
end)

win:on("click", function(x, y)
  if x < 180 then
    local i = math.floor((y - 40) / 22) + 1
    if notes[i] then open(notes[i].name) end
  end
end)

win:on("draw", function(gc)
  gc:fill(0xf8f8f8)
  gc:rect(0, 0, 180, 480, 0xeeeeee)

  local y = 40
  for _, n in ipairs(notes) do
    local color = (n.name == current) and 0x0066cc or 0x333333
    gc:text(12, y, n.name, color)
    gc:text(12, y + 11, n.attrs.words .. " words", 0x999999)
    y = y + 22
  end

  gc:text(200, 40, buffer, 0x111111)
end)

win:on("close", save)
win:run()
```

### 9.5 The point that unifies

`fs.read` is the same message in both cases. In Monitor it ends up reading a SoC register; in Notes, blocks off the SD card. The app does not tell the difference.

On Linux, the same thing requires `open`/`read` for files, `sysfs` with string parsing for sensors, `/proc` with its own format for processes, `inotify` for watches, and `getxattr` for attributes. Five APIs for what is one here.

That reduction is the whole argument, and it is the reason to build this even if nobody else uses it.

### 9.6 The UI kit

The full design of the UI kit and window manager is in [ui.md](ui.md). Summary: BeOS lineage (view tree, follow modes, one message handler per window, replicants), with the locks removed because coroutines replace threads, and with `Draw()` producing commands instead of writing into a shared buffer.

The consistency rule: **an app does not draw UI primitives.** The kit lives in `/lib/ui` and is resolved by namespace, and the visual tokens are in `/system/ui/theme`. Editing `button.lua` changes every app the next time one starts - it used to say "instantly", which was written when servers reloaded and was never true of a *library* anyway: `use` caches what it loaded, and an application holds the table it was given.

### 9.7 The first app

**The inspector.** A window that lists processes and their namespaces, and lets you open a REPL against any live server.

It is the tool you will use for the rest of development, and it is the proof that the design closes. If you can open a REPL against the app server from inside an app, redefine a function and see the change without restarting anything, the system you wanted is built.

---

## 10. Hot reload

A historical clarification first, because this gets conflated often: **BeOS was not a microkernel and did not have hot reload.** Its kernel was monolithic, drivers and BFS lived inside it, and killing the `app_server` took the desktop with it. What BeOS had was the message model (`BLooper`, `BHandler`, `BMessage`), which was a C++ library convention, not an isolation boundary.

Kosmos is more radical: servers in separate processes with separate address spaces. That is QNX. From BeOS it takes the concurrency model and the design sensibility.

### Level 1 is gone, and this is the record of removing it

**September 2026: hot reload was removed from Kosmos.** Not outranked,
removed. There is no `fs.reload`, no reload branch in `serve`, and no server
whose code can be replaced while it runs.

It happened in two steps and the second one is the honest one. The first was
a demotion: this section used to treat reload as the property the design
existed to protect, and that stopped being the order once the goal was a
system that stays responsive on a Pi 5. When the two disagreed,
responsiveness won.

The second step was that the disagreement stopped happening, because the
servers ran out. Seven of them moved to C - audio, devices, binfs, libfs,
appfs, console, ramfs - and there is no dynamic linking here, so a C server
cannot be reloaded at all. ramfs was the last one that could be, and it was
what `ROLE_RELOAD` reloaded and what `help("demos")` let you *watch* being
reloaded.

**And ramfs did not have to go.** It is 247 lines of paths and table lookups,
nothing's timing depends on it, and by this document's own rule - C is for a
server whose hot loop is small and bounded - it was the weakest candidate of
the seven. It went because the system should be one thing rather than six
servers in C and one in Lua for the sake of a feature, and that was a
deliberate trade with a known price: a milestone's permanent test deleted,
and the one demonstration of §9.1's Lisp Machine property gone with it.

What survives is the *shape*: `serve` still takes a factory rather than a
table of handlers, so a server's state and its behaviour are still separate
things. That was reload's mechanism and it is worth keeping on its own.

**Level 2 is untouched and is the architectural property anyway.** A
supervisor restarting a dead server, with clients reconnecting through the
namespace, never depended on the language a server was written in.

**What keeps anything in Lua now is not reload.** Two things do. The first is
arithmetic: code that moves almost no bytes gains a fraction of the ~2% that
structure-shaped Lua costs. The second is the shape of the bug - a Lua server
cannot have a buffer overflow. Isolation is identical either way, since both
are EL0 processes behind an address space, but one failure is a stack trace
and the other is a night with a debugger.

`diskfs` is what is left, and for neither of those reasons: its core runs on
the host as well as the guest, which is what lets `make test` check the
journal's power-loss window without booting a machine.

### Two levels

**Level 1 — reload code in a live server.** *Removed, September 2026; see above.* It was nearly free in Lua: the server kept its state in a table, received a reload message, `load()`ed the new code, and continued. The process never died and the clients never knew.

**Level 2 — restart a server that died.** A supervisor relaunches it, clients reconnect through the namespace. This is the architectural property.

### Who owns the state

The real design is here. If the app server dies, the window list went with it, no matter how clean the microkernel is.

Three ways out, in order of effort:

1. **State reconstructible from the client.** The server comes back empty and each app re-declares its window. Enough for Kosmos.
2. **State in a separate server.** You split the logic (complex, changes often, can crash) from the store (simple, stable). This is the good version.
3. **Persistent state.** That is the single-level store already rejected.

### Criticality hierarchy

As in Erlang: complexity goes far from the root, and what is near the root is so simple it cannot fail.

- **Namespace server: not restartable.** If it dies, the system died. It is kept small and boring.
- **Framebuffer and input: restartable but visible.** They come back within a couple of frames and it shows.
- **App server, filesystem, everything else: restartable.** This is where complexity lives.

Level 1 gets done at stage 5, as soon as there are servers. Level 2 after stage 6, once you know what state matters. Designing recovery before knowing what has to be recovered is guessing.

---

## 11. Hardware and bring-up

Targets, the HAL interface, the `arch/` vs `hal/` distinction, and the trap in each board: [hal.md](hal.md).

The essentials: development happens on QEMU `virt` and verification happens on real hardware. The Pi 5 has nearly all I/O hanging off the RP1 over PCIe, so the 3-pin JST-SH debug UART connector is the only way out. The rule for evaluating any future target: if you cannot get a character out over serial in the first two hours, it is not a target.

## 12. Milestones

The thirteen milestones, each with a definition of done, are in [roadmap.md](roadmap.md).

Summary: M0-M1 boot and foundations, M2 Lua in the kernel plus the first real hardware, M3 the microkernel, M4 Lua to userspace (the hardest), M5 namespaces and servers, M6 graphics and app server, M7 attributes and live queries, M8 own filesystem, M9 3D demo, M10 Doom, M11 drivers, M12 SSH client.

The targets and the trap in each piece of hardware are in [hal.md](hal.md).

## 13. What stays out

**A daily driver is not the goal, and chasing it kills the project.**

The reason is not features. A daily driver needs a browser, and a modern browser is 30 million lines that assume POSIX, threads, a JIT, a GPU and a full network stack. Porting Chromium is more work than the whole system, and at that point you stop building your OS to maintain somebody else's port. That was the wall Haiku hit, with 20 years and dozens of contributors.

Out of scope, no discussion:

- POSIX compatibility in any form
- A browser
- WiFi and Bluetooth (firmware blobs, an 802.11 stack)
- Audio with decent latency
- Multiuser and Unix-style permissions (capabilities solve it better)
- GPU acceleration
- Single-level store

### The achievable goal

An OS for daily use at **one** task. A device that turns on and does one thing well, with this architecture. There the design's advantage is real and you are not competing against 30 million lines.

The closest and nicest candidate: a typewriter. A Pi with a screen and keyboard, running only the editor with live queries. Boot to editor in two seconds, nothing else.

### The criterion for every future decision

**Does this test an idea in the design, or does it just close a feature gap against Linux?**

A filesystem with attributes tests an idea. A Chromium port closes a gap. The first is why you started the project. The second is why projects get abandoned.

---

## 14. Known risks

**No types, in a system of tens of thousands of lines of Lua.** A typo in a field name is a silent `nil` three layers down. Mitigation: every message is a table with a mandatory `type` field, validation at each server's boundary, tests from early on.

**No libraries.** Everything you need you write: JSON, dates, cryptography. For a learning project that is half the point, but it has to be clear going in.

**No visual debugger for a long time.** REPL and `print` over UART. Lua has hooks in the `debug` library, so a basic debugger is buildable and is a good stage 7 project.

**Stage 4 can stall the project for months.** Getting Lua out of the kernel to EL0 touches everything at once: address spaces, syscalls, allocator, error handling.

**The temptation to push things down to C because "it is faster".** Every time you do it you lose the live image, which is why you chose this design. A window manager in Lua you redefine at runtime from the REPL; in C you recompile and reboot. That capability is worth more than the 30x.

---

## 15. The measure of success

Reaching stage 7 with a graphical desktop booting on real hardware, with working live queries. (Hot-reloadable servers were part of this and were removed in September 2026; see §10.)

That is something almost nobody builds. It not being your everyday machine takes nothing away from it.

---

## 17. The POSIX line and the libc

This section exists because "no POSIX layer ever" is too blunt and does not explain where the real limit is.

### 17.1 Why a POSIX personality is forbidden

POSIX is not a set of functions. It is four assumptions, and all four collide head-on with the design:

- **A global tree.** `open("/etc/passwd")` assumes a universal path reachable from any process. Implementing it means creating a global namespace inside Kosmos, meaning two resource models coexisting.
- **Ambient authority.** A POSIX process can open anything its credentials give it access to. It does not ask permission, it has it by existing. It is the exact opposite of capabilities.
- **`fork()`.** It duplicates the address space with copy-on-write and inherits every file descriptor. Inheriting descriptors is ambient authority by definition, and COW is not in the kernel and should not be.
- **Signals.** Asynchronous interruption at an arbitrary point. The entire Kosmos model is messages that arrive when the process is ready.

A POSIX layer that works has to bring those four things with it. It stops being a layer on top of Kosmos and becomes a second operating system sharing the kernel.

And even if it were built well isolated, it has gravity. Every subsequent design decision drags along the question "does this break the POSIX layer?", and with software running on the other side the answer will always be conservative. Then comes the predictable part: somebody ports something, it works, and suddenly the cheap path for anything new is POSIX instead of the Kosmos protocol. It is what happened to Haiku, which has queries, attributes and BMessage, and where most software runs on POSIX and ignores all of it.

The underlying argument: Kosmos exists to answer whether a system simplifies when every resource speaks one protocol and every process sees only what is mounted. A POSIX layer answers that question with "we do not know, we covered both". The experiment loses its result, which is the only thing the project produces.

### 17.2 Where the line is

**Compatibility inside a process, with the capabilities it declared. Never POSIX semantics at system level.**

A libc shim living in the app's address space, with no ambient authority and no global tree, is correct. Doom is exactly that: it thinks it has a libc, and it runs as a normal citizen with its manifest and its capabilities.

The line was crossed the moment `fork`, signals, a global `/`, or a server handing any process access to any path shows up.

### 17.3 Scope of the libc

**For Lua (M2):** `memcpy`, `memset`, `memmove`, `strlen`, `strcmp`, `strcpy`, `strchr`, `setjmp`/`longjmp`, a minimal `snprintf`, and the `math` functions Lua asks for. An afternoon of work.

**For porting apps (M10):** `malloc`/`free` over the process heap, `printf`, `abort`, `qsort`, and `fopen`/`fread`/`fwrite`/`fclose`.

**The rule that holds the line:** every libc I/O function is a call into the process's namespace and nothing else. `fopen("/lib/config")` resolves against what was mounted for it. If it does not have it, it fails. No fallback to a global tree, no special case.

**Never:** `fork`, `exec`, `signal`, `pipe`, `socket`, `select`, `ioctl`, and all of `unistd.h`. If a port asks for one, patch the port.

**A detail that causes bugs months later:** `errno` is a global variable and with coroutines it does not work. It goes per process, in the state struct. Solve it at the start.

**References:** musl for reading clean implementations (the code is too Linux-specific to copy), PDCLib for public-domain freestanding.

### 17.4 What can be ported, then

Computational libraries with little system surface: SQLite, `stb_*`, zlib, monocypher, interpreters for other languages, old game engines.

What does not get ported: anything assuming processes, signals, BSD sockets or a global filesystem. Meaning, almost everything that is a Unix program rather than a library.
