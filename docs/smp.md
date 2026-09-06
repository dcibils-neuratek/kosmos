# SMP

**Nothing here is written yet.** This is what it would take, counted against
the code as it stands rather than estimated, because the last thing written
down about SMP was wrong for two years: `CLAUDE.md` claimed a per-CPU
struct, `TPIDR_EL1` and a per-CPU runqueue from the repository's first
commit, and none of the three has ever existed.

---

## Where it stands

One number that matters: **there is not a lock or an atomic anywhere in
`kernel/` or `arch/`.** Not an `ldxr`/`stxr`, not a `lock cmpxchg`, not a
compare-and-swap. Mutual exclusion today is a single sentence - *there is
one core, and the kernel runs with interrupts masked* - and every data
structure in the kernel is built on it.

That is not a criticism of the code. It is the correct design for a
uniprocessor and it is why `ipc.c` is 801 lines instead of two thousand.
It does mean SMP is not a feature to add beside the others: it changes the
assumption the whole kernel rests on.

**The kernel is 8,683 lines.** Small enough that this is tractable, and the
reason to do it here rather than read about it.

---

## What has to become per-CPU

Six things, and they are all currently one global each. This is the whole
of what "a per-CPU struct" means, made concrete:

| today | where | why it is per-CPU |
|---|---|---|
| `current` | `thread.c:25` | which thread is running - the fundamental one |
| `idle_thread` | `thread.c:462` | each core idles independently |
| `idle_ticks`, `busy_ticks` | `thread.c:463` | load is measured per core or not at all |
| `owner` | `arch/*/fp.c:37` | who owns the FP registers *on this core* |
| the runqueue | `sched_prio.c:76` | `head[]`, `tail[]`, `occupied` |
| the TSS / kernel stack | `arch/x86_64/gdt.c` | where a ring-3 entry lands |

And the register that finds them: **`TPIDR_EL1` on AArch64, the `GS` base
with `swapgs` on x86-64.** Neither is written. This is the smallest piece of
the work and it touches the most files, because every exception entry has to
establish it before anything else runs.

**The x86-64 port already paid part of this forward**, and it is the one
thing that transfers: `user_rsp` was a global holding the interrupted stack
pointer, it looked like per-CPU state, it was per-*thread*, and one core was
enough to prove it wrong - a process that blocked in IPC let another run and
the second overwrote the first's. The distinction is now understood and
written down. It was found the expensive way, which is the only way it gets
found.

---

## What has to be locked

Five shared structures, and they are not equally hard.

**The easy ones are the pools.** `threads[]`, `processes[]`, `endpoints[]`,
`objects[]` and the physical bitmap are arrays with an allocation function
each. One lock apiece, taken around allocate and free, is correct and
uncontended - a process is created rarely and a page is allocated at a rate
the machine can afford a lock for.

**`current` is not a lock problem**, it is the per-CPU problem above.

**Capabilities are already nearly safe**, and that is luck worth naming:
they live in `struct thread` as `caps[CAPS_PER_THREAD]`, so a thread's table
is touched by that thread and nobody else - except when one is granted,
which is the one path that needs care.

**IPC is the hard one**, and it is hard for the reason microkernels are:
`send`, `receive` and `reply` each touch *two threads and an endpoint* at
once, and the states they move through - blocked-sending, blocked-receiving,
ready - have to change together or a message is delivered twice, or to a
thread that has since died. 801 lines of it, and every one of them currently
assumes nothing else is running.

One thing about the current design helps and is worth keeping: **`deliver`
wakes the peer and enqueues it rather than switching straight to it.** A
direct handoff would be faster and would mean one core writing another
core's stack. The slower shape is the one that survives SMP.

---

## Bringing the other cores up

**ARM is nearly free.** PSCI is already there - `hal/qemu-virt/power.c` uses
it to turn the machine off - and `CPU_ON` is the same call with a different
function id and an entry point. The GIC already gives software-generated
interrupts, which is what an IPI is.

**x86-64 is the expensive half, and it is a new driver.** Kosmos drives the
8259 PIC. SMP needs the **local APIC** - for IPIs, and for a per-core timer,
because one 8253 cannot tick four cores - and it needs **ACPI MADT parsing**
to find out how many cores there are and what their APIC ids are. That is a
table walker and an interrupt controller, neither of which exists, and it
is most of the reason the two boards are not equal work.

Then `INIT`-`SIPI`-`SIPI`, a real-mode trampoline page under 1 MB, and the
same long-mode climb `boot/x86_64/start.S` already does once.

---

## The order, and why ARM first

**Do it on AArch64 first**, and this is the one recommendation here that
would be expensive to get wrong.

x86-64 is total-store-ordered: it forgives reorderings that AArch64 does
not. A locking discipline that is correct on ARM is correct on x86; one
developed on x86 will be *wrong* on ARM in ways that appear once every few
thousand boots, which is the most expensive class of bug this project has
(`hal.md` says so already, about a hazard that has been dormant precisely
because there is one core).

So: ARM first, where the memory model is strict enough to punish a mistake
while it is still cheap to find. x86 second, where bring-up is the work and
the concurrency is already settled.

Then, in dependency order:

1. **The per-CPU struct and the register that finds it.** Single core still,
   with `NR_CPUS = 1`. Nothing behaves differently; everything moves.
2. **Locks, with one core.** Take them, release them, and let them be
   uncontended. The kernel is still correct at every step and `make test`
   still passes - which is what makes this safe to do incrementally.
3. **A second core, doing nothing.** PSCI `CPU_ON` into a park loop. Proves
   bring-up, the trampoline, and that the per-CPU register is right.
4. **The idle thread on the second core.** It schedules, ticks and idles.
5. **Per-CPU runqueues.** The scheduler is already a vtable
   (`struct scheduler` in `sched.h`), so this is a policy beside
   `sched_prio.c` rather than surgery on it - the one place the existing
   design genuinely is ready.
6. **IPIs**, for preempting a remote core when a wake outranks what it is
   running. Without this a high-priority thread waits for the other core's
   quantum, and the whole responsiveness argument dies.
7. **TLB shootdown.** `as_switch` invalidates locally (`tlbi vmalle1`);
   with two cores in one address space, unmapping needs the other core told.

---

## What it would cost, honestly

The x86-64 port was about a hundred and ten lines of assembly and one
`#if`, because it was the same design against different registers. **SMP is
not that.** It changes the invariant every file in `kernel/` was written
against, and the work is spread thin across all of them rather than
concentrated in a new directory.

The bring-up is a week of fiddly and well-documented work. The locking is
where the time goes, and IPC is where the bugs will be.

---

## How it would be tested

`make stress` already exists and already asks the right question - use the
machine hard, then ask `sysinfo` whether it gave everything back. It becomes
the SMP test almost unchanged, because a lost lock shows up as a leaked
slot.

What it cannot do is find an ordering bug, and nothing can reliably. The
answers are: run it a great many times, keep the discipline simple enough to
review by reading, and prefer the shape that does not need the barrier.

---

## Not in scope

**Not** load balancing across cores beyond "run the highest-priority ready
thread here". **Not** CPU affinity. **Not** lock-free anything. Those are
optimisations of a thing that has to exist and be correct first, and this
project has been bitten before by optimising ahead of a measurement.
