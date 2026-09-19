# Threads in a process

**Agreed on 19 September 2026** (`roadmap.md` 4j). Diego: "Why don't we add
threading? To the kernel!", "Every modern os has multi threading as well as
multi processing", "Threading will give us a lot of room for speed and super
responsive ui and os", and "I want to follow the beos idea which is as
really great. Can we have threads where makes sense? In c? What about Lua
apps?"

This page is written before the kernel changes, as `smp.md` was, so that
the order is argued once and every step can be checked on its own. Nothing
here was built when it was written; the steps below say what has been
since. Where it says "today" it describes the tree as it stands
on 19 September, with a file and a line for each claim; where it says
"proposed" it is a decision still Diego's.

---

## Why, in one paragraph

A process is homed on exactly one core and never moves (`smp.md`), so **one
program can use one core**, however much work it has. Four cores under
QEMU, eight on the ThinkPad: a video decoder, an audio project's mix, a
software 3D renderer or a planet simulation each gets one of them and
watches the rest idle. Several processes could share the work, but then a
decoder, a mixer and a window are three programs passing frames through
regions and messages - the heavy way to do what threads do by sharing an
address space. `design.md` 4.5 refused threads on an argument written for
one core; it has been corrected, and this is the plan that replaces it.

## Processes and threads, as this system will have them

**A process owns things; a thread runs.** What a process owns stays the
process's, and every thread in it shares all of it:

- the address space, and so all memory, regions and mappings;
- the capabilities - endpoints it may call, regions it holds, interrupt
  lines it owns;
- the endpoints it serves, and its name, and whether it has been killed.

**What a thread has of its own is only what running needs:** registers,
including the one that points at its own data; a kernel stack; a user
stack; a home core, chosen when it is made and never changed; a priority
band; and its place in IPC - a thread in the middle of a call is blocked,
and its siblings are not.

**A thread is named by its index in its process**, never by a global
number, for the reason capabilities are: what a process was not handed, it
cannot name.

## What the kernel assumes today

Gathered by reading the tree for this page, and checked. The shape of it:
more of the kernel says "one thread" than the single pointer suggests.

1. **`struct process` holds one `struct thread *`** (`kernel/process.h`
   301), and about forty places use it as *the* thread: the process table
   `sysinfo` lists (`process.c` 93-134, one state and one core a process),
   granting sound or the screen raises one thread's band (731, 958), waking
   for audio or the network wakes one thread (762-872), kill aborts one
   thread's IPC (1031), start wakes one (1050), and `process_exit` requires
   its caller to be that thread (1139, 1189). `thread->process` already
   points the other way and is per thread, so that half is ready.

2. **The capability table is the thread's, not the process's.**
   `caps[CAPS_PER_THREAD]`, 32 slots, in `struct thread` (`thread.h` 296-319),
   whose comment says it "moves to the process" at M4 - which never
   happened. Its operations take no lock (`ipc.c` 423-680), which is correct
   only because nothing but its own thread touches it, apart from a delivery
   into it under the endpoint's lock.

3. **Teardown assumes the dying thread is the only one.** `process_exit`
   releases endpoints, interrupt lines, capabilities and memory and then
   destroys the address space (`process.c` 1132-1297). Another thread still
   running in that space, on another core, would be running on freed page
   tables: `as_destroy` does no TLB maintenance on either architecture,
   because today the only thread has already switched away.

4. **Kill is a flag and a nudge.** `process_kill` sets `killed` and aborts
   the one thread's IPC wait (`process.c` 983-1039). `ipc_abort` wakes only a
   thread blocked on an endpoint (`ipc.c` 305-309): one asleep in
   `SYS_SLEEP`, waiting for input or for a child dies when that wait ends
   on its own. A thread running on another core dies at its next syscall or
   timer tick; there is no interrupt sent to hurry it.

5. **The address space has no lock.** `struct addrspace` is a root and a
   flag; a page-table level is allocated where one is missing without a
   lock (`arch/aarch64/mmu.c` 111-126), and the windows `SYS_MAP` and
   shares are placed in are bumped without one (`syscall.c` 817-867,
   1558-1654). One thread per process is what makes all of that safe.

6. **The TLB is already right, on both boards.** AArch64 invalidates by
   broadcast (`mmu.c` 182-206). x86's shootdown asks exactly the cores whose
   loaded root is this address space (`arch/x86_64/mmu.c` 169-229) - which
   is what threads on several cores need, and was built before they
   existed.

7. **One user stack**, 256 KB at the base plus 46 MB (`process.h` 112,
   138), with a guard gap below it. From 8 GB to the 512 GB the address
   space allows, nothing is used: room for a stack per thread, each with a
   guard of its own.

8. **The user side keeps nothing per thread.** Neither `TPIDR_EL0` nor x86's
   FS base is saved on a switch - they appear nowhere in the tree. `errno`
   is one static int, "one of it because there is one thread"
   (`runtime/libc/misc.c` 14-30), and `malloc` works on globals with no lock
   (`runtime/libc/malloc.c` 94-97).

9. **IPC is already per thread**, which is the biggest piece that does not
   have to change: each thread has its own message, peer and wait
   (`thread.h` 287-294). Three edges assume one thread: a reply is sent to
   the caller's thread by pointer and priority inheritance is handed back by
   whichever thread replies (`ipc.c` 1272-1331), so a server whose reply
   comes from a different thread keeps a borrowed band; an endpoint takes
   one watcher (`ipc.c` 103-117); and `process_wait` has one waiter slot
   (`process.c` 498).

10. **There is nothing a user thread can wait on for another.** No futex,
    no event (`syscall.h`). The kernel has the piece one is built from -
    `thread_block_and_release`, which blocks and drops a lock as one step
    (`thread.c` 1665-1837).

### Two things found on the way, wrong today

- **A shared region's reference count is not locked.** `memobj_ref` and
  `memobj_unref` are a plain `++` and `--` (`memobj.c` 206-250), and they
  are reached from different processes on different cores - a capability
  delivered in a message under one endpoint's lock, another dropped at a
  process's exit under none. Two at once lose one, and a region's pages go
  back to the allocator while a window still draws into them, or never do.
  Rare, and real on four cores now; threads would make it common. **Step
  0.**
- **A spawn that fails early leaks its process slot.** `process_create`
  claims the slot and then returns `NULL` on a bad header or no address
  space without releasing it (`process.c` 272-351). Only a bad image or no
  memory reaches it. Step 0 as well.

## The design, proposed

### Making and ending a thread

`SYS_THREAD_CREATE(entry, arg)` gives the process a new thread, returning
its index. **The kernel makes its stack** - pages from `pmm`, mapped in a
slot of its own above 8 GB with an unmapped gap below it, so a thread that
overflows faults rather than writing into its neighbour. Its home is the
least busy core, as every thread's is today. It comes from the same thread
pool as every other thread - **with no number of its own**: a process may
have as many threads as the machine can hold (step 1b).

`SYS_THREAD_EXIT(code)` ends the caller. `SYS_THREAD_WAIT(index)` waits for
a sibling to end and returns its code.

**A process ends when its first thread returns or any thread exits the
process** - C's rule, and every other system's: `main` returning ends the
program. The alternative, the process living until its last thread ends,
lets a forgotten worker keep a closed application alive and invisible.

### Waiting on each other: a futex

`SYS_FUTEX_WAIT(address, expected, wait_ticks)` blocks if the word at that
address in this address space still holds `expected`; `SYS_FUTEX_WAKE
(address, count)` wakes that many. **Everything else is built on it in the
process**: a lock, a condition, a channel. The point of the shape is that it
costs nothing when nobody waits - a lock nobody contends is an atomic
instruction in the process and never a syscall - and every modern kernel
converged on it for that reason: Linux's futex, macOS's `ulock`, Windows's
`WaitOnAddress`, Zircon's futex.

It fits what the kernel is allowed to know. It is a thread waiting on an
address in an address space, which are two of the four things the kernel
knows about; it knows nothing about what the word means. The timeout is
`wait_ticks`, by the rule every timeout crossing a boundary follows.

### Killing a process that has threads

The step where the difficulty is, and the order that makes it safe:

1. **Mark it**, under a lock of the process's own.
2. **Wake every thread** from every wait it can be in - not only IPC, as
   today - and send an interrupt to each core running one, so it enters the
   kernel now rather than at its next tick.
3. **Each thread leaves by itself** on its way back to user mode, seeing
   the mark, and touches nothing of the process's as it goes.
4. **The last one out turns off the lights**: a count of live threads, and
   whichever brings it to zero releases endpoints, capabilities and memory
   and destroys the address space - by then loaded on no core.

The same path serves a process that exits normally while its workers are
still running: step 2 onwards.

### The register that is a thread's own

`TPIDR_EL0` on AArch64 and the FS base on x86 are saved and restored with
each thread, one register on each board, and point at the thread's own
block: its `errno`, its identity, and whatever a library keeps per thread.
The switch pays one register move; the ThinkPad's processor has the
`FSGSBASE` instructions, which avoids a slower model-specific register
write on x86, and is to be checked before it is relied on.

## Threads where they make sense

**In C, wherever there is a loop over bytes.** A kit's decoder, mixer or
rasteriser, Music decoding ahead, the window manager composing on one core
while another answers input. A small kit - start, wait, a lock, a
single-producer single-consumer channel - over the syscalls above, and a
thin `pthread` inside the libc for ports that ask for one. That is allowed:
*compatibility inside a process yes, at system level never* (`CLAUDE.md`),
and it is what lets FFmpeg's own frame threading decode video on several
cores for the player (`roadmap.md` 4e).

**In Lua, a thread is an interpreter of its own.** One `lua_State` cannot be
run by two threads, and a lock around it would make them take turns. So a
Lua thread runs its own file in its own state on its own core, and threads
talk through channels: a value sent is copied - by the serialiser the
runtime already has - and received on the other side. No shared tables, so
the races a lock exists for cannot be written in Lua at all. **Each state
has its own collector**, so a worker collecting its garbage never pauses the
thread that draws.

**BeOS's idea, kept; its mechanism, not.** BeOS promised that a window never
waits on work, and bought it with two threads per window, in C++, with
locks - which is also why it was hard to program. Here the window's thread
draws and answers input and nothing else; work goes to workers; a worker's
answer arrives in the window's event loop as an event, as a click does.
Coroutines stay what a program uses to wait on many things at once.

## What does not change

- **A thread has a home and does not migrate.** That is what lets IPC
  release its lock before the switch (`smp.md`), and it holds per thread:
  the threads of one process simply have different homes. `design.md` 4.5's
  last paragraph suggests clients migrating toward a server, which
  contradicts this and is to be corrected with it.
- **The scheduler.** Threads are what it has always scheduled; bands,
  inheritance and preemption are per thread already.
- **No heap for kernel objects.** Threads come from the pool; a process's
  threads are linked through a field in `struct thread`, not a list
  allocated for them.

## The steps, each checked on its own

The order `smp.md` taught: every step checkable before the next depends on
it, and the steps that cannot fail loudly come before the one that can.

0. **DONE on 19 September - the two things wrong today** (`testing.md`
   18.111). A shared region's count under the pool's lock, and the slot a
   failed spawn leaked given back. A region's count raced from every core,
   and a bad image offered more times than there are slots; unfixed, the
   first panicked with a double free on both boards. **And a third, found
   reading for step 1**: an endpoint's slot was claimed without a lock, so
   two programs starting on two cores could share one; claimed under the
   endpoint's own lock now.
1. **DONE on 19 September - capabilities move from the thread to the
   process**, with a lock (`testing.md` 18.112). One thread still, so
   nothing should behave differently, and the whole gate was the check. A
   capability in flight now carries its generation, so one destroyed on the
   way arrives stale.
1b. **Pools that grow, and no limit that is not the machine's** - **the
   thread pool DONE on 19 September** (`testing.md` 18.113), **processes
   and address spaces the same evening, on one pool written once**
   (`kernel/pool.c`, 18.114); **endpoints and regions** (18.115); **a
   region's size, what a process may map, and the reserve** (18.116); **a
   process's capability table** (18.117). **Step 1b is done.** Every pool
   the kernel keeps - processes, threads, endpoints, regions - grows by a
   slab of slots when it is full and never gives a slab back, up to a
   ceiling derived from RAM; and the limits that were numbers become the
   machine's: a region's size, what a process may map, and a process's
   capability table, which grows the same way. Measured today, a thread slot
   is 4.2 KB and a process slot 1.4 KB, whether used or not; the thread pool
   is 48, one per process and sixteen over, which threads would exhaust at
   once - so this comes before the second thread does. **And a reserve**: the
   last slots of each pool are kept for starting a process, so a program
   that makes threads until the ceiling cannot stop anybody opening
   Processes to end it. Tests: a pool driven past its old size and back, a
   process with more threads than the old pool had slots, a region past 32
   MB, and a runaway that still leaves room to start one more process.

   **What it touches, found by doing the first one.** One growable pool,
   `kernel/pool.c`, written once and used by every pool - slabs, a
   directory made at boot, a count published with release - rather than the
   same forty lines four times; the thread pool, done first by hand, moves
   onto it. The process pool drags two things with it: **address spaces**,
   a pool of their own in each `arch/` that has to be at least as large -
   a spawn once failed at eleven because it was not - and **the process
   list programs read**, which `sys_user.c` fills through a buffer of 32
   entries, so past thirty-two processes the Processes window would stop
   listing them without a word. A region's 32 MB comes from the sixteen
   index pages a `struct memobj` holds; a directory page above them makes a
   gigabyte. And each process's capability table grows by pages that go
   back when the process ends.
2. **DONE on 19 September - the thread's own register** saved and restored,
   and `errno` moved into the block it points at (`testing.md` 18.119). One
   thread still. `SYS_SET_TLS` is how a thread says where its block is; the
   two boards differ in whether the kernel must also save the register, and
   the hardware decides that rather than taste.
3. **A second thread, on the same core.** `SYS_THREAD_CREATE`, `EXIT` and
   `WAIT`; two threads counting into the same memory; a thread ending and
   being waited for. On one core, so a failure is deterministic.
4. **On other cores.** The same tests with each thread homed elsewhere, and
   *Work spreads* again: one process with four busy threads reads 100% on
   four cores.
5. **The futex.** A lock contended from four cores, a ping-pong between two
   threads, and a wake nobody waits for measured costing nothing.
6. **Kill and exit with threads anywhere** - asleep, in a call, running on
   another core, and while a sibling unmaps memory. The hardest step, so
   its tests are the kind that found SMP's bugs: kill in a loop under load,
   and `make stress` asking afterwards whether everything came back.
7. **The libc**: `malloc` under a lock, `errno` per thread; the C thread
   kit.
8. **Lua threads**: `use("/kits/thread")`, a state each, channels, and a
   worker's answers as events in a window's loop.
9. **The first users**: the video player's decoder, Music decoding ahead,
   the Game Kit's rasteriser - each measured before and after, because a
   thread that buys nothing is complexity for nothing.

## Decisions that are Diego's

**Taken on 19 September, as proposed** - Diego, having read this page:
"Great let's do it". So a process ends when its first thread returns; stacks
made by the kernel with a guard; and step 0 first. **Except the second**:
"Let's make sure we don't have caps on thread count, process count or else
like we had in the past", and "We should be able to grow as needed on
processes and threads just like beos or Linux" - so there is no number of
threads a process, and the pools grow (step 1b). The four as they were put:

1. **When a process ends**: when its first thread returns, as proposed, or
   when its last thread does.
2. ~~**Sixteen threads a process** to start with.~~ No: no limit a
   process, and pools that grow.
3. **Stacks made by the kernel**, with a guard, as proposed - or memory the
   program maps and hands in, which is more flexible and loses the guard
   unless the program leaves one.
4. **Step 0 now**, ahead of the rest, because it is wrong today whatever is
   decided about threads.
