<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Kosmos as a daily system: a review

Written on 17 November 2026, at 0.11.461. Diego asked for it the same day:
"i want to do a full review of the Kosmos OS from a UI and user
perspective, where we stand, where we are weak and where we are strong.
What needs work, rework and rethinking. What is left to be a daily driver
for me from an end user perspective. The new browser was a key element to
be able to use it daily."

This is a review from the point of view of someone using the machine, not
a code review. The question asked of every part is the same one: on an
ordinary day, using the ThinkPad T14, the Lenovo M700 or the Mac Pro 2013,
does this do what macOS, Windows or a Linux desktop would do, and if not,
what is in the way? Every judgement below names the file or document it
comes from. Each one was checked in the tree as it stood that evening, not
taken from a summary. Where the tree and a document disagree, that is
said.

The review was written by a lane beside two other workers: the main
session, on JetSki's downloads, and an office lane, on Write and the office
kits. Those two areas were changing while this was written, so what is said
about them is true as of the afternoon of 17 November.

---

## 1. In short

Kosmos is a fast, good-looking desktop with a real browser in it. It
cannot yet be someone's only computer. JetSki changed the picture more
than anything before it: the web, which is most of a working day, now
works. That means WebKit, 22 of 23 large sites inside five seconds on the
M700, YouTube with sound, HTTPS, HTTP/2 and content blocking. Around the
browser, the desktop is consistent and responsive, and most of that is
measured rather than claimed.

What stops a day on Kosmos is not one large missing subsystem. It is a set
of plain things a person meets in the first hour:

- copy and paste does not reach the browser, and carries at most 1,900
  bytes anywhere else;
- the keyboard types only US ASCII, so Spanish cannot be written;
- the M700 starts only from the Mac's boot server or from a stick;
- a Word file someone sends cannot be opened;
- a USB stick cannot be written to;
- nothing searches the disk;
- a crashed server or window manager means restarting the machine.

The laptop is further away again: it has no Wi-Fi, no touchpad, no sleep
and no lock.

**A desk machine on a wire, the M700, is about ten fixes from being
usable every day. The ThinkPad is about four large projects behind it.**
The fixes are more valuable now than any new application, and section 6
lists them in order.

---

## 2. Strong

What is genuinely good, ranked by how much a person using it would feel
it.

1. **It responds, and the proof is in numbers, not adjectives.** Several
   pieces of work have each made the machine answer faster, and each one
   was measured:
   - strict priority bands with immediate preemption, and a sixth band
     for sound (`CLAUDE.md`, *Sound is above every program*);
   - the rule that nothing on the desktop waits on a server (`design.md`
     §2);
   - the hardware cursor (`roadmap.md` 4h b);
   - JetSki's steps of 17 November.

   On the M700, the time from a wheel turn to the picture moving in
   JetSki went from 14.1 ms to 2.5 ms on a plain page (`state.md`,
   0.11.455 to 0.11.457). Groove's sound thread was never away for more
   than 14 ms with every core busy (`testing.md` 18.263). BeOS made the
   same bet, and this is the one part of it that is already won.
2. **JetSki is a real browser.** It is WebKit with WebKit2's three
   processes, running on Nebula (`docs/w7.md`). It has:
   - tabs, and Web Inspector docked in the window;
   - a padlock that explains a certificate, and a shield whose content
     blocker is on for every view (`state.md` 0.11.442 to 0.11.448);
   - Google search, YouTube with sound (`docs/media.md` M2 to M4), and
     The Verge, the New York Times and CNN opening inside five seconds
     (0.11.452);
   - Baseline and DFG JIT (0.11.449).

   It is held by `x86-jetski` and by `tools/top20.py`'s five-second
   standard. Without it Kosmos could not be used daily at all; with it,
   the web is mostly there.
3. **The desktop is one family.** Every window in 0.11.461's picture
   (`docs/screenshots/2026-11-17-1908-99eb7c23.png`) uses the same
   pieces:
   - the same header and the same type: IBM Plex, at a scale from 100 to
     200 per cent (`ui.md` 16.18, 16.20, 16.25);
   - the same sidebar in Tracker, Preferences, Passwords and Sandboxes
     (`ui.sidebar`).

   Preferences is one place, built from one table (`user/lib/settings.lua`).
   Several of the pieces people miss when they leave macOS are already
   built:
   - Shortcuts lists every system key and lets them be changed
     (`docs/shortcuts.md`);
   - Magnet snaps windows to halves (`ui.md` 16.31);
   - Super+Tab is the application switcher (`user/lib/wm/switcher.lua`);
   - the launcher pad shows a section of recently used applications;
   - notifications have a history and Do Not Disturb (`notify.c`,
     `notifications.lua`);
   - captures save the screen, with PrtSc and Super+Shift+3 for a picture
     of the screen (`wm/keys.lua`).
4. **A desktop that works with the network around it.**
   - SMB in both directions: Kosmos mounts the Mac's shares, and offers
     its own folders to the Mac (`docs/sharing.md`, `smbd.c`, `smbfs.c`).
   - Packages installs, updates and removes applications from a server,
     with their dependencies (`docs/packages.md`).
   - A keyring holds passwords, and the Passwords window shows them.
   - Mail works with Gmail over IMAP and posts a banner when mail arrives
     (`maild.lua`).
   - Telnet, VNC and the web server let the Mac reach the machine.
   - Automations can drive any window by its controls' names
     (`docs/automations.md`), which no mainstream desktop offers.
5. **It is built as BeOS should have grown up, and it can be read.**
   - Capabilities, with no ambient authority.
   - Kits that applications share. The PDF Kit, for example, serves the
     PDF Viewer, Write's export and Shortcuts' cheat sheet.
   - Live folders, through pulses (`docs/pulses.md`, P4).
   - Attributes and queries in the filesystem.
   - Every application designed in four documents before any code is
     written.

   The result is that a fault, once seen, is usually found and fixed the
   same day. JetSki's 46 WebKit patches (`runtime/patches/webkit/`) are
   the evidence: each one names one fault and one fix. For a person who
   will use the machine every day, that is the strongest promise there
   is.

Less central, but true:

- The amount of media and creative work already on the machine:
  - Groove, a drum machine and synthesiser;
  - Cafesa3D, with renders on the GPU;
  - Doom, Quake and the Super Nintendo, each in its own installed image;
  - Xavier, an assistant answering from a model run on this machine.
- This Machine and the Monitors explain the hardware better than macOS's
  System Information does.

---

## 3. Weak

What would hurt in an ordinary day, ranked by how often and how badly it
would be met. The first five would each be felt within the first hour.

1. **Copy and paste.**
   - **Text cannot be copied out of a web page, or pasted into one.**
     JetSki's pasteboard is a stub that holds nothing and drops every
     write (`runtime/ports/webkit/Source/WebCore/platform/kosmos/PasteboardKosmos.cpp`:
     "every pasteboard is empty"). It is on the roadmap under *The
     clipboard* in the WebKit list (`roadmap.md`, "the pasteboard as the
     window manager's clipboard, copy and paste in a page (W7)").
   - **Everywhere else, a copy is cut at 1,900 bytes, and it is text
     only** (`user/lib/wmproto.lua`, `CLIP_MAX = 1900`; `ui.md` 16.11).
     About two paragraphs fit. No picture, no formatted text and no file
     can cross between applications.
   - Without these, every daily task that crosses an application becomes
     manual: a password from Passwords into a login form, an address from
     Mail into Maps, a quotation from a page into Write.
2. **Spanish cannot be typed.**
   - The keymap is one US table of 128 bytes, compiled into the kernel's
     HAL (`hal/keys.c`, `keymap_plain` and `keymap_shift`).
   - There are no layouts, no dead keys and no AltGr: no á, é, í, ó, ú,
     ñ, ü, ¿ or ¡, and no euro sign.
   - The office lane plans a Spanish dictionary for the Spelling Kit
     (`roadmap.md`, *Being built now*, O3), which would check words that
     cannot be typed.
   - On the roadmap as *Typing every language*.
3. **The machine does not start by itself.**
   - The M700 boots from the Mac: `dnsmasq`, started by Diego with `sudo`
     (`tools/netboot-serve.sh`), or from a stick, whose development
     builds change every revision.
   - `/Home` already lives on the M700's own SATA disk (A3, 0.11.438).
     The kernel and the loader do not.
   - There is no installer, and no way to update the system that can go
     back to the last working version.
   - A computer that needs a second computer to boot is a test rig, not a
     daily machine.
4. **Other people's files.**
   - Write opens only `.write` (`writer.lua`, `kosmos: opens write`). A
     DOCX that arrives by mail cannot be read; the office lane plans DOCX
     in for Write's R3 (`docs/write.md`).
   - There is no spreadsheet application (`docs/sheets.md`, not started),
     and no ODT, XLSX or PPTX.
   - Each media application opens fewer formats than its kit decodes:
     - Music opens `mp3 wav` (`music.lua`), while the Media Kit decodes
       FLAC, AAC, Opus and Vorbis (`docs/media.md`);
     - Video Player opens `mp4` (`video.lua`);
     - Photo opens `png jpg jpeg` (`photo.lua`), so no GIF, WebP or HEIC
       from a phone.
5. **The browser's daily basics, and the web platform underneath.**
   - Downloads are being built today.
   - These are drawn and agreed, but not built (`state.md` 0.11.461,
     `docs/webkit.html`):
     - find in the page;
     - bookmarks beyond favourites;
     - history, and an address field that completes from it;
     - the right button's menu;
     - Save page;
     - print to PDF;
     - restoring the session after a stop.
   - These are missing from the platform (`roadmap.md`, *The browser*):
     - WebAssembly, WebGL and Web Audio, which Google Docs, Sheets, Figma
       and many sites assume;
     - WebRTC, needed by Meet, Zoom and Teams in a browser;
     - DRM (EME), needed by Netflix, Spotify and Disney+.
   - Gmail's web page reported "Cookies are disabled" on the M700
     (`state.md`, 15 November, open).
6. **USB drives.**
   - Drives are read only, and FAT16 and FAT32 only (`user/servers/drives.c`:
     "every volume on every drive, read only").
   - exFAT, which is what every SD card and stick over 32 GB comes
     formatted as, was planned as step 6f and not built: `fat_decode.c`
     has no exFAT.
   - NTFS is listed but not opened.
   - A stick cannot be written to. Phones, which use MTP, and cameras
     cannot be read.
7. **The laptop.**
   - The T14 has no Wi-Fi: the AX201 has no driver
     (`roadmap.md` *Wi-Fi*). It has no Ethernet port either, so its only
     network is a USB-C Ethernet adapter.
   - The touchpad, which is I2C-HID (`docs/thinkpad.md`), has no driver,
     and nothing on the roadmap asks for one.
   - There is no sleep (*Sleep and wake, and power*), so closing the lid
     does nothing useful.
   - Its stable build is 0.11.149 at the newest. The 0.11 desktop and
     JetSki have been built and tuned on the M700.
8. **Crashes.**
   - A server that dies is not started again. `init` says that it ended
     and nothing more (`user/init/init.lua`, the supervision comment).
     The plan for this has been on the roadmap since 2 October (*Servers
     and drivers restarted*). In a microkernel, this is the feature that
     sets it apart from a monolithic kernel, and the user does not have
     it yet.
   - If the window manager dies, every window goes with it, and the
     console it leaves does not answer (`roadmap.md`, *Known and
     unexplained*, first entry). The only remedy is a reboot.
   - JetSki shows why a page stopped. Other applications simply vanish.
9. **Finding things.**
   - Tracker's search filters the folder on screen. A `name:value` query
     searches attributes. A name cannot be searched across the disk,
     because names are not indexed (`tracker.lua`, lines 536 to 563).
   - The launcher pad finds applications only. The Spotlight-like search
     Diego asked for on 8 October is "WANTED next" (`roadmap.md`, the
     launcher's step 4).
   - Mail searches only mail already on the machine.
10. **No lock, no login, and the machine open to the network.**
    - There is no lock screen and no login, and `/Home` is not encrypted
      (*Login, lock screen and encryption at rest*).
    - Telnet, which has no login, and VNC both start with the machine by
      default (`roadmap.md`, *Every server starts with the machine by
      default*, 8 November). Anyone on the same network can run commands
      and see the screen.
    - That is acceptable for a test machine at home. It cannot be the
      default on a laptop on public Wi-Fi.
11. **Memory over a day.** Freed pages are not given back to the system.
    Without a pressure signal, WebKit's cached processes leave only on
    their timers, and there is no test that checks for leaks
    (`roadmap.md`, *Memory returned*). Most of the browser's memory is in
    those cached processes, and the browser is open all day.
12. **Bluetooth** goes as far as finding devices nearby (B4,
    `docs/bluetooth.md`). Pairing a keyboard is B5, planned. There is no
    Bluetooth audio. The roadmap's *A day on Kosmos* paragraph lists
    "Bluetooth keyboards and mice" as available today, which overstates
    it.
13. **Displays.**
    - There is one screen.
    - The resolution is chosen at boot ("The largest the firmware
      offers, or video=WxH at boot", `settings.lua`).
    - There is no second monitor, and nothing happens when one is plugged
      in (*Displays*).
14. **Calendar, contacts and notes do not exist** (`roadmap.md`,
    *Calendar*, *A Notes app*). There is no printing.

---

## 4. Work, rework, rethink

### Work: unfinished, on the right track

- **JetSki's daily features:** downloads, then the right button, find,
  Save page, the menu, bookmarks and history, print, and the page source.
  The order is agreed (`state.md` 0.11.461, `docs/webkit.html`). Then
  session restore (`w7.md` W7d).
- **The web platform:** WebAssembly, WebGL, Web Audio and WebRTC
  (`roadmap.md`, *The browser*; `w7.md`; `media.md`).
- **The office applications:** Write R1 to R3, the Spelling Kit, and Mail
  N1 to N5 (`docs/office.md`, `write.md`, `mail.md`; being built now).
  Mail's own list of gaps is honest and complete (`mail.md`, *What makes
  it too basic*).
- **Mail's HTML on WebKit** (`docs/mailweb.md`), replacing NetSurf.
- **Video, M6:** every container and codec, in the Video application
  (`media.md`).
- **The USB filesystem steps:** exFAT (6f), then writing; NTFS read only
  (`roadmap.md`, *NTFS, read only*).
- **Bluetooth B5 and onwards** (`docs/bluetooth.md`). B3, the Intel
  firmware, is BLOCKED on Diego.
- **Supervision:** servers and drivers restarted, steps 1 to 5. The
  kernel half already exists: callers are answered `IPC_ERR_GONE`
  (`kernel/ipc.c`). The `init` half does not.
- **Memory returned:** `malloc`'s free pages given back, a pressure
  signal, and a leak test (`roadmap.md`, under the browser's memory).
- **The launcher's search over files** (`roadmap.md`, launcher step 4,
  "WANTED next").
- **Sleep, Wi-Fi, lock and login, updates with a way back.** These are
  listed and ordered in *A daily driver - BeOS reimagined for this era*,
  and none of them has been started.

### Rework: built, but needs doing again

- **The clipboard** (`user/lib/wmproto.lua`; `ui.md` 16.11).
  - Today it is a string inside a 2 KB message, held by the window
    manager. The 1,900-byte cap was argued for as "the honest edge" of a
    message, and that was the wrong conclusion.
  - Copying is a one-shot action, but what is copied can be a page of
    text or a picture. By the system's own rule, data goes in a region
    and the message only says where (`CLAUDE.md`, *Control by message,
    data by shared memory*).
  - Wanted instead: typed contents (plain text, rich text or HTML, a
    picture, a list of files) in a region, with no cap, and with
    WebKit's pasteboard and drag and drop on the same door.
- **The Control-W prefix** (`user/lib/wm/keys.lua`; `docs/shortcuts.md`).
  - It was made for a virtio keyboard with "no Alt, no Super". Super and
    Alt have existed since October.
  - It still takes Control-W, which is the key to close a tab or a window
    on every other system.
  - It still ends the desktop with Control-W then Q.
  - It is why JetSki closes a tab only with the mouse (`jetski.lua`, the
    header comment).
  - Window moves, cycling and Quit can be Super chords. Control-W should
    go back to the applications.
- **Tracker's search** (`tracker.lua` 536-563). It works in two modes in
  one box, and names are not in the index. It should be redone on the
  file index the launcher needs as well: one door, used by Tracker, the
  Open window, the launcher pad and Mail.
- **The Deskbar's strip once many windows are open.** In 0.11.461's
  picture, with every application open, each button is an icon followed
  by "...". Most of what is on screen is noise. Windows should be grouped
  by application, and a count or a list should replace the labels when
  space runs out (`deskbar.lua`).
- **Formats chosen by the application instead of by the kit.** Music,
  Video and Photo list their formats in a header line (`kosmos: opens`).
  The Media Kit and the picture kits should say what they decode, and the
  file types should follow from that. Today the kit can decode a format
  that no application will open.
- **Paint** (`paint.lua`). It is a demonstration of the graphics path. It
  has no Save, no Open, no undo, and `c` clears the canvas. It is listed
  among the applications beside Write and JetSki. Either it is rebuilt as
  the roadmap's *Paint, redesigned as a serious image editor*, or it
  moves to Demos until then.
- **Displays.** The resolution is fixed at boot. Changing it, and taking
  a second screen, need the window server to rebuild its output while
  running (`docs/astra-display.md`).
- **Write's text engine inside `writer.lua`.** The office lane's O1 is
  already moving it into kits (`docs/office.md`).
- **The roadmap's *A day on Kosmos* paragraph** (`roadmap.md`, *A daily
  driver*) claims "Bluetooth keyboards and mice" and "a desk machine on a
  wire can already spend a day here". Per section 3 of this review,
  neither is true yet. The paragraph should say what is true.

### Rethink: the design itself is wrong, or missing

- **How a key reaches an application** (`hal/keys.c`; `wm/keys.lua`;
  `roadmap.md`, "Kosmos's keyboard says a key, not when it was let go").
  - What happens now: a keypress becomes a character or a terminal
    escape sequence, through one US table, inside the kernel's HAL.
    Applications never learn when a key is released.
  - That one decision is the cause of four separate gaps:
    - no layouts;
    - no dead keys or input methods;
    - web games and web applications that hold a key do not work;
    - a modifier vocabulary that had to be worked around with a prefix.
  - What a desktop needs instead:
    - key events with press and release, the physical key, the
      modifiers, and the text produced;
    - layouts and composition in user space, in the console server or
      the window manager;
    - the kernel reporting scancodes and nothing more.

  This also follows the kernel's own rule: what a key *means* is policy,
  and policy does not belong in Nebula.
- **Surviving a crash** (`design.md` §10; `user/init/init.lua`;
  `roadmap.md` *Known and unexplained*).
  - The reason to build a microkernel is that a driver or a server can
    die without taking the machine with it. Kosmos does not deliver that
    to its user yet.
  - The window manager is one Lua process. It holds the windows, the
    keys, the clipboard and the layout, and there is no recovery from
    its death.
  - The window server (`windowserver.c`) already holds the screen apart
    from it. The design question is what state has to live outside
    `wm`, so that a `wm` started again can adopt every window that is
    still running.
  - This should be answered before the window manager grows any
    further.
- **Owning the machine** (`docs/boot.md`; `roadmap.md` *The M700 booted
  over the network*).
  - Kosmos has no design for being installed: a loader on the EFI
    partition beside Windows, the system in a partition of its own, and
  two system slots, so that an update can fall back to the last version
  that booted.
  - Netboot and sticks are excellent development tools, but they are not
    how a person's computer starts.
  - *System updates with a way back* is on the roadmap. Installing is
    not.
- **A command vocabulary per application.** This is the root of the
  inconsistencies in section 7.
  - Today each application decides alone what goes in its header, what
    goes behind its dots, which keys it answers, and whether it has a
    right-button menu at all.
  - Pewter puts menus in a bar (`ui.md` 16.32). Shortcuts lists only
    system keys (`shortcuts.md`). Automations drives controls by name.
  - One declared list of an application's commands (name, key, group,
    enabled) could feed the header's dots, the right button, Pewter's
    menu bar, Shortcuts and Automations. That is BeOS's `BMessage`
    commands, done once. The office lane's `pk.toolbar` and `findbar`
    are already halfway there.
- **The person, as distinct from the program** (`roadmap.md`, *Login,
  lock screen and encryption at rest*; *Every server starts with the
  machine by default*).
  - Capabilities protect programs from each other very well. Nothing
    protects the person's data from someone at the keyboard, from
    someone on the network (Telnet with no login, and VNC, both on by
    default), or from a lost laptop.
  - The model needed is one where a session belongs to a person, the
    keyring unlocks with that person's login, and remote doors are off
    until that person turns them on.
- **Where the effort goes.** This is not a design fault in the code, but
  it is the one with the largest effect.
  - Kosmos has about 75 applications. About twenty of them are monitors,
    benchmarks or demos: twelve demos, five monitors, and the
    developer's instruments.
  - Several large new designs are agreed and waiting: Pewter, Corona,
    Maps' satellite view, AirPlay, Sandboxes' Linux, a render farm, Sheets
    and Present.
  - The daily basics above are each smaller than any of those.
  - The rule *one thing at a time* (`CLAUDE.md`) is followed. The order
    of the things, however, has favoured what is interesting over what
    is in the way.

---

## 5. A day on Kosmos

The day is on the M700, wired, which is the machine best placed for it.
The laptop's part is at the end. Each step is marked **works**, **works
badly** or **cannot**.

| When | What a person does | Mark | Why |
|---|---|---|---|
| 07:30 | Turns the computer on | works badly | The M700 needs the Mac running `netboot-serve.sh` with `sudo`, or a stick (`roadmap.md`, netboot). `/Home` is on its own disk since A3. |
| 07:31 | Logs in | cannot | There is no login, so the desktop opens for whoever is there. |
| 07:35 | Reads mail | works badly | Mail connects to Gmail over IMAP and banners new mail (`maild.lua`). There are no conversations, no Move To, no choosing several messages, and no keys (`mail.md`). HTML is drawn by NetSurf until `mailweb.md` W6. |
| 07:50 | Replies in Spanish | cannot | The keyboard has no á, ñ or ¿ (`hal/keys.c`). |
| 08:00 | Reads the news | works | JetSki: 22 of 23 large sites within five seconds, the content blocker on, scrolling at 2.5 to 5 ms (`state.md` 0.11.452 to 0.11.460). |
| 08:10 | Copies a paragraph from an article into a note | cannot | JetSki's pasteboard is empty (`PasteboardKosmos.cpp`), and there is no Notes application. |
| 08:20 | Searches a long page for a word | cannot | Find in the page is drawn but not built (`webkit.html`). |
| 08:30 | Watches a YouTube video | works | Picture and sound (`media.md` M4). Hours of it have not been measured for memory growth (`roadmap.md`, *Memory returned*). |
| 09:00 | Opens a DOCX contract from mail | cannot | Write opens `.write` only. DOCX in is R3 (`write.md`). |
| 09:15 | Reads a PDF from mail | works | The PDF Viewer, on the PDF Kit (`pdfview.lua`). |
| 09:30 | Writes a two-page letter | works badly | Write lays out pages, does styles in part, and exports PDF and DOCX. There is no spelling, no find and replace, and no recovery after a crash (`office.md`). Accents cannot be typed. |
| 10:00 | Edits a shared Google Doc or Sheet | cannot | Google Docs needs WebAssembly and more. Gmail's page said its cookies were disabled (`state.md`, 15 November). There is no Sheets of Kosmos's own. |
| 10:30 | Joins a video call | cannot | There is no WebRTC and no microphone path (`roadmap.md`, *Video calls*). |
| 11:00 | Downloads a file and finds it | works badly | Downloads are being built today, into `/Home/Downloads`. Tracker shows it live (pulses P4). |
| 11:15 | Copies photos from a phone's SD card | cannot | The card is exFAT, which is not read (6f). A phone, over MTP, is not read either. |
| 11:30 | Copies a file onto a FAT32 stick for someone | cannot | `/Drives` is read only (`drives.c`). |
| 11:45 | Copies a file to the Mac | works | SMB, both ways (`sharing.md`). |
| 12:00 | Plays music while working | works badly | Music plays MP3 and WAV, but has no library (`music.html` after VOX is not built). FLAC files are not offered. Spotify on the web needs DRM. |
| 12:30 | Takes a screenshot and pastes it into mail | works badly | PrtSc saves into Captures. A picture cannot be pasted, because the clipboard holds text only. It can be attached from the Open window. |
| 13:00 | Looks at photos | works badly | Photo zooms and shows PNG and JPEG only. There is no library and no albums (*Music and photos as libraries*). |
| 14:00 | Looks for last week's file by name | cannot | There is no search across the disk (`tracker.lua` 560). |
| 14:30 | Installs an application | works | Packages (`packages.md`). |
| 15:00 | Copies a password into a site's login | works badly | Passwords to a kit field works up to 1,900 bytes. Into JetSki, it cannot. |
| 15:30 | An application stops responding | works badly | Super and a drag moves its window. Q in the Super+Tab switcher quits it, and Processes can end it. If the application was a server, its callers are answered `IPC_ERR_GONE` and nothing restarts it. |
| 16:00 | The window manager crashes | cannot | The whole desktop is lost and the console does not answer (`roadmap.md`, *Known and unexplained*). |
| 16:30 | Changes the screen's resolution | cannot | The resolution is set at boot (`settings.lua`). |
| 17:00 | Plugs in a second monitor | cannot | There is one display (*Displays*). |
| 17:30 | Pairs Bluetooth headphones | cannot | Bluetooth finds devices nearby and does nothing more (B4). |
| 18:00 | Takes the laptop to the sofa | cannot | The T14 has no Wi-Fi, no touchpad and no sleep. |
| 21:00 | Plays a game | works | Doom, Quake and the Super Nintendo, with game mode (`docs/scheduler.md`). |
| 22:00 | Leaves the machine on, locked | cannot | There is no lock, and Telnet and VNC are open on the network. |
| 22:05 | Shuts down | works | `\_S5` from the DSDT (`state.md`, the S5 entry). |

Of 32 steps, 7 work, 9 work badly, and 16 cannot be done. On a wired
desktop, the ten fixes in section 6 turn most of the "cannot" steps into
"works".

---

## 6. What is left to be a daily driver

Each item is marked as **BLOCKER** (Kosmos cannot be used daily without
it) or **WANTED**. The *Roadmap* column quotes the heading, or says
*new*. Sizes are rough:

- **S**: days;
- **M**: one to two weeks;
- **L**: several weeks;
- **XL**: a month or more.

The order is for the M700 first, then the laptop.

| # | Item | Kind | Roadmap | Size |
|---|---|---|---|---|
| 1 | **The clipboard redone**: typed, in a region, no cap; WebKit's pasteboard on it; a picture can be pasted | BLOCKER | the pasteboard: *The clipboard* (WebKit list, "copy and paste in a page (W7)"); the cap and types: *new* | M |
| 2 | **Keyboard layouts and dead keys**: Spanish and US International, composition in user space, key release delivered | BLOCKER | *Typing every language* ("dead keys and layouts first (Spanish)"); the input model: *new* | M |
| 3 | **Boots from its own disk**: the loader on the ESP beside Windows, the system in its own partition, two slots for an update that can go back, an installer | BLOCKER | *System updates with a way back*; installing: *new* | L |
| 4 | **JetSki's basics**: downloads (in progress), find, the right button, history and completion, bookmarks, Save page, print, session restore | BLOCKER | *JetSki finished*; `docs/webkit.html`; `w7.md` W7d | M |
| 5 | **Opening other people's documents**: DOCX in Write; every format the Media Kit decodes, in Music, Video and Photo; GIF, WebP and HEIC pictures | BLOCKER | Write R3 (*Being built now*); M6 (*Video*); the rest *new* | M to L |
| 6 | **Restarting what dies**: `init` supervising servers and drivers; a crashed application reported; the window manager able to come back | BLOCKER | *Servers and drivers restarted, from the prompt or from a system application*; *Crash recovery*; restarting `wm`: *new* | L |
| 7 | **USB drives read and written**: exFAT, then writing FAT32 and exFAT, then NTFS read | BLOCKER | USB step 6f (exFAT); *NTFS, read only*; writing: *new* | M to L |
| 8 | **Search**: names indexed in kfs, one door, used by the launcher pad, Tracker and the Open window | BLOCKER | *Search* ("the indexer and a search window"); launcher step 4 "WANTED next" | M |
| 9 | **Lock, login and remote doors off by default**: a lock screen, the keyring opened by the login, Telnet and VNC off unless turned on | BLOCKER (laptop); WANTED (desk) | *Login, lock screen and encryption at rest*; remote doors off: *new* (it reverses an 8 November decision) | M |
| 10 | **Memory returned**, and a leak test of a day's browsing | BLOCKER | *Memory returned* | M |
| 11 | **Control-W given back** to the applications; the desktop's keys on Super | WANTED | *new* | S |
| 12 | **The web platform**: WebAssembly, WebGL, Web Audio | WANTED (BLOCKER for Google Docs) | *The browser*; *JetSki finished* ("the JIT") | L |
| 13 | **Mail N1**: conversations, Move To, several chosen, keys | WANTED | Mail N1 (*Being built now*) | M |
| 14 | **Write R1**: spelling (with a Spanish dictionary), find and replace, saved copies | WANTED | Write R1 with O2 and O3 | M |
| 15 | **Wi-Fi** (AX201, iwlwifi's shape) | BLOCKER (laptop) | *Wi-Fi* | XL |
| 16 | **The T14's touchpad** (I2C-HID on LPSS) | BLOCKER (laptop) | *new* | M |
| 17 | **Sleep and wake** (S0ix or S3, every driver saving its device) | BLOCKER (laptop) | *Sleep and wake, and power* | XL |
| 18 | **Bluetooth**: keyboard and mouse, then audio | WANTED | *Bluetooth, for a mouse and a keyboard*, B5 onwards | L |
| 19 | **Displays**: resolution changed while running, a second monitor | WANTED | *Displays* | L |
| 20 | **Video calls**: WebRTC and the microphone | WANTED | *Video calls*; *Low-latency audio, in and out* | XL |
| 21 | **Calendar and contacts** | WANTED | *Calendar and contacts*; *Calendar, after Apple's* | L |
| 22 | **Music and Photos as libraries** | WANTED | *Music and photos as libraries*; *Music like VOX* | L |
| 23 | **Sheets** | WANTED | *Kosmos Sheets* | XL |
| 24 | **DRM** for Netflix and Spotify on the web | WANTED | *new*. Widevine is not licensable for a hobby OS; this is likely never, and should be written down as such | - |
| 25 | **Backups** of `/Home` | WANTED | *Backups* | M |
| 26 | **Printing** | WANTED | *Printing*, last | L |

Items 1, 2, 4, 5, 8, 10 and 11 are all small or medium and touch only
software already in the tree. Together they are perhaps six weeks of
work, and they remove most of the "cannot" steps from section 5's
morning. Items 3, 6 and 7 are larger, and they make the machine
trustworthy. 15, 16 and 17 are the laptop.

---

## 7. Consistency

These are places where applications disagree with one another. Each is
small on its own. Together they are what makes a system feel unfinished.

- **Menus and commands.** There is no single place an application's
  commands live:
  - Text Editor has full menus with keys (`texteditor.lua`: New, Open,
    Find and replace, Cut, Copy, Paste, each with "Ctrl X").
  - Write has a toolbar and an Export list (`writer.lua`: "Save as
    .write", "Export PDF", "Export Word (.docx)").
  - JetSki has a header and a menu that is drawn but not built.
  - Paint has none.
  - The header rule says three controls and the dots (`ui.md` 16.20).
    Which commands sit behind the dots differs from application to
    application.
  - Pewter has a real menu bar (`ui.md` 16.32), so the same application
    looks different in two places.
- **Find.**
  - Text Editor has Control-F with replace.
  - Write has none; its O2 is planned.
  - JetSki and the PDF Viewer have none.
  - Mail has its own search field.
  - The office lane's `pk.findbar` should be used by every one of them.
- **Tabs.**
  - JetSki: Control-T for a new tab, Super+1 to 9 to choose one, and no
    key to close one (`jetski.lua`, the header comment).
  - Terminal: Control-Shift-T (`shortcuts.md`, "as a terminal's are
    elsewhere").
  - Super is defined as the system's (`shortcuts.md`, *Global and
    local*), but JetSki uses it for its own tabs. Control+1 to 9 would
    keep the rule.
- **Undo.**
  - Write, the IDE, Cafesa3D and Passwords undo.
  - Tracker has no undo for a move or a rename (`tracker.lua` 2367,
    3417), and it is the application where undo matters most.
  - Paint has no undo.
- **Open and Save.**
  - The kit's Open and Save window (`ui.md` 16.8f) is used by Text
    Editor, Write, the PDF Viewer, Photo, Video, Mail, Cafesa3D and
    JetSki (Control-O).
  - Music has no Open. Paint cannot save. Write can save only its own
    `.write` type and exports the rest.
  - Control-S saves in Text Editor and Write. Elsewhere nothing says
    what it does.
- **The right button.**
  - There is a menu in Tracker (`filemenu.lua`), the Deskbar, the
    launcher pad, Groove, Write and Mail, though Mail's is limited
    (`mail.md`: "no right-button menu on a message").
  - There is none in JetSki, the PDF Viewer, Music or Photo, where a
    person would reach for it first.
- **"Settings" or "Preferences".**
  - The system has Preferences.
  - JetSki has its own "Settings" (S1, `state.md` 0.11.444).
  - Mail's planned window is "a Settings window of Mail's own"
    (`roadmap.md`).
  - Settings are kept in both `/Home/Preferences` and `/Home/Settings`
    (`/Home/Settings/JetSki/cookies.db`).
  - One word and one folder should be chosen.
- **Application names.**
  - "Kosmos Write" and "Kosmos IDE" carry the system's name.
  - "JetSki", "Groove", "Cafesa3D" and "Xavier" are names of their own.
  - "Paint", "Music", "Calculator" and "Video Player" are plain
    descriptions.
  - The launcher pad shows all three styles side by side. macOS's
    pattern (Pages, Music, Safari, without "Apple") would argue for
    dropping "Kosmos" from Write and the IDE.
- **Formats.** As noted in section 3: Music opens `mp3 wav`, Photo
  `png jpg jpeg`, Video `mp4`, and Write `write`. The Media Kit decodes
  far more. A file Tracker shows may open in nothing, and the reason is
  different for each application.
- **Windows that draw their own header.** JetSki, Write (`pk.header`),
  Groove and Cafesa3D draw their own top band (`ui.md` 16.25). It
  matches the kit's in most looks. Each one had to be taught separately
  to repaint when the look changes. Groove deliberately has a look of its
  own, which is right for an instrument and should be stated as an
  exception.
- **Notifications.** Mail, JetSki, Cafesa3D, Automations and game mode
  post banners. Packages, which installs and updates, and Drives, when a
  stick arrives, do not. That stick banner was named as one of the first
  uses when notifications were designed (`roadmap.md`, *Notifications*,
  "a USB device arriving").
- **Wording.** Kosmos's voice is consistent and plain, and that is a
  strength. One drift: the Deskbar, the strip, the dock and the bar are
  four names for overlapping things (`ui.md` 16.13 "One bar: the Deskbar
  is the strip"; *A dock at the bottom*). Preferences calls it "the bar".
  The documents should settle on one word.

---

## Decisions that are Diego's

Each comes with a recommendation.

1. **A daily-driver season before new applications.** Should items 1 to
   11 of section 6 go before Sheets, Present, Pewter's later steps, Maps'
   satellite view and the other agreed but unstarted designs?
   *Recommended: yes.* Do it on the M700, using it as the only computer
   for a week at the end, and fix what annoys.
2. **Which machine is the daily driver.** *Recommended: the M700 now*,
   wired and on its own SATA disk. The T14 follows once Wi-Fi, the
   touchpad and sleep are done. Those are a separate, laptop-shaped
   season.
3. **Control-W.** Retire the prefix, move its commands to Super chords,
   and let Control-W close a tab or a window as it does everywhere else.
   *Recommended: yes.*
4. **The clipboard's shape.** Typed, in a region, with no cap, and
   WebKit's pasteboard on it. *Recommended: yes.* This reverses `ui.md`
   16.11's "the cap is the honest edge".
5. **Keyboard layouts out of the kernel.** Should Nebula report key
   positions and leave the meaning to user space, with key release
   delivered? *Recommended: yes.* And which layouts first: Diego's
   keyboards decide between Latin American, Spanish and US International.
6. **Installing Kosmos.** Should Kosmos boot from the M700's own disk,
   beside Windows, with two system slots for updates, and keep netboot
   for development only? *Recommended: yes.*
7. **Remote doors off by default.** Should Telnet and VNC be off unless
   turned on, and VNC need a password? This reverses the 8 November
   decision. *Recommended: yes for a stable build. Keep them on for
   development builds,* as `roadmap.md`'s Telnet plan first said.
8. **Restarting the window manager.** Should `wm`'s state be designed so
   that the window manager can be started again and keep every window?
   *Recommended: design it now, build it after supervision of the
   ordinary servers.*
9. **Web platform before native applications.** WebAssembly and WebGL in
   JetSki would make Google Docs, Sheets and many other web applications
   usable, months before a native Sheets could be. *Recommended:
   WebAssembly and WebGL before native Sheets.* Native Write and Mail
   continue, since they are already under way.
10. **DRM.** *Recommended: write down that Netflix and Spotify on the web
    are out of reach*, so that nobody plans around them. The way to play
    music is files and Music.
