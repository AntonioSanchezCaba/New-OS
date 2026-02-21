/*
 * kernel/tty.c - TTY / PTY subsystem
 *
 * Provides a POSIX-like terminal layer with line discipline, signal
 * generation, and PTY (pseudo-terminal) pairs for terminal emulators.
 */
#include <kernel/tty.h>
#include <kernel/signal.h>
#include <process.h>
#include <drivers/keyboard.h>
#include <drivers/timer.h>
#include <kernel.h>
#include <memory.h>
#include <string.h>

/* ── State ───────────────────────────────────────────────────────────── */

static tty_t  ttys[TTY_MAX];
static pty_t  ptys[PTY_MAX];
static int    active_tty = 0;

/* ── Initialization ──────────────────────────────────────────────────── */

void tty_init(void)
{
    memset(ttys, 0, sizeof(ttys));
    memset(ptys, 0, sizeof(ptys));

    for (int i = 0; i < TTY_MAX; i++) {
        ttys[i].index   = i;
        ttys[i].valid   = true;
        ttys[i].flags   = TTY_ECHO | TTY_ICANON | TTY_ISIG |
                          TTY_ONLCR | TTY_ICRNL;
        ttys[i].cc.c_intr  = 0x03; /* Ctrl-C */
        ttys[i].cc.c_quit  = 0x1C; /* Ctrl-\ */
        ttys[i].cc.c_erase = 0x7F; /* Backspace */
        ttys[i].cc.c_kill  = 0x15; /* Ctrl-U */
        ttys[i].cc.c_eof   = 0x04; /* Ctrl-D */
        ttys[i].cc.c_susp  = 0x1A; /* Ctrl-Z */
        ttys[i].fg_pgid = 1;       /* Init process group */
    }

    kinfo("TTY: %d virtual terminals initialized", TTY_MAX);
}

/* ── Get / switch ────────────────────────────────────────────────────── */

tty_t* tty_get(int index)
{
    if (index < 0 || index >= TTY_MAX) return NULL;
    return &ttys[index];
}

void tty_switch(int index)
{
    if (index < 0 || index >= TTY_MAX) return;
    active_tty = index;
    kdebug("TTY: switched to VT%d", index + 1);
}

/* ── Line discipline ─────────────────────────────────────────────────── */

/*
 * tty_input_char - feed one raw character into the TTY.
 * Called by the keyboard ISR with the decoded ASCII character.
 */
void tty_input_char(tty_t* tty, uint8_t c)
{
    if (!tty || !tty->valid) return;

    /* Translate CR → LF */
    if ((tty->flags & TTY_ICRNL) && c == '\r') c = '\n';

    /* Signal generation */
    if (tty->flags & TTY_ISIG) {
        if (c == tty->cc.c_intr) {
            signal_send(tty->fg_pgid, SIGINT);
            return;
        }
        if (c == tty->cc.c_quit) {
            signal_send(tty->fg_pgid, SIGQUIT);
            return;
        }
        if (c == tty->cc.c_susp) {
            signal_send(tty->fg_pgid, SIGTSTP);
            return;
        }
    }

    /* Raw mode: push straight to input ring */
    if (!(tty->flags & TTY_ICANON)) {
        tty_ring_push(&tty->input, c);
        if (tty->flags & TTY_ECHO) {
            tty_ring_push(&tty->output, c);
        }
        return;
    }

    /* Canonical mode: line editing */
    if (c == tty->cc.c_erase) {
        /* Erase last character in canon buf */
        if (tty->canon.tail != tty->canon.head) {
            tty->canon.tail = (tty->canon.tail == 0)
                              ? TTY_BUF_SIZE - 1
                              : tty->canon.tail - 1;
            if (tty->flags & TTY_ECHO) {
                tty_ring_push(&tty->output, '\b');
                tty_ring_push(&tty->output, ' ');
                tty_ring_push(&tty->output, '\b');
            }
        }
        return;
    }

    if (c == tty->cc.c_kill) {
        /* Kill entire line */
        tty->canon.head = tty->canon.tail = 0;
        if (tty->flags & TTY_ECHO) {
            tty_ring_push(&tty->output, '\r');
            tty_ring_push(&tty->output, '\n');
        }
        return;
    }

    if (c == tty->cc.c_eof) {
        /* EOF: mark end of input without consuming a line */
        tty_ring_push(&tty->input, 0xFF); /* Sentinel */
        return;
    }

    /* Append to canonical buffer */
    tty_ring_push(&tty->canon, c);

    /* Echo */
    if (tty->flags & TTY_ECHO) {
        if ((tty->flags & TTY_ONLCR) && c == '\n') {
            tty_ring_push(&tty->output, '\r');
        }
        tty_ring_push(&tty->output, c);
    }

    /* Line complete: move canon buffer to input ring */
    if (c == '\n') {
        while (tty->canon.head != tty->canon.tail) {
            int ch = tty_ring_pop(&tty->canon);
            if (ch >= 0) tty_ring_push(&tty->input, (uint8_t)ch);
        }
    }
}

/* ── Read ────────────────────────────────────────────────────────────── */

int tty_read(tty_t* tty, char* buf, size_t n)
{
    if (!tty || !buf || n == 0) return -1;

    size_t read = 0;

    /* Blocking wait for data */
    while (tty_ring_empty(&tty->input)) {
        scheduler_yield();
    }

    while (read < n && !tty_ring_empty(&tty->input)) {
        int c = tty_ring_pop(&tty->input);
        if (c < 0) break;
        if ((uint8_t)c == 0xFF) break; /* EOF sentinel */
        buf[read++] = (char)c;
        if ((uint8_t)c == '\n' && (tty->flags & TTY_ICANON)) break;
    }

    return (int)read;
}

/* ── Write ───────────────────────────────────────────────────────────── */

int tty_write(tty_t* tty, const char* buf, size_t n)
{
    if (!tty || !buf) return -1;

    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)buf[i];
        if ((tty->flags & TTY_ONLCR) && c == '\n') {
            tty_ring_push(&tty->output, '\r');
        }
        tty_ring_push(&tty->output, c);
    }

    return (int)n;
}

/* ── PTY ─────────────────────────────────────────────────────────────── */

int pty_open(int* master_fd, int* slave_fd)
{
    (void)master_fd; (void)slave_fd;

    /* Find a free PTY slot */
    for (int i = 0; i < PTY_MAX; i++) {
        if (!ptys[i].valid) {
            ptys[i].valid = true;
            ptys[i].index = i;
            ptys[i].slave = &ttys[0]; /* Bind to VT0 for now */
            memset(&ptys[i].m2s, 0, sizeof(tty_ring_t));
            memset(&ptys[i].s2m, 0, sizeof(tty_ring_t));
            /* In Phase 2 we assign real FDs from the VFS FD table */
            if (master_fd) *master_fd = 100 + i * 2;
            if (slave_fd)  *slave_fd  = 100 + i * 2 + 1;
            kdebug("PTY: opened pair %d (master=%d slave=%d)",
                   i, 100 + i*2, 100 + i*2 + 1);
            return i;
        }
    }
    return -ENOMEM;
}

int pty_master_write(pty_t* pty, const char* buf, size_t n)
{
    if (!pty || !buf) return -1;
    for (size_t i = 0; i < n; i++) {
        tty_ring_push(&pty->m2s, (uint8_t)buf[i]);
        if (pty->slave) tty_input_char(pty->slave, (uint8_t)buf[i]);
    }
    return (int)n;
}

int pty_master_read(pty_t* pty, char* buf, size_t n)
{
    if (!pty || !buf) return -1;
    size_t read = 0;
    while (read < n && !tty_ring_empty(&pty->s2m)) {
        int c = tty_ring_pop(&pty->s2m);
        if (c < 0) break;
        buf[read++] = (char)c;
    }
    return (int)read;
}
