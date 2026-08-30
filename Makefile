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
        hal/qemu-virt/uart.c \
        kernel/console.c \
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
# --no-warn-rwx-segments: the whole image is one segment that is read, write
# and execute, which is exactly right while there is no MMU to enforce
# anything. M1 turns the MMU on and gives .text and .rodata their real
# permissions, and the warning gets re-enabled then. Silenced rather than
# tolerated, because a build that always prints a warning teaches you to
# stop reading them.
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
