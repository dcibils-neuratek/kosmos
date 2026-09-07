# SMP

**Steps one and three are done. The rest is not.** This is what it would
take, counted against the code as it stands rather than estimated, because
the last thing written down about SMP was wrong for two years: `CLAUDE.md`
claimed a per-CPU struct, `TPIDR_EL1` and a per-CPU runqueue from the
repository's first commit, and none of the three had ever existed.

Two of those three exist now. `kernel/percpu.h` holds the struct,
`TPIDR_EL1` holds the pointer to it on AArch64, and the runqueue is still a
global - which is step five below. **And there are four instruction streams
in the machine**, three of them parked: step three below, done out of order
and for a reason recorded there.

**And re-auditing this document against the code found something it had
missed**, which is the argument for auditing against code rather than
against prose: the list of six things that have to become per-CPU was
seven. `preempt_pending` in `thread.c` is a statement about *this core's*
return path and was a file-scope `bool`.

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

**The kernel is 5,769 lines of code**, by `make size`, which counts code
and not the comments - this codebase is more than half comments on purpose.
Small enough that this is tractable, and the reason to do it here rather
than read about it.

---

## What has to become per-CPU

Six things, and they are all currently one global each. This is the whole
of what "a per-CPU struct" means, made concrete:

| what | why it is per-CPU | state |
|---|---|---|
| `current` | which thread is running - the fundamental one | **moved** |
| `idle_thread` | each core idles independently | **moved** |
| `idle_ticks`, `busy_ticks` | load is measured per core or not at all | **moved** |
| `preempt_pending` | a switch owed on *this core's* way out of an exception | **moved** - and this document had missed it |
| `owner` in `arch/*/fp.c` | who owns the FP registers *on this core* | still a global |
| the runqueue in `sched_prio.c` | `head[]`, `tail[]`, `occupied` | still globals; step 5 |
| the TSS / kernel stack | where a ring-3 entry lands | still one; x86 only |

And the register that finds them: **`TPIDR_EL1` on AArch64, the `GS` base
with `swapgs` on x86-64.** The first is written; the second is not, and the
asymmetry is larger than it looks.

`TPIDR_EL1` is *banked*: EL0 cannot see it or change it, so it is set once
per core at boot and read from anywhere afterwards. **No entry path is
touched at all.** x86 has one `GS` shared between ring 3 and ring 0, so the
same trick needs `swapgs` at every entry and every exit, in `vectors.S` and
`user.S`, with the classic hazard of an exception arriving between the two.
That is real surgery and it belongs with x86's second core rather than
before it - so that board answers from `cpus[0]` today and says so in
`arch/x86_64/cpu.h`.

This paragraph used to say "neither is written" and that both would touch
every exception entry. Half of that was wrong about AArch64, which is why
the first step turned out to cost one store at the top of `kmain` rather
than a pass over the vectors.

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

1. ~~**The per-CPU struct and the register that finds it.**~~ **Done.**
   `kernel/percpu.h`, `NR_CPUS = 1`, `TPIDR_EL1` on AArch64 and an honest
   static on x86-64. Nothing behaves differently and 130 checks say so.

   Two things worth keeping from it. `current` became a *macro* over the
   field rather than twenty-nine edited call sites, which is Linux's idiom
   and for Linux's reason: the sites were correct and a large diff whose
   only content is a change of spelling is where a real change hides. And
   `percpu_init` is the first line of `kmain`, before `hal_early_init`,
   because `thread_current` reads through it and the fault handler asks for
   the current thread on its way to reporting - so an exception arriving
   before it would take a second fault instead of printing. Removing that
   one line panics at boot, which is the right loudness.
2. **Locks, with one core.** Take them, release them, and let them be
   uncontended. The kernel is still correct at every step and `make test`
   still passes - which is what makes this safe to do incrementally.

   **Deferred, deliberately, and step three was done in front of it.** The
   reason is that this step is *untestable* in the state the kernel is in.
   Both boards already enter the kernel with interrupts masked - AArch64 by
   architecture, x86 through an interrupt gate - so on one core the pools
   genuinely need no lock, and every lock added here would be a lock that
   is never contended, never fails, and is checked by nothing. That is not
   incremental progress; it is a large diff on faith, with `make test`
   unable to tell a correct one from a broken one.

   Step three has the opposite property. A parked core touches no shared
   structure, so it needs none of this, and it is the only way to find out
   whether the per-CPU register from step one is actually per-core - which
   step one could assert and could not check.

   So the locks come back when there is a second *scheduling* core to
   contend for them, which is step four. **This is a departure from the
   order above and it is written here rather than quietly done**, because
   the order was reasoned about once and this changes it.
3. ~~**A second core, doing nothing.**~~ **Done.** PSCI `CPU_ON` into a
   park loop, in `kernel/smp.c`, with `_secondary_start` in `boot/start.S`
   and `hal_cpu_count` / `hal_cpu_on` under it. `-smp 4` boots four
   processors, one scheduling; `-smp 1` boots one and says so.

   It proves the four things nothing else could: the firmware call works
   and the entry address was right; a core started this way can turn its
   own MMU on with tables it did not build; `TPIDR_EL1` really is per-core,
   which the suite now checks by asking core *i* for its own index; and the
   machine survives having two instruction streams in it.

   **Two bugs, and both were ordering rather than concurrency**, which is
   worth recording because it is not what one braces for. `smp_start_others`
   was called fifty lines before `mmu_init`, so the secondaries enabled
   translation with tables that did not exist yet and never arrived - the
   boot said "1 in the kernel" and nothing else went wrong, which is the
   quietest possible failure. And `thread_cpu_count` returned `NR_CPUS`,
   so the machine claimed to be scheduling on four cores while three of
   them were in `wfi`; `NR_CPUS` is how many slots exist, and how many are
   scheduling is a different number that is still one.
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

**And the machine boots with four processors by default**, not only under
`make test`: `SMP ?= 4` in the Makefile, `make SMP=1 qemu` for the one-core
machine. Step three is the argument for it. Its worst bug produced no
fault, no hang and no wrong behaviour - only a boot line saying 1 where it
should have said 4 - so a bring-up path that runs only when the suite runs
is one that is checked once a session by somebody reading a number they
just wrote. Three cores in `wfi` cost a QEMU thread that is never
scheduled.

Two checks in the guest suite, and the second is the one that matters:

- *the machine says how many processors it has* - `hal_cpu_count` against
  what QEMU was told, which on `-smp 1` cannot distinguish a working
  discovery from a hardcoded 1, and is why the suite boots four.
- *every processor claimed its own slot* - `percpu_at(i)->index == i` for
  every online core. **This is what step one could not check.** On one
  processor every answer is the same answer, so a per-CPU register and a
  global are indistinguishable; with four, a `TPIDR_EL1` that was somehow
  shared would show up here and nowhere else.

What it cannot do is find an ordering bug, and nothing can reliably. The
answers are: run it a great many times, keep the discipline simple enough to
review by reading, and prefer the shape that does not need the barrier.

---

## Cores that are not alike, and why a count is the wrong question

**Every machine this is planned for after QEMU has processors that differ
from each other**, and the plan above quietly assumes they do not.

The Alienware in `docs/targets.md` is an Alder Lake i7-12700H: six
performance cores with two threads each, eight efficiency cores with one,
twenty hardware threads in total. The P-cores have 48K of L1 data cache and
reach 4.7 GHz; the E-cores have 32K and reach 3.5, and four of them share
one 2 MB L2. ARM has had the same shape for longer under a different name -
big.LITTLE, and the Pi 5's four A76s happen to be uniform only because it
is a small part.

So there are **three** ways two hardware threads can differ, and they are
not the same problem:

| | what differs | what it costs to ignore |
|---|---|---|
| **kind** | a P-core against an E-core | the compositor lands on the slow one |
| **siblings** | two threads on one P-core | two hot threads share one core's execution units while a whole core idles |
| **cache** | four E-cores share an L2 | threads that share data are placed apart |

`hal_cpu_count` answers none of that, and it is right not to: it exists
because the boot log had a caller for it today, and `CLAUDE.md` is explicit
that an interface written ahead of a second real target is the shape of the
first target with generic names.

**What matters now is not closing the design against it.** Three places
would have to change and none of them has to change yet:

- **`struct percpu` gains a kind.** It is the kernel's own struct with no
  ABI, so this costs a field the day something sets it. Not before: a field
  nothing reads is indistinguishable from a bug.
- **`sysinfo` carries it out.** `cpu[]` is already an array of a declared
  struct, and `struct cpuload` gaining a kind beside its two counters is
  additive.
- **The runqueue split at step 5 has to be per-CPU rather than per-band.**
  This is the one that would be expensive to get wrong: a design where a
  thread is enqueued centrally and pulled by whichever core is free cannot
  express "this one belongs on a P-core", and retrofitting affinity into it
  is a rewrite rather than an addition.

**And the policy is userland's, which is the answer this system already
has.** The microkernel keeps threads and priorities; it does not decide
which core a thread wants, any more than it decides what a file is. Where
each board reads the kind from is `arch/`'s business - `CPUID.1A` on x86,
where `EAX[31:24]` is 0x40 for a core and 0x20 for an atom; the per-core
MIDR and the device tree's `cpu-map` on ARM.

**This is the part of SMP worth doing here rather than reading about.** A
priority-banded, preempt-on-wake scheduler descended from BeOS has never
been asked which of two unequal processors a thread should run on, because
in 1998 there were no unequal processors. The answer is not in the
literature this design came from.

Still out of scope for the first working SMP, and deliberately: symmetric
and correct first, on cores that are all alike, which is exactly what QEMU
gives. The note above is so that "correct" does not quietly mean "assumes
they are alike" in a structure that cannot later say otherwise.

---

## Not in scope

**Not** load balancing across cores beyond "run the highest-priority ready
thread here". **Not** CPU affinity. **Not** lock-free anything. Those are
optimisations of a thing that has to exist and be correct first, and this
project has been bitten before by optimising ahead of a measurement.
