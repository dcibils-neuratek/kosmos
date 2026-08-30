# Kosmos
#
# make qemu     build and run under QEMU virt
# make test     run the suite under QEMU, exit code 0 or 1
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

# Where generated sources go. Defined here rather than beside the rules that
# produce them, because SRCS below is a := assignment and expands it on the
# spot; further down it would expand to nothing and the object would be
# named build//init_bin.c.o.
GEN := build/gen
SIZE    := $(CROSS)size

SRCS := boot/start.S \
        arch/aarch64/vectors.S \
        arch/aarch64/trap.c \
        arch/aarch64/mmu.c \
        arch/aarch64/switch.S \
        arch/aarch64/el0.S \
        user/hello.S \
        user/faulty.S \
        hal/qemu-virt/uart.c \
        hal/qemu-virt/memory.c \
        hal/qemu-virt/gic.c \
        hal/qemu-virt/timer.c \
        kernel/console.c \
        runtime/libc/string.c \
        runtime/libc/setjmp.S \
        runtime/libc/malloc.c \
        runtime/libc/misc.c \
        runtime/libc/math.c \
        runtime/libc/snprintf.c \
        runtime/libc/stdio.c \
        runtime/libc/strtod.c \
        kernel/panic.c \
        kernel/pmm.c \
        kernel/thread.c \
        kernel/sched_rr.c \
        kernel/ipc.c \
        kernel/process.c \
        kernel/syscall.c \
        kernel/main.c \
        $(GEN)/init_bin.c \
        lua/kosmos/kosmos_lua.c \
        lua/kosmos/repl.c \
        lua/kosmos/sys.c

# Upstream Lua. The core, plus the libraries that are allowed to exist.
#
# What is missing is the point, and it is a security decision rather than a
# build one (design.md 5.3): no liolib or loslib, because there is no global
# tree to open a path in and no wall clock; no loadlib, which wants dlopen;
# no ldblib, because debug.getupvalue breaks any abstraction built in Lua.
# linit.c is out too, since it opens all of them; lua/kosmos/ opens ours.
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

SRCS += $(LUA_SRCS)

# The test build is a separate image in a separate directory. Same sources
# plus the suite, with KOSMOS_TEST defined, so the tests cost the normal
# image nothing and the two never share a stale object file.
ifdef TEST
  BUILD    := build/test
  SRCS     += tests/tests.c
  TESTDEFS := -DKOSMOS_TEST -Itests
else ifdef BENCH
  BUILD    := build/bench
  SRCS     += bench/bench.c
  TESTDEFS := -DKOSMOS_BENCH -Ibench
else
  BUILD    := build
  TESTDEFS :=
endif

TARGET := $(BUILD)/kosmos.elf

# Not to be changed without discussion. See CLAUDE.md.
#
# -mgeneral-regs-only is there because the kernel does not save FP/SIMD
# registers on a context switch. If something in the kernel needs a float,
# it is badly designed, and this turns that into a compile error rather than
# a corrupted register found three milestones later.
CFLAGS_BASE := -std=c11 -ffreestanding -nostdlib -nostartfiles \
               -Wall -Wextra -Werror -fno-common -fno-strict-aliasing \
               -O2 -g \
               -Iarch/aarch64 -Ihal -Ikernel -Iruntime/include -Ilua/kosmos -Iuser \
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
# and `lua/patches/` stays empty.
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
$(BUILD)/tests/tests.c.o:            CFLAGS := $(CFLAGS_FP) -Ilua/upstream
$(BUILD)/bench/bench.c.o:            CFLAGS := $(CFLAGS_FP)

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

OBJS     := $(addprefix $(BUILD)/,$(addsuffix .o,$(SRCS)))
LUA_OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(LUA_SRCS)))

# These have to come after LUA_OBJS is assigned: make expands the target
# side of a rule as it reads it, and an empty variable there matches nothing
# and fails silently.
$(LUA_OBJS): CFLAGS := $(CFLAGS_LUA_UPSTREAM)
$(BUILD)/lua/kosmos/kosmos_lua.c.o: CFLAGS := $(CFLAGS_FP) $(LUA_FLAGS)
$(BUILD)/lua/kosmos/repl.c.o:       CFLAGS := $(CFLAGS_FP) $(LUA_FLAGS)
$(BUILD)/lua/kosmos/sys.c.o:        CFLAGS := $(CFLAGS_FP) $(LUA_FLAGS)

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

UBUILD := build-user

USER_LIBC := runtime/libc/string.c \
             runtime/libc/malloc.c \
             runtime/libc/math.c \
             runtime/libc/snprintf.c \
             runtime/libc/strtod.c \
             runtime/libc/stdio.c \
             runtime/libc/setjmp.S \
             user/lib/misc_user.c \
             user/lib/panic_user.c

USER_SRCS := user/init/start.S \
             user/init/main.c \
             user/lib/lua_glue.c \
             user/lib/sys_user.c \
             $(USER_LIBC) \
             $(LUA_SRCS) \
             $(GEN)/init_lua.c

USER_OBJS := $(addprefix $(UBUILD)/,$(addsuffix .o,$(USER_SRCS)))
USER_DEPS := $(USER_OBJS:.o=.d)

# -Ikernel is for syscall.h and panic.h, and nothing else. The syscall
# numbers are the ABI and belong to both sides of it by definition.
UCFLAGS := $(CFLAGS_BASE) \
           -Iuser/include -Ikernel -Iruntime/include \
           -Ilua/upstream -Ilua/kosmos \
           -fno-stack-protector

ULDFLAGS := -T user/user.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
            -Wl,-Map,$(UBUILD)/init.map

$(UBUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -include lua/kosmos/kosmos_lua.h -MMD -MP -c $< -o $@

$(UBUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -MMD -MP -c $< -o $@

# The Lua source init runs, and then init itself, both carried inside the
# kernel image because there is no filesystem to load them from until M8.
$(GEN)/init_lua.c: user/init/init.lua tools/bin2c.py
	@mkdir -p $(dir $@)
	python3 tools/bin2c.py $< init_lua $@

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
QEMUFLAGS := -M virt,gic-version=3 -cpu cortex-a72 -m 512M -nographic \
             -kernel $(TARGET)

.PHONY: all qemu test bench debug disasm size clean

all: $(TARGET)

$(TARGET): $(OBJS) boot/kosmos.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@ $(LIBS)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Ctrl-A then x to quit QEMU.
qemu: $(TARGET)
	$(QEMU) $(QEMUFLAGS)

# Recursive so the test image gets its own BUILD and its own flags. The
# runner lives on the host and owns the QEMU line for tests, because it needs
# semihosting and a timeout.
test:
	@$(MAKE) --no-print-directory TEST=1 build/test/kosmos.elf
	python3 tools/run_tests.py build/test/kosmos.elf

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
	$(SIZE) $(TARGET)

clean:
	rm -rf build build-user

-include $(DEPS)
-include $(USER_DEPS)
