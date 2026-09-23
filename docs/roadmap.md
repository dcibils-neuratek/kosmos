# Built, and the wishlist

**How this file is kept, Diego's instruction of 16 September: "every task
done should be marked as done in our roadmap", and "a roadmap should always
be visible to us".**

So every item carries its state in its first words - **DONE**, **IN
PROGRESS**, **BLOCKED** (and on whom), or **NEXT** - rather than leaving a
reader to infer it from a paragraph. An item that finishes is marked the day
it finishes, in the same session, beside the version that carried it. The
point is that a glance answers "where are we" without reading the prose, and
that nothing sits quietly finished-but-unmarked, which is how a roadmap
stops being trusted.


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

**A clipboard**, held by the window manager because it is the one process
every application already talks to. Drag to select, `Control-C` to copy,
`Control-V` to paste. These were behind the `Control-W` prefix until Super
arrived and gave the window manager a modifier of its own; Control-C used to
end the desktop, which meant copying closed it.

**Networking.** virtio-net, ARP, IP, ICMP, UDP and TCP with shared rings, an
HTTP server, a telnet client. It reaches the real internet through QEMU's
NAT.

**A resolver**, so an address can be a name. A query, a reply, names written
with a length in front of each label, and the compression pointers a real
server answers with. `host example.com` at a prompt, and the browser's
address bar. UDP exists only as far as this needs it — no sockets, because
nothing else has asked for any.

**Audio.** virtio-snd and Intel HDA - heard through the ThinkPad's own
speaker since 0.10.70 - a mixer, WAV and MP3. An MP3's length and bitrate come
from its Xing header when the encoder wrote one, and what plays is the
decoder's output sample for sample: Basket Case, recorded out of QEMU, matched
the Mac's decode in 99.996% of its frames and was one step off in the rest
(`testing.md` §18.77).

**Applications.** A PDF reader, a paint program, a text editor, a photo
viewer, a calculator, a music player, a file manager, a process list, a
system monitor — forty-one of them.

**Software 3D**, through TinyGL, and Doom.

**A Super Nintendo**, from LakeSnes, with its ROMs on the drive and its sound
paced by the device.

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

**Storage at full speed, performance first.** Diego, 14 September: "I expect
our usb drives and nvme to perform like any other os like Linux", and "it's
bad to have a nicely designed and modular system if it's slow and unusable".
USB step 6 waited for it, since the drive server would otherwise be built on
the block path this replaces - **and does not any more**: on 15 September,
with the stick's blocks from 17 to 303 IOPS on the ThinkPad, Diego put the
next measurement, `/home`'s path through the disk server and kfs, for later -
"we will measure /home later". In this order, each measured before and after:

1. **DONE - Disk Benchmark**, drawn first (`docs/diskbench.html`): an engine in
   `/lib/diskbench.lua`, a `diskbench` program, then the window once Diego
   has changed the drawing. A drive is read through its blocks and never
   written; a filesystem through a test file, removed afterwards.
2. **DONE - a baseline, and where the time goes**, under QEMU for the shape and on
   the ThinkPad for the numbers: the Kingston's blocks, `/home` on it, and the
   NVMe once something outside the kernel can reach it. **Where the time goes
   is built** (`/home/.device`, `testing.md` §18.64): under QEMU the device
   calls are 49 to 66% of a run on the kernel's disk and 70 to 86% on a
   stick's `/home`, because kfs makes one per 4 KB. **On the ThinkPad**
   (`9af841c`, 14 September) `/home` read 16.3 MB/s and wrote 4.6 MB/s, 39
   IOPS, with the device 92 to 100% of each run; the stick's blocks straight
   through the driver, 2.1 MB/s and 17 IOPS - 58 ms a request, close to the
   driver's 50 ms watch.
3. **IN PROGRESS - the largest measured cost first.** **Batching is built** (`testing.md`
   §18.65): the kernel's disk call moves up to 124 KB, kfs reads a file's
   neighbouring blocks in as few calls as that allows, and the journal writes
   in runs - under QEMU, sequential reads 3.9 and 6.2 times as fast, writes 1.7
   and 2.4. **The USB driver's request path came next**, because on the
   ThinkPad the device was almost all of a run - and its first cost was not
   the device: the driver's wait watched only the disk server's endpoint, so
   a read on `/dev/blocks` waited out a 50 ms deadline, 17 IOPS on the
   ThinkPad and under QEMU alike. **Built** (`testing.md` §18.71): the wait
   watches both endpoints, and under QEMU the stick's blocks read 759 MB/s
   and 9765 IOPS where `/home` on the same stick reads 219 MB/s and 928 - so
   the `/home` path, the disk server and kfs, is the next cost to measure on
   the ThinkPad. Under
   QEMU a write is 81 to 91% kfs, but a profile on the Mac puts kfs's own work
   at 1.3 ms for the 768 KB file, and QEMU makes CPU work large and its disk
   small (`testing.md` §18.65). On a real stick, the journal writing every data
   block twice may be the larger cost. The candidates reading found:
   `kfs`'s bytes through Lua strings; every block a write changes journaled,
   data included, so written twice; and no write bigger than the journal -
   254 blocks with the file's metadata, so under a megabyte, which Disk
   Benchmark's first run found; USB reads
   of 124 KB, copied on their way; the kernel's NVMe driver, polled, one
   command at a time, 4 KB a call. What they point at: the byte path in C -
   which Diego allowed, "if you need to take the filesystem from lua to c do
   it" - a block protocol that queues on a shared ring, NVMe as a userland
   driver, and chained USB transfers.
4. **OPEN - for Diego, when a measurement asks**: a device writing straight
   into a client's pages with no IOMMU to fence it.

**SMP - the mechanism is finished on both boards, and new threads spread
by default.** `docs/smp.md` is the map. **All seven steps are done**: per-CPU
state, the locks, four processors each with their own vector table,
interrupt controller, timer, idle thread and runqueue, and an IPI worth a
measured 25x on a cross-core wake. A plain boot places new threads on all
of them; `thread_create_on(cpu, ...)` puts one anywhere deliberately.

**Work spreads.** Six compute-bound processes on four processors: 100% on
every core, where before the fix three of them went idle within a second
while all six were alive. The cause was preemption rather than placement -
`thread_tick` returned before `policy->tick` on every core but zero, and
`thread_wake` decided preemption about the waking core instead of the
target. `docs/smp.md` has both, and what was ruled out first.

**Placement is on by default since 0.10.22.** The last thing that held it
off was the display harness's editor phase under `SMPWORK=4`, where the
program typed into `edit` did not come back: `ipc_call` could lose a reply to
a receiver on another core. With that fixed and x86 given its TLB shootdown,
the ThinkPad ran its desktop across all eight processors, and a plain boot
now does what `SMPWORK` used to. Next for placement is seeing work rather
than runnable threads - a demo waiting for its next frame counts as idle.

Then a panic protocol - a core that panics has to *stop* the others rather
than queue behind them, and today it stops neither them nor itself. Step
seven needed no IPI on AArch64 - `as_switch` uses `tlbi vmalle1is`, which the
hardware broadcasts - and needed one on x86-64, which has it now.

### Known, and not ours to fix

**QEMU's CoreAudio backend plays at about twice speed on this Mac.** Diego,
16 September: Music "sounds like a chipmunk" under `make qemu` and is correct
on the ThinkPad. Measured at 2.09x under `-audiodev coreaudio` and exactly
1.00x under the WAV writer and under `none` at either rate, so it is not a
rate conversion and there is nothing in Kosmos to change (`testing.md`
§18.86). QEMU 11.1.1, the same install whose `hvf` acceleration does not boot
a Kosmos kernel at all. `make NOAUDIO=1 qemu` sidesteps it.

**What it cost us was a test, and that is built**: the media suite now boots a
second guest against a device that keeps its own time. Worth revisiting if a
later QEMU fixes the backend, or if sound is ever wrong on hardware in a way
this now-real-time check would catch.

### Next, in this order

**Reordered on 2026-09-11, and USB went to the front.**

The evening that did it is in `docs/thinkpad.md` §6a: the machine would not
boot with a 64 MB disk, and the disk is 64 MB of *memory* because **Kosmos
cannot read the stick it booted from**. GRUB has to shovel the whole
filesystem in as a module before the kernel starts, and on that laptop the
image arrives with eleven contiguous pages already corrupted. The workaround
was to make the disk smaller.

So the next thing is not a subsystem this system lacks in the abstract. It
is the one that makes the machine Diego owns behave like a computer:

0. **IN PROGRESS - USB.** xHCI, then enumeration, then a mouse, then bulk transfers, then
   mass storage, then another machine's drive - FAT32 and exFAT, read-only
   first - then Ethernet. Each step ends in something visible, and two
   of them are worth the whole milestone on their own:

   - **mass storage deletes the loader-disk problem.** No module, no
     relocator, no ceiling on what can be carried.
   - **Ethernet puts the network stack on real hardware for the first
     time.** ARP, IP, TCP, DNS, `host` and the browser are written and have
     only ever run against virtio-net under emulation. The T14 has no RJ45
     and its WiFi is an AX201 - a CNVi part, with the MAC inside the chipset
     and no public documentation - so a USB-C Ethernet adapter is the
     shortest path to metal, and those chips need no firmware and no crypto.

   **Another machine's drive is Diego's step**, 14 September: his flash
   drives hold files he wants in Kosmos, and they are FAT32, or exFAT when
   they are big. Kosmos's own reader, a server in C, read-only first; writing
   to them comes later and deliberately (`README.md`).

   **Where the stack lives is settled**: in userland, as servers, not in
   `hal/`. `docs/drivers.md` is the record. The three kernel primitives a
   driver outside the kernel needs have landed, and the first such driver -
   QEMU's power button - proved them, so xHCI is a driver rather than kernel
   work.

   **Step one is built (0.10.48): the controllers are up.** Every xHCI
   controller is found on PCI by its class, taken from the firmware, halted,
   reset, and its ports read, by a driver in a process. `docs/usb.md` is
   the account and grows with each step.

   **Step two is built (0.10.54): devices are named.** Each controller is
   given its rings and interrupter and started, each device a slot, an
   address, and its descriptors read, by interrupt - MSI-X, which the PC
   board gained for it. On the ThinkPad it named three devices of five, so
   since 0.10.55 a failure says why and gets a second attempt, and the
   driver stays, naming devices as they are plugged in and pulled out.

   **Step three is built (0.10.61): a USB mouse moves the pointer.** Diego's
   call, on 13 September - "yes / lets make the mouse work" - because his USB
   mouse did nothing on the ThinkPad's desktop. The board adds it into the
   position the TrackPoint moves, and the driver waits on every controller at
   once (`usb.md` §5). **On the ThinkPad it moved, in jumps**: read by its
   Report descriptor since `a543f20`, and with ERDP written high half first
   its reports came on a deadline rather than by interrupt - fixed that
   night, and smooth on the machine the next morning: 13736 reports, none
   found by looking.

   **Step four is built (14 September): bytes each way on a stick's bulk
   endpoints.** Diego's word once a plug stopped holding the mouse. A stick's
   configuration is read for SCSI over Bulk-Only, both bulk endpoints are
   configured, and one command goes through them - INQUIRY, a wrapper out, 36
   bytes in and a status in - with what the stick says it is on its line
   (`usb.md` §6). Next: mass storage, then another machine's drive.

   **Step five, mass storage, approved on 14 September** - "usb step 5 sounds
   good. go for it." - in six parts, each ending in something visible: **5a**
   the stick's size and its first blocks read; **5b** Reset Recovery, for a
   stick that stalls; **5c** the kernel's interrupt wait also taking an
   endpoint; **5d** a block protocol, a declared shape in `blockproto.h`,
   served by the driver; **5e** kfs on the boot stick's Kosmos partition, as
   `/home`, read and written; **5f** the new stick layout - that partition
   beside the boot one - and the loader naming it, offered as an experiment
   beside a stick that has booted. The five calls under it are in
   `README.md`.

   **5a is built (14 September): a stick's size, and its first blocks.**
   TEST UNIT READY with REQUEST SENSE, READ CAPACITY (10), and READ (10) of
   block 1 and the last block, each checked for a GPT header, through
   `storage_decode.c` (`usb.md` §7). On the ThinkPad the boot stick itself
   will answer.

   **5b is built (14 September): a stick recovered.** Reset Recovery after a
   command goes wrong - the class reset, then each bulk endpoint's halt
   cleared on the controller and the stick - and the command sent again
   once; REQUEST SENSE after any command the stick fails. The usb check boots
   with `opt/kosmos/stickfault=signature` to make QEMU's stick stall
   (`usb.md` §7).

   **5c is built (14 September): one wait for interrupts and callers.**
   `SYS_IRQ_WAIT_ANY` takes an endpoint and answers `IRQ_WAIT_CALLER`, a line
   with an interrupt first, with no wake lost between the endpoint's lock
   and the lines' (`usb.md` §7).

   **5d is built (14 September): the block protocol, served by the driver.**
   `blockproto.h`, `/dev/blocks` read only for every program, and `sticks`,
   which prints a stick's partitions through it; one read moves at most 124
   KB, because one Normal TRB carries at most 131,071 bytes (`usb.md` §7).

   **5e is built (14 September): `/home` on a stick's Kosmos partition, read
   and written.** With `opt/kosmos/home=usb` the disk server keeps kfs on the
   first Kosmos partition a stick holds - found through `/dev/blocks`, written
   through an endpoint only it is given, and waited for once, because the
   shell decides where `/home` is from one read - with SYNCHRONIZE CACHE (10)
   after each write to the journal's header; and a unit is made a name, after
   reading found that a stick plugged in later would have taken `/home`'s
   requests (`usb.md` §7).

   **5f is built (14 September): a stick whose `/home` is a partition of its
   own.** `make MEGA=1 x86-usb-image USB_HOME=partition` puts the kfs disk in
   a Kosmos partition beside the ESP and its GUID on the stick's command line,
   which the loader passes on unchanged; the disk server takes the partition
   by that GUID; the kernel keeps its whole command line; and `run_uefi.py`
   boots the stick through OVMF to `/home` on it (`usb.md` §7). **It booted on
   the ThinkPad the same day** - `/home` on the Kingston's own partition, a
   file written and read back - with the desktop about 20 seconds late, not
   yet explained.

   **Built as 0.10.64-development** (`986a627`, handed to Diego to try on
   the ThinkPad), asked for by Diego on 14 September: the disk server's search for the stick, counted and reported by
   `diskinfo` in the log's own seconds, to find those 20 seconds; a stick that
   does not do SYNCHRONIZE CACHE - the Kingston answers ILLEGAL REQUEST,
   20h/00h, to every one, twice a commit - told once and not asked again;
   `log save`, the whole log to a file on `/home`; `diagnose`, asked for the
   same night - "a log file of things you need so I can send it to you for a
   full diagnosis ... instead of photos of logs" - which puts the build, the
   machine, its devices, the disk, the sticks, the processes and the whole log
   in one file on `/home`; and `make stick-log`, which reads that file off the
   stick on the Mac and never writes it, so a diagnosis reaches this Mac as
   text rather than as photographs of a screen.

   **Asked for by Diego on 15 September, after reading 0.10.65-development's
   diagnosis**:

   - **The stick starts the desktop by itself.** "i do type wm at the prompt,
     i think the desktop should start automatically yes upon booting". The
     shell already starts what `opt/kosmos/boot` names, so the stick's
     command line says `boot=wm`; the prompt is still there when the window
     manager ends. **Built**: `USB_BOOT ?= wm` in the Makefile puts
     `opt/kosmos/boot=wm` on the stick's command line, and under OVMF the
     desktop came up with nothing typed; `USB_BOOT=` makes a stick that
     stops at the prompt.
   - **The loader says when it ran**, in seconds of the counter it shares
     with the kernel: when it started and when it handed over. The kernel's
     first line came 16.6 s after the counter's zero, and Diego picks the
     stick with F12 and it "boots instantly" - so that time is the
     firmware's or the loader's, and one line tells them apart.
   - **The Processes window has no idle row.** "lets remove the idle process
     from the processes app as its confusing as it looks like there is a
     process consuming most of the cpu all the time". The row was the
     window's own, made from the kernel's idle ticks - 99% at the top of the
     list on an idle ThinkPad. The shares stay a share of every tick, so an
     idle machine reads near nothing, the kernel's row stays, and Monitor
     still draws what is idle. **Built** (`testing.md` §18.74): the shares come from
     `/lib/procshare.lua`, tested on the Mac.
   - **Music opens the song Tracker gives it.** Opened from Tracker on the
     ThinkPad, Music said "(nothing to play in /home)" beside a Tracker
     window listing the MP3 in `/home` - being found. **Music now says why
     its list is empty** (`testing.md` §18.72), in the window and as a
     `music:` line in the log `diagnose` keeps. The cause on the ThinkPad is
     not found: under OVMF the same stick image, with a sound device, after
     `diskbench` and `diagnose`, and Music launched as Tracker launches it,
     listed the song every time.

   **Step 6, drives**, designed in `docs/drives.html` and
   decided with Diego on 14 September: Tracker's sidebar as Places, System
   and Drives with each filesystem's type, `/drives/<label>`, one Open and
   Save window for every app, a Drives app that shows before it changes
   anything, and other machines' FAT16, FAT32 and exFAT read, read only, with
   names found without regard to case - NTFS left out for now. **Built in six
   pieces, in Diego's order**: 6a, the FAT reader tested on the Mac, is built
   (`usb.md` §8) - **6a DONE**; **6b DONE** (16 September), the drive server
   and `/drives`, with FAT16 and FAT32 volumes named, listed and read: ten
   checks on a stick carrying two volumes, and a control watched failing
   (`testing.md` §18.87). It was marked done once before it was: the first
   green run used a fixture with one volume and a substring check, which a
   stride bug in the listing reply also passed;
   **6c DONE** (18 September), Tracker's sidebar and trail - the
   three groups are built and drawing: `Places` holding Home and Desktop as
   `drives.html` draws them, `System` folded away, and `Drives` fetching
   `fs.volumes()` lazily when it is opened, never on the way to a first
   frame (`mount_roots` records what probing mounts at startup once cost:
   18.4 seconds, with the desktop looking hung). `ui.tree` gained `heading`,
   `quiet` and `note` for it, and a fix so a node that *starts* open fetches
   its children at all.

   **The pane is 210 pixels, Diego's call on 16 September**, out of four
   options. At 150 only a short name and type fitted together, so
   `KOSMOS HOME kfs` and `PHOTOS 2024 FAT32` suppressed the filesystem while
   `BACKUP FAT16` drew it - a feature that works under QEMU, where there are
   no drives, and vanishes on the ThinkPad, where there are. Photographed on
   x86 with the mtools fixture attached: `PHOTOS FAT32` and `BACKUP FAT16`,
   dim and to the right. The file list loses 60 pixels and both display
   harnesses are unchanged at 118 and 116. **DONE on 16 September, the clickable trail**: every
   segment of the path is a target that navigates to it, the last one
   excepted because that is where you already are. A segment's target runs
   to the start of the next, so the ` > ` between two names belongs to the
   name before it - measured, after a click three pixels inside a separator
   did nothing and looked exactly like a broken handler.
   **DONE on 16 September, a second click opens a sidebar row** (Diego:
   "I want double click to open the folders like home and desktop and else,
   not only clicking on the little arrow on the left"). The interval is one
   second, read from `/dev/cpu`'s `counter_hz` rather than assumed: half a
   second is right on a desk and wrong under TCG, where the counter runs
   against host time while the guest lags, and two presses 0.12 s apart
   measured 46 187 937 ticks - 0.74 s of counter time (`testing.md` §18.86).
   **DONE on 18 September: shortcut places** - drag a drive or a
   folder onto the sidebar and name it, `drives.html`'s MyPhotos; click it
   to go there; right-click to take it out, into the Trash. Photographed on
   x86 with two sticks - MyPhotos "on PHOTOS" by its serial, Holidays dimmed
   and "unplugged" - and checked on both boards by the display harness's
   `places` phase, 3 checks with three controls, beside 17 host checks of the
   rule (`testing.md` 18.89 and 18.90, `ui.md` 16.8e). **They key on
   the volume's own identity, read off the disk: FAT's volume serial number,
   or the GPT partition's unique GUID.** This said "unit and partition", and
   that cannot work: `xhci.c` hands a unit out with `units_named++`, "the
   next never given out", so the same stick replugged into the same port
   comes back with a new number - the very case a shortcut exists to survive.
   A serial or a partition GUID travels with the volume across a replug, a
   port and a machine, which is what Finder's Favorites and Windows' Quick
   Access key on. `/drives` gains a field to carry it.

   **DONE on 16 September, the check that verifies a stick before it is
   handed over.** `run_uefi.py` failed 0.10.75 with "the picture is still the
   firmware's" - and failed `0.10.70-stable`, the build Diego uses, in exactly
   the same way. Its three colour checks name the *kernel's boot screen*,
   while every stick handed over carries `USB_BOOT=wm` and has replaced that
   screen with the desktop before the capture. It now reads the stick's own
   command line out of the ESP and judges a desktop stick on whether a desktop
   is drawn: **PASS, 29 checks**, with a negative control that calls the
   branch a rubber stamp when it is broken on purpose (`testing.md` 18.88). **The fullness bars are not
   6c's** - read again on 16 September, every bar in `drives.html` sits
   inside a `drive-tile`, which is the Drives app, and the sidebar rows there
   carry a name and a type and nothing else. They move to 6e; **6d DONE** (18 September), the Open and Save window - **DONE, the window itself** (`b1e408a`): the same sidebar as Tracker from `/lib/sidebar.lua`, the trail, Name, Size and Kind, one click selecting and a second opening, and a filter, in every application that opens or saves (`ui.md` 16.8f). **DONE on 18 September: the Super Nintendo's File menu** - Open ROM... and Quit (`testing.md` 18.101). Its window draws its own pixels, and no such window could carry a kit menu (`window:paint` returns at once for one). **Diego chose A**: the compositor draws a menu bar above a direct window's pixels, so Doom and Quake can have one the same way (README's decision log). Open ROM from a stick already works by path, `wm snes:/drives/...`;
   **6e DONE on 19 September - the Drives app**, as `drives.html` drew it:
   the drives, the chosen one's partitions as a bar not to scale, its
   partitions as rows opening in Tracker, and Format... and New
   partition... drawn and greyed (`testing.md` 18.108). A stick's partition
   table and the partitions no filesystem claims are the drive server's to
   report next; the machine's own disk is listed, its partitions not read
   yet. **6f** exFAT.

   **6b's shape, read out of the code on 16 September, for Diego to agree
   before it is built.** `drives.html` says `/drives` is "one folder every
   program has from the moment it starts", and that drives appear and
   disappear inside it while programs run. That settles the design question:
   **one server owns the whole `/drives` prefix**, rather than a mount per
   volume. A mount is an entry in a process's *own* namespace, made when that
   process is built - so a volume appearing later would mean editing the
   namespace of every running program, which nothing can do. A server behind
   one prefix needs none of that: the matcher already routes
   `/drives/PHOTOS 2024/Italy` to it with the rest of the path intact, exactly
   as `/home` is routed today.

   What it stands on is built: `fat_decode.c` reads FAT16 and FAT32 with no
   hardware in it, and the disk server already walks every USB unit - stepping
   over the gaps pulled sticks leave - and reads each one's partition table.
   6b adds the server, not the reading.

   **Both naming questions are answered, by Diego on 16 September.** A volume
   with *no* label is **`Untitled`**, which is Finder's answer and the one a
   person already recognises. Two volumes carrying the *same* label are
   **numbered in arrival order** - `PHOTOS`, then `PHOTOS 2` - and the same
   rule settles the first question's own collision, so two unlabelled sticks
   are `Untitled` and `Untitled 2`.

   **What that costs, recorded because it will be felt**: a name depends on
   plug order, so pulling the first `PHOTOS` and replugging it can make it
   `PHOTOS 2`. A path is therefore not stable across a replug, and anything
   wanting a stable handle needs the unit and partition instead. Accepted
   deliberately: the common case is one drive with a label, and reading well
   there beats being stable in a case that is rare.

   **6b is built, as far as naming volumes goes** (16 September, `usb.md`
   §8). `user/servers/drives.c` owns the whole `/drives` prefix, started by
   init as `ROLE_DRIVES` with the USB driver's *read* endpoint and never the
   write one - so read-only is what the process can do rather than what its
   code agrees to. `drives_decode.c` is the pure half and is tested on the
   Mac: 51 checks over both partition tables, a protective MBR that must not
   be offered as a volume, kfs's superblock read from an image `mkfs` really
   wrote, FAT32's FSInfo hint, and the naming Diego settled - three controls
   watched failing. A booted machine confirms `/drives` is mounted, lists
   empty with no stick attached, and answers "no such path" for a volume that
   is not there.

   **The FAT walk is written** (16 September): a chain iterator that handles
   FAT16's fixed root area and FAT32's cluster chain, path resolution by
   component, and `list`, `read` and `getattr` on top of them. The shared
   region is one buffer, so following the table happens *before* the caller
   reads the next directory sector and anything that must survive is copied
   out first; a chain is bounded by the volume's own cluster count, so a
   cluster pointing at itself answers `DRIVES_ERR_DAMAGED` rather than
   spinning the server.

   **Where it is tested is `run_x86.py`, and that was worth finding out.**
   The aarch64 harness has no USB at all - every `usb-storage` in the tree is
   on the x86 board - so a drive can only be seen there. `tools/fatstick.py`
   makes the fixture with mtools: FAT32 labelled `PHOTOS`, one sector a
   cluster, holding a short name, a long name of 3000 bytes (a chain of six),
   and `Italy/roma.txt` one directory down. Our own `fatls` and mtools' `mdir`
   read it identically, which is what makes it a fixture rather than a thing
   the reader agrees with itself about.

   **Still owed**: the phase has not yet run green, no negative control has
   been watched failing, and free space is still checked only against a
   volume this project made. kfs volumes are listed and never opened, because
   kfs's reader is Lua and this server is C.

   **The early display this paragraph asked for already existed.** It said,
   for a day, that a machine with no serial port shows nothing until stage
   six. The panel had shown the boot log from stage two since 0.10.12 -
   `hal_fb_early` answers with the framebuffer the firmware set up, before
   there is a page allocator (`thinkpad.md` §4) - and it was found when the
   work was started, before any was written. **And on the ThinkPad it had
   not**: that firmware puts the framebuffer at `0x4000000000`, and
   `hal_fb_early` refused anything above the 4 GB the boot page tables map,
   so the machine was dark until stage six on every boot. 0.10.62 adds the
   screen to the boot page tables (`boot.md` §5). What is still dark: a hang
   after the loader's last line and before the kernel's first, and a fault
   before the trap table.

   **Before step four: the ThinkPad's boot, understood.** Diego, 13
   September: "lets first understand why the image is not booting, then we
   can bring grub back". 0.10.61 stopped after the loader's last line with a
   64 MB disk and with a 32 MB one, and everything QEMU can reproduce of that
   machine boots the same image (`boot.md` §3). In hand: `mkusb.sh` reads
   every stick back through `tools/stickcheck.py`, and the kernel draws on
   that machine's screen from stage two. **And since 14 September a check on
   the machine** that the loader read the build's bytes: the build writes the
   kernel's and the disk's page sums onto the stick, and the loader holds its
   read to them and refuses on the screen with the first page that differs
   (`boot.md` §3). What it says on the ThinkPad is the next measurement.
   **And the stick is the lead now**: on 14 September the one the ThinkPad
   booted from dropped off its bus twice in one boot, after two tries that
   went back to the firmware's menu with nothing drawn - so the next sticks
   are branded ones (Diego).
   **GRUB comes back after that**, Diego's decision: it was never the cause, and it
   brings boot arguments and a screen mode per machine.

**Agreed on 2026-09-10**, after the ThinkPad ran spread across eight
processors, and still what follows USB:

0b. **The compiled-in limits go, before a 4K display is plugged in.**
   **The goal is Diego's decision**, 12 September: every memory failure on
   the ThinkPad was a constant sized for a 512 MB QEMU guest, and a 4K screen
   is four times what the last constant was sized for. **The steps below,
   their order, and this item's place ahead of Lite XL are proposed and not
   yet agreed:**

   - the compositor's mapping allowance derived from its framebuffer - in
     progress, because it is also the ThinkPad's Terminal and JPEG bug;
   - user address space reused, since a 4 GB window that is never reused
     holds about 120 full 4K surfaces in a session;
   - the x86 RAM ceiling of about 768 MB lifted, with a higher-half kernel;
   - **DONE on 19 September** - the kernel's pools grow as they are needed,
     to a ceiling derived from RAM (`kernel/pool.c`, `testing.md`
     18.113-18.117): threads, processes, address spaces, endpoints and
     regions, with a region's size and a capability table growing too;
   - **DONE the same night** - the flat per-process cap is gone and the
     reserve took its place: a program may map what the machine has except
     the last thirty-second, which the kernel keeps for a thread's stacks, a
     pool's slab, page tables and starting the process that ends a runaway.
     **Reclaiming from the largest offender** is not built and may never need
     to be: the reserve is what the desktop cannot lose.

   Driving an *external* monitor is a separate job: it very likely needs a
   display driver for the laptop's Intel GPU, which is to be confirmed before
   it is planned.

1. **DONE - Lite XL, until it is an editor.** It is one now: it opens, edits and
   saves (`docs/litexl.md`), in its own faces. What is left is the wheel,
   resizing and the title.
2. **DONE - Quake, running.** From Chocolate Quake rather than quakegeneric, which
   builds only for 32-bit machines; the shareware attract loop plays and the
   menus answer (`runtime/upstream/quake/README.kosmos.md`). What is left:
   sound through `/dev/audio`, looking around without dragging - a relative
   pointer mode in the window manager - music, and saving. `LICENSE` and
   `FULL=1` still disagree about Doom; Quake, outside `FULL=1`, adds nothing
   to that.
3. **DONE on 18 September - the tests in five to ten minutes: 4:37.** Diego, 18 September,
   after a gate of forty: "i dont want 40 minutes tests any more, 5 to 10
   minutes max from now on so make sure the tests are built accordingly".
   Asked once before, on 11 September ("can we make it 5?"), and paused.
   The same checks in less time, never fewer (`CLAUDE.md`). What was
   measured then: no `-j` anywhere; one flags stamp shared by every build
   variant, so each variant the gate builds recompiles everything, kernel
   included; every suite run one after another, `run_x86` alone booting
   about eleven times; and about 98 s of plain sleeps in each display
   harness run. In order: a timed run, so the order of work is a number's;
   `-j` and a stamp per variant; a runner that starts the independent QEMU
   suites together, with a log per suite, and the display harness - which
   checks timing - alone at the end; then waits for the thing in place of
   fixed sleeps. **What it took** (`testing.md` 18.97): `make test` is
   `tools/gate.py`, which builds every image first and runs every suite side
   by side, six at once, longest first; `run_x86.py` runs as four groups of
   its parts and the display harness as four parts per board. 18:28 with
   the harness whole, then 4:37 - the same checks, counted. The sleeps were
   not needed for it and are still there, as is the room they leave.
4. **IN PROGRESS since 18 September - the ThinkPad's brightness and volume
   keys**, moved ahead of the battery that morning because the screen is a
   problem today and the battery is not - **and that afternoon ahead of USB
   6e as well**, Diego's choice: "yes lets do brightness and volume keys".
   USB resumes at 6e after it, with the Super Nintendo's File menu waiting
   on how a window that draws its own pixels gets a menu. Diego, 14 September: "how can i make the brightness buttons
   on the thinkpad actually work?"; and on 18 September: "its too dim now and
   i cant control it", "also the volume keys on the keyboard, as its the way
   to control the volume".

   **Nothing in Kosmos sets a brightness, so it stays wherever the firmware
   left it**, and the keyboard driver drops any extended key it has no entry
   for (`extended_code` in `hal/pc/i8042.c`), so the brightness and volume
   keys vanish there if they arrive at all. On a ThinkPad some of them are
   expected to come from the embedded controller as ACPI events rather than
   as keys - expected, and not yet seen on this one. In this order, the
   cheapest and most urgent first:

   1. **DONE on 18 September - a log line for every key the driver drops**,
      once per key, with its byte: `i8042: a key this driver has no entry
      for, e0 30`. QEMU measured the standard bytes - mute e0 20, volume down
      e0 2e, volume up e0 30 - and the display harness's `unknown keys` phase
      checks it on x86 with two controls (`testing.md` 18.92). What the
      ThinkPad sends is one `diagnose` and one `make stick-log` away.
   2. **DONE on 18 September - the volume keys**, in QEMU on both boards:
      the i8042 maps e0 20, e0 2e and e0 30, the window manager takes them
      before any window, and up and down move the master by a sixteenth. This
      said they would need no new audio work, and mute did: the server had
      `muted` per stream and none for the whole machine, and zeroing the level
      would have made it mean two things. So `master_muted` is a field of its
      own, and muted is measured silent in what the machine played
      (`testing.md` 18.93). **And on the ThinkPad, 18 September**, from stick
      0.10.80: "sound keys work!" - its keyboard sends the same bytes.
   3. **DONE on 18 September, on the ThinkPad - the DSDT, read by Kosmos itself**, for where brightness is set
      - an embedded controller register, or the graphics device's backlight -
      with its offsets from the documentation rather than from memory. Shared
      with the battery below. No Linux boot to fetch it: `hal/pc/acpi.c`
      already walks the table list for `APIC` and `MCFG`, and learns `FACP`
      too, following the FADT to the DSDT, and keeps the SSDTs; the kernel
      maps them once the MMU is on and `SYS_FIRMWARE` hands their bytes up.
      `acpi` lists them and `acpi save` writes one file a table to
      `/home/acpi`; `make stick-log FILE=/home/acpi/` brings the folder to
      the Mac, where `iasl -e SSDT*.aml -d DSDT.aml` decompiles it. Tested
      under QEMU with a table of the test's own, handed over by `-acpitable`
      and wanted back byte for byte, beside QEMU's DSDT whole
      (`testing.md` 18.95); and QEMU's DSDT, saved by Kosmos, decompiles in
      `iasl` to 3203 lines. **And from the ThinkPad the same evening**, stick
      0.10.80: a DSDT of 218 508 bytes and 13 SSDTs, all whole. **What they
      say** (`thinkpad.md` 8b): F5 and F6 are not keys - the embedded
      controller raises queries 0x14 and 0x15 - and the backlight is the
      Intel graphics device's PWM, which the firmware expects a graphics
      driver to set. So steps 4 and 5 are that register, with offsets from
      Intel's Tiger Lake PRM, and the EC's query protocol, which the battery
      shares. **It goes on stick
      0.10.80 with the volume bar** (built, 29 checks under OVMF), so one session at the ThinkPad answers
      what F5 and F6 send and how the T14 sets its backlight - and the next
      one tries the brightness keys and the Display bar. Diego, 18
      September: "when can i try the brightness bar?", "in the thinkpad".
   4. **DONE on 18 September, on the ThinkPad - a comfortable brightness set at boot**, which fixes
      "too dim" before any key works: the Intel display engine's backlight
      duty cycle, written once. **The offsets are not in any public Intel
      manual** - Tiger Lake's and Ice Lake's register volumes document only
      the utility pin's backlight, which a laptop panel does not use - so
      they come from Linux's i915 (two controllers, control, period and
      on-time at C8250h/C8254h/C8258h and C8350h on), and the base from
      Intel's own (`GTTMMADR`, BAR0 of 0/2/0). **4a, reading only**: a
      driver at EL0, `user/servers/backlight.c`, reads both controllers and
      says what they hold - a controller on, with its on-time inside its
      period, confirms the offsets on the ThinkPad before anything is
      written. **4a is built** (`c0df73d`, `testing.md` 18.96) and on stick
      0.10.81, and the ThinkPad read controller 0 on at a third (period
      19393, on-time 6464). **4b** writes that controller's on-time to 80%
      and reads it back (`29753d3`); on stick 0.10.83 Diego: "it worked! the
      brightness worked!".
   5. **DONE on 19 September, on the ThinkPad - the brightness keys**
      (stick 0.10.88: "brightness keys work!"): 5a, **reading only, on stick
      0.10.84** - `hal/pc/ec.c` watches the embedded controller and GPE0 and
      says whether SCI_EN is set, so F5 and F6 on the ThinkPad show whether
      the firmware or the system hears them (`testing.md` 18.99). **Stick
      0.10.85 saw nothing**: SCI_EN clear, so the firmware's SMM took every
      event. **5b, built on 19 September, waits on the ThinkPad**: with
      Diego's yes, the machine is switched to ACPI mode at boot, the power
      button becomes a key that shuts down, queries 14h and 15h become
      brightness keys, the backlight driver serves `/dev/backlight`, the
      window manager steps it and shows the Display bar, and powering off
      writes the DSDT's `\_S5` so it turns the T14 off rather than halting
      (`thinkpad.md` 8c, `testing.md` 18.102). And **the level shown on the screen when a key
      changes it** - Diego, 18 September: "make sure we have a way to show
      brightness bar level in the screen to know where we are on the
      brightness level", and "like a volume bar as well". A bar that
      appears over the desktop for a moment
      when a brightness or volume key is pressed, saying which and how far
      along it is, the way every laptop's does. **Drawn first and approved as
      drawn on 18 September** (`docs/levels.html`, "all is good"): after
      macOS's Display panel, top right under the bar, and **smooth rather
      than notched** ("i like the bar with smooth instead of notches") - a
      key still moves it a step, and the ThinkPad's own levels sit behind it.
      **The Sound half is DONE on 18 September**, and seen on the ThinkPad the
      same day from stick 0.10.80 - "the bar showed up and faded". In QEMU: the window manager
      draws it at the moment of the key, at the level the audio server
      answers, and `run_media.py` holds it to that level, to every press
      being heard, and to its being gone after (`ui.md` 16.8g, `testing.md`
      18.94). The Display half and the brightness keys wait for step 3.
4b. **DONE on 19 September - the Super Nintendo's size and a pause**
   (`testing.md` 18.103). Diego, 19 September: "I realized the snes emulator i cant seem to
   switch to 2x scale" - it could only be started that way, with `--scale 2`
   - then "Do the 2x option in snes emulator app menu and it will restart the
   app", "It's better than nothing", and "Also emulator needs a pause / play
   mode so I can pause a game and restart it later on". So:
   - **View**: Double Size (1024 x 960) at 1x, Normal Size (512 x 480) at 2x.
     It starts a fresh Super Nintendo on the same ROM at the other size and
     closes this one - Diego's choice over resizing a window that draws its
     own pixels in place, which the window manager refuses today and which
     would have kept the game running. The game starts again from its
     beginning.
   - **Game**: Pause and Resume, one item that says which, and the P key. A
     paused game runs no frames and shows "Paused" over its picture.
   - **Proposed, not agreed - save states.** LakeSnes can save and restore a
     whole console (`snes_saveState`, `snes_loadState`), so a game could be
     saved to `/home` and continued after quitting, and Double Size could
     keep the player's place instead of starting over.
4c. **IN PROGRESS - game controllers over USB. The Xbox 360 part is DONE on
   19 September, on the ThinkPad** (stick 0.10.88, Diego's SN30 Pro: "snes
   controller works!"); Xbox One and Series next.
   Diego: "How hard would It be to use a usb game pad controller in kosmos
   with snes emulator and other apps?", then "let's add Xbox 360 and Xbox one
   controllers support which are the most common", and "I do have a 8bit
   controller usb which is a snes controller" - the 8BitDo SN30 Pro USB,
   whose modes are "Switch mode, X-input" (8BitDo's page), so on Kosmos it
   is an Xbox 360 controller. Its place in the order is Diego's to say.
   1. **Xbox 360, wired** - Microsoft's own protocol rather than HID, class
      FFh: a fixed 20-byte report. The SN30 Pro USB speaks it. The wireless
      receiver wraps the same report and comes after.
   2. **Xbox One and Series, over USB** - the same class with a different
      protocol, and a start-up packet before the pad sends anything.
   3. **How presses reach a program**: the USB driver turns a report into
      presses and releases and hands them to the kernel as key events - a
      sibling of the call the USB mouse moves with - so they go to the
      focused window like any key. The sticks are D-pad presses past a
      threshold. Analog values, rumble and more than one player are later,
      and would want a state block shared per pad instead.
   4. **The Super Nintendo, Doom and Quake** map the pad's buttons.
   5. **Tested** on the host with reports from the protocols' own layouts,
      in QEMU for the path from the driver to a window, and **with the
      real SN30 Pro under QEMU on the Mac** through `usb-host`
      (`tools/usbhost.sh`), before the ThinkPad. **This said macOS has no
      Xbox 360 driver of its own, and it has one**:
      `com.apple.gamecontroller.driver.XboxGamepad` holds the pad's gamepad
      interface, so QEMU takes it only when run as root - as anyone else the
      pad refuses SET_CONFIGURATION, which is what the first try found.
4d. **DONE on 19 September, booted on the ThinkPad in stick 0.10.88 - a `/home` of 512 MB
   or more on the stick, with Diego's own files in it.** Diego: "from now on
   we need to make the drive image at least 512mb as we are adding more
   content to it", "and i will be adding more images, videos, etc to test in
   kosmos". The stick's `/home` has been a partition of its own since 0.10.62
   (`USB_HOME=partition`), read by the USB driver rather than carried by the
   loader, so the 32 MB cap - which came from the loader carrying the disk
   into memory - no longer has a reason on that layout. **But no bigger one
   has booted on the ThinkPad**, and the table in `boot.md` decides. So: kfs
   at 512 MB checked under QEMU first, then a stick of its own, 0.10.88, whose
   first boot on the ThinkPad is the proof - with 0.10.87 to fall back to.
   Diego's images and videos come from a folder on the Mac that the image
   build copies into `/home`, and never from the repository.
4e. **ASKED FOR on 19 September - a video player that plays MP4.** Diego:
   "add a video player with mp4 support for todays roadmap using the current
   mp4 video in the home folder for testing". The clip, `magicword-clip.mp4`,
   is **H.264 Main profile** (level 3.0, with B-frames), 640x360, 24 s, and
   **AAC-LC** stereo at 44.1 kHz - so the small Baseline-only decoders
   (h264bsd, OpenH264's) cannot play it, nor most phone video. Three pieces:
   - **The container**, ours: an MP4 reader - `moov`, the sample tables,
     `avcC` and `esds` - host-tested on the clip. **Started today.**
   - **The decoders: FFmpeg's `libavcodec`, H.264 and AAC only**, agreed
     by Diego on 19 September ("Yes"). Ported as Doom, Quake and LakeSnes
     were, vendored byte for byte with its notice - LGPL-2.1+, so in the
     FULL image (a GPLv2 work already) and out of the MIT one, and named in
     `LICENSE`. The source is a download, asked for before it is fetched.
   - **The app**, drawn first as every app is: `docs/video.html`, and
     **approved on 19 September** - Diego: "Video app looks great". So: the
     controls under the picture rather than over it, the three sizes the
     View menu draws (Actual, Double, Fit to the Screen), the keys and the
     game pad as drawn.
   Several days, not one.

   **The app is built** (20 September), and it is what the drawing shows:
   the picture at its own size with the controls under it, the window
   manager's menu bar above, File/View/Play, the three View sizes and
   **Full Screen** - which Diego asked for and which went into the window
   manager, because `maximise` cannot resize a window that draws its own
   pixels and every such application wants the same thing. What is left of
   4e is the decoders, and the sound: the kit reads a film's audio track
   and says what it is, and the volume items are in the Play menu greyed
   until it can play it.

   **AGREED on 20 September - a film we can already play, first.** Diego:
   "Perhaps we can download a mp4 video with the codecs you already have to
   see them working?" So before libavcodec: an MP4 carrying **Motion JPEG
   video and MP3 audio**, which are `gfx.jpeg` (stb_image) and `/kits/mp3`
   (minimp3), both in the tree for other reasons. It proves the whole path -
   index, frame by offset, decode, blit, the clock, audio out - with nothing
   new to port, and leaves exactly one thing unproven, which is the decoder
   that replaces them.

   `mp4.lua` read the film **unchanged**: 300 video frames in an `mp4v`
   entry at 640x360, 384 audio frames in an `mp4a` entry whose `esds` object
   type is `0x6b`, MPEG-1 Layer III. The first video sample begins `ff d8`
   and the first audio sample `ff fb`, which is to say a JPEG and an MP3
   frame, handed to decoders that already exist.

   The clip is made here rather than found: no MJPEG MP4 was worth trusting
   off the web, and there is no `ffmpeg` on this Mac, so **a minimal host
   ffmpeg is built from the source already downloaded for the port** (9.0.2,
   matched against its git tag) - `build/ffmpeg-host`, decoders and muxers
   only, 5.4 MB, host only and in `build/`, never vendored and never in the
   image. It is also the tool for every test asset after this one.

   **AGREED on 20 September - the player is a kit, and the app is one of its
   callers.** Diego: "Let's say I build an app that plays video. Can I use
   the videokit you are working on to include those capabilities in my app?"
   Yes, and that decides the shape rather than following from it: the
   playing lives in a library reached through the namespace - `video.open`,
   a frame for a moment, audio fed out - and `user/bin/video.lua` is a thin
   caller of it, as Music is a caller of `media.lua`. **Which decoder is
   behind it is not a fact its user should have to know** (`CLAUDE.md`, on
   kits): MJPEG today, H.264 when libavcodec lands, and not one call site
   changes. That is BeOS's Media Kit argument and the reason `use("/kits/x")`
   and `use("/lib/x.lua")` read the same at the call site.
4f. **AGREED on 19 September - the Game Kit: our own, for games and
   everything else that draws its own window.** It began as "vendor in
   love2d ... as we will be doing some apps that require love2d lua framework
   for graphics, audio and else", and became, the same afternoon, "can we do
   our own 2d and 3d library inspired In love 2d but kosmos optimized",
   "Like a GameKit", "That implements all required libraries in kosmos for
   ui, 2d, 3d, controllers, audio, etc.", and "Take the best ideas from
   love2d and use them wisely. I am not sure we will port many games or apps
   but build new ones on our platform like a audio editing and composing
   app, 3d planets simulations and other cool projects I have in mind".

   **Why not LÖVE itself**: it is the opposite of Lite XL on the three
   points that made that port work (`litexl.md`) - Lua 5.1 or LuaJIT against
   the 5.4 everything here runs, a large C++ core against small C, and
   OpenGL against a pixel buffer - and it needs threads in a process, which
   Kosmos does not have. Months of porting SDL, a GL and a C++ runtime before
   one rectangle, and then drawing slower than a surface already does.

   **What it is**: `use("/kits/game")`, BeOS's name - its Game Kit was
   `BWindowScreen` and `BDirectWindow` - for an application that owns its
   window's pixels and runs on a frame: a game, a planet simulation, an
   audio editor and composer. **Not a compatibility layer**: LÖVE's best
   ideas, taken on purpose, and nothing kept because LÖVE has it:
   - a program is a few callbacks - `load`, `update(dt)`, `draw`, and one
     per kind of input - and the kit owns the loop;
   - drawing is immediate: shapes, images, text, a transform stack, and
     offscreen canvases to draw into and then draw;
   - sprite batches, so a thousand sprites are one call into C;
   - sound as sources, a short one held whole and a long one streamed;
   - the game pad as a first-class input, the keys the USB driver already
     makes (`usb.md` 9);
   - and UI inside the window - a button, a slider, a list - for the tools
     an editor or a simulation needs.

   **What makes it Kosmos's**: it draws into the window's shared surface as
   Doom and the Super Nintendo do, and says which rectangles changed; every
   loop over pixels or samples is C; **a frame allocates nothing**, which
   `frames` can check, because a collector pause in a frame is the thing
   this system has learned to fear most; sound goes into the audio server's
   ring, never a message; and the loop runs on the window's frame, not on
   a clock of its own.

   **Rendering: software now, the GPU later, and the programs never know.**
   Diego: "we will implement real gpu 2d and 3d rendering in the roadmap so
   plan for that as well in GameKit", "It uses software rendering until we
   develop the hardware drivers". So drawing goes through one renderer
   interface with two halves: today the surface's C primitives for 2D and
   TinyGL (`/kits/gl`, `/lib/g3d.lua`) for 3D, and later the GPU (item 4h).
   A program written for the software half runs unchanged on the other.

   **First**, as an app is drawn before it is written, the kit is written
   before it is built: `docs/gamekit.md` - the API, one small program in it,
   and what each call costs - for Diego to change. Then the 2D half with a
   first game to prove it, then sound, then 3D.

   **STARTED on 21 September, and not in the order above.** The 2D half
   exists, and it arrived because the solar system needed it rather than
   because the document was written - which is a departure from *an app is
   drawn before it is written* and is recorded rather than quietly made.
   What justifies it: this is not an app and not an API anybody has to live
   with yet. It is a port of a rasterizer that already existed, function
   for function, so there was no design to show Diego - the shape was
   decided by `solar/soft.lua` years before Kosmos saw it. `gamekit.md` is
   still owed, and is now better informed for having one real user.

   - `user/lib/game.c` - `game.clear` and `game.line` against a surface
     *or* a Lua array, for a program that already has a framebuffer of its
     own. Written first, and **it was the wrong answer**: see below.
   - `user/lib/gamesoft.c` - `game.soft`, the whole rasterizer, owning a
     surface. Clear, blend, point, rect, frame, line, lineFast, circle,
     text, and lit textured spheres, ray-traced rings and the sun.

   **The measurement that decided the shape**, and it is the part worth
   keeping: C writing into a *Lua table* is **0.56x** - slower than the
   interpreter - because a store from C is the whole table API with its
   boxing and its barrier, about 85 ns a pixel. C writing into a **surface
   is 18.7x**. So the framebuffer had to stop being a table, and the moment
   it does, every function that indexes it has to move too. That is why the
   kit's first piece is nine hundred lines rather than two primitives.

   Measured under QEMU TCG at 960x540, Earth close-up, the whole scene:
   **6 to 8 times faster at every graphics level** - level 2 from 118 ms to
   14.4 ms, level 10 from 529 ms to 63 ms. TCG numbers are a ratio rather
   than a speed, and the ratio is the claim.

   **Held to the picture rather than to the speed**: `solar --compare` runs
   every primitive through both rasterizers and compares all 368,640 pixels
   with no tolerance, and `tools/run_game.py` is that in the gate
   (`testing.md` 18.128).
4g. **DONE on 19 September - the Super Nintendo keeps your game** (`testing.md`
   18.110). Diego:
   "Yes" to continuing a game after quitting. Two kinds of keeping, and the
   port has neither today:
   - **The cartridge's own save** - the battery-backed RAM a game like Zelda
     writes its save slots to. LakeSnes has `snes_saveBattery` and
     `snes_loadBattery`; written beside the ROM in `/home` when the game is
     quit and read when it starts, so a game saved in the game is there
     tomorrow.
   - **A save state** - the whole machine at one instant, anywhere in the
     game. `snes_saveState` and `snes_loadState`; one slot per ROM, saved on
     quit and offered on the next start, and carried across Double Size's
     restart so switching scale no longer loses your place.
4h. **WANTED since 19 September, not scheduled - real GPU rendering, 2D and
   3D.** Diego: "we will implement real gpu 2d and 3d rendering in the
   roadmap". The order is in the plan of 12 September: **virtio-gpu under
   QEMU first** - a protocol of forty pages rather than Intel's manuals, and
   what grows `hal_fb_flush` - **then virgl**, where the guest's 3D commands
   run on the Mac's own GPU and the compositor draws each window as a
   texture instead of copying it, which is the real win: not a faster blit
   but no blit. A native driver for the ThinkPad's Intel GPU after that, for
   its own sake. The Game Kit (4f) is the first program-facing user, and its
   renderer interface is where the switch happens.
4i. **WANTED since 19 September, not scheduled - low-latency audio.** Diego:
   "I plan kosmos to be a multimedia monster so we need to have low latency
   audio". An audio editor and composer (4f) is the program that asks it: a
   key played on a keyboard has to sound before the ear notices the gap,
   which is a few milliseconds, not a frame. **Measured first, because
   nobody has**: the period is 5.8 ms (256 frames at 44.1 kHz) and the ring
   several of them deep, but the time from a key to its sound - through the
   window manager, the program, the ring, the server's mix and the
   controller's own buffer - is not a number anybody has. On the ThinkPad,
   because QEMU's audio passes through the Mac's and says nothing about the
   machine. Then the shortest period the controller and the scheduler
   hold without a gap, and a priority band for the thread that mixes.
4i-b. **WANTED since 19 September - where the fortnight went: IPC costs 70%
   more and a context switch 44% more than on 5 September** (`testing.md`
   18.118). Measured at three commits, so it is attributed rather than
   suspected: the growth is everything between 5 and 19 September, which is
   the SMP programme - per-core runqueues with a lock each, a lock on every
   pool and every endpoint, the preemption path, the IPI. Some of that is
   the price of four processors being correct and some may be a lock held
   longer than it needs to be; nobody knows which, and the way to find out
   is to bisect those two numbers over the fortnight's commits. **And to run
   `make bench` where it would be seen**: it is in neither `make test` nor
   `make prepush`, which is why two weeks passed.

4j. **AGREED on 19 September - threads in a process.** Diego: "Why don't we
   add threading? To the kernel!", "Every modern os has multi threading as
   well as multi processing", "Threading will give us a lot of room for
   speed and super responsive ui and os". Refused until now in `design.md`
   4.5 on an argument written for one core; a process is homed on one of
   four (eight on the ThinkPad), so one program can use one core, and the
   media work above - decoding, mixing, rasterising, simulating - is what
   wants more. **The kernel part** is threads that share a process's address
   space and capabilities, each homed on its own core, with a way to wait
   and wake that costs nothing when nobody waits; `struct process` holds one
   thread today and about forty places in the kernel say so. **The rest**:
   per-thread storage for `errno` and the allocator's lock in the libc, C
   threads for kits, and Lua threads each with a state of their own and
   channels between them. **Written first as `docs/threads.md`**, in steps
   each checkable on its own as SMP's were: a second thread on the same
   core; on another core; exit and kill across cores with memory being
   unmapped under them; the libc; the kit; Lua. Every step a test.
   **`docs/threads.md` is written** (19 September): the inventory of what
   assumes one thread, the design, ten steps, and four decisions Diego's.
   **Its step 0, DONE the same evening (`testing.md` 18.111), was two bugs
   found on the way, wrong today**: a shared
   region's reference count is a plain `++` and `--` reached from several
   cores (`memobj.c`), and a spawn that fails early leaks its process slot
   (`process.c`).

   **Where threads go, Diego's question the same evening** - "I want to
   follow the beos idea which is as really great", "Can we have threads
   where makes sense? In c? What about Lua apps?":
   - **In C, wherever there is a heavy loop**: a kit's decoder, mixer or
     rasteriser, Music decoding ahead, the window manager composing on one
     core while another answers input. The caller does not change - the
     same `use("/kits/...")`, finishing sooner.
   - **In Lua, a thread is an interpreter of its own**, running its own file
     on its own core, and threads talk through channels - a value sent, a
     value received - never shared tables, so the races a lock exists for
     cannot be written in Lua. **Each has its own collector**, so a worker's
     pause never reaches the thread that draws.
   - **BeOS's idea, kept; its mechanism, not.** What BeOS promised was that
     a window never waits on work, and it bought that with two threads per
     window in C++ with locks - which is also why it was hard to program.
     Here the window's thread draws and answers input and nothing else,
     work goes to workers, and a worker's answer arrives in the window's
     event loop as an event, like a click. Coroutines stay for waiting on
     many things at once.
5. **NEXT - Kosmos looking like its mockups.** Diego, 18 September: "i love
   the tabs in the windows like BEOS instead of the full windoe tab like we
   have today", "can we have a appearance setting to switch between full tab
   like windows or linux or beos", and "i would like to polish the entire
   Kosmos UI in several ways. The screenshots you design look great but the
   UI then lacks polish in certain areas, like color schemes, ui layouts,
   spacing in widgets, fonts", "the fonts used in the screenhot look great".
   Three parts:

   1. **DONE on 19 September - the title's shape, a setting in
      Appearance** (`testing.md` 18.107): BeOS's tab, as wide as
      the title, or a bar across the whole window as Windows and Linux draw
      it. The code already calls it a tab (`TAB_H` in `wm.lua`) and draws it
      full width. The tab by default, being the one Diego prefers.
   2. **DONE on 19-20 September - IBM Plex as the default faces**, with the
      style guide (`docs/styleguide.html`): Diego, "Style guide looks
      great", which takes the page as drawn - **`beos` as the default
      theme** instead of starting dark, and Plex in its roles. Plex Sans and
      Plex Mono were already in `assets/fonts/` with their licences and
      unused, every default face being `spleen`, the 8x16 bitmap; the four
      roles are Plex now, with a fifth, `heading`, that the guide names and
      the kit had no role for. What it cost was real and was paid: the
      display harness finds rows by the 16-pixel face they were written
      for, so it pins its own look, and a check on the default runs before
      that pin.

      **Plex Sans Condensed was downloaded and vendored** with its licence,
      and the first desktop the change drew was wrong - for reasons that
      were not the fonts (`testing.md` 18.123, `ui.md` 16.16). The window
      manager had never loaded its own faces, applying only what
      `/home/.appearance` named, so applications laid themselves out in Plex
      while the process that draws their text still held the bitmap; and
      `gc:text` clipped every string by counting cells, exact for a bitmap
      font and a lie for any other. Both bugs were older than this item and
      both were hidden by the default being the bitmap - as were the two
      checks that should have caught them, neither of which could fail as
      written. All four fixed on 20 September.
   3. **The polish, drawn first and APPROVED on 19 September**: the style
      guide (`docs/styleguide.html`) - colours, the spacing scale, the type
      sizes, and every widget beside what it looks like today - is accepted
      as drawn, and is applied one application at a time, each photographed
      against its drawing.

      **And the widgets that still count cells**, found reviewing the fix
      above and left for this part rather than pulled forward: `ui.lua`'s
      text field places its caret at `4 + (caret - from) * GW`, highlights a
      selection `#shown * GW` wide and scrolls by `(w - 8) // GW`, where
      `GW` is `gfx.font.w` as it was when the file loaded. That is the same
      mistake as the clipping, in the one widget where being a few pixels
      out is felt on every keystroke, and the same answer applies: measure
      the prefix. The terminal already asks, through `cell()`.
5b. **AGREED on 20 September - Appearance, redrawn.** Diego, looking at it
   with IBM Plex in force: "i realised we need to revisit this appearance
   app as it needs a better design, spacing, and ordering of widgets since
   we added a lot and is too packed and small", "let's work on the design of
   this app as a part of the entire UI redesign we are working on with the
   new style layout we worked". It is seven groups stacked in one 380-pixel
   column - Palette, Desktop, Fonts (three lists side by side), a button,
   Wallpaper, Window titles, and a status line - with lists three rows deep.
   **Drawn first**, as every app is (`docs/appearance.html`), shown to him
   and changed until he agrees; the code follows the page.

   **Drawn and agreed the same afternoon.** Four questions, and his answers:
   "1 two columns, 2 miniature desktop, 3 yes, 4 nothing".

   - **Two columns**, 668 x 530 rather than 380 x 580 - wider and *shorter*,
     with lists six rows deep. Palette, desktop colour and wallpaper down
     the left; type and the window's shape down the right.
   - **A miniature desktop for the preview**, which is the expensive answer
     and the right one: a little window drawn in the palette and the faces
     being chosen, with a title bar, a heading, two buttons, a list, a
     paragraph and a terminal line - **every role at once**, and what a
     palette and a face look like *together*, neither of which a line of
     sample text can show. BeOS's panel did this.
   - **Headings become a fifth role** (`heading`, Plex Sans Bold 18): it
     arrived with the style guide and applications have been using it while
     the panel pretended it did not exist.
   - **Each role reports what it is set to** on its own row, so the panel
     answers the question you came with before you touch anything; and
     *Palette default* moves beside the palette it resets and says what it
     does.
   - Nothing else added: a light/dark switch, the pointer's size and the
     double-click speed were considered and left out, because none of them
     exist to be set yet.

5s. **AGREED on 21 September - a theme called Plex, which looks exactly
   like the mockups. Colours and faces DONE on 22 September** (`testing.md`
   18.135, `ui.md` 16.9), **and the spacing inside a widget** - list rows,
   buttons and fields (18.136). **Left: the spacing between widgets**, which
   every application sets with its own numbers - 12 and 10 in Appearance,
   14 in Music and Shortcuts, 4 in the Deskbar - so one theme-wide gap moves
   some of them under every theme. **AGREED on 22 September**: Diego, "yes
   to theme spacing" - and **superseded the same day by 5x**: after using
   it, "we should have a fixed widget layout and just use fonts that adhere
   to the widget and windows layout". Spacing is the kit's and fixed, not a
   theme's. Diego, on `docs/indicators.html`: "i love the font
   used in the mockups", "can we create a theme for kosmos that has this
   exact fonts selection, sizes and all and add it to the available
   themes?", "the theme is called Plex", and "i want the theme to look
   exactly as the mockup, same fonts same sizes same spacing, same colors".

   **What that asks of the theme format, which today is a palette and
   nothing else** (`themes.lua`, `theme.read`): fonts and spacing are not
   part of a theme. The five roles are set one by one in Appearance and
   kept in `/home/.appearance`, and the spacing - padding, row heights, the
   bar's height, the gaps between things - is numbers in the kit. So Plex
   is three pieces of work, in order:

   - **colours**: a fifth shipped theme, `plex`, from the mockups' own
     values - the desktop `#3d63b8`, the bar `#e7e7e3`, panels `#f4f4f1`,
     rules `#cfcfc9`, ink `#1e1e1e`, dim `#6c6c66`, accent `#2a55c9`, good
     `#2f8a3e`;
   - **fonts**: a theme names a face and a size for each role -
     `font.ui = ibmplexsans 13` - and Plex's are Plex Sans, Plex Sans
     Condensed SemiBold and Plex Mono at the mockups' sizes. Diego: "then
     we either need to add font selection to the theme?", and then "like a
     theme is a complete color scheme + font selection?" - **yes: a theme
     is complete**, colours, fonts and spacing, every one written in its
     file. Proposed first was a theme that could leave the fonts out and
     so leave them alone; that is a completion rule the reader has to
     know, which is what *explicit over hidden* rules out. So Photon, BeOS,
     Platinum and IRIX get the faces and spacing they use today written
     into them and look exactly as they do; choosing any theme sets all of
     it; and a role changed afterwards in Appearance is kept as the
     person's own on top of the theme, and shown as such;
   - **spacing**: the kit's paddings and row heights become tokens a theme
     can set, with today's numbers as their defaults.

   **The mockup is a web page and Kosmos is not**, so which element of it
   is which role is a decision, not a reading: the page's 16 px running
   text against a 14 px widget, a 42 px page title against a window's
   title bar. That mapping is written down and agreed with Diego before
   anything is coded: **`docs/plex.html`, 21 September** - every colour,
   face, size and spacing value with the mockup it comes from, the ones no
   mockup draws marked as proposals, and four choices for Diego where the
   mockups disagree or the kit would need new drawing. **His answers, 22
   September**: widgets in Plex Sans 14; reading text in Plex Sans 16;
   headings in Plex Sans SemiBold, a file added from the same family; and
   no rounded corners and no shadow for now.

5zb. **DONE on 22 September - a released stick in one command**
   (`tools/getstick.sh`, `testing.md` 18.146). Diego, on his MacBook Pro,
   after writing 0.10.115 from GitHub by hand: "can we make 1 single script
   that does all this commands at once? just passing by argument the version
   number to download and write to the stick?" `bash getstick.sh 0.10.115`
   downloads, checks against the release's `SHA256SUMS`, unpacks, and hands
   over to the release's `mkusb.sh`; with no version it lists the releases
   that have a stick. **Every stick release carries `SHA256SUMS` from now
   on**, and an empty `/home`.

5za. **DONE - icons at 16, 32 or 64, chosen where they are shown**
   (`testing.md` 18.148, `ui.md` 16.19). Diego: "with the new icon sizes we
   should also be able to select icon size on desktop, tracker icon view
   and else", and then, of the sizes: "16,32,64 are the correct ones" - the
   three Haiku exports that the image carries since 0.10.112 (5v), so at
   100 per cent every icon is drawn pixel for pixel, with nothing averaged.
   **Sizes are points**, like every other size once there is a scale (5z):
   a 32 at 150 per cent is 48 pixels, averaged down from the 64 by the
   window manager, which upgrades an icon's source itself.

   Built: `/lib/iconsize.lua`, the three sizes and the choice, kept per
   place in `/home/.tracker` under a key for each - `desktop_icon_px` and
   `window_icon_px` - and read when the place opens. Tracker's cell is the
   sum it always was and now says so - `max(84, px + 52)` by
   `px + 8 + 2 * GH`, the 84 by 72 it was at 32 - so the grid follows the
   pictures. **Two places, and they turned out to be two**: the desktop,
   where the only way to a menu is a right press on the background, and a
   Tracker window's View menu, which offers the sizes only in icon view
   because a list has no icons in it.

   The width was the *label's* first, which is 84 at every size, and Diego
   said what that looked like at 64: "yes widen the cell at 64". It is the
   icon with the same 26 pixels either side that the old 84 gave a 32, and
   never below 84, so only Large widens and a name that was cut to
   `cheats~.html` fits whole.

   And two things that came out of it. A menu item can be **marked** -
   a diamond in a column of its own, `mark` in `ui.lua` - so a menu of
   choices says which one you are looking at; the View menu marks its
   layout, its sort column and its icon size. And a menu bar's `items` may
   be a **function**, worked out when the menu opens rather than when the
   window was made, which is what lets a mark say what is true now.

   **A bug it found**: `fit_backdrop` moves the desktop below the strip and
   told it with a `resize` and no `moved`, so the desktop's idea of where
   it was on the screen stayed 0. Nothing had ever needed it, because the
   only thing that does is a menu and the desktop had none - the first one
   opened 32 pixels above the pointer, over the Deskbar.

   **Left**: nothing else draws a grid of icons today. The Open/Save panel
   is a list and has none; a place that grows one takes a key of its own
   and the same two lines.

5z. **APPROVED on 22 September - a size for everything, not for the
   fonts: a scale. The first two stages DONE, 0.10.115** (`testing.md`
   18.145, `ui.md` 16.18). Drawn in `docs/looks.html` and approved - "the
   proposed size slider is great as it is", "with the %" - and then Diego:
   "start on the size slider". Built: the window manager at a scale, read
   at startup and applied at its edge with each window - opening, drawing
   commands, commits, events, its own chrome and faces, own-pixel windows
   stretched - and changed with windows open from Appearance's Size row,
   which rebuilds every window at its new size. **Left, the third stage**:
   a way for an application to learn the screen's size in points *before*
   it opens a window - Lite XL sizes its buffers from `/dev/screen` first,
   so at 150 its bottom is cut; the window manager's own drawings that are
   not windows - the pointer, the level bar, a drag's label, the launcher
   pad; the small offsets inside the title bar's boxes; and a menu strip's
   padding over an own-pixel window. Diego, on the
   ThinkPad with 0.10.111: "fonts are too small on a 1920x1080 14 inch
   panel like the t14", "Make them 2 point bigger all of them and the
   window tab title 4 points bigger" - and then, before any of that was
   built: "What we can do is add a setting like Windows does that allows
   you to use the UI at a certain magnification factor", "currently it's at
   1x and you can basically put it in a 1.2x, 1.5x, even 2x", "like a
   magnification for the UI instead of choosing independent font sizes",
   "a factor multiplier of all the things in the UI", and, with a picture of
   iOS's Larger Text: "Something like that slider of iOS" - a stepped
   slider, a small A at one end and a large A at the other, with detents.

   **So the fonts were not made bigger.** A face that grows alone is what
   broke windows before 5x; a scale grows the layout and the faces
   together, so 5x holds at every step, and a larger face on the ThinkPad is
   the scale's job.

   **Proposed, in the drawing** - the Size row in Appearance, a slider of
   seven steps: 100, 110, 120, 135, 150, 175 and 200 per cent, which
   includes the three Diego named. Kept in `/home/.appearance` with the
   look and the wallpaper. At 150 the ThinkPad's 1920 by 1080 is 1280 by
   720 to a window, and a window larger than the screen it is given has to
   be fitted to it.

   **How it would be built** - applications keep their numbers: sizes and
   positions stay in the units they give today, and the scale is applied
   where pixels are made. The window manager already draws every kit
   window from its commands (`gc` in `ui.lua`), so it multiplies them as
   it draws; each face is loaded at its size times the step, so text is
   drawn at that size rather than magnified; icons are averaged down from
   Haiku's 64s (`stretch`'s `smooth`, 5v); a window that draws its own
   pixels - a game, a film, the cube - is told the step and chooses for
   itself; the pointer is divided back on the way in; and the window
   manager's own chrome, the tab and the border, scales with the rest.

   **The drag, found on the ThinkPad and fixed** (0.10.116): Diego at 110
   per cent, "if you grab a window by the titlebar ... you will see the
   mouse is off by a margin", "even worse with more scale". The window
   manager's own drag called the handler an application's `move` goes
   through, with the screen's pixels - and that handler multiplies what it
   is given, so the window went 1.1 or 1.5 times as far as the pointer and
   slid out from under it. `move_window` places a window in pixels;
   `handlers.move` converts points and calls it, as `resize_window` and
   `handlers.resize` already did.

   Every step is a test at 100 and at 150 per cent. **The steps are
   agreed as drawn**, the percentage beside the slider included; the
   design goes into `ui.md` before code.

5y. **DONE on 22 September - very few options: four looks, and nothing
   else to tune.** Diego, straight after 5x: "Too many config
   options make the system vulnerable to changes and complicated", "We
   should stick to very few options in colors and fonts", "To maintain
   clarity and efficiency" - and, correcting a proposal that kept the old
   themes and trimmed around them: "This is different from what we were
   looking for. Let's just make 3 or 4 good design options in colors and
   fonts and stick to those."

   So a **look** is a whole: colours, faces, the Deskbar's colour and the
   desktop's, designed together, on the one fixed layout of 5x. Somebody
   picks a look, not its parts. Four, drawn before anything was built
   (`docs/looks.html`) and **approved on 22 September** - "Those 4 looks are
   great": **Plex**, as built, and the default; **Plex Night**, the same in
   the dark; **Classic**, BeOS's grey and yellow in Plex type; **Studio**, a
   near-black look for the media applications with one warm accent. Built
   in three steps: **the looks as the themes - done, 0.10.109** (`testing.md`
   18.139); **the fixed layout - done, 0.10.110** (5x); and **the Appearance
   panel down to a look, a wallpaper and the Deskbar's height - done,
   0.10.111** (18.141), built from the drawing in `docs/looks.html` after
   Diego's "the panel is right, build it". The
   per-role faces and sizes, the colour swatches, the title's shape and the
   other themes left the Appearance panel, and the Deskbar's height left it
   in 0.10.112 (5v): what stays beside the look is the wallpaper.

   **And a scrollbar's thumb in the look's tab colour - done, 0.10.113**
   (`testing.md` 18.143). Diego, 22 September, with a picture of Mac OS 9's
   Appearance control panel: "i want the scrollbar handle to be colored
   after the tab bar color as an accent color like how macos 9 had it". The
   thumb is filled with `tab` and carries Platinum's grip, four raised
   ridges across its middle; the trough and the arrows stay grey. Every
   list, tree and text view gets it from the kit, and the browser draws the
   same thumb into its own pixels. **Not done: greying it in a window that
   is not in front**, which Mac OS 9 also did - a kit window does not know
   whether it is focused, so that needs the window manager to say.

   **And three more the same afternoon - done, 0.10.114** (`testing.md`
   18.144), all Diego's on 22 September:
   - **A title bar across the whole window**, not a BeOS tab: "i want to
     switch back the tabs from be os style to full width". Not a setting:
     the shape saved by the old panel (`tabs` in `/home/.appearance`) is
     read by nothing, which also retires the leftover found while the
     thumb was written up - a machine whose file said `beos` would have
     kept the tab under a new default alone.
   - **The Deskbar in the look's tab colour**: "the deskbar tab color
     should be yellow or at least the same color of the acccent color of
     the theme". `bar` and `bar_text` are gone from the theme format, so
     the tab, the Deskbar and a scrollbar's thumb are one colour that no
     look can let drift apart. Plex's Deskbar is its yellow now, not the
     stone the mockups drew; Classic's is yellow where R5's was grey.
   - **A maximise box greyed, not removed**, on a window that cannot be
     maximised: "when a window cant be maximixed we shouldnt remove the
     button we should just gray it out and disable it". Flat, its glyph in
     `text_dim`, and a press on it does nothing - before, with no box
     there, the same press took the window by its title.

5x. **DONE on 22 September (0.10.110, `testing.md` 18.140) - one fixed
   layout, and faces that fit it.**
   Diego, after using Plex at 16 on the ThinkPad: "I realized the changing
   of spacing on fonts alter the window widget placing and brakes it. We
   should have a fixed widget layout and just use fonts that adhere to the
   widget and windows layout", and then "And that layout is fixed".

   **This reverses the direction of 5s's spacing half, 0.10.106's padding
   and the Appearance panel's relayout (0.10.106):** there, a row was as
   tall as its face plus a theme's padding, so a larger face moved every
   widget below it - and an application that places its widgets at fixed
   positions broke. Now the geometry is the kit's and it does not move:

   - **Fixed sizes, in pixels, the same in every theme**: a list or menu
     row, a button, a field, a window's tab, and since 5v the Deskbar.
     Chosen so that 16-pixel IBM Plex fits, which is the size the ThinkPad
     wants.
   - **A face fits its box or is not allowed at that size.** Each role
     has a largest size its box can hold; Appearance offers only sizes that
     fit, and the kit clamps anything larger - a theme or a saved setting
     that asks for more gets the largest that fits, and the log says so.
   - **Text longer than its box is cut there**, never spilling onto the
     widget beside it.
   - **Themes change colours and faces, not geometry**: `row_pad`,
     `button_pad` and `field_pad` leave the theme format, and the Appearance
     panel lays itself out once, like any other window.
   - **The Deskbar is 32 and part of the layout** (5v). It was Diego's
     choice among three heights for an afternoon, until he fixed it.

5w. **WANTED on 22 September - the Deskbar collects its events while a menu
   is open.** On the ThinkPad with 0.10.106, after a run of Deskbar menus:
   `wm: Deskbar is not collecting its events; dropped a mouse move`. The
   window manager bounds a window's queue and drops when it is full, which
   is right; what is not right is the Deskbar stopping its polls while one
   of its menus is up, if that is what happened - a menu is a window of its
   own, and the bar should go on answering underneath it.

5v. **DONE on 22 September - the Deskbar is 32 pixels, fixed** (0.10.112,
   `testing.md` 18.142). Diego, on the ThinkPad: "Taskbar size should not
   be changeable let's make it fixed at 32". It is `theme.metrics.deskbar`,
   beside the rows, buttons and tabs of 5x, and nothing chooses it: the
   Appearance panel, the theme format, the window manager and
   `/home/.appearance` carry no height, and one a `/home` saved before is
   ignored. A 32-pixel icon in a 32-pixel bar touches both edges, so its
   icons are 24 - Haiku's 64-pixel exports averaged down (`stretch` grew a
   `smooth` mode for it), with the 16s and 64s vendored beside the 32s
   (Diego: "We can download haiku icons in 16 and 64 as well"). The tab
   height `theme.metrics` gave, 20, had been wrong since the tab grew to 26
   for the ThinkPad; it is 26, and the window manager reads it from there.

   **What it was, for an afternoon** (0.10.108, 18.138): Diego asked "i
   want to be able to change the deskbar height for instance, where do i do
   that? is there a file?" - it was `local H = 36` in `/bin/deskbar.lua` -
   and then "both": a theme named `bar_h`, and Appearance offered 36, 44
   and 52, kept in `/home/.appearance`.

5u. **WITHDRAWN on 22 September - the Deskbar's colour, chosen by the
   person** (`testing.md` 18.137, then 18.141). Built as 0.10.107, and an
   hour later the looks (5y) decided colours as a whole: the bar's colour is
   its look's, and the choice is gone from Appearance and the window
   manager.
   Diego, on seeing Plex's stone bar where BeOS's was yellow: "is that a
   setting?", "a color in the theme?", and then "keep the deskbar user
   selectable color". Each theme names its bar (`bar`, `bar_text`, since
   0.10.105) and that is where it starts; Appearance offers the bar's colour
   the way it offers the desktop's, and the choice is kept in
   `/home/.appearance` over whatever the theme says, and survives a restart.
   The words on the bar follow the colour chosen, dark on a light bar and
   light on a dark one, so no choice leaves them unreadable.

5t. **WANTED on 22 September - why the boot stick stops answering.** On
   the ThinkPad with 0.10.105, two READ (10)s to the stick Kosmos booted
   from got no answer within a second, 21 seconds after boot, and the
   stick was reset; the recovery then left the bulk OUT pipe in a state Set
   TR Dequeue Pointer refused (fixed, `testing.md` 18.136), and later that
   session `/home/.appearance` could not be written. Why a stick that reads
   fine stops answering for a second is not known, and it is the kind of
   fault that loses a setting quietly. The next stick logs the endpoint's
   state if recovery fails again, and Appearance logs why a save failed.

5r. **WANTED on 21 September - a deadlock report that names the holder.**
   `arm-display-2` panicked once at boot with `spinlock: endpoint held by
   1, wanted by 2` and did not again in a rerun or thirty boots under the
   same load (`testing.md` 18.134). The report says who waited and not
   what the holder was doing, so a real deadlock and a paused host thread
   read the same. The waiting core could ask the holder for its PC and
   stack - an IPI on both boards - before it panics, and the bound could
   be measured on the counter rather than in spins, since ten million is
   ten milliseconds on one board and a second and a half on the other.

5q. **AGREED on 21 September - a USB indicator in the bar.** Diego: "i
   also want a usb top bar indicator that upon clicking shows all things
   connected and detected on the usb ports". An icon in the Deskbar, beside
   the network's (5p), and a click that lists every device on every port:
   what it said it is - vendor and product, its name, its speed - and **what
   Kosmos does with it**: the mouse it reads, the stick it mounted as
   `/home`, the pad whose buttons are keys, the Ethernet adapter it named
   (5m), or the class of one nothing here reads.

   The driver already knows all of it and says it only to the console:
   `use_device` decides each device's kind, and since 5m-a a device that is
   none of them is named by its class rather than passed over. What is
   missing is a way to ask: the xHCI driver is a process, so the list comes
   from it through a small declared-struct query (`CLAUDE.md`, *a server
   receives exactly what it expects*), read by a desklet the way the
   battery is read - not by parsing the console. Plugging and unplugging
   changes it, which the driver already sees (`usb_hotplug`). **Drawn**,
   with 5p's since the two sit side by side, as `docs/indicators.html` on
   21 September - the ThinkPad's own devices as the driver named them, and
   four choices for Diego at the end. **His answers**: the devices inside
   the machine - the camera, the fingerprint reader, Bluetooth - are
   listed with the rest; and the icon counts every device, with the panel
   saying which Kosmos drives and which it does not - "no drivers
   available for example" - so the count and the list agree.

5p. **AGREED on 21 September - a network indicator in the bar.** Diego:
   "we need a topbar network indicator icon and status", "so we know the
   adapter is connected and the network is on". An icon in the Deskbar,
   beside the battery and the volume, whose states are the driver's own
   steps rather than a guess:

   - **no adapter** - nothing plugged in that the driver can drive, and
     the icon **greyed out** rather than gone (Diego's choice, below);
   - **an adapter, no link** - 5m-a names it today, and its MAC is known;
   - **a link**, and its speed - from the adapter's `NETWORK_CONNECTION`
     and `CONNECTION_SPEED_CHANGE` notifications, 5m-b;
   - **on the network** - an address, which needs the frames in `net.c`,
     5m-d, and an address to be given one.

   **There is an icon already, and it says nothing.** `deskbar.lua` draws
   `Prefs_Network.png` whenever the network stack answers
   `fs.net_info("/net")` - on QEMU always, on the ThinkPad never, since no
   card there is driven - and it has one state, present. So this is that
   icon given the four above, drawn differently for each, and a click
   that says them in words: the adapter's name, its MAC, the link's speed,
   the address. It is built with 5m-b, because before that the only true
   states are the first two. **Drawn** as `docs/indicators.html`, and
   Diego's choices the same evening: no adapter is a greyed-out icon, not
   a missing one; the link's speed is in the panel only, not in the bar. The Wi-Fi card would be a fifth kind of
   adapter behind the same icon, when there is a driver for one.

5o. **WANTED on 21 September - a render resolution for the solar system.**
   Diego, after 18.131: "the solar simulator works great in fullscreen but
   i do need a way to set the render resolution as it looks very
   pixelated in fullscreen". It draws at 960x540 and `stretch`es that to
   the screen, so on the ThinkPad every pixel is four. The rasterizer is C
   now and ran at 100 fps at maximum detail, so there is room to draw at
   the screen's own size; what is wanted is a choice - a scale beside the
   graphics level, carried across the relaunch that full screen does -
   rather than a fixed number swapped for a bigger one.

5n. **DONE on 21 September - a wallpaper that comes back** (`testing.md`
   18.133). Diego: "the appearance app does not rememver the wallpapers
   and other things upon restarting". The fonts were remembered; only the
   wallpaper was lost, because his plain-colour PNGs are palette images
   and the decoder refused them - and the window manager threw the reason
   away, so nothing said so. `png.c` reads colour type 3 with `tRNS`, and
   `wm` names the wallpaper it restored or why it could not.

5zd. **WANTED on 22 September - the ThinkCentre M700, and what it needs.**
   Diego: "i have some fantastic news.. i bought a lenovo thinkcentre m700
   mini pc and kosmos boots!! it works!". A 10J0/S1CK00, a 6th-generation
   Core i7 with HD Graphics 530, and **Kosmos came up on it first time** -
   the second real machine, and the first that is not the ThinkPad.

   What it needs, in the order it was found:

   - **5zd-a. A resolution worth having, and as large as the screen will
     take.** Diego: "lets go with as much as possible as currently the
     monitor supports 3440x1440 and it already worked under linux in the
     same lenovo m700". It comes up at 800x600, which is the firmware's mode
     when nothing asks for another. The loader picks a mode through UEFI's
     Graphics Output Protocol, so this is asking GOP for the modes it has
     and choosing the largest rather than taking what it hands over
     (`boot.md`). HD 530's own driver is a far larger thing and is not what
     this is - the firmware already drove 3440x1440 for Linux on this
     machine, so the mode is there to be asked for.

     **What it will cost elsewhere**: 3440x1440 is 19.8 MB of framebuffer
     against 800x600's 1.9, and the compositor's budget, the screenshot
     harness and every "the screen is 1920x1080" assumption meet a wider one
     for the first time (`roadmap.md` 4k-and-no-hard-limits). A mode to
     choose also wants a way to say which - a boot option before there is a
     panel.
   - **5zd-b. A USB keyboard.** Diego: "usb mouse works, but usb keyboard is
     yet to be added", and "we need to add support for usb keyboard!". The
     mini PC has no PS/2 port, so `hal/pc/i8042.c` finds nothing and the
     machine has a pointer and no keys. A boot keyboard is the same shape
     as the boot mouse the driver already reads - an interrupt IN endpoint
     and a fixed report (HID 1.11 B.1) - so this is `usb.md` step 9 beside
     the pad rather than anything new.
   - **5zd-c. A keyboard on the screen, driven with the mouse.** Diego, on
     the same afternoon: "we coould do a very simple virtual keyboard app
     that would help in these cases". It is the answer to a machine that has
     a pointer and no keys, and it goes on being useful afterwards - a
     keyboard nothing recognises, a keyboard unplugged, and a touch screen
     when there is one.

     **It needs no new authority**, which is the reason to build it this
     way: `SYS_KEY_PUSH` is device authority and an application has none, so
     the keys go to the *window manager* as a message and out to the focused
     window exactly as a real key does. What that does not reach is the
     console at the bare prompt, which is right - a window is what an
     application may type into.

     **Drawn before it is written**, as every application here is
     (`CLAUDE.md`): `docs/keyboard.html` first, and the code after Diego has
     looked at it.

5m. **AGREED on 21 September - a USB Ethernet driver, and the remote
   debugging it unlocks.** Diego: "what if we build a way to connect this
   and the remote machine via a simple protocol over the network so that
   the thinkpad does things and you can read the log and anything you need
   to try or run on the thinkpad from here... a remote debugging
   mechanism. is it possible?" - and then, the same evening, "I got the
   usb Ethernet dongle".

   **It is possible, and the expensive half is already done.** The ThinkPad
   has no network Kosmos can drive: its only card is `8086:a0f0`, an Intel
   AX201 over CNVi, which is firmware upload and a vendor host interface.
   But the dongle enumerated on the machine with nothing written for it:

       xhci: 00:0d.0 port 4: 0bda:8153, USB 3.0, class 0, "USB 10/100/1000 LAN"

   `0bda:8153` is a Realtek RTL8153. Bulk IN and bulk OUT - the same shape
   as the mass-storage stick that already works there (`usb.md` 6, 7).

   **Step 0, and it decides everything, and it needs no stick.** The device
   says `class 0`, which means "read the interface descriptors". The
   question is whether it offers **CDC-ECM or NCM** - published USB class
   specifications - or only Realtek's vendor interface, which is documented
   in public only by Linux's `r8152.c`.

   That distinction is not a preference, it is the licence. Kosmos is MIT.
   Working from a GPL driver's *expression* would make this file GPL;
   working from a published class specification would not. If the dongle
   speaks ECM or NCM, the driver is a standards-based one we may write
   freely, and is simpler besides.

   **And it is answered here rather than on the ThinkPad**: QEMU's
   `usb-host` passes a real device through to the guest, which is how the
   8BitDo pad was tested. Plug the dongle into the Mac, pass it to QEMU,
   and let Kosmos's own driver print its configuration descriptors.
   `simulate-dont-wait`.

   **Step 0 answered on 21 September: it speaks CDC-ECM.** Plugged into
   the Mac, the dongle offers two configurations, and macOS runs it in the
   second with Apple's generic class driver - `AppleUserECM`, which knows
   nothing about Realtek:

       0bda:8153 "USB 10/100/1000 LAN", bcdDevice 0x3100, 5 Gb/s
       bNumConfigurations 2, current configuration 2
         interface 0  class 2/6/0  CDC ECM control, one interrupt endpoint
         interface 1  class 10     CDC data, alternate 1: two bulk endpoints
       MAC 00:e0:4c:68:02:86 (Realtek's OUI), MTU 1500

   Configuration 1 is the vendor interface `r8152.c` drives, and Kosmos
   will not touch it. **So the driver is a CDC-ECM driver, written from
   the USB-IF's class specification, and it is not an RTL8153 driver at
   all** - it will run any ECM device, which is most USB Ethernet that is
   not an ASIX.

   **And it can be tested in the gate with no hardware**: QEMU's
   `usb-net` emulates a CDC Ethernet device, so the driver's permanent
   test is a QEMU machine with one plugged in, and the dongle is for the
   ThinkPad and the last check. Which configuration value `usb-net` gives
   its ECM interface is for 5m-a's descriptor dump to say, not for this
   paragraph to guess.

   **Then, in order:**
   - **5m-a. DONE on 21 September - the descriptors** (`usb.md` 10,
     `testing.md` 18.134). The driver reads every configuration a device
     has, names an ECM adapter with its MAC and its endpoints, and says the
     class of anything it cannot read. Under QEMU's `usb-net`, and on the
     dongle itself through `usb-host` with no root needed: MAC
     `00:e0:4c:68:02:86`, configuration 2, frames on interface 1 setting 1.
     Moving frames on the dongle from the Mac will need root, as the pad
     did; the gate uses `usb-net`. **And on the ThinkPad itself, the same
     day** (0.10.105): port 3 of `00:0d.0`, the same two lines.
   - **5m-b. DONE on 22 September - the link brought up**, no frames yet
     (`usb.md` 7b, `testing.md` 18.149). The configuration chosen, the Data
     interface's alternate setting selected - ECM's Data interface has a
     zero-bandwidth alternate 0, and leaving it there is the classic reason
     a correct-looking driver never receives anything - the packet filter
     asked for, and the link's notifications listened for the way a mouse's
     reports are: one read outstanding, the next queued when it comes back,
     so nothing polls and no plug waits.

     **The setting is read back with GET_INTERFACE**, because it is the one
     request in the sequence whose failure is invisible until a frame is
     expected. The line prints what the device says, so a driver that never
     sent SET_INTERFACE prints a 0.

     **QEMU's `usb-net` sends no notification while nothing moves** - forty
     seconds of an idle adapter produced none - and sends NETWORK_CONNECTION
     once frames do, which 5m-c's probe is what showed. Its speed never
     arrives, so CONNECTION_SPEED_CHANGE is read by `usb_decode_notify` under
     `test_usbdecode` on the host and will be seen first on the ThinkPad.
   - **5m-c. DONE on 22 September - frames, both ways** (`usb.md` 7c,
     `testing.md` 18.150). ARP is the right first traffic: small,
     unsolicited, and something on the other end answers without being asked
     twice. `opt/kosmos/ethprobe=<mine>,<theirs>` sends **two** requests and
     writes down the frames that come back.

     **Two, because the second is the zero-length packet's test.** The first
     is 60 bytes, Ethernet's shortest; the second is padded to a multiple of
     the bulk OUT endpoint's packet, which is exactly the length ECM 1.2
     3.3.1 wants a zero-length packet after. Without the rule the adapter is
     still waiting for the rest of a frame that has already ended and one
     answer comes back instead of two - watched as a control.

     **The probe stays, as a diagnostic.** From 5m-d the stack sends the real
     traffic; what this answers afterwards is whether the *adapter* is moving
     frames, separately from whether the stack above it is. Off unless the
     option is there.
   - **5m-d. DONE on 22 September - the frames reach `net.c`, and the
     machine is on the network** (`usb.md` 7d, `testing.md` 18.151). The
     stack called `kosmos_net_send`/`recv`, which are *syscalls* into the
     kernel's virtio driver; the kernel must not learn what USB is, so the
     stack takes frames from the **driver process** through a region -
     control by message, data by shared memory, single-producer
     single-consumer rings with indices, as `CLAUDE.md` says and as the
     audio server's ring already does. `ethring.h` and `ethproto.h`.

     **`ping 10.0.2.2` through a USB Ethernet adapter, in 0.6 ms**, on a
     machine with no card the kernel can see.

     **Four faults it found, and none was the ring**: the driver's idle pass
     drained only mouse reports, so frames were read only while something
     else was waiting; the stack drained the wire *after* blocking on its
     next message, which cost every round trip a tenth of a second on the
     kernel's own card too; `process_wake_net` stopped at the first holder
     of the grant, which is init; and a machine with no card was given no
     address at all, so `ping` said "this machine has no address yet" with
     frames moving underneath it.

     **And three things the kernel owed a driver in userland**: a wake it
     can reach (`SYS_NET_WAKE`), a wait that watches three endpoints rather
     than two, and a wait on endpoints with no interrupt lines - which is
     what that driver has on a machine with no USB controller in it.
   - **5m-e. The debug server**, once there is a network: a small program
     that takes a request and answers with output. Deliberately small, and
     the same declared-struct discipline `audioproto.h` uses.

   **A caution to write down before it is built**: a server that runs
   commands on demand is a remote shell. On a wired link between two
   machines on one desk that is fine; it must be off unless asked for, and
   it must not be in a build that leaves the house.

   - **5m-f. WANTED on 22 September - WiFi.** Diego bought a USB adapter
     and plugged it into the ThinkPad: "Also I bought a WiFi adapter i
     plugged it in and see the log". Kosmos read it exactly and said what
     it could not do. It arrives as a 2 MB virtual disk, `0bda:a192`
     "Realtek USB Disk autorun" - the driver CD a "Driver Free" adapter
     carries for Windows - and nine seconds later unplugs itself and comes
     back as `0bda:f192`, "802.11n WLAN Adapter", class ff/ff/ff: "nothing
     here reads it". That is Realtek's RTL8192FU, and the dongle's
     300 Mbps is its two chains.

     **What it would take, and it is two pieces.** The *driver* is a USB
     device with no public datasheet, and Linux's `rtl8xxxu` is the best
     description it has - which is what it is for here (`CLAUDE.md`: a
     licence is never a reason not to use something). Above it there is no
     802.11 at all: scanning, association, the WPA2 four-way handshake,
     CCMP over AES and rate control, which is ours to write whatever the
     adapter is, and is the larger half.

     **Two ways in, and the choice is technical.** This dongle, with
     `rtl8xxxu`'s 8192F support as the reference; or **the ThinkPad's own
     Intel card**, `8086:a0f0` at `00:14.3`, which `neofetch` already
     reports as "not driven" and which OpenBSD's `iwx(4)` and `net80211`
     describe in ISC code that is closer in shape to what this system
     wants. The internal card also saves a USB port and works with the lid
     shut; the dongle is the one that can be moved to another machine.

     **Order**: after 5m-b and 5m-c, which put frames through the stack on
     metal over a wire first - WiFi is that plus everything above.

5k. **DONE on 21 September - a shared mapping gives its addresses back**
   (`testing.md` 18.132). Diego, on the ThinkPad: "the video player ran
   once with the mp4 mjpeg video but not a second time", "it looks
   something remained in memory", "that was broken after watching the
   video the first time".

   **It was the kernel, and not any of the four things it looked like.**
   `p->next_share` handed out addresses in a process's shared window by a
   pointer that only ever climbed. `SYS_SHARE_UNMAP` gave the *pages* back
   and kept the *addresses* spent for ever, so a server that maps and
   unmaps in a loop marched up its window until nothing more fit - and
   then could map nothing at all, for the rest of its life, however little
   memory the machine was using.

   The filesystem server maps the caller's whole buffer on every
   `read_into`, and the video kit's buffer is 4 MB, so one ten-second film
   spends over a gigabyte of a 4 GB window. Three films and the fourth had
   nowhere to go.

   Fixed LIFO - the mark comes down when the range returned is the one
   most recently handed out - which is a deliberate half-measure argued at
   the code: a free list needs somewhere to keep holes and this kernel has
   no allocator, a bitmap would be 128 KB a process, and every server here
   has exactly the shape LIFO serves.

   Found with it, and fixed: **`wm` never released a direct window's
   shared region** on close. Found and *not* yet fixed: `film:close()`
   never closes the audio stream where `player:close()` does, and the
   audio server never reclaims a stream whose client died - eight exist,
   and `appfs`'s `forget_the_dead()` is the pattern it wants (5l).

5l. **WANTED on 21 September - the audio server reclaims a dead client's
   stream.** There are eight; only an explicit `AUDIO_OP_CLOSE` frees one;
   nothing notices a client that died. `appfs` solved exactly this for the
   `/app` registry and its comment says the shape - "a full table has had
   its dead slots back before it says it is full". And `film:close()`
   should close the stream it opened, which today only `player:close()`
   does.

5g. **DONE on 21 September - a click is an event, not a state** (`testing.md`
   18.130). Diego,
   after 0.10.99 on the ThinkPad: "i found some quircks like the mouse
   buttons be unrespiosnive under certaun scenarios", on the desktop and
   the Deskbar.

   **The code already names this bug.** `user/servers/console.c` says it
   in the comment above `fill_pointer`: "A key is an event and a position
   is a state. Keys queue, so reading them a pass late loses nothing -
   they are all still there. The pointer does not queue." A press and its
   release can fall entirely between two samples, and then `on_click`
   never fires.

   It was half fixed: the sample moved to *after* the wait, which removed
   one wake of staleness. **The race is structurally still there**, and
   the same comment names the conditions - "a window whose repaint takes a
   dozen messages: the release then arrives while the manager is draining
   those". That is a busy desktop, which is what Diego was using.

   **The fix is to make a button transition an event.** `pc_pointer_move`
   already notices the change - `if ((buttons & BUTTONS) != held[from])` -
   and throws the edge away after setting a `moved` flag. A small ring of
   transitions there, drained by `SYS_POINTER` beside the position, makes
   a click as reliable as a keystroke, which is what it should always have
   been. The position stays a state, because a position genuinely is one.

   Nothing in QEMU reproduces it reliably, because the desktop there is
   never busy enough for long enough - which is exactly why it took real
   hardware to find.

5h. **DONE on 21 September - the solar system full screen, and its
   pointer** (`testing.md` 18.131). Diego, seeing it at 100 fps on the ThinkPad: "we just need a
   way to maximize the window and able to drag, rotate and else with the
   mouse".

   **Full screen is missing and is the easy half**: the Video app already
   relaunches itself with `--size full`, `wm` has the `fullscreen` window
   property, and `solar.lua` takes a size on its command line. The same
   shape, with the graphics level left where it was.

   **The pointer is already wired**, which is the interesting part.
   `solar.lua` forwards `pointerDown`, `pointerMove` and `pointerUp`, and
   the core orbits on a drag and zooms on the wheel. So if dragging did
   nothing on the machine, it is 5g rather than a missing feature - and
   that is the thing to settle before writing any code here, because
   building a second pointer path over a dropped-event bug would hide it.

5i. **The write that did not reach the stick** (`boot.md`). `diagnose` was
   run on the ThinkPad on 21 September and `/home/diagnose.txt` was not on
   the stick afterwards, read back on the Mac - while the rest of `/home`
   was intact. Two explanations and they are very different: the write
   failed on the machine and said so, or **it succeeded and was lost**,
   which would mean nothing saved on the ThinkPad survives a power-off.

   The second is plausible rather than idle: Kosmos's USB driver has its
   `SYNCHRONIZE CACHE` refused by these devices - `ILLEGAL REQUEST
   (20h/00h)`, recorded at 0.10.65 - and the 0.10.63 check that "a file
   saved to `/home` read back" read it back *in the same session*, which a
   write cache satisfies perfectly.

   **Settled by re-running `diagnose` and reading what it prints**, before
   any code is written. If it reports bytes written, this is a data-loss
   bug and goes to the top of the list.

5j. **The video player decodes Motion JPEG only** (`roadmap.md` 4e). Not a
   bug and worth stating because it was mistaken for one on 21 September:
   `magicword-clip.mp4` is H.264 and the app refused it correctly. The
   MJPEG file it can play was never copied into `~/Kosmos/home`, so no
   stick has ever carried one.

5f. **The sound suite measures the Mac, not the machine** (`testing.md`
   18.127). It failed twice inside `make test` on 20 September and passed
   alone both times: the check is a wall-clock write latency, and the gate
   runs thirty QEMUs beside it. Either it gets a machine of its own in
   `gate.py`, as `default look` does, or it measures the guest's own
   lateness - the driver's underrun counter already knows it - rather than
   the host's scheduler. Not to be fixed by widening the threshold.

5e. **AGREED on 20 September - a pass over the seam between Lua and C.**
   Diego, after the frame-as-a-Lua-string was found: "this is the types of
   things we need to revisit and make sure we are making good use of our
   lua and c worlds where it makes sense based on speed, resources and
   performance". Not "should this be C" - the language split already
   answers that - but **where a value crosses between them for no reason**,
   which does not look slow in a profile and shows up as collector
   pressure, which is what breaks a frame deadline. Found so far and fixed:
   the video kit's frames (`testing.md` 18.126). To look at, in this order:

   - **`media.lua`'s audio path**, which reads each MP3 frame into a Lua
     string 172 times a second through `sys.region_read` - the audio
     *server* was fixed and its client may not have been. Not measured yet,
     so not yet a claim.
   - **`con.wait`'s neighbours**: the console's own marshalling was moved
     into C and took the collector out of the frame path; whatever else
     talks to a server sixty times a second deserves the same look.
   - **The kits that take or return bytes** - `gfx.png`, the PDF kit,
     `kfs` - measured before anything is moved, as `CLAUDE.md` insists.

5c. **AGREED on 20 September - every pixel position measured, not counted.**
   The same class as `gc:text`'s clipping (`testing.md` 18.123), found again
   in the Processes window Diego photographed: `mem124 of 512 MB`, a label
   and its value on top of each other, from
   `self.w - #right * gfx.font.w` - a count of the *widest* glyph in the
   widget face, which is not the width of a string. Fixed on 20 September
   wherever it appeared: `procs.lua`'s meters (the one photographed, and its
   neighbour in the same file had already been fixed alone, which is how the
   class survived), `about.lua`'s two-colour banner, `clock-replicant.lua`'s
   centring, and `pulse.lua`'s chip, reading and right edge. **What is left
   is the capacity sums** - `calc.lua`, `about.lua`'s column count,
   `deskbar.lua`, and the `GW` caches in `edit.lua`, `reader.lua` and
   `tracker.lua`, which are a cell width for a face that may not have one;
   the editors want `gfx.measure("0", "mono")` rather than the widget face's
   widest glyph - and `ui.button`'s default height (`GH + 10`) and
   `ui.field`'s scrolling, which count the same cached cell.

   **`ui.list` was done on 20 September** (`testing.md` 18.125), because
   Diego photographed it: a list spaced its rows by the height the kit
   cached when it loaded, which is before an application has been told the
   desktop's faces, so 23-pixel text sat in 16-pixel rows. `row_h()` asks
   instead, and is exactly `gfx.height()` so that nothing measured against
   the pinned bitmap moves.

5d. **AGREED on 20 September - a thread count for every process.** Diego:
   "now that we have the threads, we can show each process how many threads
   it has in the table". `sys.processes()` has no such field, so it is a
   column in four layers: the kernel's per-process info, the syscall that
   copies it, `sys_user.c`'s row, and the Processes window. It is the first
   thing `threads.md`'s work makes visible to a person, which is a good
   reason to have it.

6. **DONE on 19 September, on the ThinkPad - a battery indicator on
   the top bar**, stick 0.10.88: "battery works!" (`thinkpad.md` 8d, `testing.md` 18.104). Diego that morning:
   "The battery indicator is a must", "As I now don't know what battery is
   left". As planned: for the ThinkPad: read from the
   embedded controller with the register map the T14's own DSDT describes,
   rather than through an AML interpreter, and cached rather than read on
   every `SYS_SYSINFO`. It starts with getting the DSDT off the machine,
   which the keys above will already have done.
6b. **APPROVED on 19 September - a device playground** (`docs/playground.html`).
   Diego, having seen the drawing: "Playground also I like it. I don't need
   to put the Lua code in the app but perhaps a link to the tutorial code
   that uses that device" - so **the code is not in the window**: each page
   links to the tutorial lesson that reaches that device (item 7), which
   means an example exists once, in a file that can be opened, changed and
   run, rather than twice with one copy going stale. **It therefore follows
   the tutorial** rather than leading it: the pages whose lessons exist can
   link, and the rest wait. Battery, game pad and sound first, and it is
   called **Device playground** - Diego's own words for it, on 19 September
   and the first time he asked for it.
   Diego: "At some point we will create small showcase apps that access
   devices like the battery, gamepads, sound, network", "Right now we have
   this amazing architecture of the os but is difficult to showcase its
   simplicity of accessing hardware via the servers, devices and kits", and
   "It would be a system devices showcase app which you will be able to
   interact with hardware, query it, see it in action (like moving a
   controller pad or stick and seeing it moving on the screen), or playing a
   sound and moving the balance or volume meter and seeing how it changes
   the sound, and all other devices. Is like a device playground app to
   interact with recognized hardware". One window, a page per device the
   machine has - the battery, a game pad with its sticks drawn where they
   are, sound with a tone and its volume and balance, the network, the
   screen, the processors - each showing the few lines of Lua that reach it,
   so the page is the demonstration and the code is the lesson. **Drawn
   first**, as every app is, and a natural companion to the tutorial below.
7. **NOT STARTED - a tutorial: building Lua apps for Kosmos, in ten lessons.** Asked for by
   Diego on 14 September - "a simple tutorial on extending kosmos with lua
   which was always the idea", which is `design.md` §9.1: there is no
   distinction between writing an app and modifying the system. Ten lessons,
   from a window with a button to a music player and a paint program, and
   fifteen apps, each a showcase of one part of Kosmos. They live in
   `/home/development`, to be read, changed and run by their file: a folder a
   lesson - `/home/development/01-hello-button/` - holding its apps and a
   `lesson.md` that Reader shows. Their sources are in the tree under
   `user/development/`, and the disk image puts them in `/home`.

   | lesson | apps | what it shows of Kosmos |
   | ------ | ---- | ----------------------- |
   | 1. A window and a button | `hello-button` | an application declared in its header and run by its file; `ui.window`, `ui.label`, `ui.button`; drawing as commands the window manager keeps, and a control that paints at the click |
   | 2. Controls and layout | `converter` | fields, checkboxes, lists and a menu bar; follow-mode layout; theme colours by name; keyboard focus |
   | 3. Files and the namespace | `notes` | `fs.read`, `fs.write`, `fs.list` and `ui.editor`; what this process can reach, from `fs.mounts`, and why that is all there is |
   | 4. Attributes and queries | `people` | contacts as files with attributes, as BeOS's People kept them: `fs.setattr`, `fs.getattr` and `fs.query`, and Tracker showing the same records |
   | 5. Servers and devices | `taskview`, `deskclock` | a server is someone you ask: the process list and `-- kosmos: needs processes`, `/dev/cpu`, a stick's blocks through `/dev/blocks`; and a replicant that lives in the Deskbar |
   | 6. Your own pixels | `sketch` | a direct window's shared surface, drawn with the surface's C primitives from the mouse; double buffering; why a pixel never sits in a Lua table, and which clock paces a frame |
   | 7. Images, and an editor | `lightbox`, `paint` | pictures decoded by kits, thumbnails, drag and drop from Tracker, file types; then tools, a colour picker, undo and saving |
   | 8. Sound | `piano`, `player` | `/dev/audio`'s shared ring - control by message, data by shared memory - with notes synthesised in time; then MP3 through its kit, a playlist from a query, position and seeking |
   | 9. The network | `fetcher`, `guestbook` | `fs.resolve`, `fs.connect`, `fs.listen` and `fs.accept`, from a window that never waits on a socket; a web server with a window of its visitors |
   | 10. 3D | `orrery`, `flyover` | TinyGL from Lua through `/kits/gl` - matrices, lights, display lists - and `/lib/g3d.lua`, whose maths is Lua and whose triangles are C; measuring before moving anything |

   **Each app is finished the way the rest of the system is**: the licence
   line, English, and a check in the display harness that opens it and sees
   the one thing its lesson is about - the label that changed, the file that
   was saved, the note that played.

   **What it needs first, or will find out:** a PNG writer for `paint` to save
   with, which the screenshot shortcut wants as well; that an application run
   by its file from `/home` opens its window as one in `/bin` does, which
   lesson 1 checks before anything is written on top of it; how
   `/home/development` is filled on a stick, after USB step 5f, and on a
   machine whose `/home` is memory; and that the ThinkPad has neither sound
   nor a network yet, so lessons 8 and 9 are checked under QEMU.

**A non-blocking send.** Half a browser frame is the application blocked on
a `commit` whose handler swaps an index and records a rectangle. `SYS_CALL`,
`SYS_RECEIVE` and `SYS_REPLY` are the whole IPC surface and every message to
a server blocks for its reply by construction, so triple buffering cannot
fix it. This is a change to the IPC model — a syscall, a fixed-size queue in
the endpoint struct, a decision about what a full queue does, and
backpressure.

**x86-64, under QEMU — *done, and it runs the desktop*.** A second
architecture: a different instruction set, a different interrupt controller,
a different boot protocol, a different memory model. The 64-bit line was
drawn where it was precisely so this would not be a refactor, and it was
not: all thirteen `kernel/*.c` compile for it with no `#ifdef`, and the
whole userland — fifty thousand lines of servers, libraries, applications
and Lua — needed about a hundred and ten lines of assembly and one `#if`.

`hal/pc/` is finished too. The four virtio drivers moved into a shared
`hal/virtio/` rather than being rewritten, with each board bringing its own
transport under them: fixed offsets from a device-tree window on one, a walk
of the PCI capability list on the other. Same sequence, every register
somewhere else. The measure is that the display harness passes the same
sixty-five checks on both boards in the same time, and `make test` runs
four harnesses on the x86 image.

**What it cost was device plumbing and two prose bugs**, and `docs/hal.md`
has all of them. The one worth repeating here is that none was about x86:
the worst — a scan that reset the keyboard on its way past — was latent on
ARM too, and survived only because QEMU happens to lay virtio-mmio windows
out in the reverse of the order the devices are given.

**SMP, and real parallelism - done on both boards**, with placement on by
default since 0.10.22; *Being built now* has where it stands. This entry said
x86-64's per-CPU state was not written yet, which stopped being true when
`swapgs`, the GS base and a TSS per core arrived.

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

**Real hardware.** `docs/targets.md` is the model and the two worked
examples — a Raspberry Pi 5 and an Alienware x14, which between them make
the case that drivers group by *what the device is* rather than by
architecture or by board: the x14 and QEMU's q35 share an instruction set
and almost no device, while the x14 and the Pi 5 share the entire USB stack
and no instruction set at all.

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

- **NTFS, read only**, so the Windows files on the ThinkPad's NVMe open in
  Kosmos. Asked for by Diego on 14 September while deciding the drives
  design, then left out of USB step 6 to focus on FAT32 and exFAT: "we can
  go back to ntfs anytime later in our roadmap". What is known: Microsoft
  publishes no specification; Linux (ntfs-3g, ntfs3) and Haiku read it
  already, and all of them are GPL, so it is Kosmos's own reader with those
  as references, or their code under the GPL arrangement Doom's builds have;
  and BitLocker, on by default on many Windows 11 laptops, leaves the
  partition unreadable either way. Until then an NTFS filesystem is listed
  with its type, and not opened (`docs/drives.html`).
- **SMP.** Moved up to *Being built now* — see there, and `docs/smp.md` for
  the map. **This entry used to claim the kernel was "written SMP-ready: no
  loose mutable globals, a per-CPU pointer, a per-CPU runqueue with one CPU
  in it", and none of the three was ever true** — the same sentence
  `CLAUDE.md` carried from the first commit and corrects. All three are true
  now because somebody wrote them, seven months later, which is the
  difference between an intention and a fact.
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

- **App Inspector: an x-ray of one application, by layer.** Diego, 15
  September, after a page took a while to load in the browser: "allows you to
  do an xray of an app and what app is doing internally where is spending time
  in lua land, in c land, in servers, drivers, etc", "so the profiling is
  necessary to understand where the app is wasting time in loops in lua or in
  the filesystem, what servers are being called", "will allow me to see and
  understand where is the cpu time spent by layers, modules, services". This
  is the *app profile* he asked for on 11 September, widened from "C, Lua,
  messages" to the servers and drivers underneath.

  **His condition stands and decides the design**: "not sure if we can do this
  already or we need plumbing on every app. if every app needs plumbing thats
  not really necessary then." So nothing an application has to opt into.

  **What exists already, and it is more than half of the vocabulary.** The
  window manager keeps seven counters and hands them over, which is what
  `frames` divides by a clock - stage by stage, with what each allocates,
  which is how the console's marshalling was found to cost four times the
  worst collector pause. Processes has per-thread CPU accounting and shares
  (`/lib/procshare.lua`). `sys.info` reports the pools and `gc_pause_max`.
  None of that says *which layer inside one process*, and none of it says
  which server a process is waiting on.

  **The shape that needs no plumbing**, to be checked against the code before
  any of it is built: the kernel records, on each timer tick, the interrupted
  user PC of the processes being watched and whether the thread was running,
  in a syscall, or blocked in IPC - which is thread accounting, and stays
  inside what the kernel is allowed to know. Every process runs the same
  userland image, so **one symbol table classifies all of them** - the Lua VM
  and its collector, the C kits, the libc, the syscall path - and the
  symbolisation belongs in userland where the image is. Time blocked in IPC is
  attributed to the endpoint waited on, which is what names the *server*: the
  filesystem, the compositor, the disk. A driver's own time is that driver's
  process, which the same sampling already covers.

  **What is not yet answered**: whether the tick handler can record a PC
  cheaply enough to leave on; where the symbol table comes from at runtime;
  and how a server's time is divided among the clients that asked for it,
  which is the difference between "the filesystem is slow" and "this
  application asks it too often".

  **Drawn before it is built**, like every app since 14 September:
  `docs/appinspector.html` first, changed until Diego agrees, then the code.
  The browser loading a page is the first thing to point it at.

### Smaller, and wanted

- **FOUND on 22 September - Music's two icons have never drawn.**
  `music.lua` asks for `"File_Audio"` and `"Misc_Speaker"` without `.png`,
  and the window manager chooses a decoder by the name's suffix, so both
  commands draw nothing - the empty cover's note and the speaker beside the
  volume. Noticed while `gc:icon` learned to draw at a size (5v); the
  speaker's 14 was a crop of a 32 until then, so it would have been its
  corner even with the name right. The fix is the two names, and a check
  in `run_media.py` that the cover's note is drawn.

- **NOT STARTED - an FTP client.** Diego's, 16 September. The natural next
  program on the TCP stack after `telnet`, and a different shape of problem:
  FTP is two connections rather than one - a control channel that carries
  commands as lines, and a data channel opened per transfer - so it is the
  first thing here that has to *accept* a connection or ask the server to
  open one. `telnet` only ever connects out.

  **What that costs, named before it is started**: passive mode (`PASV`) has
  the client connect out twice, which the stack already does; active mode
  (`PORT`) needs LISTEN, which `state.md` records as absent - "this end
  connects out, which is telnet and SSH. Accepting needs..." - so passive
  first, and active only if a server refuses it. Listing is its own format
  and not a standard one; a transfer wants `/drives` and `/home` to write
  into, which USB step 6 has now made real.

  **`telnet` is already built** (`user/bin/telnet.lua`, 7 September) and is
  deliberately not a terminal: it sends what you type and prints what comes
  back, which is enough for SMTP, HTTP or a daemon's banner. Option
  negotiation - window size, echo, line mode - is a protocol of its own and
  belongs in a program that means to be a terminal. If Diego wants that
  terminal, it is a separate item and a larger one.


- `tools/mkusb.sh`'s closing message still calls the stick's loader unsigned
  GRUB, which it has not been since 13 September.
- Doom's sound, behind a hook that already exists.
- **The display harness finds things by an assumed font width.** `kosmos_w`
  in the focus phase is `len("Kosmos") * GLYPH_W`, the bitmap's 8-pixel cell,
  while the Deskbar sizes that button with `gfx.measure("Kosmos")` in whatever
  face is loaded. Any phase that leaves a different face behind moves every
  button, and the failure reads as a colour being wrong rather than a position
  - it cost three runs on 15 September (`testing.md` §18.79). Asking the guest
  where the buttons are, rather than computing it here, is the fix.
- **Jack sensing for the ThinkPad's headphones.** Since 0.10.70 the speaker
  and the headphone jack play together (`testing.md` §18.76), and Diego heard
  Basket Case through the speaker. The jack's pin, 0x21, reports presence
  detect (pin capabilities `0x0001001c`, bit 2): reading it - asked, or told
  by an unsolicited response - is what mutes the speaker when headphones go
  in. The account of how the ThinkPad got sound, from a codec that did not
  answer to an amplifier nobody switched on, is in `thinkpad.md`.
- An equaliser in the mixer — the first thing that will want the ring to
  carry something other than what was written to it.
- **Music, as VOX is**, Diego's, 14 September: "the music player is really
  barebones now. can we improve the functionality and style like vox player
  for mac?" Designed in `docs/music.html`: what is playing on top with its
  format, a bar you can drag, one row of controls, a Library found by
  each file's tags read by a reader, playlists, a mini player, and a dark
  look and a light one; WAV and MP3. Decided with Diego the same day
  (`README.md`): tags read from the file rather than written onto it, FLAC
  later - it needs a decoder and a 24-bit path in `sys.pcm` - and the Deskbar
  replicant after the player. Its playing lives in `/lib/media.lua`, one
  engine a video app can share later, at Diego's word. Seeking, which this
  line used to be on its own, is part of it. **Step 1, the engine, is
  built** (`testing.md` §18.66): Music plays through it and its bar seeks.
  **Step 2, the tag reader, is built** (§18.67). Next, the window from
  `docs/music.html`.

  **The icons, decided 15 September**: the vendored Haiku set has none of the
  transport controls - it is applications, files, folders, devices and
  preferences - so Music takes `App_MediaPlayer` for its window and Deskbar
  button, `File_Audio` for a track with no cover and `Misc_Speaker` beside the
  volume, and **draws** shuffle, previous, play, next, repeat, queue and the
  mini player. Diego chose that over vendoring more, which would be a licence
  and a pinned commit to keep in step for shapes that are four lines of Lua
  each (`docs/music.html`).

  **And drawing them needs a triangle command**, found while building: an
  application that draws through commands has `fill`, `text` and `image` and
  nothing else - `triangle` and `disc` are surface methods, reachable only by
  a window that draws its own pixels, which Music is not. Rectangles cover
  pause, the skip bars and the queue; the play arrow as a staircase of fills
  is visibly stepped at 18 pixels. **Diego chose to add the command** (16
  September) rather than accept that or generate seven pictures: the primitive
  is already in C, and the restyle after Music wants triangles for menus,
  sliders and disclosure arrows. **Built the same day** (`testing.md` §18.83):
  the command, the kit's `gc:triangle`, and a control that draws the bounding
  box instead - which is the mistake worth guarding against, and which a check
  asking only "did the ink appear" would have passed.

  **Step 3, the window, is built** (15 September, `testing.md` §18.84): the
  design drawn, in its own flat palette, with the cover at 78 and at 44, the
  larger title, the drawn transport and the library. Control C24 watched fail.
  **Left for the next pass**: the mini player's fold bound to a control (the
  resize underneath it is built), the light look behind a View menu, and
  shuffle, repeat and queue, which are drawn and inert.

  **Step 3 needed four things the system did not have**, found by reading
  on 14 September, and proposed to Diego before any is built. **He answered on
  15 September - "music first, yes to all"**: Music's window comes before USB
  step 6b, the large title is a size the UI kit carries rather than an
  application drawing its own pixels, and **Music is the pilot of the flat
  look**, after which he decides whether the other applications follow
  (`ui.md` §16.8b). What he said first: "i love the music app design. can we do
  it for real in kosmos?", and "i might redo a lot of the current apps with
  this style and design aesthetic".
  1. **A scaled blit in `gfx`**, in C: a cover is hundreds of pixels and
     drawn at 78 and 44. `photo.lua` already says one belongs there, and that
     a scaler in Lua would be the per-pixel loop `gfx.md` 19.2 forbids.
     **Built, 15 September** (`testing.md` §18.78): `dst:stretch(...)`,
     nearest neighbour, the step in 16.16 fixed point, only the destination
     clipped - control C18 watched fail. **And a second half the same day**
     (§18.82), found while planning the window: an application draws through
     commands, and no command carried a size, so the primitive was there and
     unreachable. The `image` command now carries `dw`/`dh` and the kit has
     `ui.image{ fit = true }`.
  2. **Covers out of an MP3.** `ui.image` names a picture and the window
     manager decodes it, so a cover that is bytes inside a file has no name.
     **The `/ramfs` copy this line proposed cannot work**, found by reading on
     15 September: a ramfs value is capped at 16384 bytes and `read_into` is
     not served by the ram proto at all, while a cover is hundreds of
     kilobytes. **Instead the picture travels as a region** - a request
     carrying the pages and a name, decoded by `gfx.png`/`gfx.jpeg`, which
     already take an address and a length. That is *control by message, data
     by shared memory* rather than a special case, and it keeps working when
     `/home` is not a disk. **Built, 15 September** (`testing.md` §18.80):
     the compositor's `picture` request and `media.cover`, sharing the one
     picture cache and its four-at-a-time eviction - control C20 watched
     fail.
  3. **A window asking for its own size**, for the mini player. **The window
     manager has served this since 2 September** (`handlers.resize`,
     `89bba71`), and refuses only a window that draws its own pixels, which
     Music is not - so what is missing is the application's half: a
     `window:resize` beside `window:move`. Two things found with it: the
     `resize` event leaves the kit's own `w`/`h` stale, and Music's widgets
     sit at fixed coordinates with no follow mode, so folding needs a layout
     that runs again. **Built, 15 September** (`testing.md` §18.81):
     `window:resize`, the size taken from the reply, and the event path now
     keeping those two fields - control C21 watched fail, after a first
     control that crashed instead of demonstrating it.
  4. **A title larger than the three text roles**, which an ordinary window
     draws at the sizes the desktop's font settings give. **Decided: a text
     command that carries a size**, so every application restyled after Music
     gets large text without drawing its own pixels - which is what a window
     with `direct = true` would have meant, and it would have helped Music
     alone. **Built, 15 September** (`testing.md` §18.79): `gc:text(...,
     role, px)`, resolved against a pool of faces on each side, with the
     role's own size as the answer when the pool is full - control C19
     watched fail, after a first version of the check that did not.
- A markdown viewer, for manuals inside the system.
- **Selection in the terminal**, which is where people most want to copy
  from and is the one window the clipboard cannot reach. It draws its
  scrollback itself through `ui.view` rather than through `ui.editor`, so
  it has no anchor and no cursor - and the honest fix is to lift that
  machinery out of the editor rather than to write it twice.
- **A control request a mouse refuses leaves endpoint 0 halted.**
  GET_DESCRIPTOR for its Report descriptor and SET_PROTOCOL are the two a
  mouse can answer with a STALL, and the controller then holds its default
  endpoint halted: the next request fails and the mouse is not read. Bringing
  it back is a Reset Endpoint and a new dequeue pointer, which QEMU's mouse
  answers every request and cannot be made to need. Noticed writing the
  report-protocol path on 13 September (`usb.md` §5).
- **A USB mouse whose report fails is not brought back.** A stall, or a
  transaction error after three tries, halts its endpoint, and the driver
  stops reading it until it is plugged in again. Bringing it back is a Reset
  Endpoint, a CLEAR_FEATURE to the device and a new dequeue pointer - none of
  which QEMU's mouse can be made to need, so none of which a test here could
  reach. Written down in `usb.md` §5 and left for a machine that needs it.
- **A mouse that runs at SuperSpeed is not read.** Its endpoint's largest
  payload an interval comes from a SuperSpeed Endpoint Companion descriptor,
  which the walk does not read; the driver says it met one. The other half of
  this line - a mouse that stays in the report protocol - was the ThinkPad's
  own, and is read by its Report descriptor since 13 September (`usb.md` §5).
- **A stick's LUN 0 only.** Get Max LUN is not asked, because a stick with one
  unit may stall it and a stall on endpoint 0 is not recovered from.
- **A screenshot shortcut**, Diego's, 14 September, and **made concrete on
  22 September**: "like ctrl+alt+1 full screen screenshots", "and save them
  on the desktop as screenshot-date-time.png". So **Control-Alt-1** saves
  the whole screen as a PNG **in `/home/Desktop`**, where it shows up as an
  icon, named `screenshot-2026-09-22-114503.png` - the date and time it was
  taken, with no spaces so the prompt can name it. (The 14 September
  version said a Super binding and `/home/screenshots`; this replaces it.) Most of it is
  here already: the window manager composes every frame into its backbuffer,
  `SUPER_BINDINGS` in `wm.lua` is where a shortcut goes and what the Shortcuts
  window lists, and `sysinfo`'s `epoch` is the board's real-time clock, in
  UTC, for Date & Time's offset to turn into the time on the wall. **What is
  not: nothing in Kosmos writes a PNG** - `png.c` and `inflate.c` read one -
  so it wants a writer in C, which can start with deflate's stored blocks and
  compress later. And the saving happens outside the key handler, which must
  never wait on anything: a synchronous call from there once deadlocked the
  desktop.
- **A region's pages freed while a process still has it mapped.** Found by
  reading, 14 September, and not yet shown by a test: `SYS_MEM_MAP` takes no
  reference on a region, and `memobj_unref` frees its pages when its last
  capability goes, so a process that drops the last capability to a region
  it has mapped keeps reading and writing pages the allocator may give to
  somebody else. `sys.release` and every C server here unmap first
  (`SYS_SHARE_UNMAP`), so nothing does it today - but a process that did it
  on purpose would, which is a hole in the capability system rather than a
  convention to keep. The kernel should hold a reference for a mapping, or
  unmap on the last drop. Diego asked for it on the roadmap rather than
  fixed at once.
- **kfs answers a Lua error for a root that is not a directory.** Found by a
  control for USB step 5e, 14 September: `walk` in `user/lib/kfs.lua` names
  the component before the one it has reached, and before the root there is
  none, so a filesystem whose root inode is not a directory answers `attempt
  to concatenate a nil value (field '?')` rather than a sentence. It was
  reached by a disk server reading zeros where it expected its partition -
  which 5e's units no longer allow - and a corrupted root is the other way.
  The check belongs in `tools/test_kfs.lua`, beside its other damaged disks.
- **One speed for every relative device.** A mouse and a TrackPoint want
  different speeds, and both want a curve (`hal/pc/pointer.c`).

---

**Wanted - `dev_range_ok` refuses an address the processor cannot reach.**
Found on 18 September by the backlight driver's control, fooled into taking
a network card for graphics: it built a 64-bit address from the card's next
BAR, the kernel mapped it, and the first read faulted on reserved bits in
the entry - `pte 0x800a0000fe0c801f`, bits above the machine's physical
width. A process should be refused that mapping rather than killed by it:
CPUID 80000008h's physical-address width on x86, `ID_AA64MMFR0_EL1.PARange`
on AArch64. The x86 fault report now prints the reserved-bit flag and the
entries it walked, which is what explained it.

**DONE on 18 September - Space Grotesk**, five weights, at Diego's asking:
"add the new font space grotesk" - in `assets/fonts/` with its licence, and
offered by Appearance like every other face (`testing.md` 18.98).

**DONE on 18 September - the desktop's wallpapers**, 24 photographs from
Unsplash in `FULL=1` images, chosen in Appearance by photographer; the
Lenovo ones from the same folder stay out of the public repository and can
go on a stick's `/home` (`testing.md` 18.100).

**DONE on 19 September - `make shot`'s picture without doubles, and more
of the system in it.** The login set opened Tracker, Monitor and Processes
beside the gallery's own, so two of three were in the picture twice;
`run_gallery.py` empties it first now, as `run_screenshot.py` does. And
Diego: "We need to start showing other apps in the screenshots as well" -
Music, Appearance, a Terminal, the Calculator, the gears and This Machine
join Tracker, the widgets, Processes, Monitor and the cube: eleven, none
twice.

**DONE on 19 September - the gate refuses an x86 part that no suite runs.**
`run_x86.py` gained `power_button` and `gate.py` names its parts by hand, so
the new part ran only when asked for by name - found by reading the suites,
not by anything failing. `gate.py` now holds the parts its suites name to
`run_x86.PARTS` before it starts (`testing.md` 18.102). **Still wanted**: the
display harness's phases held to `DISPLAY_PARTS` the same way - its phase
list lives inside `run_screenshot.py`'s `main`, so it needs a list at the
module's top first.

**Wanted - a window that draws its own pixels is never shrunk behind its
back, and its region is measured.** Found on 19 September while giving the
Super Nintendo a Double Size: `handlers.open` clamps every window to the
screen less its decoration, and for a direct window that is a disagreement -
the kit wrapped its two buffers at the size it asked for, the window manager
wraps them at the clamped size, so the second buffer starts in a different
place on each side. At 1920x1080 a 1024 by 960 window fits and nothing shows;
on a 1280x800 screen Double Size would draw garbage. And nothing checks that
the region is as large as two buffers of the size asked for:
`sys.memory_size` exists, and a window manager that maps a small region and
reads a large one takes the desktop down with it. A direct window's size is
its buffers' size - kept, placed so its tab is on the screen, and refused
when the region cannot hold it.

**Wanted - a stick's layout that cannot be built wrong.** On 19 September
0.10.86 was first built with `make MEGA=1 x86-usb-image`, as `CLAUDE.md`
said, and came out without `/home` in a partition - a layout the ThinkPad
has not booted since 0.10.61 - because `USB_HOME=partition` is a flag and
not the default. Caught by the size (243.3 MB against 234.9) and the missing
`opt/kosmos/home=` before it was handed over. `make usb` has the same trap:
it rebuilds the image with the default before writing. The proven layout
should be the default, and `make usb` should write the image that was
checked rather than a new one.

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

**The display harness's `/bin` walk counted one program, once.** 14
September, AArch64, in USB step 5e's gate: the shell's `fs.list("/bin")`
gave back a table with one name ending in `.lua`, where `/bin` holds 111,
and `make screenshot` run again on the same tree passed all 110 checks.
Neither obvious account fits. A request to `/bin`'s server that failed would
give no table at all - nothing is mounted below `/bin` for `ns.list` to fall
back on - and so a count of nought; and a first page alone holds 28 names
(`BIN_CHUNK`), not one. The harness keeps only what arrived after the line
it typed, so what the machine said before it is gone. Recorded as
intermittent, with its one line of evidence, and nothing fixed.

**And again on 18 September, on x86**, in the display harness run on the
ThinkPad keys' first step: `bin-scanned 1`, the guest's own count, so a list
of one name came back from `/bin` and the harness read it correctly. Not the
change being checked, which was the keyboard driver naming a key it does not
know - this phase runs long before any such key is pressed. The same tree's
`make test` passed, and the harness run again on the unchanged tree passed both boards, 124 and 125. **Since then a failure
in the display harness writes the guest's whole log to
`build/harness-failure-<board>.txt`**, so the next one arrives with
everything the machine said rather than its last two lines.

**The window manager could not find a file written two seconds earlier,
once.** 18 September, x86-64, in 0.10.77's gate: the registry phase writes
`/ramfs/viarun.lua`, waits two seconds, and types `wm /ramfs/viarun.lua`, and
the guest answered

```
process 27 (viarun) ended, code 0
wm: could not start /ramfs/viarun.lua: no such path
```

and nothing else - neither the probe's answer nor the line `viarun.lua`
prints when its own `run` fails. The same code passed this phase on both
boards an hour before, in shortcut places' check; the gate run again on the unchanged tree passed both boards, 121 and 119, and was pushed. Two things do
not fit together: something named for the file ran and ended cleanly, and the
window manager was then told the path did not exist.

**It is the second lookup in the display harness to fail once and pass on
the same tree**, after the `/bin` walk above. Both are a file server
answering as though something that is there were not, both under a guest
busy enough to be a display harness, and neither left more than a line or
two, because the harness keeps only what arrived after the line it typed.
Two of the same shape is a pattern rather than two accidents, and the next
one should be caught with the guest's whole log rather than its tail.
Recorded as intermittent, with its lines, and nothing fixed.

**A tick charged by every interrupt, fixed without a test that catches it.**
Both trap handlers called `thread_tick` on every hardware interrupt, on the
grounds that the timer was the only source - and both said in a comment that
this would stop being true. It did: a keyboard, a sound controller asking
for a period 172 times a second, a network card. Each charged a tick nobody
had spent, which shortens quanta, inflates the busy and idle counts every
processor meter reads, and sends core zero through a full scan of the thread
table for sleepers whose deadlines have not moved.

`hal_irq_handle` answers whether the tick fired now, and all four call sites
across the two architectures are gated on it.

**What is missing is a check that would have caught it**, and the first
attempt did not: `idle + busy <= hal_ticks()` passed with the bug
deliberately reinstated, because the guest suite's machine raises almost no
device interrupts and there was nothing for the invariant to notice. It was
removed rather than kept - a check that cannot fail is worse than none.

The test wants two things this system does not have yet: a **scheduler tick
count exposed to userland** beside `idle_ticks` and `busy_ticks`, and a
moment when the machine is under **device load**. Both exist separately -
`run_x86.py` already boots `audiolag`, which drives 400 HDA interrupts
through the machine - so the check is one `sysinfo` field away from being
real.

**Three framebuffer checks failed once on x86-64, and the run ended in a
page fault.** September 2026, in a `make prepush` on 0.10.13. Recorded in
full, because the entry above it is a lesson about not doing that:

```
ok 116 - boot: every stage was announced
not ok 117 - fb: the display comes up
not ok 118 - console: a write carries its colour, and UTF-8
not ok 119 - fb: the pitch is not width * 4
ok 120 - fb: page aligned and inside RAM
ok 121 - fb: every row is writable
*** page fault  cr2 0x00000000994fe000  not present, write, kernel
```

**The shape says more than the failures do.** 117 through 119 use the
framebuffer descriptor and failed; 120 and 121 use it and passed; then 122
wrote through a pointer to 0x994fe000, which is 2.5 GB and nothing this
machine maps. A descriptor that is bad for three tests, good for two, and
garbage for the sixth is not a driver that is wrong - it is state that
differs between calls.

**What was ruled out.** The same binary passed five times afterwards: three
plain runs, and two with `hal_fb_init` called three hundred extra times per
run comparing every field against the first answer. It is idempotent, and
it is not the obvious candidate - `ramfb_init` writing its configuration
through `fw_cfg` on every call.

**What is still open** is whether the descriptor or the *screen* is what
moved. Tests 117 to 119 include the console's colour check, which counts
pixels rather than reading a struct, so a display that was scrolled or
half-drawn at that moment would fail exactly those three and leave 120 and
121 - which check alignment and writability - passing. That would make the
page fault the only real fault and the other three its symptom.

Seen once in six runs of the same image; not reproduced since.

**Two scheduler checks fail now and then**, on AArch64, and each failure had
three green runs of the same build beside it: `sched: the policy is pluggable`,
twice, and `sched: the higher priority runs first`, twice - the second time in
0.10.60's first `make prepush` on 13 September 2026. `state.md` has the two
suspicions - the harness's load, and the two threads homed on different cores
so that the lower runs before the higher is awake - and neither is measured.
The next step is many runs with each thread's core printed.

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
