/*
 * include/fs/proc.h - Process filesystem (procfs)
 *
 * A read-only virtual filesystem mounted at /proc that exposes kernel
 * process table information as regular files.
 *
 * Files:
 *   /proc/uptime             - seconds.centiseconds since boot
 *   /proc/meminfo            - memory usage statistics
 *   /proc/version            - kernel version string
 *   /proc/<pid>/status       - process name, state, pid, ppid, uid, ticks
 *   /proc/<pid>/cmdline      - null-terminated process name
 */
#ifndef FS_PROC_H
#define FS_PROC_H

/* Mount the procfs at /proc.  Called during kernel filesystem init. */
void procfs_init(void);

#endif /* FS_PROC_H */
