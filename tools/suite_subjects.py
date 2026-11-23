#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""What each suite without a `watch` actually exercises: its subjects.

`tools/gate.py` runs a suite that names no `watch` on any change to the
tree, so a change to one application runs every x86 suite. This file gives
each of those suites a short list of **subjects** - what it really tests,
read off the script it runs (its parts, its phases, the programs it types,
the applications it opens) - from which the files it stands on can be
derived.

A subject is one of:

  app:NAME       user/bin/apps/NAME.lua, user/bin/apps/NAME/, or
                 user/installed/NAME/
  program:NAME   user/bin/programs/NAME.lua
  lib:NAME       user/lib/NAME (a .lua file, or a folder: lib:wm, lib:groove)
  kit:NAME       user/kits/NAME/
  server:NAME    user/servers/NAME.c
  driver:PATH    user/drivers/PATH* (driver:usb/xhci is xhci.c; driver:usb/uac
                 is every uac_ file)
  files:GLOB     anything else, as a glob from the top of the tree
  core           the suite tests the system as a whole - the kernel, boot,
                 the scheduler, memory, the whole desktop - and keeps
                 running on any change to the kernel, arch/, hal/ or init

and `tier` is "core" for a suite about the kernel, arch, hal, servers,
drivers, kits, runtime, init, the window manager or the UI kit, and "app"
for one about an application or program and what it uses.

**What is not repeated in every entry, on purpose:**

  - the suite's own scripts - its `cmd` names them, and the gate has them;
  - the pieces every window and every program stands on: the runtime, init
    and the namespace, `ui.lua`, the window manager, `gfx`, the console
    server. A suite whose point is one of those lists it; an application's
    suite that merely opens a window does not, because CLAUDE.md runs the
    whole gate when a shared piece changes ("the whole gate when something
    shared changed") - that is the deriving tool's rule to apply, not
    something to spell into two hundred lists;
  - a server the suite only passes through: every x86 suite boots the same
    image with every server started, and `/Home` on a disk is `diskfs`'s
    for half of them. A server is listed only where the suite checks what
    it does.

`python3 tools/suite_subjects.py` checks this file against `gate.py` and
the tree: every name a suite of gate's, every suite without a watch here,
every subject a path that exists.
"""

import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ---------------------------------------------------------------------------
# Bundles: the same few groups recur, so they are named once.
# ---------------------------------------------------------------------------

# The window manager and the kit a window is drawn with, for the suites
# whose point is them.
WM = ["program:wm", "lib:wm", "lib:wmproto.lua", "lib:ui.lua", "server:windowserver",
      "kit:gfx", "kit:window", "server:console"]
# Astra's own pieces: what `Astra.desktop` starts, and how a desktop is chosen.
SHELL = ["app:deskbar", "app:desktop", "app:notifications", "lib:desktops.lua",
         "files:user/desktops/*.desktop", "lib:kosmosmenu.lua", "lib:deskbarmenu.lua",
         "lib:dock.lua", "lib:theme.lua", "lib:themes.lua", "files:user/themes/*.theme"]
# The disk `/Home` lives on, for the suites that test it.
DISK = ["server:diskfs", "server:kfs", "files:user/servers/disk*", "files:user/include/diskproto.h"]
# The network stack and the cards under it.
NET = ["server:net", "kit:network", "files:user/servers/tcp_reasm.h", "files:hal/virtio/net.c"]
TLS = ["kit:tls", "kit:crypto", "files:assets/ca/*"]
# Sound, from a program's stream to the device.
AUDIO = ["server:audio", "kit:audio", "lib:audio.lua", "files:user/include/audioproto.h"]
AUDIO_X86 = AUDIO + ["files:hal/pc/hda*"]
AUDIO_ARM = AUDIO + ["files:hal/virtio/snd.c"]
# A sandbox: the monitor that runs a guest and the processor's half of it.
VMM = ["server:vmm", "files:user/servers/vmm*", "files:user/servers/vdev*",
       "files:arch/aarch64/hyp.*", "files:arch/aarch64/vcpu.c"]
SANDBOX = VMM + ["program:sandbox", "lib:sandboxes.lua"]
USB = ["driver:usb/xhci", "driver:usb/usb_decode", "files:user/include/usbproto.h"]
KEYRING = ["server:keyring", "files:user/servers/keyfile*", "program:keyring"]
SMB = ["server:smbfs", "files:user/servers/smbrpc*", "files:user/servers/smbpath*", "kit:smb",
       "program:share", "kit:crypto"]
LLM = ["server:llm", "kit:llm", "lib:llm.lua"]
MAIL = ["app:mail", "program:maild", "program:mailpass", "kit:mail", "lib:imap.lua", "lib:smtp.lua",
        "lib:pop3.lua", "lib:mailcompose.lua", "lib:mailarrived.lua", "lib:mailout.lua",
        "lib:netstream.lua"] + TLS + KEYRING
FILM = ["app:video", "lib:video.lua", "kit:playback", "kit:media"]
WRITE = ["lib:writedoc.lua", "lib:docfile.lua", "lib:richtext.lua", "lib:pageset.lua",
         "lib:pdfwrite.lua", "lib:docxwrite.lua", "lib:faces.lua", "lib:hyphen.lua", "kit:pdf",
         "lib:pdf.lua", "lib:pdfpage.lua", "kit:compress"]
AUTOMATE = ["program:automate", "lib:automate.lua", "lib:markup.lua"]
GALLERY = ["app:gallery"]


def x(*parts):
    """Bundles and subjects as one list, each once, in order."""
    out = []
    for p in parts:
        for s in ([p] if isinstance(p, str) else x(*p)):
            if s not in out:
                out.append(s)
    return out


SUBJECTS = {}


def suite(names, tier, subjects, why):
    for name in names.split():
        SUBJECTS[name] = {"tier": tier, "subjects": x(*subjects), "why": why}


# ---------------------------------------------------------------------------
# The machine itself: boots, processors, memory, the tests inside the kernel.
# ---------------------------------------------------------------------------

suite("host", "core", ["core"],
      "make host-check: every decoder, kfs.lua, the crypto, the drivers' decode halves, the "
      "GPU emulator, the LLM's dot products and greps across user/ - the whole tree on the Mac")
suite("arm-kernel x86-kernel", "core", ["core", "files:tests/*"],
      "run_tests.py: the kernel's own TAP suite (tests/tests.c) - syscalls, IPC, memory, threads")
suite("arm-sandbox", "core", ["core", "files:tests/*", "files:user/init/vmtest.c", VMM],
      "run_tests.py --virt: the kernel's TAP suite booted at EL2, so the guest test enters a guest")
suite("rebuilds", "core", ["core", "files:Makefile", "files:kernel/syscall.h"],
      "test_rebuilds.py: every object whose .d names kernel/syscall.h is rebuilt when it changes")
suite("lanes", "core", ["kit:gfx", "files:runtime/libc/*", "files:Makefile"],
      "check_lanes.py: gfx's vector rows, pack, raster and blend call no memcpy (built objects)")
suite("x86-bin", "core", ["core", "files:boot/*", "files:tools/test_efiboot.c"],
      "test_efiboot on kosmos.bin: the kernel image is one the EFI path can boot")
suite("x86-uefi", "core", ["core", "files:boot/efi/*", "files:hal/pc/loader_fb.c", "files:hal/pc/fb.c",
                           "files:tools/mkusb_image.py", "files:tools/homeimage.py"],
      "run_uefi.py: OVMF, Kosmos's loader and the image - the loader's framebuffer, /Home on the stick")
suite("x86-netboot", "core", ["core", "files:boot/efi/*", "files:tools/netboot.py", "driver:usb/xhci"],
      "run_netboot.py: PXE/TFTP loader, kernel over the network, /Home on a USB stick, long cmdline")
suite("x86-stickcheck", "core", ["files:tools/stickcheck.py", "files:tools/mkusb_image.py",
                                 "files:tools/homeimage.py", "files:boot/efi/*"],
      "test_stickcheck.py: stickcheck.py names faults made in the built UEFI image and its /Home")
suite("arm-headless x86-headless", "core", ["core", "program:hello", "program:ls", "program:apps"],
      "run_headless.py: a machine with no display still reaches a prompt and runs a program")
suite("x86-timer", "core", ["core", "files:hal/pc/timer.c", "files:hal/pc/apic*", "program:clocks"],
      "run_timer.py: the TSC and local APIC calibrations survive QEMU being stopped mid-measure")
suite("x86-virtprobe", "core", ["core", "files:kernel/vm.c", "files:arch/x86_64/vcpu.c",
                                "files:arch/x86_64/cpu.c"],
      "run_virtprobe.py: the kernel's VT-x probe says 'nothing to run a guest with' without faulting")
suite("entropy", "core", ["files:kernel/entropy.c", "files:hal/pc/entropy_bind.c",
                          "files:hal/qemu-virt/entropy_bind.c", "files:hal/virtio/rng.c", "kit:crypto"],
      "run_entropy.py: virtio-rng, RDRAND and none, through sys.entropy and crypto.random, both boards")
suite("arm-fbsize x86-fbsize", "core", ["core", "files:hal/fwcfg/*", "files:hal/pc/fb.c",
                                        "files:hal/qemu-virt/fb.c"],
      "run_fbsize.py: opt/kosmos/fb sizes the ramfb screen at boot; a bad size falls back")
suite("arm-virtio-gpu x86-virtio-gpu", "core",
      ["core", "files:hal/virtio/gpu.c", WM, SHELL, "program:monitor", "app:terminal",
       "files:assets/wallpapers/*"],
      "run_screenshot.py phases keyboard,direct,terminal,wallpapers,desktop on virtio-gpu (flush, cursor)")
suite("x86-profile arm-profile", "core", ["core", "files:kernel/profile.c", "program:profile",
                                          "files:tools/profile_report.py", "app:terminal"],
      "run_profile.py: kernel PC sampling, `profile` to /Home/profiles, the report says Lua vs C")

# x86-core: run_x86.py --parts core,power_button,battery,ethernet,ethernet_unsent,
# ethernet_pch,restart,notifications
suite("x86-core", "core",
      ["core", "files:hal/pc/ec.c", "files:hal/pc/acpi.c", "files:hal/pc/power.c",
       "files:hal/pc/s5_decode.c", "driver:power/powerbutton", "files:hal/pc/battery_decode.c",
       "server:devices", "app:deskbar", "lib:indicators.lua", "driver:net/e1000", NET,
       "program:restart", "server:notify", "program:notify", "lib:notify.lua"],
      "run_x86.py parts core (boot, SMP, memory, IRQ), power_button, battery, ethernet (e1000e "
      "MSI, unsent, PCH), restart, notifications (server + SYS_SENDER)")
# x86-storage: storage,memdisk,memory,memory_home,identity,firmware,machine_report,settings_text
suite("x86-storage", "core",
      ["core", "files:hal/pc/nvme.c", "files:hal/pc/memdisk.c", DISK, "server:ramfs",
       "files:user/servers/ramstore*", "program:diagnose", "files:hal/pc/smbios*", "program:neofetch",
       "app:about", "program:acpi", "files:hal/pc/pci.c", "files:hal/pc/devices.c", "app:machine",
       "lib:devices.lua", "lib:hardware.lua", "lib:prefs.lua", "program:save", "program:cat"],
      "run_x86.py parts storage (NVMe), memdisk, memory (>768 MB), memory_home, identity "
      "(SMBIOS/neofetch), firmware (acpi save), machine_report, settings_text (prefs as text)")
suite("x86-sound", "core",
      [AUDIO_X86, "program:beep", "program:synthstart", "kit:synth"],
      "run_x86.py parts sound (HDA tone heard), sound_slow_codec, sound_eapd, synth_start")
suite("x86-usb-1", "core",
      [USB, "driver:usb/storage_decode", "program:sticks", "program:diskbench", "lib:diskbench.lua",
       "lib:blocks.lua", "lib:gpt.lua", DISK, "program:save", NET, "program:ping"],
      "run_x86.py parts usb, usb_blocks, usb_diskbench, usb_home, usb_second_stick, "
      "usb_home_late, usb_ethernet, usb_stack (xHCI, sticks, /Home on USB, USB Ethernet)")
suite("x86-usb-2", "core",
      ["core", USB, "driver:usb/storage_decode", "driver:usb/pad_decode", "driver:usb/uac", DISK,
       "server:drives", "files:user/servers/fat_decode*", "files:user/servers/drives_decode*",
       "lib:drivelist.lua", "lib:blocks.lua", "lib:sandboxes.lua", "program:cp", "program:models",
       "program:lsusb", "program:lsdev", "files:hal/pc/pointer.c", "files:hal/pc/i8042*",
       "files:kernel/boot.c", "app:deskbar"],
      "run_x86.py parts usb_home_named, usb_sandboxes, usb_models, usb_home_large, usb_drives "
      "(FAT at /Drives), flush_refused, cmdline_long, hotplug, usb_mouse(+recovers), usb_keyboard, "
      "usb_list(+none), usb_audio, pointer (PS/2)")
suite("x86-usb-3", "core",
      [USB, "driver:usb/uac", AUDIO_X86, "program:beep", "app:mixer", "app:deskbar"],
      "run_x86.py parts usb_audio_tone, _play, _choose, _mixer, _format (USB speaker, the mix "
      "converted, devices chosen, Mixer and the Deskbar told)")

# ---------------------------------------------------------------------------
# The display harness: the whole desktop, phase by phase.
# ---------------------------------------------------------------------------

DISPLAY = ["core", WM, SHELL]
suite("arm-display-1 x86-display-1", "core",
      [DISPLAY, "program:kits", "program:latency", "app:wmlatency", "program:monitor", "program:edit",
       "app:gallery", "app:preferences", "lib:settings.lua", "app:sysmon", "lib:pulse.lua", "app:plasma",
       "app:cube3d", "lib:g3d.lua", "app:terminal", "program:hello", "lib:filetypes.lua", "app:info",
       "app:tracker", "program:play", "program:mkdir", "lib:zip.lua", "kit:compress"],
      "run_screenshot.py phases keyboard .. compress: keys, programs by name, latency, interrupt, "
      "editor, registry, widgets, scripting, idle, direct, 3d, terminal, preferences, file types, compress")
suite("arm-display-2", "core",
      [DISPLAY, "app:logview", "lib:textsize.lua", "lib:keys.lua", "driver:power/powerbutton",
       "app:gallery", "files:hal/virtio/input.c"],
      "run_screenshot.py phases log view .. drag order on ARM: log view, text size, resize, triangle, "
      "repaints, power button (driver), volume keys, compositor budget, corners, shadow, wheel")
suite("x86-display-2", "core",
      [DISPLAY, "app:logview", "lib:textsize.lua", "lib:keys.lua", "files:hal/pc/i8042*",
       "files:hal/keys.c", "app:gallery"],
      "run_screenshot.py phases log view .. drag order on x86: log view, text size, resize, triangle, "
      "repaints, unknown keys, power setting, volume keys, compositor budget, corners, shadow, wheel")
suite("arm-display-3 x86-display-3", "core",
      [DISPLAY, "lib:faces.lua", "files:assets/fonts/*", "files:assets/wallpapers/*", "app:preferences",
       "app:procs", "app:tracker", "app:calc", "app:launcheredit", "program:launcher", "program:save",
       "lib:startup.lua", "lib:panel.lua", "lib:places.lua", "lib:sidebar.lua", "lib:keys.lua"],
      "run_screenshot.py phases faces .. panel: faces, wallpapers, direct menu, super drag, no title "
      "bar, tabs, appearance, deskbar(+focus, layers), starting, desktop, places, Open/Save panel")
suite("arm-display-4 x86-display-4", "core",
      [DISPLAY, "app:machine", "app:sysmon", "app:camera", "lib:camera.lua", "driver:usb/uvc_decode",
       "driver:usb/xhci", "lib:prockind.lua", "app:gallery", "app:clock", "lib:clock-replicant.lua",
       "app:hello-win", "app:drives", "lib:drivelist.lua", "app:tracker", "lib:iconsize.lua",
       "program:pointer"],
      "run_screenshot.py phases clipboard .. icon sizes: clipboard, cpu split, monitor, camera, "
      "reaped, clicks, graphical, replicants, window_manager, drives app, scale(+changed), icon sizes")
suite("arm-display-5 x86-display-5", "core", [DISPLAY],
      "run_screenshot.py phase default look: the desktop's look on a machine with nothing saved")

# ---------------------------------------------------------------------------
# The window manager and Astra, one behaviour each.
# ---------------------------------------------------------------------------

suite("arm-windows x86-windows", "core", [WM, "app:deskbar", "app:calc", "program:tile"],
      "run_windows.py: twenty Calculators - the wm's window list paged, tile, /Running past 24 names")
suite("arm-wmwait x86-wmwait", "core", ["core", WM],
      "run_wmwait.py: the window manager sleeps to the tick a window asked for, and does not spin")
suite("x86-fieldselect", "core", [WM, "lib:keys.lua"],
      "run_fieldselect.py: ui.field's selection by double click, drag and keys, and a text emptied")
suite("x86-switcher", "core", [WM, "app:calc", "app:preferences"],
      "run_switcher.py: Super+Tab panel (lib/wm/switcher.lua) and Alt+Tab, with Calculator/Preferences")
suite("x86-shortcuts", "core", [WM, "lib:prefs.lua", "app:terminal", "app:calc", "app:shortcuts"],
      "run_shortcuts.py: the person's global keys from /Home/Preferences/shortcuts, listening windows")
suite("x86-desktops", "core", [WM, SHELL, "app:chooser", "app:menubar", "app:preferences",
                               "app:tracker", "lib:startup.lua", "lib:prefs.lua"],
      "run_desktops.py: the desktop chosen at boot, its loader and startup items, Preferences' "
      "Desktop row, Switch Desktop to Pewter")
suite("x86-platinum", "core",
      [WM, SHELL, "app:menubar", "app:calc", "files:tools/run_gallery.py", "app:tracker", "app:gallery",
       "app:music", "app:preferences", "app:terminal", "app:glgears", "app:machine", "app:procs",
       "app:sysmon", "app:netmon", "app:diskmon", "app:gpumon", "app:llmmon", "app:cube3d",
       "app:cafesa3d", "app:ide", "app:texteditor", "app:groove", "app:servers", "app:writer",
       "app:passwords", "app:maps", "app:sandboxes", "app:automations", "app:fractal", "program:tile"],
      "run_platinum.py: run_gallery's every app under Pewter places Platinum's boxes; Calculator's "
      "close, collapse and bar clicked")
suite("x86-starlight", "core", [WM, SHELL, "app:calc", "app:procs", "files:assets/wallpapers/*"],
      "run_starlight.py: Starlight's amber plate, unfurl, star, cut menu corner and nebula glass")
suite("x86-glass", "core",
      [WM, SHELL, "app:about", "app:passwords", "app:launchpad", "lib:notify.lua", "program:profile",
       "files:tools/run_gallery.py", "files:assets/wallpapers/*"],
      "run_glass.py: Liquid Glass's wallpaper glass, frosted dock, banner and window, lit rim")
suite("x86-menus", "core", [WM, SHELL, "app:menubar", "app:pdfview", "app:texteditor"],
      "run_menus.py: menus at the top of the screen under Pewter, in the window under Astra")
suite("x86-spatial", "core", [WM, SHELL, "app:tracker", "lib:iconlayout.lua"],
      "run_spatial.py: Pewter's desktop arrangement and spatial folders; Astra's Drive in Tracker")
suite("x86-magnet", "core", [WM, "app:tracker", "app:preferences", "lib:prefs.lua", "files:hal/pc/i8042*"],
      "run_magnet.py: Tracker dragged to edges and Super+arrow keys snap halves, quarters, thirds")
suite("x86-dock-1 x86-dock-2", "core",
      [WM, SHELL, "app:launchpad", "lib:launchgrid.lua", "app:calc", "app:clock", "app:preferences",
       "lib:prefs.lua", "files:assets/wallpapers/*", "program:setprop"],
      "run_dock.py --part 1|2: the Deskbar as a dock in Night, launcher pad, Kosmos menu, bar moved, "
      "wallpaper fill and centre at 1720x1440")
suite("x86-mouse", "core", [WM, "app:preferences", "lib:prefs.lua", "files:hal/pc/pointer.c",
                            "files:hal/pc/i8042*"],
      "run_mouse.py: Preferences' Mouse page - double-click span and pointer speed reach the wm and board")
suite("x86-capture", "core", [WM, "files:hal/pc/pointer.c", "files:hal/pc/i8042*"],
      "run_capture.py: pointer capture (wmproto.capture) with a PS/2 mouse: motion past the edge")
suite("x86-drawself", "core", [WM, "app:calc"],
      "run_drawself.py: every window draws its own frame; rescaled vs opened-at-150% pixel equal")
suite("x86-scene", "core", ["core", WM, SHELL, "app:calc", "app:preferences", "app:cube3d"],
      "run_scene.py: the window server composes the wm's scene - deskbar, icons, shadows, menus, scale")
suite("x86-notify", "core",
      [WM, "app:notifications", "server:notify", "program:notify", "lib:notify.lua", "app:preferences",
       "app:terminal", "app:deskbar"],
      "run_notify.py: banners, alerts, history, Do Not Disturb, Preferences' Notifications, crashes said")
suite("x86-controls", "core", [WM, "app:calc", "app:texteditor", "lib:findbar.lua"],
      "run_controls.py: every control published by name under /Running/<app>/controls (ui.lua)")
suite("arm-launch x86-launch", "core", [WM, "app:Doom", "files:user/init/elfimage.c", "files:tools/installed.py"],
      "run_launch.py: the wm keeps answering while it reads Doom's installed image a window a pass")
suite("arm-game", "core", ["kit:game", "app:solar", "lib:solar", "program:frames"],
      "run_game.py: `solar --compare` - the Game Kit's rasterizer against soft.lua, pixel for pixel")
suite("x86-game", "core", ["kit:game", "app:solar", "lib:solar", "program:frames"],
      "run_game.py: `solar --compare` - the Game Kit's rasterizer against soft.lua, pixel for pixel")

# ---------------------------------------------------------------------------
# Storage: the disk, its protocol, the filesystem's format, the drives.
# ---------------------------------------------------------------------------

suite("x86-disk", "core",
      ["core", DISK, "files:hal/virtio/blk.c", "files:tools/kfs.lua", "program:attr", "program:cat",
       "program:cp", "program:diskinfo", "program:find", "program:ls", "program:mkdir", "program:mkfs",
       "program:mv", "program:rm", "program:save", "program:touch", "program:which"],
      "run_disk.py: what is written to the disk survives a power-off; two boots of one image")
suite("arm-interchange", "core",
      [DISK, "files:tools/kfs.lua", "program:zip", "program:unzip", "program:df", "program:rm",
       "program:open", "lib:zip.lua", "lib:writedoc.lua", "kit:compress"],
      "run_interchange.py: kfs.lua put/get - files into and out of the image, zip both ways")
suite("arm-queries x86-queries", "core",
      ["core", DISK, "server:ramfs", "server:appfs", "server:binfs", "files:user/init/init.lua",
       "program:launcher", "program:kits", "lib:deskbarmenu.lua", "kit:pdf", "lib:clock.lua",
       "lib:text.lua", "lib:video.lua", "program:play"],
      "run_queries.py: attributes and live queries on both kinds of mount, the namespace's paths, "
      "/Kosmos/Apps, Kits and Deskbar listed, launchers")
suite("arm-diskwire x86-diskwire", "core", [DISK, "program:keyring"],
      "run_diskwire.py: diskfs refuses every malformed diskproto.h request by number and stays whole")
suite("arm-slowdisk x86-slowdisk", "core", [DISK, "files:hal/virtio/wait.c", "files:hal/virtio/blk.c"],
      "run_slowdisk.py: a disk throttled to 16 KB/s is waited for, and the driver stays in step")
suite("x86-ahci", "core", ["driver:storage/ahci", "server:drives", "lib:blocks.lua", "lib:drivelist.lua",
                           "lib:gpt.lua"],
      "run_ahci.py: the AHCI driver finds the q35 controller, reads the disk, serves it read only")
suite("x86-satahome", "core", ["driver:storage/ahci", DISK, "lib:blocks.lua", "program:mkfs",
                               "program:save", "program:disks", "program:diskinfo"],
      "run_sata_home.py: /Home on a SATA partition named by GUID, formatted only on `mkfs --yes`")
suite("arm-fonts x86-fonts", "core", ["kit:gfx", "lib:faces.lua"],
      "run_fonts.py: CJK faces loaded from /Home/Fonts the first time a character needs one (gfx)")
suite("arm-keyring x86-keyring", "core", [KEYRING, "program:mailpass"],
      "run_keyring.py: the keyring made, reopened and set aside across three boots; forged copy refused")
suite("arm-loader x86-loader", "core",
      ["core", "files:user/init/elfimage.c", "kit:apptest", "kit:posixtest", "files:runtime/posix/*",
       "app:Doom", "app:Quake", "app:SNES", "files:tools/protostamp.py"],
      "run_loader.py: programs from their own ELF image - apptest, posixtest, broken/stale refused, Doom")
suite("arm-tcc x86-tcc", "core",
      ["files:user/init/elfimage.c", "kit:apptest", "kit:tcc", "files:tools/protostamp.py"],
      "run_loader.py with apps-tcc: the loader's checks on images TinyCC linked")
suite("arm-tcc-2 x86-tcc-2", "core",
      ["kit:tcc", "lib:tccbuild.lua", "program:tcc", "kit:apptest", "files:tools/installed.py"],
      "run_tcc.py: C compiled and linked inside Kosmos through tccbuild.lua; errors on their line")

# ---------------------------------------------------------------------------
# The network and what speaks over it.
# ---------------------------------------------------------------------------

suite("arm-network x86-network", "core",
      ["core", NET, "files:hal/pc/virtio.c", "program:fetch", "program:httpd", "program:neofetch",
       "program:netframe", "program:netstat", "program:ping", "lib:http.lua"],
      "run_network.py: virtio-net frames in a pcap, ARP, DHCP, TCP, a page fetched, httpd served")
suite("arm-tls x86-tls", "core", [TLS, NET, "program:fetch", "lib:http.lua"],
      "run_tls.py: fetch https:// through the TLS Kit - right certificate, four refusals, roots")
suite("x86-tcploss", "core", [NET, "program:vncd"],
      "run_tcploss.py: TCP resends through opt/kosmos/net_drop, a whole screen by VNC three times")
suite("x86-tcpmtu", "core", [NET, "program:vncd"],
      "run_tcploss.py --mtu: path MTU 1400 answered with fragmentation-needed, segments made 1360")
suite("x86-tcpin x86-tcpfar", "core", [NET, "program:netspeed"],
      "run_tcpin.py (--far): out-of-order kept in the ring, window scaling; 8 MB SHA-256 whole")
suite("x86-udp", "core", [NET, "program:udp"],
      "run_udp.py: datagrams and a multicast group on a wire of its own, IGMP, ARP, echo")
suite("x86-mdns", "core", [NET, "server:mdnsd", "program:mdns", "lib:mdns.lua", "program:sharing",
                           "app:connect", "lib:netshares.lua", "files:user/servers/dnsmsg*"],
      "run_mdns.py: mdns browse/resolve against a peer, and sharing announced as Studio2 (mdnsd)")
suite("arm-share x86-share", "core", [SMB, "files:tools/smbrelay.py"],
      "run_share.py --part 1: smbfs signs into Samba - dialects, signing, sealing, the relay's bites")
suite("arm-share-2 x86-share-2", "core", [SMB, "app:tracker", "program:cat", "program:cp"],
      "run_share.py --part 2: a share is a folder under /Network, big reads, gone away and back")
suite("arm-share-3 x86-share-3", "core", [SMB, "app:tracker", "app:connect", "lib:netshares.lua",
                                          "lib:places.lua"],
      "run_share.py --part 3: Connect to Server and Tracker on the share, password field, amber band")
suite("arm-share-4 x86-share-4", "core", [SMB, KEYRING],
      "run_share.py --part 4: the keyring remembers a share's password across boots")
suite("x86-smbd", "core", ["server:smbd", "files:user/servers/smbrpc*", "files:user/servers/smbpath*",
                           "program:sharing", KEYRING, "app:servers"],
      "run_smbd.py: smbd shares /Home folders read only to smbclient; told through its own door")
suite("x86-tiles", "core", ["server:tiles", "program:tiles", "lib:tiles.lua", "kit:map", "lib:json.lua",
                            "kit:compress"],
      "run_tiles.py: /Tiles with several sources at once, cache folders, formats, slow server")
suite("arm-telnetd x86-telnetd", "app", ["program:telnetd", "files:tools/kosmos_telnet.py", "app:tracker"],
      "run_telnetd.py: telnetd and the Mac's client - output, cd, get, input, exit codes, Control-C")
suite("arm-servers x86-servers", "app",
      ["app:servers", "lib:netprogram.lua", "program:telnetd", "program:httpd", "program:vncd",
       "files:tools/kosmos_vnc.py", "app:procs", "app:calc", "app:about", "app:groove", "app:terminal"],
      "run_servers.py: the Servers window, its servers kept starting at boot, VNC looked at")
suite("x86-bluetooth", "core", ["driver:bluetooth", "driver:usb/btusb_decode", "driver:usb/xhci",
                                "program:bt", "lib:bluetooth.lua"],
      "run_bluetooth.py: the host stack over btsim through the USB driver's ring; bt scan")

# ---------------------------------------------------------------------------
# Sound, pictures and film.
# ---------------------------------------------------------------------------

suite("arm-film", "app", [FILM, "program:listen", AUDIO_ARM],
      "run_film.py: listen plays MJPEG+AAC through video.lua, held sample for sample; Video opens it")
suite("x86-film", "app", [FILM, "program:listen", AUDIO_X86],
      "run_film.py: listen plays MJPEG+AAC through video.lua, held sample for sample; Video opens it")
suite("x86-film-load", "app", [FILM, "program:listen", "program:spin", AUDIO_X86],
      "run_film.py --load: the same film under spinners in the display band")
suite("arm-media", "app", ["app:music", "lib:media.lua", "lib:wav.lua", "lib:tags.lua", "kit:playback",
                           "kit:mp3", AUDIO_ARM],
      "run_media.py: media.lua plays, seeks and finishes a tone, heard in the WAV; Music's bar seeks")
suite("arm-synth", "core", ["kit:synth", "app:groove", "lib:groove", AUDIO_ARM],
      "run_synth.py: the Synth Kit's thread plays rim shots on the beat, exports a WAV; Groove plays")
suite("arm-synth-load", "core", ["kit:synth", "app:groove", "lib:groove", "program:spin", AUDIO_ARM],
      "run_synth.py --load: the same under load, in the audio band")
suite("arm-midi x86-midi", "core", ["driver:usb/midi_decode", "driver:usb/xhci", "program:midi",
                                    "lib:midi.lua", "app:groove", "lib:groove", "kit:synth"],
      "run_midi.py: /Devices/midi with the virtual keyboard, refusals; Groove plays its pad (ARM)")
suite("arm-record x86-record", "app",
      ["app:camera", "lib:camera.lua", "kit:record", "driver:usb/uvc_decode", "driver:usb/xhci",
       "lib:video.lua", "kit:media", "program:frames"],
      "run_record.py: Camera records the test pattern to /Home/Captures; MP4 read and decoded")

# ---------------------------------------------------------------------------
# Applications.
# ---------------------------------------------------------------------------

suite("arm-write x86-write", "app", [WRITE, "kit:gfx", "files:assets/fonts/*", "program:head"],
      "run_write.py: faces' metrics, paragraphs set, a hundred pages, a PDF read three ways")
suite("arm-writeapp x86-writeapp", "app", ["app:writer", WRITE, "lib:pagedraw.lua", "lib:pagedesk.lua",
                                           "app:pdfview", "lib:filetypes.lua"],
      "run_writeapp.py: Write's page on the screen is the PDF Control-E exports, line by line")
suite("arm-editor x86-editor", "app", ["app:texteditor", "app:ide", "lib:docview.lua", "lib:textbuf.lua",
                                       "lib:keys.lua", "lib:textedit.lua"],
      "run_editor.py: keys typed into Text Editor and the IDE, saved with Control-S, read back")
suite("arm-ide x86-ide", "app", ["app:ide", "lib:lint.lua", "lib:lualex.lua", "lib:clex.lua",
                                 "lib:cdoc.lua", "lib:libdoc.lua", "lib:textsize.lua", "lib:textbuf.lua",
                                 "kit:console", "app:clock", "lib:bench.lua"],
      "run_ide.py: the IDE's editor keys, saving, project memory, Run/Stop, checking, Find a file")
suite("arm-cafesa3d-1 arm-cafesa3d-2 x86-cafesa3d-1 x86-cafesa3d-2", "app",
      ["app:cafesa3d", "kit:3d", "kit:game", "lib:k3dgpu.lua", "lib:scenefile.lua", "lib:translators",
       "lib:panel.lua", "lib:weblink.lua"],
      "run_cafesa3d.py --part 1|2: select, outline, hide, orbit, wireframe; scenes edited and saved")
suite("arm-script x86-script", "app", ["app:cafesa3d", "kit:3d", "lib:scenefile.lua"],
      "run_script.py: Cafesa3D's Script panel - runs, undo, refusals, budget, saved with the scene")
suite("arm-mail x86-mail", "app", [MAIL, NET, "lib:regions.lua"],
      "run_mail.py --part 1: imap.lua/smtp.lua against mailpeer over TLS - IDLE, CONDSTORE, STARTTLS")
suite("arm-mail-2 x86-mail-2", "app", [MAIL, "lib:automations.lua", "program:autod", "lib:automate.lua"],
      "run_mail.py --part 2: maild syncs, keeps and sends; an automation on a mail arriving")
suite("arm-mail-3 x86-mail-3", "app", [MAIL, "kit:web", "lib:webpictures.lua", "lib:http.lua",
                                       "lib:pagedraw.lua"],
      "run_mail.py --part 3: the Mail window - lists, a message's HTML laid out, Load Pictures")
suite("arm-mail-4 x86-mail-4", "app", [MAIL, "lib:addresses.lua"],
      "run_mail.py --part 4: the composer - Reply All, threading, Bcc, drafts, refused recipient")
suite("arm-mail-5 x86-mail-5", "app", [MAIL, "lib:panel.lua", "app:texteditor"],
      "run_mail.py --part 5: attachments - saved, opened in Text Editor, attached and forwarded")
suite("arm-mail-6 x86-mail-6", "app", [MAIL],
      "run_mail.py --part 6: several accounts, All Inboxes, Flagged, search, From chosen")
suite("arm-mail-7 x86-mail-7", "app", [MAIL],
      "run_mail.py --part 7: POP3 over TLS and STLS, and maild keeping a POP3 account")
suite("x86-maps", "app", ["app:maps", "kit:map", "lib:mapstyle.lua", "lib:tiles.lua", "server:tiles",
                          "program:tiles", "lib:themes.lua"],
      "run_maps.py: Maps' window, search and places, the world from a tile server, a stalled host")
suite("x86-tracker", "app", ["app:tracker", "lib:files.lua"],
      "run_tracker.py: Tracker's rows open on a double click and only on one")
suite("x86-trackerlist", "app", ["app:tracker", "lib:files.lua", "lib:filetypes.lua", "lib:prefs.lua"],
      "run_trackerlist.py: Tracker's list view on ui's columns - dates, widths kept, menus")
suite("x86-folders", "core", ["app:tracker", DISK, "server:ramfs", "files:user/servers/folderwatch*",
                              "lib:pulse.lua", "files:kernel/ipc.c"],
      "run_livefolders.py: Tracker relists a folder on the disk's and /Temporary's pulses (P4)")
suite("x86-quicklook", "app", ["app:tracker", "lib:preview.lua", "lib:pictures.lua", "lib:pdf.lua",
                               "kit:pdf", "lib:media.lua", "lib:video.lua", "app:photo", AUDIO_X86],
      "run_quicklook.py: Space in Tracker - picture, text, PDF, WAV heard, MP4 playing; Photo opens")
suite("x86-wallpapers", "app", ["app:preferences", "lib:prefs.lua", "lib:wm", "lib:panel.lua",
                                "app:tracker", "lib:places.lua", "files:assets/wallpapers/*"],
      "run_wallpapers.py: Preferences' wallpaper carousel, wm's thumbnails, Choose a picture")
suite("x86-pdfkit", "core", ["kit:pdf", "lib:pdf.lua", "lib:pdfpage.lua", "files:tools/mkpdf.py"],
      "run_pdfkit.py: the PDF Kit's text, outline, render and boxes, with controls")
suite("x86-pdfview", "app", ["app:pdfview", "kit:pdf", "lib:pdf.lua", "lib:pdfpage.lua",
                             "lib:recentdocs.lua", "files:tools/mkpdf.py"],
      "run_pdfview.py: the PDF Viewer - fit width, zoom, menus, sidebar thumbnails, find, copy")
suite("x86-automate", "app", [AUTOMATE, "app:calc", "app:texteditor", "lib:mailout.lua"],
      "run_automate.py: automation verbs - open, press, read, menu, type, mail, check, copy, notify")
suite("x86-autod", "app", [AUTOMATE, "program:autod", "lib:automations.lua", "app:calc",
                           "lib:notify.lua"],
      "run_autod.py: autod runs automations by hand, on a file appearing, failures said once")
suite("x86-automations", "app", ["app:automations", AUTOMATE, "program:autod", "lib:automations.lua",
                                 "app:calc"],
      "run_automationswin.py: the Automations window - Run now, history, switch, new, steps")
suite("arm-sysapps x86-sysapps", "app", ["app:procs", "app:sysmon", "kit:series", "lib:procshare.lua", "lib:prockind.lua",
                                         "lib:pulse.lua"],
      "run_sysapps.py: Processes' columns, files, threads, privilege and sorting; Monitor")
suite("x86-scheduler", "app", ["app:scheduler", "lib:scheduling.lua", "files:kernel/sched*",
                               "lib:wm/gamemode.lua"],
      "run_scheduler.py: the Scheduler window - lanes, Fair (round-robin), 20 ms turn, game mode")
suite("x86-netmon", "app", ["app:netmon", "kit:series", "server:net", "lib:hardware.lua"],
      "run_netmon.py: Network Monitor's rate in, busiest connection by name, mirrored graph")
suite("x86-diskmon", "app", ["app:diskmon", "kit:series", "server:drives", "lib:drivelist.lua", "driver:usb/xhci",
                             "files:tools/fatstick.py"],
      "run_diskmon.py: Disk Monitor's rate on a FAT stick, the busiest program by name")
suite("x86-gpumon", "app", ["app:gpumon", "kit:series", "program:gpustat", "program:gpu", "lib:gpu.lua",
                            "program:rendering", "lib:rendering.lua", "driver:display/intelgpu"],
      "run_gpumon.py: gpustat, the desktop's label and GPU Monitor with no Intel GPU; contexts")
suite("x86-machine", "app", ["app:machine", "lib:devices.lua", "lib:hardware.lua", "server:devices",
                             "app:deskbar", "files:hal/pc/pci.c"],
      "run_machine.py: This Machine's cards, By connection, No driver; the Deskbar's USB indicator")
suite("x86-fractal", "app", ["app:fractal", "kit:compute", "lib:eukernels.lua", "driver:display/intelgpu"],
      "run_fractal.py: GPU Fractal draws on the processor and says there is no GPU")
suite("x86-install", "app", ["program:install", "program:pkg", "lib:packages.lua", "lib:download.lua",
                             "lib:zip.lua", "lib:http.lua"],
      "run_install.py: packages listed, installed, reinstalled, a bad zip refused")
suite("x86-packages", "app", ["app:packages", "program:pkg", "lib:packages.lua", "lib:zip.lua",
                              "lib:http.lua"],
      "run_packages.py: the Packages window - update, install with a dependency, remove")
suite("arm-passwords x86-passwords", "app", ["app:passwords", KEYRING],
      "run_passwords.py: Passwords lists, sorts, shows, notes, searches and deletes keyring entries")
suite("arm-diskbench x86-diskbench", "app", ["program:diskbench", "lib:diskbench.lua", DISK],
      "run_diskbench.py: Disk Benchmark finds /Home, every row a number or a reason, file removed")
suite("arm-shell", "app",
      ["server:ramfs", "program:acpi", "program:attr", "program:df", "program:du", "program:grep",
       "program:head", "program:keys", "program:kill", "program:mkdir", "program:say", "program:stat",
       "program:tail", "program:touch", "program:tree", "program:wc", "program:which"],
      "run_shell.py: the prompt's verbs agree with each other on /Temporary")
suite("arm-nogame x86-nogame", "app", ["app:Doom", "app:Quake", "files:runtime/libc/malloc.c",
                                       "files:user/init/elfimage.c"],
      "run_nogame.py: Doom and Quake start on an empty WAD/pak and come back to Lua cleanly")
suite("x86-llm", "core", [LLM, "program:llm", "program:models", "program:ready"],
      "run_llm.py: /Running/llm answers byte for byte as the kit on the Mac; refusals, stop")
suite("x86-llm-vk", "core", [LLM, "kit:vulkan", "program:models", "program:ready"],
      "run_llmvk.py: llm-vk.elf from /Home/System/LLM, on lavapipe; F16 answers the kit's; fallback")
suite("x86-xavier", "app", ["app:assistant", "program:xavier", "lib:conversations.lua", "lib:xavierbox.lua",
                            "lib:models.lua", LLM],
      "run_assistant.py: Xavier's window - questions, follow-ups, Stop, conversations kept, settings")
suite("x86-llmmon", "app", ["app:llmmon", "kit:series", "program:llm", "program:xavier", LLM],
      "run_llmmon.py: the LLM Monitor and `llm` count askers and tokens; llm inside")
suite("x86-models", "app", ["program:models", "lib:models.lua", "lib:download.lua", "lib:http.lua",
                            "lib:json.lua", TLS],
      "run_models.py: models search/files/get from a Hugging Face stand-in, cut off and resumed")
suite("x86-modelspage", "app", ["app:assistant", "lib:models.lua", "lib:download.lua", "lib:xavierbox.lua",
                                LLM, TLS],
      "run_modelspage.py: Xavier's Models page - search, Get, Pause, Go on, Load")

# ---------------------------------------------------------------------------
# Sandboxes (ARM, at EL2).
# ---------------------------------------------------------------------------

suite("arm-sandbox-linux", "core", [SANDBOX, "files:tools/alpine.py"],
      "run_sandbox.py: Alpine in a VM - sandbox run/images, a shell, exit code, starts with Kosmos")
suite("arm-sandboxwin", "app", ["app:sandboxes", SANDBOX],
      "run_sandboxwin.py: the Sandboxes window - cards, Start, Console, Restart, Settings, New, Stop")
suite("arm-sandbox-linuxwin", "core", ["app:linux", SANDBOX, "files:user/include/vmmscreen.h"],
      "run_linuxwin.py: Linux's virtio-gpu screen in a window, pointer and keyboard into the guest")
suite("arm-sandbox-linuxsetup", "app", ["app:linux", SANDBOX, NET],
      "run_linuxsetup.py: the Linux window makes its disk, installs Alpine over the network, boots it")


# ---------------------------------------------------------------------------
# The check.
# ---------------------------------------------------------------------------

def resolve(subject):
    """The paths a subject names, from the top of the tree; empty when none."""
    if subject == "core":
        return ["(core)"]

    kind, _, name = subject.partition(":")
    found = []

    def there(*rel):
        for r in rel:
            if os.path.exists(os.path.join(ROOT, r)):
                found.append(r)

    if kind == "app":
        there("user/bin/apps/%s.lua" % name, "user/bin/apps/%s" % name, "user/installed/%s" % name)
    elif kind == "program":
        there("user/bin/programs/%s.lua" % name)
    elif kind == "lib":
        there("user/lib/%s" % name)
    elif kind == "kit":
        if os.path.isdir(os.path.join(ROOT, "user/kits", name)):
            found.append("user/kits/%s" % name)
    elif kind == "server":
        there("user/servers/%s.c" % name)
    elif kind == "driver":
        found += [os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, "user/drivers", name + "*"))]
    elif kind == "files":
        found += [os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, name))]

    return found


def main():
    sys.path.insert(0, HERE)
    import gate

    problems = []
    names = {s.name for s in gate.SUITES}
    unwatched = [s.name for s in gate.SUITES if not s.watch]

    for name in SUBJECTS:
        if name not in names:
            problems.append("%s: not a suite in tools/gate.py" % name)
        elif name not in unwatched:
            problems.append("%s: has a watch in tools/gate.py, so it needs no subjects" % name)

    for name in unwatched:
        if name not in SUBJECTS:
            problems.append("%s: a suite without a watch, and no subjects here" % name)

    for name, entry in SUBJECTS.items():
        if entry["tier"] not in ("core", "app"):
            problems.append("%s: tier %r is neither core nor app" % (name, entry["tier"]))
        if not entry["subjects"]:
            problems.append("%s: no subjects" % name)
        if not entry.get("why"):
            problems.append("%s: no why" % name)
        for s in entry["subjects"]:
            if s != "core" and s.partition(":")[0] not in ("app", "program", "lib", "kit", "server",
                                                           "driver", "files"):
                problems.append("%s: %r is no kind of subject" % (name, s))
            elif not resolve(s):
                problems.append("%s: %r names nothing in the tree" % (name, s))

    tiers = {}
    for entry in SUBJECTS.values():
        tiers[entry["tier"]] = tiers.get(entry["tier"], 0) + 1
    whole = sorted(n for n, e in SUBJECTS.items() if "core" in e["subjects"])
    subjects = {s for e in SUBJECTS.values() for s in e["subjects"]}

    print("suites: %d in gate.py, %d without a watch, %d here" % (len(names), len(unwatched), len(SUBJECTS)))
    print("tiers: %s" % ", ".join("%s %d" % kv for kv in sorted(tiers.items())))
    print("distinct subjects: %d" % len(subjects))
    print("marked `core` (whole system, %d): %s" % (len(whole), " ".join(whole)))

    if problems:
        print("\nFAIL: %d problem(s)" % len(problems))
        for p in problems:
            print("  " + p)
        return 1

    print("PASS: every suite without a watch has subjects, and every subject names a path")
    return 0


if __name__ == "__main__":
    sys.exit(main())
