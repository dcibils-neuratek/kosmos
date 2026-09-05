# Kosmos
#
# make qemu     build and run under QEMU virt, with a window
# make serial   the same, serial only, no window
# make test     run the suite under QEMU, exit code 0 or 1
# make screenshot  boot, screendump, and check the picture QEMU scans out
# make check-lua   parse every .lua file and check the globals it reads
# make debug    the same as qemu, stopped, with a gdbserver on :1234
# make clean

# Stated rather than left to fall out of rule order. Make takes the first
# explicit target it sees as the default, and adding a rule above `all`
# silently changes what a bare `make` builds.
.DEFAULT_GOAL := all

CROSS   := aarch64-none-elf-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
OBJCOPY := $(CROSS)objcopy

SIZE    := $(CROSS)size

# Which of the three images is being built. The test and bench builds each
# carry a Lua chunk the shipping one must not, so their user images are
# different binaries and cannot share a directory or a generated .c with it -
# `make` and `make test` would trade stale ones back and forth.
VARIANT := $(if $(TEST),-test,$(if $(BENCH),-bench,$(if $(DOOM),-doom)))

# Where generated sources go. Defined here rather than beside the rules that
# produce them, because SRCS below is a := assignment and expands it on the
# spot; further down it would expand to nothing and the object would be
# named build//init_bin.c.o.
# The display size, as `make FB=1920x1080`.
#
# These two constants are used in one file and nowhere else, so they go on
# that file's compile line rather than into CFLAGS. Putting them in CFLAGS
# was easier and wrong: every object depends on the flags it was built with,
# so changing the screen size rebuilt the kernel, the interpreter and the
# whole userland - seventy-eight objects to change one number that seven of
# them have never heard of.
ifdef FB
FB_FLAGS := -DFB_WIDTH=$(firstword $(subst x, ,$(FB))) \
            -DFB_HEIGHT=$(word 2,$(subst x, ,$(FB)))
endif

# What this build calls itself.
#
# VERSION is edited by a person. The rest is worked out here: the commit if
# there is one, and the date of that commit rather than the date of the
# build - so an unchanged tree produces an unchanged string, and `make` on a
# machine that has nothing to do still has nothing to do.
# major.minor.revision, from the VERSION file so that bumping it is an edit
# to one line and not a search.
#
#   revision   every push
#   minor      every milestone
#   major      when we decide something was big enough
#
# `make bump`, `make bump-minor` and `make bump-major` move it.
VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0)

# The two names, which are not the same thing and were one for too long.
# Kosmos is the operating system - the servers, the desktop, the userland.
# Nebula is the microkernel underneath it: threads, address spaces, IPC,
# capabilities, and nothing else.
OS_NAME     := Kosmos
KERNEL_NAME := Nebula


GEN := build/gen$(VARIANT)

# The host-side Lua checks. Up here for the same reason GEN is: the rules
# that use them are read further down, and a target line is expanded when
# make reads it, so a variable defined below would be empty there.
HOST_CC := cc
HOSTDIR := build/host

# The library, without the two mains and without linit's open-everything.
LUA_HOST_SRCS := $(filter-out lua/upstream/lua.c lua/upstream/luac.c \
                              lua/upstream/linit.c, $(wildcard lua/upstream/*.c))

LUA_FILES := user/init/init.lua $(wildcard user/bin/*.lua) \
             $(wildcard user/lib/*.lua) $(wildcard user/tests/*.lua)

SRCS := boot/start.S \
        arch/aarch64/vectors.S \
        arch/aarch64/trap.c \
        arch/aarch64/mmu.c \
        arch/aarch64/cpu.c \
        arch/aarch64/switch.S \
        arch/aarch64/fp.S \
        arch/aarch64/fp.c \
        arch/aarch64/el0.S \
        hal/qemu-virt/uart.c \
        hal/qemu-virt/memory.c \
        hal/qemu-virt/gic.c \
        hal/qemu-virt/timer.c \
        hal/qemu-virt/rtc.c \
        hal/qemu-virt/power.c \
        hal/qemu-virt/snd.c \
        hal/qemu-virt/fwcfg.c \
        hal/qemu-virt/fb.c \
        hal/qemu-virt/input.c \
        hal/qemu-virt/blk.c \
        kernel/console.c \
        kernel/screen.c \
        kernel/boot.c \
        $(GEN)/version.c \
        $(GEN)/font_8x16.c \
        runtime/libc/string.c \
        runtime/libc/setjmp.S \
        kernel/panic.c \
        kernel/pmm.c \
        kernel/thread.c \
        kernel/sched_rr.c \
        kernel/sched_prio.c \
        kernel/ipc.c \
        kernel/memobj.c \
        kernel/process.c \
        kernel/syscall.c \
        kernel/main.c \
        $(GEN)/init_bin.c

# What is not in that list, and why.
#
# **No Lua.** `CLAUDE.md` has said since the start that the kernel has none
# inside it from M4 onward, and until this commit that was simply untrue: the
# interpreter was most of the image, reachable from no code path, kept alive
# only because the tests drove the kernel through it. Those tests run at EL0
# now, where Lua does, and .text went from 204,800 bytes to 20,480.
#
# **No malloc, no math, no snprintf, no strtod, no stdio.** Every one of them
# was here for Lua. Nothing in kernel/, arch/ or hal/ allocates - the kernel's
# state is fixed pools - and a float anywhere in it is a bug that
# -mgeneral-regs-only turns into a compile error. The test image links them
# back for its own unit tests, which is where they are exercised.
#
# **No -lm**, for the same reason, and it goes back only in the test build.
#
# **No user/hello.S or user/faulty.S.** Those are the fixture blobs the tests
# run at EL0 to check that a process exits, that a null dereference kills only
# it, and that a syscall refuses a kernel pointer. Nothing outside the suite
# refers to them, so they were 4 KB of the shipping image that no code path
# could reach.
#
# What stays despite being unreachable in the shipping image is
# `fault_expect_begin`/`_end` in arch/aarch64/trap.c, and setjmp.S under it.
# Only the tests and the benchmarks call them. Compiling them out would mean
# the trap handler that ships is not the trap handler that was tested, and
# that is a worse trade than a couple of hundred bytes and one predictable
# branch on a path that is already an exception.

# Upstream Lua, for the user image. The core, plus the libraries that are
# allowed to exist.
#
# What is missing is the point, and it is a security decision rather than a
# build one (design.md 5.3): no liolib or loslib, because there is no global
# tree to open a path in and no wall clock; no loadlib, which wants dlopen;
# no ldblib, because debug.getupvalue breaks any abstraction built in Lua.
# linit.c is out too, since it opens all of them; user/lib/lua_glue.c opens
# ours.
LUA_SRCS := \
        lua/upstream/lapi.c     lua/upstream/lcode.c    lua/upstream/lctype.c \
        lua/upstream/ldebug.c   lua/upstream/ldo.c      lua/upstream/ldump.c \
        lua/upstream/lfunc.c    lua/upstream/lgc.c      lua/upstream/llex.c \
        lua/upstream/lmem.c     lua/upstream/lobject.c  lua/upstream/lopcodes.c \
        lua/upstream/lparser.c  lua/upstream/lstate.c   lua/upstream/lstring.c \
        lua/upstream/ltable.c   lua/upstream/ltm.c      lua/upstream/lundump.c \
        lua/upstream/lvm.c      lua/upstream/lzio.c \
        lua/upstream/lauxlib.c  lua/upstream/lbaselib.c lua/upstream/lcorolib.c \
        lua/upstream/lstrlib.c  lua/upstream/ltablib.c  lua/upstream/lmathlib.c \
        lua/upstream/lutf8lib.c

# The test build is a separate image in a separate directory. Same sources
# plus the suite, with KOSMOS_TEST defined, so the tests cost the normal
# image nothing and the two never share a stale object file.
ifdef TEST
  BUILD     := build/test
  SRCS      += tests/tests.c
  # The EL0 fixture blobs, which only the suite runs.
  SRCS      += user/hello.S user/faulty.S
  # The libc the kernel no longer links, because the unit tests for it are
  # here and they call it directly. The shipping image needs none of it.
  SRCS      += runtime/libc/malloc.c runtime/libc/misc.c \
               runtime/libc/math.c runtime/libc/snprintf.c \
               runtime/libc/strtod.c
  TESTDEFS  := -DKOSMOS_TEST -Itests
  # The user side needs the define too: main.c grows a chunk to dispatch to.
  UTESTDEFS := -DKOSMOS_TEST
else ifdef BENCH
  BUILD     := build/bench
  SRCS      += bench/bench.c
  TESTDEFS  := -DKOSMOS_BENCH -Ibench
  UTESTDEFS := -DKOSMOS_BENCH
else
  BUILD     := build
  TESTDEFS  :=
  UTESTDEFS :=
endif

TARGET := $(BUILD)/kosmos.elf

# Not to be changed without discussion. See CLAUDE.md.
#
# -mgeneral-regs-only is there because the kernel does not save FP/SIMD
# registers on a context switch. If something in the kernel needs a float,
# it is badly designed, and this turns that into a compile error rather than
# a corrupted register found three milestones later.
CFLAGS_BASE := $(if $(DOOM),-DUSER_HEAP_PAGES=3072) \
               -std=c11 -ffreestanding -nostdlib -nostartfiles \
               -Wall -Wextra -Werror -fno-common -fno-strict-aliasing \
               -O2 -g \
               -Iarch/aarch64 -Ihal -Ikernel -Iruntime/include -Iuser \
               $(TESTDEFS)

CFLAGS := $(CFLAGS_BASE) -mgeneral-regs-only

# The exceptions, and why there are any.
#
# -mgeneral-regs-only exists because the kernel does not save FP/SIMD on a
# context switch, so a float anywhere in it is a bug waiting for M3. That
# reasoning covers kernel/, arch/ and hal/, and it still does.
#
# It cannot cover code whose entire job is floating point. math.c decomposes
# doubles and snprintf.c turns them into digits, and from the next commit Lua
# is here too, whose numbers are doubles. Those files get the same flags
# minus that one, so the boundary is per file and visible rather than a flag
# quietly dropped from the whole build.
CFLAGS_FP := $(CFLAGS_BASE)

# Lua, ours and upstream's alike, needs the Lua headers and the Kosmos
# configuration forced in front of every translation unit. -include is what
# lets `lua/upstream/` stay byte-for-byte what lua.org ships: every hook it
# overrides is guarded upstream by #if !defined, so arriving first is enough
# and `lua/patches/` stays empty. Used by the user link only; the kernel does
# not compile a line of it.
LUA_FLAGS := -Ilua/upstream -Ilua/kosmos -include lua/kosmos/kosmos_lua.h

# Upstream is compiled without -Werror. It is not our code and its warnings
# are not ours to fix: the alternative is either editing it, which setup.md
# forbids for a good reason, or carrying a patch that has to be rebased on
# every release. Our own files keep -Werror, Lua's included.
CFLAGS_LUA_UPSTREAM := $(CFLAGS_FP) $(LUA_FLAGS) -Wno-error


$(BUILD)/runtime/libc/math.c.o:     CFLAGS := $(CFLAGS_FP)
$(BUILD)/runtime/libc/snprintf.c.o: CFLAGS := $(CFLAGS_FP)
$(BUILD)/runtime/libc/strtod.c.o:   CFLAGS := $(CFLAGS_FP)
$(BUILD)/$(GEN)/init_bin.c.o:        CFLAGS := $(CFLAGS_BASE)
# tests.c keeps the FP flags: it asserts on doubles, and one of its tests is
# that FP is usable at EL1 at all.
$(BUILD)/tests/tests.c.o:            CFLAGS := $(CFLAGS_FP)

# --build-id=none keeps a .note section out of an image that has no loader
# to read it.
#
# --no-warn-rwx-segments: the image is one ELF segment marked read, write and
# execute. The warning is aimed at userland binaries, where those flags are
# what the loader enforces. Here nothing reads them: there is no loader, QEMU
# copies the image in and jumps to it.
#
# The permissions that are real are the ones in the page tables, and since
# M1 they are enforced: .text is read-only and executable, .rodata read-only
# and never executable, everything else writable and never executable, with
# tests that assert a write to each of the first two faults. Splitting the
# ELF into matching segments would only make the file describe what the MMU
# already does.
LDFLAGS := -T boot/kosmos.ld \
           -Wl,--build-id=none \
           -Wl,--no-warn-rwx-segments \
           -Wl,-Map,$(BUILD)/kosmos.map

# newlib's libm, for the maths that are numerical analysis rather than bit
# manipulation: pow, fmod, sqrt and the transcendentals. The easy half is
# ours, in runtime/libc/math.c. See math.h for where the line is and why.
# Its only demand is __errno, which the design wanted per-process anyway.
LIBS := -lm

# The kernel links none of it. It has no floats by construction, and the one
# caller it used to have was Lua. The test image gets it back because the
# unit tests for our half check it against newlib's.
KLIBS := $(if $(TEST),-lm)

OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SRCS)))

DEPS := $(OBJS:.o=.d)

# ------------------------------------------------------------------
# The user side.
#
# A separate link, at a separate address, with a separate C library
# instance. Sharing the sources and not the objects is the point: the same
# libc is correct on both sides of the boundary and panic() means something
# different on each, so it is compiled twice rather than linked once.
#
# A distinct top-level directory rather than build/user, because
# build/user/x.c.o would also match the kernel's build/%.c.o pattern and
# which rule won would depend on how make breaks ties.
# ------------------------------------------------------------------

# Varies with the image for the same reason GEN does: the test and bench
# init.bin files are different binaries, and sharing a directory would mean
# sharing objects compiled with different flags.
UBUILD := build-user$(VARIANT)

USER_LIBC := runtime/libc/string.c \
             runtime/libc/malloc.c \
             runtime/libc/math.c \
             runtime/libc/snprintf.c \
             runtime/libc/strtod.c \
             runtime/libc/stdio.c \
             runtime/libc/setjmp.S \
             user/lib/misc_user.c \
             user/lib/panic_user.c

TINYGL_CFLAGS := -w -Wno-error \
                 -Iruntime/upstream/tinygl/include \
                 -Iruntime/upstream/tinygl/source

TINYGL_SRCS := $(wildcard runtime/upstream/tinygl/source/*.c)

#
# TinyGL's own demos, compiled so that eight of them can share one binary.
#
# Every one defines `draw`, `init`, `idle`, `reshape`, `key` and `main`,
# because upstream builds each as its own executable. Renaming them on the
# compile line is what lets all eight link together, and it leaves
# `runtime/upstream/tinygl/examples/` byte for byte as released - which
# patching them would not.
#
TINYGL_DEMOS := bounce cube gears mech morph3d spin teapot texobj
TINYGL_DEMO_SRCS := $(patsubst %,runtime/upstream/tinygl/examples/%.c,\
                                $(TINYGL_DEMOS))

# The rename, as a function of the demo's name.
tinygl_rename = -Ddraw=$(1)_draw -Dinit=$(1)_init -Didle=$(1)_idle \
                -Dreshape=$(1)_reshape -Dkey=$(1)_key -Dmain=$(1)_main

USER_SRCS := user/init/start.S \
             user/init/main.c \
             user/servers/audio.c \
             user/servers/devices.c \
             user/servers/binfs.c \
             user/servers/appfs.c \
             user/servers/console.c \
             user/servers/ramfs.c \
             user/lib/lua_glue.c \
             user/lib/sys_user.c \
             user/lib/gfx.c \
             user/lib/png.c \
             user/lib/docfont.c \
             user/lib/inflate.c \
             user/lib/pdftok.c \
             user/lib/gl_kosmos.c \
             user/lib/con_kosmos.c \
             user/lib/mp3_kosmos.c \
             runtime/upstream/puff/puff.c \
             $(TINYGL_SRCS) \
             $(TINYGL_DEMO_SRCS) \
             user/lib/gl_demos.c \
             $(GEN)/font_8x16.c \
             $(GEN)/programs.c \
             $(GEN)/version.c \
             $(GEN)/assets.c \
             $(GEN)/fonts.c \
             runtime/upstream/stb/stb_impl.c \
             $(GEN)/libraries.c \
             lua/kosmos/serialize.c \
             $(USER_LIBC) \
             $(LUA_SRCS) \
             $(GEN)/init_lua.c

# The Lua tests and the Lua benchmarks, each in one image only. Both used to
# run inside the kernel against a lua_State it carried; they run out here now,
# because out here is where Lua is.
ifdef TEST
USER_SRCS += $(GEN)/luatest_lua.c
endif
ifdef BENCH
USER_SRCS += $(GEN)/luabench_lua.c
endif

#
# `make DOOM=1 qemu` builds an image with Doom in it.
#
# **A build option because of the licence.** Doom is GPLv2 and Kosmos is
# MIT; there is no dynamic linking here, so anything compiled in is linked
# in and an image containing Doom is a combined work under the GPL. The line
# is drawn in the build rather than in a comment, because a licence boundary
# that depends on somebody remembering is not a boundary. See
# `user/doom/README.md`.
#
# **And because of the size.** The image is copied into every process, so a
# megabyte of Doom on an eighteen-process desktop is paid for eighteen times
# by seventeen processes that will never call it.
#
# Its own VARIANT, so the objects never mix with an ordinary build's: they
# are compiled with different flags and `make` compares timestamps, not
# command lines.
#
ifdef DOOM
#
# The 79 objects doomgeneric's own Makefile names, and not one more.
#
# Listed rather than globbed, because `runtime/upstream/doom/` is upstream's
# whole tree and eight of the files in it are *other people's* platform
# layers - SDL, Xlib, Win32, Allegro. Globbing would compile all of them and
# then fail to link four different sets of missing symbols. The list is
# upstream's, from its Makefile, which is the authority on what Doom is made
# of.
#
DOOM_NAMES := dummy am_map doomdef doomstat dstrings d_event d_items \
              d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game \
              hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale \
              i_sound i_system i_timer memio m_argv m_bbox m_cheat \
              m_config m_controls m_fixed m_menu m_misc m_random p_ceilng \
              p_doors p_enemy p_floor p_inter p_lights p_map p_maputl \
              p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec \
              p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main \
              r_plane r_segs r_sky r_things sha1 sounds statdump st_lib \
              st_stuff s_sound tables v_video wi_stuff w_checksum w_file \
              w_main w_wad z_zone i_input i_video doomgeneric

DOOM_SRCS := $(addprefix runtime/upstream/doom/,$(addsuffix .c,$(DOOM_NAMES))) \
             user/lib/doom_kosmos.c
USER_SRCS += $(DOOM_SRCS)

#
# id's source is 1997 C and does not compile clean under this project's
# flags, which is not a criticism of it - `-Wall -Wextra -Werror` did not
# exist as a habit then, and the code is thirty years old and correct.
#
# The warnings are turned off *for those files only*, further down, rather
# than for the build: Kosmos's own code including `doom_kosmos.c` is still
# held to the same bar it always was. Vendored code is not modified, which
# is the rule that decides this - patching eighty files to silence a warning
# would be exactly the modification `CLAUDE.md` forbids.
#
#
# Twelve megabytes of heap instead of two, for both halves of the build.
#
# `malloc` in a Kosmos process is the process's own heap, and Doom brought
# its own allocator: `I_ZoneBase` asks for six megabytes before the game
# draws anything, and `DG_ScreenBuffer` is another one at 640x400. Against a
# two-megabyte heap the first allocation fails, and what that looks like is
# a black window and not one line of output - Doom faults writing through
# the pointer it did not get, and the compositor goes on drawing the
# window's last contents because it owns them.
#
# The kernel needs the same number, because it is the kernel that maps the
# heap when it builds the process.
#
DOOM_HEAP := -DUSER_HEAP_PAGES=3072

#
# TinyGL, compiled on its own terms.
#
# `-w -Wno-error` for the same reason Doom gets them: this is other people's
# code and the rule about vendored trees forbids patching it. Kosmos's own
# `gl_kosmos.c` is still held to `-Wall -Wextra -Werror`; only the twenty-five
# files under `runtime/upstream/tinygl/source/` are not.
#
# It compiled freestanding on the first attempt with no errors at all, which
# is what seven thousand lines of self-contained software rasteriser looks
# like: it wants `malloc`, `memcpy`, `assert` and seven functions out of
# `math.h`, and this machine has all of them.
#
DOOM_CFLAGS := -DKOSMOS_DOOM -w -Wno-error -Iruntime/upstream/doom \
               -DNORMALUNIX -DLINUX -DDOOMGENERIC_RESX=640 \
               -DDOOMGENERIC_RESY=400
endif

USER_OBJS := $(addprefix $(UBUILD)/,$(addsuffix .o,$(USER_SRCS)))
USER_DEPS := $(USER_OBJS:.o=.d)

# -Ikernel is for syscall.h and panic.h, and nothing else. The syscall
# numbers are the ABI and belong to both sides of it by definition.
UCFLAGS := $(CFLAGS_BASE) $(UTESTDEFS) $(if $(DOOM),-DKOSMOS_DOOM -Iruntime/upstream/doom) -DKOSMOS_USER \
           -Iruntime/upstream/puff -Iruntime/upstream/stb \
           -Iruntime/upstream/minimp3 \
           -Iuser/include -Ikernel -Iruntime/include \
           -Ilua/upstream -Ilua/kosmos \
           -fno-stack-protector

ULDFLAGS := -T user/user.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
            -Wl,-Map,$(UBUILD)/init.map

# ------------------------------------------------------------------
# Rebuild when the *flags* change, not only when a file does.
#
# make compares timestamps and knows nothing about the command line. So
# `make FB=1920x1080` after an ordinary build said "Nothing to be done" and
# then ran the old image at the old size - silently, with the change
# apparently applied and visibly not working. That is the worst shape a
# build bug can take, and it cost an evening once.
#
# The fix is a file holding the flags, rewritten only when they differ, that
# every object depends on. Written with `$(file ...)` at parse time rather
# than in a recipe, because it has to be up to date before make decides what
# is out of date.
# ------------------------------------------------------------------
FLAGS_NOW := $(CFLAGS) | $(UCFLAGS)
FLAGS_FILE := $(BUILD)/flags

$(shell mkdir -p $(BUILD) $(UBUILD))
$(shell [ "$$(cat $(FLAGS_FILE) 2>/dev/null)" = '$(FLAGS_NOW)' ] \
        || printf '%s' '$(FLAGS_NOW)' > $(FLAGS_FILE))

# And a rule, so a build that has never made this variant can still make it.
# The line above only rewrites the file when it exists and differs; the test
# and bench builds use their own BUILD directory and had never seen one,
# which make reported as "no rule to make target" rather than as anything
# resembling the cause.
$(FLAGS_FILE):
	@mkdir -p $(dir $@)
	@printf '%s' '$(FLAGS_NOW)' > $@

# And the same trick for the one file that carries its own flag, so that
# changing the screen size rebuilds that file and relinks, and touches
# nothing else.
FB_FILE := $(BUILD)/fb.flags

$(shell [ "$$(cat $(FB_FILE) 2>/dev/null)" = '$(FB_FLAGS)' ] \
        || printf '%s' '$(FB_FLAGS)' > $(FB_FILE))

$(FB_FILE):
	@mkdir -p $(dir $@)
	@printf '%s' '$(FB_FLAGS)' > $@

$(BUILD)/hal/qemu-virt/fb.c.o: CFLAGS += $(FB_FLAGS)
$(BUILD)/hal/qemu-virt/fb.c.o: $(FB_FILE)

# Upstream code is compiled without -Werror, the same allowance lua/upstream
# gets: its warnings are not ours to fix and patching them would mean the
# tree no longer holding what the author released.
#
# TinyGL, compiled on its own terms, and above the general upstream rule for
# the reason that rule's neighbour already records: make takes the first
# pattern that matches, and `runtime/upstream/%.c` matches these too.
#
$(UBUILD)/runtime/upstream/tinygl/examples/bounce.c.o: runtime/upstream/tinygl/examples/bounce.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,bounce) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/cube.c.o: runtime/upstream/tinygl/examples/cube.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,cube) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/gears.c.o: runtime/upstream/tinygl/examples/gears.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,gears) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

#
# `mech` is the one that does not follow the pattern: it calls its per-frame
# function `display`, which is GLUT's name for it rather than `ui.h`'s, so
# the ordinary rename finds no `draw` to rename. Given its own line here
# rather than patched, for the same reason as everything else in this tree.
#
$(UBUILD)/runtime/upstream/tinygl/examples/mech.c.o: runtime/upstream/tinygl/examples/mech.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,mech) \
	      -Ddisplay=mech_draw \
	      -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/morph3d.c.o: runtime/upstream/tinygl/examples/morph3d.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,morph3d) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/spin.c.o: runtime/upstream/tinygl/examples/spin.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,spin) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/teapot.c.o: runtime/upstream/tinygl/examples/teapot.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,teapot) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/texobj.c.o: runtime/upstream/tinygl/examples/texobj.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,texobj) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/source/%.c.o: runtime/upstream/tinygl/source/%.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/%.c.o: runtime/upstream/%.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Wno-error -MMD -MP -c $< -o $@

#
# id's source, compiled on its own terms.
#
# Before the general rule, because make takes the first pattern that
# matches. Three differences from everything else here:
#
#   `-w` and `-Wno-error`. This is 1997 C - unused parameters, missing field
#   initialisers, K&R habits - and it is thirty years old and correct.
#   Kosmos's own code including `doom_kosmos.c` is still held to
#   `-Wall -Wextra -Werror`; only these eighty files are not. The
#   alternative was patching them, which is the one thing the rule about
#   vendored code forbids.
#
#   No `-include kosmos_lua.h`. Doom does not know what Lua is and should
#   not be told.
#
$(UBUILD)/user/lib/gl_demos.c.o: user/lib/gl_demos.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iruntime/upstream/tinygl/include -MMD -MP -c $< -o $@

$(UBUILD)/user/lib/gl_kosmos.c.o: user/lib/gl_kosmos.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iruntime/upstream/tinygl/include -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/doom/%.c.o: runtime/upstream/doom/%.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(DOOM_CFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/%.c.o: %.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -include lua/kosmos/kosmos_lua.h -MMD -MP -c $< -o $@

$(UBUILD)/%.S.o: %.S $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

# The Lua source init runs, and then init itself, both carried inside the
# kernel image because there is no filesystem to load them from until M8.
$(GEN)/init_lua.c: user/init/init.lua tools/bin2c.py $(HOSTDIR)/lua.ok
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< init_lua $@

# The font, from the BDF the author ships to an array with one byte per
# pixel row. Vendored unmodified for the same reason lua/upstream/ is.
$(GEN)/font_8x16.c: assets/fonts/spleen-8x16.bdf tools/bdf2c.py
	@mkdir -p $(dir $@)
	python3 tools/bdf2c.py $< font_8x16 $@

# ------------------------------------------------------------------
# The Lua in this repository, checked before it is embedded.
#
# Everything written in Lua here is loaded at run time inside the guest, so a
# mistake in it is not a build failure: it is a process that dies at boot, or
# a shell that vanishes the moment somebody types the word that reaches the
# broken line. Those are expensive, because a dead process cannot say why it
# died - it prints by asking the console server.
#
# Two checks, built from lua/upstream/ with the host compiler so the parser
# is exactly the one that will run the code:
#
#   luacheck    it parses
#   luaglobals  every global it reads will actually be there
#
# The second is the one that earns its place. A name that used to be a local
# and is not any more compiles perfectly happily - it is a global, and
# globals may be nil - and fails much later somewhere unhelpful. That has
# cost four separate debugging sessions in this project.
# ------------------------------------------------------------------

# -w because upstream's warnings are not ours to fix, the same reasoning the
# target build uses.
$(HOSTDIR)/luacheck: tools/luacheck.c $(LUA_HOST_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -O1 -w -Ilua/upstream -o $@ $^ -lm

$(HOSTDIR)/luac: lua/upstream/luac.c $(LUA_HOST_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -O1 -w -Ilua/upstream -o $@ $^ -lm

# The interpreter, on this machine rather than the target.
#
# For unit-testing the Lua libraries that are pure logic. `kfs.lua` is the
# case that asked for it: it is a filesystem format, its whole job is
# arithmetic over blocks, and the only thing it needs from the system is
# two functions to read and write one. Given those as stubs over a byte
# string, every branch of it can be tested here in a tenth of a second -
# including the ones that need a machine to lose power at an exact instant,
# which cannot be arranged under QEMU on purpose at all.
#
# It does not replace the guest tests and it is not allowed to. What runs
# here is the same source, but not on the same machine, against the same
# libc, or through the same syscalls. This answers "is the format correct";
# `make test` and `make disktest` answer "does it work on the machine".
#
# `linit.c` is added back here and nowhere else. The rest of the host tools
# deliberately leave it out - it opens every standard library, including the
# ones the guest does not have - but an interpreter that cannot `require`
# its own standard library cannot run a test.
$(HOSTDIR)/lua: lua/upstream/lua.c lua/upstream/linit.c $(LUA_HOST_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -O1 -w -Ilua/upstream -o $@ $^ -lm

# A stamp rather than a phony target: the generated sources depend on this,
# and a phony one would rebuild them on every make.
$(HOSTDIR)/lua.ok: $(LUA_FILES) $(HOSTDIR)/luacheck $(HOSTDIR)/luac tools/luaglobals.py
	@$(HOSTDIR)/luacheck $(LUA_FILES)
	@python3 tools/luaglobals.py $(HOSTDIR)/luac $(LUA_FILES)
	@touch $@

.PHONY: check-lua
check-lua: $(HOSTDIR)/lua.ok

# The programs in user/bin/, as Lua source the /bin server serves. There is
# no disk until M8, so a program reaches the system by being in the image.
# The version string, as a source file.
#
# Written only when it would differ, so a no-op build stays a no-op. A
# timestamp taken at build time would relink on every make and tell you
# nothing you did not already know; the commit is the thing that identifies
# what is running.
# The commit, and whether the *sources* differ from it.
#
# `builds/` is excluded on purpose. Publishing an image starts by deleting
# the previous one, which dirties the tree before this is evaluated, so
# `make release` stamped every image it built "-dirty" while the source it
# was built from was clean. The name is a claim about the source.
KOSMOS_DIRTY := $(shell git status --porcelain -- . ':!builds' 2>/dev/null | head -1)
KOSMOS_BUILD := $(shell git describe --always 2>/dev/null || echo "no-git")$(if $(KOSMOS_DIRTY),-dirty,)
KOSMOS_DATE  := $(shell git log -1 --format=%cd --date=format:'%Y-%m-%d' \
                        2>/dev/null || echo "unknown")

# The pictures in assets/images/, as a C table. Binary, so unlike programs
# and libraries these cannot travel as Lua source: a PNG contains every byte
# value there is, including the ones that would end a Lua long string early.
#
# assets/icons/ rides in the same table, and they are the file manager's
# icons. Vendored PNGs rather than shapes drawn from `g:fill`, which is what
# was there first: eleven hand-drawn rectangles per icon that looked exactly
# as hand-drawn as they were, and could not say what a script was.
#
# There is nothing to convert. The system already decodes PNG (`gfx.png`, in
# C because it is a pixel loop) and already composites alpha (`surface:blend`,
# source-over), so the release's own bytes go in and 32x32 RGBA comes out.
# The alternative - a host script turning them into C arrays, the way
# `bdf2c.py` handles the bitmap font - would have been a build step written
# to avoid using a decoder this system needs anyway.
ICON_FILES := $(sort $(wildcard assets/icons/*.png))

$(GEN)/assets.c: assets/images/test-pattern.png $(ICON_FILES) tools/assets2c.py
	@mkdir -p $(dir $@)
	python3 tools/assets2c.py assets_table $@ \
	        assets/images/test-pattern.png $(ICON_FILES)

# The outline fonts, embedded the same way.
#
# In the image rather than on the disk, because the desktop has to be able
# to draw text on a machine with no drive - which is how every display test
# runs. The cost is real and is written down in roadmap.md: the image is
# copied into every process, so these bytes are paid for per process, and
# that makes the shared read-only text mapping already on that list worth
# more than it was.
FONT_FILES := $(sort $(wildcard assets/fonts/*.ttf) $(wildcard assets/fonts/*.otf))

$(GEN)/fonts.c: $(FONT_FILES) tools/assets2c.py
	@mkdir -p $(dir $@)
	python3 tools/assets2c.py fonts_table $@ $(FONT_FILES)

$(GEN)/version.c: FORCE
	@mkdir -p $(dir $@)
	@printf '/* Generated by the Makefile. Do not edit. */\n\nconst char kosmos_name[] = "$(OS_NAME)";\nconst char kernel_name[] = "$(KERNEL_NAME)";\nconst char kosmos_version[] = "$(VERSION)";\nconst char kosmos_build[] = "$(KOSMOS_BUILD)";\nconst char kosmos_date[] = "$(KOSMOS_DATE)";\nconst char kosmos_platform[] = "QEMU virt aarch64";\n' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

.PHONY: FORCE
FORCE:

$(GEN)/programs.c: $(wildcard user/bin/*.lua) tools/progs2c.py $(HOSTDIR)/lua.ok
	@mkdir -p $(dir $@)
	python3 tools/progs2c.py programs_lua $@ $(wildcard user/bin/*.lua)

# The libraries in user/lib/, the same way and for the same reason. A
# separate store rather than a directory inside /bin, because a program is
# something you run and a library is something you load, and a `/bin` that
# lists both is a `/bin` where `ls` lies about what you can type.
$(GEN)/libraries.c: $(wildcard user/lib/*.lua) tools/progs2c.py $(HOSTDIR)/lua.ok
	@mkdir -p $(dir $@)
	python3 tools/progs2c.py libraries_lua $@ $(wildcard user/lib/*.lua)

$(GEN)/luatest_lua.c: user/tests/luatest.lua tools/bin2c.py $(HOSTDIR)/lua.ok
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< luatest_lua $@

$(GEN)/luabench_lua.c: user/tests/luabench.lua tools/bin2c.py $(HOSTDIR)/lua.ok
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< luabench_lua $@

$(UBUILD)/init.elf: $(USER_OBJS) user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(USER_OBJS) -o $@ $(LIBS)

$(UBUILD)/init.bin: $(UBUILD)/init.elf
	$(OBJCOPY) -O binary $< $@

$(GEN)/init_bin.c: $(UBUILD)/init.bin tools/bin2c.py
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< init_image $@

QEMU      := qemu-system-aarch64
# gic-version=3 is not the default. Plain `-M virt` gives a GICv2, and this
# kernel's interrupt controller code is GICv3: it would find no
# redistributor and no interrupt would ever arrive.
#
# -device ramfb is the display. It is a device rather than machine state, so
# a machine started without it simply has no screen, which is a case the
# kernel handles and `make test` exercises.
#
# -display cocoa opens a window; -serial mon:stdio keeps the serial console
# and the QEMU monitor in the terminal, which is where the shell lives. The
# two together are why this is no longer -nographic: that flag means "no
# window", and a window is now the point.
#
# Ctrl-A x still quits, from the terminal.
# -global virtio-mmio.force-legacy=false is not optional. QEMU's virtio-mmio
# transports report version 1, the legacy interface, unless told otherwise -
# a different ring layout reached through QUEUE_PFN, which modern structures
# read as garbage. Linux passes the same thing. Without it the keyboard is
# found, correctly refused, and the boot says there is none.
#
# The disk. Kept between runs rather than made fresh, because the point of
# it is that what was written is still there next time - which is the whole
# of M8. `make disk` remakes it when it needs to be empty again.
#
# The disk. Overridable, and worth overriding.
#
#   make qemu DISK=build/play.img
#
# QEMU takes a write lock on the image, so two machines cannot share one:
# the second says `Failed to get "write" lock` and refuses to start. That is
# the right behaviour - two writers on one filesystem is corruption - but it
# means a machine left running blocks the next one, and a machine running at
# all blocks `kfs.lua put` from this side.
#
# `make test` is not the problem: every harness makes its own temporary
# image, on purpose, so a pass can never be a leftover. The collision is
# between one person's machine and another's, or between a machine and the
# host tool writing to the image underneath it.
#
# Making a second image is one command, and it can be filled while it is
# made:
#
#   build/host/lua tools/kfs.lua create build/play.img 64 \
#       ~/Downloads/book.pdf:/home/book.pdf
#
DISK      := build/kosmos.img
DISK_MB   := 64

#
# What to start once the machine is up. Empty means the shell.
#
#   make qemu BOOT=wm            straight to the desktop
#   make qemu BOOT="wm blocks"   the desktop with something on it
#
# Passed through QEMU's fw_cfg, which is how a machine is told what to do
# without being rebuilt. `opt/` is the namespace QEMU reserves for exactly
# this, so nothing here can collide with a name QEMU defines itself.
#
comma     := ,
BOOT      :=
# Quoted, because a boot string with a space in it is the normal case -
# `BOOT="wm blocks"` is in the comment above and did not work: the shell
# split it and QEMU took `blocks` for a filename it could not open. The
# example was written before anything used one.
BOOTARG   := $(if $(BOOT),-fw_cfg 'name=opt/kosmos/boot$(comma)string=$(BOOT)',)

#
# Making it readable on a Retina display.
#
# QEMU draws one guest pixel per point, so a 1920x1080 guest in a window
# that is 1000 points wide is scaled *down* - a 16-pixel glyph lands in nine
# physical pixels and the desktop is unreadable at arm's length. Asking for
# more guest pixels makes that worse, not better, which is the opposite of
# what it feels like it should do.
#
# The fix is the one macOS uses for every native application: draw at half
# and show at 2x. So pick a guest size around half the screen's points and
# let `zoom-to-fit` scale it up - on a Retina panel the doubling lands on
# real pixels and the result is sharp.
#
#   make FB=1280x800 qemu          a good default on a 16-inch MacBook Pro
#   make FB=1280x800 FULLSCREEN=1 qemu
#
# `zoom-to-fit` is on always: with the window at its natural size it changes
# nothing, and it is what makes resizing the window scale the picture
# instead of revealing more grey.
#
# The real answer is a UI scale factor inside Kosmos - `appearance` already
# chooses a font and a size, so the mechanism half exists - and that is what
# a Pi 5 on a 4K monitor will need. This is the host-side stopgap.
#
FULLSCREEN_FLAG := $(if $(FULLSCREEN),$(comma)full-screen=on,)

#
# `make ZOOM=1 qemu` scales the guest to fill the window.
#
# Off by default, and it used to be on. On a Retina display the window is
# half the size the pixel count says and zooming is the only way to read it;
# on an ordinary monitor the same setting resamples a 1024x768 guest onto a
# window of some other size, and every one-pixel bevel this desktop is built
# out of goes soft. Which of those is happening depends on the monitor
# somebody plugged in this morning, so it is a flag rather than a decision.
#
ZOOM_FLAG := $(if $(ZOOM),$(comma)zoom-to-fit=on,)

#
# Sound, through the host's own audio.
#
# `coreaudio` is the macOS backend and the only one that plays in *real
# time*, which matters for more than hearing it: `wav` and `none` both take
# samples as fast as they are offered, so a latency measurement against
# either measures the backend. `beep` says which it got and refuses to
# report a number it did not earn.
#
# `make NOAUDIO=1 qemu` leaves the device out, for when something else on
# the Mac wants the audio hardware to itself.
#
AUDIO_FLAGS := $(if $(NOAUDIO),,-audiodev coreaudio$(comma)id=snd0 \
                                -device virtio-sound-device$(comma)audiodev=snd0)

#
# **`make FAST=1 qemu` runs the guest on this Mac's own cores.**
#
# `hvf` is Apple's Hypervisor.framework. The guest is aarch64 and so is the
# host, so there is nothing to translate: the instructions execute natively
# and the only thing QEMU does is emulate the devices. `gfxbench` says 4x on
# a fill, 5x on a blit and **14x on a circle drawn in Lua** - the interpreter
# is branch-heavy, which is what TCG is worst at, and the interface is Lua.
#
# Three things travel with it and none of them is optional:
#
#   - `-cpu host` is required. HVF cannot pretend to be a different core, so
#     this is the *Mac's* processor rather than a Cortex-A72 or the A76 the
#     Pi 5 has. Fidelity and speed are two targets, not one flag.
#   - GICv3 only. HVF refuses to emulate GICv2 and says so.
#   - **`-icount` is TCG only**, so `make bench` cannot use this and must
#     not: the whole reason those numbers mean anything is that instruction
#     counting is deterministic. This is for how the desktop *feels*, which
#     is the one question TCG cannot answer; `-icount` is for whether a
#     change made something slower.
#
# It needed a kernel fix to work at all. `mmio_write32` used to be a
# volatile store and the compiler chose a post-indexed addressing mode for
# it, which is correct on hardware and cannot be emulated by any hypervisor:
# ARM sets ISV=0 in the syndrome for a load or store with writeback, so
# there is nothing in the trap to say which register or what width. See
# `arch/aarch64/mmio.h`.
#
ACCEL := $(if $(FAST),-accel hvf -cpu host,-cpu cortex-a72)

QEMUFLAGS := -M virt,gic-version=3 $(ACCEL) -m 512M \
             -global virtio-mmio.force-legacy=false \
             -device ramfb -device virtio-keyboard-device \
             -device virtio-tablet-device \
             $(AUDIO_FLAGS) \
             -drive file=$(DISK),format=raw,if=none,id=disk \
             -device virtio-blk-device,drive=disk \
             $(BOOTARG) \
             -display cocoa$(ZOOM_FLAG)$(FULLSCREEN_FLAG) -serial mon:stdio \
             -kernel $(TARGET)

# The same machine with no screen, for when the window is in the way or the
# terminal is all there is - over ssh, for instance.
# No window, and therefore no keyboard: with -display none QEMU has nowhere
# to take key presses from, so the virtio device would sit there empty.
QEMUFLAGS_SERIAL := -M virt,gic-version=3 $(ACCEL) -m 512M -nographic \
                    -global virtio-mmio.force-legacy=false \
                    -drive file=$(DISK),format=raw,if=none,id=disk \
                    -device virtio-blk-device,drive=disk \
                    $(BOOTARG) \
                    -kernel $(TARGET)

.PHONY: all bump bump-minor bump-major qemu fast serial test droplet disktest powertest stress screenshot shot prepush frames bench bench-record debug disasm size clean dist release disk

# A disk image, built here, with whatever you want already in it.
#
#   make image FILES="book.pdf:/home/books/book.pdf song.mp3:/home/music/a.mp3"
#
# The same kfs.lua the machine runs, over a file. `tools/kfs.lua` also does
# ls, put, get and rm on an existing image, which is how a file gets on and
# off a disk this Mac cannot mount.
.PHONY: image
image: $(HOSTDIR)/lua
	$(HOSTDIR)/lua tools/kfs.lua create $(DISK) $(DISK_MB) $(FILES)

# An empty disk. Made when it is missing and never overwritten by accident:
# `make disk` after deleting it is a deliberate act, and a build that
# silently reformatted the disk would be a build that eats the filesystem it
# is meant to be testing.
$(DISK):
	@mkdir -p $(dir $@)
	@dd if=/dev/zero of=$@ bs=1m count=$(DISK_MB) 2>/dev/null
	@echo "$@: $(DISK_MB) MB, empty"

# Moving the version. One of these before each push, and `bump` is the one
# that happens most.
bump:
	@python3 tools/bump.py revision

bump-minor:
	@python3 tools/bump.py minor

bump-major:
	@python3 tools/bump.py major

disk: $(DISK)

#
# A Dock icon you can drop files on, and they land in the image's /home.
#
# The tedious part of putting a song or a PDF on the machine was never the
# copy - it was remembering where the image is and what `kfs.lua put` wants
# its arguments in. This is one gesture instead, and it shells out to the
# same `tools/kfs.lua` the machine itself runs, so a file put in by dropping
# it is written by the code that will read it.
#
# Built rather than committed: an `.app` is a directory of generated files,
# and the repository keeps the script it is generated from.
#
DROPLET := build/Kosmos Drop.app

droplet: $(DISK)
	@rm -rf "$(DROPLET)"
	@mkdir -p build
	@sed 's|__REPO__|$(CURDIR)|' tools/kosmos-drop.applescript > build/kosmos-drop.applescript
	@osacompile -o "$(DROPLET)" build/kosmos-drop.applescript
	@rm -f build/kosmos-drop.applescript
	@echo "Built $(DROPLET)"
	@echo "Drag it to the Dock, then drop files on it."

# Does what was written survive the power going off?
#
# A separate harness because that question cannot be asked inside one boot.
# It boots the machine twice against one image: the first run formats, the
# second is a machine that has never seen a disk and has to find a
# filesystem there. Everything else about M8 would pass with a filesystem
# that quietly forgot everything.
disktest: $(TARGET)
	python3 tools/run_disk.py $(TARGET)

all: $(TARGET)

$(TARGET): $(OBJS) boot/kosmos.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@ $(KLIBS)

$(BUILD)/%.c.o: %.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.S.o: %.S $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

#
# Everything needed to run this somewhere else, in one directory.
#
# The image is self-contained: the userland, the interpreter, every program
# and the font are inside it, so there is nothing to install and nothing to
# mount. What the other machine needs is a copy of QEMU and the right
# command line - and the command line is the part worth carrying, because
# `-global virtio-mmio.force-legacy=false` is not something anybody guesses
# and without it the keyboard and the pointer are silently absent.
#
dist: $(TARGET)
	@rm -rf build/dist && mkdir -p build/dist
	@cp $(TARGET) build/dist/kosmos.elf
	@sed 's|^image="build/kosmos.elf"|image="$$(dirname "$$0")/kosmos.elf"|' \
	    run-kosmos.sh > build/dist/run-kosmos.sh
	@chmod +x build/dist/run-kosmos.sh
	@echo "build/dist: kosmos.elf and run-kosmos.sh - copy the directory anywhere"
	@ls -l build/dist

#
# A build somebody can download and run, kept in the repository.
#
# Named by version and commit, so several can sit side by side and it is
# always clear which one is being run - and so that "it worked in the one
# from Tuesday" is a thing that can be checked rather than remembered.
#
# The image is self-contained: the userland, the interpreter, every program
# and the font are inside it. A copy of this file and `run-kosmos.sh` are
# the whole of what has to travel.
#
#
# One image per display size.
#
# The framebuffer is a static array, so its size is a compile-time constant
# and a different size is a different image. That is a consequence of having
# no allocator rather than a choice, and it costs one recompile of one file
# and a relink each - the flag is on that file's own compile line, so
# nothing else rebuilds.
#
# Changing it while the machine runs is a different question and is written
# up in hal.md: every process that called `gfx.screen()` is holding a
# mapping of a particular length, and a mode change is a protocol none of
# them speak yet.
#
RELEASE_SIZES := 1024x768 1280x800 1920x1080

# A binary that leaves this machine has been used for a while first.
#
# `make test` says the parts work and `make screenshot` says the machine
# works once. Neither notices a pool that fills on the fiftieth try, and a
# release is the thing somebody else runs without watching it.
release: $(TARGET) stress
	@mkdir -p builds
	@for size in $(RELEASE_SIZES); do \
	    rm -f $(BUILD)/hal/qemu-virt/fb.c.o $(TARGET); \
	    $(MAKE) --no-print-directory FB=$$size $(TARGET) >/dev/null; \
	    cp $(TARGET) builds/kosmos-$(VERSION)-$(KOSMOS_BUILD)-$$size.elf; \
	    echo "builds/kosmos-$(VERSION)-$(KOSMOS_BUILD)-$$size.elf"; \
	done
	@rm -f $(BUILD)/hal/qemu-virt/fb.c.o $(TARGET)
	@$(MAKE) --no-print-directory $(TARGET) >/dev/null
	@cp run-kosmos.sh builds/run-kosmos.sh
	@ls -l builds/

# Ctrl-A then x to quit QEMU.
qemu: $(TARGET) $(DISK)
	$(QEMU) $(QEMUFLAGS)

# The same machine, running on this Mac's own cores. See ACCEL above for
# what that costs and what it cannot be used for.
fast: $(TARGET) $(DISK)
	$(MAKE) FAST=1 qemu

# No window. The same system, serial only, which is how it ran until M6 and
# how it will run on a board with a cable and no monitor.
serial: $(TARGET) $(DISK)
	$(QEMU) $(QEMUFLAGS_SERIAL)

# Recursive so the test image gets its own BUILD and its own flags. The
# runner lives on the host and owns the QEMU line for tests, because it needs
# semihosting and a timeout.
test: $(TARGET) $(HOSTDIR)/lua
	@# The format, on this machine, before anything is booted. It is the
	@# fastest of the three and the one that fails first when the disk
	@# layout is wrong.
	$(HOSTDIR)/lua tools/test_kfs.lua
	@# The WAV header walker, likewise: pure Lua over a reader, so the
	@# awkward headers can be built by hand rather than found in the wild.
	$(HOSTDIR)/lua tools/test_wav.lua
	@$(MAKE) --no-print-directory TEST=1 build/test/kosmos.elf
	python3 tools/run_tests.py build/test/kosmos.elf
	@# And the same machine with nothing plugged into it. A second boot,
	@# but of the ordinary image rather than the test one: what it checks
	@# is init and the shell, which the test image replaces.
	python3 tools/run_headless.py $(TARGET)
	@# Files on and off the image from this computer, which is what a
	@# filesystem that is not FAT32 has to answer for.
	python3 tools/run_interchange.py $(TARGET)
	@# Attributes and the queries over them. M7's definition of done was a
	@# live query and nothing here ever checked one: `qbench` measures how
	@# fast a query is and would not notice it returning the wrong paths,
	@# which is what it did on the disk for as long as the disk could
	@# answer.
	python3 tools/run_queries.py $(TARGET)

# Used for a while, then asked whether it gave everything back.
#
# Not part of `make test`: that is the fast gate and this boots a machine and
# works it for minutes. This is the gate on a *release* - see the `release`
# target, which will not produce a binary that has not survived it.
.PHONY: stress
stress: $(TARGET)
	python3 tools/run_stress.py $(TARGET) $(if $(ROUNDS),$(ROUNDS),60)

# Power loss, which cannot be asked inside one boot. Not part of `make test`
# because it boots eleven times and kills five of them.
.PHONY: powertest
powertest: $(TARGET)
	python3 tools/run_power.py $(TARGET)

# The display, checked from outside the guest.
#
# Not part of `make test`, because it is a second boot and it asks QEMU
# rather than the kernel. The suite proves what the kernel wrote into its own
# memory; this proves the picture QEMU is scanning out of it, which is the
# half no test inside the guest can reach.
screenshot: $(TARGET)
	python3 tools/run_screenshot.py $(TARGET) --png build/screenshot.png

# One picture of the desktop, for the record.
#
#   make shot
#
# Boots at 1920x1080, opens Tracker, the widget gallery, Processes, Monitor
# and the cube, lays them out so all of them are visible, and saves the
# screen to `builds/screenshots/` under the date and the revision.
#
# The point is the series rather than any one of them: a repository full of
# these is the only honest account of what the desktop looked like on a
# given day. A commit message says what changed; a screenshot says what it
# became, and nothing else here records that.
#
# In `docs/`, because that is what these are. `build/` is gitignored and
# `make clean` deletes it, so a history kept there would vanish the first
# time anybody cleaned; `builds/` is for things you can run. A picture of
# what the desktop looked like on a given day is documentation, and it sits
# with the rest of it.
#
# The size is a build flag, so this rebuilds at 1920x1080 and then puts the
# default image back - otherwise the next `make screenshot` would silently
# be testing a different machine.
SHOTDIR := docs/screenshots

# Everything a push should carry.
#
#   make prepush
#
# The suites, the display harness, and a picture of the desktop. The
# screenshot is the one that would otherwise be forgotten - it is not a
# check, nothing fails without it, and a series with gaps in it is worth
# much less than a series without. So it is a step in a target rather than
# something to remember.
.PHONY: prepush
prepush: test screenshot shot
	@echo
	@echo "ready to push: suites green and $(SHOTDIR) has today's picture."


.PHONY: shot
shot:
	@$(MAKE) --no-print-directory FB=1920x1080 $(TARGET)
	@mkdir -p $(SHOTDIR)
	python3 tools/run_gallery.py $(TARGET) \
	  --out $(SHOTDIR)/$$(date +%Y-%m-%d-%H%M)-$$(git rev-parse --short HEAD).png
	@$(MAKE) --no-print-directory $(TARGET)

# Where a window manager pass goes, under an idle desktop, an animating one
# and a drag. Not gated and deliberately not: these are QEMU numbers and
# `CLAUDE.md` is clear about what those are worth. It exists because every
# other measurement here is of the kernel, and a system aiming at a
# responsive desktop had nothing at all that measured a frame.
frames: $(TARGET)
	python3 tools/run_frames.py $(TARGET) --seconds 6

# Separate image again, and a separate runner: a benchmark needs QEMU's
# -icount to be repeatable at all, and -icount makes everything several times
# slower, so the test suite must not pay for it.
bench:
	@$(MAKE) --no-print-directory BENCH=1 build/bench/kosmos.elf
	python3 tools/run_bench.py build/bench/kosmos.elf

# Records the current numbers as the new baseline. By hand, never
# automatically: testing.md 18.6 is explicit that a baseline which updates
# itself detects nothing. Run it when a number moves on purpose, and say why
# in the commit.
bench-record:
	@$(MAKE) --no-print-directory BENCH=1 build/bench/kosmos.elf
	python3 tools/run_bench.py build/bench/kosmos.elf --record

# In another terminal: aarch64-none-elf-gdb build/kosmos.elf
#                      (gdb) target remote :1234
debug: $(TARGET)
	$(QEMU) $(QEMUFLAGS) -S -gdb tcp::1234

disasm: $(TARGET)
	$(OBJDUMP) -d $(TARGET)

size: $(TARGET)
	@$(SIZE) $(TARGET)
	@echo
	@python3 tools/kernel_size.py

clean:
	rm -rf build build-user build-user-test build-user-bench

-include $(DEPS)
-include $(USER_DEPS)
