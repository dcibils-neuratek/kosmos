# State

**Update at the end of every session.** This file is what keeps you from starting over each time.

Last updated: 2026-08-31

---

## Current milestone

**M6 — Graphics and the app server. In progress.** There is a framebuffer, a
surface type, a blitter, an 8x16 font, a screen console with a scroll region
and a blinking cursor, a keyboard, and a shell that runs programs from `/bin`.

Known and not yet fixed: the window manager cannot usefully be run detached,
because it and the shell's line editor would both be draining one keyboard
and whichever asks first wins. The Terminal app is the answer - once the
shell is a window there is one reader. The framebuffer half of that problem
is already solved: a process that owns the screen takes it, and the console
stops drawing.

Still missing, and the next things to build: **a backbuffer with damage
tracking**, and then **the app server** on top of it. Today a program draws
straight at the scanned-out framebuffer, so a slow draw is visible as it
happens and two programs drawing at once would fight.

**M5 — Namespaces and servers. Done.** Its definition of done is met, both halves, and the last item on the list — taking Lua out of the kernel — is done as well.

**M4 — Lua to userspace.** Its definition of done is met. Two of its listed pieces are not built; see below.

**M3 — Microkernel. Done.**

**M2 — Lua in the kernel + second target**, whose remaining half is the second target and is blocked on cables.

Definition of done: a `>` prompt over serial where `2+2` returns `4`, under QEMU **and** on real hardware.

**The QEMU half is done.** The prompt runs Lua 5.4.8 with coroutines, closures, the string and math libraries, and errors caught by `pcall`.

**The hardware half is blocked on cables** and is the only thing left in M2.

**M0 and M1 are closed.**

## Active target

QEMU `virt` aarch64, and nothing else. Real hardware arrives at M2.

## Recently done

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
  without a rebuild. `edit /data/x.lua`, Control-S, Control-Q, then
  `run /data/x.lua`.

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
Try: fs.list("/data")   fs.read("/data/sensor")   2+2

kosmos> 2+2
4
kosmos> fs.write("/data/sensor", { celsius = 47.2, unit = "C" })
true	nil
kosmos> fs.read("/data/sensor").celsius
47.2
kosmos> fs.list("/data")[1]
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
- [ ] A backbuffer and damage tracking
- [x] An 8x16 bitmap font
- [x] A narrated boot with a progress bar, on the screen and on the serial line
- [x] The shell visible on the screen, and `help` at the prompt
- [ ] The app server in Lua: windows, decoration, stacking, focus
- [x] Input beyond the serial line — **virtio-input over virtio-mmio.** `virt` has 32 virtio-mmio transports at 0xa000000, stride 0x200, SPI 16 upward, and `virtio-keyboard-device` attaches to that bus. mmio rather than PCI is the whole point: no ECAM walk and no capability parsing, so it is a few fixed registers and one virtqueue — and the same transport then gives `virtio-gpu-device`, which is where real dirty-rectangle flush and vblank come from. The keyboard pays for the GPU.
- [ ] **Definition of done: drag a window with a hung app inside it, and have the window keep moving smoothly**

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

**The working directory travels with a program.** It is the shell's idea and no server knows about it, so it goes in the request rather than being asked for. `cd /data` then `cat notes` works, and `cat` is a program that has never heard of the shell.

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

**The namespace has a root now, and it is the one thing no server can answer.** `ls /` used to say "no such path" while `/data` and `/dev` both plainly existed — because nothing is mounted at `/`, and a path with no server behind it does not resolve. A server knows what it holds; only the namespace knows what has been *attached* to it and where, and that table lives in the process.

So `ns.list` returns whatever the server said **plus** whatever is mounted below the path, both being true: `/dev` holds `cpu` because the device server says so, and holds `console` because something else was attached there. A path with no server but with mounts under it — which is exactly what `/` is — is a directory made entirely of mount points.

Worth being clear about what this is not: there is a filesystem, and it is `servers`-in-memory. The ramfs at `/data` is a real server answering the real protocol; what M8 adds is persistence, attributes and queries, not the idea.

**`ls`, `cd`, `pwd`, `cat`, and a working directory that lives in the shell.** Not in the kernel and not in a server: a server is always told a whole path and knows nothing about where anybody thinks they are, which is what keeps `fs.read` the same operation for every caller. And "is this a directory" is answered by asking whether whoever serves it will list it — the only definition that means anything across three different servers.

**A leading slash always means a command**, and that came out of a question worth recording: aliases and Lua names can collide. `/ps` is unambiguous; a bare `ps` is treated as a command only when it does not also name something in Lua. So `type` gives you Lua's function and `/type` runs the alias you gave that name. Refusing to guess is the point — a shell where `type` sometimes means a command and sometimes means the function is a shell you cannot write anything in.

**CPU usage is the difference between two readings, never one.** The kernel charges every timer tick to the idle thread or to everything else, and both counters only rise. A single reading says what fraction of *all time since boot* was busy, which after a minute at a prompt is a number that never moves again. `ps` says so on the first call rather than printing a meaningless 0%.

Sampling at the tick rather than accumulating real time per thread is deliberate: accumulating would mean reading the counter twice on every context switch — a cost on the hottest path in the kernel to answer a question nobody asks more than once a second.

**The shell takes commands as well as Lua**, with aliases. A line is a command when its first word names one *and the rest has no Lua punctuation in it* — so `help` and `help gfx` are commands while `help("gfx")` stays an expression. Both work, which matters because the parentheses are simultaneously what people forget and what they reach for.

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

**M6 — graphics and the app server.** M5 is closed, so this is next, and `roadmap.md` names its definition of done: drag a window with a hung app inside it and have the window keep moving smoothly. That is the BeOS test, and it is the one that decides whether the system feels good.

What it needs first, in the order it needs it:

- **virtio-gpu under QEMU.** There is no framebuffer today. Nothing in M0–M5 draws a pixel; the console is a UART.
- **`gfx.surface` as userdata over flat bytes**, with the C primitive set (`fill`, `blit`, `blend`, `span`, `get`/`set`). `CLAUDE.md` is absolute that pixels never go into a Lua table and nothing in Lua computes a pixel offset — a Lua array holding 2M pixels makes the GC walk 2M slots per cycle. `gc_pause_max` is baselined and will say immediately if that rule is broken.
- **A backbuffer in cached RAM with a dirty-rectangle blit.** An uncached framebuffer is 10–50× slower, and drawing straight into it is the mistake that kills the whole thing.
- **The app server in Lua**, over the same IPC everything else already uses.

**Two things this session's work bears on directly.** `sys.ticks()` exists now, so input latency can be measured from event to pixel with a timestamp at each end, which is what `testing.md` §18.5 asks for. And the app server will be chatty — a round trip per drawing command is the shape to avoid, and `ipc_roundtrip` at 30 ticks against `serialize` at 1,365 is the number that says how much a command costs to carry.

**The FP question is still open and gets closer here.** The context switch does not save FP state. It has not mattered because only one thread at a time runs Lua, and Lua's numbers are doubles. A compositor with more than one drawing process makes it matter, and the fix is lazy FP save, which `roadmap.md` schedules at M10 for Doom.

**M2 cannot be closed without a cable**, and its remaining half is the point of the milestone: the HAL takes its real shape once there are two implementations to compare, and `hal.md` is explicit that writing that interface against one target produces the shape of QEMU wearing generic names. Nothing is gained by guessing at it now. When a cable arrives: `hal/pi1/` or `hal/pi5/`, and then the HAL interface takes its real shape.

**Two benchmarks `roadmap.md` asked for at M4 are still not built:** allocating and freeing a table, and the overhead of a syscall from Lua versus the same one from C. The second is more interesting than it was — `sys.ticks()` makes it measurable from inside a process, and it is the number that says what the EL0 boundary costs per crossing.

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
- [ ] 3.3V USB-serial adapter (Pi 1) — cheaper and arrives sooner

---

## Overall progress

| # | Milestone | Status |
|---|---|---|
| 0 | Boot under QEMU | **done** |
| 1 | MMU, exceptions, timer | **done** |
| 2 | Lua in the kernel + second target | **in progress** |
| 3 | Microkernel | **done** |
| 4 | Lua to userspace | **definition of done met** |
| 5 | Namespaces and servers | **in progress** |
| 6 | Graphics and app server | |
| 7 | Attributes, live queries, replicants | |
| 8 | Own filesystem | |
| 9 | Software 3D demo | |
| 10 | Doom | |
| 11 | Drivers (GPIO, USB, network) | |
| 12 | SSH client | |
