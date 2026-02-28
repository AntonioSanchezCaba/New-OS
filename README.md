# AetherOS — x86_64 Kernel

> **Services. Isolation. Trust.**

AetherOS is a 64-bit x86_64 operating system written from scratch in C and NASM
assembly. It is not a Linux fork and not a POSIX re-implementation. It aims for a
capability-based security model, structured IPC, and a graphical desktop
environment.

This document gives an honest, current assessment of what is implemented and
what the status of each subsystem is.

---

## Status Legend

| Tag        | Meaning                                                        |
|------------|----------------------------------------------------------------|
| **STABLE** | Fully implemented, compiles, boots, does what it claims        |
| **PARTIAL**| Core path works; edge cases, write-back, or integration absent |
| **STUB**   | Scaffolding only — initialises, logs, but does not function    |

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  User Applications  (all ring 0 for now — no user-space yet) │
│             (Terminal, Files, Editor, Settings...)            │
├──────────────────────────────────────────────────────────────┤
│                 Aether Render Engine (ARE)                    │
│        Compositor · Window manager · Widget toolkit          │
├─────────────┬─────────────┬──────────────┬───────────────────┤
│  Cap table  │  IPC ports  │   svcbus     │  secmon           │
├─────────────┴─────────────┴──────────────┴───────────────────┤
│         Memory: PMM (bitmap) + VMM (4-level) + Heap           │
│         Secondary: buddy allocator (mm/buddy.c)               │
├─────────────┬─────────────┬──────────────┬───────────────────┤
│  Scheduler  │   Signals   │   Syscalls   │   TTY/PTY         │
├─────────────┴─────────────┴──────────────┴───────────────────┤
│     Drivers: Framebuffer │ PS/2 │ PCI │ ATA │ e1000 │ Timer  │
├──────────────────────────────────────────────────────────────┤
│                  x86_64 + GRUB / Multiboot2                   │
└──────────────────────────────────────────────────────────────┘
```

> **Note:** There is no ring-3 user-space yet. All code — kernel, drivers,
> applications — runs at CPL 0. Process security is conceptual at this stage.

---

## Subsystem Status

### Boot & CPU

| Subsystem            | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| x86_64 long mode     | **STABLE**  | boot.asm: CR4/EFER/PML4 → 64-bit before C entry   |
| Higher-half kernel   | **STABLE**  | Linked at `0xFFFFFFFF80100000`, identity map kept  |
| GDT / TSS            | **STABLE**  | Flat segments + 64-bit TSS with kernel stack       |
| IDT (256 vectors)    | **STABLE**  | All CPU exceptions + IRQ 0-15 handled              |
| PIC 8259A            | **STABLE**  | IRQs remapped to 0x20-0x2F; EOI correct            |
| Multiboot2 parsing   | **STABLE**  | Memory map, framebuffer tag, module tags           |

### Memory

| Subsystem            | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| PMM (bitmap)         | **STABLE**  | Frame allocator from Multiboot2 memory map         |
| VMM (4-level paging) | **STABLE**  | Map/unmap/COW; identity + higher-half              |
| Kernel heap          | **STABLE**  | Free-list allocator, `kmalloc` / `kfree`           |
| Buddy allocator      | **STABLE**  | 16 MB secondary pool; power-of-two blocks          |

### Interrupts & Scheduling

| Subsystem            | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| Timer (PIT 100 Hz)   | **STABLE**  | Drives preemptive scheduler; sleep / callback API  |
| Keyboard (PS/2)      | **STABLE**  | Scan-code set 1 → ASCII; extended keys             |
| Mouse (PS/2)         | **STABLE**  | 3-button + scroll; software cursor                 |
| Scheduler            | **STABLE**  | Round-robin preemptive; yield; sleep               |
| Process management   | **STABLE**  | PCB, `process_create`, `waitpid`, `exit`           |
| Signals              | **STABLE**  | 32 POSIX signals; `sigaction`, `sigprocmask`       |
| TTY / PTY            | **STABLE**  | Line discipline; canonical + raw; PTY pairs        |
| Syscall table        | **STABLE**  | 50+ calls via `int 0x80`; 6-arg convention         |

### Security & IPC

| Subsystem            | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| Capability table     | **STABLE**  | Unforgeable tokens; derive, revoke, transfer       |
| IPC message ports    | **STABLE**  | Typed messages; sync / async; queue per port       |
| Service bus          | **STABLE**  | Name registry; capability distribution             |
| Shared memory        | **STABLE**  | Named regions; ref-counted; kernel-window map      |
| Security monitor     | **STABLE**  | Audit ring buffer; capability violation logging    |

### Drivers

| Driver               | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| VGA text mode        | **STABLE**  | 80×25 @ 0xB8000; color attributes                 |
| Serial (COM1)        | **STABLE**  | 115200 baud; used for kernel debug log             |
| Framebuffer          | **STABLE**  | 32bpp ARGB; double-buffered; damage tracking       |
| ATA (PIO mode)       | **STABLE**  | Primary/secondary bus; 28-bit LBA; block cache     |
| PCI bus scan         | **STABLE**  | Type-0 config space; device enumeration            |
| e1000 NIC            | **STABLE**  | Intel Gigabit (QEMU emulated); Tx/Rx ring buffers  |
| USB (UHCI)           | **STUB**    | Controller detected; no device class drivers       |
| UEFI GOP             | **STABLE**  | Multiboot2 framebuffer tag parsed; no UEFI calls   |

### Filesystem

| Driver               | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| VFS layer            | **STABLE**  | open/read/write/close/readdir/mkdir/unlink/mount   |
| ramfs                | **STABLE**  | In-memory; dynamic growth; pre-populated tree      |
| ext2                 | **PARTIAL** | Read-write on in-memory image; no write-back to disk|
| FAT32                | **PARTIAL** | BPB parse; FAT chain walk; LFN read; limited write |
| procfs               | **PARTIAL** | `/proc` stub entries; read-only; no live data yet  |
| Disk manager         | **STABLE**  | MBR scan; FAT32/ext2 auto-mount under `/mnt/`      |
| Block cache          | **STABLE**  | LRU sector cache shared across ATA + filesystems   |

### Networking

| Component            | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| ARP                  | **STABLE**  | Request/reply; 16-entry cache                      |
| IPv4                 | **STABLE**  | Checksum; basic fragmentation; routing             |
| ICMP                 | **STABLE**  | Echo request/reply (ping)                          |
| UDP                  | **STABLE**  | Send/recv; port demultiplexing                     |
| TCP                  | **PARTIAL** | 3-way handshake; send/recv; no retransmit timer    |
| BSD socket API       | **PARTIAL** | `socket/bind/connect/send/recv`; AF\_INET only     |
| DHCP                 | **PARTIAL** | DISCOVER/OFFER/REQUEST/ACK; IPv4 only              |
| DNS                  | **PARTIAL** | A-record query; no caching; no AAAA                |

### Graphics & Desktop

| Component            | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| Aether Render Engine | **STABLE**  | Surface-based compositor; Z-order; 60 FPS loop     |
| Window manager       | **STABLE**  | Move, resize, focus, Z-raise                       |
| Widget toolkit       | **STABLE**  | Button, Label, TextInput, CheckBox, ProgressBar    |
| Theme engine         | **STABLE**  | Dark/light; accent colours; `gui/theme.c`          |
| Notifications        | **STABLE**  | Toast popups; auto-dismiss; severity levels        |
| Wallpaper engine     | **PARTIAL** | Static colour fill; no BMP/PNG loader yet          |
| Window animations    | **PARTIAL** | Alpha-fade only; no slide/scale yet                |
| Boot animation       | **STABLE**  | Integer sine table; fade-in / progress / fade-out  |
| Boot splash          | **STABLE**  | Branded progress bar; step labels                  |
| Login screen         | **STABLE**  | Username / password fields; blinking cursor        |
| Desktop + taskbar    | **STABLE**  | Start menu; virtual workspaces; system tray slots  |
| Clipboard            | **STUB**    | API present; no cross-window data transfer yet     |

### Applications

All applications run in ring 0 (kernel space). No user-space isolation.

| Application          | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| Terminal             | **STABLE**  | 22 built-in commands; history; VFS navigation      |
| File Manager         | **STABLE**  | Two-pane; sidebar; delete confirmation             |
| Text Editor          | **STABLE**  | Multi-line; line numbers; scroll                   |
| System Monitor       | **STABLE**  | CPU %; memory bar; process list                    |
| Settings             | **STABLE**  | Display, system stats, about tabs                  |
| Calculator           | **STABLE**  | 4-function; %; keyboard input                      |
| Clock                | **STABLE**  | Analog + digital face; uptime                      |
| Stress test          | **STABLE**  | 6 scheduler workers; fairness bars                 |
| Image viewer         | **STUB**    | Window created; no image decode                    |
| Network config       | **STUB**    | UI skeleton only                                   |

### Package Managers

| Component            | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| apkg                 | **PARTIAL** | `.aur` archive format; install/list; CRC32         |
| pkg (.aur resolver)  | **STUB**    | Placeholder; no resolver or download logic         |

---

## Boot Flow

```
BIOS/UEFI
  └─► GRUB (Multiboot2)
         └─► boot.asm  (real mode → protected → 64-bit long mode)
                └─► kernel_main()
                       │
                       ├─ Phase 1: Serial + VGA  (output only)
                       │
                       ├─ Phase 2: GDT/TSS + IDT + PIC       ← CPU structures FIRST
                       │          (exceptions now safe to take)
                       │
                       ├─ Phase 3: PMM + VMM + Heap
                       │
                       ├─ Phase 4: Timer + Keyboard + ATA
                       │           Framebuffer + Mouse + PCI + e1000
                       │
                       ├─ Phase 5: Cap table + secmon + IPC + svcbus + shm + buddy
                       │
                       ├─ Phase 6: VFS + ramfs + diskman + procfs
                       │
                       ├─ Phase 7: process_init + scheduler_init + TTY
                       │           users + apkg + pkg
                       │
                       ├─ Phase 8: syscall_init
                       │
                       ├─ Phase 9: cpu_sti()  → scheduler now preempts
                       │           init_userland() → spawns init process
                       │
                       └─► are_run()  (Aether Render Engine — does not return)
                              ├─ boot animation (~700 ms)
                              └─► Desktop render loop @ 60 FPS
```

**Structured boot log** — each subsystem prints a `[BOOT] <name> OK` line to
serial after successful init. Connect a serial console or read QEMU stdio output
to verify the sequence.

---

## Known Limitations

1. **No ring-3 user-space.** All application code runs at privilege level 0.
   Process isolation is logically enforced by the scheduler but not by the MMU.
2. **ext2 is in-memory only.** The driver reads the partition into a heap buffer
   at boot. Writes do not flush back to disk unless `ext2_flush_all()` is called
   (done on shutdown via `diskman_shutdown()`).
3. **TCP has no retransmit timer.** The handshake and data transfer work in the
   QEMU user-mode network, which is lossless. A real network with packet loss
   will stall.
4. **USB has no device class drivers.** UHCI controller is detected; HID/mass
   storage require additional work.
5. **Cross-compiler required.** The Makefile enforces `x86_64-elf-gcc`. If not
   installed, pass `make ALLOW_SYSTEM_GCC=1` to use the system `gcc` with
   freestanding flags (works on Ubuntu x86\_64 hosts).

---

## Building

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install build-essential bison flex libgmp-dev libmpc-dev \
                 libmpfr-dev texinfo libisl-dev nasm grub-common \
                 xorriso qemu-system-x86

# Cross-compiler (first time only, ~30 min)
bash scripts/build_cross.sh
export PATH="$HOME/cross/bin:$PATH"
```

### Quick build

```bash
make                     # Build kernel.elf
make iso                 # Build bootable ISO (needs grub-mkrescue)
make run                 # Build ISO and launch QEMU

# If x86_64-elf-gcc is not installed:
make ALLOW_SYSTEM_GCC=1
```

### QEMU flags (manual)

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

Serial output on stdio shows structured `[BOOT]` checkpoints and kernel log.
The VGA/SDL window shows the graphical desktop.

---

## Repository Layout

```
New-IOS/
├── boot/           Multiboot2 header, 32→64 mode transition
├── kernel/         kernel_main, panic, log, cap, ipc, svcbus, secmon,
│                   shm, tty, signal, diskman, users, apkg, pkg
├── memory/         PMM (bitmap), VMM (4-level paging), heap allocator
├── mm/             Buddy allocator
├── interrupts/     IDT setup, PIC, ISR stubs, exception handlers
├── process/        PCB, context switch, ELF loader
├── scheduler/      Round-robin preemptive scheduler
├── syscall/        Syscall table + all 50+ implementations
├── drivers/        Framebuffer, keyboard, mouse, timer, serial,
│                   ATA, e1000, PCI, USB (stub), UEFI GOP, cursor
├── fs/             VFS, ramfs, ext2, FAT32, procfs, block cache
├── net/            ARP, IP, ICMP, UDP, TCP, DHCP, DNS, BSD sockets
├── display/        Compositor, surface management, damage tracking
├── input/          Input event service
├── aether/         ARE: surface, field, context, input, UI layer
├── surfaces/       ARE-native app surfaces (terminal, explorer, …)
├── gui/            Draw primitives, theme, widgets, notify, wallpaper,
│                   animation, workspace, clipboard, font, desktop
├── services/       Boot animation, splash, login, launcher
├── apps/           Terminal, file manager, text editor, system monitor,
│                   settings, calculator, clock, stress test,
│                   image viewer (stub), network config (stub),
│                   fs_demo, net_demo, process_demo (integration tests)
├── userland/       Init process, interactive shell
├── libc/           Freestanding string.h, printf, vsnprintf
├── include/        All public headers
├── scripts/        Cross-compiler build helper
├── Makefile        Build system
└── linker.ld       Kernel linker script (higher-half, multiboot2)
```

---

## Versioning

```c
// include/kernel/version.h — single source of truth
#define OS_NAME      "AetherOS"
#define OS_VERSION   "1.0.0"
#define OS_RELEASE   "Genesis"
#define OS_TAGLINE   "Services. Isolation. Trust."
```

---

## License

© 2026 AetherOS Project — Open Source

---

*"A system that cannot explain why it gave you access is a system that cannot
explain why it denied you access."*
— AetherOS Design Principle #1
