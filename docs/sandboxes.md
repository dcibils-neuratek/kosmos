# Sandboxes, the application

Small operating systems - Linux first - started for a task or kept as a
service, each given only what is chosen for it. Diego, 11 October 2026: "to
launch sandboxes to run small OSes like linux for specific tasks", "can
firecracker launch long lasting processes. like a http server in linux?",
"can you mockup how sandbox app and system would look like to launch new
sandboxes and manage and monitor them?" - and, on 12 October, with Alpine
already at a prompt under Kosmos: "continue with the sandbox app", "as
designed in the mockup".

The window is `docs/sandboxes.html`, agreed on 11 October ("sandbox mockup is
perfect"). What a guest *is* - the virtual CPU, its memory, the monitor and
its devices - is `docs/virtualization.md`, V1 to V3 built. This is the
feature set and the architecture of the application and the program on top
of it; `sandboxes-architecture.png` is the picture.

## What a person can do

In the window, as drawn:

- **See every sandbox** as a card: what it runs and for how long, a pill for
  its state - running, a task, stopped - its processors and memory as they
  are used now, and what it was given: ports, folders, network. The sidebar
  groups them - All, Running, Stopped, Tasks - lists the **images** there are
  to start from, and says what the **machine** has: its processors, how much
  memory is given, and whether it can run a guest at all.
- **Start, stop, restart and delete** one from its card.
- **Make a new one**: a name, an image, a task (run once, thrown away) or a
  service (kept running), its command, processors, memory, folders of
  `/Home` each read only or read and write, and the network - none, outgoing
  only, or ports forwarded from Kosmos into it - and whether it starts with
  Kosmos. Nothing is given that is not chosen there.
- **Watch one**: its processors, memory and network over the last minute,
  what it was given, and the last lines of its console; the tabs - the whole
  **console, typed into**, its folders and ports, its log, its settings.
- **Get an image** - Alpine's, as a package (`pkg install alpine`).

At the Terminal, the same through `sandbox`: `sandbox run IMAGE [--share
FOLDER[:PATH[:ro]]] [--port N:M] -- COMMAND` for a task, which says how fast
it came up and its command's exit, and throws the guest away; `sandbox list`,
`start`, `stop`, and `console` into a running one.

**Built in this order, and said when it is not there yet**: the cards,
groups, images, machine, New sandbox, start and stop, and the console come
first, on what V3 built; a **folder** needs virtio-fs (V4c) and **ports and
the network** virtio-net (V4d), so until each lands the New sandbox sheet
shows its row greyed with "not yet"; the memory a guest *uses* needs the
guest to say so (a balloon, later) - until then a card says what it was
**given**. And **on the M700 nothing runs yet**: guests need VT-x there,
which is the second architecture's step; the window says "this machine
cannot run sandboxes yet" in the Machine panel, and everything else works
from the definitions.

## What it is made of

**The application is the thinnest layer there is** (`CLAUDE.md`): the window
and `sandbox` show and ask; one library does the work, and each sandbox's
monitor is the only thing that touches its guest.

| Piece | Supplied by | New or as it is |
|---|---|---|
| The window: cards, sidebar, sheet, tabs, graphs | the UI kit - `ui.cards`, `ui.sidebar`, `ui.sheet`, `ui.tabs`, `ui.history`, `ui.meter_card` | as it is |
| **A sandbox's definition**: image, kind, command, processors, memory, folders, ports, network, starts | `/Home/Sandboxes/<name>/sandbox.txt`, a line a fact, as `package.txt` is | **new** |
| **Images**: a kernel and an initramfs | `/Home/Sandboxes/Images/<image>/`, installed as a package by `pkg` | **new**, in `tools/packages.py` and `tools/alpine.py` |
| **Reading definitions, starting, stopping, listing, attaching to a console** | **`sandboxes.lua`**, one library, for the window and `sandbox` alike | **new**, from what `sandbox.lua` does now |
| Loading a kernel into the guest's RAM | `fs.read_into` and `sys.region_copy`, through a window, never a Lua string | as V3 built it |
| Starting a monitor | `fs.vm_start`, which hands it the regions and its name in `/Running` | **grows**: no console of the caller's any more |
| **A running sandbox**: its guest, devices, console | **`vmm`**, one process each, registered as `/Running/sandbox-<name>` and answering `vmmproto.h`: its state, its console's bytes from an offset, keys in, stop | **grows**: from V3's monitor, which wrote to its starter's console |
| Its console's history | a ring in `vmm`, read by offset, so a window opened later sees the last lines and a Terminal attached later the rest | **new** |
| Processors and memory, as used | `/Running`'s process table - `vmm`'s threads, its vCPU among them, and its memory, the guest's counted to it | as it is (Processes shows them) |
| A guest's network graph | `vmm`'s own counters, once virtio-net is there | later, V4d |
| Folders | virtio-fs in `vmm`, against `/Home` through the namespace it is handed | later, V4c |
| Started with Kosmos | the startup list, which runs `sandbox start` for each definition that says so | **grows** |

**C or Lua.** `vmm` is C - it is on every exit of its guest - and the console
ring is its. Everything else is Lua: a sandbox is started a few times a day,
and a window that asks for state twice a second asks a server, which is a
message, not a loop over bytes.

**What crosses where.** The guest's RAM is a region, given at the start. The
console is text a person reads, asked for by offset - the same as the
console server's writes, a message each, because it recurs because somebody
typed or a program printed, not because a clock came round. Keys go in a
message. Nothing streams.

## The busiest path

**A guest printing**: the guest writes its serial port, which stops it
(`vmm`'s PL011, an exit a byte); the byte goes into the ring; a window
watching asks for what is new twice a second, and a Terminal attached asks
when it has drawn. So the cost a byte is one exit - about 1,300 counter
ticks under TCG, unmeasured on hardware - and the window's cost is two
messages a second, whatever the guest prints. Byte-at-a-time exits are the
PL011's nature; a busy console is what virtio-console is for, later.

## Steps

1. **S1 - the monitor as a server**: the console ring, `vmmproto.h`'s
   operations, `/Running/sandbox-<name>`; `sandboxes.lua`; definitions in
   `/Home/Sandboxes/<name>/sandbox.txt` and images in `Images/`; Alpine as a
   package; `sandbox run`, `list`, `start`, `stop`, `console`.
   **Done in 0.11.185** (`testing.md` 18.580). As built: `vmm` keeps its
   guest's console in a 64 KB ring read by offset and takes keys by message
   (its vCPU's thread and its serving thread share an SPSC ring for them),
   lingers ten seconds after its guest ends so the last lines and the exit
   code can be read, and goes at once when asked to stop; a task's exit code
   comes on a line marked with a token the starter put on the guest's
   command line (`kosmos.end=`), which the monitor keeps off the console;
   the command goes as hexadecimal (`kosmos.run=`); one initramfs serves a
   task, a service and a shell. A spawn hands `vmm` `/Running`, not a
   console, and `fs.raw` forgets a registry name whose process has gone.
2. **S2 - the window**, as drawn: cards, groups, images, machine, New
   sandbox, the watched sandbox with its console typed into.
3. **S3 - folders** (V4c): virtio-mmio and virtio-fs in `vmm`.
4. **S4 - the network** (V4d): virtio-net, outgoing through Kosmos's stack,
   and ports forwarded.
5. **S5 - started with Kosmos**, and **VT-x** - sandboxes on the M700.

Only x86-64 runs the tests, except Sandboxes' own (`CLAUDE.md`).
