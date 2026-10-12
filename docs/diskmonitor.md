# Disk Monitor

Every drive this machine is using, and what each is reading and writing -
now, over the last minute, and since the machine started - with how long a
request takes and which programs are doing it. Diego, 12 October 2026: "we
will need to do another app called Disk Monitor to moniutor input ouptput
like we do for network to show all devices and its input output bandwtudh,
procesees using io, etc". Drawn the same day in `docs/diskmonitor.html` and
agreed ("mockup for disk is perfect"), with each drive drawn as what it is
and the one pair of colours every monitor uses.

This document is the feature set and the architecture; `diskmonitor.html`
is the window and `diskmonitor-architecture.png` the picture of what it
stands on. Network Monitor's sibling, and built on what it built: the kit's
`ui.history` and `ui.meter_card`, the theme's `io_in` and `io_out`, and a
server that counts its callers by `SYS_SENDER`.

## What a person can do

- **See every drive** in a list down the left: the disk `/Home` is on, each
  stick and its volumes, each share on another computer, and the memory
  `/Temporary` is kept in - each drawn as what it is, with its reading and
  writing now and a small graph of the minute, reading in blue and writing
  in orange.
- **Choose one** and see, on the right:
  - its card: what it is, where it is, its two rates now;
  - the last minute, reading above the line and writing below, on one
    scale fitted to the busiest second;
  - **how long a request takes** - reading and writing apart, averaged over
    the last pace - and **how often the cache answered** without asking the
    drive;
  - the totals since the machine started: bytes and requests each way;
  - **the programs reading and writing it now**, the busiest first, each
    with its rates and the file it touched last.
- **Choose the pace** and **bytes or bits**, from the dots, as the other
  monitors.

## What it is made of

**The application is the thinnest layer there is** (`CLAUDE.md`): it asks
and draws. Everything it shows is counted by the server that already does
the work.

| Piece | Supplied by | New or as it is |
|---|---|---|
| The window, the list, the menu | the UI kit | as it is |
| The graphs and the card | `ui.history`, `ui.meter_card`, `io_in` and `io_out` | as Network Monitor made them (N2) |
| **The drives** | `drivelist.lua` (the machine's disk, the sticks and their volumes), `netshares.lua` (the shares), and `/Temporary` | as they are, Tracker's Drives and the Drives app's |
| **A drive's counts** - requests and bytes each way, time taken, the cache's hits | **one door for every drive, `<where>/.device`**, read as a file: `diskfs` (`/Home`) and `smbfs` (a share) answer it today (`DISK_OP_DEVICE`, for Disk Benchmark) | as it is for those two |
| A stick's counts | **the drive server** (`drives.c`), counting as `diskfs` does and answering `.device` in its own protocol (`drivesproto.h`) | **new**, small |
| Memory's counts | **`ramfs`**, the same, in `ramproto.h` | **new**, small |
| **Which programs, and their last file** | **each of the four servers**, counting by caller - the process `SYS_SENDER` names, as the network server does since N1 - and answering `<where>/.programs`, a page at a time as a listing is | **new** |
| A program's name from its id | `sys.processes()` | as it is |
| The drive pictures - an SSD, a stick, a share, memory | drawn by the application, as Network Monitor's port is | new, in the app |

**One door, four servers.** The window does not know which server is
behind a drive: it reads `.device` and `.programs` under the drive's path,
and the namespace hands each to the server that holds the path, in that
server's declared shape. A drive that cannot say is shown with its name and
"says nothing of its reading and writing" - nothing is made up.

**Asked for, not sent** (control by message): the window asks once a pace;
the servers keep running totals they keep anyway, and a table of callers
that is a few words each.

**C or Lua.** The counting is C, in the four servers, a handful of
additions on a request that already copies bytes. The window is Lua: a few
reads a pace, differences taken, a column added, a paint.

## The busiest path

**A request**, a read or a write: the server adds its bytes and the time it
took to the drive's totals and to its caller's row - found by the process
id it already learned when that process first asked - and notes the path.
Nothing allocates per request: a caller's row is made the first time it
asks and kept, bounded by the machine's processes.

**The window**, once a pace: `.device` and `.programs` for the drive shown,
and `.device` for each drive in the list for its small graph - four or five
reads at a second's pace, each a message answered from counters.

## Steps

1. **D1 - every drive says what it did**: the drive server and `ramfs`
   answer `.device` as `diskfs` and `smbfs` do; a guest suite writes and
   reads known amounts on `/Home`, `/Temporary` and a stick and reads them
   back counted, within a request's worth.
   **Done in 0.11.172** (`testing.md` 18.566), with `deviceproto.h`
   the one shape every drive answers in.
2. **D2 - which programs**: the four servers count by caller and answer
   `.programs`; a program `diskstat` prints it all, as `netstat` does; the
   suite's own program found by its own process id.
   **Done in 0.11.174** (`testing.md` 18.568), with `progcount.c` the one
   table the four servers keep.
3. **D3 - the window**: as drawn, its suite reading the rates while a
   program writes and reads, and its picture in the dated screenshot.
   **Done in 0.11.176** (`testing.md` 18.571).

Only x86-64 runs the tests for now (`CLAUDE.md`).
