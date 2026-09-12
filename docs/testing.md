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

**The Multiboot 2 half is checked once, not on every run.** `run_uefi.py`
boots the ISO, which carries no disk, and a USB image with one needs GRUB's
tools and a minute of `mcopy`. So it was booted by hand: a USB image made
with `mkusb_image.py --disk`, under OVMF as a USB stick, with the boot log
saying `a disk from the loader: 8192 KB`. The T14 has since booted a
stick from `make MEGA=1 usb` and run Doom and Quake off its disk.

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
is 0x0b0b0b and nothing else on the desktop is, so the bounding box of that
colour is the character grid - and measuring the grid rather than the window
frame is the point: the frame moves because the window manager moved it,
whether or not the application noticed anything.

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
