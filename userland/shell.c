/*
 * userland/shell.c - Simple interactive kernel shell
 *
 * Provides a command-line interface that runs as a kernel thread (ring 0).
 * Supports built-in commands:
 *   help, ls, cat, echo, ps, kill, clear, echo, cd, pwd, mkdir, touch,
 *   rm, uptime, mem, reboot, halt, exec
 */
#include <kernel.h>
#include <process.h>
#include <scheduler.h>
#include <fs/vfs.h>
#include <drivers/vga.h>
#include <drivers/serial.h>
#include <drivers/keyboard.h>
#include <drivers/timer.h>
#include <memory.h>
#include <types.h>
#include <string.h>
#include <stdarg.h>

#define SHELL_MAX_LINE   256
#define SHELL_MAX_ARGS   32

/* Current working directory */
static char cwd[512] = "/";

/* ============================================================
 * Printf helpers (shell output)
 * ============================================================ */

static void shell_puts(const char* str)
{
    vga_puts(str);
    debug_puts(str);
}

static void shell_printf(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    extern int vsnprintf(char*, size_t, const char*, va_list);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    shell_puts(buf);
}

/* ============================================================
 * Command implementations
 * ============================================================ */

static void cmd_help(int argc, char** argv)
{
    (void)argc; (void)argv;
    shell_puts(
        "AetherOS Shell - built-in commands:\n"
        "  help              - Show this help\n"
        "  ls [path]         - List directory contents\n"
        "  cat <file>        - Display file contents\n"
        "  echo [text...]    - Print text\n"
        "  ps                - List running processes\n"
        "  kill <pid>        - Kill a process\n"
        "  clear             - Clear the screen\n"
        "  cd <path>         - Change directory\n"
        "  pwd               - Print working directory\n"
        "  mkdir <path>      - Create directory\n"
        "  touch <file>      - Create empty file\n"
        "  rm <file>         - Remove file\n"
        "  uptime            - Show system uptime\n"
        "  mem               - Show memory usage\n"
        "  halt              - Halt the system\n"
        "  reboot            - Reboot (not implemented)\n"
    );
}

static void cmd_ls(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : cwd;

    vfs_node_t* dir = vfs_resolve_path(path);
    if (!dir) {
        shell_printf("ls: cannot access '%s': No such file or directory\n", path);
        return;
    }
    if (!(dir->flags & VFS_DIRECTORY)) {
        shell_printf("ls: '%s': Not a directory\n", path);
        return;
    }

    uint32_t idx = 0;
    vfs_dirent_t ent;
    while (vfs_readdir(dir, idx++, &ent) == 0) {
        if (ent.type & VFS_DIRECTORY) {
            vga_set_color(vga_make_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK));
        } else {
            vga_set_color(vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        }
        shell_printf("  %s%s\n", ent.name,
                     (ent.type & VFS_DIRECTORY) ? "/" : "");
        vga_set_color(vga_make_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    }
}

static void cmd_cat(int argc, char** argv)
{
    if (argc < 2) {
        shell_puts("Usage: cat <file>\n");
        return;
    }

    /* Build full path */
    char path[512];
    if (argv[1][0] == '/') {
        strncpy(path, argv[1], sizeof(path) - 1);
    } else {
        snprintf(path, sizeof(path), "%s/%s", cwd, argv[1]);
    }

    vfs_node_t* node = vfs_open(path, O_RDONLY);
    if (!node) {
        shell_printf("cat: %s: No such file or directory\n", argv[1]);
        return;
    }
    if (node->flags & VFS_DIRECTORY) {
        shell_printf("cat: %s: Is a directory\n", argv[1]);
        vfs_close(node);
        return;
    }

    char buf[256];
    off_t offset = 0;
    ssize_t n;

    while ((n = vfs_read(node, offset, sizeof(buf) - 1, buf)) > 0) {
        buf[n] = '\0';
        shell_puts(buf);
        offset += n;
    }

    vfs_close(node);
}

static void cmd_echo(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        shell_puts(argv[i]);
        if (i < argc - 1) shell_puts(" ");
    }
    shell_puts("\n");
}

static void cmd_ps(int argc, char** argv)
{
    (void)argc; (void)argv;

    shell_puts("  PID  PPID  STATE      NAME\n");
    shell_puts("  ---  ----  ---------  ----\n");

    static const char* state_names[] = {
        "UNUSED", "CREATED", "RUNNING", "READY",
        "SLEEPING", "WAITING", "ZOMBIE", "DEAD"
    };

    process_t* proc = process_list;
    while (proc) {
        const char* state = (proc->state < 8) ? state_names[proc->state] : "?";
        shell_printf("  %3u  %4u  %-9s  %s\n",
                     proc->pid, proc->ppid, state, proc->name);
        proc = proc->next;
    }
}

static void cmd_kill(int argc, char** argv)
{
    if (argc < 2) {
        shell_puts("Usage: kill <pid>\n");
        return;
    }

    pid_t pid = (pid_t)atoi(argv[1]);
    if (pid <= 1) {
        shell_puts("kill: cannot kill init\n");
        return;
    }

    process_kill(pid, 9); /* SIGKILL */
    shell_printf("Killed process %u\n", pid);
}

static void cmd_clear(int argc, char** argv)
{
    (void)argc; (void)argv;
    vga_clear();
}

static void cmd_cd(int argc, char** argv)
{
    if (argc < 2) {
        strncpy(cwd, "/", sizeof(cwd));
        return;
    }

    char new_path[512];
    if (argv[1][0] == '/') {
        strncpy(new_path, argv[1], sizeof(new_path) - 1);
    } else {
        snprintf(new_path, sizeof(new_path), "%s/%s", cwd, argv[1]);
    }

    vfs_node_t* node = vfs_resolve_path(new_path);
    if (!node) {
        shell_printf("cd: %s: No such file or directory\n", argv[1]);
        return;
    }
    if (!(node->flags & VFS_DIRECTORY)) {
        shell_printf("cd: %s: Not a directory\n", argv[1]);
        return;
    }

    strncpy(cwd, new_path, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = '\0';
}

static void cmd_pwd(int argc, char** argv)
{
    (void)argc; (void)argv;
    shell_printf("%s\n", cwd);
}

static void cmd_mkdir(int argc, char** argv)
{
    if (argc < 2) {
        shell_puts("Usage: mkdir <path>\n");
        return;
    }

    char path[512];
    if (argv[1][0] == '/') {
        strncpy(path, argv[1], sizeof(path) - 1);
    } else {
        snprintf(path, sizeof(path), "%s/%s", cwd, argv[1]);
    }

    if (vfs_mkdir(path) != 0) {
        shell_printf("mkdir: cannot create directory '%s'\n", argv[1]);
    }
}

static void cmd_touch(int argc, char** argv)
{
    if (argc < 2) {
        shell_puts("Usage: touch <file>\n");
        return;
    }

    char path[512];
    if (argv[1][0] == '/') {
        strncpy(path, argv[1], sizeof(path) - 1);
    } else {
        snprintf(path, sizeof(path), "%s/%s", cwd, argv[1]);
    }

    if (vfs_create(path, 0644) != 0) {
        shell_printf("touch: cannot create '%s'\n", argv[1]);
    }
}

static void cmd_rm(int argc, char** argv)
{
    if (argc < 2) {
        shell_puts("Usage: rm <file>\n");
        return;
    }

    char path[512];
    if (argv[1][0] == '/') {
        strncpy(path, argv[1], sizeof(path) - 1);
    } else {
        snprintf(path, sizeof(path), "%s/%s", cwd, argv[1]);
    }

    if (vfs_unlink(path) != 0) {
        shell_printf("rm: cannot remove '%s'\n", argv[1]);
    }
}

static void cmd_uptime(int argc, char** argv)
{
    (void)argc; (void)argv;

    uint64_t ticks  = timer_ticks();
    uint64_t secs   = ticks / TIMER_FREQ;
    uint64_t mins   = secs / 60;
    uint64_t hours  = mins / 60;

    shell_printf("Uptime: %llu:%02llu:%02llu (%llu ticks)\n",
                 hours, mins % 60, secs % 60, ticks);
}

static void cmd_mem(int argc, char** argv)
{
    (void)argc; (void)argv;

    size_t free_frames = pmm_free_frames_count();
    size_t total_frames = pmm_total_frames();
    size_t used_frames = total_frames - free_frames;

    shell_printf("Physical memory:\n");
    shell_printf("  Total: %u MB (%u frames)\n",
                 (uint32_t)(total_frames * PAGE_SIZE / (1024*1024)),
                 (uint32_t)total_frames);
    shell_printf("  Used:  %u MB (%u frames)\n",
                 (uint32_t)(used_frames * PAGE_SIZE / (1024*1024)),
                 (uint32_t)used_frames);
    shell_printf("  Free:  %u MB (%u frames)\n",
                 (uint32_t)(free_frames * PAGE_SIZE / (1024*1024)),
                 (uint32_t)free_frames);
    shell_printf("Kernel heap:\n");
    shell_printf("  Used:  %u KB\n", (uint32_t)(kheap_used() / 1024));
}

static void cmd_halt(int argc, char** argv)
{
    (void)argc; (void)argv;
    shell_puts("System halting...\n");
    cpu_cli();
    cpu_halt();
}

static void cmd_reboot(int argc, char** argv)
{
    (void)argc; (void)argv;
    shell_puts("Rebooting...\n");

    /* Reset via keyboard controller */
    uint8_t good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE);
    cpu_halt();
}

/* ============================================================
 * Command dispatch table
 * ============================================================ */

typedef struct {
    const char* name;
    void (*handler)(int argc, char** argv);
} shell_cmd_t;

static shell_cmd_t commands[] = {
    { "help",   cmd_help   },
    { "ls",     cmd_ls     },
    { "cat",    cmd_cat    },
    { "echo",   cmd_echo   },
    { "ps",     cmd_ps     },
    { "kill",   cmd_kill   },
    { "clear",  cmd_clear  },
    { "cd",     cmd_cd     },
    { "pwd",    cmd_pwd    },
    { "mkdir",  cmd_mkdir  },
    { "touch",  cmd_touch  },
    { "rm",     cmd_rm     },
    { "uptime", cmd_uptime },
    { "mem",    cmd_mem    },
    { "halt",   cmd_halt   },
    { "reboot", cmd_reboot },
    { NULL,     NULL       }
};

/* ============================================================
 * Command parser
 * ============================================================ */

static int parse_line(char* line, char** argv, int max_args)
{
    int argc = 0;
    char* saveptr = NULL;
    char* token = strtok_r(line, " \t", &saveptr);

    while (token && argc < max_args - 1) {
        argv[argc++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }
    argv[argc] = NULL;
    return argc;
}

/* ============================================================
 * Main shell loop
 * ============================================================ */

void shell_run(void)
{
    char line[SHELL_MAX_LINE];
    char* argv[SHELL_MAX_ARGS];

    shell_puts("\n");
    vga_set_color(vga_make_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    shell_puts("AetherOS shell ready. Type 'help' for available commands.\n\n");
    vga_set_color(vga_make_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));

    while (1) {
        /* Print prompt */
        vga_set_color(vga_make_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
        shell_printf("root@aetheros:%s", cwd);
        vga_set_color(vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
        shell_puts("# ");
        vga_set_color(vga_make_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));

        /* Read input */
        int len = keyboard_readline(line, sizeof(line));
        if (len == 0) continue;

        /* Parse and dispatch */
        int argc = parse_line(line, argv, SHELL_MAX_ARGS);
        if (argc == 0) continue;

        bool found = false;
        for (int i = 0; commands[i].name; i++) {
            if (strcmp(argv[0], commands[i].name) == 0) {
                commands[i].handler(argc, argv);
                found = true;
                break;
            }
        }

        if (!found) {
            shell_printf("%s: command not found\n", argv[0]);
        }
    }
}
