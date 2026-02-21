/*
 * syscall/syscall.c - System call dispatch and implementations
 *
 * System calls are invoked from user space via "int 0x80".
 * The interrupt handler calls syscall_handler() with the saved register frame.
 *
 * Register convention (matching Linux x86_64 int 0x80 style):
 *   RAX = syscall number
 *   RDI = arg1, RSI = arg2, RDX = arg3, R10 = arg4, R8 = arg5, R9 = arg6
 *   Return value in RAX (negative = -errno)
 */
#include <syscall.h>
#include <process.h>
#include <scheduler.h>
#include <memory.h>
#include <fs/vfs.h>
#include <drivers/vga.h>
#include <drivers/timer.h>
#include <interrupts.h>
#include <kernel.h>
#include <types.h>
#include <string.h>

/* Syscall dispatch table */
static syscall_fn_t syscall_table[MAX_SYSCALLS];

/* ============================================================
 * sys_write - write bytes to a file descriptor
 * fd=1 (stdout) and fd=2 (stderr) go to VGA.
 * ============================================================ */

int64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;

    const char* buf = (const char*)buf_addr;
    if (!buf) return -EFAULT;

    /* stdout and stderr: write to VGA and serial */
    if (fd == 1 || fd == 2) {
        for (uint64_t i = 0; i < count; i++) {
            vga_putchar(buf[i]);
            debug_putchar(buf[i]);
        }
        return (int64_t)count;
    }

    /* Other file descriptors */
    if (!current_process) return -EBADF;
    if (fd >= MAX_FDS || !current_process->fds[fd].file) return -EBADF;

    fd_entry_t* fde  = &current_process->fds[fd];
    ssize_t written = vfs_write(fde->file->node, fde->offset,
                                 (size_t)count, buf);
    if (written > 0) fde->offset += written;
    return written;
}

/* ============================================================
 * sys_read - read bytes from a file descriptor
 * ============================================================ */

int64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count,
                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;

    char* buf = (char*)buf_addr;
    if (!buf) return -EFAULT;

    /* stdin: read from keyboard */
    if (fd == 0) {
        for (uint64_t i = 0; i < count; i++) {
            buf[i] = keyboard_getchar();
            if (buf[i] == '\n') {
                return (int64_t)(i + 1);
            }
        }
        return (int64_t)count;
    }

    if (!current_process) return -EBADF;
    if (fd >= MAX_FDS || !current_process->fds[fd].file) return -EBADF;

    fd_entry_t* fde = &current_process->fds[fd];
    ssize_t nread   = vfs_read(fde->file->node, fde->offset,
                                (size_t)count, buf);
    if (nread > 0) fde->offset += nread;
    return nread;
}

/* ============================================================
 * sys_open - open a file and return a file descriptor
 * ============================================================ */

int64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode,
                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)mode; (void)a4; (void)a5; (void)a6;

    const char* path = (const char*)path_addr;
    if (!path) return -EFAULT;
    if (!current_process) return -ESRCH;

    vfs_node_t* node = vfs_open(path, (int)flags);
    if (!node) return -ENOENT;

    int fd = fd_alloc(current_process);
    if (fd < 0) {
        vfs_close(node);
        return -EMFILE;
    }

    /* Create a file object */
    file_t* file = (file_t*)kmalloc(sizeof(file_t));
    if (!file) {
        vfs_close(node);
        return -ENOMEM;
    }

    file->node     = node;
    file->offset   = 0;
    file->flags    = (int)flags;
    file->refcount = 1;

    current_process->fds[fd].file   = file;
    current_process->fds[fd].flags  = (int)flags;
    current_process->fds[fd].offset = 0;

    return fd;
}

/* ============================================================
 * sys_close
 * ============================================================ */

int64_t sys_close(uint64_t fd, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!current_process) return -ESRCH;
    if (fd >= MAX_FDS) return -EBADF;
    if (!current_process->fds[fd].file) return -EBADF;

    fd_close(current_process, (int)fd);
    return 0;
}

/* ============================================================
 * sys_fork
 * ============================================================ */

int64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3,
                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!current_process) return -ESRCH;

    process_t* child = process_fork(current_process);
    if (!child) return -ENOMEM;

    /* In child: fork() returns 0 */
    /* We set this in the saved context's RAX */
    child->context.rip = current_process->context.rip;

    scheduler_add(child);

    /* In parent: fork() returns child PID */
    return (int64_t)child->pid;
}

/* ============================================================
 * sys_exec
 * ============================================================ */

int64_t sys_exec(uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr,
                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)envp_addr; (void)a4; (void)a5; (void)a6;

    const char* path = (const char*)path_addr;
    if (!path || !current_process) return -EFAULT;

    return process_exec(current_process, path, (char* const*)argv_addr);
}

/* ============================================================
 * sys_exit
 * ============================================================ */

int64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3,
                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (current_process) {
        process_exit(current_process, (int)code);
    }
    scheduler_yield();
    return 0; /* Never reached */
}

/* ============================================================
 * sys_getpid
 * ============================================================ */

int64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    return current_process ? (int64_t)current_process->pid : 1;
}

/* ============================================================
 * sys_sleep - sleep for @ms milliseconds
 * ============================================================ */

int64_t sys_sleep(uint64_t ms, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    timer_sleep_ms(ms);
    return 0;
}

/* ============================================================
 * sys_kill
 * ============================================================ */

int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t a3,
                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;

    process_kill((pid_t)pid, (int)sig);
    return 0;
}

/* ============================================================
 * sys_brk - set the process break (expand heap)
 * ============================================================ */

int64_t sys_brk(uint64_t new_brk, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!current_process) return -ESRCH;

    if (new_brk == 0) return (int64_t)current_process->heap_end;

    if (new_brk < current_process->heap_start) return -EINVAL;

    /* Map new pages if expanding */
    uint64_t old_end = ALIGN_UP(current_process->heap_end, PAGE_SIZE);
    uint64_t new_end = ALIGN_UP(new_brk, PAGE_SIZE);

    if (new_end > old_end) {
        for (uint64_t addr = old_end; addr < new_end; addr += PAGE_SIZE) {
            void* phys = pmm_alloc_frame();
            if (!phys) return -ENOMEM;
            vmm_map_page(current_process->address_space, addr, (uint64_t)phys,
                         PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        }
    }

    current_process->heap_end = new_brk;
    return (int64_t)new_brk;
}

/* ============================================================
 * sys_yield - voluntarily yield the CPU
 * ============================================================ */

int64_t sys_yield(uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    scheduler_yield();
    return 0;
}

/* ============================================================
 * syscall_init - populate the syscall table
 * ============================================================ */

void syscall_init(void)
{
    memset(syscall_table, 0, sizeof(syscall_table));

    syscall_table[SYS_READ]   = sys_read;
    syscall_table[SYS_WRITE]  = sys_write;
    syscall_table[SYS_OPEN]   = sys_open;
    syscall_table[SYS_CLOSE]  = sys_close;
    syscall_table[SYS_FORK]   = sys_fork;
    syscall_table[SYS_EXEC]   = sys_exec;
    syscall_table[SYS_EXIT]   = sys_exit;
    syscall_table[SYS_GETPID] = sys_getpid;
    syscall_table[SYS_SLEEP]  = sys_sleep;
    syscall_table[SYS_KILL]   = sys_kill;
    syscall_table[SYS_BRK]    = sys_brk;
    syscall_table[SYS_YIELD]  = sys_yield;

    kinfo("Syscall table initialized (%d syscalls)", MAX_SYSCALLS);
}

/* ============================================================
 * syscall_handler - called from the interrupt dispatcher (int 0x80)
 * ============================================================ */

void syscall_handler(cpu_registers_t* regs)
{
    uint64_t syscall_num = regs->rax;

    if (syscall_num >= MAX_SYSCALLS || !syscall_table[syscall_num]) {
        kwarn("Unknown syscall %llu from PID=%u",
              syscall_num,
              current_process ? current_process->pid : 0);
        regs->rax = (uint64_t)(-ENOSYS);
        return;
    }

    /* Dispatch to handler with up to 6 arguments */
    int64_t ret = syscall_table[syscall_num](
        regs->rdi, regs->rsi, regs->rdx,
        regs->r10, regs->r8,  regs->r9
    );

    regs->rax = (uint64_t)ret;
}

/* External function referenced in keyboard.c */
extern char keyboard_getchar(void);
