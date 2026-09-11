# State

**Update at the end of every session.** This file is what keeps you from starting over each time.

Last updated: 2026-09-11

---

## Where this left off

### A disk on the USB stick, for the T14's games

Doom did not run on the T14 from `make MEGA=1 usb`, and Doom was not the
problem: the image carried it, and the stick carried no WAD. Under QEMU
`make qemu` attaches `build/kosmos.img`, the disk `make image FILES=...`
wrote; the ThinkPad boots from a stick that held GRUB and the kernel and
nothing else.

So the stick carries the disk now. `make x86-usb-image` copies
`build/kosmos.img` onto it when `kfs.lua` can read it, GRUB loads it into
memory as a module, and `hal/pc/memdisk.c` presents that memory as the
board's disk - ahead of the NVMe drive, whose `/home` is not mounted on such a
boot. The kernel reads the module from both loader protocols, keeps its pages
away from the allocator, and names it in the boot log. Under QEMU the new
check boots with an 8 MB disk handed over as a module and an empty NVMe drive
beside it: the allocator's region starts above the module, `diskinfo`
reports 16384 sectors, and a file written into the image on this Mac comes
back. With the module's pages left to the allocator, it fails.

    make image FILES="doom1.wad:/home/doom1.wad pak0.pak:/home/id1/pak0.pak"
    make MEGA=1 usb

Under OVMF - the firmware a ThinkPad's is built from - a USB image made with
`mkusb_image.py --disk` boots as a USB stick, GRUB loads the disk with
`module2`, and the boot log says `a disk from the loader: 8192 KB` with RAM
starting above it. On the x86-64 MEGA image, with a 64 MB disk of
`doom1.wad` and `pak0.pak` handed over the same way, Quake starts, plays
`demo1.dem` in e1m3 and draws, and Doom opens and draws - the first time
Quake has run on x86-64.

**And on the T14 itself, both run off the stick.** Booted on 11 September
from a stick `make MEGA=1 usb` wrote, with the WAD and the pak in the disk
GRUB loaded: Doom playing E1M1 and Quake at its main menu, in windows side
by side; Tracker listing `doom1.wad` and `id1/` in `/home`; Monitor with all
eight processors. `/home` also held the `Desktop` folder Tracker makes when
it is missing, so the machine wrote to the loader's disk as well as read it. The
watermark read 0.10.26, because the bump came after; nothing else changed.

Open:

- **Writes to the loader's disk are lost at power-off.** A USB storage driver
  is what makes the stick itself writable.
- **The disk is limited by the device window**, 256 MB shared with the
  framebuffer and every controller's registers.

**Next, as queued**: Tracker's desktop - Haiku's icons, icons on the app menu,
desktop icons below the top bar and draggable - then a launcher option to
scale Doom and Quake, then the app profiler.

### Whose work is in it, and everything in one image

About Kosmos ends with a Licences section: the project's own terms, then every
vendored component - who made it, its licence, where the text lives - read out
of the image's own copy of `LICENSE`. `user/lib/licences.lua` reads the file
and `tools/test_licences.lua` holds it to the tree, and that test found
`LICENSE` three entries short: musl's maths, NetSurf's five libraries and the
Tango icons. It draws: a MEGA image scrolled to the end of About shows the
doomgeneric and Chocolate Quake entries, bold headings over their details.

`LICENSE` also said Doom was not in an ordinary image, and it was. Diego
decided the text follows the Makefile: `FULL=1` builds the whole system, so an
ordinary image is a GPLv2 work, and `FULL=0` is the MIT one. Doom's README,
the Makefile's `FULL` comment, `CLAUDE.md` and four comments in the tree say
the same now.

`make MEGA=1 qemu` builds and runs everything this tree can put in one image -
Doom, the browser, Lite XL and Quake. Its first link failed on twenty-four
names Doom and Quake both define; Quake's copies are renamed in its forced
header. The image is 8.64 MB of the sixteen before a process's heap, which
`user/user.ld` now asserts at link time. On a MEGA image `run_litexl.py`
passes its 6 checks, `run_quake.py` its 6 and the display harness its 71,
and Doom opens and draws beside Quake. The game data still goes on the disk:

    make image FILES="doom1.wad:/home/doom1.wad pak0.pak:/home/id1/pak0.pak"
    make MEGA=1 qemu

Open:

- **Two assets have no licence**: `assets/images/test-pattern.png` and
  `assets/kosmos-ascii-art.txt`, which the build warns about on every run.
- **MEGA on x86-64 has not been built.**

**Next, in the order agreed**: the battery indicator on the top bar, which
starts with the T14's DSDT.

### Quake runs on Kosmos

`wm quake` opens a window and plays the shareware attract loop: `demo1.dem` in
e1m3, the Necropolis, drawn through the palette at twice its 320 by 240, and
Escape brings up the main menu. Chocolate Quake at `edb8209`, vendored
unmodified under `runtime/upstream/quake/`, with Kosmos's platform in
`user/lib/quake_kosmos.c` and the loop in `user/bin/quake.lua`. Only in an
image built with `make QUAKE=1`, which `FULL=1` does not include.

Not quakegeneric, which the plan named: it builds only for 32-bit machines.
`runtime/upstream/quake/README.kosmos.md` has that and everything the port
took.

It asked two things of the rest of Kosmos, both decisions in the README:

- **A region may be 32 MB.** `MEMOBJ_PAGES_MAX` was 16 MB and the pak is
  18.3 MB, so it came back "no room". "mem: a region the size of Quake's
  pak" holds it. With `MEMOBJ_PAGES_MAX` put back to 16 MB, the region test
  is the one of 146 that fails.
- **The engine has a stack of its own.** `R_EdgeDrawing` is a 205 KB frame
  and a process's stack is 256 KB, so the first frame ran off onto the guard
  page. The engine runs on a megabyte of mapped pages with a guard page under
  it, through `kosmos_call_on_stack`, and no other process's stack changed.

And one of the libc: the scanner moved to `runtime/libc/scan.c` to give Quake
`fscanf`, and moving it found `%f` storing a `double` where the standard says
`float`. `tools/test_scan.c`, 19 checks, with a negative control.

`make quake-check PAK=/path/to/pak0.pak` is the check on the machine. Its
first run passed all 6. With `quake.lua` withholding keys from the engine it
fails at the console command, 0 of 2, so what that check counts is keys Quake
received rather than an echo from somewhere else. It is not in
`make prepush`, because the pak is not in the repository.

**The gate found a race in the display harness on the way.** Its clipboard
phase clicked where the gallery asks to open, and the window manager moves
the gallery when the report gets its window first; it failed four runs in a
row, and a clean 0.10.24 fails it too. The phase now reads where both windows
were placed, and passes with either one first. `docs/testing.md` §18.16 has
how that was established, including the comparison that looked like proof
and was not.

Open:

- **No sound.** `SNDDMA_Init` finds no device; `/dev/audio` is next.
- **Looking around is a drag.** The window manager reports pointer movement
  only while the button is held; a relative mode would fix it.
- **Nothing is written.** `config.cfg`, saves and the `-condebug` log are
  refused by the libc, and a question Quake asks inside a frame - a new game
  over a running one - is answered no.
- **`LICENSE` and `FULL=1` still disagree about Doom**, unchanged; Quake is
  outside `FULL=1` and does not add to it.

**Next, in the order agreed**: a battery indicator on the top bar, which
starts with the T14's DSDT. `docs/roadmap.md` has it.

### Lite XL in its own faces, and in the gate

A `LITEXL=1` image carries `FiraSans-Regular.ttf` and `icons.ttf` now, and the
editor draws in both: Fira Sans for its interface, and icons where there were
letters - folders and chevrons in the tree view, the toolbar under it, a tab's
close button, the status bar. They sit in a table of their own,
`litexl_fonts_table`, which only `provide_image_font` reads; in `fonts_table`,
`gfx.fonts()` would offer them and Appearance would list `icons` as a face for
the desktop. The stand-ins, `provide_font` and the `/home/fonts` fallback went
with the reason for them.

Their terms are recorded beside them in `runtime/upstream/lite-xl/data/fonts/`.
`LICENSE.FiraSans` is transcribed from Lite XL's own `licenses/licenses.md`:
SIL OFL 1.1. `LICENSE.icons` is Kosmos's, because upstream wrote nothing for
that file, and it gives its evidence with the terms - Fontello in the font's
name table, a Lite XL maintainer saying the icons are Font Awesome 4, whose
fonts are SIL OFL 1.1, and five glyphs named for Lite XL. **That is the one
licence here resting on somebody's word rather than on a notice**, and the
file says so.

`make prepush` runs `make litexl-check`, which also checks now that the
launcher took both faces from the image: 6 checks. With `icons.ttf` left out
of the table it exits 1 - at the start check, because Lite XL cannot load
`core/style.lua` without its icon font.

### Lite XL edits and saves on Kosmos

`wm litexl:/home/notes.txt` opens a window with the file in a tab and the
tree view beside it; typing reaches the document, Control-S saves it, and
`head` at the prompt afterwards reads back what was typed. Control-N makes a
new document. `docs/litexl.md`, "Step seven", has what it took: Lite XL's own
`core.run()` with Kosmos inside its waits, keys in SDL's order, `io.open` over
the namespace, the installed tree worked out from `/lib`'s keys, and the
direct window's two buffers kept in step.

Only in an image built with `make LITEXL=1`. Two checks hold it:
`tools/test_litexl_host.lua` in `make test` - 34 checks on the launcher's
decisions, with a negative control for the fault below - and
`make litexl-check`, which boots the image twice and reads each saved file
back at the prompt. Its first run passed: 5 checks, both boots in
1 min 49 s under TCG.

**The fault**: a queue that consumed events by clearing slots let its count
go below zero once drained, and the next events - a Control release among
them - were written where nothing read them, so `l` and `o` reached the
editor as `ctrl+l` and `ctrl+o`. Found with `wm litexl:--trace`, which prints
every key, event, command and log line.

Open:

- **Control-C** stops the desktop before a window sees it, so the editor
  copies with Control-W and `c`.

**Next, in the order agreed**: Quake, then a battery indicator on the top bar,
which starts with the T14's DSDT. `docs/roadmap.md` has both.

### New threads on every core by default, after the ThinkPad ran on eight

**The T14 ran 0.10.20 spread across all eight of its processors.** Booted
with `opt/kosmos/smp=8` on GRUB's `multiboot2` line: all eight alive,
Processes showing threads homed on cores 0 to 7 with 31 of 48 thread slots
in use, and four software-rasterised demos at once - Cube at 153 frames a
second, Bounce at 150, Gears and Mech at 61. Monitor read 60, 0, 57, 0, 25,
0, 2 and 6 percent across the eight.

**And the pointer is smooth**, with Processes and Monitor open, where 0.10.19
lagged and jumped. Which change did it is not isolated: 0.10.20 stopped
`SYS_SYSINFO` re-initialising the i8042, locked the controller, and spread
the desktop's work across eight cores - under QEMU a click waited 35 to 48 ms
behind one busy core and 1 ms spread. One boot with only the
re-initialisation put back would say whether that was the jumps.

**So placement is on by default now.** `kmain` no longer calls
`thread_place_across(1)` when there is no boot option, and a plain boot
homes each new thread on the least busy processor that arrived.
`opt/kosmos/smp=N` stays as a way to narrow - `SMPWORK=1` puts everything
on core zero for when something breaks - and is capped at the processors
that arrived, which it was not: `smp=8` on four processors would have homed
threads on four that never started.

Two guest checks hold it. `smp: new threads go to every core by default`
reads the count before the suite pins itself to core zero, and `cpu: every
processor claimed its own slot` now asks for every slot and requires the
answer to be the cores that arrived - under the suite's pin that comparison
was one against four and could not fail. With `kmain`'s old default and no cap put back, both fail on each board and nothing else does. `run_x86.py` requires a plain
boot to say `4 of them given new threads`, and `opt/kosmos/smp=2` on the
command line to narrow it; `smp=4` would have passed there with the option
never read.

**The uneven Monitor is how placement chooses, not a fault**: it counts the
threads runnable at the instant one is created, so a demo waiting for its
next frame counts as idle and a later one can land beside it. Seeing work
rather than a moment is the next question for placement.

**And `make stress` booted four processors for the first time, and failed.**
It had passed QEMU no `-smp`, so the release gate had only ever stressed a
one-processor machine. On four, every snapshot counted one thread and about
650 pages more than the machine held at the end, and the end matched a
one-processor run exactly - nothing leaked. Two causes, one in the driver
and one in the kernel:

- **`run` returns when the program replies, before its process has
  exited**, and `stress.lua` counted at once. On one core the child was gone
  by the time the driver ran again; on four it was still leaving on another
  core, and a non-blocking drain cannot wait for it. The driver waits for
  each child now.
- **That wait could not be trusted as it was.** `process_exit` set `exited`
  on its first line, before giving anything back, and woke the parent with
  nothing held; `process_wait` scanned, named itself the waiter and blocked
  with nothing held either. On a spread machine a parent could reap a slot
  its owner was still tearing down - and the next spawn be handed it - or
  sleep through a wake that landed before it blocked, which is the
  `sys.wait` the shell makes after every foreground command. `exited`, the
  exit code and the cleared thread are published last now, under
  `processes_lock`, with the wake; the wait scans, reaps and blocks under it
  through `thread_block_and_release`, as IPC does.

`smp: a parent waits for children on other cores` holds it: a thread of its
own waits a hundred times for a child placed on another core and spawns the
next the moment each wait returns. With `process.c` as 0.10.21 had it, both boards panic on the second child - `pmm_free_page: address is below RAM`, the shape of a teardown reading a slot already reaped and handed to the next child. With the fix and the waiting driver, `make stress` passes on four processors: sixty rounds, threads 20 to 20, processes 12 to 12, and 117,141 pages free before and after.

### Nine vendored files no commit had, and the unanchored `build/` that hid them

Found during the audio ring work by making a worktree - the first thing in a
while to start from what was actually committed - and recorded then as not
fixed, because it did not belong in an audio change. Fixed in 0.10.21, which
was to have been 0.10.20 until the TLB shootdown landed with that number
first.

`.gitignore` line 1 was `build/`. Without a leading slash git reads that as a
name rather than a path, and matches a directory called `build` at any depth;
NetSurf's libparserutils, libhubbub and libcss each ship one, and it holds
sources. So nine files were in no commit from 7e56bca, the day NetSurf was
vendored, and nothing could say so: the checkout that vendored them still had
them, and a pattern that keeps a file out of a commit keeps it out of
`git status` as well. In a fresh worktree both perl generators failed, not
only the one first noticed - `make` stops at the first, and `make -k` shows
the second:

    cp: runtime/upstream/netsurf/libparserutils/build/Aliases: No such file or directory
    cp: runtime/upstream/netsurf/libparserutils/build/make-aliases.pl: No such file or directory
    make: *** [build/gen-doom-web/netsurf/aliases.inc] Error 1
    cp: runtime/upstream/netsurf/libhubbub/build/Entities: No such file or directory
    cp: runtime/upstream/netsurf/libhubbub/build/make-entities.pl: No such file or directory
    make: *** [build/gen-doom-web/netsurf/entities.inc] Error 1

Two problems in one, as the note that found it put it. The published path was
broken, since `make release` builds the full image and `builds/` is what
somebody downloads - and `make test` and `make prepush` build that image too,
which a dry run of `make test` confirms. And part of three vendored libraries
was silently not in the tree, which is the one thing the vendoring rule exists
to prevent.

- **The rule is `/build/`**, with the reason written above it, and
  `build/kosmos.img` is gone: it was inside `build/` from the day 205a294
  added it.
- **What else anchoring changed was measured rather than assumed.** Both
  rule sets were run over the main checkout's whole tree with
  `git ls-files --others --ignored --exclude-from`, which reads a tree
  without touching its index: 6,382 ignored paths before, 6,373 after, and
  the nine are the whole difference. Nothing named `build` exists under
  `lua/upstream/` or `assets/`.
- **The nine are as released.** They are byte-identical to the working
  checkout's copies, and those carry the release date, 27 December 2023 - as
  do all 4,036 files of the vendored tree except the README this project
  wrote. Three are executable and are committed so. Only `Aliases`,
  `Entities` and the two scripts that read them are used here; `Doxyfile`,
  `conv.pl`, `get-entities/` and libcss's `mkprops.pl` are in the tree
  anyway, because the rule is what was released and not what is used.
- **Proved from a checkout with nothing else to find.** A scratch worktree
  of the commit, made outside the repository's directory so that no tool
  could reach a working checkout by walking up into one, ran `make prepush`
  from nothing - once before the rebase onto the TLB shootdown, and again
  on this revision's own commit after it. Both times the generators ran
  from the committed inputs, every suite passed with the same counts as the
  working tree, and afterwards `git status --ignored` showed `build/`,
  `tools/__pycache__/` and the picture `make shot` takes, and nothing else.

**It had been fixed once already, in a commit that never reached main.**
`65fe147` on the worktree branch `claude/nice-torvalds-701f77` is this change
with the same nine blobs, made on 8 September and numbered 0.9.33; main took
0.9.33 for Lite XL thirty-seven minutes later, and nothing landed the branch
after that. A version number is a lock nobody holds - two sessions can take
the same one, and the one that loses is not told.

**The same shape is still there, and hides nothing today.** `*.o`, `*.d`,
`*.elf`, `*.bin` and `*.map` are names too, matched at any depth. The
comparison above says none of them hides a file now; a vendored `.bin` or
`.map` would vanish exactly as these did, and only a fresh clone would notice.

### The first machine runs the desktop, and the last two faults were one missing port

The ThinkPad T14 boots to a desktop that draws, ticks and follows the
TrackPoint. Getting there was a chain of faults, each hiding the next, and
every one of them is in the decision log with the evidence that found it:

- **The compositor's mapping of the framebuffer had the wrong memory type**
  (0.10.16): write-back against a PCIe framebuffer, one bus transaction per
  four bytes.
- **Every application blocked in its first poll.** The window manager's
  replies were larger than a message and the `pcall` around `sys.reply`
  threw the error away. Twelve events a reply now, and a failed reply is
  logged.
- **Tracker's sidebar asked every mount for a listing at startup**, and
  `/net` on a machine with no network card took 18.4 seconds to answer.
  Tracker is the desktop, so the desktop took eighteen seconds to exist.
- **The TrackPoint said nothing, then decoded as noise.** A configuration
  byte that kept the auxiliary port shut, and a framing check that accepted
  almost every negative delta as a packet header. The controller is
  interrupt-driven as well as polled now, and a resync logs what it throws
  away.
- **A core at 100%, and buttons that went down and never came up** - one
  fault. The T14 has no COM1; a port nothing answers reads 0xFF, and 0xFF in
  a 16550's line status register says a byte has arrived, so the console
  read phantom input for ever and the window manager posted it as keys into
  the focused window's queue. Reproduced under QEMU with `-serial none`,
  fixed by asking the scratch register, and kept fixed by a gate boot with no
  serial port that asks QEMU whether the processor ever halts.

**The x86 gate boots the machine a laptop is.** `tools/run_x86.py` clicks the
Deskbar open through QEMU's PS/2 mouse - which streams now, after months of
sending nothing - on the 8259, with a serial port and without one.

### Eight processors, and the desktop spread across them

**The T14 starts its other cores.** `log processor` on the diagnostic
build: processors 1 to 3 at APIC ids 2, 4 and 6 - its physical cores - each
reaching the kernel a millisecond after its second STARTUP. There are eight
slots now, so the next boot starts all seven.

**Threads can be spread on x86, and three things stopped a spread desktop
first:**

- **No TLB shootdown.** A translation changed on one core stayed cached on
  every other. `arch/x86_64/mmu.c` now follows every change to a live
  mapping with a round of IPIs, and a core waiting for a lock answers it.
- **Drivers written for one core.** The i8042 said so in a comment; PCI's
  index/data pair, the CMOS clock, fw_cfg, the UART and NVMe had the same
  shape. All of them lock now.
- **A reply lost between cores.** `ipc_call` woke its receiver before joining
  the reply queue, so a receiver on another core that answered at once had
  its reply refused and the caller waited for ever. With it fixed the ARM
  display harness passes under `SMPWORK=4`; with the old order put back its
  editor phase fails again, so this was the failure that kept placement off.

**And one the gate found.** A thread exiting on one core could have its slot
handed to a thread being created on another before the exit's switch was
over, and the new thread started life inside `thread_exit`: `a dead thread
was scheduled`, once in eleven x86 suite runs. A slot is reused only once its
processor has left it now, and a guest test that forces the window panics
without the fix in two runs of two on each board.

**And a fault that hurt the laptop on one core.** `SYS_SYSINFO` asked the
i8042 whether there was a keyboard by re-initialising it - several times a
second whenever Processes, Monitor or the top bar were open: both ports
disabled, the waiting bytes thrown away, the configuration byte rewritten.
On real hardware that cuts mouse packets in half, a pointer that jumps, and
QEMU - which hands over whole packets - never showed it. It answers from what
it found the first time now.

**Measured under QEMU with glmech and two cubes running:** a click reaches
the window manager in 35 to 48 ms median on one core, over two runs, and in
1 ms spread across eight - 1 ms at the ninetieth percentile too.

### What the T14 was asked, and answered

The 0.10.20 stick carried `opt/kosmos/smp=8` on GRUB's `multiboot2` line -
the first boot option a machine without fw_cfg has been able to take, and
since 0.10.22 not needed for this.

1. **All eight processors alive**, reported from the machine, and Processes
   showing threads homed on every one of them.
2. **Monitor**: eight bars, three of them well loaded while four demos ran.
3. **The pointer**, with Processes and Monitor open: smooth.

### Still open

- **Placement sees a moment rather than work.** A demo waiting for its next
  frame counts as idle when the next thread is placed, so busy threads can
  share a core while others sit at 0%. Nothing migrates, so a poor choice
  lasts the thread's life.
- **Which of three changes stopped the T14's pointer jumping** - the i8042
  no longer re-initialised by `SYS_SYSINFO`, its lock, or the spread. Not
  isolated.
- **`process_kill` still reads a process's thread with nothing held.** A
  kill that lands while the process exits on another core can call
  `ipc_abort` on a thread slot that has since been reused. Exit and wait are
  locked now; kill wants its lock order against IPC checked before it takes
  `processes_lock`.
- **Why `/net` takes 18.4 seconds** to list nothing on a machine with no
  network card. Tracker no longer asks; the call is still that slow.
- **Which change made QEMU's PS/2 mouse stream** - the configuration byte,
  the armed interrupt lines, or the framing check. Not isolated.
- **One pointer speed for two devices.** `pointer 48` sets it, nothing
  persists it, and a TrackPoint and a touchpad want different curves from a
  driver that cannot tell their packets apart.
- **`neofetch` on the T14 says `QEMU q35 x86-64` and `virtio-net`.** The
  platform is a Makefile constant compiled into the kernel, and the network
  row names the virtio driver whatever the machine has.
- **The click probes are still in**: `i8042 buttons` and `wm: button`, both
  bounded. Clicks are confirmed on the machine now - the T14's log shows a
  release reaching the window manager and an application launched from it -
  so they can come out.

### And the disk it has

`hal/pc/nvme.c` is new and works: found by PCI class, admin queue, identify,
one I/O queue, polled completions, everything through a single bounce page so
the PRP-list path does not exist to be got wrong. `make x86` boots from it
and the ARM board keeps virtio-blk, so neither driver is orphaned. Four
checks in `run_x86.py` write a file, kill the machine and read it back, which
is what establishes the field offsets - they were written from knowledge of
the specification rather than a copy of it.

**It has never seen the ThinkPad's Micron drive.** The one thing it refuses
outright is a namespace whose blocks are not 512 bytes, and it says so.

### The interrupt controller a laptop actually has

`hal/pc/apic.c` is a local APIC and I/O APIC driver, and which controller
this board drives is decided when it boots from what the firmware described.
**That was the one failure on this target with no workaround**: Intel has
been removing the 8259 pair and the 8253 from UEFI-only platforms, and a
kernel driving only those gets no scheduler tick there - a boot that prints
all twelve stages and then stops.

`opt/kosmos/irq=pic` forces the legacy path, so both are exercised on every
gate rather than the fallback rotting until the first machine that needs it.

**MSI is how a PCI device reaches it.** Under an I/O APIC a device's
Interrupt Line register means nothing, and which input the chipset's links
land on is in ACPI's `_PRT`, which is AML. The PCIe swizzle is right for
slots and wrong for a chipset's own integrated devices - the HDA controller
being exactly one. An MSI is not a wire: the device writes to the local APIC
with a vector this kernel chose.

**The bug underneath it is the one worth remembering.** `pci_enable` asks
which controller is running, at stage seven; the controller was not decided
until stage eleven. Every device was told *no*, took a line nothing was
routing, and the tone played for three times its length. The binding decides
on first use now - rather than reordering the boot, because the display
exists at stage six so that a failure after it is visible on a machine with
no serial port.

**What it unblocks:** `hal/pc/cpu_on.c` has refused to start a second
processor for want of a local APIC to send INIT and STARTUP through. The
ThinkPad has eight cores and Kosmos uses one.

Also in: `hal_irq_handle` now says whether the tick fired, which stops every
device interrupt charging the scheduler a tick on both architectures; and a
panic stops taking locks on its first line, which turns the console-lock
deadlock seen twice today into a message.

x86-64 is 46 checks.

**And the thing that matters most, found while testing it: ACPI is invisible
under UEFI.** `find_rsdp` scans the EBDA pointer at 0x40E and the BIOS area
from 0xE0000, which are both legacy conventions; UEFI passes the RSDP in the
EFI Configuration Table and OVMF leaves nothing where a scan would find it.
The same image with four processors reports four and takes the APIC under
`-kernel`, and reports one and falls back to the 8259 pair under GRUB.

On the ThinkPad, booted the way it will actually boot, that meant no
processor count, no ECAM and no APIC.

**Multiboot 2 is in, and that is fixed.** The image carries both headers and
the loader picks; `grub.cfg` says `multiboot2`. Under UEFI the same machine
now reports **four processors and drives the local APIC**, where it reported
one and fell back to the 8259 pair. `run_uefi.py` boots with four processors
and checks both, and both checks fail when the ISO is built with `multiboot`
instead - verified.

**And the machine arrived.** It is a T14 **Gen 2** - Tiger Lake, i5-1145G7,
16 GB, UEFI - rather than the Gen 1 this document assumed. `docs/thinkpad.md`
§0 has what its own report says, and three things follow: the keyboard is
i8042 (no USB keyboard in its device list, so the driver already written is
the right one), there is no VMD in front of the NVMe, and **there is no
Ethernet at all** - the network is an AX201 over CNVi, so the roadmap's
"Intel I219, ~1500 lines" is not this machine and USB Ethernet behind xHCI
is the honest path.

### Kosmos boots the way the ThinkPad will

`make x86-iso` builds a GRUB image; `make x86-uefi` runs it under OVMF, the
same EDK II a laptop's firmware is built from. It reaches the desktop on a
**1280x800 framebuffer the firmware set up**, handed over through the
multiboot video request - a path that had never once run, because QEMU's
`-kernel` is not a loader and does not answer that request.

**Three faults were hiding behind it and not one is a driver.** All three are
fatal on a machine with more than a gigabyte of memory and silent on a
machine with no serial port:

- the boot page tables mapped one gigabyte, and the loader's information
  structure may be anywhere below four. Page fault at stage three;
- the largest usable low region under UEFI starts *above* the kernel image,
  so `fine_end - ram.base` wrapped and the map asked for four quadrillion
  pages;
- the framebuffer was never mapped, because ramfb's pixels are in RAM and a
  firmware's are not. The first pixel faulted, the handler tried to report
  it, and the console lock was already held by the write that faulted:
  `spinlock: console held by 0, wanted by 0`, for ever.

**And a fourth that is a design limit rather than a bug.** RAM is identity
mapped below the region processes get, so this kernel can describe 768 MB and
a laptop has sixteen gigabytes. It used to panic; it caps now and prints what
it gave up, because the panic reached a serial port and the machine has none.
**The high-half split is what lifts the ceiling** - kernel at the top of the
canonical address space, all of physical RAM at a fixed offset, the whole low
half to the process - and it is now the biggest single thing left on this
target.

**And the panel has the log from stage two now.** The display was stage six,
and all three faults above were at three, four and five - on that machine, a
black panel with nothing to read. `hal_fb_early` asks for a framebuffer the
firmware already set up; `hal_fb_remap` is asked the instant `mmu_init`
returns and before the next character is printed, because the identity map
that made the early screen possible is gone by then and one `kputs` into it
is a deadlock rather than a message.

`run_uefi.py` is 9 checks, on the picture and on what the machine says about
itself rather than on the serial line, because a machine whose framebuffer
works stops talking to the serial line at stage six. The early-screen check
is the second one in this project to have passed for the wrong reason
first - a stopwatch, where the whole boot takes under two seconds - and it
was verified by reinstating the fault.

Tested at 1920x1080 as well as OVMF's own 1280x800: an 8 MB framebuffer maps
into the device window and draws.

### Sound on the machine a laptop is

`hal/pc/hda.c` is an Intel High Definition Audio driver, and it is **the
first driver in this tree written for hardware rather than for QEMU**.
Everything before it either exists on both machines - the i8042, the PIC,
the PIT - or is QEMU's own with no counterpart on a laptop, which is what
ramfb is. HDA is neither: a specification Intel has shipped in every chipset
since 2004, emulated faithfully enough to develop against.

`make x86` gives the machine an `ich9-intel-hda` instead of a virtio sound
device now, for the argument the i8042 already won: the driver that has to
work on a laptop should be the one exercised every time somebody runs the
system.

**The shape is not virtio's shape.** A virtio sound device is a queue; HDA
is a cyclic buffer that never stops, so there is no handing over and no
running out - only being late. The depth comes from `SDLPIB`, the
hardware's own read pointer, rather than from a counter the driver keeps;
one slot is always left free so that write-equals-play means empty; and a
finished period is zeroed in the interrupt handler, because a ring that runs
dry repeats the last thing anybody wrote at 172 Hz.

**What it cost was one sentence in the specification.** `RINTCNT` is not
only an interrupt threshold: it is also how many responses the controller
writes before it treats the response ring as full and stops consuming
commands, and what restarts it is the driver acknowledging `RIRBSTS`. With
the response interrupt disabled there is nothing to acknowledge, so
`GET_PARAMETER` on the root node answered correctly and the same call one
node down timed out, for ever. The interrupt is enabled and `INTCTL.CIE`
left clear, so the status is set, every response clears it, and the pin
never moves.

**The audio is read back off the wire, not off the boot log.** Every other
check in `run_x86.py` is a string the machine printed; a driver that
programs the controller wrongly prints exactly what a working one prints.
QEMU's `wav` backend writes what it was handed to a file and the harness
measures it: 333 ms of tone, 440 Hz from the zero crossings, and zero
samples outside the run that are not silent - which is the samples arriving,
arriving once, and the silence being real, in one capture.

**And a board binds the HAL now.** `hal/virtio/snd.c` used to *be* the sound
HAL, which was right while both boards took sound from it. `snd_bind.c` per
board asks HDA first and virtio second, exactly as `fb.c` asks the loader
first and ramfb second; `hal_snd_describe()` is what the boot log prints,
because "no sound" covers three different faults that all sound identical.

The harness is **38 checks on x86-64**, from 29 - the tone, the pitch, the
silence around it, one interrupt per period, no underrun, and a queue floor
that is a number between one and the device depth rather than the
four-out-of-four a stream that never filled would report.

**Next on this target, in the order the machine decides:** xHCI and the USB
core, because Kosmos can run from a stick with no storage of its own; then
the local APIC, now that ACPI says where it is; then PCI over ECAM, now that
MCFG does. `docs/thinkpad.md` is the log.

### A prompt you can work at

`ls`, `cat`, `mkdir` and `find` were the whole of the shell. It now has
`cp`, `mv`, `rm`, `touch`, `head`, `tail`, `wc`, `grep`, `tree`, `du` - and
an up-arrow, which is the one that changes how the machine feels to use.

**They are thin, and that is the point.** `files.copy` and `files.move`
already existed and Tracker had used them for months; the filesystem already
had `mkdir` and `delete`. `cp` is thirty lines, most of them comment, and
nothing new went into a server. `/bin` went from 87 programs to 97 without
the kernel changing.

`use("/lib/text.lua")` holds the two questions four of them share: where
lines end, and how `-n 3` is spelled. `grep` takes a Lua pattern and says
so - inventing a second pattern language here so the spelling matched a
different operating system would mean carrying a regex engine to do it.

### The up-arrow, in two line editors

`user/servers/console.c` for the boot prompt, `user/bin/terminal.lua` for
the window, with the same semantics: 0 is the line being typed, 1 is the
most recent, anything that ends a line resets it. They cannot share a ring -
one is in another process - and the thing they must not do is disagree.

A serial terminal sends ESC [ A and `hal/virtio/input.c` already maps the
keyboard's arrows to the same three bytes, so reassembling them once in the
console server gave the graphical console history for nothing.

### /ramfs, and the three operations it never had

`/data` is `/ramfs`, and it now answers `mkdir`, `delete` and `rename`.

**Everything that had ever used that mount published.** A replicant writing
its own source for `adopt`, the web server writing its status, a benchmark
tagging files to query for - nothing ever took anything back out, so an
operation with no caller was never written. `rm` is what turned an absence
into a bug: a verb that works on one mount and not another is the namespace
failing at the one thing it exists for.

The name went at the same time. Every other mount is named for its role and
`/data` was the only one whose name said nothing, while implying the one
thing that was false - data is what you least want to lose, and this mount
is gone at power off. `/ram` was rejected for reading like a device you
write raw bytes to.

### Tracker browses what it holds

The sidebar is `fs.mounts()` now. Six strings were written out in the source
and had fallen behind: `/user`, `/app` and `/ramfs` were all mounted and
none was offered, so a file manager could not reach places its own process
could see. After the rename one of them read `data` beside a path saying
`/ramfs`, which is the same rot one step on.

Two filters, both about what a browser can use: a mount inside another is
not a root, and a mount that will not answer a listing is not offered.
`/net` is a protocol rather than a tree, and so is the disk on a machine
that has none.

**The first version probed with `files.entries`**, which adds a `getattr`
per name - ninety-one for `/bin` - and Tracker opened empty and stayed that
way while it worked through them. `fs.list` answers the same question in one
round trip.

### `help fs` had never worked

Writing the cheat sheet found it. `help` also names a value in the shell's
environment, so `shadows_lua` sends the line to Lua and Lua has no such
expression. The overview has told people to type it since topics existed,
and the shell's own comment used `help gfx` as its example *of a command*.
`help "fs"` and `/help fs` work; all three places now say so.

### What is not done

**No pipes and no redirection**, and the tools that want them are the ones
not written: `sort`, `uniq`, `cut`, `tr`, `sed`, `xargs` would each have to
take a path and print, which is half of what they are for. They are waiting
on a decision about composition rather than on anybody writing them - a pipe
needs a stream between two processes, and what this system has between
processes is messages and shared memory.

Still wanted, and none of them blocked: `kill` (the syscall exists and only
the graphical Processes app uses it), `df` across every mount (nothing
reports how full `/ramfs` is), `mounts` (what `/` is made of and what serves
each), `which`, `stat`, `rmdir`, `less`, `diff`.

**The website is rewritten and unpublished.** 604 lines to 453, with the
version history and the program tables cut; it says SMP is done rather than
next, and no longer claims there is not a lock or an atomic in the kernel.
It waits on three screenshots that have not been taken.

### A console write was handing a capability over, and the text's length picked it

Three defects came out of one question - why the `neofetch` banner was slow
in a Terminal - and the third is the one that mattered.

**`con.encode_request` returned two values.** The bytes, and how many of them
were taken. Lua expands a call in final argument position to all of its
returns, so

```lua
sys.call_raw(capability, con.encode_request{ ... })
```

was `sys.call_raw(capability, bytes, length)`, and `call_raw`'s third
argument is `pass` - the capability to send with the message. **A write of
three characters handed over capability 3.**

It points both ways. The reader's capability table filled with the proceeds,
which is why a Terminal could not start a program after about eighty short
writes (`hello: no endpoint`); and the writer was giving away authority
nobody asked it for, in a system whose first principle is that you cannot
reach what you were not handed.

**It hid because an ordinary line of text is longer than any index a writer
holds**, so the transfer failed harmlessly and nothing looked wrong. The
banner is the first thing that writes in short coloured runs - one, two,
three characters - and every one of those is a valid index. The feature did
not cause the bug; it was the first thing able to show it.

The second return had no user: `con_request` chunks by `TEXT_MAX` and
advances by `#piece`. It is gone, and the caller binds the result to a local
so a future second return cannot do this again.

`con: a write carries no capability` is the permanent test - a peer holding
five capabilities writes texts of length 0 to 3 and the receiver asserts none
came attached. Checked the only way that means anything: with the bug put
back it fails, and under the bug all four writes transferred.

### The terminal paints once per burst, not once per write

`serve_console` drained non-blocking, so after answering a write it asked
again before the child it had just woken could have been scheduled. Nothing
was there, so it returned and repainted - one write, one full repaint, and a
repaint ships the whole window as drawing commands in 1200-byte batches.
Answering `n` writes with `n` repaints is about `n^2 / 11` round trips.

Measured by counting the banner's own blue every half second: **24.0 seconds
to draw twenty-three lines**, at about 950 pixels a second, with the
processor idle throughout - none of it was work. `receive_raw` already took a
timeout and this was not using it. Afterwards the banner is complete before
the window is first visible, with the same 23,136 pixels at the end.

### A display check that was passing for the wrong reason

`check_terminal` sampled its "before" ink the moment the window opened -
while the banner was still painting - so ink in its box climbed by hundreds
whether or not anything else ran, and the assertion was satisfied by the
banner arriving rather than by `hello` printing. It passed for as long as the
terminal was slow and failed the day it got fast, which is how the capability
leak surfaced at all. It now empties the window first.

**Worth keeping as a lesson about tests rather than about terminals**: the
check had been green for its whole life while the thing it named was broken.

### Still open

**`ipc_error` cannot say which resource ran out.** `sys.endpoint()` failing
reports `out of endpoints or capability slots`, one message for two very
different exhaustions - a global pool of 96 and a per-thread table of 32.
That cost two builds during the hunt above, and the fix is to split the code.

### The audio ring carries a position, and a frame is the only honest unit

`frames_played` in `struct audio_ring`: a 64-bit count of the frames of *that*
stream that have come out of the speaker, written by the server and read by
the client out of shared memory. `audioring.h` has the reasoning and
`tools/test_audioring.c` has 41 checks of it that need no machine.

**The device queue is the whole reason it cannot live on the client's
side.** A client knows what it wrote and the ring says what was taken, but
between "taken" and "heard" sit up to `device_depth` periods that only the
audio server can see. A position built from the ring alone is ahead by the
depth of the device - about 23 ms - which is small enough to look right and
large enough to be useless for the two things a position is for.

**It needs no clock, and that is the point rather than a convenience.**
`CLAUDE.md`'s two-clocks rule is about a number that arrives naked at
another process; a frame count cannot, because the unit is in the name and
there is nothing to divide by. Latency is `write * period_frames - frames_played`,
which is the ring and the device together without either side having to
know how the total is split. This is the first number that crosses a
boundary here and needs no `counter_hz` three lines above it.

What it is exact about, and what it is not: exact while a stream is feeding,
and *behind* the truth by at most the device's depth after that stream has
been starved, because the server mixed other streams into periods this one
is not in and the subtraction takes all of them off. Under-reporting is the
right way round - a progress bar that pauses is a glitch you already had,
one that jumps backwards is a bug report. The host test asserts both
directions rather than trusting the sentence.

**`audiolag` is the first caller and it found the thing worth finding.**
Every other number that program prints is a *timing* - how long a write
took, how long a turn took - and a timing is evidence about latency rather
than latency. It now prints the worst end-to-end figure and the bound it
must not cross, which is the ring's depth plus the device's.

Also fixed there, and it had been there a while: the program declared `hz`,
`us`, `chunk` and `out` **twice**, so it called `audio.open` twice and
leaked the first stream - a slot out of eight, a region and a capability -
for the whole run. It measured correctly anyway, which is why nobody saw
it.

### And the ring's depth stopped being a constant

`audio.open(name, periods)`. The plumbing was already there and unused:
`sys.ring_create` has always taken a depth, and the server reads
`r->periods` everywhere rather than the constant, so variable depth worked
end to end and nothing had ever asked for it. `AUDIO_RING_PERIODS` is now
only a default.

**Why it has to be the client's call.** A shallow ring is less to wait
behind and less to survive a late turn with; a deep one is the reverse.
A game wants the sound to arrive with the frame that caused it and would
rather risk a gap. A video player wants a clock to hang pictures on and
would far rather be 200 ms behind than skip - a gap desynchronises
everything after it, where latency is a constant you can schedule against.
No single number serves both, which is what made it stop being one.

`open` reads the depth back off the ring rather than trusting what was
asked for, because `ring_create` quietly substitutes the default for a
number it does not like, and a client computing a bound from its own
argument would be wrong without ever finding out.

**A `periods_for_ms` helper was written here and then deleted**, which is
worth recording because the deletion is the interesting part. It converted
a slack in milliseconds to a number of periods, it was four lines, and it
had no caller - and `hal.md`'s practice, which `CLAUDE.md` quotes, is that
every entry arrived with the thing that needed it and none of it was
written ahead of a caller. Two lines the video player can write once it
knows whether it wants milliseconds, frames or periods; guessing now would
have been an interface to keep rather than a convenience.

`stream:bound()` is the worst latency the stream can have - the ring plus
the device. `delay()` is what it *is*; `bound()` is what it cannot exceed,
and a `delay` past the bound means the position and the indices disagree
rather than that the machine is busy.

**`audiolag [periods]` is the instrument**, and the argument is the point of
having one: run it at 2, 8 and 32 and worst-latency and UNDERRUNS move in
opposite directions. That curve is what a sensible default should come
from, and nobody has ever been able to plot it. **Not plotted here** - see
below.

The host test is parameterised by depth now for the same reason, and it
earns it: breaking `audio_ring_space` to use the constant instead of
`r->periods` passes every fixed-depth check and fails four of the swept
ones.

### What is still missing, and it is not code

**None of this can be tuned on the current target.** The known failure
above it stands: 3-4 underruns per 2.3 s, and 194 interrupts per 400
periods, which is the device model servicing about two periods a raise.
Sweeping the depth under QEMU will produce a curve, and the curve will be
QEMU's rather than the design's - which is exactly what `CLAUDE.md` warns
about, in the subsystem where it matters most, because a real-time claim is
a claim about the worst case.

So the depth argument exists to be *used on hardware*. The Pi 5 is the
missing instrument, and it is now the thing standing between this pipeline
and being able to say anything about its latency at all.

**Not done, and deliberately:** `music` still draws its progress bar from a
local of its own also called `played`, meaning *bytes of source handed
over* - `music.lua:91` - so the bar is ahead of the sound by the whole
pipe. That is a two-line fix now and it belongs with the seek work that is
already wanted there, rather than bolted on here.

**Also not done:** `design.md` has no audio section, so the reasoning lives
in `audioring.h` and here. It wants one, and writing it is its own piece of
work rather than a paragraph appended to §7.4, which is about drawing.

### A test that failed at random, and the reason was in the test

`make test` failed once during this work with

    FAIL: `size` was stored as an attribute. It is read out of the inode,
    so there are now two answers to how big this file is...

and the filesystem was fine. `run_disk.py` writes `size=999` on the first
boot, expects it to be refused, and checked the second boot's output with
`if "999" in after` - over *everything* printed after the `attr` command,
not the listing. `attr` also prints `mtime`, which is `sys.ticks()` and so
a nine-digit number that is different every run. That run's clock read
`478999000`.

    author       diego
    extents      1
    kind         note
    mtime        478999000     <- the "999" it found
    size         25            <- the answer it was actually looking for

**The check was right and the method was wrong**, which is the dangerous
combination: it passes for years, fails for a reason unrelated to what it
tests, and the failure is a confident sentence about a bug that is not
there. It now reads the `size` line out of the listing and compares the
value, and it is scoped to the block rather than the rest of the boot.

Worth connecting to what else is written down, because there are now
**three** of these and they arrived within days of each other. `diskfs`
stamping `sys.ticks()` as `mtime` is listed under **Still open**, and this
is a second cost of it - a wall clock would not have produced the digits.
The older note about *one kernel check that failed once and never again,
with the evidence destroyed by a grep* is the same shape. And 0.10.0 adds
`sched: the policy is pluggable` failing about one run in three, below.

**Two of the three were recorded and could not be diagnosed; this one was
diagnosed because the whole log was kept.** That is the difference worth
taking from it - not that flakes should be written down, which the other
two already establish, but that a `grep` for the summary line is what
decides whether writing it down is all you will ever be able to do.

### The console has colours, and a `neofetch`

Two pieces of work, in that order, because the second needs the first.

**A console write carries a colour.** `struct con_request` gained a
`uint32_t colour` - `0xAARRGGBB`, zero meaning the console's own, so every
caller that has no opinion is unchanged and none of them were touched.
`SYS_WRITE` gained a third argument to match, `kernel/console.c` gained
`kwrite_colour`, and any Lua program can now say:

```lua
write("KOSMOS", 0xffcc2233)     -- one run, no newline added
write(" ok\n", "good")          -- or a name, against a fixed palette
```

`design.md` §4.4.1 has the argument in full. The short version is that two
other shapes were available and both are wrong: **a mode** loses, because the
console has several writers and set-then-write is two operations another
process can get between; **an escape code** loses, because this system had
already refused in-band escapes once, for the boot log, and the reason is
still written where it was decided.

The third shape - a run list in the message - is the one that looks necessary
and is not. A line in several colours is several writes, because the far side
appends until a newline arrives.

**`kwrite_colour` also replaced `kputc` in a loop**, which was one spinlock
acquisition per byte. That was not the point of the change and it is the
better half of it.

### The kernel's console reads UTF-8

`tools/bdf2c.py` now emits the Block Elements - U+2580 to U+259F, the whole
block - beside printable ASCII, and `kernel/console.c` decodes UTF-8 in a
two-word state machine on the way to the screen. `gfx.md` §19.11.1 is the
account.

It mattered because the console indexed the font *by byte*, so a three-byte
character became three hollow boxes on screen while the serial line showed it
correctly - the same output looking like two different outputs, which is the
complaint the early-run colour list answers in the other direction.

`user/lib/gfx.c` carries the same lookup, and the size check at the top of
`luaopen_gfx` is what forced it to: it panicked on the first boot after the
font grew, which is exactly what it exists for.

### `neofetch`

`user/bin/neofetch.lua`. The banner is `assets/kosmos-ascii-art.txt`, carried
in the image through `sys.asset` - which is already "a small file compiled
in" and so needed one line in the Makefile rather than a mechanism.

**Neither colour is written in the art file, and it does not need markup.**
The diagonals are Block Elements and the wordmark is ASCII line art, so which
is which is a property of the glyphs: blocks go out blue, everything else
red, and the three shades take care of themselves because the light shade
simply covers fewer pixels. The picture stays something you can open in an
editor.

It runs in two places, through `run_program` and `launch` respectively, so
`neofetch` typed at a prompt and `neofetch` at boot are the same program with
the same authority:

- the boot shell, wrapped in `pcall` - a banner may never be the reason a
  machine cannot reach a prompt
- every Terminal window, which grew to 640x700 to hold it

Three of neofetch's fields are deliberately missing, and each says something.
There is no `user@host`, because nothing here has a global name. There is no
`Terminal:`, because a program **cannot** find out - it prints to whatever it
was handed as `/dev/console` and a Terminal window mounts itself there
speaking the same protocol the server does. And where the colour bars go
there are the kernel's fixed pools instead, which is the number this system
actually wants somebody to have seen.

### What was checked

`make test` 139/139 and 135/135, `make screenshot` 71 display checks on both
architectures, and the x86-64 build. Two tests are new and both are exact
rather than approximate:

- `console: a write carries its colour, and UTF-8` counts framebuffer pixels
  of an unlikely colour after writing U+2588. The answer is 128 and not "more
  than before": three unknown boxes is a different number, a block found at
  the wrong index is a different number, and a block in the console's own
  colour is zero. A third write with no colour then proves the console's
  default survived, which is the half a *mode* would fail.
- The Lua font test draws the full block, the light shade, the first of the
  range, and a codepoint in neither range, and checks the rows against the
  BDF.

The first one taught something about the harness: it draws, so those glyphs
reach the serial line too, and a TAP result printed on the end of them does
not begin a line. A run in which every test passed was reported one test
short until it emitted a newline.

### What is not done

**Two scheduler tests flake, and it is the same shape both times.**
`sched: the policy is pluggable` failed twice on an unchanged tree, and
`sched: the higher priority runs first` once, each with three green runs of
the same build either side. Both are timing tests about which thread runs
next, so the suspicion is the harness's load rather than the scheduler -
but that is a suspicion and not a diagnosis. Written down because a flake
nobody records is a flake everybody re-discovers.

**Some of it was self-inflicted and that is worth separating out.** A gate
that failed on `timer: the period matches the rate` on x86-64 failed
because screenshots were being taken against the same machine while it ran.
A timing check under a load the harness did not ask for is not a result.

**`screen_putc` still loses the character that wraps.** Its own comment says
"wrap, then draw the character below" and it returns instead, so the
character that caused the wrap is dropped. Pre-existing, found while making
that function take a codepoint, and deliberately not fixed in the same change
- it is a behaviour change the display harness has opinions about.

**`kits <name>` has never worked.** `user/bin/kits.lua` does `args[1]`, and
`args` is a string, so indexing it reaches the `string` table and returns
nil. Unrelated to any of this; found while reading for `neofetch`.

### A thread runs on another processor, and the IPI is worth 25x

`docs/smp.md` step six. `hal_cpu_wake(cpu)` sends SGI 0 through
`ICC_SGI1R_EL1` - an interrupt one processor raises on another, with no
device behind it - and **the handler is empty.**

That is the design rather than an omission. The sender has already put the
thread on the target's runqueue; the only thing missing is for that core to
look. Taking the interrupt is what gets it out of `wfi` and into the
exception epilogue, which is where `thread_preempt_if_needed` already runs.
An IPI carrying a payload would be a message, and messages between
processors are what a runqueue and a lock already are.

**The number matches the reasoning exactly**, which is the satisfying part.
A cross-core wake, measured on the counter under TCG:

```
with the IPI      11,688 counter ticks   ~0.19 ms
without          293,688 counter ticks   ~4.7 ms
```

4.7 ms is one scheduler tick at 250 Hz. That is precisely what was predicted
from reading the code - without a poke the target notices at its own next
timer interrupt - and it is the difference between four processors being
faster than one and being slower, because a shell command here is dozens of
IPC round trips.

**Nothing depends on it for correctness**, and that was checked: the new test
passes with the IPI removed. The wake is late, not lost. So the value is a
measurement rather than an assertion, which is the right shape for something
whose whole purpose is latency.

The `dsb ishst` before the register write is not decoration. Without it the
enqueue can still be in this core's store buffer when the interrupt lands,
the target wakes, finds an empty queue and goes back to sleep - a lost wakeup
that happens rarely and is indistinguishable from a hang.

### `smp: a thread runs on another processor`

One test, and it exercises the whole of steps five and six at once. Placement,
the target's runqueue and its lock, the IPI, the target's idle loop, and
`thread_block`'s new idle fallback - each fails differently and the test
catches placement being broken (confirmed by breaking it).

**Core zero never yields while it waits.** That is the point rather than an
oversight: if the thread only ran because this processor gave up the CPU, it
would prove nothing about the other one. Core zero sits doing nothing and the
work still gets done.

### It was switched on once, to find out what breaks

Turning `thread_cpu_count()` into `smp_online()` spreads every new thread
across four cores. Two things broke inside a second:

- **`thread_block` panicked.** "Every thread is blocked" was a statement
  about the machine, and with a queue per core it is a statement about one
  processor - an empty runqueue is the ordinary state of an idle core. It
  falls back to that core's idle thread now, which is what an idle thread is
  for.
- **A dozen tests failed, and they were right to.** They are single-core
  tests of the mechanism: they mask interrupts, create three threads and
  drive them by yielding, which only works if those threads are *here*. A
  suite rewritten to tolerate placement would be a suite that had stopped
  asking its original question.

So the switch is off by default and `SMPWORK=4` turns it on.
`thread_create_on` is how anything crosses a core deliberately.

**What held the line was the drivers, and that is done.** `blk`, `net`,
`input` and `snd` each take a spinlock over their virtqueue indices, landed
in 0.9.20.

**Work spreads now.** Six compute-bound processes on four processors read
100% on every core; before the fix, three went idle within a second while
all six were alive. Both causes were in the preemption path rather than in
placement: `thread_tick` returned before `policy->tick` on every core but
zero, and `thread_wake` decided preemption about the *waking* core instead
of the target. `docs/smp.md` has the numbers and what was ruled out.

**What keeps placement off by default is one known failure**: under
`SMPWORK=4` the display harness fails at its editor phase - the program
typed into `edit` does not come back. It survived the fix above, so it is a
separate bug and it is the next one.

### One runqueue per processor, and a thread that has a home

`docs/smp.md` step five, and the locking of step two with it - the two cannot
be separated, because a queue per core is only meaningful if another core may
put something in it.

The vtable now names the queue: `enqueue(cpu, t)`, `pick_next(cpu)`,
`ready(cpu)`. "Enqueue" is not a complete instruction on a machine with more
than one queue, and the caller is frequently not the core the thread will run
on - a wake from core zero's timer puts a thread wherever it lives.

**A thread has a home and does not migrate.** `t->sched.cpu`, assigned
round-robin at creation. That is the decision worth stating, because the
alternative - one queue every core pulls from - looks simpler and is the one
that cannot be undone: it makes every scheduling decision a contended write
to a single list, and it has nowhere to say "this belongs on a performance
core", which is the first thing the laptop in `docs/targets.md` will ask for.

The cost is honest: a core can idle while another has two runnable threads.
Work stealing is the usual answer and is deliberately absent - it needs an
order between two cores' queue locks, which is the one place a deadlock could
come from here, and nothing has measured a need for it.

### The placement decision paid for itself in the hardest place

The audit said IPC's critical section had to extend **past** `thread_block` -
handed to the next thread and released on the far side of `context_switch`,
which is what Linux does in `finish_task_switch`. The reason is a genuine
race: a thread marked blocked and findable can be woken by a second core and
resumed by a third, on the stack the first core is still saving.

**With a fixed home that race cannot happen.** A wake only ever enqueues a
thread on its own core's queue, and only that core picks from it - and that
core is the one inside the switch. So `thread_block_and_release` lets go
before the switch and is correct, and none of the hand-off machinery is
needed.

That is worth recording as a *property of a decision made elsewhere*. The
comment on that function is where the cost comes back if migration is ever
added: the release would move to the far side of the switch, and every entry
into a thread - including a brand new one starting at its trampoline - would
have to know about it.

### IPC took the shape the audit predicted

**One lock per endpoint**, because no operation in `ipc.c` ever touches two:
`resolve` returns exactly one and every splice works on that one. So there is
no order between two of them to get wrong, and two conversations proceed on
two cores without meeting. A single subsystem lock would have been shorter
and would have made every message in the machine queue behind every other,
which in a microkernel is the machine.

Held from before the hand-off until the sender is *both* on `awaiting_reply`
and blocked. Those become true at different moments, which is why the release
happens inside `thread_block_and_release` rather than at the call site.

Three re-checks under the lock, all the same pattern - `ipc_reply`,
`ipc_abort`, `ipc_timed_out`. `waiting_on` names the endpoint, so it has to
be read *before* there is a lock to take; the answer is to take the lock the
unlocked read pointed at and then confirm the read still holds. Between the
two the thread may have been woken by a timeout or by the endpoint being
destroyed.

### What `sched_switch_to` cost

It refuses now when more than one processor schedules, and that is a real
feature lost rather than a detail. Swapping the policy means draining every
runnable thread out of one and into the other - with a queue per core that is
*every* core's queue at once, which would make it the single operation
defining a global lock order, and it would have to stop the other cores
mid-decision.

It exists to demonstrate that mechanism and policy are genuinely separable,
and it has done that. It is not something anybody needs while four cores are
working. The suite still swaps policies, because the suite runs with one
scheduling core.

### The console, and a deadlock I nearly built

The console lock is taken around **a whole string, not a character**. The
state would be safe either way; the *output* would not - two cores printing a
line each with a per-character lock produce two interleaved lines and no way
to read either.

That matters most in the case that made it necessary: `spin_panic` prints
from whichever core deadlocked. Which is also the hazard - **if the lock that
deadlocked is the console's, reporting it through `kputs` spins on the same
lock, panics again, and recurses until the stack runs out.** A diagnosable
deadlock would have become a silent triple fault. `spin_panic` writes bytes
to the UART directly now: no log, no screen, no lock.

### What is still not done

The drivers - `blk`, `net`, `input`, `snd` - each keep one set of virtqueue
indices touched from both a syscall and an interrupt. They are safe today
because every interrupt is routed to core zero (`GICD_IROUTER`, affinity 0)
and only core zero does the machine-wide half of a tick, but that is an
argument from routing rather than from locking and it should be one or the
other.

`panic()` itself needs a protocol rather than a lock: a core that panics has
to *stop* the others, not queue behind them.

And the switch is not thrown. `thread_cpu_count()` is still one, so every
thread is homed on core zero and nothing contends for any of these locks.
They are correct and they are untested under contention - which is stated
rather than implied, and is what step six exists to change.

### The kernel has a lock

**The first one it has ever had.** Mutual exclusion in Nebula has been one
sentence since the first commit - *there is one core, and the kernel runs
with interrupts masked* - and `docs/smp.md` step five is where that stops
being true.

`struct spinlock` in `kernel/spinlock.h`, built on one atomic per
architecture: `ldaxr`/`stxr` with a `stlr` release on AArch64, `lock xchgl`
with a plain store on x86-64, where total store ordering already provides
the release and the only missing piece is a promise from the compiler.

**Every lock masks interrupts, and there is no second flavour.** The reason
is specific rather than cautious: the structures worth locking are reached
from both a syscall and an interrupt handler - the runqueue from
`thread_yield` and from `thread_tick` - so a core holding one with
interrupts enabled can be interrupted into code that wants the same lock, on
the same core. No amount of spinning resolves that, and it is silent.
Masking on this core is enough; a different core spins and gets it.

The spin is bounded and the panic **names the lock and the processor holding
it**, which are two different failures: another core is a critical section
that never ended, this core is a path that took it twice.

**A lock is the one thing in a kernel that cannot be tested by using it** -
every structure it protects works perfectly with a lock that does nothing, on
a machine where nothing contends, which is exactly why step two called an
uncontended lock untestable and skipped it. So the test checks the mechanism:
it excludes, it names its holder, and it masks. Confirmed by breaking it both
ways - a lock that does not mask fails the check, and one that always wins
hangs the guest.

### The pools, and the bug that found itself

Every pool used the same pattern: **scan for a free slot, build in it, mark
it taken.** The window between finding and claiming is hundreds of
instructions - page allocation, a 512-entry table copy - so two cores in
`SYS_SPAWN` at once get the same thread, the same process and the same
address space, and nothing anywhere notices.

`threads[]`, `processes[]`, `objects[]`, the PMM bitmap and `spaces[]` on
both boards now claim inside the lock. `THREAD_CLAIMED` exists for exactly
that: a state that is neither of the two the allocator scans for.

**It found a bug in the doing.** Moving the claim into `alloc_process` was
not enough, because `process_create` then ran `memset(p, 0, sizeof *p)` over
the slot - clearing the flag that claims it, and reopening the window for the
length of a 160-byte memset. The suite caught it on the first run, with a
translation fault at the user text base and nothing to say why. The zeroing
now happens inside the allocator, under the lock.

`memobj_create` was the one that already claimed first, and its comment
explains why in terms of *preemption* - a syscall runs with interrupts on and
can be preempted between finding a slot and finishing with it. A second core
is the same window one step wider, which is a good illustration of how much
of SMP is already visible on one processor if you look.

### Two more that were per-CPU all along

- **The lazy-FP owner.** `arch/aarch64/fp.c`'s own comment said it was
  per-CPU state in everything but name, that `CLAUDE.md` forbids loose
  mutable globals for exactly this reason, and that it belonged in the
  per-CPU struct when SMP arrived. It is there now. The x86 board needs
  nothing: it saves eagerly, so it has no lazy owner to share.
- **`as_switch` now broadcasts.** It used `tlbi vmalle1` and `dsb nsh`, the
  local forms, while `invalidate()` two hundred lines up already used the
  broadcast ones. With two cores, a process whose pages change on one leaves
  the other holding translations for a space it thinks it has left - and a
  stale TLB entry is not a fault, it is a read of somebody else's memory that
  succeeds.

### The audit, which is the map for what is left

Four readers went through `kernel/`, `arch/` and `hal/`, each then handed to
a second reader told to find what it missed. About sixty structures, in four
groups, and `docs/smp.md` now carries the shape.

The headline: **the centre of gravity is not the runqueue.** That is the part
everyone expects and the part the vtable already makes easy. What needs care
is thread *state* and the pools underneath everything.

And the answer for IPC, which is the hard one: **the endpoint is the right
lock, one per endpoint, and the critical section has to extend across
`thread_block()`** - handed to the next thread and released on the far side
of the switch. `ipc_call` wakes the receiver and only then records
`waiting_on` and blocks, so on two cores the peer can reply before the sender
has blocked and `ipc_reply` reads a NULL `waiting_on`. That does not corrupt
memory; it silently loses a message.

**Also established: the runqueue lock cannot be held across the context
switch.** `context_switch` returns on a different stack, so a lock taken
before it would be released by a different thread than took it. Pick under
the lock, let go, then switch.

### Three the audit found that nothing else would have

None of these can fail today. All three are the same shape: correct while one
core schedules, wrong the moment one does not, and invisible to any test that
could be written now.

- **The EL0 interrupt path had no core-zero guard.** `arch/aarch64/trap.c`
  guards the machine-wide half of a tick for interrupts taken at EL1, with a
  comment explaining the invariant - and the *other* vector, an interrupt
  taken from EL0, called `thread_wake_sleepers_now` and `console_tick`
  unconditionally. Dormant because a secondary runs only its idle thread,
  which is kernel code, so no core but zero reaches it. It fires on every
  core the moment a user thread lands on a secondary.
- **`split_block` invalidated only its own core** while editing `kernel_l1`,
  the table every address space copies its top level from. `as_switch` had
  the same bug and both are broadcast now.
- **A comment that had been wrong since it was written.** The address-space
  test said `arch/` "must not include a kernel header", which is why
  `ADDRSPACE_MAX` could not be checked against `PROCESS_MAX` at compile time.
  `mmu.c` already included three of them. `arch/` being reimplemented rather
  than abstracted is a statement about what it may *know*, not a rule against
  knowing how many processes there are.

### What is not done

Per-CPU runqueues, IPC, the console and the drivers. A secondary still runs
only its idle thread, so nothing contends for any of the locks above - they
are correct and they are untested under contention, and that is stated rather
than implied.

One flake fixed on the way: `sched: the policy is pluggable` was already
bounded by the clock rather than by a count of yields, and still failed twice
- because a tick is *wall-clock* time and how much work fits inside one
changed when the suite started booting `-smp 4`. Four vCPUs round-robin in
one TCG thread, so core zero executes about a quarter of the instructions per
tick it used to. The bound is a failure bound; it is a second now.

### The diagnostics, and the bug that was not there

**An application that raised died in complete silence, and every graphical
application is launched the way that makes it silent.** `run`'s detached
path was `pcall(chunk)` with the result never read, and the window manager
launches everything detached. A second silence compounded it: a window whose
process had gone stayed on the screen for ever, fully drawn, because the
compositor owns the pixels and nothing told the desktop otherwise.

Together those two make **a dead application and a busy one identical from
the outside**, which is why an afternoon went into a `cores` window that had
supposedly frozen. It was read as a hang, then as a lost mouse event, then
as an op-count limit, then as a message-size limit. Every one of those was
wrong.

**With the first diagnostic in place it took three runs.** The application
receives the press *and* the release, `on_click` fires, `run` returns true
and `spin` is in the process list; the window draws 206 ops in 18 batches
and loops. There was no freeze. What was wrong was the *check*:
`check_cores` slept exactly five seconds and took one reading, and the new
segmented bar climbs a segment at a time where a solid fill crossed the
threshold at once. At five seconds it was under; at ten it reads 236 against
68.

Then it failed again on the next run, on the way *down* - the same fixed
sleep after "take one off". One property, two places, and fixing the first
made the second look like flakiness.

Both diagnostics have a permanent test now: a program written by the machine
that opens a window and raises, checked for the reason on the serial line
and for the window going away. 70 display checks.

**The trap in the fix itself is worth keeping.** The first attempt reported
the error with `print`, and nothing appeared - in the runner's own scope
`print` is Lua's stock one and goes nowhere. The child's `print` is
`env.print`, which writes to the console through the namespace. It had to be
`out`, and it took a probe that printed successfully from inside the program
three lines away to see the difference.

### A flaky test that was two assertions in a trenchcoat

`as: one space per possible process` created all thirty-two address spaces
and checked none refused. That is the constant it documents *and* an
undeclared assertion that nothing anywhere in the machine holds a single
slot - and the suite creates and destroys processes behind waits bounded by
a count of yields.

It went red in two runs of three once the suite started booting `-smp 4`.
Not for memory: thirty-two slots against about a hundred and twenty-eight
thousand free pages, so the margin on pages is four thousandfold and the
margin on slots is exactly zero.

It asks `as_total()` its size now, and separately takes only what is *free*,
which keeps the property that the pool really hands them out without the
hidden assumption. And one real leak was behind it:
`test_destroying_a_space_returns_its_pages` had a failure path that returned
without `as_destroy`, taking a slot out of circulation for the rest of the
run - one red line turning into a second one somewhere else, which is the
one people go and look at.

### `volatile` is not a barrier

Step three's own code had it. A secondary fills its `struct percpu` and then
increments a `volatile unsigned online` that core 0 spins on. `volatile`
stops the compiler caching the variable and says nothing to the processor
about the order two stores become visible in, so on AArch64 core 0 was
architecturally allowed to see the count rise before the slot was written -
and `cpu: every processor claimed its own slot` would read a zeroed slot,
once in some thousands of boots, on a machine nobody was watching.

`cpu_publish` is `dmb ishst` and `cpu_observe` is `dmb ishld`: named, paired,
stores-only and loads-only, inner shareable. Not `dsb sy`. On x86-64 they are
compiler barriers and say so, because total store ordering already provides
the rest - and an architecture where the barrier is free is exactly where a
missing one stays invisible.

**No test can prove this one.** It passes either way. It is in the tree
because it was reasoned about and reviewed, which is the honest status.

### The tree, cleaned

- **Ten build directories at the top level became one.** `build-user$(VARIANT)`
  sat beside `arch/` and `kernel/` in every listing, at 802 MB, and
  `make clean` named four of the ten - which is how the other six
  accumulated. The Makefile said a distinct top-level directory was
  necessary because `build/user/x.c.o` would also match the kernel's
  `build/%.c.o` pattern. It does not: an object keeps its source's path, so
  a userland object is `build/user/user/lib/gfx.c.o` and the kernel's
  pattern matches that only with a stem whose prerequisite does not exist.
  Make discards such a rule. Checked by building every variant rather than
  by arguing.
- **`graphify-out/` is no longer tracked** - 284 files and 8.9 MB of caches
  and reports, a derived artefact of the source committed beside the source.
- **166 files were missing the licence line** the first rule in `CLAUDE.md`
  demands, including all of `arch/aarch64/`. A rule nothing checks is a rule
  followed by whoever remembers it.
- The stray 64 MB `kosmos.img` at the root is gone.

### What the four investigations found, and what survived being challenged

Four read-only investigations, each then handed to a skeptic told to refute
it against the code. Three of the four were refuted on real points, which is
the argument for doing it that way:

- **`follow` modes: not refuted, and my own claim was wrong.** I had said no
  application uses them. Six do - `tracker`, `network`, `webserver`,
  `photo`, `mixer`, and `procs`'s table view - and five resize correctly.
  The blocker for Monitor and Cores is narrower than it looked:
  `pulse.panel` builds its view from a fresh table and never forwards
  `spec.follow`, so a declaration at the call site is silently dropped. The
  kit has no proportional mode, so `procs`'s four meters cannot be expressed
  by follow at all.
- **The x86 core clock: refuted on three counts.** `struct cpu_info`
  already has a `brand` member and `CPU_RAW_BRAND` is already 3, so the
  proposed names collide; and packing leaf 0x16's three fields into one word
  is *decoding in the kernel*, which `syscall.h` explicitly forbids - the ID
  registers cross as raw words and userland decodes them. Also settled: leaf
  0x16 is Intel-only and returns the *maximum basic leaf's* data where
  unimplemented, so an unguarded read prints a plausible wrong frequency;
  and under `make x86` on this Mac both sources return nothing at all.
- **SMP step four: the diagnosis survived, the plan gained three
  omissions.** The ARM generic timer is per-PE by architecture, so it
  already reaches every core; what does not is each core's redistributor,
  the four per-core `ICC_*` writes, and `timer.c`'s machine-wide tick state.
  The challenger added cache maintenance across the MMU-off window, the
  second board's `hal/pc`, and an unbounded `GICR_WAKER` spin that would
  hang a secondary silently. Written up for the next session.

### The network harness only looked like it was waiting

**Found by a `make prepush` that failed once and passed three times.** The
network suite's `boot()` types a list of commands and waits for the prompt
after each - except `wait_for` searches the whole accumulated buffer, and
`seen` was cleared once before the loop rather than before each command. So
a prompt from an *earlier* command satisfied every later wait instantly:
only the first command was ever actually waited for, and the transcript was
a snapshot of whatever the guest had emitted by the time Python finished
typing.

It passed nearly always, because nearly every command in that file answers
in milliseconds. The one that does not is the second `host example.com` - a
fresh query with a round trip to a real resolver behind it - and when it
lost the race the check reported, with confidence, a units bug in
`NET_OP_RESOLVE` that had been fixed days earlier.

**Naming a likely cause is worth a lot in a test and worth less than
nothing when the harness is the cause.** The message now says what was
observed, names the units bug as the known shape of it, and says to read
the transcript before believing it.

`tools/run_disk.py` already had the answer and has for months: its reader
takes a `since` offset, with a one-line comment saying the prompt is
already in `seen` from the last time. Same trap, same building, solved once
- because the two harnesses were written months apart and nothing connects
them.

### SMP step three: three more instruction streams, and they do nothing

**There are four processors running kernel code, and three of them are in
`wfi`.** `docs/smp.md` step three, done *before* step two, which is the
part of this worth arguing rather than the code.

```
[3/12] processor
       ARM Cortex-A72 r0p3  (MIDR_EL1 0x410fd083)
       -> 4 processors, 1 scheduling
       -> 3 of the others in the kernel too, parked in wfi
```

**Why not the locks first.** Step two is "take locks with one core, let
them be uncontended", and it is *untestable* in the state the kernel is in:
both boards already enter with interrupts masked - AArch64 by architecture,
x86 through an interrupt gate - so on one core no pool needs a lock, and
every lock added would be one nothing contends, nothing fails on, and
nothing checks. That is a large diff on faith. Step three has the opposite
property: a parked core touches no shared structure, so it needs none of
them, and it is the only way to find out whether the per-CPU register from
step one is genuinely per-core - which step one could assert and could not
check. The suite now asks core *i* for its own index and gets *i*.

**Two bugs, and neither was concurrency.** Both were ordering, which is not
what one braces for when starting a second processor.

`smp_start_others` was called fifty lines before `mmu_init`, so the
secondaries turned translation on with tables that did not exist yet and
never arrived. The failure was *silent*: no fault, no hang, nothing on the
screen. The only symptom was the boot log saying 1 where it should have
said 4, which is exactly the line that had just been written and was
therefore the line least likely to be believed.

And `thread_cpu_count` returned `NR_CPUS`, so the machine claimed to be
scheduling on four cores while three were parked. `NR_CPUS` is **how many
slots exist**; how many are scheduling is a different number and is still
one. Collapsing them would have been claiming step four.

**Where the MMU gets turned on moved into assembly, and the x86-64 link
error is what said so.** `secondary_main`'s first statement was
`mmu_enable_here()` - an AArch64 function called from a file in `kernel/`,
which refused to link the moment `kernel/smp.c` joined the x86 build. The
link error was right. How a newly started core reaches a state where it can
execute C is what `arch/` and `boot/` are for, and the two machines do not
agree: a secondary here arrives with the MMU off, one on x86-64 will arrive
from a real-mode trampoline already in long mode, because long mode
requires paging. So `boot/start.S` calls it, and `kernel/smp.c` has no
architecture in it at all.

That left one thing `kernel/smp.c` still needed to know - *where* a core
should land - and it is an architecture fact rather than a board one. The
board knows how to **start** a processor (PSCI here, `INIT`-`SIPI`-`SIPI`
on a PC) and not where it should begin executing. `cpu_secondary_entry` in
`arch/` answers that, and answers **0** on x86-64, where no trampoline
exists. Two separate refusals - `arch/x86_64/cpu.h` has no landing pad,
`hal/pc/cpu_on.c` has no local APIC to send the sequence with - and they
stay separate so that building one does not silently look like building
both.

**`make qemu` boots four processors now**, not just `make test`. `SMP ?= 4`
in the Makefile, and `make SMP=1 qemu` is the one-core machine. The bug
above is the argument: it was invisible except in a boot line, so the
bring-up path should run every time somebody boots this thing rather than
only when the suite does.

132 checks on AArch64 and 128 on x86-64. The new one is *every processor
claimed its own slot* - `percpu_at(i)->index == i` for every core that
arrived - and it is the check step one could not write, because on one
processor every answer is the same answer.

### The machine has four processors and says so

**`make test` now boots a four-core machine.** The other three are parked
in firmware and never enter the kernel, so nothing behaves differently -
but the kernel *knows they are there*, and on `-smp 1` a working discovery
is indistinguishable from a hardcoded 1.

**PSCI has no "how many processors" call**, and there is no device-tree
parser here to read `/cpus` from. What it has is `AFFINITY_INFO`, a
question about a *specific* processor - on, off, or coming on - which
answers `INVALID_PARAMETERS` for one that does not exist. Ask about 0, then
1, then 2, and count up to the first refusal. It is a pure query: it starts
nothing, which is why it can be asked at boot, long before anything is
ready to have a second core running in it.

```
[3/12] processor
       ARM Cortex-A72 r0p3  (MIDR_EL1 0x410fd083)
       -> 4 processors, 1 scheduling
       -> 3 of the others in the kernel too, parked in wfi
```

**Two numbers because they are not one.** `hal_cpu_count` is what the
machine has; `NR_CPUS` is what this kernel was built for. Reporting only
the second would describe a four-core laptop as a one-core machine - true
about Kosmos and false about the computer - and the gap between them is the
honest measure of how far this has got. `machine`, `cores` and `sysinfo`
all carry both now.

x86-64 answers 1 and `hal/pc/cpus.c` says why: the count is in the ACPI
MADT and nothing here parses ACPI. One is *true* - the kernel schedules on
one processor - where zero would be false and a guess would be worse than
either.

**And a design question the plan had not asked**, prompted by the fact that
every machine after QEMU has processors that differ from each other. The
Alder Lake in `docs/targets.md` has six performance cores of two threads
each and eight efficiency cores of one; ARM has had the same shape for
longer under a different name. **A count cannot describe that**, and there
are three separate ways two hardware threads differ - kind, whether they
are siblings on one core, and which cache they share.

`docs/smp.md` has a section on it now. The conclusion is not to build it:
it is that the *runqueue split at step 5* is the one decision that would be
expensive to get wrong, because a design where threads are enqueued
centrally and pulled by whichever core is free cannot express "this one
belongs on a P-core", and retrofitting that is a rewrite rather than an
addition. Symmetric and correct first, on cores that are all alike, which
is what QEMU gives - with the structure not closed against the day they are
not.

### `cores`, an instrument built before the thing it measures

**There is one processor, so this shows one bar.** That is the point rather
than a limitation: `docs/smp.md` step one moved the state that belongs to a
core into `struct percpu`, and nothing could see it. An app that would show
a second bar is what makes the second core visible when it arrives, and
this project builds the instrument first - `jitter` measured the noise
floor before anybody optimised against it, and `frames` measured where a
pass went before the window manager was touched.

What it took, downwards:

- **`sysinfo` grew `cpu[CPUS_MAX]`**, a `struct cpuload` per core. The
  machine's *total* stays where it was, because `htop`, `procs`, `monitor`
  and `sysmon` all read it and a total is what those four want. **A total
  cannot answer the question SMP raises**: one core pinned and three asleep
  sums to the same number as four cores at a quarter each, and those are
  opposite machines.
- **`info.cpus` stopped being the literal 1** and reads `thread_cpu_count()`.
- **`sys.cpuload()`**, an array straight out of `sysinfo`, exactly as
  `sys.bus()` is and for the same reason: it is an array, and the table
  protocol a device node speaks is for flat facts.
- **`sysmon` shows a meter per core**, labelled "processor" while there is
  one and "processor 0", "processor 1" when there are more - the same rule
  the Deskbar's window list follows.
- **`cores`**, the app: a bar per core, and buttons that add and remove a
  compute-bound worker so a person can watch one fill.

**The worker is `/bin/spin.lua`**, which exists for exactly this and says so
in its own first line - it burns a core and deliberately does not yield.
Which makes the app a demonstration of the priority bands as much as of the
cores: the window is DISPLAY and the spinner is NORMAL, so the bars keep
moving and the buttons keep answering while a core is pinned at 100%. If
they ever stopped, that would be the responsiveness claim failing and this
is where it would show.

**And the instrument found a bug in the plumbing it was written for**,
which is the argument for building it. The loop filling `sysinfo.cpu[]` was
bounded by `info.cpus` - a field assigned *seventy lines further down* - so
the bound was zero, the array stayed as `memset` left it, and every core
read 0% with two spinners running. Nothing else in the suite would have
noticed: the total was right, and the total is what everything else reads.

**Two more mistakes worth recording, both in the test rather than the
code.**

The check looked for `theme.good` and found nothing, because one spinner on
one core settles at *100%* and the bar had already turned `theme.bad`. A
test that knows only the calm colour fails exactly when the machine is
busiest, which is the case it exists to check.

And the first version left the worker running. `spin 600` is ten minutes
and it is *detached*, so Control-C to the window manager does not touch it -
the next phase's window never appeared and the harness blamed the window
rather than the spinner still burning the core behind it. Taking the worker
off is now part of the phase, and it is the better assertion anyway:
watching the meter fall proves the kill reached a process the app started.

67 display checks on each board now.

### SMP step one: the per-CPU struct, and the audit that found a seventh

`docs/smp.md`'s own first step - *"the per-CPU struct and the register that
finds it. Single core still, with `NR_CPUS = 1`. Nothing behaves
differently; everything moves."* Done, and 130 checks say nothing behaves
differently.

**Re-auditing the document against the code first was worth it twice.**

It listed six things that have to become per-CPU and there are seven.
`preempt_pending` in `thread.c` is a `volatile bool` saying a switch is owed
on the way out of the current exception - which is a statement about *this
core's* return path and nothing else. Found by reading `thread.c` for the
other six.

And it said of `TPIDR_EL1` and the `GS` base that "neither is written. This
is the smallest piece of the work and it touches the most files, because
every exception entry has to establish it." **Half of that is wrong about
AArch64.** `TPIDR_EL1` is *banked*: EL0 cannot see it or change it, so it
is set once per core at boot and read from anywhere afterwards, and no
entry path is touched at all. x86 has one `GS` for both privilege levels
and does need `swapgs` at every boundary - which is real surgery on
`vectors.S` and `user.S`, and belongs with that board's second core rather
than before it. So the step cost one store at the top of `kmain` instead of
a pass over the vectors, and that asymmetry is now the strongest single
reason `smp.md` puts AArch64 first.

**Two decisions inside it.**

`current` became a macro over `this_cpu()->current` rather than
twenty-nine edited call sites. Linux's idiom, for Linux's reason: those
sites were correct and say what they mean, and a large diff whose entire
content is a change of spelling is exactly where a real change hides.

`percpu_init(0)` is the **first line of `kmain`**, before `hal_early_init`.
`thread_current` reads through `this_cpu`, and the fault handler asks for
the current thread on its way to reporting - so an exception arriving
before it would take a second fault instead of printing the first. Deleting
that line and rebuilding panics at boot with a data abort, which is a
better failure than a test going red.

**And a test that asserts the distinction the x86 port got wrong.**
`user_rsp` was a global holding the interrupted stack pointer; it looked
like per-CPU state and was per-*thread*, and one core was enough to prove
it. So `cpu: per-CPU state is per CPU, not per thread` has two threads look
at `this_cpu()` and requires they see the same one. On a second core it
becomes a different assertion, written when there is a second core to write
it against.

Nothing is per-CPU yet that was not a global before, `NR_CPUS` is 1, and
the runqueue is still `head[]` and `tail[]` at file scope - that is step 5.

**And a flake that turned out to be the bug the file already documents.**
`sched: the policy is pluggable` failed once on each board today under a
loaded host, and the cause is `run_three_and_record` waiting out twelve
`thread_yield()` calls. Twelve yields are twelve *switches*, not twelve
turns for the three threads being watched, so on a busy machine they
sometimes are not enough - which is word for word the mistake
`ipc: a receive with a deadline gives up` records twenty lines further
down. Bounded by the clock now, which also returns the moment the three
finish rather than always yielding twelve times.

### The kernel's prose caught up with its code

The last item the 0.9.0 review left open. `kernel/` compiles for both
architectures with no `#ifdef` in any of its thirteen files, and its
comments still explained it in one architecture's vocabulary: EL0 and EL1
for the privilege levels, SP_EL1 for the stack an exception lands on, TTBR0
for the page table root, CNTFRQ_EL0 for the counter's rate, `svc` for a
syscall.

**Thirty-seven references across nine files; about twenty-five rewritten
and twelve left alone**, and the twelve are the interesting half: every one
of them names *both* boards on purpose - the syscall ABI section that
contrasts x8 with System V, `sysinfo`'s raw-register block, the note that
AArch64 never showed the sleep bug, and one historical record of a boot-log
string that used to say MIDR_EL1. Those are not drift; they are the file
doing its job.

**Two were wrong rather than parochial**, which is the reason this was
worth doing rather than a tidy:

- **`USER_TEXT_VA`, `USER_HEAP_VA` and `USER_STACK_TOP` carried absolute
  addresses in their comments** - 0x80000000, 0x81000000, 0x82000000 - and
  `USER_VA_BASE` is 0x40000000 on x86-64. All three were simply false on
  the second board. The offsets are the invariant and the base is the
  board's, so they say `base + 16 MB` now and the base is named once, with
  both values.

- **`process.c` explained why a shared code mapping is safe in ARM's terms
  only**: "both mappings are Normal, inner-shareable, write-back - what ARM
  forbids is mismatched *attributes*, not different permissions". The claim
  is right and general - x86 has the same requirement through the PAT and
  the MTRRs - but as written it read as an ARM guarantee the x86 build was
  relying on without saying so. Now it says the requirement and then how
  each architecture spells it.

That second one is the shape worth remembering. A portable file explaining
itself in one architecture's vocabulary is not merely untidy: it hides
whether the *claim* is portable or only the code is.

### Targets: three axes, not two, and one question worth half an hour

`docs/targets.md` is new and is the model for what a machine *is* here.
Written because a target brief for an Alienware x14 arrived and reading it
against the tree showed the obvious grid - architecture down one side,
board along the other - is very nearly right and breaks in a specific
place.

**The evidence is three machines and what any two of them share.** QEMU's
q35 and the x14 share an instruction set and *almost no device*: boot,
discovery, screen, interrupts, timer, PCI access, keyboard, storage and
network all differ. The x14 and the Pi 5 share *no* instruction set and
will share the entire USB stack - which on both is where the keyboard, the
disk and the network arrive, and which is thousands of lines.

So drivers group by **what the device is**, not by architecture and not by
board. Three axes: `arch/` for the instruction set, `hal/<platform>/` for
*how a machine says what it has*, and a pool of drivers matched at run
time.

**And on a PC there is no board - there is a bus.** Embedded machines do
not enumerate: the Pi 5's UART is at an address you are told. A PC
enumerates itself through ACPI, PCI and USB, so a PC target is a *discovery
mechanism* plus a driver pool, and `hal/alienware-x14.c` would be wrong for
the next laptop and wrong for this one after a firmware update.

**Two things this made honest.** `hal/pc/` is `hal/qemu-q35/` wearing a
general name - `fwcfg_port.c` is QEMU's, the framebuffer is QEMU's ramfb,
and `power.c` hardcodes an ACPI address its own comment calls "QEMU's, not
a PC's". And `hal/virtio/` plus `hal_bus_scan` are already the pool and
already the discovery: one `blk.c` drives one card over two entirely
different transports, and the bus scan already reports what it found and
whether a driver claimed it. What is missing is drivers, not a mechanism.

**On the x14 specifically.** `arch/x86_64` is done and must not be rebuilt -
the brief opens by saying Kosmos is AArch64-only, which stopped being true
at 0.9.0. What is missing is the platform: a UEFI loader, ACPI without AML,
APIC and MSI, HPET, PCI over ECAM, a framebuffer from GOP, and i8042. Very
roughly 2800 lines, all of it developable under QEMU with OVMF, and it ends
at a booting desktop on the panel. Then xHCI and the USB stack is 5000 to
7000 and is a step change - the largest single thing this project would
have attempted.

**The blocking unknown, and it is cheap.** If the internal keyboard is
i8042 the first milestone produces a machine you can type on. If it is USB
there is no input at all until xHCI, and the milestone moves from about
2800 lines to about 9000 - weeks against months. It is answered by booting
any Linux stick on the machine and reading `dmesg | grep i8042`. Nothing
else about that target should be planned first.

### On x86-64, `sys.sleep` had never slept

**Found by checking that step one worked**, rather than by the suite going
red. `make test` passed on both boards before and after; what it does not
have - did not have - is any check that a *duration* is a duration.

`ping` sleeps exactly one second between echoes, so five echoes is four
seconds of sleeping and nothing else. Timed from the host:

```
AArch64   expected 4.0s, took 4.29s     (and 2.0s -> 2.23s)
x86-64    expected 4.0s, took 0.16s     (and 2.0s -> 0.14s)
```

The residual on AArch64 is a constant ~0.25 s of round-trip overhead rather
than a scaling error - the two measurements are off by the same *absolute*
amount, not the same percentage, which is the signature that says the rate
is right. Solving both puts it within 3%.

**It predates step one.** Checked against 0.9.6: same 0.16 s. Not a
regression - a defect the change happened to make visible, because
verifying it meant measuring something nobody had measured.

**The deadline was correct and the wake was wrong.** Instrumented, x86 said
`want=999292000 got=3683000` - one second asked for at a counter it had
correctly measured at 998.9 MHz, and 3.7 ms delivered, which is one
scheduler tick.

`thread_wake_sleepers_now` woke **every thread carrying a `wake_at`**. Its
purpose is to wake threads for whom an arriving key is the thing they were
waiting for; `wake_at` says when a thread would like to be woken and
nothing about what it is waiting on, and a `sys.sleep` carries one while
waiting for nothing.

**Why only x86, with identical code on both boards.** `input_arrived` is
set by an input interrupt and cleared in exactly one place - `SYS_WAIT_INPUT`,
console owner only. Nothing calls it at a bare prompt, so the flag latches
true and every timer interrupt runs the path:

```
AArch64      thread_wake_sleepers_now fired     0 times in 5s
x86-64                                       1200 times in 5s
```

AArch64 escaped by never setting the flag, which is luck. So on x86 every
timeout in the system was four milliseconds - `sys.sleep`, `fs.wait_input`,
an IPC receive with a deadline, every server wait. **It looks like a fast
machine**, which is why it survived a port, a test suite and a display
harness.

Fixed with a `wake_on_input` flag set only by `SYS_WAIT_INPUT`, through
`thread_wait_input_until` - a separate entry point rather than an argument,
so an ordinary sleep cannot acquire the behaviour by accident, which is how
it had it.

**Two tests, and the interesting one is that the obvious test does not
work.** "A sleep lasts as long as it asked" is a property nothing had ever
checked on either board and is worth having - and it **passed with the bug
deliberately put back**, because the trigger is an input interrupt and the
test image never has one. `check_latency` in `run_screenshot.py` already
records that a test which passes with the bug reinstated is worse than no
test; this is the second instance.

The one that works calls `thread_wake_sleepers_now` directly - which is
exactly what an arriving key does - with a plain sleeper blocked. Three
lines, deterministic, both boards, and it fails the moment the distinction
is removed. Verified by removing it.

129 checks on AArch64 now, 124 on x86-64.

### One clock in the kernel, and a section that says how time works

**Step one of three**, and the point of it is that the kernel now has one
time base. `architecture.md` §5 is the whole account - three clocks, what
each answers, why two of them exist, the rates and why the ratio is not
memorable, where the two meet, what went wrong twice, and where this is
going. `CLAUDE.md` carries the rule as a principle.

**What changed is five sites.** Every deadline a thread holds - `wake_at` -
is a *counter* value now rather than a count of scheduler ticks, and
`thread_deadline_in` is the only place in the kernel that converts. It is
reached from `SYS_SLEEP`, `SYS_WAIT_INPUT` and a receive with a timeout,
and from nowhere else.

**Two things were wrong with `hal_ticks() + n`, and only one of them is
about units.**

`hal_ticks` counts interrupts **actually taken**, and `hal_ticks_missed`
exists in the HAL because they are not always taken. So a machine under
load ran every sleep long by however many ticks it had missed - a clock
that stretches exactly when something is already going wrong, which is the
worst possible moment for a timeout to grow.

And a count of interrupts cannot be handed to a comparator. The periodic
tick could never have been removed while deadlines were expressed in it,
which is why this had to come first.

**What did *not* change: the granularity.** A wakeup is still checked on the
timer tick, so it is still no finer than 4 ms. That is step three's, and it
needs step two - the LAPIC timer on x86, which SMP wants anyway because the
8254 PIT is one device for the whole machine.

**The test caught the one caller I missed**, which is worth recording
because it is the case the suite exists for. `starve_sleeper` in
`tests.c` slept with `thread_sleep_until(hal_ticks() + 1)` - a tick count,
now read as a counter value, so a number always in the past. The sleeper
stopped sleeping and became a spinner at DISPLAY priority, starving the
NORMAL thread it was meant to be sharing with. `sched: NORMAL runs while
DISPLAY sleeps` is exactly the test for that and it failed on the first
run.

### DNS was built, had one caller, and had never worked twice in a boot

**The task was "write a resolver" and the resolver was already there.**
`/net` has spoken DNS for as long as it has spoken TCP: a query, a reply,
names written with a length in front of each label, the compression
pointers a real server answers with, `NET_OP_RESOLVE`, `net.resolve` in the
kit, `fs.resolve` in the namespace, and a default server of 10.0.2.3 that
`init.lua` configures at boot.

What it did not have was **a command**, and that absence cost three
separate things: a lookup could not be tried on its own, a failed lookup
could not be told from a failed route, and `make test` had nothing to
check. The only caller was the browser's address bar, four hundred lines
into a graphical application.

So `host` exists now - `host example.com`, and `host 10.0.2.2` answers
without asking anybody, because four numbers and three dots is an answer
rather than a question.

**And writing it found that the resolver had never worked more than once
per boot.**

```
kosmos> host www.google.com
www.google.com is 142.251.155.119
kosmos> host example.com
host: example.com: the resolver did not answer in time
```

**It is the 0.9.1 bug, in another subsystem.** `struct net_request` has one
`ticks` field and four operations read it. Three convert it - `* net.hz /
250`, one of them with a comment saying *"scheduler ticks like every other
wait in this system, converted here because the counter is what this server
measures in"* - and `NET_OP_RESOLVE` added it to `kosmos_ticks()` raw. A
five-second deadline became **twenty microseconds**, so whichever query
beat the next sweep was answered and every other timed out.

**The browser worked because it was wrong in the same direction.** It
passed `counter_hz` - 62,500,000 - where the documented unit is scheduler
ticks. One caller and one reader, agreeing on the wrong unit, and nothing
to contradict either. `host` passed the documented unit and broke
immediately, which is what a second caller is for.

Fixed the way `wmproto.lua` fixed it: **the unit is in the name.**
`ticks` is `wait_ticks`, `netproto.h` says what it counts in and why the
name carries it, and the conversion is one function in `net.c` rather than
three copies and an omission. The literal 250 is gone too - the server
reads `tick_hz` from `sysinfo`, which is the kernel's number rather than a
fourth place to keep it in step.

**And NXDOMAIN started working, having never been reached.**
`dns_receive` reads the response code and answers `NET_ERR_NO_NAME`, and
that code was correct all along - a name that does not exist just timed out
before the reply could be looked at.

**`make test` checks it now**, three checks in `run_network.py`, and the
one that matters is that **two lookups in a row both answer**. One proves
nothing: one is what passed for the whole life of the bug. The two that do
not need a network - `host 10.0.2.2` and `host` with no argument - always
run; the real lookup runs whenever a resolver is reachable and says out
loud when it is not, because the rest of that file is deliberately offline
and a test that needs somebody else's uptime is a test that fails on a
train.

**The prose said none of this.** `browser.lua`'s own help page listed "No
names - DNS is a resolver this system has not got" under *What it cannot
do*; its header comment said "no DNS, so a remote address is four numbers";
`ping` and `telnet` both told the user "there is no DNS yet"; and
`roadmap.md` had a resolver under *Being built now*. Five places describing
the absence of something that had been there for months, because nothing a
person could type ever said otherwise.

### Both x86-64 defects fixed, and one of them was not where it looked

**123 of 127 on x86-64 now**, up from 117, and the four that remain are
about AArch64 rather than about the kernel: stepping ELR past a faulting
instruction, execution resuming after one, SPSel, and the lazy-FP mechanism
being disarmed until something wants it.

**The interrupt stack table.** Every x86-64 vector had `ist = 0` - "use the
stack we are already on" - so a fault from ring 0 stayed on the stack that
faulted. Fine for a deliberate fault and fatal for an overflow: the handler
pushed onto the guard page, faulted again delivering that, and the machine
triple-faulted and reset with nothing printed, which is the exact failure
the exception dump exists to eliminate. AArch64 gets this from the
architecture, because the kernel runs on SP_EL0 and every exception
switches to SP_EL1.

`boot/x86_64/kosmos.ld` now carries a 16 KB exception stack with a guard
page below it, matching `boot/kosmos.ld` line for line; `gdt.c` puts it in
`tss.ist[0]`; and `trap.c` points #PF and #DF at it. **Two vectors and not
all of them**, because an IST stack is not reentrant - the processor loads
the same address every time, so a fault taken while one is being handled
overwrites the frame being handled. #PF is where an overflow arrives and
#DF is the backstop for a #PF that could not be delivered, which is now
only possible if that stack is itself bad, and its own guard page catches
that.

The test that checks it needed one change: **it faults with a store rather
than an undefined instruction.** An IST entry belongs to a *vector*, and
`ud2` is a fault by code that still has a working stack. A test asking "does
the handler have its own stack" has to fault the way a stack overflow
faults, which is a memory access.

**And the block device, which was not a block-device bug at all.**

Every hypothesis was wrong and each was cheap to rule out: the mapping (the
capacity read through it was exact), bus mastering (`pci.c` sets
`COMMAND_MASTER`), the feature negotiation (VERSION_1 offered, taken,
`FEATURES_OK` reading back), the queue enable, the memory barriers (both
carry a `"memory"` clobber, so no read was being hoisted), and a double
init. QEMU's own trace ended the guessing in one line:

```
virtio_blk_handle_write vdev ... sector 1 nsectors 1
virtio_blk_rw_complete  vdev ... ret 0
virtio_blk_req_complete vdev ... status 0
virtio_notify           vdev ...
```

**The write completed.** The device did the work, wrote the used ring and
raised its line - and the guest made no further progress at all. Not a spin
loop failing to see a completion: an interrupt storm around it.

`hal_irq_handle` offered every non-timer line to `input_interrupt`,
`snd_interrupt` and `net_interrupt`. There was no `blk_interrupt`. **A PCI
INTx line is level-triggered and shared** - four pins handed out among every
slot - and a device holds its line asserted until its own interrupt-status
byte is read, reading being what acknowledges it. So a completed request
asserted a line that some *other* driver had unmasked, nobody read this
device's ISR, the PIC was told the interrupt was handled, the line was still
asserted, and it fired again immediately. Forever.

**AArch64 never had this, and not because that driver is better.** There
every device has an interrupt ID of its own, blk never enables its own, and
a line that is never enabled is never delivered. **Sharing is what turns
"this driver ignores interrupts" from a choice into a defect**, and PCI is
where sharing arrived. The driver still spins on the used ring and still
never sleeps; acknowledging is the whole of `blk_interrupt`'s job.

**What this says about the exercise.** Neither defect was in code the port
wrote. Both were in the space *between* what one board provides and what
the other assumes - a stack the architecture hands you, an interrupt line
the architecture keeps to itself - and neither was reachable by reading. It
took running the suite that had only ever run on one machine.

### `tests/tests.c` runs on x86-64: 117 of 127, and the ten are named

**The largest test asset here had run on one board for as long as there
were two.** 4,000 lines, 127 checks, and the only thing that exercises the
kernel from inside it - and there was no x86 image target for `TEST=1` to
build. Now there is: `make test` runs it on both.

**117 of 127 on x86-64, 127 of 127 still on AArch64**, and nothing was
weakened to get there. What moved is where the machine-specific parts
*live*, not what any test asserts.

**Most of the abstraction already existed and nobody had used it.**
`cpu_irq_disable`, `cpu_cycles`, `cpu_interrupts_save` and `cpu_current_el`
are the sixteen sites the 0.9.0 review moved out of `kernel/`, and
`cpu_current_el` already reported 1 for the kernel on both boards - so a
test asking "am I privileged" asks one question in one unit. The suite got
all of that for free. Three new headers hold what was left:

- **`tests/machine.h`** - the deliberately awkward: an instruction chosen to
  fault, a store the compiler may not reason about, a named callee-saved
  register, a page-table entry's frame and permissions. Named `machine.h`
  and not `cpu.h` because `-Itests` is on the compile line and a second
  `cpu.h` would shadow the architecture's own for every file in the build.
- **`tests/fault.h`** - `FAULT_EXPECT`, and the classifiers that say what
  kind of fault it was without saying which board's encoding.
- **`tests/exit.h`** - how the guest tells the host it passed.

**The crux was that on x86-64 you cannot step past a faulting
instruction.** AArch64's handler does `elr += 4` and the arithmetic is
exact, every A64 instruction being four bytes; an x86 instruction is one to
fifteen bytes and its length cannot be known without decoding it. So there
is no next instruction to step to and no honest way to invent one. What
that architecture gets is the *unwind* form - which ARM already had, for
the stack-overflow case where stepping would have resumed into the guard
page - and `FAULT_EXPECT` is the one macro that lets a shared test use it
on both. It cost ARM nothing: the mechanism was already there and already
tested.

**The exit is asymmetric on x86 and that is the honest shape.** ARM has
semihosting, one instruction and an exact status. x86 has QEMU's
`isa-debug-exit`, which exits `(value << 1) | 1` - always odd, and
therefore *incapable of expressing success*. So success is an ACPI
power-off, which exits 0, and only failure uses the debug port. Two
different things happened and on this machine they leave by two different
doors.

**The six AArch64-only checks, and each is about AArch64 rather than about
the kernel:** ELR pointing at the faulting instruction, execution resuming
after one, SPSel being 0, the handler having a stack of its own, a stack
overflow being survivable, and the lazy-FP mechanism being disarmed until
something wants it. The last three are the interesting group - x86 sets
`ist = 0` for every vector, so a fault from ring 0 stays on the stack that
faulted, and a *kernel stack overflow* there would push onto the guard
page, fault again, and triple-fault the machine. **That is a real gap in
the port** and the fix is an interrupt stack table entry for #PF and #DF.

**And four are a defect: the block device.** The disk is claimed correctly
on x86 - `hal_blk_init` returns true and reports the right capacity out of
configuration space - and then the first request is notified and never
completes. Ruled out: the mapping (the capacity read through it is exact),
bus mastering (`pci.c` sets `COMMAND_MASTER`), feature negotiation
(VERSION_1 offered, taken, `FEATURES_OK` reads back) and the queue being
enabled (`COMMON_QUEUE_ENABLE` before `DRIVER_OK`). And it is specific to
*this image*: `run_disk.py` passes 26 checks on the same board with the
shipping kernel, reading and writing the same disk through the same driver.
Finding out which difference matters wants QEMU's own virtio tracing rather
than another hypothesis.

**What the port found on the way**, and none of it was about x86 assembly:

- **`arch/x86_64/fp.c`'s header comment was wrong three times over** - that
  the mechanism was not built, that the 512-byte area was not in
  `struct context`, and, the dangerous one, that "preemption is not safe,
  and nothing here preempts yet". Both halves of that last were false:
  the timer preempts on this board like any other, and `switch.S` does
  `fxsave`/`fxrstor` around every switch, which is precisely what makes it
  correct. Written before those two instructions existed and never revisited.
- **A test asserted a kernel address by writing it out.** `as: the kernel
  region is refused` named 0x40000000, which is a kernel address on ARM and
  is *exactly* `USER_VA_BASE` on x86 - so it asked the kernel to refuse the
  first page of user space, the kernel correctly mapped it, and the test
  reported a failure entirely its own. A literal address in a portable test
  is a claim about one machine.
- **A mechanical conversion put a `return` inside `FAULT_EXPECT`**, which
  would have left the fault armed past the statement. The compiler caught
  it, which is the whole reason that restriction is written down in the
  macro's comment.

### A clipboard, and what a machine with no modifier keys does about it

**Text selected in one application and pasted into another**, which is the
first thing in this system that two programs have shared other than a file.
Three pieces, and the shape of each was decided by a constraint rather than
by taste.

**The window manager holds the buffer.** A clipboard is state shared
between programs that are not allowed to reach each other - the same shape
as the screen and the console - so it gets the same answer: the one process
both of them already talk to holds it, and everybody asks. No global name,
no shared page, and an application that was never handed `/app/wm` has no
clipboard, which is the correct answer rather than a missing feature.

**The keys had to go behind the prefix, and Control-C is why.** There are
no modifier keys here: a virtio keyboard gives Control plus a letter and
nothing else, and Control-C is already what stops a program - nine checks
in `run_screenshot.py` use it to get the screen back from the desktop, from
`plasma`, from `cube3d` and from a terminal. So the CUA triple was not
available, and the alternative was inventing one out of whatever control
codes are unclaimed, where every candidate carries somebody's prior: `^Y`
is paste to half the world and copy to nobody.

`Control-W a c x v` instead, under the prefix that exists precisely because
this machine has no Meta key. Behind a prefix the letters can be the ones
everybody already knows, which is the whole point of having one - and it is
the first thing added under it since the window-moving arrows.

**What crosses is the intent, not the text.** The prefix posts
`{type = "copy"}` to the focused window; `window:dispatch_edit` hands it to
the focused widget; the widget decides what its selection is and sends the
bytes back as a `clip_put`. The window manager ends up holding a string it
never looked inside, which is the same division it already keeps with
pixels, and no widget has to know which keys a board happens to have.

**Selection is an anchor and a cursor, because shift is not readable.**
Keys arrive as a byte stream with the arrows decoded out of `ESC [ A`-`D`,
so shift-plus-arrow is the same four bytes as an arrow. `ui.editor` holds
the two carets and nothing else: the press sets the anchor, the drag moves
the cursor, and any key that moves the cursor on its own drops the anchor -
which is why a click deselects without anything having to say so.
`ui.field` gets select-all and no dragged range, deliberately: one line of
text has one selection anybody actually makes.

**The cap had to be enforced in the caller, and that was the one real
bug in the design.** A clipboard entry travels inside a message, `MSG_BYTES`
is 2048, and a table too big to serialise makes `fs.send` *raise* - so
capping in the server would have been too late by one process, and an
application that copied a four-kilobyte report would have died where it
stood. `wmproto.copy` cuts at 1900 before sending. The window manager keeps
a bound of its own anyway, because a server does not get to assume its
callers are the library.

**And the truncation is shown rather than mentioned**, which is the part
worth keeping. When more was selected than fits, `ui.editor` moves the
highlight back to exactly the run that left. A selection is already a
picture of a range of text, so shrinking it makes the limit something you
can see, in the one place you were already looking - where a dialog saying
"1900 of 4212 bytes" would be the same fact, later, and in the way. The
display harness checks it by measuring that the highlighted area gets
*smaller* when you copy, which is a thing that cannot pass by accident.

**65 display checks now, on both boards**, the three new ones being the
drag, the paste into another application, and the cap.

**What it does not reach yet, and it is the obvious one: the terminal.**
That is where people most want to copy from, and it draws its own
scrollback through `ui.view` rather than through `ui.editor` - so it has no
anchor and no cursor and `Control-W c` does nothing in it. Answering copy
with "all of the scrollback" would be ten lines and would be a lie about
what copy means, since nothing would be highlighted. The honest fix is to
lift the selection machinery out of `ui.editor` so both can hold it, and
that is a refactor rather than a line.

**`This Machine` is the first thing that needed it.** The report it prints
is text whose one job after being read is being sent to somebody else, so
it is a read-only `ui.editor` rather than a widget: drag over it, or
`Control-W a`, and `Control-W c` takes it away. A read-only editor answers
copy and select-all and refuses the keys that would change what it says.

### The review before 0.9: four passes, and what prose costs

**Twenty-five commits of a second architecture, reviewed as `CLAUDE.md`
asks - not a skim, and reading the files as they now are.** The spine of it
was a pattern the port itself created: **x86 doubled several lists, and
nothing checks that they agree.**

`X86_FLAGS` against `CFLAGS_BASE` lost three things in a row, each found
only when something broke - the user base address, the screen size, and the
heap size. That last one killed the TinyGL demos with a page fault at
`USER_HEAP` plus exactly 512 pages, because the kernel mapped 512 and the
userland compiled into it believed 3072.

**What the four passes found:**

*The doubled lists.* The two flag lists now differ in exactly two places
and both are deliberate: `-Iuser`, which the userland needs through
`UCFLAGS` and the kernel merely carries, and `TESTDEFS`, which is the
finding - **`tests/tests.c` has never run on x86-64.** 127 checks, 4,085
lines, the largest single test asset here, and there is no x86 test image
target for `TEST=1` to build. `SRCS` against `X86_SRCS` is otherwise honest
pairing - `gic.c` against `pic.c`, `el0.S` against `user.S`.

*Prose that asserts hardware.* One live bug: `about.lua` said "a
microkernel with a Lua userland, on AArch64" four lines above a `Platform:`
field reading `QEMU q35 x86-64`, so the window disagreed with itself in one
screenful. It asks `/dev/cpu` now. Everything else was either guarded
already - `init.lua` does it exactly right, `c.arch == "x86-64" and "a
calibrated TSC" or "AArch64 cannot read that"` - or a comment.

*Documents against code.* `CLAUDE.md` claimed a per-CPU struct, `TPIDR_EL1`
and a per-CPU runqueue. **All three were false, and had been since the
repository's first commit** - written before there was a kernel to
describe, in the present tense, and never revisited. Corrected in place
rather than deleted, and `docs/smp.md` now counts what SMP would actually
take. The other load-bearing claims hold: no loose `volatile` on MMIO, no
hardware addresses outside `hal/`, hot reload genuinely gone.

*Dead weight.* Clean, which is the one pass that came back empty.
`sched_rr.c` looked like a candidate and is not - it is selectable at
runtime through a policy list, which is the whole point of the seam. Every
harness is reachable from a make target now; `run_disk.py` was not, and had
been asserting behaviour `init.lua` deliberately replaced for months.

**Two findings left open rather than fixed**, because both are real work
rather than a line:

- **`tests/tests.c` on x86-64.** Needs an x86 test image target. The
  suite that most directly exercises the kernel has run on one board only.
- **43 AArch64 references in `kernel/`'s comments**, across nine files.
  The code was made portable and its prose was not: `process.c` still
  explains a mapping in terms of "an L1 slot" at `0x80000000`, which is
  the ARM number, in a file that compiles for both.

**The lesson, and it is the same one four times over: code has `make test`,
and prose has nobody.** Every wrong thing found tonight was a sentence -
in a comment, a string, a document or a Makefile variable - that was true
when written and was never asked again. The port did not introduce them; it
made them visible, because a second machine is a second reader.


### x86-64: it runs the desktop

```
kosmos> cpu
x86-64
  architecture  x86-64
  cores         1  (SMP is not on yet)
kosmos> mem
510 MB of RAM at 0x100000, in 130783 pages of 4 KB
  32 MB used, 478 MB free
```

**Twelve boot stages, twelve processes, the whole Lua userland**, on QEMU's
q35. `make x86` builds and runs it; `make KVM=1 x86` runs it on an x86-64
Linux host's own cores. `tools/run_x86.py` is in `make test` and boots it
twice - once to type at it, once to run a program from the loader's command
line.

**The kernel was portable and the userland more so.** All thirteen
`kernel/*.c` compile for it with no `#ifdef` in any of them, after closing
six places where AArch64 had leaked out of `arch/` (below). The userland -
fifty thousand lines of servers, libraries, applications and Lua - needed
six syscall stubs, an entry stub, a `setjmp`, two ring barriers and one
`#if`: 30 of 30 Lua files and 36 of 37 of `user/` compiled unchanged.

**Four differences that only showed up when it ran**, and each is a place
where an ARM habit is silently wrong here:

- **`hlt` is not `wfi`.** ARM's wakes on a pending interrupt with the mask
  set; `hlt` with IF clear halts for ever. The idle loop masks across the
  check and the sleep on purpose, so the unchanged loop gave twenty timer
  interrupts and then a machine that had printed its prompt and stopped.
  `sti; hlt`.
- **`syscall` saves nothing.** `svc` is an exception and the vector writes
  thirty-one registers into the trapframe. `syscall` writes rcx and r11.
  The entry stub has to preserve everything the kernel's C will clobber.
- **The interrupted stack pointer is per-*thread*, not per-CPU.** A global
  looked right and one core disproved it: a process that blocks in IPC lets
  another run, which overwrites it. ARM's frame is on the thread's own
  SP_EL1 stack and the question never arises.
- **`thread_tick` only records.** ARM's vector epilogue calls
  `thread_preempt_if_needed`; without the same call this counted ticks
  correctly and never switched.

**And two dependencies nobody had decided on**, both found because one
toolchain bundles what the other does not: `<inttypes.h>` is not a
freestanding header, and `-lm` was newlib's. musl's maths is vendored in
`runtime/upstream/musl-math/` now and both machines compute `sin` with the
same code.

### `hal/pc/` is done, and the drivers did move rather than being rewritten

**The four virtio drivers live in `hal/virtio/` now** - block, network,
input, sound - with each board bringing its own transport underneath them:
`hal/qemu-virt/virtio.c` reads fixed offsets from a device-tree window,
`hal/pc/virtio.c` walks PCI capabilities to find four structures scattered
across a BAR. The sequence is identical and every register is somewhere
else. `hal/fwcfg/` came out the same way, because ramfb is found through
fw_cfg on both boards and only the two register accesses differ.
`absent.c` is gone; there is nothing left for it to list.

**The measure worth quoting is the display harness**, because it asks QEMU
what is on the screen rather than asking the guest:

```
PASS: 62 display checks     x86-64,  108.6s in phases
PASS: 62 display checks     aarch64, 109.0s in phases
```

The same sixty-two - a keyboard, a pointer, dragging a hung window,
scripting a running application, a replicant moved between processes, the
Deskbar, the terminal, an idle desktop being idle. And `make test` runs
four harnesses on the x86 image now rather than one: the boot test, the
headless test, the disk and the network. `make screenshot` checks both
displays.

**Three device bugs, and none of them is about x86.**

- *`virtio_open` reset the device it had only been asked to find.* Fine for
  a driver that wants the first card of its kind; wrong for `input.c`,
  which walks past devices on the way to the one it wants - a keyboard and
  a tablet are both virtio-input and telling them apart means opening each.
  So the tablet's scan reset the running keyboard, leaving it with no
  queues and no DRIVER_OK. It worked on ARM by luck: QEMU lays virtio-mmio
  windows out in the reverse of the order the devices are given, so the
  tablet came first, and swapping two flags on the ARM command line would
  have broken it identically. Discovery and claiming are two calls now,
  `virtio_open` and `virtio_begin`.
- *A declaration left behind in a board's header.* `keyboard_getchar` moved
  into the shared `hal/virtio/input.c`; its declaration stayed in
  `hal/qemu-virt/qemu-virt.h`, which is exactly where the second board
  cannot see it. `hal/pc/uart.c` read only the 16550, so the machine found
  a keyboard, said so in the boot log, and could not be typed at.
- *The screen size was never passed to this build.* `FB_FLAGS` is on one
  file's compile line on ARM for a good reason that does not apply to a
  build with no object files. Left off, `ramfb.c` took its 1024x768
  fallback and the machine came up at a resolution nothing had asked for.

**And two where a string was the bug.** `/dev/cpu` had no x86 decoder, so
`devices` printed `nil nil nil, 1 core`; it decodes CPUID now and fills in
the same field names, because four callers each growing a branch on the
architecture would be four places to get it wrong. And the same listing said
`/dev/console  PL011 UART, polled` on a machine whose console is a 16550 at
port 0x3f8 - it says what the node *is* now, because nothing in the shell
knows what the hardware is and the honest thing is not to claim.


Then **SMP**, and then **PowerPC** - see `docs/roadmap.md`, which records
what each costs. The short version: the port already found the per-CPU
boundary for SMP the hard way, PowerPC is the first big-endian target and
the audit for that was done and holds, and a G4 is 32-bit, which contradicts
a principle and so gets decided rather than drifted past.

### The browser

**The cascade is consulted.** libcss has parsed, selected and answered since
it was vendored and nothing asked it; `web_paint.c` chose a face by tag name
and an ink from a `#define`. It asks now, per element.

**The defaults became a stylesheet.** What was `face_for()` - h1 is 28
pixels and bold, `pre` is monospace, a quotation is italic - is a
user-agent sheet in `web_style.c` at UA origin. That is how a browser has
always expressed it, and it buys the thing a switch could not: an author who
writes `h1 { font-size: 44px }` wins, because the cascade already knows UA
loses to author. No code knows that rule; libcss does. `is_hidden`'s list of
tag names went the same way and is now `display: none` on six selectors, so
a page that hides something of *its own* is hidden too.

Two special cases disappeared rather than being fixed. `<strong>` inside a
heading needed a rule saying it must not take the body's bold, or the
emphasised word came out smaller than the words around it; the cascade
computes 28 pixels *and* bold together, so the face asked for is the
heading's own bold and the problem cannot arise. And a face is now
(family, weight, slant, size) asked of `gfx` as needed, rounded to a ladder
so that continuous CSS sizes cannot fill a pool of rasterised faces.

**Inheritance without composing styles.** libcss answers `INHERIT` for a
property no rule set, so `web_style_of` takes the look the element inherited
and overwrites only what is specified. Leaving a field alone *is* keeping
the parent's. Overwriting unconditionally turns inheritance into its
opposite - a `<strong>` containing an `<a>` would have unbolded the link -
which is why every property is guarded.

**Two bugs, both invisible to the compiler.**

*`css_computed_font_family(style, NULL)`* - the generated getter stores the
name list before it returns the keyword, so NULL is a write to address zero.
It was: `far 0x0` at `get_font_family`, on the first page rendered.

*`css_unit_len2device_px(style, ctx, length, unit)`* had its last two
arguments swapped. `css_fixed` and `css_unit` are both integers, so nothing
warned. It converted a length of `CSS_UNIT_PX` - which is zero - using a
unit of 45056, and answered zero; every size then fell through a range check
to the inherited one. Colour and weight arrived from the cascade and *only*
size did not, which is what made it findable: a probe reporting kind 10, the
property set, and 0 pixels.

**A disk, by default.** `run-kosmos.sh` makes a 64 MB one beside the image
if there is not one already and attaches it - which `make` has always done
for a build tree and a released binary never had. Without it there is no
filesystem at all: Doom cannot find a WAD, nothing persists, and Tracker has
nowhere to look. It arrives unformatted and says so, because zeroing a file
is something any machine can do and writing a filesystem into one is `mkfs`,
once. Checked end to end: no filesystem, format, write, power cycle, still
there.

**And there is a page**, at `docs/index.html`, which is what GitHub Pages
serves - the setting offers the repository root or `/docs` and nothing else,
so it lives beside the design documents rather than in a folder of its own.

**One kernel check failed once and has not since.** `make test` reported 1
of 127 and three runs after it were clean. The evidence is gone: the run
that failed went through a `grep` that kept only the summary line, which is
the second time in this session I have thrown away the output of the thing I
was trying to diagnose. Recorded as intermittent rather than fixed, because
nothing here fixed it.

---


**Anyone can run this now, with one command and no toolchain:**

```
sh -c "$(curl -fsSL https://raw.githubusercontent.com/dcibils-neuratek/kosmos/main/get-and-run-kosmos.sh)" -- -b "wm"
```

It asks GitHub what is published, works out the newest, fetches it and
`run-kosmos.sh`, and boots. `builds/kosmos-0.8.34-8c8c1f5-1920x1080-full.elf`
is what it gets: browser, Doom, network, every demo.

**Five bugs came out of one person trying to use it**, and not one of them
would have been found by any suite here. They were all in the layer nothing
tests: the part a person types.

  * `./run-kosmos.sh` found no released image at all. It looked for
    `kosmos.elf` and `build/kosmos.elf` - the names a *build tree* has - and
    every file anybody downloads is called `kosmos-0.8.34-<sha>-...-full.elf`.
    `builds/README.md` had been documenting "the largest build here" the
    whole time.
  * It did not look in `build/`, which is where somebody who downloads one
    image actually put it.
  * `-b "wm blocks"` booted `wm` and dropped `blocks` - the script's own
    documented example. The argument was built as a string and expanded
    unquoted, so the shell split it. Every single-word `-b` worked, which is
    why it lasted. A POSIX shell holds a list with `set --`.
  * The downloader matched `*-web.elf` and the release had started calling
    itself `-full`. Worse, it *exited* when its preference was missing
    rather than taking the best of what was there.
  * And it fetched the image and the runner every time and never looked at
    **itself**, so the fix for the one above could not reach the person
    hitting it. The one-line form above is the arrangement where that cannot
    happen, and a kept copy now says when it differs from the published one.

`sh -c "$(curl ...)"` rather than `curl | sh`, and that is not style: a pipe
*is* the script's stdin, and the last thing it does is hand over to QEMU
with `-serial mon:stdio`. Checked rather than assumed - a script whose only
statement is `read x` gets an empty line through a pipe and the real one
through `-c`.

**The lesson is about where the tests are.** Everything in this system is
gated by `make test`, `make screenshot`, `make web`, `make browser` - and
`run-kosmos.sh` is what a person types, and had never been typed by anything
but a person. Three separate bugs in one file, all of them years-obvious in
hindsight, none of them reachable from a suite that starts by building.

---


**`make qemu` is the whole system now**: the browser, Doom, the network,
every demo, at 1920x1080. `FULL=0` gives the lean image. The variants exist
so a *suite* can be small and quick, not so the machine you sit in front of
is - and `make test` and `make bench` still build their own and are
untouched.

Two things had to be fixed before that was even possible, and both were
silent.

**`VARIANT` was a chain of else-ifs**, so `DOOM=1 WEB=1` called itself
`-doom` and put a build with the browser in it into the same directory as
one without. Two different binaries under one name, which `make` settles by
timestamp. It composes now - `build-user-doom-web` - and it has to be
computed *after* the block that sets DOOM and WEB, because `:=` expands
where it stands: setting them below it made them invisible to it and the
full build went into the *lean* build's directory, which is the same
collision reached from the other end.

**`/bin` reported 74 of its 82 programs.** A listing reply holds
`BIN_CHUNK / BIN_NAME_MAX` = 74 names, and `BIN_OP_LIST` started at nought
every time and stopped when the reply was full. The eight it dropped were
the last alphabetically, and four of them were applications - so the
Deskbar, which builds its menu from that list, could not offer Tracker, the
Terminal, the top bar or the web server. Nothing failed. The menu was just
short, and had been since the seventy-fifth program was added.

The guard was `_Static_assert(BIN_CHUNK / BIN_NAME_MAX >= 64, "a list must
hold every program in the image")` - which it cannot check, because the
number of programs is not something a static assert can see. It watched the
chunk shrinking while the image grew past it. A listing pages now, the way
`read` always has, and `ns.list` had looped on `more` since the disk grew
directories too big for one message; `/bin` simply never set it.

**The check that replaces it counts on both sides.** `run_headless.py` reads
`user/bin/*.lua` on this computer and `ls /bin` on that one, and fails if
they disagree. Two independent counts of the same thing, which is what makes
it a check rather than a restatement of the code.

**And the Deskbar has a System section**, which is what the missing web
server was found by looking for. Applications had nineteen entries and held
the network configuration, the process list, the log and the web server
among the calculator and the paint program. Those are not preferences
either - a preference is a choice that stays chosen, and these are windows
onto services running right now. Four sections: Applications, System,
Preferences, Demos.

---


**The browser needs nothing running anywhere.** `wm browser` opens on a page
compiled into `browser.lua`, parsed by hubbub, walked through libdom and
painted by `web_paint.c` - the whole engine, on a document that came from
nowhere. `builds/kosmos-0.8.33-f82c43e-1280x800-web.elf` was checked that
way: no network device in the guest and nothing on the host.

It replaced an image published an hour earlier, which could do neither of
the things below and so gave nobody a choice worth having.

**Three bugs, and the first two were only visible from outside.**

*Looking at a new build meant starting a web server on the host.* Which is a
thing an operating system has no business asking of the computer running it,
and it took being told so to see it - the harness serves pages from this Mac
because that makes tests offline and deterministic, and that arrangement got
carried into the instructions as though it were a requirement. Every browser
ever written ships a start page for this reason. A Lua string rather than a
file, because a released image has no disk under it: `run-kosmos.sh` passes
no drive, so `/home` is an empty ramfs at boot.

*`Host` was the literal string `kosmos`.* HTTP/1.0 made the header optional
and the web stopped being like that twenty years ago: one address serves
hundreds of sites and this is how a server knows which was wanted. So every
real host answered the wrong thing, and the only server that ever looked
right was one serving a single site out of a directory - exactly what it had
been tested against. `fetch.lua` had it right from the start.

*Redirects were not followed.* Pointing it at CERN's 188.184.67.127 fetched
twenty-one bytes and painted an empty page, because that address is a 301 to
a path. The connection was fine; it stopped at the first thing the internet
said. Followed now, up to five.

**And an address beginning with a slash is read from the namespace** rather
than fetched, which is what lets the browser open a document on the machine
it is running on.

`make browser` has a fifth check: Home must render with the server asked for
*nothing*. It is the one most easily lost, because everything else on the
page works whether or not it holds.

What still stands between this and the web is two things and neither is
small: **no resolver**, so a remote address is four numbers, and **no TLS**,
so https is out. QEMU's NAT does give the guest real outbound internet - a
plain-HTTP host that serves by address works today.

---


**There is a binary with the browser in it.**
`builds/kosmos-0.8.32-a59fcc6-1280x800-web.elf`, gated the way `CLAUDE.md`
asks: `make stress` for 60 rounds first, and then booted *as a released
file* - not as the build tree's image - to check it reaches a prompt, opens
the browser, fetches a page from this computer and paints it.

Two things had to exist before it could be one:

  * **`make WEB=1 release`**, and a `-web` in the name. On an ordinary image
    the browser opens and says the build has no web kit, which reads like a
    broken browser rather than the wrong file. One size rather than three,
    because the five vendored libraries take an image from 1.7 MB to 5.3 MB
    and stripping saves a quarter of a megabyte - the bulk is the userland
    compiled in, not symbols.
  * **A network in `run-kosmos.sh`**, which it did not have. `ping`, `fetch`
    and the browser all ran and found nothing, which looks like a broken
    stack rather than a missing flag - the same trap
    `virtio-mmio.force-legacy=false` is documented for, one layer up.

**And the 3D demos are not CPU-bound**, which the gallery screenshot says
without being asked: the cube holds 50 fps with the processor at 22%. Each
demo's frame is a render, then a *synchronous* `commit` that waits up to one
compositor pass, then `poll` with `wait = 1` - a deliberate sleep of one
scheduler tick, there because a loop that never blocks is a thread that is
always runnable and an idle desktop used to read ninety per cent.

A tick is **four** milliseconds, not the ten that three comments claimed:
`TICK_HZ` is 250 and was 100 when they were written, and `wm.lua` had
already stopped assuming and asks. At sixty frames a second that sleep is a
quarter of a frame rather than most of one, which is a different trade than
the comments described.

**Triple buffering is not the small local change it looked like**, and the
syscall table is what says so. There is `SYS_CALL`, `SYS_RECEIVE` and
`SYS_REPLY` and *no asynchronous send*: every message to a server blocks for
its reply by construction. A third buffer changes which surface is drawn
into and removes none of the wait. Doing it properly means a new syscall, a
bounded queue in the endpoint struct - fixed-size, since the kernel has no
allocator - a decision about what a full queue does (for a compositor,
dropping a stale commit in favour of the newest is right), and backpressure
so an application cannot get more than two frames ahead. That is a change to
the IPC model and wants deciding rather than slipping in.

It would also buy less than it sounds: **window dragging is entirely inside
`wm`**, so no application waits during one and triple buffering cannot touch
it. What it does help is an application that draws its own pixels - the
browser, `pdfview`, `cube3d`, Doom - and there the cheaper alternative is a
compositor that composites in slices and drains IPC between them, which
needs no kernel change at all.

---


**The browser was measured, and half of every frame is waiting.**

Two runs of the same code: under TCG, which `CLAUDE.md` says is for
detecting a regression rather than for claiming a speed, and under `hvf`,
where the guest runs on this Mac's own cores and only the devices are
emulated. Neither is a Pi 5 number. What they are good for is
*attribution*, which was the question.

Loading a 4.3 KB page:

```
            TCG      hvf
fetch      106.6      2.9 ms
parse       11.7      1.7
layout      17.6      1.8
paint        3.5      0.6
```

A scroll frame:

```
            TCG      hvf
page blit   10.7      2.8 ms
commit      16.3      2.8
total       28.2      5.7
worst       39.5     10.8-15.6
allocated              1.02 KB
```

**Nothing is doing wasteful work.** Layout is 1.8 ms and happens once. Paint
is 0.6 ms and happens once. The per-frame blit is 884x584 pixels and 2.8 ms
is about the floor for a software compositor at that size.

**What the split found is that the other half of a frame is not computing.**
`commit` is a synchronous message whose handler swaps a buffer index and
records a damage rectangle - and it costs as much as the blit. The worst
frame moved between 10.8 and 15.6 ms across two runs of identical code,
which is the tell: it is not a cost, it is *scheduling*. A commit that lands
while the compositor is mid-pass waits out the rest of that pass.

**And it is not the collector**, which was the first suspicion and the one
this project has been bitten by before. 1.02 KB a frame, against the 4.2 KB
the desktop was making before the console moved to a struct and the 0.6 KB
it makes now. The browser is already in the good range.

Two ways at the worst case, and neither has been taken:

  * **Triple buffering.** The application always has a buffer to draw into
    and never waits for the reply that says which. Fifty per cent more
    surface memory, and local to `ui.lua`.
  * **A shorter compositor pass**, which is what `make frames` measures and
    what already paid for itself once on the desktop.

The instrumentation stays in `browser.lua`: the status line reports fetch,
parse, layout and paint after a load, and frame, blit, commit, worst and
kilobytes while scrolling. `pdfview` reports its render time for the same
reason - this is the only place in the system that can say which of four
entirely different kinds of work a slow page is.

---


**Links work.** Click one and the browser goes there. That is six things at
once - the layout kept its boxes, the click became a point on the page,
`link_at` found the run under it, the relative address resolved against the
one the browser was on, the fetch happened, and the result was laid out -
and the only way to know all six work is to arrive at the other page.

**One missing data structure was six missing features.** `web_paint.c` used
to walk the tree and draw as it went, which is the shortest way to get ink
on a page and throws away the one thing everything else needs: where each
word ended up. Nothing could be clicked, because nothing remembered which
element a word came from. Nothing could be bold inside a paragraph, because
a block was flattened to one string before it was measured. `pre` could not
keep its spaces, because there was nowhere to record that this block was
different.

So layout runs first and produces an array of **runs** - a positioned slice
of text with a face, an ink, and a link if it sits inside an `<a>`. Paint is
one loop over that array and knows nothing about the DOM. Hit testing is the
same loop with a comparison instead of a draw. `len == 0` is a rectangle - a
heading's rule, a list marker, a link's underline - so painting stays one
pass in the order things were laid out.

What arrived with it, all of it falling out of the same change: `<strong>`,
`<em>` and `<code>` inline, on a **shared baseline** rather than a shared
top edge; `pre` keeping its spaces and its line breaks; a marker on every
list item; and links drawn blue and underlined across the whole span rather
than word by word.

**Two things the picture showed that reading would not have.** An underline
per *word* rather than per link makes one link read as several. And a space
next to a `<code>` was measured in the monospace face - a visible gap in
front of every inline code span. Measuring it in the *previous* word's face
instead moved the same gap to the other side of the word, which is how the
right rule turned up: the space belongs to the text node it was written in,
so `<code>h1</code> is set` has a paragraph-width space even though a
monospace word is on one side of it. Neither of the two words either side
decides it.

**`gfx_draw_ascent` is back, with a caller this time.** It was removed a few
hours earlier for having none, which was right then: `gfx` adds the ascent
inside its drawing routine because it is the only place that knows it. Two
faces on one line is the case that needs it outside - they share a baseline,
`gfx_draw_text` takes a top, and the difference is per face.

**A block that contains a block is no longer drawn twice.** `layout_blocks`
recurses first and lays an element out as a block only when nothing below it
was, so a `<p>` inside a `<blockquote>` appears once. The cost is a `<li>`
holding text *and* a nested list: the inner items are laid out and the outer
item's own words are not. Real layout puts those in an anonymous block,
which is machinery this does not have. Losing them is the smaller wrong and
the rarer one.

**`make browser` has five checks now**: ink on the page, more than one text
height, six presses of Down moving it, Reload asking again when clicked, and
a link found *by its colour* and followed to the other page. It writes both
pictures - the page it started on and the page it arrived at.

**Still not CSS layout.** libcss parses, selects and answers, and nothing
asks it: colour, size, weight and slant all come from the tag's name. No box
model, no margins, no floats, no `width`, no images, no forms. Every inline
face is at the body size, so `<em>` inside an `<h1>` comes out small - which
is the same missing piece as the rest, because a face should be chosen by
(family, weight, slant, size) from the computed style.

---

**Layout's two blockers are gone.** Neither was layout.

*Faces.* `gfx` held one face per role and there were four roles, so a
heading could not be larger than a paragraph on the same screen. Faces are a
pool now, addressed by name and size through `gfx.face("ibmplexsans", 28)`,
and the first four entries are still the roles - so `measure`, `height` and
drawing were already taking an index and needed no new argument. The same
name and size hands back the same face rather than rasterising 95 glyphs
again, which matters when a page asks for the paragraph face on every block.

*Weights.* Every file in `assets/fonts/` was `-Regular`, so `<strong>` and
`<em>` could only ever have rendered as nothing. Four Plex weights joined
them - Sans Bold, Italic, BoldItalic and Mono Bold, 794 KB, OFL, and the
licence files already in the tree cover them because `assets2c.py` matches
on the name before the first `-`.

**Dropping those four files in broke font resolution, silently.**
`font_short_name` took everything before the first `-`, which was exactly
right while every file ended `-Regular.ttf` and became its family name. With
a second weight present `IBMPlexSans-Bold.ttf` and `-Regular.ttf` both
became `ibmplexsans` - and `FONT_FILES` is sorted, so *Bold sorts before
Regular*. The desktop's monospace font would have quietly become bold with
no error anywhere.

The name keeps the whole stem now and drops only `-Regular`, so every
existing name survives byte for byte; and `font_asset` tries exact matches
before prefixes, because `ibmplexsans` is a prefix of `ibmplexsans-bold` and
the shorthand would have picked the wrong weight.

**And `gfx` now lends its drawing to another kit** - `gfx_draw.h`: fill,
text, measure, height and *ascent*. The surface stays an opaque pointer, so
unlike `docfont.c` - which repeats `struct surface`'s first four fields and
says in a comment that two files must now agree - there is nothing to agree
about.

Two things decided inside it. `gfx_draw_text` takes an outline face only and
draws nothing without one, because silently substituting a different size
would make every line of a page the wrong height. And ascent is exposed
*separately from height*, because two faces on one line share a baseline
rather than a top edge - which is the case on every paragraph containing
`<strong>`.

62 display checks, 127/127 and 11 web checks green.

---

**The browser shows a document's structure, not a wall of text.** The kit
returns the blocks in order - `blocks()` walks the tree and emits the tags
that carry a paragraph's worth of text - and the application spaces a
heading from what follows it, bullets a list item, indents a quotation.
There are no boxes and nothing is measured twice, so it is not layout; it is
most of what makes a page readable without one.

Deliberately the *leaves*: a `div` holding three paragraphs would otherwise
emit the whole page and then each paragraph again. And the walk is bounded
at 64 deep, because a browser is handed documents written to break it and
this process has a fixed stack with a guard page under it.

**The constraint that reorders what comes next**, found while wanting a
heading to be bigger than a paragraph: `gfx` holds **one face per role, and
there are four roles** - `ui`, `title`, `text`, `mono`. A page needs a dozen
combinations of family, size and weight at once. So "headings are larger" is
not something a layout engine can express here; it is a `gfx` change first.

The glyph cache added today is already per-face, so the shape is right. What
is missing is holding more than four and addressing them by *(family, size,
weight)* rather than by role.

So the order is now:

  1. a font cache keyed by family, size and weight, replacing the four
     fixed roles. Self-contained, and everything visual waits on it;
  2. the plotter over `gfx`, proved against a hand-built box tree - so
     painting is verifiable *before* layout exists;
  3. block layout, then inline layout and line breaking.

Two and three were already agreed. One is new, and it blocks them.

---

**The text path decodes UTF-8.** It cast each byte to a codepoint, so a
three-byte character became three lookups and three boxes - which is what
the conformance benchmark showed, and it was never a missing font.

Both paths were wrong and both are fixed. The outline faces decode and now
rasterise a glyph on demand into a 128-slot codepoint-keyed cache, with the
eager ASCII array untouched so the common path costs what it always did.
The built-in 8x16 bitmap decodes too, and its width function counted *bytes*
- which made one accented character two columns wide and every wrapped line
short.

Measured rather than asserted:

    default (spleen)   "e" 8   "é" 8      was 8 and 16
    arimo 15px         "iiii" 12  "MMMM" 44   proportional, and "ß" one advance

62 display checks, 127/127 and every suite green.

**What is still not right, and it is not the parser.** A page with accented
Latin still draws boxes, because `theme.lua` sets all four font roles -
`ui`, `title`, `text`, `mono` - to `spleen`, an 8x16 bitmap face with ASCII
in it and nothing else. Switching the desktop to a proportional outline face
is an *appearance* decision and `appearance.lua` already offers it.

**Two things found on the way that are worth fixing and are not fixed:**

`gc:text` clipped with `s:sub(skip + 1, skip + room)` - a *byte* slice,
where `skip` and `room` are counted in columns. The two agreed only while
everything on screen was ASCII; on anything else it cut the line short and
could cut through the middle of a UTF-8 sequence. It slices by character now,
through `utf8.offset`, guarded with a fallback to the old behaviour because
`utf8.offset` raises on a continuation byte and a browser is handed
malformed input on purpose - the conformance benchmark contains it
deliberately.

**The mojibake recorded here was not a bug, and the correction is the
interesting part.** With a proportional face a page came back as `cafÃ©`
for `café`, and this file said something between `gc:text` and the blitter
was still handling bytes. It is not. The test page had no `<meta charset>`,
so the parser defaulted to a single-byte encoding, decoded each UTF-8 byte
as its own character, and produced a genuinely double-encoded document. The
renderer drew exactly what it was given.

Adding the charset settles it: `café` becomes `caf` and one box, `Straße`
becomes `Stra` and one box - **one box per character**, where before there
were two glyphs. The decoding is right end to end, and the remaining boxes
are only `spleen` having no glyph for those codepoints.

Worth keeping because the false finding was written down with confidence and
survived a commit. The benchmark file has a charset and never showed it;
the page I wrote to test with did not.

**A mistake to record rather than bury.** Demonstrating the font switch
meant writing `/home/.appearance`, and it was written with a `fonts` key
alone - so the `palette`, `desktop` and `wallpaper` in it were destroyed. It
has been removed so the system boots to documented defaults, and the
appearance will need setting again. Read-modify-write, not write.

---

**There is a browser window, and it shows a page fetched off the network.**
`wm browser:10.0.2.2:8000/` opens a connection, sends a GET, parses what
comes back with hubbub, and displays the document's text with the counts
underneath. Every piece of that was built separately; this is the first
thing that runs them together.

**It is not a rendered page and the window says so**, in the status line
rather than only in a comment: *text only, there is no layout engine yet*.
The chrome is NetSurf's shape - back, reload, an address, a status line -
and it is here early *because* the engine is unfinished. A window with
somewhere for each piece to appear is what makes the next piece visible when
it lands.

Two defects the first screenshot showed, both mine:

  - the title bar said "Browser" while the page was called something else.
    `win.title = ...` sets a local copy; the bar belongs to the desktop, and
    `win:retitle()` is what reaches it;
  - the byte count was the whole HTTP response while the window showed only
    the body, which is a number that quietly does not match the screen.

The kit is only in a `WEB=1` image, so the application degrades rather than
raising: on an ordinary build it opens and says which build it is on. An app
that raised there would be a broken entry in every Deskbar.

**And `kernel/` has no architecture-specific instruction left in it.** The
sixteen sites are seven inlines in `arch/aarch64/cpu.h`; what remains in
`kernel/` are two *comments* mentioning TTBR0. That is what makes "the
kernel is portable" checkable rather than aspirational - a second `arch/`
implements that header and `kernel/` does not change.

**The two maskings stayed two.** `DAIFSet` takes a bitmask, so `#3` is IRQ
*and* FIQ where `#2` is IRQ alone: the scheduler wants everything off while
it moves threads between queues, and the idle loop wants only IRQ off around
a `wfi`. One function for both would have been a silent change to when a
fast interrupt may arrive - the kind that appears once in a thousand boots
on real hardware and never under QEMU.

**Next is selection**: a `css_select_handler`, about thirty callbacks
answering libcss's questions about a libdom node, which turns the DOM and
the stylesheets into a computed style per element. Then a plotter over
`gfx` - proved against a hand-built box tree, so painting is verifiable
before layout exists - then block layout, then inline layout and line
breaking.

**Layout will be ours rather than NetSurf's**, and the reason is where a
simplification still leaves something recognisable. HTML5's error recovery
and CSS's cascade are hopeless to hand-write, which is exactly why the
vendored libraries earn their place; layout degrades gracefully. Taking
NetSurf's would mean taking its core - content, fetch, box construction -
and that is a large dependency graph for the part we can most afford to do
simply. What it costs, and it is real: no floats, no tables, no absolute
positioning, so real pages will look wrong in real ways.

**Fonts are not the ceiling, which is worth recording because it was
assumed.** `assets/fonts/` ships four outline faces and `docfont.c` already
rasterises glyphs with a coverage cache sized for a page of two thousand -
built for the PDF reader. Headings can be larger than body text and text can
be proportional. Pages can look like pages.

---

**A web page parses on Kosmos.**

    kosmos> local w = sys.kit("web")
    kosmos> local d = w.parse("<html><head><title>Hello Kosmos</title>...")
    Hello Kosmos    3    1    true

Hubbub parsed the HTML, libdom built the tree, libwapcaplet interned the
names, and libcss understood a stylesheet - all at EL0, on the freestanding
libc, through the new allocator. `use("/kits/web")` reaches it, the same way
`/kits/pdf` and `/kits/gl` are reached.

**The `3` is the part that means something.** Three `p` elements, one of them
nested inside the `div`, so `getElementsByTagName` walked a tree rather than
counting tokens. Linking proved none of that; a freestanding libc can satisfy
every symbol and still return null on the first allocation.

`make web` is the permanent check - seven of them, and each is chosen to fail
for a distinct reason:

  - the title, that a document parsed and text came back out of the tree;
  - three `p` and one `div`, that the walk is real;
  - `&amp;` becoming `&`, that the **entity table a perl script generates
    during the build** is present and consulted. A broken generation step
    still links and still parses, so nothing else would notice;
  - unclosed tags still producing a `div`, because recovering from those is
    what an HTML5 parser is *for*;
  - a stylesheet of eight properties, that the **119 property parsers
    `gen_parser` emits** are there. Missing them is a link error, but a
    *wrong* one would look like a sheet that parses and understands nothing.

Its own target rather than part of `make test`, because `WEB=1` is an
optional variant like `DOOM=1` and the ordinary image carries none of it.

**The one failure on the way was the test, not the library.** The entity
check expected `a & b` from an input I had written as `&amp;amp;` - doubly
escaped for no reason - so hubbub correctly decoded one level and returned
`a &amp; b`. Worth recording because the failure text accused a generated
file of being empty, and it was the assertion that was wrong.

**Still not a browser.** What runs is parsing. *Selection* - matching a
selector against a document - needs a `css_select_handler`, about thirty
callbacks bridging libdom's tree to libcss's questions, and that is the real
integration between the two rather than something to smuggle into the step
that proves the parsers work. Layout is after that, and is the bulk.

---

**`make WEB=1` builds the NetSurf parsing stack from a clean checkout.** The
two perl generators, the gperf run and the 119 CSS property parsers are build
steps now rather than shell history, and they write into `build/gen/` rather
than beside the source. The image goes from about 3.66 MB to 4.60 MB.

**No heap flag**, which is what the allocator work bought: `DOOM=1` carries
`-DUSER_HEAP_PAGES=3072` because a fixed 2 MB heap could not hold Doom's
zone, and a growing heap makes that whole class of compile-time workaround
unnecessary.

**Two of the four problems on the way were already in the tree.**

`make` 3.81 picks the *generic* `runtime/upstream/%.c.o` over a more specific
rule written below it. TinyGL's rule has always been above it; Doom's was
not - so `make DOOM=1` was compiling all eighty of id's files with **zero**
`DOOM_CFLAGS`: no `-DNORMALUNIX`, no screen size. Moved above, and it gets
them now.

And `FLAGS_NOW` was `$(CFLAGS) | $(UCFLAGS)`, so changing `DOOM_CFLAGS`,
`TINYGL_CFLAGS` or `WEB_CFLAGS` rebuilt nothing at all. Found by adding a
flag and watching the identical link error come back twice.

**The other two were mine.** All five libraries' private `src` trees on one
include path is a collision, not a convenience: four of them ship their own
`utils/utils.h` and two ship `utils/parserutilserror.h`, so libcss silently
got hubbub's headers and failed on `css_error_from_parserutils_error` while
gcc suggested the hubbub spelling. Each object gets only its own library's
`src` now, derived from the path it is built from; the public `include` trees
are namespaced by library name and stay shared.

And `-fcommon`, on the vendored files only. `libcss/src/stylesheet.h` ends a
struct with `} _ALIGNED;`, where `_ALIGNED` is defined nowhere in libcss and
appears in no other file - so it parses as a file-scope *variable* declared
in every translation unit that includes it. Upstream links because their
build uses the pre-GCC-10 default; `-fno-common` is mandatory here, so it
became 180 definitions of one object. Upstream's latent bug, not Kosmos's,
and the rule about not modifying a vendored tree is what decides the fix.

**One unexplained test failure, and it is recorded rather than explained.**
A `make test` run reported 1 of 127 failing, on the *default* build, which
links no NetSurf code. Four consecutive runs since - one isolated and three
repeats - are 127/127, so it is not deterministic and not the full rebuild
surfacing something. **Which test it was is unknown**: the failing run's
output went through a grep that discarded it. This tree has a known
intermittent in that suite (`thread: three threads interleave`, chased
before and confirmed not a regression) and that is the likely answer, but it
is a guess and is written down as one.

---

**The NetSurf parsing stack compiles for Kosmos, and nothing was patched.**
Five libraries vendored - libwapcaplet, libparserutils, libhubbub, libcss,
libdom, all MIT and checked against the COPYING each tarball ships rather
than against the website. 456 objects. Every symbol resolves.

The whole libc bill for a hundred and forty thousand lines of HTML parser,
CSS engine and DOM came to **three functions**: `bsearch`, `strtoul` and
`time`. Plus one switch that is their own (`-DWITHOUT_ICONV_FILTER`) and
four generated files - three from perl and gperf, and 119 CSS property
parsers from libcss's own `gen_parser`, which has a `main()` and must be
built with the *host* compiler. Skipping it looks like 117 undefined
`css__parse_*` symbols and looks nothing like the truth.

That was the question Tier 0 existed to answer - whether this libc holds up
under serious third-party C - and it holds up better than predicted.

**`qsort` was not one of the gaps, and that is worth knowing.** It exists,
in `misc_user.c`, and it is insertion sort with a comment saying so and
predicting this exact moment: "if something ever sorts a large array through
this, the profile will say so and the answer will be to write the better
algorithm then." There is no profile yet, so it stays. That is the note to
come back to when the browser is slow.

**And `time` found a bug that has nothing to do with NetSurf.**
`kosmos_lua.h` redirects `time()` to the monotonic counter so `lmathlib` can
seed itself, and its comment claimed the redirection "applies only to Lua's
translation units". It does not: the `-include` is on the catch-all rule, so
it is in front of every user file in the system. Anything outside Lua asking
what time it was got a tick count, silently - the "plausible wrong number"
that same comment calls worse than stopping.

Scoping the include to `lua/` was tried and fails: `user/init/main.c` embeds
the interpreter and needs those hooks ahead of Lua's own headers, and files
like it are not under `lua/`. So the header now says what is true, the real
`time()` `#undef`s the macro before defining itself, and code that wants a
wall clock and is not Lua must be compiled without that header - which the
NetSurf rules will do anyway, because they are not Lua.

**Nothing has run.** Compiling and linking are not evidence. The end of this
is a document parsed and a selector matched *on the machine*, and the
generated files still live in a scratch directory rather than in `build/gen/`
with rules that reproduce them from a clean checkout. Both are next.

**The shape agreed for the browser**, which is the shape the tree already
uses twice: the engine is C behind `use("/kits/web")`, and the application
is Lua - window, chrome, history, scroll offset, navigation policy. A click
goes into the kit, which hit-tests and answers "that is a link to this", and
Lua decides whether to follow it. `pdftok.c` with `pdfpage.lua`, and TinyGL
with `cube3d`, are the same arrangement. This replaces the earlier plan of
adapting NetSurf's framebuffer frontend through libnsfb: the plotter table
goes straight onto `gfx`'s surface primitives instead, which is less code
and does not drag in a frontend written for another system.

---

**The allocator was the slow thing, and it had said so in a comment since it
was written.** `malloc` was a first-fit scan over one list holding every
block, free *or allocated*, so an allocation cost what the heap already
contained - fine empty, quadratic in aggregate full. Free blocks are binned
by size class now, with the bin links living inside each free block's own
payload, which costs nothing because a free block's payload is unused by
definition and the minimum payload is exactly two pointers. The physical
address-ordered list stays, because that is what lets `free` find its
neighbours and merge both ways, which is what Lua's collector depends on.

    alloc_table   28138.160 -> 355.731     79x
    serialize      1506.201 -> 897.665     -40.4%, and untouched

`serialize` falling forty per cent is the part worth keeping. That benchmark
has moved four times without anybody editing `serialize.c`, and its note has
always said to look at what was added to the process. This time the answer
was that something was taken away.

**And the heap grows.** It was 2 MB fixed at compile time, which is why `make
DOOM=1` exists at all - Doom wanted five megabytes and the only way to give
it one was to rebuild the system with a different `-D`. `malloc` asks the
kernel for another arena when no bin can serve, up to the 48 MB a process may
map. Six megabytes of live strings now allocate in a process that could hold
two. The `DOOM=1` heap flag is redundant and still there, because proving it
needs a Doom run.

**The one trap, written into the code rather than left to be remembered:**
`sys_map` bumps a cursor and never reuses an address, so arenas are *not*
adjacent. Two blocks in different arenas can be neighbours in a bin and are
never neighbours in memory. Each arena being its own physical list with NULL
at both ends is what makes coalescing safe.

**Getting there turned up two things that were not the allocator.**

`make bench` had not built since `727b81f` gave `ipc_receive` a timeout and
left three call sites behind. That is very likely why the M4 benchmark the
allocator kept pointing at was never written: the suite could not run to
receive it.

And two numbers drifted while nobody could look - `context_switch` +3.0%,
`ipc_roundtrip` +3.3%. Neither touches Lua or the allocator. The guess is the
timed receive itself, since `thread_wake_sleepers` now scans every thread on
each tick, and it is a guess. **Their baselines were deliberately not
raised**, so `make bench` still fails on them and they stay visible;
`alloc_table` and `serialize` were recorded because they moved on purpose.

**And `malloc.c` is not userland-only, which I had assumed.** The kernel's
*test* image links it, with kernel flags and no `-Iuser/include`. The fix was
not an include path: at EL1 there is nothing to ask, because `kosmos_map` is
a syscall and the kernel does not make syscalls to itself. Growth is behind
`#ifdef KOSMOS_USER` and the heap there stays what it was handed.

---

**`httpd` serves eight at once.** Eight simultaneous requests for a 106 KB
file all come back complete in half a second of wall clock - about what one
of them costs on its own - with a ninth client deliberately stuck half way
through its request and going nowhere.

There are no threads to do that with. There is no thread syscall at all:
`SYS_SPAWN` makes a *process*, and a process has one `lua_State`, so two
threads inside one would want a lock around the interpreter and would take
turns anyway. What it has instead is nginx's shape - **one process, an event
loop, and a coroutine per connection** - and the coroutine is why `serve`
still reads as a straight line: read the request, find the file, write the
answer, with a `yield` wherever it used to wait.

**That needed the `select` this system had wanted six separate times** and
had worked around with a timer every time. `fs.poll` is it, and the shape it
arrived in is the interesting part: **two masks, not one.** A caller waiting
to read wants to hear that bytes arrived; a caller waiting to write wants to
hear that room appeared. One mask has to guess, and the version that guessed
deadlocked - it reported a connection writable only when there was room
*and* something still queued, so a client that acknowledged the whole ring
in one go left the server waiting for news that could no longer arrive.
Eight requests hung and the server logged none of them.

**Two bugs underneath it, and both had been there a while:**

- **The console's `interrupted` span the console server for ever.** Its
  drain loop read through `next_byte`, which empties the *stash* before it
  asks the hardware - so it took a byte off the stash, put it back, and took
  it again. One character typed ahead and the console never answered anybody
  again, which looks exactly like whichever program had asked having hung.
  Nothing found it for months because every caller was shaped so it could not
  happen: a status bar asks between screens with the keyboard drained, and
  the old `httpd` asked once per request, *after* `accept` had blocked. An
  event loop asking ten times a second hit it on the first pass.
- **`respond` allocated the file again on every pass.** It built `head ..
  body` and then wrote `text:sub(at)` - the whole remainder - each time
  round, so a 106 KB file allocated 106 KB, then 90, then 74, per connection.
  Six at once ran the heap out and two conversations died with "not enough
  memory" while four were served. It writes a ring's worth at a time now and
  streams the body with `fs.chunks`, so the file is never held.

**And `fs.write` no longer raises on the disk.** Chasing the concurrency
bug turned up a probe of mine that wrote 150 KB and read back 16 KB, which I
put down to `/ramfs`'s ceiling - and `/ramfs` was innocent: it returns `false,
"/ramfs is full"` and always did, and I had not looked at the return value.
The disk was not. `fs.write` above about two kilobytes reached `sys.call`
and came back as `value does not fit in a message` - an *exception* out of
the serialiser, from a call whose failures are otherwise values.

The namespace splits a long write for `/ramfs` and cannot for the disk, whose
`write` takes no offset and hands the whole body to `kfs.store`. So the same
line worked on one mount, failed with a sentence on another, and threw on a
third - which is exactly the difference a namespace exists to hide. It sends
a big string through a region now, which is the route `write_from` and
`files.copy` already took and that diskfs implemented for this reason. Three
checks in `run_interchange.py`, one of them taking the file back out of the
image byte for byte.

**And that uncovered a capability leak that had been there all along.**
Sending a value through a region hands the server a capability, and diskfs
kept every one: thirty-two is what a thread gets, so the thirty-second large
write failed with `that is not a region this process can map` - which reads
like a bad pointer rather than like a table that is full. The read side had
paid that debt since a PDF read in 256-byte windows found it on its
fifteenth read; the write side never had, and nothing noticed because
`files.copy` was its only caller. `fs.write` sending large values that way
made it an ordinary path, and a loop reaches thirty-two in a second.

That is the third resource bug of this exact shape - capability slots gone on
the sixteenth read, a region per font per size, a process table full at round
twenty-two - which is why the check for it is a loop of forty writes rather
than one write, and why `make stress` exists.

Two ceilings are unchanged and both are deliberate: `/ramfs` still holds 16 KB
a file, and diskfs still refuses more than a megabyte because it assembles
the bytes in its own heap. A big *table* still fails, because a region
carries bytes and sending a packed table through one would break the promise
that you get back the table you wrote.

**And `accept` gained a deadline**, which makes every park in the stack
bounded - the pings, the waits, the pollers and now this. `poll` saying
somebody arrived and `accept` reaching the stack are two moments, and a
reset in between would otherwise wedge an event loop for good.

Two things worth knowing that came out of the hunt rather than out of the
code. The first two "bugs" I chased were not bugs: a stale `build/kosmos.elf`
and a `wait_for` that matched the *echo* of the command it had just typed.
And a server cannot print - `sys_write` is gated on `owns_console` - so
instrumenting one means opening that gate in the kernel for the length of the
session. Both are worth remembering before the next silent hang.

---

**Kosmos serves web pages.** `httpd 80 /home` and this Mac fetches a 152 KB
PNG from it, byte for byte identical to the file on the guest's disk.

That needed the half of TCP left out on purpose - LISTEN, SYN_RECEIVED, and
a way to hand a caller a connection it did not ask for - which `roadmap.md`
said an HTTP server would be the argument to settle. It was cheaper than the
note predicting it suggested.

**Three bugs on the way and none of them said anything was wrong**, which is
the reason to write them down:

- `send_ip`'s buffer was `NET_PAYLOAD_MAX + ICMP_HEADER`, 156 bytes, because
  ICMP was its only caller. TCP could send a handshake and nothing larger.
  The capture showed a FIN with `len=0` - a server that answered nothing.
- **A FIN is a wish, not an act.** It takes a sequence number, so sending it
  while the ring still held bytes numbered it as if they did not exist. The
  image arrived as 141 KB, the server logged a complete file, and the client
  saw a clean close.
- A client had no way to wait for *outgoing* space. `wait` only woke for
  incoming bytes, so a client that filled the ring parked until the
  connection died - the first truncation, at exactly 16 KB, which is the
  ring. An ACK that frees space wakes the waiter now, and `wait` honours the
  deadline it had been recording and ignoring.

**Two apps.** `network` shows the card and edits the addresses, applies them
live, saves to `/home/.network` for the next boot, and can ping the gateway
to say whether any of it worked. `webserver` starts and stops `httpd` and
shows its requests arriving.

The manager is not the server and that is forced: `accept` blocks and a
window that blocked would stop drawing. They talk through `/ramfs` - the
server writes its state and last forty lines, the manager reads them on a
tick - which is a file rather than a message because the server has no idea
anybody is watching.

**SSH's primitives are written and checked**: SHA-256, HMAC-SHA256,
ChaCha20, Poly1305 and X25519, each against the vectors in its own
specification. Two bugs came out of that which running would never have
shown - a nonsense shift in `fe_from_bytes`, and one apparent mismatch that
was *the test* reusing RFC 8439 2.3.2's nonce for 2.4.2. What is left is
Ed25519 verification, then the binary packet protocol, key exchange,
userauth and channels.

---

**Kosmos is on the internet.** `ping 8.8.8.8` answers, in about 22 ms
through QEMU's NAT.

Four pieces, each with its own test:

`hal/qemu-virt/virtio.c` is **the transport, once**. It was three copies -
`blk.c`, `input.c`, `snd.c` - of a handshake whose *order* is the protocol,
and `qemu-virt.h` recorded the reason not to share it: "splitting the
transport out before there are two would be inventing an interface against
a single caller". That expired two devices ago and the card is the fourth.
What is shared is the conversation; what is not is the ring, because block
chains three descriptors and waits, input hands the device empty buffers,
and sound has four queues with different jobs. A generic ring over those
would be the same mistake one layer up.

`hal/qemu-virt/net.c` is **virtio-net**: two queues running in opposite
directions, a twelve-byte header the wire never sees, and `NET_F_MAC`
asked for and checked rather than assumed. `run_network.py` reads QEMU's
own pcap, because nothing inside the guest can establish that a frame left
it - `sys.net_send` returning true is a statement about a virtqueue.

`user/servers/net.c` is **Ethernet, ARP, IPv4 and ICMP**, in C because it is
a server on the packet path. Fragments and IP options are refused rather
than half-handled. `SPAWN_NET` is the disk's grant pointed outwards - whoever
can send a raw frame can claim any address and read every frame that arrives
- so one process holds the card and `/net` is what everything else asks.
A card is a device; a stack is someone you ask.

`/kits/network` and `ping`. **No capability leaves the namespace**: a program
says `fs.ping("/net", ...)`, the namespace resolves and hands the kit the
capability, exactly as `fs.wait_input` does for the console. There is
deliberately no `fs.capability`.

**And TCP.** `fetch 1.1.1.1 80 /` pulls a page off Cloudflare;
`fetch 10.0.2.2 <port> /` pulls one off this Mac, which is what the test
drives because slirp maps the host at 10.0.2.2 and nothing leaves the
machine.

A connection's bytes never travel in a message: `tcpring.h` is two
single-producer rings in a region both sides hold, and that was written down
before a byte moved rather than after somebody found a message worked for
the first ten kilobytes.

Three trades, all deliberate. **One segment in flight**, retransmitted on a
doubling timer - a queue and a sliding window buy throughput on a long fat
link and nothing on a line protocol, and it is one timer to get right
instead of four. **Out-of-order segments are dropped**, which costs a
retransmission and saves a reassembly buffer with a policy about overlapping
pieces, the well-known way to get a stack wrong. **The receive window is the
ring's free space**, not a number the stack invented - a window is a promise,
and a client that stops reading really does slow the sender down.

No LISTEN: this end connects out, which is telnet and SSH. Accepting needs
the other half of the diagram and a way to hand a caller a connection it did
not ask for, which is where the HTTP server's argument belongs.

**Not good, and written down rather than left unexamined:** the local port is
a counter. A predictable source port is one an off-path attacker can guess,
and there is no randomness here to do better with.

`telnet` is written and connects; it is half-verified, because driving it
needs a keyboard. Its loop polls `fs.keys` because the console has one reader
and there is no way to wait on two things at once - **the third time that
missing `select` has come up**, after live queries and the stack's own loop.
It is the thing to build when something needs it a fourth time.

Three things that went wrong on the way, all the same shape: **a question
asked in the wrong place.** `net_mtu` landed inside the `hal_snd_present()`
branch, so a machine with a card and no speaker reported no network.
`sys.net()` raised for a process that was not granted the card, when from
inside a program "no card" and "not mine" are the same fact - it returns nil
now, like `sys.screen()`. And the shell has to *hold* the card to pass it on,
which `init.lua` has now got wrong four times; the no-card boot is a
permanent check rather than a comment.

---

**Queries were returning the wrong paths for the whole disk, and no test
looked.** One disk is mounted three times - `/system`, `/user`, `/home`,
each naming a subtree of itself - so the namespace mapped `/home/doc.pdf`
onto `/home/doc.pdf` in the server and then put the mount prefix back on the
way out: `/home/home/doc.pdf`. And the server answered a question asked
about `/home` with everything on the disk, `/system` included.

Both survived because **every query test used `/ramfs`**, the one mount with
no root, so the two paths through that code had never both been walked.
`qbench` measures how fast a query is and `latency.lua` how quickly a watch
wakes; neither would notice the answers being wrong. M7's definition of done
was a live query and `make test` had never checked one.

`tools/run_queries.py` now does, on both kinds of mount, and it checks that
a returned path can be *read back* - a doubled prefix is still a string and
still looks like an answer. Verified by putting the bug back and watching it
fail.

**Kosmos runs under `hvf`**, natively on this Mac's cores: `make fast`.
`gfxbench` reads 4x on a fill, 5x on a blit and **14x on a circle drawn in
Lua** - the interpreter is branch-heavy, which is what TCG is worst at, and
the interface is Lua. It needed a kernel fix. `mmio_write32` was a volatile
store and GCC chose a post-indexed addressing mode for it, which is correct
on hardware and cannot be emulated by any hypervisor: ARM sets ISV=0 in the
syndrome for a load or store with writeback, so nothing in the trap says
which register or what width. The MMIO accessors are one hand-written
instruction each now, which is what Linux's `__raw_writel` does and for the
same reason. `-cpu host` is required, so speed and Pi-5 fidelity are two
targets rather than one flag, and `make bench` stays on TCG because
`-icount` does not exist without it.

**Tracker has the chrome from a Finder window**: back and forward, a search
box top right, the count bottom right. A plain word filters what is on
screen as you type; `name:value` is a query, run on Enter, whose answer is a
folder of files from all over the volume. A query result is *refreshed
twice a second rather than pushed*, because the window is already blocked in
the desktop's poll and there is no way to wait on two things at once - what
is missing is a select, or a second thread.

There is a select now, and it does not help here: `fs.poll` waits on network
connections, and what this wants is to wait on the desktop's input and a file
watch together. The two waits live in different servers, so a select over
both would have to be something the *namespace* offers rather than something
`/net` does. Worth knowing before anyone reads the paragraph above and
assumes the problem went away.

Also: **the Places pane never worked.** Two `local show` declarations, and
the tree's `on_select` closed over the one that is never assigned, so
clicking a directory in it called a nil value.

**Nothing writes an attribute yet**, so queries find nothing until you set
one with `attr`. A Get Info panel that shows and edits them is what makes
the feature real and is the next thing Tracker wants.

---

**A process holds 2224 KB where it held 7232**, and the two things that made
the difference were both consequences of there being one userland image that
every process is a copy of.

`ramfs` kept its store in `.bss` - `static struct node nodes[128]`, 2.1 MB -
so the window manager, Tracker, the shell and twelve other processes each
carried a copy of a server's private storage. It asks for the pages at
startup now, and the cost shows against `ramfs` in the process list where it
belongs.

Then the read-only half of the image - code, fonts, icons, the Lua source of
every program, 2.8 MB - stopped being copied at all. It is mapped where it
lies, one set of physical pages for the machine, read-only and executable at
EL0. Permissions live in the mapping rather than in the page, so nothing
about isolation changes; `design.md` §4.1.1 has the argument and the two
constraints it introduced (an image must be page-aligned, and its declared
read-only half must be bytes the image actually has).

`el0: separate address spaces` had to be rewritten and is a better test for
it. It checked that two processes' code was a *different* physical page,
which was a proxy for isolation and is now deliberately false. It is
`el0: code shared, writable not`, and it asserts the sharing rather than
tolerating it - a test that merely stopped looking would pass if the sharing
quietly stopped happening.

**A whole desktop with a 3D cube turning holds 47 MB where it held 148.**
The heap is now 92% of what a process privately holds, so that is where to
look next - and the answer is starting smaller and growing, not making 2 MB
bigger, because §5.2 chose that number for the collector.

**Drag and drop crosses windows.** The desktop carries a `kind` and an
opaque string it never reads, because it is the only process that knows what
is under the pointer - a press grabs, so the destination never sees one. The
reply is a one-shot right given to the window that was handed the drop and
taken back after, so that "tell the source" is not a way to post an event to
any window whose handle you can guess.

`kfs.rename` takes a path as well as a name, so a move between directories
is two directory entries being edited inside one journal transaction rather
than four megabytes read and written. The test checks the inode number, the
start block and the free count, because "the name changed" is true of a copy
too.

Found on the way: **`files.copy` had not existed since `488f981`** - the
comment block documenting it survived the icons rewrite and the function did
not, so Tracker's Paste had been calling a nil value. And **Photo could only
ever show pictures compiled into the kernel image**, which is why opening one
from Tracker said "no picture called /home/lucas-k.png"; a leading slash now
means a file, decided in the compositor, with no new message and no change to
`ui.image`'s callers.

---

**Six servers are C now, and the console was the interesting one.** The
order was audio, devices, binfs, libfs, appfs, console - each lived with
before the next was started, which is what `CLAUDE.md` asks for and what
found the problems below.

Each speaks a **declared shape** rather than a Lua table: `audioproto.h`,
`devproto.h`, `binproto.h`, `appproto.h`, `conproto.h`. `main.c` dispatches
their role numbers before the interpreter is opened, so a server has no
collector in the process at all rather than a promise not to allocate.

**The console needed a kit, and that is the finding worth carrying
forward.** It is the one protocol with *two* implementations: a terminal
window mounts itself as its child's `/dev/console`, so an application
answers the console ABI as well as the server does, and the runner that
mounts it cannot tell which it got - which is the capability discipline
working correctly. So the layout is compiled once into `use("/kits/console")`
and both sides go through it, rather than a format string in `init.lua` and
a second copy in `terminal.lua`.

`read` also stopped blocking inside a handler. The Lua console pumped its own
mailbox while a line was half-typed, which worked and cost a re-entrant
server and a `sys.yield` spin - there is no UART interrupt to park on. The C
one records who asked and answers from the loop.

**`ramfs` went, and hot reload went with it.** Decided rather than
discovered: ramfs is 247 lines of paths and table lookups, nothing's timing
depends on it, and by `CLAUDE.md`'s own rule it was the weakest candidate of
the seven. It went so the system would be one thing rather than six servers
in C and one in Lua for a feature's sake, at a price known in advance -
`ROLE_RELOAD` deleted and `help("demos")`'s watchable reload with it.
`design.md` §10 is the record, and the honest word is *removed*.

Three things that conversion found, none visible by reading: `/ramfs` had
always stored **Lua values, not bytes** - `help("fs")` promises you get back
the table you wrote - so the namespace packs and the server holds bytes with
a flag saying what they are; the two M4/M5 test clients mount it and had to
learn the protocol; and a replicant publishes a table holding its own source,
several messages long, so packed values page as well.

**`diskfs` is the only Lua server left**, because `kfs.lua` runs on the host
as well as the guest, which is what lets `make test` check the journal's
power-loss window without booting a machine.

**Audio: control by message, data by shared memory.** The period path is an
SPSC ring in shared memory (`audioring.h`) and the message says only which
slot is live. It replaced a 1024-byte period travelling *as* a message
payload 172 times a second, which manufactured 340 KB/s of garbage inside a
5.8 ms deadline. `TICK_HZ` is 250 because of it, and three constants that
silently changed meaning with the tick have been fixed.

**TinyGL is vendored**, with eight demos as eight applications under
`demos/GLDemos`, and a GL Kit at `use("/kits/gl")`. The context is 10 bytes
a pixel and `gl_kosmos.c` refuses one it cannot afford, because
`ostgl_create_context` asserts and an assert here is a panic.

**The chrome is not flat.** `theme.chrome` derives both ends of a gradient
from the one colour a palette names; window tabs and menu bars use it.

**MP3 plays.** minimp3 is vendored and is the first thing brought in here
that needed no patches at all - no libm, no allocation, sixteen-bit output by
default, and NEON is safe because `fp.S` saves the whole `q0`-`q31` file
rather than the callee-saved half. `use("/kits/mp3")` is the door; `music`
takes `.mp3` beside `.wav` and `mp3info` reports the headroom.

**61x real time on QEMU**, and the number that matters is that one 256-frame
period costs 0.095 ms to decode against a 5.8 ms deadline. That is a QEMU
number and worth what `CLAUDE.md` says QEMU numbers are worth, but sixty
times over is margin rather than a fit.

**The window manager's Lua half is about a ninth of a busy pass**, measured
with `frames` on two windows: composing 84.7% (already C), application
requests 10.2%, polls 3.5%, everything else ~1.3%. `CLAUDE.md` carried
"profile before rewriting it" as an open question for months; the answer is
no, and it is recorded there.

**What the same profile found instead**: 4.2 KB of garbage a pass, 3.63 KB of
it in `wait_input` - the call the desktop makes every pass whether or not
anything happened - against 0.02 KB for composing. It was the marshalling: a
1036-byte request string and a 1400-byte reply string, sixty times a second,
because the console had moved to a declared struct. `con.wait` does that
exchange in C into a reused table now. **0.6 KB a pass, one collection
instead of four, worst collecting pass 5.75 ms -> 1.40 ms.**

`frames` reports a KB/pass column per stage, which is what made any of this
visible.

## Next, in order

**This list was three lists stacked under one heading**, with the numbering
restarting twice - 1 to 6, then 2, 3, 4, then 3, 2, 3, 4 - and an entry
saying x86-64 was "started" months after it shipped and ran the desktop.
It read as one ordered plan and was not one, which is the worst failure
mode for the file you read at the start of every session. Rewritten as one
list, in the order the work is actually going to be done.

1. **DNS.** What stands between `188.184.67.127/` and a name. A resolver
   over UDP, a cache, and `/etc`-shaped configuration that the network
   application already has fields for and remembers only.

2. **The AArch64 references in `kernel/`'s comments** - 43 across nine
   files. `process.c` still explains a mapping in terms of "an L1 slot" at
   `0x80000000`, which is the ARM number, in a file that compiles for both.
   The code was made portable and its prose was not.

3. **SMP, on AArch64.** `docs/smp.md` is the plan, counted rather than
   guessed. Nothing here is SMP-ready today: no per-CPU struct, no
   `TPIDR_EL1` anywhere in `arch/` or `kernel/`, the runqueue is `head[]`
   and `tail[]` at file scope in `sched_prio.c`, `current` is one global in
   `thread.c`, and there is not a lock or an atomic in the kernel.

4. **SMP on x86-64, and only once ARM is thoroughly tested.** One
   architecture at a time on purpose: the bugs SMP introduces appear once
   every thousand boots, and chasing them on two boards at once means never
   knowing which half is at fault. `swapgs` and the GS base are what x86
   uses where ARM uses `TPIDR_EL1`, and the port already found the boundary
   the hard way - a global holding the interrupted stack pointer that was
   actually per-thread.

### After those, and not yet ordered against each other

- **A non-blocking send.** `SYS_CALL`, `SYS_RECEIVE` and `SYS_REPLY` are
  the whole IPC surface, so every message to a server blocks for its reply
  by construction, and triple buffering cannot fix half a browser frame
  spent waiting on a `commit` that swaps an index. A change to the IPC
  model: a syscall, a fixed-size queue in the endpoint struct because there
  is no allocator, a decision about what a full queue does, and
  backpressure.

- **Selection in the terminal**, which is where people most want to copy
  from and is the one window the clipboard cannot reach. It draws its
  scrollback through `ui.view` rather than `ui.editor`, so the honest fix
  is lifting the anchor-and-cursor machinery out of the editor rather than
  writing it twice.

- **The cascade, consulted.** libcss parses, selects and answers, and the
  renderer chooses a face by tag name and an ink from a `#define`. The
  piece in the way is a face per (family, weight, slant, size) rather than
  seven fixed ones - which is also what fixes `<em>` inside an `<h1>`
  coming out at the body size. The document should hold one select context
  built from its own `<style>` elements, and layout should ask it per
  element.

- **A box model.** Margins, padding, borders and `width`, which is what
  turns "blocks stacked down the page" into layout. Images and forms both
  wait on it; the runs are already addressable, which is the half that made
  links work.

- **Get Info, showing and editing attributes.** The query engine works and
  nothing writes an attribute, so the search box finds nothing until you
  use `attr` at a prompt.

- **A name in the index.** A query is over attributes, so `name:*.png`
  cannot be one and the search box filters locally instead. BeOS indexed
  `name` precisely so it could be a query rather than a walk.

- **A preferences app for file types.** `filetypes.by_extension` is
  compiled into the image; it wants to be a file in `/home` that an
  application edits, the way `.appearance` already is.

- **Doom's sound.** The hook is there behind `FEATURE_SOUND` and
  `i_sdlsound.c` is the model. Doom is silent.

- **An equaliser in the Mixer**, the first thing that will want the ring to
  carry something other than what was written to it.

- **Seeking in `music`** - the bar is drawn and cannot be dragged. For MP3
  that means finding a frame boundary rather than a byte offset, which is
  what `mp3.decoder():reset()` exists for.

- **SSH**, in layers with a test each: the binary packet protocol, then
  Curve25519, then ChaCha20-Poly1305, then userauth, then channels. The one
  place here where a bug is *silent* rather than loud - a stack that gets a
  sequence number wrong stops working, and a cipher that gets a nonce wrong
  keeps working and is not secure - so "it connected" is not evidence and
  every layer wants test vectors from the specification.

- **The remaining 0.6 KB a pass** is `application requests` (0.42) and
  `answering polls` (0.15) - the desktop serving its own applications over
  the table protocol. Smaller than what was just removed, and the same
  shape if it ever matters.

## Still open

- **`diskfs` stamps `sys.ticks()` as a file's `mtime`**, so a modification
  time means nothing across a reboot and Tracker has no Modified column.
  `/dev/clock` exists and answers with the epoch; what is in the way is
  that a server is spawned with the capabilities it was handed, so giving
  `diskfs` a wall clock is a mount and a decision about which servers get
  one - the same shape as the audio server having no way to read
  `/dev/cpu`. Found while checking whether a comment was still true; it was
  half true, which is the worse kind.
- **3-4 audio underruns per 2.3 s**, and six structural changes did not
  move it: the ring, the priority band, an 8x buffer, the interrupt, the C
  rewrite, and measuring during play rather than after. 194 interrupts per
  400 periods means the device services about two periods per raise, which
  is QEMU's model rather than ours. **The next measurement wants real
  hardware**, which is M2.
- **`procs` at the shell prints `/dev speaks a fixed protocol; there is no
  send to it`.** Pre-existing and verified against an unmodified tree, so
  it is not from the server conversions. `wm procs` is fine; it is the
  console path that is wrong.

## Two things found while adding a kind column, and not yet fixed

**Every launched program is handed the screen.** Both launchers say
`may_pass_screen() and SPAWN_SCREEN or 0`, so any program the shell or the
desktop starts gets `process_grant_screen` - which maps the framebuffer into
its address space *and* promotes it to `SCHED_PRIO_DISPLAY`. Two
consequences, neither intended. Any program can draw over the desktop
without going near the window manager, which is ambient authority in a
system whose first principle is that what you were not handed you cannot
reach. And the compositor's band means nothing when everything is in it -
`process_grant_screen`'s own comment says "whoever was handed the screen is
the one drawing it", which was true when only the desktop was handed it.

Confirmed on the running machine rather than only in the source: a kind
column that tested `owns & SCREEN` labelled `procs` a server.

**Fixing it uncovers something worse, which is why it is not fixed yet.**
Granting the screen only to programs that declare `kosmos: needs screen`
works, and the four programs that draw (`wm`, `deskbar`, `monitor`, `edit`)
now carry the declaration. But with ordinary programs at NORMAL instead of
DISPLAY, **a thread that spins on `sys.yield()` instead of blocking is
starved outright while the desktop runs.** `say 3 hello` never reaches its
own deadline - and its deadline is wall-clock, off `sys.ticks()`, so even one
per cent of a core would finish it. Instrumented, the loop advances only when
it makes an IPC call: a thread that *blocks* is woken and runs, a thread that
only yields is not. The display harness caught it.

So the scheduler answers for that first, and the screen change is one line
here once yielding at NORMAL is fair. This was invisible until now because
every program was promoted into the compositor's band - nothing had ever run
at NORMAL.

**TinyGL is vendored and the GL Kit works (Sep 2026).** Bellard's software
rasteriser, MIT, byte for byte in `runtime/upstream/tinygl/`. It compiled
freestanding on the first attempt with no errors - it wants `malloc`,
`memcpy`, `assert` and seven functions out of `math.h` - so it is compiled
with `-w -Wno-error` as Doom is. `/kits/gl` is the door; gears renders at
41 fps in a 388x400 window.

All eight demos are upstream C, renamed apart on the compile line because
each defines a function called `draw`. `mech` needs one extra `-D`: it calls
its per-frame function `display`, GLUT's name rather than `ui.h`'s.

**All eight are their own application now, under Demos > GLDemos**, and the
Deskbar nests one level for it: `kosmos: section demos/GLDemos`. The teapot
runs at 34 fps, gears at 41.

**What actually stopped them was a heap, not a channel.** A GL context costs
ten bytes a pixel - a colour buffer and a conversion buffer at four each, a
depth buffer at two - out of a two-megabyte heap Lua is already living in.
360x330 wants 1.2 MB and runs; 460x380 wants 1.7 MB and killed the process
with nothing said, because TinyGL *asserts* rather than returning when an
allocation fails, and an assert here is a panic.

So the kit refuses a context it cannot afford and says what would fit:

    a 460x380 context wants 1707 KB and this process may spend 1536;
    about 157286 pixels fit

The budget is three quarters of the heap, and that fraction is measured
rather than chosen - 388x400 wants 1516 KB and runs, 460x380 wants 1707 and
dies. Half the heap was tried first and would have refused the size that
demonstrably worked, which is the other way to be wrong about a limit and
the more annoying one, because it looks like caution.

**The audio server is C, and its protocol is a struct (Sep 2026).** 482
lines of Lua became 462 of C plus a 108-line header. `main.c` dispatches
role 16 before the interpreter is opened, so the process serving
`/dev/audio` has no `lua_State` at all - which is the difference between
promising not to allocate and being unable to.

`sys.call_raw` and `fs.raw` carry bytes rather than a serialised table; the
namespace still resolves the path and is otherwise not involved. The layout
is written twice, in C and as a `string.pack` format, which is the one place
`serialize.h`'s one-implementation rule is deliberately bent - a
`_Static_assert`, a load-time assert and a server length check stand where
the shared implementation used to.

**It did not change the underrun count**, which was predicted beforehand: 4
against 3, noise. Six structural changes have now been tried against that
number - the shared ring, a priority band, an eight-times deeper buffer, the
device interrupt, the C rewrite, and measuring during play instead of after
- and none of them moved it. The server meets a 5.8 ms deadline with the
whole buffer as margin (worst turn 5.4 ms, machine noise floor 0.26 ms), and
194 interrupts for 400 periods says the device completes about two periods
per raise. **The remaining question is a hardware question**, and `CLAUDE.md`
already says QEMU detects regressions rather than saying whether something is
fast.

What the C server bought is not clicks: it is that the audio path can no
longer acquire a collector by accident, and that a client can no longer send
a shape the server has to think about.

**Sleeping is `sys.sleep`; yielding in a loop is a spin (fixed, Sep 2026).**
The audio path had two of them - the server between refills, the client on a
full queue - and playing a tone cost 63% of the machine for the client and
26% for the server, against 8% for Doom, which renders a frame and then
actually waits. `SYS_SLEEP` now exposes `thread_sleep_until`, which the
kernel has had since M6 behind `SYS_WAIT_INPUT`'s console check, and the
same tone costs under 1%.

The comment in `process.c` that refused to promote the audio server to the
display band said a spinning server is the wrong shape and no band fixes it.
That was right, and it is now moot: the server sleeps, and priority
inheritance (`thread_inherit`) already gives it the caller's urgency for as
long as a client is waiting on it, which is the only time it needs any.

**The tick is 250 Hz, and the sound device chose it.** Four measurements,
delivering 580 ms of audio: spinning server 502 ms, sleeping server at
100 Hz 922 ms, sleeping server at 100 Hz with the queue doubled to 46 ms
966 ms, sleeping server at 250 Hz 520 ms. The third is the one that settles
it - **buffering more does not help, because the shortfall is in how often
the queue is topped up rather than how much it holds.** A 5.8 ms period
cannot be serviced on a 10 ms clock. `audiolag` is the instrument: if its
mean wait is a whole tick, the pipeline is running at the tick rate instead
of the device rate.

Anything measured in ticks changed meaning with it, and **the blinking
cursor is how that got found** - it went five times a second and looked
frantic, which was `CURSOR_TICKS = 25` with a comment reading "at 100 Hz".
Behind it were two that mattered more and were silent: both schedulers set
their quantum to a bare `10`, so a tenth of a second quietly became a
twenty-fifth. All three now derive from `TICK_HZ`, and `wm`'s input timeout
derives from `sysinfo.tick_hz` instead of the literal `1` it was when a tick
happened to be the interval it wanted.

The lesson is the one this system keeps relearning: **a duration written as
a count of ticks is one fact stored twice.** Nothing failed, no test caught
any of the three, and the only reason the quantum change was noticed at all
is that a cursor next to it was visible.

**Waiting is `sys.receive` with a deadline, not `sys.sleep` (Sep 2026).**
The sleep was right about cost and wrong about shape: **a server that sleeps
on a timer is deaf.** The audio server could not answer a client until the
timer got round to it, so every `play` cost a tick - and the Music window,
which hands over a dozen periods a pass, spent 45 ms on twelve round trips
that should be microseconds and played at two thirds speed with the
processor almost idle. *Slow and idle at the same time* is the signature:
it means waiting on the wrong thing.

`ipc_receive` now takes a timeout in scheduler ticks, so a server waits for
a message **or** a deadline, whichever comes first. Both halves already
existed - a thread blocked on an endpoint is `THREAD_BLOCKED`, and
`thread_wake_sleepers` wakes any blocked thread whose `wake_at` has arrived
- and had never been put together.

**Writing the test found a real race.** Unlinking the timed-out receiver
inside `ipc_receive` after it woke leaves a window: the timer makes the
thread *ready* and it does not run until the scheduler reaches it, so a
sender arriving in between was handed a receiver that had already given up -
the message lost, the sender blocked for ever. `ipc_timed_out` now unlinks
from the timer, before the thread becomes runnable. A control test with the
same scaffolding and no timeout in it passed throughout, which is what said
the fault was in the kernel rather than in the test.

**One test was written for this and is not in the tree**, which is worth
being explicit about rather than quiet. `ipc: a timed-out receiver leaves no
trace` - time out a receiver, then have a second thread call and check the
message reaches a fresh receiver - fails, and the same sequence without the
timeout passes. It may be a residual fault in the timeout path or a
thread-pool artifact of the kernel test harness; it was not isolated. What
is known: the path carries thousands of round trips a second under Music
with nothing lost, and `ipc: a receive with a deadline gives up` passes.
**Not a closed question.**

**The virtio-sound interrupt is still the right answer** and is still not
used. Polling at 250 Hz costs almost nothing and meets the deadline, but it
is a rate this system chose rather than one the device asked for. The driver
has the queue set up for it.

**An intermittent panic in `prio_pick_next`.** Seen twice today, both at
`sched_prio.c:165` reading `far 0x2b0` - `head[level]` was NULL while the
`occupied` bitmask said that level had somebody in it. Once in the benchmark
image and once in a screenshot run; both times the run before and the run
after passed with identical code, so it is timing-dependent. Not
root-caused. The queue invariant has only three writers (`prio_init`,
`prio_enqueue`, `prio_pick_next`) and they look locally consistent, so the
next place to look is a thread whose effective band changes while it is
queued - `thread_inherit` and `thread_disinherit` write `sched.effective`,
which is what `level_of` reads, and neither re-queues.

---

## A frame is measured now, and what it said

`make frames`. `wm` keeps seven stage counters, `/bin/frames.lua` reads them
over the window manager's own protocol, and `tools/run_frames.py` drives an
idle desktop, one with a plasma animating, and a window being dragged.

This existed because nothing measured a frame. All five gated benchmarks are
the kernel - IPC, context switch, page fault, allocation - in a system whose
stated aim is a desktop that stays responsive on a Pi 5, and every argument
about rewriting the window manager in C was therefore an argument about a
number nobody had.

**Under animation, composing is 83% of a busy pass, and composing is already
C.** The Lua half - event routing, focus, damage bookkeeping, layout - is
the other seventeen per cent, and most of that is IPC rather than
computation. Rewriting this process in C attacks the seventeen.

**The worst pass has never been a garbage collection.** The worst collecting
pass came to 45-69% of the worst pass overall, in every run. GC jitter is
the whole stated reason for moving a server to C, and here it is not what
the worst case is made of. That is the measurement the C question was
waiting on, and it says no for this process.

**What it found on its first run was in the compositor.** A dragged window
composed half the pixels of an animating one and took four fifths of the
time, which is a cost that does not scale with the damage - and it was
`back:fill(fx, fy, fw, fh, tab)` filling the whole frame of every window a
damage rectangle touched. Ten pixels of damage on a 360x264 window cost
95,040 of them. Clipped, plus the title and the desktop stamp only when the
rectangle reaches them: 4.24 -> 4.09 ms a composing pass, 8.84 -> 8.08 ms
worst. Small on the animating case on purpose - a plasma commits its whole
window, so that is the load the fix helps least. Small damage was the case
paying a whole window frame.

**Not gated, and deliberately.** These are QEMU numbers and `CLAUDE.md` is
clear about what those are worth: the shape survives the emulator, the
milliseconds wait for a Pi.

**What is still weak in it.** The dragging scenario composes about nineteen
frames in eight seconds, which means the synthetic drag is not reliably
landing on the title bar - the numbers from that row should not be trusted
until it is calibrated. And nothing yet decomposes *inside* compose: the
split between what the C primitives cost and what asking for them costs is
still inferred from three points rather than measured.

---

## Where the work comes from

**There are no milestones**, and there were thirteen. `docs/roadmap.md` is
two lists now - what is built and what is wanted - because a number that no
longer means anything is read as though it does: the README said "M6,
graphics" while the machine had a journalled filesystem, a TCP/IP stack and
a web browser in it.

What gets built next is chosen by hand, from the wishlist, by what is
interesting and what unblocks the most. The order agreed for now: a
resolver, then a non-blocking send, then x86-64 under QEMU, then real
hardware.

## Active target

QEMU `virt` aarch64, and nothing else. Real hardware arrives at M2.

## Recently done

- **Doom.** See above. Seven bugs on the way and the two the *system* owned
  are the ones to remember: `snprintf` ignored precision on integers, so
  `%.3d` of 33 gave "33" and Doom's HUD font lumps came out one character
  short - a missing-file error caused by a formatting bug. And C output from
  a windowed process vanished entirely, because `SYS_WRITE` is refused
  unless the process owns the console; forty thousand lines of Doom started
  up, failed and said nothing. A refused write now spills into a ring that
  something with a namespace drains.

- **`exit()` can land somewhere.** It panicked, and the comment was right -
  there is nothing to exit *to*. That is exactly what a vendored port
  breaks: `I_Error` prints and calls `exit`, and with a panic on the end the
  process died with the explanation still in a buffer. `kosmos_exit_arm()`
  is a `setjmp` a caller may arm.

- **`fopen` works for files a process already holds.** `kosmos_provide(name,
  bytes, len)` and then `fopen` finds it. Not a global tree: one process
  saying what a name means to it, which is what CLAUDE.md's "a libc whose
  I/O resolves against that process's namespace" comes to when there is no
  tree to resolve against.

- **Keys are two streams now.** `key` is characters, as always. `rawkey` is
  transitions - keycode and up/down - posted only to the focused window,
  gated on owning the console because a process that can watch every key is
  a keylogger. A character cannot say a key is *held*, which is why holding
  a direction in Doom was a step per key-repeat.

- **A clock.** PL031 at 0x9010000, read out of the device tree rather than
  remembered. `/dev/clock`, `lib/clock.lua`, and a Date & Time panel. The
  offset is an *offset*, not a timezone: there is no tzdata, so summer time
  is set by hand twice a year and the panel says so.

- **Restart and Shut Down**, in the Deskbar. PSCI over `hvc`, again from the
  device tree. Gated on `owns_procctl`, because turning the machine off is
  ending every process at once.

- **The desktop is Tracker with the frame taken off.** `backdrop = true`
  puts a window at the bottom of the stack undecorated; `strip = "top"` is
  its opposite and takes room away rather than sitting over things.

- **Real file icons.** The Tango Icon Library, public domain, at the 32x32
  size it was drawn for, decoded by `gfx.png` and composited by
  `surface:blend`. Nothing converts them.

- **Startup items**, in `/bin/startup`, read by the Deskbar - deliberately
  not on the boot path, because init's argument about a machine that cannot
  reach a prompt still stands.

- **One bug shape, five times in one session**, and it is worth naming
  because it will happen again: *two copies of one fact that agree until
  they do not*. The font role and the list selection. The bar's height and
  the height it was granted. `USER_HEAP_PAGES` in the kernel and
  `USER_HEAP_SIZE` in userland. Appearance's hardcoded layout against a font
  size the user picks. And `struct sysinfo` never being zeroed, which is the
  same thing wearing a security bug's clothes - any field the kernel does
  not write is kernel stack handed to a process.

- **A review pass, and two live races it found.** Reading `memobj.c` rather
  than remembering writing it: `memobj_unref` cleared `in_use` *before*
  walking the index to free the pages, so another `sys.memory` could claim
  the descriptor and start rewriting `index[]` mid-walk - pages freed twice,
  and pages belonging to the new region freed under it. `memobj_create`
  mirrored it, writing `pages` and the index into a slot it had not claimed.
  The old contiguous code had a narrow version of both; turning a region
  into a page list widened the window enormously. The slot is claimed first
  and released last now.

- **The text-extraction path is gone, with everything that served it.**
  `pdfpage.text`, `decode`, `char`, `tounicode`, the `/ToUnicode` parser and
  `user/bin/pdftext.lua`. It was the first idea - read a PDF as text and lay
  it out in the system font - and the renderer superseded it.

  It was not merely dead. `pdfpage.font` read and inflated the `/ToUnicode`
  stream **on every page render, per font**, to build a table nothing used
  any more. A page went 152 ms to 143 ms by deleting it.

  Also removed: `pdfpage.prepare`, which was written for an allocation
  ordering that no longer applies and was never called; and `multiply`,
  superseded by `multiply_into` the day the interpreter stopped allocating a
  matrix per glyph. `pdfpage.lua` is 30 KB down to 22 KB.

- **A comment that had become false.** `pdfpage.lua` explained its buffer
  sizes in terms of `pmm_alloc_contiguous` - true when it was written and
  wrong since regions became lists of pages. The sizes are right for their
  own reason and now say so.


- **A server runs at the priority of whoever is waiting for it.** Priority
  inheritance, from QNX, and it fits a synchronous rendezvous exactly: the
  kernel already knows who is blocked on whom, because that is what
  `ipc_call` is. `thread_inherit` on delivery in both directions,
  `thread_disinherit` in `ipc_reply`, and the scheduler queues on the
  *effective* band rather than the given one.

  This is the answer to the thing that failed two days ago. Promoting the
  console server to the input band starved the machine, because it is also
  the path every `print` takes - at the top band it outranks everything it
  serves. Inheritance means the question never arises: it sits at NORMAL and
  *becomes* urgent for exactly as long as something urgent is waiting on it.
  The note in `process.c` that recorded the failure now records the answer.

  Cleared on reply rather than unwound. A coroutine server handling two
  requests at once would need a stack of borrowed bands to be exact, and the
  error either way lasts one request - erring downward, which is the safe
  direction: a server that stays high starves the machine, one that drops
  early is briefly slower.

  There is a test, and it is the kind that would fail silently without care:
  a server created at LOW, a caller at INPUT, and the server reads its own
  effective band from *inside* the handler and again after replying.

- **The benchmark "deadlock" was a stale image, and three conclusions drawn
  from it were wrong.** Worth recording in full, because the failure was in
  the method rather than the code.

  The bench image is `build/bench/kosmos.elf`, built by `make BENCH=1
  build/bench/kosmos.elf`. Plain `make` builds `build/kosmos.elf`, a
  different target. Booting the second while believing it was the first gave
  a `thread_block: every thread is blocked` panic from an image several
  commits old - and on the strength of it: the benchmarks were declared
  hung, priority inheritance was blamed, inheritance was reverted, the panic
  "persisted", and inheritance was therefore declared innocent. Every one of
  those was reasoning about a binary nobody had rebuilt.

  Rebuilt properly, HEAD runs all five benchmarks under `-icount` and there
  is no deadlock anywhere.

  The harness timeout is still raised to thirty minutes, which is right for
  its own reason: the image has grown by the glyph rasteriser, the PDF
  scanner and the inflate kit, and `-icount` is several times slower.

- **What the benchmarks actually say about priority inheritance:**
  `context_switch` 8.375 -> 9.125 (+9.0%) and `ipc_roundtrip` 36.438 ->
  41.439 (+13.7%). Not recorded as a new baseline, because whether that is
  worth paying is a decision rather than a measurement. The cost is
  `thread_effective_priority` - a max of two fields - being called on every
  enqueue, every pick and every wake, plus the inherit and disinherit on the
  IPC path. It is optimisable: the effective band could be stored on the
  thread and recomputed only when either input changes, which would take
  most of it back.


- **A PDF renders as it was typeset.** `wm pdfview:/home/odyssey.pdf` draws
  the page in the document's own Times New Roman, at the positions its
  producer chose, from the font programs carried inside the file. 685 glyphs
  in 152 ms, and scrolling is a blit out of a surface that already holds the
  page - the interpreter does not run again and no glyph is rasterised
  twice.

  `user/lib/docfont.c` is the C half: a font loaded from bytes in the
  document, rasterised **by glyph index** rather than codepoint, cached per
  face per size, and a `draw` that takes a whole page as a flat array so a
  page is two or three crossings instead of two thousand. `pdfview` is a
  direct window (`gfx.md` 19.4), which is what lets it own its pixels.

- **A PDF's fonts have no `cmap`, and that is correct.** A CID-keyed subset
  is addressed by glyph index, so a character map means nothing and the
  producer drops it - the font here has eleven tables and `cmap` is not
  among them. `stbtt_InitFont` refuses a font without one, *and* refuses one
  whose cmap carries no encoding record it recognises: the last thing it
  does is `if (info->index_map == 0) return 0`. Rather than touch vendored
  code, `ensure_cmap` writes a 22-byte table into our own copy of the font
  and repoints the unused `post` entry at it.

- **Sixteen capabilities a thread was a number from when the userland was a
  shell.** A graphical application holds its console, its `/dev/wm`
  endpoint, the filesystem, its window's region, a read buffer, the buffers
  a page decodes through, and a region per embedded font. It ran out
  mid-page, and the failure arrived as `NO_ROOM` - which reads as "out of
  memory" and sent two evenings at the allocator. There were 117,000 free
  pages at the time.

  Three things came out of that and all three stay: the limit is 32,
  `SYS_ERR_NO_CAPS` is its own error rather than folded into `NO_ROOM`, and
  **`sysinfo` reports the region pool**, which it never did - `memobj_in_use`
  and `memobj_total` had existed since regions did and nothing had ever
  called them, so "could not allocate a region" was the same sentence
  whether the machine was out of memory or out of descriptors.

  **Sixty-four was tried first and panicked the benchmark image.** A slot is
  32 bytes, so that was 73 KB more `.bss`, and this kernel has a documented
  constraint about exactly that: the thread stacks and their guard pages
  have to stay inside the first 2 MB of RAM, the only part mapped a page at
  a time. `make test` passed and the benchmarks did not, which is the
  argument for having a third build.

- **Errors from a syscall have words now.** `ipc_error` knew five codes and
  answered "unknown error" for the rest, including every `SYS_ERR_*`. Half
  of the debugging above was reading that phrase.


- **A region is a list of pages, not a run of them.** `memobj.h` used to
  explain why they were contiguous and named the cost in its own words: "a
  large region can fail to allocate on a fragmented machine even when there
  is enough memory. That is real." It became real, so the premise changed.

  A page holds 512 pointers, so a region's pages are indexed by up to eight
  index pages taken from the allocator itself. The objection the old comment
  raised - a quarter of a megabyte of `.bss` for lists that are usually
  empty - is answered rather than ignored: the index is allocated per region
  that exists, and the descriptor grows from 40 bytes to 96, which is 24 KB
  of `.bss` instead of 10.

  What is given up: mapping walks an index instead of adding to a base, and
  a region can no longer be handed to a device expecting one physical run.
  Nothing does - DMA here uses kernel buffers, identity mapped and
  contiguous by construction.

  156 checks and the benchmarks are unmoved by it.


- **The scheduler can be changed while the machine is running.** `wm
  scheduler`: which policy, how long a turn lasts, what the timer rate is
  and how many bands there are - all read from the kernel, and the first two
  changeable from the window. There is a button that starts a busy thread,
  because an idle machine schedules identically whatever you pick and the
  app would otherwise be a display of numbers that never move.

  `SYS_SCHED_INFO` and `SYS_SCHED_SET`, and `sched_switch_to` underneath
  them. **Swapping policies had to drain, not reset.** `sched_use` calls
  `init`, which empties the queues - correct at boot, where nothing is in
  them, and a way to lose every runnable thread on the machine at any other
  moment. The threads are not in a list the kernel keeps; they are in
  whatever structure the policy chose, and `pick_next` is the only handle on
  them. So they are pulled out one at a time and handed to the new policy
  before it is installed, with interrupts masked across the exchange.

  **Quantum and policy are anyone's to change; priority is not.** Tuning the
  machine you are sitting at is not reaching into another process, and there
  is nobody to defend a single-user system from. Setting a *priority* is
  different: bands are handed out by capability precisely so nothing can
  promote itself, and a syscall for it would undo that in one line.

- **The screen owner runs in the display band; the console owner does not
  run in the input band.** The first is committed and works. The second was
  tried, because it looks like the same argument - the console owner is the
  one process allowed to read input, so it is what every keystroke waits on
  - and it starves the machine: that process is also the *output* path, so
  at the top band it outranks everything it is serving. `thread: three
  threads interleave` failed within a minute.

  The fix is one of two things this policy does not have: a boost that lasts
  only across the wake, or priority inheritance across IPC, which is QNX's
  answer and the better idea. Written up in `process.c` where the promotion
  would go.

- **A struct in `syscall.h` outside the assembler guard.** `user/hello.S`
  includes that header for the syscall numbers, and a struct there is a
  syntax error per line. It only broke the *test* image, which is the only
  build that assembles that file - so the ordinary build was clean and the
  suite caught it.


- **Priorities, and a wake that is acted on.** `kernel/sched_prio.c`: five
  named bands - idle, low, normal, display, input - with round robin inside
  each, and a thread that becomes ready while outranking the running one
  takes the CPU at the next exception instead of waiting out a quantum.
  `design.md` and `ui.md` had both called input-at-highest-priority
  non-negotiable since before there was a scheduler that could express it,
  and `sched_rr.c` said in its own first comment that there was "nothing to
  prioritise". Both are now true at the same time.

  From QNX, which is a microkernel of the same shape and a real-time system
  - and real-time means bounded, not fast. Taken: strict priority with
  immediate preemption, and a quantum that can be changed. Not taken: 256
  levels, and hard guarantees. Starvation is real under strict priority and
  is accepted deliberately; the reasoning is in the file.

  **The cost, measured rather than assumed.** `context_switch` 6.875 ->
  8.375 and `ipc_roundtrip` 30.376 -> 36.438. Read against two days ago
  rather than against yesterday: the switch is **-14.6%** net and IPC is
  flat, because lazy FP save bought the priority queue rather than the
  priority queue being free.

  The first version scanned the eight levels to find the highest occupied
  one, and cost twice that - almost everything runs at NORMAL, so almost
  every pick walked five empty bands first. An occupancy bitmask and `clz`
  make it one instruction, which is what QNX and Linux both keep. The
  benchmark caught it the same afternoon.

- **A test was quietly disabling the feature it sat above.** The policy-seam
  test swaps in a deliberately terrible LIFO scheduler and then restores the
  default - by *name*. The name it restored was `sched_round_robin`, which
  stopped being the default the moment `sched_priority` arrived, so every
  test after it ran under round robin. Both new scheduler tests reported the
  priority policy broken when what was broken was that one line.

  A test that changes global state and puts back what it *thinks* was there
  is a test that can disable a feature for everything after it, and report
  the feature as the failure.

- **And one of those tests was wrong in the other direction**: it put the
  low-priority thread at LOW while the test thread itself ran at NORMAL, so
  strict priority starved it exactly as designed and the test measured
  starvation rather than ordering. Correct behaviour, badly built test.


- **FP and SIMD are saved lazily, and it is the first piece of M10.**
  `context_switch` **9.812 -> 6.875 ticks, -29.9%**, and `ipc_roundtrip`
  **36.251 -> 30.376, -16.2%**, because a round trip is two switches and was
  paying for the whole register file twice.

  The switch does not save FP any more. It disarms it - `CPACR_EL1.FPEN` to
  0b00 - and the first floating-point instruction the incoming thread
  executes traps into `fp_fault`, which writes the previous owner's
  registers into its context, reads this thread's back, and arms FP again. A
  thread that uses FP pays one fault per time slice. A thread that does not
  pays nothing, and most do not: the kernel is built `-mgeneral-regs-only`
  and cannot emit an FP instruction, so every kernel thread is in the second
  group.

  **Trapping EL0 alone was tried first and three tests said no**, which is
  the argument for having had them. Kernel threads run at EL1, so their
  registers would have been neither saved by the switch nor faulted in by
  anything - which is exactly the bug the eager save was added to fix. And
  `longjmp` writes `d8`-`d15` from its buffer, so a kernel thread returning
  through one would overwrite whatever EL0 thread owned those registers.
  Arming both levels is simpler than either.

  `exception` went **7.250 -> 7.562, +4.3%**, and that is the price rather
  than a regression to chase: every exception now begins by asking whether
  it is an FP trap, because that has to be settled before the
  fault-expectation machinery looks at the frame. One comparison on every
  exception against the whole register file on every switch.

  Two permanent tests. The first was rewritten rather than added: it used to
  assert `CPACR.FPEN == 0b11`, which tested the old *mechanism* - FP enabled
  once at boot and never moved - and says nothing now. It asserts the
  property instead, that FP works at EL1, plus that the lazy path is what
  made it work. The second drives the mechanism directly: disarmed and
  unowned after a reset, armed and owned after one instruction.

  **That second test was wrong on its first attempt in an instructive way.**
  It called `thread_yield` to force a switch, and a yield with nothing else
  runnable does not switch at all - so the setup silently did nothing and
  the test failed for a reason unrelated to what it was checking. A test
  whose setup can quietly not happen is a test that will one day pass for
  the wrong reason.

- **doomgeneric is vendored and the WAD is on the disk.** 95 `.c` files,
  73,095 lines, unmodified under `runtime/upstream/doom/` with its licence,
  the same rule `lua/upstream/` and `stb/` follow. Its platform layer is six
  functions - `DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`,
  `DG_GetKey`, `DG_SetWindowTitle` - and `pixel_t` is `uint32_t`, so a frame
  is a blit into an XRGB8888 framebuffer rather than a conversion. Nothing
  is built yet: it is not in the Makefile.

  `doom1.wad` is 4,196,020 bytes of shareware and is **not** in the tree. It
  lives on `build/play.img` beside `odyssey.pdf`, which is what a filesystem
  is for.


- **A PDF is readable in a window.** `wm pdfview:/home/odyssey.pdf` opens
  The Odyssey and turns its pages, 142 ms a page. Text, wrapped by the
  window in the system font - not the page as it was typeset, which needs
  glyphs rasterised by index out of the font inside the document and is the
  next piece.

  What stood between it and working was memory, and finding it took three
  wrong answers. The scanner returns a batch of tokens as two Lua tables,
  and at 1024 entries those did not fit beside everything else a windowed
  program holds - a window starts around 640 KB because the UI kit is
  loaded, against 330 KB for a console one, which is exactly why `pdftext`
  worked all along and `pdfview` did not.

  Two things fixed it rather than one, and the second is the one that
  matters: the batch is 256 now, and the page list holds an object number
  per page instead of a table with a reference and four inherited
  attributes in it. That was 500 tables for a 254-page book and about
  310 KB still held after opening; the inherited attributes are found by
  walking `/Parent` when a page is asked for. Before it, 512 worked and
  1024 did not. After it, 2048 works. The batch sits at 256 well inside
  that.

- **A check that cannot see a failure reports a pass, twice in one
  evening.** The first was `grep -c ... || echo 0`, which prints `0` twice
  when it matches nothing, so every run compared unequal and every batch
  size looked broken. The second was worse: `pdfview` showed its error *in
  the window* and printed nothing, so grepping the serial log said every
  batch size worked - including the ones a screenshot plainly showed
  failing. Errors go to the serial line as well as the window now, which is
  the actual fix; the lesson is the one this project keeps paying for, that
  a test exercising a different path from the user is a test that agrees
  with you.


- **A PDF reads on the machine, and the language line moved with the
  measurement.** `/lib/pdf.lua` is the object layer - cross-reference table,
  indirect objects, the page tree - and it never holds the document: it asks
  its source for a window at a time, and parsing all 1127 objects of a 1.6 MB
  book reads 5.9% of the file. The window is 256 bytes because that was
  measured against 128, 512, 2048 and 8192, and the table is in the file.

  `/lib/pdfpage.lua` interprets the content stream and `pdfinfo`, `pdftext`
  and `pdfbench` are the programs over it. The Odyssey's 254 pages, its
  fonts, and a page of Homer as text, all from `/home/odyssey.pdf` on a real
  disk.

- **The scanner is C and the profile is why.** A page cost 1.1 seconds, of
  which 538 ms was the tokenizer - 144 microseconds a token, which is
  `string.sub` allocating a one-character string per byte. In C the same
  3,657 tokens take **4.7 ms**. The page is now 380 ms and the remainder is
  the interpreter's matrix arithmetic, which is the next thing to look at and
  has not been.

  `pdfbench` prints the profile phase by phase and stays, so the day someone
  wonders whether the C is still earning its keep the answer is one command.

- **Kits.** C libraries reached as `use("/kits/compress")`, through the
  namespace like any other library. `inflate` and the PDF scanner sat in
  `sys` for an evening and did not belong: `sys` is the syscall boundary, and
  a decompressor is not a syscall. `kits` lists them.

- **A capability can be given back, which it could not before.**
  `SYS_CAP_DROP`. A thread has sixteen slots and nothing ever released one -
  `ipc_caps_release` ran when a thread died and that was all - so the
  filesystem server, handed a buffer per request, filled its table and
  refused every request after the sixteenth for the life of the machine. The
  endpoint pool had this same bug once and was fixed; memory never got the
  matching half. Found by a PDF read in 256-byte windows, on the fifteenth
  read.

- **And releasing has to mean losing the mapping.** `SYS_SHARE_UNMAP`, because
  `SYS_UNMAP` is bounded to the window `SYS_MAP` hands out and *frees* what it
  unmaps - correct there, a double free here, since a shared region's pages
  belong to the memobj. Without it `sys.release` gave up the right to name a
  region while keeping the ability to read and write it.

- **The bug behind the bug, and it cost an evening.** `region_of` in
  `sys_user.c` caches mappings keyed by capability *index*. That was safe for
  exactly as long as an index was never reused, which was until `sys.release`
  existed. Afterwards a server dropped slot 1, the next region arrived at
  slot 1, and the cache handed back the *previous* region's address - so the
  server wrote 1811 bytes into somebody else's pages, read them back
  correctly, and reported success, while the process that owned the buffer
  saw zeroes.

  What found it was making the server read back its own write: `server_sent
  789c, server_readback 789c, client 0000` says the two capabilities are not
  the same object, and nothing else does. Three hypotheses were tested and
  discarded first - the install dedupe, generation on release, and a false
  byte count - which is worth recording because each was plausible and none
  was it.

- **An install dedupe was tried and removed.** Handing back an existing index
  for a region a thread already held, without taking a reference. Wrong in
  company: `SYS_MEM_CREATE` unrefs after installing on the stated grounds
  that install took a reference, so when the dedupe fired there the count
  went to zero and freed the region its caller had just made.

- **Decimals are parsed exactly.** The C scanner multiplied a running scale
  by 0.1 per digit, so `-2.25` came out a few units in the last place from
  what Lua's own `tonumber` gives: it prints identically and compares
  unequal. Now it is an integer mantissa divided once by a power of ten. A
  content stream is mostly fractions like `.23999999`, six before every `cm`,
  so this was a rendering bug and not only a test one. The test found it on
  its first run.

- **`BOOT` with a space in it never worked.** `make qemu BOOT="wm blocks"` is
  in the Makefile's own comment as an example; the shell split it and QEMU
  took the second word for a filename. Quoted now.

- **Three permanent tests**, one per thing above: forty regions made,
  mapped, written, read back and released; a Flate stream produced elsewhere,
  both through a string and region to region; and the scanner against a
  content stream with a negative number, a leading-dot fraction, a hex
  string and an escaped bracket in it.


- **The disk can be written from this Mac.** `tools/kfs.lua` runs the
  filesystem on the host over the image file: `create`, `ls`, `put`,
  `get`, `rm`. It is the answer to the one real cost of not using FAT32 -
  a Mac cannot mount the image, but it can write it. Both directions are
  tested by `run_interchange.py` in `make test`: a file written here is
  read inside the machine, and a file written inside is read back here.
  One implementation of the format, not two, which is what makes it
  trustworthy.

- **Subtree mounts, and the layout is real.** `ns.mount(prefix, cap,
  root)` maps a name onto part of a server, so one disk appears as
  `/system`, `/user` and `/home` - which is what `layout.md` describes and
  what a single filesystem could not do before. `mkfs` makes those three
  directories, because a formatted disk should be a Kosmos disk: without
  it `save notes.txt` failed on a fresh drive, `/home` being a mount point
  with nothing behind it.

- **Next, decided:** a PDF reader for plain documents - no forms, no
  encryption - then sound, then Doom. The PDF viewer is more tractable
  than it looks: `puff` already does Flate, `stb_truetype` already
  rasterises glyphs, and the blitter already exists. What is left is the
  object model and the content-stream interpreter, which is structure
  parsing and belongs in Lua.


- **A file can be larger than a message.** `read` takes `into` and `write`
  takes `from` - a capability to shared pages the caller owns - which is
  `read(fd, buf, n)` with the buffer named by a capability instead of a
  pointer, because a server at EL0 cannot dereference the caller's
  pointer. 200 KB out and back, in one extent, verified across a reboot.
  Before it, nothing above about two kilobytes could be written at all.
  `kfs.read_range` is the `pread` underneath, so a window is read without
  the rest of the file. `MEMOBJ_MAX` is 256, so a tiled image can hold a
  region per tile.

- **`docs/layout.md`**: what lives where, in the tree and at runtime. A
  BeOS-shaped runtime layout - `/system` for what ships, `/user` for what
  is installed, `/home` for what you made - with the caveat that it is a
  convention init hands out rather than a tree anything can walk. It also
  records two constraints any "servers in C" plan runs into: every process
  is currently the same ELF entered at a different role, and there is no
  dynamic linking, so C system libraries are a build-time fact rather than
  files.


- **A directory bigger than a message lists.** `list` answers in pieces the
  way `read` already did - offset in, `more` out. Before it, `ls` on a
  directory of two hundred files said "the answer does not fit in a
  message" and showed nothing, which the power test had been quietly
  hiding behind an `or {}`. Found because that harness printed "128 files
  survived, 0 names" and the two numbers disagreed.


- **The journal works, and M8's definition of done is met.** ext3's shape:
  write the blocks into the journal, write one commit block, apply them,
  clear the header. The ordering is the whole guarantee. `make powertest`
  kills the machine five times mid-write and finds every directory entry
  readable and no file half-written; `tools/test_kfs.lua` tests replay by
  *choosing* the instant, because a SIGKILL cannot aim at the few
  milliseconds that matter. Each of the four rules was deleted in turn to
  check its test fails without it - and one did not, which is how a hole in
  the uncommitted-journal case was found.

- **A host Lua**, `build/host/lua`, so pure-logic libraries are tested
  without booting. `kfs.lua` is the case that asked for it.

- **The language line, measured rather than argued.** The journal's
  structure in Lua costs about 2%; its checksum, a byte loop in Lua, cost
  30%. Moved to C it recovers about half - the rest is unexplained and
  worth chasing. `design.md` 6 now carries the whole reasoning: where the C
  actually runs (EL0, in the process, not the kernel), what a C server
  would cost in marshalling and hot reload, and the experiment that would
  settle it - `kfs.store` written both ways. The prediction on record is
  fifteen percent.

- **Next, by decision rather than by roadmap:** audio - a virtio-snd driver
  in the HAL, WAV first to prove the path, then a vendored MP3 decoder as
  userland C beside the font rasteriser - and a music player. Then Doom,
  then real hardware. `roadmap.md` is a guide and not a rule; it still says
  audio is out of scope, and it is out of date rather than right.


- **The serialised format is little-endian on purpose now.** It used to be
  a `memcpy` of the native bytes, which was invisible because every target
  so far agrees. It matters for the disk before anything else: attribute
  blocks are `sys.pack` output written into a block, and `kfs.lua` is
  explicitly little-endian everywhere else, so that was the one part of the
  on-disk format that depended on the machine that wrote it. A disk
  outlives a boot and can be carried to another machine. The test checks
  the actual bytes, because a round trip proves only that the two halves
  agree with each other - two matching native-endian halves round-trip
  perfectly and produce a format nothing else can read.


- **The index is rebuilt at mount, and queries work on the disk.** A scan
  of the tree reads every attribute block into `index[attribute][value] ->
  paths`, the same shape the ramfs builds. Nothing about it is written
  down: derived state that is also stored is state that can disagree with
  itself, and on a filesystem that disagreement is a query returning a file
  that is not there. `name` is indexed for every file without anybody
  declaring it, which is BFS's rule. `find` now asks every mount that can
  answer instead of the one it used to name.

- **The arrow keys are not broken.** The display harness's arrow phase
  fails often and it has been recorded as a bug twice. The input path was
  instrumented end to end and is correct at every stage: the window manager
  forwards all three bytes, the kit decodes them, and the selection moves
  sixteen pixels a press - 264, 280, 296, 312, 328, measured with the
  harness's own probe. What fails is the harness, on a busy host, at
  whichever phase it happens to be running. `testing.md` 18.11 has the two
  mistakes made while working that out, including a control run that shared
  the variable it was controlling for.


- **Kosmos is MIT.** `LICENSE` at the root, and every file this project
  writes carries a one-line notice above its description. Vendored code is
  untouched and its licences are named in `LICENSE`. `assets2c.py` now
  repeats each vendored file's licence in the generated C and warns when
  one has none - which found two fonts and an image that have none.

- **Attributes on the disk.** One block per file, pointed at by the inode
  field that had been reserved and zero since the format was laid down, and
  serialised with the same `sys.pack` a message uses. `kind`, `size`,
  `mtime` and the extent count are read from the inode and refused by
  `setattr`: a stored copy of a fact is a copy that can disagree. `kind` is
  the exception and it took a failing test to see why - structural for a
  directory, free for a file, because a People file's kind is `person`.
  Nineteen checks in `make disktest` now, across two boots.

- **A benchmark for comparing machines.** `score` prints it, `sysbench`
  draws it, and both are the same engine in `/lib/bench.lua`. Twenty-two
  measurements in six groups, two minutes, one number. Fixed time rather
  than fixed work, so a Pi 1 takes the same two minutes and reports smaller
  numbers. `testing.md` 18.10 has the three mistakes that had to be fixed
  before any of it meant anything.

- **Pac-Man is gone and Tetris is `blocks`**, titled Falling Blocks. Names
  somebody else owns are not worth the trouble on a public repository.


- **Outline fonts, with three roles.** `stb_truetype` vendored unmodified,
  TrueType and CFF, so `.otf` works as well as `.ttf`. The build embeds
  whatever is in `assets/fonts/`, so adding a face is dropping a file there.
  Three independent roles - `ui`, `text`, `mono` - because a terminal's face
  has to be fixed-width whatever the other two are, and one setting for all
  three could only be right for one of them. Appearance picks a face and a
  size per role. The check that says they are really independent: measuring
  `iii` against `WWW` gives 21/75 for a proportional face and 18/18 for the
  monospace one. See `gfx.md` §19.12.

- **A machine with no display boots again.** It had not, and silently: init
  asked for the screen grant whether or not there was a screen, the kernel
  refuses a grant it cannot give, and the refused spawn was the shell - so
  `make serial` reached stage 12 and then a prompt that never came. The
  same mistake was in `run_program`, so even with the shell fixed nothing
  would start. Both fixed by asking for the grant only when the machine can
  give it, which is what the disk already did four lines higher.
  `tools/run_headless.py` now runs as part of `make test`, and both sites
  were re-broken one at a time to prove it catches each. See `testing.md`
  §18.9.


- **The disk is real, and what is written to it survives.** `mkfs --yes`
  formats, and a machine that has never seen the disk before finds a
  filesystem on it. virtio-blk in the HAL, three syscalls behind the
  strongest grant in the system, a disk server that is the only process
  holding it, and `kfs.lua` - the format, borrowed from ext2's skeleton with
  BFS's semantics and extents instead of indirect blocks. `make disktest`
  boots the machine twice against one image, because the question cannot be
  asked inside one boot.

- **A software 3D renderer.** `wm cube3d`: a solid, shaded, rotating cube at
  about 42 fps, drawn into a surface shared with the compositor. The split
  is the point - `surface:triangle` is the only new C, because it runs once
  per pixel; the matrices, the projection, the back-face test and the depth
  sort are all Lua in `/lib/g3d.lua`, because they run once per vertex. A
  cube is 12 triangles and tens of thousands of pixels, which is why that is
  not a close call.

- **Preemption preserves the whole FP register file.** The switch saved
  `d8`-`d15`, which is right for a thread that *called* it and wrong for one
  interrupted between two instructions. Silent wrong numbers, never a crash.
  A software renderer would have been the first thing to hit it constantly.

- **Shared-memory surfaces.** `sys.memory(pages)` makes a region, the
  capability travels in an ordinary message, and both sides map the same
  pages. An application draws straight into the surface the compositor
  composites from, so a frame costs one `commit` instead of a message per
  drawing command: `plasma` runs at 42-60 fps. The rules still hold - the
  pixels are behind `gfx.wrap`, so nothing in Lua ever holds one or computes
  its offset.

- **The Terminal serves its children without sleeping on them.** A window
  that is somebody's console cannot poll once a second: every child `write`
  blocks in `sys.call` until the window next wakes, so `ls` arrived at one
  line per second. `ui` has a `poll_wait` a window can lower; the Terminal
  sets it to one tick.

- **`make release` builds one image per resolution.** `builds/` carries a
  suffixed ELF for each of 1024x768, 1280x800 and 1920x1080, and
  `run-kosmos.sh -r WxH` picks one (`-r list` says which exist). Verified by
  booting all three and reading the geometry they report, which is how the
  first version was caught building the same image three times.

- **A Terminal.** `wm terminal`: type a program's name and it runs *in the
  window*, with its output going there instead of to the machine's console.
  It is a console server - it speaks the same `write` and `read` that
  `/dev/console` speaks and hands itself to its children under that name -
  which is the design working rather than a trick played on it. No program
  knows or can ask what is behind `/dev/console`.

- **PNG.** `gfx.png(bytes)` decodes into a surface, using `puff` - zlib's own
  reference inflate - vendored under `runtime/upstream/`. `ui.image` and the
  `photo` app draw one. The picture is *named*, not carried: an application
  sends `{op="image", asset=...}` and the compositor decodes it, because a
  decoded image is megabytes and a message is two kilobytes.
- **The compositor clips each window to the damage rectangle.** It redrew
  every window in full for every rectangle, so dragging got heavier with
  every window opened. That was the jerkiness.
- **The fine-mapped low region follows the image size** instead of being a
  hardcoded 2 MB. Adding a megabyte to the image used to panic in
  `mmu_init` about a stack guard, which is nowhere near the cause.

## Known bad

**One display phase fails about one run in three, and I do not know why.**
`check_widgets` clicks the gallery's list and presses Down through QEMU's
input plumbing; the selection has to move a row. It passes standalone every
time, and it passed two full runs out of the last four.

**Sharpened 2026-08-31, and it is wider than this one phase.** Six runs that
day failed three times, at *three different* phases: Control-C out of
`plasma`, the Down arrow twice, and Control-C out of the window manager.
Every one of them is the same sentence - an input the harness sent did not
arrive - and they are not one mechanism: the arrow goes through the monitor
socket and virtio-input, and Control-C goes down the serial line. So
whatever this is, it is not the monitor backlog on its own, and naming it
"the arrow key phase" was wrong. It is *input delivered while the guest is
busy*, by either road.

One thing changed that day which is worth holding against it rather than
forgetting: the context switch got 36% slower when it started saving the
whole FP register file, and IPC 17%. That would not create a race, but it
would change the odds of one that was already there. Nothing has been
measured either way, and repetition is not the way to measure it - the way
is to timestamp the send and the guest's receipt and find which side loses
it.

What has been ruled out, so nobody repeats it:

- **Not the arrow keys themselves.** They were genuinely broken - see below
  - and that is fixed and confirmed by hand. The phase failed before and
  after, so it is something else.
- **Not latency.** Twenty-five seconds of waiting does not help.
- **Not the shared monitor socket**, though that was worth fixing: `sendkey`
  and `screendump` share one connection and neither read their replies, so
  a backlog built up. Draining it made no difference to the failure rate.

What has not been ruled out: something in the preceding phases leaving the
console or the desktop in a state this one does not expect. Every phase runs
against the same boot.

**`make test` (109 tests) is unaffected and passes every run.** The display
harness should be treated as informative rather than as a gate until this is
understood, and the honest way to understand it is probably to make each
phase boot its own guest.

**(fixed) The arrow keys did nothing on a real keyboard.** This was written
up here as an intermittent test, which it was not: it was a real bug, and
the person using the system found it in ten seconds by pressing Down.

An arrow over a serial line is three bytes - escape, '[', a letter - because
that is what a terminal sends. On a keyboard it is a single keycode with no
character at all, and the driver's keymap has one byte per code, so
`keymap_plain[108]` was zero and the key produced nothing. Arrows worked
over the cable and did nothing in the window.

It hid because **every automated check typed over the serial line**, which
is the one path that already worked. The driver now turns those keys into
the sequence a terminal would have sent, so everything above it sees one
input language rather than two, and the widget check presses real keys
through QEMU's input plumbing.

The lesson is the one this project keeps relearning: a test that exercises a
different path from the user is a test that agrees with you.

**The display harness is flaky.** Two different phases have failed on two
consecutive full runs and both pass on their own. It is the harness, not the
system: applications now block for up to a second between events, so how
long a keystroke takes to show up depends on where in that second it landed,
and several phases still sleep a fixed time and then look. `check_widgets`
was already converted to wait-for-the-result and the rest have not been.

**Nothing should be pushed until that is fixed.** A suite that fails
differently each run tells you nothing, and the first thing it will hide is
a real regression.

**A file cannot be more than about a megabyte.** `fs.read` accumulates into
a Lua string and a process's heap is 2 MB by design (`design.md` 5.2, to
keep collections short). Reading a 936 KB PNG through `/share` gave
`error: not enough memory`. This is what killed the fw_cfg experiment and it
is the constraint the real filesystem has to answer - probably by mapping
pages rather than by returning strings.

- **Three apps.** `procs`, BeOS's ProcessController - every process with a
  bar beside it, busiest first. `about`, the About box, with the machine
  down the left and what the system is down the right. `sysmon`, the
  meters. All in `/bin`, all in the Deskbar.
- **A wrapping text view** in the kit, with a few styles and scrolling.
  Written for the About box; it is also most of what a markdown viewer
  needs.
- **The Deskbar sizes itself** to the number of applications. It was two
  fixed lists of seven rows, chosen when there were five.
- **A runner is named after what it runs.** They were all called "run", so
  `ps` and the process app showed a column of identical names.

- **A process can be ended from outside.** `SYS_KILL`, for a parent, which
  is the authority `wait` already implies. It marks and unblocks; the
  process dies at its own next entry into the kernel, which is at most one
  timer period away even for a program that has stopped making syscalls.
  Windows have a BeOS close box: the application is asked, and ended a
  second later if it never listened.
- **A Deskbar**, a graphical monitor, a version stamp on the desktop.
- **Applications block instead of polling.** The window manager parks a
  `poll` until there is an event or the caller's deadline - the same parked
  reply a live query uses.

**An idle desktop now idles: one per cent, down from ninety-six.** The
virtio input devices raise interrupts, `SYS_WAIT_INPUT` lets the one process
allowed to read input sleep until there is some, and the interrupt cuts that
sleep short - so a key is still noticed at interrupt speed. Applications
were already blocking, because the compositor parks their event polls.
There is a display check that reads the meter.

- **The Deskbar.** `wm` on its own starts a desktop with a panel top right:
  every window on screen, and every program that declared itself an
  application. Click one to start it; click a running one to raise it.
  It asks the window manager what is on screen rather than asking `/app`
  what registered, because a program that opens a window directly has one
  and no registration.
- **The cursor is composited** rather than drawn on the screen after the
  fact, and a window's drawing is damaged only once it is complete. Both
  were flicker: the first showed as the cursor blinking on every click, the
  second as a window seen half-redrawn.
- **`make FB=1920x1080` now rebuilds.** It did not: make compares
  timestamps and knows nothing about the command line, so it said "Nothing
  to be done" and ran the old image at the old size.

- **Clicks reach the widgets.** A press on a title bar is the window
  manager's; a press anywhere else is forwarded to the application in that
  window's coordinates, and the kit routes it to the view under it. Click to
  focus, click a list row, tick a checkbox, put the caret in a field. A
  button fires on the release and only if the pointer is still on it.

- **Graphical mode.** A process that owns the screen takes it from the
  kernel console with `sys.screen_take(true)`, and until it gives it back
  the console writes to the serial line only. `wm` and `edit` both do it.
  Nothing is silenced - the cable has everything, which is where a machine
  running a window manager is debugged from - and a panic takes the screen
  back regardless of who holds it.

- **A mouse.** `hal/qemu-virt/input.c` drives two virtio-input devices - a
  keyboard and a tablet - told apart by asking each whether it has absolute
  axes, since both answer to the same device id. The window manager draws a
  cursor, raises a window on click, and drags one by its title bar. M6's
  definition of done is now literal rather than done with arrow keys.

- **Replicants** - M7's second definition of done, minus the dragging.
  `wm clock,adopt`: one application publishes a view as source, state and
  a `needs` list, and another, which has never heard of clocks, adopts it
  and runs it. Both tick, with different state. The replicant reports from
  *inside* its own environment what it could reach, which is the only
  honest place to ask.

- **The scripting architecture** - M7's third definition of done. An
  application registers with `/app` and answers for its own properties
  because it called `ui.window`, not because it has any scripting code:
  `apps`, `apps gallery`, and `setprop /app/gallery/title=...` renames a
  running window. The registry hands out capabilities and never forwards, so
  one slow application cannot hold everyone else's door.

- **The UI kit.** `lib/ui.lua`: a view tree with nested coordinates and real
  clipping, follow modes, and widgets - label, button, checkbox, field,
  list. `wm gallery` shows all of them. Drawing produces commands, never
  pixels, so a view's list can be resent without re-running its handler.
- **`/lib` and `use()`.** A library is a file in the process's namespace,
  loaded into the caller's own environment. No package path, no search, no
  global module table: a program that was not given /lib has none.
- **The window manager reserves one key**, Control-W, and it introduces a
  command. It used to take Tab and the arrows, which the gallery showed to
  be untenable in one screenshot.

- **`edit`, a screen editor.** The machine can write and run its own Lua
  without a rebuild. `edit /ramfs/x.lua`, Control-S, Control-Q, then
  `run /ramfs/x.lua`.

- **The scheduler was costing every yield a timer period.** The idle loop
  slept with runnable threads in the queue. A yield went from 10 ms to
  0.04 ms and an IPC round trip from 20 ms to 0.8 ms - about a thousandfold,
  system-wide, on something no benchmark could see. See `testing.md`.
- **The window manager.** `wm hello-win,stuck`: two applications, one hung,
  and the hung one's window still moves. That is M6's definition of done.
  Backbuffer, damage tracking, BeOS tabs, stacking, focus.
- **Attributes, an index over them, and live queries.** `attr`, `find`,
  `watch`. A watcher blocks in one call and the filesystem parks the reply
  until the answer changes - no timer, no repeated question. `qbench` shows
  the query cost flat against sixteen times the nodes.
- **Control-C**, and the non-blocking receive a single-threaded server needed
  in order to answer it.

- **`/bin` is a real program directory.** `htop`, `cat`, `ls`, `monitor`,
  `hello`, `benchmark`, `spin`. Typing a name that is not already a Lua name
  runs the program; a trailing `&` detaches it.
- **`monitor` redraws on its own clock**, once a second, from a detached
  process. Every earlier version did not, and none of them could be told
  apart over serial - so there is now a display check for it.
- **The Lua is checked at build time.** `tools/luacheck.c` parses every file
  and `tools/luaglobals.py` compares the globals each one reads against the
  environment it will run in. That second one exists because a lost `local`
  has killed a server four separate times, and it catches exactly that.
- **`docs/architecture.md`**, the layer diagram and one command traced from
  the keypress to the pixels.

## Working

`make qemu`, `make test`, `make bench`, `make bench-record`, `make debug`, `make disasm`, `make size`, `make clean`.

109 tests, five benchmarks, and 56 display checks. A 340 KB image, of which 232 KB is the userland carried inside it and 20 KB is the kernel's own machine code. Plus 3.2 MB of framebuffer, which is `.bss`-like and costs the file nothing.

`make qemu` opens a window and keeps the shell on the terminal. `make serial` is the old serial-only behaviour, for when there is no screen to open.

**The prompt is a process.** What you type is read by the console server, sent to the shell over IPC, evaluated in the shell's own `lua_State`, and printed back the same way.

```
Kosmos shell. A process, talking to servers.
Try: fs.list("/ramfs")   fs.read("/ramfs/sensor")   2+2

kosmos> 2+2
4
kosmos> fs.write("/ramfs/sensor", { celsius = 47.2, unit = "C" })
true	nil
kosmos> fs.read("/ramfs/sensor").celsius
47.2
kosmos> fs.list("/ramfs")[1]
sensor
kosmos> sys.write("direct")
-102
kosmos> fs.read("/nowhere")
nil	no such path: /nowhere
```

`sys.write` returning −102 is the point: the shell **cannot** print directly. Only the console server holds the serial port, and everything else has to ask it.

## M6 — where it stands

- [x] A framebuffer under QEMU, via ramfb: 1024x768, XRGB8888
- [x] `hal_fb_init` in the HAL, and a boot splash that proves the display at every boot
- [x] `make screenshot`: boot, screendump through QEMU's monitor, check the picture
- [x] `gfx.surface` as a userdata over flat bytes, and the C primitive set
- [x] Explicit `free`, a `__gc` net, and telling the GC the real size
- [x] The screen reachable from a process, as a surface
- [x] A backbuffer and damage tracking
- [x] An 8x16 bitmap font
- [x] A narrated boot with a progress bar, on the screen and on the serial line
- [x] The shell visible on the screen, and `help` at the prompt
- [x] The app server in Lua: windows, decoration, stacking, focus
- [x] Input beyond the serial line — **virtio-input over virtio-mmio.** `virt` has 32 virtio-mmio transports at 0xa000000, stride 0x200, SPI 16 upward, and `virtio-keyboard-device` attaches to that bus. mmio rather than PCI is the whole point: no ECAM walk and no capability parsing, so it is a few fixed registers and one virtqueue — and the same transport then gives `virtio-gpu-device`, which is where real dirty-rectangle flush and vblank come from. The keyboard pays for the GPU.
- [x] **Definition of done: drag a window with a hung app inside it, and have the window keep moving smoothly** — `wm hello-win,stuck`

**ramfb, not virtio-gpu, and the order is deliberate.** `hal_fb_init` is "ask the firmware for a linear framebuffer, and let it say where the pixels are", which is exactly what QEMU's ramfb and the Pi's mailbox both do. virtio-gpu is the odd one out — it needs an explicit `RESOURCE_FLUSH` after drawing — so it is the target that will earn the interface a `hal_fb_flush`, with two implementations in front of it rather than one. That is `hal.md`'s own argument applied to the display. It also cost about a hundred lines against the eight hundred that PCI enumeration plus virtqueues plus the virtio-gpu command set would have cost before a single pixel appeared, and everything above the HAL is identical either way.

**What ramfb does not give:** dirty rectangles and a vblank. QEMU rescans the whole buffer on its own schedule, so damage tracking in the compositor still saves the drawing but cannot save the scanout. Under emulation neither is the bottleneck. virtio-gpu is where both come back.

**The stride is padded on purpose: 4160 bytes, not 4096.** ramfb lets the guest choose, so it could be the tidy value, and that is the reason not to. A framebuffer whose pitch equals `width * 4` lets every address calculation in the system be written wrong and still work — for months, until the first real board, where the firmware picks whatever alignment it likes and every one of them shears at once. Two tests assert the padding, so that removing it as an oddity fails loudly.

**Two halves of the display are tested, and neither can prove the other.** `make test` proves what the kernel wrote into its own memory: that the framebuffer exists, is page aligned, is writable to the last row, and that the padded stride really moves rows. It cannot prove a pixel ever reached a screen — a wrong fourcc, a wrong stride in the ramfb config or a wrong address would leave all six passing and the display black. `make screenshot` asks QEMU instead, through the monitor, on the far side of everything this kernel controls. Both were made to fail on purpose before being trusted.

**Lua draws, and no line of it computes a pixel offset.** `gfx.surface{w=,h=}` is a userdata over flat bytes with `fill`, `span`, `blit`, `blend`, `get` and `set`; every pixel loop is in `user/lib/gfx.c` and every primitive clips rather than raising, because a window half off the edge of the screen is the normal case. `gfx.screen()` is the framebuffer as a surface, for the one process that was handed it.

**The boot log says what each stage is *for*, in one sentence.** Twelve stages: a line saying why the stage exists, then the facts it found. A log that prints "physical memory" and a number teaches nothing to somebody who does not already know why an operating system needs a page allocator before it can build a page table.

The first version of this used four to six lines a stage and was worse, not better — it filled the screen twice over and read like a lecture. One sentence and the numbers is the shape: it fits in 44 lines, which is one under what the screen holds. The longer explanations live in the source, which is where somebody who wants them is already looking.

**Backspace drew a box on the screen and worked perfectly over serial.** The console server's line editing was right all along — it sends `\b \b`, which serial terminals have understood since teletypes. The kernel's screen sink had never been told: `\b` is not printable, so it fell through to the glyph blitter, landed outside `0x20..0x7e`, and came out as the font's "no such glyph" box. Every correction while typing left a row of them. The erasing is still the space's job, not the backspace's; doing it in both places would delete the character before the one being deleted.

**The processor identifies itself**, in `arch/aarch64/cpu.c` — which is the most literal possible reading of what `arch/` is for. MIDR, the cache geometry, the physical address range and the ISA feature bits, all out of registers the architecture requires every AArch64 core to implement, so it needs no board knowledge and works on the first boot of new hardware. Part numbers from `arch/arm64/include/asm/cputype.h`, field positions from `arch/arm64/tools/sysreg` — Linux's machine-readable register description, not memory. **The raw registers are printed beside the decode**, everywhere, because a table of part numbers goes stale the moment a part ships that is not in it and a reader who can see `MIDR_EL1` can look it up.

**`/dev` is a server**, reached through the namespace over the same `list`/`read` protocol the filesystem answers. `fs.read("/dev/cpu")` is not a special case anywhere; it is an ordinary request sent somewhere else. `SYS_SYSINFO` hands back **raw** ID registers and pool counts and decodes nothing — the tables that turn `0x410fd083` into "Cortex-A72" live in Lua, so a processor the kernel has never heard of is described properly without the kernel changing.

**The device server does not list `console`, and that is the point.** The machine has one, but `/dev/console` is mounted to the console *server* — longest prefix wins — so a read of that path means "give me a line of input". The first version listed it anyway; the `devices` command dutifully read every name it was given, and the console server answered by swallowing the next thing typed at the prompt. Listing a name you do not answer for is a lie, and that is what it costs.

**A command can be a Lua program.** `alias` points one word at another; `def` compiles a line of Lua and gives it a name, argument string arriving as `...`. It is compiled at definition time, into the same environment as the prompt, so a syntax error is reported when you write it and it reaches exactly what you reach. That is the shape the idea deserved: an alias that is only a second name for a command is a convenience, and a command that is a program is a way to extend the system from inside it.

**Programs launch programs, and the shell has stopped being where they live.**

    kosmos> ls /bin
      benchmark.lua  cat.lua  hello.lua  htop.lua  spin.lua

`benchmark` and `cat` were shell commands and are programs now. `benchmark` launches `spin` — a program in /bin starting another program in /bin, with no special case anywhere: `run` is a function the runner hands its program, and it can pass on no more capabilities than it holds itself.

**`detach` is the difference between `run` and `benchmark`.** Detached, the child answers as soon as it has been told what to run and gets on with it, so "start four of these" does not mean "run four of these one at a time". Undetached, the answer comes when the program is finished, which is what a command line wants.

**The endpoint leak is closed.** `SYS_ENDPOINT_DESTROY` exposes what the kernel could already do and nothing could ask for: every program run consumed one of ninety-six for ever. No permission check beyond the capability itself — the index resolves against the caller's own table, so a process can only destroy one it was given.

**The working directory travels with a program.** It is the shell's idea and no server knows about it, so it goes in the request rather than being asked for. `cd /ramfs` then `cat notes` works, and `cat` is a program that has never heard of the shell.

**A slice-based edit silently deleted `run_program` for the third time.** The first cost nine test functions, the second cost `tests_run()` and looked like a hang, and this one left the shell calling a function that no longer existed — which killed the shell, silently, because the shell prints by asking the console server. It is written down here three times now; the rule is narrow anchors, and I keep not following it.

That failure did surface a real bug worth keeping: the bare-word program path was not wrapped in `pcall` the way the command path is, so a program that failed to *start* took the shell down. A program that fails while *running* was always isolated — it is its own process — but starting one happens in the shell.

**There is a `/bin`, and programs run in processes of their own.**

    kosmos> ls /bin
      hello.lua        704 bytes
      htop.lua        3890 bytes
    kosmos> htop

`htop` is a Lua program in `user/bin/`, carried in the image because there is no disk until M8, served by a read-only `/bin` server, and run by a `runner` process that gets the capabilities the shell chose to hand it. It shows itself in its own process table.

**This is what `exec` looks like with no ambient authority.** No path search, no inherited environment, no global tree: a program reaches exactly what it was given. A bare word runs a program only if it does not already name something in Lua — the same rule the command dispatcher uses — so nothing installed in `/bin` can shadow the language.

**The shell sends a name, not the source.** The first version sent the program in the message and could not: a program is several kilobytes and `MSG_BYTES` is 2048. Sending the name is better than making it fit — the shell no longer reads a program in order to start one, and the bytes cross the boundary once instead of twice.

**Reads can now span messages.** `MSG_BYTES` stayed at 2048 rather than growing, because `struct thread` embeds a message — every thread would pay — and `sys_call` keeps one on a 16 KB exception stack. A server holding something large answers with `more = true` and honours `offset`; one that does not ignores the field, as every existing server does.

**And `serve` no longer dies when a reply will not fit.** `sys.reply` raises on a value that does not serialise, and that call is *outside* the coroutine that isolates a handler — so the first time `/bin` was asked for a program bigger than a message, the program store died and the client saw only that its request never came back. The failure now reaches whoever asked.

**The pools were raised, and doing it found a limit nothing could see.** Threads 16 to 48, processes 8 to 32, endpoints 32 to 96. Raising the first two changed nothing: spawning still failed at eleven processes, with every pool the system could report showing plenty free — 16 of 32 processes, 17 of 48 threads, 469 MB.

The real ceiling was **`ADDRSPACE_MAX` in `arch/aarch64/mmu.c`**, a third pool nothing counted and no report mentioned. A limit nothing counts is a limit nobody can find. It is 32 now, `as_count()`/`as_total()` exist, `SYS_SYSINFO` reports them and `ps` prints them; the same spawn loop reaches 27. And because `arch/` must not include a kernel header to learn `PROCESS_MAX`, the two are tied together by a test that creates that many address spaces and fails if any is refused.

Costs, measured: 134 KB of `.bss` for all three pools, up from 44 KB. A process is ~2.3 MB of RAM when it exists, dominated by its 2 MB heap — the same 2 MB that stops a full-screen surface fitting. Both get solved by the same change.

**Known gap found while testing this:** processes spawned from the shell are never reaped, because the shell never calls `sys.wait`. init reaps its own children; nobody reaps the shell's. They hold their slots as zombies until reboot.

**A blinking cursor**, driven from the timer tick — the one thing on the screen that has to change without anybody printing. Every path that writes a cell hides it first, so the block is never left sitting on top of a character somebody just printed, and it follows `cx`/`cy` rather than remembering where it was, so scrolling does not leave a second cursor behind.

**The namespace has a root now, and it is the one thing no server can answer.** `ls /` used to say "no such path" while `/ramfs` and `/dev` both plainly existed — because nothing is mounted at `/`, and a path with no server behind it does not resolve. A server knows what it holds; only the namespace knows what has been *attached* to it and where, and that table lives in the process.

So `ns.list` returns whatever the server said **plus** whatever is mounted below the path, both being true: `/dev` holds `cpu` because the device server says so, and holds `console` because something else was attached there. A path with no server but with mounts under it — which is exactly what `/` is — is a directory made entirely of mount points.

Worth being clear about what this is not: there is a filesystem, and it is `servers`-in-memory. The ramfs at `/ramfs` is a real server answering the real protocol; what M8 adds is persistence, attributes and queries, not the idea.

**`ls`, `cd`, `pwd`, `cat`, and a working directory that lives in the shell.** Not in the kernel and not in a server: a server is always told a whole path and knows nothing about where anybody thinks they are, which is what keeps `fs.read` the same operation for every caller. And "is this a directory" is answered by asking whether whoever serves it will list it — the only definition that means anything across three different servers.

**A leading slash always means a command**, and that came out of a question worth recording: aliases and Lua names can collide. `/ps` is unambiguous; a bare `ps` is treated as a command only when it does not also name something in Lua. So `type` gives you Lua's function and `/type` runs the alias you gave that name. Refusing to guess is the point — a shell where `type` sometimes means a command and sometimes means the function is a shell you cannot write anything in.

**CPU usage is the difference between two readings, never one.** The kernel charges every timer tick to the idle thread or to everything else, and both counters only rise. A single reading says what fraction of *all time since boot* was busy, which after a minute at a prompt is a number that never moves again. `ps` says so on the first call rather than printing a meaningless 0%.

Sampling at the tick rather than accumulating real time per thread is deliberate: accumulating would mean reading the counter twice on every context switch — a cost on the hottest path in the kernel to answer a question nobody asks more than once a second.

**The shell takes commands as well as Lua**, with aliases. A line is a command when its first word names one *and the rest has no Lua punctuation in it* — so `devices` and `devices all` are commands while `devices("x")` stays an expression. **`help` is the exception that this sentence used to get wrong**: it also names a value in the environment, so it is always Lua - `help "gfx"` and `/help gfx` work and `help gfx` is a syntax error.

**A status bar, in the rows the kernel console reserves.** Two writers on one framebuffer with no compositor, which is only honest because the regions cannot overlap by construction — and is exactly the arrangement a compositor exists to stop needing.

**There is a keyboard.** virtio-input over virtio-mmio, in `hal/qemu-virt/keyboard.c`. mmio rather than pci is what makes it three hundred lines instead of nine hundred: `virt` has 32 fixed windows at 0xa000000, stride 0x200, and the whole of discovery is reading two registers thirty-two times. No PCI bus, no ECAM walk, no capability list.

**It is not a new HAL call.** A keyboard is a source of characters and `hal_getchar` is where characters come from, so the board answers from whichever of its sources has one. The console server, the shell and every process reading a line are unchanged by the keyboard existing — which is the property that says the HAL boundary was drawn in the right place.

**QEMU's virtio-mmio defaults to the legacy interface**, and this cost a debugging round. The device was found in slot 31 with the right magic and the right device id, reporting version 1 — the legacy layout, reached through `QUEUE_PFN` and `GUEST_PAGE_SIZE`, which modern structures read as garbage. The driver refused it, correctly, and the boot said "none". `-global virtio-mmio.force-legacy=false` is what Linux passes too, and it is now on every QEMU line here including both test runners. Worth knowing: the failure looked like "the device is not there" and was actually "the device is speaking the other dialect".

**Polled, not interrupt-driven**, like the UART. The console server already yields between polls, so a key in the used ring is found on the same schedule a character in the UART is. `roadmap.md`'s input-on-a-highest-priority-thread is when the interrupt starts to matter.

**And the next device is nearly free.** Everything above the last two functions in that file is the virtio transport, and `virtio-gpu-device` is the same transport with a different id — which is where a real dirty-rectangle flush and a vblank come from. It is not split into its own file yet, because there is one device and splitting it now would be inventing an interface against a single caller.

**The boot narrates itself, on the screen and on the wire.** Ten numbered stages, each with the facts that make it worth watching, and a progress bar in rows the text never scrolls through. `CLAUDE.md` says the kernel has no graphics, and it still does not have a graphics *subsystem* — what `console.c` gained is forty lines that put a glyph in a framebuffer, and the reason is `panic()`: it writes through the same console, so a panic now reaches a screen. On a board with no serial cable, a panic that prints into the void and a machine that does not work are the same thing.

`BOOT_STAGES` is a constant and the `boot_stage` calls are scattered through `kmain`, so there is a test that they still agree — a bar that stops at four fifths reads as "something hung" rather than as "somebody added a stage", and the screenshot check catches the same drift from outside.

**Keystrokes typed during boot are still lost.** Nothing polls either input source until the console server starts, and there is no input buffer. The keyboard did not change this — its ring holds sixteen events and the boot produces none, but nothing reads them until userland is up. Harmless with a person at the keyboard; it cost twenty minutes when a test harness typed too early.

**The pre-display boot log is replayed onto the screen.** The display cannot be the first thing up — it needs the MMU on first, or the framebuffer is Device memory and clearing three megabytes of it is the 10-50× penalty `gfx.md` §19.5 warns about. So the first stages happen before there is anywhere to draw them. An earlier comment argued against buffering them on the grounds that such a buffer could overflow during a panic; that was simply wrong, since nothing writes to it once the screen is attached. Two kilobytes, written before there is a second thread, replayed once.

**`help` at the prompt**, with `help("fs")`, `help("gfx")`, `help("sys")` and `help("demos")`. A table with `__tostring` and `__call`, so the bare word works as well as the call — the parentheses are the thing every newcomer forgets.

**init says which child died, by name.** It used to record the exit code into a local and drop it, on the reasoning that the console server might be the thing that just died. True, and still no reason to say nothing: a process that dies takes its own error message with it, because it prints by asking the console server and a dead process asks nothing. init is the only one left who knows.

**A slice-based rewrite of `kmain` silently deleted `tests_run()` and `bench_run()`.** The symptom was not a failure. It was `make test` appearing to hang — the image booted perfectly into a shell while the host runner sat waiting for a TAP plan that was never coming, which reads as "the boot got slow" and sent me looking at the console's scrolling for half an hour. **Second time an edit by slice has quietly dropped lines from the middle of something**; the first cost nine test functions. Narrow anchors, always.

**There is text.** Spleen 8x16, BSD-2-Clause, vendored unmodified under `assets/fonts/` beside its licence and converted by `tools/bdf2c.py` into 96 glyphs of sixteen bytes — one byte per row, MSB leftmost, which is the VGA ROM layout and is what makes drawing a glyph a shift and a test rather than a lookup. `s:text(x, y, string, colour [, background])` returns the next x, so laying out a line needs no arithmetic about pixels in Lua.

Two choices worth recording. **Linux's `font_8x16.c` was rejected**: it is the obvious thing to reach for, it is the same VGA font, and it is GPL — it would have made the kernel image GPL. And **the generated file prints each byte beside the row it draws**, because a shifted bit, a reversed row order or an off-by-one in the range are all obvious read pairwise and all subtle on a screen. Five of those were introduced deliberately and all five failed the tests.

The tests compare rendered pixels against patterns written out by hand rather than against the array that drew them — comparing the output to its own input would pass just as happily with the bits reversed.

**A pitch bug is invisible from inside the process, and that is the most useful thing found this session.** If `row_of` steps by `width * 4` instead of by the pitch, every read agrees with every write — the surface just has an unused gap at the end of each row — and *the whole suite passes, 103 of 103*. It was tried, not reasoned about. The only observer who disagrees is on the far side of the framebuffer, where the stride is 4160 and a row written 4096 bytes along lands sixteen pixels left.

So the check for the rule this whole module exists to enforce cannot live in the guest. `make screenshot` grew a second phase: it waits for the shell prompt, types a `gfx.screen()` drawing of vertical bars, screendumps, and requires them to still be vertical. With the bug in place it reports the bar "found at x=184..189" instead of 200 — the drift, exactly.

**Surfaces come from the process heap, so a full-screen one does not fit.** 1024x768 is 3.2 MB against a 2 MB heap, and `gfx.surface` says so rather than failing obscurely. That is a real limit and it has to be solved inside M6, not at M7: the app server's backbuffer is full-screen by definition. It needs pages from the kernel rather than from the Lua heap, which is the same mechanism M7's shared surfaces want.

**The `serialize` benchmark moved twice this session, and neither time was the serialiser.** +2.2% when `gfx` started being opened in every process, and +0.9% again when the font array joined it — 1364.9 to 1412.5, with `serialize.c` untouched throughout. Every process now opens the `gfx` library, and the library table, the surface metatable and its methods are more objects on a 2 MB heap, so the collector paces differently through the measured loop. Measured rather than guessed: the same tree with the `luaL_requiref` for `gfx` commented out gives 1358.2, back inside the old range.

Worth knowing as a property of the metric — **`serialize` is sensitive to what else is in the process**, not only to the serialiser, and it drifts every time anything joins the user image. If it moves and nothing in `serialize.c` or `sys_user.c` did, look at what was added. Making it independent of its process — a fixed GC pause setting and a forced collection before the loop — is a known improvement and is not done; it is written down here rather than left to be rediscovered the third time this happens. `gc_pause_max` shifted by the same cause and only +0.09%, because a long collector step is dominated by the 3000-object heap the benchmark builds rather than by a handful of library tables.

**init says why it could not start something.** Its spawns used to be `if not x then sys.exit(1) end`, and the system died at boot in total silence — kernel output looking perfectly healthy, then nothing. That is exactly what happened the first time `SPAWN_SCREEN` was refused, and it cost a debugging cycle to find. init holds the console precisely so it can speak, and the moment it most needs to is when it cannot build the system.

**The framebuffer is in its own linker section, after the stacks.** Three megabytes in `.bss` would sit before them and push both guard pages past the first 2 MB of RAM — the only part mapped a page at a time — and a guard page inside a 2 MB block cannot be punched out, so `mmu_init` would panic. `NOLOAD`, so the image file carries none of it; inside `__image_end`, so the page allocator counts the pages as the kernel's and never hands them out. There is a test for that last part, because the symptom otherwise is garbage on screen rather than anything that looks like an allocator bug.

## M5 — where it stands

- [x] The protocol: `list`, `read`, `write`, `getattr`, `setattr`, over typed records
- [x] Per-process namespaces: a mount table in the process, so what is not mounted does not exist
- [x] Lua coroutines as the servers' concurrency layer
- [x] Console server, owning the device
- [x] ramfs
- [x] Capability transfer over IPC, so userland can do its own mounting
- [x] The Lua shell as a REPL against the system
- [x] **Definition of done, both halves: the same server mounted at two paths in two processes, each seeing only its own; and the console server's code replaced while the shell was talking to it, without the shell noticing**
- [x] Hot reload level 1: `load()` of new code while preserving state and clients
- [x] Init and supervision in userland, on a spawn syscall
- [x] Removing Lua from the kernel

**The kernel starts init and nothing else.** init spawns the console server, the ramfs and the shell, passing on the capabilities it was given, and then waits. It cannot promote a child beyond itself: a spawn resolves every capability against the parent's own table, and passing on the console is refused unless the parent holds it.

**Supervision is noticing, not restarting.** init waits and learns which child ended with what code. Restarting one is hot reload level 2, and `design.md` §10 deliberately leaves that until there is state worth recovering — the server would come back empty, and deciding where its state should have lived is the actual design question.

**There is no Lua in the kernel.** Not in the boot path, not in the image, not in the source list. `CLAUDE.md` has said since the start that the kernel has none from M4 onward, and until this session that was simply untrue: the interpreter was 163,648 bytes of `.text` against 28,556 for the entire rest of the kernel, reachable from nothing, kept alive only because fifty test assertions drove the kernel through it.

Those assertions live in `user/tests/luatest.lua` now, one role per test, and `tests/tests.c` starts a process and turns its exit code into a TAP line — so the plan, the numbering and the names all stayed where they were. The two benchmarks that opened a `lua_State` moved the same way, into `user/tests/luabench.lua`.

What left with it: `malloc`, `math`, `snprintf`, `strtod`, `stdio` and `-lm`, every one of which was in the kernel for Lua and for nothing else. And the 2 MB heap `kmain` allocated for a `lua_State` it opened itself.

**Kernel machine code went from 204,800 bytes of `.text` to 20,480.** The whole image is 348,168 bytes against 569,364, and 237,579 of what is left is the userland image carried inside it — payload, not kernel.

Two fixture blobs went with it. `user/hello.S` and `user/faulty.S` are what the tests run at EL0 to check that a process exits and that a null dereference kills only it; nothing outside the suite referred to them, and they were 4 KB of the shipping image that no code path could reach.

**What stays despite being unreachable in the shipping image:** `fault_expect_begin`/`_end` in `arch/aarch64/trap.c`, and `setjmp.S` under it. Only the tests and the benchmarks call them. Compiling them out would mean the trap handler that ships is not the trap handler that was tested, and that is a worse trade than a couple of hundred bytes and one predictable branch on a path that is already an exception.

**Hot reload works because state and behaviour are separate things.** A server is a `state` table plus a factory that turns it into handlers, and `serve` takes the factory rather than the handlers. Anything captured in a closure built at startup is lost on reload; anything in `state` survives, because the new handlers are handed the same table. That is the whole mechanism, and getting it wrong is silent: the server keeps working and quietly forgets.

**A reload that does not compile is refused with the old code still serving.** Load, run, and install, in that order, each checked. A server that half-reloads is worse than one that refuses.

**Device access is a boolean, not a capability.** `process_grant_console` sets a flag, and the flag is checked by `sys.write` and `sys.getchar`. It enforces the property that matters — exactly one process owns the console and everything else asks it — but a device should be named the way everything else is, by a capability the process holds. That needs a capability that names a device rather than an endpoint.

## M4 — where it stands

- [x] One `lua_State` per process, with a bounded heap (2 MB, mapped by the kernel and not growable)
- [x] The process running at EL0, in its own address space
- [x] Syscall bindings validating capabilities, and every pointer checked before it is touched
- [x] The Lua table serialiser for IPC
- [x] Deciding which Lua libraries exist inside a process
- [x] Loading Lua code from an image embedded in the kernel
- [x] **Definition of done: two processes at EL0, separate address spaces, exchanging a Lua table. And `*(nil)` kills only the process.**
- [x] Removing Lua from the kernel — done at M5, see above
- [ ] Benchmarks: allocating and freeing a table; a syscall from Lua versus the same one from C

**Two things are deliberately temporary, and both are recorded where they are written:**

*The kernel is in TTBR0, alongside every process.* `state.md` previously said M4 would move it to TTBR1. It does not, because that is a large refactor of boot, the linker script and every kernel pointer, and it is not what buys isolation. Isolation is the AP bits: every kernel mapping is `AP=00`, EL1 read/write and no EL0 access, with PXN and UXN set. A process faulting on the kernel image gets a **permission** fault, not a translation fault — the page is in its own tables and still untouchable. TTBR1 becomes worth doing when a process needs the whole low half.

*The reply token is a raw kernel pointer.* `sys.receive` hands EL0 the address of a `struct thread`. It is safe only because `ipc_reply` checks the target is really waiting, and a value that is safe only because of what the callee checks is one audit away from not being safe. At M5 it becomes a capability index like everything else.

## M3 — done

- [x] Threads with their context, and a context switch in assembly
- [x] Round-robin scheduler with a per-CPU runqueue, behind a pluggable `struct scheduler`
- [x] Synchronous IPC: rendezvous, call, receive, reply
- [x] A per-thread capability table, indexed, with generation numbers
- [x] A separate exception stack, so a stack overflow is readable rather than a double fault
- [x] **Definition of done: 100,000 round trips, cost per round trip printed and baselined**
- [x] Address spaces: create, destroy, map pages
- [x] Syscalls exposed as Lua functions, as `sys`
- [x] Preemption, in the vector's epilogue

**One thing is deliberately temporary.** Every address space contains the kernel, because there is no TTBR1 split yet: the kernel is identity mapped through TTBR0 like everything else, so a space without it would fault on the instruction after the switch. A new space copies the kernel's top level and shares the tables below it, which is why user mappings are confined to virtual addresses at or above 2 GB and anything lower is refused rather than allowed to quietly edit the kernel's own map.

At M4 that is replaced by the real arrangement: the kernel in TTBR1 at the top, TTBR0 belonging entirely to the process, and a space containing no kernel at all.

## Decisions taken while implementing (this session)

**2026-08-31 — The disk goes to one process, and everything else asks it.** Raw sectors are every file on the machine whatever any namespace says, which makes `SPAWN_DISK` a stronger grant than the screen - the screen can only draw. So the kernel hands it to init, init hands it to exactly one child, and `mkfs` and `diskinfo` are ordinary programs that reach `/disk/super` and `/disk/format` through their namespace and cannot touch a sector.

Doing it the other way was tried first and refused itself: granting the runner `SPAWN_DISK` would have given it to every program, which is ambient authority with extra steps. The version that works is the shape `/dev`, `/bin` and `/lib` already have.

Block *contents* deliberately do not cross that boundary. A message is 2048 bytes and a block is 4096.

**2026-08-31 — The format is borrowed, not invented.** ext2's skeleton without its block groups, which exist to keep inodes near data on a spinning disk and buy nothing on an SD card. ext3's journal, whose space is reserved from the start so turning it on does not move the data blocks. BFS's attribute semantics. Extents instead of indirect blocks. Written with `string.pack`, so the format string beside the field names is the specification - and bounds-checked, which a C struct read off a disk is not. A corrupt superblock is exactly the hostile input a filesystem has to survive, and `<I4I4I8` also cannot acquire padding on the way to the Pi.

**2026-08-31 — The superblock is written last, and that is the ordering rather than a detail.** A format interrupted before it leaves a disk that says it is not a filesystem, which is true. Interrupted after, it would leave one that claims to be and is not.

**2026-08-31 — `readline` cannot be used to wait for this system's prompt.** `kosmos>` has no trailing newline, so `select` reports the pipe ready and the read then blocks for ever waiting for a line ending that only arrives when something else is printed. It cost an hour in a new harness, and `run_screenshot.py` had already solved it by reading raw bytes - which is worth knowing before writing the third one.

**2026-08-31 — A preempted thread needs the whole FP register file, and the switch is the place to save it.** `context_switch` saved `d8`-`d15`: the callee-saved set, and the correct answer for a thread that called it, because the compiler has already spilled the rest. A preempted thread called nothing. Two kernel threads holding a known value in `d0` across a spin longer than a quantum lost it on **14 preemptions out of 14**; after the fix, 0 out of 14.

Saved in the switch rather than on the exception path, because the kernel's own C is `-mgeneral-regs-only` and cannot touch an FP register - so by the time the switch runs the values are still exactly what the interrupted thread left, and a switch is far rarer than an exception. Full 128-bit `q` registers plus `fpcr`/`fpsr`: userland is not built `-mgeneral-regs-only`, so saving only what Lua's arithmetic obviously needs would be the same bug one layer down.

It costs `context_switch` 7.187 -> 9.812 and `ipc_roundtrip` 31.001 -> 36.251, both recorded rather than hidden. Lazy FP save is on the roadmap at M10 to get it back, with the caveat that it wins on switches between threads that do not touch FP and nearly every thread here runs Lua.

**The first version of that test passed with the bug.** Its spin was shorter than a 100 ms quantum, so it was never actually preempted. It now asserts the preemptions happened, and the same lesson as the arrow keys applies: a test that does not exercise the path is a test that agrees with you.

**2026-08-31 — Face winding is computed, not written by hand.** The first `g3d.cube` had four of its six faces wound the wrong way. On screen that is a cube showing *four* faces at once, which a cube cannot do - and the version where all six are backwards shows three, rotates, shades correctly, and is inside out. Neither is visible in a screenshot.

So `g3d.orient` derives it: for each triangle the right-handed normal either points toward the centre of the mesh or away, and those are exactly the two windings. The faces list only has to name the right corners. Exact for a convex mesh centred on the origin, which is stated where it is defined, because a teapot is neither.

The test that pins it is not the display check. A display check sees three faces whether the near ones or the far ones are drawn; the Lua test puts the cube in a known pose and asserts *which* face is in the middle of the picture, then turns it half a turn and asserts the opposite one is. Reverting the cull sign makes it report the far face's colour by name.

**2026-08-31 — A shared region is mapped in its own address window, because a range is what says who frees the pages.** Shared regions went into the window `SYS_MAP` hands out. A process frees everything still mapped in that window when it exits, on the grounds that it allocated it - so two processes mapping one region freed its pages twice, and the second one panicked the machine with `pmm_free_page: double free`. Opening `plasma` and pressing Control-C was enough.

The panic was the lucky half. The quiet half is that `SYS_UNMAP` would have let a process hand a region's pages back to the allocator while the other process was still drawing into them: a use-after-free with no symptom at all until the pages were handed to somebody else.

The fix is a second window, `USER_SHARE_VA`, and not a per-page owner flag. The two pieces of code that free pages by walking a range are now bounded by the window whose pages they own, and the check is where the addresses come from rather than a bit they carry. Both windows have a ceiling now as well: neither pointer reuses an address, so without one the `SYS_MAP` pointer would eventually walk into the shared window and bring the same bug back by a longer road.

Shared pages are also not charged against the mapper's budget. They are charged to whoever created the region; charging them again to everybody who maps it would mean a compositor and an application sharing one surface pay for it twice.

There is a permanent test, and the page count is checked from C rather than from inside either process - the number only becomes true once both have been reaped, and neither of them is around to look. It fails by panicking when the fix is reverted, which is how it was confirmed to test anything.

**2026-08-31 — A capability slot with a kind costs one tick per round trip, and that is the price of the design.** `ipc_roundtrip` went 30.000 to 31.001. A slot used to hold an endpoint and nothing else, so `resolve` went straight to the pointer; it holds either an endpoint or a region now, so every resolve loads the tag first and a round trip resolves twice. `context_switch` and `exception` did not move at all across the same change, which is what says the cost is in the lookup and not in the path around it. The alternative - a second table with a second index space - is the global-name design this kernel does not have. Baseline raised, with the reason in it.

`serialize` moved 1412.5 to 1450.6 in the same change, and its baseline note had already predicted the shape of it: the benchmark is sensitive to what else is in the process, and this added four bindings to every one of them.

## Concrete next step

**A PDF reader, then sound, then Doom.** Decided by hand rather than taken
from `roadmap.md`, which still schedules M9's 3D demo next and still says
audio is out of scope. The roadmap is a guide; where it disagrees with this
paragraph it is out of date rather than right.

**The PDF reader is more tractable than it looks, and that is why it is
first.** Three of its four hard parts are already in the tree: `puff` does
Flate, `stb_truetype` rasterises glyphs, and the blitter draws them. What is
left is the cross-reference table, the object model and the content-stream
interpreter — structure parsing over bytes that arrive from somewhere else,
which is the Lua side of the line `design.md` §6 draws. Plain documents
only: no forms, no encryption, no JavaScript.

Two things it will lean on that arrived in the last session. `read` takes
`into`, a capability to pages the caller owns, so a document larger than a
2 KB message crosses at all; and `kfs.read_range` is the `pread` underneath
it, so a page is read without the rest of the file. And `tools/kfs.lua`
means a real PDF can be put on a disk image from this Mac before any of it
is written — which is the right way to start, because a synthetic document
fails in none of the ways a real one does.

**Then sound**: a virtio-snd driver in the HAL, WAV first to prove the whole
path from device to speaker, then a vendored MP3 decoder as userland C
beside the font rasteriser — the same shape, and for the same reason. A
music player after it.

**Then Doom**, and then real hardware.

### Open, and not part of that

**The display harness is unreliable, and it is a gate on pushing.** About one
run in three fails at one of several phases, and the common sentence is that
an input the harness sent did not arrive — by the monitor socket or by the
serial line, so it is not one mechanism. `make test`'s 109 tests are
unaffected and pass every run. See **Known bad** above; the honest fix is
probably a guest per phase, and the honest measurement is a timestamp at each
end rather than more repetitions.

**M2 cannot be closed without a cable**, and its remaining half is the point
of the milestone: the HAL takes its real shape once there are two
implementations to compare, and `hal.md` is explicit that writing that
interface against one target produces the shape of QEMU wearing generic
names. Nothing is gained by guessing at it now. When a cable arrives:
`hal/pi1/` or `hal/pi5/`.

**Two benchmarks `roadmap.md` asked for at M4 are still not built:**
allocating and freeing a table, and the overhead of a syscall from Lua versus
the same one from C. The second is the number that says what the EL0 boundary
costs per crossing, and `sys.ticks()` makes it measurable from inside a
process.

**`list` still costs a round trip per entry** to ask for a size and a kind.
A `list` that answered with attributes would turn a listing of thirty files
from thirty-one messages into one. It is the protocol's shape, not `ls`'s
fault.

**The book is at chapter 2 of the outline in `book/OUTLINE.md`.** Chapters
are written after the thing they describe works, which is the rule that keeps
them describing what happened rather than what was intended.

## Decisions taken while implementing

Decisions that came out of writing code go here. Format: date, what was decided, why.

Design decisions (as opposed to implementation ones) go in the decision log in `README.md` and are propagated to `design.md` and `roadmap.md` in the same session.

**2026-08-30 — The kernel contains no Lua, and the tests for Lua run at EL0.**

`CLAUDE.md` had said since the first commit that the kernel has no Lua in it from M4 onward. It was false for two milestones: the interpreter was 163,648 bytes of `.text` against 28,556 for everything else in the kernel, reachable from no code path, kept alive by fifty test assertions that drove the kernel through it.

The tests were the whole of the reason, so the tests moved. `user/tests/luatest.lua` holds them, one role per test; `tests/tests.c` starts a process in a role and turns its exit code into a TAP line, which keeps the plan, the numbering and the names on the C side where they were. Same names, same count, one boundary further out.

What this bought is not speed — it was dead code, and the three benchmarks that touch no Lua are unchanged to three decimals. It bought the complexity budget back. `CLAUDE.md` allows 10k lines of kernel; the kernel's own source is 5,610, and Lua was another ~30,000 lines of C at EL1, where any bug in it is a kernel bug.

**Kernel machine code: `.text` 204,800 bytes to 20,480. Exactly ten times smaller.** Measured by building the previous commit in a worktree rather than remembered; an earlier note in this session said 192,204, which was the sum of symbol sizes out of the map and missed alignment.

**2026-08-30 — What left with Lua: `malloc`, `math`, `snprintf`, `strtod`, `stdio`, and `-lm`.** Every one of them was in the kernel for Lua and for nothing else — nothing in `kernel/`, `arch/` or `hal/` allocates or touches a float, and `-mgeneral-regs-only` has been turning the second into a compile error all along. Only `string.c` and `setjmp.S` are left, the latter because `trap.c` builds its fault-expectation mechanism on it. The test image links the rest back for its own unit tests of them, and brings up a 256 KB heap of its own to do it.

**2026-08-30 — A monotonic counter is a syscall; the wall clock stays a capability.** `SYS_TICKS` reads `CNTPCT_EL0`. `design.md` §4.4 makes `/dev/clock` something a process is handed or is not, and that does not change: what a program wants is a date, and a date comes from a server. But the server has to read the counter from somewhere, and a tick is the kind of thing that genuinely cannot be a message — it has to be sampled where the code being timed runs, or the sample measures the sampling.

It also retired a weakness that was written down and not fixed: a process could not read the counter at all, so Lua's string-hash seed at EL0 came off a stack address, and two processes from the same image start at the same address and so got the same seed.

**2026-08-30 — A benchmark harness that waits for a process must block, not spin.** The two Lua benchmarks are processes now, and the kernel side blocks in `ipc_receive` rather than yielding in a loop until the process exits. A spin would leave the harness thread runnable for the whole measurement, so every timer tick would switch into it and charge its work to the number being measured.

**And the reply has to be a value.** Replying with a zero-length message is not replying with nothing — it is replying with something that is not a serialisable Lua value, so the caller raises, the process dies, and the harness waits for ever on a sender that no longer exists. That is a hang rather than a failure, and it is what the first version of this did. The reply is now the request, echoed back.

**2026-08-30 — Recording a baseline no longer throws away its note.** `run_bench.py --record` carried `tol` across and rebuilt everything else, which silently dropped the `note` field — the part that says what a number means and why, and the part that is expensive to work out twice. Found while recording the baselines this change moved.

**2026-08-30 — Two of the five benchmarks changed what they measure, and the numbers are not comparable across the change.**

`context_switch`, `exception` and `ipc_roundtrip` are identical to three decimals, which is the evidence that the kernel itself did not change.

`gc_pause_max` moved 77,860 to 78,130, +0.3%. A collector step costs the same at EL0 as it did at EL1, which is the direct answer to whether the move cost performance: the interpreter's own work does not care what privilege level it runs at. Interrupts are now live inside the measured window, because a process cannot mask them — and for a pause metric that is more honest rather than less, since a pause the user feels includes the tick that landed in it.

`serialize` moved 1,037.6 to 1,364.9, +31%. Not a slowdown of the serialiser: `sys.pack` returns a Lua string where the C version wrote into a `struct message` and never touched the heap. Measured rather than assumed — an allocate-and-copy of a string that size through one Lua-to-C call costs 164 ticks against a 325-tick increase, so the allocation is about half of it and the rest is the second crossing and the copy back into a message on the way in. What it now measures is what a caller out here actually pays.

**2026-08-30 — The toolchain is the official ARM GNU 14.2.Rel1 `aarch64-none-elf`, unpacked under `~/toolchains`.** The Homebrew recipe `setup.md` used to recommend (`aarch64-unknown-linux-gnu`) targets Linux and brings glibc and Linux start files, which is what `-ffreestanding -nostdlib -nostartfiles` exists to avoid. It also produces differently named binaries. `setup.md` corrected in the same session. Homebrew is not installed on this machine and MacPorts has no `aarch64-elf-gcc` port, so the ARM tarball was the only path that lands on the documented binary names.

**2026-08-30 — A capability travels out of band, never inside the serialised bytes.** An index means something only in the table it came from, so the kernel translates it on delivery and what arrives is the receiver's own index. Stored as index plus one, so that zero — which is what `{0}` gives — means none.

**2026-08-30 — Anything that is "one per running thing" is stored on the running thing.** `current_process` and the address space were both global-shaped and both broke the moment there were two processes. The same shape appears again wherever a global is "enough for now".

**2026-08-30 — A thread is runnable the instant it exists, and preemption makes that instant real.** Anything created and then configured on the next line has already run. `thread_create_suspended` exists for that, and the race was found three separate times before the habit stuck: once in processes, once in `sys.spawn`, and once in tests.

**2026-08-30 — Isolation is the AP bits, not the address space layout.** Every kernel mapping is `AP=00` with PXN and UXN, so a process cannot touch kernel memory whether or not the kernel is mapped in its space. A process reading the kernel image gets a permission fault at level 3, not a translation fault, which is the difference stated as plainly as it can be.

**2026-08-30 — The address space follows the thread, and the process pointer lives on the thread.** Both were global-shaped and both broke the moment there were two processes: whichever ran last owned TTBR0 and owned `current_process`, so one process ran with another's memory underneath it and its syscalls checked pointers against the wrong address space. Anything that is "one per running thing" has to be stored on the running thing.

**2026-08-30 — A process is built before it is startable.** `process_create` leaves it suspended and `process_start` makes it runnable, because a runnable process runs: one created and granted its capabilities on the next line had already exited by then. Removing a race by construction beats masking interrupts around it.

**2026-08-30 — Copy the length, never the buffer.** A round trip moves a message five times, and copying all 512 bytes regardless of use made it thirty-six times slower. The benchmark caught it on the first run, which is the entire argument for having had one since M3.

**2026-08-30 — Preemption switches in the vector's epilogue, never in C.** A context switch moves `SP_EL1`, and everything after the switch reads the frame at `sp`, so that frame has to belong to the thread about to be resumed. Splitting the decision (`thread_tick`, in the handler, asking the policy) from the act (`thread_preempt_if_needed`, in the epilogue) is what lets the decision stay a C function the policy owns.

**A consequence that cost an instruction abort at an address that was never code:** `context_switch` had assumed `SPSel` was 0. True for a thread that yields, false for one arriving from the epilogue where taking the exception already set it to 1. `SPSel` is now part of the saved context.

**2026-08-30 — Every address space contains the kernel, and that is temporary.** There is no TTBR1 split, so a space without the kernel would fault on the instruction after the switch. A space copies the kernel's top level and shares the tables below, so user mappings are confined to 2 GB and above and anything lower is refused. M4 replaces this with the kernel in TTBR1.

**2026-08-30 — `sys` is a preview of the interface, not the interface.** At M4 these become real syscalls across a privilege boundary, and at M5 the inspection half disappears into `/proc`, read through the namespace protocol like every other resource. `design.md` §9.5 is emphatic that there must not be a second way to reach it, so `sys.threads` and `sys.memory` are scheduled for deletion rather than for extension.

**2026-08-30 — Every spawned Lua thread gets its own `lua_State`.** Not a design choice so much as the only thing that works: a `lua_State` is not reentrant, and two kernel threads inside one would corrupt it. `design.md` §2's share-nothing userland arrives early because the alternative does not run. They currently share one `malloc` heap; per-state heap limits are M4's problem.

**2026-08-30 — The scheduling policy is behind an interface, not wired into the thread code.** `thread.c` owns the mechanism and `struct scheduler` owns which runnable thread runs next, so a different algorithm is a new file. Per-thread policy state is embedded in `struct thread` rather than reached through a pointer, because there is no allocator to hand a policy its own storage; the fields are named for what algorithms need generally rather than for round robin. A test installs a deliberately terrible LIFO policy and asserts both exact orderings, which is the only proof the seam is real.

**2026-08-30 — Kernel threads run on SP_EL0 and take exceptions on SP_EL1.** Taking an exception always sets `SPSel`, so the hardware hands the handler a different stack with no code to switch it. That is what makes a stack overflow readable rather than a double fault, and it makes a double fault name itself: a fault in ordinary code lands in the first quarter of the vector table and one inside the handler lands in the second.

**A consequence that cost time:** `SP_EL1` cannot be named from EL1, because its system-register encoding is an EL2 one. `mrs x10, sp_el1` there is not a trap, it is an undefined instruction, and it arrives as EC 0x00 "unknown reason" explaining nothing. Reaching it means making it the current stack pointer briefly with `SPSel`, which in turn means the context switch has to mask interrupts.

**2026-08-30 — A thread is on exactly one IPC queue at a time.** All three of an endpoint's queues thread through the same link field, so a thread on two of them silently truncates one. Written as a warning in a comment and then done anyway; the symptom appeared three tests away from the cause.

**2026-08-30 — Dead threads release their slot, and their stacks go with it.** The stacks are already allocated with their guard pages already unmapped, which is the state a new thread wants. A recycled slot has its capability table cleared: inheriting the dead thread's capabilities would let a new thread reach endpoints it was never handed, and everything would appear to work.

**2026-08-30 — FP and SIMD are enabled at EL1, and the kernel's own C still is not allowed to use them.** `CPACR_EL1.FPEN` resets to trapping every FP access at EL0 and EL1 alike. Two things need them: `setjmp`/`longjmp` save `d8`–`d15` because AAPCS64 makes them callee-saved, and Lua's numbers are doubles. Neither is optional, so the trap has to go. `-mgeneral-regs-only` stays on every C file, so the kernel still cannot emit an FP instruction by accident; the flag restricts code generation, not what hand-written assembly may save.

Found the hard way: the first `setjmp` panicked with EC 0x07, whose name ("unhandled exception class") says nothing about floating point. There is now a test that executes an FP instruction, not merely one that reads `CPACR`.

**The consequence to carry into M3:** the context switch will not save FP state. While Lua is on a single thread that is fine. The moment it is not, this needs lazy FP save, which `roadmap.md` schedules at M10 for Doom and which will be needed sooner.

**2026-08-30 — The libc lives in `runtime/` and the kernel shares it.** At M2 there is one address space and one image, so a second copy in `kernel/` would be a duplicate symbol rather than a boundary. `kernel/string.c` is gone. The split happens at M4, when Lua moves to EL0 and `runtime/` becomes what the design calls it: the libc inside a process, whose I/O resolves against that process's namespace and nowhere else.

**2026-08-30 — QEMU `virt` defaults to a GICv2; Kosmos drives a GICv3 and passes `gic-version=3`.** Read out of QEMU's own device tree rather than assumed, after the same documents turned out to be wrong about the entry exception level. The flag is on the QEMU line in the Makefile and in `tools/run_tests.py`, and the two have to stay in agreement. `hal.md` corrected, including the claim about the Pi 5's GIC, which is now marked as an open question instead of an assumption.

**2026-08-30 — The timer rearms from the previous deadline (CVAL), never from the current time (TVAL).** TVAL sets the comparator to "now plus interval", where "now" is when the handler runs, so every period absorbs the cost of taking the interrupt and the error accumulates. Under QEMU that cost is about 195,000 counter ticks, roughly 3 ms against a 10 ms period, and a nominal 100 Hz ran at 73: eight ticks in eleven seconds of wall clock. Measured, not reasoned about. There is a test that fails if it is ever changed back, and the "ticks advance" test passes with the bug, which is why the second one exists.

**2026-08-30 — A null dereference cannot be written in C.** GCC is entitled to assume undefined behaviour never happens, so it emits the store and then treats the rest of the function as unreachable, appending a `brk`. The store faults, the handler steps ELR past it, and execution lands on the `brk`: a second fault with the expectation already spent, and a panic. Every deliberate fault in the tests goes through an inline-assembly store.

**2026-08-30 — Expected faults recover by stepping ELR, not by `setjmp`.** There is no `setjmp` until the libc arrives at M2, and every A64 instruction is four bytes, so the arithmetic is exact. Only synchronous exceptions are recoverable this way: an IRQ did not come from the instruction at ELR.

**2026-08-30 — On AArch64, `SYS_EXIT` takes a pointer, not a status.** `x1` holds the address of a two-field block: the reason code (`ADP_Stopped_ApplicationExit`, `0x20026`) and then the exit status. Passing the status directly in `x1` is the AArch32 form; it assembles, it runs, and QEMU exits 0 no matter what the guest meant. A harness that always reports success is worse than no harness, so the failure paths were exercised rather than assumed: a failing test, a hanging test, a missing banner, a build error, and QEMU's exit code on its own. All five produce a non-zero exit.

**2026-08-30 — The test image is a separate build under `build/test/`.** Same sources plus `tests/`, with `-DKOSMOS_TEST`. Two directories rather than one so the normal image never carries test code and the two cannot share a stale object file.

**2026-08-30 — `README.md` and `CLAUDE.md` moved from `docs/` to the repository root.** Their links were written relative to the root (`docs/design.md`), so from inside `docs/` every one of them resolved to `docs/docs/...` and was broken. `CLAUDE.md` also has to be at the root for Claude Code to load it automatically.

## Known bugs

**Characters typed before the prompt appears are lost.** The PL011's receive FIFO is sixteen bytes and nothing drains it until the REPL starts, so anything pasted into the terminal during boot overflows it silently. A person typing at a live prompt never sees this; it showed up feeding the REPL from a pipe. The fix is an interrupt-driven receive path with a ring buffer, which is worth doing when there is a real terminal at M6 and not before.

**An exception taken while the stack is exhausted is a double fault.** The handler builds its frame on the stack that just overflowed, so it faults again inside the vector and the kernel hangs with no output. The guard page turns a silent overflow into a readable abort, which is the improvement; it does not survive one. The fix is a separate exception stack, and it belongs with the thread work at M3.

**A minimal `kmain` with no stack faults silently.** Found while smoke-testing the toolchain. On QEMU `virt` the reset value of `sp` is 0, so any function prologue touching the stack writes to unmapped memory, takes an exception with no vector installed, and hangs with no output. It is not a bug in the system, it is the reason `boot/start.S` sets `sp` before branching to C. Recorded because the failure mode is a silent hang, which is indistinguishable from twenty other causes.

## Hardware pending

- [ ] 3-pin JST-SH debug UART cable (Pi 5) — blocks M2 on that target

**The Pi 1 is out of scope: Kosmos is 64-bit only.** `hal.md` keeps the
argument for it and why it was answered no. What replaces it as the second
*architecture* is x86-64 - a four-core Xeon Mac Pro, which is also the
first real machine on which SMP would mean anything, since it is on the
desk rather than waiting for a cable.

Measured rather than assumed, because "without a refactor" is a checkable
claim: **16 AArch64-specific sites in 8656 lines of `kernel/`**, and they
are four things - masking interrupts (`daif`, 9), idling (`wfi`, 3),
reading the cycle counter (`cntpct_el0`, 2) and reading the current
exception level (1). Extracting those behind `arch/` is an afternoon and
wants doing while it is still sixteen. Everything in `hal/qemu-virt/` is
ARM-specific and is *supposed* to be; an x86-64 box gets its own `hal/`.

---

## Overall progress

**The numbers are names, not an order** (`roadmap.md`), so this table is read
down the status column rather than across it. Work has repeatedly jumped
ahead of the numbering: M6, M7 and M8 all met their definitions of done while
M2's second half sat waiting for a cable.

| # | Milestone | Status |
|---|---|---|
| 0 | Boot under QEMU | **done** |
| 1 | MMU, exceptions, timer | **done** |
| 2 | Lua in the kernel + second target | **half done** — QEMU yes, hardware blocked on cables |
| 3 | Microkernel | **done** |
| 4 | Lua to userspace | **done** — two benchmarks on its list unbuilt |
| 5 | Namespaces and servers | **done** — both halves, plus Lua out of the kernel |
| 6 | Graphics and app server | **done** — window manager, compositor, mouse, UI kit, Terminal |
| 7 | Attributes, live queries, replicants | **done** — all three definitions of done |
| 8 | Own filesystem | **done** — kfs on virtio-blk, journalled, host tooling |
| 9 | Software 3D demo | **done ahead of order** — `cube3d`, `g3d.lua` |
| 10 | Doom | after the PDF reader and sound |
| 11 | Drivers (GPIO, USB, network) | |
| 12 | SSH client | |

**"Done" here means the milestone's own definition of done is met and has a
permanent test**, not that nothing more will ever be added to it. M6 is the
clearest case: the window drag with a hung app inside it works and is
checked, and virtio-gpu and the `hal_fb_flush` it will earn are still ahead
of it.
