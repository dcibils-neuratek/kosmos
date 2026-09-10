# SMP

**Where this stands: six of seven steps are done. The kernel is SMP-aware
and the placement policy is not switched on.** The map of what exists is
*As built*, further down; what is left and what is holding it is *What is
left*. Everything before those two sections is the reasoning, because this
is the hardest thing in the kernel and the reasoning is most of the value.

---

## 1. The four words this document rests on

If you have the tree open and know the kernel, skip to *As built*. This
section exists because SMP is where four ideas that have been quietly
separate for the whole project suddenly interact, and the interaction is the
hard part rather than any one of them.

### A thread

**A thread is one instruction stream and the stack it runs on.** In Kosmos
it is `struct thread` in `kernel/thread.h`: a saved register context, two
stacks (its own and one for exceptions), a state, a priority, and a pointer
to the process it belongs to. Threads are taken from a fixed array,
`threads[THREAD_MAX]` — there is no allocator in this kernel, so there is a
pool and nothing else.

A thread is either **running** (on a processor now), **ready** (wants a
processor), or **blocked** (waiting for something and not asking). Those
three words do the whole of the work in this document, because SMP is
largely the question of *who may change that word, and when*.

Two things about threads here are unusual and both matter later:

- **A process has exactly one thread.** There are no threads inside a
  process; a Lua program that wants concurrency uses coroutines. So "thread"
  and "process" are nearly the same population, and parallelism across cores
  means *different processes* running at once rather than one program
  spreading itself.
- **The kernel has threads of its own**, and the idle thread is one of them.
  Which turns out to matter enormously: an idle thread per processor is what
  makes a core with nothing to do a normal state rather than an error.

### Scheduling

**Scheduling is choosing which ready thread runs next.** The mechanism in
Kosmos is a *runqueue* — a linked list of ready threads — and a *policy*
that decides the order. The policy is a vtable (`struct scheduler` in
`kernel/sched.h`) with four operations: put a thread in, take the next one
out, is there anybody waiting, and has this one had long enough.

The default policy is **strict priority bands with immediate preemption**,
borrowed from QNX. Five bands; the highest ready thread runs; a thread that
becomes ready and outranks the running one takes the processor at the next
exception rather than when a quantum expires. That last property is the
whole responsiveness argument, and it is the one SMP threatens most
directly — "the next exception" is a statement about *a* processor, and
with four of them the thread that should preempt may be looking at a core
that is not the one it is on.

A **context switch** saves one thread's registers and restores another's
(`arch/aarch64/switch.S`). The subtle part, and it becomes load-bearing
later: **`context_switch` returns on a different stack.** The instruction
after it executes as the *new* thread. Anything you were holding across it
is now held by somebody else.

### IPC

**IPC is how two processes talk, and in a microkernel it is how everything
happens.** There is no shared memory by default and no global namespace: a
process reaches another only through an *endpoint* it was handed, and
reaching it means sending a message and waiting for a reply.

Kosmos's IPC is a **rendezvous**: nothing is buffered in the kernel. A
sender blocks until a receiver takes the message; a receiver blocks until
one arrives. Each endpoint keeps three queues — senders waiting to be
collected, receivers waiting for work, and senders whose message was taken
and who are now waiting for a reply.

This is why IPC is the hard subsystem for SMP, and the reason is worth
stating precisely: **a single IPC operation moves two threads and an
endpoint through states that have to change together.** A send takes a
thread off one queue, puts it on another, writes a message into a second
thread, marks that thread ready, and blocks the first — five changes that
are one event. Interleave them wrongly and a message is delivered twice, or
to a thread that has moved on, or to nobody.

And the volume matters. A shell command here is dozens of round trips: the
shell asks the filesystem, which asks the block device, and the answer comes
back the same way. Anything that makes a round trip slower makes the whole
system slower, which is why the locking decisions later are so concerned
with what is *not* shared.

### Interrupts, and what "masked" means

An interrupt is the hardware taking the processor away from what it was
doing. The timer raises one at 250 Hz and that is the scheduler's heartbeat;
a device raises one when it has something to say.

**A processor can mask them** — refuse to be interrupted for a while. That
one fact is the whole of how this kernel has stayed correct until now:

> The kernel runs with interrupts masked, so whoever is inside it cannot be
> interrupted, and there is only one processor, so nobody else is inside it
> either.

Every data structure in `kernel/` was written under that sentence. Masking
is a statement about **one** processor, which is exactly why it stops being
enough.

---

## 2. What SMP is, and how it is achieved

**Symmetric multiprocessing**: several processors that are equals — same
instruction set, same view of memory, any of them able to run any code. The
word *symmetric* distinguishes it from arrangements where one processor is
in charge and the others are subordinate.

What it buys is **throughput**, not speed. One thread runs at exactly the
same rate on four cores as on one, and a program that cannot be divided
gains nothing. What it buys *this* project is different and is the reason
it is worth the trouble:

> When the machine is busy, is there a processor free to answer me?

That is a responsiveness question, not a throughput one. It is why a
four-core machine can feel dramatically better while being no faster at
anything you can measure, and it is the same bet BeOS made in 1995 when it
shipped on a dual-processor box and made responsiveness its entire pitch.

### Concurrency is not parallelism

Kosmos was concurrent for a year before it was parallel. Dozens of threads,
preemption, priority bands, IPC — all of it worked on one core.

- **Concurrency is structure**: several things in progress, interleaved.
- **Parallelism is execution**: several things happening at the same instant.

Almost every bug in this document is code that was correct as the first and
wrong as the second. Interleaving means a switch happens *between* two
instructions; two processors means two instructions happen *at once*.

### How it is achieved, in four parts

Making a machine symmetric is not one thing. It is four, and Kosmos does
them in this order because each can be checked before the next:

1. **Start the other processors.** They come out of reset held, and firmware
   (PSCI on ARM) starts one at an address you give it. It arrives with
   nothing: no page tables enabled, no exception vectors, no idea who it is.
2. **Give each one what is private to it.** A surprising amount of a
   processor is per-processor and the code reads identically either way: its
   exception vector, its interrupt controller interface, its timer
   comparator, and a register pointing at its own state. Anything the kernel
   kept as one global that is really per-processor is a bug the moment there
   are two.
3. **Protect what is genuinely shared.** The pools, the queues, the device
   rings. This needs an *atomic* — an operation that reads and writes with
   nothing able to intervene — because masking interrupts no longer excludes
   anybody but yourself.
4. **Let them talk.** A processor that puts work where another will find it
   has to be able to say so, or the other notices at its own next tick. That
   is an inter-processor interrupt, and it is worth a measured 25x here.

### Why it is the hardest change a kernel makes

Adding a filesystem is a new directory. Adding a second architecture cost
this project about a hundred and ten lines of assembly and one `#if`,
because it was the same design against different registers.

SMP is not that. **It changes the invariant every file in `kernel/` was
written against**, and the work is spread thin across all of them rather
than concentrated anywhere. There is no `smp/` directory. There is a line in
the thread pool, a line in the page allocator, three in IPC, and a hundred
places where nothing needs to change and you have to read them to know that.

And the failures do not announce themselves:

- A pool slot handed out twice is not a crash. It is two processes sharing
  an address space and a fault somewhere else, minutes later.
- A lost wakeup is not an error. It is a thread that never runs again, which
  looks exactly like a hang.
- A stale TLB entry is not a fault. It is a *successful* read of somebody
  else's memory.
- A message delivered to a thread that has moved on is not a panic. It is
  data going to the wrong place.

All of them are rare — a window three instructions wide, hit by a timer at
250 Hz — which means a passing test suite is evidence of very little. That
is why the audit in this document exists, and why several of the fixes here
were confirmed by deliberately breaking them.

---

## 3. How Kosmos does it, and why

The philosophy in `CLAUDE.md` decided most of this before any of it was
written. Four principles did the work:

**No allocator in the kernel** meant every shared structure is a fixed pool
with a scan-and-claim allocator — and *every one of them* had the same bug:
scan for a free slot, build in it, mark it taken last. That is a window
hundreds of instructions wide. One shape of bug, five places, one fix.

**Control by message, data by shared memory** meant the kernel is small —
under six thousand lines of code — and most of the operating system is at
user level sharing nothing. So the locking here covers the kernel and *not*
the filesystem, the desktop or the browser. The hard part of SMP in a
monolithic kernel is that every subsystem is in one address space touching
the same structures; here they are separate processes that a second core can
simply run.

**Responsiveness is a design goal, not a later optimisation** decided the
IPI. A cross-core wake that waits for the target's next timer tick costs
4.7 ms; a shell command is dozens of round trips; four processors would have
been *slower* than one. So the interrupt exists, and its value is a
measurement rather than an argument.

**One thing at a time, small and verifiable** decided the order — and the
order was the most important decision in the whole exercise. The plan's own
second step was *take locks with one core and let them be uncontended*, and
it was **skipped**, because it is the one increment nothing can check: a
lock that does nothing passes every test on a machine where nothing
contends. The step that came instead — a second core that parks and touches
nothing — needs no locks at all and is the only way to find out whether the
per-CPU register is genuinely per-core.

Two decisions of Kosmos's own follow from all that, and they are the ones a
reader should take away:

- **Every lock masks interrupts**, and there is no second flavour, because
  the structures worth locking are reached from a syscall *and* an interrupt
  handler alike.
- **A thread has a home processor and does not migrate**, which is what lets
  IPC release its lock *before* the context switch instead of handing it to
  the next thread — the textbook answer, and unnecessary here.

Both are argued at length in *The decisions* below.

---

---

## As built

**What exists, in the tree, today.** The rest of this document is the plan
and the reasoning; this section is the map. Every claim here is a file you
can open.

### A processor's own state

`struct percpu` in `kernel/percpu.h`, found through `TPIDR_EL1` on AArch64
and through the `GS` base on x86-64, which `swapgs` exchanges at every entry
from ring 3 and every return to it.
`NR_CPUS` is 4 — how many *slots* exist, which is not how many the machine
has (`hal_cpu_count`), nor how many run kernel code (`smp_online`), nor how
many schedule threads (`thread_cpu_count`). Those four numbers are different
questions and three of them cross to userland in `sysinfo`.

Per core: `current`, the idle thread, `idle_ticks`/`busy_ticks`,
`preempt_pending`, and the lazy-FP owner. Per core in hardware and now in
software too: the exception vector (`VBAR_EL1`), the GIC redistributor and
CPU interface, and the generic timer's comparator and tick counts.

### How a processor comes up

`kernel/smp.c`, `boot/start.S`, `hal/qemu-virt/power.c` - and on x86-64
`hal/pc/cpu_on.c`, `hal/pc/trampoline.S`, `boot/x86_64/start.S` and
`arch/x86_64/entry.c`.

1. `hal_cpu_count()` asks PSCI `AFFINITY_INFO` about processor 0, 1, 2 …
   until the firmware refuses. There is no "how many" call and no
   device-tree parser here; counting up to the first refusal *is* the count,
   and it starts nothing.
2. `smp_start_others()` calls `hal_cpu_on(cpu, entry, cpu)` — PSCI `CPU_ON`
   — with the entry address from `cpu_secondary_entry()` in `arch/`. **The
   board knows how to start a processor and not where it should land**.
   On x86-64 the board's half is `INIT` and two `STARTUP` IPIs through the
   local APIC's command register, to the id the MADT listed, carrying the
   vector of a real-mode page under 1 MB - `hal/pc/trampoline.S`, copied into
   place only where the loader's memory map says that page is RAM. It reaches
   flat 32-bit mode and jumps to `_secondary_start32`, where the
   architecture's half begins: the long-mode climb `start.S` made once for
   core zero, on the same boot page tables, a stack by index, and
   `x86_secondary_entry` - this core's GDT and TSS, `mmu_enable_here`, the
   `syscall` MSRs and FP - before `secondary_main`.

   **Each of those steps writes a number into a word in the trampoline page,
   and the board reads it back.** Every processor asked about gets one
   `cpu_on:` line: why it was refused, the last stage it reached, or how many
   milliseconds reaching the kernel took - and `kernel/smp.c` adds `was
   started and never arrived` for one that got that far and no further. The
   first real machine counted eight processors, started none, and said only
   `0 of the others in the kernel too`.
3. `_secondary_start` sets a known `SCTLR_EL1`, traps FP, takes its two
   stacks out of `secondary_stacks[]` by index, calls `mmu_enable_here()`
   and jumps to C. Turning translation on is the last step of the
   *architecture's* entry path and lives in `boot/`, not in `kernel/smp.c`
   — which is how the x86-64 link error found it.
4. `secondary_main()` claims its `struct percpu`, installs its own vector
   table, runs `hal_irq_init_here()` and `hal_timer_init_here()`, adopts the
   idle thread core zero reserved for it, publishes itself with a release
   barrier, unmasks, and enters the same idle loop core zero runs.

Ordering matters exactly once: `smp_start_others()` must be called after
`mmu_init()` and after `hal_timer_init()`, and `kernel/main.c` says so at
the call site because both failures are silent.

### The lock

`kernel/spinlock.h`, on `cpu_lock_try` / `cpu_lock_release` in
`arch/<name>/cpu.h`.

- **Every lock masks interrupts.** There is no second flavour. The
  structures worth locking are reached from a syscall *and* from an
  interrupt handler, so a lock held with interrupts on is a self-deadlock
  waiting for a tick.
- The spin is **bounded**, and the panic names the lock and the processor
  holding it — two different failures.
- `spin_panic` writes to the UART directly, past the console, because the
  lock that deadlocked might be the console's.

Locked today: `threads[]`, `processes[]`, `objects[]`, `spaces[]` on both
boards, the physical page bitmap, every endpoint, every runqueue, and the
console.

**Every pool claims its slot inside the lock.** They all used to scan, build,
and mark taken *last* — a window hundreds of instructions wide. `THREAD_CLAIMED`
exists to be neither of the two states the allocator scans for.

### The scheduler

`kernel/sched.h`, `sched_prio.c`, `sched_rr.c`, `kernel/thread.c`.

One runqueue per processor. The vtable names the queue —
`enqueue(cpu, t)`, `pick_next(cpu)`, `ready(cpu)` — because "enqueue" is not
a complete instruction on a machine with more than one, and the caller is
frequently not the core the thread will run on.

**A thread has a home** (`t->sched.cpu`), assigned when it is created, and
does not migrate. The queue lock is held by `kernel/thread.c` and never by a
policy, and **never across `context_switch`**, which returns on a different
stack.

`thread_block_and_release(lock, flags)` is how IPC blocks: it marks the
thread blocked, picks a successor, releases the caller's lock at the instant
the thread is both findable and blocked, and only then switches. It can
release *before* the switch — where the textbook answer is to hand the lock
to the next thread — precisely because a thread has a fixed home. The
comment on it is where that cost returns if migration is ever added.

An empty runqueue is not a deadlock any more; it is an idle processor, and
`thread_block` falls back to that core's idle thread.

### IPC

`kernel/ipc.c`. **One lock per endpoint**, because no operation touches two.
Held from before the hand-off until the sender is both on `awaiting_reply`
and blocked — those become true at different moments, which is why the
release happens inside `thread_block_and_release`.

Three re-checks under the lock, all one pattern: `waiting_on` names the
endpoint, so it must be read *before* there is a lock to take; take the lock
the unlocked read pointed at, then confirm the read still holds.

### Talking between processors

`hal_cpu_wake(cpu)` in `hal/qemu-virt/gic.c` sends SGI 0 through
`ICC_SGI1R_EL1`, addressed by the target's `MPIDR_EL1` affinity — which the
target recorded for itself, because a core can only read its own.

**The handler is empty.** The sender has already enqueued the thread; the
interrupt exists to make that core *look*, and the exception epilogue
already runs the scheduler. A `dsb ishst` before the register write is what
makes the enqueue visible before the interrupt arrives.

`thread_wake` sends one when the woken thread lives elsewhere. Not to this
core — an interrupt to oneself is a wasted exception.

On x86-64 it is `apic_wake` in `hal/pc/apic.c`: a fixed IPI on vector 0x3E
through the interrupt command register, addressed by the local APIC id the
target recorded for itself in `hal_irq_init_here`, and its handler is the
acknowledgement and nothing else. A secondary's own local APIC timer ticks
at the rate core zero calibrated against the 8253, and `arch/x86_64/trap.c`
keeps the machine-wide half of a tick on core zero, exactly as
`arch/aarch64/trap.c` does.

### What userland sees

`sysinfo` carries `cpus`, `cpus_online` and `cpus_present`; `/dev/cpu` adds
`cores`, `cores_online` and `cores_present`. Monitor and `cores` draw one
segmented meter per processor, all of them measured — `sys.cpuload()`
returns a `{idle, busy}` pair per online core, and every core charges its own
ticks. `machine` and `About Kosmos` report present against scheduling.

### The switch, and how to throw it

```
make qemu                 one processor places work; the default
make SMPWORK=4 qemu       four processors place work
make SMPWORK=2 qemu       two, for halving the search space
```

`SMPWORK` is a boot option (`opt/kosmos/smp` through fw_cfg), read once in
`kmain` and handed to `thread_place_across`. It is separate from `SMP`,
which is how many processors the *machine* has — that is always four. This
is whether the kernel puts work on them.

**Off by default, and that is a policy rather than a limitation.** Every
processor that came up can run threads, and does: `thread_create_on(cpu, …)`
puts one anywhere and the suite crosses a core on every run. What is not
finished is the *confidence* — spreading every thread is the first
configuration in which two processors are inside the kernel at the same
instant on a real workload, and the locks here have never been contended.

The test image always places on one, set by the suite itself. A dozen of its
checks mask interrupts, create three threads and drive them by yielding,
which is a question about *this* processor's scheduler and only means
anything if those threads are here. Spreading them would not make the tests
better; it would make them stop asking.

### What `SMPWORK=4` does, measured before and after

Six compute-bound processes, started together, one reading per second:

```
before:  placing 1:  cpu0 100%                                    (correct)
         placing 4:  cpu0  95%   cpu1  95%   cpu2  0%   cpu3   2%
                     cpu0   0%   cpu1   0%   cpu2  0%   cpu3  71%
                     cpu0   0%   cpu1   0%   cpu2  0%   cpu3  70%   ... for ever

after:   placing 4:  cpu0 100%   cpu1 100%   cpu2 100%  cpu3 100%
                     cpu0 100%   cpu1 100%   cpu2 100%  cpu3 100%   ... for ever
```

Three processors used to go idle within a second and stay idle while all six
processes were alive. **The cause was preemption, in two halves, and neither
was placement** - a trace showed the six threads going to cpu 0,1,2,3,0,1,
exactly as intended:

- **`thread_tick` returned before `policy->tick` on every core but zero**,
  so only core zero ever preempted on a quantum. A compute-bound thread on
  cores 1-3 could not be taken off by the timer at all. The early return's
  own comment said why that was safe - "a secondary runs only its own idle
  thread" - and called itself *"a temporary invariant, named so it is found
  when the locks arrive"*. The locks arrived at step two, the runqueues at
  step five, and nothing came back to it.

- **`thread_wake` decided preemption about the wrong processor.** It
  compared the woken thread against `current`, which is a macro for
  `this_cpu()->current` - the thread on the *waking* core - and set
  `this_cpu()->preempt_pending`, the *waking* core's flag. A cross-core
  wake therefore compared against a thread the woken one will never compete
  with, and flagged a core that is not going to run it. Both halves read
  through `percpu_at(cpu)` now.

The second is why it looked like the workers had vanished. They had not:
they were enqueued on cores that had been given no reason to look, by a
kernel that had also removed the timer's ability to make them.

**What was ruled out on the way, each by an experiment rather than by
reading**, and it is recorded because three of the four were plausible:
placement itself; IPC (a worker touching no server behaves identically);
threads dying (a trace on every transition to `THREAD_DEAD` printed
nothing); and the lost-wakeup race in `thread_wake` below, which is real and
was fixed and changed nothing here.

### What is still wrong under `SMPWORK=4`

**The display harness fails at its editor phase** - the program typed into
`edit` does not come back - and that is a different bug from the one above,
which is why placement is still off by default. It survived the fix. The
desktop itself comes up and runs.

**And the desktop does not saturate**, which is not a bug and is worth not
misreading: eight applications leave the machine about a fifth busy, so the
bars show naive round-robin placement rather than a fault. Placement is
assignment by creation order - `thread_create_suspended` calls it "the
dumbest policy that is not obviously wrong" - so the same applications land
on the same cores every boot and one bar stays low. Load-aware placement is
a later question and wants something to measure first.

**What has been ruled out, each by an experiment rather than by reading:**

- *Placement itself.* A trace in `thread_create_suspended` shows the six
  threads going to cpu 0, 1, 2, 3, 0, 1 — round robin, exactly as intended.
- *IPC.* A worker that does no IPC at all after it loads — no `fs.read`, no
  `/dev/cpu`, no server — behaves identically.
- *Threads dying.* A trace on every transition to `THREAD_DEAD` prints
  nothing. The workers are alive the whole time.
- *The lost-wakeup race below.* Fixing it changed nothing here, which is why
  it is recorded as a separate bug rather than as the cause of this one.

**And one structural fault found while looking**, which is real whether or
not it is this symptom: `thread_tick` returns early on every core but zero,
*before* it reaches `policy->tick`. **So only core zero ever preempts.** A
compute-bound thread on cores 1–3 can never be taken off by the timer. The
comment there says why that was safe — "a secondary runs only its own idle
thread" — and calls itself "a temporary invariant, named so it is found when
the locks arrive". The locks arrived at step two and nobody came back to it.

The desktop shows the same fault more quietly: with eight applications
running it reads roughly 27 / 25 / 2 / 37 per core, and the third bar is the
one that never rises.

**This is why placement is off by default**, and it is a better reason than
the one written here before, which was that the confidence was missing. The
mechanism is finished; the policy is not correct yet.

---

## Where it stands

**What this replaced was a single sentence**, and it is worth keeping because
everything above is the cost of no longer being able to say it:

> There is one core, and the kernel runs with interrupts masked.

That was mutual exclusion in Nebula from the first commit until 0.9.16. Not a
criticism of the code - it is the correct design for a uniprocessor and it is
why `ipc.c` is eight hundred lines instead of two thousand. It does mean SMP
was never a feature to add beside the others: it changes the assumption every
file in `kernel/` was written against, and the work is spread thin across all
of them rather than concentrated in a new directory.

The failures are the other reason it is hard. A slot handed out twice is not
a crash, it is two processes sharing an address space and a fault somewhere
else minutes later. A lost wakeup is not an error, it is a thread that never
runs again. A stale TLB entry is not a fault, it is a *successful* read of
somebody else's memory. And all of them are rare enough that a suite passing
is evidence of very little - which is why the audit below exists and why
several of the fixes here were confirmed by deliberately breaking them.

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
| `owner` in `arch/<name>/fp.c` | who owns the FP registers *on this core* | **moved** - `fp_owner`, as `void *` so `percpu.h` need not know what a thread is |
| the runqueue in `sched_prio.c` | `head[]`, `tail[]`, `occupied` | **moved** - indexed `[NR_CPUS][SCHED_PRIORITIES]`, one lock each |
| the armed-fault slot in `trap.c` | a fault this core is expecting | still one global, and every core's handler reads it |
| the TSS / kernel stack | where a ring-3 entry lands | **moved** - a TSS per core in `arch/x86_64/gdt.c`, rsp0 written through `GS`; x86 only |

And the register that finds them: **`TPIDR_EL1` on AArch64, the `GS` base
with `swapgs` on x86-64.** Both are written now, and the asymmetry is
larger than it looks.

`TPIDR_EL1` is *banked*: EL0 cannot see it or change it, so it is set once
per core at boot and read from anywhere afterwards. **No entry path is
touched at all.** x86 has one `GS` shared between ring 3 and ring 0, so the
same trick needs `swapgs` at every entry and every exit, in `vectors.S` and
`user.S`, with the classic hazard of an exception arriving between the two.
That was real surgery and it arrived with x86's second core rather than
before it: `swapgs` is the first instruction of `syscall_entry`, the top of
`isr_common` when the saved CS says ring 3, and the last instruction before
`sysretq` and `iretq` back - and every one of those windows runs with
interrupts masked. `GS` holds the null selector from `gdt.c` on, because
loading a selector into it would replace the base the whole scheme rests on.

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

**Audited against the code rather than remembered, and the list is about
sixty rather than five.** Four readers went through `kernel/`, `arch/` and
`hal/` and each was then handed to a second reader told to find what it
missed. What follows is the shape of the answer; the code carries the
detail, because a list in a document is the thing that goes stale.

**The centre of gravity is not the runqueue.** That is the part everyone
expects, and the vtable in `sched.h` already makes it the easiest to move.
What actually needs care is thread *state* - `t->state`, the three priority
fields, `wake_at` - and the allocation pools underneath everything.

Four groups, and they want different things:

- **The pools want a lock each, and the claim must be inside it.** Every one
  of them - `threads[]`, `processes[]`, `objects[]`, `spaces[]` on both
  boards - used the same pattern: scan for a free slot, build in it, mark it
  taken. The window between finding a slot and claiming it is *hundreds of
  instructions* long, including page allocation and a 512-entry table copy,
  and two cores in `SYS_SPAWN` at once get the same thread, the same process
  and the same address space with nothing anywhere noticing. `memobj_create`
  was the one that already claimed first, and its comment says why.

  **Done**, and it found a bug in the doing. Moving the claim inside
  `alloc_process` was not enough, because `process_create` then ran
  `memset(p, 0, sizeof *p)` over the slot - clearing the very flag that
  claims it, and reopening the window for the length of a 160-byte memset.
  The suite caught it on the first run.

- **Some things want to be per-CPU rather than locked.** The lazy-FP
  `owner`, whose own comment predicted this - **done**, and it is
  `fp_owner` in `struct percpu`. The armed-fault slot in `trap.c`, which is
  **not** done and is still one machine-wide static that every core's fault
  handler consults. On x86-64: the TSS and its `rsp0`, the `user_rsp` scratch cell,
  and the `syscall`/`sysret` MSRs - none of which matters until that board
  starts a second core, and all of which is why it should not.

- ~~**The drivers want locks and are reached from interrupt handlers.**~~
  **Done**, in 0.9.20. `blk`, `net`, `input` and `snd` each take a spinlock
  over the virtqueue indices they own. A worker function plus a thin
  wrapper rather than wholesale wrapping, because several of them have more
  than one `return` and a lock taken at the top of a function with four
  exits is a lock leaked at three of them.

- **Two things want a protocol rather than a lock.** `panic()` on one core
  while another is running is not a mutual-exclusion problem - the second
  core has to be stopped, not queued. And the console's screen state can be
  locked, but a lock taken by a panicking core is a lock nobody releases.

### IPC, which is still the hard one

The audit's answer is one sentence: **the endpoint is the right lock, one
per endpoint rather than one for the subsystem, and the critical section has
to extend across `thread_block()`** - handed to the next thread and released
on the far side of the switch.

Per-endpoint costs nothing, because no operation in `ipc.c` touches two
endpoints: `resolve` returns exactly one and every splice works on that one.
What makes the section span the block is the order `ipc_call` does things
in. It wakes the receiver and only *then* pushes itself onto
`awaiting_reply`, records `waiting_on` and blocks - so on two cores the peer
can reply before the sender has blocked, and `ipc_reply` reads a NULL
`waiting_on` and returns `IPC_ERR_NO_PEER`. That is not a race that
corrupts memory; it is a message that is silently lost.

**And the runqueue lock cannot be held across the context switch**, which is
an ordering constraint disguised as an architecture detail: `context_switch`
returns on a different stack, so a lock taken before it is released by a
different thread than took it. Pick under the lock, let go, then switch.

## What is left, and what is holding it

**The mechanism is finished and the policy is not correct yet**, which is
a different sentence from the one that stood here for months and a worse
one. `thread_cpu_count()` returns `smp_online()` when `SMPWORK` asks it to;
`thread_create_on` puts a thread anywhere and it runs there.

**What used to hold the line was the drivers, and that is done.** `blk`,
`net`, `input` and `snd` each take a spinlock over the virtqueue indices
they own, landed in 0.9.20. This section named them as the single blocker
for months, and went on naming them in the release that removed them.

**What holds the line now is that work does not spread**, which was found
by measuring rather than by reasoning and is written up under *What
`SMPWORK=4` actually does today* above. Six compute-bound processes, four
processors, three of them idle within a second.

**It was switched on once before that, deliberately, to find out what
breaks.** Two things did, within a second:

- `thread_block` panicked. "Every thread is blocked" was a statement about
  the *machine* and is now a statement about one processor - an empty
  runqueue is the ordinary state of an idle core. It falls back to that
  core's idle thread now, which is what an idle thread is for.
- A dozen tests failed, and they were right to. They are single-core tests
  of the mechanism: they mask interrupts, create three threads and drive
  them by yielding, which only works if those threads are *here*. That is
  why `thread_create_on` exists rather than a global switch - a suite that
  had to be rewritten to tolerate placement would be a suite that had
  stopped asking its original question.

`panic()` also still needs a protocol rather than a lock: a core that panics
has to *stop* the others, not queue behind them.

### The five it used to say, which are still true

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
thread that has since died. 947 lines of it, and they no longer assume
nothing else is running: **there is a spinlock per endpoint**, taken across
`ipc_call`, `ipc_receive`, `ipc_reply`, `ipc_abort`, `ipc_timed_out` and
`ipc_endpoint_destroy`, with `waiting_on` re-checked under it in three
places.

Two things in that path are still open and are named rather than implied.
`ipc_endpoint_create` claims its slot in the endpoint pool with no lock at
all - the one pool that never got one, while `threads[]`, `processes[]` and
the rest did. And neither `ipc_call` nor `ipc_receive` re-checks
`ep->in_use` after taking the lock, so a call can park on an endpoint that
was destroyed between the capability lookup and the lock.

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

**x86-64 was the expensive half, and it is built.** In the order a second
core needs it:

- **The table walker** - `hal/pc/acpi.c` walks the MADT for the count, and
  now keeps each usable processor's local APIC id, which is what an `INIT`
  aimed at one core is addressed by.
- **The interrupt controller** - `hal/pc/apic.c` drives the local APIC and
  the I/O APIC, and now writes the interrupt command register that `INIT`,
  `STARTUP` and the wake IPI go through.
- **The trampoline** - a real-mode page under 1 MB that reaches flat 32-bit
  mode, and the same long-mode climb `boot/x86_64/start.S` already did once.
- **A core's own state** - the `GS` base with `swapgs` at every entry and
  exit, a TSS per core, and a local APIC timer per core.

Under QEMU four processors come up, both through `-kernel` and through GRUB
on OVMF, and the guest suite's SMP checks pass on this board for the first
time. On the ThinkPad T14 none has come up yet, and `cpu_on.c` now says why
for each. **What x86-64 still lacks is a TLB shootdown**, which AArch64 does not
need and this board does - step 7 below - so placement across cores stays
off here until it exists. The paragraph this replaces listed a table walker
and an interrupt controller as missing, and went on saying so for months
after both had arrived.

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
2. ~~**Locks, with one core.**~~ **Done**, and done out of order - it
   arrived with step five rather than before step three, for the reason
   recorded below. `kernel/spinlock.h`, and every lock masks interrupts.
   Take them, release them, and let them be
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

   So the locks came back when there was a second *scheduling* core to
   contend for them. **This was a departure from the order above and it is
   written here rather than quietly done**, because the order was reasoned
   about once and this changed it. They landed with step five, where the
   per-CPU runqueues gave them something to guard and something to test.
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
4. ~~**The idle thread on the second core.**~~ **Done.** Every processor
   that arrives installs its own vector table, wakes its own GIC
   redistributor, arms its own generic timer, adopts an idle thread core
   zero reserved for it, and unmasks. It then takes PPI 30 at `TICK_HZ` and
   charges the time to its own `struct percpu`.

   **Three things were per-core by architecture and written as if they were
   the machine's.** `VBAR_EL1` is banked, so a secondary was running with
   whatever it reset to - survivable only because a masked core takes no
   exceptions at all. The GIC redistributor is one per core and `gic.c`
   named only the first, so a secondary configuring "the" redistributor
   configured core zero's. And `timer.c` kept `deadline`, `ticks` and
   `missed` as file statics, which is four cores writing one comparator.

   `gicr_here()` finds this core's redistributor by walking GICR_TYPER and
   matching MPIDR, rather than indexing by a stride - the architecture has
   each redistributor declare its own affinity, which is the GIC saying not
   to guess. The stride is still needed to step between them and was
   measured rather than remembered: QEMU's device tree gives the region as
   base 0x080a0000 size 0xf60000, and 0xf60000 / 0x20000 is exactly 123.

   **What a secondary does *not* do is the machine's half of a tick.**
   `thread_wake_sleepers` scans `threads[]`, `policy->tick` reads and writes
   the runqueue, `console_tick` drains a device - none of them has a lock,
   and four cores doing them at `TICK_HZ` each is four cores writing one
   linked list. Core zero owns them until step two. A secondary keeps its
   own accounting, which is what makes it visible.

   Two checks, and both were confirmed by breaking them: *every processor
   takes its own ticks* (fails if a secondary does not arm its comparator)
   and *every processor idles as a thread* (fails if it does not adopt one).
   They are the first checks in this project that can tell a live secondary
   from a dead one.

   **`sysinfo` grew a third count** and it is not padding. `cpus` is how
   many run threads, `cpus_online` how many take ticks, `cpus_present` how
   many the machine has: 1, 4 and 4. Collapsing any two of them has already
   been a bug twice.
5. ~~**Per-CPU runqueues.**~~ **Done**, and with it the locking of step
   two, because the two cannot be separated: a queue per core is only
   meaningful if another core may put something in it.

   `head[NR_CPUS][SCHED_PRIORITIES]`, one occupancy mask per core, one lock
   per queue. The vtable now names the queue - `enqueue(cpu, t)`,
   `pick_next(cpu)`, `ready(cpu)` - because "enqueue" is not a complete
   instruction on a machine with more than one.

   **A thread has a home and does not migrate**, recorded in
   `t->sched.cpu` and assigned round-robin when it is created. That is the
   decision, and it is worth stating as one because the alternative - a
   single queue every core pulls from - looks simpler and cannot be undone
   later. A shared queue makes every scheduling decision a contended write
   to one list and has nowhere to express placement, which is the first
   thing a machine with unequal cores asks for.

   **And it paid for itself immediately, in the hardest place.** The audit
   said IPC's critical section had to extend *past* `thread_block` - handed
   to the next thread and released on the far side of `context_switch`, the
   way Linux releases `rq->lock` in `finish_task_switch` - because a thread
   marked blocked and findable can be woken by a second core and resumed by
   a third on the stack the first is still saving.

   With a fixed home that race cannot happen. A wake can only enqueue a
   thread on *its own* core's queue, and only that core picks from it - and
   that core is the one inside the switch. So `thread_block_and_release`
   releases before the switch and is correct, and the whole hand-off
   machinery is unnecessary. The comment on that function is where the cost
   reappears if migration is ever added.

   What it cost: `sched_switch_to`, which swaps the policy at runtime, now
   refuses when more than one processor schedules. Draining every runnable
   thread out of one policy and into another means holding every core's
   queue at once, which would make it the single operation defining a global
   lock order. It exists to demonstrate that mechanism and policy are
   separable, which it has done; it is not something anybody needs while
   four cores are working.

   IPC took the shape the audit predicted: **one lock per endpoint**, since
   no operation in `ipc.c` touches two, held from before the hand-off until
   the sender is both findable and blocked. The three re-checks under the
   lock - in `ipc_reply`, `ipc_abort`, `ipc_timed_out` - are all the same
   pattern: `waiting_on` has to be read before there is a lock to take, so
   the lock is taken and the read confirmed.
6. ~~**IPIs.**~~ **Done, and measured.** `hal_cpu_wake(cpu)` sends SGI 0
   through `ICC_SGI1R_EL1`, and the handler for it is *empty* - the sender
   has already put a thread on the target's runqueue and the only thing
   missing is for that core to look. Taking the interrupt is what gets it
   out of `wfi` and into the exception epilogue, where
   `thread_preempt_if_needed` already runs. An IPI with a payload would be a
   message, and messages between processors are what a runqueue and a lock
   already are.

   **Worth 25x, and the number matches the reasoning exactly.** A cross-core
   wake, measured on the counter under TCG:

   | | counter ticks | wall clock |
   |---|---|---|
   | with the IPI | 11,688 | ~0.19 ms |
   | without | 293,688 | ~4.7 ms |

   4.7 ms is one scheduler tick at 250 Hz, which is precisely the prediction:
   without a poke the target notices at its own next timer interrupt. The
   `dsb ishst` before the register write is what makes the enqueue visible
   before the interrupt arrives - without it the target can wake, find an
   empty queue and go back to sleep, which is a lost wakeup that happens
   rarely and looks exactly like a hang.

   Nothing depends on it for *correctness*: the check for a thread running
   on another processor passes with the IPI removed. That is deliberate, and
   it is why the value is a measurement rather than an assertion.
7. **TLB shootdown - and the premise this step was written on is wrong.**
   It said `as_switch` invalidates locally with `tlbi vmalle1`. It does
   not, and has not for some time: `arch/aarch64/mmu.c` issues
   `tlbi vmalle1is` in `as_switch` and `tlbi vaae1is` in `invalidate`, and
   the `is` suffix is *inner shareable* - the hardware broadcasts the
   invalidate to every core in the domain and the instruction does not
   retire until they have all seen it.

   So the expensive half of a shootdown - an IPI, a handshake, and a wait -
   is not needed on this architecture, and the entry that said it was would
   have had somebody build one. The only local `tlbi vmalle1` left is in
   `enable()`, which runs before any other core matters and is right to be
   local.

   **What is actually left here is narrower and still real.** Two things:
   whether every path that changes a live mapping goes through `invalidate`
   at all rather than editing a descriptor and moving on; and
   `split_block`, which replaces a live 2 MB block with a table descriptor
   without break-before-make - legal on one core by luck and not by
   architecture, because another core may hold both translations at once
   and the manual says that is a permitted TLB conflict abort. x86-64 has
   its second core now and no broadcast invalidate, so it does need the
   IPI - and the vector and the command register a shootdown would go
   through already exist, for the wake. Until it is built, nothing on x86-64
   puts a process's threads on more than one core.

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
