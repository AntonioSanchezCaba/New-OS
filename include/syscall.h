/*
 * syscall.h - System call numbers and interface for Aureon OS
 *
 * System calls are invoked from userspace via "int 0x80".
 * RAX = syscall number; args in RDI, RSI, RDX, R10, R8, R9.
 * Return value in RAX (negative = -errno).
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <types.h>
#include <interrupts.h>

/* ── File I/O (0..29) ────────────────────────────────────────────────── */
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
#define SYS_IOCTL       10
#define SYS_READDIR     11
#define SYS_MKDIR       12
#define SYS_RMDIR       13
#define SYS_UNLINK      14
#define SYS_RENAME      15
#define SYS_CHDIR       16
#define SYS_GETCWD      17
#define SYS_DUP         18
#define SYS_DUP2        19
#define SYS_PIPE        20
#define SYS_FCNTL       21
#define SYS_TRUNCATE    22
#define SYS_FTRUNCATE   23
#define SYS_SYNC        24
#define SYS_FSYNC       25
#define SYS_LINK        26
#define SYS_SYMLINK     27
#define SYS_READLINK    28
#define SYS_CHMOD       29

/* ── Process management (30..49) ─────────────────────────────────────── */
#define SYS_FORK        30
#define SYS_EXEC        31
#define SYS_EXIT        32
#define SYS_GETPID      33
#define SYS_GETPPID     34
#define SYS_SLEEP       35
#define SYS_KILL        36
#define SYS_WAITPID     37
#define SYS_YIELD       38
#define SYS_GETUID      39
#define SYS_SETUID      40
#define SYS_GETGID      41
#define SYS_SETGID      42
#define SYS_GETEUID     43
#define SYS_GETEGID     44
#define SYS_GETPGID     45
#define SYS_SETPGID     46
#define SYS_SETSID      47
#define SYS_PRCTL       48
#define SYS_CLONE       49

/* ── Signal handling (50..59) ────────────────────────────────────────── */
#define SYS_SIGACTION   50
#define SYS_SIGPROCMASK 51
#define SYS_SIGRETURN   52
#define SYS_SIGSUSPEND  53
#define SYS_SIGPENDING  54
#define SYS_ALARM       55
#define SYS_PAUSE       56

/* ── Memory management (60..69) ──────────────────────────────────────── */
#define SYS_MPROTECT    60
#define SYS_MADVISE     61
#define SYS_MREMAP      62

/* ── Networking / Sockets (70..89) ───────────────────────────────────── */
#define SYS_SOCKET      70
#define SYS_BIND        71
#define SYS_CONNECT     72
#define SYS_LISTEN      73
#define SYS_ACCEPT      74
#define SYS_SEND        75
#define SYS_RECV        76
#define SYS_SENDTO      77
#define SYS_RECVFROM    78
#define SYS_SETSOCKOPT  79
#define SYS_GETSOCKOPT  80
#define SYS_SHUTDOWN    81
#define SYS_GETPEERNAME 82
#define SYS_GETSOCKNAME 83
#define SYS_SELECT      84
#define SYS_POLL        85

/* ── Shared memory (90..94) ──────────────────────────────────────────── */
#define SYS_SHM_OPEN    90
#define SYS_SHM_MAP     91
#define SYS_SHM_UNMAP   92
#define SYS_SHM_CLOSE   93

/* ── IPC (95..99) ────────────────────────────────────────────────────── */
#define SYS_IPC_SEND    95
#define SYS_IPC_RECV    96
#define SYS_IPC_REPLY   97

/* ── Time (100..104) ─────────────────────────────────────────────────── */
#define SYS_UPTIME      100
#define SYS_GETTIMEOFDAY 101
#define SYS_SETTIMEOFDAY 102
#define SYS_NANOSLEEP   103
#define SYS_CLOCK_GETTIME 104

/* ── System info (105..109) ──────────────────────────────────────────── */
#define SYS_UNAME       105
#define SYS_SYSINFO     106
#define SYS_GETRLIMIT   107
#define SYS_SETRLIMIT   108

/* ── Package manager (110..114) ──────────────────────────────────────── */
#define SYS_PKG_INSTALL 110
#define SYS_PKG_REMOVE  111
#define SYS_PKG_LIST    112

/* ── GUI / Display (120..129) ────────────────────────────────────────── */
#define SYS_FB_INFO     120
#define SYS_FB_FLIP     121
#define SYS_WIN_CREATE  122
#define SYS_WIN_DESTROY 123
#define SYS_WIN_SHOW    124
#define SYS_WIN_MOVE    125
#define SYS_WIN_RESIZE  126
#define SYS_EVT_POLL    127
#define SYS_EVT_WAIT    128

#define MAX_SYSCALLS    256

/* ── errno values ────────────────────────────────────────────────────── */
#define ESUCCESS        0
#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define ENXIO           6
#define E2BIG           7
#define ENOEXEC         8
#define EBADF           9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EPIPE           32
#define ERANGE          34
#define ENOSYS          38
#define ENOTEMPTY       39
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define EPROTOTYPE      91
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP      95
#define ENOTSUP         95
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ECONNRESET      104
#define ENOBUFS         105
#define EISCONN         106
#define ENOTCONN        107
#define ETIMEDOUT       110
#define ECONNREFUSED    111

/* ── stat structure ──────────────────────────────────────────────────── */
typedef struct {
    uint64_t dev;
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t blksize;
    uint64_t blocks;
} stat_t;

/* mode bits */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_IRWXU  0700
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100
#define S_IRWXG  070
#define S_IRWXO  007

/* O_* flags for open */
#ifndef O_RDONLY
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_EXCL      0x80
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#endif

/* lseek whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* uname */
typedef struct {
    char sysname[65];   /* "Aureon" */
    char nodename[65];  /* hostname */
    char release[65];   /* "1.0.0" */
    char version[65];   /* Build date */
    char machine[65];   /* "x86_64" */
} utsname_t;

/* sysinfo */
typedef struct {
    uint64_t uptime;
    uint64_t totalram;
    uint64_t freeram;
    uint32_t procs;
    uint32_t cpu_freq_mhz;
} sysinfo_t;

/* ── Syscall handler type ────────────────────────────────────────────── */
typedef int64_t (*syscall_fn_t)(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                 uint64_t arg4, uint64_t arg5, uint64_t arg6);

void syscall_init(void);
void syscall_handler(cpu_registers_t* regs);

#endif /* SYSCALL_H */
