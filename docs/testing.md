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
when nothing is refused and the Terminal's grid **reaches the screen's
edges**. **It checks the property rather than a number**, so it does not care
how many pages that took.

**It asked for 800 pixels of growth until 16 September, and that was a number
pretending to be a property.** The grid is measured from the window's own
corner, and the window is placed by `wm.lua`'s search - free quarters first,
the cascade when they are gone - so how much room it has to grow into is
whatever that search left it. On a run that failed, the Terminal began at
x=969 of 1920: 951 pixels existed to its right and the check wanted 1421. The
drag worked perfectly, the grid went from 621 wide to 936 - every pixel
available - and the phase failed on arithmetic that could not have succeeded
from that position. Filling the screen is the property; growth by a constant
was a proxy that held only while the cascade happened to place the Terminal
on the left.

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

So `mkusb.sh` now reads every sector back before it ejects:
`tools/stickcheck.py` reads the stick's raw device itself, as root, rather
than a copy on this Mac's nearly full disk, and its exit code is the verdict.
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

`tools/test_stickcheck.py`, in `make test`, 10 checks. It streams the stick
`make test` has just built into the checker with faults applied on the way,
copying nothing, and **places each fault from mtools' reading of the
filesystem - `minfo`, `mshowfat` - not the checker's**, so a wrong offset in
the checker cannot put the fault in the wrong place and then name that place
correctly:

| check | what it establishes |
| ----- | ------------------- |
| the image itself | exit 0, every sector |
| the image read through a path | exit 0, the checker opening a file itself and reading it unbuffered, as `mkusb.sh` has it read a stick's raw device |
| a byte of the kernel's `.text` | damage, `kernel page 2 at 0x01002000 in .text` |
| a byte of the userland image | damage, `kernel page 512 at 0x01200000 in .rodata (the userland image)` |
| a byte of the disk | damage, `/boot/disk.img, byte 0x10000` |
| the GPT header's checksum | damage |
| half a stick | exit 1, `gave back` - a short read is never a pass, even where the image's tail is zeros |
| the kernel's directory entry | damage, `the directory /boot` |
| a link in the kernel's FAT chain | damage |
| what a mount writes, made by hand | exit 3: a cluster the image's FAT leaves free taken in both FATs, an entry in an empty root slot pointing at it, data in it, FSInfo's count one lower |

**Controls**, each a copy of the checker run by a copy of the test:

| broken | what failed |
| ------ | ----------- |
| every difference called bookkeeping | 6 of 10: every kind of damage came back exit 3 |
| the FAT's data region read one cluster late | 3 of 10: the kernel's `.text`, the userland image and the disk, each named at the wrong place |

Before the test existed, the same kinds of fault were made by hand on a clone
of the 0.10.61 stick image - five spoiled bytes, a short read, a
`.fseventsd` written with `mmd` and `mcopy` as a mount would, a file renamed
with `mren`, a FAT link broken - and each got the verdict above.

**Found with it, the first time it was used**: the read-back of the stick
that had stopped the ThinkPad was not Kosmos at all. The checker found a
hybrid ISO where the GPT should be, and the volume descriptor said `Pop_OS
24.04 amd64` - Diego had written a Linux distribution over the stick to try
it on the ThinkPad in the meantime. So that stick's bytes are gone.

**And found in `mkusb.sh`, the first time it read back a real stick.** 0.10.60
was written, the checker said `the stick holds the image, every one of its
475203 sectors as written` - and the script then printed `THIS STICK DOES NOT
HOLD THE IMAGE`. It piped `sudo dd` into the checker: `dd` reads in 4 MB
blocks and so read past the image, the checker exited once it had compared
the image, `dd` died of SIGPIPE, and under `pipefail` its 141 was the verdict.
Every check here had streamed an image *file*, where `dd` stops at the file's
end. Reproduced on the Mac before it was fixed, with the script's own lines
and a file shaped like a stick - the image followed by 4 MB more:

| the lines | on the stick-shaped file | the same with one kernel byte spoiled |
| --- | --- | --- |
| as first written, `dd` piped in | verdict 141, refused | - |
| as fixed, the checker reading the device | verdict 0, accepted | verdict 1, refused, `kernel page 2 at 0x01002000` |

The half that could not run here - the script's use of the checker - was the
half that was wrong. `the image read through a path` is the checker's side of
the path the script now takes; `sudo` and a real stick stay the other side.

**And one of this test's own checks was wrong for a real stick.** The mount
made by hand took a cluster a fixed 8 MB into the disk: free on `make test`'s
4 MB disk, inside the 32 MB disk a ThinkPad stick carries. Run against that
stick's image it failed 1 of 10, the checker rightly calling a write into the
disk damage. The cluster comes from the image's FAT now, past the last
cluster mtools lists, and the test passes against `make test`'s stick, the
0.10.60 stick and the 0.10.62 stick alike.

## 18.46 A mouse read by its Report descriptor

**The ThinkPad's USB mouse took SET_PROTOCOL(boot) and went on sending its
own reports.** On 0.10.62: moved right, the arrow went down; left, up; up and
down, nothing; and it never left the middle of the screen sideways. The
driver read bytes 0, 1 and 2 as a boot report (HID 1.11 B.2), and its first
report was `buttons 0, moved 0,-1` - a zero byte 0, which is no Report ID -
so the likeliest layout is sixteen buttons in two bytes before X and Y.
QEMU's mouse honours the boot protocol, so nothing here could have seen it
(`usb.md` §5).

So the driver asks the interface for its Report descriptor (HID 1.11 7.1.1),
prints its bytes, and `usb_decode_mouse_report` lays out the first report
with relative X and Y - the buttons, bit offsets and sizes, signedness from
the Logical Minimum (5.8), and the Report ID - and the mouse is put in the
report protocol and read by that. A descriptor that lays out no such report,
or one longer than a packet, leaves it read as a boot mouse, and the line
says why.

`tools/test_usbdecode.c`, 56 checks, 24 of them new:

| check | what it establishes |
| ----- | ------------------- |
| the Report descriptor's length | QEMU's 52 bytes, and in HID 1.11's keyboard and mouse the mouse's 0x32 rather than the keyboard's 0x3f in front of it |
| HID 1.11 E.10, byte for byte | three buttons from bit 0, X and Y a byte each from bits 8 and 16, signed from a Logical Minimum of -127 |
| QEMU's mouse, from `hw/usb/dev-hid.c` | five buttons, padding, X, Y and a wheel: 32 bits |
| sixteen buttons, a layout made here | X and Y sixteen bits each after two bytes of buttons; a report read by it is the left button, 3 right and 2 up - where read as a boot report the same bytes are no sideways movement and 3 down, which is the ThinkPad's symptom |
| a mouse with Report ID 1 beside a consumer control | ID 1, and the consumer's array stepped over |
| a keyboard as Report ID 1 and the mouse as ID 2 | ID 2's fields alone; four-byte usages with the Button page still in force; a Push and a Pop putting the report size back |
| a Usage Page given after X and Y's usages | theirs, because the page in force at the Main item is (6.2.2.8) |
| a Logical Minimum of 0 | X and Y unsigned (5.8) |
| refused | absolute X and Y, which is a tablet; X and Y 33 bits wide; an item whose data runs past the end; Report ID 0; a Pop with nothing pushed; a Push five deep; no descriptor at all |
| a long item in front | stepped over |
| fields | twelve bits across two bytes, signed and not; 32 bits, the least significant byte first; single bits; a field past the end, or of 0 or 33 bits, read as 0 |

**Controls**, each a copy of `usb_decode.c` built with the test:

| broken | what failed |
| ------ | ----------- |
| a usage's page taken when its Usage item is met, not at the Main item | 1 of 56: the Usage Page given after X and Y's usages |
| Report IDs neither kept nor counted | 2 of 56: the mouse with Report ID 1, and the mouse behind the keyboard |
| no check that an item's data is there | 1 of 56: the item cut at the end, whose missing byte is still in the array |
| X and Y always signed | 1 of 56: the Logical Minimum of 0 |

`tools/run_x86.py`'s `usb_mouse`, 17 checks, one of them new: QEMU's mouse
read by its Report descriptor - `5 buttons from bit 0, X from bit 8 in 8, Y
from bit 16 in 8, no report ID`. QEMU's report is its boot report, so the
clicks and movements pass whichever way it is read, and that line is what
says the descriptor was. **The control**, with the driver made to read every
mouse as a boot mouse once it has laid out the descriptor:

```
FAIL: 1 of 115 checks on x86-64:
  the driver did not read QEMU's mouse by its Report descriptor - five buttons, then X and Y a byte each:
    xhci: 00:03.0 port 5: read as a boot mouse, because its Report descriptor lays out no relative X and Y
```

And as written, with `xhci.c` restored byte for byte: `make test` whole - the
host test 56 of 56, x86-64 115, the UEFI boots 29, the stick check 10, and the
suites 158 of 158 and 154 of 154. **The ThinkPad has not run it yet**; on its
first boot the photograph is `log xhci`, which now carries the mouse's
descriptor bytes and the layout read from them.

## 18.47 The 64-bit registers, low half first, and reports by interrupt

**The ThinkPad's USB mouse moved in jumps** - Diego: "the mouse feels like the
kernel is reading the mouse coordinates in intervals of 20ms and the trackpad
feels 100% realtime and smooth". Its log said the driver had read 882 reports
in 25 minutes and taken a second or two for each step of naming it: answers on
the event ring, and their interrupts not coming (`usb.md` §5). The driver
wrote ERDP high half first, where xHCI 1.2 5.1 says "low Dword-first,
high-Dword second" - and QEMU clears Event Handler Busy on the low half's
write, so nothing QEMU did could show it.

**Ruled out before that**, with scratch copies of `usb_mouse`: plain MSI, as
the ThinkPad's controllers use, instead of QEMU's MSI-X - 17 of 17; and one,
four and eight processors on MSI-X and on plain MSI, 658 to 668 reports for
640 movements in every run that printed them.

**What QEMU can show is the order itself.** It traces every write to a
controller's operational and runtime registers, and `run_x86.py`'s `usb` now
boots with `-trace usb_xhci_oper_write -trace usb_xhci_runtime_write` into a
file of their own (`-D`) - on the serial line the trace's lines would land
inside the driver's. The check reads CRCR and DCBAAP, ERSTBA and ERDP, and
wants each low half followed at once by its high half, and no high half
alone. SeaBIOS writes the same registers before Kosmos runs, so the writes
read start at the driver's CONFIG, which carries the slots its line says it
enabled - SeaBIOS writes all 64 there. On the build before the fix the trace
has the firmware's writes, low half first, and then the driver's:

```
usb_xhci_oper_write off 0x0038, val 0x00000008
usb_xhci_oper_write off 0x0030, val 0x08380000
usb_xhci_oper_write off 0x0034, val 0x00000000
usb_xhci_oper_write off 0x0018, val 0x08381001
usb_xhci_oper_write off 0x001c, val 0x00000000
usb_xhci_runtime_write off 0x0028, val 0x00000001
usb_xhci_runtime_write off 0x003c, val 0x00000000
usb_xhci_runtime_write off 0x0038, val 0x08382000
```

**The count of places it breaks is not the count of wrong writes.** A run of
ERDP updates written high then low reads, by offset alone, as low-high pairs
in its middle, so each run breaks in two places however long it is. It cannot
pass: every run begins with a high half after another register.

**And a count that says whether reports come by interrupt**, because the
ThinkPad cannot be traced. The line when a mouse leaves now ends `N found by
looking`: its reports taken on any look that was not its own controller's
interrupt - a deadline, or the other controller's. `usb_mouse` wants no more
than one in ten.

| check | what it establishes |
| ----- | ------------------- |
| `usb`: the 64-bit registers in 5.1's order | CRCR, DCBAAP, ERSTBA and ERDP all written, each low half then high half, in QEMU's trace from the driver's first CONFIG |
| `usb_mouse`: reports by interrupt | the line when the mouse is pulled out counts its reports found by looking, and no more than one in ten were |

**Controls**, each a copy of `xhci.c` built and booted, and put back byte for
byte:

| broken | what failed |
| ------ | ----------- |
| ERDP written high half first again | 1 of 13 in `usb` |
| the watch waiting on no interrupt | 3 of 18 in `usb_mouse`: the new check, and the two that count reports |

```
usb: 1 of 13 checks failed
  the driver did not write every 64-bit register low half first and high half second (xHCI 1.2 5.1), in QEMU's trace of 292 writes: the order broken in 12 places, the first ERDP's high half before its low half

usb_mouse: 3 of 18 checks failed
  the driver read 162 reports before the mouse was pulled out, fewer than two rounds of a ring (510), so its Link was not tested
  the driver read 162 reports for 640 movements 11.8 ms apart - fewer than four in five, which is a mouse read late: QEMU folds a movement into the one before while that is unread
  162 of the 162 reports were found by looking rather than brought by the controller's interrupt, more than one in ten
```

And as written, with `xhci.c` restored byte for byte: `usb` 13 of 13 and
`usb_mouse` 18 of 18, and `make test` whole - x86-64 117, the UEFI boots
29, the stick check 10, the host test 56 of 56, and the suites 158 of 158
and 154 of 154. **The ThinkPad has
not run it yet**; the photograph is `log xhci` after pulling the mouse out,
for the naming steps' times and the count found by looking.

## 18.48 This Machine on a real machine's bus

**On the ThinkPad, This Machine described a q35.** It listed twenty-two
devices on bus 0 and called every one "NO DRIVER" - the two xHCI controllers
a process was driving among them - and closed on a paragraph about QEMU's
bridges and SATA controller. The NVMe drive was not listed at all, and the
window could be resized while the report inside it could not. Diego, 13
September: "The this machine app is reporting old things, we neee to update it
and make sure is resizable as well".

Four things were wrong, each with a cause of its own:

- **Bus 0 alone.** `hal_bus_scan` walked it and nothing more, and the drive
  is behind a PCI Express root port. It follows bridges now, marking each bus
  a bridge leads to and walking them in order, with a device's bus in the
  high byte of `where` - the bridge offsets from gnu-efi's `pci22.h`, since the
  PCI specification is not in the references.
- **"Driven" was a list of what `claimed_here` had been taught**: virtio, and
  one class of sound controller. `pci_enable` is the one call every driver
  makes as it takes a device, so it records the address, and the scan asks
  that record.
- **Words written into the program**: "ramfb" as every screen's source,
  "virtio-sound" as every sound device, "no host controller driver" for USB,
  and "(SMP is being built)" beside the cores. The screen's source comes from
  the board now (`screen_source` in `sysinfo`, the line `hal_fb_describe`
  gives the boot log), the sound and xHCI controllers from the bus, and the
  three processor counts are shown when they differ.
- **The editor followed its left and top alone.**

`tools/run_x86.py`'s `machine_report`, 6 checks, on a q35 with an NVMe drive
behind a `pcie-root-port` and two xHCI controllers, with `machine` typed once
the USB driver has said it runs both:

| check | what it establishes |
| ----- | ------------------- |
| the listing against QEMU | the vendor and device of every PCI function, as QEMU's own `info pci` gives them once its firmware has numbered the buses |
| the drive | listed once, off bus 0, and driven |
| the controllers | both xHCI controllers driven |
| the count | "N devices found, M driven" agreeing with the lines |
| the screen | the Framebuffer row is the board's own description from the boot log |
| nothing stale | none of the three sentences that were never true of a ThinkPad |

`tools/run_screenshot.py`, one more check in the clipboard phase: This
Machine's window dragged bigger by its grip, and the report's own background
colour grows with it, measured against the window's colour beside it.

**Two of the first run's three failures were the check's own**, and are worth
keeping. `info pci` on a QEMU stopped before its first instruction lists
nothing behind a root port, because it is the firmware that numbers the bus
there - so the reference said the drive did not exist. And a report typed at
the first prompt had the second xHCI controller undriven, truthfully, because
the USB driver takes its controllers one after the other; `boot` gained
`after=` for it. The third was a stale-text check matching "q35" in the
machine's own SMBIOS name.

**Controls**, each put back byte for byte:

| broken | what failed |
| ------ | ----------- |
| the PC's scan following no bridge | 2 of 6 in `machine_report`: QEMU's list and the report differ by exactly the drive, and the drive check |
| `pci_enable` recording nothing | 2 of 6: the drive and both xHCI controllers undriven, on a report of "10 devices found, 0 driven" |
| the report pinned to its left and top | the display harness, in the clipboard phase |

```
machine_report: 2 of 6 checks failed
  `machine` did not list every PCI function QEMU has, behind the root port as well as on bus 0:
    machine: ['1234:1111', '1b36:000c', '1b36:000d', '1b36:000d', '8086:10d3', '8086:2918', '8086:2922', '8086:2930', '8086:29c0']
    QEMU:    ['1234:1111', '1b36:000c', '1b36:000d', '1b36:000d', '1b36:0010', '8086:10d3', '8086:2918', '8086:2922', '8086:2930', '8086:29c0']
  the NVMe drive behind the root port was not listed once, off bus 0 and driven:

machine_report: 2 of 6 checks failed
  the NVMe drive behind the root port was not listed once, off bus 0 and driven:
  the two xHCI controllers the USB driver took were not both listed as driven:

FAIL: waited 20s and This Machine's window was made bigger and its report stayed the size it opened at: an editor that never said it follows the right and bottom edges, in a window the manager resized.
```

Each `machine_report` complaint is followed by the report it read, cut here; the second's ends `10 devices found, 0 driven, 10 without a driver.`

And as written, with every control put back byte for byte: `machine_report`
6 of 6, the display harness 107 with This Machine's report following its
window, and `make test` whole - x86-64 123, the UEFI boots 29, the stick check
10, the disk 33, and the suites 158 of 158 and 154 of 154.

## 18.49 The disk started once

**Every question about the disk started it again.** `SYS_DISK_INFO` and the
grant of the disk to a process both called `hal_blk_init`, and that starts the
controller: an NVMe drive disabled, reset and given new queues, a virtio disk
taken back to status 0 and walked up to DRIVER_OK. Init asked at boot, the
disk server asked every time `/home/.super` was read, and This Machine asks as
it opens. Found by reading while This Machine was being built, then counted
in QEMU's own trace: a boot that ran `diskinfo` three times started the NVMe
controller ten times, and the virtio disk went through 0, 1, 3, 11 and 15
again for each. On x86 every NVMe start also spent one of the four MSI vectors
and a mapping of the registers. Nothing had broken, because nothing was in
flight when it happened - the disk server asks between its own requests - and
a `sys.disk()` from another process, on another core, had nothing like that
to rely on.

**So the kernel starts it once**, in `kmain` beside sound, while nothing else
runs - no other thread, no other processor started, no interrupt taken - and
keeps what the board answered (`process_disk_start`, `process_disk`).
`SYS_DISK_INFO` and `process_grant_disk` answer from that, without a lock,
and the boot log names the disk through `hal_blk_describe`, which both boards
had and nothing had declared. The guest suite's two tests that called
`hal_blk_init` themselves ask what boot kept; its reads and writes still go
to the driver.

**Checked in QEMU's trace, which is where a start can be seen:**

| check | what it establishes |
| ----- | ------------------- |
| `run_x86.py`'s `storage`: the NVMe controller started once by the kernel | `pci_nvme_mmio_start_success` once after the first `pci_nvme_mmio_stopped`, in a boot that asks about the disk four times |
| `run_disk.py` on `virt`: each virtio device set ready once | `virtio_set_status ... val 15` no more than once for any device, over the first boot's dozen commands |

**The firmware starts the disk too**, and the first run of both checks on
q35 counted it: two NVMe starts, and the virtio disk set ready twice. SeaBIOS
brings a drive up to look for something to boot and leaves it running. The
kernel's NVMe start begins by stopping the controller, so a start before the
first stop is the firmware's; a virtio start has no such mark, so the virtio
count runs on `virt`, which has no firmware, and the PC's half is the NVMe
drive's.

**Controls**, with the kernel starting the disk at every question again:

| broken | what failed |
| ------ | ----------- |
| `SYS_DISK_INFO` and the grant calling `hal_blk_init` again, as before | `storage`, 1 of 5: the kernel's starts counted after the firmware's, eleven of them; and `run_disk.py` on `virt`, the virtio disk set ready more than once |

```
storage: 1 of 5 checks failed
  QEMU saw the kernel start the NVMe controller 11 times in one boot that asked about the disk four times; it is meant to start it once and keep what it found

FAIL: QEMU's trace of the first boot has 1 virtio devices set ready, and 0x1038fbd20 set ready more than once - a device started again each time the disk is asked about.
```

Eleven where the build before the fix made nine, because the control keeps the start at boot and adds one for every question after it.

And as written, with both files put back byte for byte: `storage` 5 of 5 on
q35, `run_disk.py` 34 on `virt` and 33 on q35, and `make test` whole - x86-64
124, the UEFI boots 29, the stick check 10, and the suites 158 of 158 and 154
of 154, the block device's tests among them asking what boot kept.

## 18.50 The stick held to what the build wrote

**Nothing on the machine checked that the stick handed over the build's
bytes.** The loader read the kernel and the disk once and fingerprinted what
it read, which catches memory changing afterwards and says nothing about the
read itself; `mkusb.sh` has read a stick back on the Mac since §18.45, which
is another machine's USB stack. Nothing the ThinkPad has shown rules out a
stick returning other bytes there, and nothing there could have said so. The
roadmap had it as the thing before USB step four.

**So the build writes the sums, and the loader holds its read to them.**
`mkusb_image.py` puts `\boot\kosmos.sums` beside the kernel and
`\boot\disk.sums` beside the disk - "KOSMSUMS", the file's size, the page size,
and FNV-1a over each 4096 bytes (`boot/efi/sums.h`). `sums.c` compares a read
with them, with no firmware in it, and `against_build` in the loader says what
it found: the build's, page for page, or a refusal on the screen with how
many pages differ and the first. The kernel's loader line gains `the stick
against the build: same`. A stick with no sums is said to be unchecked and
used.

What a stick says under OVMF, and then a copy of it with one byte of its
kernel changed:

```
kosmos-boot: the kernel is the build's, page for page: 1988 pages
kosmos-boot: the kernel: 7948 KB in two copies, 1988 pages fingerprinted, entry 0x01001000
kosmos-boot: the disk is the build's, page for page: 1024 pages
-> the loader: kosmos-boot, 0 pages repaired before the firmware let go, 0 after, 0 lost; the disk: same; the stick against the build: same

kosmos-boot: the kernel: 1 of its 1988 pages is not the build's, the first page 3, at byte 0x00003000
kosmos-boot: this stick does not hold the files the build wrote
kosmos-boot: Kosmos cannot start from this stick. Press a key to return to the firmware.
```

`tools/test_efiboot.c`, 59 checks, 11 of them new:

| check | what it establishes |
| ----- | ------------------- |
| FNV-1a's published vectors | the empty string and "a", which pin the offset basis and the prime |
| a file and its own sums | four pages the same, the last of them short |
| one byte changed in the third page | one page wrong, the third |
| a byte changed at the very end | the last, short page checked to its last byte |
| a file a byte short | refused for its size, and the size the build wrote kept |
| sums for three pages against a file of four | not taken for its sums |
| no magic, pages of 4095 bytes, no sums at all | each refused as not a sums file |
| an empty file and its empty sums | taken as the build's, with no pages to compare |

`tools/run_uefi.py`, 34 checks, 5 of them new: the loader saying the kernel
and the disk are the build's, the kernel saying the stick was held to them,
and a copy of the stick with the thirteenth byte of its kernel's fourth page
changed - its sums as the build wrote them - refused on the serial line with
that page named.

**The first run did not build**: `sums.h` included `stdint.h` and not
`stddef.h`, and `sums.c` uses `NULL`. And the refusal said "1 of its 1988
pages are not the build's" until one page became singular.

**And the second control found a gap in the harness.** With the build's sums
wrong the good stick is refused, which is right, and `run_uefi.py` did not say
so: its third boot of that stick waits for the kernel's first instruction
through QEMU's gdbstub, that wait ended in the socket's timeout, and the run
died in a traceback before printing a single complaint. Any refusal of the
good stick would have done the same. `run_to` now answers false when the
guest does not get there, and `thinkpad_screen` hands back what the machine
said, so the checks name what did not happen.

**Controls**, each put back byte for byte:

| broken | what failed |
| ------ | ----------- |
| the loader taking every read as the build's, whatever `sums_check` found | `run_uefi.py`, 2 of 34: the stick with one byte of its kernel changed booted, so it was neither refused nor named a page |
| `mkusb_image.py` starting each page's sum one past FNV-1a's offset basis | `run_uefi.py`, 23 of 33: the good stick refused, all 1988 of its kernel's pages not the build's, and everything after the loader never reached |
| `sums_check` stopping before the last page, which is the short one | `test_efiboot`, 1 of 59: a byte changed at the very end of a file not found |

```
FAIL: 2 of 34 checks booting through Kosmos's loader under UEFI:
  a stick with one byte of its kernel changed was not refused: [..., 'kosmos-boot: both copies of the kernel are the file, page for page', 'kosmos-boot: handing over: entry 0x01001000, information at 0x7c1af000, trampoline at 0x7c1b6000']
  the refusal of a changed kernel did not name the one page, the fourth: [...]

FAIL: 23 of 33 checks booting through Kosmos's loader under UEFI:
  the machine found no processor line; ACPI is not reaching the kernel through the loader
  ...
  the loader did not say the kernel it read is the build's: [..., "kosmos-boot: the kernel: 1988 of its 1988 pages are not the build's, the first page 0, at byte 0x00000000"]
  ...
  the kernel does not say its loader held the stick to the build's sums: no loader line
  the harness did not stop at the kernel's entry and move the loader's framebuffer to 0x4000000000
  ...

FAIL: the last, short page is not checked to its last byte
FAIL: 1 of 59 checks on the UEFI loader's decisions
```

Thirty-three rather than thirty-four in the second, because the disk's own
check waits for the loader to name a disk, and a loader that refused the
kernel never read one. **The first run of that control printed no FAIL line
at all**: it ended in `TimeoutError: timed out`, raised in `run_to` while the
harness waited for a kernel that the refusal meant would never start - which
is the gap described above, and the reason for the second run.

And as written, with every file put back byte for byte: `test_efiboot` 59,
and 51 without a kernel to read; `run_uefi.py` 34; and `make test` whole -
x86-64 124, the disk across two boots 33, the stick check 10, and the suites
158 of 158 and 154 of 154.

## 18.51 A dead driver's button comes up

**A button a driver reported down stayed down when the driver ended.** The
board keeps each pointing device's buttons until the device says they came up
(`hal/pc/pointer.c`), and the USB driver is a process: killed, faulted, or
stopped between a mouse's press and its release, nothing would ever say so,
and the desktop would go on dragging for the life of the machine. The roadmap
had it since the mouse arrived. A mouse pulled out with a button down was
already let go of by the driver, and `usb_mouse` checks that; this is the case
where the driver is what went.

**So the kernel lets go on its behalf**, beside masking the interrupt lines it
claimed: `SYS_POINTER_MOVE` records that a process reported buttons, and
`process_exit` reports no movement and no buttons for it, then wakes the
sleepers as the report itself would have.

`user/pointer-$(ARCH).S` is an EL0 fixture that reports the left button down
and exits without reporting it up, with what the kernel answered as its exit
code:

| check | what it establishes |
| ----- | ------------------- |
| `dev: a dead driver's button comes up`, on x86-64 | the report taken, and the board's pointer with no button down once the fixture has exited |
| the same, on AArch64 | the report refused, because this board's pointer is a tablet, and the board refusing it again |

**Not checked with the real driver.** `usb_mouse` boots into the desktop and
ends nothing. The fixture makes the system call the driver makes, and the
release does not ask which process it is releasing for.

**Controls**, each put back byte for byte:

| broken | what failed |
| ------ | ----------- |
| `process_exit` not letting go for a process that reported buttons | the x86-64 suite, 1 of 155: `dev: a dead driver's button comes up` |
| `SYS_POINTER_MOVE` not recording that a process reported them | the same, 1 of 155 |

```
not ok 22 - dev: a dead driver's button comes up
FAIL: 1 of 155 test(s) failed:
  not ok 22 - dev: a dead driver's button comes up
```

The same line for both. On AArch64 neither can fail, because the report is
refused before either link is reached - which is what that board's half of
the check says.

And as written, with both files put back byte for byte: the suites 159 of 159
and 155 of 155, and `make test` whole - x86-64 124, the UEFI boots 34, the disk
across two boots 33 and the stick check 10.

## 18.52 A skip that says so, and only for a missing firmware

**`run_uefi.py` had never run on a machine without OVMF.** `capture()`
answered four values when it found no firmware and `main` took two, so the
`SKIP` written for that machine was a `ValueError`; the roadmap had it from 13
September. Reading it again found the other half: the same `None` from a boot
that started and gave no picture was printed as a skip too, and exited 0 - so
a gate whose screendump failed passed without a single check.

**So `main` asks for the firmware before it boots**, skips only when that is
missing, and fails a first boot that gives no picture; `capture()` answers the
two values it was always read as.

`tools/test_run_uefi.py` runs the harness's own `main` with its firmware and
its boots replaced. Nothing is booted, so it runs in `make test` beside the
other checks on this machine:

| check | what it establishes |
| ----- | ------------------- |
| `capture()` with no firmware | it answers `(None, why)`, the two values `main` unpacks |
| `main` with no firmware | exit 0 and a `SKIP:` line naming OVMF, with nothing raised |
| `main` with the firmware and a boot that gives no picture | exit 1 and a `FAIL:` line naming it |

**Run against the harness as it was**, before the fix:

```
FAIL: 3 of 3 checks on run_uefi.py where it cannot boot:
  capture() without OVMF did not answer (None, why), which is what main unpacks: (None, None, None, 'OVMF is not installed beside qemu')
  run_uefi.py without OVMF did not skip naming it: exit None, '', and raised ValueError('too many values to unpack (expected 2, got 4)')
  run_uefi.py with OVMF and a boot that gave no picture did not fail naming it: exit 0, 'SKIP: the monitor wrote no screendump'
```

**Controls**, on the harness as it is now, each put back byte for byte:

| broken | what failed |
| ------ | ----------- |
| `capture()` answering four values again | check 1, 1 of 3: `(None, None, None, 'OVMF is not installed beside qemu')` |
| `main` not asking for the firmware before it boots | check 2, 1 of 3: exit 1 and a `FAIL:` line where the skip belongs |
| a first boot with no picture printed as a skip again | check 3, 1 of 3: exit 0 and `SKIP: the monitor wrote no screendump` |

```
FAIL: 1 of 3 checks on run_uefi.py where it cannot boot:
  capture() without OVMF did not answer (None, why), which is what main unpacks: (None, None, None, 'OVMF is not installed beside qemu')

FAIL: 1 of 3 checks on run_uefi.py where it cannot boot:
  run_uefi.py without OVMF did not skip naming it: exit 1, "FAIL: the first boot through Kosmos's loader gave no picture: OVMF is not installed beside qemu"

FAIL: 1 of 3 checks on run_uefi.py where it cannot boot:
  run_uefi.py with OVMF and a boot that gave no picture did not fail naming it: exit 0, 'SKIP: the monitor wrote no screendump'
```

Each breaks exactly one of the three, so no check stands in for another.

And as written, with `run_uefi.py` put back byte for byte: the check 3 of 3,
and `make test` whole with it among the checks on this machine -
`run_uefi.py` 34 through a real boot on the path `main` now takes, x86-64 124,
and the suites 159 of 159 and 155 of 155.

## 18.53 A plug does not hold a mouse

**A device plugged in held every USB mouse until it was named**, on either
controller. The xHCI driver has one thread, and a plug's waits - USB 2.0's
debounce, a port's reset, each command and control transfer - waited on one
controller's interrupt and kept any report that came meanwhile for
afterwards; a mouse gets its next request only when its report is read. The
roadmap had it from the mouse's first day, and the ThinkPad showed what it can
cost on 14 September: the stick that dropped off its bus took 2.3 seconds to
be named again.

**Measured first**, with a scratch copy of the check: QEMU's mouse moved every
10 ms on the second controller, a keyboard plugged in part way through, and
every doorbell the driver rang for the mouse's endpoint read out of QEMU's
trace with its time (`-msg timestamp=on`, `usb_xhci_ep_kick`):

| keyboard plugged into | longest gap before the plug | after it, as it was | after it, fixed |
| --------------------- | --------------------------- | ------------------- | --------------- |
| the mouse's own controller | 12.2 ms | 106.6 ms | 12.5 ms |
| the other controller | 11.8 ms | 103.7 ms | 11.4 ms |

The hundred milliseconds are the debounce: QEMU answers commands and transfers
at once, so nothing else a plug waits for shows here.

**The fix is one wait** (`wait_serving` in `xhci.c`): every running
controller's interrupt, each mouse's report read as it comes, the waiter
answered only with an event that is not a report, and a deadline measured on
the counter rather than counted in wakes. Every wait a plug or an unplug makes
goes through it, and the kept reports went.

`tools/run_x86.py`'s `usb_mouse`, 22 checks, 4 of them new: a keyboard plugged
into the other controller and then into the mouse's own while the mouse
moves, each named, and the longest gap between two of the mouse's requests in
the 1.5 s after each plug no more than 50 ms - read out of the trace once QEMU
has gone, since it writes the file when it pleases. `usb` and `usb_hotplug`
pass unchanged, 13 and 17.

**Controls**, each put back byte for byte:

| broken | what failed |
| ------ | ----------- |
| `xhci.c` as it was before the fix | `usb_mouse`, 2 of 22: 152.3 ms between two of the mouse's requests with the keyboard plugged into the other controller, and 148.7 with it on the mouse's own |
| the fix with only the debounce slept rather than serving | `usb_mouse`, 2 of 22: 83.5 ms and 79.8 |

```
usb_mouse: 2 of 22 checks failed
  with a keyboard plugged into the other controller, the USB mouse's requests in the 1.5 s after were 328, and the longest gap between two was 152.3 ms, more than 50: the plug held the mouse (QEMU's trace, 3286 of the mouse's requests in all)
  with a keyboard plugged into the mouse's own controller, the USB mouse's requests in the 1.5 s after were 325, and the longest gap between two was 148.7 ms, more than 50: the plug held the mouse (QEMU's trace, 3286 of the mouse's requests in all)

usb_mouse: 2 of 22 checks failed
  with a keyboard plugged into the other controller, the USB mouse's requests in the 1.5 s after were 348, and the longest gap between two was 83.5 ms, more than 50: the plug held the mouse (QEMU's trace, 3297 of the mouse's requests in all)
  with a keyboard plugged into the mouse's own controller, the USB mouse's requests in the 1.5 s after were 345, and the longest gap between two was 79.8 ms, more than 50: the plug held the mouse (QEMU's trace, 3297 of the mouse's requests in all)
```

The first is longer than the scratch measurement's 104 and 107 ms, which booted
the MEGA image into a machine doing nothing else; both are a plug holding the
mouse. The second breaks only the debounce, the one wait QEMU makes long enough
to see, and the check still sees it.

And as written, with `xhci.c` put back byte for byte and the image rebuilt:
`usb_mouse` 22, `usb` 13, `usb_hotplug` 17, and `make test` whole - x86-64
128, the UEFI boots 34, the stick check 10, and the suites 159 of 159 and 155
of 155.

## 18.54 Bulk transfers: a stick asked what it is

**USB step 4, bytes each way on a bulk endpoint**, and the stick QEMU already
plugs into the `usb` check as what proves it: a stick's bulk endpoints carry
Bulk-Only Transport and nothing else, so the smallest real exchange is one
command - INQUIRY, a 31-byte wrapper out, 36 bytes in, a 13-byte status in.
Before it the driver read the stick's configuration, found no HID interface,
and left it.

**The decoder finds a stick** (`usb_decode.c`): mass storage, SCSI,
Bulk-Only, alternate setting 0, a bulk IN and a bulk OUT, and at SuperSpeed
each endpoint's companion burst. **The driver** configures both endpoints in
one Configure Endpoint, sends SET_CONFIGURATION, carries INQUIRY with Normal
TRBs through `wait_serving`, holds the status to Bulk-Only 1.0 6.3, and says
what the stick answered.

`tools/test_usbdecode.c`, 71 checks, 15 of them new:

| check | what it establishes |
| ----- | ------------------- |
| a high-speed stick | Bulk-Only, IN 1 and OUT 2 of 512 bytes, its interface, subclass and protocol |
| QEMU's SuperSpeed stick, from `dev-storage.c` | 1024 bytes and bursts of 15, from the companions |
| bursts of 3 and 7 | each companion's burst given to the endpoint before it |
| a burst of 16 | refused, and nothing half-kept |
| OUT before IN | each direction still its own |
| USB Attached SCSI, subclass 00h, alternate setting 1 | mass storage this cannot speak to, said to be that |
| an interrupt endpoint, endpoint 0, two INs, a packet size of 0 | no stick |
| a keyboard, then a stick | the stick behind it found |
| QEMU's mouse, then a stick | the mouse taken, the stick's fields left empty |
| a hub's interface | neither |

`tools/run_x86.py`'s `usb`, 15 checks, 2 of them new: the stick's line with
its endpoints as QEMU declares them at SuperSpeed - IN 1 and OUT 2, 1024 bytes
a packet in bursts of 16 - and INQUIRY answered through them as QEMU's disk
answers it: "QEMU", "QEMU HARDDISK", device type 0.

**Controls**, each put back byte for byte:

| broken | what failed |
| ------ | ----------- |
| the decoder taking an endpoint of any type for bulk | `test_usbdecode`, 1 of 71: an interrupt endpoint taken for bulk IN |
| the two bulk endpoint types swapped in their contexts | `usb`, 1 of 15: INQUIRY stalled at its command |
| the command wrapper's signature one off | `usb`, 1 of 15: the same stall, at the same step |

```
  an interrupt endpoint was taken for bulk IN
FAIL: 1 of 71 checks on USB configuration and report descriptors.

usb: 1 of 15 checks failed
  the stick did not answer INQUIRY through its bulk endpoints as QEMU's disk does - "QEMU", "QEMU HARDDISK", device type 0:
    xhci: 00:04.0 port 1: a stick: SCSI over Bulk-Only, bulk IN endpoint 1 and OUT endpoint 2, up to 1024 bytes a packet in bursts of 16
    xhci: 00:04.0 port 1: the INQUIRY's command failed: Stall Error (6)
```

The second and third print the same two lines: the check tells them apart from
a stick that answers, not from each other, and the driver names the step and
the code either way.

And as written, with every file put back byte for byte and the image rebuilt:
`test_usbdecode` 71, `usb` 15, `usb_hotplug` 17, `usb_mouse` 22, and `make test`
whole - x86-64 130, the UEFI boots 34, the stick check 10, and the suites 159
of 159 and 155 of 155.

## 18.55 A program run by its file

**Diego's `diego.lua`, and the three places it should have run from**: the
prompt, a Terminal and Tracker. Before this a bare word looked only in
`/bin`, `/home/diego.lua` at the prompt was taken for a command called
`home`, `diego.lua` went to Lua and failed on a table called `diego`, and
Tracker opened it in the editor. `ui.md` §16.15 has the rules.

| check | run by | what it establishes |
| ----- | ------ | ------------------- |
| `tools/test_filetypes.lua`, 24 checks, 11 of them new | `make test` | `kosmos: application` on the fourth line of an opening comment declares an application, after an empty line too; a program that says nothing, no source, and the same words in a string below the comment do not; an application opens as itself with no arguments, and a console program - or a Lua file whose source could not be read - in a Terminal handed its path; a `.txt` still opens in the editor, a file nothing claims in nothing, and the editor is still what handles a `.lua`, for Edit |
| `tools/run_shell.py`, 25 checks, 6 of them new | `make test` | in `/ramfs`, a file made at the prompt runs as `./hi.lua`, `hi.lua`, `/ramfs/hi.lua` and `run hi.lua`, and from a folder below it as `../hi.lua` - each printing the argument it was given, so no landing can be mistaken for another or for the echo of the line; a `.lua` that is not there says `no such program` with the path it looked for |
| `programs by file` phase of `tools/run_screenshot.py`, 3 checks | `make screenshot`, and so `make prepush` | in a Terminal, `cd /ramfs` and then `./term.lua` draws its thirty lines in the window; and a program opened as Tracker opens one - `how_to_open`'s answer sent to the window manager - gets a Terminal from it, which runs the program: `term` ends, code 0, after the launch |

Tracker's own double-click is not driven: the decision it asks is
`test_filetypes`'s, and the phase sends what that decision answers exactly as
Tracker sends it.

**Controls**, each put back byte for byte, and the image rebuilt after:

| broken | what failed |
| ------ | ----------- |
| the prompt's rule for a file taken out | `run_shell`: `./hi.lua did not run the file in the current directory` |
| a declaration read past the opening comment | `test_filetypes`, 1 of 24: the words in a string after the comment taken for an application |
| the Terminal ignoring what it was opened to run | the display harness: the Terminal the window manager started never ran `/ramfs/term.lua` - `wm: launched terminal -> true`, and no `term` ended |
| the Terminal taking `./term.lua` for a program in `/bin` | the display harness: `./term.lua` put nothing in the Terminal's window in 25 seconds |

The first control's session is the prompt as it was, and worth having in full:

```
kosmos> ./hi.lua one
error: stdin:1: unexpected symbol near '.'
kosmos> hi.lua two
error: stdin:1: syntax error near 'two'
kosmos> /ramfs/hi.lua three
no command called ramfs; `/commands` lists them
kosmos> run hi.lua four
hi-four
process 15 (hi) ended, code 0
kosmos> nothere.lua
error: stdin:1: attempt to index a nil value (global 'nothere')
```

**`run hi.lua four` still ran under it**, because `run` finds a file itself
rather than through the prompt's rule - which is why the check has a line for
each spelling rather than one.

And as written, with every file put back byte for byte and the image rebuilt:
`make screenshot` 110 on AArch64 and 108 on x86-64, and `make test` whole -
`test_filetypes` 24, `run_shell` 25, x86-64 130, the UEFI boots 34, the stick
check 10, and the suites 159 of 159 and 155 of 155.

## 18.56 A stick's size, and its first blocks

**USB step 5a**: after INQUIRY, TEST UNIT READY until the stick is ready, READ
CAPACITY (10), and READ (10) of block 1 and of the last block, each asked
whether it holds a GUID partition table's header. `usb.md` §7 has how.

| check | what it establishes |
| ----- | ------------------- |
| `tools/test_storagedecode.c`, 48 checks | the wrappers byte for byte as Tables 5.1 and 5.2 lay them out, and the lengths they have no room for; every command block; a status valid and meaningful by 6.3, and each way it is not; capacity from QEMU's answer, in 4096-byte blocks, past 32 bits and past what READ CAPACITY (10) counts; sense in both formats, deferred, short and cut off; CRC-32's check value; and GPT headers written by `zlib`, read at the wrong block, changed, resized and empty |
| `tools/run_x86.py`'s `usb`, 17 checks, 2 of them new | its stick laid out by `mkusb_image.write_gpt`, as a real one is; the driver says it holds 32768 blocks of 512 bytes, and finds the header at block 1 and its backup at block 32767 |

**Controls**, each put back byte for byte, and the image rebuilt after:

| broken | what failed |
| ------ | ----------- |
| READ (10)'s block address written little-endian | `test_storagedecode`, 1 of 48: the address not bytes 2 to 5, big-endian; and `usb`, 1 of 17: block 1 asked for as block 16777216, which the stick failed |
| a GPT header's MyLBA not checked | `test_storagedecode`, 2 of 48: the primary header taken for one at block 32767, and the backup for one at block 1. **`usb` passed all 17** - both headers sit where they say they are, so QEMU cannot tell - which is what the host test is for |
| the backup read one past the last block | `usb`, 1 of 17: READ (10) of block 32768, which the stick failed |
| a status wrapper's tag not checked | `test_storagedecode`, 1 of 48: the status for another command's tag taken for this one's |

The first and third print the same line where the table should be:

```
xhci: 00:04.0 port 1: the stick holds 32768 blocks of 512 bytes, 16 MB
xhci: 00:04.0 port 1: the READ (10), which the stick failed
```

It names the command and not why, because only TEST UNIT READY is followed by
REQUEST SENSE. Since 5b it is followed by the stick's reason (§18.57).

And as written, with every file put back byte for byte and the image rebuilt:
`test_storagedecode` 48, `usb` 17, `usb_hotplug` 17, `usb_mouse` 22, and
`make test` whole - x86-64 132, the UEFI boots 34, the stick check 10, and the
suites 159 of 159 and 155 of 155.

## 18.57 A stick recovered

**USB step 5b**: Reset Recovery after a command goes wrong, and the command
sent again, once; and REQUEST SENSE after any command the stick fails.
`usb.md` §7 has how.

| check | what it establishes |
| ----- | ------------------- |
| `tools/run_x86.py`'s `usb`, 19 checks, 2 of them new | started with `opt/kosmos/stickfault=signature`: the driver says it spoiled the first wrapper; QEMU's stick stalls it; the driver says so, runs Reset Recovery and sends INQUIRY again - and the checks before, INQUIRY's answer, the size and both GPT headers, all pass on a stick that was recovered |

**Controls**, each put back byte for byte, and the image rebuilt after:

| broken | what failed |
| ------ | ----------- |
| Set TR Dequeue Pointer skipped | `usb`, 3 of 19: the INQUIRY sent again stalled again - the controller retried the TRB that stalled - so no answer, size or table |
| the command sent again without Reset Recovery | `usb`, 3 of 19: the INQUIRY sent again got `no answer within a second`, from an endpoint still halted |
| `opt/kosmos/stickfault` taken and not acted on | `usb`, 1 of 19: the driver said it would spoil the first wrapper, and nothing stalled or was recovered |
| a refused Reset Endpoint not followed by Stop Endpoint | `usb`, 4 of 19: `the bulk IN's Reset Endpoint failed: Context State Error (19)` - the endpoint that did not halt, which is why Stop Endpoint is there |
| the backup read one past the last block | `usb`, 1 of 19: the table's line missing, and in its place the stick's reason, which no permanent check here can make it give |

```
xhci: 00:04.0 port 1: the stick holds 32768 blocks of 512 bytes, 16 MB
xhci: 00:04.0 port 1: the READ (10), which the stick failed: ILLEGAL REQUEST (21h/00h)
```

The first two show why each half of "reset a pipe" is there, and the fourth
that the endpoint that did not halt really takes the other path.

And as written, with every file put back byte for byte and the image rebuilt:
`usb` 19, `usb_hotplug` 17, `usb_mouse` 22, and `make test` whole - x86-64
134, the UEFI boots 34, the stick check 10, and the suites 159 of 159 and 155
of 155.

## 18.58 One wait for interrupts and callers

**USB step 5c**: `SYS_IRQ_WAIT_ANY` takes an endpoint, and answers
`IRQ_WAIT_CALLER` when a caller is queued there - a line with an interrupt
first. `usb.md` §7 has why and how.

| check | what it establishes |
| ----- | ------------------- |
| `irq: a wait on lines and an endpoint takes a caller too`, in the guest suite on both boards | a line with an interrupt answered before a queued caller, and the caller at once on the next wait, its message still there to collect; a caller ten ticks into a two-second wait ends it long before its deadline, with the wait off both lines; an interrupt ending a two-second wait early leaves it no longer the endpoint's watcher, so another thread's watch is not refused |
| `tools/test_syscall_args.lua`, 112 checks | the fourth argument the kernel now reads is one the wrapper passes, `sys4`: still 53 cases and 55 calls, none short |

**Controls**, on the AArch64 guest suite, each put back byte for byte:

| broken | what failed |
| ------ | ----------- |
| a queued caller not looked at | the new test, 1 of 160 |
| a caller answered before a pending line | the same |
| the wait never recorded as the endpoint's watcher | the same |
| the watcher left in place when an interrupt ends the wait | the same |
| `ipc_call` waking a watcher with a bare `thread_wake`, outside the lines' lock | **nothing: 160 of 160** |

The test is one verdict, so the first four fail it identically; which part
failed is known from the control, not from the output.

**The fifth passing is the honest result, not a gap papered over.** What the
lines' lock closes is a window a few instructions wide - after the waiting
thread lets the endpoint's lock go and before it has blocked - and a caller
has to arrive inside it, on another core, for the wake to be lost. No test
here can aim at that, so the guard rests on the argument in `kernel/irq.c`
and `usb.md` §7 rather than on this suite, and is recorded as such.

And as written, with both files put back and both test images rebuilt:
`make test` whole - the suites 160 of 160 and 156 of 156, x86-64 134, the
UEFI boots 34, the stick check 10, and the argument audit 112.

## 18.59 The block protocol, served by the driver

**USB step 5d**: `blockproto.h`, served by the USB driver on `/dev/blocks`,
and `sticks`, which prints a stick's size, names and partitions through it.
`usb.md` §7 has how.

| check | what it establishes |
| ----- | ------------------- |
| `tools/run_x86.py`'s `usb_blocks`, 3 checks | `sticks` at the prompt says unit 0 is 32768 blocks of 512 bytes, "QEMU" "QEMU HARDDISK"; it reads, through the driver, the partition `mkusb_image.write_gpt` wrote - "KOSMOS", blocks 34 to 32734, an EFI system partition - through a region it handed over; and a program written to `/ramfs` and run by its file reads one block past the last and is refused as past the last, by the driver |
| `usb`, `usb_hotplug` and `usb_mouse` | still pass, with every command's data - INQUIRY's, READ CAPACITY's, the first blocks' - now coming through each stick's transfer buffer |

**Controls**, each put back byte for byte, and the image rebuilt after:

| broken | what failed |
| ------ | ----------- |
| the driver's past-the-end refusal taken out | `usb_blocks`, 1 of 3: the read went to the stick, which failed it - `the READ (10), which the stick failed: ILLEGAL REQUEST (21h/00h)` - and the program was told `the stick failed it`, not `that block is past the last` |
| a read copied from the device's page, not the transfer buffer | `usb_blocks`, 1 of 3: `no GUID partition table at block 1` |
| a stick never made a unit | `usb_blocks`, 3 of 3: `sticks: no USB stick is ready`, and the read `no stick at that unit` |
| the block endpoint left off the watch's wait | **nothing: 3 of 3** |

**The fourth passing is the check's limit, said out loud.** Off the wait, a
request is answered at the watch's next deadline instead of at once - up to
50 ms later - and `sticks` still prints everything, only slower. Nothing here
times a request, so what 5c buys the driver is shown by §18.58's guest test,
not by this one.

**And one the gate found before the commit.** The driver first ended, on a
machine with no USB controller, by destroying the block endpoint - so that a
program asking would be refused rather than left waiting. But the endpoint
is in the capability list every program is started with, and the kernel
refuses a spawn that names a destroyed one: on AArch64 `run_headless.py` said
`boot: hello: could not start a process for it`, after the suite's 160 had
passed. The driver now stays and answers every request, as the audio and
network servers do on a machine with no card.

And as written, with every file put back byte for byte and the image rebuilt:
`usb_blocks` 3, and `make test` whole - x86-64 137, the machine with no display
on both boards with all 111 programs in `/bin`, the UEFI boots 34, the stick
check 10, the argument audit 112, and the suites 160 of 160 and 156 of 156.

## 18.60 `/home` on a stick

**USB step 5e**: `/home` on a stick's Kosmos partition, asked for with
`opt/kosmos/home=usb`; a write endpoint only the disk server holds; a flush
after each write to the journal's header; a unit made a name; and the disk
server waiting once for its stick. `usb.md` §7 has how.

| check | what it establishes |
| ----- | ------------------- |
| `tools/test_storagedecode.c`, 50 checks, 2 of them new | WRITE (10) is 2Ah, laid out as READ (10); SYNCHRONIZE CACHE (10) is 35h with block 0 and a count of 0, which is every block |
| `tools/run_x86.py`'s `usb_home`, 4 checks | two boots of one stick with a blank Kosmos partition: `diskinfo` says 28639 sectors, on the Kosmos partition, blocks 4096 to 32734, on both; the first boot formats the partition and saves a file; the driver says the stick kept the save's flush; the second boot reads the file back |
| `usb_second_stick`, 4 checks | with `/home` on a stick on the second controller and a file saved, a stick holding a partition of its own is plugged into the first through QEMU's monitor: the driver reads it; not one of its 32768 blocks changes; a second file is saved and both are in `/home`; `sticks` shows `/home`'s stick as unit 0 and the new one as unit 1 |
| `usb_home_late`, 3 checks | the machine started with `opt/kosmos/home=usb` and no stick, and the stick plugged in five seconds after the driver says it is watching: the driver reads it and a prompt comes; `diskinfo` says `/home` is the Kosmos partition; a file saved there has extents on a disk |
| `usb_blocks`, 4 checks, 1 of them new | a program's write and flush sent on `/dev/blocks` are each answered 7, read only |
| `usb`, `usb_hotplug` and `usb_mouse` | still pass, with a data phase that can go out as well as in, and each stick given its unit by number |

**Controls**, each put back byte for byte, and the image rebuilt after. E1 to
E4 ran before the wait was added, which touches none of the lines they break;
E5 ran again once `usb_second_stick` moved onto `PluggedMachine`.

| broken | what failed |
| ------ | ----------- |
| E1: the driver's read-only refusal taken out, for a write and a flush | `usb_blocks`, 1 of 4: `refused: 0 0` - and the driver said the check's stick had kept a flush a program sent |
| E2: the partition's first block left out of the disk server's reads and writes | `usb_home`, 4 of 4: block 0 of the stick is its protective MBR, so the partition was neither blank nor a filesystem and was left alone; the shell, told there was no filesystem, kept `/home` in memory - `diskinfo: no such path`, and `save` put a file with 0 extents there |
| E3: the disk server's writes sent on `/dev/blocks` | `usb_home`, 4 of 4: every write refused, so the blank partition would not format, and `/home` went to memory as in E2 |
| E4: the flush after the journal's header left out | `usb_home`, 1 of 4: everything else passed, and the driver never said a flush was kept |
| E5: a unit the Nth stick ready again | `usb_second_stick`, 2 of 4: the second save and the `cat` both answered `[string "kfs.lua"]:986: attempt to concatenate a nil value`, and `sticks` showed the new stick as unit 0 - but not one of its blocks changed, because the disk server read that stick's zeros as its filesystem and kfs failed before it could write |
| E6: the disk server's wait for its stick taken out | `usb_home_late`, 2 of 3: the stick was read and a prompt came, and then `diskinfo` answered `no such path` and `save` put a file with 0 extents in `/home` - memory, for the life of the machine |

**Two things were found before the commit, and neither by a check that
existed.**

**A unit was a position.** 5d made a unit the Nth stick ready, counting
controllers and then slots, and 5d's own paragraph said a number moves when a
stick before it leaves. The disk server keeps the unit it found its partition
on, so reading that paragraph again while writing 5e's was enough: a stick
plugged into an earlier controller would take `/home`'s requests.
`usb_second_stick` was written against it and passed only once units became
names. Put back, as E5, it fails - and not in the check written for it:
the new stick's bytes stayed the same, because the disk server, sent to that
stick, read its zeros as `/home`'s filesystem and kfs failed before it could
write, with a Lua error from `walk` that is now on the roadmap. The files
and `sticks` are what caught it, which is why the check has all three.

**`/home` was decided before the stick was there.** What E2 printed was the
clue: not `filesystem: none` but `diskinfo: no such path`, which is `/home` in
memory. The shell decides where `/home` is once, from one read of
`/home/.super` as it builds its namespace, and init does not wait for the USB
driver. Under QEMU the driver names its stick before the shell starts, so
`usb_home` passed; on the ThinkPad naming a stick takes seconds, and nothing
said the shell would come later. `usb_home_late` plugs the stick in late to
make it happen here, and the disk server now waits for its stick once,
for at least twenty seconds. With the wait taken out, as E6, `usb_home_late`
fails in exactly the way E2 showed.

**And what the disk server printed went nowhere.** `usb_home`'s first run
looked for a line the disk server printed when it found the partition, and
the line never came: the kernel refuses a write from a process that does not
own the console, as `run_disk.py` already says of its format line. So where
`/home` is travels in `sys.disk()`'s answer and `diskinfo` says it, and the
driver, which does own a line, says a stick's first kept flush.

**What none of it shows** is a power cut survived: QEMU's stick writes
straight to a file, so a flush is seen being sent and kept, and nothing more.

And as written, with every file put back byte for byte and the image rebuilt:
`make test` whole - x86-64 149, the machine with no display on both boards
with all 111 programs in `/bin`, the disk 33 across two boots, the UEFI boots
34, the stick check 10, the argument audit 112, and the suites 160 of 160 and
156 of 156 - and `make screenshot`, 110 display checks on AArch64 and 108 on
x86-64, on its second run. The first stopped on AArch64 in the `/bin` walk,
which counted one program; the second passed on the same tree, and
`roadmap.md`'s *Known and unexplained* has it.

## 18.61 A stick whose `/home` is a partition of its own

**USB step 5f**: `mkusb_image.py --home` and `USB_HOME=partition`; the disk
server taking a partition by its GUID; the kernel's command line no shorter
than the loader's; and `stickcheck.py` naming the partition. `usb.md` §7 has
how.

| check | what it establishes |
| ----- | ------------------- |
| `tools/run_x86.py`'s `usb_home_named`, 3 checks | two sticks with a Kosmos partition each, and `opt/kosmos/home` naming the second's GUID in small letters: `/home` is unit 1's 21 MB partition, a file saved there has extents, and the first stick's bytes are unchanged |
| `cmdline_long`, 1 check | a word at the end of a 335-character command line, given through Multiboot 1's `-append`, reaches `sys.boot` |
| `tools/run_uefi.py`'s home stick, 4 checks | the stick `mkusb_image.py --home` makes, booted through OVMF on xHCI with no screen: the kernel's line says the loader handed over no disk and the stick against the build is the same; `sys.boot` gives the partition's GUID, from the stick's command line; `diskinfo` says `/home` is that partition's sectors and blocks; a file saved there has extents |
| `tools/test_stickcheck.py`, 12 checks, 2 of them new | the home stick, streamed as a stick is read back, holds its image; and a byte of its Kosmos partition changed is damage, named `the Kosmos partition, /home` |
| `usb`, `usb_hotplug`, `usb_mouse`, `usb_blocks`, `usb_home`, `usb_second_stick` and `usb_home_late` | still pass, and so do `run_uefi.py`'s first stick and its refusals, with a kernel that keeps twice the command line it did |

**Controls**, each put back byte for byte, and the images rebuilt after:

| broken | what failed |
| ------ | ----------- |
| F1: the partition's GUID ignored by the disk server | `usb_home_named`, 2 of 3: `/home` was unit 0's 13 MB partition, and the stick not named was written |
| F2: the kernel's command line back to 256 bytes | `cmdline_long`, 1 of 1: `tail: nil` - the word at the end of the 335-character line was cut off |
| F3: `mkusb_image.py --home` not putting the GUID on the command line | `run_uefi.py`'s home stick, 3 of 4: the loader handed over no disk and a kernel that was the build's, and then `sys.boot` answered `named: nil`, `diskinfo` did not name the partition, and `save` put a file with 0 extents in `/home` - memory, since nothing had asked for the stick |
| F4: `stickcheck.py` not naming the partition | `test_stickcheck.py`, 1 of 12: the changed byte was still damage, exit 1, and was named `past the ESP, where the backup GPT is` |

**What neither the image nor QEMU can show**: whether the ThinkPad's firmware
boots a stick with a second partition after its ESP, and whether the stick it
boots from answers the disk server's writes as QEMU's does. The first is a
stick to try, offered beside one that has booted.

And as written: `make test` whole - x86-64 153, the UEFI boots 38, the
stick check 12, the machine with no display on both boards with all 111
programs in `/bin`, the disk 33 across two boots, the argument audit 112,
and the suites 160 of 160 and 156 of 156 - and `make screenshot`, 110
display checks on AArch64 and 108 on x86-64.

## 18.62 FAT, read on the Mac

**USB step 6a**: `fat_decode.c`, what a FAT16 or FAT32 volume's bytes mean,
and `fatls`, which walks a volume in an image with nothing else. Nothing on
the machine uses either yet. `usb.md` §8 has how.

| check | what it establishes |
| ----- | ------------------- |
| `tools/test_fatdecode.c`, 78 checks | bytes built from the specification. A FAT16 and a FAT32 volume's geometry: RootDirSectors, FirstDataSector, the root and a cluster's sector, and the label, with `NO NAME` as none. 4,084 clusters is FAT12 and refused, 4,085 and 65,524 are FAT16, 65,525 is FAT32. Each field a boot sector is held to, refused one at a time. Where a cluster's entry is, and what it says: the end of a chain, a bad cluster, free, a cluster past the last, and FAT32's top four bits ignored. Short names: `DIR_NTRes`'s two case bits, 0x05, 0xE5, 0x00, a byte above 0x7F, dot and dotdot, the label, and an entry both a directory and a label. Long names: two pieces, another name's checksum, a piece that fills exactly, UTF-8 from UTF-16 and a surrogate pair, a lone surrogate, pieces out of order, a free entry inside a set, an `LDIR_Type` not zero, and 21 pieces. A name found without regard to case, and an accented one compared exactly |
| `tools/test_fat.py`, 24 checks | four volumes mtools made in a 64 MB image - FAT16 at 4 sectors a cluster and FAT32 at 1, each with no partition table and in an MBR partition at sector 2048 - read by `fatls`: the kind and the label; every directory, three deep; every file's name, size and bytes, among them a long name, a name with an accent, an empty file and files one byte either side of a cluster; a file in two runs of clusters; and `photos 2024/ITALY/img_2213.jpg` finding `/Photos 2024/Italy/IMG_2213.JPG` |

**Controls**, each a copy of `fat_decode.c` changed in the scratchpad and
compiled with the tests there, so the tree was never touched:

| broken | what failed |
| ------ | ----------- |
| C1: FAT12's boundary, `< 4085` made `<=` | `test_fatdecode`, 1 of 75: 4,085 clusters was no longer FAT16 |
| C2: FAT32's boundary, `< 65525` made `<=` | `test_fatdecode`, 1 of 75: 65,525 clusters was no longer FAT32 |
| C3: FAT32's top four bits kept | `test_fatdecode`, 2 of 75: 0x10000000 was not free, and 0x30000005 did not lead to cluster 5 |
| C4: a long name's checksum not compared | `test_fatdecode`, 1 of 75: another name's pieces became this entry's name |
| C5: 0x05 taken for a free entry | `test_fatdecode`, 1 of 75 |
| C6: `DIR_NTRes`'s case bits ignored | `test_fatdecode`, 3 of 75; `test_fat.py`, 8 of 24: `HELLO.TXT`, `EMPTY.DAT`, `NOTES.TXT` and `SPREAD.BIN` in capitals on every volume |
| C7: names compared with their case | `test_fatdecode`, 1 of 75: `readme.txt` did not find `README.TXT`; `test_fat.py`, 4 of 24: the picture not found in the wrong case, on every volume |
| C8: a cluster's sector one cluster off | `test_fat.py`, 14 of 24: directories and files read from their neighbours - a `/Photos 2024/Rome`, and wrong bytes in `hello.txt` |

**Two things these found before the controls ran.** The first `test_fat.py`
failed its two-runs check on both FAT32 volumes with a correct reader: mtools
follows FAT32's free cluster hint past a hole, so the file never went in two
runs until the test set the hint back to cluster 2. And a control that did not
bite: turning 0x05 back into 0xE5 changed nothing, because both are shown as
`_`. That line was removed, and the rule's one visible effect - the entry is
not free - is what C5 breaks.

And as written: `make test` whole - FAT 75 and 24, x86-64 153, the UEFI
boots 38, the stick check 12, the machine with no display on both boards
with all 111 programs in `/bin`, the disk 33 across two boots, the argument
audit 112, and the suites 160 of 160 and 156 of 156. The display harness was
not run: nothing in the image changed, since `fat_decode.c` is not in it
until 6b.

## 18.63 Disk Benchmark at the prompt

**Storage at full speed, step 1** (`roadmap.md`, *Being built now*):
`/lib/diskbench.lua`, the `diskbench` program, and `blocks.lua`'s `fill`.
The window comes later, from `docs/diskbench.html`. Under QEMU the speed is
QEMU's, so what these hold the benchmark to is **saying what it measured and
what it could not**.

| check | what it establishes |
| ----- | ------------------- |
| `tools/run_diskbench.py`, 7 checks | a blank disk the machine formats itself: `diskbench` lists `/home`; `diskbench /home 1 1` reads and writes sequentially at speeds above zero and reads 4 KB at random at a number of IOPS above zero; the random write says a write replaces the whole file; the rows eight and thirty-two at once each say, for read and write, that nothing queues; the run is saved in `/home/benchmarks` and `ls` shows it there; and its test file is gone from `/home/.diskbench` afterwards |
| `tools/run_x86.py`'s `usb_diskbench`, 5 checks | the stick `usb_blocks` reads, with `diskbench usb 0 1 1`: listed as unit 0 by its INQUIRY names; its sequential and random reads measured through `/dev/blocks` and both writes refused by the program; its sequential reads said to be 124 KB, the most one USB read moves; and the stick's image file the same, by SHA-256, before and after |

**Controls**, each made in the tree, built and run, and every file put back
byte for byte after (checked):

| broken | what failed |
| ------ | ----------- |
| C1: the test file not removed afterwards | `run_diskbench.py`: its test file was left in `/home/.diskbench` |
| C2: the queued rows measured one at a time, and labelled eight and thirty-two at once | `run_diskbench.py`: `sequential 1 MB x8` read 90.1 and wrote 25.3 MB/s, and `random 4 KB x32` read 1954 IOPS - both caught as rows that should have said nothing queues |
| C4: the run not saved | `run_diskbench.py`: nothing in `/home/benchmarks` |
| C1, C2 and C4 together | 4 of 7, each failing its own check |
| C3: a 1 MB test file again | `run_diskbench.py`, 6 of 7: the journal refused the file, so no row ran; only the listing passed |
| C5: `fill` moving nothing | `usb_diskbench`, 2 of 5: both read rows said "nothing was moved" |

**What QEMU cannot show**: any speed. The numbers that mean something are the
ThinkPad's - the Kingston's blocks, and `/home` on its partition - and they
are the baseline step 2 takes.

**Found by the first run, and written into `kfs.lua`, `roadmap.md` and
`README.md`**: a 1 MB test file could not be written at all - "more blocks
changed than the journal can hold". kfs journals every block a write changes,
data included, and one transaction holds at most 254 blocks with the file's
metadata, so no write reaches a megabyte, though `diskfs` accepts one. The test
file is sized from `kfs.JOURNAL_BLOCKS` now, and says why.

**Not given a control**: the stick unchanged by SHA-256. Nothing in
`diskbench` writes a drive, and `/dev/blocks` refuses a write from any program
(`usb_blocks`), so there is no change to this program that could make it write
one; the check stands as a guard for a later version that could.

**And the first control script was wrong, and said it was right.** It kept
each file it broke under its name alone, and two of them are called
`diskbench.lua` - so the engine was put back as the program, and a check that
compared each file with the copy it had kept said byte for byte. It came out
because the second control found no text to change. The engine was rebuilt
from its draft with the two changes made since, and the script now keeps each
file by its whole path and compares against checksums taken before anything
was broken; these are that script's results.

And as written: `make test` whole - Disk Benchmark 7, x86-64 158 with
`usb_diskbench`'s 5 among them, the UEFI boots 38, the stick check 12, the
machine with no display on both boards with all 112 programs in `/bin`, the
disk 33 across two boots, the argument audit 112, FAT 75 and 24, and the
suites 160 of 160 and 156 of 156. The display harness was not run: nothing a
screen shows changed.

## 18.64 Where the time went

**Storage at full speed, step 2** (`roadmap.md`, *Being built now*): the disk
server counts what its device costs - calls, bytes and counter ticks inside
`sys.disk_read` and `sys.disk_write`, wrapped after `stick_home` has put a
stick's behind them - and answers `/home/.device`, which touches no disk, so
asking adds nothing to the answer. Disk Benchmark reads it before and after
each `/home` run and prints the device's share.

| check | what it establishes |
| ----- | ------------------- |
| `tools/run_diskbench.py`, 8 checks, 1 of them new | the sequential read and write and the random read on `/home` each say how much of their run was the device, above zero, and the two parts add to 100 |
| `run_x86.py`'s `usb_diskbench` | unchanged: a drive's line says the USB driver does not count its own time yet |

**Control**, made in the tree, built and run, and put back byte for byte
against a checksum taken before (checked):

| broken | what failed |
| ------ | ----------- |
| C6: the disk server's reads and writes not adding their counter ticks | `run_diskbench.py`, 1 of 8: all three shares were 0% |

**What it showed under QEMU**, for the shape of it and not for a speed:

| `/home` on | sequential read | sequential write | random 4 KB read |
| ---------- | --------------- | ---------------- | ---------------- |
| the kernel's disk, AArch64 and virtio | 90.4 MB/s, the device 66% | 26.0 MB/s, 51% | 1897 IOPS, 49% |
| a USB stick's Kosmos partition, x86 | 34.3 MB/s, 86% | 13.6 MB/s, 70% | 975 IOPS, 72% |

**"The device" is everything inside one block call**: the system call or the
message to the USB driver, the driver, and QEMU's device behind it - and kfs
makes one such call for every 4 KB it reads or writes, since `kfs.read_block`
asks for one block and the journal writes one block at a time. On a stick's
`/home`, which is the ThinkPad's path, that is most of every run. A stick's
call already accepts 124 KB; the kernel's takes 4 KB, in both its bounce
buffer and the binding's `DISK_MAX_READ`. That is step 3.

And as written: `make test` whole - Disk Benchmark 8, x86-64 158, the UEFI
boots 38, the stick check 12, the machine with no display on both boards with
all 112 programs in `/bin`, the disk 33 across two boots, kfs's format 47, the
argument audit 112, and the suites 160 of 160 and 156 of 156.

## 18.65 In runs, not a call a block

**Storage at full speed, step 3** (`roadmap.md`, *Being built now*): step 2
measured the block calls as most of every run, and kfs made one for every
4 KB. Now the kernel's disk call moves up to 124 KB - thirty-one pages, what
one USB read moves - and says so in `struct diskinfo`'s `most`, which
`sys.disk()` passes on and a stick's `/home` answers too; kfs reads a file's
neighbouring blocks in as few calls as that allows, without cutting them into
4 KB strings; the disk server hands a read over in 124 KB windows; and the
journal writes its descriptor and data as one run, and after the commit block
the blocks' homes sorted into runs.

| check | what it establishes |
| ----- | ------------------- |
| `tools/test_kfs.lua`, 53 checks, 6 of them new | the stand-in disk refuses a call over 124 KB and counts calls as well as blocks, and says `most`; a 40-block file reads back whole in at most two calls; a window across 26 blocks is one call and the right bytes; a commit makes at most a quarter as many write calls as it writes blocks, and reads back; and every check that was here - the power-loss window, a replay done twice, an uncommitted journal ignored - still passes with the journal written in runs |
| `tools/kfs.lua`, the image tool, by hand | it stands in for the disk with no `sys.disk`, so kfs moves a block at a time for it, as it always did: a 1 MB file put into an image and got back identical on the patched kfs |
| `tools/run_diskbench.py`, 8, and `run_x86.py`'s `usb_diskbench`, 5 | unchanged, and passing with the kernel moving 124 KB a call and kfs reading and journaling in runs |

**Controls**, each in a copy of the patched `kfs.lua` beside a copy of the
test, so the tree was never touched:

| broken | what failed |
| ------ | ----------- |
| C7: a file's blocks read one a call | `test_kfs.lua`, 2 of 53: the 40-block file took 40 calls, and the 26-block window 26 |
| C8: the journal's writes one block a call | `test_kfs.lua`, 1 of 53: 89 calls for 89 blocks |
| C9: a window's offset into its first block lost | `test_kfs.lua`, 3 of 53: the new window's bytes, and two window checks that were already here |

**Before and after, under QEMU** - the runs of §18.64 again, on the same
footing, which is the one comparison QEMU's numbers are good for:

| `/home` on | | sequential read | sequential write | random 4 KB read |
| ---------- | - | --------------- | ---------------- | ---------------- |
| the kernel's disk, AArch64 | before | 90.4 MB/s, the device 66% | 26.0 MB/s, 51% | 1897 IOPS, 49% |
| | after | 352.6 MB/s, 40% | 43.9 MB/s, 9% | 1908 IOPS, 48% |
| a USB stick's Kosmos partition, x86 | before | 34.3 MB/s, 86% | 13.6 MB/s, 70% | 975 IOPS, 72% |
| | after | 211.8 MB/s, 57% | 33.3 MB/s, 19% | 958 IOPS, 72% |

**What it says.** Sequential reads are 3.9 and 6.2 times as fast and writes
1.7 and 2.4, because reading the 768 KB test file is now about seven device
calls where it was a hundred and ninety-two. A random 4 KB read did not move,
and was not expected to: it is one block, so it was one call before and is one
call now, and its time is the request itself - from the program through the
namespace to the disk server and back. And **a write is now mostly not the
device**: 81 and 91% of a sequential write is kfs and what is around it - the
whole file assembled in the disk server's Lua heap, cut into block strings,
checksummed, and written twice through the journal. That is the next cost.

**And why QEMU cannot say which part of it.** kfs's write path was profiled
on this Mac, the same `kfs.lua` over a stand-in disk that costs nothing: ten
stores of the 768 KB file in transactions took 1.3 ms each of kfs's own Lua -
allocating its blocks about half a millisecond of it, since the bitmap scan
skips a full byte in one test - plus the journal's checksum, which is C on the
machine and was a Lua stand-in there. Under QEMU the same write took about
17 ms, 91% of it not the device. The difference is QEMU: it runs the guest's
code through a translator many times slower than the silicon, and its disk is a
file on this Mac that costs almost nothing - so every write here is CPU work
made large beside a device made small. `CLAUDE.md` says QEMU's numbers are not
performance numbers, and this is the case that shows it: what a write costs on
the ThinkPad, where the device is a real stick and the journal writes each
data block twice, is the ThinkPad's to say.


And as written: `make test` whole - kfs's format 53, Disk Benchmark 8, x86-64
158 with `usb_diskbench`'s 5, the UEFI boots 38, the stick check 12, the
machine with no display on both boards with all 112 programs in `/bin`, the
disk 33 across two boots, the argument audit 112, and the suites 160 of 160
and 156 of 156.

## 18.66 The media engine, heard

**Music, step 1** (`docs/music.html`, `roadmap.md`): `/lib/media.lua`, the
engine under Music and under a video app later - reading a file a window at
a time, decoding, converting to what the device takes, feeding the audio
server without waiting on it, the clock and seeking - taken out of `music.lua`,
which is now its first user and looks as it did, with a bar that seeks when it
is clicked. Time is the sound's: the position is the frames that came out of
the speaker plus where the last seek landed. A seek closes the stream and opens
it again, since nothing can drop what a stream has queued, and the engine gives
its read pages back when it closes, which `music.lua` never did.

| check | what it establishes |
| ----- | ------------------- |
| `tools/run_media.py`, 8 checks | a six-second tone made on the host, on a disk; QEMU's virtio-sound writing what the guest plays to a WAV here. **At the prompt**: `media.open` says six seconds; after a second of playing the position is about one; after a seek to four and a little more, about four and a half; the file finishes at about six; and about three seconds of tone come out of the speaker - one before the seek and two after - at the level they went in. **In Music**: Play clicked and then the bar three quarters along, and about three more seconds come out, not six; and Music prints no error |

**Controls**, made in the tree, built and run, and put back byte for byte
against checksums taken before:

| broken | what failed |
| ------ | ----------- |
| C10: a seek moves the clock and not the reading | `run_media.py`, 2 of 8: 5.73 seconds of tone came out at the prompt, and 5.64 in Music |
| C11: a seek forgets where it landed | `run_media.py`, 2 of 8: 0.586 just after the seek to four, and finished at 1.985 |
| C12: Music's bar does not seek when clicked | `run_media.py`, 1 of 8: 6.01 seconds came out of Music |

The engine's first run, before the test existed, printed a position of 1.016
after a second, 4.580 after the seek to four and 0.6 more, finished at 5.985,
and 3.04 seconds of tone heard.

And as written: `make test` whole - the media engine 8, Disk Benchmark 8,
kfs's format 53, x86-64 158, the UEFI boots 38, the stick check 12, the
machine with no display on both boards with all 112 programs in `/bin`, the
disk 33 across two boots, the argument audit 112, and the suites 160 of 160
and 156 of 156.

## 18.67 What an audio file says about itself

**Music, step 2** (`docs/music.html`): `/lib/tags.lua`, the reader Diego
chose - "cant we just gget the attributes from the mp3 file with a reader?" -
so a song's title, artist, album, genre, year, track and cover are read from
the file and nothing is written onto it; and `media.tags(path)` in front of it,
reading through one page. An MP3's ID3v2 tag, versions 2.2, 2.3 and 2.4, with
ID3v1 filling what it leaves out or standing alone, and a WAV's `LIST INFO`.
The cover comes back as where it is and how long, not its bytes.

| check | what it establishes |
| ----- | ------------------- |
| `tools/test_tags.lua`, 23 checks | files built by hand from the format and read back on the host through the same `tags.lua`. **ID3v2.3**: a Latin-1 title, a UTF-16 artist with a byte order mark, an album ended by a NUL, `(43)Punk` read as Punk, the year and the track, and a 5000-byte cover found at the byte where its picture starts. **ID3v2.4**: syncsafe frame sizes - including a 1000-byte cover with a frame after it, which only a size read as seven bits a byte finds - UTF-8, a year out of a whole date, a genre that is only a number left out, and a compressed frame skipped. **ID3v2.2**: three-letter frames and a PNG cover in place. An extended header stepped over; a tag that says it is longer than the file read as far as it goes, without failing. **UTF-16**: a surrogate pair one character and a lone half U+FFFD. **ID3v1** alone, with ID3v1.1's track, and filling in after ID3v2. A **WAV's** `LIST INFO`, with odd lengths padded. A file with no tag says nothing; Latin-1 that is not UTF-8 becomes UTF-8 |

**Controls**, each in a copy of `tags.lua` beside a copy of the test, so the
tree was never touched:

| broken | what failed |
| ------ | ----------- |
| C13: ID3v2.4's frame sizes read as plain numbers | first nothing - and then, with the 1000-byte frame added, 2 of 23: the frame after it was not found, and the cover was not in place |
| C14: a cover's description skipped a byte short | 2 of 21: the ID3v2.3 cover at 160 for 5001 bytes where it is 161 for 5000, and the ID3v2.2 cover |
| C15: ID3v2.3's extended header not stepped over | 1 of 21: the title after it was not read |

**C13 is the one worth keeping.** Every ID3v2.4 frame the test first built
was under 128 bytes, and below 128 a syncsafe size and a plain one are the same
four bytes - so a reader that got the size wrong passed a test that could not
tell. A cover is thousands of bytes, and a tagger's comment is often hundreds;
the frame of 1000 is there so the difference is one the test can see.

**What it does not do, as `tags.lua` says**: a genre given only as ID3v1's
number is left out, since the table of names would be written from memory;
compressed and encrypted frames are skipped; and a tag unsynchronised as a
whole gives its text and no cover.

**And the first real file, which said nothing.** Diego's Basket Case, the MP3
on his disk, was read twice by the same `tags.lua` - on this Mac from the file
in his Downloads, and on the machine through `media.tags` from a copy of his
disk - and both answered that it has no title, artist, album, genre, year,
track or cover. The bytes agree: its ID3v2.4 tag is 127 bytes, three `TXXX`
frames an MP4 container carried over (`major_brand dash`, `minor_version 0`,
`compatible_brands iso6mp41`) and `TSSE Lavf62.3.100`, the encoder, with no
ID3v1 and no APE tag at its end. A file converted from a video, and a reader
that is right to find nothing: so Music names a song like it by its file, as
`docs/music.html` already draws one.

And as written: `make test` whole - audio tags 23, the media engine 8, Disk
Benchmark 8, kfs's format 53, x86-64 158, the UEFI boots 38, the stick check
12, the machine with no display on both boards with all 112 programs in
`/bin`, the disk 33 across two boots, the argument audit 112, and the suites
160 of 160 and 156 of 156.

## 18.68 A stick that does not do SYNCHRONIZE CACHE, told once

**What it found on the ThinkPad.** The Kingston answers every SYNCHRONIZE
CACHE (10) with ILLEGAL REQUEST, 20h/00h, and the driver sent one after each
write to the journal's header - twice a commit - each followed by a REQUEST
SENSE and a line on the screen (`usb.md` §7).

**Seen under QEMU first, before any fix.** QEMU's stick does every flush, so
the stick's image is opened through blkdebug with a rule that fails each flush
with EINVAL, which QEMU's SCSI disk answers as ILLEGAL REQUEST, 24h/00h:

```
[inject-error]
event = "flush_to_disk"
iotype = "flush"
errno = "22"
once = "off"
```

On the 0.10.63 build three saves printed `the SYNCHRONIZE CACHE (10), which
the stick failed: ILLEGAL REQUEST (24h/00h)` six times, and `diskinfo` gave the
reason as `the USB driver refused it, error 6` - the ThinkPad's photographs,
reproduced.

**Two checks, one on each side.** `test_storagedecode` holds
`scsi_not_supported` to six senses: 20h/00h and 24h/00h are a command the
stick does not do; 24h/02h, 21h/00h (a block out of range), NOT READY with
20h/00h and ILLEGAL REQUEST with no code sent are not - 56 checks. And
`run_x86.py`'s `usb_flush_refused` boots that stick, saves three files and asks
`diskinfo`: the driver says once that the stick does not do the command and
never that the stick failed it, the saves land, and the cache line gives the
reason:

```
xhci: 00:04.0 port 1: the stick does not do SYNCHRONIZE CACHE (10), so it is not asked again: ILLEGAL REQUEST (24h/00h)
  its cache: not written out when asked, so a commit is only as safe as the stick (the stick does not do SYNCHRONIZE CACHE)
```

| Control | What failed |
|---|---|
| C5: `scsi_not_supported` ignores the qualifier | 1 of 56: 24h/02h taken as 24h/00h |
| C1: the driver does not remember the refusal | `usb_flush_refused`: 6 lines saying the stick does not do it, one a flush |

**C1 did not bite the first time, and that is why there is one memory.** The
disk server also stopped asking after the first refusal, so the driver was
never sent a second flush - and with the driver's memory taken out, the check
still passed with one line. Two memories of one fact are
two things to keep in step, and here one hid the other from the test. So the
driver, which owns the stick, remembers; the disk server asks every time and
is answered from that memory without a transfer, which costs it a message.

## 18.69 What finding the stick took

**What it is for.** On the ThinkPad the driver had the stick ready at 4.963 s
and the prompt came at 22 (`boot.md`), and the disk server may wait up to
twenty seconds for the stick without a word to anyone - it owns no console.
So it counts every look, the step each look that found nothing stopped at, and
the counter at the first look and at the one that found it; `diskinfo` says
them in the log's own seconds, from `sys.info().log_origin`, the counter's
reading at the log's zero, which the kernel now hands out.

**Checked twice.** `usb_home`, where the stick is there at boot: the origin is
set and no later than `sys.ticks()`, and `diskinfo` names a first look no later
than the look that found it. `usb_home_late`, where the stick goes in five
seconds after the driver starts watching: at least ten looks, at least nine of
them finding no stick named, and the find at least a second after the first:

```
  found at 6.52 s, by look 47; the first look was at 0.10 s
    46 look(s) before it found no stick named yet
```

| Control | What failed |
|---|---|
| C2: a look that finds nothing is not counted by where it stopped | `usb_home_late`: `found at 6.77 s, by look 48`, and no line of looks before it |
| C6: `console_log_origin` answers 0 | `usb_home`: `origin false` |

**What C6 cannot show under QEMU**, and says so: the times themselves barely
move without the origin - first look 0.33 s rather than 0.10 - because QEMU's
counter starts with the machine, a moment before the kernel. On the ThinkPad
the firmware's seconds come first, and that is the difference the field exists
for; the check holds the field to its definition instead.

## 18.70 A diagnosis off the stick

**What it replaces.** Everything that reached this Mac from the ThinkPad was a
photograph of forty lines of a screen. Diego: "a log file of things you need so
I can send it to you for a full diagnosis ... instead of photos of logs".
`diagnose` writes the build, the machine, the device server's nodes, the disk,
`/home`, the sticks, the processes and the whole log to `/home/diagnose.txt`;
`log save` writes the log alone; `make stick-log` on the Mac copies the stick's
Kosmos partition out through its raw device and takes the file from the copy
with `kfs.lua` (`usb.md` §7).

**On the Mac, `test_sticklog.py`** (6 checks): a stick image built as
`mkusb_image.py` builds one, with a log on a kfs disk in its Kosmos partition;
`sticklog.py` copies that partition byte for byte and leaves the stick as it
was; `kfs.lua get` gives the log back from the copy; and a disk with no GPT, a
GPT with no Kosmos partition and a source ending inside the partition are each
refused by name.

**On the machine, `usb_home`**: `log save` and `diagnose` are typed on the
first boot, and after both boots the stick's partition is copied out by
`sticklog.py` and both files taken from it by `kfs.lua` - the whole way a
diagnosis reaches the Mac but the raw device. The log has to end with the
command that saved it; the diagnosis has to begin `Kosmos diagnosis`, hold all
eight sections and the Kosmos partition's line, and end with `diagnose`.

| Control | What failed |
|---|---|
| C4: `sticklog.py` starts a block late | 2 of 6: the copy is not the disk put in it, and `kfs: that image does not hold a Kosmos filesystem` |
| C3: `log save` writes 4096 bytes | `usb_home`: what came back was 4096 bytes ending in the middle of the driver's start |
| C7: `diagnose` leaves out the log | `usb_home`: 12451 bytes, missing `== log` |

**The first `diagnose` wrote nothing, and the check is what said so.** It read
every name `/dev` lists, and `fs.list` gives what is mounted below a directory
as well as what its server holds - `/dev/console` among them, whose read is a
line somebody types. It sat at the prompt waiting for one: 0 bytes on the stick,
where `log save` beside it had worked. Now three names are written as not read,
with the reason: `/dev/console`, and `/dev/audio` and `/dev/blocks`, which speak
protocols of their own.

And as written: `make test` whole - x86-64 166, eight more than before
(`usb_flush_refused` 3, four more in `usb_home` and one in `usb_home_late`),
a file read off a stick's Kosmos partition 6, what a stick is sent and answers
56, the UEFI boots 38, the stick check 12, the disk 33 across two boots, the
machine with no display on both boards with all 113 programs in `/bin`, the
argument audit 112, and the suites 160 of 160 and 156 of 156.

## 18.71 Two endpoints on one wait

**What it found.** On the ThinkPad `diskbench usb 0` read the stick's blocks
at 2.1 MB/s and 17 IOPS, 58 ms a request. Under QEMU, before anything was
changed, the same stick model gave the same: 2.1 MB/s, 17 IOPS, 58.8 ms a
request - while `diskbench /home`, through the disk server's write endpoint,
read 220.5 MB/s and 938 IOPS, 1.1 ms. A stick that answers `/home` in a
millisecond does not take fifty-eight to answer its own blocks: the USB
driver's `SYS_IRQ_WAIT_ANY` watched only the write endpoint, and a request on
`/dev/blocks` waited for the wait's 50 ms deadline (`usb.md` §7).

**The fix**: the wait takes a second endpoint, as the syscall's fifth
argument; a caller on the first answers `IRQ_WAIT_CALLER` and on the second
`IRQ_WAIT_CALLER + 1`; the two endpoints' locks are taken in the order of where
the endpoints are, and one named twice is refused; a thread has a watch slot
for each, so one that dies watching both is taken off both. The xHCI driver
waits on its write endpoint and `/dev/blocks` together.

**Under QEMU, the same two boots before and after** (`diskbench ... 1 1`):

| | sequential read | random 4 KB read |
|---|---|---|
| `/dev/blocks`, before | 2.1 MB/s | 17 IOPS, 58.8 ms a request |
| `/dev/blocks`, after | 759.4 MB/s | 9765 IOPS, 0.1 ms |
| `/home` on the same stick model, before | 220.5 MB/s | 938 IOPS, 1.1 ms |
| `/home`, after | 218.8 MB/s | 928 IOPS, 1.1 ms |

QEMU's numbers, and what they say is that the wait is gone - not how fast a
stick is. `/home` is now the slower of the two by a long way, which is the disk
server, kfs and the filesystem's messages, and that is the next thing to
measure on the ThinkPad.

**Three checks.**

- **The guest suite, on both boards**: `irq: a wait on two endpoints takes a
  caller on either`. A call on the second endpoint ten ticks into a two-second
  wait ends it early with `IRQ_WAIT_CALLER + 1`, and one on the first with
  `IRQ_WAIT_CALLER`; one endpoint named twice is refused; and neither is left
  watched, so another thread's watch of each is not refused - 161 of 161 on
  AArch64 and 157 of 157 on x86-64. The eleven calls the suite already made
  moved to the new signature.
- **`run_x86.py`'s `usb_diskbench`**: random reads on the stick's blocks at
  200 IOPS or more, where the wait's deadline allows 20 - 6 checks.
- **The syscall audit**: `SYS_IRQ_WAIT_ANY` reads `arg[4]`, and
  `kosmos_irq_wait_any` passes five - 112 checks.

| Control | What failed |
|---|---|
| C8: the driver leaves `/dev/blocks` off its wait | `usb_diskbench`: random reads at 18 IOPS |
| C9: the wait looks at the first endpoint's caller only, and watches only it | the AArch64 suite, 1 of 161: the two-endpoint test |
| C10: one endpoint named twice answered as a deadline, not refused | the AArch64 suite, 1 of 161: the two-endpoint test |
| C11: the wait never takes itself off its endpoints | the AArch64 suite, 2 of 161: this test and `a wait on lines and an endpoint takes a caller too`, each finding another thread's watch refused |

And as written: `make test` whole - x86-64 167, one more than before
(`usb_diskbench`'s floor of 200 IOPS), the syscall audit 112, the UEFI boots
38, the stick check 12, the disk 33 across two boots, the machine with no
display on both boards with all 113 programs in `/bin`, and the suites 161 of
161 and 157 of 157.

## 18.72 Music says why its list is empty, and the stick starts the desktop

**What the ThinkPad showed.** Opened from Tracker - "opened
Green-Day-Basket-Case.mp3 in music" - Music said "(nothing to play in /home)"
beside a Tracker window listing the MP3 in `/home`. Music lists its folder
with `fs.list` and keeps the names that end in `.mp3` or `.wav`, and it threw
away why that came to nothing: a list that failed and a folder with no music
in it drew the same line and logged nothing.

**Not reproduced.** Under OVMF, the ThinkPad's own stick image
(`0.10.65-development`) listed `/home` whole in a program started at the
prompt and in one started by `wm` - 20 names, then 23 after `diskbench` and
`diagnose` - and Music, started by `wm` and launched through `/app/wm` as
Tracker launches it, with an Intel HDA device and without one, listed the
song and selected it every time. So what is checked is that the next time it
happens it says why.

**The check**, in `run_media.py`: a machine of its own, because the shell is
inside `wm` until it ends, runs `wm music:/home/nowhere/song.mp3`, and Music
has to log `music: could not list /home/nowhere:` with a reason - the line
`diagnose` will carry off the ThinkPad. And the existing window check still
finds no `music:` line on a folder that lists - 9 checks.

**The stick starts the desktop by itself.** Diego had been typing `wm` at the
prompt on every boot. `USB_BOOT ?= wm` puts `opt/kosmos/boot=wm` on the stick's
command line, which the shell starts as though it were typed. Checked under
OVMF on a stick built that way: the command line the loader reads says
`opt/kosmos/boot=wm opt/kosmos/home=...`, the shell says `starting wm`, and the
desktop, the Deskbar and the four programs started at login arrive with nothing
typed. `make test` builds its own sticks with `mkusb_image.py` and so boots
them to a prompt as before.

| Control | What failed |
|---|---|
| C12: Music keeps why it could not list a folder to itself | `run_media.py`, 1 of 9: the log said `wm: window Music` and nothing about `/home/nowhere` |

And as written: `make test` whole - the media engine 9, x86-64 167, the UEFI
boots 38, the stick check 12, the disk 33 across two boots, the machine with no
display on both boards with all 113 programs in `/bin`, and the suites 161 of
161 and 157 of 157.

## 18.73 A codec given time, and a codec made to be late

**What the ThinkPad showed.** Its codec did not answer its root node on
`9af841c`'s boot, answered on `895aa3f`'s - `sound: Intel HDA`, where Music
could have played - and was gone again on `ce21147`'s, where Music said "this
machine has no sound device". Nothing in `hal/pc/hda.c` changed across the
three. **The driver gave the codec half a million reads**, to announce itself
in `STATESTS` after the reset and to answer each verb, and a Tiger Lake
finishes those in a few milliseconds.

**The fix**: milliseconds on channel 2 of the 8253, which
`pc_timer_wait_ms` polls without an interrupt - sound comes up at stage seven,
before the timer, which is why it counted reads. A millisecond after reset,
then up to 200 ms for a codec to announce itself and 500 ms for an answer,
each after a thousand quick reads so a codec that answers at once costs
nothing, and no long wait again once a verb has gone unanswered. And the boot
log says how long it took:

```
-> the codec announced itself 1 ms after reset, and answered its root node in 0 ms
-> sound: Intel HDA, 44100 Hz stereo, 256-frame periods (5 ms), 4 deep
```

**A codec made to be late.** QEMU's codec answers at once, so
`opt/kosmos/hdaslow=150` holds back what it announces and answers until 150 ms
after the reset. `run_x86.py`'s `sound_slow_codec` boots with it and needs
sound up and the codec said to have announced itself at least 150 ms in:

```
-> the codec announced itself 150 ms after reset, and answered its root node in 0 ms
```

`sound`, the tone played through the HDA device and heard back, is unchanged
and passes.

| Control | What failed |
|---|---|
| C13: both waits a millisecond, as a count of reads is on a fast processor | `sound_slow_codec`: `no codec announced itself on the link in 1 ms after the controller left reset`, then `no sound: an HDA controller with no codec on the link` |

**What it cannot show** is that the ThinkPad's codec was late rather than
unreachable. The next boot's log says which: a number of milliseconds and
sound, or the same failure after the full wait - and then the other account,
a controller in the mode Intel's DSP firmware drives, is the one left.

And as written: `make test` whole - x86-64 168, one more than before
(`sound_slow_codec`), the media engine 9, the UEFI boots 38, the stick check
12, the disk 33 across two boots, the machine with no display on both boards
with all 113 programs in `/bin`, and the suites 161 of 161 and 157 of 157.

## 18.74 The Processes window without an idle row

**What the ThinkPad showed.** On an idle desktop the Processes window's top
row read `idle 99%` in red, and a person reads that as a process eating the
machine. Diego, 15 September: "its confusing as it looks like there is a
process consuming most of the cpu all the time". The row was the window's own,
made from the kernel's idle ticks beside the kernel's row.

**The change.** The shares move out of `procs.lua` into `/lib/procshare.lua`,
a function the window calls every sample: each process's ticks since the last
sample over every tick that passed, idle ones included - what `sysmon` divides
by - and the kernel's row, busy ticks less every tick charged to a process.
No idle row. An idle machine reads near nothing everywhere, and Monitor still
draws what is idle.

**The check, on this Mac**: `tools/test_procshare.lua` holds the function to
numbers - the first look all nought, a hundred ticks later 30% for the process
that ran 30 of them and 10% for the kernel, exactly three rows for two
processes and the kernel, a process started between looks at nought rather
than its whole life, and a kernel row that never goes below nought - 11 checks.
And the window itself opened on AArch64 in the display harness with no error.

| Control | What failed |
|---|---|
| C14: an idle row put back among the rows | `test_procshare.lua`: four rows where there should be three, and a row that is the machine's idle time |

**A slip worth recording.** The first run of C14 was applied while an x86
build was starting in the background, and the file is a build input; it was
restored by checksum within a second, and the next build regenerated what it
needed. Controls run only with nothing building since. And the first check
that the window opened was an x86 boot, whose machine `run_x86.py` gives no
screen unless a check asks for `ramfb` - so the window manager never started,
which looked like a window that never appeared.

And as written: `make test` whole - the Processes window's shares 11, x86-64
168, the media engine 9, the UEFI boots 38, the stick check 12, the disk 33
across two boots, the machine with no display on both boards with all 113
programs in `/bin`, and the suites 161 of 161 and 157 of 157.

## 18.75 Where the sound goes, said when it plays

**What the ThinkPad showed.** With the codec given milliseconds
(0.10.68-development), Music played Basket Case - `44100 Hz stereo 16-bit
MP3`, its position moving and its meter green - and Diego heard nothing. So
the codec answers and samples reach it, and the sound goes to a pin that is not
the speaker, or to the speaker with something left off. The driver chooses the
first output pin that routes, in the codec's order, and printed what the codec
is made of only when nothing routed - so a machine that played and was silent
said nothing about where.

**The change.** On success too, the boot log names the converter and the pin
the codec plays through, and then every pin: whether it is an output, its raw
pin capabilities and its configuration default, which is where "internal
speaker" and "headphone jack" are written (spec section 7.3.3.31). Printed
raw rather than named, as the failure dump already was. Under QEMU:

```
-> the codec plays converter 0x02 through pin 0x03
->   codec node 0x03 is a pin, output, pin caps 0x00000010, config 0x00004010, from 0x02
```

**The check**: `run_x86.py`'s `sound` needs both lines, beside the tone it
already plays and hears - 11 checks with `sound_slow_codec`.

| Control | What failed |
|---|---|
| C15: the route line reworded, as a driver that no longer says it would | `sound`, 1 of 10: `the codec plays through 0x02 through pin 0x03` is not the converter and pin said |

And as written: `make test` whole - x86-64 169, one more than before (`sound`
saying its route), the media engine 9, the Processes window's shares 11, the
UEFI boots 38, and the suites 161 of 161 and 157 of 157.

## 18.76 The amplifier switched on, and the jack beside the speaker

**What the ThinkPad said.** 0.10.69-development's boot log, through
`make stick-log`: the codec plays converter 0x02 through pin 0x14, whose
configuration default `0x90170110` is a fixed internal speaker - the right pin.
Music played into it and nothing was heard, and headphones were silent as well.
Three things in the driver, each read against the HDA specification (Rev. 1.0a,
downloaded for this) rather than from memory:

- **EAPD was never set.** The speaker's and the jack's pin capabilities,
  `0x00010014` and `0x0001001c`, both have bit 16, *EAPD Capable* (§7.3.4.9).
  EAPD is bit 1 of verb 70Ch (§7.3.3.16, Table 93), the power of the amplifier
  the pin feeds - 0 is that amplifier in D3 and 1 is it in D0 - and nothing
  wrote it.
- **The headphone jack was never enabled.** The driver enabled the first pin
  that routed and stopped; pin 0x21, `0x0421101f`, is a jack and routes from
  converter 0x02 as well.
- **"Not connected" was the wrong value.** The driver skipped a pin whose
  connectivity bits read 3. Table 109 says 01b is no physical connection and
  11b is a jack and an internal device both. The ThinkPad's unused pins read
  `0x411111f0`, 01b, and were not skipped - harmless there only because the
  speaker came first.

**The change.** `drive_pin` enables a pin's output, and its headphone amplifier
where it has one, unmutes it, and on a pin that controls EAPD sets bit 1,
keeping BTL and the swap as the codec had them; then it reads back the pin
control (F07h) and EAPD/BTL (F0Ch) and says them. The first route is driven,
then every other connected output pin that routes from the same converter -
speaker and jack together, until something reads the jack's presence.
`CONFIG_PORT_NONE` is 1. Under QEMU:

```
-> the codec drives pin 0x03: control 0x00000040
```

**The check.** QEMU's codec has no EAPD pin, so `opt/kosmos/hdaeapd=1` has the
driver take its output pin for one. **And QEMU's codec keeps no EAPD/BTL
register** - it answers 0 to F0Ch whatever was written - so a read-back could
not tell a driver that sets EAPD from one that does not. `run_x86.py`'s
`sound_eapd` boots the codec with `debug=1` instead, which names every verb it
does not handle, and needs 70Ch to arrive carrying bit 1:

```
hda-output: hda_audio_command: not handled: nid 3 (out), verb 0x70c, payload 0x2
```

and sound up, with the pin's line saying what EAPD/BTL reads back. `sound`
needs the pin's control read back as well: 14 checks with `sound_slow_codec`.

| Control | What failed |
|---|---|
| C16: the 70Ch write taken out, as a driver that finds the EAPD pin and never sets it | `sound_eapd`, 1 of 2: `70Ch never arrived at the codec` |

**What QEMU cannot say**: whether an amplifier comes up, and whether a second
output pin plays - QEMU's codec has one output pin, so the loop over the others
runs first on the ThinkPad. Its boot log names each pin driven and what the
codec kept.

**And the ThinkPad answered**: Diego heard Basket Case through its speaker on
0.10.70-development the same day - "it works great".

And as written: `make test` whole - x86-64 172, three more than before (`sound`
reading its pin back, `sound_eapd`'s write and its line), the media engine 9,
the Processes window's shares 11, the UEFI boots 38, and the suites 161 of 161
and 157 of 157.

## 18.77 An MP3's length from its Xing header, and the sound held to its decoder

**What the ThinkPad showed.** With sound working (0.10.70), Music said `MP3 64
kbps` and `9:58` for Basket Case, and Diego heard it as low quality: "64kbps
and sounds low quality. can we play 196kbps mp3?". Two questions, answered
separately.

**Is anything lowering the quality?** No, measured. Twenty seconds of Basket
Case played through `media.lua` under QEMU, written out by QEMU's WAV writer,
and compared with the same MP3 decoded on the Mac by the same vendored minimp3
(`hostdecode.c` in the session's scratchpad, the kit's own definitions):

```
frames with sound: 835965 (18.96 s)
identical to the Mac's decode: 835930 (99.9958%)
different: 35                  each one step of 65536, in one channel
silent gaps after the first sound: none
```

The 35 are most likely rounding in the decoder's floating point - an ARM build
here, an x86 one on the Mac - and are not heard. The file really is 197 kbps on
average (`afinfo`: 197002 bits per second), and what reaches the device is its
decode. The conversion to the device's rate is a copy at 44100 Hz, the mixer
is unity at full gain, and the ThinkPad's amplifiers are written at 0 dB. So
what sounded thin is the laptop's speaker.

**Why Music said 64.** The file is a 137-byte ID3 tag, then **a Xing header
frame** - 64 kbps, 208 bytes, holding no music but `Xing`, flags `0x0f`, 7441
frames, 4786800 bytes, a seek table and the encoder's name - then 7441 frames
at 32 to 320 kbps. `mp3.probe` took the first frame that decodes, which is the
header frame, for the file: 64 kbps, and a length of the file's bytes over 64
kbps, `9:58`.

**The change.** The kit reads a Xing or Info header where a Layer III frame's
side information ends - 32 bytes after the header for MPEG-1 in stereo, 17 in
mono, 17 and 9 for MPEG-2 and 2.5, two more with a CRC, which is what minimp3's
`L3_read_side_info` reads - and when it counts the frames, `probe` returns the
length exactly (frames times 1152 samples over the rate), the average bitrate
from the bytes, `vbr`, and an offset past the header frame. `media.lua` takes
that length; Music says `MP3 197 kbps VBR`. On the Mac, the reader drafted for
it gave Basket Case 194.377 s and 197.0 kbps, which is what `afinfo` says.

**The check.** Diego's song is not the repository's, so `run_media.py` makes a
VBR MP3 of its own - a Xing header frame, then 400 silent frames of 128 and
192 kbps alternating - which minimp3 on the Mac decodes as 401 frames of 1152
samples. `media.open` has to say 10.449 s, 160 kbps and VBR, where the header
frame taken for the music is 64 kbps and 26 s. 10 checks.

| Control | What failed |
|---|---|
| C17: `vbr_header` returning false, as a kit that does not read the header | `run_media.py`, 1 of 10: `media: vbr 26.101 s 64 kbps nil` - the header frame read as the music, as Music said for Basket Case |

And as written: `make test` whole - the media engine 10, one more than before
(the VBR MP3), audio tags 23, x86-64 172, the Processes window's shares 11, the
UEFI boots 38, and the suites 161 of 161 and 157 of 157.

## 18.78 A picture drawn at a size that is not its own

**Why there is one now.** `gfx.md` 19.9 listed scaling among the things
deliberately left out - "they get added when a case appears, not before" - and
the case appeared: Music's design draws an album cover at 78 pixels beside
what is playing and at 44 in each row of its list, and a cover inside an MP3
is five hundred pixels or more. Without a scaler Music would draw the corner
of a cover twice. Three other places had been waiting in comments for the
same thing: the photo viewer, which can only pan, and the image widget every
application inherits, and icons fixed at one size because any other would be
a crop.

**It is C because it touches every pixel it writes** - `gfx.md` 19.2's rule,
and the one measurement that settles it is already in the tree: a PDF scanner
that took 538 ms in Lua and 4.7 ms in C for identical output.

**The primitive.** `dst:stretch(src, sx, sy, sw, sh, dx, dy, dw, dh
[, alpha])`, nearest neighbour, the step held in 16.16 fixed point so a
five-hundred-pixel cover costs an add per pixel rather than a division.
`alpha` is `blend`'s: absent it copies, given it composites.

**The one thing that is not like `blit`.** `clip()` moves a paired source
origin one for one, and that is exactly wrong when the rectangles are
different sizes - a destination edge cut by a window's border maps back to a
fraction of a source pixel. So the destination is clipped and each source
position is computed from where the pixel landed, with a position outside the
source clamped to its edge rather than refused.

**The checks** (`luatest.lua` role 48, `tests.c`): doubling, where each source
pixel has to fill a 2x2 block and nothing outside the rectangle may be
touched; halving, where the surviving pixel has to be the nearest one; a
rectangle clipped on the left, which must draw the source's right-hand part -
the check that fails if the clip moves the source origin; a ten-pixel-wide
surface, whose rows are padded to a cache line, so a scaler doing its own row
arithmetic reads the wrong row from row one; alpha, half of white over black;
empty and off-surface rectangles, which draw nothing rather than raising; and
an alpha of 300, which is refused.

| Control | What failed |
|---|---|
| C18: the step fixed at one source pixel per destination pixel, as a scaler that ignores the size it was asked for | `run_tests.py`, 1 of 162: `not ok 153 - gfx: a picture drawn at another size` |

And as written: `make test` whole - the guest suites 162 of 162 and 158 of
158, each one more than before, and every other line as it was.

## 18.79 A heading larger than the desktop's text

**What this retires.** `ui.md` said a window drawing through commands "cannot
have a heading at 28 pixels and a paragraph at 16 on the same screen": a text
command carried a role - one of four faces, each at a size chosen in
Appearance - and nothing else. Music's design wants a large title, and Diego
chose on 15 September ("yes to all") **a size the kit carries** over a window
drawing its own pixels, because what comes after Music is other applications
restyled and a size in the kit gives every one of them large text for nothing.

**What crosses is a size, never a face number.** `gfx`'s `role_of` does take a
number, so an index would have resolved in the *compositor's* process - where
that slot was never loaded - and `l_text` would have fallen through to the
8x16 bitmap with nothing raised anywhere. Silently wrong is worse than
refused, so `{ op = "text", ..., role = "ui", px = 28 }` carries the size and
each side resolves it against its own pool of faces.

**When the pool is full.** `gfx.face` keeps eight slots beyond the four roles
and answers `nil, "no room for another face"` rather than evicting one - so a
window asking for more sizes than the machine will hold draws those at the
role's own size, said once in the log, instead of losing its headings to the
bitmap font.

**Measuring happens in the face that draws**, which is the way this goes
wrong quietly: a widget laid out on the widget font's cell and drawn at 28
pixels runs off its own edge. The kit asks for the same face in its own
process - every application links `gfx` - and clips against that.

**Two things already wrong, corrected with it**: the kit applied three of the
desktop's four roles, so an application asking for `title` measured against a
face it had never loaded; and `gfx.md` was headed "Three faces, not one" while
four exist.

**The check** (`run_screenshot.py`, the `text size` phase): a window draws one
line at the role's size and one asked for at 28, and each **block of
consecutive inked rows** is measured off the screenshot - 15 rows against 10.
**A scalable face is chosen first**, because the default face for every role
is the bitmap, which exists at one size: asking it for 28 can only fall back,
and both lines would come out identical - a check that measures nothing and
passes.

**The first version of this check did not bite, and that is the part worth
recording.** It counted inked *rows in two bands*, above and below. The window
has a few rows of chrome near its bottom edge, inside the lower band, so with
C19 applied - every line drawn at its role's size - the lower band still
counted 13 against the upper band's 10 and **the control passed**. A test that
passes whether or not the feature is there is worth nothing, and the only
reason this was caught is that the control is run rather than assumed. What
found the cause was printing rather than asserting: one boot that dumped the
ink profile of the window, which showed the two lines at rows 30-39 and
103-117 and the chrome at 137, 142 and 147. Blocks of consecutive rows cannot
be padded that way; specks under three rows are ignored.

**And three runs were lost to a phase that did not put the machine back.** It
left its window open, so a later phase that finds the Deskbar's buttons by
position read the wrong pixels; then it left a 20-pixel face loaded, and that
phase computes a button's place from `len("Kosmos") * GLYPH_W`, the bitmap's
8-pixel cell, so every button had moved; then it wrote an empty settings file
and took the dark palette with it, and the same phase read the right pixels in
the wrong palette - three greens out. **A phase hands back the screen, the
face and the palette**, and the order matters: anything typed while a window
manager holds the screen does not run until it lets go.

| Control | What failed |
|---|---|
| C19: the compositor resolving no size, so every line comes out at its role's | the `text size` phase, 1 of 112: `the line asked for at 28 pixels is 10 rows tall and the line at the role's size is 10 ... The blocks found: [(30, 10), (102, 10)]` |

And as written: the display harness 112 checks, two more than before, and
`make test` whole as it was.

## 18.80 A cover out of an MP3, drawn without a file

**What this is for.** Music's design shows the album cover twice, at 78 pixels
beside what is playing and at 44 in each row of the list. A cover lives inside
the file: `tags.lua` finds it and returns `{ mime, offset, bytes }` rather than
the bytes, deliberately, so that listing a library of a thousand songs does not
decode a thousand pictures to show ten.

**The plan in `roadmap.md` could not work, and reading said so before anything
was built.** It proposed copying those bytes to `/ramfs` and naming that path,
since `ui.image` names a picture and the compositor finds it. But a `/ramfs`
value is capped at 16384 bytes and `read_into` is not served by the ram proto
at all, while a cover is hundreds of kilobytes.

**So the picture travels as pages.** A request carries the name, the mime type
and the length, with the region holding the bytes: **control by message, data
by shared memory**, which is the system's own rule rather than an exception
made here. It needed no decoder work - `gfx.png` and `gfx.jpeg` already take
an address and a length, and `picture_from_file` already maps a region and
decodes out of it - and it keeps working where a copy through a file would
not, since `/home` is not always a disk.

Three things it has to get right, and each is a bug that would look like
something else:

- **The capability is let go on every path out**, not only the happy one. A
  thread gets sixteen, and a server that keeps them answers sixteen requests
  and refuses every one after - which is what a PDF read in small windows
  found on its fifteenth read (`init.lua`).
- **The name carries the track.** The cache is keyed by name, so one fixed
  name would hand every song the first song's picture - and it would read as a
  caching bug in Music rather than a naming mistake here.
- **One cache and one eviction rule.** `remember_picture` is shared with the
  file path, so a handed-over picture is held and freed exactly as one read
  off the disk is: four at a time. Two queues would be two answers to how many
  decoded photographs the one process the desktop depends on may hold.

`media.cover(path)` is the other half: the bytes into a region with one read
from where the tag says the picture begins - `read_into` puts what it read at
the region's start rather than at the offset it was given, so a short read is
reported rather than stitched - then the region handed over, released, and the
name returned.

**The check** (`run_media.py`): an MP3 built here whose ID3v2.3 tag carries a
**real** 64x64 PNG of one colour, opened through `media.cover`, drawn with
`ui.image`, and that colour counted on the screen - more than 2000 of the 4096
pixels it was drawn into. The tag is built as `test_tags.lua` builds one; the
picture is a real PNG rather than the stand-in bytes those tests use, because
this one has to decode. It was decoded on this Mac first, against the same
vendored stb the compositor uses, before any of it reached a guest: 64x64,
first pixel `20 c0 40`.

| Control | What failed |
|---|---|
| C20: the picture decoded and then forgotten, so the reply says it worked and the cache holds nothing | `run_media.py`, 1 of 11: `the cover's own colour covers 0 of the 4096 pixels it was drawn into`, with the program still reporting `cover:/home/cover.mp3 64x64` |

And as written: the media suite 11 checks, one more than before.
\n
## 18.81 A window that asks for its own size

**Most of this already existed.** `handlers.resize` has been in the window
manager since 2 September and refuses only a window that draws its own pixels,
whose two buffers are the application's own region and cannot be grown from
the compositor's side. **Nothing in the userland ever asked**: the only
matches for a resize in `user/` were the kit's own layout walk and the event
it receives. So Music's mini player - that window with its list folded away -
had no way to fold, and the roadmap's "nothing lets an application ask" was
wrong about where the gap was.

**And the half that was missing came with a bug attached.** The `resize` event
lays the view tree out again and *left the window's own `w` and `h` as they
were*, so after a drag on the grip a window's own numbers were the ones it
opened with. Nothing caught it because `window.width` reads the root view
rather than those fields. `window:resize` takes its size from the reply for
the same reason, rather than waiting for the event that follows.

**The check** (`run_screenshot.py`, the `window resize` phase, 3 checks): a
window opens at 300x200 drawing one block of colour over its whole area, a
click asks for 160x90, and then - the reply says `true 160x90`; the kit's own
fields say 160x90; and **the block of colour on screen is narrower and
shorter than it was**. The third is the one that matters: the compositor
throws the old surface away and allocates a new one, so a window whose
*numbers* changed and whose surface did not would answer perfectly and still
be the old size in front of you.

| Control | What failed |
|---|---|
| C21: the method reporting success, updating its own numbers, and never sending the request | the `window resize` phase, 1 of 115: `the window's colour still covers 300 columns and 200 rows, where it covered 300 and 200 before: the reply said 160x90 but the surface on screen did not change` |

**The first version of that control was not worth having**, and it is the
same lesson as §18.79 one step further. It left the method reading a field of
a reply that no longer existed, so the click handler raised and the phase
failed with "the window never said what its resize answered" - evidence that
the program stopped printing, not that the window failed to resize. **A
control has to fail for the reason the check exists**, or all it proves is
that the code was disturbed.

And as written: the display harness 115 checks, three more than before.

## 18.82 A picture drawn at the size the command asks for

**The primitive existed and nothing could reach it.** §18.78 put a scaler in
`gfx` for Music's covers, and then Music could not use it: an application draws
through commands, and the `image` command carried *which part of the picture*
and never *how big to draw it*. A cover inside an MP3 is five hundred pixels
and the design draws it at 78 and at 44, so what a window got was the corner of
a sleeve, twice. Found while planning the window rather than while building it,
which is the cheap moment to find it.

**So the command carries a drawn size.** `dw` and `dh` beside the source
rectangle; the compositor calls `stretch` when they are there and `blit` or
`blend` when they are not, which keeps a photograph and an icon paying exactly
what they did before. In the kit it is `ui.image{ fit = true }`: the whole
picture at the widget's size, where the widget's own behaviour - pan a
photograph that is bigger than its box - stays the default, because that is
right for Photo and wrong for a sleeve.

**The check** (`run_media.py`): the cover embedded in the test's MP3 is now
**four quarters of four colours**, and it is drawn into a box half its size.
Two things are then true only if the drawn size reached the scaler: the first
quarter's colour covers about a quarter of the box, and **all four colours are
in the box at once**. A crop shows one.

**Both halves of that were needed, and the first version had neither.** The
cover was one colour drawn into a box its own size, so the check passed whether
or not anything scaled - the same fault as §18.79's first check, and caught
here by asking what the control would have to break rather than by the control
itself.

**And then the threshold was stale.** With four quarters the first colour is
256 of the 1024 pixels, exactly a quarter, and the check still demanded 900
from when the picture was one colour - so the first run of the corrected test
failed at the number that proves it right. A number written for one picture is
not a number for another.

| Control | What failed |
|---|---|
| C22: the compositor ignoring the drawn size, so a fitted picture is a crop of its corner again | `run_media.py`, 2 of 12: `the cover's first quarter covers 1024 of the 1024 pixels the picture was drawn into, where a quarter is about 256`, and `the cover drawn at half its size shows 1 of its four colours`. A crop of the corner fills the box exactly, which is why the count alone could not have caught it and the four colours could |

And as written: the media suite 12 checks, one more than before.
\n
## 18.83 A triangle, drawn by a window that does not own its pixels

**The primitive was there and out of reach**, which is the second time in one
day: `gfx.c` has had `triangle` since it was written, and an application
drawing through commands has `fill`, `text` and `image` and nothing else - the
shape verbs are surface methods, reachable only by a window that draws its own
pixels. Music is not one, so its play arrow would have been a staircase of
thin fills, visibly stepped at the 18 pixels the design draws it at.

**Diego chose the command** (16 September) over the two alternatives put to
him - rectangles only, or seven generated pictures - because the restyle after
Music wants the same shape for menus, sliders and disclosure arrows, and the
primitive is already written.

So `{ op = "triangle", x1, y1, x2, y2, x3, y3, color }`, clipped in the kit by
the view's rectangle as every other verb is and clipped again by the
compositor against the window. `gfx` has taken doubles for these since it was
written, so a half pixel is expressible and an arrow's point lands where it
was asked for.

**The check** (`run_screenshot.py`, the `triangle` phase, 2 checks): a window
fills a dark square and draws a triangle over its lower-left half. A point well
inside the shape is the ink colour, **and the opposite corner - the one the
hypotenuse cuts off - is not**. The second is the whole of it: a check that
only asked whether the ink appeared would pass on a filled rectangle.

| Control | What failed |
|---|---|
| C23: the compositor filling the triangle's bounding box, which is what a rectangle-only implementation would have had to settle for | the `triangle` phase, 1 of 117: `the corner beyond the triangle's long edge is (240, 144, 32) as well, so what was drawn is the bounding box and not a triangle` |

**The control is deliberately not "remove the drawing".** Taking the command
out would fail the first check and prove very little; drawing the bounding box
is the mistake that would really have been made, and only the second check
catches it.

And as written: the display harness 117 checks, two more than before.
\n
## 18.84 Music's window, and what a screenshot cannot tell you

**The design built.** `docs/music.html`, approved on 14 September and the
pilot of a second look for the whole system: what is playing across the top
with its cover, the chips saying what the file is, a title larger than the
text under it, a seek bar, the transport in one row, the sources, the library
and the foot - in its own flat palette rather than the desktop's. `ui.md`
§16.8b still says Kosmos is dimensional on purpose, and stands until Diego has
used this.

**Six pieces had to exist first** (§18.78 to §18.83), and two of them existed
only because building this window asked how it would actually draw: the scaler
and the triangle were both in `gfx.c` and unreachable from an application.

**The check** (`run_media.py`, three of its fifteen): the cover square shows
**four colours**, not one - the test's song carries a picture of four quarters,
so a crop or a failure to fetch shows a single colour; the title's rows of ink
are **taller** than the artist line above it; and the play arrow's base is a
different colour from the space past its point, which is what tells a triangle
from a rectangle.

| Control | What failed |
|---|---|
| C24: Music never fetching the cover, so the square shows the file's icon on a panel | `run_media.py`, 1 of 15: `the cover in Music's window shows 1 of its four colours ([(56, 56, 60)])` - the panel grey, which is what a window with no picture draws |

**And the bug the checks caught, which no screenshot would have.** Pressing
play loaded the track, started it, and left the window waking once a second -
so it handed the audio server twelve periods a second where it drains far
more. Sound started and starved: a third of a second, then nothing. The window
before this one set its pacing from its Play button; this one called `load`
and stopped. **A picture of a window says nothing about whether it feeds a
deadline**, which is the whole argument for a check that listens.

**Four faults the screenshots did find**, and they were only found by looking:
the cover square drew the image widget's "no picture" apology, because the
window had never handed it a name - which is why a window that paints its own
layout now has `gc:picture` instead; the meter drew as a row of dashes at a
peak of zero; shuffle and queue were unreadable at 18 pixels; and every
right-aligned string lost its tail.

**That last one took three wrong guesses and then a measurement**, which is
the lesson worth keeping. It was not the font used for measuring (that was
half of it), and not the window's width (the client area really is 380), and
not the published `width` property (which reads `nil` as a field, and quietly
left the layout wrong). **`gc:text` clips by whole character cells**, a cell
being the width of "0" in the drawing face, so a string given exactly its
measured width of room loses its last characters. A probe that printed the
numbers from inside a running window settled in two minutes what three
plausible theories could not.

And as written: the media suite 15 checks, three more than before.
\n
## 18.85 Three faults Diego found in ten minutes

He opened Music under QEMU on 16 September and reported two things; the
screenshot he sent showed a third he had not mentioned.

**"MP3 64 kbps" and 9:55, for a song of 3:14 at 197.** The fault §18.77 was
supposed to have fixed, in a file that has an ID3 tag - which is every file
anybody owns. `mp3dec_decode_frame` reports `frame_bytes` as *everything it
consumed*, the bytes skipped to find the frame included, and `frame_offset` as
how many of those were skipped. The probe subtracted only `frame_bytes`, so on
a file with a 137-byte tag it looked for the Xing header inside the tag, found
nothing, and fell back to the header frame's own 64 kbps.

**The test could not have caught it**, and that is the part worth keeping: the
VBR file `run_media.py` generates had no ID3 tag, which is the one shape a real
file never comes in. It carries one now.

| Control | What failed |
|---|---|
| C25: the frame taken from where the decoder was asked to start rather than where the frame began | `run_media.py`, 1 of 16: `a variable-bitrate MP3 ... its header frame read as the music is 64 kbps and 26 s` - Diego's symptom exactly, on the generated file |

**"Resizing does not work."** Music placed its five views once, at the size
the window opened with, and nothing moved them - so a window dragged bigger
kept the design at its old size with the rest showing `0xff202020`, the grey a
freshly allocated surface is filled with. It lays out again now: the player,
the transport and the sources keep their heights, the library takes the room
that appears, and every right-hand edge follows the width.

The check here is the *symptom* rather than the mechanism - no pixel of
Music's window may be that grey - because a window whose numbers changed and
whose drawing did not is exactly the fault. The relayout itself belongs in the
display harness, where a window can be told to grow.

**"When dragging its all flickery."** And, decisively: *the other windows are
not*. Every frame begins by clearing the window and draws over it, and the
compositor holds the damage back until the last batch, so a repaint is never
seen half-drawn. **A drag is the exception**: it damages the window on every
step of the pointer and composites whatever is in the surface at that instant.
Only Music repaints continuously - to move a clock and a meter - so only Music
was caught in the gap between the clear and the contents.

Two changes, and the first is the real one: **a window whose views cover every
pixel can say `background = false`** and the clear is not sent, so the worst a
mid-drag composite can catch is a window with some parts updated rather than an
empty one. And Music repaints ten times a second instead of twenty-five, which
is more than a person can see on a clock and a fifth of the exposure.

And as written: the media suite 16 checks, one more than before.

**The resize claim got its evidence afterwards**, and it took four goes - all
of them this file's scaffolding rather than the window. A phase that left its
own window open, a stale mark that made a wait look already satisfied, a
duplicated teardown that typed into a shell which had already come back, and
twice a patch written against text that had not been printed first. The
feature was never in doubt; the proof was.

What it proves now (`run_screenshot.py`, the `window resize` phase, four
checks): a window asks to shrink and the reply, the kit's own fields and the
drawing all agree - and then **a second window asks to grow, and none of the
room it gains may be `0xff202020`**, the grey a freshly allocated surface is
filled with. That grey is exactly what Diego saw.

| Control | What failed |
|---|---|
| C26: an application that ignores its resize event, laying out for the size it opened with | the `window resize` phase, 1 of 118: `3562 places in the grown window are the grey a new surface is filled with` |

**And the mini player, which is what the resize was built for**: Music's
seventh transport control folds the window to 330x150 and folds it back. The
library, the sources and the foot are given no height when folded, so the same
window is two windows - which is how `docs/music.html` draws it.

## 18.86 A device that keeps its own time

**Diego, 16 September: "there is a issue with the music player as in qemu is
playing at 2x speed or more, it sounds like a chipmunk! in the thinkpad is
ok", and "the progress bar is not real time, instead is advancing like 2 o 3
seconds per real second".** The media suite was green at 16 checks while this
was true, and had been green over it for as long as the suite existed.

**The suite could not have caught it, and the reason is the interesting
part.** Every check in `run_media.py` listens to QEMU's WAV writer, and the
writer is paced by QEMU's own timer: it waits for the guest. Both ends are
therefore on one clock, and "the sound agrees with the machine that made it"
is a tautology however fast either of them is going. A real device is not
like that - it drains on its own clock and the guest keeps up or does not.

**What the measurement said**, from a three-clock probe printing host time,
the guest's counter and `p:position()` together:

| backend | position / real time |
|---|---|
| `wav` | 1.00 |
| `none` | 1.00 |
| `none`, forced 48000 | 1.00 |
| `none`, forced 44100 | 1.00 |
| `coreaudio` | **2.09** |
| `coreaudio`, forced 44100 | **2.08** |

A six-second tone played out in 2.36 seconds. Host time and guest time agreed
with each other to a few milliseconds in every one of the six runs; only the
count of frames the *device* reported having played doubled. Forcing the
backend to 44100 changed nothing, so it is not a rate conversion - it is
QEMU 11.1.1's CoreAudio backend retiring buffers at about twice real time, on
the same install where `make fast`'s hvf is already recorded broken. There is
nothing to fix in Kosmos, which is why the ThinkPad is correct.

**Five mechanisms were proposed and the measurements killed all five**: a
rate disagreement between `HAL_SND_RATE` and the driver, stereo data consumed
as mono, the previous night's MP3 `frame_offset` change, TCG running behind
real time, and a 44100-into-48000 conversion. Each was plausible from the
symptom and none survived contact with a number. Three complete runs were
also thrown away by a regex anchored on `$`, which serial output's carriage
return can never match - the same never-matching-matcher fault as §18.85's
`on_key`, made twice more in one day.

What it proves now (`run_media.py`, the last phase, two checks): the suite
boots a second guest against `-audiodev none`, which keeps its own time and
makes no sound, plays the tone, and holds the position to real time and to
the guest's own clock, both within 0.85 to 1.15.

| Control | What failed |
|---|---|
| C27: the same phase pointed at `coreaudio`, the backend Diego heard | 2 of 18: `4.91 s of sound came out in 2.25 s of real time - 2.18x`, and `the position disagreed with the guest's own clock - 2.17x` |

**The gap was never a missing assertion. It was a missing device**, and
`run_screenshot.use_audiodev` exists so any harness can ask for one.

## 18.87 A stride, and two faults in the proof of it

**The feature is small and the lesson is not.** `/drives` answered a listing
with 104-byte `drives_volume` records while the namespace decoded 80-byte
`drives_entry` records. Only the first volume was ever right - a name is the
first 64 bytes of both structs - and the second was read 24 bytes into the
middle of the first record, the third 48, the fourth 72.

**The first proof could not have found it, in two independent ways.**

The fixture had *one* volume, which is exactly the case where a stride error
is invisible: with one record there is no second offset to get wrong. And the
check was `"BACKUP" in volumes`, a substring test. Decoded offline, two
records at a stride of 80 give `PHOTOS` and then an **empty string** - the
second read lands in the middle of the first record's sizes, which trim to
nothing - and an empty name is invisible to a substring test. So even with
two volumes the old check would have passed.

It was reported green at 7 of 7, and marked DONE in the roadmap, before any
of that was known. The bug was found by reading the code while planning 6c,
not by testing.

**Then the control could not run, twice, and said nothing about it.** The
mutation reverted the call site and left `answer_volume_entries` defined and
unused - a hard error under `-Werror`. The build failed, `make` kept the
previous good binary, and the phase booted the *fixed* image. Both runs
reported "10 passed, 0 failed" and both were meaningless. Timestamps could
not tell that apart, because the control's own cleanup rebuild touched the
same files.

**What settled it was making the mutated build say who it was.** A `say()`
line printed at startup, checked in the guest's own output: `marker seen: NO`
is what proved the control had never run. The third attempt kept every symbol
referenced - the faulty body calls through to `answer_volumes` - compiled,
booted, and printed its marker.

What it proves now (`run_x86.py`, the `usb_drives` phase, ten checks): a
stick with two volumes - `PHOTOS` (FAT32, one sector a cluster) and `BACKUP`
(FAT16, four) - listed as an exact set, a short name and a long name, a file
one directory down, a 3000-byte file that is a chain of six clusters, and
FAT16's fixed root directory read. `tools/fatstick.py` builds it with mtools,
and `fatls` and `mdir` read it identically.

| Control | What failed |
|---|---|
| C31: the cluster chain is never followed | 1 of 10: the 3000-byte file read back -1 bytes |
| C32: a path stops at its first component | 2 of 10: `hello.txt` and the chain |
| C33: the listing answers with volume records | 1 of 10: `/drives listed ['PHOTOS'] where both volumes should be there, exactly` |

**Three rules out of it.** A fixture with one of something cannot test how
that something is counted. A substring test passes on an empty string, so
assert the set. And a control that does not say which binary it is, is not a
control - it is a rebuild you are hoping happened.


## 18.88 The check that condemned a working stick

**A stick was built for Diego, booted under OVMF, and failed four checks -
and the stick was fine.** `run_uefi.py` reported 4 of 31 on
`kosmos-usb-0.10.75-development.img`: only 0.0% of the screen was Kosmos's
ground colour, "the picture is still the firmware's", no boot-log green, no
wordmark. That is the exact shape of the fault Diego has been handed twice
before - a stick that stops after the loader - so nothing was handed over.

**The control is what saved it.** `kosmos-usb-0.10.70-stable.img` - the build
Diego used on the ThinkPad, played Basket Case through, and called stable -
fails the identical four. A known-good artifact failing a check means the
check is wrong, and that is the whole reason a control is run before a
diagnosis rather than after one.

**But a failing control only says the check is unsound; it does not say the
stick is sound.** Those are different claims and the second is the one that
decides whether somebody is handed a USB stick. So two positive measurements
were taken:

- **the serial line**, which the four screen checks cannot speak for: shell,
  `wm`, the desktop, the Deskbar, and windows for Tracker, Monitor, Log and
  Processes, all up at 1280x800, with `kfs, 8 of 32 MB free`. Identical on
  0.10.75 and on the stable build;
- **a colour census of the frame** rather than one guessed constant: 2347
  distinct colours, 28.3% BeOS desktop blue, 19.3% panel grey. A firmware
  screen is a logo on a flat ground and counts in the dozens.

**The cause.** All three colour checks name the *kernel's boot screen* -
`GROUND` is `0x0d1117`, `GREEN` the boot log's headings, `RED` the wordmark.
Every stick handed over is built with `USB_BOOT ?= wm`, so the desktop starts
by itself and paints over that screen long before the capture at 30 seconds.
The harness's own images pass no arguments at all and sit at the prompt with
the boot screen still displayed, which is what the checks were written
against. **No MEGA stick had ever been through this harness**, so the day
sticks began starting the desktop, the check silently inverted: it now failed
precisely the sticks that worked.

**The fix reads the artifact instead of being told.** `mkusb_image.py` writes
the kernel's arguments to `\boot\kosmos.cmdline` on the ESP, and the ESP
begins at the GPT's first usable sector - 34, which is 17408 bytes in - so
`boot_args()` reads it back with mtools. A stick that says
`opt/kosmos/boot=` is judged on whether a desktop is drawn, counted in
distinct colours rather than named in one constant, because the desktop's
ground is a colour the user picks and `theme.lua` says so - the running stick
showed BeOS blue where the default is `dark`. With the fix: **PASS, 29
checks.**

**And the branch that already worked still does**, which is the half a fix
like this usually forgets. `run_uefi.py` on the harness's own three images -
`kosmos-uefi.img`, `kosmos-refusal.img` and `kosmos-uefi-home.img`, exactly
as the Makefile invokes it - gives **PASS, 38 checks**, the refusal and the
`/home` partition among them. Those images carry no `kosmos.cmdline` at all,
so `boot_args()` returns "" and they take the old path unchanged: the fix
adds a branch rather than moving the existing one.

**The control, watched failing, and it took three tries to be worth
anything.** The first version was `not (drawn(blank) >= DRAWN_ENOUGH)` -
the line above it rewritten, restating its premise and watching nothing fail.
The second failed for a reason that had nothing to do with the branch: it
reused a temporary image the earlier cases delete, so `main()` answered
`SKIP` before it looked at a screen. The third drives `main()` twice, with a
desktop stick showing a blank screen and then a drawn one. Broken on purpose
with `DRAWN_ENOUGH = 0`, the suite says: *a desktop stick showing a blank
screen drew no complaint, so the branch is a rubber stamp*. Restored, 9 of 9.

**And a measurement fault of my own worth recording**, because it is the same
class as the `$`-anchored regex in 18.87. The first verification was run as
`python3 tools/run_uefi.py ... | tail -25`, and the exit code of a pipeline is
the *last* command's. `tail` succeeded, the harness had exited 1, and the run
was reported as passing. A pipe discards the one signal a test exists to
give.


## 18.89 A volume remembered by what it is

**Tracker's shortcut places have to find a volume again after its drive has
been unplugged**, and the plan was to key them on the unit and the partition.
Reading `xhci.c` before building on that found `units_named++` - "the next
never given out" - so the same stick in the same port comes back as a new
unit. The plan would have broken on the first replug, which is the one case
it existed for. `drivesproto.h` had called the pair "the stable handle".

So `/drives` now reports each volume's own identity (`usb.md` 6c): a FAT
volume's serial, `BS_VolID`, or a GPT partition's unique GUID, as text that
says which - `fat:1A2B-3C4D`, `gpt:BA231D95-...`.

**Three checks, one at each layer, and each watched failing for the reason it
exists:**

| Layer | The check | Broken on purpose | What it said |
| ----- | --------- | ----------------- | ------------ |
| the FAT decoder | `test_fatdecode`, 78 (3 new): the serial at 39 on FAT16 and 67 on FAT32, and none without `BS_BootSig` 0x29 | the serial read from byte 40 | 1 of 78: *FAT16's serial is BS_VolID, at byte 39* |
| the partition decoder | `test_drivesdecode`, 53 (2 new): a GPT entry's GUID from bytes 16 to 31, sixteen distinct bytes so a shifted read cannot match; an MBR partition has none | the GUID read from byte 17 | 1 of 53: *a GPT partition carries its UniquePartitionGUID* |
| the server, the protocol and the namespace | `run_x86.py`'s `/drives` phase, 11 (1 new): every volume's id, as the whole set | the server's reply without `id_kind` | 1 of 11: *got {'PHOTOS': 'nil', 'BACKUP': 'nil'}* - and the other ten still passed, so the check is the plumbing and nothing else |

**The host checks alone would not have been enough, and `usb.md` already
said why**: a test that builds its own boot sector "cannot catch a field read
at the wrong offset, since the test would write it at the same wrong offset".
Mine writes the serial at 39 and reads it at 39. What makes 39 *right* is
mtools, somebody else's reading of the format: `fatstick.py` now stamps
`-N 1A2B3C4D` and `-N 0BADCAFE`, a serial nobody chose would only ever be
checkable for being *some* serial, and the guest answered exactly those.
`fatstick.IDS` is the one place both the stamp's expectation and the check
read from.

**The stride became a name on the way.** The namespace stepped through a page
of volume records by a bare `104`, and this change moves the record to 120.
It is `DRIVES_VOLUME_BYTES` now, asserted at load against the packed format,
and `drivesproto.h` holds the C side with a `_Static_assert` - both facts were
comments before. A stride that disagrees with its record is 18.87.

## 18.90 A place: made by a drop, opened by a click, taken out by a right-click

**Shortcut places are two halves and each has its own test.** The rule - what
a place remembers and how it is found again - is `/lib/places.lua`, checked on
the Mac with no machine booted. The wiring - a drop on the sidebar, the name
box, a click, a right-click - is Tracker, and runs only in a guest.

**`tools/test_places.lua`, 17 checks, in `make test`.** A folder on a drive
remembers the volume's identity and the path inside it; a folder elsewhere its
path; a volume with nothing to know it by is refused rather than remembered by
its name. Then the part that is the whole point: after a replug under a new
unit and a new name, `PHOTOS 2`, it finds its own volume - and while the real
stick is away and a *different* stick called `PHOTOS` is plugged in, it says
*unplugged*.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| `resolve` keyed on the volume's name rather than its identity | 2 of 17: the replugged stick and the other stick called PHOTOS both resolved to `/drives/PHOTOS/Italy` - somebody else's drive, opened without a word |

**The display harness's `places` phase, 3 checks, on both boards.** Tracker is
opened on a folder holding one folder; its row is dragged onto the sidebar,
the offered name taken with Enter, the new place clicked and then
right-clicked. **Checked on the files wherever a file can say it**: afterwards
the place is in the Trash with `kind = "place"` and the path it pointed at,
which proves the drop and the name box wrote it and the right-click moved
rather than destroyed it - and it is no longer in `/home/Places`. The click is
checked on the list, which held one row before and none after. The harness is
diskless, so `/home` starts empty every boot and nothing can pass on a place an
earlier run left.

| Broken on purpose | What the phase said |
| ----------------- | ------------------- |
| the drop handler swallows every drop | *did not become a place - nothing in /home/Places and nothing in the Trash*: `placecheck false nil nil` |
| the right-click says it worked and moves nothing | *a right-click on a place did not take it out of Places*: `placecheck true nil nil` |
| a click on a sidebar row goes nowhere | *clicking the place did not open it*: 268 pixels of a row in the list's first row before the click, and 268 after |

Each fails on its own sentence and no other, because the phase checks in the
order the paths depend on each other: nothing made means nothing to click or
remove, so that is said first and alone.

**Two faults of my own in writing it, both the same class as 18.87's.** The
first run failed a Tracker it had never heard from: `placecheck (\S+) ...`
matched the *echo* of the command that asks, `placecheck " .. tostring(p`,
before the answer was printed. `run_x86.py` prints `"drives" .. ": ids"` for
exactly this reason, and the marker is now joined by Lua the same way. It is
the `$`-anchored regex of three days earlier in another form: a check reading
the wrong line and reporting it as the machine's answer.

**And a fault the phase exists to catch next time, found this time by
reading.** The drop handler sits near line 800 and called `focus_on`, which was
`local function focus_on` near line 2000. Lua binds a name when it compiles, so
to the handler it was a global that does not exist, and the first drop on
Places would have stopped Tracker with "attempt to call a nil value". Nothing
parses that; only running it does. `focus_on` is forward-declared beside
`show` and `visit`, which are there for the same reason.

## 18.91 The Open window: a path that says which of three things happened

**The display harness's `panel` phase, 3 checks, on both boards.** A small
program opens the Open window on a folder holding a folder, `a.txt` and
`b.sfc`, with a filter that keeps `.sfc`, and prints whatever it is handed.
Folders come first and then names, so the list's second row is `a.txt`
without the filter and `b.sfc` with it - and so **the path the application
is handed is the filter's evidence**, not a picture of a list. One click on
that row must hand over nothing, since the old panel chose a file the moment
it was clicked; a second must hand over `/home/picktest/b.sfc`, whole.

**Two seconds between the lone click and the pair**, and the reason is the
one 16.8c measured: a second click is anything within a second of the
counter, and under emulation the counter runs ahead of the guest. A shorter
pause would make the lone click and the pair's first into a double, and the
check that one click chooses nothing would pass without proving it.

| Broken on purpose | What the phase said |
| ----------------- | ------------------- |
| the filter ignored, every file shown | *the Open window's filter did not hide a.txt*: `picked /home/picktest/a.txt` |
| one click chooses, as the old panel did | *one click on a file in the Open window chose it - one click selects, and a second click or Enter opens* |
| a second click does nothing | *a second click on a file in the Open window handed the application nothing* |

The second was run twice, and the first run's reason was lost: three controls
wrote into one file and only the last survived, so all that was known was
that it had failed. A control is worth nothing until its reason is read, so it
was run again alone and read.

**Written against two mistakes already made today**, so neither was made
again: the marker the program prints is joined by Lua, `'pick' .. 'ed '`, so
the echo of the line that writes the program cannot be read as its answer
(18.90); and the program is started as `wm /ramfs/pick.lua`, a path alone.
`wm` starts every comma-separated entry as a program, so the form the
triangle and resize phases use - `wm tri,/ramfs/tri.lua` - tries
`/bin/tri.lua` first and says it could not, harmlessly, every run.

**What was checked by eye, once, and is not a test**: the window photographed
on a folder of seven entries, with folders first, sizes as `3.0 KB`, kinds as
`jpg` and `folder`, and a long name cut to fit its column with `...` rather
than running under the size - never in the middle of a UTF-8 character. And
Tracker's trail, moved into the kit as `ui.trail`, clicked on `home` from
`/home/Desktop`, went to `/home`.

## 18.92 A key the keyboard driver does not know, named once

**`hal/pc/i8042.c` dropped every key behind an 0xe0 prefix that its table did
not know, and said nothing.** That is where the ThinkPad's volume and
brightness keys went, if they arrive as keys at all - and a key that vanished
in silence cannot be told from one that never came. So each is named in the
kernel's log the first time it goes down, with its byte, and never again,
because a held key repeats and `diagnose` carries the log off the stick. It
is still dropped; the loss is visible, and nothing above changes.

**Measured through QEMU's PS/2 keyboard rather than recalled**, with the
driver's own new line reporting what arrived:

```
e0 21   calculator      (pressed twice: named once)
e0 20   mute
e0 2e   volume down
e0 30   volume up       (pressed twice: named once)
```

and Home, which the driver knows, named not at all. The volume keys' bytes
are the standard ones; whether the ThinkPad's embedded controller sends the
same is the stick's to say.

**The display harness's `unknown keys` phase, 3 checks, x86 only** - the ARM
board's keyboard is virtio. **The calculator key, because nothing will ever
map it**: the volume keys are about to have entries, and a test built on them
would stop testing this the day they did.

| Broken on purpose | What the phase said |
| ----------------- | ------------------- |
| named on every press, the bitmap ignored | *the calculator key, pressed twice, was named 2 times* |
| dropped in silence, as it always was | *a key the keyboard driver has no entry for was dropped in silence* |

**The second control was run twice, and the first run proved nothing.** It
removed the call to `unknown_extended`, which left that function unused, and
`-Werror` refused to compile it - so `make` kept the kernel from the control
before, and the phase tested *that*, reporting the first control's failure
word for word. `build exit=2` was printed beside it and nearly read past.
This is 6b's C33 again, the control that does not compile and silently tests
the last binary that did. The control was rewritten to compile - the line
removed, the bookkeeping kept - and the chain that runs controls now refuses
to run a phase against a build that failed.

## 18.93 The volume keys, from the keyboard to the mix - and muted is silent

**Three layers, and each is proved where it can be seen.** The keyboard driver
maps e0 20, e0 2e and e0 30 - the bytes 18.92 measured - to evdev's mute,
volume down and volume up, which travel as raw events and never as characters
(`hal_key_char` answers -1 above the typing block). The window manager takes
them before any window, as it takes Super: a key that changes the machine's
volume is not a game's to swallow. And the audio server has a master mute of
its own - a field on both sides of `audioproto.h` - because muting by setting
the level to zero would make one stored value mean two things, and move the
Mixer's master slider to nought while muted.

**The display harness's `volume keys` phase, 2 checks, on both boards**: on
x86 through the i8042's table, on the ARM board through virtio's own codes.
Each of up, down and mute must reach the window manager - one pattern per
key, because a shared one let the mute line stand in for a volume-up that
never arrived - and on x86 none may still be a key the driver does not know.
The harness has no sound device, so this is the key path and only that.

**`run_media.py`, 4 more checks, 22 in all: whether the machine goes quiet**,
measured in what it played rather than read from a reply. A second of the
tone while muted adds nothing to the recording; a second after unmuting is
heard; and the level is the same before, during and after, so unmuting comes
back to it. Two programs, so the recording is measured between them rather
than across a pause somebody would have to time.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| the audio server ignores master mute | `run_media.py`, 1 of 22: *a second of the tone while muted was heard for 1.02 s - muted has to be silent* |
| the window manager does not take the volume keys | the phase, on the ARM board: *the volume key 'up' never reached the window manager* |
| the i8042's table without the volume entries | the phase, on x86: the same - the keys dropped again before the window manager could see them |

Every control was built and its build's exit checked before the phase ran,
since 18.92's second control tested a kernel that had not compiled.

## 18.94 The level bar: at the level the keys set, every press heard, and gone

**The Sound half of `docs/levels.html`, held to what the window manager says
the level is** rather than merely found on the screen. `run_media.py`'s own
session, because it needs a sound device to have a level at all: a window
well away from the corner, the corner photographed, volume down three times,
and the corner photographed again - then three and a half seconds later, a
third time.

**4 checks, 26 in all**:

- the corner changed, and the window manager said what it set;
- **all three presses were heard** - which is the bug this section exists
  for: the first version called `gfx.surface(w, h)`, the kit takes a table,
  and the error raised inside the key handler took the window manager's keys
  down with it, so the bar never drew and only the first press arrived;
- along the track's middle row the white of the fill runs from its start to
  the knob, which sits where `wm.lua`'s arithmetic puts the level the key
  said - within three pixels;
- the corner is exactly as it was before the key, three and a half seconds
  after the last one: faded, and nothing left behind.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| the original: `gfx.surface(osd.W, osd.H)`, and no `pcall` around the drawing | 3 of 26 on the bar: *drew no level bar*, *the window manager said 1 of them ['240'] - drawing the level bar is losing keys*, and *the fill is 0 pixels of white* |
| the fill drawn full whatever the level | 1 of 26: *the level bar's fill is 176 pixels of white, where 208 of 256 puts its knob at 151* |
| two seconds' hold made ten | 1 of 26: *the level bar was still on the screen three and a half seconds after the last key* |

**The first control found a fourth failure that was not the bar's.** The
master-mute check (18.93) read *'mute: muted true level 256 2'* - the line
caught while QEMU was still printing it, since `wait_for` returns on the
first words and the numbers after them may not have arrived. It waited for
a phrase and then parsed a line; it now waits for the line.

## 18.95 The firmware's AML, off the machine byte for byte

**The ThinkPad's brightness is set where its DSDT says, and the DSDT is
AML** - a bytecode Kosmos does not run. So the machine hands the bytes over
and a person reads them: the kernel keeps where the DSDT and SSDTs are while
it walks ACPI at boot, following the FADT to the DSDT, which is the one table
the XSDT does not list; maps them once the MMU is on; and `SYS_FIRMWARE`
copies them out a page at a time. `acpi save` writes one file a table to
`/home/acpi`, and `make stick-log FILE=/home/acpi/` brings the folder to the
Mac.

**`run_x86.py`'s `firmware` session, 6 checks, with a table whose every byte
is known.** `-acpitable` hands the machine an SSDT the test makes - `Name
(KOSM, 0x2026)`, a string, and a buffer of six thousand bytes, 6088 in all,
so it crosses a page between reads - beside QEMU's own DSDT. The boot log
names the DSDT and its size; `acpi` lists both tables whole; `acpi save` says
it saved them; and off the disk, through `kfs.lua getdir` as `make
stick-log` takes them, the DSDT is whole - signature, the length it states,
the file's size and the boot log's all agreeing, summing to zero - and the
test's table is exactly the bytes QEMU was given.

**Once by hand, not on every run: `iasl -d` on what came off the disk.**
QEMU's DSDT, 8462 bytes, decompiled to 3203 lines of ASL - `Device (PCI0)`,
`Device (RTC)` and the rest - which is the whole path the ThinkPad's will
take. `make test` does not need ACPICA and does not use it; `iasl` also read
the test's SSDT back as the three names it was built from.

**`test_sticklog.py`, 7 checks**: a folder of two binary files, every byte
value in them, comes off a stick's copy whole through `getdir`. **And
`run_shell.py` on the ARM board**: `acpi` says there are no tables, since
`virt` is a device tree rather than ACPI.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| the FADT not followed | the session, 3 of 6: *firmware tables: no DSDT, and 1 SSDT*, no DSDT listed, and *the DSDT off the disk is not whole: 0 bytes* |
| `SYS_FIRMWARE` copying from the start of the table whatever the offset | 4 of 6: both tables listed as *DOES NOT SUM TO ZERO*, the DSDT off the disk *summing to 247*, the test's table not listed whole |
| `getdir` taking only the first file | `test_sticklog.py`, 1 of 7: *the folder taken from the copy is not the folder put on the stick: ['DSDT.aml'], of ['DSDT.aml', 'SSDT1.aml']* |
| `acpi` on a board with none printing *acpi: nothing* | `run_shell.py`: *acpi on a board with no ACPI did not say there were no tables* |

**The `getdir` control was run twice.** The first broke it by testing the
loop's index, which the line inside the loop had already shadowed with
something that is not a number - so `getdir` raised, took nothing, and the
check failed for a reason that was not the one being tested. It failed; it
did not bite. The second counts, takes exactly one file, and is the row above.

## 18.96 The backlight, read before anything writes it

**The ThinkPad's brightness is the Intel display engine's backlight PWM**
(`thinkpad.md` 8b), and the offsets of its registers are in no public Intel
manual - they come from Linux. So the first driver for it only reads: the
board finds Intel's graphics at 0/2/0, by vendor, class and a 64-bit BAR,
and says where the backlight block is; `user/servers/backlight.c` maps that
one page and reports both controllers - on or off, the share of each period
the output is driven, and the raw control, period and on-time. A controller
on, with its on-time inside its period, is what confirms the offsets on the
machine, and only then does anything write.

**`test_backlightdecode`, 13 host checks**, because QEMU has no Intel
graphics and the ThinkPad gives one reading: a device that does not answer,
off, on in either polarity from dark to full, rounded down, a period of
nought and an on-time past its period refused, and periods near 2^32 with
the arithmetic in 64 bits.

**`run_x86.py`, 1 more check, 2b**: q35 has an Intel network card,
8086:10d3, at 00:02.0 - exactly where Intel's graphics would be - and the
driver has to say there is nothing to read rather than take it for one.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| all ones believed | the host test, 1 of 13: *all ones was not read as a device that does not answer* |
| the percentage in 32 bits | 2 of 13: *a period near 2^32 overflowed the arithmetic*, and the all-ones period not read as full |
| the finder checking vendor only, not class or BAR type | 2b: *backlight: Intel graphics at 00:02.0 ... process 12 (backlight) ended, code -1* |
| `init` never starting the driver | 2b: *nothing from the backlight driver at all* |

**Two of those taught something.** The 32-bit control was run twice: the
first time it printed the *all-ones* complaint, because Apple's `make` keeps
times to the second and the restore, the next edit and the last build fell
in one - so it tested the previous control's binary. It failed; it did not
bite. Forced, it failed on the overflow. **And the fooled finder's driver
died** reading the page it had mapped, with a report that said "not
present" of a mapped page. The report ignored bit 3 of the error code, and
now prints it - *a reserved bit set in an entry* - with the four entries the
walk met: `pte 0x800a0000fe0c801f`, a physical address the finder had built
from the network card's *next* BAR, above what the processor can address.
The real finder refuses that by the BAR's type. The kernel mapping it at all
is on the roadmap.

## 18.97 The tests in five minutes: every suite side by side

**Diego, 18 September 2026**, after a gate that ran for forty minutes: "i
dont want 40 minutes tests any more, 5 to 10 minutes max from now on so make
sure the tests are built accordingly". **The same checks, in less time -
never fewer to make the number** (`CLAUDE.md`).

**Where the forty went was not the checks.** Every suite ran after the one
before it, on one of the Mac's ten cores, although each is its own QEMU with
its own disk, its own sockets and a random port - sharing nothing. `make
test` is now `tools/gate.py`, and `make host-check` and `make gate-images`
are its halves:

1. every image the suites boot is built first, each variant with `-j`;
2. every suite - the host checks included - runs side by side, six at once,
   the longest first by what each took last time (`build/gate/times.json`),
   each writing its own log in `build/gate/`;
3. the two that were most of it run in parts: `run_x86.py`, some thirty
   machines one after another, as four groups of its parts (`--parts`), and
   the display harness, some forty phases in one machine a board, as four
   parts a board, cut where the measured phase times come to about a
   hundred seconds and in the harness's own order (`--phases`).

| Run | Took | Slowest |
| --- | ---- | ------- |
| the old `make test` and `make screenshot`, one after another | about 40 minutes | - |
| every suite side by side, the display harness whole | 18:28 | the two display harnesses, 403 s and 402 s, one after the other |
| the same with the parts | **4:37** | `x86-usb-2` 121 s, the display parts 83 s to 120 s |

**The same checks, counted rather than assumed.** `run_x86.py`'s four groups
said 42, 39, 45 and 64 - its 190 whole. The display parts each check the boot
screen and the bars again, thirty checks, so the ARM board's 65, 49, 51 and
51 are 126 and x86's 65, 50, 51 and 51 are 127 - both the numbers they were.

**None needed a quiet machine.** The first version ran sound and the display
harness alone at the end, because they have checks about timing - sound in
real time, scheduling latency, the idle desktop, the compositor's budget.
All of them passed with six machines running, so they share like everything
else; a suite marked `alone` still would not, and a run one fails in because
of the others is the evidence for marking it.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| a phase asked for by a name no phase has | `run_screenshot.py`: *FAIL: no phase ran by these names: no such phase* - a typo in the gate's table cannot run nothing and pass |
| a suite that fails, inside the gate | *FAIL: 1 of 2 suites in 22 s - x86-bin*, exit 1 |

**What is still there to take.** The display harness's fixed sleeps, about 98
seconds a board, were not needed for this and remain; `x86-usb-2` is the
longest part and can be split. Five minutes is not a floor, and a suite that
pushes the whole past ten is the thing to fix before anything else lands.

## 18.98 Every outline face loads and draws - Space Grotesk among them

**Space Grotesk, five weights**, added on 18 September at Diego's asking:
Light, Regular, Medium, SemiBold and Bold, the static files of the Google
Fonts download byte for byte, under the SIL Open Font License beside them as
`LICENSE.SpaceGrotesk`. The build embeds every `.ttf` in `assets/fonts/`, so
nothing else had to change for Appearance to offer them - which is also why
nothing had ever checked that a face it offers can be drawn.

**The display harness's `faces` phase, 2 checks, on both boards**: a program
lists `gfx.fonts()`, loads each outline face at 24 pixels, measures a word
with it and draws it into a surface of its own; every face has to load,
measure wider than nothing and light more than fifty pixels, and all five
weights of Space Grotesk have to be among them. Nineteen faces. **Run once
per eight**, because a process holds eight outline faces beside its four
roles (`FACES_MAX` in `gfx.c`) and never lets one go - asking one process
for all nineteen would fail at the ninth for a reason that is not a font's.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| forty kilobytes of noise in `assets/fonts/` as `Broken-Regular.ttf` | *the faces program died loading a face after atkinsonhyperlegible - data abort from a lower EL. A face in assets/fonts is not a font stb_truetype can read* |

**The control found two things beyond the phase.** A broken font is not
refused - stb_truetype believes the offsets inside the file, and the noise
sent the program reading an address nothing maps, so the phase names a
death as well as a refusal. **And removing the control's font did not
remove it from the image**: make rebuilds a table when a file it depends on
is newer, and a file that is gone is newer than nothing, so the next "real"
run died on the same font. Every font or icon ever taken out of `assets/`
had stayed in the image until something else touched its table. The fonts
and assets tables now depend on a stamp holding their list of files,
rewritten when the list changes - the flags stamps' trick, for the same kind
of question - and the real run passed on both boards.

## 18.99 The embedded controller, watched before anything answers it

**F5 and F6 on the ThinkPad are embedded-controller events** (`thinkpad.md`
8b), and who hears them depends on something Kosmos has never touched: until
an operating system switches the machine to ACPI mode, the firmware's SMM
services the controller's events itself. So `hal/pc/ec.c` only reads - what
the FADT says about events, whether SCI_EN is set, whether a controller
answers at 66h - and on core 0's tick logs every change in the controller's
status and in GPE0's status bits, the first sixty-four. Nothing is written
to any port. The FADT and ECDT offsets come from `iasl -T` templates
compiled and disassembled.

**`run_x86.py`'s check 2c**: q35 says SCI 9, SMI command port B2h, SCI_EN
clear - SeaBIOS leaves ACPI mode to the OS - and nothing answers at 66h.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| the FADT's SMI command port read two bytes late | 2c: *ec: the FADT: SCI 9, SMI command port 0x00* |

**And the gate learned something while it ran.** The x86 suites, run to
check the timer tick the watcher sits on, failed once in x86's HDA sessions
- *audiolag: UNDERRUNS 1, worst write 33216 us* - with five other machines
running; alone they passed twice. So those three sessions are a suite of
their own that runs on a quiet machine at the end (`x86-sound`, 9 seconds),
which is the evidence `gate.py` said to wait for before marking one.

## 18.100 The desktop's wallpapers, carried in the image and on the screen

**Twenty-four photographs from Unsplash**, at Diego's asking on 18
September: "can we also add /Users/diego/Downloads/kosmos-wallpapers to
kosmos", "convert them to jpg at 1920x1080 as much as possible so they save
space before adding them to the image". Scaled to cover 1920x1080 and saved
at quality 80 by `tools/wallpapers.sh` - 9.2 MB, where the downloads were
65 - in `assets/wallpapers/` with the Unsplash License and a README crediting
each photographer (`README.md` there has why they are committed made rather
than made by the build, which is a departure from how vendored data is kept).
In `FULL=1` images only, in a table of their own named `wallpaper/<file>`;
`sys.asset` looks in both, Appearance lists them by photographer after the
pictures in `/home`, and the window manager's `picture_from` takes either a
path or a name in the image.

**The image ran into the heap.** With them a `FULL=1` userland image passed
sixteen megabytes, where the heap began, and the link refused it - the
linker script's own assert. The heap and the stack moved up: the image may
be 32 MB, the heap starts there and the stack's top is at 46, below the
screen at 48 and the mappings at 64 (`kernel/process.h`,
`user/include/kosmos.h`, `user/user.ld`, the three places that must agree).
Its read-only half is shared, so the room is address space, not memory.

**The first move put the stack's top at 48, which is where the screen is
mapped** - `USER_SCREEN_VA`, missed because the search for the old numbers
looked for sixteen and thirty-two - and the x86 kernel suite failed "el0:
code shared, writable not" every time. Not for that, it turned out: every
page-table claim in the test was true, and its two processes exited two
yields after it stopped waiting. It waited two hundred yields, which on an
idle core are microseconds, for processes that run on other cores; the new
layout made start-up just slow enough to lose that race on x86 every time.
It waits by the clock now, five seconds as a cap, the way the trace test
above it does. The stack came down to 46 all the same, because a stack
whose guard page is the framebuffer is its own bug.

**The display harness's `wallpapers` phase, 3 checks, on both boards**: 24
wallpapers carried; the first decodes at 1920x1080; and named in
`/home/.appearance`, the desktop starts with it, and the screen at three
points is exactly the decoded pixels. The first run chose the middle of the
screen as a point and read the pointer's outline there, which is where it
starts - the picture showed the photograph under the desktop's icons and a
black arrow.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| the window manager's branch that takes a picture from the image | *the desktop started and the screen never showed wallpaper/alexander-slattery-LI748t0BK8w.jpg at the three points its decode gave* |

## 18.101 A menu bar above a window that draws its own pixels

**The Super Nintendo has a File menu - Open ROM... and Quit** - which Diego
asked for on 14 September and chose the shape of on the 18th: the window
manager draws the strip above a direct window's pixels, as the kit draws
`ui.menubar` - the same gradient, groove and title spacing - and the
application's buffer is the area below it (`strips` in `wm.lua`). A press
on a title is a `menubar` event with where the menu should open; the kit
opens an ordinary menu window owned by the window, so the menu is the same
as every other one (`window:direct_event`). The pointer and a commit's
damage stay in the buffer's own coordinates. Open ROM... is the Open
window at `/home/roms/snes`, showing `.sfc` and `.smc`, and starts a fresh
Super Nintendo on the choice at the same scale.

**The display harness's `direct menu` phase, 3 checks, on both boards**, with
a program of its own because the harness has no ROM and never will: a
direct window with File (Say hello, Quit) and blue pixels. The window is
120 rows and a strip taller, the blue starts under a strip that is not blue,
File then Say hello reaches the program's `on_choose`, and File then Quit
ends it.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| the press on the strip not handled | *a press on File in the strip did not reach the program as a menubar event* |

**And the build's own checker found the first placement wrong**: the
composing code that calls `strips.compose` is earlier in `wm.lua` than the
table was, so there it read a global that does not exist - which would
have failed the first time such a window was drawn. `tools/luaglobals.py`
refused the build, and the table moved above its first user.

## 18.102 ACPI mode, the power button, and S5

**The machine is switched to ACPI mode at boot and its events become keys**
(`thinkpad.md` 8c): the power button as `KEY_POWER`, and the ThinkPad's
F5 and F6 - embedded-controller queries 14h and 15h - as brightness keys the
window manager answers through `/dev/backlight`. Powering off writes the
DSDT's `\_S5` sleep type to the FADT's PM1a control, where it wrote QEMU's.

**`run_x86.py`'s check 2c** now holds q35 to the whole of it: the FADT
facts, *switched to ACPI mode - 0x02 to port 0xb2*, the power button taken,
nothing at 66h, and *S5 is sleep type 0 ... at 0x0604*.

**`run_x86.py`'s `power_button` part, 6 checks, in `x86-core`**: the desktop
boots, no press is heard before one is made, then QEMU's `system_powerdown`
sets PWRBTN_STS - which QEMU does only with PWRBTN_EN set, as `ec.c` sets it
- and the kernel must hear it, the window manager must say *the power button
- shutting down*, and QEMU must exit, because S5 was entered.

**`test_s5decode`, 13 checks on the host**: the T14's shape (BytePrefix 7)
and q35's (ZeroOp), a root-named `\_S5_`, a two-byte PkgLength, a
WordPrefix, a string and an `_S4_` that are not it; and refused - Ones, 8, a
name reference, an empty package, one cut off by the table's end, none.
**Why on the host**: QEMU's sleep type is 0, which is what the board wrote
before it read one, so under QEMU a decoder that found nothing powers off
exactly like one that works.

**`test_backlightdecode`, 26 checks now**: the keys' levels on the T14's
period - 80% reads as 205, every level from 16 to 256 reads back as it was
set, 0 is raised to the floor, past 256 is the whole period, and a
controller that is off or inconsistent gets no on-time.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| `KEY_POWER` never queued | power_button: *the kernel heard the power button and the window manager never took the key*, and *the machine is still running after the power button* |

**The gate found a crash the new mount caused, in four suites at once.**
`df`, `find` and `diagnose` ask every mount, and a mount with no protocol
named is sent tables - so `/dev/backlight` was sent a `query` table, answered
BACKLIGHT_ERR_BAD_OP, and the reply's first byte, 2, unpacked as `true`:
*init:1715: attempt to index a boolean value (local 'reply')*, in
`arm-shell`, `arm-interchange`, `x86-usb-1` and `x86-disk`. `/dev/audio`
and `/dev/blocks` had been sent the same tables for months and survived only
because their BAD_OP numbers unpack as `false` (1) and a string (5), which
a caller reads as a failure. **Fixed as a class**: a C
server's mount names its protocol (`audio`, `blocks`, `backlight`), and the
namespace's `request` refuses a table to any protocol it does not route,
with a sentence - what `ns.send` already did.

**And the gate now refuses to start with an x86 part no suite runs**:
`power_button` was in `run_x86.PARTS` and in no suite until it was read for.
`uncovered_x86_parts` holds the suites to the list; with `power_button`
taken out of `x86-core` it names it.

**What QEMU cannot show**: the embedded controller. q35 has none, so the
query path and the brightness keys are proved on the ThinkPad or not at
all, which is why `ec.c` logs every query it gets, and the window manager
every level it sets.

## 18.103 The Super Nintendo's View and Game menus

**Double Size, Normal Size, Pause and Resume** (`roadmap.md` 4b). Diego, 19
September: "Do the 2x option in snes emulator app menu and it will restart
the app", and "the emulator needs a pause / play mode". View has one item,
naming the size it goes to, and starts a fresh Super Nintendo on the same ROM
at that size - a window that draws its own pixels cannot be resized, so the
game starts again. Game has Pause or Resume, whichever applies, and so does
P: a paused console runs no frames and draws "Paused" over its last picture.

**`run_media.py`'s Super Nintendo phase, 8 checks, in `arm-media`**, on a
cartridge the test makes: 32 KB, five instructions that loop for ever and a
LoROM header - ours, not game data, which the display harness has never
had. The harness is diskless and `/ramfs` holds 16 KB a value, so the phase
lives where there is a disk. Game, then Pause: the frame it paused at is
the frame it resumed at two seconds later, over a thousand pixels of the
box are on the screen, and none two seconds after Resume. P twice does the
same with frames run in between. View, then Double Size, opens a 1024 by 960
Super Nintendo and the first one ends; its View, then Normal Size, opens a
512-wide one. The window manager now says which menu bar title was pressed -
*wm: menu bar Game of kosmos-test at 164,106* - so a title missed because
the face changed is named rather than guessed.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| the loop runs frames while paused | *paused at frame 368 and resumed at frame 426, two seconds later: a paused console went on running*; and *0 pixels of the Paused box*, drawn over by the frames |
| Double Size relaunches at the same scale | *View, then Double Size, did not open a 1024 by 960 Super Nintendo: None* |

`arm-media` is 1:41 with it, from 1:09 - under the slowest suite, so the
gate's whole is unchanged.

## 18.104 The battery on the top bar

**The ThinkPad's charge, from its embedded controller to the Deskbar**
(`thinkpad.md` 8d). Diego, 19 September: "The battery indicator is a must".

**`test_batterydecode`, 10 checks on the host**: every state `GBST` in the
T14's DSDT tells apart - charging, discharging, both bits (charging wins),
full on the charger, critical - the charge rounded to the nearest percent
and held at 100, no battery, and the two readings `GBST` would not trust,
refused.

**`run_x86.py`'s `battery` part, 6 checks, in `x86-core`.** q35 has no
embedded controller, so the reading is `opt/kosmos/battery`'s - the kernel
takes `57,charging` or `8` there instead of the registers, and says at boot
that it did - and everything above the controller is what runs on the
ThinkPad. At a prompt `/dev/battery` says 57, charging, on AC; with the
desktop the Deskbar says "57% charging" with no red in the bar, and "8%"
with red. And **`power_button`, with no option, now checks the Deskbar says
nothing about a battery** - the question mark it drew before any reading
existed is gone, and a machine that reads none shows none.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| `/dev/battery` not served | *did not say 57, charging, on AC: BATTERY nil nil nil*, and neither Deskbar said anything |
| the low battery not drawn red | *8% and discharging drew 0 pixels of red in the bar* |

**A control that first proved nothing**: its edit put a `--` inside a call
in `deskbar.lua`, the build failed at `luacheck`, and the run that followed
used the binary before it and passed. Caught by the build's exit status,
which is now checked before any control run is believed.

**What QEMU cannot show**: the registers themselves, and whether the T14's
controller answers RD_EC in ACPI mode as the DSDT assumes. The kernel says
the first reading at boot - *ec: the battery: 83%, discharging, on
battery* - which is the line to read on the ThinkPad.

## 18.105 An Xbox 360 controller's buttons, as keys

**Diego's 8BitDo SN30 Pro USB is an Xbox 360 pad** (045E:028E in X-input
mode), and so is every pad that speaks X-input (`usb.md` 9). The driver
reads it on the mouse's path and hands each button to the kernel as a key
(`SYS_KEY_PUSH`, `hal_key_push`); the Super Nintendo, Doom and Quake map
evdev's gamepad codes.

- **`test_paddecode`, 27 checks on the host**: every button by itself from
  both bytes, byte 3's unused bit, the evdev codes, the sticks and triggers,
  the left stick as the D-pad past half-way with its hysteresis, the D-pad
  and the stick together as one direction, and four reports that are not
  input refused.
- **`test_usbdecode`, 3 checks more**: the 360's configuration found - its
  descriptor of type 21h not taken for HID's - and a headset's interface
  and a pad with no IN refused. **The test's own descriptor was wrong at
  first**: the headset's 27-byte descriptor written with 14 of its bytes,
  which the walk rightly called malformed.
- **The kernel suite, on both boards**: *input: a driver's key comes out,
  and is let go*.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| `hal_key_release_all` queues no release | *not ok - input: a driver's key comes out, and is let go*, on both boards |

**The real pad under QEMU on the Mac**, through `tools/usbhost.sh`: the
driver found it - *port 5: 045e:028e, USB 2.0, class 255, "Controller"* -
took it for a pad, configured its endpoint, and its SET_CONFIGURATION
stalled, because macOS's own Xbox 360 driver holds the interface. **Run as
root, by Diego, it worked**: *an Xbox 360 controller, read from endpoint 1,
up to 32 bytes; its buttons are keys*, a first report with nothing held,
and then every button he pressed, down and up - south, east, west, north,
L, R, both triggers, Select, Start, and the D-pad's four directions. The
driver says thirty-two changes and stops, so the sticks, pressed after, are
not in the log.

## 18.106 The 8253 stops in ACPI mode, and a 512 MB `/home`

**Stick 0.10.87 crawled on the ThinkPad** (`boot.md`'s table): ACPI mode on,
the battery read - *24%, discharging* - then about three minutes before the
sound card said anything, and the boot stopped at stage 10. **The 8253
stopped counting once the machine was in ACPI mode.** Every wait before the
scheduler's tick was a spin on its channel two, bounded at ten million
reads of the port - milliseconds under QEMU, ten seconds a wait on silicon -
and the sound card's setup is a run of them, the timer's calibration two
more. QEMU's 8253 never stops, so nothing here could have seen it.

**So the TSC is measured against the 8253 before the switch, and every
wait is on the TSC from then on** (`timer.c`); the timer's calibration takes
that same measurement rather than a second one against a chip that may have
stopped. After the switch the kernel asks whether channel two still counts
and says so - *acpi: the 8253 still counts*, under QEMU, checked by
`run_x86.py`'s 2c - which is the line to read on the ThinkPad's next boot.
`opt/kosmos/acpi=off` leaves a machine in the firmware's mode, one word in a
stick's `\boot\kosmos.cmdline`.

**The stick's `/home` at 512 MB**, from `~/Kosmos/home` (`roadmap.md` 4d):

- **`test_homeimage.py`, 10 checks on the host**: folders kept, a dot-file
  kept, `.DS_Store`, a `._` file and a name with a colon left out, the size
  asked for, a nested file back byte for byte.
- **`run_x86.py`'s `usb_home_large`, 4 checks, in `x86-usb-2`**: a 512 MB
  `/home` made by `homeimage.py` with a 40 MB file and then a 256 KB one -
  found in the image's bytes past 32 MB - on a stick `write_gpt` lays out,
  the kernel told its GUID. `/home` is all 1,048,576 sectors, the machine's
  `df` agrees with `kfs.lua df`, and the far file reads back.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| macOS's litter not filtered | *`/home` holds .DS_Store*, and *macOS's ._ file went in* |

## 18.107 BeOS's tab, and a bar across by choice

**A window's title is a tab as wide as what is on it, by default**, and a
bar across the whole window when Appearance says so (`ui.md` 16.8b). A
window is a tab on a body (`tabs.shape`): the compositor cuts that shape
out of what is behind, `window_at` finds what is under the pointer by it,
and the tab's paint, title and boxes stay inside it. The window manager
says each tab's width as it places a window - *wm: window Front at
350,600 500x150, a tab 124 wide* - which is how a harness finds the
minimise box at the tab's end.

**The display harness's `tabs` phase, 4 checks, both boards**: two windows
of one program, Front a third over Behind so its tab's row lies across
Behind's body. Beside the tab is Behind's blue; asked for the bar across -
by the same theme message Appearance sends - that point is Front's; asked
for the tab again, blue again; and a press there reaches Behind. Stepped by
clicks on Front, not by timers.

| Broken on purpose | What it said |
| ----------------- | ------------ |
| the pointer takes the whole row as the window's | *a press beside Front's tab did not reach Behind - the pointer still takes the whole row as Front's* |

**And the harness learned two things.** A line typed at the prompt is cut
at about a kilobyte, so a longer program goes in in pieces joined at the
prompt. And the window manager moves a new window that would be more than
half buried, so a test that wants two windows overlapping has to overlap
them by less. The `deskbar focus` phase pressed the minimise box 44 pixels
in from the frame's right edge; it reads the tab's width now.

## 18.108 The Drives app

**USB step 6e, drawn first in `drives.html`**: every drive, the chosen
one's partitions as a bar - not to scale, so a small partition beside a
large stick is still something to see - its partitions as rows with the
chosen one opening in Tracker, and Format... and New partition... drawn and
greyed, because it shows before it changes anything. The model is
`drivelist.lua`: each stick from the USB driver, each volume from the drive
server, the machine's own disk from `sys.disk()`, and what no volume
accounts for said as "free or unread" rather than guessed. The kit's
buttons gained `disabled`.

- **`run_x86.py`'s `usb_drives`, one check more**: on the two-volume FAT
  stick, the model sees one USB stick of 67,108,864 bytes holding PHOTOS
  then BACKUP, with 4 MB they do not account for. Run as a program of its
  own, because the one before it is typed at the prompt and a line there is
  cut at about a kilobyte - adding these lines to it silently stopped the
  whole program, and every check in the part failed at once.
- **The display harness's `drives app`, 3 checks, both boards**: the window
  opens, says what it found, and draws without a Lua error - on a machine
  with no stick and no disk, "No drives" in its first list.
- Looked at once with the FAT stick under x86 QEMU: the stick, PHOTOS and
  BACKUP and the 4 MB beside them, PHOTOS's use a dash because FAT32's free
  count is a hint, BACKUP's 44 KB.


## 18.109 A run leaves nothing in the temporary directory

**The Mac's disk filled on 19 September**, and `make prepush` died in
`x86-usb-1` with "No space left on device" while writing a test stick - a
place that had nothing to do with the cause. The suites had left 1515
directories and files in the temporary directory, 19 GB: every
`tempfile.mkdtemp` was a promise to remember a `finally`, some tools kept it
and most did not, and nothing could tell them apart. The 512 MB `/home`
test of 18.106 left 595 MB a run by itself.

So **`tools/scratch.py` is the one way a tool makes a temporary file**: a
process gets one directory, `kosmos-` and eight characters, made the first
time it asks and removed when the process exits; `directory`, `path` and
`disk` put things in it. The name is short because a QEMU monitor is a Unix
socket inside it, and macOS refuses a socket path over 104 bytes. A
`made-by` file inside says which command made it.

- **`test_scratch.py`, 10 checks, in the host suite**: the scanner finds all
  five ways of making a temporary file in a decoy of its own, and none in
  `tools/` outside `scratch.py`; a process ending normally, by
  `sys.exit(3)` and by an uncaught exception leaves its temporary directory
  empty, three processes with their own `TMPDIR`; and one ending by
  `os._exit`, which skips the tidying, is left, found by `leftovers` and
  named by `made_by` - the path `gate.py` takes.
- **`gate.py` fails a run that leaves anything**, naming each leftover by
  what made it - so a tool killed before it could tidy up is a failure with
  a name rather than a disk that fills a month later.
- Control, watched: with the removal switched off, the three endings fail,
  each naming the directory it left.

## 18.110 The Super Nintendo keeps your game

**Roadmap 4g, Diego's "Yes" on 19 September.** Closing the Super Nintendo -
Quit, the close box, `Super + Q`, or Open ROM and View starting another -
writes two files beside the ROM: `Name.srm`, the cartridge's own
battery-backed RAM, where a game writes its save slots, and `Name.state`,
the whole machine at that instant in LakeSnes's own format. Opening it again
reads the cartridge's save, then continues from the state. Game gained
**Reset**, the console's button, which is how to start again now that
closing keeps your place. Both pass through one region the size of a state,
with `fs.write_from` and `fs.read_into`, so a quarter of a megabyte never
becomes a Lua string.

- **The test cartridge got a battery**: two instructions more, `SEP #$20`
  and `STA $700000` of 4Bh into its RAM, and a header saying ROM, RAM and a
  battery, 2 KB of it.
- **The media suite's Super Nintendo phase, 6 checks more**: the first start
  on a disk with no saves says it is starting fresh; Double Size keeps the
  1x console at a frame and the 2x one continues from the same frame, with
  the cartridge's 2 KB kept; Game, then Reset, then Normal Size, and the
  game is kept at a frame lower than the one it was reset at, and continued
  from it; and, read off the disk on the Mac after the machine is gone,
  `kosmos-test.srm` is 2048 bytes with 4Bh first, and `kosmos-test.state`
  starts "LSSF" and holds its own length at byte 8.
- Measured on the ARM board: kept at frame 703 and continued at 703; reset
  at frame 1006 and kept at 265; a state of 260 KB.
- Control, watched: with the state never read, both continuing checks
  fail - "kept kosmos-test at frame 689 ... then None" - and nothing else
  does.

## 18.111 Three things wrong on four cores, before threads

**`threads.md` step 0**: found while reading the kernel for threads, and
wrong today whatever threads become.

**A shared region's reference count was not locked.** `memobj_ref` and
`memobj_unref` were a plain `++` and `--`, reached from different processes
on different cores. They take the region pool's lock now, for the check and
the change; the pages are still freed outside it, and the slot is given back
under it - by `unwind`, a failed create's path, as well.

**A refused image kept its process slot.** Six of `process_create`'s
refusals returned holding the slot `alloc_process` had claimed, each then
counted by `process_count` as a live process that never ran. `give_back`
releases it under the lock that claimed it, on every failure.

- **`mem: a region's count holds on every core`**, both boards: every core
  takes and drops a reference to one region twenty thousand times, starting
  together, and the region ends with exactly the one it began with.
- **`proc: a refused image gives its slot back`**, both boards: a page of
  zeroes offered as a program thirty-six times - more than the pool's
  thirty-two slots - and the count of processes does not move.
- Controls, watched on the code as it was: the first panicked on both
  boards with "pmm_free_page: double free" - a lost count freed the region
  while the other cores still held it, which is the bug itself; the second
  failed, and the leaked slots filled the pool, so five tests after it that
  start processes failed too.

**And an endpoint could be two servers'.** Found while reading for step 1:
`ipc_endpoint_create` claimed its slot by testing `in_use` and storing
`true`, and `SYS_ENDPOINT_CREATE` is a syscall two programs on two cores
reach together. The claim is made under the endpoint's own lock now - the
one `teardown` holds when it gives the slot back, so claiming and releasing
are ordered by the same lock and nothing new was needed.

- **`ipc: endpoints made at once are each their own`**, both boards: every
  core makes eight endpoints at the same instant, sixteen rounds, and no
  endpoint may be in two cores' tables.
- Control, watched on the code as it was: it failed on the ARM board at
  once, and passed on x86 with one round - QEMU interleaves an x86 guest's
  cores on this Mac rather than running them at once, so a window a few
  instructions wide seldom opens. With sixteen rounds it fails on both.

## 18.112 Capabilities belong to the process

**`threads.md` step 1.** The capability table was the thread's - a comment
said it would move to the process "at M4", and it never did - which is the
same thing while a process has one thread and the wrong thing once it has
two. It is `struct captable` now, with a lock of its own: a process has one
and every thread of it points at it; a kernel thread, which has no process,
points at one of its own. Taken after an endpoint's lock and before the
region pool's, and never held across freeing pages - a slot is emptied under
it and its region let go of after.

**And a capability in flight carries its generation.** A capability sent in
a message was resolved against the sender's table and installed in the
receiver's with the object's generation *at install time*. An endpoint
destroyed on another core in between, and its slot made somebody else's,
would have reached the receiver as a working capability to the stranger. It
arrives stale now; a region is referenced only if it is still the one the
sender named (`memobj_ref_as`).

- The whole gate, 29 suites, is the check that nothing behaves differently
  with one thread a process: green, 4:42.
- **`mem: a reference is taken only for the region it names`**, both
  boards: the right generation is referenced, a wrong one refused without
  touching the count, a freed region refused. Control, watched: with the
  generation not compared, it fails.
- **A test that could not fail, found and repaired.** The check that a
  capability held across a server's restart is refused faked one by
  setting a spare slot's endpoint and an old generation, and not its kind -
  so the slot stayed empty and the call was refused for being empty. Its
  kind is set now; with the generation check switched off, the kernel
  suite panics at that test, where before it would have passed.
- The boot thread and each core's idle thread are adopted rather than
  created, so they are given their tables where they are adopted: with the
  table a pointer, a slot nobody set was a data abort at the fifteenth test.

## 18.113 The thread pool grows

**`threads.md` step 1b, its first pool.** Forty-eight thread slots in
`.bss`, compiled in - the boot thread, one a process and sixteen over - are
now slabs of sixteen, pages from `pmm`, made when every slot is taken and
never given back, up to a thread for every 256 KB of memory: 2,048 on the
512 MB ARM board, 2,032 on the x86 one, 65,536 in 16 GB. The boot screen
says it: "64 slots, growing to 2048 as they are wanted". A claim takes a
never-used slot, then a dead one whose thread has left, and only then a new
slab, so the pool is as large as the most threads ever alive at once.

- **`thread: the pool grows`**, both boards: a hundred threads alive at
  once - twice the old pool, and more than the sixty-four slots made at
  boot - every one made, the pool larger for it, every one run, and the
  count back to where it was.
- Control, watched: with growth stopped at the boot slabs, that test fails,
  and the tests after it fail with it, finding the pool full of its threads.
- `sched_switch_to` drained the runqueue into an array of `THREAD_MAX`
  pointers on a sixteen-kilobyte stack; it drains into a list through
  `sched.next` now. Two tests that sized arrays by the pool take a fixed
  snapshot instead, and the one that touched every slot touches the free
  ones that exist rather than creating until a create fails - which, in a
  pool that grows, would fill the machine.
- **Still a scan a tick**: `thread_wake_sleepers` walks every slot made so
  far, 250 times a second, looking for sleepers that are due. Proportional
  to the most threads ever alive rather than to a compiled-in number, and
  fine at hundreds; a queue of sleepers kept in order of when they wake is
  the answer when it is not.

## 18.114 One pool that grows, and processes in it

**`kernel/pool.c`, written once**: slabs of at least sixteen objects, pages
from `pmm`, a directory made at boot, never given back, to a ceiling from
the machine's memory; the slot count published with release. The thread
pool of 18.113 moved onto it unchanged, and processes and address spaces
followed.

**Processes**: thirty-two at boot, a slot for every megabyte of memory -
512 on either board - so at 2.3 MB a process the memory runs out before
the slots do. **Address spaces** grow with them, to the same ceiling and
eight over: the kernel sizes that pool when it makes its own
(`as_pool_init`), so the two numbers that once disagreed - sixteen spaces
for thirty-two processes, and a spawn refused at eleven - are decided in one
place. **The process list programs read** was a buffer of thirty-two in
`sys_user.c`; it doubles until the list fits, so Processes cannot stop
listing at the old pool's size without a word.

**And an abandoned child's capabilities are released.** `sys_spawn` grants
a child its capabilities before it starts and abandons it when a later
grant fails; the region among them kept its reference, and was never freed.
`process_abandon` releases the process's table now, and gives its slot back
under the pool's lock.

- **`proc: the pool grows, and gives everything back`**, both boards: forty
  processes at once, each with an address space and a shared region, then
  all abandoned; twice, and the second round ends with exactly the free
  memory the first did. Controls, watched: without releasing the table on
  abandon, it fails; with the process pool not growing, it fails.
- The boot screen: "32 slots, growing to 512; each gets its own page
  tables, heap and stack".

## 18.115 Endpoints and regions grow

**`threads.md` step 1b, the next two pools, on `pool.c`.** Endpoints were
ninety-six, compiled in; they are ninety-six at boot and one for every
64 KB of memory at most - 8,232 on the ARM board. Regions were two hundred
and fifty-six; they are that many at boot and one for every 16 KB at most -
32,784. A new slab of endpoints has its locks made to say nobody holds them,
and a new slab of regions starts at generation one, which is what each
pool's `fresh` is for. The boot screen: "8232 endpoints and 32784 regions at
most".

- **`ipc: endpoints and regions grow past their old pools`**, both boards: a
  hundred and twenty endpoints alive at once - five kernel threads making
  twenty-four each, a table holding thirty-two - and three hundred regions,
  every one made, and every one gone after. Controls, watched: with either
  pool not growing, it fails.
- **An atomic add is a library call** on AArch64 without LSE - the first
  version of this test used `__atomic_fetch_add` and did not link, wanting
  `__aarch64_ldadd4_acq_rel`. A plain load with acquire and a store with
  release, which `pool.c` uses, are single instructions; anything that reads,
  changes and writes back is not, here, and takes a lock or a flag apiece.

## 18.116 A region as large as the machine allows, and the reserve

**`threads.md` step 1b, the sizes rather than the counts.**

**A region was 32 MB** because a descriptor carried sixteen index pointers,
each naming a page of 512 page pointers. It carries **one directory page**
now - 512 index pages, 512 pages each - so a region may be a gigabyte, or
half of memory when that is less (`memobj_pages_max`). The descriptor is
smaller for it: one pointer where there were sixteen.

**A process could map 48 MB**, plus twelve screens' worth for whoever held
the screen. Both numbers are gone. What bounds a mapping is the window's own
size and **the reserve**: `pmm_room_for_user` refuses to give a program the
last thirty-second of memory - between 8 MB and 256 MB, so 16 MB on the 512
MB board - which is what the kernel needs for a thread's stacks, a pool's
slab, page tables, and starting the process that would end a runaway.

- **`mem: a region larger than the old cap`**, both boards: a 40 MB region
  made, reached at its last page and given back whole; a region one page
  over the ceiling refused; everything the machine has left refused, because
  of the reserve. Controls, watched: with the ceiling back at 32 MB it
  fails, and with the reserve switched off it fails.
- **A Lua check that had stopped meaning anything**: `gfx.surface{4096,
  4096}` - 64 MB - was expected to fail, and it failed because of the 48 MB
  cap. With the cap gone, 64 MB is an ordinary surface on a 512 MB board.
  It asks for 65536 square, 16 GB, which no machine here can serve; the
  check was always that a refusal is clean rather than a fault.

## 18.117 A capability table that grows, and the x86 image that had run out of room

**`threads.md` step 1b, the last of the limits.** A table held thirty-two
capabilities - sixteen until a PDF viewer ran out mid-page. Thirty-two still
live in the table itself, so an ordinary process allocates nothing; past them
it takes a page of capabilities at a time through a page of chunk pointers,
up to 52,224 on a 4 KB page. The pages go back when the process ends - and
when a *kernel thread* ends, since its table is its own; a process's threads
point at the process's, which `process_exit` releases.

- **`cap: a table holds more than it has room for`**, both boards: two
  hundred regions held at once by one thread, every one resolving, the count
  past two hundred, and the free memory afterwards exactly what it was.
  Controls, watched: with the table not growing it fails, and with a dying
  kernel thread not releasing its own table it fails.

**And the x86 image had four kilobytes of room left.** `x86-kernel` stopped
booting - "the boot banner never appeared" - and QEMU's own words were
"invalid bss_end_addr address". The multiboot header said `load_end_addr`
was zero, which means "to the end of the file", and the file a loader is
handed here is the **ELF**, whose debug information is most of a megabyte:
the kernel's size came out larger than the memory the header reserves. The
margin had been shrinking for months and this day's work spent the last of
it. Both headers say `__load_end` now, so the size comes from addresses and
not from however large a build's debug information happens to be.

## 18.118 The benchmarks, and what two weeks had cost

**`make bench` was two weeks stale and both kernel numbers had grown.** Run
on 19 September after the pools:

| | 5 September | before that evening | after |
|---|---|---|---|
| `context_switch` | 8.375 | 12.064 | 12.066 |
| `ipc_roundtrip` | 36.438 | 61.944 | 62.960 |

**Attributed by building the benchmark at three commits** rather than
guessing: at the one that recorded the baselines (which reproduces them
within 3%, so the harness is consistent), at the last commit before that
evening, and at its end. So the evening cost **1.6% of an IPC round trip
and nothing measurable on a context switch**, and the rest - 44% and 70% -
arrived over the previous fortnight, which is the SMP programme: per-core
runqueues with a lock each, a lock on every pool and every endpoint, the
preemption path, the IPI.

**The 1.6% is a capability table that belongs to the process**: one
indirection and an acquire load on every resolve. It was 10% when the
table's lock sat on the read path, and that is why readers do not take it -
a slot's kind is published last with release and read with acquire, so a
reader sees a slot finished or empty (`design.md` 4.3).

**Nobody noticed for a fortnight because `make test` does not run the
benchmarks**, and `make prepush` does not either. The baselines carry the
measurement and the reasoning now, so the next regression is visible again;
finding where the fortnight's went is `roadmap.md`.

## 18.119 A thread's own pointer

**`threads.md` step 2.** `TPIDR_EL0` on AArch64 and the FS base on x86 now
belong to the thread: `SYS_SET_TLS` says where a thread's own block is, the
kernel keeps the number with the thread and loads it on every switch, and
`errno` moved into that block - it was one static int a process, "because
there is one thread".

**The two boards differ, and the hardware decides it.** AArch64's register is
writable at EL0, so a switch saves it as well as restoring it; x86's FS base
cannot be written from user mode unless the kernel sets `CR4.FSGSBASE`, which
it does not, so the kernel's record is the only writer and a switch only
restores. **Neither writes anything when it does not have to**: the processor
remembers what it has loaded, so two kernel threads - whose pointer is zero -
switch without touching a register.

**Reading it is one instruction**, which is what makes `errno` free: `mrs
TPIDR_EL0` on ARM, and on x86 the first word at `%fs:0`, since a process
cannot read the base itself - which is why a block begins with its own
address.

- **`thread: its own pointer survives a switch`**, both boards: two threads
  set different values and yield to each other a thousand times; each still
  reads its own. Control, watched: with the switch not restoring it, both
  boards fail.
- **`make bench` after it, because the switch now touches a register**, and
  it cost more than it had to. Saving and restoring every time: +3.6% of a
  context switch. Saving only when the processor held a user pointer: +4.1%,
  because the branch and the per-CPU lookup are dearer than the register
  read. **One comparison of the two threads' records, writing the register
  only when they differ: +2.1%**, and that is what is kept - two loads and a
  not-taken branch, counted at full price under `-icount` and close to free
  on a processor that predicts.
- **The kernel does not save the register**, and that is the rule that makes
  the cheap version correct: a thread says what belongs in it with
  `SYS_SET_TLS`, and what a switch loads is that record. AArch64 lets a
  program write `TPIDR_EL0` itself and x86 does not let it near the FS base,
  so a program relying on writing it would work on one board and not the
  other; what it gets instead is a value that lasts until its next switch.

## 18.120 A test that sampled a steady state

**`cpu: every processor idles as a thread` failed three times out of three**
after step 2 added two instructions to the context switch - and passed three
times out of three without them, on a kernel that was otherwise identical.
The property it checks is a steady state: a core with nothing to do runs its
idle thread. It read that one instant after the tests before it had been
making threads, so a secondary still finishing one failed it.

Attributed rather than guessed at: the same suite was built at the previous
commit and run three times (clean), then with the step's kernel half removed
(clean), then with it back (three failures). **The change was timing, not the
property.** The test waits up to a second for each core to reach idle now; a
core that never does fails exactly as it did before.

The same evening's other flake, the wallpaper check in `arm-display-3`, did
not reproduce and is left alone - a failure that repeats is a bug and one
that does not is a note.

## 18.121 A process with two threads

**`threads.md` step 3, and the first one.** `SYS_THREAD_CREATE` takes an
entry and one word and answers with the thread's index in its process;
`SYS_THREAD_EXIT` ends the caller with a code; `SYS_THREAD_WAIT` waits for
an index and returns that code, whether or not the thread has already
finished - a thread that has ended keeps its slot until somebody asks, as a
process does.

**The kernel makes each thread's stack**: a megabyte of address space above
the share window, the 256 KB stack at the top of it and the rest left
unmapped, so an overflow faults in empty space rather than in a neighbour.
**And each thread's own block**, one page at the bottom of that slot, whose
first word is its own address.

**The block is the kernel's job, and the reason is a portability trap the
first user thread walked into.** `errno` reads through the thread pointer,
and on x86 that read goes *through* the FS base - so a thread whose base is
zero faults on address zero at its first `errno`, while AArch64, whose
register a program can read directly, quietly returned a fallback. One board
worked and the other faulted. Every thread now has a block before its first
instruction.

- **`thread: a process with two threads`**, both boards, through a C role in
  the test image (`CTEST_THREADS`) because Lua cannot hand the kernel an
  entry point: a thread is started, counts to a thousand in memory both
  share, is waited for and gives back its code; each thread still reads its
  own `errno`; waiting a second time says there is no such thread, its slot
  having gone back when its code was read. The pages are all back afterwards.
  Control, watched: with thread creation refused, it fails.

**Two things went wrong on x86 and neither was what it looked like.**
Marking the process killed while waiting for its siblings made the exiting
thread's own return path call `process_exit` again - a reboot in the middle
of the suite rather than an error - so step 3 waits for threads that are
leaving and leaves killing to step 6. And a "hang" after test 58 was the
machine being loaded by QEMUs left from earlier experiments: with the
machine quiet, the suite runs in 8.5 seconds and passes 170 of 170, twice.

## 18.122 IBM Plex, and what a machine nobody has told looks like

**Roadmap 5, part 2, from the style guide Diego approved** ("Style guide
looks great"). Every role was `spleen`, an 8 by 16 bitmap: exact, free, and
what made a finished desktop look like a terminal that had grown windows.
The defaults are IBM Plex now - widgets in Plex Sans 14, a title in Plex
Sans Condensed 15, text and the terminal in Plex Mono 13 - and a `heading`
role exists at Plex Sans Bold 18, which the guide names and the kit had no
role for. The bitmap is still in the image and still what a console at a
fixed size should ask for.

**Plex Sans Condensed was not in the tree** and is now: one face, 198 KB,
from IBM's own repository under the SIL Open Font License, with the licence
beside it as every vendored thing here carries one.

- **The harness pins the bitmap faces** in its own `.appearance`, because
  forty phases find a row, a baseline or a column by the 16-pixel face they
  were written for. Pinning is honest only with something checking the
  default, which is the next line.
- **`default look`, 4 checks, both boards**: on a machine with nothing
  saved, before the pin, the three roles report Plex at their sizes **and
  each face measures more than nothing** - a name that resolved to no face
  would draw empty windows and pass a check that only compared names.
- **A lesson learned twice**: the first version waited for the line's first
  characters and read `ibmplexsans 14 ibm`, because a line arrives from QEMU
  in pieces. It waits for a marker printed after the line now, which is what
  `run_media.py` already says in a comment of its own.

**And a bug the change exposed rather than caused: the Deskbar measured its
own layout at load time.** `KOSMOS_W` - 12, the icon, 8, the width of the
word "Kosmos" and 12 - was computed when the file loaded, which is before
the process has ever opened a window and therefore before it has been told
what the desktop's faces are: `ui.window` brings them back in its reply, and
a theme change sends them again. So the word was measured in the kit's
default face and drawn in the desktop's, and the two agreed only while they
were the same face. They stopped agreeing the day the default became Plex,
and the Deskbar's focus check found it at once: every button sat a few
pixels from where the bar thought it was. `kosmos_w()` measures when it
draws now.

**And the pin had to be in one place.** Six phases rewrite
`/home/.appearance` for their own reasons - the wallpapers one names a
picture in it, others put the palette back - and each wrote the palette
alone, which dropped the faces the setup had pinned. The next desktop to
start came up in Plex, and the Deskbar check three phases later found its
buttons a few pixels from where it expected them. `appearance()` builds that
file now, palette and faces together, and every phase writes through it.

## 18.123 Two fonts at once, and two checks that could not fail

**What the screenshot of 0.10.90 showed**: menu titles overlapping
(`FileGo View`), `New folder` drawn as `New folde`, `Delet`, `Widge`,
`Press m`. What it looked like was a font that did not fit. What it was, was
two bugs that had been true all along and that the bitmap had hidden.

### The window manager never loaded its own faces

`load_appearance` applied `saved.fonts` - the table in
`/home/.appearance` - and returned early when there was no settings file at
all. So the window manager loaded a face only if somebody had been to the
Appearance panel. Nothing showed it for as long as the default was the
bitmap, because **a face that is not loaded is the bitmap**: "never applied"
and "applied spleen" draw the same pixels.

The defaults still reached applications, because what the window manager
sends them is `theme.fonts` - what was asked for - rather than what it
loaded. So every application loaded Plex and laid itself out in it, while
the process that draws the text of every ordinary window still had the 8 by
16 bitmap.

The arithmetic is visible in the picture, which is how it was found rather
than guessed: the menu bar's spans were computed from 18 pixels for "File"
and 14 for "Go", and the glyphs were drawn on an 8-pixel grid - 32 pixels
for "File" - so "Go" began 14 pixels inside it. `apply_fonts(saved.fonts or
theme.fonts)` is the fix, and a role that fails to load is now advertised as
whatever is in force instead of what was wanted.

### Text was clipped to a view by counting cells

`gc:text` cut every string an application draws to `room = width // GW`,
one cell per character. That is exact for a bitmap font and a lie for
everything else, and it is the same mistake as computing a pixel offset in
Lua, one level up: **nothing in Lua computes a character count from a width
either.** `New folder` in a 96-pixel button lost its `r` with twenty-five
pixels to spare.

It measures now - `gfx.measure` once when the whole string fits, which is
nearly always, and a binary search over characters when it does not, about
four measurements for a label.

### The check that was watching could not fail

`default look` (18.122) read `theme.fonts`, the constants in the file it had
just loaded, and compared them with themselves; then it measured three
strings in a console process where no face had ever loaded, so every width
came back from the bitmap and "the face drew" was `48 > 0`. **It passed on
the build whose screenshot is above.**

It asks the window manager now, which is the only thing that can answer:
the `theme` reply carries `held`, the faces it actually loaded, beside
`fonts`, the ones it hands on. **9 checks**: the roles in the file say what
the style guide says; all four faces load; ten M's measure wider than ten
i's, so the face that loaded is proportional rather than a silent fallback;
and each of the four roles the window manager holds is the one it
advertises. Reverting the one-line fix fails it with *"the window manager
draws ui in none, not ibmplexsans/14"*.

**It costs a machine of its own** (`gate.py`, a fifth display part). The
question needs a desktop on a machine nobody has told anything, which is
the moment before the harness pins its faces - and a desktop cannot be
quit, so starting one takes the console for the rest of that boot. In a
whole `make screenshot` run the three checks that need no desktop still run.

### And the battery's red, for the same reason one level down

`x86-core`'s battery check counted pixels of exactly `0xe04848`, which is
every pixel of a glyph while the glyph is one bit to a pixel. Against an
antialiased face the count fell to zero and the check said the low battery
was not shown as low, on a screen that was showing it in red.

Counting *reddish* pixels instead is not enough on its own: the battery
icon is orange and 79 of its pixels are reddish, which a fixed threshold
reads as a low battery on a machine that is charging. So the check is a
**difference between the two machines** - 143 reddish pixels at 8 per cent
against 79 while charging - and the icon, being the same on both, cancels.
With the red disabled it reads 79 against 79 and fails, which is what a
control is for.

**And the control has to be built.** The first attempt at it ran
`make build/x86_64/kosmos.elf`, which is not a target - make says "Nothing
to be done" and leaves the binary alone - with the output discarded, so the
suite ran against a binary from eight minutes earlier and the control
"passed". The x86 image is built by `make x86-build`.

## 18.124 The Appearance panel, and a window that measures itself

**Roadmap 5b**, drawn first (`docs/appearance.html`) and agreed the same
afternoon: two columns at 668 wide instead of seven groups in a 380-pixel
one, lists six rows deep, every font role reporting the face it is set to,
the title's shape drawn rather than described, and `heading` joining the
roles.

**What is worth testing is invisible in a screenshot.** The panel's height
is *measured* - the two columns are summed at the faces in force, and the
window is resized to what they came to - and a window that measured itself
looks exactly like one whose constant happens to fit. It regressed twice
while being written, both times silently: once the sums ran after
`win:run()`, so they happened when the window closed; once they ran before
the widgets they were measuring existed. Both times the window stayed at
the 530 it had asked for and the bottom group hung off the end of it.

So the panel says what it laid out, and `appearance` in the display harness
holds it to that line. **4 checks**: that it opened at all, that it is 668
wide as drawn, that it offers five roles rather than the four it had, and
that its height is not 530.

**The line reports `win.h`, not the sum.** Printing the arithmetic would
have passed on both regressions - the sum is correct in each of them, and
what failed was the resize reaching the window manager. `win.h` is what the
window manager agreed to, which is the only number that means the window is
really that tall. With the resize commented out the phase fails with *"the
panel is 530 tall, which is the size it asked for before it knew the
faces"*.

**And a two-copies-of-one-fact bug it inherited.** The palette list's
selection was set where the list was built, which is before the saved
settings are read - so a machine with `beos` saved opened the panel with
`dark` highlighted and, four inches below, a status line saying `in force:
beos`. It is the same bug the file's own comment describes about `role()`,
in the widget beside it. The display now catches up with the state in one
function, `reflect`, and the palette row is set there and nowhere else.

## 18.125 A list row is as tall as its face, and a role costs a slot

Two consequences of the same afternoon, both found by looking rather than
by reasoning.

**The rows.** Diego, seeing the Appearance panel's font list: "each row
should be able to show the contents without any overlapping on the other
rows below or above the item selected". `ui.list` spaced its rows by `GH`,
which is `gfx.font.h` *as it stood when the kit loaded* - and in an
application that is before the desktop has said what its faces are, because
the faces arrive in the reply to the window the kit is about to open. So
the list drew rows 16 apart with 23-pixel text in them.

It asks now: `row_h()` is `gfx.height()`, called where it is used. **No
padding**, and that is deliberate rather than mean - under the 8x16 bitmap
it is exactly 16, which is what `GH` was, so not one of the forty display
checks measured against that face moves. Under a proportional face it is
that face's height, which is the whole of the fix.

**The slot.** Adding `heading` as a fifth role broke two faces that had
nothing to do with it: `FACES_MAX` was a flat 12, so the eight faces a
program may ask for *by size* were whatever the roles left over, and the
fifth role quietly made them seven. `gfx.face` then answered "no room for
another face" for `ibmplexsans-italic` and `spacegrotesk-light`, which is
the same sentence it says when a program has really asked for nine sizes.

The `faces` phase caught it on both boards, and the fix is to stop the two
numbers sharing: `FACES_MAX` is `ROLE_COUNT + FACES_SIZED` now, so a sixth
role costs a slot of its own rather than one of the sized pool's.

**Neither would have been found by reading the diff.** One needed a
photograph of a list, the other needed a suite that loads every face in the
image - and the face that failed was in neither the role that was added nor
the file that added it.

## 18.126 A film built here, and the seam between Lua and C

**The Video app** (`docs/video.html`, `roadmap.md` 4e) is a caller of the
media kit: the picture at its own size, the controls under it and never
over it, the window manager's menu bar above, File/View/Play, and the two
states that are not a film - nothing open, and a film this system cannot
decode, which names what it is and what would play.

### The fixture: a film, because none goes in the repository

`test_mp4.lua` says it about the container and it is as true of the
pictures. So `run_media.py` **builds one**: four frames of flat grey, each
a different grey, 16 by 16 at ten a second, in an MP4 with the `esds` that
says its pictures are JPEGs.

The JPEG is written by hand and is the smallest baseline one that means
anything - a single 8x8 block whose only coefficient is DC, which *is* a
flat grey square, with the standard Huffman tables. So what the suite
exercises is the decoder in the image rather than an encoder written here
to be exercised by it. `ffmpeg` on the Mac reads the result as *mjpeg
(Baseline), gray, 16x16*, which is the independent opinion that the bytes
are really a JPEG.

**5 checks**, in the suite that already builds a disk: that the kit opens
it and says Motion JPEG 16x16 of four frames; that asking for the frame at
four moments gives back the four greys it was made from; that nothing was
dropped; and that `media.open` still opens an MP3 as a song, which is the
other half of one door. Off-by-one in the frame lookup fails it with
*"should decode to about [40, 120, 200, 96] and came back as [120, 200,
96, 96]"*.

**Three of those checks failed first on their own patterns**, not on the
system: `Motion JPEG` is two words where the regex wanted one, and `$` does
not reach past the `\r` in a guest's `\r\n` - which is why every other
pattern in that file says `\r?\n`.

### What the fixture found: `mp4v` is not a codec

Building a film by hand meant deciding what to put in its sample entry, and
that exposed a guess. `mp4v` means "MPEG-4 systems describes this", and
*which* codec is the object type in the entry's `esds`: 0x6c is JPEG, 0x20
is MPEG-4 Visual. `mp4.lua` read `esds` **only for audio**, so every `mp4v`
film looked alike and the kit mapped all of them to JPEG - right about the
file in front of me and wrong about MPEG-4 Visual, which would have been
handed to a JPEG decoder to fail as "would not decode" rather than as "this
system has no decoder for that".

### And the seam: a frame was a Lua string

Diego, seeing the kit: *"why are we decoding mp4 in lua and not c? what is
the mp4.lua lib for?"* The answer is that `mp4.lua` is the *index* - a few
hundred boxes, read once, and it runs on the host so the format is tested
without booting - and the decoding was always C. But the question found
something real in between: `film:frame` read each frame into a **Lua
string** before handing it to the C decoder. Ten kilobytes, thirty times a
second.

**`gfx.jpeg` has taken `(address, length)` since the window manager needed
it for wallpapers**, with a comment saying exactly why - bytes about to be
thrown away should not be bytes the collector walks. The facility existed,
the reasoning was written down, and the new kit used the string form
anyway, while `mp4.lua`'s own header claimed frames are never touched here.

Measured the same way, 200 frames drawn flat out:

| | a string per frame | straight from the region |
|---|---|---|
| 200 frames | 3.58 s | **3.22 s** |
| read | 7.25 ms | **6.04 ms** |
| decode | 9.34 ms | 9.62 ms |
| what the collector saw grow | 7.8 KB | **1.2 KB** |

**Ten per cent, and the garbage gone.** The lesson is not "that should have
been C": the decoder *was* C and the index *should* be Lua. It is that the
**seam** between them was copying a value into Lua's world for no reason on
its way to C - which does not show up as slow code in a profile, it shows
up as collector pressure, which is what actually breaks a frame deadline.

## 18.127 The sound suite fails when the gate is busy, and that is the suite's fault

**Twice on 20 September**: `x86-sound` failed inside `make test` with *"the
HDA ring underran: UNDERRUNS 2, worst write 64181 us"* and then passed
14/14 - and later 39/39 - run on its own a few minutes later, with nothing
changed. The first time the machine was busy with a QEMU somebody had left
playing a film; the second time it was busy with the gate's own thirty
suites.

**It is a real measurement of the wrong thing.** The check asks how long a
write to the audio ring took in *wall-clock* microseconds, which on a host
running thirty emulators is a measurement of the host's scheduler. The
second complaint beside it - "the queue floor is not a number between one
and the device depth" - is the same underrun read a second way, so one
stall counts twice.

**What not to do about it**: widen the threshold until it stops firing.
That is the number the check exists to watch, and a suite that cannot fail
is 18.123's lesson.

**What to do**, unresolved and now `roadmap.md` 5f: either give this suite
the machine to itself in `gate.py` - it is short, and the gate already runs
`default look` alone for a different reason - or measure the guest's own
idea of lateness, which is what the underrun counter in the driver already
knows and which does not depend on what else the Mac is doing.

Until then, **a sound failure inside the gate is re-run alone before it is
believed**, and this section is the reason that is not special pleading.


## 18.128 A rasterizer ported to C, and the check that it draws the same picture

**Nine hundred lines of numeric C, and the only thing that makes them
legitimate is that they draw the picture the Lua already drew.**
`user/lib/gamesoft.c` is `user/lib/solar/soft.lua` function for function -
clear, blend, point, rect, frame, line, lineFast, circle, text, and the
three expensive ones: a lit textured sphere, ray-traced rings and the sun.
The core on disk is untouched and still runs under stock `lua` and LÖVE;
the host swaps the module in through its own `require` (`solar.lua`), and
`--lua` runs the original.

**Why all of it rather than the two hot primitives.** The first attempt
moved `clear` and `line` to C writing into the core's Lua table, and
`solar --bench` said, clearing 960x540 ten times:

```
Lua into a table       24.81 ms
C into that table      43.81 ms   0.56x
C into a surface        1.32 ms  18.72x
```

**C into a Lua table is slower than the interpreter.** `fb[i] = c` in Lua
is one VM instruction reaching the table's array part; the same store from
C is `lua_pushinteger` and `lua_rawseti` - the whole table API with its
boxing and its write barrier - about 85 ns a pixel, which at 960x540 is
thirteen milliseconds a frame in *stores alone*. The interpreter was never
the slow part; the representation was, and a C loop over it pays the toll
twice.

So the framebuffer had to become a surface - and `sphere`, `ring` and `sun`
index `self.fb` directly inside their inner loops, so the moment it does,
they have to come too. All of it or none of it, and that is the general
lesson: **moving a loop to C is worth nothing until the data it walks stops
being a Lua value.**

**What it bought**, under QEMU TCG at 960x540 focused on Earth - a ratio
rather than a speed, which is what TCG is for:

| level | 1 | 2 | 4 | 7 | 10 |
|-------|---|---|---|---|----|
| Lua   | 100.6 ms | 118.1 | 131.9 | 251.5 | 528.7 |
| C     | 12.3 ms  | 14.4  | 24.3  | 43.5  | 63.3 |
| | 8.2x | 8.2x | 5.4x | 5.8x | 8.4x |

### The check

**`solar --compare`, and `tools/run_game.py` in the gate as `arm-game`.**
Every primitive is called on both rasterizers with identical arguments -
lines at every slope including the ones that clip on each edge and the
degenerate one, points at all three sizes, the three expensive ones at
block sizes 1 and 2, with and without an atmosphere, with the ring shadow
on, and text last so glyphs blend rather than merely cover - and then all
368,640 pixels are compared.

**No tolerance.** One channel out by one is a failure, because a rounding
difference that shows on a single pixel here is the one that shows on a
hundred thousand when a planet fills the screen. It names the first
disagreement with both colours, because "17 pixels differ" says nothing
and "at 412,88 the Lua says 3f4a5e and the C says 3f4a5d" says which
`floor` to go and look at.

**It also counts how much of the frame was drawn on**, and that is not
decoration: two rasterizers that drew nothing agree on every pixel, so
`0 differ` is a sentence a broken script and a correct port both produce.
105,346 of 368,640 are drawn on, and the guest refuses to call anything
below a tenth a pass. This is the mistake `check_default_look` made by
comparing a table with itself (18.123), caught in advance this time.

### The control, which bit

Changing one constant - `S:point`'s corner weight, 0.25 to 0.35 - made it
report **exactly five differing pixels**, naming the first. Five is
arithmetically right: the four corners of the size-3 point at 80,40, plus
the single on-screen corner of the one drawn at the origin. A check of
this shape that has never been shown to fail is a check nobody should
believe, and this one was shown before the constant was put back.

That constant is not hypothetical. It is the bug this port actually had:
written from memory of having read the file, `0.35` for both the plus and
the corners, when the source uses `0.6`/`0.35` for the plus and `0.25` for
the corners. Reading the source again before trusting the port is what
found it - and the comparison is what would have found it anyway.

### Three places that deliberately differ

All in the safe direction, and each marked SAFER in the file: a texture
read is bounds-checked where `string.byte` past the end would return nil
and raise on the next arithmetic; a truncated PPM is refused rather than
returned short; a glyph shorter than its cell reads as blank rather than
raising on `nil > 0`. The Lua faults or throws in all three, so nothing
that works today can tell the difference - and a C port that faulted
instead would be a memory bug rather than an error message.

## 18.129 A perfect rasterizer drawing into the buffer nobody was looking at

**The pixel comparison passed, and the window was black.** 18.128's check
said all 368,640 pixels agreed; the app reported 43.2 frames a second at
graphics level 4; the screenshot showed a black rectangle with a title bar
on it. Both statements were true.

The host had done this:

```lua
local dst = win:surface()          -- once, before the loop
while win.running do
  app:draw()
  present(dst)
  win:commit{ ... }
end
```

`window:surface()` returns `self.region[self.region.draw_into]`, and
`commit` flips `draw_into` to the other buffer. So every frame after the
first went into the buffer that had just been shown, while the buffer
being shown was never written. The old code called `win:surface()` twice
inside the loop and the hoist looked like tidying.

**What makes this worth a section is that the comparison could not have
caught it, ever.** `--compare` checks *what is drawn*; this was a bug about
*where it lands*. A check of one is structurally blind to the other, and a
suite of perfect checks on one axis still ships a black window.

So `run_game.py` grew a second half: boot a machine with a display, open
the app, screendump, and count the lit pixels **inside the window's own
rectangle** - not the screen, because the desktop behind it is a
photograph of mountains and would pass any "there are colours here" test
on its own.

**Control**: hoisting `win:surface()` back out of the loop makes it report
**7 lit pixels** where a drawn frame has tens of thousands. Put back, it
passes. The whole suite is 5 seconds.

### The class, not the instance

**A handle to something that flips must not be cached across the flip**,
and this system has now met that shape twice in different clothes. The
other is 18.20: a capability index left in a parent after the endpoint
behind it was destroyed, which names *nothing* rather than the wrong
thing - the kernel answers "the endpoint was destroyed" and a slot goes
back to the pool. Same class, opposite symptom, which is why neither
reminded anybody of the other.

The general form: **a value that names a slot rather than a thing, held
longer than the slot's identity lasts.** The kernel's answer was to make
the stale index *fail loudly*; a back buffer cannot do that, because
writing to it is entirely legal - it is simply not the one being shown. So
the only defence here is not to keep it, and the only way to know is to
look at the screen.

`ui.lua`'s own helpers ask for the surface each pass, which is why nothing
else in the system has this. This host stopped doing so for about ninety
minutes.

## 18.130 A click is an event, and the desktop was sampling it

**Diego, after 0.10.99 on the ThinkPad**: "i found some quircks like the
mouse buttons be unrespiosnive under certaun scenarios" - on the desktop
and the Deskbar.

**The code had already worked out why and said so.** Above `fill_pointer`
in `user/servers/console.c`:

> A key is an event and a position is a state. Keys queue, so reading them
> a pass late loses nothing - they are all still there. The pointer does
> not queue.

`SYS_POINTER` answered where the pointer is and what is held *now*. A
press and a release that both happened between two of the window
manager's passes left `buttons` exactly as they found it, so the click
never existed. That comment had fixed the half sampling order could fix -
taking the sample *after* the wait rather than before - and named the half
it could not: "a window whose repaint takes a dozen messages: the release
then arrives while the manager is draining those."

That is a busy desktop. It is also why QEMU never found it and a real
machine did.

### The fix: the pointer gets what the keyboard has always had

`hal/keys.c` has kept key transitions in a ring since the first driver
outside the kernel. `hal/pointer_edges.c` is that, for buttons, and
deliberately the same shape - a reader should not have to learn two ideas
about what an input event is.

- **Shared by both boards**, beside `hal/keys.c`. The PC records an edge
  when its *merged* state changes (a TrackPoint and a USB mouse are two
  sources); the virt board records one when the tablet's buttons change.
  The two lose a click for different reasons and lose it just the same.
- **The position travels with the edge**, because a click is at a place. A
  press in a menu and a release elsewhere are two facts.
- **The oldest is dropped when full**, never the newest: dropping the
  newest would leave a press whose release had been thrown away, which is
  a button held down for ever - worse than the bug being fixed.
- **`dropped` is reported** all the way to `wm`, which says so once. A
  transition nobody will ever see is exactly what this path exists to
  prevent, so losing one is not allowed to be silent.

The chain is `hal_pointer_edge` → `SYS_POINTER` → the console server →
`con.wait` → `wm.lua`, which **replays each edge through `pointer_pass`
before the current state**. That needed no new logic in `pointer_pass`: it
already decides press-or-release by comparing with the last state it saw,
so feeding it the states in the order they really happened produces the
presses and releases that really happened.

**One hazard closed on the way.** `CON_OP_POINTER` - the plain "where is
the pointer" any program may ask - also calls `SYS_POINTER`, and reading
*takes* the transitions. Without care, a program asking where the pointer
is would have stolen the window manager's clicks and thrown them away:
clicks going missing *because something else looked*, which is nastier
than the original bug. The console server keeps a stash that both calls
append to and only a reply carrying clicks empties.

### Two checks, at two levels, and both controls bite

- **`input: a click between two looks is not lost`** (`make test`, both
  boards): `hal_pointer_move` down then up with no poll between, then
  assert the state says nothing is held *and* that two transitions are
  waiting, and that reading took them. **Control**: making
  `hal_pointer_edge` a no-op turns it to `not ok 149`.
- **The fourth check of the `clicks` display phase**: a press and a
  release sent as **one QMP event batch**, so the board processes both
  before anybody looks. Every other click in that phase is press, sleep,
  release - three calls - and is sampled correctly even by the old code,
  which is why none of them ever caught this. **Control**: removing the
  replay from `wm.lua` fails it with "the click was dropped because the
  pointer was sampled rather than its transitions read".

The second control is the interesting one. It reproduces on an idle
machine in QEMU what a loaded machine did by itself, which is the general
trick worth keeping: **when a bug needs the machine to be busy, find the
way to make the race certain instead of likely.**

### A Lua limit met on the way

`wm.lua`'s main chunk is at Lua's ceiling of **200 locals**, and adding
one more makes the file refuse to parse - reporting the overflow at
whatever innocent line happens to be last, which cost two rebuilds to
understand. The pointer's bookkeeping is one table, `pointer_log`, folded
into the slot the old `pointers_said` counter had. Worth knowing before
the next thing this file needs.

## 18.131 Full screen, and an application that killed the window manager

**Diego, seeing the rasterizer run at a hundred frames a second on the
ThinkPad**: "we just need a way to maximize the window and able to drag,
rotate and else with the mouse". The pointer half was already wired -
`solar.lua` has forwarded `pointerDown`, `pointerMove` and `pointerUp`
since the port - so what he was missing there was 18.130's lost click.
Full screen was genuinely absent.

It is a **relaunch**, not a resize, and that was Diego's own instruction
when the port was planned: "we will relaunch it in fullscreen as we do
with the other apps now when needed". The reason holds twice over here: a
window that draws its own pixels has its shared region allocated when it
opens, and so does the film the rasterizer draws into.

What travels across the relaunch is the graphics level, the body in focus
and the date - what a person had set up and would otherwise set up again.
The camera's angle does not: `setFocus` re-aims it, and carrying yaw,
pitch and distance would be the host reaching further into the core's
state than a host should.

### The bug it found, which was the window manager's

The first attempt asked for full screen with the *film's* size, 960x540.
The window manager made the window the screen - 1024x768 in the harness -
and then **died**:

```
process "wm" died: data abort from a lower EL
  far     0x00000001803f5000
  cause   read, translation fault, level 3
```

A window that draws its own pixels is composited straight out of the
region the application allocated. Asking for full screen having made a
smaller region tells the window manager to read past the end of it.

**The comment right above the fault had stated the contract**: "it has
*already* made buffers the size of the screen". Nothing checked it, and an
unchecked contract is a wish. `CLAUDE.md` is explicit that a server "has
to stay correct when the caller is wrong, out of date, or hostile" - and
here an application's arithmetic mistake took the whole desktop with it.

Both halves fixed:

- **`wm` refuses** a full-screen window smaller than the screen, naming
  both sizes. **Control**: making `solar --full` ask for 960x540 again
  gets "no window" and a live desktop, where it used to get a translation
  fault and a dead one.
- **`solar` asks for the screen**, from `gfx.screen():size()`, as the
  Video app already did. The *film* stays 960x540 either way: cost is per
  pixel, so rendering at the panel's size would be four times the work,
  and `stretch` blows it up in one C call.

### And a number that was not the window's size

`present` and the damage rectangle used `width * scale` - what was
*requested*. Full screen is the case where that is not what arrived, so
the destination now comes from `win:surface():size()`, which is the only
thing that knows. The pointer is mapped the same way: at full screen a
click at the right-hand edge of a 1920 screen has to land at the
right-hand edge of a 960-wide film, which `ev.x // scale` does not do.

## 18.132 The addresses that never came back

**Diego, on the ThinkPad**: "the video player ran once with the mp4 mjpeg
video but not a second time", "it looks something remained in memory",
"that was broken after watching the video the first time". Something had
remained, and it was not memory.

### What it looked like, and why every guess was wrong

The player reported `no moov box - not an MP4, or not a whole one`, which
says the file is bad. The file was fine. Four suspects were eliminated by
measurement before the real one appeared:

| test | result |
|------|--------|
| `play` six times with **no window manager** | decoded every time |
| `play` four times, each under a **fresh desktop** | fails on the 4th, always |
| `wc` on the whole film after three plays | reads all 2,854,583 bytes |
| `diskfs` after three plays | alive, healthy |
| starting and stopping the desktop five times, no film | memory flat |

So not the decoder, not the media kit, not the filesystem, not the window
manager's lifecycle. Memory looked *flat* the whole time - 72 MB of 512 -
which is what made it hard: **the pages were being freed all along. Only
the addresses were lost.**

`play` with no desktop never decodes a frame; it prints the film's details
and exits. With a desktop it decodes three hundred, and the filesystem
server maps the caller's entire 4 MB buffer on **every one of those
reads**. One ten-second film spends over a gigabyte of a 4 GB window.

### The cause

`p->next_share` is where a process's next shared mapping goes, and it only
ever climbed. `SYS_SHARE_UNMAP` returned the pages to their owner and left
the addresses spent, so a server that maps and unmaps in a loop walks up
its window and then can map nothing at all, ever again.

Fixed LIFO: when the range going back is the one most recently handed out,
the mark comes down. That is deliberately a half-measure and the code says
so - a free list needs somewhere to keep the holes and this kernel has no
allocator; a bitmap would be 128 KB a process for a 4 GB window; and every
server here has the one shape LIFO serves exactly, which is map the
caller's buffer, answer, unmap, wait.

### What made it findable

Two diagnostics, and the second is the lesson.

`"that is not a region this process can map"` has three causes and named
none of them. It now says which, and that is what turned a week of
guessing into an afternoon: the answer was **"the kernel would not map
it"**, not a full table, which pointed straight at the address space.

**A server cannot print.** It is spawned with one capability, so a
`kosmos_write` inside `diskfs` goes nowhere - an instrumented build said
nothing at all and looked like a build that had not taken. The reason had
to travel *in the reply*. That is worth remembering before instrumenting
anything below the shell again.

### The check, and the control that confirmed the arithmetic

`tools/run_media.py` hands the filesystem server a 4 MB buffer twelve
hundred times and requires all twelve hundred. **Control**: disabling the
two-line fix reports "a server ran out of shared address space after
**1015** buffers of 1200" - and 4 GB divided by 4 MB is 1024. The number
the control produced is the number the diagnosis predicts, which is a
stronger result than a test that merely goes red.

### Also found

- **`wm` never released a direct window's shared region** when the window
  closed. A real per-window leak, fixed here; it was not this bug, and
  measuring said so rather than hoping.
- `film:close()` never closes the audio stream that `player:close()` does,
  and the audio server never reclaims a stream whose client died - eight
  exist and nothing notices a corpse. Recorded as `roadmap.md` 5l rather
  than fixed in the same change.

## 18.133 A wallpaper that would not come back, and said nothing

**Diego, on the ThinkPad, 21 September**: "the appearance app does not
rememver the wallpapers and other things upon restarting". The other
things were remembered. The boot log shows `wm: faces ui=ibmplexsans/14`,
which is his choice rather than the default, so `/home/.appearance`, the
disk and the load were all working. Only the wallpaper failed, and it
failed in silence.

### Two faults, one on each side of a line

**The decoder refused palette images.** `blue.png`, `black.png` and
`white.png` in his wallpaper folder are all colour type 3, because a field
of one colour is exactly what a palette is for and every tool writes one
that way. `png.c` read grey, RGB and RGBA and nothing else. It now reads
`PLTE` and `tRNS`, unfilters the indices and then looks them up, and an
index past the palette is black rather than a read off the end - the same
choice the texture reader in `gamesoft.c` makes, for the same reason.

**The window manager threw the reason away.** `wallpaper_load` answers
`nil, why`, and the restore took only the truth of it. A saved setting that
does not apply and does not say so leaves nothing to search for. It now
prints `wm: wallpaper <name>`, or `... would not load: <why>`:

```
wm: wallpaper /home/blue.png
wm: wallpaper /home/nosuch.png would not load: cannot read /home/nosuch.png
```

### The checks

- **`png: a palette, its alpha, and no palette`** (`luatest.lua` role 49).
  A 4x2 palette PNG built inside the role, byte by byte, with real CRCs
  and a stored deflate block, so any other program would open it too:
  three entries, `tRNS` for two of them, one index past the palette, and a
  second row filtered with Up - so a decoder that looked indices up before
  unfiltering them gets the deltas and fails. A palette image with no
  `PLTE` must be refused, and for that reason.
- **The display harness's wallpaper phase** now also requires the window
  manager to say `wm: wallpaper <the one it saved>`, and no refusal.

**Control**: `png.c` as it was at 0.10.102, with the new test in place.
One of 176 fails, and it is this one, for the right reason:

```
luatest: a palette PNG did not decode: png: a colour type this does not
do - a palette, or grey+alpha
not ok 132 - png: a palette, its alpha, and no palette
```

**And a flake, recorded rather than rerun away.** The first `make prepush`
of this change failed one test of 172 on x86, `sched: the policy is
pluggable`, with round robin running its threads `132` - an order its record
in 18.39, always `231`, has not shown before. Nothing in this change is near the
scheduler; the same image ran the suite alone five times, 172 of 172 each,
and the second whole prepush was green in 4:56. It stays the open item
`roadmap.md` already names.

## 18.134 An adapter the driver walked past without a word

The USB Ethernet dongle enumerated on the ThinkPad and said its name -
`0bda:8153, USB 3.0, class 0, "USB 10/100/1000 LAN"` - and then nothing.
That silence was two things. `use_device` read a device's **first**
configuration and stopped, and the RTL8153's first is Realtek's own
interface; the standard one, CDC-ECM, is its second. And a configuration
that was none of a mouse, a stick or a pad returned without a line, so
nothing on the screen said there was anything to look for.

### What changed

- `use_device` asks for each configuration in turn while none is of use,
  up to eight, and takes a CDC-ECM function where it finds one
  (`usb_decode_ecm`). A device that is nothing here in all of them is said
  by its first interface's class - `class 02/02/ff - nothing here reads it`.
- The adapter is named with its MAC address, read from the string its
  Ethernet Networking descriptor names in the device's first language, and
  its frames' interface, setting and endpoints. It is not configured: that
  is 7b (`usb.md` 10).

### The checks

- **`test_usbdecode`**, 107 checks: both of the RTL8153's configurations
  as libusb read them from the dongle on this Mac, byte for byte, and every
  refusal `usb.md` 10 lists. Two controls, each a scratch copy of the
  decoder with one rule taken out: without the Union's controlling
  interface held to the Communications one, *"a Union that says interface 5
  controls was taken"*; without each setting starting its endpoints afresh,
  nothing failed - the RTL8153's setting 0 has no endpoints to leak - so a
  configuration with a half setting before the whole one was added, and
  that control now fails it: *"a setting with half the endpoints lent the
  next one its bulk IN"*.
- **`run_x86.py` `usb_ethernet`**, 5 checks, in `x86-usb-1`: QEMU's
  `usb-net` with the MAC `52:54:00:4b:4d:53` given to it, which must come
  back; its ECM function in configuration value 1, which is its second
  descriptor after RNDIS; 1514-byte frames; interface 1 setting 1, bulk 2
  each way of 64 bytes, and the link on interrupt IN 1. **Control**: the
  driver built to read only the first configuration says `class 02/02/ff -
  nothing here reads it`, and the check fails.

### And on the dongle itself

Passed to QEMU with `usb-host`, without root:

```
xhci: 00:02.0 port 1: USB Ethernet, CDC-ECM, in configuration 2: MAC 00:e0:4c:68:02:86, frames up to 1514 bytes; not driven yet
xhci: 00:02.0 port 1: its frames on interface 1 setting 1, bulk IN 1 and OUT 2 of 1024 bytes; its link on interrupt IN 3
```

The same MAC macOS gives `en9`. Descriptor requests reach a device macOS
holds; interfaces do not (`libusb_detach_kernel_driver: -3 [ACCESS]`), so
7b on the real dongle, from the Mac, is a `sudo` run for Diego.

**And a panic in the gate, chased before it was rerun.** The first
`make prepush` of 0.10.104 stopped `arm-display-2` at its first boot, just
after the banner, with `spinlock: endpoint held by 1, wanted by 2` and
`PANIC: spinlock: gave up waiting`. Nothing in 0.10.104 is in the kernel
or on the ARM board's USB path. `spinlock.h` records that the bound, ten
million spins, is **about ten milliseconds on AArch64 under TCG** - shorter
than a scheduling slice on a Mac running six four-processor guests on ten
cores, so a holder whose host thread is paused trips it exactly as a
deadlock would, and the line cannot say which.

So it was tried again the way it happened: the same part alone passed its
49 checks, and **thirty boots of the same image, six at a time**, all
reached userland with no panic. That says rare and load-shaped, and it
does not say harmless: `smp.md` records the one earlier time this fired,
when the holder had faulted after taking the lock. What would settle it is
the holder's side - what core 1 was doing - which the panic does not print.
Worth having the next time: `spin_panic` asking the holder for its PC.

## 18.135 A theme that names its faces, and Plex

**Diego, 21 and 22 September**: "i want the theme to look exactly as the
mockup, same fonts same sizes same spacing, same colors", "like a theme is a
complete color scheme + font selection?", and then his four choices from
`docs/plex.html`: widgets in Plex Sans 14, reading text in Plex Sans 16,
headings in Plex Sans SemiBold, no rounded corners or shadow for now.

### What was built

- **A theme file names a face and a size for each role** -
  `font.heading = ibmplexsans-semibold 15` - checked for the role and a size
  between 6 and 96 pixels, and told line by line when it is wrong.
- **Plex**, the fifth theme in `themes.lua`: the mockups' colours and the
  five faces chosen. The four themes that were palettes name the faces they
  always had. `IBMPlexSans-SemiBold.ttf` joins the fonts, from IBM's
  `@ibm/plex-sans@1.1.0` release - the one `IBMPlexSans-Bold.ttf` is,
  byte for byte - font version 3.005, under the licence already beside it.
- **Appearance**: "Theme" where it said "Palette"; choosing one sets its
  five faces; a face changed afterwards says ", yours"; `Back to this theme`
  restores both; `wm appearance:--theme <name>` does what a click does and
  prints the faces the window manager *holds*.
- **`theme.apply` copies colours only** - a hazard the change would have
  opened, since it copied every field and a theme now carries `fonts`.

### Found on the way

**A saved theme did not survive a restart, unless it was `dark` or
`light`.** The window manager applied `/home/.appearance`'s theme by name,
and the only names it knew were the two palettes compiled into `theme.lua`
- so Photon, BeOS, Platinum, IRIX and now Plex were written down
faithfully and came back as `dark`, with the faces restored beside them
because those are saved spelled out, and nothing said so because
`theme.apply`'s answer was thrown away. That is the rest of Diego's "does
not rememver the wallpapers **and other things** upon restarting" - the
wallpaper was 18.133, his faces came back, and his theme did not. It now
finds a saved theme where Appearance finds themes, `themes.lua` and
`/system/themes`, and says `wm: theme <name>` or `... would not load:
<why>`. Found by writing the restart check before the fix: *"a desktop
started with Plex saved wears 'dark ibmplexsans-semibold/15'"*.

**The harness read a line before it had arrived.** `wait_for` returns the
moment its text is on the serial line; the Appearance phase then read to
the end of the line and got `668x590,` - half of it - once the window
manager's new `wm: theme` line moved where the serial reads fell. A race
the check always had. `wait_for_line` waits for the newline as well, and
the phase's four reads use it.

**The first picture of Plex had a yellow Deskbar and `spleen` highlighted.**
The Deskbar paints its strip in `theme.tab`, the window tab's colour, and
Plex's tab is the video mockup's yellow while the indicators mockup draws
the bar in stone. So the bar has colours of its own, `bar` and `bar_text`:
every theme that ships sets them to its tab's, so none of them changes,
and Plex to `#e7e7e3` and `#1e1e1e`; `test_theme.lua` holds both. And
Appearance highlighted a face only on an exact name, so a title set to
`ibmplexsanscondensed` - the start of the one file there is - left the list
on `spleen`; it now matches by `font_asset`'s rule, exact then prefix.

**Every shipped theme was read with complaints, and always had been.** A
comment line holding a bare `#` - the blank line inside a block of comments,
in all five - was "not `key = value`", because the rule that strips a
comment wants a space after the `#` so that `#ffffff` stays a colour. A
line that *starts* with `#` is a comment now, whatever follows.
`tools/test_theme.lua` found it the first time it ran.

### The checks

- **`tools/test_theme.lua`**, 124 checks, in `make host-check`, handed
  `FONT_FILES` so every face a theme names is held to what the image
  embeds, by `font_asset`'s own rule: each shipped theme clean and complete;
  the four old ones unchanged; Plex value for value against `docs/plex.html`;
  three bad lines told; a role left out coming from defaults a theme cannot
  change; and a palette applied without its faces. **Control**: the old
  copy-everything `theme.apply` fails exactly *"applying Plex replaced the
  faces in force with its own, without loading one"*.
- **The display harness's `appearance` phase**, 3 more checks: `wm
  appearance:--theme plex` must leave the window manager holding Plex's five
  faces; `/home/.appearance` must hold `plex` and its SemiBold headings; and
  a desktop started afresh must wear `plex` with `ibmplexsans-semibold/15`
  in force. Then the harness's own appearance goes back. **Controls**: the
  restart check, run before the window manager could find a shipped theme,
  failed as above; and an Appearance whose `take_theme_faces` does nothing
  leaves the window manager holding
  `title=spleen/16 ui=spleen/16 heading=ibmplexsans-bold/18 ...` - the
  harness's pinned faces - and the check fails.

## 18.136 What the ThinkPad said about 0.10.105

Diego ran the 0.10.105 stick on 22 September and sent two photographs: the
Appearance panel after choosing faces at 16, and `log`. Five things were in
them, and each is answered here.

### The adapter, on the machine it is for

```
xhci: 00:0d.0 port 3: USB Ethernet, CDC-ECM, in configuration 2: MAC 00:e0:4c:68:02:86, frames up to 1514 bytes; not driven yet
xhci: 00:0d.0 port 3: its frames on interface 1 setting 1, bulk IN 1 and OUT 2 of 1024 bytes; its link on interrupt IN 3
```

The same two lines the Mac's `usb-host` gave (18.134), on port 3 of the
Thunderbolt controller at `00:0d.0`. 5m-a is done on metal.

### A reply too big for a message

`wm: reply for poll failed: value does not fit in a message`. The window
manager filled a poll's reply by count - twelve events - on the assumption
an event is small, and a theme event is a whole palette and five faces;
three of them already overflow 2048 bytes, and choosing faces quickly in
Appearance queues that many. The reply failed and the window's events went
with it. Now the reply is filled while it fits, asked of the serialiser
itself through a new `sys.fits` (the byte count `sys.pack` would produce,
with no string made), and an event too big to go alone is dropped and said
rather than stopping the queue.

**Check**, the display harness's `appearance` phase: a program sends the
window manager four theme messages that change nothing, so four theme
events queue for its window, and then polls - all four have to arrive.
**Control**: the reply filled by count again reproduces the ThinkPad's own
line, `wm: reply for poll failed: value does not fit in a message`, and the
program never counts its events.

### A panel laid out at the faces it opened with

With widgets and the terminal at 16, the Appearance panel's role list ran
its last row off the bottom of its box, and its status line slid under the
window titles group - the line that said the choice was "not saved", and
why. The panel measured its layout once. Now every number it is made of is
worked out in one `measure`, and `layout` places every widget and sizes the
window, when the panel opens and again from `win.on_theme`: a hook the kit
calls after applying a theme event, for the rare window whose layout is
made of the faces. `roadmap.md` 5b's remaining half.

**Check**: after `--theme plex` the panel must say `appearance: laid out
again, ...` and come out taller, since the harness's pinned faces are the
16-pixel bitmap and Plex's are larger with padded rows. **Control**: with no
`on_theme` the relayout line never comes.

**And the check found a bug of its own.** The first run had the panel
*shorter* under Plex. Appearance sent the window manager a theme's colours
and not its spacing, so Plex chosen in the panel arrived without its padded
rows - only a restart, which reads the whole theme file, put them right. It
sends both now.

### "Not saved", and a pipe left in the wrong state

The status line read `applied heading = ibmplexsans-semibold 16, not
saved`: `/home/.appearance` could not be written, so nothing chosen would
have survived a restart. The reason was under the other group. Two changes,
and the second is a guess at the cause rather than a finding:

- **Appearance says a failed save in the log**: `appearance: not saved to
  /home/.appearance: <why>`, so `log appearance` can read it.
- **The log had the USB stick recovered badly a few minutes earlier**: two
  READ (10)s that got no answer within a second, the stick reset, and in
  red, `the bulk OUT's Set TR Dequeue Pointer failed: Context State Error
  (19)` - on the pipe every write to the stick uses. `reset_pipe` tried
  Reset Endpoint and read its refusal as "running", tried Stop Endpoint and
  read *its* refusal as "already stopped" - which is also what a pipe that
  halted between the two refuses with - and Set TR Dequeue Pointer takes only
  Stopped or Error. It now reads the EP State the controller keeps in the
  output context (xHCI 6.2.3, values from FreeBSD's `xhci.h`), stops a
  Running endpoint, resets a Halted one, reads again, and moves the dequeue
  pointer only when it is Stopped or Error - or says which state it found.
  **Checked** by `run_x86.py`'s `usb` part, whose stick stalls a command on
  purpose: both pipes recover, 28 checks with `usb_blocks` and
  `usb_ethernet`. **Not checked**: the order the ThinkPad hit, a stall
  landing while an endpoint is being stopped, which QEMU's stick cannot be
  made to do. The next stick says so in the log if it happens again.

Why the stick stopped answering READs at 21 seconds is not known.

### Faces at 16

"In the thinkpad fonts look smaller than on qemu so the default font size
for regular and widgets is 16." The defaults, which Photon, BeOS, Platinum
and IRIX name, and Plex's widgets, are 16; the default-look checks hold the
new sizes.

### Plex's spacing, inside a widget

A theme file names three paddings - `row_pad`, `button_pad`, `field_pad` -
carried with the colours, and the kit's lists, buttons and fields read them.
Every theme but Plex names the numbers the kit had written in (rows 0,
buttons 5 by 12, fields 3 by 4), so none of them moves; Plex names the
spec's 7, 5 by 16 and 5 by 8. `list:row_height()` answers the row's height.

**Checks**: `test_theme.lua`, now 135, holds every theme's spacing and four
bad spacing lines; the harness's Plex restart probe opens a window and
requires a list row of its face plus 14 and a button of its words plus 32.
**Control**: a list that ignores `row_pad` measures "its face and 0".

## 18.137 The Deskbar's colour, chosen

**Diego, 22 September**, on seeing Plex's stone bar where BeOS's is yellow:
"is that a setting?", "a color in the theme?", and then "keep the deskbar
user selectable color". A theme names where the bar starts (`bar`,
`bar_text`, 0.10.105); Appearance now offers it beside the desktop's colour
- eight swatches: the bars the shipped themes paint, and two darks - and a
choice is kept in `/home/.appearance` over the theme's and survives a
restart. `Back to this theme` gives the theme's own back.

**The words follow the colour.** A theme names its bar's words beside its
bar; a colour picked in Appearance has no theme to say them, so
`theme.ink_on` works them out - dark on a light ground, white on a dark one,
by BT.709 luminance with the line at 140 of 255, a little above the middle
because a mid grey reads better with dark words.

### The checks

- **`test_theme.lua`**, 9 more: every swatch Appearance offers, and one
  more blue, get the ink they have to.
- **The display harness's `appearance` phase**, 2 more: `wm
  appearance:--bar 336698` - the colour chosen as a swatch chooses it - is
  applied and written down; and a desktop started afresh paints the bar's
  bottom row `#336698` with white words on it. **Control**: a window manager
  that ignores a saved bar at startup never paints it, and the check fails.

## 18.138 The Deskbar's height, chosen

**Diego, 22 September**: "i want to be able to change the deskbar height
for instance, where do i do that? is there a file?" It was `local H = 36`
in `/bin/deskbar.lua`, compiled into the image. Asked whether a theme value
or a choice in Appearance: "both".

- **A theme names `bar_h`**, a spacing value with bounds of its own - 36 to
  64, because the bar's icons are 32 pixels and the compositor does not
  scale a picture yet. Every theme that ships names 36.
- **Appearance offers 36, 44 and 52** under the bar's colours, kept in
  `/home/.appearance` over the theme's; `Back to this theme` restores it;
  `--bar-height` chooses one from a command line.
- **The Deskbar is as tall as the theme says**: it opens at 36, before the
  theme has reached it, and resizes itself at once and from `on_theme`.

### A refusal nobody heard

The first run of the check drew a 36-pixel bar with 52 chosen, and said
nothing. The Deskbar's `win:resize` was refused: the window manager's resize
handler asked `resizable`, which answers whether a window gets a *sizing
grip*, and a strip rightly gets none. But the question a request needs is
whether the window may be another size, and a strip may - it is only a
window that draws its own pixels, with a shared surface of a fixed size, that
may not. The handler asks that now, and a strip that changes height gives
the room above every other window back (`recount_strips`).

### The checks

- **`test_theme.lua`**, 151: every theme's `bar_h`, and a 20-pixel bar
  refused with its bounds.
- **The display harness's `appearance` phase**, 2 more: `wm
  appearance:--bar-height 52` is applied and written down, and a desktop
  started afresh paints the harness's yellow bar to its 52nd row and the
  desktop below it. **Control**: a window manager that ignores a saved
  height draws the old 36-pixel bar, and the check fails.

## 18.139 Four looks

**Diego, 22 September**: "Let's just make 3 or 4 good design options in
colors and fonts and stick to those", and on the drawing
(`docs/looks.html`), "Those 4 looks are great". So `themes.lua` ships four
looks and nothing else - **Plex**, **Plex Night**, **Classic** and **Studio**
- each its colours and its Deskbar designed together, and all four naming
the same five faces, which are now the kit's own defaults: a machine nobody
has set up draws the words Plex draws. Plex is that machine's look, where
BeOS was. Photon, Platinum and IRIX are in the history; BeOS lives on as
Classic, its researched values and notes kept. The Appearance panel lists
the four by their titles - not every palette its process holds, which
included the kit's own `dark` and `light` - and the panel's own trimming to a
look, a wallpaper and the Deskbar's height comes after the fixed layout (5x).

### The check that arrived with a bug to find

`test_theme.lua` holds each look to the shared faces and to legibility:
words on windows, words on the Deskbar, and **a title's words on both of its
tabs**. The last was written after the first photographs of the dark looks,
in which every unfocused window's title had vanished: a tab's words are one
colour on a focused tab and an idle one, and Plex Night's and Studio's idle
tabs were the slate of their windows, with dark words on them. They are a
light grey now. **Control**: the first build's idle tabs, `#2c3038` and
`#24262c`, sit 21 and 22 luminance points from their words against the 100
the check wants, and fail it.

151 checks. The display harness's default-look phase holds the new
defaults - titles in Plex Sans Condensed 14, the reading text in Plex Sans
16, the terminal in Plex Mono 14.

**And one phase that leaned on the default.** The display harness's Log
View phase wrote an appearance with no palette and so wore a fresh
machine's look - BeOS, which it was written against - and looked for its
console's `#0b0b0b`. With Plex the default, the console was `#1c1c1e` and
the phase failed on both boards in the first 0.10.109 prepush. It names
Classic now, BeOS's palette, as its notes always said it meant to.

## 18.140 One fixed layout

**Diego, 22 September**, after using faces at 16 over a layout that
followed them: "the changing of spacing on fonts alter the window widget
placing and brakes it. We should have a fixed widget layout and just use
fonts that adhere to the widget and windows layout", "and that layout is
fixed" (`roadmap.md` 5x).

### What changed

- **`theme.metrics`**: a list, tree or menu row is **24** pixels, a button
  **28**, a field **26**, a window's tab **20** - in every look and at every
  face. `ui.lua` reads them; `ui.metrics` hands them to applications.
- **The kit's widgets are those sizes**, and the words in them are centred
  by `gfx.height()`, asked when drawn - where they used to be sized by
  `GH`, the face as it stood when the kit loaded, which is how a larger face
  made every row taller and moved everything below it. A checkbox's box is
  16 pixels; a menu row with a picture is still as tall as its 32-pixel icon.
- **A theme carries no geometry.** The same morning's `row_pad`,
  `button_pad` and `field_pad` are gone from the format and from the looks;
  a file that names one is told it is no token. The Deskbar's height is the
  one geometric thing left, and it is Diego's choice (5v), never a look's.

### The checks

- **`test_theme.lua`**, 161: the fixed sizes, and **every look's faces fit
  their boxes** - widgets, text, headings and the terminal a 24-pixel row,
  titles the 20-pixel tab, with a pixel either side, since `gfx.c` makes a
  face's ascent and descent come to its size and rounding can add one. A
  theme naming `row_pad` or `bar_h` is told. **Control**: a look whose
  widget face is 23 pixels fails it in all four.
- **The display harness**: under Plex a list's row is 24, a button given
  no size its words and 32, and 28 tall. And the Appearance panel, laid out
  again in Plex's faces, **keeps exactly its size** - the check that held
  the opposite that morning, that it grew with the faces, is turned round.
  Three checks that found a list's rows by the 16-pixel face - the arrow keys
  moving a selection, the Open window's second row, Tracker's Places - find
  them by the fixed row now (`LAYOUT_ROW`).

## 18.141 Appearance: a look, a wallpaper, the Deskbar's height

**Diego, 22 September**, on the drawing at the foot of `docs/looks.html`:
"the panel is right, build it". The Appearance panel was a theme list, a
colour for the desktop and one for the Deskbar, five font roles each with a
face and a size, a preview, and a title's shape - about 1,070 lines. It is
the four looks as cards, each a desktop in miniature painted in that look's
own colours; the wallpapers; and the Deskbar's three heights - 380 lines,
560 by 426 on the fixed layout, and the same size in every look.

- **Choosing a look sends its colours and its faces** - a look is a whole,
  and a machine still holding faces picked in the old panel gets the look's
  back - and `/home/.appearance` holds the look, the wallpaper and the height
  and nothing else. A failed write still says so in the log.
- **The Deskbar's colour is the look's again.** 0.10.107 made it a choice
  of its own the same morning; the looks decided colours an hour later, and
  the choice, the window manager's override and `theme.ink_on` are gone.
- **Labels may name a face's role**, `role = "heading"`, which is how the
  panel's three headings are drawn in the look's heading face.

### The checks

- **The display harness's `appearance` phase**: the panel says it is
  560x426 and offers four looks; `--theme plex` leaves the window manager
  holding Plex's five faces; `/home/.appearance` then holds `plex` and no
  faces of its own; a desktop started afresh wears Plex with its headings in
  SemiBold 15; the layout probe still finds rows of 24 and buttons of 28;
  four theme events reach a window; and a height of 52 chosen and kept - now
  on Plex's stone bar, since the panel saves a look with the height and the
  harness's `dark` is not one. 11 checks.
- **`test_theme.lua`**, 152, without the ink checks that went with the
  Deskbar's colour.

## 18.142 The Deskbar at 32, and an icon at any size

**Diego, 22 September**, on the ThinkPad with 0.10.111: "Taskbar size should
not be changeable let's make it fixed at 32". It had been 36, then a choice
of 36, 44 or 52 for an afternoon (18.138).

- **The Deskbar is `theme.metrics.deskbar`, 32**, beside the rows, buttons
  and tabs of the fixed layout. `bar_h`, `theme.spacing` and its bounds,
  the window manager's two overrides, the Appearance panel's height row and
  `--bar-height`, and the Deskbar's resize on `on_theme` are gone - and
  `win.on_theme` with it, since nothing else used it. A `bar_h` a `/home`
  saved before is read by nothing.
- **Its icons are 24**, because a 32-pixel icon in a 32-pixel bar touches
  both edges. The image carries Haiku's 16s and 64s beside the 32s now,
  from the same commit, and `gc:icon` draws those three sizes pixel for
  pixel and any other by averaging the 64 down - the op carries `smooth`,
  and `stretch` grew the mode: each destination pixel the area-weighted
  mean of the source it covers, weighted by alpha when composited.
- **The tab is 26 in `theme.metrics`**, where it said 20 while the window
  manager drew 26; `wm.lua` reads it from there.
- **The Appearance panel is 560 by 362**, a look and a wallpaper.

### The checks

- **The guest suite's `gfx: a picture drawn at another size`**, three
  more: four greys averaged into one pixel are `ff2b2b2b`; three pixels
  into two split the middle one, `ff323232 ffdcdcdc`; and white beside a
  transparent pixel, averaged over blue, is `ff8080ff`. Blue because over
  black the average that ignores alpha comes out right.
  **Controls, both watched**: `stretch` ignoring `smooth` fails the first
  with `ff111111`; `area_sample` weighing every pixel as opaque fails the
  third with `ff808080`.
- **The display harness's `appearance` phase**: the panel is 560x362; and
  over a `/home/.appearance` that says `bar_h = 52`, as 0.10.111 wrote it, a
  desktop paints the harness's yellow bar to its 32nd row and the desk
  below its 34th. **Control, watched**: the Deskbar at 36 fails it - "never
  drew a Deskbar 32 pixels tall".
- **`test_theme.lua`**, 152: the layout's numbers, the `ui` face in the
  Deskbar's box, and `theme.current()` carrying no height.
- **Found by the gate, not by reading**: the harness's own sum for where
  the Deskbar's first window button starts still had a 32-pixel icon in
  it, so the focus phase sampled each button's icon instead of its fill.
  It reads `DESKBAR_ICON` now.

## 18.143 A scrollbar's thumb in the tab's colour

**Diego, 22 September**, with a picture of Mac OS 9's Appearance control
panel: "i want the scrollbar handle to be colored after the tab bar color as
an accent color like how macos 9 had it". The thumb was the widget grey,
the same as the arrows beside it.

- **The kit's thumb is filled with `tab`** and carries Platinum's grip:
  four raised ridges, eight pixels wide, each a line of the tab's colour lit
  by 55 per cent over one shaded by 35 (`theme.toward`). The trough and the
  arrows stay grey. Every list, tree and text view that scrolls draws
  through `draw_scrollbar`, so all of them changed at once.
- **The browser draws its own scrollbar** into its own pixels, and draws
  the same thumb.

### The checks

- **The display harness's `widgets` phase**: in the gallery's list - five
  items in four rows, so it has a bar - the strip the bar occupies holds at
  least 200 pixels of the harness look's yellow tab and 16 of the grip's
  shaded ridge, `a58100`. Counted inside the list's last eighteen columns,
  so the window's own tab cannot be what was found. **Control, watched**:
  the thumb filled with `raised` again fails it - "0 pixels of the tab's
  yellow and 32 of its shaded ridges".
- **`test_theme.lua`**, 155: `theme.toward` lights the yellow to `ffe58c`
  and shades it to `a58100`, and moves a colour by nothing not at all.

## 18.144 A title bar across, the Deskbar in the tab's colour, a box greyed

Three of Diego's on 22 September, the same afternoon:

- **"i want to switch back the tabs from be os style to full width".**
  `tabs.width` is the frame's width, and the shape a `/home/.appearance`
  saved when it was a choice - `tabs` - is read by nothing, and a theme
  message no longer carries one.
- **"the deskbar tab color should be yellow or at least the same color of
  the acccent color of the theme".** `bar` and `bar_text` leave the theme
  format; the Deskbar draws in `tab` and `tab_text`, as a focused title bar
  and a scrollbar's thumb do. Appearance's cards draw their Deskbar and a
  title bar across the miniature window the same way.
- **"when a window cant be maximixed we shouldnt remove the button we
  should just gray it out and disable it".** A window that draws its own
  pixels has its maximise box, flat, its glyph in `text_dim`; a press on it
  does nothing, where before the same press - with no box there - took the
  window by its title and dragged it.

### The checks

- **The display harness's `tabs` phase, rewritten.** It tested a BeOS tab:
  beside it the window behind, for the eye and the pointer. Now, over a
  `/home/.appearance` saying `tabs = "beos"`, two own-pixel windows: Front's
  title row, where a tab would have ended, is the tab's yellow (within
  `TAB_TOL`, since the bar is a gradient); the window manager says Front's
  bar is its frame's width; Front's maximise glyph is `text_dim`; and a
  press on that box dragged sixty pixels leaves Front where it was. 4.
- **The `appearance` phase's Deskbar**, now in Plex: a desktop started over
  `palette = "plex"` and `bar_h = 52` paints its bar to row 32 in Plex's
  tab yellow, `#f2c230`, not the stone it was.
- **`test_theme.lua`**, 150: a theme naming `bar` or `bar_text` is told
  they are not tokens, and the Deskbar's legibility is the focused tab's.
- **Four controls, each watched failing its own check**: the tab held to
  120 pixels ("Front's title bar is 120 wide and its frame 504"); the
  greyed box not drawn (its glyph read as the tab's yellow); a press on it
  that drags, as before (Front's corner went from its green to Behind's
  blue); and the Deskbar drawn in `raised` ("never drew a Deskbar 32
  pixels tall in Plex's tab yellow").
- **Seen once and not this change's: the desktop stopped before its `q`.**
  `x86-display-3` after the part's first pass, at the Appearance phase: the
  harness's Control-W Q left the window manager ended with a stray `q` at
  the shell's prompt, which then prefixed the next command, so `wm
  appearance:--theme plex` was never run. The part passed alone right
  after. It came back on the next change and was found then: the phase
  waited for the prompt from its own start, not from the stop (18.145).

## 18.145 Everything at a scale

**Diego, 22 September**: a setting "like Windows does", "a factor
multiplier of all the things in the UI", "Something like that slider of
iOS"; the drawing in `docs/looks.html` approved - "the proposed size slider
is great as it is", "with the %" - and "start on the size slider"
(`roadmap.md` 5z, `ui.md` 16.18).

- **The window manager converts at its edge with each window** and works
  in the screen's pixels inside, as before. `scale` holds the percentage,
  one of seven steps, and the window's own factor is `win.pct`: a window
  opening has its size and place multiplied and the reply divided back; a
  drawing command has its rectangle multiplied by its two edges, its text
  placed in a face loaded at the scale, its triangle's corners multiplied,
  and a picture drawn at its scaled size, averaged, an icon from its 64;
  a commit's damage lands where the surface's pixels do; every event
  through `post` is divided back; a move or a resize asked for is in
  points. The chrome's sizes are rewritten at the scale.
- **A window drawing its own pixels keeps its surface** at the size it
  asked for, and the compositor stretches it to its place: `stretch` took
  a clip rectangle so a damaged piece can be composed without the rest. A
  full-screen window's factor is one.
- **Appearance's Size row**: the slider as drawn, seven steps, a knob in
  the look's accent that follows the pointer at once and applies when let
  go, the percentage beside it, and `--scale` for a script. The panel is
  560 by 430. The Deskbar follows its width when a scale changes it.
- **A change of scale with windows open** rebuilds each at its new size in
  pixels, keeping its size and place in points, posts a theme event so each
  draws again, and says `wm: rescaled <title> to WxH`. `scale.pt` rounds to
  the nearest point, so a trip to 150 and back comes home exact.
- **Sized faces are given back** (`gfx.release_faces`), where the window
  manager clears its cache of them - which a change of scale does. The
  pool was eight, fixed, and never emptied; a few changes of scale would
  have filled it. And its message says the reason `gfx` gave, where it
  said "no room" for everything: under the harness's pinned bitmap faces
  the Appearance slider's large A asked for a size a bitmap cannot make.

### The checks

- **`scale`, a new display phase, at 150**: the window manager says so;
  the gallery, asking for 460 by 330, is 690 by 495; the own-pixel
  window's title bar is 39 rows, counted as the rows above it that are not
  the desk - by the tab's colour it failed once, because which of the two
  windows opens last, and is focused, is a race; the gallery's selection
  bar is 36; a click on its
  third row selects the third row; and a 200 by 100 own-pixel window is 300
  by 150 with its green reaching the corner. The screen it saw is kept in
  `build/scale-150.ppm`. 6.
- **`scale changed`**: `wm gallery,appearance:--scale 150` rebuilds the
  open gallery at 690 by 495 and writes 150 down; `--scale 100` brings it
  back to 460 by 330. 3.
- **The `appearance` phase**: the panel is 560 by 430.
- **The guest suite**: `stretch` with a clip writes inside it only, each
  pixel the one the whole rectangle would put there; and `gfx: faces by
  size are given back` - eight fill the pool, a ninth is refused, and after
  a release one loads.
- **Six controls, each watched failing its own check**: windows opened at
  100 ("asked for 460x330 and is 460x330"); events not divided back ("the
  selection bar at 396, not near 468"); an own-pixel window blitted rather
  than stretched (its corner the tab's yellow, not its green); the chrome
  at 100 ("title bar is 26 rows"); a change of scale that does not resize
  ("rescaled gallery to 460x330"); and a release that releases nothing
  ("given back, a face did not load again").
- **The gate refuses a display phase no part runs.** The scale's phase was
  written, wired into the harness, and reported as "0 on everything at 150
  per cent" by a gate that passed: `DISPLAY_PARTS` names each part's
  phases and nothing checked that every phase was in one. `gate.py` reads
  `run_screenshot.py`'s phases now and will not start while one is left
  out - held, like `uncovered_x86_parts`, with the scale's phase taken out
  of its part as the control.
- **The Appearance phase's race, found and fixed.** Seen twice on x86 -
  the second time on this change - as `wm appearance:--theme plex` never
  run: its stop waited for the prompt from the phase's start, where the
  prompt `wm appearance` was typed at already was, so the wait ended at
  once and the next command raced the window manager's exit. It waits from
  the stop now, as the other twenty-one stops did.
