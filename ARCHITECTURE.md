# Aether OS — Architecture & Design Document

## Identity

**Name**: Aether OS
**Version**: 0.1.0 "Genesis"
**Tagline**: *Services. Isolation. Trust.*
**Architecture**: Hybrid Microkernel with Capability-Based Security

---

## Design Manifesto

Aether OS is not a Linux fork, not a Windows clone, and not POSIX-dependent
internally. It is a clean-sheet 64-bit operating system designed around three
inviolable principles:

1. **Every resource access requires a capability token** — there is no ambient
   authority, no root user, no global permission table. If you cannot present
   a valid capability, you cannot touch the resource.

2. **All inter-component communication uses structured message passing** — there
   is no shared global state between services. IPC ports are the nervous system
   of the OS.

3. **Every service is isolated and restartable** — a crashed display service
   does not crash the kernel. A corrupted filesystem service can be restarted
   without rebooting. The kernel is a small, trusted referee.

---

## What Makes Aether OS Different

| Feature               | Linux             | Windows             | **Aether OS**            |
|-----------------------|-------------------|---------------------|--------------------------|
| Architecture          | Monolithic kernel | Hybrid (NT)         | **Hybrid microkernel**   |
| Security model        | UID/GID + DAC     | ACL + UAC           | **Capability tokens**    |
| IPC primitive         | Sockets/pipes     | LPC/ALPC            | **Typed message ports**  |
| Driver model          | Kernel modules    | Kernel drivers      | **User-space services**  |
| Display server        | X11/Wayland       | DWM                 | **Aether Compositor**    |
| Root equivalent       | root (uid=0)      | SYSTEM              | **No root — never**      |
| Crash isolation       | Limited           | Limited             | **Service-level**        |
| Permission model      | FS-based          | Registry + ACL      | **Declared capabilities**|

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    User Applications                     │
│         (sandboxed, declared capability manifest)        │
├─────────────────────────────────────────────────────────┤
│                    Aether SDK / Toolkit                  │
│       (UI framework, IPC helpers, capability API)        │
├──────────────┬──────────────┬───────────────────────────┤
│  Display     │  Input       │  Filesystem   │  Network   │
│  Service     │  Service     │  Service      │  Service   │
│  (Compositor)│  (InputSvc)  │  (VFS)        │  (NetSvc)  │
├──────────────┴──────────────┴───────────────────────────┤
│              Service Bus (svcbus)                        │
│         Name registry + capability distribution          │
├─────────────────────────────────────────────────────────┤
│                    KERNEL BOUNDARY                       │
├──────────────┬──────────────┬───────────────────────────┤
│  IPC Engine  │  Capability  │  Security     │ Scheduler  │
│  (Typed      │  Table       │  Monitor      │ (priority  │
│   messages)  │  (unforgeable│  (secmon)     │  + IPC-    │
│              │   tokens)    │               │  blocking) │
├──────────────┴──────────────┴───────────────────────────┤
│           Memory Management                              │
│   PMM (buddy allocator) + VMM + COW + demand paging     │
├─────────────────────────────────────────────────────────┤
│           Hardware Abstraction                           │
│   Framebuffer │ PS/2 │ PCI │ ATA │ e1000 │ Timer        │
├─────────────────────────────────────────────────────────┤
│              x86_64 Hardware + UEFI/Multiboot2           │
└─────────────────────────────────────────────────────────┘
```

---

## Component Descriptions

### 1. Capability System (`kernel/cap.c`)

The capability system is the foundation of all security in Aether OS.

A **capability** is a kernel-managed token that represents permission to
perform specific operations on a specific resource:

```
cap_id_t = opaque 32-bit token (only the kernel can forge one)

cap_t {
    id       : unique token
    type     : PORT | MEMORY | SERVICE | DEVICE | FILE | TASK | DISPLAY
    rights   : READ | WRITE | EXECUTE | MAP | GRANT | REVOKE | MANAGE
    object   : pointer to the managed resource
    parent   : the capability this was derived from
    owner    : task that holds this capability
}
```

Operations:
- `cap_create(type, rights, object)` — kernel-only, creates root capability
- `cap_derive(cap, reduced_rights)` — create a lesser-rights copy
- `cap_transfer(cap, task)` — move capability to another task
- `cap_revoke(cap)` — invalidate capability (cascades to derived caps)

**There is no way to forge a capability. A task either has one or it doesn't.**

### 2. IPC Engine (`kernel/ipc.c`)

All communication in Aether OS flows through **typed message ports**:

```
Port: a kernel-managed channel endpoint
      guarded by a PORT capability

Message: {
    type       : identifies the protocol
    reply_port : where to send replies (for sync calls)
    caps[]     : capability tokens being transferred
    data[]     : inline payload (≤ 256 bytes)
}
```

Modes:
- **Async** (`ipc_send`): fire-and-forget, returns immediately
- **Sync** (`ipc_call`): blocks until reply arrives
- **Receive** (`ipc_receive`): wait for a message on your port

Zero-copy for large data: send a MEMORY capability pointing to a shared
region instead of copying bytes through the message.

### 3. Service Bus (`kernel/svcbus.c`)

The service bus is the name registry. Services register by name:

```
svcbus_register("aether.display", port, tid, version)
svcbus_lookup("aether.display") → port + capability
```

Well-known service names:
- `aether.display`  — Compositor / display service
- `aether.input`    — Unified input event service
- `aether.vfs`      — Virtual filesystem service
- `aether.network`  — Network stack service
- `aether.audio`    — Audio mixing service

### 4. Security Monitor (`kernel/secmon.c`)

A passive audit component that:
- Logs all capability creations and revocations
- Monitors IPC message flow between services
- Detects policy violations (e.g., non-privileged service acquiring IRQ cap)
- Maintains a tamper-evident audit ring buffer

### 5. Buddy Allocator (`mm/buddy.c`)

Replaces the simple bitmap PMM for larger allocations:
- Orders 0–10 (4 KB to 4 MB blocks)
- O(log n) alloc and free
- Automatic coalescing of freed buddies
- Used by the compositor for surface buffers, DMA, and large heaps

### 6. Aether Compositor (`display/compositor.c`)

Not X11. Not Wayland. Aether's own compositor:
- **Surface-based**: every window is a surface with its own pixel buffer
- **IPC protocol**: windows request surfaces via `MSG_TYPE_DISPLAY` messages
- **Damage tracking**: only redraws regions that changed (efficient)
- **Scene graph**: surfaces have Z-order, opacity, and transform properties
- **Wayland-inspired simplicity**: no legacy display protocol baggage

### 7. Unified Input Service (`input/input_svc.c`)

Keyboard and mouse drivers post raw events to the input service port.
The input service:
- Translates raw scancodes to structured key events
- Dispatches events to the focused surface's task via IPC
- Manages focus (which surface receives input)
- Supports hotkeys and system-level shortcuts

### 8. Application Launcher (`services/launcher.c`)

The shell of Aether OS:
- Application grid with icons
- Integrated search
- System dashboard (clock, battery, network)
- Permission prompt UI (capability request dialogs)

---

## Boot Sequence

```
GRUB/UEFI → boot.asm
           ↓
    Long mode entry
           ↓
    kernel_main()
           ↓
    Phase 1: PMM + VMM + Heap
           ↓
    Phase 2: IDT + PIC + Timer + Interrupts
           ↓
    Phase 3: Drivers (keyboard, mouse, framebuffer, PCI)
           ↓
    Phase 4: AETHER KERNEL LAYER
             ├── cap_table_init()   (capability table)
             ├── ipc_init()         (IPC engine)
             ├── svcbus_init()      (service registry)
             ├── secmon_init()      (security monitor)
             └── buddy_init()       (buddy allocator)
           ↓
    Phase 5: CORE SERVICES
             ├── compositor_init()  (display service starts)
             ├── input_svc_init()   (input service starts)
             └── launcher_init()    (launcher starts)
           ↓
    → Graphical desktop with IPC-managed windows
```

---

## IPC Protocol Reference

### Display Protocol (MSG_TYPE_DISPLAY = 0x3000)

| Code   | Name                  | Description                        |
|--------|-----------------------|------------------------------------|
| 0x3001 | CREATE_SURFACE        | Allocate a new surface             |
| 0x3002 | DESTROY_SURFACE       | Release surface resources          |
| 0x3003 | SET_SURFACE_GEOMETRY  | Move/resize surface                |
| 0x3004 | SET_SURFACE_VISIBLE   | Show/hide surface                  |
| 0x3005 | COMMIT_SURFACE        | Signal render complete, flip       |
| 0x3006 | SET_SURFACE_TITLE     | Update surface/window title        |
| 0x3007 | RAISE_SURFACE         | Bring surface to front             |
| 0x3008 | GET_DISPLAY_INFO      | Query screen dimensions/format     |

### Input Protocol (MSG_TYPE_INPUT = 0x4000)

| Code   | Name                  | Description                        |
|--------|-----------------------|------------------------------------|
| 0x4001 | KEY_DOWN              | Key pressed event                  |
| 0x4002 | KEY_UP                | Key released event                 |
| 0x4003 | MOUSE_MOVE            | Pointer moved                      |
| 0x4004 | MOUSE_BUTTON          | Button press/release               |
| 0x4005 | FOCUS_IN              | Surface gained input focus         |
| 0x4006 | FOCUS_OUT             | Surface lost input focus           |
| 0x4007 | REGISTER_SURFACE      | Register surface for input events  |

---

## Capability Type Reference

| Type              | Value | Description                              |
|-------------------|-------|------------------------------------------|
| CAP_TYPE_NULL     | 0     | Null / placeholder                       |
| CAP_TYPE_MEMORY   | 1     | Physical/virtual memory region           |
| CAP_TYPE_PORT     | 2     | IPC message port endpoint                |
| CAP_TYPE_SERVICE  | 3     | Service reference (via service bus)      |
| CAP_TYPE_DEVICE   | 4     | Hardware device (PCI, etc.)              |
| CAP_TYPE_FILE     | 5     | File or directory object                 |
| CAP_TYPE_TASK     | 6     | Task/process reference                   |
| CAP_TYPE_DISPLAY  | 7     | Display surface pixel buffer             |
| CAP_TYPE_IRQLINE  | 8     | Hardware interrupt line                  |

---

## Future Roadmap

- **v0.2**: True user-space drivers (ring 3 services with MMU isolation)
- **v0.3**: ARM64 port (architecture-abstracted boot layer)
- **v0.4**: RISC-V port
- **v0.5**: Persistent storage service with integrity verification
- **v1.0**: Self-hosting (Aether can build Aether)

---

*"A system that cannot explain why it gave you access is a system that
cannot explain why it denied you access."*
— Aether OS Design Principle #1
