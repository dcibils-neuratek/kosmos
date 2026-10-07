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

#
# **Which processor.** `ARCH=x86_64` builds the second one.
#
# The 64-bit line was drawn where it is precisely so that this is a new
# `arch/` and not a refactor: `kernel/` has no architecture-specific
# instruction left in it, and what a port has to supply is six headers.
#
# It is a *bring-up* today and the target below says so - what builds is the
# boot path, the serial port and enough C to report that long mode is on.
# The kernel itself follows, one piece at a time, which is the order this
# project used on ARM and for the same reason: a kernel that cannot print is
# a kernel debugged by bisecting a hang.
#
ARCH    ?= aarch64

CROSS   := $(if $(filter x86_64,$(ARCH)),x86_64-elf-,aarch64-none-elf-)
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
OBJCOPY := $(CROSS)objcopy
NM      := $(CROSS)nm

SIZE    := $(CROSS)size

# Which of the three images is being built. The test and bench builds each
# carry a Lua chunk the shipping one must not, so their user images are
# different binaries and cannot share a directory or a generated .c with it -
# `make` and `make test` would trade stale ones back and forth.
#
# **`make qemu` is the whole system, at 1920x1080.**
#
# Doom, the browser, the network, every demo - because the thing you want to
# do with an operating system you are building is use it, and a build that
# leaves half of it out is a build that tells you about half of it. The
# variants exist so that *a suite* can be small and quick, not so that the
# machine you sit in front of is.
#
# `FULL=0` opts out and gives the lean image, which is a couple of seconds
# quicker to link and 3.5 MB smaller. The test and bench images set their
# own shape and are left alone: `make test` builds a chunk the shipping
# image must not carry, and its whole value is being fast enough to run
# without thinking about it.
#
# **And the licence, which follows from the first line.** Nothing here is
# linked dynamically, so what an image carries decides its terms, and
# `LICENSE` says which, and the About window reads it out of the image. The
# image `FULL=1` builds carries FFmpeg's LGPL decoder; Doom, which made it a
# GPLv2 work until 28 September, is an application beside it now
# (`docs/elf.md` step 5), and `doom.elf` is the GPLv2 work.
#
# **`MEGA=1` is `FULL=1` now, and kept so the command a stick is built with
# still works.** It was everything this tree could put in one image: what
# `FULL=1` turns on, and Quake. Lite XL left the tree on 26 September, and
# Doom, Quake and the Super Nintendo are installed applications since 28
# September (`docs/elf.md` step 5) - images of their own that `make apps`
# links and a stick carries in `/Home/Apps` - so there is nothing left that
# only MEGA carries. The test and bench images ignore it as they ignore
# `FULL`, and the checks that build a variant of their own say `MEGA=`.
#
ifeq ($(MEGA),1)
ifndef TEST
ifndef BENCH
FULL   := 1
endif
endif
endif

FULL ?= 1

ifeq ($(FULL),1)
ifndef TEST
ifndef BENCH
WEB  := 1
FFMPEG := 1
WALLPAPERS := 1
FB   ?= 1920x1080
endif
endif
endif

#
# **Composed, not chosen.** This was a chain of else-ifs, so `DOOM=1 WEB=1`
# called itself `-doom` and put a build with the browser in it into the same
# directory as one without - two different binaries under one name, which
# `make` settles by timestamp and gets wrong.
#
# It has to come *after* the block above, because `:=` expands where it
# stands: with FULL setting DOOM and WEB below this line, neither was
# visible here and the full build went into the lean build's directory -
# which is the collision this name exists to prevent, arrived at from the
# other direction.
#
#
# The architecture is part of it, and only when it is not the default.
#
# Two architectures share this tree and their objects are not
# interchangeable - an x86-64 `.o` in `build/kernel/` would be found by
# make, be newer than its source, and be linked into an ARM image, which
# fails at the link with a message about incompatible formats and no hint at
# all about why. Left out of the name for aarch64 so that every path in
# every document that was written before there was a second one still says
# what it says.
VARIANT := $(if $(filter-out aarch64,$(ARCH)),-$(ARCH))$(if $(TEST),-test)$(if $(BENCH),-bench)$(if $(WEB),-web)$(if $(FFMPEG),-ffmpeg)

#
# **Defined here, beside VARIANT, and not beside the flags that use it.**
# `UCFLAGS` and `ULDFLAGS` are `:=` assignments and expand it on the spot;
# further down it expanded to nothing, and what that produced was a userland
# compiled with `-DKOSMOS_USER_BASE=` and an `#error` that fires. That is
# the good outcome - the same mistake with `X86_BUILD` silently produced a
# generated file named `/font_8x16.c`, and with `FULL` and `VARIANT` it
# produced an image built at the wrong size. Three times now.
#
# Where a process image is linked, which has to agree with `USER_VA_BASE` in
# `arch/$(ARCH)/mmu.h`. The linker cannot read a C header, so those two are
# the irreducible pair; everything else takes it from here.
#
# Three things consume it: `user/user.ld` through `--defsym`, and
# `user/include/kosmos.h` through `-DKOSMOS_USER_BASE`, which is where the
# userland's own idea of its heap and stack comes from. That header used to
# write the number a third time and it went wrong the first time anything
# changed it - see the comment there.
#
# **The two architectures differ and the reason is the code model.** x86-64
# addresses static data with a sign-extended 32-bit displacement by default,
# which reaches -2GB to +2GB - and 0x80000000 is exactly the first address
# it cannot. The whole image fails to link with `relocation truncated to
# fit`, hundreds of times. So the user region there begins at 1 GB, which is
# also the boundary of the first PDPT slot: the kernel gets slot 0 and a
# process gets 1 upward, which is the same arrangement AArch64 has one level
# up. `mmu_init` panics if RAM would reach that far.
USER_BASE := $(if $(filter x86_64,$(ARCH)),0x40000000,0x80000000)

# What the image was built for, in the boot banner and along the bottom of
# the desktop. One place, because it was two: the kernel's generated
# version.c had it right and the userland's had "QEMU virt aarch64" written
# into it, so the desktop on the x86 machine named the other one.
#
# **What it was built for, not what it is running on.** The PC build said
# "QEMU q35 x86-64" here, and a ThinkPad printed that as its Host in
# `neofetch` and its Platform in About. The PC board reads the machine's name
# out of SMBIOS now (`hal_machine_ident`), so this names the board, which is
# true of every machine the image boots. The ARM string keeps QEMU in it
# because it is true there: that board's addresses are `virt`'s, and it runs
# on nothing else.
PLATFORM := $(if $(filter x86_64,$(ARCH)),PC x86-64,QEMU virt aarch64)


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

# Both halves of user/bin/, in one list. `apps/` and `programs/` are a
# reading order for whoever opens the tree; `/bin` itself is flat, and
# `progs2c.py` serves each file under its basename.
#
# **An app is one file or a directory, and both are gathered here.** Most
# are a single `.lua`. The four with a vendored engine under them - Doom,
# Quake, the Super Nintendo and the browser - own a directory with
# their own C in it, because that C is not reusable and never was: it is
# the binding to one engine, for one app. Diego, 23 September: "a kit is a
# reusable piece of code that an app, service, or server can leverage and
# reuse. doom is an specific app."
BIN_LUA := $(wildcard user/bin/apps/*.lua) $(wildcard user/bin/apps/*/*.lua) \
           $(wildcard user/bin/programs/*.lua)

LUA_FILES := user/init/init.lua $(BIN_LUA) $(wildcard user/installed/*/*.lua) \
             $(wildcard user/lib/*.lua) $(wildcard user/lib/translators/*.lua) \
             $(wildcard user/lib/wm/*.lua) $(wildcard user/lib/groove/*.lua) \
             $(wildcard user/tests/*.lua)

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
        hal/qemu-virt/virtio.c \
        hal/virtio/wait.c \
        hal/virtio/net.c \
        hal/virtio/snd.c \
        hal/qemu-virt/snd_bind.c \
        hal/fwcfg/fwcfg.c \
        hal/qemu-virt/fwcfg_mmio.c \
        hal/fwcfg/fbpixels.c \
        hal/fwcfg/ramfb.c \
        hal/virtio/gpu.c \
        hal/qemu-virt/fb.c \
        hal/virtio/input.c \
        hal/qemu-virt/input_bind.c \
        hal/keys.c \
        hal/pointer_edges.c \
        hal/qemu-virt/input_describe.c \
        hal/virtio/blk.c \
        hal/qemu-virt/blk_bind.c \
        hal/virtio/rng.c \
        hal/qemu-virt/entropy_bind.c \
        hal/qemu-virt/devices.c \
        kernel/console.c \
        kernel/screen.c \
        kernel/boot.c \
        $(GEN)/version.c \
        $(GEN)/font_8x16.c \
        runtime/libc/string.c \
        runtime/libc/setjmp-$(ARCH).S \
        kernel/panic.c \
        kernel/pmm.c \
        kernel/pmm_place.c \
        kernel/pool.c \
        kernel/thread.c \
        kernel/sched_rr.c \
        kernel/sched_prio.c \
        kernel/ipc.c \
        kernel/memobj.c \
        kernel/sharemap.c \
        kernel/irq.c \
        kernel/process.c \
        kernel/smp.c \
        kernel/spinlock.c \
        kernel/syscall.c \
        kernel/profile.c \
        kernel/entropy.c \
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
# linit.c is out too, since it opens all of them; user/init/lua_glue.c opens
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
  SRCS      += user/hello-$(ARCH).S user/faulty-$(ARCH).S user/pointer-$(ARCH).S
  # The libc the kernel no longer links, because the unit tests for it are
  # here and they call it directly. The shipping image needs none of it.
  SRCS      += runtime/libc/malloc.c \
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
#
# **No heap flag, on either board, because the allocator grows.**
#
# `make DOOM=1` used to carry `-DUSER_HEAP_PAGES=3072`: Doom asks for a six
# megabyte zone before it draws anything and a fixed two megabyte heap
# could not hold it. That is a compile-time answer to a runtime question,
# and `runtime/libc/malloc.c` replaced it - `grow()` asks the kernel for
# another arena when the bins run dry, so a program that needs memory asks
# for it and one that does not never pays.
#
# **The flag outlived the reason and cost a quarter of a gigabyte.** The
# kernel allocates *and zeroes* `USER_HEAP_PAGES` for every process it
# builds, so twelve megabytes for Doom was twelve megabytes for the shell,
# the Deskbar, `binfs` and eighteen others: 21 processes at 13 MB each on a
# 507 MB machine. At the default they start at two and grow if they ever
# need to.
#
# It also had to be in two flag lists at once, which is how it was found.
# `X86_FLAGS` never had it, so that kernel mapped 512 pages while the
# userland compiled into it was built for 3072 - and `heap_init` was told
# it managed twelve megabytes of which two were mapped. The allocator never
# called `grow()`, because it believed it still had initial arena left. The
# TinyGL demos died at USER_HEAP + exactly 512 pages.
#
# One number in `user/include/kosmos.h` now, defaulted and not overridden,
# so there is no longer a `-D` that can be passed to one side and not the
# other.
#
CFLAGS_BASE := \
               -std=c11 -ffreestanding -nostdlib -nostartfiles \
               -Wall -Wextra -Werror -fno-common -fno-strict-aliasing \
               -O2 -g \
               -Iarch/$(ARCH) -Ihal -Ihal/virtio -Ihal/fwcfg -Ikernel -Iruntime/include -Iuser \
               $(TESTDEFS)

CFLAGS := $(CFLAGS_BASE) -mgeneral-regs-only

# The exceptions, and why there are any.
#
# -mgeneral-regs-only exists because the kernel may not touch an FP or SIMD
# register at all: lazy FP save (`arch/*/fp.c`) disarms them on a switch and
# serves the first use with a trap, which a kernel thread must never take.
# That reasoning covers kernel/, arch/ and hal/, and it still does.
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

# ------------------------------------------------------------------
# The maths that is numerical analysis rather than bit manipulation.
#
# **This was `-lm` and the `-lm` was newlib's**, which worked because ARM's
# official GNU toolchain happens to bundle it. Homebrew's `x86_64-elf-gcc`
# ships `libgcc.a` and nothing else, so the second architecture found a
# dependency on what a toolchain vendor chose to package rather than on
# anything this system decided. `runtime/upstream/musl-math/README.kosmos.md`
# has the whole of it.
#
# Forty-five files, compiled here rather than linked from an archive,
# because the closure was taken over undefined symbols and every one of
# them is reached.
# ------------------------------------------------------------------
MUSL := runtime/upstream/musl-math
MUSL_SRCS := $(wildcard $(MUSL)/src/math/*.c)

# `hidden` and `weak` are ELF visibility attributes musl's own build injects
# through a generated vis.h. This system links one static binary and has no
# use for either. Nothing in the vendored tree is modified; these come from
# outside it, which is the same arrangement lua/upstream/ has.
#
# runtime/include first, so musl compiles against this system's headers.
# See the README for why musl's own include/ is not on this list.
MUSL_CFLAGS := -std=c99 -w -Dhidden= -Dweak= \
               -I$(MUSL)/src/internal -I$(MUSL)/arch/$(ARCH) \
               -I$(MUSL)/arch/generic -I$(MUSL)/include

#
# **libgcc, on the userland's link and nowhere else**: the compiler's own
# support routines, which `-nostdlib` leaves out along with the C library it
# was aimed at. FFmpeg's `av_sscanf` reads a number into a `long double`,
# which on AArch64 is a 128-bit float with no instructions behind it, so
# the compiler turns its arithmetic into calls - `__addtf3`, `__multf3` -
# that only libgcc defines. It is not a C library and brings none in; a
# function comes out of the archive only when a call names it. On x86-64 a
# `long double` is the x87's, in hardware, and nothing is taken.
#
LIBS := -lgcc

# The kernel links none of it. It has no floats by construction, and the one
# caller it ever had was Lua.
KLIBS :=

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
# Under build/, with everything else this repository generates.
# ------------------------------------------------------------------

# Varies with the image for the same reason GEN does: the test and bench
# init.bin files are different binaries, and sharing a directory would mean
# sharing objects compiled with different flags.
#
# **This was `build-user$(VARIANT)` at the top level, and it made ten
# directories.** One per variant - `build-user-doom-web`,
# `build-user-x86_64-test`, and so on - each beside `arch/`, `kernel/` and
# `user/` in every file listing, and 802 MB of them. `make clean` removed
# four of the ten, which is how the other six accumulated.
#
# The comment here used to say a distinct top-level directory was necessary,
# because `build/user/x.c.o` would also match the kernel's `build/%.c.o`
# pattern "and which rule won would depend on how make breaks ties". That
# reasoning does not survive looking at the paths. An object keeps its
# source's path under the build root, so the userland's are
# `build/user/user/kits/gfx/gfx.c.o` - and the kernel's pattern matches that only
# with a stem of `user/user/kits/gfx/gfx`, whose prerequisite
# `user/user/kits/gfx/gfx.c` does not exist. Make discards a pattern rule whose
# prerequisites cannot be made, so there is no tie to break.
#
# It is also checked rather than argued: the kernel is built with
# `-mgeneral-regs-only`, so a userland file compiled by the wrong rule does
# not silently succeed - the first `float` in musl's math is a compile
# error. Every variant builds.
UBUILD := build/user$(VARIANT)

# TinyCC's source and the patched copy built from it (`docs/tinycc.md`): for
# the Mac's cross compilers in every build, and for the C Kit in the full one.
TCC_UP   := runtime/upstream/tinycc
TCC_HOST := $(HOSTDIR)/tinycc
TCC_KIT_TARGET := $(if $(filter x86_64,$(ARCH)),TCC_TARGET_X86_64,TCC_TARGET_ARM64)

USER_LIBC := runtime/libc/string.c \
             runtime/libc/malloc.c \
             runtime/libc/math.c \
             runtime/libc/snprintf.c \
             runtime/libc/strtod.c \
             runtime/libc/stdio.c \
             runtime/libc/scan.c \
             runtime/libc/time.c \
             runtime/libc/setjmp-$(ARCH).S \
             runtime/libc/callstack-$(ARCH).S \
             user/init/misc_user.c \
             user/init/clock_user.c \
             user/init/panic_user.c

#
# Quake, from Chocolate Quake, GPL like Doom - and, like Doom, an installed
# application since 28 September (`docs/elf.md` step 5): `make apps` links
# `QUAKE_SRCS` into `apps/quake.elf`, which lives in `/Home/Apps/Quake`
# beside `quake.lua` and the pak, and no system image carries it. It was
# `make QUAKE=1`, outside `FULL=1`.
#
# Chocolate Quake rather than the quakegeneric this began from, because
# quakegeneric says it builds only for 32-bit machines and Kosmos is only
# 64-bit; Chocolate Quake ships arm64 builds of the same WinQuake 1.09 code.
#
# **Two lists.** `QUAKE_ENGINE` is upstream's - 78 files, which `make quake`
# compiles on their own to say whether they still build against the shim.
# `QUAKE_SRCS` is what goes into the image: the engine, and
# `user/installed/Quake/quake_kosmos.c`, the platform under it (`Sys_*`, `VID_*`, `IN_*`,
# `SNDDMA_*`), which is Kosmos's and held to the ordinary flags.
#
# **Left out, to be replaced rather than patched**: `main.c`,
# `sys/src/sys.c`, all of `video/src/`, the four SDL input files,
# `snd_sdl.c`, music and its codecs (`bgmusic.c`, `snd_codec.c`,
# `snd_wave.c`, MP3, Vorbis, FLAC), the UDP driver and the datagram driver
# over it (`net_udp.c`, `net_dgrm.c`) with the table naming them
# (`net_drivers.c`), and `end_screen/`. `net_socket.c` stays: it is the
# engine's own book of connections, and Loopback keeps one too.
#
# **`-iquote` rather than `-I` for Quake's own headers.** `console.h` and
# `screen.h` are also the names of headers in `kernel/`, which every
# userland compile has on its path; `-iquote` directories are searched first
# for `#include "..."`, so no ordering of flags can pick the wrong one.
# `-Iuser/installed/Quake` is the SDL shim the engine's `#include <SDL.h>` and
# `<SDL_stdinc.h>` resolve to, so nothing upstream released is touched.
#
# `HAVE_STRCPY` and its two siblings are what upstream's CMake detects, and
# this libc has all three.
#
QUAKE_DIR := runtime/upstream/quake/src

QUAKE_INCLUDES := $(addprefix -iquote ,$(wildcard $(QUAKE_DIR)/*/include)) \
                  -iquote $(QUAKE_DIR) \
                  -Iuser/installed/Quake \
                  -include user/installed/Quake/kosmos_quake.h

QUAKE_CFLAGS := -w -Wno-error \
                -DHAVE_STRCPY -DHAVE_STRNCPY -DHAVE_STRCAT \
                $(QUAKE_INCLUDES)

QUAKE_ENGINE := $(addprefix $(QUAKE_DIR)/, \
                  camera/src/chase.c \
                  camera/src/view.c \
                  client/src/cl_demo.c \
                  client/src/cl_input.c \
                  client/src/cl_main.c \
                  client/src/cl_parse.c \
                  client/src/cl_tent.c \
                  cmd/src/cmd.c \
                  common/src/com_argv.c \
                  common/src/com_byte.c \
                  common/src/com_ext.c \
                  common/src/com_fs.c \
                  common/src/com_init.c \
                  common/src/com_link.c \
                  common/src/com_msg.c \
                  common/src/com_sizebuf.c \
                  common/src/com_stdio.c \
                  common/src/com_stdlib.c \
                  common/src/com_string.c \
                  common/src/com_token.c \
                  common/src/com_va.c \
                  console/src/console.c \
                  console/src/cvar.c \
                  crc/src/crc.c \
                  host/src/host.c \
                  host/src/host_cmd.c \
                  input/src/keys.c \
                  mathlib/src/mathlib.c \
                  memory/src/zone.c \
                  menu/src/menu.c \
                  model/src/model.c \
                  net/src/net_loop.c \
                  net/src/net_main.c \
                  net/src/net_poll.c \
                  net/src/net_socket.c \
                  net/src/net_vcr.c \
                  progs/src/pr_cmds.c \
                  progs/src/pr_edict.c \
                  progs/src/pr_exec.c \
                  renderer/src/d_edge.c \
                  renderer/src/d_fill.c \
                  renderer/src/d_init.c \
                  renderer/src/d_modech.c \
                  renderer/src/d_part.c \
                  renderer/src/d_polyse.c \
                  renderer/src/d_scan.c \
                  renderer/src/d_sky.c \
                  renderer/src/d_sprite.c \
                  renderer/src/d_surf.c \
                  renderer/src/d_vars.c \
                  renderer/src/d_zpoint.c \
                  renderer/src/draw.c \
                  renderer/src/nonintel.c \
                  renderer/src/r_aclip.c \
                  renderer/src/r_alias.c \
                  renderer/src/r_bsp.c \
                  renderer/src/r_draw.c \
                  renderer/src/r_edge.c \
                  renderer/src/r_efrag.c \
                  renderer/src/r_light.c \
                  renderer/src/r_main.c \
                  renderer/src/r_misc.c \
                  renderer/src/r_part.c \
                  renderer/src/r_sky.c \
                  renderer/src/r_sprite.c \
                  renderer/src/r_surf.c \
                  renderer/src/r_vars.c \
                  screen/src/screen.c \
                  server/src/sv_main.c \
                  server/src/sv_move.c \
                  server/src/sv_phys.c \
                  server/src/sv_user.c \
                  server/src/sv_world.c \
                  sound/src/snd_dma.c \
                  sound/src/snd_mem.c \
                  sound/src/snd_mix.c \
                  status_bar/src/sbar.c \
                  wad/src/wad.c)

QUAKE_SRCS := $(QUAKE_ENGINE) user/installed/Quake/quake_kosmos.c

#
# The Record Kit's two vendored halves, `minih264e` and `minimp4`, on their
# own terms - `-w -Wno-error`, for the reason every vendored thing gets it -
# each in a file of its own (`user/kits/record/record_config.h` says how and
# why). The kit's own two files keep every warning.
#
RECORD_CFLAGS := -w -Wno-error

#
# ufbx, which reads FBX for the 3D Kit (`runtime/upstream/ufbx/`), on its
# own terms too. Its switches are in a header of ours,
# `user/kits/3d/k3d_ufbx.h`, which `ufbx.h` includes first when
# `UFBX_CONFIG_HEADER` names it - so `k3d_fbx.c`, which keeps every
# warning, is compiled against the configuration ufbx was, and neither
# vendored file is touched.
#
UFBX_CONFIG := -Iruntime/upstream/ufbx -Iuser/kits/3d '-DUFBX_CONFIG_HEADER="k3d_ufbx.h"'
UFBX_CFLAGS := -w -Wno-error $(UFBX_CONFIG)

# BearSSL (`runtime/upstream/bearssl/`), the TLS Kit's protocol, on its own
# terms: `-w`, as vendored code is, and the parts that would reach an
# operating system switched off by `-D` rather than by an edit - its
# randomness is injected from `SYS_ENTROPY` and a certificate's dates are
# held to the machine's clock, both by the kit (`user/kits/tls/`).
BEARSSL_SRCS   := $(sort $(wildcard runtime/upstream/bearssl/src/*.c \
                                    runtime/upstream/bearssl/src/*/*.c))
BEARSSL_IFLAGS := -Iruntime/upstream/bearssl/inc
BEARSSL_CFLAGS := -w -Wno-error -Iruntime/upstream/bearssl/src \
                  -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 \
                  -DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0 -DBR_RDRAND=0

# libsmb2 (`runtime/upstream/libsmb2/`), the SMB Kit's protocol, on its own
# terms (`docs/sharing.md` step N2): `-w`, as vendored code is, against a
# `config.h` of Kosmos's own that is the whole port - its socket a
# connection on the network stack's ring, its randomness the kernel's
# (`user/kits/smb/port/`). Left out of the build, by name: its own
# cryptography, which `user/kits/smb/smb_crypto.c` gives to the Crypto Kit
# and BearSSL instead; its signing, `smb2-signing.c`, whose AES-CMAC keyed
# AES again every sixteen bytes and which `user/kits/smb/smb_signing.c`
# supplies in full over the kit's (step N4); Kerberos; the synchronous API,
# a `poll` loop; and `compat.c`, other platforms' shims. Nothing in the tree
# is edited.
LIBSMB2 := runtime/upstream/libsmb2
LIBSMB2_LEFT_OUT := aes aes128ccm aes_reference aes_apple md4c md5 hmac-md5 \
                    hmac sha1 sha224-256 sha384-512 usha smb2-signing \
                    krb5-wrapper sync compat
LIBSMB2_SRCS := $(filter-out $(patsubst %,$(LIBSMB2)/lib/%.c,$(LIBSMB2_LEFT_OUT)),\
                             $(sort $(wildcard $(LIBSMB2)/lib/*.c)))
SMB_IFLAGS := -DHAVE_CONFIG_H -Iuser/kits/smb/port -Iuser/kits/smb \
              -isystem $(LIBSMB2)/include -isystem $(LIBSMB2)/include/smb2 \
              -isystem $(LIBSMB2)/lib $(BEARSSL_IFLAGS)
LIBSMB2_CFLAGS := -w -Wno-error $(SMB_IFLAGS)

# The SMB Kit's own C and smbfs: libsmb2's headers on the path as system
# headers, so its warnings stay its own, and every warning on for ours.
SMB_KOSMOS := user/kits/smb/smb_transport.c user/kits/smb/smb_crypto.c \
              user/kits/smb/smb_signing.c user/kits/smb/ntlm_name.c \
              user/servers/smbfs.c

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

USER_SRCS := user/init/start-$(ARCH).S \
             user/init/main.c \
             user/servers/audio.c \
             user/servers/devices.c \
             user/servers/binfs.c \
             user/servers/appfs.c \
             user/servers/notify.c \
             $(SMB_KOSMOS) \
             $(LIBSMB2_SRCS) \
             user/servers/console.c \
             user/servers/ramfs.c \
             user/servers/ramstore.c \
             user/servers/net.c \
             user/servers/drives.c \
             user/servers/drives_decode.c \
             user/servers/fat_decode.c \
             user/servers/diskfs.c \
             user/servers/clock_epoch.c \
             user/servers/keyring.c \
             user/servers/keyfile.c \
             user/servers/kfs.c \
             user/servers/diskcache.c \
             user/servers/packflat.c \
             user/drivers/net/e1000.c \
             user/drivers/net/e1000_decode.c \
             user/drivers/usb/xhci.c \
             user/drivers/usb/usb_decode.c \
             user/drivers/usb/pad_decode.c \
             user/drivers/usb/uvc_decode.c \
             user/drivers/usb/midi_decode.c \
             user/drivers/usb/storage_decode.c \
             user/drivers/display/backlight.c \
             user/drivers/display/backlight_decode.c \
             user/drivers/power/powerbutton.c \
             user/init/say.c \
             user/kits/network/net_kosmos.c \
             user/kits/crypto/crypto.c \
             user/kits/crypto/crypto_kosmos.c \
             user/kits/crypto/md4.c \
             user/kits/crypto/cmac.c \
             user/kits/crypto/ccm.c \
             user/kits/crypto/kdf.c \
             user/init/lua_glue.c \
             user/init/sys_user.c \
             user/init/elfimage.c \
             user/kits/gfx/gfx.c \
             user/kits/gfx/shadow.c \
             user/kits/gfx/yuv.c \
             user/kits/gfx/pack.c \
             user/kits/gfx/rows.c \
             user/kits/window/window.c \
             user/init/syscalls.c \
             user/kits/game/game.c \
             user/kits/game/gamesoft.c \
             user/kits/3d/k3d_mesh.c \
             user/kits/3d/k3d_raster.c \
             user/kits/3d/k3d_trace.c \
             user/kits/3d/k3d_texture.c \
             user/kits/3d/k3d_formats.c \
             user/kits/3d/k3d_fbx.c \
             runtime/upstream/ufbx/ufbx.c \
             user/kits/3d/k3d_kosmos.c \
             user/kits/gfx/png.c \
             user/kits/gfx/jpeg.c \
             user/kits/gfx/docfont.c \
             user/kits/gfx/face.c \
             user/kits/compress/inflate.c \
             user/kits/compress/deflate.c \
             user/kits/compress/gzip.c \
             user/kits/compress/base64.c \
             user/kits/synth/synth_dsp.c \
             user/kits/synth/synth_engine.c \
             user/kits/synth/synth_lua.c \
             user/kits/synth/synth_kosmos.c \
             user/kits/pdf/pdftok.c \
             user/kits/gl/gl_kosmos.c \
             user/kits/console/con_kosmos.c \
             user/kits/mp3/mp3_kosmos.c \
             user/kits/record/record_kosmos.c \
             user/kits/record/record_core.c \
             user/kits/record/record_h264.c \
             user/kits/record/record_mp4.c \
             $(MUSL_SRCS) \
             runtime/upstream/miniz/miniz.c \
             $(BEARSSL_SRCS) \
             user/kits/tls/tls_kosmos.c \
             $(TINYGL_SRCS) \
             $(TINYGL_DEMO_SRCS) \
             user/kits/gl/gl_demos.c \
             $(GEN)/font_8x16.c \
             $(GEN)/programs.c \
             $(GEN)/version.c \
             $(GEN)/assets.c \
             $(GEN)/wallpapers.c \
             $(GEN)/fonts.c \
             runtime/upstream/stb/stb_impl.c \
             $(GEN)/libraries.c \
             lua/kosmos/serialize.c \
             $(USER_LIBC) \
             $(LUA_SRCS) \
             $(GEN)/init_lua.c \
             $(GEN)/tabletext_lua.c \
             $(GEN)/protostamp.c

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
# There was a `make DOOM=1` here, and `FULL=1` turned it on: Doom compiled
# into the system's image, which made every ordinary image a GPLv2 work and
# every process pay for a megabyte one of them called. **Doom is an installed
# application now** (`docs/elf.md` step 5) - `apps/doom.elf`, linked below
# beside `apptest.elf` and built by `make apps` - so there is no variant to
# choose and no image to name after it.
#
#
# `FFMPEG=1` - FFmpeg's H.264 decoder, which is the H.264 Kit
# (`user/kits/ffmpeg/`, `roadmap.md` 4e), on in `FULL=1`.
#
# **The files are the closure, not a choice.** `tools/ffmpeg_vendor.py`
# configures FFmpeg 9.0.2 for this system, compiles everything its build
# would, and keeps what the linker pulls in from the kit's entry points -
# 95 objects of about 170, and their headers, into `runtime/upstream/ffmpeg/`
# unmodified. It writes the list below it too, in `ffmpeg.mk`, so the two
# cannot disagree; `runtime/upstream/ffmpeg/README.kosmos.md` is the account.
#
# FFmpeg's headers and the configuration for this system go *first* on the
# include path, ahead of everything UCFLAGS names: its sources include
# `"config.h"` and `<stdatomic.h>` by names nothing else here may answer.
# `-DHAVE_AV_CONFIG_H` is what FFmpeg's own build adds to every library
# object, and without it its headers never include the configuration at all.
# `-std=c17 -O3` and the rest are what its `configure` chose; `-w -Wno-error`
# because its warnings are not ours to fix, for the reason Doom's are not.
#
# LGPL 2.1 or later, linked statically like everything here; `LICENSE`
# lists it and the About window reads that out.
#
include user/kits/ffmpeg/ffmpeg.mk

FFMPEG_DIR    := runtime/upstream/ffmpeg
FFMPEG_KOSMOS := user/kits/ffmpeg/ffmpeg_log.c user/kits/ffmpeg/h264_core.c \
                 user/kits/ffmpeg/aac_core.c
FFMPEG_SRCS   := $(addprefix $(FFMPEG_DIR)/,$(addsuffix .c,$(FFMPEG_NAMES))) \
                 $(FFMPEG_KOSMOS) user/kits/ffmpeg/h264_kosmos.c \
                 user/kits/ffmpeg/aac_kosmos.c
FFMPEG_IFLAGS := -Iuser/kits/ffmpeg/config -I$(FFMPEG_DIR)
FFMPEG_CFLAGS := -std=c17 -U__STRICT_ANSI__ -O3 -fno-math-errno \
                 -fno-signed-zeros -w -Wno-error -DHAVE_AV_CONFIG_H \
                 $(FFMPEG_CPPFLAGS)

ifdef FFMPEG
USER_SRCS += $(FFMPEG_SRCS)
endif

#
# LakeSnes, a Super Nintendo - an installed application since 28 September
# (`docs/elf.md` step 5): `make apps` links `SNES_SRCS` into `apps/snes.elf`,
# in `/Home/Apps/SNES` beside `snes.lua`. It was `SNES=1`, on in `FULL=1`,
# and every process paid its writable data for the one that ran it.
#
# The twelve files upstream's own Makefile names for the core, listed rather
# than globbed for the reason Doom's are. Its SDL frontend, tracer and zip
# reader are not built: `user/installed/SNES/snes_kosmos.c` stands where the frontend
# was. `runtime/upstream/lakesnes/README.kosmos.md` is the account.
#
SNES_DIR   := runtime/upstream/lakesnes/snes
SNES_NAMES := spc dsp apu cpu dma ppu cart cx4 input statehandler snes \
              snes_other
SNES_SRCS  := $(addprefix $(SNES_DIR)/,$(addsuffix .c,$(SNES_NAMES))) \
              user/installed/SNES/snes_kosmos.c user/installed/SNES/snes_blit.c
SNES_CFLAGS := -w -Wno-error -iquote $(SNES_DIR)

#
# **Doom is an installed application now, not part of the system**
# (`docs/elf.md` step 5): its engine and its binding are linked into an
# image of their own, `apps/doom.elf`, which goes in `/Home/Apps/Doom` beside
# `doom.lua` and its WAD, and the system's image carries none of it. So what
# follows is defined whatever the build, and nothing adds it to `USER_SRCS`.
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
             user/installed/Doom/doom_kosmos.c

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
# It needed the kernel to agree, because it is the kernel that maps the
# heap when it builds the process - and `DOOM_HEAP` was already dead, the
# flag having moved into CFLAGS_BASE and nothing reading this. Both are
# gone: `grow()` asks for another arena when the first runs out.
#
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

#
# `make WEB=1 qemu` builds an image with the NetSurf parsing stack in it.
#
# **A build option for the reason Doom was one: size.** These are five
# libraries and a hundred and forty thousand lines, and a build that does not
# want a browser should not carry a CSS engine. When the GPLv2 browser core
# joins them the flag will be doing licence work too, as `DOOM` did before
# Doom became an application of its own - and drawing that line in the build rather than in a comment is the
# same argument, because a boundary somebody has to remember is not one.
#
# **No heap flag**, which was once a thing worth saying about this build in
# particular and is now true of every build: nothing carries
# `-DUSER_HEAP_PAGES` any more, because the heap grows and a program that
# needs more asks for it. See the note above `CFLAGS_BASE`.
#
ifdef WEB

NS       := runtime/upstream/netsurf
WEB_LIBS := libwapcaplet libparserutils libhubbub libcss libdom libsvgtiny

#
# Everything under each library's `src`, minus the one file that is a *build
# tool*: `css_property_parser_gen.c` has a `main()`, is compiled with the
# host compiler below, and emits the 119 property parsers the target needs.
# Cross-compiling it looks like 117 undefined `css__parse_*` symbols.
#
WEB_SRCS := $(filter-out %/css_property_parser_gen.c, \
              $(foreach l,$(WEB_LIBS),$(shell find $(NS)/$(l)/src -name '*.c')))

#
# And libdom's hubbub binding, which is the whole point and is not under
# `src`. `bindings/xml/` is its sibling and stays out: it wants expat or
# libxml, and neither is here.
#
WEB_SRCS += $(NS)/libdom/bindings/hubbub/parser.c

#
# **And its XML binding, with expat beneath it** (`roadmap.md` 6zz j5): an
# SVG is XML, and libsvgtiny reads one through libdom's XML parser, which is
# expat's. Expat 2.8.5, vendored in `runtime/upstream/expat/` as released,
# configured by `runtime/config/expat_config.h` rather than by a `configure`
# that would have to run here.
#
WEB_SRCS += $(NS)/libdom/bindings/xml/expat_xmlparser.c
EXPAT := runtime/upstream/expat
WEB_SRCS += $(EXPAT)/lib/xmlparse.c $(EXPAT)/lib/xmlrole.c \
            $(EXPAT)/lib/xmltok.c $(EXPAT)/lib/random_arc4random_buf.c
EXPAT_CFLAGS := -w -Wno-error -DHAVE_EXPAT_CONFIG_H -Iruntime/config \
                -I$(EXPAT)/lib

#
# Two of libdom's files, patched as the build makes them and compiled in place
# of the upstream ones, which stay as NetSurf released them - the patches and
# why are in `runtime/patches/netsurf/`. Made under a prefix of their own:
# `$(GEN)/netsurf/%.c` has a rule that gives libcss's include paths, and Mac's
# make 3.81 would take whichever pattern it met first.
#
WEB_PATCHED := libdom/src/events/event_target.c libdom/src/events/dispatch.c \
               libdom/src/core/element.c
WEB_SRCS := $(filter-out $(addprefix $(NS)/,$(WEB_PATCHED)),$(WEB_SRCS)) \
            $(addprefix $(GEN)/nspatched/,$(WEB_PATCHED))

# Kept rather than deleted as intermediates, so what was compiled can be read.
.SECONDARY: $(addprefix $(GEN)/nspatched/,$(WEB_PATCHED))

# And headers, which a patched copy replaces by being found first: NetSurf
# includes them by their path from the top of its tree, so the copies' tree
# is on the include path ahead of it (`WEB_NSB_CFLAGS`), and they are made
# before anything is compiled (`WEB_GEN`).
WEB_PATCHED_H := netsurf/content/fetch.h
.SECONDARY: $(addprefix $(GEN)/nspatched/,$(WEB_PATCHED_H))

# Kosmos's own side of it, held to the ordinary flags rather than the
# vendored ones - it is not vendored.
WEB_SRCS += user/bin/apps/browser/web_kosmos.c user/bin/apps/browser/web_select.c user/bin/apps/browser/web_style.c \
            user/bin/apps/browser/web_paint.c user/bin/apps/browser/web_netsurf.c \
            user/bin/apps/browser/web_svg.c user/bin/apps/browser/web_raster.c

#
# **NetSurf's layout** (`roadmap.md` 6zz j): the box tree, layout, tables,
# flex and the drawing of boxes, with the CSS selection and the utilities
# they use - NetSurf 3.11's own files, as released, in
# `runtime/upstream/netsurf/netsurf/` (its README says which and why).
# What they call of the browser around them is `web_netsurf.c`.
#
# GPLv2, so an image carrying the browser is a GPLv2 work as a whole
# (`LICENSE`); Kosmos's own sources stay MIT.
#
# Two build switches that are NetSurf's own: the filters its log reads
# its level from, which `utils/nsoption.c` names in its option table.
#
NSB := $(NS)/netsurf
WEB_NETSURF := \
    content/handlers/html/box_construct.c content/handlers/html/box_inspect.c \
    content/handlers/html/box_manipulate.c content/handlers/html/box_normalise.c \
    content/handlers/html/box_special.c content/handlers/html/font.c \
    content/handlers/html/layout.c content/handlers/html/layout_flex.c \
    content/handlers/html/redraw.c content/handlers/html/redraw_border.c \
    content/handlers/html/table.c \
    content/handlers/html/forms.c content/handlers/html/form.c \
    content/handlers/html/box_textarea.c desktop/textarea.c utils/url.c \
    content/handlers/css/select.c content/handlers/css/hints.c \
    content/handlers/css/internal.c content/handlers/css/dump.c \
    desktop/plot_style.c desktop/system_colour.c \
    utils/corestrings.c utils/nsoption.c utils/talloc.c \
    utils/nsurl/nsurl.c utils/nsurl/parse.c
WEB_SRCS += $(addprefix $(NSB)/,$(WEB_NETSURF))
WEB_NSB_CFLAGS := -I$(GEN)/nspatched/netsurf \
                  -I$(NSB) -I$(NSB)/include -I$(NSB)/content/handlers \
                  '-DNETSURF_BUILTIN_LOG_FILTER="level:WARNING"' \
                  '-DNETSURF_BUILTIN_VERBOSE_FILTER="level:VERBOSE"'

# The property names, read out of the same file their own build reads.
WEB_PROPS   := $(shell sed -n 's/^\([^\#][^:]*\):.*/\1/p' \
                 $(NS)/libcss/src/parse/properties/properties.gen)
WEB_GEN_CSS := $(addprefix $(GEN)/netsurf/css/autogenerated_,\
                 $(addsuffix .c,$(WEB_PROPS)))

# The three that are not CSS parsers: two perl scripts and a gperf run.
WEB_GEN := $(GEN)/netsurf/aliases.inc \
           $(GEN)/netsurf/entities.inc \
           $(GEN)/netsurf/treebuilder/autogenerated-element-type.c \
           $(GEN)/netsurf/dom/bindings/hubbub/parser.h \
           $(GEN)/netsurf/dom/bindings/xml/xmlparser.h \
           $(GEN)/netsurf/autogenerated_colors.c \
           $(addprefix $(GEN)/nspatched/,$(WEB_PATCHED_H))

USER_SRCS += $(WEB_SRCS) $(WEB_GEN_CSS)

#
# **The C Kit** (`docs/tinycc.md`, step C3): TinyCC inside Kosmos, in the
# full image alone - the applications' images never compile anything, and
# 0.4 MB in each would be for nothing. Its own three files are ordinary
# userland C; TinyCC itself is compiled from the patched copy the Mac's
# cross compilers are built from (`TCC_HOST`), with `shim.h` forced in -
# what TinyCC asks of a Unix, answered by the kit and not by the libc.
#
USER_SRCS += user/kits/tcc/tcc_kosmos.c user/kits/tcc/shim.c user/kits/tcc/stamp.c \
             $(TCC_HOST)/libtcc.c

#
# `-w -Wno-error` for the reason Doom and TinyGL get them: vendored code is
# not modified, so it is not held to flags it was never written under.
#
# `-DWITHOUT_ICONV_FILTER` is libparserutils's own switch. There is no
# `iconv` here and its built-in codecs are what this system can offer.
#
#
# **The private `src` trees are NOT on this path, and that is the whole
# subtlety of building these five together.** Four of them have their own
# `utils/utils.h` and two have their own `utils/parserutilserror.h`, so one
# shared include path means whichever library is named first wins and the
# others silently get its headers. What that looks like is libcss failing on
# `css_error_from_parserutils_error` while gcc helpfully suggests
# `hubbub_error_from_parserutils_error` - a real collision wearing a typo's
# clothes.
#
# The public `include` trees are safe to share, because every one of them is
# namespaced by the library's own name. Each object gets its own `src` added
# by the rule below, derived from the path it is being built from.
#
#
# `-fcommon`, and it is worth knowing exactly what it buys.
#
# `libcss/src/stylesheet.h` ends a struct with `} _ALIGNED;` where `_ALIGNED`
# is meant to be an attribute macro - except it is defined nowhere in libcss
# and appears in no other file. So it parses as a *variable name*, and every
# translation unit including that header declares one at file scope.
#
# Under the pre-GCC-10 default those merge into a common symbol and nobody
# notices, which is why upstream builds. `CLAUDE.md` makes `-fno-common`
# mandatory here, so instead they are 180 definitions of the same object and
# the link fails.
#
# This is upstream's latent bug and not Kosmos's, and the rule about not
# modifying a vendored tree is what decides the fix: the flag goes on *these
# files only*, exactly as `-w -Wno-error` does. Kosmos's own code keeps
# `-fno-common`, which is the flag that found this in the first place.
#
#
# **And `NDEBUG`, as NetSurf's own builds have it.** Without it hubbub's
# tree builder and tokeniser print the name of their state for every token -
# "a slightly nasty debugging hook", its comment says - and every `assert`
# in the four libraries runs. The browser had been built that way from the
# start: a formatted line a token, refused by the console and kept aside,
# found by the profile of Wikipedia's Dam article on 30 September.
#
WEB_CFLAGS := -w -Wno-error -fcommon -DWITHOUT_ICONV_FILTER -DNDEBUG \
              $(foreach l,$(WEB_LIBS),-I$(NS)/$(l)/include) \
              -I$(GEN)/netsurf -I$(GEN)/netsurf/css -I$(EXPAT)/lib
endif

USER_OBJS := $(addprefix $(UBUILD)/,$(addsuffix .o,$(USER_SRCS)))
USER_DEPS := $(USER_OBJS:.o=.d)

# miniz, for its deflater and its inflater (`runtime/upstream/miniz/README.md`,
# `roadmap.md` 6v and 6zz g): no stdio, no time, no archives and no
# allocator. On every userland file, since `miniz.h` reads them wherever it
# is included and the kit that includes it has to agree with the file that
# defines it. The inflater, `tinfl`, is gzip's (`user/kits/compress/gzip.c`)
# since 30 September and the only one since 5 October, when `puff` - zlib's
# small inflater, which PNG and `inflate` used - left the tree; it was left
# out while `puff` was the one wanted, and leaving it out took the archive
# code with it - which is now said here.
MINIZ_FLAGS := -DMINIZ_NO_STDIO -DMINIZ_NO_TIME \
               -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -DMINIZ_NO_MALLOC \
               -DMINIZ_NO_ARCHIVE_APIS

# -Ikernel is for syscall.h and panic.h, and nothing else. The syscall
# numbers are the ABI and belong to both sides of it by definition.
UCFLAGS := $(CFLAGS_BASE) $(UTESTDEFS) -DKOSMOS_USER_BASE=$(USER_BASE) $(if $(WEB),-DKOSMOS_WEB) $(if $(FFMPEG),-DKOSMOS_FFMPEG) -DKOSMOS_USER \
           -Iruntime/upstream/stb \
           -Iruntime/upstream/miniz $(MINIZ_FLAGS) \
           -Iruntime/upstream/minimp3 \
           -Iruntime/upstream/minih264 -Iruntime/upstream/minimp4 \
           -Iuser/include -Ikernel -Iruntime/include \
           -Ilua/upstream -Ilua/kosmos \
           -fno-stack-protector


ULDFLAGS := -T user/user.ld -Wl,--defsym=USER_BASE=$(USER_BASE) \
            -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
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
#
# The vendored flag sets are in here too, and were not.
#
# `FLAGS_NOW` was `$(CFLAGS) | $(UCFLAGS)`, so changing `DOOM_CFLAGS`,
# `TINYGL_CFLAGS` or `WEB_CFLAGS` rebuilt nothing at all - the objects still
# matched a flags file that had not moved. Adding `-fcommon` to the NetSurf
# build and watching the identical link error come back twice is how this
# was found. They expand to nothing when their variant is not selected.
#
#
# **Two stamps, because there are two sets of objects and they do not share
# a build directory.**
#
# There was one, `$(BUILD)/flags`, carrying every flag in the build - and
# `BUILD` is only ever `build`, `build/test` or `build/bench`. It does *not*
# vary with `DOOM`, `WEB` or `QUAKE`; `UBUILD` does. So the
# userland flags were recorded in a file the kernel's objects also depended
# on, and every switch between variants rewrote it.
#
# What that cost was the gate. `make prepush` builds plain, then `MEGA=1`,
# then plain again for the screenshot, and each switch changed
# `$(QUAKE_CFLAGS)` in the stamp - so **the whole
# kernel was recompiled three times for flags no kernel object uses.**
#
# Now each stamp covers exactly the flags its own objects are compiled with,
# which is what a stamp is for. The userland one lives in `UBUILD`, so it is
# per-variant and a MEGA build and a plain one cannot invalidate each other
# at all.
#
#
# **And one a processor.** `$(BUILD)` does not vary with `ARCH`: a
# `make ARCH=x86_64` puts its objects in `build/x86_64` but was writing its
# flags into `build/flags` and `build/test/flags` - the ARM kernels' own
# stamps - at parse time, before it compiled a thing. Every gate builds the
# x86 images after the ARM ones, so the next gate found both ARM kernels'
# stamps holding x86 flags and recompiled them, 127 objects, with nothing
# changed; that was most of the images step's 96 seconds, and what took the
# gate past ten minutes on 28 September (`roadmap.md` 6zp).
#
KFLAGS_NOW := $(CFLAGS)
KFLAGS_FILE := $(BUILD)/flags-$(ARCH)

UFLAGS_NOW := $(UCFLAGS) | $(LIBSMB2_CFLAGS) | $(DOOM_CFLAGS) | $(TINYGL_CFLAGS) | $(RECORD_CFLAGS) | $(UFBX_CFLAGS) | $(WEB_CFLAGS) $(WEB_NSB_CFLAGS) $(EXPAT_CFLAGS) | $(MUSL_CFLAGS) | $(QUAKE_CFLAGS) | $(SNES_CFLAGS)$(if $(FFMPEG), | $(FFMPEG_CFLAGS))
UFLAGS_FILE := $(UBUILD)/flags

$(shell mkdir -p $(BUILD) $(UBUILD))
$(shell [ "$$(cat $(KFLAGS_FILE) 2>/dev/null)" = '$(KFLAGS_NOW)' ] \
        || printf '%s' '$(KFLAGS_NOW)' > $(KFLAGS_FILE))
$(shell [ "$$(cat $(UFLAGS_FILE) 2>/dev/null)" = '$(UFLAGS_NOW)' ] \
        || printf '%s' '$(UFLAGS_NOW)' > $(UFLAGS_FILE))

# And a rule each, so a build that has never made this variant can still
# make it. The lines above only rewrite a file when it exists and differs;
# the test and bench builds use their own directories and had never seen
# one, which make reported as "no rule to make target" rather than as
# anything resembling the cause.
$(KFLAGS_FILE):
	@mkdir -p $(dir $@)
	@printf '%s' '$(KFLAGS_NOW)' > $@

$(UFLAGS_FILE):
	@mkdir -p $(dir $@)
	@printf '%s' '$(UFLAGS_NOW)' > $@

# And the same trick for the one file that carries its own flag, so that
# changing the screen size rebuilds that file and relinks, and touches
# nothing else.
FB_FILE := $(BUILD)/fb-$(ARCH).flags

$(shell [ "$$(cat $(FB_FILE) 2>/dev/null)" = '$(FB_FLAGS)' ] \
        || printf '%s' '$(FB_FLAGS)' > $(FB_FILE))

$(FB_FILE):
	@mkdir -p $(dir $@)
	@printf '%s' '$(FB_FLAGS)' > $@

$(BUILD)/hal/fwcfg/fbpixels.c.o: CFLAGS += $(FB_FLAGS)
$(BUILD)/hal/fwcfg/fbpixels.c.o: $(FB_FILE)

# Upstream code is compiled without -Werror, the same allowance lua/upstream
# gets: its warnings are not ours to fix and patching them would mean the
# tree no longer holding what the author released.
#
# TinyGL, compiled on its own terms, and above the general upstream rule for
# the reason that rule's neighbour already records: make takes the first
# pattern that matches, and `runtime/upstream/%.c` matches these too.
#
$(UBUILD)/runtime/upstream/tinygl/examples/bounce.c.o: runtime/upstream/tinygl/examples/bounce.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,bounce) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/cube.c.o: runtime/upstream/tinygl/examples/cube.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,cube) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/gears.c.o: runtime/upstream/tinygl/examples/gears.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,gears) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

#
# `mech` is the one that does not follow the pattern: it calls its per-frame
# function `display`, which is GLUT's name for it rather than `ui.h`'s, so
# the ordinary rename finds no `draw` to rename. Given its own line here
# rather than patched, for the same reason as everything else in this tree.
#
$(UBUILD)/runtime/upstream/tinygl/examples/mech.c.o: runtime/upstream/tinygl/examples/mech.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,mech) \
	      -Ddisplay=mech_draw \
	      -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/morph3d.c.o: runtime/upstream/tinygl/examples/morph3d.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,morph3d) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/spin.c.o: runtime/upstream/tinygl/examples/spin.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,spin) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/teapot.c.o: runtime/upstream/tinygl/examples/teapot.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,teapot) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/examples/texobj.c.o: runtime/upstream/tinygl/examples/texobj.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) $(call tinygl_rename,texobj) -Iruntime/upstream/tinygl/examples -MMD -MP -c $< -o $@

$(UBUILD)/user/kits/record/record_h264.c.o: user/kits/record/record_h264.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(RECORD_CFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/user/kits/record/record_mp4.c.o: user/kits/record/record_mp4.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(RECORD_CFLAGS) -MMD -MP -c $< -o $@

# libsmb2, and the SMB Kit's own files with smbfs (above).
$(UBUILD)/$(LIBSMB2)/lib/%.c.o: $(LIBSMB2)/lib/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(LIBSMB2_CFLAGS) -MMD -MP -c $< -o $@

$(addprefix $(UBUILD)/,$(addsuffix .o,$(SMB_KOSMOS))): $(UBUILD)/%.c.o: %.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(SMB_IFLAGS) -MMD -MP -c $< -o $@

# BearSSL, and the TLS Kit with its headers and the anchors the roots make.
$(UBUILD)/runtime/upstream/bearssl/%.c.o: runtime/upstream/bearssl/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(BEARSSL_IFLAGS) $(UCFLAGS) $(BEARSSL_CFLAGS) -MMD -MP -c $< -o $@

# The Crypto Kit's AES-CMAC, AES-CCM and SP 800-108 KDF, on BearSSL's AES,
# CCM and HMAC.
$(UBUILD)/user/kits/crypto/cmac.c.o $(UBUILD)/user/kits/crypto/ccm.c.o \
$(UBUILD)/user/kits/crypto/kdf.c.o: $(UBUILD)/%.c.o: %.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(BEARSSL_IFLAGS) $(UCFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/user/kits/tls/tls_kosmos.c.o: user/kits/tls/tls_kosmos.c $(HOSTDIR)/tls_anchors.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(BEARSSL_IFLAGS) -I$(HOSTDIR) $(UCFLAGS) -MMD -MP -c $< -o $@

# ufbx and the 3D Kit's reader of it, against the same switches.
$(UBUILD)/runtime/upstream/ufbx/ufbx.c.o: runtime/upstream/ufbx/ufbx.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UFBX_CFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/user/kits/3d/k3d_fbx.c.o: user/kits/3d/k3d_fbx.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(UFBX_CONFIG) -MMD -MP -c $< -o $@

# FFmpeg, on its own terms (the `FFMPEG=1` note above).
$(UBUILD)/runtime/upstream/ffmpeg/%.c.o: runtime/upstream/ffmpeg/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(FFMPEG_IFLAGS) $(UCFLAGS) $(FFMPEG_CFLAGS) -MMD -MP -c $< -o $@

# And the Kosmos files that include FFmpeg's headers: FFmpeg's include
# path, and every warning this project's own code is held to.
$(addprefix $(UBUILD)/,$(addsuffix .o,$(FFMPEG_KOSMOS))): $(UBUILD)/%.c.o: %.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(FFMPEG_IFLAGS) $(UCFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/tinygl/source/%.c.o: runtime/upstream/tinygl/source/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) -MMD -MP -c $< -o $@

#
# musl's maths.
#
# Note what is *not* on this line: `-include lua/kosmos/kosmos_lua.h`, which
# the catch-all rule below adds to everything. That header redirects `time`
# and defines a handful of names for Lua's benefit, and a vendored libm has
# no business seeing any of it - the NetSurf and Doom rules omit it for the
# same reason.
#
$(UBUILD)/$(MUSL)/src/math/%.c.o: $(MUSL)/src/math/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(MUSL_CFLAGS) -MMD -MP -c $< -o $@

#
# The NetSurf libraries, and the files their build generates.
#
# Note what is *not* on these two lines: `-include lua/kosmos/kosmos_lua.h`,
# which the catch-all rule below adds to everything. That header redirects
# `time()` to the monotonic counter for `lmathlib`'s benefit, and these
# libraries want the wall clock. Doom's rule omits it for the same reason.
#
# The generated headers are order-only prerequisites: the sources include
# them, and `make` cannot know that from the source alone.
#
# NetSurf's layout first, since Mac's make 3.81 takes the first pattern that
# matches and the one below would give it a library's `src` instead.
$(UBUILD)/runtime/upstream/netsurf/netsurf/%.c.o: runtime/upstream/netsurf/netsurf/%.c \
                                                 $(UFLAGS_FILE) | $(WEB_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(WEB_CFLAGS) $(WEB_NSB_CFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/netsurf/%.c.o: runtime/upstream/netsurf/%.c \
                                         $(UFLAGS_FILE) | $(WEB_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(WEB_CFLAGS) \
	      -Iruntime/upstream/netsurf/$(firstword $(subst /, ,$*))/src \
	      -MMD -MP -c $< -o $@

#
# Kosmos's own web kit: the ordinary flags, so it is still held to
# `-Wall -Wextra -Werror` and `-fno-common`, plus the public headers of the
# libraries it calls. `gl_kosmos.c` has the same arrangement with TinyGL.
#
$(UBUILD)/user/bin/apps/browser/web_%.c.o: user/bin/apps/browser/web_%.c $(UFLAGS_FILE) | $(WEB_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) \
	      $(foreach l,$(WEB_LIBS),-Iruntime/upstream/netsurf/$(l)/include) \
	      $(WEB_NSB_CFLAGS) -I$(GEN)/netsurf -MMD -MP -c $< -o $@

# A patched file: upstream's, copied and patched (`WEB_PATCHED` above).
$(GEN)/nspatched/%.c: $(NS)/%.c runtime/patches/netsurf/%.c.patch
	@mkdir -p $(dir $@)
	cp $(NS)/$*.c $@.tmp && patch -s $@.tmp runtime/patches/netsurf/$*.c.patch && mv $@.tmp $@

$(GEN)/nspatched/%.h: $(NS)/%.h runtime/patches/netsurf/%.h.patch
	@mkdir -p $(dir $@)
	cp $(NS)/$*.h $@.tmp && patch -s $@.tmp runtime/patches/netsurf/$*.h.patch && mv $@.tmp $@

# And compiled with its own library's `src`, as the upstream file would be.
$(UBUILD)/$(GEN)/nspatched/%.c.o: $(GEN)/nspatched/%.c $(UFLAGS_FILE) | $(WEB_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(WEB_CFLAGS) \
	      -I$(NS)/$(firstword $(subst /, ,$*))/src \
	      -MMD -MP -c $< -o $@

# The generated property parsers are libcss's, so they get libcss's `src`.
$(UBUILD)/$(GEN)/netsurf/%.c.o: $(GEN)/netsurf/%.c $(UFLAGS_FILE) | $(WEB_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(WEB_CFLAGS) \
	      -Iruntime/upstream/netsurf/libcss/src \
	      -Iruntime/upstream/netsurf/libcss/src/parse/properties \
	      -MMD -MP -c $< -o $@

#
# Generated into `build/`, never beside the source.
#
# Both perl scripts write to a path relative to the working directory and
# inside the library's own tree, so each runs in a staging copy holding only
# the input it reads. Letting them write where they want is how a vendored
# tree stops being what was released - which happened here once, by hand.
#
$(GEN)/netsurf/aliases.inc:
	@mkdir -p $(dir $@) $(GEN)/netsurf/stage-pu/build \
	          $(GEN)/netsurf/stage-pu/src/charset
	@cp runtime/upstream/netsurf/libparserutils/build/Aliases \
	    runtime/upstream/netsurf/libparserutils/build/make-aliases.pl \
	    $(GEN)/netsurf/stage-pu/build/
	cd $(GEN)/netsurf/stage-pu && perl build/make-aliases.pl
	@cp $(GEN)/netsurf/stage-pu/src/charset/aliases.inc $@

$(GEN)/netsurf/entities.inc:
	@mkdir -p $(dir $@) $(GEN)/netsurf/stage-hb/build \
	          $(GEN)/netsurf/stage-hb/src/tokeniser
	@cp runtime/upstream/netsurf/libhubbub/build/Entities \
	    runtime/upstream/netsurf/libhubbub/build/make-entities.pl \
	    $(GEN)/netsurf/stage-hb/build/
	cd $(GEN)/netsurf/stage-hb && perl build/make-entities.pl
	@cp $(GEN)/netsurf/stage-hb/src/tokeniser/entities.inc $@

# libsvgtiny's colour names, the same way: its `sed`, for the same reason.
$(GEN)/netsurf/autogenerated_colors.c: runtime/upstream/netsurf/libsvgtiny/src/colors.gperf
	@mkdir -p $(dir $@)
	gperf --output-file=$@.tmp $<
	@sed -e 's/^\(const struct svgtiny_named_color\)/static \1/' $@.tmp > $@
	@rm -f $@.tmp

# expat, warnings off as for every vendored tree, and configured by ours.
$(UBUILD)/runtime/upstream/expat/%.c.o: runtime/upstream/expat/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(EXPAT_CFLAGS) -MMD -MP -c $< -o $@

# The `sed` is theirs: the table is file-local in the translation unit that
# includes this, and without it every copy is a global that collides.
$(GEN)/netsurf/treebuilder/autogenerated-element-type.c: \
        runtime/upstream/netsurf/libhubbub/src/treebuilder/element-type.gperf
	@mkdir -p $(dir $@)
	gperf --output-file=$@.tmp $<
	@sed -e 's/^\(const struct element_type_map\)/static \1/' $@.tmp > $@
	@rm -f $@.tmp

#
# libdom's binding header, at the path libdom expects to find it once
# installed: `<dom/bindings/hubbub/parser.h>`. The source tree keeps it at
# `bindings/hubbub/`, so pointing `-I` at that directory would put its
# `hubbub/` beside libhubbub's own and let one shadow the other. Copied into
# the shape instead, which costs nothing and cannot collide.
#
$(GEN)/netsurf/dom/bindings/hubbub/parser.h:
	@mkdir -p $(dir $@)
	@cp runtime/upstream/netsurf/libdom/bindings/hubbub/parser.h \
	    runtime/upstream/netsurf/libdom/bindings/hubbub/errors.h $(dir $@)

# And the XML binding's, for libsvgtiny's `<dom/bindings/xml/xmlparser.h>`.
$(GEN)/netsurf/dom/bindings/xml/xmlparser.h:
	@mkdir -p $(dir $@)
	@cp runtime/upstream/netsurf/libdom/bindings/xml/xmlparser.h \
	    runtime/upstream/netsurf/libdom/bindings/xml/xmlerror.h $(dir $@)

# Built with the *host* compiler, because it runs here rather than there.
$(GEN)/netsurf/gen_parser: \
        runtime/upstream/netsurf/libcss/src/parse/properties/css_property_parser_gen.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O1 -w -o $@ $<

$(GEN)/netsurf/css/autogenerated_%.c: \
        runtime/upstream/netsurf/libcss/src/parse/properties/properties.gen \
        $(GEN)/netsurf/gen_parser
	@mkdir -p $(dir $@)
	@$(GEN)/netsurf/gen_parser -o $@ "$$(grep '^$*:' $<)"

#
# A pattern rule more specific than the one below it has to be *written*
# above it: `make` 3.81 picks the generic `runtime/upstream/%.c.o` otherwise,
# and the flags a vendored tree needs are silently dropped.
#
# **That is not hypothetical.** `DOOM_CFLAGS` was reaching none of Doom's
# eighty files - no `-DNORMALUNIX`, no screen size - because its rule sat
# below the generic one. TinyGL's has always been above, which is why TinyGL
# built and why this is the shape to copy.
#
$(UBUILD)/runtime/upstream/doom/%.c.o: runtime/upstream/doom/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(DOOM_CFLAGS) -MMD -MP -c $< -o $@

# Doom's binding, Kosmos's own and so every warning on, with Doom's headers.
$(UBUILD)/user/installed/Doom/doom_kosmos.c.o: user/installed/Doom/doom_kosmos.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iruntime/upstream/doom -include lua/kosmos/kosmos_lua.h -MMD -MP -c $< -o $@

# Quake's, above the generic rule for the reason Doom's is.
$(UBUILD)/runtime/upstream/quake/%.c.o: runtime/upstream/quake/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(QUAKE_CFLAGS) -MMD -MP -c $< -o $@

# And Kosmos's half of it: Quake's headers on the path, and the warnings on,
# because this file is ours - all but `-Wcomment`, which five `//` comments
# in Quake's own headers set off by ending in a backslash, and which is about
# upstream's text rather than anything this file does.
$(UBUILD)/user/installed/Quake/quake_kosmos.c.o: user/installed/Quake/quake_kosmos.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(QUAKE_INCLUDES) -Wno-comment -MMD -MP -c $< -o $@

# LakeSnes's core, above the generic rule for the reason Doom's is.
$(UBUILD)/runtime/upstream/lakesnes/%.c.o: runtime/upstream/lakesnes/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(SNES_CFLAGS) -MMD -MP -c $< -o $@

# And Kosmos's half: the core's headers on the path, and every warning on.
$(UBUILD)/user/installed/SNES/snes_kosmos.c.o: user/installed/SNES/snes_kosmos.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -iquote $(SNES_DIR) -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/%.c.o: runtime/upstream/%.c $(UFLAGS_FILE)
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
$(UBUILD)/user/kits/gl/gl_demos.c.o: user/kits/gl/gl_demos.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iruntime/upstream/tinygl/include -MMD -MP -c $< -o $@

$(UBUILD)/user/kits/gl/gl_kosmos.c.o: user/kits/gl/gl_kosmos.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iruntime/upstream/tinygl/include -MMD -MP -c $< -o $@

$(UBUILD)/%.c.o: %.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -include lua/kosmos/kosmos_lua.h -MMD -MP -c $< -o $@

$(UBUILD)/%.S.o: %.S $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

# The Lua source init runs, and then init itself, both carried inside the
# kernel image because there is no filesystem to load them from until M8.
$(GEN)/init_lua.c: user/init/init.lua tools/bin2c.py $(HOSTDIR)/lua.ok
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< init_lua $@

# A table as text and back (`user/init/tabletext.lua`): loaded before the
# chunk above in every process, as the global `tabletext`, so a table stored
# in a file is a file a person can read (Diego, 4 October: "I don't like
# binary files for settings for anything in the system").
$(GEN)/tabletext_lua.c: user/init/tabletext.lua tools/bin2c.py $(HOSTDIR)/lua.ok
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< tabletext_lua $@

# Which protocols an image speaks, as one word (`tools/protostamp.py`): in the
# system's image and every installed application's, compared when one starts
# the other, so an application built for another Kosmos says so.
PROTO_HEADERS := $(wildcard user/include/*proto.h) $(wildcard user/include/*ring.h)

$(GEN)/protostamp.c: $(PROTO_HEADERS) tools/protostamp.py
	@mkdir -p $(dir $@)
	python3 tools/protostamp.py $@ $(PROTO_HEADERS)

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
#   luaparse    it parses
#   luaglobals  every global it reads will actually be there
#
# `luaparse` was `luacheck` until 27 September, when the real luacheck
# arrived for the IDE (runtime/upstream/luacheck/) and the name had to be
# the one that says what this does.
#
# The second is the one that earns its place. A name that used to be a local
# and is not any more compiles perfectly happily - it is a global, and
# globals may be nil - and fails much later somewhere unhelpful. That has
# cost four separate debugging sessions in this project.
# ------------------------------------------------------------------

# -w because upstream's warnings are not ours to fix, the same reasoning the
# target build uses.
$(HOSTDIR)/luaparse: tools/luaparse.c $(LUA_HOST_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -O1 -w -Ilua/upstream -o $@ $^ -lm

# The Synth Kit's engine on this machine (`roadmap.md` 6zh): its pure half -
# the sound, the engine, the song read from Lua - with every warning on, and
# a Lua to drive it, whose own warnings are not ours.
SYNTH_PURE := user/kits/synth/synth_dsp.c user/kits/synth/synth_engine.c \
              user/kits/synth/synth_lua.c

$(HOSTDIR)/test_synth: tools/test_synth.c $(SYNTH_PURE) $(wildcard user/kits/synth/*.h) \
                       lua/upstream/linit.c $(LUA_HOST_SRCS)
	@mkdir -p $(HOSTDIR)/synth
	@for f in tools/test_synth.c $(SYNTH_PURE); do \
	    $(HOST_CC) -O2 -std=c11 -Wall -Wextra -Werror -Iuser/kits/synth -Ilua/upstream \
	        -c $$f -o $(HOSTDIR)/synth/$$(basename $$f .c).o || exit 1; \
	done
	$(HOST_CC) -O1 -w -Ilua/upstream -o $@ $(HOSTDIR)/synth/*.o lua/upstream/linit.c \
	    $(LUA_HOST_SRCS) -lm

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
#
# The scanf family's scanner, on this machine.
#
# `runtime/libc/scan.c` includes nothing of Kosmos's, so the host compiler
# builds it as the cross one does. `vsscanf` and `sscanf` are renamed on the
# command line, so the ones the test calls are Kosmos's and not the host's.
#
$(HOSTDIR)/test_scan: tools/test_scan.c runtime/libc/scan.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -O1 -o $@ \
	    -Dvsscanf=kosmos_test_vsscanf -Dsscanf=kosmos_test_sscanf \
	    tools/test_scan.c runtime/libc/scan.c

# The libc's four that move and compare memory, against a byte loop at every
# alignment (`testing.md` 18.329) - built twice, because `string.c` is
# compiled twice and the copies take different paths: as a process links it,
# words at any address, and as the kernel does, words only where both
# addresses agree. Renamed so the host's are not what is called, and with
# the host's fortified string.h out of the way, which would rename them back.
STRING_TEST_FLAGS := -std=c11 -Wall -Wextra -O2 \
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -Dmemcpy=kosmos_test_memcpy -Dmemmove=kosmos_test_memmove \
    -Dmemset=kosmos_test_memset -Dmemcmp=kosmos_test_memcmp

$(HOSTDIR)/test_string: tools/test_string.c runtime/libc/string.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(STRING_TEST_FLAGS) -DKOSMOS_USER \
	    '-DSIDE="as a process links them"' -o $@ \
	    tools/test_string.c runtime/libc/string.c

$(HOSTDIR)/test_string_kernel: tools/test_string.c runtime/libc/string.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(STRING_TEST_FLAGS) '-DSIDE="as the kernel links them"' \
	    -DKOSMOS_TEST_ALIGNMENT -o $@ tools/test_string.c runtime/libc/string.c

#
# The audio ring's arithmetic, on this machine.
#
# `audioring.h` is a header two processes agree on and nothing but `stdint.h`
# underneath it, so the host compiler builds it exactly as the cross compiler
# does - and a position that is wrong is a *number* that is wrong, which no
# amount of listening to QEMU would find.
#
$(HOSTDIR)/test_audioring: tools/test_audioring.c user/include/audioring.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ tools/test_audioring.c

#
# The framebuffer a loader hands over, checked without a machine.
#
# QEMU's `-kernel` ignores multiboot's video request, so nothing under
# emulation ever takes that branch - and on a laptop it is the only way to
# get a screen. Same split as `kfs.lua`: the decision is a pure function and
# this is where it is asked the awkward questions.
#
$(HOSTDIR)/test_loaderfb: tools/test_loaderfb.c hal/pc/loader_fb.c hal/pc/multiboot.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_loaderfb.c hal/pc/loader_fb.c

#
# And the UEFI loader's decisions - where the kernel goes, the firmware's map
# in Multiboot 2's shape, the information structure - read back through
# `loader_fb.c`, the kernel's own parser, because what counts is whether the
# kernel agrees. With a kernel image as its argument it also asks that the
# build's `kosmos.bin` is one the loader will take.
#
$(HOSTDIR)/test_efiboot: tools/test_efiboot.c boot/efi/mbi.c boot/efi/mbi.h boot/efi/sums.c boot/efi/sums.h hal/pc/loader_fb.c hal/pc/multiboot2.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -I boot/efi -I hal/pc -o $@ \
	        tools/test_efiboot.c boot/efi/mbi.c boot/efi/sums.c hal/pc/loader_fb.c

#
# And the Super Nintendo's picture at a scale: the only code between the
# core's pixels and the window, asked without a core or a ROM, including
# every byte it must not touch.
#
$(HOSTDIR)/test_snesblit: tools/test_snesblit.c user/installed/SNES/snes_blit.c user/installed/SNES/snes_blit.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -Iuser/installed/SNES -o $@ \
	        tools/test_snesblit.c user/installed/SNES/snes_blit.c

#
# And where the page bitmap goes, for the same reason one layer down.
#
# `pmm_init` put its bitmap immediately after the kernel image and panicked
# if the memory the board reported began above it - right for every machine
# whose largest usable block is the one the kernel was loaded into, which is
# what `-kernel` gives and what firmware does not promise. OVMF reports
# 0x900000 at every memory size from 512 MB to 32 GB, below the image end at
# 0xf98000, so no QEMU configuration here reaches the case at all.
#
$(HOSTDIR)/test_pmmplace: tools/test_pmmplace.c kernel/pmm_place.c kernel/pmm_place.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_pmmplace.c kernel/pmm_place.c

#
# And what the APIC registers say, for the same reason a third time: the
# chipset of the first real machine reports a hundred and twenty I/O APIC
# inputs, QEMU's reports twenty-four whatever it is given, and the driver
# refused anything above sixty-four - so the case that matters is arithmetic
# no emulator here produces.
#
$(HOSTDIR)/test_apicdecode: tools/test_apicdecode.c hal/pc/apic_decode.c hal/pc/apic_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_apicdecode.c hal/pc/apic_decode.c

#
# **BearSSL's own `brssl`, on the Mac, for one command**: `brssl ta`, which
# turns Mozilla's roots (`assets/ca/cacert.pem`) into the trust anchors the
# TLS Kit is compiled with - a build step, as the fonts are, rather than a
# converter of our own that would have to agree with BearSSL about DER.
# Built from the vendored sources whole, in one command, since it is built
# once; BearSSL's own randomness and clock are left on here, on the Mac.
#
$(HOSTDIR)/brssl: $(BEARSSL_SRCS) $(wildcard runtime/upstream/bearssl/tools/*.c runtime/upstream/bearssl/tools/*.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -w -Iruntime/upstream/bearssl/inc -Iruntime/upstream/bearssl/src \
	        -o $@ $(BEARSSL_SRCS) $(wildcard runtime/upstream/bearssl/tools/*.c)

$(HOSTDIR)/tls_anchors.c: assets/ca/cacert.pem $(HOSTDIR)/brssl
	$(HOSTDIR)/brssl ta assets/ca/cacert.pem > $@.tmp 2> /dev/null
	mv $@.tmp $@

#
# **The keyboard controller's drain, with the test as the controller**
# (`hal/pc/i8042_drain.c`): the M700's firmware answers as an i8042 until
# the USB driver takes the controller, and its ports read 0xff after -
# which the drain took as bytes, sixty-four port reads at every question.
# No emulator here has a controller that goes away.
#
$(HOSTDIR)/test_i8042drain: tools/test_i8042drain.c hal/pc/i8042_drain.c hal/pc/i8042_drain.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_i8042drain.c hal/pc/i8042_drain.c

#
# **The Crypto Kit against its specifications' vectors** (`user/kits/crypto/`):
# SHA-256, HMAC, ChaCha20, Poly1305, X25519 and DES. `crypto.c` said this
# check existed from the day it arrived; it was written on 29 September,
# with DES for VNC (`testing.md` 18.291).
#
# `time()` in a process: sysinfo once a minute, the counter between
# (`testing.md` 18.301), against a stand-in `kosmos.h` that counts calls.
$(HOSTDIR)/test_clock: tools/test_clock.c user/init/clock_user.c tools/stubs/clock/kosmos.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -DKOSMOS_CLOCK_TEST -Itools/stubs/clock \
	        -o $@ tools/test_clock.c user/init/clock_user.c

#
# And what SMB 2/3 needs (`docs/sharing.md`, N1): the kit's MD4, AES-CMAC and
# SP 800-108 KDF, and BearSSL's HMAC-MD5 and AES-CCM composed as SMB uses
# them - so the BearSSL files those stand on are built in, as vendored code
# is, with `-w`. Natively, where the AES is `aes_ct64`, and through Rosetta
# as `test_crypto_x86`, where it is AES-NI: the kit chooses, and the two
# runs are its two choices.
TEST_CRYPTO_SRCS := tools/test_crypto.c user/kits/crypto/crypto.c \
                    user/kits/crypto/md4.c user/kits/crypto/cmac.c user/kits/crypto/ccm.c \
                    user/kits/crypto/kdf.c
TEST_CRYPTO_BEARSSL := $(addprefix runtime/upstream/bearssl/src/, \
                    aead/ccm.c mac/hmac.c hash/md5.c hash/sha2small.c \
                    symcipher/aes_common.c symcipher/aes_ct64.c \
                    symcipher/aes_ct64_enc.c symcipher/aes_ct64_ctrcbc.c \
                    symcipher/aes_x86ni.c symcipher/aes_x86ni_ctrcbc.c) \
                    $(wildcard runtime/upstream/bearssl/src/codec/*.c)

# The keyring's file (`docs/keyring.md`, K2): `keyfile.c` on the Mac, on the
# kit's CCM, SHA-256 and MD4 and BearSSL's AES, as `test_crypto` builds them.
TEST_KEYFILE_SRCS := tools/test_keyfile.c user/servers/keyfile.c user/kits/crypto/crypto.c \
                     user/kits/crypto/md4.c user/kits/crypto/cmac.c user/kits/crypto/ccm.c

$(HOSTDIR)/test_keyfile: $(TEST_KEYFILE_SRCS) $(TEST_CRYPTO_BEARSSL) user/include/crypto.h \
                         user/include/keyproto.h user/servers/keyfile.h
	@mkdir -p $(dir $@)
	@rm -rf $@.o && mkdir -p $@.o
	cd $@.o && $(HOST_CC) -O1 -w -I$(CURDIR)/runtime/upstream/bearssl/inc \
	        -I$(CURDIR)/runtime/upstream/bearssl/src -c $(addprefix $(CURDIR)/,$(TEST_CRYPTO_BEARSSL))
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -Iuser/include -Iuser/servers $(BEARSSL_IFLAGS) -o $@ \
	        $(TEST_KEYFILE_SRCS) $@.o/*.o

# A keyring made on the Mac for a suite's disk (K6), sealed by `keyfile.c`.
$(HOSTDIR)/keyring_seed: tools/keyring_seed.c $(TEST_KEYFILE_SRCS) $(TEST_CRYPTO_BEARSSL) \
                         user/include/keyproto.h user/servers/keyfile.h
	@mkdir -p $(dir $@)
	@rm -rf $@.o && mkdir -p $@.o
	cd $@.o && $(HOST_CC) -O1 -w -I$(CURDIR)/runtime/upstream/bearssl/inc \
	        -I$(CURDIR)/runtime/upstream/bearssl/src -c $(addprefix $(CURDIR)/,$(TEST_CRYPTO_BEARSSL))
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -Iuser/include -Iuser/servers $(BEARSSL_IFLAGS) -o $@ \
	        tools/keyring_seed.c $(filter-out tools/test_keyfile.c,$(TEST_KEYFILE_SRCS)) $@.o/*.o

# Wycheproof's AES-CCM vectors, as shipped, made a header (K1).
$(HOSTDIR)/aes_ccm_vectors.h: tools/vectors/wycheproof/aes_ccm_test.json tools/wycheproof2c.py
	@mkdir -p $(dir $@)
	python3 tools/wycheproof2c.py $< $@

$(HOSTDIR)/test_crypto: $(TEST_CRYPTO_SRCS) $(TEST_CRYPTO_BEARSSL) user/include/crypto.h $(HOSTDIR)/aes_ccm_vectors.h
	@mkdir -p $(dir $@)
	@rm -rf $@.o && mkdir -p $@.o
	cd $@.o && $(HOST_CC) -O1 -w -I$(CURDIR)/runtime/upstream/bearssl/inc \
	        -I$(CURDIR)/runtime/upstream/bearssl/src -c $(addprefix $(CURDIR)/,$(TEST_CRYPTO_BEARSSL))
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -Iuser/include -I$(HOSTDIR) $(BEARSSL_IFLAGS) -o $@ \
	        $(TEST_CRYPTO_SRCS) $@.o/*.o

$(HOSTDIR)/test_crypto_x86: $(TEST_CRYPTO_SRCS) $(TEST_CRYPTO_BEARSSL) user/include/crypto.h $(HOSTDIR)/aes_ccm_vectors.h
	@mkdir -p $(dir $@)
	@rm -rf $@.o && mkdir -p $@.o
	cd $@.o && $(HOST_CC) -arch x86_64 -O1 -w -I$(CURDIR)/runtime/upstream/bearssl/inc \
	        -I$(CURDIR)/runtime/upstream/bearssl/src -c $(addprefix $(CURDIR)/,$(TEST_CRYPTO_BEARSSL))
	$(HOST_CC) -arch x86_64 -std=c11 -Wall -Wextra -Werror -O1 -Iuser/include -I$(HOSTDIR) $(BEARSSL_IFLAGS) -o $@ \
	        $(TEST_CRYPTO_SRCS) $@.o/*.o

#
# And SMB's signatures (`docs/sharing.md`, N4): `user/kits/smb/smb_signing.c`,
# which replaces libsmb2's `smb2-signing.c` in the machine, held to that very
# file - compiled beside it with its names given a `ref_` prefix on the
# compile line, standing on libsmb2's own AES and SHA rather than the kit's
# or BearSSL's, so the two share no code. Natively (`aes_ct64`) and through
# Rosetta (AES-NI), as `test_crypto` is.
TEST_SMBSIGN_REF := smb2-signing aes aes_reference hmac sha1 sha224-256 sha384-512 usha
TEST_SMBSIGN_RENAME := -Dsmb3_aes_cmac_128=ref_smb3_aes_cmac_128 \
                       -Dsmb2_calc_signature=ref_smb2_calc_signature \
                       -Dsmb2_pdu_add_signature=ref_smb2_pdu_add_signature \
                       -Dsmb2_pdu_check_signature=ref_smb2_pdu_check_signature
TEST_SMBSIGN_SRCS := tools/test_smbsign.c user/kits/smb/smb_signing.c user/kits/crypto/cmac.c

define test_smbsign_rule
$(HOSTDIR)/$(1): $(TEST_SMBSIGN_SRCS) $(TEST_CRYPTO_BEARSSL) user/include/crypto.h \
	        tools/libsmb2_mac_config.h $(patsubst %,$(LIBSMB2)/lib/%.c,$(TEST_SMBSIGN_REF))
	@mkdir -p $$(dir $$@)
	@rm -rf $$@.o && mkdir -p $$@.o
	@mkdir -p $$@.o/ref
	@cp tools/libsmb2_mac_config.h $$@.o/config.h
	cd $$@.o && $(HOST_CC) $(2) -O1 -w -I$(CURDIR)/runtime/upstream/bearssl/inc \
	        -I$(CURDIR)/runtime/upstream/bearssl/src -c $$(addprefix $(CURDIR)/,$(TEST_CRYPTO_BEARSSL))
	cd $$@.o/ref && $(HOST_CC) $(2) -O1 -w -DHAVE_CONFIG_H $(TEST_SMBSIGN_RENAME) -I.. \
	        -I$(CURDIR)/$(LIBSMB2)/include -I$(CURDIR)/$(LIBSMB2)/include/smb2 \
	        -I$(CURDIR)/$(LIBSMB2)/lib -c $$(patsubst %,$(CURDIR)/$(LIBSMB2)/lib/%.c,$(TEST_SMBSIGN_REF))
	$(HOST_CC) $(2) -std=c11 -Wall -Wextra -Werror -O1 -DHAVE_CONFIG_H -I$$@.o \
	        -Iuser/include $(BEARSSL_IFLAGS) -isystem $(LIBSMB2)/include \
	        -isystem $(LIBSMB2)/include/smb2 -isystem $(LIBSMB2)/lib -o $$@ \
	        $(TEST_SMBSIGN_SRCS) $$@.o/*.o $$@.o/ref/*.o
endef

$(eval $(call test_smbsign_rule,test_smbsign,))
$(eval $(call test_smbsign_rule,test_smbsign_x86,-arch x86_64))

#
# And what SMBIOS says the machine is called, for the same reason from the
# other side: QEMU can be told to put a ThinkPad's name in its table, and
# `run_x86.py` does, but it cannot be made to produce a *malformed* table -
# and refusing those without walking off the end of memory at boot is most
# of what the decoder is for.
#
$(HOSTDIR)/test_smbiosdecode: tools/test_smbiosdecode.c hal/pc/smbios_decode.c hal/pc/smbios_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_smbiosdecode.c hal/pc/smbios_decode.c

#
# And a USB configuration descriptor, walked, for that reason a third time:
# QEMU's mouse sends one well-formed configuration and nothing else, and a
# device's lengths are the device's to get wrong. `usb_decode.h` has more.
#
#
# And an Intel backlight PWM's three registers, for that reason again and
# more so: QEMU has no Intel graphics at all, so the driver's only reading
# under QEMU is none, and the ThinkPad gives one. `backlight_decode.h` has more.
#
$(HOSTDIR)/test_backlightdecode: tools/test_backlightdecode.c user/drivers/display/backlight_decode.c user/drivers/display/backlight_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_backlightdecode.c user/drivers/display/backlight_decode.c

#
# And a ThinkPad's battery registers, for the backlight's reason again: QEMU
# has no embedded controller, so only the ThinkPad hands this real bytes.
# `battery_decode.h` has more.
#
$(HOSTDIR)/test_batterydecode: tools/test_batterydecode.c hal/pc/battery_decode.c hal/pc/battery_decode.h hal/hal.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -Ihal -o $@ \
	        tools/test_batterydecode.c hal/pc/battery_decode.c

#
# And `\_S5`'s sleep type, for the same reason in its plainest form: QEMU's
# is 0, which is also what the board wrote before it read one, so under QEMU
# a decoder that finds nothing powers off exactly like one that works. The
# ThinkPad's is 7. `s5_decode.h` has more.
#
$(HOSTDIR)/test_s5decode: tools/test_s5decode.c hal/pc/s5_decode.c hal/pc/s5_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_s5decode.c hal/pc/s5_decode.c

#
# And an Xbox 360 controller's reports, for the same reason: QEMU has no
# game controller to plug in. `pad_decode.h` has more.
#
$(HOSTDIR)/test_paddecode: tools/test_paddecode.c user/drivers/usb/pad_decode.c user/drivers/usb/pad_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_paddecode.c user/drivers/usb/pad_decode.c

#
# A USB Video Class camera's descriptors, probe and payloads (`roadmap.md`
# 6d): QEMU has no camera, so Diego's C920's own bytes are the fixture.
# With the undefined-behaviour sanitizer; the address sanitizer hangs before
# `main` on this Mac, so the reads past the end it would have caught are
# caught by a guard page in the test itself.
#
$(HOSTDIR)/test_uvcdecode: tools/test_uvcdecode.c tools/uvc_c920.h user/drivers/usb/uvc_decode.c user/drivers/usb/uvc_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -Wall -Wextra \
	        -Werror -O1 -g -fsanitize=undefined -fno-sanitize-recover=all \
	        -o $@ tools/test_uvcdecode.c user/drivers/usb/uvc_decode.c

# A USB MIDI device's descriptors and event packets (`roadmap.md` 6zg):
# QEMU has none, so a Launchpad MK2's printed bytes and the Launchkey's
# shape are the fixtures.
$(HOSTDIR)/test_mididecode: tools/test_mididecode.c user/drivers/usb/midi_decode.c user/drivers/usb/midi_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -Wall -Wextra \
	        -Werror -O1 -g -fsanitize=undefined -fno-sanitize-recover=all \
	        -o $@ tools/test_mididecode.c user/drivers/usb/midi_decode.c

# `depth.h` on the Mac: the rule the Synth Kit keeps its ring by and the audio
# server the device (`roadmap.md` 4i, `testing.md` 18.269).
$(HOSTDIR)/test_depth: tools/test_depth.c user/include/depth.h
	@mkdir -p $(HOSTDIR)
	$(HOST_CC) -O2 -std=c11 -Wall -Wextra -Werror -o $@ tools/test_depth.c

$(HOSTDIR)/test_usbdecode: tools/test_usbdecode.c user/drivers/usb/usb_decode.c user/drivers/usb/usb_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_usbdecode.c user/drivers/usb/usb_decode.c

$(HOSTDIR)/test_e1000decode: tools/test_e1000decode.c user/drivers/net/e1000_decode.c user/drivers/net/e1000_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -Iuser/drivers/net -o $@ \
	        tools/test_e1000decode.c user/drivers/net/e1000_decode.c

#
# And what a stick is sent and what it answers - Bulk-Only's wrappers, SCSI's
# command blocks, capacity, sense and a GPT header - for that reason once
# more: QEMU's stick answers every command, well. `storage_decode.h` has more.
#
$(HOSTDIR)/test_storagedecode: tools/test_storagedecode.c user/drivers/usb/storage_decode.c user/drivers/usb/storage_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_storagedecode.c user/drivers/usb/storage_decode.c

#
# And what a FAT volume's bytes mean - its boot sector, its table, and a
# directory's short and long names - twice over, because this reader is ours
# and the drives it will read are not. `test_fatdecode` builds its bytes from
# the specification; `fatls` walks the volumes mtools makes, which is somebody
# else's reading of the same format, and `tools/test_fat.py` holds the two to
# the files that went in. `fat_decode.h` has more.
#
#
# A window's shadow, held on this computer to the method it replaced
# (`roadmap.md` 5zu): the same pixels, each within a step, a clip that
# changes nothing inside it - and how long each takes.
#
#
# A camera's YUY2 into the screen's pixels (`roadmap.md` 6d): every Y, U
# and V against BT.601 in floating point, and a frame straight and mirrored.
#
# **A surface in a viewer's pixel format** (`user/kits/gfx/pack.c`): eight
# pixels at a time held to one at a time, natively for NEON and through
# Rosetta for SSE2, with a frame timed both ways - VNC's packing (18.291).
#
$(HOSTDIR)/test_pack: tools/test_pack.c user/kits/gfx/pack.c user/kits/gfx/pack.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -Iuser/kits/gfx -o $@ \
	        tools/test_pack.c user/kits/gfx/pack.c

# **gzip, read** (`user/kits/compress/gzip.c`): a stream Python wrote, every
# header flag, two members, a megabyte, every way of being cut short, checks
# that lie and a caller that says stop (`roadmap.md` 6zz g).
#
$(HOSTDIR)/test_gunzip: tools/test_gunzip.c user/kits/compress/gzip.c user/kits/compress/gzip.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 $(MINIZ_FLAGS) \
	        -Iuser/kits/compress -Iruntime/upstream/miniz -o $@ \
	        tools/test_gunzip.c user/kits/compress/gzip.c runtime/upstream/miniz/miniz.c

$(HOSTDIR)/test_pack_x86: tools/test_pack.c user/kits/gfx/pack.c user/kits/gfx/pack.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -arch x86_64 -std=c11 -Wall -Wextra -Werror -O2 -Iuser/kits/gfx -o $@ \
	        tools/test_pack.c user/kits/gfx/pack.c

#
# **The SVG rasteriser** (`user/bin/apps/browser/web_raster.c`, `roadmap.md`
# 6zz j5): what a shape covers, and four pixels at a time held to one at a
# time, natively for NEON and through Rosetta for SSE2. `-ffp-contract=off`
# because the two have to be the same float operations to agree to the bit,
# and a fused multiply-add in one and not the other is not; GCC builds the
# guest with it off already, as it does for any ISO `-std`. And clang's own
# vectorisers off, because left on they turn the one-at-a-time reference into
# lanes by themselves, and the time printed compares vectors with vectors -
# the guest's GCC is not to be relied on to do the same, which is why the
# lanes are written out. Written vector types are not touched by the flags.
#
RASTER_TEST_FLAGS := -std=c11 -Wall -Wextra -Werror -O2 -ffp-contract=off \
                     -fno-vectorize -fno-slp-vectorize -Iuser/bin/apps/browser

$(HOSTDIR)/test_raster: tools/test_raster.c user/bin/apps/browser/web_raster.c user/bin/apps/browser/web_raster.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(RASTER_TEST_FLAGS) -o $@ \
	        tools/test_raster.c user/bin/apps/browser/web_raster.c

#
# **Filling, and a glyph over a row** (`user/kits/gfx/rows.c`, `roadmap.md`
# 6zz h): four pixels at a time held to one, every coverage over every ink
# and ground, natively for NEON and through Rosetta for SSE2 - built as the
# rasteriser's test is, its vectorisers off so the scalar loop is one.
#
ROWS_TEST_FLAGS := -std=c11 -Wall -Wextra -Werror -O2 -fno-vectorize \
                   -fno-slp-vectorize -Iuser/kits/gfx

$(HOSTDIR)/test_rows: tools/test_rows.c user/kits/gfx/rows.c user/kits/gfx/rows.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(ROWS_TEST_FLAGS) -o $@ tools/test_rows.c user/kits/gfx/rows.c

$(HOSTDIR)/test_rows_x86: tools/test_rows.c user/kits/gfx/rows.c user/kits/gfx/rows.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -arch x86_64 $(ROWS_TEST_FLAGS) -o $@ tools/test_rows.c user/kits/gfx/rows.c

$(HOSTDIR)/test_raster_x86: tools/test_raster.c user/bin/apps/browser/web_raster.c user/bin/apps/browser/web_raster.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -arch x86_64 $(RASTER_TEST_FLAGS) -o $@ \
	        tools/test_raster.c user/bin/apps/browser/web_raster.c

$(HOSTDIR)/test_yuv: tools/test_yuv.c user/kits/gfx/yuv.c user/kits/gfx/yuv.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ \
	        tools/test_yuv.c user/kits/gfx/yuv.c -lm

# The 3D Kit (`roadmap.md` 4l, Cafesa3D): every shape wound outwards and
# counted, and the rasteriser on scenes small enough to reason about.
#
K3D_CORE := user/kits/3d/k3d_mesh.c user/kits/3d/k3d_raster.c user/kits/3d/k3d_texture.c \
            user/kits/3d/k3d_formats.c

$(HOSTDIR)/test_k3d: tools/test_k3d.c $(K3D_CORE) user/kits/3d/k3d.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ \
	        tools/test_k3d.c $(K3D_CORE) -lm

# Its FBX reader (`tools/test_fbx.c`): Blender's, Maya's and 3ds Max's own
# files, each against the OBJ its program exported of the same scene, and
# damaged copies refused. ufbx is an object of its own, being a megabyte of
# C that does not change when the test does.
$(HOSTDIR)/ufbx.o: runtime/upstream/ufbx/ufbx.c runtime/upstream/ufbx/ufbx.h \
                   user/kits/3d/k3d_ufbx.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -w -O1 $(UFBX_CONFIG) -c -o $@ $<

$(HOSTDIR)/test_fbx: tools/test_fbx.c user/kits/3d/k3d_fbx.c user/kits/3d/k3d_formats.c \
                     user/kits/3d/k3d.h user/kits/3d/k3d_ufbx.h $(HOSTDIR)/ufbx.o
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 $(UFBX_CONFIG) -o $@ \
	        tools/test_fbx.c user/kits/3d/k3d_fbx.c user/kits/3d/k3d_formats.c \
	        $(HOSTDIR)/ufbx.o -lm

# Its ray tracer: shapes against their triangles, the four-wide hierarchy
# against every triangle, light against the arithmetic, and four threads
# against one, bit for bit (`tools/test_trace.c`).
$(HOSTDIR)/test_trace: tools/test_trace.c $(K3D_CORE) user/kits/3d/k3d_trace.c user/kits/3d/k3d.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ \
	        tools/test_trace.c $(K3D_CORE) user/kits/3d/k3d_trace.c -lm -lpthread

#
# FFmpeg's H.264 decoder and the kit's core on this Mac, held to FFmpeg's
# own conformance checksums (`tools/test_h264.c`). The 95 objects are
# compiled against Kosmos's headers and Kosmos's configuration, as for the
# guest, and linked over the Mac's C library; `test_h264_libc.c` is the
# three names the two libraries spell differently. The streams are fetched
# once into `build/downloads/` and held to their sums
# (`tools/fetch_conformance.py h264`).
#
FFMPEG_HOST_FLAGS := -std=c17 -O2 -w -ffreestanding -DHAVE_AV_CONFIG_H \
                     $(FFMPEG_CPPFLAGS) $(FFMPEG_IFLAGS) -Iruntime/include \
                     -Ikernel -Iuser/include
FFMPEG_HOST_OBJS  := $(addprefix $(HOSTDIR)/ffmpeg/,$(addsuffix .o,$(FFMPEG_NAMES)))

$(HOSTDIR)/ffmpeg/%.o: $(FFMPEG_DIR)/%.c user/kits/ffmpeg/ffmpeg.mk
	@mkdir -p $(dir $@)
	$(HOST_CC) $(FFMPEG_HOST_FLAGS) -c $< -o $@

$(HOSTDIR)/kits/%.o: user/kits/ffmpeg/%.c user/kits/ffmpeg/%.h \
                    user/kits/ffmpeg/ffmpeg_log.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -ffreestanding \
	        $(FFMPEG_IFLAGS) -Iruntime/include -Ikernel -c $< -o $@

FFMPEG_HOST_KITS := $(HOSTDIR)/kits/ffmpeg_log.o $(HOSTDIR)/kits/h264_core.o \
                    $(HOSTDIR)/kits/aac_core.o

$(HOSTDIR)/test_h264: tools/test_h264.c tools/test_h264_libc.c \
                      user/kits/ffmpeg/h264_core.h $(FFMPEG_HOST_KITS) \
                      $(FFMPEG_HOST_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ tools/test_h264.c \
	        tools/test_h264_libc.c $(FFMPEG_HOST_KITS) $(FFMPEG_HOST_OBJS)

# The AAC Kit's decoder, the same way, held to FFmpeg's PCM references for
# twelve conformance streams, which `/lib/mp4.lua` reads on this Mac's Lua
# through `tools/mp4index.lua` (`tools/test_aac.c`).
$(HOSTDIR)/test_aac: tools/test_aac.c tools/test_h264_libc.c \
                     user/kits/ffmpeg/aac_core.h $(FFMPEG_HOST_KITS) \
                     $(FFMPEG_HOST_OBJS) $(HOSTDIR)/lua
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ tools/test_aac.c \
	        tools/test_h264_libc.c $(FFMPEG_HOST_KITS) $(FFMPEG_HOST_OBJS)

#
# Broken-down time, `runtime/libc/time.c`, against the Mac's own libc
# (`tools/test_time.c`). Compiled with its functions renamed, so the two
# are in one program and every answer is compared with the host's.
#
$(HOSTDIR)/test_time: tools/test_time.c runtime/libc/time.c runtime/include/time.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -c -o $@-libc.o \
	        -Dgmtime=k_gmtime -Dlocaltime=k_localtime -Dmktime=k_mktime \
	        -Dstrftime=k_strftime runtime/libc/time.c
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ \
	        tools/test_time.c $@-libc.o

#
# The same test built for x86-64, so the SSE2 path is held on this Mac as
# well as the NEON one: Rosetta runs it (`tools/test_yuv.c`).
#
$(HOSTDIR)/test_yuv_x86: tools/test_yuv.c user/kits/gfx/yuv.c user/kits/gfx/yuv.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -arch x86_64 -std=c11 -Wall -Wextra -Werror -O2 -o $@ \
	        tools/test_yuv.c user/kits/gfx/yuv.c -lm

#
# The Record Kit's core on this Mac (`tools/test_record.c`), with the two
# vendored halves beside it: `minih264e` and `minimp4` compiled on their own
# terms, `-w`, as they are in the guest, and the core and the test with every
# warning. `test_record_mp4.lua` then reads the file it wrote.
#
RECORD_HOST := -Iruntime/upstream/minih264 -Iruntime/upstream/minimp4

$(HOSTDIR)/record_h264.o: user/kits/record/record_h264.c \
                          user/kits/record/record_config.h \
                          runtime/upstream/minih264/minih264e.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -w -O2 $(RECORD_HOST) -c $< -o $@

$(HOSTDIR)/record_mp4.o: user/kits/record/record_mp4.c \
                         user/kits/record/record_config.h \
                         runtime/upstream/minimp4/minimp4.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -w -O2 $(RECORD_HOST) -c $< -o $@

$(HOSTDIR)/test_record: tools/test_record.c user/kits/record/record_core.c \
                        user/kits/record/record_core.h \
                        user/kits/record/record_config.h \
                        user/kits/gfx/yuv.c user/kits/gfx/yuv.h \
                        $(HOSTDIR)/record_h264.o $(HOSTDIR)/record_mp4.o
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fno-common \
	        -fno-strict-aliasing -O2 $(RECORD_HOST) -Iruntime/upstream/stb \
	        -o $@ tools/test_record.c user/kits/record/record_core.c \
	        user/kits/gfx/yuv.c $(HOSTDIR)/record_h264.o \
	        $(HOSTDIR)/record_mp4.o -lm

$(HOSTDIR)/test_shadow: tools/test_shadow.c user/kits/gfx/shadow.c user/kits/gfx/shadow.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ \
	        tools/test_shadow.c user/kits/gfx/shadow.c

# The ELF reader (`docs/elf.md` step 2): a Kosmos program built in memory,
# read into its image and broken twenty-three ways - and the system's own ELF
# read into exactly the bytes `objcopy` made of it, when it has been built.
$(HOSTDIR)/test_elfimage: tools/test_elfimage.c user/init/elfimage.c user/include/elfimage.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -Iuser/include -o $@ \
	        tools/test_elfimage.c user/init/elfimage.c

$(HOSTDIR)/test_fatdecode: tools/test_fatdecode.c user/servers/fat_decode.c user/servers/fat_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_fatdecode.c user/servers/fat_decode.c

$(HOSTDIR)/fatls: tools/fatls.c user/servers/fat_decode.c user/servers/fat_decode.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/fatls.c user/servers/fat_decode.c

#
# And where the volumes on a drive are, and what they are called (USB step
# 6b): the two partition tables, kfs's superblock, FAT32's free-cluster hint,
# and the naming Diego settled on 16 September - `Untitled` for a volume with
# no label, and a repeated label numbered in arrival order.
#
# **The kfs half is held to a volume `mkfs` really wrote**, and that is the
# point of the fixture rather than a convenience. This test first built a
# superblock by hand and passed while agreeing with nothing: a real 32 MB
# volume has 512 inodes, its journal at block 18 and its data at 274, where
# the invented numbers were 64, 40 and 64. The drive server reads a volume
# with `kfs.c` itself since `docs/diskfs.md` step 4 - there is no second
# copy of the layout - and the fixture is made by `tools/kfs.lua` on it.
#
$(HOSTDIR)/kfs-fixture.img: tools/kfs.lua $(HOSTDIR)/lua
	@mkdir -p $(dir $@)
	@rm -f $@
	$(HOSTDIR)/lua tools/kfs.lua create $@ 32 >/dev/null

#
# /Temporary's store, on this machine: everything `ramfs.c` does but receive
# and reply, driven with the requests the namespace sends (step 4 after
# 0.11) - more than the 128 files and 16 KB it once held, and a ceiling that
# refuses.
#
$(HOSTDIR)/test_ramstore: tools/test_ramstore.c user/servers/ramstore.c \
	        user/servers/ramstore.h user/include/ramproto.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -fno-strict-aliasing \
	        -Iuser/include -Iuser/servers -o $@ \
	        tools/test_ramstore.c user/servers/ramstore.c

#
# libsmb2 on this Mac (`docs/sharing.md` N0): its own `smb2-ls-async` and
# `smb2-cat-async`, built from the vendored tree as released with the
# `config.h` its cmake would have made, for `tools/test_smbpeer.py` to hold
# to Samba run as the user. Its Kerberos and Apple AES are left out, as the
# port leaves them.
#
LIBSMB2_HOST_SRCS := $(filter-out $(LIBSMB2)/lib/aes_apple.c $(LIBSMB2)/lib/krb5-wrapper.c,$(wildcard $(LIBSMB2)/lib/*.c))

$(HOSTDIR)/libsmb2/%: $(LIBSMB2)/examples/%.c $(LIBSMB2_HOST_SRCS) tools/libsmb2_mac_config.h
	@mkdir -p $(dir $@)
	@cp tools/libsmb2_mac_config.h $(dir $@)config.h
	$(HOST_CC) -O2 -w -DHAVE_CONFIG_H '-D_U_=__attribute__((unused))' \
	        -I$(dir $@) -I$(LIBSMB2)/include -I$(LIBSMB2)/include/smb2 \
	        -I$(LIBSMB2)/lib $(LIBSMB2_HOST_SRCS) $< -o $@

# What a server calls itself, out of NTLM's challenge (`testing.md` 18.415).
$(HOSTDIR)/test_ntlmname: tools/test_ntlmname.c user/kits/smb/ntlm_name.c \
	        user/kits/smb/ntlm_name.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -Iuser/kits/smb -o $@ \
	        tools/test_ntlmname.c user/kits/smb/ntlm_name.c

$(HOSTDIR)/test_drivesdecode: tools/test_drivesdecode.c \
	        user/servers/drives_decode.c user/servers/drives_decode.h \
	        user/servers/fat_decode.c user/servers/fat_decode.h \
	        user/servers/kfs.c user/servers/kfs.h \
	        user/drivers/usb/storage_decode.c user/drivers/usb/storage_decode.h \
	        $(HOSTDIR)/kfs-fixture.img
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -Iuser -o $@ \
	        -DKFS_FIXTURE='"$(HOSTDIR)/kfs-fixture.img"' \
	        tools/test_drivesdecode.c user/servers/drives_decode.c \
	        user/servers/fat_decode.c user/servers/kfs.c \
	        user/drivers/usb/storage_decode.c

#
# And whether the userland image's canary works, which is the same argument
# a fourth time with one difference: the two halves are written in different
# languages.
#
# `tools/bin2c.py` computes the checksums on the host in Python and
# `kernel/image_sum.h` recomputes them on the machine in C. If those ever
# disagree the boot log calls a healthy image corrupt, on every machine - and
# a canary that cries on a healthy boot is worse than none, because the next
# real one is read as the same false alarm. Nothing at run time can catch
# that: the two never meet except on the machine whose memory is in question.
#
# So the fixture goes through the real script, and the test compiles against
# what it emitted. Ten thousand and one bytes rather than a round number, so
# the last page is short - which is the case every real blob has and a wrong
# length would report as one permanently bad page.
#
$(HOSTDIR)/imagesum_fixture.c: tools/bin2c.py
	@mkdir -p $(dir $@)
	@python3 -c "import sys; sys.stdout.buffer.write(bytes((i * 37 + 11) % 251 for i in range(10001)))" \
	        > $(HOSTDIR)/imagesum_fixture.bin
	@python3 tools/bin2c.py $(HOSTDIR)/imagesum_fixture.bin fixture $@

# The userland image's sums, by `kernel/image_sum.h` on the host, for
# `bin2c.py --incbin` (`roadmap.md` 6zp): the kernel's own functions, so the
# build and the machine cannot disagree about what a sum is.
$(HOSTDIR)/imagesums: tools/imagesums.c kernel/image_sum.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ tools/imagesums.c

$(HOSTDIR)/test_imagesum: tools/test_imagesum.c kernel/image_sum.h \
                          $(HOSTDIR)/imagesum_fixture.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_imagesum.c $(HOSTDIR)/imagesum_fixture.c

# The Mac's Lua, with the filesystem's C core in it as `require "kfsc"`
# (`docs/diskfs.md`): `tools/host_lua.c` runs a script as upstream's `lua.c`
# does, and `tools/kfs.lua` makes every disk with the C the disk server is
# moving to. The core and the wrapper are built with every warning, as the
# machine builds them; upstream Lua with none, as it always was here.
$(HOSTDIR)/kfs.o: user/servers/kfs.c user/servers/kfs.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -c -o $@ user/servers/kfs.c

$(HOSTDIR)/host_lua.o: tools/host_lua.c user/servers/kfs.h user/servers/packflat.h \
                       user/servers/diskcache.h lua/kosmos/serialize.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -Ilua/upstream -Ilua/kosmos \
	      -Ikernel -Iarch/aarch64 -c -o $@ tools/host_lua.c

# The disk server's reader of `sys.pack`'s tables, and the serialiser itself
# for it to be held to (`tools/test_packflat.lua`). `serialize.c` reads the
# kernel's `struct message`, whose headers are the ARM board's: this Mac is
# an arm64 one, and nothing here calls what their assembly is for.
$(HOSTDIR)/packflat.o: user/servers/packflat.c user/servers/packflat.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -c -o $@ user/servers/packflat.c

$(HOSTDIR)/serialize.o: lua/kosmos/serialize.c lua/kosmos/serialize.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -Ilua/upstream -Ilua/kosmos \
	      -Ikernel -Iarch/aarch64 -c -o $@ lua/kosmos/serialize.c

$(HOSTDIR)/diskcache.o: user/servers/diskcache.c user/servers/diskcache.h user/servers/kfs.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -c -o $@ user/servers/diskcache.c

# The disk server's cache, asked the questions `blockcache.lua`'s test asked
# of the Lua cache it replaced (`docs/diskfs.md` step 3).
$(HOSTDIR)/test_diskcache: tools/test_diskcache.c user/servers/diskcache.c \
                           user/servers/diskcache.h user/servers/kfs.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ tools/test_diskcache.c \
	      user/servers/diskcache.c

$(HOSTDIR)/lua: $(HOSTDIR)/kfs.o $(HOSTDIR)/host_lua.o $(HOSTDIR)/packflat.o \
                $(HOSTDIR)/serialize.o $(HOSTDIR)/diskcache.o lua/upstream/linit.c \
                $(LUA_HOST_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -O1 -w -Ilua/upstream -o $@ $^ -lm

# A stamp rather than a phony target: the generated sources depend on this,
# and a phony one would rebuild them on every make.
$(HOSTDIR)/lua.ok: $(LUA_FILES) $(HOSTDIR)/luaparse $(HOSTDIR)/luac tools/luaglobals.py
	@$(HOSTDIR)/luaparse $(LUA_FILES)
	@python3 tools/luaglobals.py $(HOSTDIR)/luac $(LUA_FILES)
	@touch $@

.PHONY: check-lua
check-lua: $(HOSTDIR)/lua.ok

# Everything in user/bin/, as Lua source the /bin server serves. There is
# no disk until M8, so a program reaches the system by being in the image.
#
# **Two directories, one namespace.** `apps/` is what opens a window and
# `programs/` is what prints - the split is `glossary.md`'s and it is for
# whoever is reading the tree, not for the machine. Both are served flat at
# `/bin`, so a name typed at the prompt is the same name it always was, and
# `progs2c.py` takes each file's basename.
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
#
# `docs/` and the top-level dot-directories are left out for the same
# reason: nothing in an image comes from them. A ThinkPad photograph of
# 0.10.54 said `d6dfabb-dirty` because of an editor's `.vscode/` and a stray
# screenshot. Untracked files elsewhere still count, because `user/bin/` is
# picked up by wildcard.
#
# **Except the two parts of `docs/` the image carries**, asked about on
# their own: the cheat sheet and Cafesa3D's tutorial (the assets rule
# below). All of `docs/` used to count, and on 27 September a note written
# into `boot.md` while the gate ran made its second make `-dirty` and its
# first not: `make apps` relinked `init.elf` for the new string and had no
# reason to make `init.bin` again, so the host suite found the two
# disagreeing - a failure about the notes, in a gate about the code.
#
# **One make, one name** - worked out by the make somebody typed and handed
# to every make it starts, rather than asked of git again by each. The same
# evening the note in `boot.md` was fixed, a *commit* made during the gate
# did the same thing: `gate-images` is several makes in turn, the one after
# the commit named a different revision, and `make apps` relinked `init.elf`
# for it and left `init.bin`. Whatever happens to the tree while a build
# runs, the build is of one revision and says one.
#
IMAGE_DOCS := docs/cheatsheet.html docs/cafesa3d-tutorial

ifndef KOSMOS_BUILD
KOSMOS_DIRTY := $(shell { git status --porcelain -- . ':!builds' ':!docs' ':!.*'; \
                          git status --porcelain -- $(IMAGE_DOCS); } 2>/dev/null | head -1)
KOSMOS_BUILD := $(shell git describe --always 2>/dev/null || echo "no-git")$(if $(KOSMOS_DIRTY),-dirty,)
KOSMOS_DATE  := $(shell git log -1 --format=%cd --date=format:'%Y-%m-%d' \
                        2>/dev/null || echo "unknown")
endif

export KOSMOS_BUILD KOSMOS_DATE

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

# The same icons at 16 and at 64, as the same release exports them, in the
# image as `16x16/<name>` and `64x64/<name>` (`assets/icons/README.md`).
# `gc:icon` draws each size it has as it is, and any other size from the 64
# averaged down - the Deskbar's 24 today, and every icon at a scale later.
ICON16_FILES := $(sort $(wildcard assets/icons/16x16/*.png))
ICON64_FILES := $(sort $(wildcard assets/icons/64x64/*.png))

# The line icons the mockups draw, at every size the scale asks for, in the
# image as `line/<name>-<px>` (`tools/lineicons.py`). Coverage only: the kit
# paints a look's colour through them (`gfx.c`'s `tint`).
LINE_FILES := $(sort $(wildcard assets/icons/line/*.png))

# Not a picture, and in the same table anyway.
#
# `assets/*.txt` is the project's own artwork rather than something
# vendored - the banner `neofetch` draws. It is here because the table is
# already "small files carried inside the image", which is exactly what it
# is, and `sys.asset` is already the door to them. A second mechanism for
# one text file would be a second mechanism.
#
# Text, so it *could* have travelled as Lua source. It does not, because
# then it would be a program in /bin or a library in /lib - two places whose
# listings are meant to be things you can run and things you can load - and
# a picture is neither.
ART_FILES := $(sort $(wildcard assets/*.txt))

# And `LICENSE`, the one file from the root of the tree the image carries.
# The About window lists what it says, read with `sys.asset("LICENSE")`,
# so the window keeps no licence text of its own to fall out of step with
# the file. `licence_for` finds `LICENSE` itself as the only licence in its
# directory, so it is not reported as unlicensed.
#
# And `docs/cheatsheet.html`, which the desktop writes into `/Home/Desktop`
# whenever it finds it missing or different - see `tracker.lua`. It is the
# project's own work, so there is no licence beside it and the generated
# file says so, which is what that line of the report is for.

#
# **A file taken out rebuilds its table too.** Make rebuilds when a
# prerequisite is newer, and a file that is gone is newer than nothing - so
# a font or an icon removed from `assets/` stayed in the image, unnoticed,
# until something else touched the table. The faces check found it on 18
# September: a control's broken font, removed after the run, was still in
# the "real" image and killed the next one. So each table also depends on
# a stamp holding its list of files, rewritten only when the list changes,
# which is the flags stamps' trick for the same kind of question.
#
ASSET_LIST := $(GEN)/assets.list

# NetSurf's own default stylesheets, which its layout is written against
# (`roadmap.md` 6zz j): carried as `netsurf/default.css` and
# `netsurf/quirks.css`, and handed to the web kit by the browser. In the
# list below so that adding them rebuilt the table: vendored with their
# release dates, they were older than it, and make saw nothing to do.
NETSURF_SHEETS := runtime/upstream/netsurf/netsurf/resources/default.css \
                  runtime/upstream/netsurf/netsurf/resources/quirks.css

# Liang's hyphenation patterns, each language's beside its licence
# (`assets/hyphenation/README.kosmos.md`), for Kosmos Write's setting.
HYPH_FILES := $(sort $(wildcard assets/hyphenation/*.txt))

ASSET_FILES := $(ICON_FILES) $(ICON16_FILES) $(ICON64_FILES) $(LINE_FILES) \
               $(ART_FILES) $(NETSURF_SHEETS) $(HYPH_FILES)
$(shell mkdir -p $(GEN); [ "$$(cat $(ASSET_LIST) 2>/dev/null)" = '$(ASSET_FILES)' ] \
        || printf '%s' '$(ASSET_FILES)' > $(ASSET_LIST))

$(ASSET_LIST):
	@mkdir -p $(dir $@)
	@printf '%s' '$(ASSET_FILES)' > $@

# Cafesa3D's sample scenes (`roadmap.md` 4l): written by the build from
# `tools/cafesa3d_samples.py`, which is the scenes, and carried in the image
# as `scenes/house.gltf` and the rest - so nothing generated is in the tree.
SCENE_FILES := $(GEN)/scenes/house.gltf $(GEN)/scenes/car.gltf $(GEN)/scenes/plane.gltf

$(GEN)/scenes/.made: tools/cafesa3d_samples.py
	python3 tools/cafesa3d_samples.py $(GEN)/scenes
	@touch $@

# **A recipe, even one that does nothing, and it is not decoration.** With
# none, make never looks at the files' times again once the stamp is made,
# so it held the assets against the scenes' old times and carried the old
# scenes in the image - found on 26 September, when the second cut of the
# samples opened in Kosmos as the first.
$(SCENE_FILES): $(GEN)/scenes/.made
	@:

# Cafesa3D's tutorial, `docs/cafesa3d-tutorial/`: every page and picture
# in the folder, carried as `tutorial/cafesa3d/index.html` and the rest and
# read by the browser where they lie (`asset:` addresses). The folder is
# flat because a name here is the file's own. `tools/test_tutorial.lua`
# holds that nothing in it is unreachable from the first page.
TUTORIAL_FILES := $(wildcard docs/cafesa3d-tutorial/*.html docs/cafesa3d-tutorial/*.png)


# **Whom the TLS Kit trusts, by name**, for the browser's Settings (`roadmap.md`
# 6zz d5): the date of Mozilla's bundle and each root's name, read out of
# curl's file, where every certificate follows its name underlined with `=`.
# A few kilobytes where the bundle is 225, which the image does not carry -
# BearSSL's anchors are made from it at build time (`tls_anchors.c`).
ROOT_NAMES := $(GEN)/ca/roots.txt

$(ROOT_NAMES): assets/ca/cacert.pem
	@mkdir -p $(dir $@)
	awk '/^## Certificate data from Mozilla as of:/ { sub(/^## Certificate data from Mozilla as of: /, ""); print "as of " $$0 } prev != "" && /^=+$$/ { print prev } { prev = $$0 }' $< > $@

$(GEN)/assets.c: assets/images/test-pattern.png assets/images/test-quads.jpg \
                 assets/images/test-screen.jpg \
                 $(ASSET_FILES) $(ASSET_LIST) LICENSE \
                 docs/cheatsheet.html tools/assets2c.py $(SCENE_FILES) $(TUTORIAL_FILES) \
                 $(NETSURF_SHEETS) $(ROOT_NAMES)
	@mkdir -p $(dir $@)
	python3 tools/assets2c.py assets_table $@ \
	        assets/images/test-pattern.png assets/images/test-quads.jpg \
	        assets/images/test-screen.jpg \
	        $(ICON_FILES) $(ART_FILES) LICENSE \
	        docs/cheatsheet.html \
	        --prefix=16x16/ $(ICON16_FILES) \
	        --prefix=64x64/ $(ICON64_FILES) \
	        --prefix=line/ $(LINE_FILES) \
	        --prefix=scenes/ $(SCENE_FILES) \
	        --prefix=tutorial/cafesa3d/ $(TUTORIAL_FILES) \
	        --prefix=netsurf/ $(NETSURF_SHEETS) \
	        --prefix=hyphenation/ $(HYPH_FILES) \
	        --prefix=ca/ $(ROOT_NAMES)

# The outline fonts, embedded the same way.
#
# In the image rather than on the disk, because the desktop has to be able
# to draw text on a machine with no drive - which is how every display test
# runs. They are read-only data, so they are paid for once: the image's
# code and read-only data are mapped into every process from one copy
# (design.md, "The read-only half is mapped where it lies"). This said each
# process paid for them, which was true until that mapping was built.
FONT_FILES := $(sort $(wildcard assets/fonts/*.ttf) $(wildcard assets/fonts/*.otf))

FONT_LIST := $(GEN)/fonts.list
$(shell mkdir -p $(GEN); [ "$$(cat $(FONT_LIST) 2>/dev/null)" = '$(FONT_FILES)' ] \
        || printf '%s' '$(FONT_FILES)' > $(FONT_LIST))

$(FONT_LIST):
	@mkdir -p $(dir $@)
	@printf '%s' '$(FONT_FILES)' > $@

#
# **The desktop's pictures**, in `FULL=1` images only - nine megabytes of
# photographs are the desktop's, and the test, bench and lean images have no
# use for them (`assets/wallpapers/README.md`). A table of their own, so an
# image without them carries an empty one rather than a different
# `sys.asset`, and named `wallpaper/<file>` so a list of what the image
# carries says which pictures are meant for the desktop.
#
WALLPAPER_FILES := $(if $(WALLPAPERS),$(sort $(wildcard assets/wallpapers/*.jpg)))

WALLPAPER_LIST := $(GEN)/wallpapers.list
$(shell mkdir -p $(GEN); [ "$$(cat $(WALLPAPER_LIST) 2>/dev/null)" = '$(WALLPAPER_FILES)' ] \
        || printf '%s' '$(WALLPAPER_FILES)' > $(WALLPAPER_LIST))

$(WALLPAPER_LIST):
	@mkdir -p $(dir $@)
	@printf '%s' '$(WALLPAPER_FILES)' > $@

$(GEN)/wallpapers.c: $(WALLPAPER_FILES) $(WALLPAPER_LIST) tools/assets2c.py
	@mkdir -p $(dir $@)
	python3 tools/assets2c.py wallpapers_table $@ --prefix=wallpaper/ $(WALLPAPER_FILES)

$(GEN)/fonts.c: $(FONT_FILES) $(FONT_LIST) tools/assets2c.py
	@mkdir -p $(dir $@)
	python3 tools/assets2c.py fonts_table $@ $(FONT_FILES)

$(GEN)/version.c: FORCE
	@mkdir -p $(dir $@)
	@printf '/* Generated by the Makefile. Do not edit. */\n\nconst char kosmos_name[] = "$(OS_NAME)";\nconst char kernel_name[] = "$(KERNEL_NAME)";\nconst char kosmos_version[] = "$(VERSION)";\nconst char kosmos_build[] = "$(KOSMOS_BUILD)";\nconst char kosmos_date[] = "$(KOSMOS_DATE)";\nconst char kosmos_platform[] = "$(PLATFORM)";\n' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

.PHONY: FORCE
FORCE:

# **And the looks** (`roadmap.md` 6s c3b): `user/themes/*.theme`, carried
# under `themes/` in the same store and shown as `/Kosmos/Themes` - a folder
# of the store, which the applications and the programs, its top level,
# never include. A file name without a space: make cannot hold one.
THEMES := $(wildcard user/themes/*.theme)

# **Pages the menu opens** (`user/pages/*.page`): a launcher in the shipped
# menu that starts the browser at an address - the cheat sheet, a tutorial -
# made by `binfs` from the file's lines as an application's is from its
# header, and kept in `pages/` so neither /Kosmos/Apps nor /Kosmos/Programs
# lists them.
PAGES := $(wildcard user/pages/*.page)

# **The IDE's templates** (`docs/tinycc.md`, C6): a folder each, read only in
# `/Kosmos/Templates`, copied whole into a new project.
TEMPLATES := $(wildcard user/templates/*/*)

$(GEN)/programs.c: $(BIN_LUA) $(THEMES) $(PAGES) $(TEMPLATES) tools/progs2c.py $(HOSTDIR)/lua.ok
	@mkdir -p $(dir $@)
	python3 tools/progs2c.py programs_lua $@ $(BIN_LUA) \
	    --rooted user/themes themes/ $(THEMES) \
	    --rooted user/pages pages/ $(PAGES) \
	    --rooted user/templates templates/ $(TEMPLATES)

# The libraries in user/lib/, the same way and for the same reason. A
# separate store rather than a directory inside /bin, because a program is
# something you run and a library is something you load, and a `/bin` that
# lists both is a `/bin` where `ls` lies about what you can type.
#
# The solar system's portable core: nine Lua files that are a program's
# library rather than a program, carried under `solar/` so
# `use("/lib/solar/app.lua")` reads them straight out. The library store is
# the honest place for them - they are libraries carried in the image, which
# is what `/lib` is - and it costs nothing: `binfs.c` finds an entry with
# `strcmp`, so a key with slashes reads straight out, and no server, role or
# capability had to be invented to serve them. Lite XL's 78 files were the
# first to be carried this way, until it left the tree on 26 September. They live in a directory of their own because they arrive from
# another repository unmodified - `user/lib/solar/README.kosmos.md` - and a
# wildcard over `user/lib/*.lua` deliberately does not reach into it.
#
SOLAR_DATA := $(shell find user/lib/solar -name '*.lua' 2>/dev/null)
SOLAR_ROOTED := --rooted user/lib/solar solar/

#
# Cafesa3D's translators - one Lua file a format, found by Cafesa3D when it
# starts (`roadmap.md` 4l, 5c) - under `translators/`, so a new format is a
# new file here and nothing else.
#
TRANSLATORS := $(wildcard user/lib/translators/*.lua)
TRANSLATORS_ROOTED := --rooted user/lib/translators translators/

#
# **The window manager's parts** (`roadmap.md` 6zn): `wm.lua` in files by
# job, under `wm/`, used by whole path as any library is.
#
WM_PARTS := $(wildcard user/lib/wm/*.lua)
WM_ROOTED := --rooted user/lib/wm wm/

#
# **Groove's parts** (`roadmap.md` 6zh): PulseMusic's files, converted -
# the window, the widgets, the song, the sound bank and the demos - under
# `groove/`, used by whole path. The sound itself is the Synth Kit.
#
GROOVE_PARTS := $(wildcard user/lib/groove/*.lua)
GROOVE_ROOTED := --rooted user/lib/groove groove/

#
# **luacheck**, for the IDE's checking (`roadmap.md` 6n, step 4): vendored
# unmodified in runtime/upstream/luacheck/ and carried as `/lib/luacheck/`,
# where `/lib/lint.lua`'s `require` finds its modules. Not in LUA_FILES:
# it is upstream's Lua, written for a Lua with `io` and `require`, and the
# checks above are about Kosmos's.
#
LUACHECK := $(shell find runtime/upstream/luacheck/src/luacheck -name '*.lua' 2>/dev/null)
LUACHECK_ROOTED := --rooted runtime/upstream/luacheck/src/luacheck luacheck/

$(GEN)/libraries.c: $(wildcard user/lib/*.lua) $(SOLAR_DATA) $(TRANSLATORS) \
                    $(WM_PARTS) $(GROOVE_PARTS) $(LUACHECK) tools/progs2c.py $(HOSTDIR)/lua.ok
	@mkdir -p $(dir $@)
	python3 tools/progs2c.py libraries_lua $@ $(wildcard user/lib/*.lua) \
	    $(SOLAR_ROOTED) $(SOLAR_DATA) \
	    $(TRANSLATORS_ROOTED) $(TRANSLATORS) \
	    $(WM_ROOTED) $(WM_PARTS) \
	    $(GROOVE_ROOTED) $(GROOVE_PARTS) \
	    $(LUACHECK_ROOTED) $(LUACHECK)

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

#
# **A program's own image** (`docs/elf.md` step 3): the system's objects and
# the kit it brings, linked as `init.elf` is. Nothing is compiled twice - a
# kit it brings is declared weak in `sys_user.c`, so that file is the same
# in every image - and an image costs a link. `apptest` is the loader's test
# kit, in no image but this one; the build says so every time it links it,
# because a kit that leaked into the system's image would make the loader's
# test pass without the loader.
#
APPTEST_OBJS := $(UBUILD)/user/kits/apptest/apptest.c.o

$(UBUILD)/apps/apptest.elf: $(USER_OBJS) $(APPTEST_OBJS) $(UBUILD)/init.elf user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(USER_OBJS) $(APPTEST_OBJS) -o $@ $(LIBS)
	@$(NM) $@ | grep -q ' T kosmos_apptest_kit$$' \
	    || { echo "apps: apptest.elf does not have its kit"; rm -f $@; exit 1; }
	@! $(NM) $(UBUILD)/init.elf | grep -q ' T kosmos_apptest_kit$$' \
	    || { echo "apps: the system's image has the loader's test kit in it"; rm -f $@; exit 1; }

#
# **Doom's image** (`docs/elf.md` step 5): the system's objects and Doom's,
# linked as `apptest.elf` is, and held to the same two promises - the image
# has the kit, and the system's does not.
#
DOOM_OBJS := $(addprefix $(UBUILD)/,$(addsuffix .o,$(DOOM_SRCS)))

$(UBUILD)/apps/doom.elf: $(USER_OBJS) $(DOOM_OBJS) $(UBUILD)/init.elf user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(USER_OBJS) $(DOOM_OBJS) -o $@ $(LIBS)
	@$(NM) $@ | grep -q ' T kosmos_doom_kit$$' \
	    || { echo "apps: doom.elf does not have Doom"; rm -f $@; exit 1; }
	@! $(NM) $(UBUILD)/init.elf | grep -q ' T kosmos_doom_kit$$' \
	    || { echo "apps: the system's image has Doom in it"; rm -f $@; exit 1; }

# **Quake's and the Super Nintendo's**, the same way and to the same two
# promises (`docs/elf.md` step 5).
QUAKE_OBJS := $(addprefix $(UBUILD)/,$(addsuffix .o,$(QUAKE_SRCS)))
SNES_OBJS  := $(addprefix $(UBUILD)/,$(addsuffix .o,$(SNES_SRCS)))

$(UBUILD)/apps/quake.elf: $(USER_OBJS) $(QUAKE_OBJS) $(UBUILD)/init.elf user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(USER_OBJS) $(QUAKE_OBJS) -o $@ $(LIBS)
	@$(NM) $@ | grep -q ' T kosmos_quake_kit$$' \
	    || { echo "apps: quake.elf does not have Quake"; rm -f $@; exit 1; }
	@! $(NM) $(UBUILD)/init.elf | grep -q ' T kosmos_quake_kit$$' \
	    || { echo "apps: the system's image has Quake in it"; rm -f $@; exit 1; }

$(UBUILD)/apps/snes.elf: $(USER_OBJS) $(SNES_OBJS) $(UBUILD)/init.elf user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(ULDFLAGS) $(USER_OBJS) $(SNES_OBJS) -o $@ $(LIBS)
	@$(NM) $@ | grep -q ' T kosmos_snes_kit$$' \
	    || { echo "apps: snes.elf does not have the Super Nintendo"; rm -f $@; exit 1; }
	@! $(NM) $(UBUILD)/init.elf | grep -q ' T kosmos_snes_kit$$' \
	    || { echo "apps: the system's image has the Super Nintendo in it"; rm -f $@; exit 1; }

#
# **Against the lean userland, whatever this build is.** An application's
# image carries its own copy of the runtime and not the system's files -
# binfs serves those from the system's image - and linked against a `FULL=1`
# userland it carried the wallpapers, the browser and FFmpeg as well: Doom's
# was 33 MB stripped, read off the disk at every first start, for nothing it
# uses. So `make apps` builds `build/user/apps/` (or `build/user-x86_64/`),
# the one place `run_loader.py` and the stick take them from, and `MEGA=`
# says so because an inherited one would turn `FULL` back on.
#
#
# **TinyCC** (`docs/tinycc.md`, step C1): built on the Mac from the vendored
# source, as shipped, with Kosmos's layout as a patch applied to a copy -
# `.text.start` first, two segments, the code a page into the file, and
# `__bss_start` and `__bss_end` defined - so the loader takes what it links.
# Cross compilers for both processors, in one build of TinyCC's own make,
# run with nothing of this one's variables in its environment.
#
TCC      := $(TCC_HOST)/$(if $(filter x86_64,$(ARCH)),x86_64,arm64)-tcc
TCC_INC  := -nostdinc -Iuser/kits/tcc/include -I$(TCC_HOST)/include -Ilua/upstream \
            -Ilua/kosmos -Iruntime/include -Iuser/include -DKOSMOS_USER \
            -include lua/kosmos/kosmos_lua.h

# TinyCC for Kosmos itself, the C Kit's compiler (C3): the same patched copy,
# vendored code under `-w` as Doom's is, `-std=gnu11` for the extensions it
# is written in, and none of its parts a compiler that writes a file and runs
# nothing needs - no `-run`, no bounds checker, no backtraces, no lock.
$(UBUILD)/$(TCC_HOST)/libtcc.c.o: $(TCC_HOST)/.built user/kits/tcc/shim.h \
                                  user/kits/tcc/sys/time.h $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -std=gnu11 -w -Wno-error -Iuser/kits/tcc -I$(TCC_HOST) \
	        -include user/kits/tcc/shim.h -D$(TCC_KIT_TARGET) -DONE_SOURCE=1 \
	        -DCONFIG_TCC_STATIC=1 -DCONFIG_TCC_BACKTRACE=0 -DCONFIG_TCC_BCHECK=0 \
	        -DCONFIG_TCC_SEMLOCK=0 -DTCC_KOSMOS_LAYOUT=1 -DTCC_VERSION='"0.9.28rc"' \
	        -DCONFIG_TCCDIR='"/Home/Developer"' -c $(TCC_HOST)/libtcc.c -o $@

$(UBUILD)/user/kits/tcc/tcc_kosmos.c.o: $(TCC_HOST)/.built
$(UBUILD)/user/kits/tcc/tcc_kosmos.c.o: UCFLAGS += -I$(TCC_HOST) -Iuser/kits/tcc
$(UBUILD)/user/kits/tcc/stamp.c.o: UCFLAGS += -Iuser/kits/tcc
$(UBUILD)/user/kits/tcc/shim.c.o: UCFLAGS += -Iuser/kits/tcc

$(TCC_HOST)/.built: $(wildcard $(TCC_UP)/*.c $(TCC_UP)/*.h $(TCC_UP)/lib/* $(TCC_UP)/include/*) \
                    $(wildcard runtime/patches/tinycc/*.patch)
	@rm -rf $(TCC_HOST) && mkdir -p $(TCC_HOST)
	cp -R $(TCC_UP)/. $(TCC_HOST)/
	for p in runtime/patches/tinycc/*.patch; do patch -s -d $(TCC_HOST) -p0 < $$p || exit 1; done
	cd $(TCC_HOST) && env -i PATH="$$PATH" HOME="$$HOME" sh -c \
	    './configure >/dev/null && make cross-arm64 cross-x86_64 CFLAGS="-O2 -DTCC_KOSMOS_LAYOUT" >/dev/null 2>&1'
	@test -x $(TCC_HOST)/arm64-tcc && test -x $(TCC_HOST)/x86_64-tcc
	@touch $@

$(HOSTDIR)/tcc_stamp: tools/tcc_stamp.c user/kits/tcc/stamp.c user/kits/tcc/stamp.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -Iuser/kits/tcc -o $@ \
	        tools/tcc_stamp.c user/kits/tcc/stamp.c

#
# **The loader's test kit, built by TinyCC** (C1's test): `apptest.c` compiled
# by TinyCC and linked by it with the lean userland - packed into one object
# by GCC's `ld -r`, as the image will carry it (C2) - and stamped. In a folder
# of its own beside `apps/`, with the other applications linked in, so
# `run_loader.py` runs over it exactly as it runs over GCC's.
#
$(UBUILD)/tcc/runtime.o: $(USER_OBJS)
	@mkdir -p $(dir $@)
	$(CROSS)ld -r $(USER_OBJS) -o $@

$(UBUILD)/tcc/head.o: user/kits/tcc/head.c $(TCC_HOST)/.built
	@mkdir -p $(dir $@)
	$(TCC) -c $< -o $@

$(UBUILD)/tcc/apptest.o: user/kits/apptest/apptest.c $(TCC_HOST)/.built
	@mkdir -p $(dir $@)
	$(TCC) $(TCC_INC) -c $< -o $@

#
# **The developer files** (step C2, `/Home/Developer` - Diego's decision 1):
# what TinyCC needs to build an image anywhere, and nothing else. The runtime
# without its debugging information, which carries the build's protocol
# stamp behind its marker, so the C Kit can refuse a pack from another build;
# libgcc; the header slot; and the headers - TinyCC's own, the two GCC gives
# Kosmos's build and TinyCC lacks, Lua's, the libc's and Kosmos's. Installed
# by `make install-apps` and carried by a stick's `/Home` (`installed.py`).
#
DEV := $(UBUILD)/developer

$(DEV)/.made: $(UBUILD)/tcc/runtime.o $(UBUILD)/tcc/head.o $(TCC_HOST)/.built \
              $(wildcard user/kits/tcc/include/*.h user/kits/window/kosmos_window.h kernel/syscall.h runtime/include/*.h runtime/include/sys/*.h \
                         user/include/*.h lua/upstream/*.h lua/kosmos/kosmos_lua.h)
	@rm -rf $(DEV) && mkdir -p $(DEV)/include/sys $(DEV)/include/lua
	$(OBJCOPY) --strip-debug $(UBUILD)/tcc/runtime.o $(DEV)/runtime.o
	cp $(shell $(CC) -print-libgcc-file-name) $(DEV)/libgcc.a
	cp $(UBUILD)/tcc/head.o $(DEV)/head.o
	cp $(TCC_HOST)/include/*.h user/kits/tcc/include/*.h user/kits/window/kosmos_window.h runtime/include/*.h \
	   user/include/*.h kernel/syscall.h lua/kosmos/kosmos_lua.h $(DEV)/include/
	cp runtime/include/sys/*.h $(DEV)/include/sys/
	cp lua/upstream/lua.h lua/upstream/luaconf.h lua/upstream/lauxlib.h \
	   lua/upstream/lualib.h $(DEV)/include/
	@grep -q "KOSMOS-PROTOSTAMP:" $(DEV)/runtime.o \
	    || { echo "developer: the runtime carries no protocol stamp"; exit 1; }
	@touch $@

#
# Linked from the developer files alone - its headers and nothing of the
# tree's - so the test that runs it says the pack is enough.
#
$(UBUILD)/tcc/apptest-dev.o: user/kits/apptest/apptest.c $(DEV)/.made
	$(TCC) -nostdinc -I$(DEV)/include -DKOSMOS_USER -include kosmos_lua.h -c $< -o $@

$(UBUILD)/apps-tcc/apptest.elf: $(DEV)/.made $(UBUILD)/tcc/apptest-dev.o $(HOSTDIR)/tcc_stamp \
                                $(UBUILD)/apps/doom.elf $(UBUILD)/apps/quake.elf $(UBUILD)/apps/snes.elf
	@mkdir -p $(dir $@)
	$(TCC) -nostdlib -static -Wl,-Ttext=$(USER_BASE) -o $@ $(DEV)/head.o \
	        $(DEV)/runtime.o $(UBUILD)/tcc/apptest-dev.o $(DEV)/libgcc.a
	$(HOSTDIR)/tcc_stamp $@ $(USER_BASE)
	@for a in doom quake snes; do ln -sf ../apps/$$a.elf $(dir $@)$$a.elf; done

.PHONY: apps app-images
apps:
	@$(MAKE) --no-print-directory FULL=0 MEGA= app-images

app-images: $(UBUILD)/apps/apptest.elf $(UBUILD)/apps/doom.elf \
            $(UBUILD)/apps/quake.elf $(UBUILD)/apps/snes.elf \
            $(UBUILD)/apps-tcc/apptest.elf

$(GEN)/init_bin.c: $(UBUILD)/init.bin tools/bin2c.py $(HOSTDIR)/imagesums
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py --incbin $(HOSTDIR)/imagesums $< init_image $@

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
#   build/host/lua tools/kfs.lua create build/play.img 32 \
#       ~/Downloads/book.pdf:/Home/book.pdf
#
#
# **128 MB since 21 September**, because the solar system's baked textures
# are eleven and a half megabytes and the old 32 would not hold them beside
# Doom's WAD and a Super Nintendo cartridge. Diego: "grow the disk to
# 128mb".
#
# **What that costs, and it is not nothing.** 32 was the number that worked
# on *both* QEMU and the ThinkPad's stick, where a disk over 32 MB does not
# boot - `tools/mkusb_image.py` refuses one and `docs/thinkpad.md` §6a has
# why. That refusal is still there and is now the thing that catches it, so
# **the old stick layout needs `make usb USB_HOME=disk DISK_MB=32`**; the
# layout every stick has had since 19 September carries `/Home` in a
# partition of its own and does not use this disk at all.
#
DISK      := build/kosmos.img
DISK_MB   ?= 128

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

#
# The network, and what `user` mode actually is.
#
# QEMU's `-netdev user` is slirp: a NAT in the emulator, no privileges
# needed. The guest is 10.0.2.15, the gateway and DNS are 10.0.2.2 and
# 10.0.2.3, and slirp answers ARP for them. **Guest ICMP is translated into
# a host ping socket** rather than put on a wire, which works unprivileged
# on macOS - so `ping 8.8.8.8` really does reach 8.8.8.8, through a
# translation rather than as raw ICMP.
#
# Real layer 2 is `-netdev vmnet-shared`, which needs root. Worth knowing
# before anybody claims a frame went out exactly as written.
#
# `make NONET=1 qemu` leaves the card out, which is how the no-card branch
# of `hal_net_init` gets exercised.
#
#
# `HTTP=8080` forwards a port *in*, which is the other direction.
#
# slirp NATs outbound and drops everything inbound, so a server inside the
# machine is unreachable from this computer until a port is forwarded.
# `make HTTP=8080 qemu` then `curl localhost:8080` reaches `httpd` on port 80
# inside the guest - which is what makes the HTTP server testable at all.
#
FORWARD := $(if $(HTTP),$(comma)hostfwd=tcp::$(HTTP)-:80,)$(if $(TELNET),$(comma)hostfwd=tcp::$(TELNET)-:23,)$(if $(VNC),$(comma)hostfwd=tcp::$(VNC)-:5900,)

# **`TELNET=2323` and `VNC=5901`, the same way**, for the command line and
# the screen (`telnetd` on 23 inside, `vncd` on 5900). Diego, 30 September,
# with the Servers window open in `make qemu`: "how can i telnet into the
# kosmos qemu instance?" - 10.0.2.15 is slirp's address for the guest and
# this computer cannot reach it. `TELNET=` also starts `telnetd` at boot, as
# a development stick does, so `make TELNET=2323 qemu` and then `telnet
# localhost 2323` is the whole of it. `VNC=` forwards only: the screen is
# switched on in Servers, since it lends the desktop. 5901 rather than 5900
# on this side, which macOS's own Screen Sharing may hold.
TELNETARG := $(if $(TELNET),-fw_cfg 'name=opt/kosmos/telnetd$(comma)string=23',)

NET_FLAGS := $(if $(NONET),,-netdev user$(comma)id=net0$(FORWARD) \
                            -device virtio-net-device$(comma)netdev=net0)

# How many processors the machine has, which is not how many Kosmos
# schedules on.
#
# **Four, so that the machine you use is the machine the suite tests.** The
# other three are started at boot, claim their `struct percpu` and park in
# `wfi` - `docs/smp.md` step three - and until step four they run no
# threads. `make SMP=1 qemu` is the single-core machine, and the difference
# is visible in `cores`, in `sysmon`, and in the boot log's third line.
#
# It costs nothing to leave on: a core in `wfi` is a QEMU thread that is not
# scheduled. What it buys is that the bring-up path runs every single time
# somebody boots this thing, rather than only under `make test` - and the
# one bug in it that reached a commit was invisible except in that line.
SMP ?= 4

# How many processors new threads are spread across, inside the guest.
#
# **Separate from `SMP`, which is how many the machine has.** The default is
# every processor that arrived, and this narrows it: `make SMPWORK=1 qemu`
# homes every new thread on core zero, as every boot did until 0.10.22, and
# `make SMPWORK=2 qemu` halves the search space when something breaks. It
# cannot widen past what arrived; `docs/smp.md` has why it is on.
SMPWORK ?=

SMPARG := $(if $(SMPWORK),-fw_cfg 'name=opt/kosmos/smp$(comma)string=$(SMPWORK)',)

# **The display: ramfb, or virtio-gpu when asked** (`roadmap.md` 4h a) -
# `make GPU=virtio qemu`. ramfb scans the guest's pixels out as they are;
# virtio-gpu shows only what the guest sends it, which is what
# `hal_fb_flush` exists for, and this Mac's QEMU has it without virgl.
GPU ?= ramfb
ARM_DISPLAY := $(if $(filter virtio,$(GPU)),-device virtio-gpu-device,-device ramfb)
X86_DISPLAY := $(if $(filter virtio,$(GPU)),-device virtio-gpu-pci,-device ramfb)

QEMUFLAGS := -M virt,gic-version=3 $(ACCEL) -m 512M -smp $(SMP) $(SMPARG) \
             -global virtio-mmio.force-legacy=false \
             $(ARM_DISPLAY) -device virtio-keyboard-device \
             -device virtio-tablet-device \
             -device virtio-rng-device \
             $(AUDIO_FLAGS) \
             $(NET_FLAGS) \
             -drive file=$(DISK),format=raw,if=none,id=disk \
             -device virtio-blk-device,drive=disk \
             $(BOOTARG) $(TELNETARG) \
             -display cocoa$(ZOOM_FLAG)$(FULLSCREEN_FLAG) -serial mon:stdio \
             -kernel $(TARGET)

# The same machine with no screen, for when the window is in the way or the
# terminal is all there is - over ssh, for instance.
# No window, and therefore no keyboard: with -display none QEMU has nowhere
# to take key presses from, so the virtio device would sit there empty.
QEMUFLAGS_SERIAL := -M virt,gic-version=3 $(ACCEL) -m 512M -smp $(SMP) $(SMPARG) -nographic \
                    -global virtio-mmio.force-legacy=false \
                    $(NET_FLAGS) \
                    -drive file=$(DISK),format=raw,if=none,id=disk \
                    -device virtio-blk-device,drive=disk \
                    $(BOOTARG) $(TELNETARG) \
                    -kernel $(TARGET)

.PHONY: all bump bump-minor bump-major qemu fast serial test droplet disktest powertest stress screenshot shot prepush frames bench bench-record debug disasm size clean dist release disk

# A disk image, built here, with whatever you want already in it.
#
#   make image FILES="book.pdf:/Home/books/book.pdf song.mp3:/Home/music/a.mp3"
#
# The same kfs.lua the machine runs, over a file. `tools/kfs.lua` also does
# ls, put, get and rm on an existing image, which is how a file gets on and
# off a disk this Mac cannot mount.
.PHONY: image
image: $(HOSTDIR)/lua
	$(HOSTDIR)/lua tools/kfs.lua create $(DISK) $(DISK_MB) $(FILES)

#
# **The applications the build installs, onto that disk** (`docs/elf.md`
# step 5): Doom, Quake and the Super Nintendo into `/Home/Apps`, each its Lua
# and its image, stripped, over what is there, and the WAD and the pak from
# the top of `HOME_DIR` when the folder has them - `tools/installed.py`'s
# list. Nothing else on the disk is touched; a stick gets the same.
#
.PHONY: install-apps
install-apps: $(HOSTDIR)/lua
	@$(MAKE) --no-print-directory apps
	@for pair in $$(python3 tools/installed.py aarch64 "$(HOME_DIR)"); do \
	    $(HOSTDIR)/lua tools/kfs.lua put $(DISK) "$${pair%%:*}" "$${pair#*:}" || exit 1; \
	done

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

# **The browser's test page into `/Home/www` on the QEMU disk**
# (`assets/www/`, `roadmap.md` 6zz a), where the Servers window's web server
# serves it - Diego, 30 September: "put a sample doc in /www so you can try".
# The disk is formatted by its first boot, so this is for one that has been
# booted, and never while QEMU has it open.
.PHONY: www
www: $(HOSTDIR)/lua
	@test -s $(DISK) || { echo "$(DISK): boot it once first (make qemu), so it is formatted"; exit 1; }
	@if pgrep -f "qemu-system.*$(DISK)" >/dev/null; then echo "QEMU has $(DISK) open: quit it first"; exit 1; fi
	@for f in $(wildcard assets/www/*.html assets/www/*.css assets/www/*.png assets/www/*.jpg assets/www/*.svg); do \
	    $(HOSTDIR)/lua tools/kfs.lua put $(DISK) $$f /Home/www/$$(basename $$f) || exit 1; \
	done
	@echo "the test page is in /Home/www: switch Web on in Servers, and make HTTP=8080 qemu serves it at localhost:8080"

#
# A Dock icon you can drop files on, and they land in the image's /Home.
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

$(BUILD)/%.c.o: %.c $(KFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.S.o: %.S $(KFLAGS_FILE)
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
# The display size an image is built with is only its default.
#
# It was one image per display size, the framebuffer a static array and its
# size a compile-time constant. Since the pixels come from the page
# allocator the size is chosen when the machine starts - `opt/kosmos/fb=WxH`
# through fw_cfg, which `run-kosmos.sh -r` passes (`roadmap.md` 6zt) - and
# `FB=` says what an image does without one.
#
# Changing it while the machine runs is a different question and is written
# up in hal.md: every process that called `gfx.screen()` is holding a
# mapping of a particular length, and a mode change is a protocol none of
# them speak yet.
#
#
# One size for a full image and three for a lean one.
#
# A full image is 6.2 MB against 1.7 - the browser's five vendored libraries
# and Doom - and stripping saves a quarter of a megabyte, because the bulk
# is the userland compiled in rather than symbols. Three copies of that in a
# repository is eighteen megabytes to say the same thing three times, and
# the one size to pick is the one `make qemu` now defaults to.
#
#
# Two sizes for a full image and three for a lean one.
#
# 1920x1080 is what `make qemu` defaults to, and 1280x720 is there because a
# laptop panel that size is a real thing to be testing on and a compiled-in
# framebuffer cannot be resized: the size is the build. A downloaded image
# too big for the screen is not a preference, it is unusable.
#
RELEASE_SIZES := $(if $(WEB),1280x720 1920x1080,\
                   1024x768 1280x800 1920x1080)

# What is in it, in the name, because the images are not interchangeable and
# a name that did not say so is a trap: the browser opens on an image built
# without it and reports no web kit, which reads like a broken browser
# rather than the wrong file. `-full` is the whole system, `FULL=1`, which
# is what `make release` builds unless told otherwise; Doom is no longer in
# it, being an application installed beside it (`docs/elf.md` step 5).
RELEASE_TAG := $(if $(filter 1,$(FULL)),-full,$(if $(WEB),-web,))

# A binary that leaves this machine has been used for a while first.
#
# `make test` says the parts work and `make screenshot` says the machine
# works once. Neither notices a pool that fills on the fiftieth try, and a
# release is the thing somebody else runs without watching it.
release: $(TARGET) stress
	@mkdir -p builds
	@for size in $(RELEASE_SIZES); do \
	    rm -f $(BUILD)/hal/fwcfg/fbpixels.c.o $(TARGET); \
	    $(MAKE) --no-print-directory FB=$$size $(TARGET) >/dev/null; \
	    cp $(TARGET) \
	       builds/kosmos-$(VERSION)-$(KOSMOS_BUILD)-$$size$(RELEASE_TAG).elf; \
	    echo "builds/kosmos-$(VERSION)-$(KOSMOS_BUILD)-$$size$(RELEASE_TAG).elf"; \
	done
	@rm -f $(BUILD)/hal/fwcfg/fbpixels.c.o $(TARGET)
	@$(MAKE) --no-print-directory $(TARGET) >/dev/null
	@cp run-kosmos.sh builds/run-kosmos.sh
	@ls -l builds/

# Ctrl-A then x to quit QEMU.
qemu: $(TARGET) $(DISK)
	$(QEMU) $(QEMUFLAGS)

#
# x86-64, under QEMU.
#
#   make x86            build it and boot it
#   make x86-build      build only
#
# **A flat binary, not an ELF, and the reason is worth keeping.** A
# multiboot loader parses ELF32; this image is ELF64, and QEMU refuses it
# with "Cannot load x86-64 image, give a 32bit one." Bit 16 of the multiboot
# flags - the a.out kludge - tells the loader to use five explicit addresses
# from the header instead of parsing the format, and then the file may be
# anything. `objcopy -O binary` is what makes it anything.
#
# `-mno-red-zone` is not optional either: the red zone is 128 bytes below
# the stack pointer that a leaf function may use without adjusting `rsp`,
# and an interrupt handler pushes over it. Nobody notices until there are
# interrupts.
#
# $(FB_FLAGS) is here rather than on one file's compile line, and the
# comment above it explains why that is right there and wrong here: on ARM
# putting the screen size in CFLAGS rebuilt seventy-eight objects to change
# a number seven of them have never heard of. This build has no objects. It
# is one compile-and-link of every source, so there is nothing finer than
# "all of it" to attach a flag to, and no saving to be had by trying.
#
# Left out, it was not a missing size but the *wrong* one: `ramfb.c` has a
# 1024x768 fallback for a build that names none, so the machine came up at
# a resolution nothing had asked for and the ARM build had not used in
# months. It looked like a display bug and was a Makefile line.
#
# No floating point in the kernel, which is this architecture's spelling of
# `-mgeneral-regs-only`. Kept in a variable of its own because two files in
# the test build have to be compiled *without* it - see `X86_FLAGS_FP`.
#
X86_NO_FP := -mno-mmx -mno-sse -mno-sse2

X86_FLAGS := -std=c11 -ffreestanding -nostdlib -nostartfiles \
             -Wall -Wextra -Werror -fno-common -fno-strict-aliasing -O2 -g \
             -mno-red-zone $(X86_NO_FP) \
             $(FB_FLAGS) \
             -Ihal -Ihal/virtio -Ihal/fwcfg -Iarch/x86_64 -Ikernel -Iruntime/include

#
# The test image gets its own directory, exactly as the ARM one does.
#
# Same sources plus the suite, with `KOSMOS_TEST` defined - so the tests
# cost the shipping image nothing and the two never share a stale object.
# `build/test` is taken by the ARM build, which sets `BUILD` without
# reference to the architecture, so this is the second name rather than a
# suffix on the first.
X86_BUILD := build/x86_64$(if $(TEST),-test)

X86_SRCS  := boot/x86_64/start.S \
             arch/x86_64/vectors.S \
             arch/x86_64/trap.c \
             arch/x86_64/mmu.c \
             arch/x86_64/cpu.c \
             arch/x86_64/switch.S \
             arch/x86_64/fp.c \
             arch/x86_64/gdt.c \
             arch/x86_64/user.c \
             arch/x86_64/user.S \
             arch/x86_64/entry.c \
             hal/pc/uart.c \
             hal/pc/memory.c \
             hal/pc/pic.c \
             hal/pc/apic.c \
             hal/pc/apic_decode.c \
             hal/pc/irq_bind.c \
             hal/pc/timer.c \
             hal/pc/rtc.c \
             hal/pc/power.c \
             hal/pc/s5_decode.c \
             hal/pc/battery_decode.c \
             hal/pc/cpus.c \
             hal/pc/cpu_on.c \
             hal/pc/cpu_here.c \
             hal/pc/trampoline.S \
             hal/fwcfg/fwcfg.c \
             hal/fwcfg/fbpixels.c \
             hal/fwcfg/ramfb.c \
             hal/virtio/gpu.c \
             hal/pc/fb.c \
             hal/pc/loader_fb.c \
             hal/pc/fwcfg_port.c \
             hal/pc/boot_option.c \
             hal/pc/acpi.c \
             hal/pc/ec.c \
             hal/pc/smbios.c \
             hal/pc/smbios_decode.c \
             hal/pc/pci.c \
             hal/pc/virtio.c \
             hal/virtio/wait.c \
             hal/virtio/blk.c \
             hal/pc/nvme.c \
             hal/pc/memdisk.c \
             hal/pc/blk_bind.c \
             hal/virtio/rng.c \
             hal/pc/entropy_bind.c \
             hal/pc/devices.c \
             hal/virtio/net.c \
             hal/virtio/input.c \
             hal/pc/i8042.c \
             hal/pc/i8042_drain.c \
             hal/pc/input_bind.c \
             hal/pc/pointer.c \
             hal/keys.c \
             hal/pointer_edges.c \
             hal/pc/input_describe.c \
             hal/virtio/snd.c \
             hal/pc/hda.c \
             hal/pc/snd_bind.c \
             kernel/console.c \
             kernel/screen.c \
             kernel/boot.c \
             $(X86_BUILD)/version.c \
             $(X86_BUILD)/font_8x16.c \
             runtime/libc/string.c \
             runtime/libc/setjmp-x86_64.S \
             kernel/panic.c \
             kernel/pmm.c \
             kernel/pmm_place.c \
             kernel/pool.c \
             kernel/thread.c \
             kernel/sched_rr.c \
             kernel/sched_prio.c \
             kernel/ipc.c \
             kernel/memobj.c \
             kernel/sharemap.c \
             kernel/irq.c \
             kernel/process.c \
             kernel/smp.c \
             kernel/spinlock.c \
             kernel/syscall.c \
             kernel/profile.c \
             kernel/entropy.c \
             kernel/main.c \
             $(X86_BUILD)/init_bin.c

ifdef TEST
  # The same additions `SRCS` gets on the other board, and for the same
  # reasons: the suite, the ring-3 fixture blobs only it runs, and the libc
  # the kernel no longer links because the unit tests for it call it
  # directly.
  X86_SRCS += tests/tests.c \
              user/hello-x86_64.S user/faulty-x86_64.S user/pointer-x86_64.S \
              runtime/libc/malloc.c \
              runtime/libc/math.c runtime/libc/snprintf.c \
              runtime/libc/strtod.c

  # `-Iuser` because the blobs include `syscall.h` from there, and `-Itests`
  # for the suite's own headers. `$(TESTDEFS)` carries `-DKOSMOS_TEST`.
  X86_FLAGS += $(TESTDEFS) -Iuser

  #
  # The four files that need floating point, compiled with it.
  #
  # `CFLAGS_FP` does exactly this on the other board and for the same
  # reason: the suite tests `snprintf("%f")` and the maths library, and the
  # unit tests for a thing have to be able to call it. **This build has no
  # per-object rules to hang a flag on** - it is one compile-and-link of
  # every source - so these four become objects first and are handed to the
  # link beside the rest.
  #
  # It also settles a second thing, and settles it correctly rather than by
  # accident. `runtime/include/math.h` refuses to compile where
  # `FLT_EVAL_METHOD` is 2, which is what x87 reports and what a build
  # without SSE gets: every `double_t` would be a `long double`, and Kosmos
  # has decided it will not have one. Turning SSE on for these files makes
  # the answer 0, which is the machine actually being used - x86-64 does
  # its arithmetic in SSE and has since it was designed.
  #
  X86_FP_SRCS := tests/tests.c runtime/libc/math.c \
                 runtime/libc/snprintf.c runtime/libc/strtod.c
  X86_FP_OBJS := $(patsubst %,$(X86_BUILD)/%.o,$(X86_FP_SRCS))

  X86_SRCS := $(filter-out $(X86_FP_SRCS),$(X86_SRCS)) $(X86_FP_OBJS)

  #
  # **And what they include, so a changed header makes them again.** The
  # rest of the kernel is compiled afresh at every link, so it never went
  # stale; these four are objects, and their `.d` files were written and
  # never read. On 26 September the heap moved in `kernel/process.h`, the
  # kernel was rebuilt with it and `tests.c` was not, and the suite looked
  # for the heap where it used to be - failing every time on x86-64 and
  # never on AArch64, whose objects' files `DEPS` has always named.
  #
  -include $(X86_FP_OBJS:.o=.d)
endif

# Everything the kernel is built with, minus the ban on FP.
X86_FLAGS_FP := $(filter-out $(X86_NO_FP),$(X86_FLAGS))

$(X86_BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(X86_FLAGS_FP) -MMD -MP -c $< -o $@

# The userland image, built for this architecture and turned into an array.
# The same two steps the ARM image takes, with the arch carried through
# `VARIANT` so the objects of the two never meet.
# `$(UBUILD)`, not a written-out path: inside the recursive call above it is
# `build-user-x86_64`, and naming it literally is how the two came to
# disagree once already.
$(X86_BUILD)/init_bin.c: $(UBUILD)/init.bin tools/bin2c.py $(HOSTDIR)/imagesums
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py --incbin $(HOSTDIR)/imagesums $< init_image $@

.PHONY: x86 x86-build
#
# `FULL=0` is passed through, and it has to be.
#
# Inside the recursive call, `FULL` decides `WEB` and the rest, which decide
# `VARIANT`, which decides `UBUILD` - so leaving it out built the userland
# into `build-user-x86_64-doom-web` while the rule below named
# `build-user-x86_64`. The image then linked against whatever `init_bin.c`
# happened to be lying there, which was one built before the user region
# moved, and the machine faulted at an address from the previous layout.
#
# Doom and the browser are not off here because they would fail: they are
# off because nothing has drawn a pixel on this architecture yet.
#
# `FULL` is passed through rather than forced, and it has to be *passed*.
#
# Inside the recursive call `FULL` decides `WEB` and the rest, which decide
# `VARIANT`, which decides `UBUILD` - so leaving it out once built the
# userland into a directory the rule below did not name, and the image
# linked against whatever `init_bin.c` was lying there. It was one built
# before the user region moved, and the machine faulted at an address from
# the previous layout.
#
# It was `FULL=0` for as long as there was nothing to draw on. There is now.
x86-build:
	@mkdir -p $(X86_BUILD)
	@# `TEST` is passed for the same reason `FULL` is: it moves `VARIANT`,
	@# which moves `UBUILD`, and leaving it out builds the userland into a
	@# directory the rule below does not name. It also carries `UTESTDEFS`
	@# to the userland, where `main.c` grows a chunk for the suite to
	@# dispatch to.
	@$(MAKE) --no-print-directory ARCH=x86_64 FULL=$(FULL) TEST=$(TEST) $(X86_BUILD)/kosmos.bin

# Who this is and what it was built from, for the other architecture. A
# second rule rather than a shared one for the same reason the font has one:
# $(GEN) moves with the image variant and this build has no variants. The
# platform string is the one line that differs, and it is the line that
# makes `uname` on this machine say something true.
$(X86_BUILD)/version.c: FORCE
	@mkdir -p $(dir $@)
	@printf '/* Generated by the Makefile. Do not edit. */\n\nconst char kosmos_name[] = "$(OS_NAME)";\nconst char kernel_name[] = "$(KERNEL_NAME)";\nconst char kosmos_version[] = "$(VERSION)";\nconst char kosmos_build[] = "$(KOSMOS_BUILD)";\nconst char kosmos_date[] = "$(KOSMOS_DATE)";\nconst char kosmos_platform[] = "$(PLATFORM)";\n' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

# The same font the ARM image carries, from the same BDF through the same
# converter. A second copy of the rule rather than a shared one, because
# $(GEN) moves with the image variant and this build has no variants.
$(X86_BUILD)/font_8x16.c: assets/fonts/spleen-8x16.bdf tools/bdf2c.py
	@mkdir -p $(dir $@)
	python3 tools/bdf2c.py $< font_8x16 $@

$(X86_BUILD)/kosmos.bin: $(X86_SRCS) boot/x86_64/kosmos.ld
	@mkdir -p $(X86_BUILD)
	$(CC) $(X86_FLAGS) -T boot/x86_64/kosmos.ld -Wl,--build-id=none \
	      -o $(X86_BUILD)/kosmos.elf $(X86_SRCS)
	$(OBJCOPY) -O binary $(X86_BUILD)/kosmos.elf $@
	@# What every source read, so a header alone changing rebuilds this.
	@$(CC) $(X86_FLAGS) -MM -MP -MT $@ $(filter %.c %.S,$(X86_SRCS)) > $@.d
	@echo "$@: $$(wc -c < $@) bytes"

# Accelerate it on a machine whose processor is the one being emulated.
#
# `make KVM=1 x86` on an x86-64 Linux host runs the guest on the host's own
# cores, which is the whole point of a second architecture: a PC running
# this natively rather than a PC emulating an ARM emulating this. It needs
# `-cpu host`, because KVM cannot pretend to be another processor - the same
# trade `make fast` makes on ARM, and it is why `make bench` stays on TCG.
#
# It does nothing on this Mac. Apple Silicon's hypervisor virtualises the
# processor it is, and that is not an x86-64.
ifeq ($(KVM),1)
X86_ACCEL := -accel kvm -cpu host
else
X86_ACCEL :=
endif

#
# The whole machine, the way `make qemu` gives it on ARM: a display, a
# keyboard, a pointer, a disk, a card and a sound device.
#
# `-vga none` is not optional. q35 adds a VGA adapter unless told not to,
# and QEMU scans out the *first* display device - so ramfb draws faithfully
# into memory nobody looks at, and a screenshot is 640x480 of black.
#
# `make SERIAL=1 x86` is the other one: no window, everything on the
# terminal, which is what `make serial` is on the other board.
# The same display the ARM target picks, so the window behaves the same way
# on this Mac and falls back to something sane elsewhere.
X86_DISPLAY := $(if $(filter Darwin,$(shell uname)),cocoa$(ZOOM_FLAG)$(FULLSCREEN_FLAG),gtk)

#
# **A tablet but no virtio keyboard.** q35's i8042 is the keyboard on this
# board - see `hal/pc/input_bind.c` - and leaving a virtio one attached
# would mean QEMU delivering keys to it instead, so the driver that has to
# work on a laptop would be the one never exercised. The tablet stays
# because the i8042's auxiliary port does not yet deliver.
#
# **And an NVMe drive rather than a virtio-blk one**, for that argument a
# second time. A ThinkPad's storage is NVMe or nothing - `docs/thinkpad.md`
# has the table, and there is no SATA option on the machine at all - so the
# driver that has to work there is the one this board exercises, and
# `hal/qemu-virt/` keeps virtio-blk so neither is orphaned.
#
# **And an Intel HDA controller rather than a virtio sound device**, for
# exactly that argument a third time. A laptop has an HDA controller and no
# virtio anything, `hal/pc/hda.c` is the driver that has to work there, and
# a machine configured with the easy device would leave it untested.
# `hal/qemu-virt/` still runs virtio-sound, so neither driver is orphaned.
#
X86_DEVICES := $(X86_DISPLAY) \
               -device virtio-tablet-pci \
               -device virtio-rng-pci \
               -device virtio-net-pci,netdev=n0 -netdev user,id=n0$(FORWARD) \
               -drive file=$(DISK),format=raw,if=none,id=d0 \
               -device nvme,drive=d0,serial=kosmos \
               -device ich9-intel-hda -device hda-output,audiodev=a0 \
               -audiodev coreaudio,id=a0

#
# A stick somebody can boot, which is the thing `-kernel` is not.
#
# **QEMU's `-kernel` is not a loader**, and every x86 boot in this project
# went through it for a long time. It reads the multiboot header, copies the
# image in and jumps - and it does not answer the video request, so the one
# path a laptop depends on entirely had never run. `docs/thinkpad.md` has what
# that hid: three faults, none of them a driver, all of them fatal and silent
# on a machine with no serial port.
#
# **The loader is Kosmos's own, and it was GRUB until 13 September 2026.**
# `boot/efi/` is `BOOTX64.EFI`: it claims the kernel's memory from the
# firmware by address, refuses on the screen what it cannot claim, keeps two
# copies of the kernel with a checksum a page, and checks and repairs them
# before and after ExitBootServices, then the kernel in its place. It hands
# over the Multiboot 2 tags GRUB did, so the kernel did not change for it.
# `docs/boot.md` has why: on the ThinkPad a GRUB-loaded image arrived with
# bytes already changed in some layouts, and under OVMF GRUB had been loading
# the kernel over memory the firmware keeps.
#
# Built with the same x86-64 compiler as the kernel, freestanding and
# position-independent, and linked as a PE32+ EFI application by the same
# binutils, whose `i386pep` emulation does it. No other tool is needed. It
# carries the kernel's boot-log font, because it draws its own lines: the
# ThinkPad's firmware console showed none of them.
#
EFI_LOADER := $(X86_BUILD)/BOOTX64.EFI
EFI_CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -mno-red-zone \
              -mgeneral-regs-only -fpie -fvisibility=hidden \
              -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -fno-tree-loop-distribute-patterns -O2 -Wall -Wextra -Werror

$(EFI_LOADER): boot/efi/loader.c boot/efi/mbi.c boot/efi/mbi.h boot/efi/trampoline.S \
               boot/efi/sums.c boot/efi/sums.h $(X86_BUILD)/font_8x16.c
	@mkdir -p $(X86_BUILD)/efi
	x86_64-elf-gcc $(EFI_CFLAGS) -c boot/efi/loader.c -o $(X86_BUILD)/efi/loader.o
	x86_64-elf-gcc $(EFI_CFLAGS) -c boot/efi/mbi.c -o $(X86_BUILD)/efi/mbi.o
	x86_64-elf-gcc $(EFI_CFLAGS) -c boot/efi/sums.c -o $(X86_BUILD)/efi/sums.o
	x86_64-elf-gcc $(EFI_CFLAGS) -c $(X86_BUILD)/font_8x16.c -o $(X86_BUILD)/efi/font.o
	x86_64-elf-gcc -c boot/efi/trampoline.S -o $(X86_BUILD)/efi/trampoline.o
	x86_64-elf-objcopy -R .comment $(X86_BUILD)/efi/loader.o
	x86_64-elf-objcopy -R .comment $(X86_BUILD)/efi/mbi.o
	x86_64-elf-objcopy -R .comment $(X86_BUILD)/efi/sums.o
	x86_64-elf-objcopy -R .comment $(X86_BUILD)/efi/font.o
	x86_64-elf-ld -m i386pep --subsystem 10 -e efi_main --image-base 0x10000000 \
	    -o $@ $(X86_BUILD)/efi/loader.o $(X86_BUILD)/efi/mbi.o $(X86_BUILD)/efi/sums.o \
	    $(X86_BUILD)/efi/font.o \
	    $(X86_BUILD)/efi/trampoline.o
	@ls -l $@

#
# And booting it the way the machine will: firmware, a loader, an image.
#
# `edk2-x86_64-code.fd` is OVMF, the same EDK II a ThinkPad's firmware is
# built from. `-vga std` gives it something to put a GOP on; a laptop has a
# panel and needs no equivalent.
#
OVMF_CODE := $(shell brew --prefix qemu 2>/dev/null)/share/qemu/edk2-x86_64-code.fd
OVMF_VARS := $(shell brew --prefix qemu 2>/dev/null)/share/qemu/edk2-i386-vars.fd

#
# **The closest thing to the ThinkPad that exists on this desk**, and the
# device list is a statement rather than a convenience.
#
# `make x86` is the whole system: ramfb, a virtio tablet, virtio networking,
# a virtio disk. Every one of those is QEMU's own and none of them is on a
# laptop. This machine has what the T14 has and nothing else - firmware that
# sets a mode, a loader, a panel, an i8042, and an Intel HDA controller - so
# anything that quietly depends on virtio fails here rather than on the
# hardware.
#
# What it therefore does *not* have, deliberately: a pointer, because the
# i8042's auxiliary port does not deliver yet and the TrackPoint is what it
# will be; a disk, because the T14's is NVMe and there is no driver; and a
# network card, because the I219 is not written either. The desktop comes up
# keyboard-only with `/Home` in memory, which is exactly what the first real
# boot will look like.
#
# 16 GB and 1920x1080 because that is the machine. Both were faults once:
# more than a gigabyte faulted at boot stage three until the boot page
# tables covered four, and the panel's 8 MB framebuffer is four times what
# OVMF offers by default.
#
PANEL ?= 1920x1080
PANEL_W := $(word 1,$(subst x, ,$(PANEL)))
PANEL_H := $(word 2,$(subst x, ,$(PANEL)))

#
# The stick, in one line.
#
# Builds the image and then hands it to `tools/mkusb.sh`, which is where
# every check lives: only external physical drives are offered, the answer
# is re-checked against that list, an internal drive and the running
# system's own disk are refused by two further routes, and the confirmation
# is the drive's name typed out rather than a `y`.
#
# A make target rather than a documented `dd` line, for the reason the
# script's own header gives: a `dd` copied out of a README is one keystroke
# from the disk this Mac boots from, and it gives no warning at all.
#
# **Named for its version, and a development build until Diego says
# otherwise.** Diego, 14 September 2026: "we always need 1 stable build we
# agree is stable to use", with `-stable` and `-development` in the names.
# So this writes `kosmos-usb-<version>-development.img`; a build he agrees is
# stable is renamed `-stable` by hand, the same bytes, never by a rebuild
# (`CLAUDE.md`, *How to work here*).
#
USB_IMG := $(X86_BUILD)/kosmos-usb-$(VERSION)-development.img

#
# The stick's image: a GPT, one EFI System Partition, Kosmos's loader and
# Kosmos.
#
# **One FAT partition, and the loader reads nothing else**, which is what a
# UEFI firmware is required to read. A `grub-mkrescue` ISO once booted perfectly under OVMF and
# dropped to `grub rescue>` on the ThinkPad, because its modules were only in
# an El Torito image the firmware did not choose. `tools/mkusb_image.py` has
# the rest.
#
# `KOSMOS_ARGS` is the kernel's command line - on a machine with no fw_cfg,
# the only way to give it a boot option. The loader reads it from
# `\boot\kosmos.cmdline`:
#
#     make usb KOSMOS_ARGS=opt/kosmos/smp=1
#
# **And `$(DISK)` on the stick as well, when it holds a filesystem** - the
# disk `make image FILES=...` makes, which is where Doom's WAD and Quake's
# pak live. The loader reads it into memory and hands it over, and Kosmos
# mounts it at boot, ahead of the machine's own drive; `hal/pc/blk_bind.c`
# says why. Only a disk `kfs.lua` can read goes on: `make qemu` leaves an
# empty one behind, and carrying that would hide a ThinkPad's NVMe `/Home`
# for nothing.
#
#     make image FILES="doom1.wad:/Home/doom1.wad pak0.pak:/Home/id1/pak0.pak"
#     make MEGA=1 usb
#
# **Or `/Home` in a partition of its own - the default since 19 September**
# (USB step 5f). A second partition beside the ESP, whose GUID the kernel is
# told: Kosmos opens `/Home` on the stick it started from through its own USB
# driver, and nothing is loaded into memory. **The ThinkPad has booted this
# layout four times** - `c70d9df`, `9af841c`, `895aa3f` and `b5ce4a4`, the
# last of which Diego used and called stable. It was a flag until 0.10.86 was
# built without it, as `CLAUDE.md` read, and came out the other layout; so it
# is the default, and `USB_HOME=disk` asks for the old one.
#
# **That `/Home` is made fresh for each stick, from a folder on this Mac**:
# `HOME_DIR`, `~/Kosmos/home`, at `STICK_HOME_MB`, 512 - Diego, 19 September:
# "from now on we need to make the drive image at least 512mb", "and i will
# be adding more images, videos, etc". `tools/homeimage.py` has the rest. The
# folder is his and never the repository's.
#
#     make MEGA=1 x86-usb-image
#
# **And the desktop, started by itself.** Diego, 15 September: "i think the
# desktop should start automatically yes upon booting". The shell starts what
# `opt/kosmos/boot` names exactly as though it were typed, and the prompt is
# back when it ends. `USB_BOOT=` makes a stick that stops at the prompt.
#
USB_BOOT ?= wm

# **And its command line on the network**, by Telnet (`roadmap.md`, remote;
# Diego, 29 September - "no key, local network only", and started by itself
# on a development stick): `telnetd` on port 23, before the desktop.
# `USB_TELNETD=` makes a stick without it. A stick made stable is the same
# bytes renamed, so it keeps it.
#
USB_TELNETD ?= 23

# **And the installed applications** (`docs/elf.md` step 5): Doom, Quake
# and the Super Nintendo, each its Lua and its image, stripped, into
# `/Home/Apps/<name>` - the build's, over anything the folder has there -
# and the data a game plays from the top of the folder copied in beside
# them, since the WAD and the pak are part of the game (Diego, 27
# September) and the folder is his to leave as it is. `tools/installed.py`
# is the list, for this and for `install-apps`.
#
#
# **The M700 booted over the network** (`roadmap.md`; Diego, 2 October: "lets
# try network boot"). `netboot` lays out what a boot server hands it - the
# loader, the kernel, the kernel's sums and the command line of the stick in
# the machine, whose `/Home` stays there (`STICK=`, the newest development
# stick by default) - and `netboot-serve` starts `dnsmasq` as a proxy to
# serve it, which asks for Diego's password (`tools/netboot-serve.sh`).
#
.PHONY: netboot netboot-serve

# `NETBOOT_ADD` - words after the stick's: the screen's keys and pointer lent
# to a viewer from boot, for the build, boot and test loop (Diego, 3
# October). `make netboot NETBOOT_ADD=` serves the stick's words alone.
NETBOOT_ADD ?= opt/kosmos/vnc=control

netboot: x86-build $(EFI_LOADER)
	python3 tools/netboot.py $(X86_BUILD)/kosmos.bin --loader $(EFI_LOADER) $(if $(STICK),--stick $(STICK)) $(if $(NETBOOT_ADD),--add "$(NETBOOT_ADD)") --out build/netboot

netboot-serve:
	bash tools/netboot-serve.sh build/netboot

# **The M700's suite** (`roadmap.md`, a suite on the M700; Diego, 3
# October: "a subset of tests on the m700 mostly for performance tests and
# real Hardware"): this build served, the M700 restarted into it over
# Telnet, and `tools/run_m700.py` - its hardware checked, its numbers kept
# beside the last run's in build/m700, every application opened and
# pictured, all of it shown on the M700's screen as it goes. Needs the boot
# server running (`netboot-serve`) and the M700 on the network.
.PHONY: m700
m700: netboot
	python3 tools/run_m700.py --restart $(M700)

USB_HOME      ?= partition
HOME_DIR      ?= $(HOME)/Kosmos/home
STICK_HOME_MB ?= 512
STICK_HOME    := build/stick-home.img

x86-usb-image: x86-build $(HOSTDIR)/lua $(EFI_LOADER)
	@if [ "$(USB_HOME)" = partition ]; then \
	    $(MAKE) --no-print-directory ARCH=x86_64 apps && \
	    python3 tools/homeimage.py "$(HOME_DIR)" $(STICK_HOME) $(STICK_HOME_MB) \
	        $$(python3 tools/installed.py x86_64 "$(HOME_DIR)") && \
	    echo "$(HOME_DIR) goes on the stick as /Home, $(STICK_HOME_MB) MB, in a partition of its own" && \
	    python3 tools/mkusb_image.py $(X86_BUILD)/kosmos.bin $(USB_IMG) --loader $(EFI_LOADER) --home $(STICK_HOME) $(if $(USB_BOOT),opt/kosmos/boot=$(USB_BOOT)) $(if $(USB_TELNETD),opt/kosmos/telnetd=$(USB_TELNETD)) $(KOSMOS_ARGS); \
	elif [ -f $(DISK) ] && $(HOSTDIR)/lua tools/kfs.lua ls $(DISK) >/dev/null 2>&1; then \
	    echo "$(DISK) goes on the stick too: the loader reads it and Kosmos mounts it"; \
	    python3 tools/mkusb_image.py $(X86_BUILD)/kosmos.bin $(USB_IMG) --loader $(EFI_LOADER) --disk $(DISK) $(if $(USB_BOOT),opt/kosmos/boot=$(USB_BOOT)) $(KOSMOS_ARGS); \
	else \
	    python3 tools/mkusb_image.py $(X86_BUILD)/kosmos.bin $(USB_IMG) --loader $(EFI_LOADER) $(if $(USB_BOOT),opt/kosmos/boot=$(USB_BOOT)) $(KOSMOS_ARGS); \
	fi
	@# **Its symbols, beside it** (`tools/profile_report.py`): a profile taken
	@# on the machine names its addresses from exactly these, and a rebuild
	@# after the stick was made would name them wrongly. The system's image,
	@# the kernel and the installed applications, unstripped.
	@S=$(USB_IMG:.img=.symbols); rm -rf $$S && mkdir -p $$S && \
	cp $(X86_BUILD)/kosmos.elf $$S/kernel.elf && \
	cp $$($(MAKE) --no-print-directory -s ARCH=x86_64 FULL=$(FULL) ubuild-path)/init.elf $$S/init.elf && \
	for a in build/user-x86_64/apps/*.elf; do [ -f "$$a" ] && cp "$$a" $$S/; done; \
	echo "its symbols, for a profile taken on it: $$S"

# **The M700 from here, by Telnet** (`tools/kosmos_telnet.py`): a profile
# taken there, fetched and read, in one command. `HOST` is its address -
# `python3 tools/kosmos_telnet.py find` says which - and `SECONDS` how long.
.PHONY: remote-profile
remote-profile:
	@test -n "$(HOST)" || { echo "HOST=<address>; python3 tools/kosmos_telnet.py find says which"; exit 1; }
	@mkdir -p build/stick-profiles
	python3 tools/kosmos_telnet.py $(HOST) run "profile $(if $(SECONDS),$(SECONDS),30) remote"
	python3 tools/kosmos_telnet.py $(HOST) get /Home/profiles/remote.kprof build/stick-profiles/remote.kprof
	python3 tools/profile_report.py build/stick-profiles/remote.kprof

# Where this build's userland is, for a rule that has to name its `init.elf`.
.PHONY: ubuild-path
ubuild-path:
	@echo $(UBUILD)

# **A profile, named** (`roadmap.md`, the App Inspector's first step):
# `profile` on the machine, `make stick-log FILE=/Home/profiles/` here, and
# this reads the newest - or `KPROF=path` - against the symbols that ran,
# into `build/profiles/<name>.html` and the terminal.
.PHONY: profile-report
profile-report:
	@python3 tools/profile_report.py $(KPROF)

usb: x86-usb-image
	@bash tools/mkusb.sh $(USB_IMG)

# A file from `/Home` on a Kosmos stick, onto this Mac: `diagnose` on the
# machine, then `make stick-log` here, which puts `/Home/diagnose.txt` in
# `build/stick-diagnose.txt` - `FILE=/Home/log.txt` for what `log save` wrote,
# or any other file, and `FILE=/Home/acpi/` for a whole folder, into
# `build/stick-acpi/`. It reads the stick and never writes it; macOS asks for a
# password, because only root may read a whole disk (`tools/sticklog.sh`).
stick-log: $(HOSTDIR)/lua
	@bash tools/sticklog.sh $(if $(FILE),$(FILE),/Home/diagnose.txt) $(HOSTDIR)/lua

x86-uefi: x86-usb-image
	@cp $(OVMF_VARS) $(X86_BUILD)/ovmf-vars.fd
	qemu-system-x86_64 -M q35 -m $(if $(MEM),$(MEM),16G) -no-reboot \
	  -drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,unit=1,file=$(X86_BUILD)/ovmf-vars.fd \
	  -vga none -device VGA,xres=$(PANEL_W),yres=$(PANEL_H) \
	  -device ich9-intel-hda -device hda-output,audiodev=a0 \
	  -audiodev coreaudio,id=a0 \
	  $(if $(SERIAL),-display none -serial stdio,-display $(X86_DISPLAY) -serial mon:stdio) \
	  -device qemu-xhci,id=xhci \
	  -drive if=none,id=stick,format=raw,file=$(USB_IMG) \
	  -device usb-storage,bus=xhci.0,drive=stick

x86: x86-build $(DISK)
	qemu-system-x86_64 -M q35 -m 512M -no-reboot $(X86_ACCEL) -vga none \
	  $(if $(SERIAL),-nographic,-display $(X86_DISPLAY) -serial mon:stdio) \
	  $(X86_DEVICES) \
	  $(if $(BOOT),-fw_cfg name=opt/kosmos/boot$(comma)string=$(BOOT)) \
	  $(TELNETARG) \
	  -kernel $(X86_BUILD)/kosmos.bin

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
# The host half of the tests: every check that boots nothing. Seconds, and
# run by `tools/gate.py` beside the machines rather than before them.
host-check: $(HOSTDIR)/test_keyfile $(HOSTDIR)/test_ntlmname $(HOSTDIR)/libsmb2/smb2-ls-async $(HOSTDIR)/libsmb2/smb2-cat-async $(HOSTDIR)/test_ramstore $(HOSTDIR)/test_clock $(HOSTDIR)/test_crypto $(HOSTDIR)/test_crypto_x86 $(HOSTDIR)/test_smbsign $(HOSTDIR)/test_smbsign_x86 $(HOSTDIR)/test_e1000decode $(HOSTDIR)/lua $(HOSTDIR)/test_diskcache $(HOSTDIR)/test_audioring $(HOSTDIR)/test_loaderfb $(HOSTDIR)/test_efiboot $(HOSTDIR)/test_pmmplace $(HOSTDIR)/test_apicdecode $(HOSTDIR)/test_i8042drain $(HOSTDIR)/test_smbiosdecode $(HOSTDIR)/test_usbdecode $(HOSTDIR)/test_uvcdecode $(HOSTDIR)/test_mididecode $(HOSTDIR)/test_depth $(HOSTDIR)/test_backlightdecode $(HOSTDIR)/test_s5decode $(HOSTDIR)/test_batterydecode $(HOSTDIR)/test_paddecode $(HOSTDIR)/test_storagedecode $(HOSTDIR)/test_fatdecode $(HOSTDIR)/fatls $(HOSTDIR)/test_drivesdecode $(HOSTDIR)/test_scan $(HOSTDIR)/test_string $(HOSTDIR)/test_string_kernel $(HOSTDIR)/test_imagesum $(HOSTDIR)/test_elfimage $(HOSTDIR)/test_snesblit $(HOSTDIR)/test_shadow $(HOSTDIR)/test_yuv $(HOSTDIR)/test_yuv_x86 $(HOSTDIR)/test_pack $(HOSTDIR)/test_pack_x86 $(HOSTDIR)/test_raster $(HOSTDIR)/test_raster_x86 $(HOSTDIR)/test_rows $(HOSTDIR)/test_rows_x86 $(HOSTDIR)/test_gunzip $(HOSTDIR)/test_k3d $(HOSTDIR)/test_fbx $(HOSTDIR)/test_trace $(HOSTDIR)/test_record $(HOSTDIR)/test_time $(HOSTDIR)/test_h264 $(HOSTDIR)/test_aac $(HOSTDIR)/test_synth
	@# No C outside `kosmos_lua_open` puts a name into every Lua state.
	@# Doom's, Quake's and the Super Nintendo's kits did, and a global with
	@# a program's name hides the program from the prompt: `snes --scale 3`
	@# printed a table. This reads the source, so it sees every variant -
	@# `MEGA=1` included, which the display harness never boots. It also
	@# found the one the move itself left behind. design.md §6.
	@# Comment lines are skipped: the first version matched the comment in
	@# `sys_user.c` that says this search exists, and failed on it.
	@if grep -rn --include='*.c' --include='*.h' --exclude-dir=upstream \
	    'lua_setglobal\|lua_pushglobaltable\|LUA_RIDX_GLOBALS' \
	    user runtime lua/kosmos | grep -v '^user/init/lua_glue.c:' \
	    | grep -v '^[^:]*:[0-9]*: *\(/\*\|\*\|//\)'; then \
	    echo "FAIL: C sets a Lua global outside kosmos_lua_open. A kit is"; \
	    echo "      reached with use(\"/kits/<name>\"); see design.md §6."; \
	    exit 1; \
	fi
	@echo "cglobals: no C sets a Lua global outside kosmos_lua_open"
	@# **Every setting through the settings kit** (Diego, 4 October: "So
	@# apps use that kit instead of inventing their own way"). Nothing in an
	@# application, a program or a library names the settings folder but
	@# `prefs.lua`, which reads and writes it as text for all of them -
	@# `prefs.open(name, defaults)`, `prefs.folder(name)`. A comment may say it.
	@if grep -rn '/Home/Preferences' user/bin user/lib user/installed \
	    | grep -v '^user/lib/prefs.lua:' \
	    | grep -v '^[^:]*:[0-9]*: *--'; then \
	    echo "FAIL: a program names /Home/Preferences itself. A setting goes"; \
	    echo "      through use(\"/Kosmos/Libraries/prefs.lua\"); see design.md 8.3e."; \
	    exit 1; \
	fi
	@echo "prefs: every setting through the settings kit"
	@# The format, `kfs.c`, on this machine before anything is booted - the
	@# fastest of these and the one that fails first when the disk layout is
	@# wrong - plain and through the disk server's cache (`docs/diskfs.md`).
	$(HOSTDIR)/lua tools/test_kfs.lua
	KFS_CACHE=1 $(HOSTDIR)/lua tools/test_kfs.lua
	@# And the host's disk tool through every command, twice: the same
	@# images and the same words both times (step 2; held to the Lua's until
	@# step 4 took the Lua out).
	python3 tools/test_kfs_tool.py
	@# The disk server's reader of `sys.pack`'s tables, held to the
	@# serialiser itself (step 3).
	$(HOSTDIR)/lua tools/test_packflat.lua
	@# /Temporary's store, which grows to a ceiling, and the one door to a
	@# region, whose windows land each at its own offset (5 October 2026).
	$(HOSTDIR)/test_ramstore
	$(HOSTDIR)/lua tools/test_regions.lua
	@# The paths, text and addresses every program shares, one copy of each
	@# since the second half of the review's second copies (5 October 2026):
	@# a path made whole and a folder with its parents; thousands, a bar and
	@# lines; an address both ways and a network program's scaffolding.
	$(HOSTDIR)/lua tools/test_files.lua
	$(HOSTDIR)/lua tools/test_text.lua
	$(HOSTDIR)/lua tools/test_ipv4.lua
	@# libsmb2, as vendored, against Samba run as the user on this Mac, at
	@# every dialect sharing will speak (`docs/sharing.md` N0); a skip that
	@# says so where Homebrew's Samba is not installed.
	python3 tools/test_smbpeer.py $(HOSTDIR)/libsmb2
	@# And what a server calls itself, out of NTLM's challenge: its NetBIOS
	@# computer name, its DNS name's first label, and never a piece of its
	@# address, which is what macOS's server named itself (18.415).
	$(HOSTDIR)/test_ntlmname
	$(HOSTDIR)/test_diskcache
	@# And what an audio file says about itself - ID3v2, ID3v1 and a WAV's
	@# INFO - read through the same tags.lua Music uses, on this machine.
	$(HOSTDIR)/lua tools/test_tags.lua
	$(HOSTDIR)/lua tools/test_procshare.lua
	@# The kit's keys: every sequence the board makes read back whole, as
	@# its key and its modifiers, and nothing typed that was not (6n, step 0).
	$(HOSTDIR)/lua tools/test_keys.lua
	@# The browser's cache: HTTP's dates, what a reply says of keeping it,
	@# and the store over an fs in memory (`roadmap.md` 6zz k).
	$(HOSTDIR)/lua tools/test_httpcache.lua
	@# http.lua's refresh: a <meta http-equiv="refresh"> read as HTML reads it.
	$(HOSTDIR)/lua tools/test_http.lua
	@# The browser's favorites as files: names from titles, the order they
	@# were starred, folders, and removed wherever kept (`roadmap.md` 6zz d3).
	$(HOSTDIR)/lua tools/test_favorites.lua
	@# And its history on the disk, a file a day: recorded once a day each,
	@# lately across days, searched, and let go after its days (6zz d4).
	$(HOSTDIR)/lua tools/test_history.lua
	@# And its settings: read back as written, false included, and what is
	@# not one of the choices the default (6zz d5).
	$(HOSTDIR)/lua tools/test_browserprefs.lua
	@# A menu longer than the screen grouped into submenus that fit, every
	@# item reached once and in order (a select's options, 6zz j6).
	$(HOSTDIR)/lua tools/test_longmenu.lua
	@# The IDE's editor: the text it edits, every edit undoable, and Lua
	@# coloured a line at a time with what carries across lines (6n, step 1).
	$(HOSTDIR)/lua tools/test_textbuf.lua
	@# And Text Editor's page: where a line breaks into rows, and the
	@# caret's steps whole characters (`roadmap.md` 6zs).
	$(HOSTDIR)/lua tools/test_docview.lua
	@# And the Markdown it styles while it is written (6zs step 2).
	$(HOSTDIR)/lua tools/test_mdstyle.lua
	@# And the Synth Kit's engine, heard: Groove's sound (6zh).
	$(HOSTDIR)/test_synth tools/test_synth.lua
	@# And Groove's Lua held to it: the sound bank, the demos, projects.
	$(HOSTDIR)/test_synth tools/test_groove.lua
	$(HOSTDIR)/lua tools/test_lualex.lua
	@# And its checking: Lua's own parser, and the vendored luacheck loaded
	@# as the machine loads it (6n, step 4).
	$(HOSTDIR)/lua tools/test_lint.lua
	@# And what a library offers, read from its source - the IDE's
	@# suggestions and its check of a name a library has not got (6n, step 5).
	$(HOSTDIR)/lua tools/test_libdoc.lua
	@# And an MP4's index, for the video player (roadmap 4e).
	$(HOSTDIR)/lua tools/test_mp4.lua
	@# JSON, and Cafesa3D's scenes read out of glTF: the samples written
	@# here and read back, their two descriptions held to each other.
	$(HOSTDIR)/lua tools/test_json.lua
	python3 tools/cafesa3d_samples.py $(HOSTDIR)/scenes
	$(HOSTDIR)/lua tools/test_scenefile.lua $(HOSTDIR)/scenes
	@# And Cafesa3D's tutorial held to Cafesa3D: every page and picture
	@# reachable from the first page, every picture one the browser can
	@# decode at the size its page gives it, only what the browser draws,
	@# and every control a page names in bold one the application has.
	$(HOSTDIR)/lua tools/test_tutorial.lua $(TUTORIAL_FILES)
	@# The WAV header walker, likewise: pure Lua over a reader, so the
	@# awkward headers can be built by hand rather than found in the wild.
	$(HOSTDIR)/lua tools/test_wav.lua
	$(HOSTDIR)/lua tools/test_iconlayout.lua
	@# The themes that ship, colours and faces (roadmap 5s): each read with
	@# no complaint, every face one the image embeds - which is why it is
	@# handed FONT_FILES - and Plex as docs/plex.html lists it.
	$(HOSTDIR)/lua tools/test_theme.lua $(FONT_FILES)
	@# The dock's arithmetic (roadmap, a dock at the bottom): its cells for
	@# what is pinned and what runs, where each goes, what a press does.
	$(HOSTDIR)/lua tools/test_dock.lua
	$(HOSTDIR)/lua tools/test_launchgrid.lua
	@# A table as text, values only: what a settings file is.
	$(HOSTDIR)/lua tools/test_tabletext.lua
	@# The settings kit every application keeps its settings with.
	$(HOSTDIR)/lua tools/test_prefs.lua
	@# Kosmos Write's document (docs/write.md, W1): checked once is
	@# checked, out as text and back the same, and what a hundred pages cost.
	$(HOSTDIR)/lua tools/test_writedoc.lua
	@# Its pages (W2): lines broken and placed, pages filled without a
	@# line left alone, and which face a look is set in.
	$(HOSTDIR)/lua tools/test_pageset.lua
	@# Its hyphenation (W4e) and its DOCX (W6).
	$(HOSTDIR)/lua tools/test_hyphen.lua
	$(HOSTDIR)/lua tools/test_docx.lua
	@# The Deskbar's menu, read off a folder tree - what counts as an item,
	@# what order things come in, how deep a folder may go. The store it
	@# reads through is a table here, which is the whole reason the reading
	@# lives in `/lib` and not inside the Deskbar.
	$(HOSTDIR)/lua tools/test_deskbarmenu.lua
	$(HOSTDIR)/lua tools/test_places.lua
	@# Shares as a window shows them (docs/sharing.md N6): the Network
	@# group, an address as typed, the status line, the gone-away banner.
	$(HOSTDIR)/lua tools/test_netshares.lua
	@# What a right click offers on each kind of thing, and how Info counts
	@# a folder a slice at a time (`roadmap.md` 6za).
	$(HOSTDIR)/lua tools/test_filemenu.lua
	$(HOSTDIR)/lua tools/test_tally.lua
	@# The local time of a moment: the Deskbar's clock, and Info's Modified.
	$(HOSTDIR)/lua tools/test_clock.lua
	@# What kind of thing a process is, for Processes: a driver by its
	@# device authority, a server by init starting it, the rest by /bin.
	$(HOSTDIR)/lua tools/test_prockind.lua
	@# And what a file *is*: the attribute first, the extension second.
	$(HOSTDIR)/lua tools/test_filetypes.lua $(wildcard user/bin/apps/*.lua user/bin/apps/*/*.lua user/bin/programs/*.lua user/installed/*/*.lua)
	@# And the audio ring's position arithmetic. It models the client, the
	@# server and the device queue, because the thing worth asserting is
	@# that a period taken out of the ring is not yet a period heard.
	$(HOSTDIR)/test_audioring
	@# And the scanf family's scanner, which reads Quake's demos out of a
	@# pak: `%f` writes a float, and nothing past a length is read.
	$(HOSTDIR)/test_scan
	@# And the libc's memcpy, memset, memcmp and memmove, against a byte
	@# loop at every alignment, as a process links them and as the kernel
	@# does - the two copies take different paths (`testing.md` 18.329).
	$(HOSTDIR)/test_string
	$(HOSTDIR)/test_string_kernel
	@#
	@# And where the page bitmap goes, which is the same shape of test one
	@# layer down: arithmetic with an awkward case that firmware produces
	@# and QEMU does not.
	$(HOSTDIR)/test_pmmplace
	$(HOSTDIR)/test_loaderfb
	$(HOSTDIR)/test_efiboot
	$(HOSTDIR)/test_apicdecode
	$(HOSTDIR)/test_i8042drain
	$(HOSTDIR)/test_clock
	$(HOSTDIR)/test_crypto
	$(HOSTDIR)/test_crypto_x86
	@# And the keyring's file, sealed whole (`docs/keyring.md`, K2).
	$(HOSTDIR)/test_keyfile
	@# And who a launcher hands the keyring's door to (K4).
	$(HOSTDIR)/lua tools/test_keyring_grant.lua user/init/init.lua
	$(HOSTDIR)/test_smbsign
	$(HOSTDIR)/test_smbsign_x86
	$(HOSTDIR)/test_snesblit
	$(HOSTDIR)/test_shadow
	$(HOSTDIR)/test_yuv
	$(HOSTDIR)/test_k3d
	@# Its FBX reader, against the programs' own files (`roadmap.md` 4l).
	python3 tools/fetch_conformance.py fbx
	$(HOSTDIR)/test_fbx
	$(HOSTDIR)/test_trace
	$(HOSTDIR)/test_yuv_x86
	$(HOSTDIR)/test_pack
	$(HOSTDIR)/test_pack_x86
	$(HOSTDIR)/test_raster
	$(HOSTDIR)/test_raster_x86
	$(HOSTDIR)/test_rows
	$(HOSTDIR)/test_rows_x86
	$(HOSTDIR)/test_gunzip
	@# Broken-down time, which FFmpeg's option parser and logger reach.
	$(HOSTDIR)/test_time
	@# And the H.264 Kit's decoder: eighteen conformance streams, every
	@# picture's checksum against FFmpeg's own (`roadmap.md` 4e).
	python3 tools/fetch_conformance.py h264
	$(HOSTDIR)/test_h264
	@# And the AAC Kit's: twelve streams against FFmpeg's PCM, every
	@# channel within a step, and six channels mixed to two.
	python3 tools/fetch_conformance.py aac
	$(HOSTDIR)/test_aac
	@# The camera's recording: H.264 in an MP4, decoded by FFmpeg, and read
	@# by the video player's own MP4 reader (`roadmap.md` 6d 8f).
	$(HOSTDIR)/test_record
	$(HOSTDIR)/lua tools/test_record_mp4.lua
	@# And the script that travels beside a released image, held to the
	@# command line it gives QEMU (a stand-in QEMU prints it).
	sh tools/test_runscript.sh
	$(HOSTDIR)/test_smbiosdecode
	$(HOSTDIR)/test_usbdecode
	$(HOSTDIR)/test_uvcdecode
	$(HOSTDIR)/test_mididecode
	$(HOSTDIR)/test_depth
	$(HOSTDIR)/test_backlightdecode
	$(HOSTDIR)/test_s5decode
	$(HOSTDIR)/test_batterydecode
	$(HOSTDIR)/test_paddecode
	$(HOSTDIR)/test_storagedecode
	@# And FAT, the drives' filesystem, read from bytes the specification
	@# describes and then from volumes mtools made.
	$(HOSTDIR)/test_fatdecode
	python3 tools/test_fat.py $(HOSTDIR)/fatls
	@# And where a drive's volumes are, which is the step above reading one:
	@# both partition tables, kfs's superblock read from a volume mkfs wrote,
	@# FAT32's free-cluster hint, and what an unlabelled or repeated name is
	@# called (USB step 6b).
	$(HOSTDIR)/test_drivesdecode
	@# `make stick-log` without the stick: an image built as `mkusb_image.py`
	@# builds one, its Kosmos partition copied out by `sticklog.py`, and a log
	@# taken from the copy by `kfs.lua`.
	python3 tools/test_sticklog.py $(HOSTDIR)/lua
	@# And the stick's /Home, made from a folder on this Mac.
	python3 tools/test_homeimage.py
	@# And a released stick fetched and checked on another Mac: getstick.sh
	@# against a release on this disk, its mkusb.sh one that writes nothing.
	python3 tools/test_getstick.py
	@# And a window's own text size - the Terminal's and Log View's View
	@# menu - which is arithmetic over a settings file and a face name.
	$(HOSTDIR)/lua tools/test_textsize.lua
	@# And how big the icons are where a grid of them is drawn: the three
	@# Haiku exports and nothing else, kept per place in one file.
	$(HOSTDIR)/lua tools/test_iconsize.lua
	@# And the list Preferences draws itself from: every setting in a
	@# category that exists, every default on its own list of choices, and
	@# a write that keeps what another program put in the same file.
	$(HOSTDIR)/lua tools/test_settings.lua
	@# And every method an application calls on its window: Lua resolves one
	@# at the call, so a name the kit does not have is a control that ends
	@# the program when somebody presses it, and nothing else notices.
	python3 tools/test_winmethods.py
	@# And an Intel Ethernet controller's registers and descriptors: the
	@# link, the MAC, and the errors a frame can arrive with.
	$(HOSTDIR)/test_e1000decode
	@# And the tools' temporary files: made only through scratch.py, and gone
	@# when the tool is (19 GB were left behind before, and filled the disk).
	python3 tools/test_scratch.py
	@# And the userland image's canary, over a blob the real script
	@# generated during this build - because its two halves are Python and
	@# C and nothing at run time can notice them disagreeing.
	$(HOSTDIR)/test_imagesum
	@if [ -f $(UBUILD)/init.elf ] && [ -f $(UBUILD)/init.bin ]; then \
	    $(HOSTDIR)/test_elfimage $(UBUILD)/init.elf $(UBUILD)/init.bin; \
	else \
	    $(HOSTDIR)/test_elfimage; \
	fi
	@# And LICENSE, read the way the About window reads it: every line of
	@# it, and every vendored tree named in it - so a library added without
	@# an entry fails here, by name.
	$(HOSTDIR)/lua tools/test_licences.lua LICENSE $(wildcard runtime/upstream/*/) lua/upstream/ \
	        assets/hyphenation/
	@# And the line icons: every one the list names rendered at every size,
	@# white with its coverage, nothing stray, and every name the Lua uses
	@# among them - `tools/lineicons.py` is run by hand, so nothing else
	@# would notice an icon named and never drawn.
	python3 tools/test_lineicons.py
	@# And every syscall's arguments: what the kernel reads from each case,
	@# against what userland passes. A wrapper that passes fewer hands the
	@# kernel whatever the register last held - SYS_MEM_CREATE's flags did.
	$(HOSTDIR)/lua tools/test_syscall_args.lua kernel/syscall.c $(sort $(wildcard user/*/*.c user/*/*.h lua/kosmos/*.c runtime/libc/*.c))
	@# And `run_uefi.py` where it cannot boot anything: no OVMF is a skip
	@# that says so, and a boot that gives no picture fails. Nothing boots.
	python3 tools/test_run_uefi.py

# Every image the suites boot, built before any of them starts, each with
# `-j`: the plain one, the test image, and on x86 the same two plus the
# UEFI sticks `run_uefi.py` and `test_stickcheck.py` read. Building them up
# front is what lets the suites run side by side without two of them
# building the same thing at once.
J ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

#
# **The applications' images against the test userland** (`roadmap.md`
# 6zp): the loader's suite needs an image of each, and linking them against
# a userland built for them alone was two more userlands to compile every
# time a library changed - 100 seconds of images where 56 had been. The
# test userland is built here anyway; an image linked against it carries
# the suites' roles as well, which nothing asks it for. A stick and
# `install-apps` still link against the lean one.
#
gate-images: $(TARGET) $(HOSTDIR)/lua
	@$(MAKE) --no-print-directory -j$(J) TEST=1 build/test/kosmos.elf
	@$(MAKE) --no-print-directory -j$(J) TEST=1 apps
	@if command -v x86_64-elf-gcc >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory -j$(J) x86-build >/dev/null && \
	    $(MAKE) --no-print-directory -j$(J) TEST=1 x86-build >/dev/null && \
	    $(MAKE) --no-print-directory -j$(J) ARCH=x86_64 TEST=1 apps >/dev/null && \
	    $(MAKE) --no-print-directory $(EFI_LOADER) >/dev/null && \
	    $(HOSTDIR)/lua tools/kfs.lua create build/x86_64/uefi-disk.img 4 >/dev/null && \
	    python3 tools/mkusb_image.py build/x86_64/kosmos.bin build/x86_64/kosmos-uefi.img --loader $(EFI_LOADER) --disk build/x86_64/uefi-disk.img >/dev/null && \
	    python3 tools/mkusb_image.py build/x86_64/kosmos.bin build/x86_64/kosmos-uefi-home.img --loader $(EFI_LOADER) --home build/x86_64/uefi-disk.img >/dev/null && \
	    python3 tools/netboot.py build/x86_64/kosmos.bin --loader $(EFI_LOADER) --stick build/x86_64/kosmos-uefi-home.img --out build/x86_64/netboot-test >/dev/null && \
	    python3 tools/mkusb_image.py build/x86_64/kosmos.bin build/x86_64/kosmos-uefi-video.img --loader $(EFI_LOADER) video=1024x768 >/dev/null && \
	    head -c 65536 /dev/zero > build/x86_64/uefi-zeros.bin && \
	    python3 tools/mkusb_image.py build/x86_64/uefi-zeros.bin build/x86_64/kosmos-refusal.img --loader $(EFI_LOADER) >/dev/null; \
	else \
	    echo "SKIP: the x86-64 images, because x86_64-elf-gcc is not installed."; \
	fi

# **The tests, in five to ten minutes** - Diego, 18 September 2026: "i dont
# want 40 minutes tests any more, 5 to 10 minutes max from now on". The same
# checks as ever, run side by side by `tools/gate.py`, which says why each
# suite is there and how long it took. `J=` sets how many at once.
# `make test ONLY=arm-browser-1,x86-browser-1`: the suites for what changed,
# by name (CLAUDE.md, "Test what changed"); with no ONLY, all of them.
test:
	@python3 tools/gate.py --jobs $(J) $(if $(ONLY),--only $(ONLY),)

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
#
# And on both boards, because the display is where the two differ most:
# ramfb is found through fw_cfg either way, but the keyboard and the
# pointer behind it are a device-tree window on one machine and a walk of
# the PCI bus on the other. The sixty-two checks are the same sixty-two,
# which is the point - a second architecture that passes a *different*
# suite has not been shown to work, it has been shown to be different.
#
# Skipped rather than failed where the cross compiler is not installed, and
# said out loud, for the reason `test` gives at greater length.
screenshot: $(TARGET)
	python3 tools/run_screenshot.py $(TARGET) --png build/screenshot.png
	@if command -v x86_64-elf-gcc >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory x86-build >/dev/null && \
	    python3 tools/run_screenshot.py $(X86_BUILD)/kosmos.elf; \
	else \
	    echo "SKIP: the x86-64 display, because x86_64-elf-gcc is not installed."; \
	fi

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
#
# The web libraries, running rather than merely linked.
#
# Its own target rather than part of `make test`, because `WEB=1` is an
# optional variant and the images the suites build carry none of it.
#
web: $(HOSTDIR)/lua
	@$(MAKE) --no-print-directory WEB=1 $(TARGET)
	python3 tools/run_web.py $(TARGET)

#
# The browser, with a page in it.
#
#   make browser
#   make browser PAGE=/somewhere/else.html
#
# `make web` proves the libraries parse, which says nothing about whether
# anything is drawn - and drawing is the whole difference between a parser
# and a browser. This boots the same image, serves a page from this computer
# over slirp so no packet leaves the machine, points the browser at it, and
# looks at the screen. The picture lands in `build/browser.png` either way,
# because a failure is exactly when you want to see it.
#
browser: $(HOSTDIR)/lua
	@$(MAKE) --no-print-directory WEB=1 $(TARGET)
	python3 tools/run_browser.py $(TARGET) --out build/browser.png \
	  $(if $(PAGE),--page $(PAGE),)

# Cafesa3D's tutorial's pictures, taken again: the image booted, Cafesa3D
# driven, the car of chapters 6 and 7 built by its steps and rendered, and
# every picture written into `docs/cafesa3d-tutorial/`. Twenty minutes and
# more under TCG, most of it the render, which is why it is not a suite;
# run it when Cafesa3D's look changes, and look at the pictures before
# committing them. It fails, by name, on a step the pages give that cannot
# be followed.
tutorial-shots: $(HOSTDIR)/lua
	@$(MAKE) --no-print-directory $(TARGET)
	python3 tools/cafesa3d_tutorial_shots.py $(TARGET) docs/cafesa3d-tutorial

# Everything in one image, linked. `MEGA=1` is where Doom and Quake meet in
# a single link - where a name both of them define shows up - and the image
# that comes closest to its heap, which `user/user.ld` asserts. Built rather
# than booted: the checks before it boot their own images, and `shot` builds
# the ordinary one again after it.
.PHONY: mega
mega:
	@$(MAKE) --no-print-directory MEGA=1 $(TARGET)

#
# **The stages in order, each compiled in parallel.**
#
# They were prerequisites - `prepush: test screenshot litexl-check mega
# shot`, when Lite XL was in the tree - which is correct and slow. Make
# builds a prerequisite's own dependencies one at a time unless told
# otherwise, so every object in every variant was compiled serially on a
# machine with ten cores.
#
# `-j` on the whole thing is not the answer: the stages would run at
# once, which means several QEMUs racing for the same build directories and
# a screenshot taken of whichever image happened to be linked last. The
# stages are *ordered* on purpose.
#
# So the ordering stays here in the recipe and the parallelism goes inside
# each stage, where it is safe: one stage at a time, its compiles spread
# across `J` jobs. `make J=4 prepush` for a quieter machine.
#
prepush:
	@$(MAKE) --no-print-directory -j$(J) test
	@$(MAKE) --no-print-directory -j$(J) mega
	@$(MAKE) --no-print-directory -j$(J) shot
	@echo
	@echo "ready to push: suites green and $(SHOTDIR) has today's picture."


.PHONY: shot web browser tutorial-shots
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

# **Where a benchmark's instructions go**, by kernel function and exactly
# (`testing.md` 18.343): the benchmark image under a QEMU plugin that counts
# every instruction, summed by the symbol table. The plugin is built against
# the header the installed QEMU ships, so its API is the binary's.
QEMU_PLUGIN_INCLUDE := $(abspath $(dir $(shell command -v qemu-system-aarch64))../include)

$(HOSTDIR)/qemu_pcprof.dylib: tools/qemu_pcprof.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -shared -fPIC -undefined dynamic_lookup \
	        -I$(QEMU_PLUGIN_INCLUDE) $(shell pkg-config --cflags glib-2.0) -o $@ $<

.PHONY: bench-profile
bench-profile: $(HOSTDIR)/qemu_pcprof.dylib
	@$(MAKE) --no-print-directory BENCH=1 build/bench/kosmos.elf
	python3 tools/prof_bench.py build/bench/kosmos.elf ipc --kinds
	python3 tools/prof_bench.py build/bench/kosmos.elf switch --kinds

# Records the current numbers as the new baseline. By hand, never
# automatically: testing.md 18.6 is explicit that a baseline which updates
# itself detects nothing. Run it when a number moves on purpose, and say why
# in the commit.
bench-record:
	@$(MAKE) --no-print-directory BENCH=1 build/bench/kosmos.elf
	python3 tools/run_bench.py build/bench/kosmos.elf --record

#
# Whether upstream's engine still compiles against the shim, file by file,
# without building an image. The question worth asking after moving the
# vendored tree forward, and the one a link failure answers badly.
#
.PHONY: quake
quake:
	@mkdir -p build/quake
	@ok=0; fail=0; \
	for f in $(QUAKE_ENGINE); do \
	    o=build/quake/$$(echo $$f | tr / _).o; \
	    if $(CC) $(UCFLAGS) $(QUAKE_CFLAGS) -c $$f -o $$o 2>build/quake/err; then \
	        printf "  ok    %-54s %s bytes\n" "$$f" "$$(wc -c < $$o | tr -d ' ')"; \
	        ok=$$((ok + 1)); \
	    else \
	        printf "  FAIL  %s\n" "$$f"; \
	        head -5 build/quake/err | sed 's/^/        /'; \
	        fail=$$((fail + 1)); \
	    fi; \
	done; \
	echo; \
	echo "  $$ok of $$((ok + fail)) Quake engine files compile."; \
	echo "  quake.elf carries them and user/installed/Quake/quake_kosmos.c."; \
	test $$fail -eq 0


# Quake on the machine: a window, the engine started, the demo and its map,
# the game drawn, a command typed at Quake's console and answered, and
# Control-C closing it without a fault.
#
# Not part of `make test` or `make prepush`: it needs the shareware
# `pak0.pak`, which is not in the repository, so it is handed one. The pak is
# asked for before the build, which takes minutes, rather than after it.
.PHONY: quake-check
quake-check: $(HOSTDIR)/lua
	@test -f "$(PAK)" || { echo "FAIL: no pak. make quake-check PAK=/path/to/pak0.pak"; exit 1; }
	@$(MAKE) --no-print-directory MEGA= FULL=0
	@$(MAKE) --no-print-directory apps
	python3 tools/run_quake.py build/kosmos.elf $(PAK)

# The Super Nintendo on the machine: a ROM from `/Home/roms/snes`, a window
# drawing the game, its sound out of a virtio-sound device and recorded to a
# WAV, and how many frames a second the core really manages - which is the
# number this port was started to find. Enter is pressed to get past title
# screens and not checked; the pictures show whether it arrived.
#
# Not part of `make test` or `make prepush`, for the reason `quake-check` is
# not: it needs a ROM, and no ROM is in the repository. The pictures it takes
# go to `build/snes/`.
.PHONY: snes-check
snes-check: $(HOSTDIR)/lua
	@test -f "$(ROM)" || { echo "FAIL: no ROM. make snes-check ROM=/path/to/game.sfc"; exit 1; }
	@$(MAKE) --no-print-directory MEGA= FULL=0
	@$(MAKE) --no-print-directory apps
	python3 tools/run_snes.py build/kosmos.elf "$(ROM)"

# **Doom playing its own demo**, with a WAD - the shareware one will do - and
# `profile` watching: its frames a second, and whether it sleeps while it
# waits rather than spinning (`tools/run_doom.py`). Outside the gate for
# `quake-check`'s reason: the WAD is id's, and never in the repository.
.PHONY: doom-check
doom-check: $(HOSTDIR)/lua
	@test -f "$(WAD)" || { echo "FAIL: no WAD. make doom-check WAD=/path/to/doom1.wad"; exit 1; }
	@$(MAKE) --no-print-directory MEGA= FULL=0
	@$(MAKE) --no-print-directory apps
	python3 tools/run_doom.py build/kosmos.elf "$(WAD)"

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

# One directory now, so this removes everything rather than the four of ten
# it happened to name. That is the other half of moving the userland's
# objects under `build/`: a clean that misses some of what a build makes is
# a clean you cannot trust, and the six it left behind are what made the
# tree look the way it did.
clean:
	rm -rf build

-include $(DEPS)

#
# **Every dependency file the userland's objects have written**, whichever
# list the object is in - not only `USER_OBJS`'s. The installed
# applications' objects (`DOOM_OBJS`, `QUAKE_OBJS`, `SNES_OBJS`, the app
# test's) wrote theirs with `-MMD` like everything else, and nothing read
# them, so they were never rebuilt when a header changed. On 29 September
# Quake died with a general protection fault on the M700 every time it
# started: `quake_kosmos.c.o` was compiled on the 28th, before 4i-f grew
# `struct sysinfo` to 2744 bytes, and `l_start` kept the old size on its
# stack while the kernel wrote the new one over its return address
# (`testing.md` 18.281). Found by the file, not by a list, so the next
# application's objects are believed too.
#
-include $(shell find $(UBUILD) -name '*.d' 2>/dev/null)

# And the x86 kernel's, which is one compile of every source and so wrote
# none: its rule named its sources and none of their headers.
-include $(X86_BUILD)/kosmos.bin.d
