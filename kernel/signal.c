#include <errno.h>
/*
 * kernel/signal.c - POSIX signal subsystem
 *
 * Implements signal delivery for processes.  Signals are stored as a pending
 * bitmask inside the process and delivered at the next safe point (syscall
 * return or IRQ return) via signal_deliver_pending().
 *
 * Signal delivery model:
 *   1. signal_send(pid, sig)    — sets the pending bit, wakes sleeping proc
 *   2. syscall_handler() calls signal_deliver_pending() before returning
 *   3. For kernel-handled signals (SIGKILL/SIGSTOP): act immediately
 *   4. For user-handled signals: set up a trampoline on the user stack
 *      so that the signal handler runs, then returns to interrupted code
 *      (full user-mode trampoline is Phase-2; Phase-1 terminates the proc)
 */
#include <kernel/signal.h>
#include <process.h>
#include <scheduler.h>
#include <kernel.h>
#include <memory.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static inline bool sig_valid(int sig) {
    return sig >= 1 && sig < NSIG;
}

static inline signal_state_t* proc_sigstate(process_t* p) {
    return &p->sigstate;
}

/* ── Initialization ──────────────────────────────────────────────────── */

void signal_init_process(void* vproc)
{
    process_t* proc = (process_t*)vproc;
    signal_state_t* ss = proc_sigstate(proc);

    memset(ss, 0, sizeof(*ss));

    /* Set default dispositions: most signals terminate the process */
    for (int i = 1; i < NSIG; i++) {
        ss->actions[i - 1].sa_handler = SIG_DFL;
    }

    /* SIGCHLD and SIGCONT default to ignore */
    ss->actions[SIGCHLD - 1].sa_handler = SIG_IGN;
    ss->actions[SIGCONT - 1].sa_handler = SIG_IGN;
    ss->actions[SIGWINCH - 1].sa_handler = SIG_IGN;
}

/* ── Signal delivery ─────────────────────────────────────────────────── */

int signal_send(pid_t pid, int sig)
{
    if (!sig_valid(sig)) return -EINVAL;

    process_t* proc = process_get_by_pid(pid);
    if (!proc) return -ESRCH;

    return signal_send_proc(proc, sig);
}

int signal_send_proc(void* vproc, int sig)
{
    process_t* proc = (process_t*)vproc;
    if (!proc || !sig_valid(sig)) return -EINVAL;

    signal_state_t* ss = proc_sigstate(proc);

    /* SIGKILL and SIGSTOP can never be blocked */
    if (sig != SIGKILL && sig != SIGSTOP) {
        if (sigismember(&ss->blocked, sig)) {
            /* Signal is blocked; mark as pending and return */
            sigaddset(&ss->pending, sig);
            return 0;
        }
    }

    sigaddset(&ss->pending, sig);

    /* Wake the process if it's sleeping so it can handle the signal */
    if (proc->state == PROC_STATE_SLEEPING ||
        proc->state == PROC_STATE_WAITING) {
        process_wake(proc);
    }

    return 0;
}

/* ── Signal trampoline ───────────────────────────────────────────────── */

/*
 * x86-64 trampoline written onto the user stack:
 *   B8 34 00 00 00   mov eax, 52  (SYS_SIGRETURN)
 *   CD 80            int 0x80
 * Total: 7 bytes.
 */
#define SIGTRAMP_SIZE 7
static const uint8_t g_sigreturn_tramp[SIGTRAMP_SIZE] = {
    0xB8, 52, 0x00, 0x00, 0x00,   /* mov eax, 52  */
    0xCD, 0x80                     /* int 0x80     */
};

/*
 * signal_setup_frame - build a signal frame on the user stack and redirect
 * the interrupted register state (via cpu_registers_t* regs) to @handler.
 *
 * Stack layout after setup (addresses decrease downward):
 *   original user RSP
 *   [trampoline: 7 bytes]          ← tramp_addr
 *   [sig_frame_t: saved registers] ← frame pointer (RSP after handler ret)
 *   [return address → tramp_addr]  ← new user RSP (handler sees [RSP]=tramp)
 */
static void signal_setup_frame(cpu_registers_t* regs,
                                sighandler_t handler, int sig)
{
    uint64_t usp = regs->rsp;

    /* ① Write trampoline code below current user RSP */
    usp -= SIGTRAMP_SIZE;
    memcpy((void*)(uintptr_t)usp, g_sigreturn_tramp, SIGTRAMP_SIZE);
    uint64_t tramp_addr = usp;

    /* ② Align to 16 bytes then carve out sig_frame_t */
    usp &= ~(uint64_t)0xF;
    usp -= sizeof(sig_frame_t);
    sig_frame_t* sf = (sig_frame_t*)(uintptr_t)usp;

    /* ③ Save full interrupted register state */
    sf->r15    = regs->r15;
    sf->r14    = regs->r14;
    sf->r13    = regs->r13;
    sf->r12    = regs->r12;
    sf->r11    = regs->r11;
    sf->r10    = regs->r10;
    sf->r9     = regs->r9;
    sf->r8     = regs->r8;
    sf->rbp    = regs->rbp;
    sf->rdi    = regs->rdi;
    sf->rsi    = regs->rsi;
    sf->rdx    = regs->rdx;
    sf->rcx    = regs->rcx;
    sf->rbx    = regs->rbx;
    sf->rax    = regs->rax;
    sf->rflags = regs->rflags;
    sf->rip    = regs->rip;
    sf->rsp    = regs->rsp;
    sf->signo  = (uint64_t)sig;

    /* ④ Push the trampoline address as the handler's return address */
    usp -= 8;
    *(uint64_t*)(uintptr_t)usp = tramp_addr;

    /* ⑤ Redirect interrupted context to the signal handler */
    regs->rsp = usp;
    regs->rip = (uint64_t)(uintptr_t)handler;
    regs->rdi = (uint64_t)sig;    /* arg1 = signum */
    regs->rsi = 0;                /* arg2 = NULL   (no siginfo_t) */
    regs->rdx = 0;                /* arg3 = NULL   (no ucontext)  */
}

/*
 * signal_do_sigreturn - restore interrupted user context from sig_frame_t.
 *
 * Called directly from syscall_handler (not via dispatch table) so that
 * the interrupt frame pointer can be accessed.
 *
 * When `int 0x80` fires inside the trampoline:
 *   - handler's `ret` popped the return address → RSP advanced past it
 *   - RSP at int 0x80 == &sig_frame_t (the frame we placed above the ret addr)
 */
void signal_do_sigreturn(void* vregs)
{
    cpu_registers_t* regs = (cpu_registers_t*)vregs;
    if (!regs) return;

    sig_frame_t* sf = (sig_frame_t*)(uintptr_t)regs->rsp;

    regs->r15    = sf->r15;
    regs->r14    = sf->r14;
    regs->r13    = sf->r13;
    regs->r12    = sf->r12;
    regs->r11    = sf->r11;
    regs->r10    = sf->r10;
    regs->r9     = sf->r9;
    regs->r8     = sf->r8;
    regs->rbp    = sf->rbp;
    regs->rdi    = sf->rdi;
    regs->rsi    = sf->rsi;
    regs->rdx    = sf->rdx;
    regs->rcx    = sf->rcx;
    regs->rbx    = sf->rbx;
    regs->rax    = sf->rax;
    regs->rflags = sf->rflags;
    regs->rip    = sf->rip;
    regs->rsp    = sf->rsp;

    kinfo("PID %u: sigreturn — restored context to RIP=0x%llx",
          current_process ? (unsigned)current_process->pid : 0u,
          (unsigned long long)regs->rip);
}

/* ── Check and deliver all pending signals ───────────────────────────── */

/*
 * signal_deliver_pending - deliver the first pending unblocked signal.
 *
 * @vregs: cpu_registers_t* frame from syscall/interrupt entry, or NULL.
 *         When non-NULL and CS indicates ring 3, user-defined handlers are
 *         invoked via a trampoline on the user stack.
 */
void signal_deliver_pending(void* vproc, void* vregs)
{
    process_t* proc = (process_t*)vproc;
    if (!proc) return;

    signal_state_t* ss = proc_sigstate(proc);

    /* No pending unblocked signals → fast path */
    sigset_t deliverable = ss->pending & ~ss->blocked;
    if (!deliverable) return;

    /* frame pointer and whether it's a ring-3 user process */
    cpu_registers_t* regs = (cpu_registers_t*)vregs;
    bool ring3 = regs && ((regs->cs & 3) == 3);

    /* Deliver signals in priority order (lowest number first) */
    for (int sig = 1; sig < NSIG; sig++) {
        if (!sigismember(&deliverable, sig)) continue;

        sigdelset(&ss->pending, sig);

        sighandler_t handler = ss->actions[sig - 1].sa_handler;

        /* SIG_IGN: drop it */
        if (handler == SIG_IGN) continue;

        /* SIGKILL and SIGSTOP are always handled by the kernel */
        if (sig == SIGKILL) {
            kinfo("PID %u killed by SIGKILL", proc->pid);
            process_exit(proc, -sig);
            scheduler_yield();
            return; /* Never reached */
        }

        if (sig == SIGSTOP) {
            proc->state = PROC_STATE_WAITING;
            scheduler_yield();
            continue;
        }

        if (sig == SIGCONT) {
            if (proc->state == PROC_STATE_WAITING) {
                proc->state = PROC_STATE_READY;
                scheduler_add(proc);
            }
            continue;
        }

        /* SIG_DFL: apply default action */
        if (handler == SIG_DFL) {
            signal_default_action(proc, sig);
            return;
        }

        /* User-defined handler */
        if (ring3) {
            /*
             * Ring-3 process: set up trampoline on the user stack so the
             * handler runs, then sigreturn restores the interrupted context.
             */
            /* Block the signal during handler execution (unless SA_NODEFER) */
            if (!(ss->actions[sig - 1].sa_flags & SA_NODEFER))
                sigaddset(&ss->blocked, sig);

            /* SA_RESETHAND: reset disposition to SIG_DFL on first delivery */
            if (ss->actions[sig - 1].sa_flags & SA_RESETHAND)
                ss->actions[sig - 1].sa_handler = SIG_DFL;

            kinfo("PID %u: signal %d → user handler @%p (trampoline)",
                  proc->pid, sig, (void*)(uintptr_t)handler);
            signal_setup_frame(regs, handler, sig);
            return;
        }

        /* Kernel thread or no frame — fall back to default action */
        kinfo("PID %u: signal %d → user handler @%p (ring-0, applying default)",
              proc->pid, sig, (void*)(uintptr_t)handler);
        signal_default_action(proc, sig);
        return;
    }
}

bool signal_has_pending(void* vproc)
{
    process_t* proc = (process_t*)vproc;
    if (!proc) return false;
    signal_state_t* ss = proc_sigstate(proc);
    return (ss->pending & ~ss->blocked) != 0;
}

/* ── Default signal action ───────────────────────────────────────────── */

void signal_default_action(void* vproc, int sig)
{
    process_t* proc = (process_t*)vproc;

    switch (sig) {
    /* Terminate */
    case SIGHUP: case SIGINT: case SIGQUIT: case SIGILL:
    case SIGTRAP: case SIGABRT: case SIGBUS: case SIGFPE:
    case SIGSEGV: case SIGPIPE: case SIGALRM: case SIGTERM:
    case SIGUSR1: case SIGUSR2:
        kinfo("PID %u terminated by signal %d", proc->pid, sig);
        process_exit(proc, -sig);
        scheduler_yield();
        break;

    /* Stop */
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        proc->state = PROC_STATE_WAITING;
        scheduler_yield();
        break;

    /* Ignore by default */
    case SIGCHLD: case SIGCONT: case SIGWINCH:
        break;

    default:
        kinfo("PID %u terminated by unknown signal %d", proc->pid, sig);
        process_exit(proc, -sig);
        scheduler_yield();
        break;
    }
}

/* ── sigaction ───────────────────────────────────────────────────────── */

int signal_do_sigaction(int sig, const sigaction_t* act, sigaction_t* old)
{
    if (!sig_valid(sig)) return -EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;

    process_t* proc = current_process;
    if (!proc) return -ESRCH;

    signal_state_t* ss = proc_sigstate(proc);

    if (old) *old = ss->actions[sig - 1];
    if (act) ss->actions[sig - 1] = *act;

    return 0;
}

/* ── sigprocmask ─────────────────────────────────────────────────────── */

int signal_do_sigprocmask(int how, const sigset_t* set, sigset_t* old)
{
    process_t* proc = current_process;
    if (!proc) return -ESRCH;

    signal_state_t* ss = proc_sigstate(proc);
    if (old) *old = ss->blocked;

    if (!set) return 0;

    /* SIGKILL and SIGSTOP can never be blocked */
    sigset_t mask = *set;
    sigdelset(&mask, SIGKILL);
    sigdelset(&mask, SIGSTOP);

    switch (how) {
    case SIG_BLOCK:   ss->blocked |= mask;  break;
    case SIG_UNBLOCK: ss->blocked &= ~mask; break;
    case SIG_SETMASK: ss->blocked  = mask;  break;
    default: return -EINVAL;
    }

    return 0;
}

/* ── Kernel-generated faults ─────────────────────────────────────────── */

void signal_raise_fault(int sig, uint64_t fault_addr)
{
    process_t* proc = current_process;
    if (!proc) return;

    kwarn("PID %u: fault signal %d at 0x%llx", proc->pid, sig, fault_addr);
    signal_send_proc(proc, sig);
    signal_deliver_pending(proc, NULL); /* no interrupt frame from fault path */
}
