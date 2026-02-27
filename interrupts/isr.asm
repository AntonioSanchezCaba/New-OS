;;; interrupts/isr.asm - Interrupt Service Routine stubs
;;;
;;; Each CPU exception and hardware IRQ needs its own stub that:
;;;   1. Optionally pushes a dummy error code (for exceptions without one)
;;;   2. Pushes the interrupt vector number
;;;   3. Saves all general-purpose registers
;;;   4. Calls the C interrupt_dispatch() function
;;;   5. Restores registers and returns from the interrupt (iretq)
;;;
;;; The 64-bit IRET frame on the stack looks like:
;;;   [SS] [RSP] [RFLAGS] [CS] [RIP] [error_code] [int_no]
;;; followed by our saved registers.

[BITS 64]

;;; External C dispatcher
extern interrupt_dispatch

;;; Macro: ISR without error code (CPU does not push one)
%macro ISR_NOERR 1
global isr%1
isr%1:
    push    qword 0         ; Dummy error code
    push    qword %1        ; Interrupt vector number
    jmp     isr_common
%endmacro

;;; Macro: ISR with error code (CPU pushes error code automatically)
%macro ISR_ERR 1
global isr%1
isr%1:
    push    qword %1        ; Interrupt vector number (error code already on stack)
    jmp     isr_common
%endmacro

;;; Macro: IRQ stub (hardware interrupts - no error code)
%macro IRQ 2
global irq%1
irq%1:
    push    qword 0         ; Dummy error code
    push    qword %2        ; IRQ vector number (32 + irq_line)
    jmp     isr_common
%endmacro

;;; =========================================================
;;; CPU Exception stubs (vectors 0-31)
;;; =========================================================
ISR_NOERR  0    ; #DE Divide by zero
ISR_NOERR  1    ; #DB Debug
ISR_NOERR  2    ; NMI
ISR_NOERR  3    ; #BP Breakpoint
ISR_NOERR  4    ; #OF Overflow
ISR_NOERR  5    ; #BR Bound range
ISR_NOERR  6    ; #UD Invalid opcode
ISR_NOERR  7    ; #NM Device not available
ISR_ERR    8    ; #DF Double fault (error code = 0)
ISR_NOERR  9    ; Coprocessor segment overrun (legacy)
ISR_ERR    10   ; #TS Invalid TSS
ISR_ERR    11   ; #NP Segment not present
ISR_ERR    12   ; #SS Stack segment fault
ISR_ERR    13   ; #GP General protection fault
ISR_ERR    14   ; #PF Page fault
ISR_NOERR  15   ; Reserved
ISR_NOERR  16   ; #MF x87 FPU error
ISR_ERR    17   ; #AC Alignment check
ISR_NOERR  18   ; #MC Machine check
ISR_NOERR  19   ; #XM SIMD FP exception
ISR_NOERR  20   ; #VE Virtualization exception
ISR_NOERR  21   ; Reserved
ISR_NOERR  22   ; Reserved
ISR_NOERR  23   ; Reserved
ISR_NOERR  24   ; Reserved
ISR_NOERR  25   ; Reserved
ISR_NOERR  26   ; Reserved
ISR_NOERR  27   ; Reserved
ISR_NOERR  28   ; Reserved
ISR_NOERR  29   ; Reserved
ISR_ERR    30   ; #SX Security exception
ISR_NOERR  31   ; Reserved

;;; =========================================================
;;; Hardware IRQ stubs (vectors 32-47, IRQ 0-15)
;;; =========================================================
IRQ  0,  32    ; IRQ0  - PIT Timer
IRQ  1,  33    ; IRQ1  - PS/2 Keyboard
IRQ  2,  34    ; IRQ2  - PIC cascade
IRQ  3,  35    ; IRQ3  - COM2
IRQ  4,  36    ; IRQ4  - COM1
IRQ  5,  37    ; IRQ5  - LPT2
IRQ  6,  38    ; IRQ6  - Floppy
IRQ  7,  39    ; IRQ7  - LPT1 / spurious
IRQ  8,  40    ; IRQ8  - RTC
IRQ  9,  41    ; IRQ9  - Free
IRQ  10, 42    ; IRQ10 - Free
IRQ  11, 43    ; IRQ11 - Free
IRQ  12, 44    ; IRQ12 - PS/2 Mouse
IRQ  13, 45    ; IRQ13 - FPU
IRQ  14, 46    ; IRQ14 - Primary ATA
IRQ  15, 47    ; IRQ15 - Secondary ATA

;;; =========================================================
;;; Syscall stub (int 0x80)
;;; =========================================================
global isr128
isr128:
    push    qword 0
    push    qword 128
    jmp     isr_common

;;; =========================================================
;;; isr_common - save context, call C handler, restore context
;;; =========================================================
section .text
isr_common:
    ;; Save all general-purpose registers (in the order cpu_registers_t expects)
    ;; Stack so far (top = last push):
    ;;   [err_code] [int_no] ... (pushed by stub above)
    ;;   [rip] [cs] [rflags] [rsp] [ss]  ... (pushed by CPU on interrupt)
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    ;; Set up kernel data segments (in case we interrupted user code)
    mov     ax, 0x10        ; Kernel data segment
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax

    ;; Pass pointer to the saved register frame as first argument (RDI)
    mov     rdi, rsp

    ;; Align stack to 16 bytes (required by System V AMD64 ABI for function calls)
    and     rsp, ~0xF

    ;; Call the C interrupt dispatcher
    call    interrupt_dispatch

    ;; Restore stack alignment (undo the 'and' above by reloading RSP from frame)
    mov     rsp, rdi

    ;; Restore general-purpose registers
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax

    ;; Remove int_no and err_code from the stack
    add     rsp, 16

    ;; Return from interrupt (restores RIP, CS, RFLAGS, RSP, SS from CPU frame)
    iretq

; Mark stack as non-executable (suppresses GNU-stack linker warning)
section .note.GNU-stack noalloc noexec nowrite progbits
