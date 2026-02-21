/*
 * syscall.h - System call numbers and interface
 *
 * System calls are invoked from userspace via "int 0x80".
 * RAX holds the syscall number; arguments in RDI, RSI, RDX, R10, R8, R9.
 * Return value in RAX (negative = error).
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <types.h>
#include <interrupts.h>

/* System call numbers */
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_STAT        4
#define SYS_FSTAT       5
#define SYS_LSEEK       6
#define SYS_MMAP        7
#define SYS_MUNMAP      8
#define SYS_BRK         9
#define SYS_FORK        10
#define SYS_EXEC        11
#define SYS_EXIT        12
#define SYS_GETPID      13
#define SYS_SLEEP       14
#define SYS_KILL        15
#define SYS_GETPPID     16
#define SYS_CHDIR       17
#define SYS_GETCWD      18
#define SYS_MKDIR       19
#define SYS_RMDIR       20
#define SYS_UNLINK      21
#define SYS_READDIR     22
#define SYS_YIELD       23
#define SYS_UPTIME      24
#define SYS_WAITPID     25

#define MAX_SYSCALLS    256

/* Syscall error codes */
#define ESUCCESS        0
#define EPERM           1    /* Operation not permitted */
#define ENOENT          2    /* No such file or directory */
#define ESRCH           3    /* No such process */
#define EINTR           4    /* Interrupted system call */
#define EIO             5    /* I/O error */
#define ENOMEM          12   /* Out of memory */
#define EACCES          13   /* Permission denied */
#define EFAULT          14   /* Bad address */
#define ENOTDIR         20   /* Not a directory */
#define EISDIR          21   /* Is a directory */
#define EINVAL          22   /* Invalid argument */
#define EMFILE          24   /* Too many open files */
#define ENOSPC          28   /* No space left on device */
#define ENOSYS          38   /* Function not implemented */
#define ERANGE          34   /* Result too large */

/* Syscall handler type */
typedef int64_t (*syscall_fn_t)(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                 uint64_t arg4, uint64_t arg5, uint64_t arg6);

/* Syscall subsystem API */
void syscall_init(void);
void syscall_handler(cpu_registers_t* regs);

/* Individual syscall implementations */
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count, uint64_t, uint64_t, uint64_t);
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count, uint64_t, uint64_t, uint64_t);
int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, uint64_t, uint64_t, uint64_t);
int64_t sys_close(uint64_t fd, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_fork(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_exec(uint64_t path, uint64_t argv, uint64_t envp, uint64_t, uint64_t, uint64_t);
int64_t sys_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_sleep(uint64_t ms, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_brk(uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int64_t sys_yield(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

#endif /* SYSCALL_H */
