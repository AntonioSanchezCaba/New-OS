# NovOS - 64-bit x86_64 Operating System

A fully functional, 64-bit operating system written in C and x86_64 Assembly from scratch. Boots on real hardware or QEMU via Multiboot2/GRUB.

---

## Architecture Overview

### Boot Process

```
BIOS/UEFI
   └─► GRUB (Multiboot2)
          └─► boot.asm (_start, 32-bit protected mode)
                 ├─ Verify Multiboot2 magic
                 ├─ Set up 4-level page tables (identity + higher-half map)
                 │    PML4[0]   → PDPT → PD (identity map 0–1GB)
                 │    PML4[511] → PDPT → PD (kernel at 0xFFFFFFFF80000000)
                 ├─ Enable PAE + Long Mode (EFER.LME) + Paging
                 ├─ Load 64-bit GDT
                 └─► long_mode_entry (64-bit)
                        ├─ Set up segment registers
                        ├─ Switch to kernel virtual stack
                        └─► kernel_main()
```

### Virtual Memory Layout

```
Virtual Address Space (x86_64, 48-bit):
┌──────────────────────────────────────┐ 0xFFFFFFFFFFFFFFFF
│  Kernel heap                         │ 0xFFFFFFFF90000000 - 0xFFFFFFFFA0000000
│  Kernel image (text/data/bss)        │ 0xFFFFFFFF80100000
│  Kernel VMA base                     │ 0xFFFFFFFF80000000
├──────────────────────────────────────┤
│  (non-canonical hole)                │
├──────────────────────────────────────┤ 0x00007FFFFFFFFFFF
│  User stack (grows down)             │ 0x00007FFFFFFFE000
│  User heap  (grows up via brk)       │
│  User ELF segments (text/data/bss)   │ 0x0000000000400000
│  (not mapped)                        │ 0x0000000000000000
└──────────────────────────────────────┘
```

### Memory Management

**Physical Memory Manager (PMM)** — `memory/pmm.c`
- Bitmap allocator: 1 bit per 4KB frame
- Initialized from Multiboot2 memory map
- `pmm_alloc_frame()` / `pmm_free_frame()`

**Virtual Memory Manager (VMM)** — `memory/vmm.c`
- 4-level page tables: PML4 → PDPT → PD → PT
- Supports 2MB huge pages for early kernel mapping
- `vmm_map_page()`, `vmm_clone_address_space()` (for fork)
- Copy-on-write address space cloning

**Kernel Heap** — `memory/heap.c`
- Free-list allocator with block coalescing
- `kmalloc()`, `kfree()`, `krealloc()`, `kcalloc()`
- Expands by mapping new physical frames on demand

### Interrupt Handling

```
CPU Exception / IRQ fires
   └─► ISR stub (isr.asm) — saves all GP registers to stack frame
          └─► interrupt_dispatch() (handlers.c)
                 ├─► handle_exception() — CPU exceptions 0–31
                 │    └─► vmm_handle_page_fault() for #PF
                 ├─► handle_irq() — hardware IRQs 32–47
                 │    ├─ IRQ0 → timer_irq_handler()
                 │    ├─ IRQ1 → keyboard_irq_handler()
                 │    └─ IRQ14/15 → ATA interrupts
                 └─► syscall_handler() — int 0x80
```

- **IDT**: 256 gates, 64-bit interrupt gates
- **PIC**: Intel 8259 remapped to vectors 0x20–0x2F
- **GDT/TSS**: 7-entry GDT with 64-bit TSS for kernel stack on ring-3 interrupts

### Process Scheduler

**Round-Robin Preemptive Scheduler** — `scheduler/scheduler.c`
- Circular ready queue (array-based, O(1) enqueue/dequeue)
- Time quantum: 5 timer ticks (50ms at 100Hz)
- Preemption: PIT timer IRQ calls `scheduler_tick()` every 10ms
- Context switch saves/restores: RBX, RBP, R12–R15, RSP, RIP, RFLAGS

**Process Control Block** — `process/process.c`
- Unique PID, parent PID, process name
- Separate kernel stack (16KB) per process
- Own page table (PML4) for address space isolation
- File descriptor table (64 entries)
- States: CREATED → READY → RUNNING → SLEEPING/WAITING → ZOMBIE → DEAD

**Context Switching** — `process/context.asm`
- `context_switch(old, new)`: saves current context to `old`, loads `new`
- `switch_to_usermode(rip, rsp)`: transitions to ring 3 via IRETQ

### System Calls (int 0x80)

| # | Name    | Args                    | Description              |
|---|---------|-------------------------|--------------------------|
| 0 | read    | fd, buf, count          | Read from file/stdin     |
| 1 | write   | fd, buf, count          | Write to file/stdout     |
| 2 | open    | path, flags, mode       | Open file                |
| 3 | close   | fd                      | Close file descriptor    |
|10 | fork    | —                       | Clone process            |
|11 | exec    | path, argv, envp        | Execute ELF binary       |
|12 | exit    | code                    | Terminate process        |
|13 | getpid  | —                       | Get current PID          |
|14 | sleep   | ms                      | Sleep for N milliseconds |
|15 | kill    | pid, sig                | Kill process             |
| 9 | brk     | new_brk                 | Set heap end (sbrk)      |
|23 | yield   | —                       | Voluntary CPU yield      |

### Filesystem (VFS + ramfs)

**VFS Layer** — `fs/vfs.c`
- Unified interface for any backing filesystem
- Path resolution with `/`-separated components
- Standard operations: open, close, read, write, readdir, mkdir, create, unlink

**RAM Filesystem** — `fs/ramfs.c`
- In-memory filesystem backed by kernel heap
- Files stored as `uint8_t*` arrays (grow dynamically)
- Directories store up to 64 children
- Pre-populated at boot: `/bin`, `/etc`, `/dev`, `/proc`, `/tmp`, `/home`

### Drivers

| Driver     | File                  | Description                          |
|------------|-----------------------|--------------------------------------|
| VGA        | `drivers/vga.c`       | 80×25 text mode, scrolling, color    |
| Keyboard   | `drivers/keyboard.c`  | PS/2 scancode set 1, ring buffer     |
| Timer      | `drivers/timer.c`     | PIT 8254, 100Hz, drives scheduler    |
| Serial     | `drivers/serial.c`    | 16550 UART, 115200 baud, debug out   |
| ATA        | `drivers/ata.c`       | ATA PIO, LBA28/LBA48, 4 drives       |

---

## Directory Structure

```
NovOS/
├── boot/           Multiboot2 header, 32→64 mode transition, GDT
├── kernel/         Kernel main, panic handler, logging
├── memory/         PMM (bitmap), VMM (4-level paging), heap allocator
├── interrupts/     IDT setup, PIC driver, ISR stubs, exception handlers
├── process/        PCB management, ELF loader, context switching
├── scheduler/      Round-robin preemptive scheduler
├── syscall/        System call table and implementations
├── drivers/        VGA, keyboard, timer, serial, ATA
├── fs/             VFS layer, RAM filesystem
├── userland/       Init process, interactive shell
├── libc/           Freestanding string.h, printf implementations
├── include/        All public headers
├── scripts/        Cross-compiler build helper
├── Makefile        Build system
└── linker.ld       Kernel linker script
```

---

## Build Instructions

### Prerequisites

Install cross-compiler and build tools:

```bash
# Ubuntu / Debian
sudo apt install build-essential bison flex libgmp-dev libmpc-dev \
                 libmpfr-dev texinfo libisl-dev nasm grub-common \
                 xorriso qemu-system-x86

# macOS (Homebrew)
brew install gmp mpfr libmpc nasm xorriso qemu
brew install x86_64-elf-gcc  # Available in OSDev tap
```

### Build the cross-compiler (first time)

```bash
bash scripts/build_cross.sh
export PATH="$HOME/cross/bin:$PATH"
```

### Build NovOS

```bash
make          # Build kernel.elf and novos.iso
make run      # Build and launch in QEMU
```

### QEMU Run Command (manual)

```bash
qemu-system-x86_64 \
    -cdrom build/novos.iso \
    -m 256M               \
    -serial stdio         \
    -no-reboot            \
    -no-shutdown
```

### Debug with GDB

```bash
make debug    # Starts QEMU paused, GDB server on port 1234
# In another terminal:
x86_64-elf-gdb build/kernel.elf
(gdb) target remote localhost:1234
(gdb) break kernel_main
(gdb) continue
```

---

## Shell Commands

Once booted, the interactive shell supports:

| Command          | Description                      |
|------------------|----------------------------------|
| `help`           | List all commands                |
| `ls [path]`      | List directory contents          |
| `cat <file>`     | Display file contents            |
| `echo [text...]` | Print text to screen             |
| `ps`             | List all running processes       |
| `kill <pid>`     | Kill a process by PID            |
| `clear`          | Clear the screen                 |
| `cd <path>`      | Change directory                 |
| `pwd`            | Print current directory          |
| `mkdir <path>`   | Create a directory               |
| `touch <file>`   | Create an empty file             |
| `rm <file>`      | Remove a file                    |
| `uptime`         | Show system uptime               |
| `mem`            | Show memory usage statistics     |
| `halt`           | Halt the system                  |
| `reboot`         | Reboot via keyboard controller   |

---

## Implementation Notes

### Why Multiboot2?
Multiboot2 is the easiest way to get GRUB to load a kernel without writing a full bootloader. GRUB handles the BIOS/UEFI complexity and gives us a clean 32-bit protected mode environment to start from.

### Higher-half Kernel
The kernel is linked at `0xFFFFFFFF80100000` but physically loaded at `0x100000`. This separates kernel virtual memory from user virtual memory, preventing user processes from directly accessing kernel code/data.

### No Red Zone
The `-mno-red-zone` flag is critical for kernel code. The "red zone" is a 128-byte area below RSP that userland code can use freely. Interrupts would corrupt this area in kernel mode without this flag.

### No SSE/MMX
We disable SSE/MMX/AVX with `-mno-sse -mno-mmx` because these instructions require the FPU to be initialized (CR0.EM must be clear, CR4.OSFXSR must be set), and they use XMM registers that we don't save/restore in our context switch code.

### Locking
Spinlocks and critical sections use `cli`/`sti` to disable interrupts. For a proper SMP kernel, atomic operations and per-CPU structures would be needed.
