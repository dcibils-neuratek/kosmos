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
# **And the licence, which follows from the first line.** Doom is GPLv2 and
# nothing here is linked dynamically, so the image `FULL=1` builds is a
# combined work under the GPL, and `FULL=0` is the MIT one. `LICENSE` says
# so, and the About window reads it out of the image.
#
# **`MEGA=1` is everything this tree can put in one image**: what `FULL=1`
# turns on, and the two it does not carry - Lite XL and Quake. It is for
# running the whole of it at once, not for the suites: the test and bench
# images ignore it as they ignore `FULL`, and the checks that build a variant
# of their own say `MEGA=` so an inherited one cannot change what they check.
# The image is a GPL work (`LICENSE`), and the game data is still not in it:
# `make image FILES=...` puts `doom1.wad` and `pak0.pak` on the disk.
#
ifeq ($(MEGA),1)
ifndef TEST
ifndef BENCH
FULL   := 1
LITEXL := 1
QUAKE  := 1
endif
endif
endif

FULL ?= 1

ifeq ($(FULL),1)
ifndef TEST
ifndef BENCH
DOOM := 1
WEB  := 1
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
VARIANT := $(if $(filter-out aarch64,$(ARCH)),-$(ARCH))$(if $(TEST),-test)$(if $(BENCH),-bench)$(if $(DOOM),-doom)$(if $(WEB),-web)$(if $(LITEXL),-litexl)$(if $(QUAKE),-quake)

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

# What the machine says it is, in the boot banner and along the bottom of
# the desktop. One place, because it was two: the kernel's generated
# version.c had it right and the userland's had "QEMU virt aarch64" written
# into it, so the desktop on the x86 machine named the other one.
PLATFORM := $(if $(filter x86_64,$(ARCH)),QEMU q35 x86-64,QEMU virt aarch64)


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
        hal/qemu-virt/virtio.c \
        hal/virtio/net.c \
        hal/virtio/snd.c \
        hal/qemu-virt/snd_bind.c \
        hal/fwcfg/fwcfg.c \
        hal/qemu-virt/fwcfg_mmio.c \
        hal/fwcfg/ramfb.c \
        hal/qemu-virt/fb.c \
        hal/virtio/input.c \
        hal/qemu-virt/input_bind.c \
        hal/keys.c \
        hal/qemu-virt/input_describe.c \
        hal/virtio/blk.c \
        hal/qemu-virt/blk_bind.c \
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
        kernel/thread.c \
        kernel/sched_rr.c \
        kernel/sched_prio.c \
        kernel/ipc.c \
        kernel/memobj.c \
        kernel/process.c \
        kernel/smp.c \
        kernel/spinlock.c \
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
  SRCS      += user/hello-$(ARCH).S user/faulty-$(ARCH).S
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
# Thirty-two files, compiled here rather than linked from an archive,
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

LIBS :=

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
# `build/user/user/lib/gfx.c.o` - and the kernel's pattern matches that only
# with a stem of `user/user/lib/gfx`, whose prerequisite
# `user/user/lib/gfx.c` does not exist. Make discards a pattern rule whose
# prerequisites cannot be made, so there is no tie to break.
#
# It is also checked rather than argued: the kernel is built with
# `-mgeneral-regs-only`, so a userland file compiled by the wrong rule does
# not silently succeed - the first `float` in musl's math is a compile
# error. Every variant builds.
UBUILD := build/user$(VARIANT)

USER_LIBC := runtime/libc/string.c \
             runtime/libc/malloc.c \
             runtime/libc/math.c \
             runtime/libc/snprintf.c \
             runtime/libc/strtod.c \
             runtime/libc/stdio.c \
             runtime/libc/scan.c \
             runtime/libc/setjmp-$(ARCH).S \
             runtime/libc/callstack-$(ARCH).S \
             user/lib/misc_user.c \
             user/lib/panic_user.c

#
# Lite XL, vendored, and this is step one of a port rather than a finished
# one. `runtime/upstream/lite-xl/README.kosmos.md` is the account.
#
# **What is built here is only what needs nothing that does not exist.**
# `api/utf8.c` and `arena_allocator.c` call no SDL function and include no
# SDL header, and they compile against this toolchain unmodified - which is
# the fact that said the port was worth starting, and is worth having in the
# build so that it stays true rather than being remembered.
#
# Everything else in `src/` stops at one line, `#include <SDL.h>`, and waits
# for the shim. `rencache.c`, `api/renderer.c` and `renderer.c` join this
# list when it exists; `api/process.c`, `api/dirmonitor/` and
# `src/bundle_open.m` never will, for the reasons the README gives.
#
# `-w -Wno-error` for the reason every vendored thing here gets it: these
# are somebody else's warnings and this build has no business failing on
# them.
#
# **`-Iuser/lib/litexl` is the whole mechanism of this port.** The shim
# directory is on the include path, so the vendored `#include <SDL.h>` -
# which appears in three of upstream's headers and reaches every source
# file through them - resolves to `user/lib/litexl/SDL.h`, and not one line
# of what upstream released has to be touched.
LITEXL_CFLAGS := -w -Wno-error \
                 -Iruntime/upstream/lite-xl/src \
                 -Iuser/lib/litexl \
                 -Iuser/lib

#
# **Two lists, because a port has a front edge.**
#
# `LITEXL_SRCS` goes into the image, and everything in it links: the shim,
# and the upstream files that call nothing which does not exist yet.
#
# `LITEXL_STAGED` compiles and does not link. `rencache.c` and
# `api/renderer.c` are *finished* as far as compiling goes - they have no
# SDL in them at all and never needed a line changed - but they call the
# `ren_*` and `renwin_*` functions that steps three and four will write, so
# putting them in the image would break the link for everybody.
#
# `make litexl` compiles both lists and says where the edge is. That is the
# difference between a port that is progressing and one that is asserted to
# be: the compiler says which files are done, every time, rather than a
# checklist in a document saying so once.
#
LITEXL_SRCS := user/lib/litexl_sdl.c \
               user/lib/litexl_render.c \
               runtime/upstream/lite-xl/src/api/utf8.c \
               runtime/upstream/lite-xl/src/arena_allocator.c \
               runtime/upstream/lite-xl/src/renwindow.c \
               runtime/upstream/lite-xl/src/rencache.c \
               runtime/upstream/lite-xl/src/api/renderer.c \
               runtime/upstream/lite-xl/src/api/api.c \
               user/lib/litexl_system.c \
               user/lib/litexl_match.c

# Nothing is waiting on the renderer any more. `api/system.c` and `main.c`
# join this when step five writes their half of the shim.
LITEXL_STAGED :=

#
# `make QUAKE=1` - Quake, from Chocolate Quake, GPL like Doom and outside
# `FULL=1` for the same reasons.
#
# Chocolate Quake rather than the quakegeneric this began from, because
# quakegeneric says it builds only for 32-bit machines and Kosmos is only
# 64-bit; Chocolate Quake ships arm64 builds of the same WinQuake 1.09 code.
#
# **Two lists.** `QUAKE_ENGINE` is upstream's - 78 files, which `make quake`
# compiles on their own to say whether they still build against the shim.
# `QUAKE_SRCS` is what goes into the image: the engine, and
# `user/lib/quake_kosmos.c`, the platform under it (`Sys_*`, `VID_*`, `IN_*`,
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
# `-Iuser/lib/quake` is the SDL shim the engine's `#include <SDL.h>` and
# `<SDL_stdinc.h>` resolve to, so nothing upstream released is touched.
#
# `HAVE_STRCPY` and its two siblings are what upstream's CMake detects, and
# this libc has all three.
#
QUAKE_DIR := runtime/upstream/quake/src

QUAKE_INCLUDES := $(addprefix -iquote ,$(wildcard $(QUAKE_DIR)/*/include)) \
                  -iquote $(QUAKE_DIR) \
                  -Iuser/lib/quake \
                  -include user/lib/quake/kosmos_quake.h

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

QUAKE_SRCS := $(QUAKE_ENGINE) user/lib/quake_kosmos.c

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
             user/servers/console.c \
             user/servers/ramfs.c \
             user/servers/net.c \
             user/lib/net_kosmos.c \
             user/lib/crypto.c \
             user/lib/lua_glue.c \
             user/lib/sys_user.c \
             user/lib/gfx.c \
             user/lib/png.c \
             user/lib/jpeg.c \
             user/lib/docfont.c \
             user/lib/inflate.c \
             user/lib/pdftok.c \
             user/lib/gl_kosmos.c \
             user/lib/con_kosmos.c \
             user/lib/mp3_kosmos.c \
             $(MUSL_SRCS) \
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
ifdef LITEXL
USER_SRCS += $(LITEXL_SRCS) $(GEN)/litexl_fonts.c
endif

ifdef QUAKE
USER_SRCS += $(QUAKE_SRCS)
endif

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
endif

#
# `make WEB=1 qemu` builds an image with the NetSurf parsing stack in it.
#
# **A build option for the reason Doom is one: size.** These are five
# libraries and a hundred and forty thousand lines, and a build that does not
# want a browser should not carry a CSS engine. When the GPLv2 browser core
# joins them the flag will be doing licence work too, exactly as `DOOM`
# does - and drawing that line in the build rather than in a comment is the
# same argument, because a boundary somebody has to remember is not one.
#
# **No heap flag**, which was once a thing worth saying about this build in
# particular and is now true of every build: nothing carries
# `-DUSER_HEAP_PAGES` any more, because the heap grows and a program that
# needs more asks for it. See the note above `CFLAGS_BASE`.
#
ifdef WEB

NS       := runtime/upstream/netsurf
WEB_LIBS := libwapcaplet libparserutils libhubbub libcss libdom

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

# Kosmos's own side of it, held to the ordinary flags rather than the
# vendored ones - it is not vendored.
WEB_SRCS += user/lib/web_kosmos.c user/lib/web_select.c user/lib/web_style.c \
            user/lib/web_paint.c

# The property names, read out of the same file their own build reads.
WEB_PROPS   := $(shell sed -n 's/^\([^\#][^:]*\):.*/\1/p' \
                 $(NS)/libcss/src/parse/properties/properties.gen)
WEB_GEN_CSS := $(addprefix $(GEN)/netsurf/css/autogenerated_,\
                 $(addsuffix .c,$(WEB_PROPS)))

# The three that are not CSS parsers: two perl scripts and a gperf run.
WEB_GEN := $(GEN)/netsurf/aliases.inc \
           $(GEN)/netsurf/entities.inc \
           $(GEN)/netsurf/treebuilder/autogenerated-element-type.c \
           $(GEN)/netsurf/dom/bindings/hubbub/parser.h

USER_SRCS += $(WEB_SRCS) $(WEB_GEN_CSS)

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
WEB_CFLAGS := -w -Wno-error -fcommon -DWITHOUT_ICONV_FILTER \
              $(foreach l,$(WEB_LIBS),-I$(NS)/$(l)/include) \
              -I$(GEN)/netsurf -I$(GEN)/netsurf/css
endif

USER_OBJS := $(addprefix $(UBUILD)/,$(addsuffix .o,$(USER_SRCS)))
USER_DEPS := $(USER_OBJS:.o=.d)

# -Ikernel is for syscall.h and panic.h, and nothing else. The syscall
# numbers are the ABI and belong to both sides of it by definition.
UCFLAGS := $(CFLAGS_BASE) $(UTESTDEFS) -DKOSMOS_USER_BASE=$(USER_BASE) $(if $(DOOM),-DKOSMOS_DOOM -Iruntime/upstream/doom) $(if $(WEB),-DKOSMOS_WEB) $(if $(LITEXL),-DKOSMOS_LITEXL) $(if $(QUAKE),-DKOSMOS_QUAKE) -DKOSMOS_USER \
           -Iruntime/upstream/puff -Iruntime/upstream/stb \
           -Iruntime/upstream/minimp3 \
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
# vary with `DOOM`, `WEB`, `LITEXL` or `QUAKE`; `UBUILD` does. So the
# userland flags were recorded in a file the kernel's objects also depended
# on, and every switch between variants rewrote it.
#
# What that cost was the gate. `make prepush` builds plain, then `MEGA=1`,
# then plain again for the screenshot, and each switch changed
# `$(LITEXL_CFLAGS)` or `$(QUAKE_CFLAGS)` in the stamp - so **the whole
# kernel was recompiled three times for flags no kernel object uses.**
#
# Now each stamp covers exactly the flags its own objects are compiled with,
# which is what a stamp is for. The userland one lives in `UBUILD`, so it is
# per-variant and a MEGA build and a plain one cannot invalidate each other
# at all.
#
KFLAGS_NOW := $(CFLAGS)
KFLAGS_FILE := $(BUILD)/flags

UFLAGS_NOW := $(UCFLAGS) | $(DOOM_CFLAGS) | $(TINYGL_CFLAGS) | $(WEB_CFLAGS) | $(MUSL_CFLAGS) | $(LITEXL_CFLAGS)$(if $(QUAKE), | $(QUAKE_CFLAGS))
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
FB_FILE := $(BUILD)/fb.flags

$(shell [ "$$(cat $(FB_FILE) 2>/dev/null)" = '$(FB_FLAGS)' ] \
        || printf '%s' '$(FB_FLAGS)' > $(FB_FILE))

$(FB_FILE):
	@mkdir -p $(dir $@)
	@printf '%s' '$(FB_FLAGS)' > $@

$(BUILD)/hal/fwcfg/ramfb.c.o: CFLAGS += $(FB_FLAGS)
$(BUILD)/hal/fwcfg/ramfb.c.o: $(FB_FILE)

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

$(UBUILD)/runtime/upstream/tinygl/source/%.c.o: runtime/upstream/tinygl/source/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TINYGL_CFLAGS) -MMD -MP -c $< -o $@

#
# Kosmos's own half of the Lite XL port, which needs upstream's headers on
# the path: `litexl_render.c` implements `renderer.h`, so it has to see it.
# An explicit rule each, because the generic `user/lib` one carries only
# `UCFLAGS` and this is the one place under `user/lib` that needs more.
#
$(UBUILD)/user/lib/litexl_sdl.c.o: user/lib/litexl_sdl.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(LITEXL_CFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/user/lib/litexl_render.c.o: user/lib/litexl_render.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(LITEXL_CFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/user/lib/litexl_system.c.o: user/lib/litexl_system.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(LITEXL_CFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/user/lib/litexl_match.c.o: user/lib/litexl_match.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(LITEXL_CFLAGS) -MMD -MP -c $< -o $@

#
# Lite XL. Two patterns because its sources are one directory deep in
# places - `src/api/utf8.c` - and a single `%` does not cross a slash.
#
$(UBUILD)/runtime/upstream/lite-xl/src/%.c.o: runtime/upstream/lite-xl/src/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(LITEXL_CFLAGS) -MMD -MP -c $< -o $@

$(UBUILD)/runtime/upstream/lite-xl/src/api/%.c.o: runtime/upstream/lite-xl/src/api/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(LITEXL_CFLAGS) -MMD -MP -c $< -o $@

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
$(UBUILD)/user/lib/web_%.c.o: user/lib/web_%.c $(UFLAGS_FILE) | $(WEB_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) \
	      $(foreach l,$(WEB_LIBS),-Iruntime/upstream/netsurf/$(l)/include) \
	      -I$(GEN)/netsurf -MMD -MP -c $< -o $@

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

# Quake's, above the generic rule for the reason Doom's is.
$(UBUILD)/runtime/upstream/quake/%.c.o: runtime/upstream/quake/%.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(QUAKE_CFLAGS) -MMD -MP -c $< -o $@

# And Kosmos's half of it: Quake's headers on the path, and the warnings on,
# because this file is ours - all but `-Wcomment`, which five `//` comments
# in Quake's own headers set off by ending in a backslash, and which is about
# upstream's text rather than anything this file does.
$(UBUILD)/user/lib/quake_kosmos.c.o: user/lib/quake_kosmos.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(QUAKE_INCLUDES) -Wno-comment -MMD -MP -c $< -o $@

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
$(UBUILD)/user/lib/gl_demos.c.o: user/lib/gl_demos.c $(UFLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iruntime/upstream/tinygl/include -MMD -MP -c $< -o $@

$(UBUILD)/user/lib/gl_kosmos.c.o: user/lib/gl_kosmos.c $(UFLAGS_FILE)
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
#
# The Lite XL surface shim, built for *this* machine.
#
# It depends on `stdlib.h` and `string.h` and nothing else - no syscalls, no
# Kosmos headers, no framebuffer - so the host compiler builds it exactly as
# the cross one does. That is worth keeping rather than being a coincidence:
# it is what lets the shim be tested in a second instead of in an emulator,
# and `-Wall -Wextra -Werror` here is a second compiler's opinion of code the
# vendored build compiles with `-w`.
#
$(HOSTDIR)/test_litexl: tools/test_litexl_surface.c user/lib/litexl_sdl.c \
                        user/lib/litexl_render.c user/lib/litexl_match.c \
                        user/lib/litexl/SDL.h \
                        runtime/upstream/lite-xl/src/renwindow.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -O1 -o $@ \
	    -Iuser/lib/litexl -Iruntime/upstream/lite-xl/src \
	    -Iruntime/upstream/stb \
	    tools/test_litexl_surface.c user/lib/litexl_sdl.c \
	    user/lib/litexl_render.c user/lib/litexl_match.c \
	    runtime/upstream/stb/stb_impl.c \
	    runtime/upstream/lite-xl/src/renwindow.c -lm

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

$(HOSTDIR)/test_imagesum: tools/test_imagesum.c kernel/image_sum.h \
                          $(HOSTDIR)/imagesum_fixture.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -o $@ \
	        tools/test_imagesum.c $(HOSTDIR)/imagesum_fixture.c

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
# And `docs/cheatsheet.html`, which the desktop writes into `/home/Desktop`
# whenever it finds it missing or different - see `tracker.lua`. It is the
# project's own work, so there is no licence beside it and the generated
# file says so, which is what that line of the report is for.

$(GEN)/assets.c: assets/images/test-pattern.png assets/images/test-quads.jpg \
                 $(ICON_FILES) $(ART_FILES) LICENSE \
                 docs/cheatsheet.html tools/assets2c.py
	@mkdir -p $(dir $@)
	python3 tools/assets2c.py assets_table $@ \
	        assets/images/test-pattern.png assets/images/test-quads.jpg \
	        $(ICON_FILES) $(ART_FILES) LICENSE \
	        docs/cheatsheet.html

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

#
# Lite XL's own faces, in a `LITEXL=1` image only, and in a table of their own.
#
# Not in `fonts_table`, because that table is also what `gfx.fonts()` offers:
# Appearance would list `icons` as a face for the desktop, and every other
# image would carry 300 KB it never draws. The editor is the one reader -
# `provide_image_font` looks here after `fonts_table`. Their terms are
# recorded beside them, in `LICENSE.FiraSans` and `LICENSE.icons`, and
# `runtime/upstream/lite-xl/README.kosmos.md` says where those came from.
#
LITEXL_FONT_FILES := runtime/upstream/lite-xl/data/fonts/FiraSans-Regular.ttf \
                     runtime/upstream/lite-xl/data/fonts/icons.ttf

$(GEN)/litexl_fonts.c: $(LITEXL_FONT_FILES) tools/assets2c.py
	@mkdir -p $(dir $@)
	python3 tools/assets2c.py litexl_fonts_table $@ $(LITEXL_FONT_FILES)

$(GEN)/version.c: FORCE
	@mkdir -p $(dir $@)
	@printf '/* Generated by the Makefile. Do not edit. */\n\nconst char kosmos_name[] = "$(OS_NAME)";\nconst char kernel_name[] = "$(KERNEL_NAME)";\nconst char kosmos_version[] = "$(VERSION)";\nconst char kosmos_build[] = "$(KOSMOS_BUILD)";\nconst char kosmos_date[] = "$(KOSMOS_DATE)";\nconst char kosmos_platform[] = "$(PLATFORM)";\n' > $@.tmp
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
#
# Lite XL's Lua, when it is being built: 78 files that are the editor.
#
# They go into the *library* store rather than a store of their own, and
# that is the honest place for them - they are libraries carried in the
# image, which is what `/lib` is. It also costs nothing: `binfs.c` finds an
# entry with `strcmp`, so a key with slashes reads straight out, and no
# server, role or capability had to be invented to serve them.
#
LITEXL_DATA := $(if $(LITEXL),$(shell find runtime/upstream/lite-xl/data \
                                   -name '*.lua' 2>/dev/null))

LITEXL_ROOTED := $(if $(LITEXL),--rooted runtime/upstream/lite-xl/data litexl/)

$(GEN)/libraries.c: $(wildcard user/lib/*.lua) $(LITEXL_DATA) tools/progs2c.py \
                    $(HOSTDIR)/lua.ok
	@mkdir -p $(dir $@)
	python3 tools/progs2c.py libraries_lua $@ $(wildcard user/lib/*.lua) \
	    $(LITEXL_ROOTED) $(LITEXL_DATA)

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
FORWARD := $(if $(HTTP),$(comma)hostfwd=tcp::$(HTTP)-:80,)

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

QEMUFLAGS := -M virt,gic-version=3 $(ACCEL) -m 512M -smp $(SMP) $(SMPARG) \
             -global virtio-mmio.force-legacy=false \
             -device ramfb -device virtio-keyboard-device \
             -device virtio-tablet-device \
             $(AUDIO_FLAGS) \
             $(NET_FLAGS) \
             -drive file=$(DISK),format=raw,if=none,id=disk \
             -device virtio-blk-device,drive=disk \
             $(BOOTARG) \
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
RELEASE_SIZES := $(if $(or $(WEB),$(DOOM)),1280x720 1920x1080,\
                   1024x768 1280x800 1920x1080)

# What is in it, in the name, because the images are not interchangeable and
# a name that did not say so is a trap: the browser opens on an image built
# without it and reports no web kit, which reads like a broken browser
# rather than the wrong file. `-full` is browser and Doom, which is what
# `make release` builds unless told otherwise.
RELEASE_TAG := $(if $(and $(DOOM),$(WEB)),-full,\
                 $(if $(WEB),-web,$(if $(DOOM),-doom,)))

# A binary that leaves this machine has been used for a while first.
#
# `make test` says the parts work and `make screenshot` says the machine
# works once. Neither notices a pool that fills on the fiftieth try, and a
# release is the thing somebody else runs without watching it.
release: $(TARGET) stress
	@mkdir -p builds
	@for size in $(RELEASE_SIZES); do \
	    rm -f $(BUILD)/hal/fwcfg/ramfb.c.o $(TARGET); \
	    $(MAKE) --no-print-directory FB=$$size $(TARGET) >/dev/null; \
	    cp $(TARGET) \
	       builds/kosmos-$(VERSION)-$(KOSMOS_BUILD)-$$size$(RELEASE_TAG).elf; \
	    echo "builds/kosmos-$(VERSION)-$(KOSMOS_BUILD)-$$size$(RELEASE_TAG).elf"; \
	done
	@rm -f $(BUILD)/hal/fwcfg/ramfb.c.o $(TARGET)
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
             hal/pc/cpus.c \
             hal/pc/cpu_on.c \
             hal/pc/cpu_here.c \
             hal/pc/trampoline.S \
             hal/fwcfg/fwcfg.c \
             hal/fwcfg/ramfb.c \
             hal/pc/fb.c \
             hal/pc/loader_fb.c \
             hal/pc/fwcfg_port.c \
             hal/pc/boot_option.c \
             hal/pc/acpi.c \
             hal/pc/pci.c \
             hal/pc/virtio.c \
             hal/virtio/blk.c \
             hal/pc/nvme.c \
             hal/pc/memdisk.c \
             hal/pc/blk_bind.c \
             hal/virtio/net.c \
             hal/virtio/input.c \
             hal/pc/i8042.c \
             hal/pc/input_bind.c \
             hal/keys.c \
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
             kernel/thread.c \
             kernel/sched_rr.c \
             kernel/sched_prio.c \
             kernel/ipc.c \
             kernel/memobj.c \
             kernel/process.c \
             kernel/smp.c \
             kernel/spinlock.c \
             kernel/syscall.c \
             kernel/main.c \
             $(X86_BUILD)/init_bin.c

ifdef TEST
  # The same additions `SRCS` gets on the other board, and for the same
  # reasons: the suite, the ring-3 fixture blobs only it runs, and the libc
  # the kernel no longer links because the unit tests for it call it
  # directly.
  X86_SRCS += tests/tests.c \
              user/hello-x86_64.S user/faulty-x86_64.S \
              runtime/libc/malloc.c runtime/libc/misc.c \
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
$(X86_BUILD)/init_bin.c: $(UBUILD)/init.bin tools/bin2c.py
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< init_image $@

.PHONY: x86 x86-build
#
# `FULL=0` is passed through, and it has to be.
#
# Inside the recursive call, `FULL` decides `DOOM` and `WEB`, which decide
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
# Inside the recursive call `FULL` decides `DOOM` and `WEB`, which decide
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
X86_DEVICES := -device ramfb \
               -device virtio-tablet-pci \
               -device virtio-net-pci,netdev=n0 -netdev user,id=n0 \
               -drive file=$(DISK),format=raw,if=none,id=d0 \
               -device nvme,drive=d0,serial=kosmos \
               -device ich9-intel-hda -device hda-output,audiodev=a0 \
               -audiodev coreaudio,id=a0

#
# A stick somebody can boot, which is the thing `-kernel` is not.
#
# **QEMU's `-kernel` is not a loader**, and every x86 boot in this project
# went through it until now. It reads the multiboot header, copies the image
# in and jumps - and it does not answer the video request, so the one path a
# laptop depends on entirely had never run. `docs/thinkpad.md` has what that
# hid: three faults, none of them a driver, all of them fatal and silent on
# a machine with no serial port.
#
# So this builds the real thing: GRUB, a filesystem, and an image that boots
# the way the ThinkPad will.
#
# **`insmod efi_gop` rather than `all_video`, and it is not a detail.** With
# every video driver loaded GRUB picks its own - under QEMU that is the
# bochs one, which hands over 800x600 at *24* bits per pixel, and
# `loader_fb.c` refuses it because everything above `struct fb` treats a
# pixel as one 32-bit word. Asking the firmware's own GOP gives the panel's
# mode at 32 bits, which is what a laptop has and what OVMF reports here.
#
# UEFI only, because that is what this GRUB was built for and because it is
# the half that matters: a 2020 ThinkPad may have no CSM at all, and GRUB
# under UEFI boots a multiboot 1 kernel perfectly well - which this proves
# rather than assumes.
#
GRUB_MKRESCUE := x86_64-elf-grub-mkrescue
ISO           := $(X86_BUILD)/kosmos.iso

#
# The recipe hangs off `x86-build` rather than off `kosmos.bin`, because a
# prerequisite on that file asks *this* make to build it - and this make is
# the AArch64 one, which greets `-mno-sse` with `did you mean -fno-dse`.
# The x86 image is built by a sub-make with its own toolchain, and the only
# honest way to say so is to depend on the target that runs it.
#
x86-iso: x86-build
	@rm -rf $(X86_BUILD)/iso
	@mkdir -p $(X86_BUILD)/iso/boot/grub
	@cp $(X86_BUILD)/kosmos.bin $(X86_BUILD)/iso/boot/
	@printf 'set timeout=0\nset default=0\n\nmenuentry "Kosmos" {\n  insmod efi_gop\n  multiboot2 /boot/kosmos.bin\n  boot\n}\n' \
	  > $(X86_BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(ISO) $(X86_BUILD)/iso 2>/dev/null
	@ls -l $(ISO)

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
# keyboard-only with `/home` in memory, which is exactly what the first real
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
USB_IMG := $(X86_BUILD)/kosmos-usb.img

#
# The stick's image: a GPT, one EFI System Partition, GRUB and Kosmos.
#
# **This replaced the `grub-mkrescue` ISO because the ISO failed on the
# machine.** It booted perfectly under OVMF and reached this on a real
# ThinkPad:
#
#     error: file '/boot/grub/x86_64-efi/boot.mod' not found.
#     Entering rescue mode...
#
# `grub-mkrescue` keeps its modules only inside the El Torito FAT image and
# leaves nothing at that path on the ISO9660 filesystem beside it; under
# QEMU GRUB's idea of its own root resolved to the FAT image and found
# them, and on that firmware it resolved elsewhere and did not.
# `tools/mkusb_image.py` has the whole account, including the two further
# things that went wrong while fixing it.
#
# `KOSMOS_ARGS` puts words on GRUB's `multiboot2` line - the kernel's command
# line, and on a machine with no fw_cfg the only way to give it a boot option:
#
#     make usb KOSMOS_ARGS=opt/kosmos/smp=1
#
# **And `$(DISK)` on the stick as well, when it holds a filesystem** - the
# disk `make image FILES=...` makes, which is where Doom's WAD and Quake's
# pak live. GRUB loads it into memory beside the kernel and Kosmos mounts it
# at boot, ahead of the machine's own drive; `hal/pc/blk_bind.c` says why.
# Only a disk `kfs.lua` can read goes on: `make qemu` leaves an empty one
# behind, and carrying that would hide a ThinkPad's NVMe `/home` for nothing.
#
#     make image FILES="doom1.wad:/home/doom1.wad pak0.pak:/home/id1/pak0.pak"
#     make MEGA=1 usb
#
x86-usb-image: x86-build $(HOSTDIR)/lua
	@if [ -f $(DISK) ] && $(HOSTDIR)/lua tools/kfs.lua ls $(DISK) >/dev/null 2>&1; then \
	    echo "$(DISK) goes on the stick too: GRUB loads it, Kosmos mounts it"; \
	    python3 tools/mkusb_image.py $(X86_BUILD)/kosmos.bin $(USB_IMG) --disk $(DISK) $(KOSMOS_ARGS); \
	else \
	    python3 tools/mkusb_image.py $(X86_BUILD)/kosmos.bin $(USB_IMG) $(KOSMOS_ARGS); \
	fi

usb: x86-usb-image
	@bash tools/mkusb.sh $(USB_IMG)

x86-uefi: x86-iso
	@cp $(OVMF_VARS) $(X86_BUILD)/ovmf-vars.fd
	qemu-system-x86_64 -M q35 -m $(if $(MEM),$(MEM),16G) -no-reboot \
	  -drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,unit=1,file=$(X86_BUILD)/ovmf-vars.fd \
	  -vga none -device VGA,xres=$(PANEL_W),yres=$(PANEL_H) \
	  -device ich9-intel-hda -device hda-output,audiodev=a0 \
	  -audiodev coreaudio,id=a0 \
	  $(if $(SERIAL),-display none -serial stdio,-display $(X86_DISPLAY) -serial mon:stdio) \
	  -cdrom $(ISO)

x86: x86-build $(DISK)
	qemu-system-x86_64 -M q35 -m 512M -no-reboot $(X86_ACCEL) -vga none \
	  $(if $(SERIAL),-nographic,-display $(X86_DISPLAY) -serial mon:stdio) \
	  $(X86_DEVICES) \
	  $(if $(BOOT),-fw_cfg name=opt/kosmos/boot$(comma)string=$(BOOT)) \
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
test: $(TARGET) $(HOSTDIR)/lua $(HOSTDIR)/test_litexl $(HOSTDIR)/test_audioring $(HOSTDIR)/test_loaderfb $(HOSTDIR)/test_pmmplace $(HOSTDIR)/test_apicdecode $(HOSTDIR)/test_scan $(HOSTDIR)/test_imagesum
	@# The format, on this machine, before anything is booted. It is the
	@# fastest of the three and the one that fails first when the disk
	@# layout is wrong.
	$(HOSTDIR)/lua tools/test_kfs.lua
	@# The WAV header walker, likewise: pure Lua over a reader, so the
	@# awkward headers can be built by hand rather than found in the wild.
	$(HOSTDIR)/lua tools/test_wav.lua
	$(HOSTDIR)/lua tools/test_iconlayout.lua
	@# The Deskbar's menu, read off a folder tree - what counts as an item,
	@# what order things come in, how deep a folder may go. The store it
	@# reads through is a table here, which is the whole reason the reading
	@# lives in `/lib` and not inside the Deskbar.
	$(HOSTDIR)/lua tools/test_deskbarmenu.lua
	@# And what a file *is*: the attribute first, the extension second.
	$(HOSTDIR)/lua tools/test_filetypes.lua
	@# And the audio ring's position arithmetic. It models the client, the
	@# server and the device queue, because the thing worth asserting is
	@# that a period taken out of the ring is not yet a period heard.
	$(HOSTDIR)/test_audioring
	@# And the scanf family's scanner, which reads Quake's demos out of a
	@# pak: `%f` writes a float, and nothing past a length is read.
	$(HOSTDIR)/test_scan
	@#
	@# And where the page bitmap goes, which is the same shape of test one
	@# layer down: arithmetic with an awkward case that firmware produces
	@# and QEMU does not.
	$(HOSTDIR)/test_pmmplace
	$(HOSTDIR)/test_loaderfb
	$(HOSTDIR)/test_apicdecode
	@# And the userland image's canary, over a blob the real script
	@# generated during this build - because its two halves are Python and
	@# C and nothing at run time can notice them disagreeing.
	$(HOSTDIR)/test_imagesum
	@# And the Lite XL surface shim, which is C and still needs no machine:
	@# `make litexl` says the port's sources compile, and this says the part
	@# of them Kosmos wrote is correct. Different claims.
	$(HOSTDIR)/test_litexl
	@# And the port's Lua half - nineteen thousand lines of somebody else's
	@# code - loaded and initialised with the C modules stubbed. It is the
	@# same interpreter either way, so this needs no machine either.
	$(HOSTDIR)/lua tools/test_litexl_lua.lua
	$(HOSTDIR)/lua tools/test_litexl_host.lua
	@# And LICENSE, read the way the About window reads it: every line of
	@# it, and every vendored tree named in it - so a library added without
	@# an entry fails here, by name.
	$(HOSTDIR)/lua tools/test_licences.lua LICENSE $(wildcard runtime/upstream/*/) lua/upstream/
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
	@# And the prompt as a place to work rather than a place to look. The
	@# verbs check each other rather than a constant written in the
	@# harness: `wc` says five lines, so `head` and `tail` have to name
	@# the first and last two of exactly those.
	python3 tools/run_shell.py $(TARGET)
	@# A frame off the card and onto the wire, read back out of QEMU's own
	@# capture - because nothing inside the guest can establish that one
	@# left. And a second boot with no card, which is the branch every
	@# device grant in init.lua carries a comment about getting wrong.
	python3 tools/run_network.py $(TARGET)
	@# And the second architecture, which until now proved nothing that
	@# stayed proved: a staging kmain printed what it found and a person
	@# read it. Skipped rather than failed where the cross compiler is not
	@# installed - and said out loud, because a suite that quietly runs
	@# fewer checks on one machine than another is worse than one that does
	@# not run them at all.
	@#
	@# Five of them on the other board now, and the choice is about
	@# *what is board-specific* rather than about coverage for its own
	@# sake. `run_x86.py` boots it; `run_headless.py` asks whether a
	@# machine with no display still reaches a prompt, which is the
	@# branch every device grant carries a comment about; `run_disk.py`
	@# and `run_network.py` are the two that go through a driver this
	@# board finds over PCI rather than in a device-tree window.
	@#
	@# And `run_tests.py`, which is the one that was missing and is the
	@# largest test asset here: 4,000 lines and the only thing that
	@# exercises the kernel from inside it. It ran on one board for as
	@# long as there were two, which the 0.9.0 review found and could
	@# not fix in a line - the suite was written in AArch64 assembly in
	@# thirty-five places, exited through ARM semihosting, and ran two
	@# hand-written AArch64 blobs at EL0.
	@#
	@# 123 of it runs here against 127 there, and the four that do not
	@# are about AArch64 itself rather than about the kernel: stepping
	@# ELR past a faulting instruction, execution resuming after one,
	@# SPSel, and the lazy-FP mechanism being disarmed until something
	@# wants it. Nothing is skipped for being inconvenient.
	@#
	@# **`run_uefi.py` is the one that boots the way a machine will**, and
	@# it is here because everything above it goes through QEMU's
	@# `-kernel`, which is not a loader. It reads the multiboot header,
	@# copies the image in and jumps - and it does not answer the video
	@# request, so the path a laptop depends on entirely had never run
	@# while fourteen host checks on the decision all passed. Three
	@# faults were hiding behind that and none was a driver.
	@#
	@# It also checks the *screen* rather than the serial line, because a
	@# machine whose framebuffer works stops talking to the serial line at
	@# stage six. Skipped where GRUB or OVMF is not installed, out loud.
	@#
	@# `run_interchange.py` and `run_queries.py` are deliberately not
	@# here, and this says so out loud rather than leaving a gap
	@# somebody has to notice: their guest half is the same filesystem
	@# `run_disk.py` has just exercised on this board, and their host
	@# half does not boot anything. Adding them would double what this
	@# costs to check the same code twice.
	@if command -v x86_64-elf-gcc >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory x86-build >/dev/null && \
	    python3 tools/run_x86.py build/x86_64/kosmos.elf && \
	    python3 tools/run_headless.py build/x86_64/kosmos.elf && \
	    python3 tools/run_disk.py build/x86_64/kosmos.elf && \
	    python3 tools/run_network.py build/x86_64/kosmos.elf && \
	    $(MAKE) --no-print-directory x86-iso >/dev/null 2>&1 && \
	    python3 tools/run_uefi.py build/x86_64/kosmos.iso && \
	    $(MAKE) --no-print-directory TEST=1 x86-build >/dev/null && \
	    python3 tools/run_tests.py build/x86_64-test/kosmos.elf --timeout 90; \
	else \
	    echo "SKIP: x86-64, because x86_64-elf-gcc is not installed."; \
	fi

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
# optional variant like `DOOM=1` and the ordinary image carries none of it.
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
# shot` - which is correct and slow. Make builds a prerequisite's own
# dependencies one at a time unless told otherwise, so every object in every
# variant was compiled serially on a machine with ten cores.
#
# `-j` on the whole thing is not the answer: the five stages would run at
# once, which means several QEMUs racing for the same build directories and
# a screenshot taken of whichever image happened to be linked last. The
# stages are *ordered* on purpose.
#
# So the ordering stays here in the recipe and the parallelism goes inside
# each stage, where it is safe: one stage at a time, its compiles spread
# across `J` jobs. `make J=4 prepush` for a quieter machine.
#
J ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

prepush:
	@$(MAKE) --no-print-directory -j$(J) test
	@$(MAKE) --no-print-directory -j$(J) screenshot
	@$(MAKE) --no-print-directory -j$(J) litexl-check
	@$(MAKE) --no-print-directory -j$(J) mega
	@$(MAKE) --no-print-directory -j$(J) shot
	@echo
	@echo "ready to push: suites green and $(SHOTDIR) has today's picture."


.PHONY: shot web browser
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

#
# How far Lite XL has got: every file that compiles, and every one that
# does not yet.
#
# Compiles rather than links, deliberately. The staged files call functions
# steps three and four have not written, so a link would fail for a reason
# that says nothing about whether the *porting* is working. What this
# answers is the one question worth asking between steps: does upstream's C
# still build against the shim as it stands.
#
.PHONY: litexl
litexl:
	@mkdir -p build/litexl
	@ok=0; fail=0; \
	for f in $(LITEXL_SRCS) $(LITEXL_STAGED); do \
	    if $(CC) $(UCFLAGS) $(LITEXL_CFLAGS) -c $$f \
	         -o build/litexl/$$(basename $$f).o 2>build/litexl/err; then \
	        printf "  ok    %-52s %s bytes\n" "$$f" \
	               "$$(wc -c < build/litexl/$$(basename $$f).o | tr -d ' ')"; \
	        ok=$$((ok + 1)); \
	    else \
	        printf "  FAIL  %s\n" "$$f"; \
	        head -3 build/litexl/err | sed 's/^/        /'; \
	        fail=$$((fail + 1)); \
	    fi; \
	done; \
	echo; \
	echo "  $$ok of $$((ok + fail)) Lite XL translation units compile."; \
	echo "  In the image: $(words $(LITEXL_SRCS)).  Waiting on ren_*/renwin_*: $(words $(LITEXL_STAGED))."; \
	test $$fail -eq 0

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
	echo "  A QUAKE=1 image carries them and user/lib/quake_kosmos.c."; \
	test $$fail -eq 0

# Lite XL on the machine: a window, a title that follows its file, a file
# edited and saved, Control-N, and a new document saved under a name - each
# file read back at the prompt afterwards, so a pass is the file saying what
# was typed rather than a picture of text.
#
# Not part of `make test`: it needs an image built with `LITEXL=1`, which the
# ordinary image is not, and it boots that image twice. `make prepush` runs
# it before `shot`: this leaves a lean `LITEXL=1` image in `build/kosmos.elf`,
# and `shot` builds the ordinary one again before it takes its picture.
.PHONY: litexl-check
litexl-check:
	@$(MAKE) --no-print-directory MEGA= FULL=0 LITEXL=1
	python3 tools/run_litexl.py build/kosmos.elf

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
	@$(MAKE) --no-print-directory MEGA= FULL=0 QUAKE=1
	python3 tools/run_quake.py build/kosmos.elf $(PAK)

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
-include $(USER_DEPS)
