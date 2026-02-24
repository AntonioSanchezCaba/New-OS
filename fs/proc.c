/*
 * fs/proc.c - Process filesystem (procfs)
 *
 * Read-only virtual filesystem mounted at /proc.
 * File contents are generated on-the-fly from kernel state.
 *
 * Mount:  vfs_mount("/proc", &g_proc_root, NULL)  — called from procfs_init()
 *
 * Directory layout:
 *   /proc/uptime          seconds.centiseconds since boot
 *   /proc/meminfo         total/free/used memory in kB
 *   /proc/version         kernel version string
 *   /proc/<pid>/          one directory per live process
 *   /proc/<pid>/status    process name, state, pid, ppid, uid, ticks
 *   /proc/<pid>/cmdline   null-terminated process name
 */
#include <fs/proc.h>
#include <fs/vfs.h>
#include <process.h>
#include <memory.h>
#include <kernel.h>
#include <string.h>
#include <drivers/timer.h>
#include <kernel/version.h>

/* ── Node type tags ───────────────────────────────────────────────────── */
#define PROC_T_ROOT        0
#define PROC_T_UPTIME      1
#define PROC_T_MEMINFO     2
#define PROC_T_VERSION     3
#define PROC_T_PID_DIR     4
#define PROC_T_PID_STATUS  5
#define PROC_T_PID_CMDLINE 6

typedef struct {
    int   type;
    pid_t pid;
} proc_priv_t;

/* ── Static buffers ───────────────────────────────────────────────────── */
/* Only one readdir outstanding at a time in a single-tasking kernel */
static vfs_dirent_t g_proc_dirent;
/* Content generation scratch buffer */
#define PROC_BUF 2048
static char g_proc_buf[PROC_BUF];

/* ── Forward declarations ─────────────────────────────────────────────── */
static ssize_t        proc_file_read(vfs_node_t*, off_t, size_t, void*);
static vfs_node_t*    proc_root_finddir(vfs_node_t*, const char*);
static vfs_dirent_t*  proc_root_readdir(vfs_node_t*, uint32_t);
static vfs_node_t*    proc_pid_finddir(vfs_node_t*, const char*);
static vfs_dirent_t*  proc_pid_readdir(vfs_node_t*, uint32_t);
static void           proc_dyn_close(vfs_node_t*);

/* ── VFS ops tables ───────────────────────────────────────────────────── */
static vfs_ops_t g_proc_root_ops = {
    .read    = NULL,
    .write   = NULL,
    .finddir = proc_root_finddir,
    .readdir = proc_root_readdir,
};

static vfs_ops_t g_proc_file_ops = {
    .read  = proc_file_read,
    .write = NULL,
    .close = proc_dyn_close,
};

static vfs_ops_t g_proc_pid_ops = {
    .read    = NULL,
    .write   = NULL,
    .finddir = proc_pid_finddir,
    .readdir = proc_pid_readdir,
    .close   = proc_dyn_close,
};

/* Persistent root node (never freed) */
static vfs_node_t  g_proc_root;
static proc_priv_t g_proc_root_priv;

/* ── Node allocation ──────────────────────────────────────────────────── */

/*
 * Allocate a dynamic proc node.  The proc_priv_t is placed immediately
 * after the vfs_node_t in a single allocation; proc_dyn_close() frees it.
 */
static vfs_node_t* proc_alloc(const char* name, uint32_t flags,
                               int type, pid_t pid, vfs_ops_t* ops)
{
    size_t total = sizeof(vfs_node_t) + sizeof(proc_priv_t);
    uint8_t* mem = (uint8_t*)kmalloc(total);
    if (!mem) return NULL;
    memset(mem, 0, total);

    vfs_node_t*  n  = (vfs_node_t*)mem;
    proc_priv_t* pp = (proc_priv_t*)(mem + sizeof(vfs_node_t));

    strncpy(n->name, name, VFS_NAME_MAX);
    n->flags    = flags;
    n->mode     = (flags & VFS_DIRECTORY) ? 0555 : 0444;
    n->ops      = ops;
    n->refcount = 1;
    n->impl     = pp;

    pp->type = type;
    pp->pid  = pid;

    return n;
}

static void proc_dyn_close(vfs_node_t* node)
{
    kfree(node);   /* frees the combined vfs_node_t + proc_priv_t block */
}

/* ── Content generators ───────────────────────────────────────────────── */

static int gen_uptime(char* buf, int sz)
{
    uint32_t ticks   = timer_get_ticks();
    uint32_t secs    = ticks / 100;
    uint32_t centis  = ticks % 100;
    return snprintf(buf, (size_t)sz, "%u.%02u 0.00\n", secs, centis);
}

static int gen_meminfo(char* buf, int sz)
{
    size_t total_kb = pmm_total_frames() * (4096 / 1024);
    size_t free_kb  = pmm_free_frames_count() * (4096 / 1024);
    size_t used_kb  = total_kb - free_kb;
    return snprintf(buf, (size_t)sz,
        "MemTotal:  %8zu kB\n"
        "MemFree:   %8zu kB\n"
        "MemUsed:   %8zu kB\n",
        total_kb, free_kb, used_kb);
}

static int gen_version(char* buf, int sz)
{
    return snprintf(buf, (size_t)sz,
        "%s version " OS_VERSION " (" OS_BUILD_DATE ")\n", OS_NAME);
}

static int gen_pid_status(char* buf, int sz, pid_t pid)
{
    process_t* p = process_get_by_pid(pid);
    if (!p)
        return snprintf(buf, (size_t)sz, "Name:\t(gone)\nState:\tZ\n");

    static const char state_chars[] = "UCRRSSWX";
    char sc = (p->state < 8) ? state_chars[p->state] : '?';

    return snprintf(buf, (size_t)sz,
        "Name:\t%s\n"
        "State:\t%c\n"
        "Pid:\t%u\n"
        "PPid:\t%u\n"
        "Uid:\t%u\n"
        "VmSize:\t%llu kB\n"
        "Ticks:\t%llu\n",
        p->name, sc,
        (unsigned)p->pid, (unsigned)p->ppid, (unsigned)p->uid,
        (unsigned long long)((p->heap_end > p->heap_start)
                              ? (p->heap_end - p->heap_start) / 1024 : 0),
        (unsigned long long)p->total_ticks);
}

static int gen_pid_cmdline(char* buf, int sz, pid_t pid)
{
    process_t* p = process_get_by_pid(pid);
    if (!p || p->name[0] == '\0') return 0;
    int len = (int)strlen(p->name);
    if (len >= sz) len = sz - 1;
    memcpy(buf, p->name, (size_t)len);
    buf[len] = '\0';
    return len + 1;  /* include NUL */
}

/* ── proc_file_read ───────────────────────────────────────────────────── */

static ssize_t proc_file_read(vfs_node_t* node, off_t offset, size_t size,
                               void* buf)
{
    if (!node || !buf || !node->impl) return -EIO;
    proc_priv_t* pp = (proc_priv_t*)node->impl;

    int n = 0;
    switch (pp->type) {
    case PROC_T_UPTIME:      n = gen_uptime(g_proc_buf, PROC_BUF);   break;
    case PROC_T_MEMINFO:     n = gen_meminfo(g_proc_buf, PROC_BUF);  break;
    case PROC_T_VERSION:     n = gen_version(g_proc_buf, PROC_BUF);  break;
    case PROC_T_PID_STATUS:  n = gen_pid_status(g_proc_buf, PROC_BUF, pp->pid); break;
    case PROC_T_PID_CMDLINE: n = gen_pid_cmdline(g_proc_buf, PROC_BUF, pp->pid); break;
    default: return -EIO;
    }

    if (n < 0) n = 0;
    if ((size_t)offset >= (size_t)n) return 0;

    size_t avail   = (size_t)n - (size_t)offset;
    size_t to_copy = (size < avail) ? size : avail;
    memcpy(buf, g_proc_buf + offset, to_copy);
    return (ssize_t)to_copy;
}

/* ── Root directory (/proc/) ──────────────────────────────────────────── */

/* Fixed entries in /proc/ */
#define PROC_STATIC_N  3
static const char* proc_static_name[PROC_STATIC_N] = { "uptime","meminfo","version" };
static const int   proc_static_type[PROC_STATIC_N] = { PROC_T_UPTIME, PROC_T_MEMINFO, PROC_T_VERSION };

static vfs_dirent_t* proc_root_readdir(vfs_node_t* node, uint32_t idx)
{
    (void)node;

    /* Fixed files */
    if (idx < PROC_STATIC_N) {
        strncpy(g_proc_dirent.name, proc_static_name[idx], VFS_NAME_MAX);
        g_proc_dirent.type = VFS_TYPE_FILE;
        g_proc_dirent.size = 0;
        return &g_proc_dirent;
    }

    /* One entry per live process */
    uint32_t want = idx - PROC_STATIC_N;
    uint32_t cnt  = 0;
    for (process_t* p = process_list; p; p = p->next) {
        if (p->state == PROC_STATE_UNUSED || p->state == PROC_STATE_DEAD)
            continue;
        if (cnt == want) {
            /* Convert PID to string */
            char tmp[16];
            uint32_t pid = (uint32_t)p->pid;
            int ti = 0;
            if (pid == 0) {
                tmp[ti++] = '0';
            } else {
                char rev[12]; int ri = 0;
                while (pid) { rev[ri++] = (char)('0' + pid % 10); pid /= 10; }
                while (ri > 0) tmp[ti++] = rev[--ri];
            }
            tmp[ti] = '\0';
            strncpy(g_proc_dirent.name, tmp, VFS_NAME_MAX);
            g_proc_dirent.type = VFS_TYPE_DIR;
            g_proc_dirent.size = 0;
            return &g_proc_dirent;
        }
        cnt++;
    }
    return NULL;
}

static vfs_node_t* proc_root_finddir(vfs_node_t* node, const char* name)
{
    (void)node;

    /* Check fixed files */
    for (int i = 0; i < PROC_STATIC_N; i++) {
        if (strcmp(name, proc_static_name[i]) == 0)
            return proc_alloc(name, VFS_FILE, proc_static_type[i], 0,
                              &g_proc_file_ops);
    }

    /* Numeric PID? */
    if (name[0] == '\0') return NULL;
    for (const char* c = name; *c; c++)
        if (*c < '0' || *c > '9') return NULL;

    pid_t pid = (pid_t)strtoul(name, NULL, 10);
    process_t* p = process_get_by_pid(pid);
    if (!p || p->state == PROC_STATE_UNUSED || p->state == PROC_STATE_DEAD)
        return NULL;

    return proc_alloc(name, VFS_DIRECTORY, PROC_T_PID_DIR, pid, &g_proc_pid_ops);
}

/* ── PID directory (/proc/<pid>/) ─────────────────────────────────────── */

static const char* pid_file_names[2] = { "status", "cmdline" };
static const int   pid_file_types[2] = { PROC_T_PID_STATUS, PROC_T_PID_CMDLINE };

static vfs_dirent_t* proc_pid_readdir(vfs_node_t* node, uint32_t idx)
{
    (void)node;
    if (idx >= 2) return NULL;
    strncpy(g_proc_dirent.name, pid_file_names[idx], VFS_NAME_MAX);
    g_proc_dirent.type = VFS_TYPE_FILE;
    g_proc_dirent.size = 0;
    return &g_proc_dirent;
}

static vfs_node_t* proc_pid_finddir(vfs_node_t* node, const char* name)
{
    if (!node || !node->impl) return NULL;
    proc_priv_t* pp = (proc_priv_t*)node->impl;
    for (int i = 0; i < 2; i++) {
        if (strcmp(name, pid_file_names[i]) == 0)
            return proc_alloc(name, VFS_FILE, pid_file_types[i], pp->pid,
                              &g_proc_file_ops);
    }
    return NULL;
}

/* ── procfs_init ──────────────────────────────────────────────────────── */

void procfs_init(void)
{
    memset(&g_proc_root,      0, sizeof(g_proc_root));
    memset(&g_proc_root_priv, 0, sizeof(g_proc_root_priv));

    strncpy(g_proc_root.name, "proc", VFS_NAME_MAX);
    g_proc_root.flags  = VFS_DIRECTORY;
    g_proc_root.mode   = 0555;
    g_proc_root.ops    = &g_proc_root_ops;
    g_proc_root.impl   = &g_proc_root_priv;
    g_proc_root_priv.type = PROC_T_ROOT;

    int r = vfs_mount("/proc", &g_proc_root, NULL);
    if (r != 0)
        kwarn("procfs: failed to mount at /proc (err %d)", r);
    else
        kinfo("procfs: mounted at /proc — uptime, meminfo, version, <pid>/status");
}
