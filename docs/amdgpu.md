<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
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
- **3D and Vulkan, V1 to V10** - the road to radv on these cards, below.
  V1 (interrupts) is built and runs at boot; V2 (the GART and programs'
  address spaces) and V3 (the command processor and its microcode) are
  built and run when asked (`gpu engine gart`, `gpu engine cp`); V4's
  packets are built; V5 to V10 are planned. Held by
  `tools/test_amdgpu_engine.c`, 279 checks.
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

Named as Intel's were, `m700-2d.md` and `gpu3d.md`. A1, A2, A4 and A6 are
now *3D and Vulkan*'s V1, V2, V3 and V7, below, where they are built and
planned in detail; A3 and A5 stand as written.

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

## 3D and Vulkan

Diego, 20 November 2026: "For the AMD gpu driver we need full 3d and vulkan
hardware support". What follows is the road from the DMA engines of 3a to
radv drawing a frame on the Mac Pro and Flight on top of it, in the order it
has to be built. Each step says what proves it - a host test with controls,
and the exact `amdgpu:` line the Mac Pro's boot or a typed `gpu` command
shows - and every register is named with the Linux v6.12 file and line it
is from. Where a datum is missing, the step says so.

**Built so far: V1, V2 and V3's code, the packets V4 needs, and their
checks.** None of it has run on the card yet. The pure parts are held on
the Mac by `tools/test_amdgpu_engine.c`: 279 checks, and every byte offset
the new code uses is read back from amdgpu's own `*_d.h` headers, which
number the registers in dwords from AMD's register database, separately
from radeon's byte offsets. Eleven controls each break one line of the
code and each turns red (*The checks*, below).

| Step | What | State | Seen on the Mac Pro |
|---|---|---|---|
| V1 | The IH ring and the card's MSI; the DMA engines' traps | built, held | at boot |
| V2a | The GART: context 0 over system pages | built, held | `gpu engine gart` (x 2) |
| V2b | Programs' address spaces: VMIDs 1-15, two levels | built, held | the same command |
| V3 | The RLC and the CP's microcode, ring 0, ME_INITIALIZE, the ring test, an EOP fence | built, held; firmware put in the image by a Makefile patch | `gpu engine cp` (x 3) |
| V4 | Indirect buffers in a VMID, fences a process can see | packets built and held; the verb to come | - |
| V5 | The GFX engine set up: `si_gpu_init`, the tiling table, the clear state | to build | - |
| V6 | `gpuproto.h`'s context verbs on this driver | to build | - |
| V7 | `radv_kosmos`: radv's winsys on the GPU Kit | to build | - |
| V8 | A spinning cube (`vkcube`'s equivalent) by radv on the card | to build | - |
| V9 | Buffers in VRAM | to build | - |
| V10 | The 3D Kit's Vulkan path, and Flight, on radv | to build | - |

### The shape of the code

Everything that can be computed without the card is: an interrupt entry
decoded, a page table entry built, a ring's control word, a PM4 packet.
Bringing a block up is a **program** - `struct amdgpu_prog` in `amdgpu_ih.h`,
a list of register writes in Linux's order, with a delay or a bounded poll
where Linux has a `udelay` or a loop - computed by a pure function and
performed by the driver's `run`. That is why the order and every value is
held on the Mac, and why the Mac Pro only has to say whether the card
agrees.

| File | What |
|---|---|
| `amdgpu_ih.h`, `.c` | programs; the IH set up and its entries read (V1) |
| `amdgpu_vm.h`, `.c` | the GART and VMIDs: entries, geometry, the enable program, a table walk, a fault (V2) |
| `amdgpu_cp.h`, `.c` | firmware checked, the RLC and CP brought up, PM4 packets (V3, V4) |
| `amdgpu_fw.h`, `.c` | the firmware in the image by `.incbin`, when the build names it |
| `amdgpu_decode.c` | the DMA engine's TRAP, INDIRECT_BUFFER, SRBM_WRITE and VM flush added |
| `amdgpu.c` | the driver: V1 at boot, V2 and V3 on request |

**Nothing waits on the card without a bound.** A program's poll is
`AMDGPU_POLL_US`, Linux's `usec_timeout`, 100 ms: spun for the first
millisecond, a tick apart after. A fence word is waited for 50 ticks, an
interrupt entry ten. **Nothing at boot writes the display**: V1 masks the
display controllers' interrupts and touches no other display register.

### V1. Interrupts

The IH is a ring in memory the GPU writes a 16-byte entry into for every
event, raising its MSI after. 64 KB, as `si_ih_sw_init` sizes it, in the
driver's work area in VRAM (`WORK_IH`, BAR 0 + 0x50000 past the work area's
start); with no GART at boot it cannot be anywhere else.

- **Set up as `si_ih_irq_init`** (amdgpu `si_ih.c` 62-99): ring and
  interrupts off, the pointers nought, INTERRUPT_CNTL2 the dummy read's
  page >> 8, INTERRUPT_CNTL without DUMMY_RD_OVERRIDE and - the ring being
  in VRAM - with IH_REQ_NONSNOOP_EN, which radeon's `si.c` 5985-5986 says a
  ring in VRAM wants and amdgpu, whose ring is in RAM, clears. IH_RB_BASE
  the ring >> 8, IH_RB_CNTL OVERFLOW_ENABLE, OVERFLOW_CLEAR and the size
  (log2 of its dwords << 1, `si_ih.c` 77-81) - no write-back, so the write
  pointer is read from IH_RB_WPTR as radeon does without it - then IH_CNTL
  with MC_WRREQ_CREDIT 16, MC_WR_CLEAN_CNT 16, VMID 0, and RPTR_REARM only
  with an MSI (`si_ih.c` 90-93).
- **Every other source quiet**, `si_disable_interrupt_state` (radeon `si.c`
  5928-5956): CP ring 0 keeps only CNTX_BUSY and CNTX_EMPTY, rings 1 and 2
  nought, both DMA engines' TRAP_ENABLE off, GRBM and SRBM interrupts off,
  each display controller's INT_MASK and GRPH_INT_CONTROL nought. The
  hot-plug pins and the DAC are the display's and are left alone.
- **Then on**, `si_ih_enable_interrupts` (`si_ih.c` 35-45): IH_CNTL's
  ENABLE_INTR, then the ring's RB_ENABLE.
- **The proof**: 3a's DMA test, at boot, puts a TRAP after its fence
  (`si_dma_ring_emit_fence`, amdgpu `si_dma.c` 91-111) with DMA_CNTL's
  TRAP_ENABLE set (`si_irq_set`, radeon `si.c` 6060-6089), and the entry
  must arrive: source 224 for DMA 0 and 244 for DMA 1 (`si_irq_process`,
  6266-6411). An entry is `src_id` in dword 0 7:0, `src_data` dword 1 27:0,
  ring dword 2 7:0, VMID 15:8 (`si_ih_decode_iv`, `si_ih.c` 132-151).
- **Registers**: IH_RB_CNTL 0x3e00 ... IH_CNTL 0x3e18 (radeon `sid.h`
  651-685; amdgpu `oss_1_0_d.h` 225-235, dwords 0xF80-0xF86),
  INTERRUPT_CNTL 0x5468 and _CNTL2 0x546c (`bif_3_0_d.h` 633-634),
  CP_INT_CNTL_RING0-2 0xc1a8-0xc1b0 (`sid.h` 1276-1278), DMA_CNTL 0xd02c
  (amdgpu `sid.h` 1897, dword 0x340b), INT_MASK 0x6b40 (`dce_6_0_d.h`
  4495), GRPH_INT_CONTROL 0x685c (`dce_6_0_d.h` 3831).
- **What the kernel must give**: the MSI. `hal/pc/devices.c` hands the AMD
  kinds `.intid = 0` today; the patch takes the MSI with `pci_take_
  interrupt` once and keeps it, as `intel_gpu_interrupt` does, and turns
  bus mastering on - which MSI needs (an MSI is a write by the device), the
  GART needs, and `pci_take_interrupt` does not do. Without it the driver
  polls the ring, and the line says so.

The Mac Pro's lines, at boot, per card:

```
amdgpu: card 0: IH on: a 64 KB ring at MC 0xf400250000, its MSI vector 50
amdgpu: card 0: DMA 0 works: word deadbeef, fence 1, ...
amdgpu: card 0: IH entry: source 224 (DMA 0's trap), data 0000000, ring 0, VMID 0
amdgpu: card 0: DMA 0's trap reached the IH: source 224, ring 0; its MSI heard
```

and the same for DMA 1 with 244. Without the kernel's patch the first reads
`no MSI told by the board - entries are polled` and the last `no MSI to
hear`. A ring that stays empty says `DMA 0's trap did NOT reach the IH`,
with IH_RB_WPTR, IH_RB_CNTL and IH_CNTL as read.

### V2. The GART, and programs' address spaces

**Asked for, never at boot**: `gpu engine gart` (GPU_OP_ENGINE_TEST with
`x` 2). It turns the memory controller's translation on under a screen the
firmware drew, and the first time should be watched.

**V2a, the GART.** Context 0 of the MC's VM, one level (depth 0), mapping
GTT_BYTES - 64 MB, a 128 KB table in the work area - page for page to
system memory.

- **Where it goes**: `radeon_gtt_location` (`radeon_device.c` 603-625) -
  after VRAM when there is more room above it than below, in a 40-bit MC
  space, alignment nought on Southern Islands.
- **The order**, `gmc_v6_0_gart_enable` (amdgpu `gmc_v6_0.c` 457-545) with
  `si_pcie_gart_enable`'s values (radeon `si.c` 4265-4350):
  MC_VM_MX_L1_TLB_CNTL = 0xA << 7 | ENABLE_L1_TLB | ENABLE_L1_FRAGMENT_
  PROCESSING | SYSTEM_ACCESS_MODE_NOT_IN_SYS | ENABLE_ADVANCED_DRIVER_MODEL
  = 0x55b; VM_L2_CNTL = ENABLE_L2_CACHE | L2_FRAGMENT | PTE and PDE0 LRU by
  write | EFFECTIVE_L2_QUEUE_SIZE(7) | CONTEXT1_IDENTITY_ACCESS_MODE(1);
  VM_L2_CNTL2 both invalidates; VM_L2_CNTL3 BIGK_ASSOCIATIVITY |
  BANK_SELECT(4) | BIGK_FRAGMENT_SIZE(4) (radeon's; amdgpu writes its
  fragment size there); context 0's START, END in pages, BASE the table >>
  12, PROTECTION_FAULT_DEFAULT the dummy page >> 12, CNTL2 nought, CNTL
  ENABLE | DEPTH(0) | RANGE_PROTECTION_FAULT_ENABLE_DEFAULT; 0x15d4, 0x15d8
  and 0x15dc nought (named nowhere; both drivers clear them); and the flush,
  HDP_MEM_COHERENCY_FLUSH_CNTL then VM_INVALIDATE_REQUEST bit 0
  (`si_pcie_gart_tlb_flush`, 4255-4262).
- **Entries**: 64 bits, the address in 39:12 (`gmc_v6_0_get_vm_pde`'s
  check, 0xFFFFFF0000000FFF), VALID 0, SYSTEM 1, SNOOPED 2, READABLE 5,
  WRITEABLE 6, FRAGMENT 11:7 (radeon `radeon.h` 847-855, amdgpu
  `amdgpu_vm.h` 57-70). EXECUTABLE, bit 4, is stripped on SI (`gmc_v6_0.c`
  372-377). A page of system memory is 0x67 in the low bits.
- **A choice that differs from Linux**: every entry not bound points at the
  dummy page, *valid*. radeon leaves an unbound entry invalid (RADEON_GART_
  PAGE_DUMMY is 0, `radeon.h` 599) and lets a fault default to the dummy
  page - but context 0 is told to default only range faults, and where an
  invalid entry inside the range writes is not written anywhere this
  driver has read. A valid entry to a page this driver owns writes
  somewhere known.
- **The system aperture** must be VRAM so that, under NOT_IN_SYS, a VRAM
  address - the rings, the IH, the work area - is not translated. `si_mc_
  program` sets it so (`si.c` 4144-4150); the driver writes it only when the
  firmware did not.
- **The proof**: SYS_PAGES pages of system memory, one physical run
  (`kosmos_mem_create_flags(.., MEM_CONTIGUOUS)` and `kosmos_mem_phys`). GTT
  page 0 is system page 0, GTT page 1 is system page **2**, every other the
  dummy, page 4. DMA 0 writes a different number to GTT pages 0, 1 and 2,
  and they must arrive at system pages 0, 2 and 4 - which says the table is
  read, and not the address taken as it is - with every other word nought.

**V2b, programs' address spaces.** Contexts 1 to 15, two levels: a page
directory of PDEs, each the MC address of a page table in VRAM with VALID
(amdgpu `amdgpu_gmc.c` 113-114), and tables of PTEs.

- **The geometry**, radeon's rule when none is given (`radeon_device.c`
  1179-1190): bits = log2(GB) + 18; the block - log2 of a table's entries,
  VM_CONTEXT1_CNTL taking it less 9 - is bits - 9 up to 8 GB and
  (bits + 3) / 2 above. The test uses **8 GB**, radeon's default
  (`radeon_drv.c` 139): a 4 KB directory and 32 KB tables. amdgpu uses 64
  GB (`amdgpu_vm_adjust_size(adev, 64, 9, 1, 40)`, `gmc_v6_0.c` 826) -
  block 13, 16 KB and 64 KB - which is what radv will be handed (V7); the
  geometry is one number in the driver.
- **Contexts 1-15**: START nought, END max_pfn - 1, every VMID's base the
  GART's table until a program's is written (`gmc_v6_0.c` 517-527), the
  fault default the dummy page, CNTL2 = 4, and CNTL = ENABLE | DEPTH(1) |
  BLOCK_SIZE(block - 9) | every fault with its interrupt and to the default
  page (radeon `si.c` 4325-4342) - 0x040db6db at block 13.
- **VMID n's base**: VM_CONTEXT0_PAGE_TABLE_BASE_ADDR + 4n for 0-7
  (0x153c), VM_CONTEXT8_... + 4(n - 8) for 8-15 (0x1438), the directory's
  MC address >> 12 (`si.c` 4317-4323).
- **Flushed from the DMA ring** before an IB runs in a VMID (`si_dma_vm_
  flush`, radeon `si_dma.c` 185-215): SRBM_WRITE of the base, of the HDP
  flush and of VM_INVALIDATE_REQUEST 1 << vmid, then POLL_REG_MEM on
  VM_INVALIDATE_REQUEST. **One datum disagrees**: radeon puts
  VM_INVALIDATE_REQUEST's byte offset (0x1478) in the poll, amdgpu its dword
  (0x51e, amdgpu `si_dma.c` 445). The driver follows amdgpu; the poll's
  function is "always", so the wrong one would cost a read of the wrong
  register and not a hang, and the card will say which is right.
- **The DMA engine's IB** (`si_dma_ring_emit_ib`, amdgpu `si_dma.c`
  62-77): NOPs until the ring's write pointer is 5 modulo 8, then
  DMA_IB_PACKET(4, vmid, 0) - (4 << 28) | (vmid << 20) - the address 32-byte
  aligned, and (length << 12) | the high byte.
- **The proof**: VMIDs 1 and 2 each map VA 0x100000 to a page of its own
  (system pages 1 and 5) and VA 0x101000 to the same page of indirect
  buffers. DMA 0 flushes VMID 1, runs an IB there that writes 0x11111111 to
  VA 0x100000, flushes VMID 2, runs one that writes 0x22222222 to the same
  VA; each must land on its own page. **The control**: a third IB in VMID 1
  writes VA 0x200800, which neither maps, and an IH entry from source 146
  or 147 must come, with VM_CONTEXT1_PROTECTION_FAULT_STATUS (0x14dc) saying
  VMID 1 (28:25) wrote (24), and _ADDR (0x14fc) page 0x200 (`sid.h`
  420-435); the fault is let go by setting VM_CONTEXT1_CNTL2 bit 0, as
  `si_irq_process` does (`si.c` 6350-6355).

The lines, after `gpu engine gart`:

```
amdgpu: card 0: GART on: 64 MB at MC 0xf500000000, its table at MC 0xf400261000; programs' contexts 8 GB, block 12
amdgpu: card 0: GART works: GTT pages 0, 1, 2 wrote 6a770000 6a770001 6a770002 to system pages 0, 2 and the dummy; fence reached, N us
amdgpu: card 0: VMIDs work: the same address wrote 11111111 in VMID 1 and 22222222 in VMID 2, each to its own page
amdgpu: card 0: IH entry: source 146 (a VM fault), ...
amdgpu: card 0: the control faulted as it should: source 146, VMID 1 writing 0x0000200000, client .., protections ..; its write went to the dummy page; MSI heard
```

(The MC addresses are what this card's FB_LOCATION puts them at; the
numbers above are illustrations.) It needs bus mastering on, and says
`bus master is off` and stops where the firmware left it off and the
kernel's patch is not in.

**Not known, and what will tell**: whether a PTE for a page *in VRAM* is the
VRAM offset or the MC address. amdgpu writes `vram_base_offset` plus the
offset (nought on a discrete card, `gmc_v6_0.c` 873; `amdgpu_gmc.c`
1051-1053), which reads as the offset, but `amdgpu_vm.c` - where a VRAM
PTE is built - was not fetched. V2 maps only system pages for that reason;
V9 settles it on the card with a control: one PTE each way, and which one
the engine's write lands through.

### V3. The command processor

The CP is three processors - the PFP that fetches, the ME that executes,
the CE that holds constants - and the RLC beside them, and none does
anything until its microcode is written into it a word at a time.

- **The firmware**: AMD's, linux-firmware `radeon/`, downloaded into
  `build/downloads/radeon-firmware/` (12 files, 266 KB). The four the CP
  needs per chip: `tahiti_pfp.bin`, `_me.bin`, `_ce.bin`, `_rlc.bin` (8832,
  8832, 8832 and 8448 bytes), or `pitcairn_*` (a D300); the PFP, ME and CE
  are byte for byte the same for both chips, the RLC is not (`ucode_version`
  0xd on Tahiti, 0x7 on Pitcairn). They are the new kind, with a header
  (`radeon_ucode.h` 156-198): size, header size, version 1.0, IP 6.0, the
  version, the payload's length and offset (0x100), a crc32. The payload
  must be exactly SI_PFP/PM4/CE_UCODE_SIZE 2144 dwords or SI_RLC_UCODE_SIZE
  2048 (`radeon_ucode.h` 35-37, 51; `si.c` 1668-1671 requires it). **The
  crc32 is not checked**: it is not zlib's crc32 over the payload, and what
  it is over is written in nothing fetched. The MC and SMC firmware are not
  needed for a card the firmware posted - both Mac Pro cards' memory
  controllers run (step 1).
- **Carried in the image** by `amdgpu_fw.c`: `.incbin` of the eight files
  between labels in `.rodata` when the build passes
  `-DAMDGPU_FW_DIR="build/downloads/radeon-firmware"`, 70 KB; without it
  `amdgpu_fw_image` answers no and the driver says `the PFP's firmware is
  not in this image`. Nothing is weak, so a build without the files has
  nothing to resolve.
- **The RLC** (`gfx_v6_0_rlc_resume`, amdgpu `gfx_v6_0.c` 2480-2524):
  stopped - RLC_CNTL nought, the GUI idle interrupt off with Linux's read of
  DB_DEPTH_INFO and its wait for RLC_STAT & 0xf to say clock and power on
  (2214-2239), both SERDES masters idle (2398-2413) - and reset, GRBM_SOFT_
  RESET's SOFT_RESET_RLC for 50 us (2457-2463). Without power gating
  (`gfx_v6_0_init_pg`, 2896-2899) the save-and-restore and clear-state
  buffers' addresses >> 8; RL_BASE, RL_SIZE, LB_CNTL nought, LB_CNTR_MAX
  all ones, LB_CNTR_INIT nought, LB_INIT_CU_MASK all ones, MC_CNTL and
  UCODE_CNTL nought; for each word i, RLC_UCODE_ADDR i and RLC_UCODE_DATA
  it; ADDR nought. Load balancing off - Linux has it only for DDR3
  (2465-2474), and these cards are GDDR5 - which selects every SE and SH
  and writes SPI_LB_CU_MASK 0xff (2388-2396). Then RLC_ENABLE, and the GUI
  idle interrupt back.
- **DB_DEPTH_INFO is at 0x2803c**, past the 76 KB the driver maps, and is
  read through MM_INDEX and MM_DATA (0 and 4) as radeon reads past its
  mapping (`radeon.h` 2455-2470).
- **The CP's microcode** (`gfx_v6_0_cp_gfx_load_microcode`, 1927-1980):
  halted - CP_ME_CNTL ME, PFP and CE halt, SCRATCH_UMSK nought, 50 us
  (1914-1925) - then PFP_UCODE_ADDR nought, the PFP's words into
  PFP_UCODE_DATA, ADDR nought; the CE's the same; ME_RAM_WADDR nought, the
  ME's into ME_RAM_DATA, WADDR nought; then the four address registers
  nought.
- **Ring 0** (`gfx_v6_0_cp_gfx_resume`, 2047-2099): SEM_WAIT_TIMER,
  SEM_INCOMPLETE_TIMER_CNTL, RB_WPTR_DELAY, CP_DEBUG and SCRATCH_ADDR
  nought; CP_RB0_CNTL = (log2(4096 / 8) << 8) | log2(ring / 8), then with
  RB_RPTR_WR_ENA; WPTR nought; the read pointer's write-back address;
  SCRATCH_UMSK nought; 1 ms; CNTL without RPTR_WR_ENA; RB0_BASE the ring >>
  8. A 4 KB ring in the work area.
- **The first packets** (`gfx_v6_0_cp_gfx_start`, 1982-2045), committed
  while the CP is halted, then CP_ME_CNTL nought: PACKET3(ME_INITIALIZE, 5)
  1, 0, max_hw_contexts - 1 = 7 (8 on every SI part, 1579-1613),
  DEVICE_ID(1) << 16, 0, 0; PACKET3(SET_BASE, 2) CE_PARTITION_BASE (3),
  0xc000, 0xe000. A type-3 header is (3 << 30) | (op << 8) | (count - 1)
  << 16 (`sid.h` 1595-1597, RADEON_PACKET_TYPE3 3, `radeon_reg.h` 3719).
- **The proof, Linux's ring test** (`gfx_v6_0_ring_test_ring`, 1766-1794):
  SCRATCH_REG0 (0x8500) set to 0xCAFEDEAD by the processor, then
  SET_CONFIG_REG of it, 0xDEADBEEF, by the CP; the register must read
  0xDEADBEEF. **Then an end-of-pipe fence** (`gfx_v6_0_ring_emit_fence`,
  1803-1829): SET_CONFIG_REG CP_COHER_CNTL2 nought, SURFACE_SYNC of TCL1,
  TC, SH_KCACHE and SH_ICACHE, then EVENT_WRITE_EOP of
  CACHE_FLUSH_AND_INV_TS_EVENT (20, `sid.h` 1390) with EVENT_INDEX 5,
  DATA_SEL 1 and INT_SEL 2, writing 1 to a word - with CP_INT_CNTL_RING0's
  TIME_STAMP_INT_ENABLE (bit 26) on, so source 181 must reach the IH.
- **Not sent yet**: the clear state. `gfx_v6_0_cp_gfx_start` follows
  SET_BASE with PREAMBLE_CNTL BEGIN_CLEAR_STATE, the context registers of
  `si_cs_data` (`clearstate_si.h`, AMD's, MIT), PREAMBLE_CNTL
  END_CLEAR_STATE, CLEAR_STATE, and SET_CONTEXT_REG 0x316 = 0xe, 0x10. That
  is drawing's, V5's. The CP is halted again after the test.

The lines, after `gpu engine cp`:

```
amdgpu: card 0: CP: microcode PFP 4f, ME 8e, CE 39, RLC 0d loaded (it was halted)
amdgpu: card 0: IH entry: source 181 (a CP end of pipe), ...
amdgpu: card 0: CP ring works: SCRATCH_REG0 deadbeef, rptr 001c, N us; EOP fence reached, source 181 in the IH, MSI heard
```

The versions are the Tahiti files' (0x4f, 0x8e, 0x39, 0xd); a Pitcairn
card says RLC 07. `the RLC would not stop - waiting on register 0c34c` is
RLC_STAT not reading clock and power on in 100 ms.

### V4. Indirect buffers and fences a process can see

What radv submits is an indirect buffer: its commands in its own memory,
run by the GFX ring in its VMID.

- **The packets, built and held**: PACKET3(INDIRECT_BUFFER, 2) (0x32;
  INDIRECT_BUFFER_CONST 0x31 for the CE, after SWITCH_BUFFER 0x8b, the
  first of a frame) - the address dword aligned, its high 16 bits, and
  length | vmid << 24 (`gfx_v6_0_ring_emit_ib`, 1831-1863); the VM flush
  on the GFX ring (`si_vm_flush`, radeon `si.c` 5056-5099) - WRITE_DATA by
  the PFP (ENGINE_SEL 1, DST_SEL 0) of the VMID's base, of the HDP flush and
  of the invalidate, WAIT_REG_MEM "always" on VM_INVALIDATE_REQUEST, and
  PFP_SYNC_ME; the fence of V3 with DATA_SEL 2 for a 64-bit value.
- **What is left**: a VMID per submission, not per context - amdgpu's
  `amdgpu_vmid_grab`: a context's page directory is given whichever of
  VMIDs 1-7 is free or least recently used (amdgpu keeps 1-7 for GFX,
  `gmc_v6_0.c` 860-863) and flushed into it, so the number of contexts is
  not limited to fifteen. The fence page `gpuproto.h` already gives a
  context (CONTEXT_NEW), written by the EOP's 64-bit value through the GART,
  and the pulse to FENCE_WAIT's endpoint on source 181.
- **The proof, to be written**: an IB in VMID 1 that writes a number, and
  its EOP fence on a page bound in the same context; then two contexts
  whose IBs write the same VA to different pages, as V2b does with the DMA
  engine.
- **The line**: `amdgpu: card 0: IB in VMID 1 ran: word ..., fence 1 on the
  context's page, source 181`.

### V5. The GFX engine set up for drawing

What radv expects the kernel to have done, and reads back:

- `si_gpu_init` (radeon `si.c` 3069 on): GB_ADDR_CONFIG (0x98f8), the
  render backends - CC_RB_BACKEND_DISABLE (0x98f4) and
  GC_USER_RB_BACKEND_DISABLE (0x9b7c) - and the raster config they decide,
  and the tiling table, `si_tiling_mode_table_init` (`si.c` 2469 on): 32
  GB_TILE_MODEn from 0x9910, one table for Tahiti and Pitcairn, another for
  Verde, Oland and Hainan. radv learns all of these from the kernel
  (`ac_query_gpu_info`, Mesa `ac_gpu_info.c` 1438) and computes every
  surface's layout from them.
- The clear state: `clearstate_si.h` and `clearstate_defs.h` vendored as
  AMD shipped them, the RLC's clear-state buffer filled from them as
  `gfx_v6_0_rlc_init` does, and `gfx_v6_0_cp_gfx_start`'s second half sent.
- **The proof**: GB_ADDR_CONFIG and the tile modes read back as written,
  and a CLEAR_STATE followed by the ring test still answering. **Missing
  datum**: the golden registers (`si_init_golden_registers`, `si.c` 1208) - a list per
  chip of register values AMD says to set; fetched with `si.c` but not yet
  read line by line.

### V6. The context verbs on this driver

`gpuproto.h`'s programs' contexts (`gpu3d.md` §5.3) are the right shape for
an AMD GPU as they are: a context is a page table and a timeline, BIND puts a
region at an address the caller chose, EXEC runs a batch and signals, and
FENCE_WAIT pulses. They are reused, and only four things differ:

| Verb | On the AMD GPU |
|---|---|
| CONTEXT_NEW (`engine`) | a page directory in VRAM and a fence page in the GART; `GPU_ENGINE_RENDER` the GFX ring, `GPU_ENGINE_COPY` a DMA engine |
| BIND (`flags`) | GPU_CACHE_WB is a system page SYSTEM \| SNOOPED, GPU_CACHE_UC SYSTEM without SNOOPED; GPU_CACHE_WB_ELLC is Intel's and is refused |
| EXEC | an INDIRECT_BUFFER on the GFX ring, or DMA_IB_PACKET on a DMA engine; `gpu` the IB's VA in the context |
| CONTEXT_STATE | `words[5]`, the faults, from source 146/147 entries with the context's VMID |
| **new: QUERY** | what radv's `radeon_info` needs and only the driver knows - below |

- **GPU_OP_QUERY, new.** A declared struct of what `ac_query_gpu_info`
  fills from `amdgpu_query_info`: the device and revision ids, the family,
  VRAM and GART sizes, the CU and RB masks per SE and SH, GB_ADDR_CONFIG,
  the 32 tile modes, the max engine and memory clocks, the CP's and RLC's
  firmware versions, the VA range given to contexts. Mesa's own
  derivations then run on it unchanged (V7).
- **GPU_ERR_NO_DEVICE's comment** says "no Intel GPU this driver knows";
  it becomes "no GPU".
- **The context core is the Intel driver's** - `intelgpu_ctx.c` is
  hardware-free in everything but its page table, `igpu_ppgtt`. The kits
  premise asks for one core and two backends: a GPU-neutral `gpu_ctx.c`
  (the contexts, their bindings, the timelines, the waits and pulses), and
  the page table and the submission behind a small table of functions -
  Intel's PPGTT and ring, AMD's VMID tables and IB. Proposed as a patch,
  not done: it is the Intel driver's file.

### V7. radv on Kosmos

radv is Mesa's Vulkan driver for AMD, and supports Southern Islands
(`radv_is_gpu_supported`, Mesa 26.2.5 `radv_physical_device.c` 2504) with
ACO compiling its shaders. It talks to the kernel through a **winsys**,
`struct radeon_winsys` (`radv_radeon_winsys.h` 228 on, 44 functions), whose
one implementation is amdgpu's DRM interface (`winsys/amdgpu/`).

- **A `radv_kosmos` winsys at that seam**, as anv got `anv_kosmos`
  (`runtime/ports/mesa/src/intel/vulkan/kosmos/`): `winsys/kosmos/`, on
  the GPU Kit (`kosmos_gpu.h`), its fd the capability to `/Devices/gpu`.
  - **A buffer object is a region**, its handle the region's capability
    plus one; `buffer_create` makes one, `buffer_map` maps it, and every BO
    is **resident** - BIND at creation - so radv's per-submission BO lists
    are empty (`is_local` on every BO, radv's own fast path).
  - **Virtual addresses** radv chooses, in the range QUERY gave; BIND at
    them, softpinned, as anv's are.
  - **A context per queue**, CONTEXT_NEW; `cs_submit` is EXEC of the IB
    with the context's timeline value; radv's command streams are built by
    its own `radv_cs` code, which is not the winsys's.
  - **Sync**: today `info->syncobj_sync_type = vk_drm_syncobj_get_type(fd)`
    (`radv_amdgpu_winsys.c` 363). anv's "point" `vk_sync` type -
    `anv_kosmos.c` 132-320, a point on a context's timeline waited for by
    the fence page and FENCE_WAIT's pulse - has nothing Intel's in it: it
    moves to `src/vulkan/runtime/vk_kosmos_sync.c` and both drivers use it.
  - **GFX6 specifics radv already knows**: `can_chain_ib2` is false before
    GFX7 (`ac_gpu_info.c` 1150), so radv sends one IB per submission
    rather than chaining; and compute queues are not offered on GFX6
    (`radv_physical_device.c` 146-147, "may hang"), so only the GFX queue
    and the DMA (transfer) queue exist.
- **`radeon_info` from QUERY**: `ac_query_gpu_info` (Mesa `ac_gpu_info.c`
  1438) fills it by `ac_drm_query_*` (`ac_linux_drm.h`, about 45
  functions, plain structs). A query-only Kosmos `ac_drm_device` answers
  those from the QUERY reply, so Mesa's derivations - every limit and
  feature radv reads - are Mesa's and not a copy of them.
- **Patched in radv**: `radv_physical_device_try_create` (2518 on) opens a
  DRM device - the enumerate hook becomes Kosmos's, as anv's did; the
  winsys is chosen at `radv_device.c` 1347; the rest is the freestanding
  build anv's port already solved (`0001-kosmos-is-an-os.patch`,
  `0006-anv-on-kosmos.patch`).
- **GTT first**: every BO in system pages, an APU's shape. VRAM BOs are V9.

### V8. A spinning cube

The order to the first frame, each a check before the next: `vulkaninfo`'s
equivalent naming the device and its limits from QUERY; a buffer filled by
`vkCmdFillBuffer` on the transfer queue (the DMA engine, as 3a); a compute-
free clear of an image by the GFX queue (`vkCmdClearColorImage`); a
triangle; then the cube - the 3D Kit's own spinning cube, which `make test`
already draws on lavapipe and the M700's anv - into a window by
`wsi_common_kosmos.c`. **The line**: GPU Monitor's frame count rising with
the cube's, and the IH's source-181 count rising with it.

### V9. Buffers in VRAM

A region over a range of BAR 0 - a new kind of memory object in the kernel,
write-combined - so radv's DEVICE_LOCAL heap is VRAM and the compositor can
flip to a buffer the GPU drew. Settles V2's open question first (a VRAM PTE,
two ways, a control). BAR 0 is 256 MB; the rest of VRAM is reachable by the
engines only, which radv already handles as a heap without CPU access.

### V10. The 3D Kit and Flight

The 3D Kit's Vulkan path - `user/kits/3d/k3d_vk*.c` - asks for "a Vulkan
device" and gets lavapipe, anv or radv as the loader finds them; nothing in
the Kit or in Flight names a vendor. **The proof** is Flight over its city
on the Mac Pro at the frame rate its F-steps hold on the M700, and the
gallery screenshot's Cafesa3D rendered by radv.

### The checks

`tools/test_amdgpu_engine.c`, by hand (the lane builds nothing with make):

```
cc -std=c11 -Wall -Wextra -Werror -O1 -o build/host/test_amdgpu_engine \
   tools/test_amdgpu_engine.c user/drivers/display/amdgpu_ih.c \
   user/drivers/display/amdgpu_vm.c user/drivers/display/amdgpu_cp.c \
   user/drivers/display/amdgpu_decode.c
build/host/test_amdgpu_engine build/downloads/linux-amd build/downloads/radeon-firmware
```

279 checks: the IH program's order and values; an entry decoded; the
write pointer's overflow; the VM geometry at 8, 64 and 1024 GB and three
refusals; a PTE and a PDE; a two-level walk over a model of VRAM, with a
missing table and a missing page; the GTT placed above and below VRAM; the
GART program in order to its last write; the system aperture left alone or
written; a fault decoded; the DMA IB's padding at three ring positions and
its VM flush; the RLC's stop, prepare and start, the CP's halt, load and
resume; every PM4 packet; 85 offsets against amdgpu's dword headers; and
the eight firmware files parsed, each also cut short, given another IP
version and taken as another engine's - which must be refused.

**Controls**, each a copy of the code with one line broken, built and run
(the tree is never edited for them):

| Broken | Turned red |
|---|---|
| context 1 given DEPTH(0) | CONTEXT1_CNTL, and the order after it |
| the IH ring never given RB_ENABLE | "and last, the ring on" |
| EOP event 21 for 20 | the EVENT_WRITE_EOP check |
| CP_RB0_WPTR 0xc118 for 0xc114 | its offset against `gfx_6_0_d.h` |
| the DMA IB padded to 4 for 5 | the padding, header, address and length |
| the firmware's IP version not checked | "as IP 7.0 refused", all eight |
| the block size always bits - 9 | 64 GB, 1 TB, the index and the walk |
| the RPTR_WR_ENA write dropped | CP resume's order |
| a poll's mask taken as a write | "a poll's mask and value are not a write" (the first version of this check did not bite, and was rewritten) |
| the VMID bases >> 8 for >> 12 | context 0's base and the order after |
| the IV's VMID from 23:16 | the entry's decode |


## Files

- `user/drivers/display/amdgpu.c` - the driver.
- `user/drivers/display/amdgpu_decode.h`, `.c` - the registers, the PCI ID
  table (73 Southern Islands ids, generated from `drm_pciids.h`), the
  decoding, and the DMA commands.
- `tools/test_amdgpu_decode.c` - 56 checks; four of the decode's lines
  broken one at a time (NOOFCHAN's shift, the fill's high byte, a
  controller's offset, the copy's operands) each turn one or more red.
- `user/drivers/display/amdgpu_ih.h`, `.c` - programs, and the IH (V1).
- `user/drivers/display/amdgpu_vm.h`, `.c` - the GART and VMIDs (V2).
- `user/drivers/display/amdgpu_cp.h`, `.c` - the firmware, the RLC and the
  CP, PM4 (V3, V4).
- `user/drivers/display/amdgpu_fw.h`, `.c` - the firmware in the image.
- `tools/test_amdgpu_engine.c` - 279 checks, eleven controls (*The checks*).
