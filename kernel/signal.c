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

/* ── Check and deliver all pending signals ───────────────────────────── */

void signal_deliver_pending(void* vproc)
{
    process_t* proc = (process_t*)vproc;
    if (!proc) return;

    signal_state_t* ss = proc_sigstate(proc);

    /* No pending unblocked signals → fast path */
    sigset_t deliverable = ss->pending & ~ss->blocked;
    if (!deliverable) return;

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

        /*
         * User-defined handler:
         * Full trampoline setup (push sigframe on user stack, redirect RIP)
         * is implemented in Phase 2.  For now, log and apply default.
         */
        kinfo("PID %u: signal %d → user handler @%p (trampoline pending)",
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
    signal_deliver_pending(proc);
}
