/*
 * include/kernel/signal.h - POSIX signal subsystem
 *
 * Provides full POSIX signal semantics:
 *   - 32 standard signals (SIGKILL, SIGTERM, SIGSEGV, …)
 *   - Per-process pending/blocked bitmasks
 *   - sigaction() with SA_RESTART, SA_SIGINFO flags
 *   - Signal delivery on return from kernel (syscall exit, IRQ exit)
 *   - Kernel-generated signals (SIGSEGV from page fault, SIGFPE from #DE)
 */
#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include <types.h>

/* ── Signal numbers ──────────────────────────────────────────────────── */
#define SIGHUP    1   /* Hangup */
#define SIGINT    2   /* Interrupt (Ctrl-C) */
#define SIGQUIT   3   /* Quit (Ctrl-\) */
#define SIGILL    4   /* Illegal instruction */
#define SIGTRAP   5   /* Trace / breakpoint */
#define SIGABRT   6   /* Abort */
#define SIGBUS    7   /* Bus error (unaligned access) */
#define SIGFPE    8   /* Floating-point / divide-by-zero */
#define SIGKILL   9   /* Kill (cannot be caught or ignored) */
#define SIGUSR1   10  /* User-defined 1 */
#define SIGSEGV   11  /* Segmentation fault */
#define SIGUSR2   12  /* User-defined 2 */
#define SIGPIPE   13  /* Broken pipe */
#define SIGALRM   14  /* Alarm clock */
#define SIGTERM   15  /* Termination request */
#define SIGCHLD   17  /* Child stopped or exited */
#define SIGCONT   18  /* Continue if stopped */
#define SIGSTOP   19  /* Stop (cannot be caught or ignored) */
#define SIGTSTP   20  /* Terminal stop (Ctrl-Z) */
#define SIGTTIN   21  /* Background read from terminal */
#define SIGTTOU   22  /* Background write to terminal */
#define SIGWINCH  28  /* Terminal window resize */

#define NSIG      32  /* Number of signals */

/* ── Signal bitmask type ─────────────────────────────────────────────── */
typedef uint32_t sigset_t;

#define sigemptyset(set)         (*(set) = 0)
#define sigfillset(set)          (*(set) = 0xFFFFFFFFu)
#define sigaddset(set, sig)      (*(set) |=  (1u << ((sig) - 1)))
#define sigdelset(set, sig)      (*(set) &= ~(1u << ((sig) - 1)))
#define sigismember(set, sig)    (!!(*(set) &  (1u << ((sig) - 1))))

/* ── sigaction flags ─────────────────────────────────────────────────── */
#define SA_NOCLDSTOP  0x0001   /* Don't send SIGCHLD when child stops */
#define SA_NOCLDWAIT  0x0002   /* Don't create zombies on child exit */
#define SA_SIGINFO    0x0004   /* Use sa_sigaction instead of sa_handler */
#define SA_RESTART    0x0010   /* Restart interrupted system calls */
#define SA_NODEFER    0x0040   /* Don't block this signal while handling */
#define SA_RESETHAND  0x0080   /* Reset to SIG_DFL on entry to handler */
#define SA_ONSTACK    0x0100   /* Call handler on alternate signal stack */

/* ── Signal dispositions ─────────────────────────────────────────────── */
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)   /* Default action */
#define SIG_IGN ((sighandler_t)1)   /* Ignore */
#define SIG_ERR ((sighandler_t)-1)  /* Error return */

/* ── siginfo_t ───────────────────────────────────────────────────────── */
typedef struct {
    int      si_signo;   /* Signal number */
    int      si_errno;   /* Error code */
    int      si_code;    /* Signal code */
    pid_t    si_pid;     /* Sending process PID */
    uint64_t si_addr;    /* Fault address (SIGSEGV/SIGBUS) */
    int      si_status;  /* Exit status (SIGCHLD) */
} siginfo_t;

/* ── struct sigaction ────────────────────────────────────────────────── */
typedef struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, siginfo_t*, void*);
    };
    sigset_t  sa_mask;    /* Extra signals to block during handler */
    int       sa_flags;
} sigaction_t;

/* ── Per-process signal state (embedded in process_t) ───────────────── */
typedef struct {
    sigaction_t actions[NSIG]; /* Disposition for each signal */
    sigset_t    pending;       /* Signals delivered but not yet handled */
    sigset_t    blocked;       /* Signals currently masked */
    uint64_t    alt_stack;     /* Alternate signal stack base (0 = none) */
    size_t      alt_stack_sz;
} signal_state_t;

/*
 * sig_frame_t - User-mode register state saved on the user stack before
 * calling a signal handler.  sys_sigreturn() restores execution from it.
 *
 * Layout on the user stack (growing downward, set up by signal_deliver_pending):
 *   [trampoline bytes: mov eax,52; int 0x80]  ← tramp_addr
 *   [sig_frame_t]                              ← RSP after handler's ret
 *   [return address → tramp_addr]              ← new user RSP (handler entry)
 */
typedef struct {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9,  r8;
    uint64_t rbp, rdi, rsi, rdx;
    uint64_t rcx, rbx, rax;
    uint64_t rflags;
    uint64_t rip;    /* interrupted user RIP */
    uint64_t rsp;    /* interrupted user RSP */
    uint64_t signo;  /* signal number (for validation) */
} sig_frame_t;

/* ── Signal subsystem API ────────────────────────────────────────────── */
void   signal_init_process(void* proc);                       /* Zero out state */
int    signal_send(pid_t pid, int sig);                       /* Send to process */
int    signal_send_proc(void* proc, int sig);                 /* Direct send */

/*
 * signal_deliver_pending - check pending signals and deliver the first
 * unblocked one.  Must be called before returning to user space.
 *
 * @regs: pointer to the cpu_registers_t interrupt frame (cpu_registers_t*),
 *        or NULL when no interrupt frame is available (kernel-mode context).
 *        When non-NULL and the process is running in ring 3, user-defined
 *        handlers are dispatched via a trampoline on the user stack.
 *        When NULL or the process is in ring 0, SIG_DFL is applied instead.
 */
void   signal_deliver_pending(void* proc, void* regs);
bool   signal_has_pending(void* proc);
int    signal_do_sigaction(int sig, const sigaction_t* act,
                            sigaction_t* old);                /* Change disposition */
int    signal_do_sigprocmask(int how, const sigset_t* set,
                              sigset_t* old);
void   signal_default_action(void* proc, int sig);            /* Apply SIG_DFL */

/*
 * signal_do_sigreturn - restore interrupted user context from sig_frame_t.
 * Called directly from syscall_handler() (not via dispatch table) so that
 * the interrupt frame pointer can be passed in.
 */
void   signal_do_sigreturn(void* regs);

/* Called from interrupt handlers to inject kernel signals */
void   signal_raise_fault(int sig, uint64_t fault_addr);      /* SIGSEGV/SIGBUS/SIGFPE */

/* howarg for sigprocmask */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

#endif /* KERNEL_SIGNAL_H */
