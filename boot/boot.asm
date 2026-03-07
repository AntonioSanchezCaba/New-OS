;;; boot.asm - Multiboot2 entry point and long mode transition
;;;
;;; This file is the very first code that GRUB jumps to after loading our
;;; kernel. We arrive in 32-bit protected mode with:

;;; Boot page table physical addresses (hardcoded low conventional memory).
;;; Using 0x1000-0x7000 avoids any ELF NOLOAD / .boot.bss addressing issues
;;; in v86/copy.sh. These pages are within the 0-6MB identity map.
%define BOOT_PML4       0x1000
%define BOOT_PDPT_LOW   0x2000
%define BOOT_PDPT_HIGH  0x3000
%define BOOT_PD         0x4000
%define BOOT_PT0        0x5000
%define BOOT_PT1        0x6000
%define BOOT_PT2        0x7000
;;;   EAX = 0x36D76289 (Multiboot2 magic)
;;;   EBX = physical address of Multiboot2 info structure
;;;
;;; Our job here is to:
;;;   1. Validate the multiboot2 magic
;;;   2. Save the multiboot info pointer
;;;   3. Set up initial page tables (identity map + higher-half map)
;;;   4. Enable PAE + Long Mode + Paging
;;;   5. Load a 64-bit GDT
;;;   6. Far-jump into 64-bit mode
;;;   7. Call kernel_main in 64-bit code

%define KERNEL_VMA_OFFSET 0xFFFFFFFF80000000

;;; =========================================================
;;; Multiboot2 Header
;;; Must be within the first 32KB of the kernel image.
;;; =========================================================
section .multiboot2
align 8

mb2_header_start:
    dd  0xE85250D6                               ; Multiboot2 magic
    dd  0                                        ; Architecture: i386 (protected mode)
    dd  mb2_header_end - mb2_header_start        ; Header length
    dd  -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start)) ; Checksum

    ;; EFI amd64 entry address tag (type 9, optional)
    ;; When GRUB is running as a 64-bit EFI application it calls _start_efi64
    ;; directly in long mode, bypassing the entire EFER.LME dance.
    ;; If GRUB is a BIOS application it ignores this tag (flags bit 0 = optional).
    align 8
    dw  9                                        ; Tag type: EFI amd64 entry addr
    dw  1                                        ; flags bit 0: optional
    dd  12                                       ; Tag size
    dd  (_start_efi64 - KERNEL_VMA_OFFSET)       ; Physical entry address

    align 8
    ;; End tag (required)
    dw  0
    dw  0
    dd  8
mb2_header_end:

;;; =========================================================
;;; 32-bit boot code
;;; =========================================================
section .boot.text
[BITS 32]

;;; VGA diagnostic: write char CH (with attribute AT) to row 24 col COL
;;; Usage: %vga_diag CH, AT, COL   (all literals, no registers clobbered except via ecx/edi)
;;; Safe to use before paging (physical 0xB8000 accessible in 32-bit PM)
%macro vga_diag 3    ; CH, ATTR, COL
    mov  word [0xB8000 + (24 * 80 + %3) * 2], (%2 << 8) | %1
%endmacro

global _start
_start:
    ;; Disable interrupts immediately
    cli

    ;; VGA diag col 0: '1' cyan-on-black = 32-bit entry reached
    vga_diag '1', 0x0B, 0

    ;; === Early serial debug: write 'A' to COM1 (0x3F8) ===
    ;; Works even without full UART init — BIOS/GRUB leave UART usable.
    ;; IMPORTANT: Must not clobber EAX (multiboot2 magic) or EBX (info ptr).
    ;; Stack is not set up yet, so use ECX as scratch.
    mov ecx, eax
    mov dx, 0x3F8
    mov al, 'A'
    out dx, al
    mov eax, ecx

    ;; Verify Multiboot2 magic
    cmp eax, 0x36D76289
    jne .no_multiboot

    ;; Debug: 'B' — multiboot2 magic OK
    mov dx, 0x3F8
    mov al, 'B'
    out dx, al

    ;; Save multiboot2 info pointer (EBX) to physical memory location.
    ;; boot.bss symbols have VMA = physical (no KERNEL_VMA_OFFSET), use directly.
    mov [multiboot_info_ptr], ebx

    ;; Set up the initial 32-bit stack (in boot BSS)
    mov esp, boot_stack_top

    ;; Check that CPUID is supported
    call check_cpuid
    jc .no_cpuid

    ;; Long mode check skipped: v86/copy.sh CPUID does not expose the LM bit
    ;; even though v86 fully supports 64-bit execution.  GRUB loaded our
    ;; Multiboot2 ELF64 kernel, which is only possible on a 64-bit CPU.

    ;; Debug: 'C' — CPU checks passed
    mov dx, 0x3F8
    mov al, 'C'
    out dx, al

    ;; Set up initial page tables for:
    ;;   a) Identity-map of first 2GB (so boot code can continue running)
    ;;   b) Higher-half map at 0xFFFFFFFF80000000 -> physical 0
    call setup_page_tables

    ;; Debug: 'D' — page tables set up
    mov dx, 0x3F8
    mov al, 'D'
    out dx, al
    ;; VGA diag col 1: '2' = page tables written
    vga_diag '2', 0x0B, 1

    ;; Enable PAE (Physical Address Extension) - required for long mode
    ;; (no-op if we're already in IA-32e; harmless to set again)
    mov eax, cr4
    or  eax, (1 << 5)   ; CR4.PAE
    mov cr4, eax

    ;; Load PML4 into CR3 (hardcoded low physical address).
    ;; If already in IA-32e compat mode this switches from GRUB's page tables
    ;; to ours; our tables identity-map 0-6MB so the next fetch works fine.
    mov eax, BOOT_PML4
    mov cr3, eax

    ;; === Check if we are already in IA-32e paging mode ===
    ;; If GRUB entered us in 32-bit compatibility mode (LMA=1, CR0.PG=1) we
    ;; must NOT touch EFER or repeat the CR0.PG write — WRMSR(EFER) with PG=1
    ;; causes #GP on real hardware and v86 appears to silently clear EFER on
    ;; return from that #GP, breaking everything.  Instead, just proceed to the
    ;; GDT load and far return.
    mov eax, cr0
    test eax, (1 << 31)     ; CR0.PG already set?
    jnz .paging_already_on  ; yes → already in IA-32e, skip EFER + CR0.PG

    ;; ---------------------------------------------------------------
    ;; 32-bit protected mode path: need to activate long mode ourselves
    ;; ---------------------------------------------------------------
    vga_diag 'P', 0x0B, 2  ; col 2: 'P' = entered as 32-bit PM (PG was 0)

    ;; Set EFER.LME via read-modify-write (proper sequence per Intel spec).
    ;; We read first so any existing bits (NXE, SCE, etc.) are preserved.
    ;; If v86/copy.sh raises #GP for WRMSR when CPUID[80000001h][29]=0,
    ;; GRUB's IDT handler returns via IRET → EFER unchanged → we see 'N'.
    mov ecx, 0xC0000080
    rdmsr                       ; Read current EFER into EDX:EAX
    or  eax, (1 << 8)           ; Set LME bit
    wrmsr                       ; Write back

    ;; Diagnostic-only readback to verify whether WRMSR was honoured.
    mov ecx, 0xC0000080
    rdmsr
    test eax, (1 << 8)
    jz  .efer_readback_zero
    vga_diag 'L', 0x0A, 3   ; col 3: 'L' = RDMSR confirmed LME set
    jmp .efer_cont
.efer_readback_zero:
    vga_diag 'N', 0x0E, 3   ; col 3: 'N' = RDMSR returned 0 (possible #GP)
.efer_cont:

    ;; Verify PML4[0] value (flags=0x07)
    mov eax, [BOOT_PML4]
    cmp eax, (BOOT_PDPT_LOW | 7)
    je  .pml4_ok
    vga_diag 'W', 0x0C, 4   ; col 4: 'W' = PML4[0] wrong
    jmp .do_cr0_pg
.pml4_ok:
    vga_diag 'V', 0x0A, 4   ; col 4: 'V' = PML4[0] verified correct
.do_cr0_pg:

    ;; Enable paging → activates IA-32e long mode (EFER.LMA becomes 1)
    mov eax, cr0
    or  eax, (1 << 31)  ; CR0.PG = 1
    or  eax, (1 << 0)   ; CR0.PE = 1 (already set by GRUB)
    mov cr0, eax

    ;; If we reach here paging is now active
    vga_diag '4', 0x0A, 5   ; col 5: '4' = CR0.PG set, IA-32e paging active
    jmp .paging_done

    ;; ---------------------------------------------------------------
    ;; IA-32e compatibility mode path: GRUB already set up paging/LM
    ;; ---------------------------------------------------------------
.paging_already_on:
    ;; We are in 32-bit IA-32e compatibility mode (LMA=1, CR0.PG=1).
    ;; CR3 was just loaded with our PML4 above.  No EFER/CR0.PG manipulation
    ;; needed — long mode is already active.
    vga_diag 'C', 0x0D, 2  ; col 2: 'C' = compat mode at entry (PG was 1)
    vga_diag 'K', 0x0D, 3  ; col 3: 'K' = CR3 switched, long mode already on
    vga_diag '4', 0x0A, 5  ; col 5: '4' = paging confirmed active

.paging_done:
    ;; Load our 64-bit GDT (gdt64_ptr is in .boot.data, VMA = physical)
    lgdt [gdt64_ptr]

    ;; Debug: 'f' — GDT loaded, about to far-jump to 64-bit CS
    mov dx, 0x3F8
    mov al, 'f'
    out dx, al

    ;; Transition to 64-bit code segment via far return (retf).
    ;; This is equivalent to jmp 0x08:(long_mode_entry - KERNEL_VMA_OFFSET)
    ;; but avoids the 0xEA opcode which some emulators mishandle in
    ;; compatibility mode. retf pops EIP then CS from the stack.
    push dword 0x08
    push dword (long_mode_entry - KERNEL_VMA_OFFSET)
    retf

.no_multiboot:
    mov esi, msg_no_multiboot   ; .boot.data: physical = VMA, no offset needed
    jmp boot_error32

.no_cpuid:
    mov esi, msg_no_cpuid
    jmp boot_error32


;;; =========================================================
;;; boot_serial_puts32 - write a string to COM1 (32-bit)
;;; ESI = pointer to null-terminated string
;;; Clobbers: EAX, EDX, ESI
;;; =========================================================
boot_serial_puts32:
    mov dx, 0x3F8 + 5   ; LSR
.wait_tx:
    in  al, dx
    test al, 0x20       ; THRE bit
    jz  .wait_tx
    mov dx, 0x3F8
    lodsb
    test al, al
    jz   .done
    out  dx, al
    jmp  boot_serial_puts32
.done:
    ret

;;; =========================================================
;;; check_cpuid - verify CPUID instruction is available
;;; Returns: CF set on failure
;;; =========================================================
check_cpuid:
    ;; Try to flip bit 21 in EFLAGS (ID flag)
    pushfd
    pop  eax
    mov  ecx, eax
    xor  eax, (1 << 21)
    push eax
    popfd
    pushfd
    pop  eax
    push ecx
    popfd
    xor  eax, ecx
    jz   .no_cpuid      ; If bit didn't change, CPUID not supported
    clc
    ret
.no_cpuid:
    stc
    ret

;;; =========================================================
;;; check_long_mode - verify 64-bit long mode is available
;;; Returns: CF set on failure
;;; =========================================================
check_long_mode:
    ;; CPUID with EAX=0x80000000 to get extended function support
    mov  eax, 0x80000000
    cpuid
    cmp  eax, 0x80000001
    jb   .no_long_mode   ; Extended functions not supported

    ;; CPUID with EAX=0x80000001 to check for long mode bit
    mov  eax, 0x80000001
    cpuid
    test edx, (1 << 29)  ; Bit 29 = LM (Long Mode)
    jz   .no_long_mode
    clc
    ret
.no_long_mode:
    stc
    ret

;;; =========================================================
;;; setup_page_tables
;;; Sets up 4-level paging with standard 4KB pages (no huge pages).
;;; Maps 0-6MB identity and at 0xFFFFFFFF80000000 (higher half).
;;; 6MB covers all kernel sections including BSS (~4.8MB max LMA).
;;;
;;; Structure:
;;;   PML4[0]   -> PDPT_LOW[0]   -> PD[0..2] -> PT0, PT1, PT2
;;;   PML4[511] -> PDPT_HIGH[510]-> PD[0..2] -> PT0, PT1, PT2
;;;   PT[n] = (n * 4096) | 3  (identity-mapped 4KB pages, 0..6MB)
;;; =========================================================
setup_page_tables:
    cld             ; Ensure forward direction for stosd

    ;; Clear 7 pages: PML4, PDPT_LOW, PDPT_HIGH, PD, PT0, PT1, PT2
    mov edi, BOOT_PML4
    xor eax, eax
    mov ecx, (7 * 4096) / 4
    rep stosd

    ;; ---- PML4 ----
    ;; Flags: P=1, RW=1, U/S=1 (0x07) — U/S=1 ensures no supervisor-only
    ;; checks interfere with v86's paging implementation.
    mov eax, BOOT_PDPT_LOW
    or  eax, 7                          ; Present + RW + U/S
    mov [BOOT_PML4], eax                ; PML4[0] -> PDPT_LOW

    mov eax, BOOT_PDPT_HIGH
    or  eax, 7
    mov [BOOT_PML4 + 511*8], eax        ; PML4[511] -> PDPT_HIGH

    ;; ---- PDPTs (both point to the same PD) ----
    mov eax, BOOT_PD
    or  eax, 7
    mov [BOOT_PDPT_LOW], eax            ; PDPT_LOW[0] -> PD

    ;; PDPT_HIGH[510] -> same PD (VA 0xFFFFFFFF80000000 -> PML4[511], PDPT[510])
    mov [BOOT_PDPT_HIGH + 510*8], eax   ; PDPT_HIGH[510] -> PD

    ;; ---- PD entries (4KB page tables, no PS/huge bit) ----
    mov eax, BOOT_PT0
    or  eax, 7
    mov [BOOT_PD], eax                  ; PD[0] -> PT0 (covers 0..2MB)

    mov eax, BOOT_PT1
    or  eax, 7
    mov [BOOT_PD + 8], eax              ; PD[1] -> PT1 (covers 2MB..4MB)

    mov eax, BOOT_PT2
    or  eax, 7
    mov [BOOT_PD + 16], eax             ; PD[2] -> PT2 (covers 4MB..6MB)

    ;; ---- Fill PT0 + PT1 + PT2 with identity-mapped 4KB entries ----
    ;; PT[n] = (n * 4096) | 7, for n = 0 .. 3*512-1 (P+RW+U/S)
    mov edi, BOOT_PT0
    mov eax, 0x007          ; PA=0, Present+RW+U/S
    mov ecx, 3 * 512        ; 1536 entries across 3 PTs
.fill_pts:
    mov  [edi], eax         ; Write lower 32 bits (upper 32 = 0 from clear)
    add  edi, 8             ; Advance to next 8-byte entry slot
    add  eax, 0x1000        ; Next 4KB physical page
    loop .fill_pts

    ret

;;; =========================================================
;;; boot_error32 - print an error string and halt (32-bit)
;;; ESI = pointer to null-terminated string (physical address)
;;; =========================================================
boot_error32:
    ;; Write error to serial first (visible in graphical mode)
    push esi
    call boot_serial_puts32
    pop esi

    ;; Also write to VGA text buffer (in case of text mode)
    mov edi, 0xB8000    ; VGA text buffer
    mov ah,  0x4F       ; White on red
.loop:
    lodsb
    test al, al
    jz   .halt
    stosw
    jmp  .loop
.halt:
    cli
    hlt
    jmp .halt

;;; =========================================================
;;; Error messages (physical addresses in .boot.data)
;;; =========================================================
section .boot.data

msg_no_multiboot:   db "ERROR: Not loaded by Multiboot2 bootloader!", 0
msg_no_cpuid:       db "ERROR: CPUID not supported by this CPU!", 0
msg_no_long_mode:   db "ERROR: 64-bit Long Mode not supported by this CPU!", 0

;;; =========================================================
;;; 64-bit GDT
;;; =========================================================
align 8
gdt64:
    ;; Null descriptor (entry 0)
    dq 0

    ;; Kernel code segment (selector 0x08)
    ;; Base=0, Limit=0, 64-bit, DPL=0, Execute/Read
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)

    ;; Kernel data segment (selector 0x10)
    ;; Base=0, Limit=0, 64-bit, DPL=0, Read/Write
    dq (1 << 41) | (1 << 44) | (1 << 47)

    ;; User data segment (selector 0x18, DPL=3)
    dq (1 << 41) | (1 << 44) | (1 << 47) | (3 << 45)

    ;; User code segment (selector 0x20, DPL=3)
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53) | (3 << 45)

    ;; TSS descriptor placeholder (16 bytes, filled in by gdt_init())
gdt64_tss_lo: dq 0
gdt64_tss_hi: dq 0

gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1   ; Limit
    dq gdt64                    ; Base — gdt64 is in .boot.data (VMA = physical)

;;; =========================================================
;;; Page tables (4KB aligned, in boot BSS)
;;; =========================================================
section .boot.bss nobits
align 4096

global boot_pml4
boot_pml4:
    resb 4096

boot_pdpt_low:
    resb 4096

boot_pdpt_high:
    resb 4096

boot_pd:
    resb 4096

boot_pt0:
    resb 4096

boot_pt1:
    resb 4096

boot_pt2:
    resb 4096

;;; Boot stack (16KB)
align 16
boot_stack_bottom:
    resb 16384
boot_stack_top:

;;; Multiboot info pointer (saved by 32-bit code)
global multiboot_info_ptr
multiboot_info_ptr:
    resq 1

;;; =========================================================
;;; 64-bit long mode entry
;;; =========================================================
section .text
[BITS 64]

extern kernel_main

;;; =========================================================
;;; _start_efi64 — Multiboot2 EFI amd64 entry (tag type 9)
;;;
;;; Called by GRUB when it is running as a 64-bit EFI application.
;;; Machine state at entry:
;;;   RAX = Multiboot2 magic (0x36D76289)
;;;   RBX = physical address of Multiboot2 info structure
;;;   64-bit long mode active, GRUB's own paging in effect
;;;   Interrupts may be enabled; GRUB's GDT/IDT still loaded
;;;
;;; We must:
;;;   1. Switch to a safe temporary stack (avoids dependency on GRUB stack)
;;;   2. Set up our page tables at 0x1000-0x7000
;;;   3. Switch CR3 to our PML4
;;;   4. Load our 64-bit GDT and reload CS/DS
;;;   5. Set up the real kernel stack and call kernel_main
;;; =========================================================
global _start_efi64
_start_efi64:
    cli                         ; Disable interrupts — we're about to switch GDT/CR3

    ;; VGA diag col 0: 'E' = EFI64 entry (vs '1' for 32-bit PM entry)
    mov word [0xB8000 + (24 * 80 + 0) * 2], (0x0A << 8) | 'E'

    ;; Save multiboot info pointer (RBX) before clobbering anything.
    ;; multiboot_info_ptr lives in .boot.bss; its LMA = VMA = physical address.
    mov [multiboot_info_ptr], rbx

    ;; Switch to a temporary stack in low conventional memory.
    ;; Page tables occupy 0x1000-0x7FFF; use 0x8FF0 as a safe 16-byte-aligned top.
    ;; This avoids any dependency on GRUB's stack after we switch page tables.
    mov rsp, 0x8FF0

    ;; Set up our page tables (identity 0-6MB + higher-half at 0xFFFFFFFF80000000)
    call setup_page_tables_64

    ;; VGA diag col 1: '2' = page tables built
    mov word [0xB8000 + (24 * 80 + 1) * 2], (0x0B << 8) | '2'

    ;; Switch to our PML4 (GRUB's paging replaced; our identity map keeps
    ;; the next instruction fetch valid since this code is within 0-6MB).
    mov rax, BOOT_PML4
    mov cr3, rax

    ;; Load our 64-bit GDT (physical address; still identity-mapped now)
    lgdt [gdt64_ptr]

    ;; Far return to reload CS = 0x08 (our 64-bit code segment).
    ;; push RIP first, then CS; o64 retf pops 64-bit RIP then 16-bit CS.
    lea  rax, [rel .cs_reloaded]
    push qword 0x08
    push rax
    o64 retf

.cs_reloaded:
    ;; Update all data segment registers to kernel data selector
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ;; Switch to the real kernel stack (virtual higher-half address)
    mov rsp, kernel_stack_top

    ;; Reload GDT pointer using virtual address (higher-half mapping now active)
    lgdt [gdt64_ptr_virt]

    ;; VGA diag col 5: '4' = long mode fully set up (mirrors 32-bit PM path)
    mov word [0xB8000 + (24 * 80 + 5) * 2], (0x0A << 8) | '4'

    ;; Clear frame pointer for stack-trace termination
    xor rbp, rbp

    ;; First argument: multiboot2 info pointer, adjusted to virtual address
    mov rdi, [multiboot_info_ptr]
    add rdi, KERNEL_VMA_OFFSET

    ;; Jump into the common 64-bit entry (sets up segments, VGA diag col 6, etc.)
    jmp long_mode_entry

;;; =========================================================
;;; setup_page_tables_64 — 64-bit version of setup_page_tables
;;;
;;; Sets up the same 4-level paging structure as the 32-bit version:
;;;   Identity map 0-6MB (low half)
;;;   Higher-half map 0xFFFFFFFF80000000 → physical 0
;;; Uses rep stosq (not stosd) for proper 64-bit clearing.
;;; Safe to call before or after CR3 switch.
;;; Clobbers: RAX, RCX, RDI
;;; =========================================================
setup_page_tables_64:
    ;; Clear 7 pages: PML4, PDPT_LOW, PDPT_HIGH, PD, PT0, PT1, PT2
    mov rdi, BOOT_PML4
    xor rax, rax
    mov rcx, (7 * 4096) / 8    ; 64-bit clear (stosq = 8 bytes per iteration)
    rep stosq

    ;; ---- PML4 ----
    mov rax, BOOT_PDPT_LOW
    or  rax, 7                          ; Present + RW + U/S
    mov [BOOT_PML4], rax                ; PML4[0] -> PDPT_LOW

    mov rax, BOOT_PDPT_HIGH
    or  rax, 7
    mov [BOOT_PML4 + 511*8], rax        ; PML4[511] -> PDPT_HIGH

    ;; ---- PDPTs ----
    mov rax, BOOT_PD
    or  rax, 7
    mov [BOOT_PDPT_LOW], rax            ; PDPT_LOW[0] -> PD
    mov [BOOT_PDPT_HIGH + 510*8], rax   ; PDPT_HIGH[510] -> PD

    ;; ---- PD entries ----
    mov rax, BOOT_PT0
    or  rax, 7
    mov [BOOT_PD], rax                  ; PD[0] -> PT0

    mov rax, BOOT_PT1
    or  rax, 7
    mov [BOOT_PD + 8], rax              ; PD[1] -> PT1

    mov rax, BOOT_PT2
    or  rax, 7
    mov [BOOT_PD + 16], rax             ; PD[2] -> PT2

    ;; ---- Fill PTs: identity-mapped 4KB pages 0..6MB ----
    mov rdi, BOOT_PT0
    mov rax, 0x007                      ; PA=0, Present+RW+U/S
    mov rcx, 3 * 512                    ; 1536 entries across 3 page tables
.fill_pts_64:
    mov  [rdi], rax
    add  rdi, 8
    add  rax, 0x1000
    loop .fill_pts_64

    ret

long_mode_entry:
    ;; VGA diag col 6: '5' = 64-bit long mode reached
    mov word [0xB8000 + (24 * 80 + 6) * 2], (0x0D << 8) | '5'
    ;; Debug: 'F' — 64-bit long mode reached!
    mov dx, 0x3F8
    mov al, 'F'
    out dx, al

    ;; We're now in 64-bit long mode!
    ;; Update all segment registers to use the kernel data segment
    mov ax, 0x10        ; Kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ;; Set up the kernel stack at its virtual address
    mov rsp, kernel_stack_top

    ;; Reload the GDT with virtual address
    lgdt [gdt64_ptr_virt]

    ;; Clear RBP for stack trace termination
    xor rbp, rbp

    ;; Load multiboot2 info pointer into RDI (first argument).
    ;; multiboot_info_ptr lives in .boot.bss (physical address, identity-mapped).
    ;; Access via identity map and add higher-half offset.
    mov rdi, [multiboot_info_ptr]
    add rdi, KERNEL_VMA_OFFSET

    ;; Debug: 'G' — about to call kernel_main
    push rdi
    mov dx, 0x3F8
    mov al, 'G'
    out dx, al
    pop rdi

    ;; Jump to kernel main (must be done via absolute address in higher half)
    call kernel_main

    ;; If kernel_main returns (shouldn't happen), halt
.halt:
    cli
    hlt
    jmp .halt

;;; Virtual-address GDT pointer (for post-paging reload)
gdt64_ptr_virt:
    dw gdt64_end - gdt64 - 1
    dq gdt64                    ; This is the VIRTUAL address of gdt64

;;; =========================================================
;;; Kernel stack (64KB, in kernel BSS)
;;; =========================================================
section .bss
align 16

global kernel_stack_bottom, kernel_stack_top
kernel_stack_bottom:
    resb 65536          ; 64KB kernel stack
kernel_stack_top:

; Mark stack as non-executable (suppresses GNU-stack linker warning)
section .note.GNU-stack noalloc noexec nowrite progbits
