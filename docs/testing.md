# Testing and performance

Three different things that get conflated often:

- **Correctness.** Does it do what it says? Binary, no nuance.
- **Budget.** Does it fit in the time it has? A frame is 16.6ms and that's it.
- **Regression.** Did it get worse since yesterday? A comparison against a stored baseline.

Each one needs its own mechanism. A single "performance" number answers none of the three.

---

## 18.1 The harness

**The runner lives on the host, the tests run in the guest, serial is the channel.**

```
make test        # runs the whole suite under QEMU, exit code 0 or 1
make test M=3    # only the tests up to milestone 3
make bench       # runs the benchmarks and compares against baselines
```

Of the three, only `make test` exists today. `M=` is pointless while there is
one milestone's worth of tests, and `make bench` arrives with the first number
worth recording, which is the IPC round trip at M3.

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
- Overhead of a syscall called from Lua versus the same one from C

### Namespace and servers (M5)

- Path resolution
- A node `read`, end to end from the client
- How many concurrent clients a server takes before latency degrades
- The cost of a hot reload

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
| M5 | Two processes see different namespaces from the same server. Hot reload without the client noticing |
| M6 | Dragging a window with a hung app inside it stays smooth |
| M7 | A live query updates without polling when another process writes. An `fs.write` into an app's namespace changes its state |
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
