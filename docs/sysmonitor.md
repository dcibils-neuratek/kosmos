# System Monitor

Every monitor Kosmos has, in one window: the whole machine at a glance, and
a page for each part of it - its name, its clocks, its power, its
temperature and its history. Diego, 22 November 2026:

> "one of the things i want to consolidate y a system monitor app that has
> gpu monitor, disk monitor, network monitor, cpu monitor, memory,
> processes and all that we have been building in different apps."

> "i want to have 1 place to see all the system complnents usage in a way i
> can see all at once, or specigic components with specific graphs the wey
> i can see the gpu now with each part and its clock, devuce name, power in
> watts, etc."

> "i dont want to loose any of the metrics we have now, but i do want to
> have a single place tthat consolidates all monitors and then i can expan
> on each one with specific metrics and history of consumption, usage,
> temperature, etc."

`roadmap.md`, *System Monitor, every monitor in one place*. This is the
design, written before any of it is built: the feature set, the
architecture and the steps. `sysmonitor.html` is the window and
`sysmonitor-architecture.png` the picture of what it stands on.

**The contract is the inventory below.** Every row in it is something a
person can see today in one of the monitors. Each row says where it goes
in System Monitor. An old window is retired only when every one of its rows
has been seen in its new place, and a suite holds that.

---

## Every metric today

Seven windows show the machine, and two bars summarise it. They were read
for this document as they are on 22 November 2026: `sysmon.lua`,
`procs.lua`, `gpumon.lua`, `diskmon.lua`, `netmon.lua`, `llmmon.lua`, the
monitoring half of `scheduler.lua`, and the Deskbar's and Pewter's meters
in `indicators.lua`. About This Machine (`machine.lua`) is an inventory
rather than a monitor, and is at the end.

"Goes to" names a page of System Monitor: **Overview**, **CPU**,
**Memory**, **Processes**, **GPU**, **Disks**, **Network**, **LLM**, **Heat
and power**.

### Monitor - `sysmon.lua` ("System Monitor", the processors)

| # | What it shows | Where it comes from | Goes to |
|---|---|---|---|
| 1 | The chip, with the maker's mark on its lid and the part under it ("intel", "i7-6700") | `pulse.processor()` from `/Devices/cpu`'s `name`; `ui.processor_chip` | CPU card; Overview tile |
| 2 | The model ("Core i7-6700") | `pulse.processor()` | CPU card; Overview |
| 3 | How many processors ("8 processors") | `#sys.cpuload()` | CPU card |
| 4 | The rated clock ("rated 3.40 GHz") | `pulse.processor()`, the brand string | CPU card |
| 5 | The features line (VT-x, SSE4.2, AVX, AES-NI, RDRAND), virtualization green when on | `pulse.processor_features()`, `/Devices/cpu`'s raw ID words | CPU card |
| 6 | How fast the machine runs now, the mean of the cores ("3.41 GHz, now, across all cores") | `pulse.speeds`: each core's `clock_actual` and `clock_reference` (APERF/MPERF) over the pace | CPU card; Overview |
| 7 | The package's temperature ("62 °C"), or the hottest core's | `pulse.heat`: `sys.info().package_c`, else `sys.cpuload()[c].temp_c` | CPU card; Heat and power; Overview |
| 8 | The package's power ("28 W") | `pulse.heat`: two readings of `sys.info().energy_uj` (RAPL) | CPU card; Heat and power; Overview |
| 9 | Why it is held: "held: too hot" or "held: power limit" | `pulse.heat`: `sys.info().held` | CPU card; Heat and power |
| 10 | Each core's share of the time, busy | `sys.cpuload()`: `busy`, `idle` ticks, two readings | CPU graphs; Overview |
| 11 | Each core's split, user and kernel | `sys.cpuload()`: `user_counter`, `kernel_counter` | CPU graphs |
| 12 | Each core's speed ("3.41 GHz") | `pulse.speeds` | CPU graphs |
| 13 | A graph a core: kernel red under user green, 60 columns | `ui.history` | CPU graphs; Overview (small) |
| 14 | "Show each core's bar": a bar a core, user and kernel %, GHz, total | as 10-12 | CPU, the same option |
| 15 | One graph for the machine past 32 cores, and "Show a graph a core" | as 10-11 | CPU, the same rule |
| 16 | The heading: "8 processors · 23% busy" | the mean of 10 | CPU heading |
| 17 | The pace: every 0.5, 1 or 2 s, the history begun again on a change | the window kit's clock | the window's dots, for every page |
| 18 | The words over the history ("The last minute") and the foot ("A column a second") | the pace | every graph |

Computed and, until S6, not drawn: `pulse.heat`'s `limit_c` (TjMax),
`limit_w` (PL1) and `burst_w` (PL2). `clocks` prints them at the prompt.
Since S6 they are drawn on **Heat and power** and under the CPU page's
cores, with PL1's averaging window (`window_s`) beside them.

### Process Viewer - `procs.lua`

| # | What it shows | Where it comes from | Goes to |
|---|---|---|---|
| 19 | The header's line: processes, threads (so many in the kernel), spaces, up for | `sys.processes()`, `/Devices/kernel`, `sys.ticks()` | Processes header |
| 20 | What the last End did, in that line for four seconds | `sys.kill`, or the window manager's `end_process` | Processes header |
| 21 | Memory, used of total MB, as a bar | `/Devices/memory`: `total_mb`, `free_mb` | Processes band; Memory; Overview |
| 22 | Threads, of the most there can be | `/Devices/kernel`: `threads`, `threads_max` | Processes band; Memory (pools) |
| 23 | Processes, of the most | `/Devices/kernel`: `processes`, `processes_max` | Processes band; Memory (pools) |
| 24 | Endpoints, of the most | `/Devices/kernel`: `endpoints`, `endpoints_max` | Processes band; Memory (pools) |
| 25 | Column **id** | `sys.processes()` | Processes |
| 26 | Column **name** | `sys.processes()` | Processes |
| 27 | Column **file**, the whole path it runs | `sys.processes()`'s `from` | Processes |
| 28 | Column **kind** (app, program, server, driver, kernel) | `prockind.lua`, from authority, parent and the file's declarations | Processes |
| 29 | Column **privilege** (ring 3, ring 0; EL0, EL1) | `/Devices/cpu`'s `arch` and `el` | Processes |
| 30 | Column **draws** (kit, pixels, screen) | the window manager's window list, `wmproto.windows()` | Processes |
| 31 | Column **priority**, the band by name | `sys.scheduler().band_names` | Processes |
| 32 | Column **core**, the thread's home | `sys.processes()`'s `cpu` | Processes |
| 33 | Column **threads** | `sys.processes()` | Processes |
| 34 | Column **memory** ("640 KB", "4.3 MB") | `sys.processes()`'s `held` pages, `files.size` | Processes; Memory (who holds it) |
| 35 | Column **processor**, a share and its bar | `procshare.rows`, two readings | Processes; Overview (busiest) |
| 36 | Nebula's own row: what it holds, its threads (idle among them), ring 0 | `/Devices/kernel`'s `memory_kb`; threads less the processes' | Processes; Memory |
| 37 | Sort by any column, each way, ties by id, empty cells last | the window | Processes |
| 38 | Columns shown and hidden, dragged wider, fitted, reset, and kept | `ui.columns`, `prefs` | Processes |
| 39 | A selected row follows its process as the list reorders | the window | Processes |
| 40 | **End**: any process, with `needs processes` | `sys.kill`; the window manager after it | Processes |
| 41 | Refresh, the wheel | the window | Processes |

Kept and not drawn: `spaces` of `spaces_max`. It is in the header's line
as a count, and its bar goes to **Memory** with the other pools.

### GPU Monitor - `gpumon.lua`

| # | What it shows | Where it comes from | Goes to |
|---|---|---|---|
| 42 | The card: Intel's chip, the model ("HD Graphics 530"), `8086:1912`, "350 to 1150 MHz" | `gpu.stats()` (`GPU_OP_STATS`, `intelgpu`) | GPU card; Overview |
| 43 | The card's rates: the clock now, and asleep % | `gpu.stats()`, `gpu.since` | GPU card; Overview |
| 44 | Each engine's chip: Render %, Blitter %, Video -, Video enhance - | `render_busy_us`, `blitter_busy_us` over the pace | GPU engines |
| 45 | Busy over the minute, the busier of the two engines | as 44 | GPU; Overview |
| 46 | The clock over the minute, as a share of the most | `freq_mhz`, `freq_max_mhz` | GPU |
| 47 | Asleep (RC6), in the clock's heading | `rc6`, 1280 ns a count | GPU |
| 48 | Memory over the minute, bytes in buffers against 256 MB | the buffer table | GPU; Memory |
| 49 | The display: flips against frames over the minute; "an underrun seen" | `flips`, `frames`, `underrun` | GPU |
| 50 | Who holds its memory: holder, size (w × h), memory, for | the buffer table; `sys.processes()` | GPU |
| 51 | Side: Utilisation, render and blitter | as 44 | GPU side |
| 52 | Side: Clock, now of most | as 42 | GPU side |
| 53 | Side: Asleep (RC6) | as 47 | GPU side |
| 54 | Side: **Power** in watts, or "not known here" | `sys.info().gpu_energy_uj` (RAPL's PP1), two readings | GPU side; Heat and power; Overview |
| 55 | Side: Memory, in so many buffers | the buffer table | GPU side |
| 56 | Side: Display, size and measured refresh | `width`, `height`, `frames` | GPU side |
| 57 | Side: the rendering mode, its six stages coloured, what it needs | the window manager's `rendering`; `rendering.lua` | GPU side |
| 58 | Side: Vulkan "1.4, Mesa anv", OpenGL ES "not yet", the device id | written in the window; `gpu.vendor` | GPU side |
| 59 | No GPU: "No GPU this driver knows", the driver's sentence, "Under QEMU there is no Intel GPU to watch" | `gpu.stats()`'s refusal | GPU, the same words |
| 60 | `--tell`: a line a sample for a suite | the window | System Monitor's `--tell`, per page |

### Disk Monitor - `diskmon.lua`

| # | What it shows | Where it comes from | Goes to |
|---|---|---|---|
| 61 | The drives down the left: `/Home` (the disk, a stick, or memory), each stick's volumes, each share connected, `/Temporary` | `drivelist.drives()`, `netshares` | Disks list; the sidebar; Overview |
| 62 | Each drive's picture: an SSD, a stick, a share, memory | `ui.device_picture` | Disks |
| 63 | Each drive's name and where it is ("the machine's disk · /Home") | `drivelist` | Disks |
| 64 | Each drive's reading and writing now, and a small mirrored graph | `<drive>/.device` (`struct disk_device`), two readings | Disks list; Overview |
| 65 | "says nothing of its reading and writing", for a drive that does not answer | `.device` refused | Disks |
| 66 | The chosen drive's card: kind, name, where, reading now, writing now | as 64 | Disks |
| 67 | The minute, reading above the line and writing below, its scale said | `ui.history`, mirrored | Disks |
| 68 | The peak each way, in the heading | the history | Disks |
| 69 | A read takes (ms), over the pace or since the start | `read_counter_ticks` / `reads` | Disks |
| 70 | A write takes (ms) | `write_counter_ticks` / `writes` | Disks |
| 71 | Cache answered (%) | `cache_hits`, `cache_misses` | Disks; Memory |
| 72 | Read · written since the machine started | `read_bytes`, `write_bytes` | Disks |
| 73 | Programs using it: program, reading, writing, last file; "so many now" | `<drive>/.programs` (`SYS_SENDER` counts); `sys.processes()` | Disks; Processes (the selected process) |
| 74 | Bytes or bits a second | the dots | the window's dots |

### Network Monitor - `netmon.lua`

| # | What it shows | Where it comes from | Goes to |
|---|---|---|---|
| 75 | The card's maker and model ("Intel Ethernet I219-V") | `hardware.network(sys.bus())` | Network card; Overview |
| 76 | Its address and MTU, or "not on a network" | `fs.net_info("/Network")` | Network card |
| 77 | In now, out now | `fs.net_stats` (`NET_OP_STATS`), two readings | Network card; Overview |
| 78 | The minute, in above the line and out below, its scale said | `ui.history`, mirrored | Network; Overview (small) |
| 79 | The peak each way, in the heading | the history | Network |
| 80 | Received, since the machine started | `bytes_in` | Network |
| 81 | Sent | `bytes_out` | Network |
| 82 | Packets in · out | `frames_in`, `frames_out` | Network |
| 83 | Dropped, by the card's ring or the stack | `dropped_in + dropped_out` | Network |
| 84 | Connections: program, to (address:port, "listening on 80", "port 5353, datagrams"), state, in, out; busiest first; "so many open, the busiest 6" | `net_stats_conn`, `SYS_SENDER`'s process | Network; Processes (the selected process) |
| 85 | "Since the machine started 2 h 14 min ago" | `sys.ticks()` | Network foot; Overview header |
| 86 | Bytes or bits a second | the dots | the window's dots |

### LLM Monitor - `llmmon.lua`

| # | What it shows | Where it comes from | Goes to |
|---|---|---|---|
| 87 | Loaded: the model, its quantisation, its size, where it runs ("on HD Graphics 530", "4 cores AVX2"); Loading %; Nothing | `llm.stats()` (`llmproto.h`) | LLM; Overview |
| 88 | Writing: tokens a second now, the last minute's mean, the best | `answer_tokens`, `answer_us` | LLM; Overview |
| 89 | Reading: tokens a second, so many in so long, so many kept | `prompt_tokens`, `prompt_us`, `kept_tokens` | LLM |
| 90 | First word: how long a person waited, the last minute's range | `first_us` | LLM |
| 91 | Writing over five minutes, a column every two seconds | `written_tokens` over `up_us` | LLM |
| 92 | Who is asking: application, state, answers, read, written | `llm.stats().askers` | LLM; Processes (the selected process) |
| 93 | "The LLM server does not answer: ..." | `llm.stats()`'s refusal | LLM |
| 94 | **Inside**: a layer's heads and where they look, its neurons that fired, the residual stream, the candidates | `llm.inside()`, the ring of passages (`llmring.h`, `llminside.h`) | stays in LLM Monitor (see *What happens to each window*) |
| 95 | **Story**: a token's passage as a tower, the logit lens | `llm.inside_layer` | stays in LLM Monitor |

### Scheduler - `scheduler.lua`, its watching half

The Scheduler is a preference: its modes, its bands and its quantum are
choices. What it *watches* is:

| # | What it shows | Where it comes from | Goes to |
|---|---|---|---|
| 96 | Who ran, the last two seconds: a lane a core, a colour a band | `sys.sched_lane(c)` | CPU, beside the graphs |
| 97 | Switches a second | the lanes' `switches`, two readings | CPU; Overview |
| 98 | Keys' longest wait | the lanes' `waits[INPUT]` | CPU |
| 99 | Game mode: which application has it | the window manager's `game` | CPU |

### The Deskbar's and Pewter's meters - `indicators.lua`

| # | What it shows | Where it comes from | Goes to |
|---|---|---|---|
| 100 | How busy the machine is, a bar | `/Devices/kernel`'s `idle_ticks`, `busy_ticks`, two readings | stays in the bars; Overview |
| 101 | How much memory is spoken for, a bar | `/Devices/memory` | stays in the bars; Overview |
| 102 | The battery: its charge, charging | `/Devices/battery` | stays in the bars; Heat and power; Overview where there is one |
| 103 | A click on the meters opens Monitor | the bar | opens System Monitor's Overview |

### About This Machine - `machine.lua`, and the programs

About This Machine shows *what* the machine is: every device as a card,
driven or not, by kind and by connection. It shows no usage, so none of
its rows is a metric. It stays as it is. System Monitor shows a device's
name the way About This Machine does, from the same libraries (`pulse`,
`hardware`, `devices`, `gpu.stats().model`). Each page's card links to
the device's card there.

The programs at the prompt - `clocks`, `htop`, `gpustat`, `netstat`,
`diskstat`, `monitor` - are the same numbers in text. They stay, and they
read the same libraries the pages do.

**In all: 9 windows and bars, 103 rows.** Three of them (94, 95, 103) are
not graphs or numbers and stay where they are. The other 100 each have a
place in System Monitor.

---

## Sensors: what can be measured today

Diego asked for temperature and power. Here is what the hardware and
Kosmos give now, read from the code (`arch/x86_64/cpu.c`,
`kernel/thread.c`, `kernel/syscall.h`, `user/drivers/`, `hal/pc/`):

| What | M700 (i7-6700, Skylake, model 5Eh) | T14 (i5-1145G7, Tiger Lake, 8Ch) | Mac Pro (Xeon E5-1620 v2, 3Eh) | QEMU | Read by |
|---|---|---|---|---|---|
| Each core's temperature | yes | yes | yes | no | Nebula, `IA32_THERM_STATUS` on each core's tick -> `sys.cpuload()[c].temp_c` |
| The package's temperature, TjMax | yes | yes | yes ("59 C" seen) | no | `IA32_PACKAGE_THERM_STATUS`, `MSR_TEMPERATURE_TARGET` -> `sys.info()` |
| The package's power (RAPL) | yes | yes | yes ("16 W" seen) | no | `MSR_PKG_ENERGY_STATUS` -> `energy_uj` |
| Its limits PL1, PL2, the window | yes | yes | yes | no | `MSR_PKG_POWER_LIMIT` |
| Held: too hot, power limit | yes | yes | yes | no | the package's status bits |
| The GPU's power (RAPL PP1) | yes | **no**: Tiger Lake is not on the list | no GPU on the die | no | `MSR_PP1_ENERGY_STATUS` for Sandy Bridge to Comet Lake (`gpu_energy_models`) |
| The GPU's clock, sleep, engines | yes | no driver for its Xe GPU | no (the firmware's screen) | no (virtio-gpu) | `intelgpu`'s `GPU_OP_STATS` |
| The battery's charge | - | yes | - | no | `hal_battery_read`, the EC every 30 s -> `/Devices/battery` |

**What is not measured today, and what each would cost:**

- **The GPU's temperature.** An Intel GPU on the die has no sensor of its
  own, so the package's temperature stands in for it and the page says so.
  The AMD driver reads the FirePro's (`amdgpu_temperature`, from
  `CG_MULT_THERMAL`) once, into the log. Adding it to `GPU_OP_STATS` is
  small new driver work.
- **The cores' power and the memory's power.** RAPL's PP0 (`0x639`) and
  DRAM (`0x619`) planes are not read. Reading them is small work in Nebula,
  beside PP1. Which models really have the DRAM plane is not verified
  here.
- **The GPU's power on the T14.** It needs Tiger Lake added to
  `gpu_energy_models`, once it is known that the model has PP1. Not
  verified.
- **A disk's temperature.** `ahci.c` asks IDENTIFY DEVICE and nothing
  else, and nothing reads SMART. A SATA disk's temperature is SMART READ
  DATA, attribute 194, or SCT status. An NVMe drive's is its health log,
  page 02h. Both are new driver work, on a driver that must not wait on the
  drive inside a request.
- **The disk's model and size.** `ahci.c` has them from IDENTIFY and says
  them in the log. They are not in any reply. Small work: a field in
  `.device`, or `drivelist`.
- **The network link's speed.** `e1000.c` knows it and logs it, and
  `NET_OP_INFO` has no field for it. Small work.
- **Fans.** None are read. The T14's fans belong to its embedded
  controller (`hal/pc/ec.c`: "The fans and the temperature stay the
  embedded controller's"). The M700's are on a Super I/O chip that has no
  driver, and which chip it is has not been checked. This is new driver
  work, needing that chip's datasheet.
- **The battery's draw in watts, and the time left.** ACPI's `_BST` gives
  the rate, and it is not read. Small work, in the board.
- **The memory's clock and temperature.** Not read. The modules' SPD is on
  SMBus, which nothing drives. About This Machine already lists the
  memory's speed as something it cannot answer.

A page shows a measurement where the machine gives one. Where it does not,
the page says "not measured on this machine" in the measurement's place,
and never shows a 0 that looks like a reading. QEMU's processor gives none
of the above, and the suites hold that sentence (*How it is tested*).

---

## What a person can do

- **See the whole machine at once: the Overview.** One tile for each part
  that this machine has. Each tile gives its device's name, the one or two
  numbers that say how it is doing now, and a small graph of the history
  window:
  - **CPU**: the chip and its model, the share busy, the clock now, the
    package's °C and W, and a small graph for each core;
  - **Memory**: used of total, and the kernel's part;
  - **GPU**: its model, busy, the clock, asleep %, watts;
  - **each drive**: its name, reading and writing now;
  - **Network**: the card, in and out now;
  - **LLM**, while the server runs: the model, tokens a second;
  - **Heat and power**: the package, the GPU and the battery, in one line;
  - **the busiest programs**: the top five by processor, each with its
    share and memory.

  A click on a tile opens its page. Nothing that is absent gets a tile:
  under QEMU there is no GPU tile, and the M700 has no battery.
- **Open one part's page** from the sidebar, which always shows. Each row
  in it carries one live number: CPU 23%, GPU 4%, Network 340 KB/s.
- **CPU**: everything Monitor shows (rows 1-18) - the chip card with its
  features, a graph a core with kernel and user time and each core's speed,
  the bars on asking, the one graph past 32 cores. Added to that:
  - **each core's temperature**, beside its speed;
  - **the package's temperature and power over the history window**, as
    two graphs under the cores';
  - **who ran**, the Scheduler's lanes, read-only, with switches a second,
    the keys' longest wait, and which application has game mode. "Change
    in Scheduler" opens the Scheduler.
- **Memory**: used, free and the kernel's part over the history window;
  the four pools as bars - threads, processes, endpoints and address
  spaces, each of its ceiling (rows 22-24, and spaces for the first time);
  who holds the most (the ten largest processes and Nebula); and what the
  GPU's buffers and `/Temporary` hold.
- **Processes**: the Process Viewer whole (rows 19-41) - every column,
  sorting, columns shown, hidden and kept, End. Added to that, **the
  selected process in a strip under the table**: its processor share and
  memory over the minute, the drives it reads and writes (from each
  drive's `.programs`), its connections (from `NET_OP_STATS`), its GPU
  buffers and its LLM answers. Today those are four lists in four windows,
  joined by process id. Here they become one.
- **GPU**: GPU Monitor whole (rows 42-59) - the card, a chip for each
  engine (render, blitter, video, video enhance) with its share, busy, the
  clock and RC6, memory, the display, who holds its memory, and down the
  right the facts. Added to that:
  - **the power in watts over the history window**, as a graph, not only
    the number now;
  - **the temperature**, where the GPU has a sensor, or else the package's
    temperature labelled as the package's.
- **Disks**: Disk Monitor whole (rows 61-74): the drives as a list, the
  chosen one's card, its minute, how long a request takes, the cache, the
  totals and the programs. Added to that, once the driver says them, the
  disk's model and size, and its temperature after SMART.
- **Network**: Network Monitor whole (rows 75-86). Added to that, once
  `NET_OP_INFO` says it, the link's speed.
- **LLM**: LLM Monitor's main page (rows 87-93). **Inside** and **Story**
  open LLM Monitor (see *What happens to each window*).
- **Heat and power**: every temperature and every watt the machine
  measures, on one page. Each core's temperature as a row of numbers, and
  the package's against TjMax as a graph. The package's power against PL1
  and PL2, with "held" marked where it was held. The GPU's power. The
  battery's charge, and whether it is charging. Each row this machine does
  not measure says so, by name.
- **Choose the history window**: **1 minute, 10 minutes, 1 hour**, in the
  header, for every page at once. A graph is a column a pace for the last
  minute. Longer windows are kept at coarser steps, and a column there
  shows the step's mean with its highest point marked, so a spike of one
  second still shows in an hour's graph.
- **Choose the pace** - 0.5, 1 or 2 s - and **bytes or bits** a second,
  from the dots, as today, once for the whole window.
- **Hover over a column** to read its time and its values ("14:02:31 -
  render 41%, 1100 MHz, 15.8 W").
- **Open System Monitor on a page** from outside: `sysmon gpu`, `sysmon
  disks /Home`. The Deskbar's meters open the Overview. `--tell` prints a
  line a sample for each page shown, for the suites.

---

## The architecture

**The application is the thinnest layer** (*Kits supply, applications
orchestrate*). System Monitor counts nothing. Every number is counted by
the server, driver or kernel that owns the thing, as today. Each answers a
declared shape, and the window asks on its own clock. Three things are
new: a C kit for history, a Lua library that turns readings into rates,
and pages that are mostly the old windows' code moved.

### Every piece, and who supplies it

| Piece | Supplied by | New or as it is |
|---|---|---|
| The window, the header, the sidebar, menus, the table's columns, the card, the device pictures, the chip | the UI kit: `ui.window`, `ui.header`, `ui.sidebar` (Preferences'), `ui.columns` (Tracker's and Processes'), `ui.meter_card`, `ui.device_picture`, `ui.processor_chip`, `ui.group_heading` | as it is |
| **A series of numbers over time**: rings at three steps (a pace, 10 s, 60 s), each column keeping its mean and its highest, drawn into a surface stacked, mirrored or as a line | **the Series Kit**, `user/kits/series/`, `/Kosmos/Kits/series`, C. Its ring is a declared shape in `user/include/seriesring.h` | **new**. `ui.history` becomes a door to it, so every monitor and its suite draw as before |
| **Readings turned into rates**: two snapshots of a source and the seconds between them, giving the rates and the facts | **`meters.lua`**, `user/lib/meters.lua`: one function for each source. Today these differences are worked out inline five times - `sysmon`'s `split`, `diskmon`'s and `netmon`'s rate loops, `indicators.lua`'s busy, `procs`' totals | **new**, moved out of the windows rather than written again |
| The processor's name, features, speeds, heat | `pulse.lua` | as it is. `sysmon`'s `split` moves into it as `pulse.split` |
| The GPU's snapshot and rates | `gpu.lua`: `gpu.stats`, `gpu.since`, `gpu.vendor` | as it is |
| A process's share, its kind | `procshare.lua`, `prockind.lua` | as it is |
| The drives, the shares | `drivelist.lua`, `netshares.lua` | as it is |
| The card's name | `hardware.lua` | as it is |
| The rendering mode's stages | `rendering.lua` | as it is |
| The lanes' drawing (who ran) | moved out of `scheduler.lua` into the kit as **`ui.lanes`**, so the Scheduler and the CPU page draw with one copy | **moved** |
| **The pages** | `user/lib/sysmon/`: `overview.lua`, `cpu.lua`, `memory.lua`, `processes.lua`, `gpu.lua`, `disks.lua`, `network.lua`, `llm.lua`, `heat.lua`. Each is a view and a `sample`, the old window's body moved in | **moved**, and new for Memory, Heat and power and the Overview |
| Processor time, user and kernel, speeds, each core's temperature | Nebula: `sys.cpuload()` | as it is |
| The package's heat, RAPL's energy, PP1, the limits | Nebula: `sys.info()` | as it is. PP0 and DRAM later, small |
| Processes and their threads, memory, band, core, file | Nebula: `sys.processes()`; the band names `sys.scheduler()` | as it is |
| Who ran, switches, the keys' wait | Nebula: `sys.sched_lane(c)` | as it is |
| Memory and the pools | the devices server: `/Devices/memory`, `/Devices/kernel` | as it is |
| The battery | the devices server: `/Devices/battery` | as it is |
| The GPU's counters and buffers | `intelgpu`: `GPU_OP_STATS` | as it is. The AMD driver's temperature later |
| Network counts and connections | the network server: `NET_OP_STATS`, `NET_OP_INFO` | as it is. The link's speed later |
| A drive's counts and its programs | each drive's server - `diskfs`, `ramfs`, `smbfs`, the sticks' - `.device` and `.programs` (`deviceproto.h`) | as it is. The model and size, then SMART, later |
| The LLM's counts and askers | the LLM server: `llm.stats()` | as it is |
| How each window draws; game mode | the window manager: `wmproto.windows()`, `rendering`, `game` | as it is |
| Ending a process | Nebula: `sys.kill` with `needs processes` (`SPAWN_PROCCTL`) | as it is |
| **History from the machine's start** | **a recorder**, a server in C that samples the totals every second and writes the rings into a region the Deskbar and System Monitor map read-only | **for Diego to decide**, decision 2 |

**Does another application want this?** The Series Kit is wanted by every
monitor, the Deskbar's and Pewter's meters, LLM Monitor's writing graph,
the Scheduler's lanes, and later Music's levels and Groove's period
timings. `meters.lua` is wanted by the bars (`indicators.lua` keeps its
own busy arithmetic today, a second copy of the arithmetic in `sysmon`)
and by the programs at the prompt. The pages are System Monitor's own.

**Does something already supply this?** `ui.history` already draws a
minute. It is the Series Kit's first user and becomes its door, so it does
not stay as a second copy. `ui.sidebar`, `ui.columns`, `ui.meter_card` and
the device pictures are used as they are. Nothing in the table above is
written twice.

### Why a kit for history, and why in C

`ui.history.push` keeps a Lua table for every sample: `{ kernel, user }`,
one table a column a series, with `table.remove(list, 1)` past sixty. For a
minute that is fine. An hour at a second is 3,600 tables a series. The CPU
page alone has eight cores, two series each, plus speeds and temperatures.
That is a hundred thousand small tables for the collector to walk, in a
window that repaints every second. `ui.history.draw` is also a `g:fill`
from Lua for every column. That is sixty a graph today, and 600 a graph
over ten minutes drawn a pixel a column.

So history is **numbers in a flat ring in C, drawn by C into the surface**.
Pixels never go in a Lua table (`gfx.md` §19.1). These are not pixels, but
the reason is the same: the collector should not walk data that only C
reads. Lua says what to draw and where. `series:draw(g, x, y, w, h, spec)`
is one call a graph. This is the *loop over bytes* test in `CLAUDE.md`: a
column loop is one, and the kit is where it belongs. The vector unit is not
needed for drawing columns of a rectangle each. The consolidation - mean
and highest over ten samples - is a plain loop, and the host test holds
it.

**The ring's shape**, in `seriesring.h`: a header (the number of series,
the three steps, each step's column count and head index, the pace in
counter ticks), then each series' name and unit, then for each step
`columns × series` pairs of `double`, the mean and the highest, and each
column's start on the counter (for S7's readout), with one spare slot a
step so a reader never meets the slot being written. Each series gets a
fixed 600 columns at the pace, 360 at 10 s and 1,440 at 60 s: about 57 KB
a series as built, and about 3.1 MB for the 80 series the M700 would keep.
**It said `float` until S1 was built**, at 19 KB a series: a float holds a
byte rate exactly only to 16 MB, and the mirrored graphs print their scale,
so the lane that built it chose doubles and said so (0.12.97). The window's own rings live in its own memory. The
recorder's, if Diego chooses it, are the same shape in a region.

### What travels in a message and what in a region

- **Every reading is a message**, asked on the window's clock: one request,
  one reply of numbers in a declared shape - `GPU_OP_STATS`,
  `NET_OP_STATS`, `struct disk_device`, `llm.stats` - or a syscall that
  answers from the kernel's own counters (`sys.cpuload`, `sys.info`,
  `sys.processes`, `sys.sched_lane`). These are one-shot answers to a
  question. They recur because the window chose a pace, not because
  hardware says so. They pass the rate test in *Control by message, data by
  shared memory*: each happens because somebody asked.
- **The lists stay as they are**: a page of connections into a region the
  caller owns (`NET_OP_STATS`), a page of programs (`.programs`), the GPU's
  buffer table. They are asked only while their page shows.
- **History, if it is kept by a recorder, is a region.** A column every
  second, forever, recurs because a clock came round, so by the rule it
  lives in a region. The recorder writes, every reader only reads, and the
  only thing both touch is each step's head index, published with a
  release store after the column is written (single producer, many
  readers: a reader that reads the head, then the column, then the head
  again, and finds it moved, reads again). Kept inside the window, it is
  the window's own memory and nothing crosses.

### C or Lua

| Piece | Language | Why |
|---|---|---|
| The pages, the Overview, the sidebar, the sampling | Lua | they run for a person. They are layout, a few dozen subtractions a second and a table of at most a few hundred rows. Structure-shaped Lua costs about 2% |
| `meters.lua` | Lua | differences of a few dozen counters; host-tested as `test_pulse.lua` is |
| **The Series Kit** | **C** | a loop over columns, on every repaint and every sample, and data the collector should never walk |
| The recorder (decision 2) | C | a server: it runs on behalf of other processes, and its timing is theirs (*Language split*) |
| Every counter | C, where it is today | in Nebula, the drivers and the servers; nothing changes |

### Sampling, and what each page asks

**Every source is asked once a pace (a second to start), whatever page
shows**, so a page opened after ten minutes has ten minutes of history.
Only the totals are asked always, and they are cheap fixed shapes:

| Asked every pace | Cost |
|---|---|
| `sys.cpuload()`, `sys.info()`, `sys.sched_lane(c)` for each core | syscalls; no server |
| `/Devices/memory`, `/Devices/kernel` | two messages to the devices server |
| `gpu.stats()` | one message to `intelgpu`: a handful of register reads and its buffer table |
| `fs.net_stats` | one message to the network server; its first page of connections |
| `<drive>/.device` for each drive | one message to each drive's server: `/Home`, `/Temporary`, each stick, each share |
| `llm.stats()`, while `/Running/llm` exists | one message |
| `/Devices/battery`, where there is one | one message |
| `sys.processes()` | a syscall, about a hundred rows: for Processes' table and the Overview's busiest five |

**Asked only while their page shows**: a drive's `.programs`, the window
manager's window list (Processes' **draws** column) and `rendering` (the
GPU page's mode), the window manager's `game` (CPU).

**About 12 messages and 5 syscalls a second on the M700**, with four
drives. Today the Deskbar, Monitor, Processes and GPU Monitor, all open,
ask about the same between them. This number is counted, not measured, and
step S3 measures it.

**Nothing waits on a server, and this window asks eight of them.** A
window draws what it last heard. A source that has not answered this pace
keeps its last column, and the next column is a gap, not a zero. But a
call blocks, and today a server that never answers holds whoever asked.
That is the 23-second Deskbar of `testing.md` 18.345. With one window
asking eight servers, one stuck server would freeze every page. So:

1. Each source is asked in turn in the tick, the cheap ones first, and a
   source whose last answer took longer than a quarter of the pace is asked
   only every fourth pace and marked "slow to answer" on its page.
2. When `roadmap.md`'s **call with a deadline** exists, every ask uses it,
   and the rule above goes away. System Monitor is the strongest argument
   for that call so far.
3. The recorder (decision 2) would take the window off the servers
   altogether: it would read a region and ask nobody.

### The busiest path: the Overview, every second

1. **The tick.** The window kit's clock, at the pace.
2. **Ask** the totals above: about 17 answers, each a fixed shape.
3. **Take the differences** in `meters.lua` and push one column into each
   series: about 80 floats written into C rings. No table is made for
   history. The replies themselves are still tables (`fs.read`'s), as in
   every monitor today.
4. **Paint the Overview**: eight tiles, each a card (`ui.meter_card`'s
   fills and text), two or three numbers, and one `series:draw` for its
   graph - about ten C calls for the graphs where `ui.history.draw` would
   make about a thousand `g:fill`s - and the busiest five's rows.
5. **Commit** the surface to the window manager, as every window does.

**Where the time goes, expected and not yet measured**: in step 4's text
and card fills, which are C already. Then in step 2's round trips, about
a dozen. The only round trip measured so far is `wmlatency`'s, 0.31 ms
to the window manager on an idle desktop (`state-history.md`). That is a
QEMU number, so it gives only the shape. Then step 3's Lua, a few dozen subtractions.
The graphs, which would have been the largest part in Lua, become the
smallest. **Step S3 measures it**: `--tell` prints, once every ten samples,
how long the ask, the differences and the paint took, by the counter, and
what the sample allocated (`collectgarbage("count")` before and after). It
is measured on the M700 and the T14, not under TCG. The aim is the whole
second's work under 5 ms on the M700 with the Overview showing. That
number is to be checked, not claimed.

**A page other than the Overview** costs the same asking and one page's
paint. The Processes page adds the table, as Processes costs today. The
CPU page past 16 cores is the most graphs at once: thirty-two, one C call
each.

### What happens to each window

| Window | What happens | When |
|---|---|---|
| **Monitor** (`sysmon.lua`) | **Becomes System Monitor.** The file, the name "System Monitor" it already carries, its icon and its place in the menu stay. Its window becomes the CPU page, and its suite's Monitor half follows the page | S3 |
| **Process Viewer** (`procs.lua`) | **Retired** once the Processes page shows rows 19-41. `procs` at the prompt and its launcher open System Monitor on Processes (decision 3) | S9 |
| **GPU Monitor** (`gpumon.lua`) | **Retired** once the GPU page shows rows 42-60 | S9 |
| **Disk Monitor** (`diskmon.lua`) | **Retired** once the Disks page shows rows 61-74 | S9 |
| **Network Monitor** (`netmon.lua`) | **Retired** once the Network page shows rows 75-86 | S9 |
| **LLM Monitor** (`llmmon.lua`) | **Kept**, for Inside and Story, which are a lesson in how a model works rather than a meter (decision 4). Its main page's rows 87-93 are System Monitor's LLM page as well, drawn from the same `meters.lua` source, so they are not a second copy of the arithmetic | S8 |
| **Scheduler** (`scheduler.lua`) | **Kept**: it is a preference. Its lanes move into the kit as `ui.lanes`, and the CPU page draws them read-only | S6 |
| **About This Machine** (`machine.lua`) | **Kept**: an inventory, not a monitor. Each page's card opens the device's card there | as it is |
| **The Deskbar's and Pewter's meters** | **Kept**, reading `meters.lua`. A click opens the Overview | S3 |
| `clocks`, `htop`, `gpustat`, `netstat`, `diskstat`, `monitor` | **Kept** at the prompt, on the same libraries | as they are |

**A window goes only when its rows are proven in their new place.** Step S9
removes a window only after the inventory check below passes for every row
of that window.

### How it is tested

Today each monitor has a suite under QEMU that opens it, reads what it says
with `--tell` or its printed lines, and clicks it with QEMU's tablet:
`x86-sysapps` (`run_sysapps.py`: Processes' headings sorted and Monitor's
paces held to the counter), `x86-gpumon`, `x86-diskmon`, `x86-netmon` and
`x86-llmmon`. Each runs in the gate when what it stands on changed. On the
Mac, `test_pulse.lua` and `test_procshare` hold the arithmetic.

- **On the Mac**:
  - `tools/test_series.c`, new: the rings - push, wrap, the coarser steps'
    mean and highest, a gap - and `draw` held pixel for pixel to
    `ui.history.draw` for the stacked and mirrored modes (the control: a
    graph drawn one column off fails).
  - `tools/test_meters.lua`, new: every source's differences from recorded
    readings - the M700's from `testing.md` (`gpustat`'s 17 October lines,
    the heat's) - including a counter that wrapped (PP1's 32 bits, RC6's),
    a server started again, and a reading missing.
- **Under QEMU** - **`x86-sysmon`**, split into two halves that run side by
  side to keep within the budget, as the browser's suite was:
  - **the inventory check**: System Monitor prints, with `--tell`, the
    label and value of every row on every page it can show. The suite
    holds them to the list in this document, a row number each. **A row
    missing from its page fails the suite.** This is the "lose nothing"
    contract, made into a check that is never deleted. Its control: a row
    removed from a page fails it.
  - each page opened from the sidebar by a click, and its first line read;
  - the paces and the history windows held to the counter, as
    `run_sysapps.py` holds Monitor's today;
  - Processes' headings sorted and End used on `spin`, as today;
  - QEMU's absences: no GPU tile and the GPU page's "Under QEMU there is no
    Intel GPU to watch"; "not measured on this machine" for every heat
    row; no battery tile;
  - Disk's and Network's rates while a transfer runs, as `x86-diskmon` and
    `x86-netmon` do now.
  - Their checks move into `x86-sysmon` page by page. The old suites go in
    S9 with their windows, never before.
- **On the M700**: the readings only metal gives - the GPU's clock, RC6,
  engines and watts, the package's °C and W, the cores' temperatures -
  seen to move while Flight runs, and the cost of a second measured (S3).
  The T14 for the battery.
- **The picture**: the dated screenshot shows System Monitor on its
  Overview in place of Monitor, Processes and GPU Monitor (*A new
  application joins the picture*).

Only x86-64 runs the tests for now (`CLAUDE.md`).

---

## Steps

Each is small, has its own check, and leaves every existing monitor
working until S9.

1. **S1 - the Series Kit.** `user/kits/series/`, `seriesring.h`: rings at
   three steps, mean and highest, gaps, `draw` stacked, mirrored and as a
   line. `ui.history` becomes its door, and Monitor, Disk, Network, GPU and
   LLM Monitor draw through it unchanged. Their suites pass as they are.
   `test_series.c`.
2. **S2 - `meters.lua`.** Each source's snapshot and differences, moved out
   of `sysmon`, `procs`, `gpumon`, `diskmon`, `netmon`, `llmmon` and
   `indicators.lua`, and each of those reading it. Their suites stay as
   they are. `test_meters.lua` with the M700's readings.
3. **S3 - the window, the Overview and CPU.** `sysmon.lua` becomes System
   Monitor: the sidebar, the Overview's tiles, the CPU page (Monitor's
   window moved, with each core's temperature added). The Deskbar opens
   the Overview. `x86-sysmon` begins with the inventory check for rows
   1-18. **The cost of a second measured** on the M700.
4. **S4 - Processes.** Process Viewer's table, band and End as a page.
   Rows 19-41 into the inventory check, with `run_sysapps.py`'s heading
   checks moved over. The selected process's strip: its share and memory
   over the minute.
5. **S5 - GPU, Disks, Network.** Each window's body moved in as a page,
   rows 42-86 into the check, with the suites' checks. The GPU's watts as a
   graph. The selected process's strip gains its drives, connections and
   buffers.
6. **S6 - Memory, Heat and power, and who ran.** The two new pages, from
   readings that exist today. TjMax, PL1 and PL2 drawn for the first time.
   `ui.lanes` moved out of the Scheduler, and the CPU page's lanes and
   tiles (rows 96-99). **Built, 24 November 2026** (0.12.107), folded from
   its lane; the pages agreed by Diego from the mockup ("S6 mockup is
   fine"). With it every page lays out at the window's width: a size
   dragged by the grip is kept, no page drawn narrower than its own, and
   the next page shown at it (`x86-sysmon`'s grip check).
7. **S7 - the history windows.** 1 minute, 10 minutes and 1 hour from the
   kit's three steps, the highest marked, the hover's readout.
   `test_series.c` holds the steps against a fake clock.
8. **S8 - LLM.** LLM Monitor's main page as a page (rows 87-93), from
   `meters.lua`. Inside and Story open LLM Monitor.
9. **S9 - the old windows go.** Process Viewer, GPU Monitor, Disk Monitor
   and Network Monitor removed, each only with its rows all passing in
   `x86-sysmon`. Their names open the page (decision 3). Their suites
   removed with them. The dated screenshot changed.
10. **S10 - the recorder**, if Diego chooses it (decision 2): a C server
    keeping the rings in a region from the machine's start, the Deskbar
    and System Monitor mapping it read-only.
11. **Sensors, each a step of its own, when Diego wants them** (decision
    5): the disk's model and size in `.device`; the link's speed in
    `NET_OP_INFO`; RAPL's PP0 and DRAM planes; PP1 on Tiger Lake; the
    AMD GPU's temperature in `GPU_OP_STATS`; SMART temperature in `ahci`;
    the battery's draw. Each comes with its row on its page and its line
    in the inventory.

---

## Decisions - agreed

**Agreed on 22 November 2026, every recommendation taken** (Diego, 22 November: "agreed with your recommendations, go ahead with usb audio"). Each decision below stands as recommended.

1. **What the window is called, and what it opens on.** "System Monitor",
   the name `sysmon.lua` already carries, opening on the Overview. The
   Deskbar's meters open it too. **Recommended**: yes to both.
2. **History from the machine's start, or from when the window opened.**
   (a) The window keeps its own history: nothing new runs when it is
   closed, and the graphs begin empty each time it opens. (b) A recorder,
   a small server in C, samples the totals every second from boot and
   keeps an hour (or a day at a minute a column) in a region. System
   Monitor and the Deskbar read it, so "history of consumption" means since
   the machine started, and the window asks no server at all. It costs a
   process and about a dozen messages a second, always (counted, not
   measured). **Recommended**: (a) first, through S9, and (b) as S10 once
   you have used the window. The ring is the same shape either way, so
   nothing is thrown away.
3. **What the old names do once their windows go.** `procs`, `gpumon`,
   `diskmon` and `netmon`, at the prompt and in the menu, could open
   System Monitor on their page, or be removed with their windows.
   **Recommended**: open the page. It costs four lines, and every launcher
   and habit keeps working.
4. **LLM Monitor.** Kept for Inside and Story, with its meters also on
   System Monitor's LLM page. Or moved in whole, with Inside and Story as
   tabs of the LLM page. **Recommended**: kept. Inside and Story are a
   teaching tool with a page each and their own data, a ring the server
   fills only while one is open. They are not a meter, and the window is
   already the largest monitor (1,122 lines).
5. **Which new sensors, and in what order.** None of them is needed for
   S1-S9. **Recommended**, cheapest first: the disk's model and size and
   the link's speed (each a field in an existing reply); then RAPL's PP0
   and DRAM planes and PP1 on Tiger Lake (small, in Nebula, once the models
   are checked against Linux's `intel_rapl`); then the AMD GPU's
   temperature; then SMART on the M700's SATA disk. Fans last, if at all:
   they need the M700's Super I/O chip identified and a driver for it, and
   on the T14 they belong to the embedded controller.
6. **The history windows.** 1 minute, 10 minutes and 1 hour, at a pace, 10
   s and 60 s a column. A day would need the recorder. **Recommended**:
   those three, and a day only with (b).
7. **Ending a process from a window that also reads everything.** System
   Monitor needs `needs processes` (End) and `needs network`, which
   Processes and Network Monitor each held alone. A single window now holds
   both. Nothing it reads is secret - `sys.processes()` is anyone's - and
   the authority is the same one Processes had. **Recommended**: accept
   it, and say so in the window's header comment.
