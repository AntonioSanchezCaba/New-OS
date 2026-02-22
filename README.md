# AetherOS v1.0.0 — "Genesis"

> **Services. Isolation. Trust.**

AetherOS is a 64-bit x86_64 desktop operating system written from scratch in C
and NASM assembly. It is not a Linux fork, not a POSIX re-implementation, and
not a teaching toy. It is a clean-sheet hybrid microkernel OS with a
capability-based security model, a custom compositor, and a complete graphical
desktop environment.

---

## What Is AetherOS?

AetherOS is built on three inviolable principles:

1. **Every resource access requires a capability token** — there is no ambient
   authority, no root user, no global permission table.
2. **All inter-component communication uses structured message passing** — no
   shared global state between services; IPC ports are the nervous system.
3. **Every service is isolated and restartable** — a crashed display service
   does not crash the kernel.

The v1.0.0 "Genesis" release is the first feature-complete milestone: a fully
bootable graphical desktop with a working window manager, compositor, and a
suite of bundled applications.

---

## Architecture at a Glance

```
┌──────────────────────────────────────────────────────────────┐
│                      User Applications                        │
│             (Terminal, Files, Editor, Settings...)            │
├──────────────────────────────────────────────────────────────┤
│                      GUI Toolkit / SDK                        │
│          (draw.c, theme.c, widgets.c, window.c)               │
├─────────────┬─────────────┬──────────────┬───────────────────┤
│  Compositor │  Input Svc  │  VFS Service │  Network Service  │
│  (display/) │  (input/)   │  (fs/)       │  (net/)           │
├─────────────┴─────────────┴──────────────┴───────────────────┤
│                   Service Bus (svcbus)                        │
├─────────────┬──────────────┬─────────────┬───────────────────┤
│  IPC Engine │  Capability  │   Security  │   Scheduler       │
│  (ipc.c)    │  Table (cap) │   Monitor   │   (Round-Robin)   │
├─────────────┴──────────────┴─────────────┴───────────────────┤
│         Memory: PMM (buddy) + VMM (4-level paging) + Heap     │
├──────────────────────────────────────────────────────────────┤
│     Drivers: Framebuffer │ PS/2 │ PCI │ ATA │ e1000 │ Timer  │
├──────────────────────────────────────────────────────────────┤
│                  x86_64 + GRUB / Multiboot2                   │
└──────────────────────────────────────────────────────────────┘
```

---

## Genesis Feature Set (v1.0.0)

### Kernel

| Subsystem          | Status | Notes                                          |
|--------------------|--------|------------------------------------------------|
| x86_64 long mode   | ✅     | 4-level page tables, higher-half kernel        |
| Memory management  | ✅     | PMM bitmap + buddy, VMM COW, free-list heap    |
| IDT / IRQ          | ✅     | PIC 8259, timer, keyboard, mouse               |
| Scheduler          | ✅     | Round-robin preemptive, 100 Hz PIT             |
| Processes          | ✅     | fork/exec/waitpid, credentials, signal state   |
| Signals            | ✅     | 32 POSIX signals, sigaction, sigprocmask       |
| Syscall table      | ✅     | 50+ syscalls (int 0x80, 6-arg convention)      |
| Capability system  | ✅     | Unforgeable tokens, derive, revoke, transfer   |
| IPC engine         | ✅     | Typed message ports, sync/async, zero-copy     |
| Service bus        | ✅     | Name registry, capability distribution         |
| Shared memory      | ✅     | Named regions, ref-counted, multi-space map    |
| TTY/PTY            | ✅     | Line discipline, cooked/raw, PTY pairs         |
| Package manager    | ✅     | .aur archive format, CRC32, DB persistence     |

### Filesystem

| Driver             | Status | Notes                                          |
|--------------------|--------|------------------------------------------------|
| VFS layer          | ✅     | Unified interface, stat, readdir, mount        |
| ramfs              | ✅     | In-memory, dynamic growth, pre-populated tree  |
| ext2               | ✅     | Read/write, indirect blocks, in-memory image   |
| FAT32              | ✅     | BPB parsing, FAT chain, LFN skeleton           |

### Networking

| Component          | Status | Notes                                          |
|--------------------|--------|------------------------------------------------|
| e1000 driver       | ✅     | Intel Gigabit (QEMU emulated)                  |
| ARP                | ✅     | Request/reply, cache                           |
| IP layer           | ✅     | Fragmentation, routing                         |
| ICMP               | ✅     | Ping echo request/reply                        |
| UDP                | ✅     | Sockets, send/recv                             |
| TCP                | ✅     | 3-way handshake, data transfer, teardown       |
| BSD socket API     | ✅     | socket/bind/connect/listen/accept/send/recv    |

### Graphics & Desktop

| Component          | Status | Notes                                          |
|--------------------|--------|------------------------------------------------|
| Framebuffer        | ✅     | Double-buffered, 32bpp ARGB, damage tracking   |
| Compositor         | ✅     | Surface-based, Z-order, alpha blend, shadows   |
| Window manager     | ✅     | Move, resize, minimize, focus, Z-raise         |
| Theme engine       | ✅     | Dark/light, 4 accents, 60+ named color fields  |
| Widget toolkit     | ✅     | Button, Label, TextInput, CheckBox, ProgressBar|
| Boot animation     | ✅     | Integer sine table, smooth fade-in/out phases  |
| Boot splash        | ✅     | Branded progress bar, phase labels             |
| Login screen       | ✅     | Username/password fields, blinking cursor      |
| Desktop + Taskbar  | ✅     | Start menu, virtual workspaces, system tray    |
| Notifications      | ✅     | Toast popups, auto-dismiss, typed severity     |

### Applications

| Application        | Key Features                                                  |
|--------------------|---------------------------------------------------------------|
| Terminal           | 22 built-in commands, history (↑/↓), VFS navigation          |
| File Manager       | Two-pane, sidebar, double-click nav, delete confirmation      |
| Text Editor        | Multi-line, line numbers, gutter, cursor, scrollback          |
| System Monitor     | CPU %, memory bar, live process list                          |
| Settings           | Display, system stats, about tabs; theme/accent switching     |
| Calculator         | 4-function, %, negate, keyboard input                         |
| Clock              | Analog + digital face, uptime display                         |
| Stress Test        | 6 scheduler workers, fairness bars, heap statistics           |

---

## Boot Flow

```
BIOS/UEFI
  └─► GRUB (Multiboot2)
         └─► boot.asm  (32-bit → 64-bit long mode transition)
                └─► kernel_main()
                       ├─ Phase 1: PMM + VMM + Heap
                       ├─ Phase 2: IDT + PIC + Timer + Interrupts
                       ├─ Phase 3: Drivers (FB, keyboard, mouse, PCI, net)
                       ├─ Phase 4: Kernel services (cap, ipc, svcbus, secmon)
                       ├─ Phase 5: Core services (compositor, input, launcher)
                       └─► gui_run()
                              ├─ splash_run()  — AetherOS boot splash
                              ├─ login_run()   — graphical login
                              └─► Desktop loop (~60 FPS)
```

---

## Building AetherOS

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install build-essential bison flex libgmp-dev libmpc-dev \
                 libmpfr-dev texinfo libisl-dev nasm grub-common \
                 xorriso qemu-system-x86

# macOS
brew install gmp mpfr libmpc nasm xorriso qemu
```

### Cross-compiler (first time only)

```bash
bash scripts/build_cross.sh
export PATH="$HOME/cross/bin:$PATH"
```

### Build and Run

```bash
make              # Build kernel.elf + aetheros.iso
make run          # Build and launch in QEMU (graphical window)
make debug        # QEMU paused + GDB server on :1234
```

### Manual QEMU Launch

```bash
qemu-system-x86_64          \
  -cdrom build/aetheros.iso \
  -m 256M                   \
  -vga std                  \
  -serial stdio             \
  -netdev user,id=net0      \
  -device e1000,netdev=net0 \
  -no-reboot -no-shutdown
```

The VGA window shows the graphical desktop. Serial/stdio shows kernel debug logs.

---

## Repository Structure

```
New-IOS/
├── boot/           Multiboot2 header, 32→64 mode, GDT/TSS setup
├── kernel/         kernel_main, panic, logging, cap, ipc, svcbus, secmon
│                   pkg (package manager), shm, tty, signal
├── memory/         PMM (bitmap + buddy), VMM (4-level), heap allocator
├── interrupts/     IDT, PIC, ISR stubs, exception handlers
├── process/        PCB, ELF loader, context switch, fork/exec/waitpid
├── scheduler/      Round-robin preemptive scheduler
├── syscall/        Syscall table (50+ entries), all implementations
├── drivers/        Framebuffer, keyboard, mouse, timer, serial, ATA, e1000
├── fs/             VFS layer, ramfs, ext2, fat32
├── net/            ARP, IP, ICMP, UDP, TCP, BSD socket API
├── display/        Compositor, surface management, damage tracking
├── input/          Input service, focus management, event dispatch
├── gui/            Window manager, draw primitives, theme, widgets, notify
├── services/       Boot animation, splash, login, launcher
├── apps/           Terminal, file manager, text editor, system monitor,
│                   settings, calculator, clock, stress test
├── userland/       Init process, interactive shell
├── libc/           Freestanding string.h, printf, vsnprintf
├── include/        All public headers (kernel/version.h is canonical)
├── scripts/        Cross-compiler build helper
├── Makefile        Build system
└── linker.ld       Kernel linker script
```

---

## Versioning

All branding strings are defined in a single header:

```c
// include/kernel/version.h  — single source of truth
#define OS_NAME      "AetherOS"
#define OS_VERSION   "1.0.0"
#define OS_RELEASE   "Genesis"
#define OS_TAGLINE   "Services. Isolation. Trust."
#define OS_COPYRIGHT "© 2026 AetherOS Project"
```

No subsystem, application, or service may hardcode these values. All must
`#include <kernel/version.h>` and use the macros.

---

## Roadmap

| Version | Milestone                                                         |
|---------|-------------------------------------------------------------------|
| v1.1    | User-space driver model (ring-3 services with MMU isolation)      |
| v1.2    | Persistent ext4 filesystem, boot from real disk                   |
| v1.3    | Audio service (PCM mixing, beep driver)                           |
| v2.0    | ARM64 port (Raspberry Pi 4/5 target)                              |
| v2.1    | RISC-V port                                                       |
| v3.0    | Self-hosting: AetherOS builds AetherOS                            |

---

## License

© 2026 AetherOS Project — Open Source

---

*"A system that cannot explain why it gave you access is a system that cannot
explain why it denied you access."*
— AetherOS Design Principle #1
