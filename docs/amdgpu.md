# The Mac Pro's GPUs

The Mac Pro 2013 has two AMD FirePro cards, and until now Kosmos used them
only as the firmware left them: one of them scans out the screen the
firmware lit, and nothing else happens on either. This is the driver that
finds them, says what they are, reads the display they run, and gives
their DMA engines their first work - the same road `m700-2d.md` took for
the M700's Intel GPU, at the start of it.

Diego, 20 November 2026: "work on getting our amd gpu for mac pro driven by
a driver as we did with the intel gpu".

## In short

- **The cards** are Southern Islands, GCN 1.0: a D300 is Pitcairn, a D500
  and a D700 are Tahiti. Which one this Mac Pro has is not known until it
  boots, and the driver's first line says it.
- **Step 1, find and identify** - built: both cards, each named from its
  PCI ids, its memory, its compute units, its engines' state, its VM.
- **Step 2, the display** - built: all six display controllers read back,
  the one that scans compared with the screen the kernel has, and a page
  flip to a picture of the driver's own and back, on demand (`gpu cursor 5`).
- **Step 3a, the DMA engines** - built: each engine's ring set up in VRAM,
  a number written, 64 KB filled and copied, a fence - at boot, where the
  memory controller says an address reaches VRAM untranslated.
- **None of it is verified on the machine yet.** QEMU has no AMD GPU; the
  decode is held on the Mac by `tools/test_amdgpu_decode.c`, 56 checks with
  controls, and the driver by the Mac Pro's boot tonight.

## The hardware

| Card | PCI id | Subsystem (Apple) | Chip | Expected (Apple's specification - not read yet) |
|---|---|---|---|---|
| FirePro D300 | 1002:6810 | 106b:012a, 106b:012b | Pitcairn | 20 CUs, 2 GB, 256-bit |
| FirePro D500 | 1002:679e | 106b:0125, 106b:0126 | Tahiti LE | 24 CUs, 3 GB, 384-bit |
| FirePro D700 | 1002:6798 | 106b:0127, 106b:0128 | Tahiti XT | 32 CUs, 6 GB, 384-bit |

The device ids are the PCI ID Repository's (`pci.ids`, fetched 20 November
2026: the D-series are Apple subsystems under 6798 "Tahiti XT", 679e "Tahiti
LE" and 6810 "Curacao XT / Trinidad XT"); that 6810 is Pitcairn and the
other two Tahiti is Linux's `include/drm/drm_pciids.h` (lines 259-325 for
the two families), with amdgpu's `amdgpu_drv.c` agreeing for every id but
0x684c. Two subsystem ids for each card - one per card in the machine; the
right column is what the driver's lines are to be held to, not a fact yet.

Each card has three windows (`radeon_device.c` lines 1393-1398,
`amdgpu_device.c` 4156-4160): **BAR 0** the VRAM aperture, **BAR 2** the
registers - BAR 5 only from Bonaire, which is Sea Islands - and BAR 4 I/O
ports, unused. Both memory BARs are 64-bit.

**The two cards are not the same to the firmware.** One has the display
connectors and is the one whose GOP lights the screen; the other is the
"compute" card. Whether Apple's firmware posts the second at all - starts
its memory controller - is one of tonight's questions: if not, its
CONFIG_MEMSIZE reads nought and MC_SEQ_SUP_CNTL's run bit is clear, and the
driver says "the firmware did not post this card" and leaves it.

## What the kernel needs

QEMU has no model of the card and the kernel has no device kind for it, so
the driver can be folded in before the kernel can find it: with no kind,
`kosmos_dev_find` answers no, `amdgpu_server` returns false having said
nothing, and the role goes on to the Intel driver as before.

The kind is two kinds at one index: `DEV_AMD_GPU`, BAR 2, and `DEV_AMD_VRAM`,
BAR 0 - found by vendor 1002h and class 03h, so the second card's
"other display" class 0380h is found too and the HD Audio function beside
each (class 04h) is not. **Neither sized nor enabled**, the Intel GPU's
reason: sizing a BAR turns memory decoding off, and the kernel console
writes into one card's BAR 0. The register BAR is reported as 256 KB - the
size every Southern Islands card's `lspci` gives, not an AMD document's -
and the driver maps only the first 76 KB, which holds every register it
uses; the aperture's size is the card's own CONFIG_APER_SIZE.
`hal_device_config` serves the first kind, for the subsystem and the
command word. Written in a lane, the kinds were folded into `hal/hal.h`,
`hal/pc/devices.c`, `kernel/syscall.h` and `.c` and `user/init/main.c` in
0.12.29 (`testing.md` 18.949).

**One door.** `/Devices/gpu` stays one endpoint served by one process: the
GPU role runs `amdgpu_server` first, which keeps the door where the board
has an AMD GPU and names itself `amdgpu`, and `intelgpu_server` otherwise.
`init.lua` does not change. What a machine with an AMD GPU gives up is the
Intel driver's stand-in for programs' contexts, which runs a batch's MI_
commands on the processor where there is no Intel GPU (`gpu3d.md` G6e);
on the Mac Pro those verbs answer "no device" until this driver has
contexts of its own.

## What the driver does, step by step

`user/drivers/display/amdgpu.c`, its decoding in `amdgpu_decode.c`.

### 1. Found and identified

For each card the board names, in this order, every line beginning
`amdgpu: card N:`:

1. **Where it is**: bus, device, function, `1002:dddd`, revision, subsystem,
   class, the card's name and the chip's - then BAR 2 and BAR 0's
   addresses and the command word, saying whether memory decoding and bus
   mastering are on.
2. **Whether these are its registers**: CC_DRM_ID_STRAPS's device id
   (19:4) held to PCI's. All ones means the BAR does not answer, and the
   driver stops there for that card.
3. **Its memory**: CONFIG_MEMSIZE in MB (with `si_mc_init`'s rule for
   garbage in 31:16), the width from MC_ARB_RAMCFG and MC_SHARED_CHMAP,
   GDDR5 and the memory's vendor from MC_SEQ_MISC0, and whether the memory
   controller runs (MC_SEQ_SUP_CNTL's RUN bit - `si_mc_load_microcode`'s
   own test for whether the MC firmware must be loaded).
4. **Where VRAM is**: MC_VM_FB_LOCATION (16 MB units, the top inclusive),
   HDP_NONSURFACE_BASE - the MC address BAR 0 begins at - and the aperture.
5. **The VM**: MC_VM_MX_L1_TLB_CNTL and its system access mode, VM_L2_CNTL,
   VM_CONTEXT0_CNTL, the system aperture.
6. **Its shape**: active compute units, each of the shader arrays selected
   through GRBM_GFX_INDEX and read as `si_get_cu_active_bitmap` does, then
   broadcast again as Linux leaves it - the one register written at boot -
   and the die's temperature.
7. **Its engines**: GRBM and SRBM status, SRBM_SOFT_RESET, CP_ME_CNTL's
   halts, RLC_CNTL, and each DMA engine's ring registers, status and power
   gating.

### 2. The display

**2a. Read back.** Every display controller the family has (six on Tahiti
and Pitcairn): for each running one its viewport, whether graphics is on,
its surface's size, depth and format, linear or tiled, its pitch in pixels,
the primary surface's MC address, GRPH_UPDATE, and the frame count. The
first running one with graphics on is *the* controller, and its surface is
then held to three things: inside VRAM, inside BAR 0 (and where: `BAR 0 +
offset, physical address`), and the size and pitch `sysinfo` says the
kernel's screen has. Nothing is written.

**2b. Flipped, on demand** - `gpu cursor 5`, the verb that on the M700
shows a test picture on the cursor plane. Here it is a page flip: a picture
of the driver's own - eight colour bars inside an orange border, so a
picture cut off or shifted shows it - drawn through BAR 0 past the work
area, the controller's primary surface written as `evergreen_page_flip`
does (`evergreen.c` line 1416: the high word, then the low, which arms the
update for the vertical blank; the pitch unchanged, since it is the
screen's), GRPH_UPDATE's pending bit watched until the display takes it,
held for the seconds asked, and the firmware's surface put back the same
way. The line says how many frames each flip took and that the surface
reads as the firmware's again.

It is not done at boot because it is visible, and because a boot that
changes the screen should be one somebody asked for.

### 3a. The DMA engines' first work

Southern Islands has two DMA engines, at 0xd000 and 0xd800, which copy and
fill linear memory and need **no firmware** - `si_init_microcode` loads
pfp, me, ce, rlc, mc and smc and nothing for DMA (DMA firmware arrives with
Sea Islands' SDMA). That is why they are first: the graphics ring needs
the CP's three microcodes and the RLC's before it will take a command.

For each engine, on each card, at boot and again on `gpu engine` (DMA 0)
and `gpu engine render` (DMA 1):

- **Only if it is safe**: the card posted, a work area in VRAM, the engine
  not in soft reset, its ring not already on - and **an engine's address
  known to reach VRAM**. The engine's accesses go through the memory
  controller's VM as VMID 0. Linux's setting is SYSTEM_ACCESS_MODE_NOT_IN_SYS
  (`si_pcie_gart_enable`, `si.c` 4277), under which an address inside the
  system aperture is not translated, and `si_mc_program` (4145) sets that
  aperture to VRAM. So the test runs when the L1 TLB is off, the mode is
  PA_ONLY, context 0 is off (an inference: no page table for it to use), or
  the work area is inside the system aperture. In any other state the
  address could land anywhere, system RAM too, and the line says
  "TRANSLATED: no DMA test".
- **The work area** is 320 KB of VRAM past the firmware's screen, rounded
  up a megabyte and a megabyte more (from 16 MB on a card that shows
  nothing): the two 4 KB rings, a page of words, and 256 KB for the test.
- **The ring set up as `si_dma_start`** (amdgpu `si_dma.c` line 126), in
  its order: the semaphore timers nought, RB_CNTL's size, the pointers
  nought, the write-back address (write-back left off: the read pointer is
  read from its register), RB_BASE, IB_CNTL with CMD_VMID_FORCE, DMA_CNTL
  without the context-empty interrupt, and RB_ENABLE.
- **Four commands**: WRITE of 0xdeadbeef to a word (`si_dma_ring_test_ring`,
  192), CONSTANT_FILL of 64 KB (`si_dma_emit_fill_buffer`, 796), COPY of
  those 64 KB (`si_dma_emit_copy_buffer`, 772), and a FENCE writing 1
  (`si_dma_ring_emit_fence`, 91, without its trap). The host data path is
  flushed after the processor's writes (`si_flush_hdp`, `si.c` 1483) and
  invalidated before each read of what the engine wrote (`si_invalidate_hdp`).
- **Waited for** a scheduler tick at a time, 200 ms at most; then every
  word of the fill and the copy counted, and the ring switched off again
  (`si_dma_stop`), so each test starts from the same place.

The line says `DMA n works:` with the word, the fence, 16384 of 16384
words filled and copied and the microseconds, or `FAILED:` with the same
and the ring's registers.

### What is answered at `/Devices/gpu`

| Verb | On this driver |
|---|---|
| GPU_OP_LOOK (`gpu`) | every card looked at again, in the log |
| GPU_OP_STATS (GPU Monitor) | the scanning card's device id, size, frame count, flips; the firmware's screen as buffer 0 |
| GPU_OP_ENGINE_TEST (`gpu engine [render]`) | step 3a on DMA 0, or 1, on every card |
| GPU_OP_CURSOR_TEST (`gpu cursor N`) | step 2b, the flip test |
| GPU_OP_FLIPPED | flips made, the surface being scanned |
| everything else | GPU_ERR_NO_DEVICE - the window manager's cue to draw its own arrow and compose into the firmware's screen, as on QEMU |

## Every register, and where it is from

Linux v6.12, fetched into `build/downloads/linux-amd/` (2 MB, from
`raw.githubusercontent.com/torvalds/linux/v6.12`). Radeon's headers give
bytes, amdgpu's `asic_reg/` dwords; where both exist the test holds them
equal (`mm` name times four).

| Register | Offset | Source |
|---|---|---|
| SRBM_STATUS, SRBM_SOFT_RESET, SRBM_STATUS2 | 0x0e50, 0x0e60, 0x0ec4 | radeon `sid.h` 332, 342, 365 |
| CG_MULT_THERMAL_STATUS (CTF_TEMP 17:9) | 0x0714 | `sid.h` 201; `si_get_temp`, `si.c` 1335 |
| VM_L2_CNTL, VM_CONTEXT0_CNTL | 0x1400, 0x1410 | `sid.h` 369, 392 |
| MC_SHARED_CHMAP (NOOFCHAN 15:12) | 0x2004 | `sid.h` 461; `si_mc_init`, `si.c` 4186 |
| MC_VM_FB_LOCATION | 0x2024 | `sid.h` 466; written by `si_mc_program`, `si.c` 4151 |
| MC_VM_SYSTEM_APERTURE_LOW/HIGH | 0x2034, 0x2038 | `sid.h` 470 |
| MC_VM_MX_L1_TLB_CNTL | 0x2064 | `sid.h` 474 |
| MC_ARB_RAMCFG (CHANSIZE 8, override 11) | 0x2760 | `sid.h` 497 |
| MC_SEQ_SUP_CNTL (RUN 0) | 0x28c8 | `sid.h` 533; `si.c` 1603 |
| MC_SEQ_MISC0 | 0x2a00 | `sid.h` 551 |
| HDP_NONSURFACE_BASE | 0x2c04 | `sid.h` 639; `si_mc_program` 4155 |
| HDP_DEBUG0 | 0x2f30 | amdgpu `oss_1_0_d.h` 134 (0x0bcc) |
| CONFIG_MEMSIZE (MB on SI) | 0x5428 | `sid.h` 678 |
| CONFIG_APER_SIZE | 0x5430 | amdgpu `bif_3_0_d.h` 623 (0x150c) - **its unit is not stated anywhere read; taken as bytes and believed only as a power of two from 16 MB to 16 GB** |
| HDP_MEM_COHERENCY_FLUSH_CNTL | 0x5480 | `sid.h` 687; `bif_3_0_d.h` 628 |
| CC_DRM_ID_STRAPS (device id 19:4) | 0x5564 | `oss_1_0_d.h` 125, `oss_1_0_sh_mask.h` 28 |
| GRBM_STATUS, GRBM_GFX_INDEX | 0x8010, 0x802c | `sid.h` 942, 998; `si_select_se_sh`, `si.c` 2927 |
| CP_ME_CNTL, RLC_CNTL | 0x86d8, 0xc300 | `sid.h` 1027, 1300 |
| CC_ / GC_USER_SHADER_ARRAY_CONFIG | 0x89bc, 0x89c0 | `sid.h` 1077, 1080; `si.c` 5279 |
| DMA_RB_CNTL, _BASE, _RPTR, _WPTR | 0xd000-0xd00c | `sid.h` 1815-1824 |
| DMA_RB_RPTR_ADDR_HI/LO, DMA_IB_CNTL, DMA_CNTL, DMA_STATUS_REG | 0xd01c-0xd034 | `sid.h` 1826-1840; CMD_VMID_FORCE amdgpu `sid.h` 1895 |
| DMA_SEM_INCOMPLETE/WAIT_FAIL_TIMER_CNTL | 0xd044, 0xd048 | `r600d.h` 640 |
| DMA_PG | 0xd0d4 | `sid.h` 1848 |
| The second DMA engine | + 0x800 | `sid.h` 1813 |
| DMA_PACKET and its commands | - | `sid.h` 1853-1880 |
| VGA_RENDER_CONTROL, D1VGA/D2VGA_CONTROL | 0x0300, 0x0330, 0x0338 | `dce_6_0_d.h` 4393, 1041, 1042 |
| GRPH_ENABLE ... GRPH_FLIP_CONTROL | 0x6800-0x6848 | `evergreen_reg.h` 60-163 |
| VIEWPORT_SIZE | 0x6d74 | `evergreen_reg.h` 221; `dce_6_0_d.h` 4402 |
| CRTC_CONTROL, CRTC_STATUS, CRTC_STATUS_FRAME_COUNT | 0x6e70, 0x6e8c, 0x6e98 | `evergreen_reg.h` 233, 238; `sid.h` 906 |
| The controllers' offsets | 0, 0xc00, 0x9800, 0xa400, 0xb000, 0xbc00 | `evergreen_reg.h` 224-229; amdgpu `sid.h` 1994-1999 |
| Families' shapes | - | `si_gpu_init`, `si.c` 3078-3155 |
| Controllers per family | - | `radeon_asic.c` 2471-2476 |

**One thing Linux has that cannot happen**: `si_get_temp` tests `temp &
0x200` after a nine-bit mask, so the "past the scale" reading of 255 never
occurs. The decode keeps the test as Linux has it; the host test notes it.

## What tonight's boot will tell us

Every line begins `amdgpu:`. In order, and what each settles:

1. `card 0: 03:00.0 1002:6798 rev 00, subsystem 106b:0127 ... - FirePro
   D700, Tahiti` - **which cards this Mac Pro has**, and that the board
   found both (a `card 1:` line).
2. `registers (BAR 2) at ..., VRAM aperture (BAR 0) at ...; command ...` -
   whether the firmware turned memory decoding on for the second card.
3. `CC_DRM_ID_STRAPS ... matches PCI's` - that BAR 2 is the registers.
4. `... MB of VRAM ..., 384-bit, GDDR5 ...; memory controller running` -
   **whether the second card was posted**.
5. `VRAM at MC ..., BAR 0 begins at MC ..., aperture 256 MB (CONFIG_APER_SIZE
   ...)` - the unit of CONFIG_APER_SIZE, and the map from BAR to MC.
6. `VM: L1 TLB ... context 0 ..., system aperture ...` - **how the firmware
   left the VM**, which decides whether 3a runs.
7. `32 of 32 compute units active (2048 stream processors), 45 C`.
8. `DMA 0: RB_CNTL ...` - whether anything had a ring on.
9. `controller 0 on, 2560x1440, graphics 2560x1440 at 32 bpp ... linear,
   pitch ... surface MC ...` and `controller 0's surface is BAR 0 + ...;
   the firmware's screen, size and pitch the same` - **step 2a's proof**.
10. `work area at BAR 0 + ..., MC ... - an engine's address there is VRAM`.
11. `DMA 0 works: word deadbeef, fence 1, filled 16384 and copied 16384 of
    16384 words, ... us` - **step 3a**, for each engine on each card.

Then, typed: `gpu cursor 5` - colour bars in an orange border for five
seconds and the desktop back - and the line `flip to MC ... taken in 1
frames, back in 1; surface read ... then ... - the firmware's again`.

**If anything surprises it**, the line says so in capitals (MEMORY OFF, NOT
RUNNING, OUTSIDE VRAM, TRANSLATED, FAILED) and the driver goes no further
on that card; the screen is never written at boot.

## Next steps

Named as Intel's were, `m700-2d.md` and `gpu3d.md`:

- **A1. Interrupts.** The card's MSI, the IH ring (`si_irq_init`), so a
  fence and a vertical blank are heard rather than polled. The kind needs
  `pci_take_interrupt`, as the Intel GPU's has.
- **A2. A GART.** VM context 0 with a page table of the driver's own
  (`si_pcie_gart_enable`): system pages the engines can reach, so a
  window's picture in RAM can be copied to VRAM without passing through the
  processor. Until then every buffer the engines touch is in VRAM, and
  BAR 0 is the only door to it - 256 MB of it.
- **A3. The compositor on the DMA engine.** BUFFER_NEW in VRAM as a region
  a process can map - which needs the kernel to hand a range of BAR 0 out
  as a region, write-combined: a new kind of memory object - FLIP to it,
  and BLIT as one COPY per row of a rectangle (Southern Islands' DMA has
  no rectangle copy; amdgpu uses only linear copies). Then the window
  manager composes in VRAM and flips, as it does on the M700.
- **A4. The graphics ring.** The CP's microcode - `tahiti_pfp.bin`,
  `_me.bin`, `_ce.bin`, `_rlc.bin` (and `_mc.bin` and `_smc.bin` for a card
  the firmware did not post), or `pitcairn_*` - from linux-firmware, fetched
  into `build/downloads/radeon-firmware/` (12 files, 266 KB; CE, ME and PFP
  are byte for byte the same for both chips). Loaded as `si_cp_load_
  microcode` does, through CP_PFP_UCODE_ADDR/DATA and their kin, then the
  RLC, then a ring the way `si_cp_resume` sets it. The first PM4 packet a
  WRITE_DATA, as 3a's first DMA command is a WRITE.
- **A5. Compute.** GCN's compute queues, a kernel of the Compute Kit's on
  the CUs - and the second card, which shows nothing, given to Xavier
  (`xavier.md`, *All the acceleration there is*: the model's layers across
  both cards, or one card the model and one the desktop).
- **A6. Vulkan by radv.** Mesa's radv supports Southern Islands (GFX6)
  through ACO. Its winsys is amdgpu's kernel interface - `libdrm_amdgpu`'s
  buffer objects, VA mapping per process, command submission with fences
  and syncobjs, and the context per queue - so what this driver would have
  to give is the same shape `gpu3d.md` §5.3 gave anv on Intel: per-process
  VM (contexts 1 to 15 of the MC's VM), buffers bound at addresses the
  process chooses, submission of an IB (`DMA_IB_PACKET` / PM4
  INDIRECT_BUFFER) with a fence the process can see, and a timeline. radv
  would talk to it through a Kosmos winsys of its own, as anv talks to
  `intelgpu_ctx` - not through a DRM device.

## Files

- `user/drivers/display/amdgpu.c` - the driver.
- `user/drivers/display/amdgpu_decode.h`, `.c` - the registers, the PCI ID
  table (73 Southern Islands ids, generated from `drm_pciids.h`), the
  decoding, and the DMA commands.
- `tools/test_amdgpu_decode.c` - 56 checks; four of the decode's lines
  broken one at a time (NOOFCHAN's shift, the fill's high byte, a
  controller's offset, the copy's operands) each turn one or more red.
