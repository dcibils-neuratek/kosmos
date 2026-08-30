# Kosmos
#
# make qemu     build and run under QEMU virt
# make debug    the same, stopped, with a gdbserver on :1234
# make clean

CROSS   := aarch64-none-elf-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
GDB     := $(CROSS)gdb

BUILD   := build
TARGET  := $(BUILD)/kosmos.elf

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
          -Iarch/aarch64 -Ihal

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

SRCS := boot/start.S \
        hal/qemu-virt/uart.c \
        kernel/main.c

OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SRCS)))
DEPS := $(OBJS:.o=.d)

QEMU      := qemu-system-aarch64
QEMUFLAGS := -M virt -cpu cortex-a72 -m 512M -nographic -kernel $(TARGET)

.PHONY: all qemu debug disasm clean

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

# In another terminal: aarch64-none-elf-gdb build/kosmos.elf
#                      (gdb) target remote :1234
debug: $(TARGET)
	$(QEMU) $(QEMUFLAGS) -S -gdb tcp::1234

disasm: $(TARGET)
	$(OBJDUMP) -d $(TARGET)

clean:
	rm -rf $(BUILD)

-include $(DEPS)
