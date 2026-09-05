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

