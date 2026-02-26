/*
 * process.h - Process management structures and interfaces
 *
 * Defines the Process Control Block (PCB), process states, context switching,
 * and the public API for creating/destroying processes and threads.
 */
#ifndef PROCESS_H
#define PROCESS_H

#include <types.h>
#include <memory.h>
#include <interrupts.h>
#include <kernel/signal.h>
#include <kernel/cap.h>

/* Maximum number of simultaneous processes */
#define MAX_PROCESSES   256
#define MAX_THREADS     1024

/* Maximum filename length */
#define MAX_PATH_LEN    256
#define MAX_NAME_LEN    64

/* Maximum open file descriptors per process */
#define MAX_FDS         64

/* Maximum capability tokens a process may hold */
#define PROC_MAX_CAPS   16

/* Process states */
typedef enum {
    PROC_STATE_UNUSED = 0,  /* Slot is free */
    PROC_STATE_CREATED,     /* Created but not yet scheduled */
    PROC_STATE_RUNNING,     /* Currently executing on a CPU */
    PROC_STATE_READY,       /* Ready to run, waiting for CPU */
    PROC_STATE_SLEEPING,    /* Waiting on a timer */
    PROC_STATE_WAITING,     /* Waiting on I/O or event */
    PROC_STATE_ZOMBIE,      /* Exited, waiting for parent to collect */
    PROC_STATE_DEAD         /* Fully cleaned up */
} proc_state_t;

/* Saved CPU context for context switching */
typedef struct {
    /* Callee-saved registers (per System V AMD64 ABI) */
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsp;  /* Stack pointer */
    uint64_t rip;  /* Instruction pointer (return address) */
    uint64_t rflags;

    /* Segment registers */
    uint64_t cs;
    uint64_t ss;
} cpu_context_t;

/* File descriptor table entry */
struct file;  /* Forward declaration */
typedef struct {
    struct file* file;  /* Points to open file object */
    int          flags;
    off_t        offset;
} fd_entry_t;

/* Process Control Block */
typedef struct process {
    pid_t         pid;              /* Process ID */
    pid_t         ppid;            /* Parent process ID */
    pid_t         pgid;            /* Process group ID (for signals) */
    pid_t         sid;             /* Session ID */
    char          name[MAX_NAME_LEN];
    char          cwd[MAX_PATH_LEN]; /* Current working directory */
    proc_state_t  state;
    int           exit_code;

    /* Memory */
    page_table_t* address_space;  /* PML4 page table */
    uint64_t      kernel_stack;   /* Top of kernel stack for this process */
    uint64_t      kernel_stack_size;
    uint64_t      user_stack;     /* User-space stack top */
    uint64_t      heap_start;     /* User heap start */
    uint64_t      heap_end;       /* Current user heap end (brk) */

    /* CPU context (saved during context switch) */
    cpu_context_t context;

    /* Scheduling */
    int           priority;       /* 0 = highest */
    uint64_t      time_slice;     /* Remaining ticks */
    uint64_t      total_ticks;    /* Total CPU ticks used */
    uint64_t      sleep_until;    /* Wake up at this tick (for sleep) */

    /* Signal subsystem */
    signal_state_t sigstate;      /* Per-process signal state */

    /* Credentials */
    uint32_t      uid;            /* User ID (0 = root) */
    uint32_t      gid;            /* Group ID */
    uint32_t      euid;           /* Effective UID */
    uint32_t      egid;           /* Effective GID */

    /* File descriptors */
    fd_entry_t    fds[MAX_FDS];

    /* Capability table — tokens granted at launch from package manifest */
    cap_id_t      cap_ids[PROC_MAX_CAPS]; /* Owned capability tokens   */
    int           cap_count;              /* Number of active caps      */

    /* Accounting */
    uint64_t      start_tick;     /* Timer tick when process was created */
    uint64_t      user_ticks;     /* Ticks spent in user mode */
    uint64_t      sys_ticks;      /* Ticks spent in kernel mode */

    /* Process list links */
    struct process* next;
    struct process* prev;
    struct process* children;     /* Linked list of child processes */
    struct process* sibling;      /* Next sibling in parent's child list */
} process_t;

/* Process list */
extern process_t* process_list;
extern process_t* current_process;

/* Process management API */
void       process_init(void);
process_t* process_create(const char* name, void (*entry)(void), bool kernel);
process_t* process_fork(process_t* parent);
int        process_exec(process_t* proc, const char* path, char* const argv[]);
void       process_exit(process_t* proc, int exit_code);
void       process_kill(pid_t pid, int signal);
process_t* process_get_by_pid(pid_t pid);
void       process_sleep(uint64_t ticks);
void       process_wake(process_t* proc);
pid_t      process_waitpid(pid_t pid, int* status, int options);

/* Credential helpers */
bool       process_is_root(process_t* proc);
int        process_setuid(process_t* proc, uint32_t uid);
int        process_setgid(process_t* proc, uint32_t gid);

/* FD management */
int  fd_alloc(process_t* proc);
void fd_close(process_t* proc, int fd);
int  fd_dup(process_t* proc, int fd);
int  fd_dup2(process_t* proc, int oldfd, int newfd);

/* Context switch (implemented in context.asm) */
void context_switch(cpu_context_t* old_ctx, cpu_context_t* new_ctx);

/* Kernel idle process entry */
void idle_process(void);

/* Switch to user mode (ring 3) */
void switch_to_usermode(uint64_t user_rip, uint64_t user_rsp);

#endif /* PROCESS_H */
