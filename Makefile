# Kosmos
#
# make qemu     build and run under QEMU virt
# make test     run the suite under QEMU, exit code 0 or 1
# make debug    the same as qemu, stopped, with a gdbserver on :1234
# make clean

CROSS   := aarch64-none-elf-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size

SRCS := boot/start.S \
        arch/aarch64/vectors.S \
        arch/aarch64/trap.c \
        arch/aarch64/mmu.c \
        hal/qemu-virt/uart.c \
        hal/qemu-virt/memory.c \
        kernel/console.c \
        kernel/panic.c \
        kernel/pmm.c \
        kernel/string.c \
        kernel/main.c

# The test build is a separate image in a separate directory. Same sources
# plus the suite, with KOSMOS_TEST defined, so the tests cost the normal
# image nothing and the two never share a stale object file.
ifdef TEST
  BUILD    := build/test
  SRCS     += tests/tests.c
  TESTDEFS := -DKOSMOS_TEST -Itests
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
CFLAGS := -std=c11 -ffreestanding -nostdlib -nostartfiles \
          -Wall -Wextra -Werror -fno-common -fno-strict-aliasing \
          -mgeneral-regs-only \
          -O2 -g \
          -Iarch/aarch64 -Ihal -Ikernel $(TESTDEFS)

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

OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SRCS)))
DEPS := $(OBJS:.o=.d)

QEMU      := qemu-system-aarch64
QEMUFLAGS := -M virt -cpu cortex-a72 -m 512M -nographic -kernel $(TARGET)

.PHONY: all qemu test debug disasm size clean

all: $(TARGET)

$(TARGET): $(OBJS) boot/kosmos.ld
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

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

# In another terminal: aarch64-none-elf-gdb build/kosmos.elf
#                      (gdb) target remote :1234
debug: $(TARGET)
	$(QEMU) $(QEMUFLAGS) -S -gdb tcp::1234

disasm: $(TARGET)
	$(OBJDUMP) -d $(TARGET)

size: $(TARGET)
	$(SIZE) $(TARGET)

clean:
	rm -rf build

-include $(DEPS)
