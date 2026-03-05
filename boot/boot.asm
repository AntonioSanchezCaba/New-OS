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

    ;; Framebuffer tag (request 1024x768 32bpp linear framebuffer)
    dw  5                   ; Tag type: framebuffer
    dw  1                   ; Flags: optional (don't fail if unsupported)
    dd  20                  ; Size
    dd  1024                ; Preferred width
    dd  768                 ; Preferred height
    dd  32                  ; Preferred depth (32bpp)

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

    ;; Verify Multiboot2 magic
    cmp eax, 0x36D76289
    jne .no_multiboot

    ;; Save multiboot2 info pointer (EBX) to physical memory location.
    ;; boot.bss symbols have VMA = physical (no KERNEL_VMA_OFFSET), use directly.
    mov [multiboot_info_ptr], ebx

    ;; Set up the initial 32-bit stack (in boot BSS)
    mov esp, boot_stack_top

    ;; Check that CPUID is supported
    call check_cpuid
    jc .no_cpuid

    ;; Check that Long Mode is supported
    call check_long_mode
    jc .no_long_mode

    ;; Set up initial page tables for:
    ;;   a) Identity-map of first 2GB (so boot code can continue running)
    ;;   b) Higher-half map at 0xFFFFFFFF80000000 -> physical 0
    call setup_page_tables

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
    or  eax, (1 << 11)  ; EFER.NXE = 1 (enable No-Execute bit)
    wrmsr

    ;; Enable paging (which also activates long mode)
    mov eax, cr0
    or  eax, (1 << 31)  ; CR0.PG = 1
    or  eax, (1 << 0)   ; CR0.PE = 1 (should already be set by GRUB)
    mov cr0, eax

    ;; Load our 64-bit GDT (gdt64_ptr is in .boot.data, physical = VMA)
    lgdt [gdt64_ptr]

    ;; Far jump to flush the pipeline and enter 64-bit code segment
    jmp 0x08:(long_mode_entry - KERNEL_VMA_OFFSET)

.no_multiboot:
    mov esi, msg_no_multiboot   ; .boot.data: physical = VMA, no offset needed
    jmp boot_error32

.no_cpuid:
    mov esi, msg_no_cpuid
    jmp boot_error32

.no_long_mode:
    mov esi, msg_no_long_mode
    jmp boot_error32

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
;;; Sets up a minimal 4-level paging structure using 2MB pages.
;;;
;;; Memory layout of page tables (placed just below 0x100000):
;;;   0x1000: PML4
;;;   0x2000: PDPT for identity map (entries 0 and 510)
;;;   0x3000: PD  for first 2GB using 2MB huge pages
;;;   0x4000: PDPT for higher-half (PML4[511])
;;;   0x5000: PD  for higher-half -> same 2GB
;;; =========================================================
setup_page_tables:
    ;; All boot_* symbols are in .boot.bss: VMA = LMA = physical address.
    ;; Do NOT subtract KERNEL_VMA_OFFSET — use them directly.

    ;; Clear all 5 page tables (5 * 4096 = 20480 bytes)
    mov edi, boot_pml4
    xor eax, eax
    mov ecx, (5 * 4096) / 4
    rep stosd

    ;; ---- PML4 ----
    ;; PML4[0] -> PDPT (identity map, covers 0x0 - 0x7FFFFFFF)
    mov eax, boot_pdpt_low
    or  eax, 3                              ; Present + RW
    mov [boot_pml4], eax

    ;; PML4[511] -> PDPT (higher half: 0xFFFFFFFF80000000+)
    mov eax, boot_pdpt_high
    or  eax, 3
    mov [boot_pml4 + 511*8], eax

    ;; ---- Lower PDPT ----
    ;; PDPT[0] -> PD (first 1GB)
    mov eax, boot_pd
    or  eax, 3
    mov [boot_pdpt_low], eax

    ;; ---- Higher PDPT ----
    ;; PDPT[510] -> PD (maps 0xFFFFFFFF80000000 to physical 0x0)
    ;; Note: 0xFFFFFFFF80000000 is in PML4[511], PDPT[510]
    ;;   Virtual: 0xFFFF_FFFF_8000_0000
    ;;     PML4 index = (0xFFFF_FFFF_8000_0000 >> 39) & 0x1FF = 511
    ;;     PDPT index = (0xFFFF_FFFF_8000_0000 >> 30) & 0x1FF = 510
    mov [boot_pdpt_high + 510*8], eax

    ;; ---- Page Directory (shared by both PDPTs) ----
    ;; Map 512 * 2MB = 1GB using huge pages
    mov eax, 0x83       ; Present + RW + Huge (2MB)
    mov ecx, 0
.fill_pd:
    mov [boot_pd + ecx*8], eax
    add eax, 0x200000   ; Next 2MB
    inc ecx
    cmp ecx, 512
    jl  .fill_pd

    ret

;;; =========================================================
;;; boot_error32 - print an error string and halt (32-bit)
;;; ESI = pointer to null-terminated string (physical address)
;;; =========================================================
boot_error32:
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

boot_pd2:
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
