;;; boot.asm - Multiboot2 entry point and long mode transition
;;;
;;; This file is the very first code that GRUB jumps to after loading our
;;; kernel. We arrive in 32-bit protected mode with:
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

global _start
_start:
    ;; Disable interrupts immediately
    cli

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

    ;; Enable PAE (Physical Address Extension) - required for long mode
    mov eax, cr4
    or  eax, (1 << 5)   ; CR4.PAE
    mov cr4, eax

    ;; Load PML4 into CR3 (boot_pml4 physical address = VMA for boot.bss)
    mov eax, boot_pml4
    mov cr3, eax

    ;; Enable Long Mode via the EFER MSR
    mov ecx, 0xC0000080  ; EFER MSR number
    rdmsr
    or  eax, (1 << 8)   ; EFER.LME = 1
    ;; NXE (bit 11) intentionally NOT set — v86 may not support it
    wrmsr

    ;; Debug: 'E' — long mode configured
    mov dx, 0x3F8
    mov al, 'E'
    out dx, al

    ;; Enable paging (which also activates long mode)
    mov eax, cr0
    or  eax, (1 << 31)  ; CR0.PG = 1
    or  eax, (1 << 0)   ; CR0.PE = 1 (should already be set by GRUB)
    mov cr0, eax

    ;; Debug: 'e' — paging+long mode now active (compatibility mode)
    mov dx, 0x3F8
    mov al, 'e'
    out dx, al

    ;; Load our 64-bit GDT (gdt64_ptr is in .boot.data, physical = VMA)
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
;;;
;;; 4KB pages are used because v86/copy.sh does not support 2MB
;;; huge-page (PS-bit) PDEs in IA-32e mode — enabling paging with
;;; huge pages causes an immediate triple fault.
;;; =========================================================
setup_page_tables:
    ;; All boot_* symbols are in .boot.bss: VMA = LMA = physical address.

    ;; Clear 7 pages: PML4, PDPT_LOW, PDPT_HIGH, PD, PT0, PT1, PT2
    mov edi, boot_pml4
    xor eax, eax
    mov ecx, (7 * 4096) / 4
    rep stosd

    ;; ---- PML4 ----
    mov eax, boot_pdpt_low
    or  eax, 3                          ; Present + RW
    mov [boot_pml4], eax                ; PML4[0] -> PDPT_LOW

    mov eax, boot_pdpt_high
    or  eax, 3
    mov [boot_pml4 + 511*8], eax        ; PML4[511] -> PDPT_HIGH

    ;; ---- PDPTs (both point to the same PD) ----
    mov eax, boot_pd
    or  eax, 3
    mov [boot_pdpt_low], eax            ; PDPT_LOW[0] -> PD

    ;; PDPT_HIGH[510] -> same PD (VA 0xFFFFFFFF80000000 -> PML4[511], PDPT[510])
    mov [boot_pdpt_high + 510*8], eax   ; PDPT_HIGH[510] -> PD

    ;; ---- PD entries (4KB page tables, no PS/huge bit) ----
    mov eax, boot_pt0
    or  eax, 3
    mov [boot_pd], eax                  ; PD[0] -> PT0 (covers 0..2MB)

    mov eax, boot_pt1
    or  eax, 3
    mov [boot_pd + 8], eax              ; PD[1] -> PT1 (covers 2MB..4MB)

    mov eax, boot_pt2
    or  eax, 3
    mov [boot_pd + 16], eax             ; PD[2] -> PT2 (covers 4MB..6MB)

    ;; ---- Fill PT0 + PT1 + PT2 with identity-mapped 4KB entries ----
    ;; PT[n] = (n * 4096) | 3, for n = 0 .. 3*512-1
    mov edi, boot_pt0
    mov eax, 0x003          ; PA=0, Present+RW
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

long_mode_entry:
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
