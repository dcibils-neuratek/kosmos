# Graph Report - kosmos  (2026-09-06)

## Corpus Check
- Corpus is ~46,298 words - fits in a single context window. You may not need a graph.

## Summary
- 2920 nodes · 5402 edges · 238 communities (151 shown, 21 thin omitted)
- Extraction: 85% EXTRACTED · 15% INFERRED · 0% AMBIGUOUS · INFERRED: 821 edges (avg confidence: 0.86)
- Token cost: 593,405 input · 0 output

## Community Hubs (Navigation)
- Network Stack and Syscall ABI
- Browser Paint and Draw Bridge
- Filesystem Format (kfs)
- Browser Application
- Syscall Bindings
- Window Manager
- Widget Kit
- AArch64 CPU and Context
- CSS Selection Handler
- WAV Header Tests
- Ramfs Protocol
- PDF Kit
- Sysinfo and Audio Counters
- Display Harness
- Graphics Primitives
- AArch64 Exception Handling
- x86-64 CPU Identification
- Design Principles
- Tracker File Manager
- Curve25519 and ChaCha20
- Kernel Screen and Guards
- Blocks Game
- Browser Harness
- Pmm
- Mmu
- Audio
- Console
- Process
- Mmu
- Glossary
- Gl kosmos
- Thread
- Process
- Hal
- Testing
- Ipc
- Hal
- Pic
- Snd
- Run disk
- Setup
- Run screenshot
- Tracker
- Devices
- G3d
- Ui
- Test linked
- Input
- Misc user
- Console
- Design
- Smp
- State
- State
- State
- Architecture
- Gfx
- Clock
- Pdfview
- Kosmos
- Lua glue
- Docfont
- Doom kosmos
- Test page
- State
- Virtio
- Run screenshot
- Bench
- Con kosmos
- Glossary
- Run tests
- Appfs
- Init
- Context
- User
- Glossary
- Layout
- Uart
- Virtio
- Net
- Trap
- Glossary
- State
- State
- Pci
- Syscall
- Run network
- Ui
- Glossary
- State
- Syscall
- Run frames
- About
- Httpd
- Procs
- Binfs
- State
- Fwcfg
- Pc
- Timer
- Ipc
- Calc
- Inflate
- Beos
- State
- Mkpattern
- Reader
- Png
- Gfx
- Pdftok
- Smp
- Virtio
- Syscall
- Progs2c
- Run bench
- Edit
- Gfx
- State
- State
- Rtc
- Gic
- Uart
- Run power
- Terminal
- Mp3 kosmos
- Setup
- Syscall
- Bdf2c
- Network
- Audioring
- State
- Syscall
- Run screenshot
- Run screenshot
- Appearance
- Fwcfg port
- Pc
- Virtio
- Assets2c
- Kernel size
- Luaglobals
- Run web
- Run x86
- Frames
- Paint
- Glossary
- Power
- Run stress
- Deskbar
- Sysmon
- Gl demos
- Architecture
- Setup
- Virtio
- Bin2c
- Bump
- Editor
- Filetypes
- Beos
- Glossary
- Roadmap
- Beos
- Beos
- Glossary
- Glossary
- Glossary
- Glossary
- Glossary
- Roadmap
- Roadmap
- Roadmap
- State

## God Nodes (most connected - your core abstractions)
1. `syscall_dispatch()` - 53 edges
2. `kmain()` - 45 edges
3. `sysinfo` - 39 edges
4. `Failure` - 35 edges
5. `process` - 28 edges
6. `sys.ticks()` - 28 edges
7. `fail()` - 27 edges
8. `Guest` - 23 edges
9. `parse_ppm()` - 23 edges
10. `main()` - 22 edges

## Surprising Connections (you probably didn't know these)
- `Browser layout test page` --semantically_similar_to--> `Boundaries testable from outside`  [INFERRED] [semantically similar]
  tools/test_page.html → docs/architecture.md
- `as_destroy()` --calls--> `pmm_free_page()`  [INFERRED]
  arch/x86_64/mmu.c → kernel/pmm.c
- `syscall_dispatch()` --calls--> `hal_input_pending()`  [INFERRED]
  kernel/syscall.c → hal/virtio/input.c
- `syscall_dispatch()` --calls--> `hal_key_event()`  [INFERRED]
  kernel/syscall.c → hal/virtio/input.c
- `syscall_dispatch()` --calls--> `hal_net_info()`  [INFERRED]
  kernel/syscall.c → hal/virtio/net.c

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **The seven servers moved to C, one at a time** — docs_state_audio_server, docs_state_devices_server, docs_state_binfs, docs_state_libfs, docs_state_appfs, docs_state_console_server, docs_state_ramfs, docs_state_declared_shape, docs_state_language_split [EXTRACTED 1.00]
- **The vendored NetSurf parsing stack behind /kits/web** — docs_state_libhubbub, docs_state_libcss, docs_state_libdom, docs_state_libwapcaplet, docs_state_libparserutils, docs_state_web_kit [EXTRACTED 1.00]
- **The x86-64 port: a second architecture end to end** — docs_state_arch_x86_64_port, docs_state_hal_pc, docs_state_hal_virtio, docs_state_hal_fwcfg, docs_state_musl_math, docs_state_display_harness, docs_state_doubled_lists, docs_state_prose_has_nobody [EXTRACTED 1.00]
- **The path a frame takes, from event to framebuffer** — docs_gfx_full_path, docs_gfx_double_buffer_commit, docs_ui_compositing, docs_design_drawing_model, docs_testing_input_latency, docs_design_uncached_framebuffer [INFERRED 0.85]
- **The order a second architecture is brought up in** — docs_hal_x86_64, docs_hal_long_mode, docs_hal_paging_x86, docs_hal_pic_remap, docs_hal_gdt_ring3, docs_hal_context_switch_x86 [EXTRACTED 1.00]
- **Properties invisible from where the test runs** — docs_testing_benchmark_blind_spot, docs_testing_deleted_rule, docs_testing_run_headless, docs_testing_run_screenshot, docs_gfx_pitch_and_format [INFERRED 0.85]
- **One command crossing every boundary** — docs_architecture_cat_trace, docs_architecture_runner, docs_architecture_console_server, docs_architecture_eighteen_syscalls, docs_architecture_name_not_bytes, docs_glossary_namespace [EXTRACTED 1.00]
- **The eight words for the parts of the system** — docs_glossary_server, docs_glossary_kit, docs_glossary_library, docs_glossary_program, docs_glossary_app, docs_glossary_tool, docs_glossary_driver, docs_glossary_kit_vs_server [EXTRACTED 1.00]
- **The C/Lua line as a layer rather than a judgement** — docs_glossary_language_split, docs_glossary_jitter_argument, docs_glossary_shape_of_the_bug, docs_index_layer_not_judgement, docs_index_audio_server_lesson, docs_glossary_hot_reload_level_1, docs_architecture_c_vs_lua_rule [INFERRED 0.85]

## Communities (238 total, 21 thin omitted)

### Community 0 - "Network Stack and Syscall ABI"
Cohesion: 0.07
Nodes (86): kosmos_call(), kosmos_cap_drop(), kosmos_mem_create(), kosmos_reply(), kosmos_ticks(), tcp_ring, tcp_ring_acquire(), bytes (+78 more)

### Community 1 - "Browser Paint and Draw Bridge"
Cohesion: 0.06
Nodes (71): css_fixed, css_origin, css_unit, dom_document, gfx_draw_height(), gfx_draw_measure(), css_error, css_stylesheet (+63 more)

### Community 2 - "Filesystem Format (kfs)"
Cohesion: 0.08
Nodes (55): die(), ensure(), mounted(), open(), put(), sys.disk_read(), sys.disk_write(), fresh() (+47 more)

### Community 3 - "Browser Application"
Cohesion: 0.07
Nodes (56): sys.ticks(), cell(), cells_of(), drop(), fits(), interval(), lock(), rand() (+48 more)

### Community 4 - "Syscall Bindings"
Cohesion: 0.07
Nodes (64): kosmos_disk_info(), kosmos_disk_read(), kosmos_disk_write(), kosmos_kill(), kosmos_log(), kosmos_mem_map(), kosmos_mem_size(), kosmos_net_recv() (+56 more)

### Community 5 - "Window Manager"
Cohesion: 0.07
Nodes (53): add_damage(), answer_waiting(), badge_size(), boxes_x(), charge(), collect_closing(), compose(), compose_rect() (+45 more)

### Community 6 - "Widget Kit"
Cohesion: 0.06
Nodes (26): apply_focus(), bevel(), dispatch(), gc:fill(), gc:frame(), gc:raised(), gc:sunken(), gc:text() (+18 more)

### Community 7 - "AArch64 CPU and Context"
Cohesion: 0.05
Nodes (27): cpu_identify(), cpu_info, architecture, counter_hz, ctr, id, id_name, implementer (+19 more)

### Community 8 - "CSS Selection Handler"
Cohesion: 0.14
Nodes (44): css_hint, css_qname, css_select_handler, attr_named(), attr_of(), css_error, dom_node, dom_string (+36 more)

### Community 9 - "WAV Header Tests"
Cohesion: 0.08
Nodes (34): chunk(), fmt(), ok(), reader(), refused(), riff(), u16(), u32() (+26 more)

### Community 10 - "Ramfs Protocol"
Cohesion: 0.09
Nodes (38): ram_attr, kind, name, value, ram_reply, count, error, length (+30 more)

### Community 11 - "PDF Kit"
Cohesion: 0.08
Nodes (25): cursor(), Doc:get(), Doc:resolve(), find_startxref(), parse_dict(), parse_name(), parse_number(), pdf.cursor() (+17 more)

### Community 12 - "Sysinfo and Audio Counters"
Cohesion: 0.05
Nodes (37): sysinfo, audio_channels, audio_dry, audio_floor, audio_period, audio_periods, audio_rate, audio_wakes (+29 more)

### Community 13 - "Display Harness"
Cohesion: 0.10
Nodes (36): _band_changed(), check_3d(), check_bars(), check_boot_screen(), check_direct(), check_editor(), check_graphical_mode(), check_interrupt() (+28 more)

### Community 14 - "Graphics Primitives"
Cohesion: 0.16
Nodes (34): lua_State, check_surface(), clip(), fill_span(), font_asset(), font_short_name(), gfx_draw_fill(), ifloor() (+26 more)

### Community 15 - "AArch64 Exception Handling"
Cohesion: 0.10
Nodes (22): dfsc_name(), die_if_killed(), dump(), dump_body(), ec_name(), fault_expect_unwind(), is_abort(), trap_handler() (+14 more)

### Community 16 - "x86-64 CPU Identification"
Cohesion: 0.06
Nodes (23): cpu_identify(), cpu_info, address, brand, counter_hz, family, feat1_ecx, feat1_edx (+15 more)

### Community 17 - "Design Principles"
Cohesion: 0.09
Nodes (34): Typed attributes and live queries, Capabilities by index into a per-process array, The criticality hierarchy, One real filesystem on disk, Haiku, The index is rebuilt at mount, never stored, The inspector, the first app, Synchronous rendezvous IPC (+26 more)

### Community 18 - "Tracker File Manager"
Cohesion: 0.12
Nodes (31): at_point(), box_of(), cell_of(), chosen(), delete_selected(), do_copy(), do_cut(), do_open() (+23 more)

### Community 19 - "Curve25519 and ChaCha20"
Cohesion: 0.16
Nodes (31): fe, chacha20(), chacha20_block(), fe_0(), fe_1(), fe_add(), fe_carry(), fe_copy() (+23 more)

### Community 20 - "Kernel Screen and Guards"
Cohesion: 0.12
Nodes (28): console_log(), console_screen_suspend(), process_may_read(), process_may_write(), process_set_name(), process_table(), range_ok(), screen_get() (+20 more)

### Community 21 - "Blocks Game"
Cohesion: 0.09
Nodes (10): addrspace, proc_info, thread, highest(), level_of(), prio_enqueue(), prio_pick_next(), prio_preempts() (+2 more)

### Community 22 - "Browser Harness"
Cohesion: 0.11
Nodes (28): dark_rows(), find_link(), find_page(), main(), Which rows in a rectangle have ink on them. Ink rather than "not the…, The heights of the consecutive inked stretches, which are text lines., The middle of the first stretch of link-blue text, or None. By colour rather…, The top-left of the browser's page area, or None if it is not there. Found by… (+20 more)

### Community 23 - "Pmm"
Cohesion: 0.12
Nodes (26): as_create(), as_destroy(), as_create(), ipc_caps_release(), memobj_create(), memobj_in_use(), memobj_init(), memobj_page() (+18 more)

### Community 24 - "Mmu"
Cohesion: 0.14
Nodes (21): alloc_table(), as_destroy(), as_map(), as_page_entry(), as_page_phys(), as_unmap(), as_user_may(), descend() (+13 more)

### Community 25 - "Audio"
Cohesion: 0.14
Nodes (27): audio_ring_acquire(), audio_ring_consumed(), audio_ring_publish(), audio_ring_ready(), audio_ring_slot(), audio_ring_space(), audio_ring_valid(), kosmos_snd_queued() (+19 more)

### Community 26 - "Console"
Cohesion: 0.21
Nodes (26): boot_fact(), boot_fact_begin(), boot_fact_end(), boot_stage(), boot_stages_done(), boot_why(), can_draw(), console_attach_screen() (+18 more)

### Community 27 - "Process"
Cohesion: 0.07
Nodes (28): process, arg, exit_code, exited, heap_pages, id, image, image_len (+20 more)

### Community 28 - "Mmu"
Cohesion: 0.14
Nodes (18): alloc_table(), as_map(), as_page_entry(), as_page_phys(), as_unmap(), as_user_may(), descend(), enable() (+10 more)

### Community 29 - "Glossary"
Cohesion: 0.09
Nodes (25): Kosmos Architecture, The kernel as the narrowest box, The stale SMP-ready claim, Statically declared kernel pools, Pervasive multithreading - pushing sand, not rocks, Threads with shared memory, and the locking API it cost, Coroutine, IPC (+17 more)

### Community 30 - "Gl kosmos"
Cohesion: 0.16
Nodes (24): GLenum, lua_State, capability(), check_ctx(), kosmos_gl_kit(), l_begin(), l_blit(), l_call_list() (+16 more)

### Community 31 - "Thread"
Cohesion: 0.13
Nodes (23): net_interrupt(), process_wake_net(), alloc_stack(), alloc_thread(), copy_name(), refresh_effective(), sched_current(), sched_use() (+15 more)

### Community 32 - "Process"
Cohesion: 0.13
Nodes (23): hal_blk_init(), hal_net_present(), hal_snd_present(), alloc_process(), process_abandon(), process_create(), process_grant_audio(), process_grant_console() (+15 more)

### Community 33 - "Hal"
Cohesion: 0.13
Nodes (23): Framebuffer drivers: pitch and channel order, Hardware and bring-up, SMP, supported by design and off, The uncached framebuffer, and the backbuffer rule, Cache coherency before the blit, Pitch and format travel with the handle, The display size as a compile-time constant, hal_fb_flush, the entry virtio-gpu will add (+15 more)

### Community 34 - "Testing"
Cohesion: 0.11
Nodes (22): The arrow keys are not broken, bench/baselines.json, The blind spot a benchmark suite has by construction, make browser, a camera with checks on it, C self-tests, A check that survives deleting its rule is not testing that rule, The scores are a geometric mean, The harness: runner on the host, tests in the guest, serial the channel (+14 more)

### Community 35 - "Ipc"
Cohesion: 0.24
Nodes (22): cap_t, deliver(), install(), ipc_abort(), ipc_call(), ipc_cap_drop(), ipc_cap_grant(), ipc_endpoint_create() (+14 more)

### Community 36 - "Hal"
Cohesion: 0.13
Nodes (22): C discipline and the mandatory flags, arch/ versus hal/, The x86-64 context switch, CR0.WP, without which a read-only mapping is decoration, CR4.SMEP, and why SMAP is off, Endianness, which turned out to be free, The GDT, the TSS and ring 3, The GIC version is not the default (+14 more)

### Community 37 - "Pic"
Cohesion: 0.17
Nodes (16): cpu_set_counter_hz(), pc_in8(), pc_out8(), apply_masks(), eoi(), hal_irq_handle(), hal_irq_init(), in_service() (+8 more)

### Community 38 - "Snd"
Cohesion: 0.16
Nodes (20): consume(), control(), find_output_stream(), hal_snd_dry(), hal_snd_floor(), hal_snd_init(), hal_snd_queued(), hal_snd_wakes() (+12 more)

### Community 39 - "Run disk"
Cohesion: 0.14
Nodes (18): boot(), Failure, machine(), main(), Exception, qemu_args(), Does what was written to the disk survive the machine being turned off? That…, The board this image is for, with one disk attached. The two lists are the same… (+10 more)

### Community 40 - "Setup"
Cohesion: 0.11
Nodes (20): Capability, Endpoint, Freestanding, RP1 - the Pi 5 southbridge, Capabilities, from seL4, Real hardware, a Raspberry Pi 5, ARM Architecture Reference Manual (ARMv8-A), The ARM GNU bare-metal toolchain (+12 more)

### Community 41 - "Run screenshot"
Cohesion: 0.14
Nodes (7): Guest, A booted image, with its serial on pipes and its monitor on a socket. Two…, Drain whatever the serial line has produced, without blocking., The other monitor. Key presses go through the human monitor's `sendkey`, which…, Absolute position, in the tablet's own 0..32767 range., One key press and release, through QEMU's own input plumbing. This is the only…, Empties whatever the monitor has said and nobody read. `sendkey` and…

### Community 42 - "Tracker"
Cohesion: 0.16
Nodes (19): do_paste(), draw_icons(), go_back(), go_forward(), new_folder(), rename_field:on_enter(), rows:draw(), rows:drop() (+11 more)

### Community 43 - "Devices"
Cohesion: 0.25
Nodes (18): answer(), arm_part(), devices_server(), hex(), implementer(), node_cpu(), node_cpu_aarch64(), node_cpu_x86_64() (+10 more)

### Community 44 - "G3d"
Cohesion: 0.13
Nodes (10): g3d.cube(), g3d.multiply(), g3d.orient(), g3d.render(), check(), draw(), near(), row_is() (+2 more)

### Community 45 - "Ui"
Cohesion: 0.13
Nodes (19): Kosmos, Oberon, The seven principles, A Lua table until a declared struct, The UI kit and its consistency rule, The app server and window manager, The UI build order, Control-W, the one reserved key (+11 more)

### Community 46 - "Test linked"
Cohesion: 0.11
Nodes (19): 0.8 - a web browser, and a network under it, What there is to try, A box model, Forms, Images in the browser, Networking, built, A resolver, A web browser (+11 more)

### Community 47 - "Input"
Cohesion: 0.18
Nodes (16): absolute_range(), claim(), hal_input_pending(), hal_key_event(), hal_keyboard_init(), hal_pointer_init(), hal_pointer_poll(), has_absolute_axes() (+8 more)

### Community 48 - "Misc user"
Cohesion: 0.13
Nodes (8): time_t, atoi(), is_space(), sscanf(), strtol(), time(), vsscanf(), va_list

### Community 49 - "Console"
Cohesion: 0.20
Nodes (18): kosmos_getchar(), kosmos_key_event(), kosmos_pointer(), kosmos_wait_input(), l_getchar(), l_key_event(), l_wait_input(), answer() (+10 more)

### Community 50 - "Design"
Cohesion: 0.14
Nodes (18): BeOS, Lua coroutines as the concurrency model, diskfs and kfs.lua, Hot reload, removed September 2026, The journal checksum measurement, kfs.store as the measurement that would settle it, Known risks, The C/Lua language split (+10 more)

### Community 51 - "Smp"
Cohesion: 0.11
Nodes (18): GIC, TPIDR_EL1, get-and-run-kosmos.sh - one command to run it, Only macOS is tested, 0.9 - a second architecture, Two QEMU flags that are not guessable, The way out is to cut scope, not to abandon, hal/virtio - shared drivers, per-board transport (+10 more)

### Community 52 - "State"
Cohesion: 0.12
Nodes (17): audio server, 3-4 audio underruns per 2.3 s (open), browser (browser.lua), /kits/compress, DNS resolver (next), Synchronous IPC (SYS_CALL / SYS_RECEIVE / SYS_REPLY), ipc_receive with a deadline, C is what runs for another process, Lua for a person (+9 more)

### Community 53 - "State"
Cohesion: 0.14
Nodes (17): Capabilities by index, no global names, The display harness (62 display checks), Input delivered while the guest is busy (harness flakiness), init and supervision, Lazy FP/SIMD save, make bench and baselines, make test, namespace kit (new_namespace) (+9 more)

### Community 54 - "State"
Cohesion: 0.14
Nodes (18): cube3d, Drag and drop across windows, g3d.lua - software 3D, gfx (surface primitives), hal/fwcfg, hal/pc, hal/qemu-virt, hal/virtio (shared drivers) (+10 more)

### Community 55 - "Architecture"
Cohesion: 0.13
Nodes (17): Capability index at the syscall boundary, Tracing cat /bin/ls.lua across every boundary, The console server, The eighteen syscalls, The shell hands the name, not the bytes, No global namespace, A process cannot be ended from outside, The runner (+9 more)

### Community 56 - "Gfx"
Cohesion: 0.21
Nodes (17): Where fluidity is won, The incremental GC and its pauses, The graphics build order, Double buffering and an explicit commit, The full path, top to bottom, The GC does not see surface memory, map, the slow escape hatch, Pixels do not live in Lua tables (+9 more)

### Community 57 - "Clock"
Cohesion: 0.21
Nodes (12): refresh(), ticker:tick(), bar:draw(), bar:mouse(), lit(), spans(), civil(), clock.date_string() (+4 more)

### Community 58 - "Pdfview"
Cohesion: 0.21
Nodes (13): chooser(), frame(), open(), show(), sink:key(), sink:mouse(), source_for(), describe() (+5 more)

### Community 59 - "Kosmos"
Cohesion: 0.12
Nodes (16): kosmos_boot_option(), kosmos_endpoint(), kosmos_net_info(), kosmos_share_unmap(), kosmos_sleep(), kosmos_sysinfo(), kosmos_yield(), DG_Init() (+8 more)

### Community 60 - "Lua glue"
Cohesion: 0.16
Nodes (13): kosmos_exit(), kosmos_write(), at_panic(), lua_State, kosmos_lua_dostring(), kosmos_lua_open(), kosmos_lua_seed(), kosmos_lua_time() (+5 more)

### Community 61 - "Docfont"
Cohesion: 0.25
Nodes (15): lua_State, cache_clear(), check_font(), ensure_cmap(), glyph_of(), kosmos_docfont_open(), l_docfont(), l_draw() (+7 more)

### Community 62 - "Doom kosmos"
Cohesion: 0.16
Nodes (13): lua_State, DG_GetTicksMs(), kosmos_CloseFile(), kosmos_doom_open(), kosmos_OpenFile(), kosmos_Read(), l_frame(), l_key() (+5 more)

### Community 63 - "Test page"
Cohesion: 0.13
Nodes (16): C at EL0 inside a process, C vs Lua decided by blast radius, Pixel loops are the standing C exception, Boundaries testable from outside, Compositing, doomgeneric, Pitch / stride, See it running - the screenshots (+8 more)

### Community 64 - "State"
Cohesion: 0.14
Nodes (16): The resource bug that appears on the Nth try, Per-thread capability table (32 slots), diskfs, get-and-run-kosmos.sh, tools/kfs.lua (host-side disk tooling), The journal, kfs (the filesystem format), make stress (+8 more)

### Community 65 - "Virtio"
Cohesion: 0.23
Nodes (12): read16(), read8(), virtio_ack_interrupt(), virtio_begin(), virtio_fail(), virtio_features(), virtio_notify(), virtio_queue_attach() (+4 more)

### Community 66 - "Run screenshot"
Cohesion: 0.15
Nodes (16): check_clicks(), check_idle(), check_widgets(), check_window_manager(), count_windows(), find_colour_anywhere(), A rectangle of the screen, as bytes, for comparing against itself., How many windows are on screen, counted by their title bars. **Not by rows… (+8 more)

### Community 67 - "Bench"
Cohesion: 0.21
Nodes (9): draw(), finish(), rate(), bench.cleanup(), bench.group_score(), bench.groups_in_order(), bench.now(), bench.report() (+1 more)

### Community 68 - "Con kosmos"
Cohesion: 0.27
Nodes (14): lua_Integer, lua_State, clear_array(), copy_text(), field_u32(), kosmos_console_kit(), l_decode_reply(), l_decode_request() (+6 more)

### Community 69 - "Glossary"
Cohesion: 0.14
Nodes (15): One binary, many roles, BeOS was monolithic, Kosmos Glossary, Hot reload level 1 - removed, Hot reload level 2 - supervised restart, Kit - C that runs inside your own process, A kit is code you run; a server is someone you ask, Library - the same position, in Lua (+7 more)

### Community 70 - "Run tests"
Cohesion: 0.21
Nodes (14): check(), disk_args(), fail(), Failure, main(), parse_tap(), Exception, Pull the plan and the results out of the serial stream. The stream is not pure… (+6 more)

### Community 71 - "Appfs"
Cohesion: 0.20
Nodes (12): kosmos_endpoint_destroy(), kosmos_setname(), main(), named(), say(), l_destroy(), l_setname(), answer() (+4 more)

### Community 72 - "Init"
Cohesion: 0.21
Nodes (12): diskfs_main(), launch(), line(), may_pass_audio(), may_pass_net(), may_pass_screen(), new_namespace(), out() (+4 more)

### Community 73 - "Context"
Cohesion: 0.14
Nodes (12): context, fx, kernel_stack, r12, r13, r14, r15, rbp (+4 more)

### Community 74 - "User"
Cohesion: 0.21
Nodes (7): kmain_x86(), fp_init(), gdt_init(), reload(), rdmsr(), user_init(), wrmsr()

### Community 75 - "Glossary"
Cohesion: 0.16
Nodes (14): The window manager owns every pixel, Direct Graphics Access / BDirectWindow, Client/server architecture, app_server (BeOS), Backbuffer, Blit, Drawing commands (model B), Framebuffer (+6 more)

### Community 76 - "Layout"
Cohesion: 0.15
Nodes (14): BeOS lineage, The BeOS Bible (Scot Hacker, Peachpit Press, 1999), App - graphical, Program - console-based, Replicant, The desktop, from BeOS, 0.6 - the desktop, The block-character wordmark (+6 more)

### Community 77 - "Uart"
Cohesion: 0.21
Nodes (8): pc_capture_memory(), hal_power_off(), out16(), hal_early_init(), hal_getchar(), hal_putchar(), inb(), outb()

### Community 78 - "Virtio"
Cohesion: 0.26
Nodes (9): reg_read(), reg_write(), virtio_ack_interrupt(), virtio_begin(), virtio_fail(), virtio_features(), virtio_notify(), virtio_queue_attach() (+1 more)

### Community 79 - "Net"
Cohesion: 0.27
Nodes (10): hal_blk_read(), hal_blk_write(), request(), hal_net_info(), hal_net_init(), hal_net_recv(), hal_net_send(), offer() (+2 more)

### Community 80 - "Trap"
Cohesion: 0.15
Nodes (12): fault_info, elr, esr, far, handler_sp, trapframe, elr, esr (+4 more)

### Community 81 - "Glossary"
Cohesion: 0.17
Nodes (13): The system is usable from the REPL before the app server exists, Incremental / generational GC, The argument for C is jitter rather than speed, Which language, and why, REPL, What argues for Lua now is the shape of the bug, The audio server taught the rule, A blank canvas you can write applications on, without a toolchain (+5 more)

### Community 82 - "State"
Cohesion: 0.18
Nodes (13): about (About box), arch/aarch64, x86-64 port, devices server (/dev), Kosmos, Lua userland, tools/luacheck.c and luaglobals.py, There are no milestones (there were thirteen) (+5 more)

### Community 83 - "State"
Cohesion: 0.21
Nodes (13): A box model (next), css_computed_font_family NULL write, css_unit_len2device_px swapped arguments, docfont.c - fonts from inside a document, Face pool keyed by family, weight, slant, size, font_short_name weight collision, gfx_draw.h - lending drawing to another kit, IBM Plex outline faces (+5 more)

### Community 84 - "Pci"
Cohesion: 0.32
Nodes (11): address_of(), in32(), out32(), pci_config_read(), pci_config_write(), pci_enable(), pci_find(), read_bars() (+3 more)

### Community 85 - "Syscall"
Cohesion: 0.15
Nodes (12): diskinfo, reserved, sector_size, sectors, netinfo, mac, mtu, present (+4 more)

### Community 86 - "Run network"
Cohesion: 0.23
Nodes (12): _at_once(), boot(), Failure, _fetch(), frames(), main(), Exception, Ask the guest for the same large file `how_many` times at once. **With one… (+4 more)

### Community 87 - "Ui"
Cohesion: 0.21
Nodes (13): apply_fonts(), load_appearance(), theme.apply(), theme.override(), apply_fonts(), dispatch_drop(), dispatch_mouse(), serve_properties() (+5 more)

### Community 88 - "Glossary"
Cohesion: 0.18
Nodes (12): arch/aarch64 - which CPU are you, Drivers still linked into the kernel, hal/qemu-virt - which peripheral do you have, Driver, 16.16 fixed point, Long-descriptor page tables, lua_State, MMU (+4 more)

### Community 89 - "State"
Cohesion: 0.18
Nodes (12): ADDRSPACE_MAX - the pool nothing counted, monitor / sysmon, Intermittent panic in prio_pick_next, Priority inheritance across IPC, procs / process list, sched_prio.c - five priority bands, sched_rr.c - round robin, struct scheduler policy seam (+4 more)

### Community 90 - "Syscall"
Cohesion: 0.17
Nodes (12): proc_info, caps, exit_code, exited, held, id, name, owns (+4 more)

### Community 91 - "Run frames"
Cohesion: 0.30
Nodes (11): collect(), geometry(), main(), A window dragged across the screen while the profile runs. The drag is what a…, Where a window manager pass goes, under three different loads. Kosmos is aiming…, Wait for a report and hand back the lines of it., scenario_animating(), scenario_dragging() (+3 more)

### Community 92 - "About"
Cohesion: 0.18
Nodes (10): fact(), ticker:tick(), field_at(), button(), chrome(), open(), ui.button(), ui.field() (+2 more)

### Community 93 - "Httpd"
Cohesion: 0.35
Nodes (11): content_type(), dotted(), head_for(), note(), publish(), resolve(), respond(), respond_file() (+3 more)

### Community 94 - "Procs"
Cohesion: 0.21
Nodes (8): kind_of(), sampler:tick(), table_view:mouse(), draw_scrollbar(), has_arrows(), thumb_of(), triangle(), ui.scrollbar_mouse()

### Community 95 - "Binfs"
Cohesion: 0.27
Nodes (9): kosmos_receive(), l_receive_raw(), answer(), binfs_server(), copy_word(), declared(), fill_attrs(), find() (+1 more)

### Community 96 - "State"
Cohesion: 0.20
Nodes (11): Attributes and the index, fetch, fs.poll - the select this system wanted six times, Live queries, net server (Ethernet/ARP/IPv4/ICMP), network app, /kits/network, SSH primitives (+3 more)

### Community 97 - "Fwcfg"
Cohesion: 0.36
Nodes (8): fwcfg_dma(), fwcfg_entry(), fwcfg_find(), fwcfg_present(), fwcfg_read(), fwcfg_write(), hal_boot_option(), hal_fb_init()

### Community 98 - "Pc"
Cohesion: 0.18
Nodes (11): multiboot_info, boot_device, cmdline, flags, mem_lower, mem_upper, mmap_addr, mmap_length (+3 more)

### Community 99 - "Timer"
Cohesion: 0.29
Nodes (7): gic_enable_ppi(), arm_next(), hal_timer_init(), read_cntfrq(), read_cntpct(), set_deadline(), timer_interrupt()

### Community 100 - "Ipc"
Cohesion: 0.20
Nodes (10): cap_t, memobj, message, cap_plus_one, data, message_get_cap(), length, message_set_cap() (+2 more)

### Community 101 - "Calc"
Cohesion: 0.40
Nodes (10): apply(), as_number(), clear(), digit(), operator(), present(), press(), refresh() (+2 more)

### Community 102 - "Inflate"
Cohesion: 0.31
Nodes (10): kosmos_unmap(), l_free(), l_gc(), lua_State, kosmos_compress_kit(), l_inflate(), l_inflate_into(), l_inflated_size() (+2 more)

### Community 103 - "Beos"
Cohesion: 0.27
Nodes (10): BFS: a filesystem shaped like a database, Entity files (BeOS People files), The Giampaolo interview, The three always-indexed attributes, Attributes, diskfs is the exception, and not for language reasons, Live query, Get Info, and editing attributes from the desktop (+2 more)

### Community 104 - "State"
Cohesion: 0.22
Nodes (10): /kits/console, console server, Doubled lists that nothing checks agree, /kits/gl, httpd, Slice-based edits silently delete code, Terminal, tests/tests.c (+2 more)

### Community 105 - "Mkpattern"
Cohesion: 0.27
Nodes (9): chunk(), filtered(), main(), pixel(), The test pattern in assets/images/, generated. Deliberately awkward, because a…, A blue disc on a checkerboard, with one translucent corner., One scanline, encoded with PNG filter `kind`., The screendump, as something a person can open. (+1 more)

### Community 106 - "Reader"
Cohesion: 0.27
Nodes (5): load(), relayout(), inline(), markdown.parse(), markdown.wrap()

### Community 107 - "Png"
Cohesion: 0.31
Nodes (9): kosmos_map(), l_new(), be32(), lua_State, kosmos_png_open(), l_png(), map_pages(), paeth() (+1 more)

### Community 108 - "Gfx"
Cohesion: 0.29
Nodes (10): draw_outline_text(), face_at(), gfx_draw_ascent(), gfx_draw_text(), glyph_for(), glyph_of(), l_text(), text_width() (+2 more)

### Community 109 - "Pdftok"
Cohesion: 0.42
Nodes (9): lua_State, hex_string(), hex_value(), is_delim(), is_space(), kosmos_pdf_kit(), l_pdf_scan(), literal() (+1 more)

### Community 110 - "Smp"
Cohesion: 0.25
Nodes (9): IPI - inter-processor interrupt, Memory barrier, TLB shootdown, Weak memory model, Do SMP on AArch64 first, The seven-step order, IPIs for preempting a remote core, Per-CPU runqueues behind the scheduler vtable (+1 more)

### Community 111 - "Virtio"
Cohesion: 0.22
Nodes (9): virtio_device, base, config, features, index, isr, notify, notify_mul (+1 more)

### Community 112 - "Syscall"
Cohesion: 0.22
Nodes (9): pointer_info, buttons, max_x, max_y, min_x, min_y, moved, x (+1 more)

### Community 113 - "Progs2c"
Cohesion: 0.31
Nodes (8): c_quote(), c_string(), main(), quote(), The programs in user/bin/, as one Lua table. There is no disk. A program has to…, Lua long-bracket, with enough equals signs to be unambiguous. A program may…, A C string literal, split a line at a time so the file stays readable., One C string literal, split across lines so nothing is unreadable.

### Community 114 - "Run bench"
Cohesion: 0.36
Nodes (8): compare(), load_baselines(), main(), Write the current numbers as the new baselines. By hand and never…, Host-side benchmark runner. Boots the benchmark image under QEMU, reads the…, Boot the image and return {name: (per_op, iterations)}., record(), run()

### Community 115 - "Edit"
Cohesion: 0.39
Nodes (8): backspace(), clamp(), draw(), insert(), key(), save(), scroll_into_view(), split_line()

### Community 116 - "Gfx"
Cohesion: 0.32
Nodes (8): The drawing model: commands, not a shared buffer, No GPU: the framebuffer is enough, What a Lua/C crossing costs, Doom, the ideal shape, gfxbench, The shared-memory pixel path, The direct window (direct = true), Draw produces a list of commands

### Community 117 - "State"
Cohesion: 0.36
Nodes (8): audioring.h - SPSC period ring, Control by message, data by shared memory, edit (screen editor), Hot reload (removed), ramfs (/data), Read-only image mapped rather than copied, Single-producer single-consumer ring, tcpring.h - two SPSC rings in a shared region

### Community 118 - "State"
Cohesion: 0.25
Nodes (8): Doom, doomgeneric (vendored), libdom (vendored), libhubbub (vendored), libparserutils (vendored), libwapcaplet (vendored), Binned, growing malloc, /kits/web

### Community 119 - "Rtc"
Cohesion: 0.54
Nodes (7): cmos(), days_from_civil(), from_bcd(), hal_rtc_seconds(), read_once(), same(), updating()

### Community 120 - "Gic"
Cohesion: 0.32
Nodes (6): gic_acknowledge(), gic_enable_spi(), gic_end_of_interrupt(), hal_irq_handle(), virtio_enable_interrupt(), snd_interrupt()

### Community 122 - "Run power"
Cohesion: 0.39
Nodes (7): args(), boot(), Failure, main(), Exception, Cuts the power in the middle of writing, and checks what came back. This is…, Boots, types, and either waits or is killed part way through.

### Community 123 - "Terminal"
Cohesion: 0.43
Nodes (5): emit(), launch(), serve_console(), win:on_frame(), win:on_key()

### Community 124 - "Mp3 kosmos"
Cohesion: 0.50
Nodes (7): lua_State, check(), kosmos_mp3_kit(), l_decode(), l_decoder(), l_probe(), l_reset()

### Community 125 - "Setup"
Cohesion: 0.29
Nodes (7): Context switch, ESR / ELR / FAR, One kernel check failed once, What to do when you get stuck, The exception handler that prints ESR, ELR and FAR, GDB against QEMU, Debugging without a debugger, over the UART

### Community 126 - "Syscall"
Cohesion: 0.29
Nodes (7): schedinfo, name, policies, policy, priorities, quantum, tick_hz

### Community 127 - "Bdf2c"
Cohesion: 0.38
Nodes (6): main(), parse(), provenance(), A BDF bitmap font, as a C array. BDF is the interchange format every bitmap…, Every glyph in the file, as {codepoint: [row bytes]}. BDF puts the width in…, The COMMENT block at the top, which is where the font says who it is.

### Community 128 - "Network"
Cohesion: 0.43
Nodes (5): apply(), collect(), device:draw(), mac_text(), to_bytes()

### Community 129 - "Audioring"
Cohesion: 0.29
Nodes (7): audio_ring, magic, period_bytes, periods, read, reserved, write

### Community 132 - "State"
Cohesion: 0.40
Nodes (6): appfs (/app registry), /bin reported 74 of its 82 programs, binfs (/bin), Declared shape (struct protocols), Deskbar, Scripting architecture (/app)

### Community 133 - "Syscall"
Cohesion: 0.33
Nodes (6): screen_info, address, height, pitch, reserved, width

### Community 134 - "Run screenshot"
Cohesion: 0.33
Nodes (5): device(), extra_args(), machine(), What QEMU calls a virtio device on this machine. The same hardware under two…, Append device arguments to whichever board's list is in force.

### Community 135 - "Run screenshot"
Cohesion: 0.33
Nodes (6): is_tab(), Whether the pixel at byte offset `at` belongs to a tab of `base`., How wide the widest run of tab colour on screen is, in pixels. It used to be…, The first row holding the focused window's decoration, or None. The top of the…, tab_top(), tab_width()

### Community 136 - "Appearance"
Cohesion: 0.47
Nodes (3): reflect(), role(), send()

### Community 139 - "Fwcfg port"
Cohesion: 0.60
Nodes (4): fwcfg_reg_read(), fwcfg_reg_write(), in32(), out32()

### Community 140 - "Pc"
Cohesion: 0.40
Nodes (5): multiboot_mmap, base, length, size, type

### Community 141 - "Virtio"
Cohesion: 0.40
Nodes (5): vring_desc, addr, flags, len, next

### Community 142 - "Assets2c"
Cohesion: 0.50
Nodes (4): licence_for(), main(), The licence file sitting beside a vendored file, if there is one. The rule in…, The files in assets/images/, as one C table. Binary, so unlike programs and…

### Community 143 - "Kernel size"
Cohesion: 0.50
Nodes (4): main(), measure(), How big is the kernel? Reported, not enforced. `CLAUDE.md` is explicit that the…, (total lines, code lines) for one file. Block comments are tracked across…

### Community 144 - "Luaglobals"
Cohesion: 0.60
Nodes (4): check(), environment_for(), main(), Every global the Lua in this repository reads, checked against what will…

### Community 145 - "Run web"
Cohesion: 0.50
Nodes (4): Failure, main(), Exception, The NetSurf libraries, running on the machine. Five libraries compiling and…

### Community 146 - "Run x86"
Cohesion: 0.50
Nodes (4): boot(), main(), Boots Kosmos on x86-64 and checks that it is the same system. This replaced a…, Boots, optionally types at the prompt, and returns everything printed. One line…

### Community 147 - "Frames"
Cohesion: 0.60
Nodes (3): ms(), report(), us()

### Community 148 - "Paint"
Cohesion: 0.60
Nodes (4): draw_palette(), present(), stroke(), touch()

### Community 149 - "Glossary"
Cohesion: 0.67
Nodes (4): Big-endian / little-endian, Open Firmware, The big-endian audit, PowerPC, on a G5 or a G4 iMac

### Community 150 - "Power"
Cohesion: 0.83
Nodes (3): hal_power_off(), hal_restart(), psci()

### Community 151 - "Run stress"
Cohesion: 0.67
Nodes (3): main(), Runs the machine hard for a while and asks whether it gave everything back.…, run()

### Community 153 - "Sysmon"
Cohesion: 0.67
Nodes (3): add(), meter(), sampler:tick()

### Community 155 - "Architecture"
Cohesion: 1.00
Nodes (3): No POSIX personality at system level, BeOS was POSIX-compatible; Kosmos is not, What it is not: Unix, Windows, macOS, AmigaOS, a BeOS clone

### Community 156 - "Setup"
Cohesion: 0.67
Nodes (3): longjmp, setjmp/longjmp demands the most care, Never edit lua/upstream directly

### Community 158 - "Virtio"
Cohesion: 0.67
Nodes (3): vring_used_elem, id, len

## Ambiguous Edges - Review These
- `Lazy FP/SIMD save` → `Input delivered while the guest is busy (harness flakiness)`  [AMBIGUOUS]
  docs/state.md · relation: conceptually_related_to
- `The C/Lua language split` → `Double buffering and an explicit commit`  [AMBIGUOUS]
  docs/gfx.md · relation: references
- `The HAL interface, minimal on purpose` → `hal_fb_flush, the entry virtio-gpu will add`  [AMBIGUOUS]
  docs/hal.md · relation: references

## Knowledge Gaps
- **345 isolated node(s):** `thread`, `memobj`, `tag`, `cap_plus_one`, `length` (+340 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 797 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **21 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `Lazy FP/SIMD save` and `Input delivered while the guest is busy (harness flakiness)`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **What is the exact relationship between `The C/Lua language split` and `Double buffering and an explicit commit`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **What is the exact relationship between `The HAL interface, minimal on purpose` and `hal_fb_flush, the entry virtio-gpu will add`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **Why does `sys.ticks()` connect `Browser Application` to `Filesystem Format (kfs)`, `Testing`, `Bench`, `Window Manager`, `Tracker File Manager`, `Ui`, `Sysmon`, `Pdfview`, `About`?**
  _High betweenness centrality (0.030) - this node is a cross-community bridge._
- **Why does `timeit()` connect `Testing` to `Browser Application`?**
  _High betweenness centrality (0.017) - this node is a cross-community bridge._
- **Why does `The blind spot a benchmark suite has by construction` connect `Testing` to `Gfx`, `Design Principles`?**
  _High betweenness centrality (0.017) - this node is a cross-community bridge._
- **Are the 40 inferred relationships involving `syscall_dispatch()` (e.g. with `trap_handler()` and `hal_boot_option()`) actually correct?**
  _`syscall_dispatch()` has 40 INFERRED edges - model-reasoned connections that need verification._