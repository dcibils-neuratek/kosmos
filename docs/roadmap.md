# Built, and the wishlist

**There are no milestones any more.** There were thirteen of them, numbered,
each with a definition of done, and the rule was that one did not start
until the previous one met its criterion. That was the right shape for a
kernel that did not boot yet. It stopped being the right shape somewhere
around the point where the order was last revised by hand, and the numbers
went on being quoted long after they had stopped describing anything: the
README said "M6, graphics" while the machine had a journalled filesystem, a
TCP/IP stack and a web browser in it.

A number that no longer means anything is worse than no number, because it
is read as though it does. So this is two lists: what exists, and what is
wanted. Nothing here is scheduled, and the order things get built in is
decided by what is interesting and what unblocks the most.

---

## What is built

**The kernel — Nebula.** Threads, address spaces, IPC and capabilities, and
nothing else. No allocator anywhere in it. 39-bit virtual addresses through
three levels of long-descriptor tables, exceptions and a vector table,
a timer, the GIC. Strict priority bands with immediate preemption, and lazy
FP save — a thread that never touches a floating-point register never pays
for one, which is every kernel thread by construction.

**Lua at EL0.** One `lua_State` per process, upstream Lua unmodified, a
freestanding libc under it, and a serialiser for messages.

**Namespaces**, per process, with no global names. What you were not handed,
you cannot name — a program without `/dev` does not get permission denied,
the path does not exist for it.

**Servers**, each a process behind its own address space: the program store,
the library store, the application registry, devices, a RAM filesystem, the
console, audio, the network, and the disk filesystem.

**A filesystem of its own**, with a journal — checked against power loss by
killing the machine at an exact instant rather than hoping. Attributes on
files, and live queries over them.

**Graphics.** A framebuffer, surfaces with C pixel loops, outline fonts
rasterised on demand from TrueType, UTF-8 throughout, and a compositor.
Windows are drawing commands by default and a shared double-buffered
surface when an application asks — which is what lets one draw a page at
twenty-eight pixels and a paragraph at sixteen.

**A desktop.** A window manager with menus, resizing, scrollbars and
stacking; a widget kit; themes; the Deskbar; Tracker; a terminal; and
replicants — a view published by one process and adopted by another.

**Networking.** virtio-net, ARP, IP, ICMP and TCP with shared rings, an
HTTP server, a telnet client. It reaches the real internet through QEMU's
NAT.

**Audio.** virtio-snd, a mixer, WAV and MP3.

**Applications.** A PDF reader, a paint program, a text editor, a photo
viewer, a calculator, a music player, a file manager, a process list, a
system monitor — forty-one of them.

**Software 3D**, through TinyGL, and Doom.

**A web browser.** hubbub, libdom and libcss running at EL0: HTML parsed
into a DOM, the CSS cascade run with a user-agent stylesheet, text laid out
into boxes that survive the paint, and links you can click.

**Tooling.** `make test`, `make screenshot`, `make bench`, `make stress`,
`make browser`, `make release`, and one command that downloads the newest
image and runs it.

---

## The wishlist

Not in order, and nothing here is promised. Roughly by how much each one
unblocks.

### Being built now

**A resolver.** An address is four numbers today, which is the single
biggest thing between the browser and the web. UDP exists in the stack only
as much as DNS needs — no sockets, because nothing else has asked for any.

### Next

**A non-blocking send.** Half a browser frame is the application blocked on
a `commit` whose handler swaps an index and records a rectangle. `SYS_CALL`,
`SYS_RECEIVE` and `SYS_REPLY` are the whole IPC surface and every message to
a server blocks for its reply by construction, so triple buffering cannot
fix it. This is a change to the IPC model — a syscall, a fixed-size queue in
the endpoint struct, a decision about what a full queue does, and
backpressure.

**x86-64, under QEMU — *done, and it boots to a shell*.** A second
architecture: a different instruction set, a different interrupt controller,
a different boot protocol, a different memory model. The 64-bit line was
drawn where it was precisely so this would not be a refactor, and it was
not: all thirteen `kernel/*.c` compile for it with no `#ifdef`, and the
whole userland — fifty thousand lines of servers, libraries, applications
and Lua — needed about a hundred and ten lines of assembly and one `#if`.

What is left there is `hal/pc/`: a framebuffer, a keyboard, a pointer and
virtio over PCI, which is a different way of *finding* the same devices
QEMU gives the ARM board. `docs/hal.md` has the differences that were not
cosmetic.

**SMP, and real parallelism.** The next thing, and the port just paid for
part of it in advance. Two pieces of state on x86-64 are per-CPU rather
than per-thread — the TSS holding the stack an entry from ring 3 lands on,
and whoever owns the floating-point registers — and finding the boundary
was not theoretical: a global holding the interrupted stack pointer looked
like per-CPU state, was actually per-*thread*, and produced a process
returning to user level on another process's stack. One core was enough to
prove it wrong. `swapgs` and the GS base are what x86 replaces that global
with; `TPIDR_EL1` is ARM's, and neither is written yet.

**PowerPC, on a G5 or a G4 iMac.** Wanted, and it changes two things this
project has written down as settled.

*It is big-endian*, and would be the first. The audit in `docs/hal.md` was
done before there was any reason to need it and it holds: every
`string.pack` in the on-disk format carries an explicit `<`, a tree-wide
search for native-endian packs finds none, and the network stack builds
big-endian by construction with byte shifts rather than by relying on the
host. What would have to move is `runtime/include/endian.h`, which today
says little-endian because both targets are — it becomes per-architecture,
which is one file and the reason it exists.

*The G4 is 32-bit*, and `CLAUDE.md` says Kosmos is 64-bit and only 64-bit —
"a 32-bit machine is neither, because it is out of scope". That is a
principle in the way, and the rule for those is to say which one, say what
it costs, and decide deliberately rather than drift. The G5 (PPC970) is
64-bit and needs none of that conversation. A G4 does: `runtime/include/`
has two files that assert 64-bit outright — `inttypes.h`, whose `PRIu64` is
`"lu"` because both targets are LP64, and `math.h`, which `#error`s on the
`FLT_EVAL_METHOD` a 32-bit x87 machine reports. Neither is hard; both are
places the decision would have to be made on purpose.

**Real hardware**, a Raspberry Pi 5. Chosen because it is hard: a desktop
that feels fast on it is a result rather than an emulator number. Every
performance question in this project is currently answered "measure it on
the Pi", and the Pi is not here yet.

### The browser

- **A box model.** Margins, padding, borders, `width`, floats. Images and
  forms both wait on it.
- **Images.** `stb_image` is already vendored for the PDF reader.
- **Forms**, which need a box that takes keys.
- **TLS**, without which most of the web refuses to speak.

### The system

- **SMP.** Moved up to *Next* — see there. Single core today, but written
  SMP-ready: no loose mutable globals, a per-CPU pointer, a per-CPU
  runqueue with one CPU in it.
- **An ELF loader**, so a program can be loaded rather than compiled in.
- **SSH**, in layers with test vectors at each: the binary packet protocol,
  Curve25519, ChaCha20-Poly1305, userauth, channels. The one place here
  where a bug is *silent* — a wrong nonce keeps working and is not secure —
  so "it connected" is not evidence.
- **Get Info**, and editing attributes from the desktop. The query engine
  works and nothing writes an attribute, so the search box finds only what
  `attr` put there.
- **A name in the query index.** `name:*.png` cannot be a query today, so
  the search box filters locally instead. BeOS indexed `name` precisely so
  it could be one.
- **File types**, as a preferences application rather than a table compiled
  into the image.

### Smaller, and wanted

- Doom's sound, behind a hook that already exists.
- An equaliser in the mixer — the first thing that will want the ring to
  carry something other than what was written to it.
- Seeking in the music player: the bar is drawn and cannot be dragged.
- A markdown viewer, for manuals inside the system.

---

## Known and unexplained

**Three to four audio underruns per 2.3 seconds.** Six structural changes
did not move it: the ring, the priority band, an eight-times buffer, the
interrupt, the C rewrite, and measuring during play rather than after. 194
interrupts per 400 periods says the device services about two periods per
raise, which is QEMU's model rather than this one. The next measurement
wants real hardware.

**One kernel check failed once**, 1 of 127, and has not since. The evidence
was destroyed by a `grep` that kept only the summary line. Recorded as
intermittent rather than fixed, because nothing fixed it.

---

## What to do when you get stuck

Every piece of this has a point where something does not work and you do not know why. It is normal and it is half the learning.

The order that works:

1. Instrument over UART until you find the last line that executed
2. Check memory barriers and cache maintenance
3. Verify the MMIO address against the datasheet, not against what you remember
4. Compare with QEMU if the bug is on hardware, or the other way around
5. Reduce to the smallest thing that reproduces the problem

And if something stretches past what is tolerable, **the way out is to cut
scope, not to abandon**. A desktop without SMP is still a desktop, and it
was one for a year.
