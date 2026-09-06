# HAL and targets


---

## x86-64, and what a second architecture actually cost

**It runs the desktop.** `make x86` puts Kosmos into long mode on QEMU's
q35 and then does everything the ARM build does: twelve boot stages, the
whole Lua userland, a window manager, the Deskbar, Tracker, the widget
gallery, the terminal, the browser, the network stack and the disk.
`tools/run_screenshot.py` is the measure worth quoting, because it asks
QEMU what is on the screen rather than asking the guest:

```
PASS: 62 display checks     x86-64, 108.6s in phases
PASS: 62 display checks     aarch64, 109.0s in phases
```

The same sixty-two, and to within half a second the same time - which is
TCG emulating both, and says nothing about either processor.

**What it cost, once `kernel/` compiled, was almost entirely device
plumbing**, and three bugs are worth recording because none of them is
about x86:

  * *`virtio_open` reset the device it had only been asked to find.* Fine
    for a driver that wants the first card of its kind; wrong for
    `input.c`, which walks past devices on the way to the one it wants,
    because a keyboard and a tablet are both virtio-input and telling them
    apart means opening each. The tablet's scan reset the running keyboard.
    It survived on `qemu-virt` because QEMU lays virtio-mmio windows out in
    the reverse of the order the devices are given, so the tablet came
    first - swapping two flags on the ARM command line would have broken it
    there identically. Finding and claiming are two calls now.
  * *A declaration left behind in a board's header.* `keyboard_getchar`
    moved into the shared `hal/virtio/input.c` and its declaration stayed
    in `hal/qemu-virt/qemu-virt.h`, where the second board cannot see it -
    so `hal/pc/uart.c` read only the 16550. The PC found a keyboard, said
    so, and could not be typed at.
  * *The screen size was never passed to this build.* `FB_FLAGS` sits on
    one file's compile line on ARM, for a good reason that does not apply
    to a build with no object files; left off, `ramfb.c` took its 1024x768
    fallback and the machine came up at a resolution nothing had asked for.

And two places where a *string* was the bug rather than the code: `/dev/cpu`
had no x86 decoder, so `devices` printed "nil nil nil, 1 core", and the same
listing said `/dev/console  PL011 UART, polled` on a machine whose console
is a 16550 at port 0x3f8. Both are the mistake the boot log already made and
this file already records, one layer up and in the userland.

`make KVM=1 x86` runs it on an x86-64 Linux host's own cores. It needs
`-cpu host` - KVM cannot pretend to be another processor - which is the same
trade `make fast` makes on ARM. It does nothing on an Apple Silicon Mac,
whose hypervisor virtualises the processor it is.

The order was the one this project used on ARM and it was worth repeating:
print, then exceptions, then the memory map, then the switch, then ring 3. A
kernel that cannot print is a kernel debugged by bisecting a hang, and an
exception handler that has never run is an exception handler that does not
work.

**The shape of the problem is different in one way that decides the rest.**
ARM hands you 64-bit execution and asks which privilege level you would
like. An x86 starts in 16-bit real mode, a multiboot loader has already put
it in 32-bit protected mode, and long mode has to be *built*: page tables, a
GDT with the L bit set, `CR4.PAE`, `EFER.LME`, `CR0.PG`, and a far jump - in
that order, before one 64-bit instruction may run.

**Three things were wrong on the first attempt and all three are worth
recording**, because each looks like something else when it fails:

  * *The page tables live in `.bss`, and `.bss` is zeroed by the kernel.*
    Zeroing it after building them walks over `pml4` while the processor is
    translating through it, and what kills the machine is not the write but
    the first TLB miss after it. Zeroing happens first now.
  * *`rdmsr` answers in EDX:EAX*, two 32-bit halves, always. The `"=A"`
    constraint means that pair on 32-bit x86 and means "rax or rdx,
    whichever" on x86-64, so asking for it reads half the register and looks
    like it worked.
  * *A multiboot loader parses ELF32.* This image is ELF64 and QEMU says so
    plainly. Bit 16 of the flags - the a.out kludge - makes the loader use
    five explicit addresses from the header instead of parsing the format,
    and then the file may be a flat binary and its bitness stops being the
    loader's business.

**`-mno-red-zone` is not a tuning flag.** The red zone is 128 bytes below
`rsp` that a leaf function may use without adjusting the stack pointer, and
an interrupt handler pushes straight over it. Nothing notices until there
are interrupts, which is the worst time to find out.

**`-mno-mmx -mno-sse -mno-sse2` are this architecture's
`-mgeneral-regs-only`**, and they are there for the same reason: the kernel
may not touch a floating-point register at all, and a flag turns that from a
promise into a compile error. It is what makes lazy FP save possible on
either machine - a kernel thread never faults because it never can.

### The interrupt controller has to move before interrupts are enabled

The 8259 delivers IRQ 0-15 as vectors 8-15 out of reset, and vectors 8 to 15
are *exceptions*: 8 is double fault, 13 general protection, 14 page fault. A
timer tick would arrive as a double fault, and the report would be an
accurate description of something that never happened. `hal/pc/pic.c`
remaps both chips to 32 and 40 before `sti` is ever executed.

`hal_irq_handle` takes no argument, and keeping it that way costs two port
accesses a tick. The processor already knew which vector it dispatched
through - it is in the trap frame - but the ARM side reads the GIC's IAR at
exactly this point, so asking the 8259 which line is in service means both
boards do the same thing in the same place and `arch/x86_64/trap.c` never
learns what a PIC is. Widening the HAL for one board's convenience would
have made the other invent a number to pass.

`hal_ticks_missed` returns 0 here and says why in the file. On ARM the timer
is one-shot, so a deadline already in the past is a comparison the re-arm
was making anyway; the PIT free-runs, and the 8259 collapses any number of
repeats into a single pending bit, so by the time the handler runs the
evidence is gone. The APIC has a proper answer, and this is one of the
things that will pay for moving to it.

### Paging: three differences that are not cosmetic

Four levels rather than three - long mode has no equivalent of the switch
that drops AArch64 to a 39-bit address - but the bottom three are ARM's L1,
L2 and L3 exactly, and everything Kosmos maps lives under 512 GB, so PML4
has one live entry. The extra level costs an address space a second page
rather than a different design.

  * **Permissions accumulate down the walk.** On ARM a table descriptor's
    attributes are optional restrictions and only the leaf normally speaks.
    On x86 a page is writable only if `RW` is set at all four levels, and
    reachable from ring 3 only if `US` is set at all four. So intermediate
    entries are permissive and the leaf carries the policy - any other
    arrangement stores one permission in four places that can disagree.
  * **`CR0.WP`, without which every read-only mapping is decoration.** Out
    of reset a ring 0 write to a page with `RW` clear *succeeds*: the
    permission is enforced against ring 3 only, and there is no AArch64
    counterpart to that at all. It was found by narrowing `.text` to
    read-only, reading the entry back correct, and watching a deliberate
    store go straight through. The map was right and the machine was
    ignoring it.
  * **One NX bit for two privilege levels**, where ARM has PXN and UXN. A
    page a process may execute the kernel may execute too. `CR4.SMEP` is the
    architecture's answer and `mmu.c` sets it where CPUID reports it, which
    buys the same guarantee with a control register instead of a descriptor
    bit. SMAP is deliberately off: the kernel reads user memory on every
    IPC, and doing that under SMAP means `STAC`/`CLAC` around each access -
    work that belongs with the syscall path.

And one thing the second architecture found in the first. The ARM side maps
the tail of RAM by dividing by 2 MB and letting the division truncate, which
is exact because `virt` reports a whole number of 2 MB pages. A PC does not,
so the x86 side maps the tail in 4 KB pages. **The ARM version is latent
rather than wrong** - it would need a machine reporting an odd amount of RAM
- and it is left alone rather than changed mid-port.

### The context switch, where the ABI asks for less

Seventy-two bytes against AArch64's six hundred and fifty-six, and the
difference is System V rather than the processor: it makes every XMM
register caller-saved, so a thread that *called* the switch has had the
compiler spill its floating point already. AAPCS64 keeps `d8`-`d15` across a
call, which is why the ARM switch has to have an answer for FP even in the
cooperative case.

There is no XMM state in `struct context` yet and that is a gap rather than
a difference: a *preempted* thread needs it on both machines. It is written
down instead of half-built, because the pieces only make sense together -
`CR0.TS` is the disarm, the fault is vector 7, which the trap table already
calls "device not available", and the handler shape is the one `fp.c`
settled on.

One stack moves rather than two. AArch64 gives a thread SP_EL0 and SP_EL1
and has to exchange a pair and record which was selected; an x86 interrupt
taken in ring 0 keeps the current stack and one taken in ring 3 loads RSP0
from the TSS. So the kernel stack a thread's exceptions land on is a TSS
field set when the thread is scheduled, not a register saved when it is
switched away.

`rip` cannot be moved to, so the restore pushes it onto the incoming
thread's own stack and returns - putting it back in the slot the outgoing
`call` took it out of.

### Ring 3, which needs a table AArch64 has no counterpart to

A privilege level on ARM is bits in PSTATE, and dropping to EL0 is an `eret`
with the right ones in SPSR. Here it is a property of a *segment
descriptor*, so `arch/x86_64/gdt.c` exists and `arch/aarch64/` has nothing
like it: five descriptors, in the one order the syscall pair will accept,
plus a task state segment holding the stack an entry into the kernel lands
on.

**The order is arithmetic rather than convention.** `syscall` takes CS from
`IA32_STAR[47:32]` and SS from that plus 8; `sysretq` takes SS from
`IA32_STAR[63:48]` plus 8 and CS from that plus 16. So the kernel pair is
adjacent one way, the user pair adjacent the other way, and 0x18 is a hole
only a 32-bit `sysretl` would load - which this kernel does not execute and
will not.

**`syscall` does not switch the stack.** That is the whole of what makes its
entry different from an exception's, and it is why `IA32_FMASK` has to clear
IF: there are two instructions where the kernel is running at ring 0 on a
stack the *process* chose, and an interrupt there would push a ring 0 frame
onto memory the process picked. AArch64 has no equivalent exposure - the
hardware selects SP_EL1 before the first instruction of the handler. The
kernel stack comes out of the TSS, which is where an interrupt from ring 3
would have got it too, so there is one answer to "which stack does an entry
land on" rather than two that can disagree.

**`gdt_init` runs before `mmu_init`, and that is load-bearing.** The
processor *writes* the accessed bit into a descriptor when a segment
register is loaded. `start.S`'s table is in `.rodata`, which `mmu_init`
narrows to read-only, so a descriptor with that bit still clear would fault
on the next interrupt, from inside the interrupt. The table `gdt_init`
builds is in `.bss` and writable, which removes the question rather than
relying on the first tick having already set the bit.

**`user_rsp` and the TSS are per-CPU state in everything but name**, and
`user.S` says so where `fp.c` says it about `owner`. With one core there is
one of each; with two, both belong in a per-CPU structure reached through
the GS base, which is what `swapgs` exists for and what this entry will
grow. x86 forces that structure earlier than ARM does, because `syscall`
hands the kernel no stack and there is nowhere else for the anchor to be.

### Endianness, which turned out to be free

Both targets are little-endian - AArch64 *can* be big-endian through
`SCTLR_EL1.EE` and Kosmos never sets it - but nothing here depends on that,
which was checked rather than assumed:

  * every `string.pack` in the on-disk format carries an explicit `<`, and a
    search for native-endian packs across the whole tree finds none. A disk
    written on one architecture reads on the other;
  * the network stack's `put16`/`get16` do explicit byte shifts, so they are
    big-endian by construction on any host;
  * the IPC structs never cross architectures - both ends of a message are
    processes on the same machine - so their layout is a local question.

### What is not done

**The six headers `kernel/` includes from `arch/` all exist now** -
`context.h`, `cpu.h`, `mmio.h`, `mmu.h`, `page.h`, `trap.h` - and
`kernel/pmm.c` compiles here unchanged. What is missing is everything above
them.

In order:

  * ~~The rest of `kernel/`.~~ **Done: all thirteen files compile for
    x86-64, with no `#ifdef` in any of them.** It took closing six places
    where AArch64 had leaked out of `arch/`, two of which a search for
    register names does not find - `process.c` decoding page descriptor
    bits in the check on every syscall pointer, and the boot log printing
    the literal string "MIDR_EL1". `docs/state.md` lists all six.
  * **Lazy FP**, as above.
  * ~~**A `hal/pc/` worth the name.**~~ **Done.** A framebuffer, a keyboard,
    a pointer, a block device, a network card, a sound device and a clock.
    The four virtio drivers did move across rather than being rewritten:
    they live in `hal/virtio/` now and each board brings its own transport,
    `hal/pc/virtio.c` walking PCI capabilities where `hal/qemu-virt/`
    reads fixed offsets from a window. Same sequence, every register
    somewhere else.
  * ~~**Preemption.**~~ **Done**, and it needed a call the ARM vector
    epilogue makes and `isr_common` did not: `thread_tick` only records.
  * **Trying it on a real PC.** `make KVM=1 x86` is there and the boot path
    exercises `CR4.SMEP` under `-cpu host`. Nothing has run on metal yet,
    and the machine to run it on is the same one the serial cable is
    waiting for.


---

## The distinction that matters

It is easy to blur, and blurring it leads to a useless abstraction.

**`arch/` is "which CPU are you".** Page table format, exception vector, context switch, memory barriers, privilege modes. **It is not abstracted across architectures, it is reimplemented.** Trying to abstract ARM and PowerPC page tables under a common interface produces an interface that is no good for either.

**`hal/` is "which peripheral do you have".** UART, interrupt controller, timer, framebuffer, storage. Common interface, one implementation per board. Here abstraction does work, because the operations are genuinely equivalent: pushing out a character is pushing out a character.

Two boards with the same CPU share `arch/` and differ in `hal/`. QEMU virt and the Pi 5 are both AArch64: they share all of `arch/aarch64/` and only the HAL changes.

---



## The display size, and changing it

`make FB=1920x1080` builds for that size. It is a compile-time constant
because there is no allocator: the pixels are a static array in a NOLOAD
section, so a bigger screen costs memory and nothing in the image file.

Nothing above the HAL needs telling. The console works out its rows and
columns from the geometry it is handed, `gfx.screen()` reports what the
kernel reports, and the window manager scales the pointer against the size
it was given - which is exactly why `hal_pointer_poll` hands out the
device's own range rather than screen coordinates.

**Changing it while the machine runs is a different question**, and worth
writing down before somebody assumes it follows for free. ramfb makes the
device side easy: the guest owns the configuration, so a new width, height
and stride written back through fw_cfg is a mode change, and QEMU resizes
its window. Three things do not follow:

- **The buffer is one fixed size.** A runtime mode would have to be capped
  at a maximum chosen at build time, and the array sized for that maximum
  rather than for the mode in use.
- **Everything holding geometry is holding a copy.** The console has its
  rows and columns, the window manager has a backbuffer that is a surface of
  a particular size, and every process that called `gfx.screen()` has a
  mapping of a particular length. A mode change is a message all of them
  have to be able to receive, which is a protocol that does not exist.
- **virtio-gpu does not work this way at all.** It is the target that earns
  the HAL a `hal_fb_flush`, and it is also the one where a mode set is a
  command to the device rather than a write to a config blob. Designing the
  runtime interface against ramfb alone would produce the shape of ramfb
  with a general name - the exact mistake `CLAUDE.md` warns about for the
  HAL as a whole.

So: build-time now, and the runtime version is worth doing after virtio-gpu
rather than before it, when there are two implementations to design against
instead of one.


## Two devices that are the same device

The keyboard and the tablet are both virtio-input. Both answer to device id
18. Nothing in the MMIO register window distinguishes them, so a driver that
scans for "the input device" finds whichever window comes first and calls it
a keyboard.

Telling them apart means asking each one what kinds of event it can produce:
write `VIRTIO_INPUT_CFG_EV_BITS` into the selector and `EV_ABS` into the
sub-selector, then read the length that comes back. Non-zero means the device
has absolute axes to describe, and a keyboard has none.

Two consequences worth knowing before touching `hal/qemu-virt/input.c`:

**Configuration space is bytes.** The first three fields of a virtio config
space are single bytes - selector, sub-selector, length - and reading them
as one 32-bit access works here and is allowed to fail on a device that
decodes the access width. `mmio_read8`/`mmio_write8` exist for this and for
nothing else.

**The probe costs a partial bring-up.** Configuration space is read after the
device has been told a driver is present, so a window holding the wrong kind
of device has been reset and acknowledged and then left alone. That is
harmless - claiming it later starts with another reset - and it is the price
of the two devices being indistinguishable from outside.

The file is instance-based for the same reason. It held one set of globals
when there was one device; the ring, the buffers and the position in the used
ring now belong to a `struct vinput`, because two devices of the same kind
cannot share them.


## The interface

Minimal on purpose. **Do not expand it speculatively.**

```c
void     hal_early_init(void);        // the minimum needed to have output
void     hal_putchar(char c);         // serial
void     hal_fb_init(struct fb *out); // address, w, h, pitch, format
void     hal_irq_init(void);
void     hal_timer_init(uint32_t hz);
uint64_t hal_ticks(void);

// M11. Frames, and nothing above them: no addresses, no protocols, no
// checksums. What an IP address means is not a driver's business, and a HAL
// that grew one would be a HAL with an opinion about the internet.
bool     hal_net_init(struct netdev *out);   // false when there is no card
bool     hal_net_send(const void *frame, unsigned bytes);
int      hal_net_recv(void *frame, unsigned max);   // 0 when nothing waiting
bool     hal_net_arrived(void);
bool     hal_net_present(void);
bool     hal_net_info(struct netdev *out);
```

`hal.h` is the authority and this list is a summary; where they disagree,
that file is right and this one is stale.

**And below the HAL, `virtio.c`.** Four devices on this board speak
virtio-mmio - input, block, sound, network - and the handshake is identical
for all four while the *rings* are not: block chains three descriptors and
waits, input hands the device empty buffers, sound has four queues with
different jobs. So the conversation is shared and the ring is not. That is
not a HAL interface - no other board implements it - and it lives in
`hal/qemu-virt/` for exactly that reason.

An abstraction written against a single target is always the shape of that target with generic names. The right interface appears at milestone 2, once there is a second real target in front of you. Until then, the only rule that matters is this:

**Zero MMIO addresses outside `hal/`.** Not one. That single rule gets you 80% of the way there without designing anything up front.

---

## Targets

### QEMU `virt` aarch64 — the development target

Where everything gets written. PL011, GIC and virtio: standard, documented, and with the serial console going straight to your terminal.

**The GIC version is not the default.** `-M virt` gives a **GICv2** (`arm,cortex-a15-gic` in its device tree, distributor at `0x08000000` and a memory-mapped CPU interface at `0x08010000`). GICv3 has to be asked for:

```
-M virt,gic-version=3
```

which moves the redistributors to `0x080a0000` and puts the CPU interface in system registers instead of MMIO. Kosmos drives a GICv3, so that flag is on the QEMU line in the Makefile and in `tools/run_tests.py`, and booting the image on a plain `-M virt` finds no redistributor and never receives an interrupt.

The fastest way to settle a question like this is to ask QEMU rather than to remember:

```
qemu-system-aarch64 -M virt -machine dumpdtb=virt.dtb
dtc -I dtb -O dts virt.dtb
```

The device tree also gives the RAM size and the timer's interrupt numbers.

Detail: **which exception level you get depends on the options, so read `CurrentEL` instead of assuming.** Measured against QEMU 9.1.2:

| Machine | Entry level |
|---|---|
| `-M virt` | EL1 |
| `-M virt,virtualization=on` | EL2 |
| `-M virt,secure=on` | EL3 |

The plain `-M virt` this project develops against lands at EL1, so the drop is not exercised there. Real hardware is the case that needs it: the Pi firmware hands off at EL2. `boot/start.S` handles both and parks on anything else, and `virtualization=on` is the way to exercise the EL2 path under QEMU.

The secondary cores start too. If you do not park them by checking the core ID in the entry code, you will see the output four times and not understand why.

### Raspberry Pi 1 — the argument, and why the answer is no

**Decided against in September 2026: Kosmos is 64-bit only.** The section
below was an argument *for* the Pi 1 as the first real hardware, and it is
kept rather than deleted because it was a good argument and the cost of
refusing it is real.

**What was in favour, and all of it still true:**

- **GPIO 14/15 are UART directly.** Three wires to a 3.3V adapter and you
  have a console. None of the Pi 5's problem.
- More bare-metal material than any other board in existence, and every
  BCM2835 register documented in twenty places.
- A single core. No temptation to do SMP, no races.
- The BCM2835 mailbox framebuffer is the canonical example, and the Pi 5
  uses the same conceptual interface. What you learn transfers.
- Boot by copying a file to the SD card. Iteration in seconds.
- QEMU emulates `raspi1ap`, so the port could be developed without the board.

**And the strongest point, which is what is being given up:** porting to
ARMv6 would force `arch/` and `hal/` genuinely apart. With two AArch64
targets that boundary stays fuzzy, because everything works the same way and
nothing punishes an assumption. With ARMv6 in the mix the assumptions
surface on their own - where 64 bits was assumed, where exception levels
were, where a GIC was.

**What decided it:** the Pi 1 is ARM1176JZF-S, ARMv6, 32-bit, with no
64-bit mode at all. Different instruction set, short-descriptor page tables
instead of long-descriptor, CPU modes with banked registers instead of
exception levels, a different toolchain. That is a complete second
`arch/armv6/` - the layout has always said architectures are *reimplemented*
rather than abstracted - and it would be written against a 700 MHz core
where compositing is slow, for a system whose stated question is whether it
can be made fast.

**And the thing it was for arrives anyway, from a better direction.**
x86-64 is intended at some point, and it is a second instruction set, a
different interrupt controller, a different boot protocol and a different
memory model. Every assumption the ARMv6 port was supposed to surface gets
surfaced by it - with none of the Pi 1's cost, because it is 64-bit, it is
fast, and the machines are already on the desk.

So the `arch/` boundary does get tested. It gets tested by a target that
does not require giving up the one constraint worth keeping.

Recorded here rather than deleted, because a rejected option with its
reasoning intact is worth more than a tidy document: if the reason changes,
the argument is already written.

### Raspberry Pi 5 — the main target

Notably more hostile to bare metal than the Pi 4.

**The wall:** nearly all I/O (USB, Ethernet, GPIO and the 40-pin header's UART) hangs off the **RP1**, a southbridge that talks over PCIe. To get a character out of the header UART you would have to bring up PCIe and write an RP1 driver before you even have a `printf`.

**The way out:** the dedicated debug UART connector, the **3-pin JST-SH** next to the HDMI. It goes straight to the BCM2712's UART without passing through RP1. That cable is the only lifeline for months.

Known traps:

- **The framebuffer pitch is almost never `width * 4`.** The firmware aligns it. Use the pitch the mailbox returns, or the image comes out skewed.
- **BGRA channel order**, not RGBA.
- **The framebuffer is mapped uncached.** Writing directly to it is 10-50x slower than to RAM. Always a cached backbuffer and a blit of the dirty rectangle at the end.
- **The Pi 5's MMIO addresses differ from the Pi 4's.** There is a lot of stale material floating around. Verify against the BCM2712 datasheet.
- **Which GIC the BCM2712 has is unverified.** Written down here as an open question rather than an assumption: QEMU's default turned out not to be the version these documents assumed, and the same guess about the Pi would cost a day. Settle it against the datasheet before writing a line of `hal/pi5/`.
- The framebuffer mailbox interface changed from the Pi 4.
- All four cores start at once and the firmware leaves them in a spin loop waiting for an address in a mailbox.

**Weak memory model.** AArch64 is far more aggressive than x86. Orderings x86 forgives fail here once every thousand boots. It is the most expensive class of bug in the project.

### NVIDIA Jetson — a cheap port

AArch64, same ISA. All of `kernel/`, `lua/`, `runtime/`, `servers/` and `apps/` port without touching anything. Only `hal/` changes. It is a week.

Check which one you have: the original Nano (2019) is a Tegra X1, Cortex-A57, GICv2. The Orin Nano is Cortex-A78AE, GICv3.

In favor: Tegra UARTs are 8250/NS16550, better documented than the PL011, and the header exposes a UART with no southbridge in the way.

Against: NVIDIA's boot chain is signed. BootROM, TegraBoot, USB flashing in recovery mode with SDK Manager. Compared to copying a file to an SD card, the iteration cycle is considerably worse, and that matters when you reboot two hundred times a day.

Milestone 3 or 4, once the HAL has been exercised.

### PowerPC G4 — the port that teaches the most

A Mac mini G4. Far out at the end, as a deliberate exercise.

**What makes it valuable: it is big-endian.** Every other target is little-endian. For Kosmos that lands in one precise place: the Lua table serializer for IPC. Today you can write a `uint32_t` and read it on the other side without thinking. With big-endian in the mix, you have to define an explicit wire format and honor it. That discipline never develops if all your targets agree.

**Open Firmware solves the console.** It stays alive after loading your kernel and exposes a client interface. `putchar` is an OF call. And its device tree has the framebuffer already configured, with address, width and pitch. It is the smoothest bring-up of any target, with no driver to write.

**The cost: the MMU is another world.** PowerPC uses segment registers plus an inverted hashed page table. It is not a radix tree like ARM or x86: you do not choose where the entry goes, you hash the virtual address and the hardware searches a bucket. Collisions and evictions are your responsibility. It is a genuinely different model with no analogy to what you already know.

QEMU emulates `mac99`, so the port can be developed without touching the hardware.

### x86-64 — not on the Mac Pro 2013

**The Mac Pro 2013 has no serial output. None.** No port, no header. To get a character out you would need a full USB stack before having `printf`. It is the RP1 problem without the back door.

Without debug output, the project does not exist.

Add Apple's EFI, all I/O over Thunderbolt, no SD card, and two FirePros. It is the worst possible x86 machine for this.

The irony is that **x86-64 in general is a better learning target than ARM**: the OSDev wiki is written for x86, there is far more material, and the TSO memory model saves you half the barrier bugs. The problem is not x86, it is that machine.

If you ever want x86, get a cheap mini-PC **with a serial header**, and treat it as a deliberate exercise.

---

### x86-64 — the second architecture, not a second board

**Intended, unscheduled, and the first thing that will be a genuine
`arch/` rather than a `hal/`.** Worth writing down now because it changes
what "keep the layers apart" is protecting: with two AArch64 targets the
boundary is a promise, and with x86-64 it becomes a compile error.

**What is a second `arch/x86_64/`, and it is most of one:**

- Page tables. Four levels, different bits, one root in `CR3` rather than
  the `TTBR0`/`TTBR1` split - so the "everything below `USER_VA_BASE` is the
  kernel" arrangement is expressed by a canonical-half convention instead of
  by hardware.
- Exception and interrupt entry. An IDT, not a sixteen-entry vector table.
- Context switch, and a different calling convention under `setjmp`.
- The syscall path: `syscall`/`sysret` rather than `svc`.
- Barriers, which mostly become *nothing*: x86-64 is strongly ordered, so
  `mmio_read32`/`mmio_write32` lose their `dmb` and keep their shape. That is
  the accessor pair earning its existence a second time.
- **`-mno-red-zone`, and it is not optional.** The SysV ABI lets a leaf
  function use 128 bytes below the stack pointer without adjusting it, and an
  interrupt arriving on that stack writes straight through them. It is the
  x86-64 equivalent of `-mgeneral-regs-only`: a rule turned into a flag,
  where forgetting it produces corruption rather than an error. `-mno-sse
  -mno-mmx -mno-80387` go with it, for the same reason the kernel may not
  touch an FP register here.
- Lazy FP save translates directly: `CR0.TS` and `xsave` play the part
  `CPACR_EL1.FPEN` plays now.

**What is a `hal/x86_64-pc/` and nothing more:** the interrupt controller
(local APIC and IO-APIC, or MSI, where the GIC is now), the timer (the APIC
timer or HPET, where the generic timer is now), the console UART, and the
framebuffer - which under UEFI arrives as a linear buffer the firmware
chose, which is exactly the shape `hal_fb_init` already has.

**What does not change, and this is the whole point of the microkernel:**
everything above the kernel. Lua, the servers, the namespace, the window
manager, the applications. A Lua server neither knows nor cares which
instruction set it is running on, and the day that turns out to be false
will be worth knowing about.


## The rule for choosing a target

**If you cannot get a character out over serial in the first two hours, it is not a target.**

Everything else can be solved with time. That cannot.

---

## Recommended order

**QEMU virt → Pi 1 → Pi 5 → Jetson → G4.**

From easiest to debug to hardest, not the other way around.

Each jump breaks a different assumption. The Pi 1 breaks "64 bits and exception levels". The G4 breaks "little-endian" and "page tables are a tree I control". That last one teaches the most about which part of the design was essential and which part was an assumption about the hardware.

None of them before milestone 8, except the first port at milestone 2. Porting to a foreign architecture before the design is proven makes you generalize over assumptions you do not yet know are correct.
