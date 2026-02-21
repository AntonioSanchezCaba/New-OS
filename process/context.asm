;;; process/context.asm - Low-level context switching
;;;
;;; context_switch(old_ctx, new_ctx) saves the current CPU state into
;;; old_ctx and restores the CPU state from new_ctx.
;;;
;;; The cpu_context_t struct layout (from process.h):
;;;   offset  0: r15
;;;   offset  8: r14
;;;   offset 16: r13
;;;   offset 24: r12
;;;   offset 32: rbx
;;;   offset 40: rbp
;;;   offset 48: rsp
;;;   offset 56: rip
;;;   offset 64: rflags
;;;   offset 72: cs
;;;   offset 80: ss

[BITS 64]
section .text

;;; void context_switch(cpu_context_t* old_ctx, cpu_context_t* new_ctx)
;;; RDI = old_ctx, RSI = new_ctx
global context_switch
context_switch:
    ;; ---- Save current context into old_ctx ----
    mov     [rdi + 0],  r15
    mov     [rdi + 8],  r14
    mov     [rdi + 16], r13
    mov     [rdi + 24], r12
    mov     [rdi + 32], rbx
    mov     [rdi + 40], rbp
    mov     [rdi + 48], rsp

    ;; Save RFLAGS
    pushfq
    pop     qword [rdi + 64]

    ;; Save the return address (where this function was called FROM)
    ;; so that when we "return" in the new context, we land at the right place
    mov     rax, [rsp]          ; Return address is at top of stack
    mov     [rdi + 56], rax     ; Save as RIP

    ;; Save segment registers
    xor     rax, rax
    mov     ax, cs
    mov     [rdi + 72], rax
    mov     ax, ss
    mov     [rdi + 80], rax

    ;; ---- Restore new context from new_ctx ----
    mov     r15, [rsi + 0]
    mov     r14, [rsi + 8]
    mov     r13, [rsi + 16]
    mov     r12, [rsi + 24]
    mov     rbx, [rsi + 32]
    mov     rbp, [rsi + 40]
    mov     rsp, [rsi + 48]     ; Switch stack pointer

    ;; Restore RFLAGS
    push    qword [rsi + 64]
    popfq

    ;; Jump to the saved RIP of the new context
    ;; This simulates returning from context_switch() in the new thread
    mov     rax, [rsi + 56]
    jmp     rax


;;; ============================================================
;;; switch_to_usermode(user_rip, user_rsp)
;;;
;;; Drops CPU privilege from ring 0 to ring 3 using IRETQ.
;;; Constructs a fake IRET frame on the stack:
;;;   [SS:RPL=3] [user RSP] [RFLAGS] [CS:RPL=3] [user RIP]
;;;
;;; RDI = user_rip, RSI = user_rsp
;;; ============================================================
global switch_to_usermode
switch_to_usermode:
    ;; Load user-space data segment registers
    mov     ax, (0x18 | 3)      ; User data selector (ring 3)
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax

    ;; Build IRETQ frame:
    push    qword (0x18 | 3)    ; SS (user data, ring 3)
    push    rsi                  ; RSP (user stack)
    push    qword 0x202          ; RFLAGS: IF=1, reserved bit 1=1
    push    qword (0x20 | 3)    ; CS (user code, ring 3)
    push    rdi                  ; RIP (user entry point)

    iretq


;;; ============================================================
;;; kernel_thread_entry - wrapper for new kernel threads
;;;
;;; When a new kernel thread is created, its initial RIP points here.
;;; We pop the entry function off the stack and call it.
;;; If it returns, we call process_exit().
;;; ============================================================
global kernel_thread_entry
extern process_exit
extern current_process

kernel_thread_entry:
    ;; The entry function was pushed as the first item after context
    ;; Restore interrupts (they were disabled during context switch)
    sti

    ;; Call the thread entry function (pointer in RBX, saved by convention)
    call    rbx

    ;; If the thread returns, exit with code 0
    xor     rdi, rdi
    mov     rsi, rdi
    call    process_exit

    ;; Should never reach here
.halt:
    cli
    hlt
    jmp     .halt
