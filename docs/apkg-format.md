# AetherOS Package Format — `.apkg` Specification

Version 2 · AetherOS Application Runtime

---

## Overview

`.apkg` is the native binary package format for AetherOS.  A package bundles
a statically-linked ELF64 executable with metadata, dependency declarations,
and a **capability manifest** that names every kernel resource the application
is permitted to access.

The kernel enforces the manifest at launch time.  An application that does not
declare a capability cannot obtain one — there is no ambient authority, no
`root`, no privilege escalation path.

---

## File Layout

```
┌─────────────────────────────┐  offset 0
│       apkg_header_t         │  (variable size, see below)
├─────────────────────────────┤  offset sizeof(apkg_header_t)
│  apkg_dep_t[dep_count]      │  dep_count × 48 bytes
├─────────────────────────────┤  offset = header + dep table
│  uint8_t payload[payload_size] │  raw ELF64 executable
└─────────────────────────────┘
```

---

## Header (`apkg_header_t`)

All multi-byte integers are **little-endian**.  The struct is packed
(`__attribute__((packed))`).

| Offset | Size | Field            | Description                          |
|--------|------|------------------|--------------------------------------|
| 0      | 4    | `magic`          | `"APKG"` (0x41 0x50 0x4B 0x47)      |
| 4      | 2    | `fmt_version`    | Format version: `1` (legacy) or `2` |
| 6      | 2    | `flags`          | Reserved, must be 0                  |
| 8      | 32   | `name`           | Package name, NUL-terminated         |
| 40     | 16   | `version`        | Semver string, e.g. `"1.2.0"`       |
| 56     | 128  | `description`    | Short description, NUL-terminated    |
| 184    | 32   | `author`         | Author name/email, NUL-terminated    |
| 216    | 1    | `dep_count`      | Number of dependency entries (0–8)   |
| 217    | 3    | `_pad`           | Reserved, must be 0                  |
| 220    | 4    | `payload_size`   | Byte size of the ELF payload         |
| 224    | 4    | `payload_crc32`  | ISO 3309 CRC32 of payload bytes      |
| 228    | 1    | `cap_count`      | Declared capabilities (0–5) **v2**  |
| 229    | 3    | `_cap_pad`       | Reserved, must be 0                  |
| 232    | 40   | `caps[5]`        | Capability declarations **v2**       |
| 272    | 4    | `_reserved`      | Reserved, must be 0                  |

**Version compatibility:** v1 packages have `cap_count = 0` (reserved bytes
were zero-initialised) and are accepted by the v2 loader.  They run fully
sandboxed with no capability grants.

---

## Dependency Entry (`apkg_dep_t`) — 48 bytes each

| Offset | Size | Field     | Description                          |
|--------|------|-----------|--------------------------------------|
| 0      | 32   | `name`    | Required package name, NUL-term      |
| 32     | 16   | `min_ver` | Minimum semver string, e.g. `"1.0.0"`|

The installer verifies that every listed dependency is already in the package
database at or above `min_ver` before recording the installation.

---

## Capability Declaration (`apkg_cap_decl_t`) — 8 bytes each

| Offset | Size | Field    | Description                            |
|--------|------|----------|----------------------------------------|
| 0      | 1    | `type`   | `cap_type_t` — resource class          |
| 1      | 3    | `_pad`   | Reserved, must be 0                    |
| 4      | 4    | `rights` | `cap_rights_t` bitmask                 |

### Capability Types (`cap_type_t`)

| Value | Name              | Resource                          |
|-------|-------------------|-----------------------------------|
| 0     | `CAP_TYPE_NULL`   | Placeholder / skip                |
| 1     | `CAP_TYPE_MEMORY` | Physical / virtual memory region  |
| 2     | `CAP_TYPE_PORT`   | IPC message-port endpoint         |
| 3     | `CAP_TYPE_SERVICE`| Service-bus entry                 |
| 4     | `CAP_TYPE_DEVICE` | Hardware device (PCI BAR, etc.)   |
| 5     | `CAP_TYPE_FILE`   | File or directory object          |
| 6     | `CAP_TYPE_TASK`   | Task / process reference          |
| 7     | `CAP_TYPE_DISPLAY`| Display surface pixel buffer      |
| 8     | `CAP_TYPE_IRQLINE`| Hardware interrupt line           |

### Rights Bitmask (`cap_rights_t`)

| Bit | Constant             | Meaning                        |
|-----|----------------------|--------------------------------|
| 0   | `CAP_RIGHT_READ`     | Read / receive                 |
| 1   | `CAP_RIGHT_WRITE`    | Write / send                   |
| 2   | `CAP_RIGHT_EXECUTE`  | Invoke / execute               |
| 3   | `CAP_RIGHT_MAP`      | Map into address space         |
| 4   | `CAP_RIGHT_GRANT`    | Transfer to another task       |
| 5   | `CAP_RIGHT_REVOKE`   | Revoke this capability         |
| 6   | `CAP_RIGHT_MANAGE`   | Derive sub-capabilities        |

---

## Payload

The payload is a **statically-linked ELF64** (`ET_EXEC`) executable targeting
`EM_X86_64`.  Dynamic linking is not supported.

The kernel loader (`apkg_exec`):
1. Validates ELF magic, class (64-bit), endianness (LE), type (EXEC), arch (x86-64).
2. Maps all `PT_LOAD` segments into a fresh page-table address space with
   correct `PTE_PRESENT | PTE_USER` flags; `PF_W` → `PTE_WRITABLE`;
   absence of `PF_X` → `PTE_NX`.
3. Zeroes BSS (`p_memsz > p_filesz`).
4. Allocates 8 pages (32 KiB) user stack at `0x7FFFFFFFF000` downward.
5. Sets `CS = 0x23` (ring-3 code), `SS = 0x1B` (ring-3 data), `RFLAGS = 0x202`.
6. Grants capability tokens from the manifest, storing them in
   `process_t.cap_ids[]`.
7. Enqueues the process with the scheduler.

---

## Install Flow

```
apkg_repo_scan("/sys/packages/cache")
    ↓ reads header → fills g_catalog[]

apkg_repo_install_by_name("myapp")
    ↓ find in catalog
    ↓ read .apkg file into heap buffer
    ↓ validate magic + version (1 or 2)
    ↓ check CRC32 of payload
    ↓ check dependencies in g_db[]
    ↓ record in g_db[], persist CSV to /sys/pkg/db
    returns 0

apkg_exec("myapp")
    ↓ verify installed
    ↓ locate .apkg path in catalog
    ↓ read file, locate payload
    ↓ save cap manifest (type[], rights[]) before freeing buffer
    ↓ process_create()
    ↓ elf_load() — maps PT_LOAD segments
    ↓ allocate + map user stack
    ↓ for each cap_decl: cap_create() → proc->cap_ids[]
    ↓ scheduler_add()
    returns pid
```

---

## Shell Commands

```
install <name>   Install a package from /sys/packages/cache
run <name>       Launch an installed package (creates process)
packages         List all installed packages
repo             List packages available in the repo cache
                 (* marks already-installed packages)
```

---

## Demo: Install and Run

Assuming `/sys/packages/cache/hello.apkg` exists with format v2 and one
`CAP_TYPE_DISPLAY | CAP_RIGHT_WRITE` declaration:

```
aether> repo
1 package(s) available:
    hello                 v1.0.0      caps:1   Hello World demo app
  (* = installed)

aether> install hello
install: fetching 'hello'...
install: 'hello' installed successfully

aether> packages
1 package(s) installed:
  hello                 v1.0.0        AetherTeam

aether> run hello
run: 'hello' launched as pid 4
```

Kernel log output during `run hello`:

```
[APKG] exec: granted 1/1 capability token(s) to 'hello' (pid=4)
[APKG] exec: launched 'hello' (pid=4, entry=0x400000)
[ELF]  loaded segment v=0x400000-0x401000 (filesz=256, memsz=256)
[ELF]  loaded binary, entry=0x400000, heap_start=0x401000
```

---

## Building a v2 Package (host-side tool sketch)

```c
/* Pseudo-code for a package builder */
apkg_header_t hdr = {0};
memcpy(hdr.magic, "APKG", 4);
hdr.fmt_version   = 2;
strncpy(hdr.name, "hello", sizeof(hdr.name)-1);
strncpy(hdr.version, "1.0.0", sizeof(hdr.version)-1);
strncpy(hdr.author, "AetherTeam", sizeof(hdr.author)-1);
strncpy(hdr.description, "Hello World demo app", sizeof(hdr.description)-1);

/* Declare one capability: write access to the display */
hdr.cap_count     = 1;
hdr.caps[0].type  = CAP_TYPE_DISPLAY;   /* 7 */
hdr.caps[0].rights= CAP_RIGHT_WRITE;    /* 0x02 */

/* Load ELF payload */
uint8_t *elf_data = read_file("hello.elf");
size_t   elf_size = file_size("hello.elf");
hdr.payload_size  = (uint32_t)elf_size;
hdr.payload_crc32 = apkg_crc32(elf_data, elf_size);

/* Write: header + payload */
FILE* out = fopen("hello.apkg", "wb");
fwrite(&hdr,     sizeof(hdr), 1, out);
/* dep table omitted (dep_count=0) */
fwrite(elf_data, elf_size,    1, out);
fclose(out);
```
