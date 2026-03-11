/*
 * process/process.c - Process Control Block management
 *
 * Handles creating, forking, executing, and destroying processes.
 * Each process has:
 *   - A unique PID
 *   - Its own kernel stack (for interrupt handling)
 *   - Its own page table (address space)
 *   - A cpu_context_t saved during context switches
 */
#include <process.h>
#include <scheduler.h>
#include <memory.h>
#include <interrupts.h>
#include <fs/vfs.h>
#include <elf.h>
#include <kernel.h>
#include <kernel/signal.h>
#include <drivers/timer.h>
#include <types.h>
#include <string.h>

/* Global process table */
static process_t process_table[MAX_PROCESSES];
process_t* process_list   = NULL;
process_t* current_process = NULL;

/* PID allocator */
static pid_t next_pid = 1;

/* Kernel stack size per process */
#define KSTACK_SIZE (16 * 1024)   /* 16KB kernel stack */

/* ============================================================
 * Internal helpers
 * ============================================================ */

static process_t* alloc_process_slot(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROC_STATE_UNUSED) {
            memset(&process_table[i], 0, sizeof(process_t));
            return &process_table[i];
        }
    }
    return NULL;
}

static void process_list_add(process_t* proc)
{
    proc->next = process_list;
    proc->prev = NULL;
    if (process_list) process_list->prev = proc;
    process_list = proc;
}

static void process_list_remove(process_t* proc)
{
    if (proc->prev) proc->prev->next = proc->next;
    else            process_list = proc->next;
    if (proc->next) proc->next->prev = proc->prev;
    proc->next = proc->prev = NULL;
}

/*
 * setup_kernel_stack - allocate a kernel stack for a process and
 * initialize it so that the first context switch "returns" to @entry.
 *
 * Stack layout after setup (from high to low address):
 *   [entry function ptr] <- initial RBX (for kernel_thread_entry)
 *   [kernel_thread_entry] <- initial RIP
 *   [0] <- initial RBP
 *
 * The cpu_context_t.rsp will point to the return address slot.
 */
extern void kernel_thread_entry(void);

static uint64_t setup_kernel_stack(process_t* proc, void (*entry)(void))
{
    /* Allocate kernel stack pages */
    void* stack_phys = pmm_alloc_frames(KSTACK_SIZE / PAGE_SIZE);
    if (!stack_phys) return 0;

    uint64_t stack_virt = (uint64_t)PHYS_TO_VIRT(stack_phys);
    proc->kernel_stack      = stack_virt;
    proc->kernel_stack_size = KSTACK_SIZE;

    /* Set up initial kernel stack frame */
    uint64_t* sp = (uint64_t*)(stack_virt + KSTACK_SIZE);

    /* Push entry function pointer (will be in RBX when kernel_thread_entry runs) */
    *--sp = (uint64_t)entry;

    /* Push kernel_thread_entry as the "return address" */
    *--sp = (uint64_t)kernel_thread_entry;

    return (uint64_t)sp;
}

/* ============================================================
 * process_init - initialize the process subsystem
 * ============================================================ */

void process_init(void)
{
    memset(process_table, 0, sizeof(process_table));
    process_list    = NULL;
    current_process = NULL;
    next_pid        = 1;
    kinfo("Process subsystem initialized (max %d processes)", MAX_PROCESSES);
}

/* ============================================================
 * process_create - create a new process
 * @name:   process name (for display)
 * @entry:  function to execute as process entry point
 * @kernel: true = kernel process (ring 0), false = user process
 * ============================================================ */

process_t* process_create(const char* name, void (*entry)(void), bool kernel)
{
    process_t* proc = alloc_process_slot();
    if (!proc) {
        kerror("process_create: process table full");
        return NULL;
    }

    proc->pid   = next_pid++;
    proc->ppid  = current_process ? current_process->pid : 0;
    proc->pgid  = proc->pid;
    proc->sid   = proc->pid;
    proc->state = PROC_STATE_CREATED;
    strncpy(proc->name, name, MAX_NAME_LEN - 1);
    proc->name[MAX_NAME_LEN - 1] = '\0';
    strncpy(proc->cwd, "/", MAX_PATH_LEN - 1);
    proc->priority      = 5;
    proc->time_slice    = SCHED_DEFAULT_QUANTUM;
    proc->total_ticks   = 0;
    proc->sleep_until   = 0;
    proc->exit_code     = 0;
    proc->uid   = 0; proc->euid = 0;
    proc->gid   = 0; proc->egid = 0;
    proc->start_tick    = timer_get_ticks();
    proc->user_ticks    = 0;
    proc->sys_ticks     = 0;
    proc->children      = NULL;
    proc->sibling       = NULL;

    /* Initialize signal state */
    signal_init_process(proc);

    /* Create address space */
    if (kernel) {
        /* Kernel processes share the kernel page table */
        proc->address_space = kernel_pml4;
    } else {
        proc->address_space = vmm_create_address_space();
        if (!proc->address_space) {
            proc->state = PROC_STATE_UNUSED;
            return NULL;
        }
    }

    /* Set up kernel stack */
    uint64_t initial_rsp = setup_kernel_stack(proc, entry);
    if (!initial_rsp) {
        if (!kernel) vmm_destroy_address_space(proc->address_space);
        proc->state = PROC_STATE_UNUSED;
        return NULL;
    }

    /* Initialize CPU context */
    memset(&proc->context, 0, sizeof(cpu_context_t));
    proc->context.rsp    = initial_rsp;
    proc->context.rip    = (uint64_t)kernel_thread_entry; /* debug snapshot */
    proc->context.rbx    = (uint64_t)entry;  /* kernel_thread_entry calls rbx */
    proc->context.rflags = 0x202; /* IF=1 */
    proc->context.cs     = GDT_KERNEL_CODE;
    proc->context.ss     = GDT_KERNEL_DATA;

    /* Set up file descriptors: stdin, stdout, stderr */
    /* (VFS nodes for these will be set up when VFS is ready) */

    /* Add to process list */
    process_list_add(proc);

    kdebug("Created process '%s' PID=%u", proc->name, proc->pid);
    return proc;
}

/* ============================================================
 * process_fork - create a child process (clone of parent)
 * ============================================================ */

process_t* process_fork(process_t* parent)
{
    process_t* child = alloc_process_slot();
    if (!child) return NULL;

    /* Copy parent PCB */
    memcpy(child, parent, sizeof(process_t));

    child->pid   = next_pid++;
    child->ppid  = parent->pid;
    child->state = PROC_STATE_CREATED;
    child->next  = child->prev = NULL;
    child->total_ticks = 0;
    child->time_slice  = SCHED_DEFAULT_QUANTUM;

    /* Clone address space (copy-on-write) */
    child->address_space = vmm_clone_address_space(parent->address_space);
    if (!child->address_space) {
        child->state = PROC_STATE_UNUSED;
        return NULL;
    }

    /* Allocate a new kernel stack for the child */
    void* kstack_phys = pmm_alloc_frames(KSTACK_SIZE / PAGE_SIZE);
    if (!kstack_phys) {
        vmm_destroy_address_space(child->address_space);
        child->state = PROC_STATE_UNUSED;
        return NULL;
    }

    child->kernel_stack      = (uint64_t)PHYS_TO_VIRT(kstack_phys);
    child->kernel_stack_size = KSTACK_SIZE;

    /* The child context is a copy of the parent's context.
     * The child will "return" from fork() with return value 0 (set in syscall). */

    process_list_add(child);
    kdebug("Forked PID=%u -> child PID=%u", parent->pid, child->pid);
    return child;
}

/* ============================================================
 * process_exec - replace the current process image with a new ELF
 * ============================================================ */

int process_exec(process_t* proc, const char* path, char* const argv[])
{
    (void)argv;
    /* Open the ELF file */
    vfs_node_t* node = vfs_open(path, O_RDONLY);
    if (!node) {
        kerror("exec: cannot open '%s'", path);
        return -ENOENT;
    }

    /* Read ELF into a temporary buffer */
    size_t file_size = node->size;
    void* elf_data = kmalloc(file_size);
    if (!elf_data) {
        vfs_close(node);
        return -ENOMEM;
    }

    if (vfs_read(node, 0, file_size, elf_data) != (ssize_t)file_size) {
        kfree(elf_data);
        vfs_close(node);
        return -EIO;
    }
    vfs_close(node);

    /* Validate ELF */
    if (!elf_validate(elf_data, file_size)) {
        kfree(elf_data);
        return -EINVAL;
    }

    /* Destroy old address space and create a new one */
    vmm_destroy_address_space(proc->address_space);
    proc->address_space = vmm_create_address_space();
    if (!proc->address_space) {
        kfree(elf_data);
        kpanic("exec: failed to create new address space");
    }

    /* Load ELF into the new address space */
    uint64_t entry_point = 0;
    if (elf_load(proc, elf_data, file_size, &entry_point) != 0) {
        kfree(elf_data);
        return -ENOEXEC;
    }
    kfree(elf_data);

    /* Set up user stack */
    uint64_t user_stack_top = USER_STACK_TOP;
    for (uint64_t addr = user_stack_top - USER_STACK_SIZE;
         addr < user_stack_top; addr += PAGE_SIZE)
    {
        void* phys = pmm_alloc_frame();
        if (!phys) return -ENOMEM;
        vmm_map_page(proc->address_space, addr, (uint64_t)phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }
    proc->user_stack = user_stack_top;

    /* Update process context to run in ring 3 */
    proc->context.rip    = entry_point;
    proc->context.rsp    = user_stack_top - 8;
    proc->context.rflags = 0x202;
    proc->context.cs     = GDT_USER_CODE | 3;
    proc->context.ss     = GDT_USER_DATA | 3;

    strncpy(proc->name, path, MAX_NAME_LEN - 1);

    kinfo("exec: loaded '%s', entry=0x%llx", path, entry_point);
    return 0;
}

/* ============================================================
 * process_exit - terminate a process
 * ============================================================ */

void process_exit(process_t* proc, int exit_code)
{
    if (!proc || proc->state == PROC_STATE_DEAD) return;

    proc->exit_code = exit_code;
    proc->state     = PROC_STATE_ZOMBIE;

    kdebug("Process '%s' (PID=%u) exited with code %d",
           proc->name, proc->pid, exit_code);

    /* Close all open file descriptors */
    for (int i = 0; i < MAX_FDS; i++) {
        if (proc->fds[i].file) {
            fd_close(proc, i);
        }
    }

    /* Free address space (if not kernel) */
    if (proc->address_space != kernel_pml4) {
        vmm_destroy_address_space(proc->address_space);
        proc->address_space = NULL;
    }

    /* Free kernel stack */
    if (proc->kernel_stack) {
        pmm_free_frames((void*)VIRT_TO_PHYS(proc->kernel_stack),
                        proc->kernel_stack_size / PAGE_SIZE);
        proc->kernel_stack = 0;
    }

    proc->state = PROC_STATE_DEAD;
    process_list_remove(proc);
    proc->state = PROC_STATE_UNUSED;

    /* Yield to the scheduler */
    if (proc == current_process) {
        current_process = NULL;
        scheduler_yield();
    }
}

/* ============================================================
 * process_kill - send a signal to a process
 * ============================================================ */

void process_kill(pid_t pid, int signal)
{
    process_t* proc = process_get_by_pid(pid);
    if (!proc) return;

    signal_send_proc(proc, signal);
}

process_t* process_get_by_pid(pid_t pid)
{
    process_t* proc = process_list;
    while (proc) {
        if (proc->pid == pid) return proc;
        proc = proc->next;
    }
    return NULL;
}

/* ============================================================
 * Sleep / wake
 * ============================================================ */

void process_sleep(uint64_t ticks)
{
    timer_sleep_ticks(ticks);
}

void process_wake(process_t* proc)
{
    scheduler_unblock(proc);
}

/* ============================================================
 * File descriptor management
 * ============================================================ */

int fd_alloc(process_t* proc)
{
    for (int i = 0; i < MAX_FDS; i++) {
        if (!proc->fds[i].file) return i;
    }
    return -1;
}

void fd_close(process_t* proc, int fd)
{
    if (fd < 0 || fd >= MAX_FDS) return;
    if (proc->fds[fd].file) {
        vfs_close(proc->fds[fd].file->node);
        proc->fds[fd].file   = NULL;
        proc->fds[fd].flags  = 0;
        proc->fds[fd].offset = 0;
    }
}

/* ============================================================
 * Idle process
 * ============================================================ */

void idle_process(void)
{
    while (1) {
        cpu_halt();
    }
}

/* ============================================================
 * waitpid - wait for a child process to change state
 * ============================================================ */

pid_t process_waitpid(pid_t pid, int* status, int options)
{
    (void)options;

    if (!current_process) return -ESRCH;

    /* Spin until the target child becomes a zombie */
    for (int attempts = 0; attempts < 10000; attempts++) {
        process_t* p = process_list;
        while (p) {
            if ((pid == (pid_t)-1 || p->pid == pid) &&
                p->ppid == current_process->pid &&
                p->state == PROC_STATE_ZOMBIE) {
                pid_t child_pid = p->pid;
                if (status) *status = p->exit_code;
                p->state = PROC_STATE_DEAD;
                return child_pid;
            }
            p = p->next;
        }
        scheduler_yield();
    }

    return -ECHILD;
}

/* ============================================================
 * Credential management
 * ============================================================ */

bool process_is_root(process_t* proc)
{
    return proc && proc->euid == 0;
}

int process_setuid(process_t* proc, uint32_t uid)
{
    if (!proc) return -ESRCH;
    /* Only root can set UID to arbitrary value */
    if (proc->euid != 0 && uid != proc->uid) return -EPERM;
    proc->uid  = uid;
    proc->euid = uid;
    return 0;
}

int process_setgid(process_t* proc, uint32_t gid)
{
    if (!proc) return -ESRCH;
    if (proc->egid != 0 && gid != proc->gid) return -EPERM;
    proc->gid  = gid;
    proc->egid = gid;
    return 0;
}

/* ============================================================
 * FD dup/dup2
 * ============================================================ */

int fd_dup(process_t* proc, int fd)
{
    if (!proc || fd < 0 || fd >= MAX_FDS) return -EBADF;
    if (!proc->fds[fd].file) return -EBADF;

    int new_fd = fd_alloc(proc);
    if (new_fd < 0) return -EMFILE;

    proc->fds[new_fd] = proc->fds[fd];
    if (proc->fds[new_fd].file) proc->fds[new_fd].file->refcount++;
    return new_fd;
}

int fd_dup2(process_t* proc, int oldfd, int newfd)
{
    if (!proc || oldfd < 0 || oldfd >= MAX_FDS || newfd < 0 || newfd >= MAX_FDS)
        return -EBADF;
    if (!proc->fds[oldfd].file) return -EBADF;
    if (oldfd == newfd) return newfd;

    if (proc->fds[newfd].file) fd_close(proc, newfd);

    proc->fds[newfd] = proc->fds[oldfd];
    if (proc->fds[newfd].file) proc->fds[newfd].file->refcount++;
    return newfd;
}
