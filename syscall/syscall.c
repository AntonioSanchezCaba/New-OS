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
#include <drivers/framebuffer.h>
#include <interrupts.h>
#include <kernel.h>
#include <kernel/signal.h>
#include <kernel/shm.h>
#include <kernel/ipc.h>
#include <kernel/pkg.h>
#include <net/socket.h>
#include <gui/window.h>
#include <gui/event.h>
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
 * New syscall implementations
 * ============================================================ */

/* sys_stat */
int64_t sys_stat(uint64_t path_addr, uint64_t stat_addr, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    const char* path = (const char*)path_addr;
    stat_t*     st   = (stat_t*)stat_addr;
    if (!path || !st) return -EFAULT;

    vfs_node_t* node = vfs_open(path, 0);
    if (!node) return -ENOENT;

    vfs_stat_t vst;
    int r = vfs_stat(node, &vst);
    vfs_close(node);
    if (r != 0) return r;

    memset(st, 0, sizeof(*st));
    st->size  = vst.size;
    st->mode  = (vst.type == VFS_TYPE_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->nlink = 1;
    st->uid   = 0;
    st->gid   = 0;
    return 0;
}

/* sys_fstat */
int64_t sys_fstat(uint64_t fd, uint64_t stat_addr, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    stat_t* st = (stat_t*)stat_addr;
    if (!st || !current_process) return -EFAULT;
    if (fd >= MAX_FDS || !current_process->fds[fd].file) return -EBADF;

    vfs_stat_t vst;
    int r = vfs_stat(current_process->fds[fd].file->node, &vst);
    if (r != 0) return r;

    memset(st, 0, sizeof(*st));
    st->size  = vst.size;
    st->mode  = (vst.type == VFS_TYPE_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->nlink = 1;
    return 0;
}

/* sys_lseek */
int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (!current_process) return -ESRCH;
    if (fd >= MAX_FDS || !current_process->fds[fd].file) return -EBADF;

    fd_entry_t* fde  = &current_process->fds[fd];
    vfs_node_t* node = fde->file->node;
    vfs_stat_t  vst;

    switch (whence) {
    case SEEK_SET: fde->offset = offset; break;
    case SEEK_CUR: fde->offset += offset; break;
    case SEEK_END:
        if (vfs_stat(node, &vst) == 0) fde->offset = vst.size + offset;
        else return -EIO;
        break;
    default: return -EINVAL;
    }
    return (int64_t)fde->offset;
}

/* sys_mkdir */
int64_t sys_mkdir(uint64_t path_addr, uint64_t mode, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
    const char* path = (const char*)path_addr;
    if (!path) return -EFAULT;
    return vfs_mkdir(path);
}

/* sys_rmdir */
int64_t sys_rmdir(uint64_t path_addr, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    const char* path = (const char*)path_addr;
    if (!path) return -EFAULT;
    return vfs_rmdir(path);
}

/* sys_unlink */
int64_t sys_unlink(uint64_t path_addr, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    const char* path = (const char*)path_addr;
    if (!path) return -EFAULT;
    return vfs_unlink(path);
}

/* sys_readdir */
int64_t sys_readdir(uint64_t fd, uint64_t dirent_addr, uint64_t count,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)count; (void)a4; (void)a5; (void)a6;
    if (!current_process) return -ESRCH;
    if (fd >= MAX_FDS || !current_process->fds[fd].file) return -EBADF;

    vfs_dirent_t* de = (vfs_dirent_t*)dirent_addr;
    if (!de) return -EFAULT;

    fd_entry_t* fde = &current_process->fds[fd];
    int r = vfs_readdir(fde->file->node, (uint32_t)fde->offset, de);
    if (r == 0) fde->offset++;
    return r;
}

/* sys_chdir */
int64_t sys_chdir(uint64_t path_addr, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    const char* path = (const char*)path_addr;
    if (!path || !current_process) return -EFAULT;

    vfs_node_t* node = vfs_open(path, 0);
    if (!node) return -ENOENT;
    vfs_close(node);

    strncpy(current_process->cwd, path, MAX_PATH_LEN - 1);
    return 0;
}

/* sys_getcwd */
int64_t sys_getcwd(uint64_t buf_addr, uint64_t size, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    char* buf = (char*)buf_addr;
    if (!buf || size == 0 || !current_process) return -EFAULT;

    const char* cwd = current_process->cwd[0] ? current_process->cwd : "/";
    size_t len = strlen(cwd);
    if (len >= size) return -ERANGE;
    memcpy(buf, cwd, len + 1);
    return (int64_t)buf_addr;
}

/* sys_getppid */
int64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return current_process ? (int64_t)current_process->ppid : 0;
}

/* sys_waitpid */
int64_t sys_waitpid(uint64_t pid, uint64_t status_addr, uint64_t options,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    int* status = (int*)status_addr;
    return process_waitpid((pid_t)pid, status, (int)options);
}

/* sys_getuid / sys_geteuid / sys_getgid / sys_getegid */
int64_t sys_getuid(uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
  return current_process ? (int64_t)current_process->uid : 0; }

int64_t sys_geteuid(uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
  return current_process ? (int64_t)current_process->euid : 0; }

int64_t sys_getgid(uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
  return current_process ? (int64_t)current_process->gid : 0; }

int64_t sys_getegid(uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
  return current_process ? (int64_t)current_process->egid : 0; }

int64_t sys_setuid(uint64_t uid, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
  return process_setuid(current_process, (uint32_t)uid); }

int64_t sys_setgid(uint64_t gid, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
  return process_setgid(current_process, (uint32_t)gid); }

/* sys_sigaction */
int64_t sys_sigaction(uint64_t sig, uint64_t act_addr, uint64_t old_addr,
                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    const sigaction_t* act = (const sigaction_t*)act_addr;
    sigaction_t*       old = (sigaction_t*)old_addr;
    return signal_do_sigaction((int)sig, act, old);
}

/* sys_sigprocmask */
int64_t sys_sigprocmask(uint64_t how, uint64_t set_addr, uint64_t old_addr,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    const sigset_t* set = (const sigset_t*)set_addr;
    sigset_t*       old = (sigset_t*)old_addr;
    return signal_do_sigprocmask((int)how, set, old);
}

/* sys_sigpending */
int64_t sys_sigpending(uint64_t set_addr, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    sigset_t* set = (sigset_t*)set_addr;
    if (!set || !current_process) return -EFAULT;
    *set = current_process->sigstate.pending;
    return 0;
}

/* sys_uptime */
int64_t sys_uptime(uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return (int64_t)timer_get_ticks();
}

/* sys_uname */
int64_t sys_uname(uint64_t buf_addr, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    utsname_t* u = (utsname_t*)buf_addr;
    if (!u) return -EFAULT;
    strncpy(u->sysname,  "Aureon",  64);
    strncpy(u->nodename, "aureon",  64);
    strncpy(u->release,  "1.0.0",   64);
    strncpy(u->version,  "2026",    64);
    strncpy(u->machine,  "x86_64",  64);
    return 0;
}

/* sys_sysinfo */
int64_t sys_sysinfo(uint64_t buf_addr, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    sysinfo_t* si = (sysinfo_t*)buf_addr;
    if (!si) return -EFAULT;
    si->uptime       = timer_get_ticks() / 100;
    si->totalram     = (uint64_t)pmm_total_frames() * PAGE_SIZE;
    si->freeram      = (uint64_t)pmm_free_frames_count() * PAGE_SIZE;
    si->procs        = 0; /* TODO: count from process list */
    si->cpu_freq_mhz = 2400;
    return 0;
}

/* sys_mmap */
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset)
{
    (void)prot; (void)flags; (void)fd; (void)offset;
    if (!current_process || length == 0) return -EINVAL;

    length = ALIGN_UP(length, PAGE_SIZE);
    uint64_t vaddr = addr ? addr : (current_process->heap_end);
    vaddr = ALIGN_UP(vaddr, PAGE_SIZE);

    for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
        void* phys = pmm_alloc_frame();
        if (!phys) return -ENOMEM;
        memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        vmm_map_page(current_process->address_space, vaddr + off, (uint64_t)phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    current_process->heap_end = vaddr + length;
    return (int64_t)vaddr;
}

/* sys_munmap */
int64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!current_process || !addr) return -EINVAL;
    length = ALIGN_UP(length, PAGE_SIZE);
    for (uint64_t off = 0; off < length; off += PAGE_SIZE) {
        uint64_t phys = vmm_get_physical(current_process->address_space, addr + off);
        if (phys) {
            vmm_unmap_page(current_process->address_space, addr + off);
            pmm_free_frame((void*)phys);
        }
    }
    return 0;
}

/* sys_dup */
int64_t sys_dup(uint64_t fd, uint64_t a2, uint64_t a3,
                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!current_process) return -ESRCH;
    return fd_dup(current_process, (int)fd);
}

/* sys_dup2 */
int64_t sys_dup2(uint64_t old_fd, uint64_t new_fd, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!current_process) return -ESRCH;
    return fd_dup2(current_process, (int)old_fd, (int)new_fd);
}

/* sys_fb_info */
typedef struct { uint32_t w, h, bpp, pitch; } fb_info_t;
int64_t sys_fb_info(uint64_t buf_addr, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    fb_info_t* info = (fb_info_t*)buf_addr;
    if (!info) return -EFAULT;
    if (!fb_ready()) return -ENODEV;
    info->w     = fb.width;
    info->h     = fb.height;
    info->bpp   = fb.bpp;
    info->pitch = fb.pitch;
    return 0;
}

/* sys_fb_flip */
int64_t sys_fb_flip(uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    fb_flip_damage();
    return 0;
}

/* ============================================================
 * syscall_init - populate the syscall table
 * ============================================================ */

void syscall_init(void)
{
    memset(syscall_table, 0, sizeof(syscall_table));

    /* File I/O */
    syscall_table[SYS_READ]      = sys_read;
    syscall_table[SYS_WRITE]     = sys_write;
    syscall_table[SYS_OPEN]      = sys_open;
    syscall_table[SYS_CLOSE]     = sys_close;
    syscall_table[SYS_STAT]      = sys_stat;
    syscall_table[SYS_FSTAT]     = sys_fstat;
    syscall_table[SYS_LSEEK]     = sys_lseek;
    syscall_table[SYS_MMAP]      = sys_mmap;
    syscall_table[SYS_MUNMAP]    = sys_munmap;
    syscall_table[SYS_BRK]       = sys_brk;
    syscall_table[SYS_READDIR]   = sys_readdir;
    syscall_table[SYS_MKDIR]     = sys_mkdir;
    syscall_table[SYS_RMDIR]     = sys_rmdir;
    syscall_table[SYS_UNLINK]    = sys_unlink;
    syscall_table[SYS_CHDIR]     = sys_chdir;
    syscall_table[SYS_GETCWD]    = sys_getcwd;
    syscall_table[SYS_DUP]       = sys_dup;
    syscall_table[SYS_DUP2]      = sys_dup2;

    /* Process management */
    syscall_table[SYS_FORK]      = sys_fork;
    syscall_table[SYS_EXEC]      = sys_exec;
    syscall_table[SYS_EXIT]      = sys_exit;
    syscall_table[SYS_GETPID]    = sys_getpid;
    syscall_table[SYS_GETPPID]   = sys_getppid;
    syscall_table[SYS_SLEEP]     = sys_sleep;
    syscall_table[SYS_KILL]      = sys_kill;
    syscall_table[SYS_WAITPID]   = sys_waitpid;
    syscall_table[SYS_YIELD]     = sys_yield;
    syscall_table[SYS_GETUID]    = sys_getuid;
    syscall_table[SYS_SETUID]    = sys_setuid;
    syscall_table[SYS_GETGID]    = sys_getgid;
    syscall_table[SYS_SETGID]    = sys_setgid;
    syscall_table[SYS_GETEUID]   = sys_geteuid;
    syscall_table[SYS_GETEGID]   = sys_getegid;

    /* Signals */
    syscall_table[SYS_SIGACTION]   = sys_sigaction;
    syscall_table[SYS_SIGPROCMASK] = sys_sigprocmask;
    syscall_table[SYS_SIGPENDING]  = sys_sigpending;

    /* Sockets */
    syscall_table[SYS_SOCKET]     = sys_socket;
    syscall_table[SYS_BIND]       = sys_bind;
    syscall_table[SYS_CONNECT]    = sys_connect;
    syscall_table[SYS_LISTEN]     = sys_listen;
    syscall_table[SYS_ACCEPT]     = sys_accept;
    syscall_table[SYS_SEND]       = sys_send;
    syscall_table[SYS_RECV]       = sys_recv;
    syscall_table[SYS_SENDTO]     = sys_sendto;
    syscall_table[SYS_RECVFROM]   = sys_recvfrom;

    /* Shared memory */
    syscall_table[SYS_SHM_OPEN]   = sys_shm_open;
    syscall_table[SYS_SHM_MAP]    = sys_shm_map;
    syscall_table[SYS_SHM_UNMAP]  = sys_shm_unmap;
    syscall_table[SYS_SHM_CLOSE]  = sys_shm_close;

    /* Time */
    syscall_table[SYS_UPTIME]     = sys_uptime;

    /* System info */
    syscall_table[SYS_UNAME]      = sys_uname;
    syscall_table[SYS_SYSINFO]    = sys_sysinfo;

    /* Package manager */
    syscall_table[SYS_PKG_INSTALL] = sys_pkg_install;
    syscall_table[SYS_PKG_REMOVE]  = sys_pkg_remove;
    syscall_table[SYS_PKG_LIST]    = sys_pkg_list;

    /* GUI */
    syscall_table[SYS_FB_INFO]    = sys_fb_info;
    syscall_table[SYS_FB_FLIP]    = sys_fb_flip;

    kinfo("Syscall table initialized (%d syscalls registered)", MAX_SYSCALLS);
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

    /* Deliver any pending signals before returning to userspace */
    if (current_process && signal_has_pending(current_process)) {
        signal_deliver_pending(current_process);
    }
}

/* External function referenced in keyboard.c */
extern char keyboard_getchar(void);
