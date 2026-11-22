# Display modes

The resolution and the refresh rate chosen from what the monitor says it
can show. Diego, 18 October 2026: "my monitor is a 144hz monitr, will the
screen preferecens panel allow me to change resolution and refresh rate per
the reported by the monitor?"; and 22 November 2026, agreeing the order
after Xavier on Vulkan and USB audio: "also i wanto to build a resolution
selector for the screen as i wan tot be able to switch resolutions and
refresh rates available for the monitor".

Today the firmware chooses, and Kosmos keeps what it chose: on the M700,
3440 x 1440 or 1720 x 1440 depending on the boot, at 60 Hz, with
`video=WxH` on the command line as the only way to ask for anything else
(`boot.md`). This is the design for asking the monitor, offering what it
answers, and setting the mode. It is written before any of it is built:
the feature set and the architecture here, the Displays page in
`docs/displaymodes.html`, what it stands on in
`docs/displaymodes-architecture.png`. `roadmap.md` has the entry
(*Resolution and refresh rate from what the monitor reports*).

**The question Diego asked first deserves its answer first: 144 Hz at
3440 x 1440 is probably beyond the M700.** That mode needs a pixel clock of
about 780 MHz even with reduced blanking. The HD 530 is a Skylake display
engine: its DisplayPort is version 1.2, whose four lanes at HBR2 carry
17.28 Gbit/s of pixels, which is 720 MHz at 24 bits a pixel; and i915 gives
Skylake a highest pixel clock equal to its highest display clock (CDCLK),
675 MHz. Both figures are from memory of i915 and Intel's documents and
D1 below reads the machine's own, but if they hold, then on this GPU and
this monitor: **3440 x 1440 at 120 Hz and at 100 Hz, if the monitor lists
them, and 144 Hz at a smaller size**. The page shows 144 Hz anyway, greyed,
with the reason, because the monitor did offer it and a person should
know why it is not given.

---

## What a person can do

All of it on **Preferences' Displays page**, in the group *Screen*, above
the rendering modes. There is no new application: Displays is where a
person looks for this on every system, and the page already holds the
screen's resolution as a fact (`settings.lua`, *displays*).

- **See the monitor named**: its own name and maker, as its EDID gives
  them; its size in inches, from the millimetres it reports; how it is
  connected (DisplayPort or HDMI); its native resolution; and the mode in
  use now, with **the refresh rate measured** - the frames the pipe
  counted in the last second, as GPU Monitor counts them - beside the one
  asked for, so a mode that does not hold says so.
- **See every mode the monitor reports**: each resolution once, largest
  first, with its shape (21:9, 16:9, 4:3) and *native* on the monitor's
  own; under the chosen resolution, each refresh rate it is offered at, as
  a row of choices - 144, 120, 100, 60, 50 Hz.
- **See which of them this machine can carry**, and why not the others: a
  mode the monitor offers and the link or the display engine cannot carry
  is shown greyed with one sentence - "Needs 782 MHz; this GPU carries up
  to 675". Nothing the monitor said is hidden; nothing is offered that
  will not work.
- **Choose one.** The screen changes at once. The window keeps its place,
  every window is fitted into the new screen, the Deskbar and the dock
  follow its edges, the wallpaper is fitted again.
- **Keep it, or have it come back by itself.** A bar over the foot of the
  page counts fifteen seconds - "Keeping 3440 x 1440 at 120 Hz in 12 s",
  *Keep it*, *Go back* - exactly as the rendering modes' does today. Nothing
  answered, and the mode before returns: a mode that leaves the monitor
  black, or out of range, undoes itself without a key pressed. **The
  countdown is the window server's**, not the page's, so it still runs out
  when Preferences or the window manager has died with the screen black.
- **Keep the choice across boots.** A mode kept is remembered with the
  monitor's identity - its maker, product and serial number - and set
  again when the desktop starts, on that monitor only. Another monitor
  plugged in gets the firmware's choice until it is given one of its own.
  A boot that should not set it - a monitor that changed its mind - has
  the way out the loader already has: a word on the stick's command line,
  `display=firmware`, beside `video=WxH`.
- **The same from the prompt**: `display` names the monitor and lists its
  modes as the page does; `display 3440x1440@120` sets one, with the same
  countdown and `display keep` to keep it. It is what the suites drive.
- **Under QEMU, with virtio-gpu** (`make GPU=virtio x86`): QEMU's own
  monitor, named as QEMU names it in the EDID it makes, with its list of
  sizes - offered and set as on the M700. The refresh rate is QEMU's word
  and nothing else, and the page says so. This is what makes the whole
  path - the list, the change, the countdown, the revert, the windows
  fitted - testable in the gate.
- **On a screen the firmware drew and Kosmos does not drive** - ramfb under
  QEMU, the T14's Iris Xe today, the Mac Pro's FirePro until its display
  controller is driven: the page names the mode the firmware chose and
  says plainly that it cannot change it while running, and how to choose
  another at boot (`video=WxH` in `\boot\kosmos.cmdline`). No list that
  cannot be acted on.

Not now: two monitors at once, which is a second pipe and a desktop that
spans; a monitor plugged in while running, which is the hot-plug interrupt
(D9); colour depth beyond 8 bits a channel, HDR and variable refresh
(Adaptive-Sync), each a feature of its own on top of this; and lower
resolutions *scaled* by the GPU onto a panel's one native mode, which is
what a laptop wants and the M700 does not (D9). The pieces are shaped so
these can come; none of it is built for them.

---

## The architecture

```
  Preferences' Displays page, `display`, This Machine,      (Lua, the UI kit)
  GPU Monitor, neofetch
                 |  display.lua: the one door from Lua - `/Running/wm`'s
                 |  `display` request, as `rendering` is asked today
  ------------------------------------------------------------------------
  wm, the window manager                                     (Lua, a program)
     asks the window server; on a new size, fits every window, the
     strips, the wallpaper, and tells every window ("screen")
                 |  windowproto.h: WS_OP_MODES, WS_OP_MODE, WS_OP_KEEP
  ------------------------------------------------------------------------
  the window server                                          (C, a server)
     the one door to the screen: holds the monitor's list from memory,
     the mode on trial and its fifteen seconds, the buffers of the new size
        |  gpuproto.h: GPU_OP_DISPLAY,      |  SYS_SCREEN_MODE: the HAL's
        |  GPU_OP_MODE_SET                  |  display (virtio-gpu)
  -------------------------------------   ---------------------------------
  intelgpu, the M700's display  (C)       Nebula's HAL, virtio-gpu   (C)
     EDID over AUX (DisplayPort) or          VIRTIO_GPU_F_EDID: GET_EDID;
     GMBUS (HDMI); timings, M/N, the         a mode is a resource of the
     DPLL's link rate, link training,        new size and SET_SCANOUT
     the display clock, watermarks
  -------------------------------------------------------------------------
  Kits: edid.c - the monitor's bytes decoded, a pure C file with a host
  test, linked by every C process that needs it; and intelgpu_decode.c's
  sums for what a link and the display engine can carry
```

### Every piece, and who supplies it

| Piece | Supplied by | New? |
|---|---|---|
| **Reading the monitor's EDID** on the M700 | `intelgpu`: over DisplayPort's AUX channel (I2C-over-AUX to address 50h, and the DPCD for the sink's link capabilities), or over GMBUS (the PCH's I2C to DDC) for HDMI. Read once when the driver starts and kept; asked again only on hot-plug, later | new, in the driver |
| **Reading it under QEMU** | `hal/virtio/gpu.c`: the EDID feature it declines today ("No virgl, no EDID, no blobs") taken, and `VIRTIO_GPU_CMD_GET_EDID` asked once at start; the bytes kept by the kernel for the screen's holder | new, in the HAL |
| **Decoding it** | **`user/kits/display/edid.c`**, a pure decoder in C over a byte array: the base block (maker's PNP id, product, serial, name, size in millimetres, the established, standard and detailed timings, the range limits), CTA-861's extension (its detailed timings and video codes), and DisplayID's (where a high-refresh ultrawide often puts its fastest modes). No allocation, no system call, `tools/test_edid.c` holding it on the Mac as `test_intelgpu_decode` holds its decoder | **new kit** |
| **What this machine can carry** | `intelgpu_decode.c`: a mode's pixel clock against the link (lanes times rate times 8/10, over bits a pixel) and against the display clock; the M/N values for the transcoder. Pure sums, host-tested | new, beside the decoder that is there |
| **Setting the mode** on the M700 | `intelgpu`: plane, pipe, transcoder and DDI off in i915's order; the transcoder's timings (`TRANS_HTOTAL` and the five after it), `PIPESRC`, `PIPE_DATA_M1`/`N1` and `PIPE_LINK_M1`/`N1`; the DPLL's link rate (`DPLL_CTRL1`, `DPLL_CTRL2`, `DPLL_STATUS`); the link trained (`DP_TP_CTL`, `DDI_BUF_CTL`, the DPCD over AUX); the display clock raised when the mode needs it (`CDCLK_CTL`, asked of the PCU first); the display buffer's shares and watermarks recomputed; then on in the reverse order, the plane last | new, in the driver |
| **Setting it under QEMU** | the HAL: a resource of the new size backed by guest pages, `SET_SCANOUT`, the old resource unreferenced - what 4h a did at boot, done again | new, in the HAL (`roadmap.md` 4h c) |
| **The door, the trial and the revert** | **the window server**: it holds the screen, so it is the one process whose buffers must change with it. It asks the driver for the list at start and answers from memory; a mode asked for is set, the buffers of the new size made, wm told, and the mode *on trial* until WS_OP_KEEP - fifteen seconds on its own clock, then the mode before, set the same way | new verbs in an existing server |
| **The new size, laid out** | **wm**: W and H become the new screen's; every window fitted inside it as `scale.rescale` fits them to a new scale today; full-screen and maximised windows given the new screen; the strips, the dock and the wallpaper again; and every window told `{ type = "screen", w, h }` so an application that asked once can ask again | new, in wm, after `scale.rescale`'s pattern |
| **The kernel's screen record** | `kernel/screen.c`, written once at boot today. `SYS_SCREEN_MODE` from the screen's holder replaces it: on the HAL's display, the HAL's new framebuffer; on the M700, the driver's buffer of the new size handed as a region - so the console, a panic and `/Devices/screen` follow the mode rather than describing the boot's. One call, its caller the process holding the screen | new syscall (Diego's decision 2) |
| **Kept across boots** | a preferences file, `/Home/Preferences/display`, beside `appearance` (`prefs.lua`): the monitor's identity and the mode kept. Read by wm as it starts, which asks the window server for that mode if that monitor is there - the window server, in C, reads no files | new file, `prefs.lua`'s |
| **The page** | Preferences: a new `kind = "modes"` item in the *Screen* group in place of the fact that is there; **the keep-or-go-back bar the rendering modes already have** (`preferences.lua`, *Keep this mode, or go back*), taken out into one component both use rather than drawn a second time | Lua, in the existing app |
| **The one door from Lua** | **`user/lib/display.lua`**: `display.monitor()`, `display.modes()`, `display.set(w, h, hz)`, `display.keep()`. Preferences, `display`, This Machine's monitor card, GPU Monitor's refresh and neofetch's Display row all ask through it | **new library** |
| **The program** | `user/bin/programs/display.lua` | new |

**Does another application want this?** Asked of each piece, as the
premise says:

- *The EDID decoder*: yes, and not only for this page. This Machine wants
  the monitor as a card (`machine.md`); neofetch wants a Display row;
  the Mac Pro's `amdgpu` will want it the day its display controllers are
  set; the Pi 5's HDMI does the same thing; and Sandboxes' Linux guest is
  handed a screen whose sizes could be the real monitor's. So it is a kit
  from the start, one C file, one header, linked into whoever is a C
  process needing it - the driver and the window server now - and its
  answer reaches Lua only already decoded, through `display.lua`.
- *The mode list and the mode set*: yes - games. Doom, Quake and Flight
  run full-screen, and a game that wants 1920 x 1080 at 144 Hz on a large
  monitor asks the same door, through wm, with the same countdown left out
  because the game is there to undo it. That is a later use and nothing
  here is built for it; it is why the verbs are wm's and the window
  server's rather than Preferences'.
- *The countdown*: yes, it already has two users - the rendering modes
  and this - so it is one component, not a second copy.
- *What a link can carry*: only the GPU drivers. It stays in
  `intelgpu_decode.c`; `amdgpu` will have its own sums for its own links.

**Does something already supply this?** The keep-or-go-back bar (above);
the measured refresh (`GPU_OP_STATS`'s `PIPE_FRMCOUNT`, GPU Monitor's);
the windows fitted to a new geometry (`scale.rescale`); the buffers of the
screen's size (`GPU_OP_BUFFER_NEW`, which flips already refuse when they
are not the screen's size - `GPU_ERR_NO_BUFFER`); the `video=WxH` escape
at boot (`loader.c`). Each is used, not copied.

### What crosses where

| What | How |
|---|---|
| The monitor and its modes | **a message**, one-shot: a declared shape, `struct display_mode` - width, height, refresh in millihertz, pixel clock in kilohertz, flags (native, interlaced, carried, why not) - at most 48 of them, under 2 KB. Asked when somebody looks, never on a clock |
| The EDID's own bytes | **a message** where they travel at all - 128 bytes a block, rarely more than four; asked once, and only from the driver or the kernel to the window server. Lua never sees them |
| A mode asked for, kept, gone back to | **messages**: Preferences to wm, wm to the window server, the window server to the driver or the kernel |
| The screen's pixels at the new size | **regions**, as now: the back buffer wm hands the window server, the driver's buffers the window server flips between. They are made again, at the new size, and the old ones let go |
| "The screen is now W x H" | **a message** to each window, posted as the theme's is - never a call that waits |

Nothing here recurs because the hardware says so; every message is
somebody asking. The only stream - the frames - stays in the regions it is
in today.

### What is C and what is Lua

**C**: the decoder, the driver's every register, the HAL's virtio-gpu, the
kernel's record, the window server's trial and its timer. Each runs on
behalf of another process, and a mode change is the one moment where a
mistake is a black screen nobody can see past - which is exactly why the
revert lives in C, in the process that outlives the window manager.

**Lua**: the page, the program, `display.lua`, and wm fitting windows into
the new size, which is layout and tables - the kind of code `CLAUDE.md`
keeps in Lua - and happens once a change, not once a frame.

### The busiest path: a mode chosen, end to end

There is no hot path - this happens when a person clicks, a few times a
year - so the question is not speed but how long the screen is black, and
whether anything else stops while it is.

| | Step | Where the time goes, estimated |
|---|---|---|
| 1 | Preferences paints the choice at once (*instant feedback*) and posts `display` to wm | under a millisecond |
| 2 | wm asks the window server, WS_OP_MODE; the window server asks the driver, GPU_OP_MODE_SET, **and holds wm's reply** rather than waiting in its loop | a message each |
| 3 | The driver: plane, pipe, transcoder, DDI off; the DPLL; timings and M/N; the link trained; the pipe on; a vertical blank waited for, polled a tick at a time | DPLL lock and link training a few milliseconds each, AUX transactions a few hundred microseconds; under 50 ms in all |
| 4 | **The monitor finds the new signal** | **one to three seconds**, the monitor's own, and the whole of what a person sees. Nothing in Kosmos can shorten it |
| 5 | The window server makes its buffers of the new size (two, contiguous, 19.8 MB each at 3440 x 1440, their GTT entries written) and tells the kernel the new screen | tens of milliseconds |
| 6 | wm: W and H, a back buffer of the new size handed over, every window fitted, the strips and wallpaper; one whole frame composed | a frame and a half; the wallpaper's scaling is the largest part |
| 7 | Every window told `screen`; each redraws in its own time | each window's own |
| 8 | The countdown: the window server's fifteen seconds, the page drawing what is left | - |

**What does not stop**: sound - Groove plays through a mode change, being
in its own band and not on the screen's path; input - keys and the pointer
reach wm throughout, and the pointer is drawn by the cursor plane, which is
re-armed with the plane; the network and the disks. **What does**: frames,
for step 3 to step 6, which is the black the monitor shows anyway.

### How it is tested

**On the Mac** (`host`): `test_edid` - QEMU's own EDID as QEMU makes it,
then the M700's monitor's real bytes once D3 has read them out, kept in
`tools/` as a fixture, and synthetic blocks for each path: the four timing
kinds of the base block, a CTA extension, a DisplayID extension with a
Type I timing, a checksum wrong, a block cut short, an extension count
larger than the bytes - each decoded or refused by name. `test_intelgpu_decode`
grows: what a link of one, two and four lanes at each rate carries; the
M/N values for 3440 x 1440 at 60, 100 and 120 Hz checked against i915's
`intel_link_compute_m_n` worked by hand; and 144 Hz refused with its
reason.

**Under QEMU, in the gate** (`x86-display`, a suite of its own, with
`GPU=virtio`): `display` names QEMU's monitor and lists its sizes; `display
1280x800` and the screendump is 1280 x 800, every window inside it, the
Deskbar along its new top; nothing said for fifteen seconds and the
screendump is the old size again - the revert, the control; `display
1280x800` then `display keep`, a restart, and the desktop comes up at 1280 x
800 - the choice kept. Under ramfb, the same program says the firmware
chose and nothing can be set, and the page has no list. The suite waits
for the thing, the screen's size in the log, never a number of seconds -
except the fifteen of the countdown, which is the thing.

**Only on the M700**: the AUX or GMBUS read, every register written, the
link trained, whether the monitor shows it, and whether a refresh holds -
**measured**, `PIPE_FRMCOUNT` over ten seconds against the mode asked for,
and the pipe's underrun bit clear. Diego's eyes for the rest: no
flicker, no tearing, the pointer smooth.

### The risks

- **A black screen.** The countdown is the answer, and it is only an
  answer if it cannot be stopped by what broke: so it is the window
  server's, in C, and the mode it returns to is the one the firmware had
  already proved. The driver also refuses, before writing anything, a mode
  whose link or clock it has worked out it cannot carry.
- **The display clock.** If the firmware left CDCLK at what 60 Hz needed,
  every faster mode at 3440 x 1440 needs it raised, and on Skylake that is
  a handshake with the PCU (`SKL_PCODE_CDCLK_CONTROL` in i915), done wrong
  a hang of the display engine. D1 reads what it is; D7 raises it, alone,
  after every lesser mode works.
- **Underruns.** The display buffer's shares and watermarks were the
  firmware's for 60 Hz (`share_line` says them today); a faster mode with
  the same ones underruns, which shows as flicker or a line of garbage. They
  are recomputed after `skl_watermark.c` (in `build/downloads/i915/`), and
  the underrun bit is read after every mode set, as the cursor test reads it.
- **Link training.** i915 retrains the DisplayPort link on every full mode
  set, so even a refresh change at the same link rate is very likely a
  training, which is clock recovery and channel equalisation over AUX with
  the sink's say at each step. A monitor that will not train at a rate
  stays at the one it trained at, and the modes that need more are greyed.
- **Every copy of the screen's size.** wm keeps W and H in locals and hands
  them out (`W = W, H = H` in a table it builds, among others); an
  application asks once at load (Preferences' own `SCREEN_W`). Each is
  found and made to follow the `screen` event; a suite checks the Deskbar,
  a maximised window and a full-screen one after a change.
- **The kernel's record.** It is written once and read without a lock
  today, which `screen.c` says is what makes a file-scope variable
  acceptable. Replacing it at run time takes that away: it gets a lock, and
  `SYS_SCREEN_MODE` is held to the screen's holder alone.
- **Register offsets.** Every one is to come from i915's sources, fetched
  into `build/downloads/i915/` and cited in `intelgpu_decode.h`, and from
  Intel's Skylake PRM (Volume 2c, the display registers) where i915 is
  unclear. The timing, M/N, DDI, DP_TP, DPLL and CDCLK registers are in
  `intel_display_regs.h` there now; **the AUX channel's and GMBUS's are
  not** (i915 keeps them in `intel_dp_aux_regs.h` and `intel_gmbus_regs.h`),
  and are fetched in D3 before a line is written. No offset is written
  from memory.

---

## Steps

Each small, each with its test, the desktop working after every one.

- **D1 - the M700's display read, nothing written.** `gpu look` says more
  of what the firmware lit: which DDI and whether it drives DisplayPort or
  HDMI (`TRANS_DDI_FUNC_CTL`'s mode select), the link's rate and lanes
  (`DPLL_CTRL1`, `DDI_BUF_CTL`), the display clock (`CDCLK_CTL`), the
  transcoder's six timing registers, and from them and M/N the pixel clock
  and the refresh - said beside the refresh `PIPE_FRMCOUNT` measures. Host
  test: the decoders, held to i915's fields. *This is also what answers
  how the monitor is connected; the documents do not record it.*
- **D2 - the EDID kit.** `user/kits/display/edid.c` and `tools/test_edid.c`,
  on the Mac alone: the base block, CTA-861, DisplayID; QEMU's EDID as the
  first fixture.
- **D3 - the monitor's EDID read on the M700.** Over AUX or GMBUS, by
  D1's answer; once at start; `gpu edid` prints it whole in hex and
  decoded, and the bytes become `test_edid`'s second fixture. Read only.
- **D4 - the list, everywhere.** `GPU_OP_DISPLAY` in the driver; the HAL's
  EDID under virtio-gpu; WS_OP_MODES; wm's `display`; `display.lua`; the
  `display` program; the Displays page's list, with what cannot be carried
  greyed and why. Nothing set yet. Gate: `x86-display`'s list under
  QEMU.
- **D5 - a mode set, under QEMU.** The HAL's resource of a new size,
  `SYS_SCREEN_MODE`, the window server's buffers and trial, wm's new
  geometry and the `screen` event, the page's countdown. Gate: the change,
  the revert, the keep.
- **D6 - a mode set on the M700, at the link the firmware trained.**
  Modes whose pixel clock fits the link rate and lanes and the display
  clock as the firmware left them: off, timings, M/N, the link retrained
  at its own rate, on, watermarks recomputed. Measured: the refresh by
  `PIPE_FRMCOUNT`, the underrun bit, Diego at the monitor.
- **D7 - faster: the link and the clock raised.** A higher link rate or
  more lanes where the sink's DPCD allows it, and CDCLK raised through
  the PCU. This is the step that gives 100 and 120 Hz at 3440 x 1440, if
  D1 shows the firmware left the clock low. Alone, because it is the
  riskiest write in this document.
- **D8 - kept across boots.** `/Home/Preferences/display`, the monitor's
  identity, the mode set as the desktop starts, and `display=firmware` on
  the command line as the way out. Gate: kept across a restart under QEMU.
- **D9 - later, each its own decision when it is wanted**: the hot-plug
  interrupt, a monitor changed while running; HDMI's WRPLL dividers, if
  the M700 is ever on HDMI; the pipe's scaler for a panel's lower
  resolutions (the T14, when its Gen12 display is driven); the Mac Pro's
  DCE 6 display controllers; full-screen games asking for a mode.

---

## Decisions - agreed

**Agreed on 22 November 2026, every recommendation taken** (Diego, 22 November: "agreed with your recommendations, go ahead with usb audio"). Each decision below stands as recommended.

1. **Where the countdown lives.** In the window server, in C, so it runs
   out even when Preferences or wm died with the screen black; or in
   Preferences, as the rendering modes' does today. *Recommended: the
   window server*, with the page only drawing what is left. The rendering
   modes could move there too, later, for the same reason.
2. **The kernel's screen record after a change.** Replaced by
   `SYS_SCREEN_MODE` from the screen's holder, so the console, a panic and
   `/Devices/screen` describe the screen that is on; or left at the boot's
   mode, so the kernel never learns of a change and the console writes
   into memory nobody scans. *Recommended: replaced*, one syscall held to
   one caller, at the cost of a lock where there was none.
3. **Modes this machine cannot carry.** Shown greyed with the reason, or
   hidden. *Recommended: shown*, since the monitor said 144 Hz and a
   person who bought it for that should see why it is not given, rather
   than wonder whether Kosmos read the monitor at all.
4. **How the choice survives a boot.** Set as the desktop starts - wm
   reads the file and asks the window server - on the same monitor only, with `display=firmware` as
   the way out; or handed to the loader to choose at boot. *Recommended:
   as the desktop starts* - the firmware's GOP offers no refresh rate and
   probably no 144, 120 or 100 Hz mode at all, so the loader cannot choose
   what is asked; the cost is a second mode change a second or two into
   the desktop.
5. **Firmware-only screens.** The page names the firmware's mode and the
   `video=WxH` way at boot, with no list; or the GOP's modes listed and the
   choice written to the stick's `\boot\kosmos.cmdline` for the next boot.
   *Recommended: no list for now* - the second needs Kosmos to write the
   FAT partition, which nothing does yet, for machines (the T14, the Mac
   Pro) whose own drivers are the better answer.
6. **Lower resolutions on the M700.** Real modes from the monitor's EDID
   only, each a true mode change; or also any size scaled by the pipe's
   scaler onto the native mode, with no retiming. *Recommended: the
   monitor's own modes first* (D6, D7); the scaler is a laptop's answer
   and waits for one (D9).
7. **The ninth step's order.** None of D9 is agreed by this document;
   each is asked for when it is wanted. *Recommended: hot-plug first* of
   them, since a monitor's cable is the thing most likely to move.
