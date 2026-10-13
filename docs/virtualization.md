# Virtual machines in Kosmos, before they are built

Diego, 6 October 2026: "Is there a way we could port qemu to kosmos?"; 10
October: "can we create a virtual machine environment to run android
apps?"; 11 October: "i want to explore adding virtualization support to
kosmos". This is the exploration, for Diego to decide on: what it is, the
two ways to build it, the steps, and the questions that are his. Nothing
here is built.

## What it is for

Diego, the same day: "to launch sandboxes to run small OSes like linux for
specific tasks". So the first purpose is **a sandbox**: a small operating
system started for one job, given what that job needs and nothing more, and
gone when it is done - the shape of Firecracker's microVMs behind AWS
Lambda, which boot a Linux in well under a second with a console, a disk,
a network and nothing else.

```
   sandbox run alpine --share /Home/Projects/site -- make build
```

- a guest booted from a small image in `/Home/Sandboxes`, in a fraction of
  a second, with its own memory and processors;
- a folder of `/Home` given to it - that folder, and nothing else of the
  machine - and its network only if asked;
- the command run, its output shown, its exit code returned, and the guest
  thrown away; or kept running, reached from a Terminal.

It is Kosmos's capability idea, a level up: **what you were not handed, you
cannot reach** - a guest is handed a folder and a network the way a process
is handed capabilities.

## What else it would give

- **Another operating system in a window**: Linux first, Android later
  (`roadmap.md`, *Android applications, in a virtual machine*), at the
  processor's own speed rather than an emulator's.
- **Kosmos as a host**, which is a strong test of everything it already has:
  threads, address spaces, IPC, regions, the window server, the network
  stack and the disk server all serve a guest.
- **POSIX without a POSIX personality**: a program that wants a Linux
  world - a compiler toolchain, a Linux-only tool - gets a whole one in a
  sandbox, and Kosmos still grows no `fork` or `exec` of its own
  (`design.md` §17).

## Not porting QEMU

QEMU as a program is the wrong shape. It is written against POSIX - threads,
signals, `mmap`, file descriptors, `select` - and glib, which Kosmos
refuses at the system level; a compatibility layer inside its process
would be months. Without hardware help it is also TCG: an emulator,
compiling the guest's code as it runs, which needs memory both writable and
executable - a question Kosmos has not had to answer. A guest would run at a
small fraction of the machine's speed.

**The shape that fits is the hardware's virtualization, split as Kosmos
splits everything**: the mechanism in Nebula, the policy in a user process -
as KVM and QEMU are on Linux, and as seL4 and QNX do it in microkernels.

## The architecture

```
   an application: "Machines" - start, stop, a guest's screen in a window
         |   messages: start, stop, the screen's region
         v
   vmm (C, a server per guest) - the machine's devices, in user space:
     virtio console, block (a disk image in /Home), net (through the stack),
     input (keys and pointer from its window), GPU 2D (frames in a region)
         |   exits as messages: "the guest touched device X", "it halted"
         v
   Nebula: two new kinds of object, by capability
     a virtual CPU     a thread that runs guest code; run until it exits
     guest memory      an address space seen through a second set of page
                       tables (EPT on Intel, stage 2 on ARM)
         |
         v
   the processor: VT-x and EPT (the M700's i7-6700 has both), or EL2 on ARM
```

**What Nebula learns, and why it belongs there.** A virtual CPU is a thread
that enters the guest instead of user code; guest memory is an address space
with a different page-table format. Both are what the kernel already is -
threads and address spaces, handed out by capability - so the principle
that the kernel knows threads, address spaces, IPC and capabilities and
nothing else still holds. Everything a guest *sees* - its disk, its
network, its screen - is the monitor's, in user space, where a bug is a
crashed server and not a crashed machine.

**The busiest path is an exit.** A guest touching a device stops, Nebula
turns that into a message to the monitor, the monitor answers, the guest
goes on. Virtio exists to make those rare: a guest puts many requests in a
ring of shared memory and touches the device once - Kosmos's own *control by
message, data by shared memory*. The number to measure first is one exit's
round trip; a few microseconds is the target, and the window server's
round trip on the M700 is about 20.

**What is C and what is Lua.** Nebula's part is C, as all of Nebula is. The
monitor is C - a device model is on every guest's path. The Machines
window is Lua, the thinnest layer.

## Two ways to begin

| | **x86-64, VT-x, on the M700** | **AArch64, EL2, under QEMU** |
|---|---|---|
| Where it runs | the M700 and the ThinkPad, today's machines | QEMU's `virt` board with `virtualization=on`; later the Pi 5 |
| Testable in the gate | **no**: QEMU's emulator does not emulate VT-x, so only the real machine can run it | **yes**: QEMU emulates EL2, so every step gets a suite |
| What changes first | VMXON, a VMCS per virtual CPU, EPT tables, the exit handler | Nebula stays in EL2 at boot instead of dropping to EL1 (`boot/start.S`), stage-2 tables, the exit vector |
| Cost | iterating by netboot, every try on the M700 | the ARM suites back on (paused since 8 October) |

The design is the same either way; the second architecture is a port of
Nebula's part, while the monitor and the window are shared.

## Steps

- **V1** - a virtual CPU that runs ten guest instructions and stops: the
  exit seen, its reason said. Measured: one exit's round trip.
  **Done in 0.11.181** (`testing.md` 18.576), in the shape Linux uses on
  cores without VHE: Nebula stays at EL1, and a core that arrives at EL2
  leaves a stub there (`arch/aarch64/hyp.S`) - its vector, a stack, and the
  EL2 settings EL1 needs to run as before (the counter and its timer, the
  GIC's system registers, VPIDR and VMPIDR, no FP traps). An `hvc` from
  the kernel with a `hyp_vcpu` puts the host's 23 EL1 registers and its
  x19-x30 aside, loads the guest's, turns on the second stage (a 39-bit
  guest space walked from level 1, `vcpu.c`) and enters; anything EL2 traps
  - an HVC, a stage-2 fault, the host's tick (HCR.IMO) - comes back as the
  `hvc`'s answer. A guest counts x0 to 9 and stops on `hvc #0x4b`; a load
  from an unmapped page stops with EC 0x24 and HPFAR naming it; an exit's
  round trip is about 1,300 counter ticks under TCG (a regression number,
  not a speed). The whole kernel suite passes booted at EL2 as at EL1.
  **Learned**: QEMU's PSCI starts a secondary core at its caller's level,
  EL1, so only core zero has the stub under QEMU; a vCPU runs on a core
  that does (`hyp_present` is per core), and a board's firmware that starts
  cores at EL2 gives every core one. **Owed before a real board**: EL2's
  own translation - with it off EL2 reads memory uncached, and QEMU models
  no caches to show the difference - and the FP registers, the GIC's
  virtual interface and the guest's timer, which Linux needs at V3.
- **V2** - guest memory: a guest with its own page tables reading and
  writing what the monitor gave it, and a fault on anything else.
  **Done in 0.11.182** (`testing.md` 18.577): a guest and a vCPU are two new
  capability kinds (`CAP_GUEST`, `CAP_VCPU`, `kernel/vm.h`), owned by the
  process that made them as an interrupt line is and ended with it, each
  from a pool that grows. **A guest's memory is a region**: the monitor
  makes one, maps it into itself, and gives it to the guest at a guest
  physical address (`SYS_GUEST_MAP`) - so loading a kernel is writing to
  its own memory, and the guest holds the region until it ends. A VMID a
  guest, of 255 or 65,535 as the core says; a vCPU's translations dropped
  on its first entry. `SYS_VCPU_RUN` enters **once** and returns every exit,
  the host's own tick included (`VCPU_EXIT_HOST`, "run it again"): the
  interrupt is taken on the way back to user level and a kill takes effect
  there, as for any syscall, which a loop in the kernel would have had to
  reinvent - and syscalls on this board run with interrupts masked. The
  arch half is `arch_vm_*`; x86-64 answers `SYS_ERR_NO_DEVICE` until VT-x.
  **Owed**: a guest's code written by the monitor reaches a guest running
  uncached only after a clean to the point of coherency, which the
  monitor cannot do from EL0 and QEMU does not need - the kernel will do it
  on `SYS_GUEST_MAP` or at entry, before a real board.
- **V3** - `vmm` with a serial console: a small Linux kernel booting to
  its prompt in a Terminal window. Measured: the time from asking to the
  prompt - under a second is the sandbox's mark.
  **Alpine runs, 0.11.183** (`testing.md` 18.578): `sandbox run alpine` reads
  Alpine 3.24.2's kernel (Linux 6.18, unwrapped from its EFI zboot by
  `tools/alpine.py`) and an initramfs of its minirootfs into a region that
  becomes the guest's RAM, and `fs.vm_start` starts `vmm` (C, a role) with
  it, a setup page (`vmmproto.h`) and the program's console. The monitor
  writes a device tree (`fdt.c`) - one CPU, PSCI over `hvc`, the generic
  timer, a GICv3, a PL011 - and runs the vCPU: the PL011 and the GIC's
  distributor and redistributor are emulated by their stage-2 faults, and
  interrupts reach the guest through the processor's own virtual CPU
  interface (four list registers, loaded and saved at EL2 with ICH_VMCR);
  its virtual timer is its own while it runs and off while the host does;
  its `wfi` is trapped and the monitor sleeps until its timer is due; its FP
  registers are lent to it around each entry (`fp_lend`). Linux boots,
  claims `ttyAMA0`, runs Alpine's `/init`, which says what it is and
  switches the machine off through PSCI. **Two things it took of the
  kernel**: a spawn may now hand the child a region, not only an endpoint
  (`ipc_cap_grant`), and the console is handed to `vmm` by the namespace,
  never held raw by the program. **V3c, the keyboard, 0.11.184** (`testing.md`
  18.579): what is typed at Kosmos's console goes to the PL011's receiver -
  the monitor takes it with `CON_OP_KEYS`, and when the guest idles it waits
  on the console with `CON_OP_WAIT`, so a key or the guest's timer wakes it,
  whichever comes first - and the guest's terminal escapes are dropped on
  the way out, since Kosmos's console has no escape language by design. A
  sandbox whose `/init` ends in `sh` on `ttyAMA0` (`tools/alpine.py
  --shell`) is an Alpine prompt that answers. **V3 is done: Alpine Linux at
  a prompt, on Kosmos.** The time from asking to the prompt is about 14 s
  under TCG, which says nothing of the M700 and is not yet measured there.
- **V4** - **the sandbox**: virtio block for its image, a folder of `/Home`
  shared into it (virtio-fs, or 9p, which is smaller), the network through
  Kosmos's stack only when asked, and `sandbox run IMAGE -- COMMAND`
  returning the command's output and exit code.
- **V5** - the screen and input, for a guest that wants a desktop: virtio
  GPU in 2D, its frames in a region the window server composes like any
  window's; keys and pointer from it. The Machines application, drawn
  first.
- **V6** - Android, as a guest of its own (Android-x86 or AOSP's Cuttlefish,
  built for virtio).

Each step leaves a permanent test, which the second column of the table
decides more than anything else.

**Size, honestly**: V1 to V3 several weeks of focused work; V4 and V5 about
as much again; Android a project of its own on top.

## Decided, 12 October 2026

Diego, as Sandboxes began: "the initial goal for sandboxes is to run a linux
distro like alpine", and to the questions below -

1. **ARM in QEMU first.** V1 to V3 are built on AArch64 at EL2 under QEMU's
   `virt` board with `virtualization=on`, so each step has a suite in the
   gate; VT-x on the M700 follows with the design proven. The ARM suites
   stay off as he said on 8 October, **except the sandbox's own**, which run
   in every gate (`CLAUDE.md`, *x86-64 only, for now*).
2. **Alpine Linux** is the first guest, to its prompt on a serial console.
3. **9p first** for a shared folder, virtio-fs later.
4. **Images come as packages**: the package server offers them and `pkg` or
   the Packages window installs them into `/Home/Sandboxes`.
5. **Now** - the top of the queue.

The mockup's own three (`docs/sandboxes.html`) are settled by the same
answers where they overlap - images as packages - and a sandbox's screen is a
console first, as recommended; the window's shape is asked again at V5, when
there is a window.

## For Diego to decide, as it was asked

1. **x86 on the M700 first, or ARM in QEMU first?** Recommended: **ARM in
   QEMU** for V1-V3, because every step can be held by a suite in the gate
   - then VT-x on the M700, with the design proven. The cost is putting the
   ARM suites back. If Intel first matters more, each step is tried on the
   M700 by netboot instead.
2. **The first guest**: a small Linux to a serial prompt (recommended), and
   which one - Alpine is a few megabytes, and boots in a moment.
3. **How a folder is shared**: 9p (small, simple, slower) first and
   virtio-fs later, or virtio-fs from the start?
4. **When**: after `smbd` and Tracker's work in the queue, or before?
