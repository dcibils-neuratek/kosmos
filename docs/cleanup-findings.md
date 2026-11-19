<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# The review before 0.12.0: every finding

The eight readers' reports of 18 November 2026, numbered as `cleanup.md`'s batches cite them. Each says its kind, its risk, where, what, and the action proposed. A finding is a claim until its batch checks it.

```

##### documentation truth - CLAUDE.md, README.md, docs/*
[1] stale-doc low docs/state.md:5 ~40L
  state.md is current to 0.11.492; VERSION is 0.11.507. Revisions 0.11.493-0.11.507 are not recorded: listen, x86-film in the audio band, x86-profile, the futex deadline, desktop style, S4 roll and focus sweep, Save page as, JetSki's menu, History, JetSki in files, the refused first touch, flat title bars, the dock's ghost pin. It
  -> rewrite: add the 0.11.493-0.11.507 entries and a new Next before 0.12.0
[2] stale-doc low docs/state.md:5 ~12000L
  The 'Last updated' line is one paragraph of about 10,000 characters that lists every revision back to 0.11.393. It contradicts itself: it says '0.11.487-0.11.489, pushed' near the start and ends with 'nothing since 0.11.363 pushed'. state.md is 13,102 lines, the same size as the roadmap, so it no longer works as 'where it is thi
  -> rewrite: one short Last-updated line, older sections moved to a history file or dropped (testing.md and the logbook already hold them)
[3] stale-doc low CLAUDE.md:234 ~20L
  Build says '`make qemu` is the whole system, at 1920x1080: the browser, the network, every demo', and line 222 says 'make FULL=0 qemu # without the browser and FFmpeg'. `make qemu` is the AArch64 virt machine. The old browser was removed in 0.11.461, and JetSki's webkit.elf is built only for x86_64, so `make qemu` has no browser
  -> rewrite the Build block: say which machine each target is, add the x86 targets, and say what FULL is now
[4] stale-doc low CLAUDE.md:97 ~6L
  'Active target today: QEMU virt aarch64, and nothing else. Real hardware is on the wishlist and the serial cable is not here yet.' Line 101 says 'AArch64 today, x86-64 later'. In fact x86-64 has its own arch/ and hal/pc/, the gate runs x86 only (Diego, 8 October, in the same file), and Kosmos runs on a ThinkPad T14, an M700 and 
  -> rewrite the paragraph to match the targets as they stand
[5] stale-doc low CLAUDE.md:1159 ~15L
  Layout is stale in several places. kits/ lists 14 kits, but user/kits has about 30 production kits (audio, debug, egl, llm, mail, map, media, playback, smb, storage, synth, tcc, web, webkit, window, ...) plus test kits. drivers/ lists 'net/ usb/ display/ power/' and leaves out storage/ and bluetooth/. installed/ names 'Doom/, Qu
  -> rewrite the Layout block from ls
[6] stale-doc low CLAUDE.md:576 ~4L
  'Freestanding changes go in lua/patches/ and are applied during the build.' There is no lua/patches/ directory. The Makefile says the -include of lua/kosmos/kosmos_lua.h is what lets upstream stay untouched 'and lua/patches/ stays empty'. docs/setup.md:165 and docs/layout.md ('changes are patches applied at build time') say the 
  -> rewrite comment: changes are a forced -include of lua/kosmos/kosmos_lua.h; fix setup.md:165 and layout.md too
[7] stale-doc low CLAUDE.md:783 ~30L
  The HAL block calls itself 'The HAL interface as it actually stands' but names 26 functions. hal/hal.h declares about 82, including the hal_blk_*, hal_net_*, hal_snd_*, hal_*_describe, hal_power_off, hal_restart, hal_rtc_seconds, hal_battery_read, hal_firmware_table and hal_machine_ident families.
  -> rewrite: either list it all or call it 'a selection' and point to hal.h
[8] stale-doc low CLAUDE.md:1009 ~10L
  'Test what changed' says the whole gate, `make test`, runs when a shared piece changed, and line 1024 says `make test LIBS=1` runs every library suite. Since 14 November gate.py runs each suite only when what it watches changed, so `make test` is already selective. `ALL=1` is the flag, and LIBS=1 is 'its older name'. prepush use
  -> rewrite both paragraphs to describe the fingerprinted gate and ALL=1
[9] stale-doc low CLAUDE.md:918 ~2L
  'That /Home is made fresh from ~/Kosmos/home at 512 MB (HOME_DIR, STICK_HOME_MB ...)'. The default is now 2048.
  -> rewrite comment: 2048 MB, and when it changed
[10] stale-doc low CLAUDE.md:467 ~1L
  'Not borrowed: 256 priority levels, where six say everything this system has to say.' The scheduler has seven named levels now (IDLE, LOW, NORMAL, GAME, DISPLAY, AUDIO, INPUT) in SCHED_PRIORITIES 8 slots, since game mode added GAME.
  -> rewrite comment: seven
[11] stale-doc low CLAUDE.md:587 ~1L
  *app or program* says an app is 'marked `-- kosmos: application` on its first line'. The licence section of the same file says the marker may sit anywhere in the opening comment block, because the copyright line now comes first.
  -> rewrite comment: 'in its opening comment'
[12] stale-doc low CLAUDE.md:119 ~15L
  The document map's status labels are behind the documents they point to. Pulses: 'agreed, to be built', but P1 and P2 were built in 0.11.227 and sys.pulse exists. Playback Kit: 'to be built', but it is built (state 0.11.422). Pewter: 'to be built', but P1-P4 were built on 8 November. Mail on WebKit: 'to be built', but W1 and W2 
  -> rewrite each map line's status
[13] stale-doc low docs/pulses.md:3 ~2L
  The header still says 'Designed 14 October 2026. Nothing here is built.' Line 130 of the same file records P1 built in 0.11.227 along with P2's runtime half, and tracker.lua uses pulses.
  -> rewrite comment in the header to say what is built
[14] stale-doc low docs/mailweb.md:2 ~2L
  The title is 'Mail's HTML on WebKit, before it is built', but W1 (one WebKit image, 0.11.435) and W2 (views, 0.11.436) are built. adblock.md:5 has the same problem: 'Designed ... before it is built', while B1-B3 are built.
  -> rewrite comment: say which steps are built
[15] stale-doc low README.md:228 ~30L
  README's Layout block is a tree of September. It says bin/apps has '53' apps (there are 69) and programs '64' (there are 105). It says 'doom/ quake/ snes/ litexl/ browser/ each own the C': Doom, Quake and SNES live in user/installed/, and Lite XL and the browser are removed. It lists lib 'kfs' (removed in diskfs step 4) and 'ser
  -> rewrite the block, or replace it with a pointer to CLAUDE.md/layout.md so there is one copy
[16] stale-doc low README.md:176 ~8L
  The 'How to read this source tree' table gives use("/kits/pdf") and use("/lib/ui.lua"). The code uses /Kosmos/Kits and /Kosmos/Libraries. Lines 197-199 put doom_kosmos.c 'in user/bin/apps/doom/' and name 'Lite XL and the browser' as apps with their own C.
  -> rewrite comment: the current namespace paths and directories
[17] stale-doc low README.md:99 ~10L
  'What it is NOT' argues that a modern browser is '30 million lines that assume POSIX, threads, a JIT, a GPU' and that porting one is more than the whole system. It also forbids 'BSD sockets'. WebKit with JavaScriptCore's JIT is now ported as JetSki. CLAUDE.md (11 November) allows process-private socket() and POSIX names in runti
  -> rewrite those two sections to the current position, keeping the old one as history
[18] stale-doc low README.md:159 ~4L
  'Building it' says `make qemu` is 'the whole system at 1920x1080 - browser, Doom, network, every demo'. Doom is not in it: it is installed into /Home/Apps by `make install-apps`, as CLAUDE.md says. On AArch64 there is no browser since 0.11.461. The 'Run it' examples (line 49, `-b "wm browser"`) only work against the old released
  -> rewrite comment
[19] stale-doc low docs/architecture.md:677 ~25L
  Four bullets in the 'still missing' list are now false. 'Every message blocks ... SYS_CALL, SYS_RECEIVE and SYS_REPLY are the whole IPC surface': SYS_PULSE exists. 'An address is four numbers. There is no resolver ... there are no sockets': there is a resolver (NET_OP_RESOLVE) and POSIX sockets. 'The screen is one framebuffer wi
  -> rewrite comment: drop or rewrite the four bullets
[20] stale-doc low docs/layout.md:26 ~8L
  Its repository table is stale. user/lib lists 'ui.lua, kfs.lua, theme.lua, zip.lua', and kfs.lua is gone. runtime/upstream lists 'puff', which was removed on 5 October. user/init is described as 'init, the servers, the shell', but the servers are C in user/servers. Only arch/aarch64/ and hal/qemu-virt/ are listed. Lines 166-168 
  -> rewrite those rows
[21] stale-doc low docs/design.md:1057 ~15L
  §8.3d says '`user/lib/blockcache.lua` wraps sys.disk_read...' in the disk server. blockcache.lua was deleted in diskfs step 4. The cache is now diskcache.c in the C server. §'What would change this decision' (line ~798) still proposes building kfs.store both ways and records 'the prediction on record: fifteen percent'. That expe
  -> rewrite comment: point to diskcache.c and record the outcome
[22] stale-doc low docs/hal.md:343 ~1L
  'Two consequences worth knowing before touching hal/qemu-virt/input.c'. That file was deleted. The virtio-input code that does the EV_BITS/EV_ABS probing is in hal/virtio/input.c, and hal/qemu-virt now has input_bind.c and input_describe.c.
  -> rewrite comment: hal/virtio/input.c
[23] stale-doc low docs/usb.md:1834 ~3L
  'kfs volumes are listed and never opened, because kfs's reader is user/lib/kfs.lua and this server is C - reading one here means kfs in C'. user/lib/kfs.lua is gone, and kfs has been in C since 29 September (user/servers/kfs.c). The stated reason no longer holds. docs/diskfs.md:22,130 also name user/lib/kfs.lua, but as history i
  -> rewrite comment: say what still keeps drives from opening a kfs volume, if anything
[24] stale-doc low docs/linux.md:130 ~4L
  L1a says `tools/sandboxes_drive.py` gives a stick in use its /Sandboxes partition (also at line 138, and in roadmap.md:763 and :9920). The tool was deleted in 0.11.322. The M700's /Sandboxes is now on SATA.
  -> rewrite comment: say what replaced it, or mark it as history
[25] stale-doc low docs/playback.md:116 ~1L
  The architecture table says the resampler 'moved from sys_user.c to user/lib/c/pcm.c, one copy that sys.pcm and this kit both call'. user/lib/c/ does not exist. The one copy is the header user/include/pcmconvert.h, which sys_user.c and playback_core.c include. The CLAUDE.md map still calls this kit 'to be built'.
  -> rewrite comment: user/include/pcmconvert.h
[26] stale-doc low docs/roadmap.md:873 ~3L
  An open item says 'The browser's keys move with it: its tabs are on Super today - Super W closes one ... (tools/run_browser.py) - and become Control's'. That browser and run_browser.py were removed in 0.11.461, so this part of the item now concerns JetSki or nothing.
  -> rewrite comment: restate for JetSki or drop the clause
[27] stale-doc low docs/setup.md:165 ~2L
  The 'Upstream Lua' tree diagram lists 'lua/patches/ the freestanding changes, applied during the build'. The directory does not exist (the same claim as CLAUDE.md:576).
  -> rewrite comment
[28] stale-doc low CLAUDE.md:1005 ~2L
  *A push carries a picture* says `make shot` saves the screenshot 'under the date and the revision'. The filename is the date plus the git short hash, not the version. The same paragraph says prepush runs 'the suites, the display harness, a MEGA link and make shot', but prepush is now `ALL=1 test`, `mega` and `shot`.
  -> rewrite comment
[29] stale-doc low book/logbook.md:4446 ~60L
  The logbook's last entry is '18 November 2026 - Liquid Glass' (up to 0.11.486). Per the 'explain in plain terms' practice every major development goes here, but Starlight S3/S4, Save page as, JetSki's menu and History (0.11.487-0.11.507) are missing.
  -> rewrite comment: add the missing entries before 0.12.0
[30] dead-file low TEST=1: ~0L
  At the repository root are an empty directory named `TEST=1`, an empty directory `apps-web-ffmpeg`, and `webkit_kit.d`, a dependency file whose source is in a scratchpad. All three are stray build leftovers sitting beside arch/ and kernel/. CLAUDE.md says everything the build generates goes under build/. They are untracked: git 
  -> remove (Diego's call; untracked), and find the make invocation that wrote `TEST=1` as a directory
[31] stale-doc low docs: ~11L
  Eleven docs are not reached from either CLAUDE.md's map or README's: diskmonitor.md, gpumonitor.md, graph.md, keyring.md, machine.md, mail.md, music.md, netmonitor.md, present.md, rendering.md, sheets.md. Some describe built applications (Mail, Keyring) and some plans (present, sheets), so a reader cannot tell which is which.
  -> add them to the map with a status, or mark them as history

##### Applications: user/bin/apps/ (except deskbar) and 
[32] bug low user/bin/apps/menubar.lua:2 ~1L
  Pewter's menu bar declares `kosmos: application` but no `kosmos: section`. binfs's menu_path defaults a missing section to "applications", and deskbarmenu.programs excludes only "deskbar" and `section none`. So "Menu bar" appears in the Deskbar's Applications menu and the launcher pad, and a press starts a second top strip.
  -> fix: add `-- kosmos: section none` to menubar.lua, as desktop.lua, chooser.lua and launchpad.lua have
[33] bug low user/bin/apps/video.lua:6 ~2L
  The header declares `kosmos: opens mp4`, but the player accepts mp4, m4v and mov (FILMS at line 76). filetypes.table builds what-opens-what only from headers, so Tracker never offers Video Player for a .mov or .m4v that it can play. play.lua also declares only mp4.
  -> fix: `-- kosmos: opens mp4 m4v mov` (and the same in play.lua if it plays them)
[34] other low user/bin/apps/video.lua:98 ~4L
  `local FILMS` is declared twice. At line 76 it is the extension set; at line 98 it is redeclared as the Open window's start folder. is_film keeps the first through its upvalue, so it works, but every later `FILMS` means a path. A reader of is_film's caller sees `start = FILMS, filter = is_film` and has to work out that the two F
  -> rewrite: rename one, e.g. FILM_TYPES and FILMS_FOLDER
[35] dead-code low user/bin/apps/mail.lua:1303 ~11L
  `local function unread_total()` sums each account's inbox unseen count, and nothing calls it.
  -> Remove
[36] dead-code low user/bin/apps/machine.lua:457 ~1L
  `local VENDORS = hardware.VENDORS` is never read; only CLASSES and VIRTIO are used.
  -> Remove the line (and 'The tables are...' stays true for the other two)
[37] dead-code low user/bin/apps/tracker.lua:916 ~1L
  `local columns_keep = { per_folder = false }   -- prefs: where widths are kept` is never read; per-folder columns are handled by columns_from.
  -> Remove
[38] dead-code low user/bin/apps/tracker.lua:2913 ~1L
  `local server_label, share_name = netshares.under(path)`: server_label is never used, only share_name.
  -> `local _, share_name = ...`
[39] stale-comment low user/bin/apps/gallery.lua:38 ~24L
  `local theme = ui.theme` sits under a seven-line comment explaining why an app must hold the kit's palette and not a copy. But gallery.lua never reads `theme`. logview.lua:59 and terminal.lua:74 have the same block, word for word, also with `theme` unused. The comment describes a use that is not there.
  -> Remove the local and its comment in the three files
[40] stale-comment low user/installed/JetSki/jetski.lua:849 ~4L
  The comment reads "in the old browser's words (`browser.lua`, `how_came`)", but browser.lua was deleted in 0.11.461. user/installed/JetSki/settings.lua:8-9 also cites "the old browser's (`browser.lua`, d5)".
  -> rewrite comment: say what the padlock and Settings are, without the dead file
[41] stale-comment low user/installed/JetSki/jetski.lua:42 ~3L
  "A window that draws its own pixels ... as the NetSurf browser's is" and line 1357 "as the NetSurf browser's field has them" both compare JetSki to a browser that no longer exists.
  -> rewrite comment
[42] stale-comment low user/installed/JetSki/jetski.lua:37 ~2L
  The comment says the WebKit image is "Kosmos's WebKit ..., which Mail runs in too". Mail does not run in it yet: mail.lua has no `-- kosmos: image webkit` and still draws HTML with the NetSurf web kit. CLAUDE.md lists mailweb as "agreed, to be built".
  -> rewrite comment: 'which Mail is to run in (docs/mailweb.md)'
[43] stale-comment low user/bin/apps/mail.lua:67 ~7L
  Mail calls the NetSurf kit "the browser's engine" and says it is set up "as the browser sets it up" (lines 31, 60, 67, 73, 730). Line 97 says "The status bar along the bottom, as the browser's (`STAT` there too)". The browser that used that engine is gone; Mail is the kit's only user. Line 73 prints "mail: the browser's engine w
  -> rewrite comments and the print: 'the web kit (NetSurf)'
[44] dead-code medium user/lib/browserprefs.lua:61 ~30L
  These are JetSki's settings, but several are now read by nothing. prefs.agent(), the AGENTS table (Lynx/NetSurf/3DS user agents) and the agent and agent_words defaults belonged to the NetSurf browser. The `bar` and `costs` defaults have no reader either. JetSki's Settings page shows only home, opens, zoom, images, javascript and
  -> Remove agent/AGENTS/agent_words/bar/costs and their test checks (tools/test_browserprefs.lua:114-131), or wire them into JetSki if wanted
[45] duplicate low user/bin/apps/netmon.lua:61 ~30L
  netmon.lua:61-81 and diskmon.lua:58-74 hold the same `scaled`, `rate` (bits or bytes) and `amount` functions, line for line. files.size (user/lib/files.lua:302) is the system's byte formatter and rounds slightly differently. Two monitors print rates two ways, and a third copy is one edit away.
  -> merge into one kit function (files.size gaining a rate/bits form, or ui's monitor helpers) and use it from both
[46] duplicate low user/installed/JetSki/downloads.lua:28 ~7L
  Downloads defines its own `amount(n)` (bytes/KB/MB/GB with its own rounding, "%d bytes") rather than files.size, which JetSki already has through J.files. A download's size then reads differently in JetSki and in Tracker.
  -> merge into files.size
[47] duplicate low user/bin/apps/gpumon.lua:222 ~30L
  `local function heading(g, x, y, left, right[, colour])` is the same body in four monitors: gpumon.lua:222, llmmon.lua:238, netmon.lua:276 and diskmon.lua:413 (diskmon has INNER for M.inner). It draws a group's heading at the left and a dim value at the right, which is a kit widget.
  -> merge into ui.lua beside ui.history / ui.meter_card
[48] other low user/bin/apps/pdfview.lua:1 ~3L
  pdfview.lua, scheduler.lua and writer.lua open with `-- kosmos: application` on line 1 and have no copyright line. CLAUDE.md requires every file the project writes to open with it, above the descriptive comment and below nothing.
  -> fix: add the copyright line above the header (the header block rule still finds `kosmos:` lines below it)
[49] stale-comment low user/lib/deskbarmenu.lua:230 ~2L
  "each filed by its header's `section` - Doom under Demos" (and filetypes.lua:497 "`declared(source, "section")` is `demos` for Doom"). doom.lua's header says `kosmos: section applications`.
  -> rewrite both comments (e.g. 'Doom under Applications', or use a GL demo as the example)
[50] other low user/installed/Quake/quake.lua:9 ~1L
  Quake takes a pak path as its argument (`wm quake:/Temporary/pak0.pak`) but declares no `kosmos: opens`. Doom declares `opens wad` and SNES `opens sfc smc`, so a .pak in Tracker opens with nothing.
  -> add `-- kosmos: opens pak` if a pak opened from Tracker should start Quake
[51] stale-comment low user/bin/apps/desktop.lua:11 ~2L
  "the Deskbar and the Startup panel both list what is in `/bin` marked as an application" is written in the present tense. startup.lua (the Startup panel) was removed in 0.11.5, and `/bin` is /Kosmos/Apps now. Line 31 already puts it in the past tense.
  -> rewrite comment
[52] stale-comment low user/bin/apps/about.lua:250 ~1L
  The section header "Uptime, which is the only thing on this window that changes" sits above a ticker that also rewrites the Memory fact every tick (line 269).
  -> rewrite comment: 'Uptime and free memory, the two things that change'
[53] wasted-work low user/bin/apps/connect.lua:270 ~10L
  `body:draw` reads `/Devices/clock` on every repaint to word the recent connections' dates. It also calls ui.wrapped and netshares.split, allocating fresh line tables each paint. This is a server round trip inside a repaint, which the 'a window draws what it has already heard' rule forbids.
  -> fix: read the clock on the window's tick (or when `recent` changes), keep the words, draw them
[54] wasted-work low user/bin/apps/adopt.lua:39 ~8L
  It waits up to five seconds for the replicant by spinning `fs.read(...)` with `sys.yield()` in a loop: a busy poll of ramfs at full speed. That goes against 'no loop sleeps for nought while it waits for a time'. It is a dev fixture (Adopt Test, driven by run_screenshot's replicants phase), so the cost is small.
  -> fix: wait on the window's clock (ui tick) or a short sleep between reads
[55] other low user/bin/apps/paint.lua:5 ~1L
  Paint is filed under Applications, but it opens nothing, saves nothing and takes no file. Its own header calls it the worked example of gfx.md 19.7's brush-stroke path. It is a demo listed beside real applications.
  -> Move to `section demos` (or give it Open and Save), Diego's call

##### Astra - the window manager (wm.lua, user/lib/wm/),
[56] wasted-work medium user/bin/programs/wm.lua:3985 ~30L
  Every window opened gets `surface = gfx.surface{ w = w_, h = h_, shared = true }`, filled with 0xff202020 at 4008, even when a region came with the open (every kit window, menu, hello-win, stuck since D2e). That surface is shown only when `win.shared` is nil (describe 3038, 2455), so for region windows it is w*h*4 bytes of share
  -> fix: make win.surface only when no region was mapped (or make wmlatency hand over a region and remove the fallback, swap_surface's non-shared branch at 3507-3535 with it)
[57] dead-code low user/lib/ui.lua:1439 ~35L
  ui.splitter: a split-pane grip widget with no caller.
  -> Remove
[58] dead-code low user/lib/ui.lua:10571 ~4L
  ui.undrag(win), sending a `drag` with no kind, is never called.
  -> Remove
[59] dead-code low user/lib/ui.lua:9983 ~4L
  window:close_menu(), 'the old single-menu names, kept because the menu bar ... still reads better this way' (9942), has no caller; everything calls close_menus.
  -> Remove, and reword the 9942 comment so it covers only the push_menu/open_menu names that are still used
[60] dead-code low user/bin/programs/wm.lua:205 ~22L
  OUT.MARGIN and OUT.TITLE_IN ('the controls from the end of the tab, and the title from its start') are set at 205-206 and rescaled in scale.chrome at 278-279, but nothing reads them. The title on a tab went with one chrome's step 3, and the three lights' place now comes from the header (OUT.lights_at).
  -> Remove both, and the 190-204 comment that explains them
[61] dead-code low user/bin/programs/wm.lua:1915 ~8L
  OUT.PLATINUM_SLOT = { maximise = 0, shade = 1, close = 2 } is never read; the drawing uses SCENE_SLOT (2843) and the press uses OUT.SLOT/OUT.IN_SLOT.
  -> Remove
[62] duplicate low user/bin/programs/wm.lua:2843 ~3L
  SCENE_SLOT = { maximise = 0, minimise = 1, shade = 1, close = 2 } is a second copy of OUT.SLOT (1905). The OUT.SLOT comment says 'One table that the drawing and the press both read, so a box cannot be drawn in one slot and pressed in another', which is no longer true: describe() reads SCENE_SLOT and pointer.lua reads OUT.SLOT/IN
  -> Merge: add shade = 1 to OUT.SLOT and use it in describe
[63] dead-code medium user/lib/notify.lua:133 ~2L
  notify.remove(id) (OP_REMOVE) has no caller. The notify server's NOTIFY_OP_REMOVE branch (user/servers/notify.c:221) is therefore reached by nothing and tested by nothing.
  -> Remove, or keep it and give it a caller and a test; unproven: the C op may be wanted by a future history 'dismiss'
[64] stale-comment low user/lib/wm/keys.lua:38 ~45L
  The prefix comment says 'There are no modifiers to escape into. A virtio keyboard gives Control plus a letter and nothing else - no Alt, no Super', and lists 'Control-W then a/c/x/v' for select-all/copy/cut/paste, with 'The clipboard four are here rather than as four more reserved keys'. Now Super and Alt exist (this same file d
  -> Rewrite the comment: the prefix is now Tab, arrows, Q, C (literal Control-C) and Control-W
[65] stale-comment low user/lib/wm/keys.lua:228 ~4L
  Two comments point to a `CLIP_KEYS` table ('for the reason `CLIP_KEYS` above gives' at 228; 'the four clipboard keys already live in `CLIP_KEYS`' at 478). No such table exists; it is EDIT_KEYS/EDIT_BINDINGS in wm.lua. Line 950 also cites 'as Lite XL did', and Lite XL was removed from the tree on 26 September.
  -> Rewrite the comments to name EDIT_KEYS, and drop the Lite XL clause
[66] stale-comment low user/bin/programs/wm.lua:39 ~18L
  The file's main 'why a separate process' comment says an application 'sends a list of drawing commands - `ui.md` 16.6, commands and not a shared buffer - and this process renders them into a surface it owns and keeps', and that 'Its window still has its last contents, because those contents were never in its address space'. Sinc
  -> Rewrite to the current model: the app's region plus the window server's composing, and what a hung application keeps (its last committed buffer)
[67] stale-comment low user/bin/programs/wm.lua:3818 ~10L
  'Everything else here sends drawing *commands* and this process owns every pixel ... So an application may hand over a region instead' - presents the region as the exception when it is now the only way.
  -> Rewrite the comment
[68] stale-comment low user/bin/programs/wm.lua:1786 ~3L
  OUT.wants_head: 'A window that draws no header of its own - one sending commands, as `hello-win` teaches - has its top rows for its title bar'. hello-win now draws its own pixels into a region (its own header comment says so since D2).
  -> Rewrite the comment
[69] stale-comment low user/bin/programs/wm.lua:921 ~1L
  step()'s trace comment: 'what survives is the last few hundred passes before you pressed Control-C'. Control-C is copy now; ending the desktop is Control-W then Q.
  -> Rewrite the comment
[70] stale-comment low user/lib/ui.lua:14 ~15L
  The kit's opening comment: 'drawing produces *commands* ... and the list goes to the window manager in a message', and 'a view that has not changed can have its list resent without re-running its handler, and a window's drawing can be logged, replayed, or inspected from the shell'. Since D2e the ops are painted in-process by pai
  -> Rewrite the comment
[71] stale-comment low user/lib/ui.lua:7903 ~15L
  In ui.window: 'The ordinary path sends drawing commands and the compositor owns every pixel ... This is for the cases where that is the wrong trade' about direct=true. And 7920-7923: draws_itself is 'Asked for ... while it is proved, window by window; the window manager declines it at a scale ... and the window sends its drawing
  -> Rewrite both paragraphs
[72] stale-comment medium user/lib/wm/strips.lua:9 ~20L
  The strip comment says the kit 'cannot draw a menu bar into' a direct window ('`window:paint` returns at once for one'), with SNES's File menu as the example; ui.lua:8058 says the same. Since the kit's band (ui.lua:7887-7896) the kit draws a direct window's menu titles itself, and ui.lua:8064 sends `menubar` only when `not band`
  -> Rewrite both comments; consider whether video full screen should use the band too and strips.lua be removed (measure first / confirm)
[73] stale-comment low user/lib/paint.lua:5 ~10L
  paint.lua's header: 'A window that sends drawing ... sends a list of commands ... and the window manager draws them into the window's surface', and 'Now the window manager and `ui.paint_view` replay the same commands'. The window manager no longer loads paint.lua. ui.lua:7749 also calls paint.scale 'the window manager's own arit
  -> Rewrite: the kit paints a window's ops into its own region
[74] stale-comment low user/lib/theme.lua:375 ~3L
  Glass tokens comment: 'What the scene does with them arrives a step at a time (LG2 the blur, LG3 the lens); until then a glass look is its tints said as solid colours.' LG2-LG6 are built (0.11.473-0.11.488).
  -> Rewrite the comment
[75] stale-comment low user/lib/theme.lua:462 ~6L
  theme.metrics: 'Zero for either is a square, hard-edged window ... what a look may still ask for: the metrics are the same table a theme may set, so a look that wants 1995 can have it', and `shadow ... and a look's own, if it says`. theme.read forces `out.corner = theme.CORNER -- every look's, never its own` (649), and shadow is
  -> Rewrite the comment
[76] stale-comment low user/lib/theme.lua:430 ~8L
  '`tab` is the window manager's `TAB_H`, which reads it', and metrics.tab = 26 'a window's title tab'. There is no TAB_H in wm.lua and no tab since one chrome. The metric's only use is the cascade step (wm.lua:147, 280), whose comment at 208-211 still says 'A tab's height, so the one underneath always has a strip of its own title
  -> Rename or recomment it as the cascade step, and fix both comments
[77] stale-comment low user/lib/theme.lua:951 ~12L
  vgradient: 'The window manager composes clipped to the damage rectangle, so a title bar arrives here in slices', and at 959 'The window manager composes onto one'. The window manager composes nothing in Lua any more (compose.lua header), and every caller (ui.lua:9416, menubar.lua:146, strips.lua:58) paints a whole band.
  -> Rewrite the comment (y0/span may be removable too)
[78] stale-comment low user/lib/themes.lua:16 ~8L
  '**Six looks, and nothing else to choose**' and 'Night, the sixth, last'. There are fourteen in themes.order (Aurora...Liquid Glass Night came after), and Night is no longer last. Line 31 also says 'The chrome - window tabs and menu bars', and window tabs are gone.
  -> Rewrite the comment
[79] stale-comment low user/lib/ui.lua:101 ~2L
  'a row is 24 pixels, a button 28, a field 26, whatever face is in force'. The metrics are row 32, button 31, field 31 since 24 September (theme.lua:447-452, whose own comment 437-439 records the change). theme.lua:411's 'So a list or menu row is 24 pixels, a button 28, a field 26' carries the same old numbers.
  -> Rewrite the comment (point to theme.metrics instead of numbers)
[80] duplicate low user/bin/apps/deskbar.lua:1049 ~20L
  deskbar's halfway(a, b) is theme.mix(a, b, 500), and launchpad.lua:160's local mix(a, b, t) is theme.mix in 255ths. Two hand-copies of the kit's colour mixer (llmmon.lua:535 is a third, outside this area).
  -> Merge into theme.mix
[81] duplicate low user/bin/programs/wm.lua:551 ~10L
  `OUT.corner = OUT.want_corner and scale.px(theme.metrics.corner or 0) or 0` / the same for shadow is written out in five places (274-275 in scale.chrome, 551-552, 5039-5040, 7232-7233, plus 5027 and a variant at 7266/7279), where scale.chrome() already computes both.
  -> Merge into one function (scale.chrome or a small OUT.outside())
[82] duplicate low user/lib/theme.lua:404 ~3L
  STYLED = { cut, star, plate, flat, title_bars, shadow } is a second list of theme.STYLE (722); a seventh style key added to one and not the other would let a look say it again.
  -> Build STYLED from theme.STYLE (move it up or build it lazily)
[83] wasted-work low user/bin/apps/deskbar.lua:2650 ~12L
  dock_frame runs every pass of the dock's loop (12 a second while a program breathes). Each pass it builds a 7-element table and table.concat's it into a fresh string with four tostring calls just to see whether the strip changed, plus `reply and reply.events or {}` and a wmproto.poll message on the strip window.
  -> measure first (frames' allocation column on the Deskbar); compare the fields one by one against the last-said values instead
[84] wasted-work low user/bin/programs/wm.lua:8489 ~3L
  The window manager's main loop does `ipairs(input.keys or {})`, `input.events or {}` and `input.clicks or {}`, which is up to three empty tables per pass when nothing happened. That is the habit the comment at 8626 says 'this loop cannot afford'.
  -> fix: one shared local EMPTY = {} (or `if input.keys then`)
[85] wasted-work low user/bin/programs/wm.lua:296 ~2L
  scale.event allocates `{ "x", "y", "w", "h" }` on every call, and post() calls it for every positioned event to a window at a scale other than 100% (each pointer move).
  -> Hoist the key list to a file-level constant
[86] wasted-work low user/bin/programs/wm.lua:1923 ~8L
  OUT.platinum_colours() builds a new table each call, and describe() calls it on every compose pass under Pewter's Platinum chrome, only to pass its 4 numbers on 3 times.
  -> Cache it per theme.sunken
[87] speed-candidate low user/bin/apps/deskbar.lua:1211 ~15L
  bar:draw paints its gradient with one g:fill per row (32 fill ops, each a table, each with lit() arithmetic), a second hand-written vertical gradient beside theme.vgradient. theme.vgradient does the same per-row fill loop for every kit header (ui.lua:9416, 46 rows) on every paint.
  -> measure first; if it shows in the profile, add one gradient op to paint.lua/gfx and use it in both places
[88] speed-candidate medium user/lib/ui.lua:10194 ~0L
  Every repaint of a kit window builds a new gc and a new table for every drawing op (gc:fill etc. append `{ op = ..., x = ..., ... }`), then paint.scale rewrites each op at a non-100% scale, all to be thrown away right after paint_ops. Since D2e the ops never leave the process, so the table-per-op shape is now only an internal st
  -> measure first (gc_pause_max for an animating kit window); a reused ops pool or direct surface calls would remove it
[89] dead-code high user/bin/apps/deskbar.lua:159 ~60L
  retire_seed(): a one-time migration (28 September) that moves the old Deskbar seed's launchers to the Trash when /Home/Deskbar/.seeded exists. It runs at every Deskbar start and does nothing once the record is gone.
  -> Remove once Diego confirms every home in use has been migrated (unproven)
[90] stale-comment low user/bin/apps/deskbar.lua:2686 ~1L
  'A desktop with no Deskbar, as Pewter will be, opens them too'. Pewter already has no Deskbar (Pewter.desktop shell = notifications, desktop:pewter, menubar).
  -> Rewrite the comment
[91] stale-doc low user/desktops/Astra.desktop:16 ~3L
  The key list in the file that documents the format says chrome is 'astra, or platinum', and leaves out starlight (Starlight.desktop:16, desktops.lua M.CHROMES) and the `look` key.
  -> Rewrite the comment
[92] stale-comment low tools/run_scene.py:94 ~1L
  boot()'s docstring says 'A desktop started as `wm compose-check`', but it boots 'wm Astra.desktop' (line 102), and compose-check no longer exists (the module docstring at 9-12 says so itself).
  -> Rewrite the docstring

##### user/kits/ (every C kit) and runtime/ outside runt
[93] dead-code medium user/kits/web/web_kosmos.c:1357 ~650L
  The web kit's forms and fetching API is left over from the old browser and no Lua calls it: doc methods ns_click, ns_select, ns_select_choose, ns_key, ns_focused, ns_blur, ns_sent, ns_dirty, ns_costs, ns_sheets, ns_sheet, ns_imports, ns_import, and kit functions web.join, web.zoom, web.events. Behind them in web_netsurf.c sit we
  -> Remove the Lua bindings and the web_ns_* form, sheet and import functions they reach. Keep the browser_window_* stand-ins NetSurf's html handler links against (web_netsurf.c around 1340-1530); only fo
[94] dead-code medium user/kits/web/web_kosmos.c:659 ~2000L
  Kosmos's own HTML layout (doc:render, doc:link_at, doc:images, through web_paint.c's web_page_* functions, 1,184 lines, and web_style.c's web_style_open/of/close) was the browser's fallback 'where NetSurf could not'. No application calls it now. Its only remaining caller is the RENDER probe in tools/run_web.py, which `make web` 
  -> Decide whether `make web`'s probe is worth keeping. If not, remove web_paint.c/.h, the web_style_open/of/close half of web_style.c, l_render, l_link_at, l_images, l_meta, l_lang, l_title, l_count, l_t
[95] stale-comment low user/kits/web/web_netsurf.c:123 ~30L
  The web kit's comments still describe a browser that was deleted on 17 November. Examples: "It is the browser's only join: `browser.lua`'s `resolve` is this" (l.126-127); "the browser, whose fetching is Lua's (`browser.lua`)" (l.1344); "A form sent: kept for the browser, which fetches it ... (`browser.lua`)" (l.1478-1479); web_p
  -> Rewrite the header comments to say the kit is Mail's HTML (until mailweb.md W6), and drop the browser.lua and suite references, preferably in the same change as the two removals above.
[96] wasted-work low user/kits/web/web_netsurf.c:1039 ~80L
  Every NetSurf plot call in Mail's paint goes through TIMED(), which reads kosmos_ticks() twice and accumulates into `costs`. Layout and picture paths time themselves too (2733, 2767, 2881, 3203). The only reader was doc:ns_costs, which no Lua calls, so every message painted pays for numbers nobody reads.
  -> Remove along with ns_costs, or keep it behind a debug flag. Measure first if anyone argues it is free.
[97] dead-code low user/kits/ffmpeg/aac_kosmos.c:124 ~155L
  The AAC Kit's Lua door (use("/Kosmos/Kits/aac"), aac.decoder, d:decode, d:reset, d:close; 147 lines) has no caller. video.lua now plays a film's sound through the Playback Kit, which calls aac_core.c directly from C. The kit is still registered in the kit table (sys_user.c:2941) and still built (Makefile:1026).
  -> Remove aac_kosmos.c, its Makefile entry and the { "aac", kosmos_aac_kit } row in user/init/sys_user.c, and keep aac_core.c.
[98] dead-code low user/kits/playback/playback_kosmos.c:201 ~12L
  p:frame_string(s), a frame handed over as a Lua string, is registered (l.275) and documented (l.20), but every caller uses p:frame(at, n) from a region, which is what the control-by-message/data-in-a-region rule asks for.
  -> Remove l_frame_string, its table row and its doc line.
[99] dead-code low user/kits/gfx/gfx.c:709 ~20L
  The Lua binding surface:blit_round has no Lua caller. The C primitive blit_round_on (draw.c:305) is still used by scene_core.c:281, so only the binding is dead. draw.c:280 still documents the `dst:blit_round(...)` Lua form.
  -> Remove l_blit_round and the { "blit_round", ... } row (gfx.c:3531), and reword draw.c:280 to describe the C call.
[100] dead-code low user/kits/webkit/webkit_kit.c:222 ~15L
  webkit.download_folder(path) is registered (l.834) but JetSki never calls it, so downloads always go to the compiled-in "/Home/Downloads" (webkit.cpp:512). The comment at webkit.cpp:490 says "Where: the folder JetSki names (`webkit_download_folder`)", which is not what happens.
  -> Either remove the binding and webkit_download_folder and fix the comment to say the folder is /Home/Downloads, or have JetSki name it. Both copies of the path would then come from one place.
[101] dead-code low user/kits/mail/mail_kosmos.c:436 ~15L
  mail.encode_words is set on the kit table but no Lua calls it. mail_encode_words in C is used inside mail.build and by tools/test_mail.c, so only the Lua binding is unused.
  -> Remove l_encode_words and its setfield.
[102] dead-code low user/kits/sqltest/sqltest_kit.c:96 ~40L
  sqltest's exec(path, sql), backed by sqltest_exec in sqltest.c:167, is registered and documented, but its test program never calls it. tools/run_sql.py uses only fuzzcheck, speedtest1, locks and keep.
  -> Remove exec and sqltest_exec, or add a check in run_sql.py that uses it.
[103] duplicate low user/include/bytes.h:14 ~60L
  bytes.h says it is the one copy of the byte-order readers and writers, but kit files still write their own: smb/ntlm_name.c:19-28 (le16, le32), crypto/md4.c:25 (le32), crypto/kdf.c:26 (be32 store, which is put_be32), debug/debug.c:48-53 (u32, u64, which are get_le32 and get_le64), map/pmtiles.c around 40-53 (a le64 loop and i32)
  -> Have them include "bytes.h" and delete the local copies. Byte-for-byte equivalent; the host tests (test_crypto, test_mail, test_smb and others) hold the results.
[104] duplicate medium user/kits/gfx/gfx.c:1442 ~100L
  UTF-8 is decoded and encoded by separate private helpers in five kits: gfx.c:1442 utf8_next (decode, U+FFFD on error), llm/llm_core.c:720 put_utf8 and :735 get_utf8, mail/charset.c:66 put_utf8 and :90 utf8_ok, gfx/face.c around 140-151 (an inline encoder), and smb/ntlm_name.c around 95-110 (an inline encoder in utf8_of). Each ma
  -> Write one user/include/utf8.h (inline decode-one and encode-one, as bytes.h is) and move the five onto it, checking that each caller's error behaviour still holds (the mail charset tests, llm tokenize
[105] duplicate medium user/kits/gfx/gfx.c:521 ~30L
  Mixing a colour onto an opaque destination by coverage, ((s*a + d*(255-a) + 127)/255) per channel, is written out separately in gfx.c:521 mix(), gfx/rows.c:~83-90 (text ink), gfx/scene_core.c:254 (inline), and 3d/k3d_raster.c:161 blend(). The k3d copy has no +127 rounding, so it rounds differently from the other three. surface.h
  -> Add one opaque-mix inline to surface.h, next to over, and use it in all four places. Check gfx's pixel tests, and decide on purpose whether k3d's truncation is wanted.
[106] duplicate low user/kits/gfx/gfx.c:3642 ~20L
  gfx.encode_png uses stb_image_write's PNG writer, which carries its own zlib compressor (stbiw__zlib_compress). The comment that justified this, runtime/upstream/stb/stb_impl.c:79 "this image has an inflater and nothing that compresses", stopped being true when the Compress Kit gained miniz's tdefl (user/kits/compress/deflate.c)
  -> Define STBIW_ZLIB_COMPRESS to a small wrapper over tdefl (tdefl_compress_mem_to_heap with the zlib header flag) in Kosmos's stb_impl.c, and correct that comment. Measure the PNG size and time before a
[107] duplicate low user/kits/compress/inflate.c:12 ~5L
  There are two deflate implementations: the Compress Kit's miniz (tinfl and tdefl), used by png.c, PDF, zip, gzip, http and VNC, and zlib 1.4, which W5 builds for libpng, FreeType, libxml2, curl and OpenSSL in the WebKit images. inflate.c and gzip.h still say "the system's only one".
  -> Already planned on the roadmap. Until it is done, reword 'only inflater' to 'the Compress Kit's one inflater' so the comment does not claim something the tree contradicts.
[108] duplicate low user/kits/ffmpeg/h264_kosmos.c:1 ~0L
  MP4 is demuxed twice (user/lib/mp4.lua for Video, and libavformat in the Media Kit, user/kits/media/media.c, for JetSki). H.264 and AAC are reached through two doors (the ffmpeg h264 and aac kits, and the Media Kit's decoders over the same FFmpeg).
  -> Already planned as media.md M6. Make sure the h264 kit's Lua door goes in the same step; its AAC sibling can go now (separate finding).
[109] stale-comment low user/init/sys_user.c:2927 ~2L
  The kit table has an orphaned comment, "Brotli, woff2, libavif and dav1d, in no image but their test's (`docs/w5.md` W5f).", with no entry under it. The next line is the WebCore comment and its row. formattest is reached as a project kit (kosmos_project_kit in user/kits/formattest/formattest_kit.c:42), not through this table.
  -> Delete the two comment lines, or replace them with a note that W5f's kit is a project kit.
[110] stale-comment low user/kits/apptest/apptest_tls.c:34 ~1L
  "pthreads will begin a thread the same way (W3)." W3 is built: runtime/posix/pthread.c:386 calls kosmos_tls_begin() for each new thread.
  -> Change it to the present tense, e.g. 'pthread_create begins a thread the same way (runtime/posix/pthread.c)'.
[111] stale-comment low runtime/libc/scan.c:5 ~1L
  "`sscanf` and `vsscanf` lived in `user/lib/misc_user.c`". That path does not exist and user/lib holds only .lua files. The file is user/init/misc_user.c, which runtime/include/stdlib.h:83 and runtime/posix/process.c:8 both name correctly.
  -> Correct the path to user/init/misc_user.c.
[112] stale-comment low user/kits/webkit/webkit.cpp:490 ~2L
  "Where: the folder JetSki names (`webkit_download_folder`)". JetSki names no folder; the default "/Home/Downloads" at l.512 is always the one used.
  -> Fix together with the download_folder finding.

##### user/servers/ (except vmm.c), user/drivers/, user/
[113] bug medium user/servers/appfs.c:221 ~10L
  APP_OP_UNREGISTER destroys the endpoint registered under any name, for any caller. There is no check that the sender is the one who registered it, and kernel/ipc.c ipc_endpoint_destroy (line 1281) only resolves the capability; it does not check ownership. Any program handed /Running can send {type="unregister", name="wm"} and ap
  -> fix: record the registering process (SYS_SENDER, as notify.c does) in struct entry and honour UNREGISTER only from it; or just drop the capability (kosmos_cap_drop) instead of destroying the endpoint,
[114] bug low user/servers/appfs.c:181 ~8L
  A capability that arrives with a request is never dropped unless the request is a successful REGISTER. That covers REGISTER refused as APP_ERR_FULL, a short message, and LOOKUP, LIST or UNREGISTER sent with a capability attached. Each one stays in appfs's capability table for good, so a careless or hostile client can make the ta
  -> fix: kosmos_cap_drop(cap) on every path where cap >= 0 and it was not stored
[115] dead-code low user/servers/vmmblk.c:304 ~12L
  vmmblk_counted() (declared vmmblk.h:26) has no caller. The B.reads and B.writes counters it returns (lines 53, 117, 139) are bumped on every virtio-blk request and never read.
  -> Remove the function, its declaration and the two counters, or connect them to the monitor's VMM_OP_STATE if a sandbox's disk traffic is wanted
[116] dead-code low user/servers/vmmnet.c:696 ~14L
  vmmnet_busy() (declared vmmnet.h:30) has had no caller since 0.11.204, when vmm.c stopped choosing a 1- or 2-tick wait from it.
  -> Remove the function and its declaration
[117] dead-code low user/drivers/bluetooth/gap_decode.c:316 ~16L
  gap_kind_name() (declared gap_decode.h:108) has no caller: neither the Bluetooth driver nor its host test turns a kind into words. The Lua side does that itself.
  -> Remove it, or have tools/test_btdecode.c use it if it is meant for the log
[118] dead-code low user/include/hciring.h:87 ~5L
  hci_ring_ready() is defined and never used. The transport and the stack compute write - read inline or use hci_ring_space.
  -> Remove
[119] dead-code low user/include/llmring.h:54 ~8L
  llm_answer_acquire() is advertised at line 16 as the C reader's load, but there is no C reader. The only reader is Lua, through sys.region_load32.
  -> Remove it and the clause in the comment at line 16, or keep it knowingly for a future C reader
[120] dead-code low user/drivers/net/e1000.c:196 ~1L
  LINK_MS, "how long a link is waited for, once", is defined and never used. The driver no longer waits for a link: say_link reports it when it changes, as the no-waiting-on-hardware rule requires. The constant and its comment describe a wait that no longer happens.
  -> Remove
[121] dead-code low user/drivers/usb/xhci.c:3975 ~1L
  ARP_REQUEST_BYTES (42) is defined next to ether_probe and never used. The probe pads to ETHER_LEAST.
  -> Remove
[122] stale-comment low user/servers/net.c:20 ~3L
  The file's header says "One segment in flight on a retransmission timer". Since 10 October the stack sends a window of segments under a congestion window: cwnd, RFC 5681 slow start, most_in_flight.
  -> rewrite comment: a window of segments under cwnd and the peer's window; add RFC 5681 (and 1112 for IGMP) to the RFC list at line 41
[123] stale-comment low user/servers/net.c:26 ~3L
  The header says "There are no UDP sockets, because nothing has asked for one." NET_OP_UDP_BIND exists and mDNS uses it. The comment at line 1428 even quotes the old sentence as history.
  -> rewrite comment: UDP for DNS and DHCP, plus a program's bound port (NET_OP_UDP_BIND)
[124] stale-comment low user/servers/windowserver.c:621 ~8L
  The comment says composing's opaque parts "stay the processor's for now ... off, until the blitter copies faster (XY_FAST_COPY_BLT, next)". The code turns it on whenever rendering mode 4 or higher is chosen (lines 629, 1572), so it is a selectable mode now, not off. XY_FAST_COPY_BLT appears nowhere in the display driver.
  -> rewrite comment: off by default (rendering starts at 2), on at mode 4, and why that is not the default
[125] stale-comment low user/include/keyproto.h:39 ~4L
  KEY_KIND_MAIL is marked "later", but the keyring has served a mail door since 8 October and Mail declares it. The op comments for GET, PUT and FORGET (lines 28-31) still say "smb:" when the mail and sharing doors use the same ops for their own kinds.
  -> rewrite comment: drop "later" from KEY_KIND_MAIL; say "a kind's own door" rather than "smb:" on GET, PUT and FORGET
[126] stale-comment low user/servers/drives_decode.h:111 ~2L
  Two comments name functions that do not exist. Line 111 says "`drives_fs_of` calls them OTHER", and line 167 says "`drives_free` says which it gave". The real code is drives.c, where v->fs = FS_KIND_OTHER at 460/486 and the free count comes from measure_free (341) and fat16_free (302).
  -> rewrite comment: name measure_free and the FS_KIND_OTHER assignment in drives.c
[127] stale-comment low user/servers/smbfs.c:2428 ~2L
  Two comments (2428 and 2947) say a deferred listing or open "is begun by the loop when it ends (`chains_waiting`)". No function or variable has that name. The code that restarts them is the block "Listings and opens that waited for another tree's chain" inside trees_wanted (line 3325, at about 3374-3395).
  -> rewrite comment: point to trees_wanted
[128] duplicate low user/servers/vdev.h:101 ~30L
  vdev.h defines its own little-endian vget16/32/64 and vput16/32/64. bytes.h already supplies get_le16/32/64 and put_le16/32/64 and calls itself "the one copy". vmmfs.c:108-113 then aliases them again as get16..put64, and get16 is unused. vmmgpu.c:85-87 has a third set (memcpy get32/get64/put32), vmmnet.c:143-146 a big-endian set
  -> merge into bytes.h: include it and use get_le*/put_le*/get_be*/put_be*; delete the local copies and the vmmfs aliases
[129] duplicate low user/servers/smbd.c:1252 ~20L
  More byte-order helpers outside bytes.h. smbd.c:1252-1254 has put16/put32/put64 (LE), keyfile.c:42-55 has put32/get32 (LE), and dnsmsg.c:12 and mdnsd.c:181 have the same be16. mdnsd already includes dnsmsg.h, so even the DNS pair duplicates itself.
  -> merge into bytes.h (put_le16/32/64, get_le32, get_be16)
[130] duplicate low user/servers/smbfs.c:2074 ~35L
  Three hand-written case-insensitive comparisons: same_name in smbfs.c:2074, mdnsd.c:168 and drives_decode.c:336. The libc already supplies strcasecmp, and appfs.c:47 uses it for exactly this.
  -> merge into strcasecmp (strings.h); drives_decode.c is host-tested too, so confirm the host build has it (it does on macOS)
[131] duplicate medium user/drivers/usb/storage_decode.c:250 ~20L
  storage_crc32 is a bit-at-a-time CRC-32 (polynomial 0xEDB88320), used to check a GPT header's CRC. The Compress Kit already supplies CRC-32 through zlib/miniz (compress.crc32, user/kits/compress/deflate.c:126). This is a second copy of the same checksum in the same image.
  -> Check that the driver's object can link the kit's crc32 (zlib's crc32()), then merge; otherwise record why it cannot

##### Makefile and *.mk, tools/ (host scripts, run_*.py 
[132] dead-file low tools/test_layout.py:1 ~2L
  The layout rule checker (servers must not touch hardware; user/lib holds only Lua; user/kits holds no Lua; no kit named like an app) is not run by any make target or gate suite, and it never was. Yet README.md:192, docs/architecture.md:288 and docs/glossary.md:65 say it 'keeps it true' and that a .c under user/lib 'fails the gat
  -> fix: add `python3 tools/test_layout.py` to the host-check recipe (it boots nothing and takes under a second), or delete it and correct the three documents
[133] other medium tools/gate.py:1323 ~20L
  Since the x86-only rule of 8 October, 16 ARM-only suites have no x86 twin, so a default `make test` never runs them: arm-interchange, arm-write, arm-queries, arm-shell, arm-diskbench, arm-launch, arm-diskwire, arm-passwords, arm-share-3 (the sharing windows), arm-share-4 (keyring remembers a share), arm-writeapp, arm-media, arm-
  -> measure first: add x86- twins for the ones that are not about the ARM board (most of them are Lua over wm), or name them in roadmap.md as uncovered until ARM resumes
[134] stale-comment medium tools/test_tutorial.lua:93 ~25L
  The tutorial check still requires every tutorial page to use only the tags that NetSurf's web_style.c and web_paint.c draw ('What Kosmos's browser draws', TAGS), and every picture to be one gfx.png decodes 'so the browser shows it pixel for pixel'. Since 0.11.461 tutorials open in JetSki (WebKit) through weblink.lua. NetSurf rem
  -> rewrite comment and decide: drop or relax the TAGS and gfx.png checks now that WebKit draws the pages, or say why the NetSurf subset is still wanted
[135] dead-file low tools/run_smpshot.py:31 ~200L
  An SMP screenshot harness that no make target, gate suite or script runs. Its docstring says "`Makefile`'s `smpshot` target does the first", but no smpshot target exists and the Makefile history shows none ever did. It also defaults KOSMOS_SMPWORK to 4 (line 47) while the docstring says the unset value is the default. Its commen
  -> Remove, or add the promised `smpshot` target and fix the docstring and comments
[136] dead-file medium tools/test_pdf.lua:17 ~150L
  A host test of user/lib/pdf.lua that nothing runs. It defaults to /Users/diego/Downloads/pg1727-images-3.pdf, and it was last touched on 2 September.
  -> Wire it into host-check against a committed fixture (tools/mkpdf.py already makes PDFs), or remove it; x86-pdfkit/x86-pdfview cover the PDF path in the guest. Whether it still runs is unproven.
[137] dead-code low Makefile:453 ~6L
  CFLAGS_LUA_UPSTREAM is defined, along with a paragraph explaining that upstream Lua is compiled without -Werror, but no rule ever expands it.
  -> Remove the variable and its comment, or apply it to the lua/upstream objects if that was the intent
[138] dead-code low Makefile:1322 ~1L
  USER_DEPS := $(USER_OBJS:.o=.d) is never included or expanded. Userland .d files are pulled in by `-include $(shell find $(UBUILD) -name '*.d')` at line 6481 instead.
  -> Remove
[139] stale-comment low Makefile:23 ~10L
  'It is a *bring-up* today and the target below says so - what builds is the boot path, the serial port and enough C to report that long mode is on.' x86-64 is now the platform the whole gate runs on, with SMP, the desktop and the M700/ThinkPad sticks.
  -> rewrite comment
[140] stale-comment low Makefile:97 ~15L
  The VARIANT comment speaks of `DOOM=1 WEB=1` and 'FULL setting DOOM and WEB below this line'. Line 1382 says BUILD 'does *not* vary with `DOOM`, `WEB` or `QUAKE`; `UBUILD` does', and that prepush switches between 'plain' and 'MEGA=1'. Neither DOOM nor QUAKE is a make variable any more: Doom and Quake are installed applications. 
  -> rewrite comment
[141] stale-comment low Makefile:6291 ~6L
  The comment on the `mega` target says 'MEGA=1 is where Doom and Quake meet in a single link - where a name both of them define shows up - and the image that comes closest to its heap', and that `shot` 'builds the ordinary one again after it'. Doom and Quake are separate images now and MEGA=1 is the default FULL=1 image, so prepu
  -> Rewrite the comment to say what the step proves now (ARM full image links), or fold it into shot
[142] stale-comment low Makefile:4976 ~25L
  Three contradictory comment blocks sit above `release`: 'One size for a full image and three for a lean one' (4976), then 'Two sizes for a full image and three for a lean one' (4985). There is also 'A full image is 6.2 MB ... the browser's five vendored libraries and Doom' (4978), and 'a compiled-in framebuffer cannot be resized
  -> Rewrite into one block that says what is true
[143] wasted-work medium Makefile:4992 ~10L
  `make release` still relinks and copies one image per RELEASE_SIZES entry (two for full, three for lean), deleting fbpixels.c.o each time. Its own comment at 4962-4968 says the size an image is built with is only its default, and any size is chosen at boot with `run-kosmos.sh -r`. So every image after the first is a redundant mu
  -> Measure first: confirm run-kosmos.sh -r covers every size, then release one image
[144] stale-comment low Makefile:1134 ~8L
  'a build that does not want a browser should not carry a CSS engine. When the GPLv2 browser core joins them the flag will be doing licence work too'. The NetSurf browser has been removed (0.11.461); WEB=1 now carries only Mail's HTML kit, and it is on by default under FULL=1.
  -> rewrite comment
[145] stale-comment low tools/run_web.py:22 ~4L
  'Not run by `make test`, because `WEB=1` is an optional variant like `DOOM=1` and the ordinary image carries none of this.' DOOM=1 no longer exists, and the ordinary image (FULL=1, the default) does carry the web kit. The Makefile comment above `web:` (6264-6268) says the same. The NetSurf kit's own checks therefore run only by 
  -> Rewrite the comment; consider an x86 suite for it, since Mail's HTML depends on this kit
[146] dead-code low tools/run_screenshot.py:2418 ~40L
  tab_width() and tab_top() are never called. tab_width's docstring even says it 'is still what several checks want'.
  -> Remove (about 40 lines)
[147] dead-code low tools/run_screenshot.py:616 ~5L
  Guest.line_starting() is never called by any harness.
  -> Remove
[148] dead-code low tests/machine.h:78 ~12L
  test_load_from() is a static inline helper that no test calls; only test_store_to is used.
  -> Remove, or write the 'pages that may not be read' test it was meant for
[149] dead-code low tools/farpeer.py:65 ~4L
  tcp_options() wraps tcp_options_all()[:2], and nothing calls it; run_tcpin.py imports farpeer as F and uses tcp_options_all.
  -> Remove
[150] stale-comment low tools/gate.py:25 ~3L
  The module docstring says two suites are marked `alone` ('Two do: x86's HDA sessions ... and Groove under heavy load'). Three are: x86-sound, x86-film-load and arm-synth-load.
  -> rewrite comment
[151] stale-comment low tools/luaglobals.py:58 ~4L
  'They are kits now, `use("/Kosmos/Kits/doom")`'. Doom, Quake and SNES are installed applications reached as use("doom.elf") and never as /Kosmos/Kits/doom. tools/run_screenshot.py:1281 says the same thing ('They are `use("/Kosmos/Kits/snes")` now').
  -> rewrite comment (both places)
[152] stale-comment low tools/run_screenshot.py:183 ~2L
  'what lets `run_web.py`, `run_browser.py` and `run_gallery.py` point at either board'. run_browser.py was deleted in 0.11.461.
  -> rewrite comment
[153] dead-file high tools/run_site.py:6 ~400L
  A W6d tool that paints a saved site with webcoretest.elf because 'Until W7 there is no window and no network loader'. record_site.py (line 11) says the same. JetSki now loads real sites over the network: x86-sites, x86-jetski and the top20.py benchmark. No make target or gate suite runs either script; record_site.py only points 
  -> Remove both, or rewrite their docstrings if they are still wanted for offline comparison with Chrome. Whether anyone still uses them by hand is unproven.
[154] speed-candidate low tools/run_screenshot.py:2418 ~0L
  The display harness (11,902 lines, run as 10 gate suites) does its pixel analysis in pure-Python loops over full 1920x1080 PPMs: is_tab per pixel, find_colour, count_windows, _strip and bars_drawn. A full-screen scan is about 2M Python iterations per call, inside a gate held to 5-10 minutes.
  -> measure first: take each phase's time from the harness's phase_times and the host-side time spent in these helpers before vectorising (bytes.find, memoryview strides or numpy)

##### Nebula and below: kernel/, arch/aarch64, arch/x86_
[155] bug medium arch/aarch64/trap.c:447 ~4L
  The IRQ path taken from EL0 (vector index 9) handles input sleepers, the tick and console_tick on core zero, but it never calls `hal_snd_wants()` / `process_wake_audio()`. The EL1 path (line 379) and x86's single path (x86_64/trap.c:445) both do. virtio_snd_wants is read-and-clear, so a period interrupt taken while core zero run
  -> fix: add the hal_snd_wants/process_wake_audio pair to the EL0 path, or better, fold the core-zero work into one kernel function (see the duplicate finding). Confirm with an arm audio suite under a spi
[156] duplicate medium arch/aarch64/trap.c:363 ~60L
  What core zero does on an interrupt (tick charge and profile sample, audio wake, input-sleeper wake, console_tick, the secondary-core guard) is written out three times: in aarch64's EL1 IRQ path, in aarch64's EL0 IRQ path, and in x86_64/trap.c:401-470. The copies have already drifted: the guard was missing from the EL0 copy once
  -> merge into one kernel function, e.g. irq_after(bool tick, uintptr_t pc, bool from_user), called by each arch's handler
[157] dead-code low kernel/ipc.c:789 ~8L
  `ipc_hold_memory` is a one-line wrapper over `ipc_hold_memory_as(t, index, NULL)`. Nothing calls it any more. Every holder uses the `_as` form, which also reports read-only.
  -> Remove it and its declaration in ipc.h; update testing.md 18.298's mention if wanted
[158] dead-code low kernel/thread.h:206 ~3L
  `sched.key` (uint64_t in struct thread) is never written or read. Its only mention is the comment at thread.c:2380, which presents it as 'already there for a deadline-ordered queue'. That queue was never built.
  -> Remove the field and reword the thread.c:2376-2381 comment
[159] dead-code low hal/pc/blk_bind.c:80 ~20L
  The PC board's `hal_blk_present` is never called. The kernel never asks this HAL entry, and the only caller anywhere is hal/qemu-virt/virtio.c:266, which links the qemu-virt definition. On the PC this leaves `memdisk_present` (hal/pc/memdisk.c:117) and `nvme_present` (hal/pc/nvme.c:636) reachable only through dead code. hal.h:93
  -> Remove the PC definition along with memdisk_present/nvme_present, or keep them only if hal/pc's bus scan should mark the disk claimed the way qemu-virt does (pci.c:874 uses was_taken instead)
[160] stale-comment low hal/pc/memdisk.h:5 ~4L
  The comment says "GRUB reads the USB stick through the firmware, which Kosmos cannot yet do itself - there is no xHCI driver". Kosmos's own UEFI loader (boot/efi/loader.c) replaced GRUB on 13 September 2026, and user/drivers/usb/xhci.c is the xHCI driver that serves sticks to /Devices/blocks.
  -> rewrite comment: the loader (boot/efi/loader.c) reads the disk image via UEFI and hands it over as a Multiboot 2 module
[161] stale-comment low hal/pc/memory.c:124 ~4L
  Two comments still say that boot options travel on GRUB's line. memory.c:124 reads "a laptop booted by GRUB is told `opt/kosmos/smp=1` on the `multiboot2` line". kernel/main.c:1199 reads "or the word on GRUB's line". The words now come from `\boot\kosmos.cmdline`, which Kosmos's loader reads (CMDLINE_MAX in boot/efi/loader.c, wh
  -> rewrite both comments to name the stick's \boot\kosmos.cmdline and `make usb KOSMOS_ARGS=`
[162] stale-comment low kernel/process.h:557 ~3L
  The comment says `owns_devices` is held "for now, by the two drivers init starts - the power button and the USB host controllers". init.lua now spawns six roles with SPAWN_DEVICES: AHCI, e1000, xHCI, the power button, backlight and the Intel GPU.
  -> rewrite comment: init and every userland driver it starts, until a devices server holds the authority
[163] stale-comment low kernel/sched_prio.c:28 ~10L
  The header says "five [levels] say everything this system has to say" and that "the only threads above `PRIO_NORMAL` are the ones that read input and put pixels on the screen, and both block almost immediately. If that stops being true, this comment is where to start." It has stopped being true. sched.h now has seven bands, incl
  -> rewrite comment: the band count, the game and audio bands, and what bounds each one (the audio budget, game mode's core of its own)
[164] stale-comment low kernel/sched_rr.c:14 ~3L
  The comment says "This is the policy the IPC benchmark at the end of M3 is measured against". The benchmarks run under sched_priority, the boot default (thread.c:905). Round robin is now an alternative policy that sched_switch_to can select and the guest tests exercise.
  -> rewrite comment: say what the policy is for now (a switchable alternative and a fairness control in the tests)
[165] stale-comment low kernel/pmm.c:223 ~6L
  The comment justifies one global pmm lock with "a page is allocated when a process starts or a region is made, not on any hot path". Pages are now handed out on touch from fault paths: reservations (reserve.c:496, which backs malloc and JavaScriptCore's heap), regions filled on touch (memobj.c:110) and thread stacks below their 
  -> rewrite comment; measure first: pmm_lock hold time under a JetSki page load on 4 cores before considering per-CPU free lists
[166] stale-comment low arch/aarch64/trap.c:384 ~20L
  Two comments are no longer true. Line 384 says "The timer is the scheduler's clock, and it is the only interrupt source there is, so every IRQ is a tick. When there is a second source, hal_irq_handle has to say which one fired". hal_irq_handle already returns `tick`, and the code above it (line 363) uses that. Lines 455-466 say 
  -> Delete the 384 paragraph and reword 455-466 into the past tense or drop it
[167] stale-comment low kernel/memobj.h:119 ~5L
  This and three other comments name functions or files that no longer exist under those names. memobj.h:119 points to `memobj.c`, `index_page`, but memobj.c has index_slot and page_slot. process.h:362 says a stack index is given back in `process_thread_start`, but there is no such function: it is given back when the thread is col
  -> rewrite the five comments with the current names
[168] stale-comment low kernel/syscall.h:1331 ~12L
  Kernel comments still name the namespace's old paths in the present tense. `/dev/cpu` appears at syscall.h:1331, arch/aarch64/cpu.h:94 and hal/pc/timer.c:412. `/dev/blocks` appears at kernel/irq.c:383-433, kernel/syscall.h:407, kernel/syscall.c:2367 and kernel/thread.h:103-111. hal/pc/blk_bind.c:10 says "the drive's `/home`". Us
  -> rewrite the comments with /Devices/cpu, /Devices/blocks and /Home. The quoted browser line at x86_64/cpu.c:225 is history and can stay.
[169] wasted-work medium kernel/main.c:376 ~10L
  `check_init_image` runs four times every boot (lines 376, 821, 1299 and 1371). Each call is a byte-at-a-time FNV-64 over the whole userland image. A full x86 build's image is 44,302,276 bytes, so that is about 177 MB of serially dependent multiplies before init starts. It is a diagnostic for the T14 boot roulette of September. O
  -> measure first: time a boot with the four checks against one (for example only 'before init is built from it'), under TCG and on the M700. Consider a word-wide hash or checking only on a flag.
[170] speed-candidate medium arch/aarch64/mmu.c:759 ~3L
  Every address-space switch issues `tlbi vmalle1is` + `dsb ish`, a broadcast invalidate of the whole TLB on every core in the inner-shareable domain. Each IPC round trip between processes (two switches) therefore wipes the TLBs of the other three cores too. The comment's reason (another core keeping translations after pages chang
  -> measure first: ipc_roundtrip and the stress suite with 4 cores busy, comparing local vmalle1 + dsb nsh in as_switch against the broadcast form. ASIDs are the larger fix.
[171] speed-candidate low kernel/thread.c:2442 ~30L
  `thread_wake_sleepers` scans every thread slot the pool has ever made, on core zero, 250 times a second, with no deadline structure. The pools now grow to a RAM-derived ceiling and a browser session creates many threads, so this per-tick cost grows without bound and runs in interrupt context. The comment's suggested fix relies o
  -> measure first: tick duration on core 0 with JetSki open (slots_made in the hundreds or thousands) before replacing it with a deadline-ordered list
[172] speed-candidate low kernel/process.c:576 ~20L
  Finding a thread's index is O(n^2). For each candidate index from 1 upwards it walks the process's whole `threads` list and then `starting`. It does this while holding the global `processes_lock` (process.c:338), which masks interrupts. A process with n threads makes every new thread cost about n^2/2 list steps with interrupts o
  -> measure first: pthread_create latency in a process with 500 and 2000 live threads. A per-process free-index bitmap would make it O(n/64).
[173] speed-candidate medium arch/x86_64/vcpu.c:851 ~30L
  Every guest run does VMPTRLD, then VMLAUNCH, then VMCLEAR (never VMRESUME), plus sgdt/sidt and segment reloads and rewriting host state. That is a full VMCS flush to memory on each VM exit. The file says "it can be measured later". Kernel threads have homes and move only when asked, so the reason (the vCPU may be run on another 
  -> measure first: exits per second for a Linux guest's timer and MMIO-heavy load, comparing the current path with one that remembers the last core and uses VMRESUME
[174] other medium kernel/percpu.h:48 ~10L
  `#define NR_CPUS 8` is a compiled-in ceiling that sizes secondary stacks (smp.c:70-71), profile rings and the per-CPU syscall tables (profile.c), and smp.c:225 starts min(present, 8). The third machine, the Mac Pro 2013 (a Xeon E5 with up to 12 cores and 24 threads), and the 20-thread Alienware in targets.md are already over it.
  -> measure first what the per-CPU arrays cost at 32 or 64 entries, or derive the count at boot from hal_cpu_count. Decide deliberately rather than raising the number again.
[175] dead-code low arch/x86_64/vcpu.c:189 ~3L
  `EXIT_FAILED_ENTRY` (bit 31 of the exit reason, a VM-entry failure) is defined and never tested. A failed entry reaches the monitor as VCPU_EXIT_TRAP with the raw reason. Nothing marks it as distinct from an ordinary exit with the same basic reason, while arch_vcpu_missed masks with 0xffff. kernel/syscall.h:1218 `CPU_ARCH_UNKNOW
  -> Either decode bit 31 into its own exit kind in arch_vcpu_run, or remove the constant

##### user/lib/ (minus ui.lua, theme.lua, desktops.lua, 
[176] dead-file low user/lib/longmenu.lua:1 ~130L
  longmenu.grouped (a long menu grouped into submenus) was written for the old NetSurf browser's <select>. Since the old browser went, only its own host test loads it.
  -> Remove the library, tools/test_longmenu.lua and its Makefile rule, or wire it into ui.lua's menus if JetSki's <select> wants it
[177] dead-code low user/lib/http.lua:844 ~65L
  The cache path in http.get (opts.cache, opts.revalidate, how.cached) served httpcache.lua, which was deleted with the old browser. No caller passes opts.cache, so every call goes straight to get_uncached.
  -> Remove lines 833-896's cache branch (http.get becomes get_uncached) and the opts.cache/opts.revalidate doc lines
[178] dead-code medium user/lib/http.lua:508 ~70L
  Streaming the body to opts.on_body (hand_on, begin_handing, the gunzip_stream inflater, how.streamed, how.stream_said) existed so the old browser could feed its parser as a page arrived. Nothing passes on_body now.
  -> Remove the on_body sink and streaming inflater; keep the whole-body gunzip
[179] dead-code low user/lib/http.lua:111 ~48L
  http.refresh parses `<meta http-equiv=refresh>` and Refresh headers, which only a browser does. It is held only by test_http.lua.
  -> Remove with its test cases
[180] dead-code low user/lib/http.lua:174 ~10L
  http.authorities() and http.authority_names() fed the old browser's about:authorities page. Nothing calls either.
  -> Remove both
[181] dead-code low user/lib/http.lua:921 ~8L
  get_many's opts.each callback (and its doc at line 372) has no caller. Mail's get_many passes only pause.
  -> Remove
[182] dead-code medium user/lib/browserprefs.lua:79 ~45L
  The old browser's settings remain: AGENTS (Lynx/NetSurf/3DS user agents), prefs.agent, DEFAULTS.agent/agent_words/bar/costs, CHOICES.agent, prefs.ROOTS = 'about:authorities', and prefs.DIR/FILE/TABS (the old browser's folder). JetSki passes its own /Home/Settings/JetSki paths and reads only home, opens, zoom, images, javascript,
  -> Remove the agent table, ROOTS, unused defaults and choices, and make read/write/tabs take a required path (JetSki's). Trim test_browserprefs.lua to match
[183] dead-code low user/lib/clock.lua:64 ~20L
  clock.days (days_from_civil) was kept for httpcache.lua's Date/Expires parsing, and its comment says so. httpcache.lua is gone, and only test_clock.lua calls it.
  -> Remove, or keep and rewrite the comment if a second calendar user is expected
[184] dead-code low user/lib/clock.lua:99 ~4L
  clock.set_offset is a second way to write the time zone. Preferences writes it through settings.set (settings.CLOCK), and only test_clock.lua calls set_offset.
  -> Remove, and have the test set the offset through settings
[185] dead-code low user/lib/history.lua:107 ~18L
  history.lately (the newest pages across days, for the old browser's new tab) is called only by test_history.lua.
  -> Remove, or use it in JetSki's new tab
[186] dead-code low user/lib/drivelist.lua:140 ~4L
  drivelist.is_sandboxes has no caller at all. held() and HELD replaced it.
  -> Remove
[187] dead-code low user/lib/launchgrid.lua:142 ~8L
  grid.next_category ('what Tab does') is called only by test_launchgrid.lua. In launchpad.lua Tab now switches between the sidebar and the grid (line 603), and Up/Down step through the sections in their own loop. So the function is unused and its comment is false.
  -> Remove, or have launchpad's Up/Down use it
[188] dead-code low user/lib/settings.lua:630 ~8L
  settings.name_of (the name of a choice's current value) is called only by test_settings.lua.
  -> Remove, or use it in preferences.lua where a dropdown's label is composed
[189] dead-code medium user/lib/eu.lua:545 ~14L
  eu.encode_line (assemble one GPU instruction alone) has no production caller. Only test_eu.lua uses it.
  -> Keep if it is a deliberate test seam, otherwise move it into the test
[190] duplicate low user/lib/mailcompose.lua:274 ~30L
  Three Lua copies turn HTML into text with chained gsubs: mailcompose.lua:274-280, mail.lua:557-569 (html_text) and automate.lua:432-435 (words_of). They already disagree: mail removes <script> and <tr>, mailcompose does not, and automate decodes only &nbsp; and &amp;. Separately, the Mail Kit's C preview (user/kits/mail/mime.c ~
  -> Merge into one door, for example markup.text(html) beside markup.escape, or a Mail Kit call that already does it in C
[191] duplicate medium user/init/init.lua:7441 ~120L
  The runner (around 7380-7500) and the shell's run_program (around 5455-5535) each turn a program's `kosmos: needs` into spawn flags and appended caps with *_at indices, in two hand-written parallel blocks. They have already diverged: windowserver is handled only in the shell's copy, and the caps are appended in a different order
  -> Merge into one function (needs, held caps) -> flags, caps, places, used by both paths
[192] duplicate low user/init/init.lua:7363 ~25L
  The 25-line comment 'The screen to everything, which is wrong and is staying for now...' appears word for word at 5414 and at 7363. It also says 'the four programs that draw (`wm`, `deskbar`, `monitor`, `edit`)', but deskbar is now an app in user/bin/apps/.
  -> Keep one copy (it can go with the merged function above), the other points to it, and drop deskbar from the list
[193] duplicate low user/bin/programs/neofetch.lua:200 ~8L
  The processor's name for a person is composed in three places with different fallbacks: pulse.name() (implementer and part, trimmed), neofetch.lua:200 (implementer, part and revision, else arch) and about.lua:131 (implementer and part, else 'unknown'). pulse.lua's header says 'About and neofetch can say the name the same way', b
  -> Have neofetch and About call pulse.name()
[194] stale-comment low user/lib/markup.lua:14 ~5L
  The comment reads: 'There were two ... Kosmos Write's DOCX writer and the browser's own pages ... (`browser.lua`) ... A third caller uses this'. browser.lua is gone, and docxwrite.lua is now the only caller.
  -> Rewrite the comment, and give it a second caller by moving the HTML-to-text helper here
[195] stale-comment low user/lib/diskbench.lua:180 ~3L
  The random-write row for a filesystem says 'not yet: a write replaces the whole file'. fs.write_from_at now writes at an offset (files.lua:393 and download.lua:125 use it, and kfs.c has kfs_write_range), so the reason given is no longer true.
  -> Measure random 4 KB writes with fs.write_from_at, or rewrite the reason
[196] stale-comment low user/bin/programs/qbench.lua:2 ~10L
  The header says 'The most important number in M7' (milestones were abolished), and it explains the getattr control as 'a Lua process with a much larger live heap'. /Temporary has been ramfs.c, a C server, since September 2026.
  -> Rewrite the header and the control's rationale (keep the control)
[197] stale-comment low user/bin/programs/spin.lua:32 ~6L
  The comment cites the `cores` app's header and its buttons as the failure observed. The Processor Cores app was removed on 12 October 2026 (pulse.lua's header records it).
  -> Rewrite the paragraph without `cores`
[198] stale-doc low user/bin/programs/benchmark.lua:9 ~3L
  The header and the printed hint say 'Watch with `htop 5` or `monitor watch`'. monitor takes a number of seconds (tonumber('watch') is nil, so it runs for 600 s), and since 15 October init refuses the screen to a program while the desktop runs, so `monitor` only prints that the desktop has the screen.
  -> Point to htop and Monitor (the app) instead
[199] stale-comment low user/bin/programs/monitor.lua:19 ~3L
  The comment says '...a program that never asks still cannot be stopped from outside'. `kill` and Processes now end any process from outside (spin.lua's own header says so).
  -> Rewrite the sentence
[200] stale-comment low user/lib/pulse.lua:10 ~2L
  'Monitor and `clocks` use it; About and neofetch can say the name the same way.' In fact Processes, Monitor and clocks use it, and About and neofetch do not (see the duplicate finding on neofetch.lua).
  -> Fix along with the merge
[201] stale-comment low user/lib/http.lua:163 ~6L
  The QUIET_SECONDS comment and opts.pause's doc describe 'a window stepping a fetch between its passes (6zz l2)'. That was the old browser. Today's callers are packages.lua and mail.lua's get_many, which yield to a coroutine.
  -> Rewrite when the browser-only options are removed
[202] bug low user/bin/programs/telnetd.lua:125 ~10L
  take() parses Telnet IAC sequences one read at a time with no carry-over state for a partly received command. If a read ends just after IAC, command is nil, so the else branch consumes the IAC alone. The next read's DO/WILL byte (253/251) and its option byte are then added to the typed line as characters. If a read ends after IA
  -> Fix: keep an incomplete IAC tail in s (for example s.iac_pending) and prepend it to the next read
[203] speed-candidate low user/bin/programs/telnetd.lua:157 ~10L
  Each typed byte is appended with s.partial = s.partial .. string.char(b), and each refusal with s.out = s.out .. string.char(...). A pasted long line allocates a new string per byte, so the cost is quadratic.
  -> Measure first: find the run of plain bytes with string.find and append the slice, or collect into a table
[204] stale-comment low user/init/init.lua:4466 ~40L
  Eight tombstone comments mark code that moved to C: 'No `binfs` here', 'No `appfs` here', 'No `diskfs` here ... `kfs.lua` and `blockcache.lua` under them', 'No `devices_main`', and at 423, 4398 and 7077-7099 'No branch for ROLE_...'. They record history rather than the code, and blockcache.lua no longer exists anywhere.
  -> Merge into one short note near the role table (which C roles main.c dispatches before Lua), or drop them
[205] dead-code low user/init/init.lua:17 ~1L
  local ROLE_CLIENT = 0 is defined and never read. Role 0, the ramfs client that tests/tests.c starts with process_create(..., 0), falls through to the end of the file rather than being tested by name.
  -> Either test `role == ROLE_CLIENT or role == ROLE_CLIENT_B` before the client chunk at 7851 or keep the constant only as documentation
```
