# Linux on Kosmos

Linux in a Kosmos window: Alpine, in a sandbox, with its screen, keyboard
and pointer in a window of its own - **Chromium there for the web Kosmos's
own browser does not try to host**, and any other Linux application beside
it. Diego, 14 October 2026: "the linux guest idea is great as well to run
chromium in alpine linux and see it on the kosmos screen. that also serves
as a way to run native linux apps on kosmos. Linux-on-Kosmos feature" - and
the same evening, "can we prioritize linux on kosmos so we can run a web
browser ?".

Designed before it is built (`CLAUDE.md`, *How to work here*): this file
is its feature set and its architecture, `docs/linux.html` its window,
`docs/linux-architecture.png` its diagram, drawn from
`docs/linux-architecture.html`. **None of it is built until Diego has
agreed all four.** He answered its three questions on 14 October
(*Decided by Diego*, at the end).

**What it stands on is already running.** Sandboxes (`docs/sandboxes.md`)
boot Alpine on the M700 under VT-x in about a second (X1-X3, 0.11.201-
0.11.203): its network out through Kosmos's stack, ports carried in,
folders of `/Home` shared, a console. What a sandbox does not have is a
screen, a keyboard, a pointer and a disk of its own. Those four are what
this adds.

## What it does

- **Opens Linux in a window** - Linux in the Deskbar and the launcher pad,
  as an application. The first time, it sets Linux up: Alpine and Chromium
  installed onto Linux's disk, with progress said as it goes; after that,
  it starts in a few seconds where it was left.
- **The browser**: Chromium, filling the window, as `cage` runs one
  application - or a whole Linux desktop, Xfce, with Chromium in it and a
  terminal beside (decided: `cage` first, the application a setting).
- **Typed into and pointed at** like any window: the keyboard, the pointer,
  the wheel, a click; Super keys and Kosmos's own shortcuts stay Kosmos's.
- **Resized**, and Linux's screen follows the window's size; Full screen
  from the View menu.
- **Files both ways**: `/Home/Downloads` shared into Linux, read and write,
  so what Chromium downloads is in Kosmos's Downloads; more folders from
  its settings, each read only or read and write, as a sandbox's are.
- **Copy and paste text** between Linux and Kosmos.
- **On the network as Kosmos is**: outgoing through Kosmos's stack, with
  nothing of its own on the wire.
- **More Linux applications**: `apk add` in its terminal (Xfce), each one
  then in Linux's own menu.
- **Stopped and started** from its menu or from Sandboxes, where it is a
  card like any sandbox; its disk keeps everything between starts.

Not in the first version: **sound**; **3D** in the guest (the screen is a
2D framebuffer, so Chromium draws in software - fine for the web, not for
games); **more than one processor** for Linux (one core first - step L6);
USB devices; pictures on the clipboard.

## What it is made of

The premise (`CLAUDE.md`, *kits, servers and drivers supply*): **the
devices are the sandbox monitor's**, so every sandbox can have a screen,
input and a disk - Linux-on-Kosmos is the first to use them, not the only
one. The Linux application is the window: it shows the screen the monitor
paints and hands it the window's keys and pointer.

| Piece | What supplies it | New? |
|---|---|---|
| Linux itself, at the processor's speed | the sandbox monitor `vmm` (`vmm.c`, `vmm_x86.c`), VT-x in Nebula (X1) | no |
| Its devices' transport | virtio-mmio (`vdev.c`), as the folders' and the network's | no |
| **A screen**: the guest draws, the monitor shows | **virtio-gpu, 2D**, in `vmm`: resources in the guest's RAM, transferred into a region the window can see, flushes said as damage | **new, in vmm** |
| **Keyboard and pointer** | **virtio-input**: a keyboard and a tablet (absolute), fed from a ring the window writes | **new, in vmm** |
| **A disk that keeps** | **virtio-blk**, backed by a file in `/Sandboxes` - a drive of its own (decided) - through `diskfs`: read into a region, written with `DISK_OP_WRITE_AT` (N14's, 0.11.237), a piece a request | **new, in vmm** |
| **`/Sandboxes`, a drive for guests' disks** | a partition of its own on the stick, a second `diskfs`, mounted beside `/Home` | **new** |
| The network | virtio-net and the gateway (`vmmnet.c`, S4) | no |
| Folders of `/Home` | virtio-fs (`vmmfs.c`, S3) - its 16 MB write cap lifted with `WRITE_AT` | grows |
| Starting, stopping, a definition | the Sandboxes kit and `sandbox`, `vmmproto.h` | grows: a screen's region and an input ring |
| The window: the screen blitted, keys and pointer sent, menus | **the Linux application** (Lua, a direct window), blits in C (`gfx`) | **new app** |
| First-run setup | a script Linux runs from its initramfs: the disk formatted, Alpine and Chromium installed with `apk` through the gateway | **new** |
| Linux's kernel and its drivers | Alpine's `linux-virt`: `virtio_gpu`, `virtio_input`, `virtio_blk` are in it | no |

**What is C and what is Lua.** C: every device (the monitor is a server),
the copy of a frame into the region, the blit into the window. Lua: the
window, its menus, its settings, the first-run's progress.

**What travels where** (*control by message, data by shared memory*):
- **The screen is a region**: the monitor copies what the guest flushed
  into it, double-buffered, and tells the window with a **pulse** (P1) whose
  value says which buffer; the damage rectangles sit in the region beside
  it. No frame is ever a message.
- **Input is a ring in the same region**, single-producer single-consumer:
  the window writes each key and pointer event, the monitor reads them when
  the guest asks, and a pulse wakes it. Nothing on the desktop waits on the
  monitor.
- **The disk's blocks** go between `diskfs` and the guest's RAM through a
  region, never a message.

## Its busiest path, and where the time goes

**A frame of Chromium.** Chromium draws into its buffer in the guest; the
guest's virtio-gpu driver asks for the changed rectangle to be transferred
and flushed. The monitor copies that rectangle from the guest's RAM into
the screen region (C, a memcpy a row), and pulses the window; the window
blits it into its own surface (C) and commits the damage; the window
manager composes. A full 1280x800 frame is 4 MB copied twice - well inside
what the M700's memory moves sixty times a second. Scrolling a page is a
few hundred kilobytes.

**Where the time is expected to go**: Chromium's own drawing, in software,
on one core - which is why more cores (L6) comes before anything else once
it works. Measured on the M700, as everything here is.

**The disk**: a guest block write is a `WRITE_AT` of 4 KB pieces, each a
`diskfs` transaction; the first run's install is the heaviest it will see.
Measured too; batching a request's blocks into one piece is the obvious
first step if it is slow.

## How it is tested

QEMU cannot emulate VT-x, so an x86 guest runs only on the M700. **The
devices are shared code**, so each is held by the ARM sandbox suites
(`arm-sandbox-*`, which run in every gate) with Alpine arm64 under QEMU's
EL2: a disk written, rebooted and read back; the guest's framebuffer's
pixels seen in the window's region; a key typed into the guest from the
ring. Then each step is tried on the M700 before the next, as X1-X3 were.

## Steps

1. **L1 - a disk that keeps**: `/Sandboxes` made and mounted, and
   virtio-blk on a file in it; a file written in the guest, the sandbox
   stopped and started, the file there.
   - **L1a, `/Sandboxes`: built** (`testing.md` 18.636) - a second disk
     server on the partition `opt/kosmos/sandboxes` names, the USB driver
     guarding both drives, and `tools/sandboxes_drive.py` to give a stick
     in use the partition with `/Home` left as it is (the table moved to
     the stick's end; run once with `sudo` on the unmounted stick).
   - **L1b, virtio-blk: built** (`testing.md` 18.637) - `vmmblk.c` in the
     monitor on a file named in the setup, written in place
     (`DISK_IN_PLACE`), `sandbox disk new` and `--disk`; Alpine writes and
     reads back its disk under QEMU. One C door to the disk protocol,
     `diskask.c`, for it and the four that had copies. **On the M700**: once
     the stick has `/Sandboxes` (`tools/sandboxes_drive.py`), `sandbox disk
     new /Sandboxes/linux.img 4096` and a sandbox given it.
   - What L1 leaves for later, measured first: the vCPU waits while its
     disk is read or written (one thread); the region is mapped by the disk
     server each request.
2. **L2 - a screen**: virtio-gpu 2D and the Linux window showing it - the
   guest's console drawn in the window.
   **Built** (`testing.md` 18.638): `vmmgpu.c` in the monitor, the screen
   region (`vmmscreen.h`) made by whoever starts the sandbox, and the Linux
   window drawing it - Alpine's console in a Kosmos window. 512 MB until L4
   gives it 2 GB; the window starts the sandbox and stops it on close until
   L4 makes Linux a sandbox of its own that keeps running.
3. **L3 - keyboard and pointer**: virtio-input from the window - typed into
   the guest's console, a click where it was pointed.
4. **L4 - Chromium**: the first-run setup, `cage` (or Xfce) and Chromium,
   Downloads shared; Linux in the Deskbar.
5. **L5 - the window's size followed, and copy and paste.**
6. **L6 - more processors**: the guest given several cores (an APIC and
   IOAPIC in the monitor, a vCPU a thread).

## Decided by Diego, 14 October 2026

1. **Chromium alone, in `cage`, for now** - "cage for now but i might want
   other apps as well". So `cage` runs one application and Chromium is the
   first; what it runs is a setting rather than a constant, so another Linux
   application - or, later, a desktop such as Xfce - is a choice in Linux's
   settings and not a redesign. Nothing in the devices or the window
   depends on which.
2. **Guests' disks on a drive of their own, `/Sandboxes`** - Diego's own
   idea: "what if instead /home we crate a new /Sandboxes drive with virtual
   machibes disks?", "and not just 1 big drive", "that pollutes the home?".
   A partition of its own in the stick's free space (the Kingston is 128
   GB, of which `/Home` uses 2), formatted kfs, served by a second `diskfs`
   and mounted at `/Sandboxes`, each sandbox's disk a file in it -
   `/Sandboxes/linux.img`. `/Home` is untouched and cannot be filled by a
   guest; the drive shows in Tracker as one of its own, and could move to
   the M700's internal drive later with nothing above it changing. 32 GB.
3. **2 GB of memory**, of the M700's 8; one core until L6.
