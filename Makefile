##############################################################################
# NovOS Makefile
#
# Build a bootable 64-bit x86_64 kernel using a cross-compiler.
#
# Requirements:
#   - x86_64-elf-gcc   (cross-compiler, see TOOLCHAIN section below)
#   - x86_64-elf-ld
#   - nasm             (for .asm files)
#   - grub-mkrescue    (to build the bootable ISO)
#   - xorriso          (required by grub-mkrescue)
#   - qemu-system-x86_64 (for running)
#
# Quick start:
#   make          - Build the kernel and ISO
#   make run      - Build and run in QEMU
#   make debug    - Run with GDB server on port 1234
#   make clean    - Remove build artifacts
##############################################################################

##############################################################################
# Toolchain configuration
#
# If you have a cross-compiler installed as x86_64-elf-gcc, use:
#   CC = x86_64-elf-gcc
#   LD = x86_64-elf-ld
#   AS = nasm
#
# If your host is already x86_64 Linux and you trust the system compiler
# (less safe, can accidentally include host libraries), use:
#   CC = gcc
#   LD = ld
##############################################################################

CC  := x86_64-elf-gcc
CXX := x86_64-elf-g++
LD  := x86_64-elf-ld
AS  := nasm
OBJCOPY := x86_64-elf-objcopy

# Fallback: if cross-compiler not found, try system compiler
ifeq ($(shell which $(CC) 2>/dev/null),)
    CC  := gcc
    CXX := g++
    LD  := ld
    OBJCOPY := objcopy
    $(warning "x86_64-elf-gcc not found, using system compiler (may cause issues)")
endif

##############################################################################
# Directories
##############################################################################

BUILD   := build
ISODIR  := $(BUILD)/iso
GRUBDIR := $(ISODIR)/boot/grub

KERNEL_BIN  := $(BUILD)/kernel.elf
KERNEL_ISO  := $(BUILD)/novos.iso

##############################################################################
# Compiler / assembler flags
##############################################################################

# Core C flags for a freestanding 64-bit kernel
CFLAGS := \
    -m64                          \
    -mcmodel=kernel               \
    -mno-red-zone                 \
    -mno-mmx                      \
    -mno-sse                      \
    -mno-sse2                     \
    -ffreestanding                \
    -fno-builtin                  \
    -fno-stack-protector          \
    -fno-pie                      \
    -fno-pic                      \
    -fno-omit-frame-pointer       \
    -Wall                         \
    -Wextra                       \
    -Wshadow                      \
    -std=gnu11                    \
    -O2                           \
    -Iinclude

# Debug build: add -g and disable optimization
ifeq ($(DEBUG), 1)
    CFLAGS += -g -O0
    $(info Debug build enabled)
endif

# NASM flags: 64-bit ELF output
ASFLAGS := -f elf64

# Linker flags: no standard library, use our linker script
LDFLAGS := \
    -T linker.ld                  \
    -nostdlib                     \
    -z max-page-size=0x1000       \
    --no-warn-mismatch

##############################################################################
# Source files
##############################################################################

# Boot assembly sources
BOOT_ASM_SRCS := \
    boot/boot.asm               \
    boot/gdt.c                  \
    interrupts/isr.asm          \
    process/context.asm

# C sources: all .c files in these directories
C_SRCS := \
    kernel/kernel.c             \
    kernel/panic.c              \
    kernel/log.c                \
    kernel/multiboot2.c         \
    memory/pmm.c                \
    memory/vmm.c                \
    memory/heap.c               \
    interrupts/idt.c            \
    interrupts/pic.c            \
    interrupts/handlers.c       \
    drivers/vga.c               \
    drivers/serial.c            \
    drivers/timer.c             \
    drivers/keyboard.c          \
    drivers/ata.c               \
    drivers/framebuffer.c       \
    drivers/mouse.c             \
    drivers/pci.c               \
    drivers/e1000.c             \
    process/process.c           \
    scheduler/scheduler.c       \
    syscall/syscall.c           \
    fs/vfs.c                    \
    fs/ramfs.c                  \
    userland/elf_loader.c       \
    userland/init.c             \
    userland/shell.c            \
    libc/string.c               \
    libc/printf.c               \
    gui/font.c                  \
    gui/draw.c                  \
    gui/event.c                 \
    gui/window_manager.c        \
    gui/desktop.c               \
    apps/terminal.c             \
    apps/filemanager.c          \
    apps/texteditor.c           \
    apps/sysmonitor.c           \
    apps/settings.c             \
    gui/theme.c                 \
    gui/notify.c                \
    net/net.c                   \
    net/ethernet.c              \
    net/ip.c                    \
    net/tcp.c                   \
    kernel/cap.c                \
    kernel/ipc.c                \
    kernel/svcbus.c             \
    kernel/secmon.c             \
    mm/buddy.c                  \
    display/compositor.c        \
    input/input_svc.c           \
    services/launcher.c         \
    services/splash.c           \
    services/login.c

# All object files
BOOT_OBJS := $(patsubst %.asm, $(BUILD)/%.o, \
               $(filter %.asm, $(BOOT_ASM_SRCS)))
BOOT_OBJS += $(patsubst %.c,   $(BUILD)/%.o, \
               $(filter %.c,   $(BOOT_ASM_SRCS)))

C_OBJS    := $(patsubst %.c, $(BUILD)/%.o, $(C_SRCS))

ALL_OBJS  := $(BOOT_OBJS) $(C_OBJS)

##############################################################################
# Build rules
##############################################################################

.PHONY: all run debug clean iso dirs

all: iso

# Create build directory structure
dirs:
	@mkdir -p $(BUILD)/boot
	@mkdir -p $(BUILD)/kernel
	@mkdir -p $(BUILD)/memory
	@mkdir -p $(BUILD)/interrupts
	@mkdir -p $(BUILD)/process
	@mkdir -p $(BUILD)/scheduler
	@mkdir -p $(BUILD)/syscall
	@mkdir -p $(BUILD)/drivers
	@mkdir -p $(BUILD)/fs
	@mkdir -p $(BUILD)/userland
	@mkdir -p $(BUILD)/libc
	@mkdir -p $(BUILD)/gui
	@mkdir -p $(BUILD)/apps
	@mkdir -p $(BUILD)/net
	@mkdir -p $(BUILD)/display
	@mkdir -p $(BUILD)/input
	@mkdir -p $(BUILD)/services
	@mkdir -p $(BUILD)/mm
	@mkdir -p $(GRUBDIR)

# Compile C files
$(BUILD)/%.o: %.c | dirs
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Assemble .asm files (NASM)
$(BUILD)/%.o: %.asm | dirs
	@echo "  AS   $<"
	@$(AS) $(ASFLAGS) $< -o $@

# Link the kernel ELF
$(KERNEL_BIN): $(ALL_OBJS) linker.ld
	@echo "  LD   $@"
	@$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@
	@echo "  Kernel size: $$(du -h $@ | cut -f1)"

# Create bootable ISO with GRUB
iso: $(KERNEL_BIN)
	@echo "  Creating bootable ISO..."
	@cp $(KERNEL_BIN) $(ISODIR)/boot/kernel.elf
	@printf 'set timeout=3\nset default=0\n\nmenuentry "NovOS" {\n    multiboot2 /boot/kernel.elf\n    boot\n}\n' \
	  > $(GRUBDIR)/grub.cfg
	@grub-mkrescue -o $(KERNEL_ISO) $(ISODIR) 2>/dev/null || \
	 grub2-mkrescue -o $(KERNEL_ISO) $(ISODIR) 2>/dev/null || \
	 (echo "ERROR: grub-mkrescue not found. Install grub-common and xorriso." && exit 1)
	@echo "  ISO created: $(KERNEL_ISO)"

##############################################################################
# QEMU run targets
##############################################################################

QEMU := qemu-system-x86_64
QEMU_ARGS := \
    -cdrom $(KERNEL_ISO)          \
    -m 256M                       \
    -vga std                      \
    -serial stdio                 \
    -netdev user,id=net0          \
    -device e1000,netdev=net0     \
    -no-reboot                    \
    -no-shutdown

# Run: boot in QEMU with serial output on stdio
run: iso
	@echo "  Launching QEMU..."
	@echo "  (Serial output appears here; VGA output in QEMU window)"
	@echo "  Press Ctrl+C to quit."
	$(QEMU) $(QEMU_ARGS)

# Run in QEMU without graphical window (serial only)
run-nographic: iso
	$(QEMU) $(QEMU_ARGS) -nographic

# Debug: start QEMU with GDB remote stub
debug: iso
	@echo "  Starting QEMU in debug mode..."
	@echo "  Connect GDB with:"
	@echo "    gdb $(KERNEL_BIN)"
	@echo "    (gdb) target remote localhost:1234"
	@echo "    (gdb) continue"
	$(QEMU) $(QEMU_ARGS) -s -S

# Run with KVM acceleration (Linux hosts with KVM support)
run-kvm: iso
	$(QEMU) $(QEMU_ARGS) -enable-kvm -cpu host

##############################################################################
# Utility targets
##############################################################################

# Disassemble the kernel
disasm: $(KERNEL_BIN)
	x86_64-elf-objdump -d -M intel $(KERNEL_BIN) | less

# Show kernel symbols
symbols: $(KERNEL_BIN)
	x86_64-elf-nm $(KERNEL_BIN) | sort | less

# Show kernel ELF headers
headers: $(KERNEL_BIN)
	x86_64-elf-readelf -a $(KERNEL_BIN) | less

# Check kernel sections
sections: $(KERNEL_BIN)
	x86_64-elf-objdump -h $(KERNEL_BIN)

# Verify multiboot2 header is at the right location
verify: $(KERNEL_BIN)
	@echo "Checking Multiboot2 magic (should appear near start of binary):"
	@x86_64-elf-objdump -s -j .multiboot2 $(KERNEL_BIN) 2>/dev/null || \
	 x86_64-elf-objdump -s $(KERNEL_BIN) | head -40

clean:
	@echo "  Cleaning build artifacts..."
	@rm -rf $(BUILD)
	@echo "  Done."

##############################################################################
# Cross-compiler installation helper
##############################################################################

# Download and build x86_64-elf cross-compiler (takes ~30 min on first run)
# Usage: make cross-compiler
cross-compiler:
	@echo "Building x86_64-elf cross-compiler..."
	@bash scripts/build_cross.sh

##############################################################################
# Dependency generation
##############################################################################

# Auto-generate .d dependency files (included if they exist)
-include $(ALL_OBJS:.o=.d)

$(BUILD)/%.d: %.c | dirs
	@$(CC) $(CFLAGS) -MM -MT $(BUILD)/$*.o $< > $@

.PRECIOUS: $(BUILD)/%.d
