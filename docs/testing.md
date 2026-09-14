# Testing and performance

Three different things that get conflated often:

- **Correctness.** Does it do what it says? Binary, no nuance.
- **Budget.** Does it fit in the time it has? A frame is 16.6ms and that's it.
- **Regression.** Did it get worse since yesterday? A comparison against a stored baseline.

Each one needs its own mechanism. A single "performance" number answers none of the three.

---



## The blind spot a benchmark suite has by construction

`bench/` measures operations. It cannot measure the system the operations
run in, and the difference is not academic.

The idle thread's loop was `thread_yield(); wfi;` unconditionally. A thread
that yielded switched to idle, idle returned from its own yield, and slept -
with a runnable thread sitting in the queue - until the next timer interrupt.
Every yield cost a full timer period and every IPC round trip cost two: ten
and twenty milliseconds at 100 Hz, on paths whose own cost is around thirty
counter ticks.

The whole system was roughly a thousand times slower than it should be, and
every number in `bench/` was green. `ipc_roundtrip` measures two threads that
are both runnable, so it never goes near the idle thread; it read 30.000
ticks before the fix and 30.312 after. `context_switch` and `exception` were
identical to three decimal places.

It was found by measuring something else. A query benchmark came out at
twenty milliseconds a call, which is far too slow to be a hash lookup, and
the first thing to do with a number like that is to measure the floor
underneath it - a yield, and a round trip that does nothing. Both were exactly
one and exactly two timer periods, and a measurement that lands on a round
multiple of the clock is never about the work.

Two lessons, and the second is the one that generalises:

**A benchmark of an operation says nothing about the machine it runs on.**
Latency between operations needs its own measurement, and `bench/` had none.
`user/bin/latency.lua` is that measurement now, and the display harness
reads its verdict.

**Where a test can run decides what it can see.** The obvious place for this
was `make test`, and a test was written there - it passed with the bug
deliberately put back. During the suite the kernel's first thread is running
the suite, so it never reaches the idle loop, and a yield with an empty
runqueue returns instantly. A test that cannot fail is worse than no test,
because it is also a claim. It was deleted and moved to the harness that
boots the shipping image.

**And measure the floor before believing a slope.** The query benchmark then
came out sloped, 1.8x across sixteen times the nodes, and said so - wrongly.
The slope was in the round trip carrying the query, not the query: a
filesystem with eight hundred more nodes is a process with a much larger live
heap, and every allocation pays a share of collecting it. A control at each
size - one `getattr`, which touches no index - rises by the same amount, and
what is left after subtracting it is flat.


## The display, from outside the guest

`tools/run_screenshot.py` boots QEMU, asks its monitor for the picture being
scanned out, and inspects it. It exists because some properties are
structurally invisible from inside the guest:

| phase | what it catches |
| ----- | --------------- |
| the boot screen | geometry, the banner, the progress bar reaching the end (a short bar means BOOT_STAGES disagrees with the stages actually announced), and the channel order - a wrong fourcc turns the green bar blue |
| bars drawn from Lua | a pitch bug. Replacing the pitch with `width * 4` passes 103 of 103 in-guest tests, because every read and every write agree with each other. Only an observer on the other side of the framebuffer disagrees |
| real key events | injected through QEMU's input plumbing into virtio-input, a path sharing nothing with the serial line everything else types over |
| a detached program still drawing | `monitor 30 &` has to keep redrawing the reserved rows on a clock of its own. Three earlier versions did not, and none of them could be told apart over serial |

The last one has a trap worth remembering. The first version of the check took
two pictures 3.0 seconds apart and reported a completely static screen - on a
system where both the cursor and the status bar were moving. The cursor blinks
twice a second, so any whole number of seconds catches it in the same phase.
Sampling intervals aligned to something the system does are how a test comes
back green about a screen it never really looked at.


## 18.1 The harness

**The runner lives on the host, the tests run in the guest, serial is the channel.**

```
make test        # runs the whole suite under QEMU, exit code 0 or 1
make test M=3    # only the tests up to milestone 3
make bench       # runs the benchmarks and compares against baselines
```

`make test` and `make bench` exist. `M=` is still pointless while the whole
suite runs in under two seconds; it becomes worth having when it does not.

`make bench` uses a separate image and a separate runner from `make test`,
because `-icount` is what makes a measurement repeatable and it also makes
QEMU several times slower. The tests must not pay for that.

`make bench-record` writes the current numbers as the new baselines. By hand,
never automatically, for the reason in §18.6.

The determinism `-icount` buys is not theoretical. The same spin loop measures
88187, 88687 and 89062 ticks across three ordinary runs, and 75001, 75000,
75001 under `-icount`. With interrupts masked during the measurement as well,
the benchmark numbers come out bit-identical run to run, which is why a 2%
tolerance is generous rather than tight.

The guest prints a TAP-style protocol over the UART:

```
1..4
ok 1 - mmu: identity map
ok 2 - mmu: page fault reports correct FAR
not ok 3 - ipc: reply wakes the receiver
ok 4 - ipc: destroying an endpoint wakes blocked threads
```

A Python script on the host launches QEMU, reads the serial, parses it, and exits with the matching code. Forty lines.

### The exit code from inside the guest

On aarch64 the clean way is **semihosting**. Launch QEMU with `-semihosting-config enable=on,target=native` and have the guest execute `HLT #0xF000` with `SYS_EXIT` (`0x18`) in `x0` and the code in `x1`. QEMU terminates with that code.

Without it, the host has to kill QEMU on a timeout and guess from the output. It works, but it is fragile and slow.

It also gives you a way to abort a hung test: a watchdog on the timer that calls `SYS_EXIT` with an error code.

### When to build it

**At M0, along with the first line of output.** This is not pulling future work forward: it is fifty lines and it is what gives every subsequent milestone a safety net.

If it is deferred, it never gets built. That is the general rule about harnesses.

---

## 18.2 The three layers of tests

### C self-tests (M0-M3)

Before Lua exists there is no other option. An array of functions returning bool, run at the end of boot, with the result printed in TAP format.

```c
static bool test_mmu_identity(void) { ... }
static bool test_page_fault_far(void) { ... }

static const struct test tests[] = {
  { "mmu: identity map", test_mmu_identity },
  { "mmu: page fault reports correct FAR", test_page_fault_far },
};
```

Compiled with `-DKOSMOS_TEST` so they take no space in the normal image.

**The special case that has to be solved:** several kernel tests verify that something *fails* correctly (a page fault, an invalid access, a destroyed endpoint). That needs an exception handler that knows "this exception was expected, record it and continue" instead of dying. It is a flag in the handler plus a `setjmp`, and it has to be anticipated when the vector is written at M1.

**And "continue" means something different on each architecture, which is what it cost to run this suite on a second board.** AArch64's handler steps ELR past the faulting instruction — exact arithmetic, every A64 instruction being four bytes. **On x86-64 there is no such number:** an instruction is one to fifteen bytes and its length cannot be known without decoding it, so there is no next instruction to step to and no honest way to invent one.

So the recovery both boards use is the *unwind* form, which AArch64 already had for the one case stepping could not serve — a stack overflow, where resuming would fault into the guard page again. `tests/fault.h`'s `FAULT_EXPECT` is the single macro over it, and the whole conversion cost AArch64 nothing, because the mechanism was there and already tested. The two tests that are *about* stepping stay AArch64-only and say so; written through `FAULT_EXPECT` the second would have passed by unwinding around the assignment it checks, which is a test that cannot fail.

**The other three things a portable kernel suite needs**, and each was found by the suite not compiling rather than by anyone predicting it:

- **`tests/machine.h`** — the deliberately awkward operations no kernel header should hold: an instruction chosen to fault, a store the compiler may not reason about, a named callee-saved register held across a switch, a page-table entry's frame and permissions. Most of what a test needs was *already* behind `arch/<board>/cpu.h` — `cpu_irq_disable`, `cpu_cycles`, `cpu_interrupts_save`, and `cpu_current_el`, which already reports 1 for the kernel on both boards.
- **`tests/exit.h`** — how the guest sets the host's exit code, and the one place the boards differ irreconcilably. AArch64 has semihosting: one instruction, an exact status. x86-64 has QEMU's `isa-debug-exit`, which exits `(value << 1) | 1` — always odd, and therefore *incapable of expressing success*. So success is an ACPI power-off, which exits 0, and only failure uses the debug port. The asymmetry is the honest shape: two different things happened and they leave by two different doors.
- **A per-board fixture blob.** `user/hello-<arch>.S` and `user/faulty-<arch>.S` — the four processes that each do exactly one thing they are not allowed to. What is proved belongs to the microkernel; only the instruction set differs.

**A literal address in a portable test is a claim about one machine.** `as: the kernel region is refused` named 0x40000000 — a kernel address on AArch64, and *exactly* `USER_VA_BASE` on x86-64. On the second board it asked the kernel to refuse the first page of user space, the kernel correctly mapped it, and the test reported a failure entirely its own.

### Lua tests (M2 onward)

As soon as there is an interpreter, tests are written in Lua and the ergonomics change completely. The C self-tests freeze: the existing ones stay, new ones go in Lua.

**They run at EL0, in a process.** From M5 the kernel has no interpreter, so a Lua test is `user/tests/luatest.lua` in a role of its own, started by a driver in `tests/tests.c` that turns its exit code into a TAP line. The plan, the numbering and the names stay on the C side; the assertions are out here, where Lua is. A failing assertion `error()`s, and `user/init/main.c` prints it — that line is not TAP, so the host runner ignores it and a human reading the output gets the reason.

```lua
test("namespace: what is not mounted does not exist", function()
  local proc = spawn{ needs = { "/dev/clock" } }
  assert_error(function() proc:read("/home/x") end, "no such path")
end)
```

### Host-driven integration (M5 onward)

Scenarios that need several processes and coordination. The host launches QEMU, sends commands to the shell over serial, and checks the output.

This is where the system's properties get tested, not its functions: that a server reloads without the client noticing, that a dead process drags nobody down with it, that two processes see different namespaces from the same server.

---

## 18.2b Testing more than one processor

**A lock is the one thing in a kernel that cannot be tested by using it.**
Every structure a lock protects works perfectly well with a lock that does
nothing at all, on a machine where nothing contends — which is why the plan's
own second step, "take locks with one core and let them be uncontended", was
skipped: it is the one increment nothing can check.

So the SMP checks are about *mechanism*, and each was confirmed by breaking
it. That last part is not ceremony. A test for a race that has never fired is
indistinguishable from a test that asserts nothing, and the only way to tell
them apart is to make the thing fail on purpose and watch the test notice.

| Check | Fails when |
|---|---|
| `lock: a spinlock excludes, names its holder and masks` | it does not mask (interrupts still on inside), or it hands the same word to two callers |
| `cpu: the machine says how many processors it has` | `hal_cpu_count` is hardcoded — which `-smp 1` cannot detect |
| `cpu: every processor claimed its own slot` | `TPIDR_EL1` is not really per-core, or placement answers with more processors than arrived - asked for every slot, it must answer with the cores that started |
| `cpu: every processor takes its own ticks` | a secondary never armed its comparator |
| `cpu: every processor idles as a thread` | a secondary ticks but has no `current` to charge it to |
| `smp: a thread runs on another processor` | placement, the target's runqueue, its lock, or its idle loop |
| `smp: new threads go to every core by default` | a plain boot homing new threads on fewer processors than arrived - `kmain`'s old `thread_place_across(1)`. Read before the suite pins itself to core zero, since the pin is the suite's decision and this is about the kernel's |
| `smp: a changed mapping reaches every core` | a TLB shootdown that never arrives or is not answered: core 1 reads a page through a user address while core zero unmaps it and maps others there. The reader masks interrupts, because with them on it passed with the shootdown switched off; now, switched off, it reads the old page and fails |
| `smp: a slot is reused only once its thread has left` | a dead thread's slot handed to a new thread while the dead one was still switching away - which started the new thread inside `thread_exit`. Every slot is touched so a new thread must reuse one; a thread exits on another core with an address space still loaded, so its last switch changes page tables inside the window; and core zero creates a thread on a third core the instant the slot reads dead, two hundred times. Without the fix it panics - `a dead thread was scheduled` - in two runs of two on each board |
| `smp: a parent waits for children on other cores` | `process_exit` publishing `exited` before its teardown, or a parent's wait and a child's wake meeting with nothing held - a slot reaped while its owner still writes to it, or a wake lost. A thread of its own waits a hundred times for a child placed on another core, and spawns the next the moment each wait returns. The suite's own thread cannot do the waiting: it is core zero's idle thread, and an idle thread that blocks panics. With `process.c` as 0.10.21 had it, both boards panic on the second child: `pmm_free_page: address is below RAM` |
| `smp: a reply reaches a caller on another core` | a reply lost between cores: two thousand calls from core 2 to a server on core 1. With `ipc_call`'s old order - waking the receiver before joining the reply queue - it fails within 111 rounds, one reply refused |

Three things about that table are deliberate.

**The suite boots `-smp 4`.** On one processor a working discovery is
indistinguishable from a hardcoded 1, and `percpu_at(i)->index == i` cannot
be told from a global. The whole first half of the table needs a second core
to mean anything.

**It also changed the suite's timing**, which caught the project out twice.
Four vCPUs round-robin in one TCG thread, so core zero executes roughly a
quarter of the instructions per tick that it used to. Two tests that were
bounded by the clock still failed, because a tick is *wall-clock* time and
how much work fits inside one had changed. A third failed because it consumed
a whole fixed pool and was therefore an undeclared assertion that nothing
else in the machine held a slot.

**`smp: a thread runs on another processor` never yields.** Core zero sits
doing nothing while it waits. If the thread only ran because this processor
gave up the CPU, it would prove nothing about the other one.

### What cannot be checked here, and what is done instead

Mutual exclusion **under real contention** is not tested by the suite. It
is no longer `thread_cpu_count()` that prevents it - that returns
`smp_online()` on every plain boot since 0.10.22 - but the suite, which pins
itself to one core with `thread_place_across(1)` because a dozen of its
checks mask interrupts, create three threads and drive them by yielding, and
only mean anything if those threads are here. The harnesses that boot the
real image - the display, the disk, the network, `run_x86.py` - run spread
now. Two things stand in for it inside the suite:

- **An audit.** Four readers over `kernel/`, `arch/` and `hal/`, each handed
  to a second reader told to refute it. About sixty structures, and it found
  three things nothing else would have: an interrupt path missing a
  core-zero guard, a TLB invalidation that was local where it had to be
  broadcast, and a comment that had been wrong since it was written.
- **Deliberately switching placement on** to see what breaks, then off
  again. Two things did, inside a second, and both are recorded rather than
  quietly fixed.

And one fix is in the tree that **no test can confirm**: the release/acquire
pair around publishing a secondary's `struct percpu`. It passes whether or
not it is correct. It is there because it was reasoned about and reviewed,
and saying so is the honest status of it.

---

## 18.3 QEMU and hardware measure different things

**This has to be clear before looking at any number.**

QEMU is a JIT translator. Wall time inside QEMU has no relationship to time on hardware. A QEMU number in milliseconds means nothing.

But QEMU has something hardware does not: **with `-icount shift=N` it is deterministic.** The same image run twice yields exactly the same count. That makes QEMU the right instrument for detecting regressions, because any difference is a real difference in work, with no noise from scheduling, caches or thermals.

And hardware has what QEMU does not: the truth. `PMCCNTR_EL0` (the PMU cycle counter) gives real cycles on the Pi. **Careful: under QEMU the PMU is not faithfully emulated and its numbers mean nothing.**

The split works out like this:

| | QEMU with `-icount` | Real hardware |
|---|---|---|
| What it measures | work, in instructions | time, in cycles |
| Deterministic | yes | no |
| Good for | detecting regressions | knowing whether it fits the budget |
| Runs | on every commit | when a milestone closes |

A change that raises the QEMU count by 30% is a regression even if the Pi does not notice. A change that blows the frame budget on the Pi is a problem even if QEMU looks identical.

---

## 18.4 What to measure at each layer

Each milestone adds its metrics and none of them get removed afterwards.

### Kernel (M3)

- **IPC round-trip.** The most important number in the system. Two threads, 100,000 round trips, divided. Everything else is built on top of it.
- Bare context switch
- Syscall entry and exit
- Page fault latency
- Interrupt latency: from the timer firing to the first instruction of the handler

### Lua runtime (M4)

- Serializing and deserializing a typical message
- Allocating and freeing a table
- **Maximum GC pause.** Not the average. It is the number that decides whether the system stutters, and it will be the project's recurring problem.
- Overhead of a syscall called from Lua versus the same one from C (still not built)

### Namespace and servers (M5)

- Path resolution
- A node `read`, end to end from the client
- How many concurrent clients a server takes before latency degrades

### Graphics (M6)

- Blitter throughput, in MB/s
- Time to compose a typical frame
- **Input latency: from the event to the pixel changing.** This is the metric that determines whether the system feels good. Hard to measure, and worth the effort: instrument it with a timestamp on the event and another on the flip.
- Damage tracking overhead versus full redraw

### Filesystem (M7-M8)

- Query time versus number of files. **It has to be flat.** If it grows with file count, the index is not working and the whole premise of the filesystem collapses.
- Live query notification latency
- Sequential read and write
- Mount time

### Apps (M9-M10)

- **p99 frame time, never the average.**
- App startup time
- Resident memory per process

---

## 18.5 The average lies

The system's entire thesis is BeOS-style fluidity, and fluidity is **consistent latency**, not high throughput.

A system averaging 8ms per frame with a 40ms spike every two seconds feels worse than one holding a steady 14ms. The first has the better average and it is the one that feels wrong.

So, for anything with a frame budget:

- Record the **maximum** and the **p99**; the average is reference only
- Tests assert on the maximum: `assert(frame_max < 16.6ms)`
- A histogram is worth more than a number. The outliers are the information.

When a periodic spike shows up, the first suspect is the Lua GC. The second is a large `memcpy` into the uncached framebuffer.

---

## 18.6 Baselines and regressions

The reference numbers live in the repo: `bench/baselines.json`, with the date, the commit and the milestone.

```json
{
  "ipc_roundtrip_icount": { "value": 1840, "tol": 0.05, "commit": "..." },
  "gc_pause_max_icount":  { "value": 91200, "tol": 0.10, "commit": "..." }
}
```

`make bench` compares and fails if something exceeded its tolerance.

Two rules that make this work instead of turning into noise:

**A baseline is updated by hand, never automatically.** If a change makes a number worse on purpose (because it added a feature that was needed), raise the baseline in the same commit, with the reason in the message. A baseline that updates itself detects nothing.

**Different tolerances depending on the noise.** Under QEMU with `-icount`, 2% is already signal. On hardware, with caches and thermals in the way, you need 10% before it stops being noise.

---

## 18.7 Tests as milestone gates

Each milestone's definition of done becomes a test, and that test is never deleted.

| # | The test that stays forever |
|---|---|
| M0 | Boot prints the expected line |
| M1 | A deliberate invalid access reports the data abort with the correct FAR |
| M2 | The REPL evaluates `2+2` and returns `4` |
| M3 | 100,000 IPC round trips, with the count inside tolerance |
| M4 | A process doing `*(nil)` dies without taking the system with it |
| M5 | Two processes see different namespaces from the same server. ~~Hot reload without the client noticing~~ — withdrawn September 2026 with the feature; `design.md` §10 |
| M6 | Dragging a window with a hung app inside it stays smooth |
| M7 | A live query updates without polling when another process writes. An `fs.write` into an app's namespace changes its state. **`run_queries.py` checks which paths come back, on both kinds of mount** - the milestone was signed off with no such test, and the disk had been answering `/home` with `/home/home/...` and with `/system`'s files ever since |
| M8 | Power cut during a write and the filesystem mounts clean |
| M9 | 60fps without a single stutter for a minute |
| M10 | Doom at a sustained 35fps |

With that, the full suite is the history of the project. At M8, if something breaks the IPC from M3, it surfaces on the commit and not three weeks later.

---

## 18.8 What to resist

**Do not measure everything.** A metric nobody looks at is noise you still have to maintain. If a number is not going to change a decision, do not instrument it.

**Do not optimize against QEMU.** QEMU numbers are for detecting change, not for knowing whether something is fast. Optimizing to shave an icount the Pi never notices is wasted time.

**Do not chase a number that is inside budget.** If the frame fits in 16.6ms with room to spare, taking it from 8ms to 6ms buys nothing. The goal is to fit, not to win a benchmark.

**Do not delete an old test because "we know that works by now".** That is exactly the test that will catch the regression.

---

## 18.9 The machine that is not there

Every runner in `tools/` attached `-device ramfb`, because every runner was
about pixels. That left one shape of machine untested: the one with no
display at all - which is `make serial`, and which is also a real board
with nothing plugged into it.

It was broken, and silently. init asked the kernel for the screen grant
whether or not there was a screen; the kernel refuses a grant it cannot
give; the refused spawn was the shell. So the machine booted through all
twelve stages, printed nothing that looked wrong, and stopped at a prompt
that never came. The identical mistake had already been made and fixed for
the disk, four lines higher in the same function, with a comment above it
explaining exactly this.

`tools/run_headless.py` is the test that would have caught it, and it runs
as part of `make test`. Four checks: a prompt appears, no server said it
could not start, a program runs, and the program's own output arrives.

The third one earns its place separately. Programs are spawned through a
different path than the shell, and that path had the same bug: with only
the shell's site fixed, the machine reached a prompt and then refused to
run anything. Both sites were re-broken one at a time to confirm each
check fails on its own, which is the only way to know a test tests what its
name says.

The general lesson is not about screens. **A test suite that always
provides every device only ever tests the machine you have.** The absent
device is a configuration, and it is the configuration real hardware
arrives in.

---

## 18.10 A score for a machine

`make bench` answers "did this change make it slower", under `-icount`, on
this desk. It cannot answer "is this board faster than that board", and
that question arrives the moment there is more than one board.

`score` is the answer to the second one, and `sysbench` is the same thing
with a window. Twenty-two measurements in six groups - processor, memory,
runtime, kernel, graphics, filesystem - taking about two minutes, ending
in one number.

**Fixed time, not fixed work.** Each measurement runs for four seconds and
counts what it got through. Fixed work would take four seconds here and
eleven minutes on a Pi 1, and the slow machine is the one most worth
measuring. This way the run costs the same everywhere and the slow machine
simply reports smaller numbers.

**The batch size is found, not chosen.** Start at one iteration and double
until a batch lasts long enough to time honestly. Nothing has to be tuned
per target, which is the property that makes the same code meaningful on a
machine a hundred times slower.

**A batch size that changes what a unit costs is a broken measurement.**
The string test originally appended *n* strings to one table and joined it,
so the work per unit depended on the batch the calibrator happened to pick.
It read as ordinary noise - 39,000 one run and 50,000 the next, same
machine, nothing changed. It is now a fixed thirty-two strings per
iteration. Any test whose per-unit cost varies with *n* can only compare a
machine to itself, and barely that.

**Never infer a count from a return value.** Batches used to return a
running total so it was obvious the loop did something, and the harness
could not tell that apart from a declared unit count. The integer test
reported twenty-seven billion operations a second - about forty times what
the hardware can retire, and really just the value of `x`. Units are
declared in their own field now.

**The scores are a geometric mean**, because they are ratios. The
arithmetic mean of "twice as fast at one thing, half as fast at another" is
1.25, which claims the machine is better when it is exactly even.

**The windowed one is slower than the printed one, and that is real.**
Painting the results costs the machine being measured. Throttled to twice a
second it lands within about twenty percent of `score`; the rest is the
desktop honestly running. `score` is the number to quote, and it is the one
a new board can produce over a serial cable with no display attached -
which is the state every board arrives in.

And the standing warning applies harder here than anywhere: **under
emulation this measures the host, the emulator and the guest at once.**
Comparing a QEMU run against a real board is comparing nothing to nothing.

---

## 18.11 The arrow keys are not broken, and how that was established

The display harness fails on the arrow-key phase often enough that it has
been written down as a bug twice. It is not one, and this section exists so
it is not chased a third time.

The input path was instrumented end to end and every stage is correct:

- The window manager receives all three bytes of every arrow - `27`, `91`,
  `66` - and forwards each to the focused window.
- The widget kit's escape machine decodes them: `esc=1`, then `esc=2`, then
  a code of `-2` with `handled=true`. Every time.
- The selection moves sixteen pixels a press. Measured with the harness's
  own colour probe, four presses running: 264, 280, 296, 312, 328.

What fails is the *harness*, and it fails at a different phase on each run -
the arrows, then a button click, then Control-C getting the screen back.
That moving target is the signature of a timing problem rather than a
broken feature, and it correlates with load on the machine running QEMU
rather than with anything in the guest.

Two mistakes were made while establishing this, and both are worth keeping:

**The comparison against HEAD proved nothing.** Stashing the day's work and
seeing the same failure looked like proof the bug predated it. It was run
with five stray QEMU processes left over from earlier experiments still on
the host, one of them using most of a core. A control that shares the
variable it is controlling for is not a control. `pkill qemu` before
believing a timing result.

**The first probe was broken and looked like evidence.** Reporting the
kit's state by assigning to `self.title` produced no output, which read as
"no key ever arrived". Lua's `__newindex` does not fire for a key that is
already present, so the assignment silently did nothing. It was caught by
checking the probe against a keystroke known to arrive - a serial one -
before trusting what it said about a keystroke that might not.

**What is actually worth fixing** is the harness's sensitivity, not the
system: phases that wait on a fixed deadline for a screen to change will
fail on a busy host whatever the guest does.

---

## 18.12 Testing a thing that only fails when the power goes off

M8's definition of done is "cut power during a write and mount clean". That
is two different tests, and conflating them produced a bad one first.

**`make powertest` kills the machine.** SIGKILL, five times, at five
different instants during a run of four hundred writes, and after each one
the machine is booted again and asked: can every name in the directory be
read, and does every file that came back hold exactly what was written to
it? A file that is missing is fine - it was not finished. A file that is
half there is not, and neither is a name that lists but cannot be opened,
which is a directory entry pointing at an inode that was never written.

**`build/host/lua tools/test_kfs.lua` tests recovery.** It has to be
separate, and the reason is worth stating: the journal's guarantee is about
one instant - after the commit block lands and before the last data block
reaches its home. Under QEMU that is a few milliseconds inside a
fifty-millisecond write. Five kills will usually miss it.

The first version of the power test asserted that at least one run replayed
a transaction. It was asserting on luck, and a test that fails when a coin
comes up tails is worse than no test. So the instant is chosen instead:
`kfs.commit(sb, "after-commit")` returns as soon as the transaction is
durable and before any of it is applied, which is exactly the state a mount
must recover from.

That parameter exists only for the test, and that is the right trade. The
alternative is not testing the one property the whole milestone is about.

### The host interpreter, and what it is not for

`kfs.lua` is arithmetic over blocks. Given stubs for reading and writing
one, every branch of it runs on this machine in a fraction of a second -
including the ones that need power to fail at an exact moment.

It does not replace the guest tests and must not. The same source runs, but
not on the same machine, against the same libc, or through the same
syscalls. This answers "is the format correct". `make test` and
`make disktest` answer "does it work on the machine".

### Four rules, and deleting each one to check

Every check here guards a rule rather than a value, so each rule was
deleted in turn to confirm its check fails without it:

| deleted | caught |
|---|---|
| the checksum comparison | yes |
| `recover` entirely | yes |
| the "was it committed" test | **no** |
| journal writes | yes |

The third is the one worth having. The uncommitted case blanked the
journal header, so recovery refused it for having no magic and the state
field was never exercised at all. Rebuilt with a *valid* header - real
magic, real count, real checksum, only the state saying empty - it fails
correctly.

It took three attempts and each wrong one passed. **A check that still
passes when the rule it names is deleted is not testing that rule**, and
the only way to find that out is to delete it and watch.



## 18.13 A camera with three checks on it

```
make web                       # the NetSurf libraries, running
make browser                   # the browser, with a page in it
make browser PAGE=/some.html   # the same, on a page of your own
```

Both boot a `WEB=1` image - the ordinary one, since `FULL=1` turns the web
kit on - and `make test` builds an image of its own without it, so neither
is part of `make test`.

`make web` asks the guest to parse things and answers over serial: a title
out of a tree, a `p` counted through a walk rather than a token count, `&amp;`
decoded from a generated table, a stylesheet understood, the cascade run,
and a height and some ink from the painter. Serial is enough because every
one of those is a *number the guest can say*.

`make browser` is the one that cannot be. It serves a page from this
computer - slirp maps the Mac as 10.0.2.2, so no packet leaves the machine -
points the browser at it and looks at the screen. What it is mostly is a
camera, and `build/browser.png` is written whether it passes or fails,
because a failure is exactly when the picture is wanted.

The three checks are deliberately blunt, because a check that goes stale is
worse than no check:

| check | what it catches |
| ----- | --------------- |
| the page area has ink on it | a window that opened, filled its paper, and drew nothing. This has been the failure at every stage of the browser so far, and in a thumbnail it is indistinguishable from a working one |
| more than one text height is present | the whole reason the browser draws its own pixels. If every line came back the same height, it is being rasterised by the compositor in one face and the direct window bought nothing |
| six presses of Down change the picture | the page is laid out once into a surface taller than the window and scrolled by blitting a band out of it. A browser that lays out correctly and will not move is one nobody can read the bottom of |
| clicking Reload asks the server again | a direct window has no widgets, so every control in the chrome is a rectangle the application knows the position of and a click is a comparison against it. Nothing else here exercises that arithmetic |
| a link, found by its colour, leads to the other page | six things at once: the layout kept its boxes, the click became a page coordinate, the run under it was found, its relative address resolved, the fetch happened, and the result was laid out. Finding it *by colour* also establishes that the run knew it was inside an `<a>` |

Six presses rather than one, and for the same reason the detached-program
check sleeps 3.3 seconds rather than 3: a line is forty pixels and the check
compares whole rows, so on a page of evenly spaced paragraphs one line's
movement can leave a row looking much as it did. A quarter of a screen
cannot.

**What is deliberately not checked is what the page says.** Comparing
against a reference rendering would be a check that fails every time the
layout improves, which is every time somebody does the work. The page in
`tools/test_page.html` describes what it is testing in its own text, so the
picture is readable by a person and the harness only has to establish that
there is a picture at all. `tools/test_linked.html` is the other end of its
links, and lists what the renderer still cannot do.

Two pictures come out, not one: `build/browser.png` is the page it started
on and `build/browser-linked.png` is the page it arrived at. The server
being asked proves the click was routed; only the second picture proves what
came back was laid out.

**The status line in those pictures is half the point of them.** It reports
fetch, parse, layout and paint after a load, and frame, blit, commit, the
worst frame and the kilobytes allocated while scrolling - so a picture taken
for the record is also a profile. Run the same capture with `-accel hvf
-cpu host` spliced into `QEMU_ARGS` and the same line reads native speeds
instead of TCG ones; `docs/state.md` has both columns for the run that
found the commit wait.

## 18.14 An editor, checked through the files it saved

```
make test            # three of the four, on this machine
make litexl-check    # the fourth: a LITEXL=1 image, booted twice
```

Lite XL has four checks, and three of them boot nothing, because most of
what can go wrong in the port does not need a machine to go wrong on.

| check | run by | what it establishes |
| ----- | ------ | ------------------- |
| `tools/test_litexl_surface.c`, 58 checks | `make test` | the SDL shim Kosmos wrote and the renderer over it, built with the host compiler - which works because the shim needs `stdlib.h`, `string.h` and nothing else |
| `tools/test_litexl_lua.lua`, 9 checks | `make test` | the vendored Lua still loads: the module graph resolves and `core.init()` returns, on `build/host/lua` with the C modules stubbed |
| `tools/test_litexl_host.lua`, 34 checks | `make test` | the launcher's own decisions - paths, the installed tree worked out from keys, files, the event queue, key names and the damage rectangle |
| `tools/run_litexl.py`, 7 checks | `make litexl-check`, and so `make prepush` | the editor: a window, Lite XL's own faces out of the image, a title that follows its file, a file edited and saved, Control-N, and a new document saved under a name |

**The check on the machine reads what the editor saved, not what it drew.**
Each boot starts the editor from the prompt, types through QEMU's own
keyboard, saves with Control-S, stops the desktop with Control-C, and asks
`head` for the file. A picture of the window would pass with the save
quietly failing; the file cannot. The first boot counts three more checks for
the start: `wm` says it opened a window, forty seconds later none of the
launcher's failure lines has appeared, and the launcher says Lite XL's own UI
and icon faces came from the image rather than being stood in for. With
`icons.ttf` left out of the table the check exits 1, but at the second of
those rather than the third: Lite XL cannot load `core/style.lua` without its
icon font. The third is for a face that arrives from somewhere else.

**Control-N is checked by what the editor did.** `wm litexl:--trace` prints
every command Lite XL runs, and the check waits for
`command core:new-doc -> true`. That is the check that would have caught the
queue fault `docs/litexl.md` describes, where the key reached the window and
the command never ran.

**The title is checked by what the window manager said**, for the same
reason: Lite XL composes the name and the desktop draws it, so the evidence
is the compositor's line rather than the screen. `wm: window Lite XL is now
~/notes.txt - Lite XL` when the file opens, `~/notes.txt* - Lite XL` once a
key has been typed, and the plain name again after Control-S. The last of
the three is read after the file has been, because the file is the better
evidence that the save happened at all.

**Its negative control has been run**, before the check was believed: with
`set_window_title` the empty function it was until this revision, it exits 1
at the first of the three, and the only line about that window is where it
was placed. The title had been listed as something the window manager could
not do, for as long as the port existed.

**The host test's negative control has been run.** With the old consumption
put back - the slot cleared, and the storage never started again - it exits
1 and fails exactly two checks: a drained queue is empty, and an event pushed
after the queue drained is the next one out. It is the exercise 18.12
describes, for the reason it gives: a check that still passes with the fault
put back is not checking the fault.

**`make prepush` runs `make litexl-check`**, since 0.10.24 - before `shot`,
because the check leaves a lean `LITEXL=1` image in `build/kosmos.elf` and
`shot` builds the ordinary one again before it takes its picture. It costs
the gate a build of its own and two boots, 1 min 49 s together under TCG.

## 18.15 Quake, checked through what it says

```
make test                               # the scanner, and a pak-sized region
make quake-check PAK=/path/to/pak0.pak  # the game, on a QUAKE=1 image
```

| check | run by | what it establishes |
| ----- | ------ | ------------------- |
| `tools/test_scan.c`, 19 checks | `make test` | the scanf family's scanner: `%f` writes a float and nothing past it, and nothing past a length is read - which is how `fscanf` reads a demo inside a pak |
| "mem: a region the size of Quake's pak" | `make test` | a 4563-page region is made, reached at its last page and given back whole, and one page over `MEMOBJ_PAGES_MAX` is refused |
| `tools/run_quake.py`, 6 checks | `make quake-check` | a window; the engine's start-up; the demo and the map it loads; more than 32 colours in the window; a command typed at Quake's console answered; Control-C closing it without a fault |

**Both of `make test`'s new checks have had their fixes undone to check
them.** With `%f` storing a double and numbers copied past the length,
`test_scan` fails exactly the three checks about those. With
`MEMOBJ_PAGES_MAX` put back to 16 MB, the region test is the one of 146 that
fails.

**The console command is the check that says keys arrive, and it is counted
twice.** Quake echoes a line when it is entered and `echo` prints the word
again when the command runs, so two appearances mean the keys arrived and the
command ran, and one means only the first.

**`make quake-check` is not in `make prepush`**, and cannot be: the pak is not
in the repository and will not be. Its first run passed all 6. With
`quake.lua` withholding keys from the engine it fails at the console command,
0 of 2, so what that check counts is keys Quake received rather than an echo
from somewhere else.

## 18.16 A clipboard that was never broken, and a control that was not one

The display harness failed its clipboard phase four runs in a row while
0.10.25 was being gated: "Control-W v changed nothing in the gallery's text
field". 0.10.24 had passed it that morning, and a clean 0.10.24 built beside
it passed again, so the revision was bisected: the region cap put back, then
`quake.lua` taken out of the image. It failed both times.

**Then both images were run once more with every screendump the phase takes
saved, and the result flipped**: 0.10.24 failed, 0.10.25 passed. The
pictures said why. `wm machine,gallery` starts both, and the gallery asks for
60,90, which is under the report. When `machine` gets its window first, the
window manager moves the gallery into a free quarter (`taken_at` in
`wm.lua`), and every click the harness aimed at the top left lands in the
report - so the paste goes to a read-only editor, and nothing changes.

**The mistake is §18.11's, in another shape.** One passing run of the old
image was taken as the control, when the thing it had to hold still was a
race. A comparison against a race needs the race settled in both of its
runs, or enough runs of each to see it go both ways.

The fix is in the harness, not the system. The window manager prints where it
put every window, unconditionally, and the phase now reads both lines and
makes each click an offset into the window it is meant for, checked to be
clear of the other one's frame. It has passed in both layouts: three runs
with the gallery placed first, and one with the report forced first - a
program written at the shell asks for the gallery three seconds after the
report opens, which is the layout that failed, and the harness as it was
fails that run again.

## 18.17 LICENSE held to the tree, and one image with everything in it

| check | run by | what it establishes |
| ----- | ------ | ------------------- |
| `tools/test_licences.lua` | `make test` | LICENSE is read the way About reads it - its terms, every entry and detail, nothing stray - and every directory under `runtime/upstream/` and `lua/upstream/` is named in an entry |
| "licence: the image carries LICENSE" | `make test` | the file About reads is in the image's asset table |
| a MEGA link | `make prepush` | Doom, the browser, Lite XL and Quake link into one image with no name defined twice, and the image ends before its heap |

**Each check has had what it guards taken away.** With musl's entry cut out
of a copy of LICENSE, `test_licences.lua` fails and names
`runtime/upstream/musl-math/`; with one detail line at four spaces instead
of six, it reports the line as stray. Given eight megabytes instead of
sixteen, the MEGA link stops at `user/user.ld`'s assertion. With `LICENSE`
left out of the asset table, "licence: the image carries LICENSE" is the one
test of 147 that fails.

## 18.18 A disk that arrives with the kernel

| check | run by | what it establishes |
| ----- | ------ | ------------------- |
| `memdisk` in `tools/run_x86.py` | `make test` | a kfs image handed over as a Multiboot 1 module is the disk the machine mounts - the boot log names it, `diskinfo` reports its 16384 sectors and a file written into it on the build machine comes back - with an empty NVMe drive attached that it has to win over |

**It has had what it guards taken away.** With `keep_disk_out_of_ram`
disabled, so the allocator is handed the module's pages, the boot log
reports 510 MB of RAM at 1 MB instead of 487 MB above the module,
`diskinfo` reports no disk, and the file never comes back.

**The Multiboot 2 half was checked by hand once**, while `run_uefi.py`
booted a GRUB ISO with no disk: a USB image made with `mkusb_image.py --disk`,
under OVMF as a USB stick, with the boot log saying `a disk from the loader:
8192 KB`. The T14 has since booted a stick from `make MEGA=1 usb` and run Doom
and Quake off its disk. Since 13 September `run_uefi.py` boots the stick
`make usb` writes, through Kosmos's own loader, and a stick with the 32 MB
disk passes the same checks (§18.42).

## 18.19 The desktop, and names of sixty-four characters

| check | run by | what it establishes |
| ----- | ------ | ------------------- |
| `tools/test_iconlayout.lua` | `make test` | new icons fill the first column down, then the next; a placed icon keeps its place and the next new one does not land on it; a place off the screen is pulled back onto it; a name too long for two lines keeps its extension - nine checks on the build machine, with no guest |
| `desktop` phase of `tools/run_screenshot.py` | `make screenshot` | `wm desktop,deskbar`, with an empty `/home/.startup` so nothing opens over it, puts the backdrop at the strip's height; the window manager's stamp is legible *through* the desktop, which is the layer a wallpaper is painted in; Drive dragged out of its cell is drawn where it was let go, and `desktop_x` and `desktop_y` say the same at the prompt; dragged onto the Trash it goes in and the Trash's picture changes; Drive is a launcher for Tracker at `/`, with the Trash and the cheat sheet beside it |
| `L-RAMFS` and `L-HOME` in `tools/run_queries.py` | `make test` | a 64-character name reads back on the disk, and in memory inside a 64-character directory - 136 bytes of path, where `/ramfs` used to hold 128 |

**Each has had what it guards taken away.** With the window manager neither
placing nor refitting the backdrop below the strip, the desktop phase fails
on its first check and the log reads `wm: window Tracker at 0,0 1920x1080`.
With the drop not writing `desktop_x` and `desktop_y`, the icon is still
drawn where it was let go and the prompt answers `DESK-AT nil nil` - which
is the whole difference between a picture and a fact. With the desktop not
making the Trash, Drive cannot be thrown away and the desktop holds
`Drive,cheatsheet.html`. `test_iconlayout.lua` fails when an overlap is
never seen, and again when a long name is cut instead of keeping its end.

**And one of those controls found a hole in the check itself**, which is
the argument for running them. Two runs failed saying there was nowhere
bare to drop on, which had nothing to do with what was sabotaged: the phase
looked for somewhere in one band of the screen, where the application the
`deskbar` phase starts sometimes sits. Worse, the step before it - Drive is
drawn in its cell - passed on a screen with *no desktop on it at all*,
because it only asks whether pixels differ from the desktop's colour. The
phase now waits for the desktop to be on the screen before looking for
anything on it, and searches all of it.

**The stamp stands in for a wallpaper**, which a harness booting without a
disk cannot put on the machine. Both are painted by `draw_desktop`, in the
pass that runs only where no window reaches, so a desktop that occludes
hides the one exactly as it hides the other - and the stamp is in the image
already. With the backdrop occluding again and copied rather than blended,
the phase fails with `the window manager's stamp in the bottom-right corner
is not visible through the desktop`, and every other check in it still
passes.

## 18.20 A name in /app, and what a process takes with it

| check | run by | what it establishes |
| ----- | ------ | ------------------- |
| "ipc: an endpoint ends with its process" | `make test` | a server that takes a client's call and is killed before answering gives its endpoint back to the pool, the client is woken with "the endpoint was destroyed", and the capability left in the parent names nothing |
| "app: a dead holder's name is taken back" | `make test` | the real `appfs`, spawned as role 14, gives `wm` again after a holder that destroyed its endpoint without unregistering and after one that was killed, and a process holding only the registry looks `wm` up and reaches the holder still there |
| `registry` in `tools/run_screenshot.py`, 3 checks | `make screenshot`, and so `make prepush` | `wm`, Control-C, `wm` again: each time, a program the window manager launched starts another with `run` and no shares, which looks up `/app/wm`, calls it, and says what `/app` holds |

**The display phase's first start is its control.** It runs before any other
phase starts a window manager, so nothing in the registry can be stale yet:
the probe has to be answered there, and a failure on the second start is the
restart's. It is checked by what the probe prints rather than by the screen,
because nothing in it opens a window.

**All three were run on the tree as it was, before either half of the fix.**
The two suite tests were the two of 149 that failed, and the phase passed its
first start and failed its second with `no such path: /app/wm` and `/app`
holding `['wm', 'wm2']` - the Tracker's failure, reproduced.

**And each half has been taken away on its own, on the test image.**

- *`process_exit` not calling `ipc_endpoints_release`:* "ipc: an endpoint
  ends with its process" fails with `the killed server's endpoint was not
  given back: 10 in use, 9 before`, and the registry test fails at its killed
  holder with `a holder that was killed kept its name: the next was
  registered as wm2`. The display phase was not run for this one, and should
  not notice it: a window manager stopped with Control-C destroys its own
  endpoint on the way out, which is why the kernel's half needs a test of
  its own.
- *`appfs` not calling `forget_the_dead`:* the registry test fails at its
  first holder with `a holder that destroyed its endpoint and left kept its
  name: the next was registered as wm2`, and the kernel test passes.

**The first of those controls found a check that failed without saying
why.** The kernel test counted the pool last, behind a receive with a
500-tick timeout, and with the kernel's half removed that receive had not
come back when the suite gave up on the role: `not ok`, and nothing else.
The count needs no waiting - `process_exit` gives endpoints back before a
wait can return - so it comes first now, and the same control produces the
sentence above.

**Three failures in those runs were not these tests.** `sched: the policy is
pluggable` failed in two of the three control runs and passed in the fixed
and unfixed ones; `state.md` has recorded it failing about one run in three
for a long time. And with `forget_the_dead` removed, the two tests after the
registry test failed too. The first says why: `child 156 exited 1`, where 156
was the failed test's `appfs`. Its endpoint had ended with the role that made
it, so it stopped, and its end was reported to the next role as that role's
own child, because a child keeps pointing at its parent's slot after the
parent ends. The second failed without a message. `state.md` has the fault
as an open item.

**`make test` then failed on x86-64 alone, and the registry test had found a
fault in the kernel rather than in itself.** The suite passed 149 of 149 on
AArch64 and gave `not ok 119 - app: a dead holder's name is taken back` on
x86-64, with no message: the role never ended. Its killed holder serves in a
loop, and on x86-64 a kill was checked only when an interrupt returned to
ring 3 - the `syscall` stub in `arch/x86_64/user.S` went back with `sysretq`
and never looked. So the holder came back from its aborted `receive`, went
straight into another, and was waited for for ever. The kernel test had shown
the same thing more quietly: its killed server ended with its own `exit(1)`
rather than -1. `trap_syscall_leave` makes the check after every syscall, as
AArch64 always has, and the x86-64 suite passes 145 of 145 with every killed
process ending -1. The failing run is that fix's control.

## 18.21 One bar, a menu made of files, and the right button

| check | run by | what it establishes |
| ----- | ------ | ------------------- |
| `tools/test_deskbarmenu.lua`, 9 checks | `make test` | the folders under `/home/Deskbar` are the sections, in their own order; only `kind == "launcher"` is an item; a folder inside a section is a submenu and submenus come before launchers, each sorted; a launcher carries its program and its arguments, with no arguments reported as an empty string rather than nothing; a folder that contains itself is read to a depth and then stops |
| `tools/test_filetypes.lua`, 13 checks | `make test` | the `type` attribute beats the extension, a leading dot is not an extension, a launcher written today is of type `launcher` and one carrying only `kind` still is, `launcheredit` handles the type, and a launcher is never recognised by its name |
| `context` phase of `tools/run_screenshot.py`, 3 checks | `make screenshot`, and so `make prepush` | a right press on a probe's button reaches its `on_context`, does **not** press the button, and a left press on the same pixel still does |
| `deskbar` phase | the same | a bare `wm` opens the strip at 0,0, and walking the Kosmos menu into Applications and choosing the first item opens a window |
| `desktop` phase | the same | `wm desktop,deskbar` with an empty startup list puts the backdrop at the strip's height |
| the third `deskbar` check | the same | **clicking the bar does not resize the desktop.** `pointer_pass` raises whatever is under the pointer before asking what kind of window it is, so a click on the Deskbar raises the strip; `raise` counted the strips while the window was out of `windows`, found none, and gave the backdrop the whole screen - then put it back and resized it again. Every click reallocated and repainted an 8 MB surface twice |

**That one was found on hardware and could not be seen under QEMU.** On a
2.6 GHz ThinkPad it is a flicker across the whole screen on every click; in
emulation it hid in the noise and the gate was green throughout. The check
counts what the window manager says - it announces the backdrop's size every
time it sets one - so it now catches in emulation a fault that only a real
machine could show. Put the census back between the `remove` and the append
and it fails with `clicking the Deskbar resized the desktop 2 time(s)`.

**Each host test has had what it guards taken away.** With anything in the
folder counting as an item, `test_deskbarmenu` fails on the file that is not
a launcher; with launchers before submenus it fails on the order and on four
more; with no depth guard it blows the stack at 62,000 levels. With the
launcher not read out of `kind`, `test_filetypes` fails on the launcher that
predates the `type` attribute; with the attribute no longer beating the
extension it fails on three.

**The `context` phase's controls found the check reporting the wrong half.**
With the kit no longer saying it understands `button`, nothing arrives and it
fails saying so. With the kit routing a right press like a left one, the
button *is* pressed - and the check first reported "on_context never fired",
which is a symptom rather than the fault. The order was swapped so the
sentence that says what happened comes first.

**And the redesign broke six checks that encoded the old shape rather than
the behaviour**, which is worth listing because every one of them looked
like a bug in the code:

- The x86 pointer check clicked the centre of the Deskbar's window, which is
  now the middle of a bar as wide as the screen.
- `check_deskbar` expected exactly one window after `wm` - the Deskbar's own
  tab. A strip is chrome and has none.
- The baseline was sampled while the Deskbar's startup applications were
  still opening, so "a window appeared" came true without the menu being
  touched. **That passed, which is worse than failing.**
- `count_windows` counts long runs of tab colour, and the bar is
  tab-coloured, full width, and redraws every second as its clock ticks, so
  the count wobbled between frames. It takes a `from_y` now and the bar is
  not counted.
- Even below the strip the count was wrong: a new window overlapping an old
  one merges into its cluster. The check asks the window manager whether it
  opened a window instead - the rule this file already states for everything
  else, *asked of the thing that decides it rather than read off the picture
  alone*.
- `desktop_drawn` looks for mostly-desktop-colour below the strip, and the
  Deskbar starts four applications that cover it. The phase writes an empty
  `/home/.startup` first, because `startup.lua` treats absent and empty as
  different things on purpose.


## 18.22 A canary for bytes nothing can write

**The fault this exists for has never been reproduced here**, and that is
the whole shape of it.

A ThinkPad T14 booted from a USB stick with a 64 MB disk carried as a GRUB
module dies at the last stage: `process 8 (diskfs) ended, code 1`, and the
desktop never comes up. The same kernel with an 8 MB module boots perfectly
and has done for weeks. The same image under QEMU with OVMF, eight cores,
sixteen gigabytes and an NVMe drive boots every time.

**The first thing that was missing was a way for the server to say
anything.** `diskfs` is spawned with one capability - its own endpoint - and
`sys_write` refuses any process that does not own the console, deliberately.
So `print` reaches nobody, and so does the `say` in `user/init/main.c` that
reports a Lua error: a Lua error in that server has been invisible since it
was written, and `ended, code 1` was the entire diagnosis. What is left is
the exit code, which init prints, so `diskfs_main` now spends five of them:

| code | what raised |
|---|---|
| 11 | `sys.libraries()` or its chunk, for a reason not below |
| 12 | `kfs.lua` would not load or run |
| 13 | the serve loop |
| 14 | out of memory |
| 15 | **a syntax error** - the source arrived corrupted |

1 is deliberately not among them: that is what `main.c` returns for a Lua
error it could not print, and a diagnostic that cannot be told apart from
the fault it diagnoses is worse than none. Using it cost a boot.

The machine answered **15**. The Lua source of `/lib` did not parse - so it
is not memory exhaustion, not `kfs`, and not the disk. It is the same fault
as `beep`'s `/lib/audio.lua:1: unexpected symbol near '$'` on the same
laptop, months earlier: two symptoms, one bug.

### What the instrument is

Those bytes live in one place. `process_create` maps the read-only half of
the userland image straight out of the kernel's own copy - one set of
physical pages for every address space - so the code, the fonts and the Lua
source of every program are these bytes and no others. No process can write
one: the mapping is read only, and the pages sit below the region the
allocator hands out, so nothing the kernel allocates can land there either.

So `tools/bin2c.py` writes down what it emitted - a checksum of the whole
blob and one per 4 KB page - and the kernel asks, **four times in one boot**,
whether that is still what is there:

| asked | what is between it and the last |
|---|---|
| as the loader left it | nothing of this kernel but the trap table |
| with the page tables built | the allocator chose a region; the tables were built over it |
| with the devices up | the display, the input devices, the storage controller - everything that hands a physical address to something that is not the processor |
| before init is built from it | the rest |

Four rather than one because a boot of that machine is expensive: a single
run says which gap the bytes changed under instead of three runs bisecting
it. A change is reported with the count of bad pages, the address of the
first, and the sixteen bytes it holds instead - which says whether what
landed there reads as somebody's DMA, somebody's text, or zeroes.

Beside it, **the loader's whole memory map**, reserved entries and all.
`consider` in `hal/pc/memory.c` sees type 1 and throws the rest away at the
moment of the walk, which is right for deciding what to manage and is why
nobody could answer "what does the firmware think is at this address" - the
first question to ask about memory that changes where nothing can write it.
QEMU's map is nine entries under `-kernel` and twenty-five through GRUB; a
laptop's is longer, and none of it was visible.

**The cap was wrong on the first try and the boot said so**, which is the
only reason it was noticed. Twenty-four was chosen as "more than any machine
would have" and OVMF's map is *exactly* twenty-four, so the array filled to
the brim and looked complete - the same failure as a screenshot check that
passes because nothing happened. The count seen is now reported beside the
count kept, and the cap is forty-eight.

**And the first thing the dump found, on the first boot it ran:** three
entries between `0x00800000` and `0x00900000` come back type 4 - reserved,
and to be preserved - and this kernel runs from `0x00100000` to past
`0x01300000`, straight through them, with the userland image on top. It
survives under emulation, so overlapping is not sufficient on its own; what
that says is that nothing writes those pages here after the loader hands
over, which is exactly what a real machine's firmware does not promise. The
line naming it is in the dump: `** UNDER THE USERLAND IMAGE **`.

### And the test, which is of the instrument

`build/host/test_imagesum`, 16 checks, in `make test`.

**The two halves are written in different languages and neither can check
the other at run time.** `bin2c.py` computes the checksums on the host in
Python; `kernel/image_sum.h` recomputes them on the machine in C. If those
ever disagree the boot log calls a healthy image corrupt on every machine -
and a canary that cries on a healthy boot is worse than none, because the
next real one is read as the same false alarm. Nothing at run time can catch
that: the two never meet except on the machine whose memory is in question.

So the test compiles against a blob **the real script generated during the
build**, from a fixture the Makefile writes. 10001 bytes rather than a round
number, because the last page has to be short: that is the case every real
blob has, and hashing it over the padding instead of its own bytes would
report one permanently bad page on every boot for ever.

**The negative control:** one byte of a copy is changed and the walk has to
notice, name the page it is on, and name only that one - once in the middle
of the blob and once in the short last page. With `IMAGE_SUM_FNV32_PRIME`
changed by two, 7 of the 16 checks fail, including both published FNV-1a
vectors. A checker that always answered "fine" would pass the first three
and fail the rest.

What the test cannot say is whether the ThinkPad's memory is sound. What it
says is that **when the instrument reports something on that machine, the
reading can be believed** - which is the only claim a host test was ever in
a position to make about a fault it cannot reproduce.

---

## 18.23 A window that kept its old size, and one that redrew for nothing

Three bugs in the Terminal, reported in one sentence each - *the content does
not resize*, *the content flickers*, *is it redrawing while sitting still* -
and the third is the cause of the second.

### The grid that never grew

`terminal.lua` built its view with the width and height of the window it
opens at, and the widget kit resizes a child only if that child said it
**follows** an edge. The default is left and top, "something that sits where
it was put", so the view stayed 624x560 inside a window that had become
912x716, with the window's own colour around it.

Nothing was wrong with the arithmetic. `draw` divides `self.w` and `self.h`
by the monospace cell on every pass, so the rows and columns were always
correct *for the size the view believed it was*. This is the shape of bug
where reading the drawing code proves it right and the bug is one level up.

**The check drags the sizing grip and measures the black.** `theme.console`
is 0x0b0b0b, and while the Terminal is the only window drawn in it the
bounding box of that colour is the character grid - and measuring the grid
rather than the window frame is the point: the frame moves because the window
manager moved it, whether or not the application noticed anything. Log View
draws on the same black since 0.10.47, so the `compositor budget` phase, which
has both on the screen, measures the grid from the Terminal's own corner
instead (§18.31).

The negative control was run before the fix was kept, by removing the one
`follow` line and rebuilding:

```
before   console 622x558 at (99,49)
after    console 622x558 at (99,49)      grew by 0 x 0
```

and with it:

```
before   console 622x558 at (99,49)
after    console 912x702 at (99,49)      grew by 290 x 144
```

290x144 is exactly the room left on a 1024x768 screen, which is the other
half of the check: a number that matched the drag rather than merely being
larger.

### A hook that was emptied and not deleted

The Terminal repainted itself once a second, for ever, with nothing to
redraw. `pump` was a 0x0 view that drew nothing and whose `tick` had been
emptied when its work moved into `on_frame` - the right move, and the empty
function was left behind.

```lua
local pump = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function pump:tick()
end
```

**An empty function is not nothing to the kit.** `window:add` tests whether a
child *has* a `tick`, because a child that does means "this changes on its
own, like a clock" - so the window was given a `tick_every` of one second and
the run loop marked it dirty on that clock whether or not anything had
happened. The whole window went to the compositor: the banner, every run of
every line, sixty-odd drawing commands batched into several messages.

That is also where the flicker came from. The window manager writes a
window's surface on every batch and holds the *damage* until the last one, so
a frame is never composited half-drawn **by its own damage** - but the
surface is live memory, and anything else damaging the screen mid-frame
composites a terminal that has been cleared and not yet re-texted. The
Deskbar clock damages once a second. So did this.

The class, which is the part worth keeping: **a hook that is emptied should
be deleted, because having the hook is the signal.** The kit cannot tell an
empty `tick` from a full one and should not have to.

### And how often it wakes

`win.poll_wait_ticks = 1` was set once and never cleared, so a Terminal at
its prompt woke every scheduler tick - sixty times a second - to serve
children it did not have. The reason it exists is real: a program's `write`
blocks until this loop answers it, and `ls` came out one line a second before
it was added.

Typing never depended on it, and that is what makes the fix safe. A held poll
is answered the moment the window manager has an event for the window, so the
wait bounds how long a *program* waits to be answered and nothing else. It is
raised while `busy`, and for half a second after anything writes - not
`busy` alone, because a program can leave something of its own behind and
whatever it left inherited this window as its console.

---

## 18.24 Four quadrants, and a PNG that must not decode

`gfx.jpeg` is stb_image, and stb_image is not what the test is for. What is
on trial is this *build* of it - `STBI_ONLY_JPEG`, `STBI_NO_STDIO`,
`STBI_NO_THREAD_LOCALS`, the last of which is the difference between linking
and not - and the conversion in `user/lib/jpeg.c` from stb's RGBA in memory
order to this system's `0xAARRGGBB` word on a padded pitch.

`assets/images/test-quads.jpg` is four solid 64x64 blocks: red, green, blue,
near-white. Solid, because **JPEG is lossy and there is no byte-for-byte
answer to check against.** What a correct decode guarantees is that the
middle of a flat area comes back the colour it went in as, within the error
the quantiser is allowed; the edges between blocks are where a lossy codec
rings, so nothing looks at them. The tolerance is 16, which is comfortably
inside "the same colour" and nowhere near "a different quadrant".

The PNG test pattern is a poor JPEG fixture for exactly the reason it is a
good PNG one: its whole point is per-row filters and an alpha channel, and
JPEG has neither.

**Two negative controls, because without them the four colour checks prove
nothing** - they would all pass on a function that returned a 128x128 surface
of the right colours whatever it was handed.

1. **The PNG, decoded as a JPEG, must fail.** This is the confusion that can
   actually happen: the window manager picks a decoder from the name on the
   end of the file, so a picture named wrongly has to be refused rather than
   turned into noise.
2. **A JPEG that stops being one halfway through.** Sixty-four bytes of the
   compressed data replaced - the shape of a truncated download or a bad
   block. It may refuse or it may produce something ugly; what it must not do
   is take the process with it, and that is the check.

The fixture is a committed file rather than a generated one, deliberately. A
decoder's regression test wants bytes that never move: a fixture regenerated
by the build could start failing because the *encoder* on the build machine
changed, which is a day spent on the wrong question.

## 18.25 A Super Nintendo, and a name too long for /app

```
make test                               # a 46-byte name in /app, at the prompt
make snes-check ROM=/path/to/game.sfc   # the console, on an SNES=1 image
```

| check | run by | what it establishes |
| ----- | ------ | ------------------- |
| `tools/run_queries.py`, `A-LONG` | `make test` | a 46-byte name registered in `/app` the way `ui.window` registers a window comes back as its first 23 bytes, rather than as an error that ends the process that asked |
| `tools/run_snes.py`, 5 checks | `make snes-check` | a ROM from `/home/roms/snes` started; a window; the loop reported its rate; more than 8 colours in the window; no fault |

**The `/app` check has had its fix undone to check it.** With the namespace
kit packing the whole name again, `run_queries.py` stops at that check with
the error first seen when a ROM's window died opening - `bad argument #3 to
'pack' (string longer than given size)`. With the cut back it passes, 17 of
17.

**`make snes-check` reports a rate rather than judging one.** `snes.lua`
prints its frames a second every ten seconds and the harness prints those
lines back; a TCG frame rate is not a threshold worth failing on. It also
presses Enter four times to get past title screens, and **does not check
that the presses arrived** - the picture shows it, and the picture is saved
as `build/snes/screen.png` and `window.png` for exactly that, because more
than 8 colours says the window is drawn and not that it is drawn right.

**It is not in `make prepush`**, for `quake-check`'s reason: no ROM is in the
repository, and none will be. Its first run failed - the window died
opening, which is the `/app` bug above. After the fix it passed all 5, at
about 22 frames a second of 60, on Super Mario All-Stars' game-select screen.

## 18.26 Sound that is heard, and the rest of a class

```
make test                               # long names at /dev and /bin, at the prompt
make snes-check ROM=/path/to/game.sfc   # now with the sound recorded
```

| check | run by | what it establishes |
| ----- | ------ | ------------------- |
| `tools/run_snes.py`, 7 checks | `make snes-check` | the two new ones: the ROM opened a sound stream, and the device played more than five seconds that are not silence |
| `tools/run_queries.py`, `F-LONG` | `make test` | a 40-byte name under `/dev` and an 80-byte one under `/bin` are answered by their protocols rather than raised inside the namespace kit |

**`KOSMOS_AUDIO_WAV=path`** is the harness's second hook beside
`KOSMOS_DISK`: it gives the guest virtio-sound with QEMU's WAV writer behind
it, so what the device played is a file when the guest has gone. The default
stays silent, for the reason it stays diskless.

**Both checks have had their fixes undone.** With `snes.sound` withheld, the
stream still opens and the device plays nothing - "the device played 0.0 s".
With the `/dev` and `/bin` guards removed, the prompt prints
`F-LONG false false init:698: bad argument #4 to 'pack'`.

**The machine can tell sound from silence; whether it is the right sound was
checked once, natively.** The same ROM run on the build machine with no
input made 3583 sounding periods in its first 1500 frames, and the first 479
of them - console frames 101 to 355 - are in the guest's recording byte for
byte and in order, until the harness's key presses make the two runs
different games. It is not a permanent check, because it needs a ROM and a
native build of the core. What it established is that nothing between
`snes_setSamples` and the device changes a sample: not the slot writing, not
the channel order, not the server's unity mix.

---

## 18.27 A power button, pressed twice

The driver primitives had one piece no suite could reach: the blocking half of
`SYS_IRQ_WAIT`. A kernel test can call `irq_deliver` the way the handler does,
but a thread that waited with nothing pending waited for ever - only a device
could end that wait. So the test was a device. Since 0.10.52 a wait can have a
deadline and the suite reaches that half too (§18.35); the power button stays,
because it is still the one test in which a device is what ends the wait.

QEMU `virt` wires its power key to a PL061 GPIO controller, and QMP's
`system_powerdown` pulses it. `user/servers/powerbutton.c` finds the
controller, maps it, claims its interrupt and blocks; the display harness
waits for `powerbutton: waiting on line 3`, presses, and expects
`powerbutton: pressed`.

**Twice, because a one-press test cannot fail in the way that matters.** The
kernel masks a line when it delivers it, and only `SYS_IRQ_ACK` unmasks it. A
driver that never acks reports the first press and never hears the second -
and the negative control, run by deleting the ack and rebuilding, printed
exactly that:

```
control applied: the driver never acks
FAIL: the first press was reported and the second was not.
  powerbutton: waiting on line 3
  kosmos> powerbutton: pressed
```

The two failure messages are different on purpose. A first press that never
arrives points at delivery or the wait; a missing second points at the ack,
or at a controller cleared after the ack instead of before it. A test that
says "the button did not work" for both would leave that to be worked out.

aarch64 only. The PC's power button is an ACPI event rather than a GPIO line,
so there is nothing there for this driver to find - and on that board it asks,
is told "no device", and exits.

---

## 18.28 A codec that says where it stopped

The ThinkPad's boot log said `no sound: an HDA codec with no output path`,
and 0.10.34 added a topology dump for exactly that machine - every widget, its
connections, each pin's configuration. On the next boot the dump never printed.

That silence was the evidence. The dump runs only after an audio function
group has been found and nothing routed, so a missing dump means
`find_widgets` gave up *before* routing, at one of three earlier exits: the
codec's root node did not answer, no function group was of type audio, or an
audio group would not say how many nodes it had. All three printed the same
sentence as a routing failure. Three faults, one message.

**Counted, not printed where they happen.** Each skipped group increments a
counter, and one line is printed only on the way out with nothing, so a codec
that works - one with a modem group skipped on the way to its audio group,
say - pays nothing at all.

Checked under QEMU with `tools/run_x86.py`'s `boot()`, an `ich9-intel-hda`
and an `hda-output` codec, by forcing each exit in turn:

```
baseline, a working codec:
  -> sound: Intel HDA, 44100 Hz stereo, 256-frame periods (5 ms), 4 deep
  beep: 440 Hz, 333 ms of sound in 286 ms, 58 periods
root node forced silent:
  -> the codec did not answer its root node, so nothing about it is known
every function group forced non-audio:
  -> codec: 0x01 function groups from node 0x01; 0x00 did not answer,
     0x01 not audio (type 0x01), 0x00 audio but gave no node count
```

The baseline matters as much as the controls: it is what shows the new line
costs a working machine nothing. The line is for the ThinkPad, whose processor
is a Tiger Lake. If it says the root node did not answer, the likely cause is
an audio controller in Intel's DSP firmware's hands, where the codec is not
reachable with plain HDA commands at all - and that would be a far larger
piece of work than a graph walk.

---

## 18.29 A full-screen picture and a maximised window, at 1920x1080

The ThinkPad refused its own desktop. Dragging a Terminal to full size logged
`no room for a 1916x1016 surface: the kernel refused 1905 pages` on every
step and the window stayed put, and a JPEG wallpaper would not load. The
window manager was allowed 48 MB of mappings, like every process, and at
1920x1080 it holds more than that at once: its backbuffer, the backdrop, the
wallpaper, a maximised window, a second surface for that window while it
resizes, and a decode. Its allowance is derived from the framebuffer now
(`map_budget` in `kernel/syscall.c`).

The display harness's `compositor budget` phase is that afternoon in QEMU:
the desktop, a Terminal, Log View and `photo:test-screen.jpg`, then the
Terminal raised and dragged by its grip to the bottom-right corner. It passes
when nothing is refused and the Terminal's grid grows by at least 800 pixels.
**It checks the property rather than a number**, so it does not care how many
pages that took.

**It took three attempts to make it able to fail.** The first pressed the
grip while Processes was stacked over it, so nothing was resized and nothing
was refused. The second raised the Terminal first and passed against the
flat budget too. A 4:2:0 JPEG leaves the decoder about three megabytes less
scratch than the ThinkPad's 4:4:4 wallpapers, and `malloc`'s arenas are
never given back, so that difference is still counted when the drag starts.
With a 4:4:4 picture and one more window it fails exactly as the ThinkPad
did:

```
=== with the derived budget: must pass
PASS: 2 compositor budget checks
=== control, the committed flat budget: must fail
FAIL: the window manager was refused 3 surface(s) at 1920x1080 with a
full-screen picture decoded - first: 'wm: no room for a 1920x1044 surface
(7830 KB): the kernel refused 1958 pages'.
```

The lesson is the one the power button taught from the other side: **a
negative control is not a formality.** Before it ran, this phase was a
passing test that could not fail.

## 18.30 A bar that shows the focus where it went, at once

Diego, on the ThinkPad: when he moved the focus to another application, its
button on the Deskbar took "like half a second" to show as selected. The bar
learned where the focus was by asking the window manager for its list of
windows on its tick, which is once a second.

**Measured before anything changed.** Two notes the window manager writes
under `wm trace`, each stamped with the counter at the moment it is written
rather than when it is printed (`trace_us`): `focus <title> at <us>` when
the top of its stack changes, and `draw <title> at <us>` when a window
finishes a frame. The difference is how long the bar took to finish a frame
after the focus moved, on the one clock both happened on. The bar's frames
came about 1140 ms apart:

| how the focus moved | focus to the bar's next frame |
| ------------------- | ----------------------------- |
| Control-W Tab, five times | 463, 1029, 290, 564, 648 ms |
| a press on a window | 592 ms |
| a press on the bar's own button | 92 and 102 ms |

The bar's own presses were never the problem: it paints what it asked for.
Everything else waited for the tick. The window manager now posts a window
that asked with `watch` a `windows` event whenever its list would answer
differently (`tell_watchers`, `ui.md` §16.13).

**The `deskbar focus` phase of `tools/run_screenshot.py`** starts
`wm trace,deskbar,clock,calc` and moves the focus three rounds of three
ways - a press on the other window's tab, Control-W Tab, and a press on the
other window's button on the bar - and every one has to be shown within
400 ms, with the picture after it drawing that window's button pressed and
the other not. Then it minimises the focused window by its own box, and that
button must not be drawn pressed: the window stays at the top of the stack,
so it is still reported `focused`, and the bar drew it as the window you are
in. Every path now costs what the bar's own press always did, which is its
paint - about a tenth of a second here, with `trace` printing every stage of
every pass. **That is a QEMU number and the bound is not a speed claim**; it
is there to tell a bar that is told from one that asks on a clock.

```
=== the fix
deskbar focus: the bar finished a frame 124, 132, 121, 116, 145, 133, 115,
133, 117, 119 ms after the focus moved
PASS: 3 deskbar focus checks
=== control: the old bar, a 300 ms bound, before the waits were stepped
deskbar focus: the bar finished a frame 287, 1047, 285, 104, 928, 284, 101,
906, 276, 97 ms after the focus moved
FAIL: the Deskbar showed a focus change more than 300 ms after it happened:
a press on the other window's tab to Calculator took 1047 ms; ...
=== control: the old bar, the phase as committed
FAIL: after a press on a window's tab moved the focus to Calculator, the
Deskbar's buttons for ['Calculator', 'clock'] are [(255, 212, 61),
(255, 212, 61)]; pressed is (204, 169, 48) ...
=== control: the fix, with the bar's old reading of `focused`
FAIL: Calculator, minimised by its own box, is drawn pressed on the Deskbar
- as the window you are in. ...
```

**The first control is the one that changed the check.** Every Control-W
Tab read about 280 ms against the old bar - under the bound - and every tab
press about a second. The harness's own rhythm, a screendump and the same
sleeps between each change, had put each kind of sample at the same point of
the tick every time, so the keyboard alone would have passed against the bug.
The wait before each change now grows by 170 ms and wraps, which walks the
samples round a tick of that size. **A test made of identical repeated
actions samples a periodic fault at one phase of it**, and a clock inside the
thing being tested is a periodic fault.

The second control failed on the picture rather than the time, and that is
the check doing its job: the stepped wait put a press just after the old bar
had asked for its list, so its next frame arrived within the bound and showed
the focus where it had been.

**Not covered, and each is its own task**: a press on the bar raises the
strip and gives it the keyboard focus, and a window minimised by its box
keeps the keys; and the bar repaints itself every second whether or not the
minute or a meter moved.
## 18.31 Log View, on black and following the log

Diego, from the ThinkPad: Log View should look like the Terminal - black, and
scrolling as lines arrive - and it had an overlapping title and grey text that
could not be read. All three were reproduced in QEMU at 1920x1080 before
anything changed; `ui.md` §16.14 has the causes and the design.

The display harness's `log view` phase is four checks, and **the conditions
are the ThinkPad's rather than the harness's**:

- **The BeOS palette.** Every other phase runs the dark one, where the window
  colour is #161b22, and there the old grey-on-grey window would have passed
  "the rows are on a dark ground".
- **IBM Plex Mono at 20 pixels** for the interface and monospace roles. At
  spleen's 16 the cell the old window laid text out on and the face it drew
  in are the same size, so the overlap cannot be seen at the harness's font.

What is logged is under the phase's control. `/ramfs/logger.lua` opens a small
window and prints when it is clicked: forty plain lines and one Log View
colours as a fault, then forty more and one it colours as a boot stage.
**Found by colour rather than by shape**, because the kernel stamps every line
in the ring, so every row starts with the same nine characters and a
one-character line is not a narrow row. **Printed on the release**, because
the window manager logs its first thirty clicks itself and logs a release
before it delivers it, so the harness's lines always come after its own.

1. **On black**: most of the text area is `theme.console`, with light text on
   it.
2. **Nothing overlaps**: at least four rows, each shorter than the row pitch,
   and the pitch no smaller than the face.
3. **It follows**: the fault line appears in the lower half of the view, under
   plain lines, with nothing touched.
4. **It holds, then follows again**: one row up, the second batch arrives, the
   rows on screen do not change and `new lines below` appears; one row down
   and the stage line is in view.

It runs on both boards, like every phase here: 4 checks in 18.6 s on aarch64
and in 18.2 s on x86-64, where the arrow keys arrive through the PS/2
controller rather than virtio.

### Four builds made to fail, each in one way

Each is the tree as committed with one change, run through this phase alone:

```
the old logview.lua
FAIL: Log View's rows are not on a dark ground: almost none of its window
is the console colour, and the commonest colour there is #d8d8d8.

rows measured on the bitmap cell: `MH` is 16, not gfx.height("mono")
FAIL: Log View's rows overlap: 24 rows of text 16 pixels apart in a
20-pixel face, and 0 of them as tall as that.

never takes the newest text, even at the bottom
FAIL: Log View has a console box but is not text on it: 57130 of 57130
sampled pixels are the console colour and 0 are light.
FAIL: a line logged while Log View was open never came into the lower half
of its view, under the plain lines printed before it.

takes the newest text even while scrolled back
FAIL: more was logged while Log View was scrolled back and nothing in its
corner said so.
FAIL: Log View jumped to the newest lines while it was scrolled back.

as committed
PASS: 4 checks in 18.6s
```

**Three of those controls found the check wrong before they found anything
else**, and each correction is a comment where it was made:

- **The old window's first failure said "0 of 0 sampled pixels".** Black text
  anti-aliased onto grey lands on exactly #0b0b0b here and there, and a box
  drawn round a handful of those is not a console. The box has to cover a
  quarter of the window now, and the message says the window is grey.
- **Rows 16 pixels apart in a 20-pixel face passed.** Plex Mono's bracket is
  short enough to leave a pixel between rows at that pitch, so "no row as tall
  as the pitch" did not see text laid out on the wrong cell. The pitch is held
  to the face's size as well, which is the cause rather than one of the ways
  it sometimes shows.
- **A window that always took the newest text passed check 4.** Three rows up,
  the held view was nothing but plain lines, and those look the same whichever
  batch printed them. One row up keeps the fault line in view, so a jump takes
  it away. And the check had waited for the note before comparing, so that
  window failed on the note and never reached the comparison: it now waits for
  the logger's last line on the serial port, then for the note, then compares,
  and reports the two separately.

A fourth mistake was the harness's alone, found by the change as committed:
the fault line's position was taken before the click that focuses the view,
the window manager logs that click as two lines, and every row had moved up by
two before the arrow moved it down by one. It is measured once the view has
settled after the click.

**The lesson is the power button's again** (§18.27): a check is not finished
when it passes, but when a build broken in exactly the way it names makes it
fail for exactly that reason. Two of these four would have shipped as passing
tests that could not fail.

### And a phase it broke

The first gate after rebasing failed somewhere else. The `compositor budget`
phase (§18.29) said the Terminal's grid had not grown - `(99, 49, 1570, 958)`
before the drag, `(99, 49, 1905, 1061)` after - while the picture it left
behind showed a Terminal filling the screen. That phase opens Log View beside
the Terminal, and it measured the grid as the bounding box of `theme.console`
on the whole screen, on the strength of a comment saying nothing else on the
desktop was that colour. Since this change Log View is, so "before" was a box
round both windows. It passed in the gate before the rebase and fails every
time after it; whatever differs between the two, a measurement any other black
window can inflate was not measuring the Terminal.

It measures the Terminal from its own corner now, along the first row and the
first column of the grid, which never hold text, until another window's frame
or the grid's own edge stops the walk. The same image, and a control whose
Terminal does not follow its window's edges:

```
as committed
PASS: 2 checks in 40.7s

the Terminal's view without `follow`
FAIL: nothing was refused and the Terminal's grid did not grow to full
size: (11, 507, 632, 1064) before the drag, (11, 507, 632, 1064) after.
```

§18.23 said the same thing about the colour, and is corrected.

---

## 18.32 Two USB controllers, and a stick on the second

`tools/run_x86.py` boots q35 with two `qemu-xhci` controllers, a
`usb-storage` device on the second and a `usb-kbd` on the first, and reads
what `user/servers/xhci.c` says. It passes when the driver reports two
controllers at different PCI addresses, exactly one USB 3 device, on the
second of them, the keyboard's USB 2 port with its speed unknown, and a
closing line that names both controllers. The first boot of the harness,
which has no USB controller, must hear nothing from the driver at all.

**Two controllers because the index is the part one cannot test.** A driver
that always asked for the first controller would find it, reset it and read
its ports perfectly, and on a machine with two it would never see the other.

What QEMU prints:

```
xhci: 00:03.0, version 1.0, 8 ports, 64 slots
xhci: 00:03.0 has no firmware handoff to make
xhci: 00:03.0 port 5, USB 2: a device, its speed unknown until the port is reset
xhci: 00:04.0, version 1.0, 8 ports, 64 slots
xhci: 00:04.0 has no firmware handoff to make
xhci: 00:04.0 port 1, USB 3: a SuperSpeed device (speed ID 4)
xhci: 2 controllers (00:03.0, 00:04.0), 2 ports with something plugged in
```

**Both halves were broken on purpose and watched fail.** With the port
registers read four bytes off, at PORTPMSC instead of PORTSC:

```
FAIL: 2 of 5
  the driver did not report exactly one device plugged in:
  ...
    xhci: 2 controllers, 0 ports with something plugged in
```

And with every index answered by the first controller, the same controller
four times and never the stick:

```
FAIL: 3 of 5
  the driver did not report two different xHCI controllers:
    xhci: 00:03.0, version 1.0, 8 ports, 64 slots
    xhci: 00:03.0 has no firmware handoff to make
  ...
    xhci: 4 controllers, 0 ports with something plugged in
```

**The first attempt at that second control proved nothing, and it is worth
saying why.** Written as `if (seen == 0)`, it left `index` unused, the build
stopped on `-Werror=unused-parameter`, and the script moved on to restoring
the file. A control that does not build has not run. It was rewritten as
`seen == (index & 0u)`, which uses the parameter and ignores it.

**And a keyboard on the first controller, for its port** - added the day the
ThinkPad ran the driver and named four USB 2 ports "Full-speed". That was a
field the specification calls invalid on a USB 2 port until the port is
reset (Table 5-27), and this step resets no port. QEMU shows the same thing
from its side: before any reset it reports its keyboard as High-speed. The
check wants that port reported with its speed unknown and no speed named on
any USB 2 port, and against the 0.10.48 driver it fails four of seven:

```
FAIL: 4 of 7
  ...
    xhci: 00:03.0 port 5, USB 2: a High-speed device (speed ID 3)
  ...
  a speed was named on a USB 2 port, where the field is invalid until the port is reset:
```

The same run also failed the closing line, which now names the controllers
because on the ThinkPad the second controller's lines had scrolled away.

**What this cannot test is the firmware handoff**: QEMU's controller has no
Legacy Support capability, so the driver says it has no handoff to make. The
ThinkPad runs that path first, and `usb.md` lists what its line can say.

---

## 18.33 A request wakes the window manager

**Two checks, one for each half.**

**The kernel's, in the suite:** `ipc: a caller ends a watched sleep`. A thread
sleeps for two seconds watching an endpoint, another calls it ten ticks in, and
the sleep must end long before the two seconds with the message still waiting
to be collected; a second sleep, taken with a caller already queued, must not
sleep at all. Input is left out (`or_input` false), because on x86 a latched
input flag can wake an input sleeper every tick and would end the first sleep
early whether a caller could or not. With the wake in `ipc_call` removed:

```
ok 55 - ipc: call and reply
not ok 56 - ipc: a caller ends a watched sleep
ok 57 - ipc: both arrival orders work
...
FAIL: 1 of 154 test(s) failed:
  not ok 56 - ipc: a caller ends a watched sleep
```

**The first version of this test panicked the kernel**, and the reason is a
rule of the suite worth knowing: `thread_block: this processor has no idle
thread`. It slept in the suite's own thread, which runs on core zero with
nothing behind it and must never block. The sleeping moved into a thread of its
own, and the suite's thread only yields until both report.

**The system's, in the display harness:** `wmlatency` opens a window, asks the
window manager two hundred times for events it does not have, and judges the
average against half a scheduler tick. Against the manager as it was, three
runs:

```
wmlatency: a round trip to the window manager takes 11.479 ms on average and 22.972 ms at worst, against a scheduler tick of 4.000 ms
FAIL: the window manager answers in 2.86 scheduler ticks on average. It answers when its sleep ends, not when it is asked.
```

and 11.661 and 11.311 ms. With the fix, 0.324, 0.309 and 0.314 ms - 0.08 of a
tick. With the window manager's `watch_input` call removed and nothing else
changed:

```
wmlatency: a round trip to the window manager takes 11.558 ms on average and 14.070 ms at worst, against a scheduler tick of 4.000 ms
FAIL: the window manager answers in 2.88 scheduler ticks on average. It answers when its sleep ends, not when it is asked.
```

and 11.716 and 11.178 ms in the other two runs, the same as before the fix.

`latency.lua` gained the right tick rate on the way: it had 100 Hz written into
it while the kernel ran at 250, so every threshold was two and a half times
looser than it read. The suites stand at aarch64 154/154 and x86-64 150/150.

---

## 18.34 The Super Nintendo at twice the size

`wm snes:--scale 2`, or a launcher carrying those words, opens the game in a
1024 by 960 window, every pixel of the console's picture a two-by-two block.
Two checks, because it can go wrong in two places, and neither needs a ROM.

**The pixels, on the host.** `tools/test_snesblit.c` gives `snes_blit` a
four-by-three picture, every pixel a different colour and none opaque, and
surfaces shaped the awkward ways real ones are: rows padded wider than the
window, a window smaller than the doubled picture, and a pitch too narrow to
hold a row. Every byte the copy must not touch is a canary. With the copy
changed to never repeat a pixel:

```
FAIL: scale 2 makes every pixel a two-by-two block, and leaves the padding alone
FAIL: scale 2 into a smaller window writes only what fits
FAIL: 2 of 5 checks on the Super Nintendo's picture at a scale
```

**Restoring the file is not enough to see it pass again.** The first rerun
still said `2 of 5`: the restore landed in the same second as the broken
build, and GNU Make 3.81 compares whole seconds, so it kept the broken binary.
Deleting `build/host/test_snesblit` and building again gave all five. A control
run by a script has to force the rebuild after restoring, or its "restored"
line reports the control twice.

**The option, in the display harness.** ROMs are named with spaces, so the
whole line after the option is the name, and the harness asks both ways that
can go wrong: `wm snes:--scale 3` must be refused by name, and
`wm snes:--scale 2 nosuch.sfc` must look for exactly `nosuch.sfc`. The
harness counts it as "2 on the Super Nintendo's --scale reaching the window
and not the ROM's name". Against the `snes.lua` from before the option
existed, with what the guest said split at its line breaks:

```
phase FAILED: `wm snes:--scale 3` was not refused by name; the program said:
wm snes:--scale 3
wm: started /bin/snes.lua as 16
snes: no /home/roms/snes/--scale 3: no such path
process 16 (snes) ended, code 0
```

**The first version of the phase typed `snes --scale 3` at the prompt**, and
the prompt printed `table: 0x00000081007890`. The core registers itself as a
global named `snes` in every Lua state, the shell's included, so at the
prompt the name is the core and `--scale 3` is a Lua comment. The phase starts
the program through the window manager, as the Deskbar and a launcher do; the
name at the prompt is a separate fault, and older than this.

**What neither can say is how a real game looks at 1024 by 960**, or what it
costs in frames. That is `make snes-check ROM=...`'s, which needs a ROM, and
the ThinkPad's.

---

## 18.35 An interrupt wait with a deadline

`irq: a wait with a deadline, and a delivery that ends one`, in the kernel
suite, and the first test of `irq_wait`'s blocking half that needs no device.

The suite claims one line and both threads wait on it. The first waits ten
ticks with nothing coming, and must come back with `SYS_NO_INTERRUPT` - not before nine
ticks, and long before a second - with the line no longer naming it, so that
a delivery afterwards is counted as pending and taken without anybody
blocking. A second thread waits two seconds, is delivered to from the suite
once it is seen blocked, and must come back with 0 long before its deadline.
Both are threads of their own, because the suite's thread must never block,
and the line is released at the end whatever happened, so a waiter still
blocked is woken with `SYS_ERR_DENIED` rather than left on a line nobody owns.

With the waiter left on the line after a deadline:

```
not ok 131 - irq: a wait with a deadline, and a delivery that ends one
...
FAIL: 1 of 155 test(s) failed:
  not ok 131 - irq: a wait with a deadline, and a delivery that ends one
```

With no deadline ever set, the first waiter never came back. The test gave
up rather than hanging the suite, which went on to finish all 155:

```
not ok 131 - irq: a wait with a deadline, and a delivery that ends one
...
FAIL: 1 of 155 test(s) failed:
  not ok 131 - irq: a wait with a deadline, and a delivery that ends one
```

The suite reports the test as one result, so both controls print the same
line; what differs is what was taken out. Each control restored `kernel/irq.c` a second and more after its build and
gave the file a fresh timestamp, because of what §18.34 found GNU Make 3.81
doing to a file restored within the same second. The suites stand at
aarch64 155/155 and x86-64 151/151.

**What this cannot show is an MSI reaching a process on x86**: nothing in
either suite raises one, and the power button is on the ARM board. That is
the xHCI driver's to show, with the controller's first interrupt.

---

## 18.36 Every syscall's arguments

`tools/test_syscall_args.lua`, in `make test` beside the LICENSE check and,
like it, a host check that needs no machine.

**The fault it is for is silent.** A wrapper names how many arguments it
loads - `sys1`, `sys2` - and the kernel's case reads `sc->arg[0]` to
`sc->arg[4]`. Both architectures copy every argument register into `sc->arg`
whether the wrapper loaded it or not (`arch/aarch64/trap.c`,
`arch/x86_64/user.S`), so a wrapper that passes too few compiles, runs, and
hands the kernel whatever that register last held. `kosmos_mem_create` did
exactly that with `SYS_MEM_CREATE`'s flags.

The check reads the highest `sc->arg[N]` in each case of `kernel/syscall.c`,
with adjacent labels that have nothing between them sharing the next one's
body, and every `sysN(SYS_...)` call in the userland sources `make test` hands
it. It fails by name for a call that passes fewer than its case reads. It also
fails a scan that found fewer than forty cases or forty calls, because an empty
scan would otherwise pass everything. Two wildcards can name one file, so a
file is read once.

On the tree before the fix:

```
FAIL: 1 of 104 checks on syscall arguments:
  user/include/kosmos.h: sys1(SYS_MEM_CREATE) passes 1, and the kernel reads 2
```

and on the fixed one:

```
PASS: 108 checks on what each syscall reads and what userland passes it (51 cases, 53 calls, none short).
```

**What it does not see**: a syscall made in assembly, which loads its
registers by hand - `user/hello-*.S`, `user/faulty-*.S` and init's
`user/init/start-*.S`, on both architectures - and a case that read its
arguments through a helper rather than `sc->arg` directly, which none does
today: `syscall_dispatch` is the only function the frame is handed to.

---

## 18.37 USB enumeration, by interrupt, checked against QEMU

`tools/run_x86.py`'s USB check, rewritten for step 2 (`usb.md` §4): 14
checks, four of them with the negative controls below.

**Compared with QEMU, not with this driver's idea of QEMU.** A second QEMU with
the same `-device` lines, `-S` so it never runs an instruction, is asked
`info usb` over its monitor, and the devices' speeds must match it. Its
"Product" turned out to be QEMU's name for the device model - "QEMU USB MSD" -
where the stick's own string descriptor says "QEMU USB HARDDRIVE", so product
strings are checked against the QEMU binary instead, which carries both. Its
first run also corrected this check's docstring, which had the keyboard as
full-speed: QEMU attaches it at 480 Mb/s.

**The first run's "no interrupt" was two faults, found by instrumenting.** A
probe in `msi_enable` printed the controller's capability list - MSI-X at 90h,
PCI Express at A0h, no MSI - and MSI-X went into `pci.c`. The check still
failed, and a probe in `apic.c` then printed each interrupt as delivered to
the driver's claim: the driver had found the No-Op's answer on the ring
before it ever waited, because QEMU completes a command inside the doorbell
write, and credited only interrupts it had waited for. It asks the line now.
Both probes were taken out.

For the first two controls the runner kept only the last thirty lines, which
cut the `FAIL: N of 14` count; what is quoted is what it printed.

With no USB 2 port reset:

```
xhci: 00:03.0 port 5, USB 2: a device whose port would not reset
...
the driver did not read two devices' descriptors and product strings
the devices' speeds are not the ones QEMU says it attached them at:
    driver: ['SuperSpeed']
    QEMU:   ['High-speed', 'SuperSpeed']
the driver's closing line is not what two controllers, a stick and a keyboard should give:
    xhci: 2 controllers (00:03.0, 00:04.0), 2 ports with something plugged in, 1 device named
```

With the input control context adding the slot and not endpoint 0, which QEMU
refuses with a TRB Error:

```
xhci: 00:03.0 port 5: no slot and address for the device
xhci: 00:04.0 port 1: no slot and address for the device
...
the devices' speeds are not the ones QEMU says it attached them at:
    driver: []
    QEMU:   ['High-speed', 'SuperSpeed']
a product string the driver read is not one the QEMU binary carries: []
the driver's closing line is not what two controllers, a stick and a keyboard should give:
    xhci: 2 controllers (00:03.0, 00:04.0), 2 ports with something plugged in, 0 devices named
```

With the interrupter never enabled:

```
FAIL: 1 of 14
  a No-Op command was not answered by interrupt on both controllers - "found by looking" means the rings work and no interrupt reached the driver:
    xhci: 00:03.0 answered a No-Op command on its event ring, found by looking: no interrupt within a second
    xhci: 00:04.0 answered a No-Op command on its event ring, found by looking: no interrupt within a second
```

With the kernel never enabling MSI-X:

```
FAIL: 1 of 14
  a No-Op command was not answered by interrupt on both controllers - "found by looking" means the rings work and no interrupt reached the driver:
    xhci: 00:03.0 runs: contexts of 32 bytes, 0 scratchpad pages, 8 slots enabled, interrupt 19
    xhci: 00:03.0 answered a No-Op command on its event ring, found by looking: no interrupt within a second
    xhci: 00:04.0 runs: contexts of 32 bytes, 0 scratchpad pages, 8 slots enabled, interrupt 16
    xhci: 00:04.0 answered a No-Op command on its event ring, found by looking: no interrupt within a second
```

While these ran, the check's boot waited for "devices named", which a run that
names one device never prints, so the controls that named fewer than two
waited out the 90-second timeout before failing. It waits for the closing
line's "plugged in, " now. That changes when a failing run returns, not what
it returns: `boot()` hands back everything printed either way.

**What none of it shows** is the ThinkPad: scratchpad pages, 64-byte contexts,
a full-speed device's packet size, a chipset's MSI that takes time to arrive.
The controller's "runs:" line is written so one photograph answers the first
two.

## 18.38 USB devices pulled out and put back

`usb_hotplug` in `tools/run_x86.py` (`usb.md` §4): 17 checks, with the two
negative controls below.

The machine of §18.37 with a QEMU monitor. Once the driver says it is
watching, `device_del` pulls the keyboard out and `device_add
usb-kbd,bus=usb0.0,port=1` puts a new one on the port it left, which the
driver calls port 5. Each round must bring exactly one unplug line naming the
keyboard, then exactly one keyboard named on the same port and nothing
unplugged. There are as many rounds as the keyboard's controller says it
enabled slots: eight.

**Two rounds were not enough, and a control is what said so.** The first
version replugged twice, believing QEMU refuses to address a port that a slot
still holds, so a driver that never disabled the slot could not name the
second keyboard. With the driver's Disable Slot taken out it passed, 18 of
18. QEMU's `hcd-xhci.c` says why: `xhci_address_slot` does refuse a port
another slot holds, but `xhci_detach_slot` forgets the port as the device
leaves. What QEMU keeps is the slot enabled - Enable Slot takes the lowest
slot that is not - so a driver that keeps its slots runs out, and with the
keyboard found at boot the check plugs in one keyboard more than there are
slots.

**Before that, the same port had to be asked for.** Without `port=`,
`device_add` takes the next free port, and a probe put the new keyboards on
ports 6 and 7, where a slot left on port 5 is in nobody's way.

**Reading the driver to write this down found a slot kept for good.** A
device that failed Address Device twice kept its second slot: the port
records no slot for a device it could not address, so its unplug disabled
nothing. Slots are given back through one function now, in all four places.
QEMU addresses every device the first time, so that path is read and not
run; the ThinkPad's ports 7 and 10 are where it runs.

Full output was kept in a log per run this time. The runner prints the serial
output in a failure as one quoted string; below it is a line to a line.

With a port's change bits never cleared, both devices were unplugged and
named again on every pass from boot, which fails §18.37's checks as well:

```
FAIL: 5 of 17
  the driver did not report exactly two devices plugged in:
  ...
  the driver did not read two devices' descriptors and product strings:
  ...
  the devices' speeds are not the ones QEMU says it attached them at:
  ...
  the stick is on the second controller, 00:04.0, and the driver named it on ['00:04.0', '00:04.0', '00:04.0', ...
  round 1 of 8: putting a keyboard back on port 5 did not name it there exactly once, with nothing unplugged:
    ...
    xhci: 00:03.0 port 5: 0627:0001, USB 2.0, class 0, "QEMU USB Keyboard"
    xhci: 00:04.0 port 1: unplugged, 46f4:0001 "QEMU USB HARDDRIVE"
    xhci: 00:04.0 port 1, USB 3: a SuperSpeed device (speed ID 4)
    xhci: 00:04.0 port 1: 46f4:0001, USB 3.0, class 0, "QEMU USB HARDDRIVE"
    xhci: 00:03.0 port 5: unplugged, 0627:0001 "QEMU USB Keyboard"
    xhci: 00:03.0 port 5, USB 2: a High-speed device (speed ID 3), after its reset
    xhci: 00:03.0 port 5: 0627:0001, USB 2.0, class 0, "QEMU USB Keyboard"
    ...
```

With an unplugged device's slot never given back:

```
FAIL: 1 of 31
  round 8 of 8: putting a keyboard back on port 5 did not name it there exactly once, with nothing unplugged:
    xhci: 00:03.0 port 5, USB 2: a High-speed device (speed ID 3), after its reset
    xhci: 00:03.0 port 5: Enable Slot gave a slot past the ones enabled
    xhci: 00:03.0 port 5: Enable Slot gave a slot past the ones enabled, again after a reset and a pause
    xhci: 00:03.0 port 5: no slot and address for the device
```

**QEMU handed out a ninth slot where the driver enabled eight** - it looks
among all 64 it has, where 5.4.7 makes slots 1 to MaxSlotsEn the active ones.
Before this revision the driver kept that slot and printed "failed: no answer
within a second" for a command that had been answered; it gives the slot back
now and says what came.

Both runs counted with the no-controller boot and §18.37's checks, in a
scratch script that runs only those; the driver as written passed all 31.

---

## 18.39 A switch with interrupts on, and a fault the runner passed

`thread: blocking switches mask, and unmask after`, in the kernel suite on both
boards, and the first word of x86's exception report.

A thread - on core 1, where there is one - turns interrupts on and blocks
three ways: `thread_block`, which the suite ends; `thread_sleep_until`, which a
tick ends; and `ipc_receive`, which the endpoint's destruction ends. After each
it checks that interrupts are on again. The suite waits to see each block
before ending it, and gives up rather than hanging if one never comes.

Then it waits for the thread to be gone - dead, and no longer named by core 1
as leaving - and fails if it is not. `sched: the policy is pluggable` runs
three tests later and swaps the scheduling policy, which is one global vtable
that every core's queues go through, and its `init` clears all of them. It
assumes nothing is scheduling anywhere else, and a thread still on its way out
of core 1 is.

What no test can do is put an interrupt inside the switch, which is a few
instructions wide and was hit in four runs of forty. So `switch_into` panics
when it is entered with interrupts on, and this test makes sure every blocking
path reaches that check on every run. `docs/smp.md` has the bug and how it was
measured.

With the lock released into the caller's state before the switch, as it was:

```
ok 13 - thread: three threads interleave

PANIC: thread: a switch with interrupts enabled

FAIL: the kernel panicked. The dump above says which instruction faulted (elr) and on what address (far).
```

on AArch64, and after `ok 10` on x86: the first kernel thread that blocks is in
`thread: block and wake`, before this test.

With the switch masked and the caller's state never given back:

```
not ok 12 - thread: blocking switches mask, and unmask after
...
not ok 54 - ipc: a caller ends a watched sleep

FAIL: the guest did not exit within 90.0s.
```

on x86 - a thread given back masked is never preempted, and the suite stalls.
AArch64 the same, at the same two tests:

```
not ok 15 - thread: blocking switches mask, and unmask after
...
not ok 57 - ipc: a caller ends a watched sleep

FAIL: the guest did not exit within 400.0s.
```

**How often, before and after.** Forty runs of 0.10.52's x86 suite, one or two
guests at a time on this Mac, took a kernel fault in four - two of those runs
failed and two passed - and a fifth failed `sched: the higher priority runs
first` with no fault, which is not shown to be the same bug. Seventy-eight
runs with the fix, two at a time, took no fault and failed nothing.

With **four** guests at once the host is busier than a gate usually is, and
three other tests showed through:

- `sched: the policy is pluggable` failed seven times in 230 runs with the
  fix - three in forty, none in ninety, four in a hundred - always with round
  robin running its threads `231`, an order its long record of failures has
  never shown. A count of switches per thread said thread 1 had been started,
  preempted before its first line, and put behind the other two. What
  preempted it was `preempt_pending`, already set on core zero before the test
  began and still set when thread 1 started - on every run, and the same on
  0.10.54 without this fix. The suite's own thread was never given a band, so
  it is idle-band: every thread it creates under the priority policy outranks
  it and flags core zero, `thread_yield` switches to that thread and leaves the
  flag, and the next interrupt preempts whoever is running. LIFO is immune,
  because a preempted thread goes back on top. It is older than this work and
  is left for work of its own. Waiting for this test's thread to be gone was
  added while it looked like the cause, and is kept because the hazard it
  closes is real, not because it fixed anything;
- the old kernel against the fixed one, thirty runs each, side by side: the
  old one took this bug's panic once, and each failed `smp: a new thread
  avoids a loaded core` once - so that one is older than this, and is a
  placement count taken at an instant;
- `thread: returning exits cleanly` failed twice without a word: once in
  sixty loaded runs, and once in this change's first `make prepush`. It counts
  live threads on every core after eight yields. This test's thread is dead
  before it returns and is not counted, and the stale flag above costs a yield
  round per spurious preemption, which is a suspicion rather than a diagnosis.
  It now says which count was wrong and whose thread moved it, and a hundred
  loaded runs since have not failed it once.

**A kernel fault on a secondary passed.** x86 reported one as `***` and
`halted.`, and `tools/run_tests.py` ends a run on `PANIC:` and nothing else. A
scratch test at the end of the table that faults on the last core, with the
old report:

```
*** page fault
...
ok 153 - scratch: a fault on another core

PASS: 153/153
```

and with the report beginning `PANIC:`, as AArch64's always has:

```
PANIC: page fault
...
FAIL: the kernel panicked. The dump above says which instruction faulted (elr) and on what address (far).
```

The scratch test is not kept: it is a fault on purpose, and nothing in the
suite can expect one on another core.

**A control that proved nothing, so the next one does not.** The x86 suite's
own object is not rebuilt when a kernel header changes - `tests.c.o` has a
dependency file that nothing includes. One diagnostic build added a field to
`struct percpu` and the next took it out, and the suite went on reading
`idle_ticks` eight bytes off: `cpu: every processor idles as a thread` failed
on every run for no reason in the kernel. Until that is fixed, touch
`tests/tests.c` after changing a kernel header.

The suites stand at aarch64 156/156 and x86-64 152/152.

## 18.40 A program reached by typing its name

`snes --scale 3` at the prompt printed `table: 0x00000081002300` and ran
nothing. Doom's, Quake's and the Super Nintendo's kits were opened by `gfx.c`
into every Lua state as globals named after their programs, and the shell
sends a word that already names something in its environment to Lua. So
`--scale 3` became a comment, and `doom /nowhere.wad` a division by a global
called `nowhere`. They are `use("/kits/snes")` and so on now (`design.md` §6).
Typed at 0.10.45, before the fix:

```
default image                              MEGA=1
doom            table: 0x000000810075d0    doom            table: 0x000000810075d0
quake           quake: this image was not  quake           table: 0x00000081007890
                built with QUAKE=1
snes --scale 3  table: 0x00000081007890    snes --scale 3  table: 0x00000081002300
```

The display harness's `programs by name` phase runs at the bare prompt, and
has three checks:

- **Nothing in `/bin` is hidden.** The shell walks `/bin` against its own
  environment and prints every program whose name is already there. It fails
  on any, not on a list of three, so the next thing to leak a global is caught
  whatever it is called. It prints its count too, and fails under 20, because
  a walk that saw nothing hides nothing.
- **Every kit the image lists is a table**: `type(sys.kit(k))` for each name
  `sys.kit_names()` gives.
- **Three lines reach their programs.** `snes --scale 3`, `doom /nowhere.wad`
  and `quake /nowhere.pak` must each print a line starting with the program's
  name, which only the program prints - and must not say "not built with"
  when the image lists that kit. Each argument is refused before a window
  opens.

**The second check is there because the first version of this phase passed a
broken fix.** Moving the three to `/kits`, one `lua_setglobal` stayed in
`snes_kosmos.c`. It popped the table into a global, so `sys.kit("snes")`
returned what lay underneath - its own argument, the string `"snes"` - and the
program, seeing a string, said:

```
snes --scale 3
snes: this image was not built with SNES=1
```

in an image built with it. That line starts `snes: `, so the phase passed, in
both images. What found it was a search of the source for `lua_setglobal`,
which `make test` now runs for that reason: it reads every build variant,
while the harness boots only the default image and cannot see a global that
exists under `MEGA=1` alone. Before the fix, the default image's
`quake /nowhere.pak` passed for exactly that reason - Quake is not compiled
in, so nothing hid it.

The typed lines are a function of their own, `reaches_program`, because the
walk fails first on this bug and would otherwise be the only half ever seen
failing. Every control, each half run alone against saved images:

```
=== the walk, before the fix, default image: must fail
FAIL: 2 of 109 program(s) in /bin cannot be run by typing their name,
because the shell's environment already holds that name: doom, snes.
=== the walk, before the fix, MEGA=1: must fail
FAIL: 3 of 109 program(s) in /bin ... doom, quake, snes.
=== the typed lines alone, before the fix, MEGA=1: must fail
FAIL: `snes --scale 3` ... table: 0x00000081002300
FAIL: `doom /nowhere.wad` ... error: stdin:1: attempt to index a nil value (global 'nowhere')
FAIL: `quake /nowhere.pak` ... error: stdin:1: attempt to index a nil value (global 'nowhere')
=== the kit check, the fix with the leftover, default and MEGA=1: must fail
FAIL: sys.kit answered with something other than a table for /kits/snes (a string).
=== the typed lines alone, the fix with the leftover, snes and doom listed: must fail
FAIL: `snes --scale 3` reached /bin/snes.lua, and it said the image was not
built with it - but this image lists /kits/snes
PASS: `doom /nowhere.wad` reached /bin/doom.lua
=== fixed, default and MEGA=1: must pass
PASS: 5 programs-by-name checks
```

**And the two source checks.** The search in `make test`, against the tree
with the leftover:

```
user/lib/snes_kosmos.c:400:    lua_setglobal(L, "snes");
FAIL (exit 1)
```

It skips comment lines, and learned it the usual way: its first run inside
`make prepush` failed on the comment in `sys_user.c` that describes it. So it
was run once more over a copy of the tree holding that comment, `lua_glue.c`
and the leftover put back, and named the leftover alone.

and `tools/luaglobals.py`, which no longer lists `doom`, `quake` and `snes`
for `/bin`: run on the three programs as they were at 0.10.45 it refuses each
for reading its global, and on the fixed ones it passes. `lua.ok` is a
prerequisite of the image, so a program that goes back to reading one fails
the build before anything boots.

---

## 18.41 Three rows about the machine that nothing had read

`neofetch` on the ThinkPad T14, photographed with 0.10.48:

```
Host      QEMU q35 x86-64
Disk      kfs, 30 of 32 MB free
Network   virtio-net at 0.0.0.0
```

The machine is not QEMU, has no virtio-net, and its disk had been made on the
host with 14,656,879 bytes of files in 32 MB. **All three reproduced in QEMU
before anything was changed**, on x86 with a disk made by the same command,
and q35's default e1000e standing in for an Ethernet controller nothing
drives:

```
Host       QEMU q35 x86-64
Disk       kfs, 30 of 32 MB free
Network    virtio-net at 0.0.0.0
```

The host's own count of that image's bitmap was 4,337 free blocks, 16.9 MB.
So the host tool was right and the machine was not.

**Each row was a label standing in for a reading.**

- *Disk.* The disk server answered `.super` with `free_blocks = blocks -
  data_at`, every block past the metadata, from the commit that made the disk
  real. The superblock has no free count and never had. `kfs.free_blocks`
  counts the bitmap, only for blocks the disk has, and `tools/kfs.lua df`
  prints the same count on the host. `tools/test_kfs.lua` also had a check
  comparing `sb.free` before and after a rename: kfs has no such field, so
  it compared nil with nil and could not fail.
- *Host.* `b.platform` was the Makefile's `PLATFORM`, compiled into every PC
  build. The PC board now reads SMBIOS's System Information, through the EFI
  System Table a Multiboot 2 loader passes on, or in the BIOS area. It copies
  it during `hal_early_init`, because `mmu_init` stops mapping firmware
  memory, and hands it to userland in `sysinfo` (`hal_machine_ident`).
- *Network.* The stack answers `net_info` with or without a card, its address
  is four zero bytes until init configures one, and `neofetch` called any
  four-byte answer virtio-net. Cards are named from the bus now
  (`/lib/hardware.lua`), driven or not, and the address is shown only when
  the stack says it has a card.

**What checks it**, each watched fail:

```
tools/test_kfs.lua              47 checks, 8 of them on the free count
  free_blocks = blocks - data_at             FAIL: 6 of 47
  every clear bit, past the end of the disk  FAIL: 1 of 47
tools/run_interchange.py        the machine's df against kfs.lua df
  .super answering blocks - data_at again    FAIL: ... 7918 blocks free of
                                             8192 against 7910 blocks free
                                             of 8192
tools/test_smbiosdecode.c       20 checks on the decoder
  checksums not checked                      FAIL: 2 of 20
  the walk allowed 512 bytes past the table  FAIL: 1 of 20
  control characters kept                    FAIL: 1 of 20
tools/run_x86.py, identity      5 checks, two boots
  Host from b.platform, no 3.0 entry point   3 fail: Host 'PC x86-64', and
                                             "not named; no SMBIOS entry
                                             point in the BIOS area"
  the old Network row                        1 fails: 'virtio-net at 0.0.0.0'
tools/run_uefi.py               the name, through the EFI System Table
  ConfigurationTable read at offset 104      FAIL: 1 of 15 ... "not named; no
                                             SMBIOS in the EFI system table
                                             or the BIOS area"
tools/run_network.py            a driven card still reads virtio-net at
                                10.0.2.15, on both boards
  the device-tree bus shape ignored          FAIL: 'a card the bus did not
                                             list at 10.0.2.15'
```

**Why QEMU can stand in for the ThinkPad here.** `-smbios type=1` writes any
strings into System Information, so the second identity boot names itself
`LENOVO 20W000T9US ThinkPad T14 Gen 2i`. And `smbios-entry-point-type=64`
gives it the 3.0 entry point a 2021 firmware is likely to use. QEMU's default
is the 2.1 one, so without that option half of `smbios_entry` would never run
under emulation. With it, SeaBIOS offers *only* the 3.0 anchor, which is why
the control that disabled it named nothing at all.

**What is not covered**, said rather than left to be found:

- The 2.1 entry point reached through the EFI table (OVMF's default) was
  seen once by hand, as `SMBIOS 2.8 in the EFI system table`. No harness
  boots it: `run_uefi.py` asks for 3.0 because that is the ThinkPad's likely
  path.
- A table above 4 GB is refused rather than read, and nothing here produces
  one.
- **`run_network.py`'s `neofetch` check has a boot to itself**, and that is a
  finding rather than a style. Typed into the same boot as `netframe`,
  whichever went second never arrived - no echo, a bare prompt - and the
  first `make test` of this change failed on it. The committed 0.10.45,
  built from an archive and probed with its own harness, lost `host` after
  `host 10.0.2.2` the same way, in both boots tried. Yet the same file's
  resolver phase, four commands in one boot, passed in the next full run. So
  it is intermittent or conditional, and not understood; `state.md` has what
  is known.
- The ThinkPad itself. The Lenovo strings in the tests are the shape Lenovo's
  firmware uses, not a copy of this machine's table.

## 18.42 Kosmos's own UEFI loader, and the kernel at 16 MB

Two checks, both in `make test` (`boot.md` has the loader and why it exists):

| check | what it establishes |
| ----- | ------------------- |
| `tools/test_efiboot.c`, 40 checks, and 48 given `build/x86_64/kosmos.bin` | the header parser on synthetic images - a bad checksum, a MIPS header, an entry outside what is loaded, a `bss_end` below `load_end`, no entry tag, an unknown required tag, a header past 32 KB or past what was read - and on the build's own kernel, which must ask for 16 MB from offset 0 and be loaded whole; the firmware's map, unsorted and fragmented with 48-byte descriptors, merged into eight ranges and typed as GRUB typed it; and an information structure built by `mbi.c` read back with the kernel's own `mb2_find` and `mb2_framebuffer_from`: the command line, the disk, three map entries, the framebuffer, the EFI system table and the RSDP |
| `tools/run_uefi.py`, 22 checks, and 24 given a refusal stick | the stick `mkusb_image.py` makes, with a 4 MB disk `kfs.lua` made, booted under OVMF as a USB drive on an xHCI controller: the loader's own lines - the kernel's place at 16 MB, both copies matching the file, handing over - the kernel's line saying nothing was repaired and nothing lost and `the disk: same` (`none` on a stick without one), `this kernel is 0x01000000..`, and nothing of the firmware's under it; plus the fifteen it had, the screen, ACPI, SMBIOS through the EFI system table and the other processors among them; and a second stick, whose kernel is 64 KB of zeros, refused on the serial line and drawn on by the loader in the lower half of the screen, its ground and its ink |

`make test`'s stick carries a 4 MB disk, so the module tag, the firmware's
placement of the disk and the loader's second look at it are checked on every
run, and the ThinkPad's stick with the 32 MB disk passes the same 22 by hand.
The disk check and the refusal came in 0.10.60, and the controls below ran
before both, which is why their boots count 21.

**Found on the way, each before it could matter:**

- **The loader's first run was refused**, and that was the finding. OVMF
  would not give the kernel 0x00100000..0x009c0000, and the loader printed
  why: ACPI NVS at 0x800000..0x900000 and boot services data from 0x900000 -
  memory GRUB had been loading the kernel over. The kernel moved to 16 MB.
- **`kernel/main.c` said 1 MB whatever the link address said**: `0x100000UL`
  in four places, the kernel's range line among them. The first boot at 16 MB
  printed `this kernel is 0x00100000..0x018c0000` and three `UNDER THIS
  KERNEL` lines that were no longer true. `__image_start` now, the name the
  ARM linker script already had.
- **The `before=` count was written fourteen characters late**, through an
  offset worked out backwards from the next field. Read before the loader
  first ran; a pointer is recorded where the word is appended now.
- **Two references to the trampoline's bytes went through the GOT**, the only
  relocations in the loader that were not PC-relative. Declared hidden, and
  the build's own check says there are none.

**The controls**, each an edit to one file, a build, a boot and a restore, with
the full output kept per run. The two that spoil the kernel are the reason the
loader exists, so each was booted a second time to read what the loader and
the kernel said.

With one byte of a page of one in-memory copy flipped before the first check:

```
kosmos-boot: 1 pages of the kernel changed in memory and were repaired, 0 could not be
kosmos-boot: handing over: entry 0x01001000, information at 0x7c5cf000, trampoline at 0x7e3cd000
       -> this kernel is 0x01000000..0x018c0000
       -> the loader: kosmos-boot, 1 pages repaired before the firmware let go, 0 after, 0 lost; the disk: none
init: process 11 exited cleanly

FAIL: 2 of 21 checks booting through Kosmos's loader under UEFI:
  the loader found the kernel's copies changed, or never checked: [...]
  the kernel does not report a clean hand-over from its loader: -> the loader: kosmos-boot, 1 pages repaired before the firmware let go, 0 after, 0 lost; the disk: none
```

With one byte of the kernel flipped in its place, after the copy and after
ExitBootServices:

```
kosmos-boot: both copies of the kernel are the file, page for page
kosmos-boot: handing over: entry 0x01001000, information at 0x7c5cf000, trampoline at 0x7e3cd000
       -> this kernel is 0x01000000..0x018c0000
       -> the loader: kosmos-boot, 0 pages repaired before the firmware let go, 1 after, 0 lost; the disk: none
init: process 11 exited cleanly

FAIL: 1 of 21 checks booting through Kosmos's loader under UEFI:
  the kernel does not report a clean hand-over from its loader: -> the loader: kosmos-boot, 0 pages repaired before the firmware let go, 1 after, 0 lost; the disk: none
```

**Both boots reached userland with the damage repaired and said.** That is the
ThinkPad's fault from 11 September - bytes changed where nothing in Kosmos
wrote them - happening on purpose, once on each side of the firmware letting
go, and survived instead of silent.

With the kernel linked at 1 MB again, the loader refuses, and the screen
says why before it waits for a key:

```
kosmos-boot: the kernel must be at 0x00100000..0x009c0000, and the firmware keeps part of it:
kosmos-boot:   0x00800000..0x00808000  ACPI NVS
kosmos-boot:   0x0080b000..0x0080c000  ACPI NVS
kosmos-boot:   0x00811000..0x00900000  ACPI NVS
kosmos-boot: the kernel's memory is not the loader's to give
kosmos-boot: Kosmos cannot start from this stick. Press a key to return to the firmware.

FAIL: 15 of 21 checks booting through Kosmos's loader under UEFI:
  the machine found no processor line; ACPI is not reaching the kernel through the loader
  ...
  the loader did not place the kernel at 16 MB: [...]
  the loader never handed over: [...]
```

Boot services data at 0x900000 is not in that list, because the loader would
have borrowed it; only what the firmware keeps is named.

With the memory map left unmerged:

```
FAIL: the map is not merged into eight ranges
FAIL: boot services data and free memory below 640 KB are not one range
...
FAIL: 9 of 48 checks on the UEFI loader's decisions
```

With information tags not padded to eight bytes, the kernel's own parser
loses every tag after the first:

```
FAIL: the kernel does not find the disk where it was put
FAIL: the kernel does not read three map entries of 24 bytes
FAIL: the kernel refuses the framebuffer, or reads it wrong
FAIL: the kernel does not find the EFI system table, and SMBIOS with it
FAIL: the kernel does not get the RSDP, and ACPI with it
FAIL: 5 of 47 checks on the UEFI loader's decisions
```

And the loader as written, after every restore: `run_uefi.py` 21 of 21, and
the host test 48 of 48.

**And the disk, since 0.10.60.** The loader fingerprints the disk when it
reads it and again once the firmware has let go, and leaves `same`, `diff` or
`none` for the kernel's line to print - and this check read only the part of
that line about the kernel's pages. A loader built outside the tree that
changes the last byte of the disk after fingerprinting it, which is free
space on a fresh `kfs` disk so nothing else notices, shows the hole. Its
stick, with a 4 MB disk, under the check as it was:

```
PASS: 21 checks booting through Kosmos's loader under UEFI (the firmware's memory claimed and checked, the kernel at 16 MB, and Kosmos drawing its own 1280x800 screen).
```

and under the check as it is:

```
FAIL: 1 of 22 checks booting through Kosmos's loader under UEFI:
  the kernel does not say `the disk: same` after the loader read a disk: -> the loader: kosmos-boot, 0 pages repaired before the firmware let go, 0 after, 0 lost; the disk: diff
```

With the loader as written, a stick with the 4 MB disk and one without it
both pass 22 of 22, the first held to `same` and the second to `none` by what
the loader's own line said it carried.

**And a refusal, on the screen, since 0.10.60.** The ThinkPad's first stick
through this loader was a black panel that went back to the Boot Menu at a key
press: a refusal, said through a firmware console that machine does not show,
while every check here read the serial line and passed. So `make test` boots a
second stick, whose kernel is 64 KB of zeros, and screendumps it while the
loader waits for a key. With the loader as written:

```
PASS: 24 checks booting through Kosmos's loader under UEFI (the firmware's memory claimed and checked, the kernel at 16 MB, and Kosmos drawing its own 1280x800 screen).
```

With a copy of it built outside the tree whose `draw_line` returns at once -
OVMF's own console still shows the refusal in the upper half, and the check
does not count it:

```
FAIL: 1 of 24 checks booting through Kosmos's loader under UEFI:
  the loader's refusal is not drawn in the lower half of the screen: 0.0% ground, 0.00% ink
```

And with a stick that boots given where the refusal belongs, both checks fail,
the second on ink rather than ground - the desktop's ground fills the lower
half, and none of it is the loader's lines (`[2J[01;01H` is the console being
cleared, as it reaches the serial line):

```
FAIL: 2 of 24 checks booting through Kosmos's loader under UEFI:
  a stick whose kernel is zeros was not refused on the serial line: [2J[01;01Hkosmos-boot: Kosmos's own loader, on EDK II firmware revision 0x00010000
  the loader's refusal is not drawn in the lower half of the screen: 93.7% ground, 0.00% ink
```

What this cannot show is the ThinkPad's console, which is the reason the loader
draws at all: under OVMF the firmware's text console shows either way.

**What none of it shows** is the ThinkPad's own map at the moment the loader
runs, and whether its firmware writes into memory it has handed out. The
loader's lines on that machine are that measurement.

## 18.43 A USB mouse, and the pointer it shares with the TrackPoint

Four checks, all in `make test` (`usb.md` §5 has the mouse, and `CLAUDE.md`
and `hal/pc/pointer.c` the pointer):

| check | what it establishes |
| ----- | ------------------- |
| `tools/test_usbdecode.c`, 32 checks | a configuration descriptor walked on the host: QEMU's mouse, laid out from `dev-hid.c`'s declarations - configuration 1, interface 0, endpoint 1, 4 bytes, interval 7; HID 1.11 Appendix E's keyboard and mouse, where the mouse is behind the keyboard and must not take the keyboard's endpoint; a keyboard alone and a stick, neither of them a mouse; every length a device can get wrong - 0, 1, past the end, a total longer than what arrived, a total that cuts a descriptor or the configuration itself - each of which must end the walk; an OUT, bulk or zero endpoint, alternate setting 1 and a HID interface with no protocol, none of them taken; and high speed's extra transactions kept out of the packet size |
| `irq: a wait on two lines takes whichever has one`, on both boards | `irq_wait_any` answers with the line that had an interrupt and takes that line's count, the lowest when both have one; blocks on both and is woken by a delivery on the second, long before its deadline; comes back at a deadline with nothing on either; leaves neither line naming it afterwards, so the next delivery is counted rather than waking a thread that has gone; and refuses a line released underneath it |
| `input: a driver's movement adds to the pointer`, on both boards | on the PC, `hal_pointer_move` moves the position the TrackPoint moves, at its speed, from the corner: right and down positive, a button held and let go, each marked as moved once and cleared by the look, and a middle button that is not the pointer's. On the ARM board, whose pointer is a tablet or nothing, the call is refused and the speed is zero |
| `tools/run_x86.py`'s `usb_mouse`, 16 checks | q35 on its I/O APIC with two xHCI controllers and QEMU's mouse on the second: the driver reads it there, the two controllers on interrupts of their own; 640 movements through `mouse_set`, no more than 30 ms apart - 11.5 on this Mac - answered by at least four reports in five and more than two rounds of a ring, 665 in all; a click on the Deskbar's button that opens its menu; a button pressed through USB, and let go by the driver when the mouse is pulled out holding it; and a full-speed mouse plugged back in, read every 8 ms for the 10 its descriptor asks, from its first report |

`tools/test_syscall_args.lua` reads the two new calls as well: 53 cases, 55
calls, none short.

**Found on the way, each before it could matter:**

- **The first run of `usb_mouse` was on the 8259s**, copied from `pointer`,
  and it passed with the control that makes the driver wait on one
  controller. Under `opt/kosmos/irq=pic` QEMU's two controllers share line
  11: the second's claim is refused, it says "not claimed, so polled", and
  the shared line woke the driver for both. The check boots on the I/O APIC
  now, as the ThinkPad does, and asserts the two interrupts.
- **And on the I/O APIC that control still passed**, because the movements
  were not 20 ms apart. `Monitor.ask` waits for as much quiet as it is asked
  for, and reads its socket with a 50 ms timeout, so it cannot see a shorter
  quiet: every movement came 50 ms after the last, and a driver waiting 50 ms
  on the other controller kept up. The movements go out on a 2 ms timeout
  now, and the check measures their spacing and fails one over 30 ms before
  it believes a count.
- **No ring had gone round before.** A ring is 255 requests and a Link back
  to its start, and nothing had sent one ring more than a few dozen: the
  Link and the cycle bit it turns over had never run. A mouse reaches them
  in two seconds of movement.
- **The keyboard in `usb` and `usb_hotplug` is a HID device too**, and now
  says it is not a boot mouse. Neither check's patterns match that line,
  which running both confirmed.

**The controls**, each an edit to one file, a build, the check that should
catch it and a restore, by a script that compared every file with its
original afterwards.

The descriptor walk, as copies compiled beside the host test:

| broken | what failed |
| ------ | ----------- |
| a descriptor's length not held to what is left of the total | 2 of 32: "a descriptor running past the end was walked", "a total that cuts the endpoint in half was walked" |
| alternate settings not looked at | 1 of 32: "a mouse at alternate setting 1 was taken" |
| a total longer than what arrived believed | 1 of 32: "a total longer than what arrived was believed" |
| the endpoint's direction not looked at | 1 of 32: "an OUT endpoint was taken for the mouse's reports" |
| a length of 0 accepted | no answer: the walk stops moving, and the run was killed after five seconds (exit 142) |

The kernel, each against a whole suite:

```
irq_wait_any leaving its waiter on the other lines (AArch64):
  not ok 133 - irq: a wait on two lines takes whichever has one
  FAIL: 1 of 158 test(s) failed

irq_wait_any recording itself as the first line's waiter only (AArch64):
  not ok 133 - irq: a wait on two lines takes whichever has one
  FAIL: 1 of 158 test(s) failed

the board adding a report's Y upwards (x86-64):
  not ok 133 - input: a driver's movement adds to the pointer
  FAIL: 1 of 154 test(s) failed

the board holding every bit of a source's buttons (x86-64):
  not ok 133 - input: a driver's movement adds to the pointer
  FAIL: 1 of 154 test(s) failed
```

The driver, each against `usb_mouse` as it stands - on the I/O APIC, with
the movements' spacing measured:

| broken | what failed |
| ------ | ----------- |
| `detach` not letting go of the mouse's buttons | 1 of 16: "a button held on the USB mouse as it was pulled out never came up" |
| the watch waiting on the first controller's line only | 2 of 16: "the driver read 161 reports for 640 movements 11.6 ms apart - fewer than four in five", and fewer than two rounds of a ring |
| a full-speed bInterval taken for a power of two | 1 of 16: "a full-speed mouse asking for a report every 10 ms was not read every 8 ms" |
| `ring_push` not turning the cycle bit over at the Link | 5 of 16: the driver read exactly 255 reports - one ring - and then nothing: no menu, no button, no release |

The one that waits on one line passed twice before it failed here - 13 of 13
on the 8259s, and 14 of 14 on the I/O APIC with the movements 50 ms apart -
which is where the first two findings above come from.

And as written, after every restore: the host test 32 of 32, the suites 158
of 158 and 154 of 154, and `usb_mouse` 16 of 16, the driver reading 665
reports for 640 movements 11.5 ms apart.

## 18.44 The ThinkPad's screen, above 4 GB

**The early screen passed its check under OVMF for months while on the
ThinkPad it had never once worked.** That machine's firmware puts its
framebuffer at `0x4000000000`; `hal_fb_early` refused anything past the four
gigabytes `start.S` maps; so every boot of it was dark from the loader's last
line to stage six, and two sticks on 13 September stopped with nothing on the
panel but the loader's lines. OVMF's screen is at `0x80000000`, which is why
no check here could see it (`boot.md` §3 and §5).

`mmu_boot_map_high` adds a screen above 4 GB to the boot page tables - 2 MB
entries, uncached, from three pages kept in `.bss` - and `hal_fb_early` takes
it instead of refusing.

Five checks, in `tools/run_uefi.py`, which `make test` runs: the stick it
already boots, booted once more with its screen where the ThinkPad's is.
**Nothing in the loader or the kernel changes for the test.** QEMU starts
paused with its gdbstub; a hardware breakpoint at `_start` stops it at the
kernel's first instruction with the loader's structure in `ebx`; the
framebuffer tag in that structure is rewritten to `0x4000000000` at 1920x1080
with 7680 bytes a row; and the memory there is a `pc-dimm` that no map the
firmware hands over contains, which is what a graphics aperture is. The
monitor's `pmemsave` reads the pixels back.

| check | what it establishes |
| ----- | ------------------- |
| the tag moved | the harness stopped at the entry, found a Multiboot 2 structure in `ebx`, and the kernel is told 0x4000000000 at 1920x1080 |
| drawn by stage four | at a second breakpoint, `pmm_init`, the pixels at 0x4000000000 are more than half the kernel's ground and carry the log's green: the screen was used before anything could have mapped it the late way |
| since stage two | the kernel's own display fact is `the panel has had this log since stage two` |
| 1920x1080 | the display stage took the screen it was handed |
| at the prompt | the log's green and the wordmark's red are in those pixels once `kosmos>` is printed |

**The control**, with `hal_fb_early` refusing a screen above 4 GB again, as
before 0.10.62:

```
FAIL: 2 of 29 checks booting through Kosmos's loader under UEFI:
  with the screen at 0x4000000000 nothing was drawn by the start of stage four (0.0% ground, 0.00% green): the kernel is dark there until stage six, as it was on the ThinkPad
  with the screen at 0x4000000000 the kernel says: -> attached here; everything above it was replayed
```

Only those two: the late mapping at stage six still takes the screen, which
is exactly why that machine reached its desktop and nobody knew. With
`hal/pc/fb.c` restored byte for byte and rebuilt, 29 of 29.

**Found before it was a check**: the same rig, as a scratch script, booted the
exact 0.10.61 image that stopped on the ThinkPad with the screen moved there,
and it reached the prompt - as it did with that machine's memory shape (16 GB,
usable memory ending near 2.2 GB, eight processors), with QEMU's fullest
processor, and with an Intel client model with SMEP on. Whatever stops that
machine, it is in nothing QEMU models of it.

## 18.45 A stick read back after it is written

**Nothing between the build and the kernel ever asked whether the machine is
handed the bytes the build wrote.** `mkusb.sh` wrote with `dd` and ejected;
the loader reads the kernel off the stick once, and both of its copies and
every fingerprint come from that read; the kernel's canary knows the build's
sums for the userland image only. A stick that gives back bytes nobody wrote
fits every ThinkPad boot since 11 September, and QEMU could never show it,
because it reads the image file (`boot.md` §3).

So `mkusb.sh` now reads every sector back before it ejects, straight into
`tools/stickcheck.py` rather than into a file on this Mac's nearly full disk.
The checker reads the image's GPT and FAT32 and names each difference: the
MBR, the GPT, the FAT's own sectors, or a file and the offset in it - and in
the kernel, the page, the address and the ELF section. **It tells two kinds
apart**, because macOS may mount the stick as soon as `dd` lets go and a
mount writes: a free cluster taken, a directory entry in a slot the image
left empty, FSInfo's count, the dirty bits in the second FAT entry, a
last-access date. That is *bookkeeping*, exit 3, and `mkusb.sh` accepts it;
anything else is *damage* - a byte of a file, a FAT entry of a cluster a file
uses, a directory entry the image wrote, the boot sector, the GPT - exit 1,
and `mkusb.sh` says not to boot the stick.

`tools/test_stickcheck.py`, in `make test`, 9 checks. It streams the stick
`make test` has just built into the checker with faults applied on the way,
copying nothing, and **places each fault from mtools' reading of the
filesystem - `minfo`, `mshowfat` - not the checker's**, so a wrong offset in
the checker cannot put the fault in the wrong place and then name that place
correctly:

| check | what it establishes |
| ----- | ------------------- |
| the image itself | exit 0, every sector |
| a byte of the kernel's `.text` | damage, `kernel page 2 at 0x01002000 in .text` |
| a byte of the userland image | damage, `kernel page 512 at 0x01200000 in .rodata (the userland image)` |
| a byte of the disk | damage, `/boot/disk.img, byte 0x10000` |
| the GPT header's checksum | damage |
| half a stick | exit 1, `gave back` - a short read is never a pass, even where the image's tail is zeros |
| the kernel's directory entry | damage, `the directory /boot` |
| a link in the kernel's FAT chain | damage |
| what a mount writes, made by hand | exit 3: a free cluster taken in both FATs, an entry in an empty root slot pointing at it, data in it, FSInfo's count one lower |

**Controls**, each a copy of the checker run by a copy of the test:

| broken | what failed |
| ------ | ----------- |
| every difference called bookkeeping | 6 of 9: every kind of damage came back exit 3 |
| the FAT's data region read one cluster late | 3 of 9: the kernel's `.text`, the userland image and the disk, each named at the wrong place |

Before the test existed, the same kinds of fault were made by hand on a clone
of the 0.10.61 stick image - five spoiled bytes, a short read, a
`.fseventsd` written with `mmd` and `mcopy` as a mount would, a file renamed
with `mren`, a FAT link broken - and each got the verdict above.

**Found with it, the first time it was used**: the read-back of the stick
that had stopped the ThinkPad was not Kosmos at all. The checker found a
hybrid ISO where the GPT should be, and the volume descriptor said `Pop_OS
24.04 amd64` - Diego had written a Linux distribution over the stick to try
it on the ThinkPad in the meantime. So that stick's bytes are gone, and the
first real read-back of a Kosmos stick is the next one `mkusb.sh` writes: the
read-back itself needs a stick and `sudo`, which no test here has.
