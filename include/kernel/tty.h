/*
 * include/kernel/tty.h - Terminal / TTY layer
 *
 * Provides a POSIX-like terminal abstraction with:
 *   - Line discipline (cooked / raw modes)
 *   - Canonical input with line editing (backspace, Ctrl-C, Ctrl-U)
 *   - PTY (pseudo-terminal) pairs for terminal emulators
 *   - Per-TTY foreground process group (session / job control stub)
 *
 * Architecture:
 *
 *   Keyboard driver          PTY master (terminal app)
 *       │                          │
 *       ▼                          ▼
 *   tty_t  ──line discipline──  pty_t
 *       │                          │
 *       ▼                          ▼
 *   Application reads          Application writes
 *   via SYS_READ(fd)           via SYS_WRITE(fd)
 *
 * Each TTY has:
 *   - input_buf  : raw bytes from keyboard / PTY master
 *   - canon_buf  : line-completed line (cooked mode)
 *   - output_buf : bytes going to the display / PTY master
 */
#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include <types.h>

#define TTY_MAX          8      /* Virtual TTYs (VT1..VT8) */
#define PTY_MAX          32     /* PTY pairs */
#define TTY_BUF_SIZE     4096

/* Termios-lite flags */
#define TTY_ECHO         (1u << 0)  /* Echo input to output */
#define TTY_ICANON       (1u << 1)  /* Canonical (line-buffered) mode */
#define TTY_ISIG         (1u << 2)  /* Generate signals from special chars */
#define TTY_ONLCR        (1u << 3)  /* Translate \n → \r\n on output */
#define TTY_ICRNL        (1u << 4)  /* Translate \r → \n on input */

/* Special characters */
typedef struct {
    uint8_t c_intr;   /* SIGINT  (default Ctrl-C = 0x03) */
    uint8_t c_quit;   /* SIGQUIT (default Ctrl-\ = 0x1C) */
    uint8_t c_erase;  /* Erase   (default Backspace = 0x7F) */
    uint8_t c_kill;   /* Kill line (default Ctrl-U = 0x15) */
    uint8_t c_eof;    /* EOF     (default Ctrl-D = 0x04) */
    uint8_t c_susp;   /* SIGTSTP (default Ctrl-Z = 0x1A) */
} tty_cc_t;

/* Ring buffer */
typedef struct {
    uint8_t  buf[TTY_BUF_SIZE];
    uint32_t head, tail;
} tty_ring_t;

/* TTY descriptor */
typedef struct tty {
    int      index;         /* 0..TTY_MAX-1 */
    bool     valid;
    uint32_t flags;         /* TTY_ECHO | TTY_ICANON | … */
    tty_cc_t cc;            /* Special characters */

    tty_ring_t input;       /* Raw input from HW or PTY master */
    tty_ring_t canon;       /* Completed lines (cooked mode) */
    tty_ring_t output;      /* Output to display / PTY master */

    pid_t    fg_pgid;       /* Foreground process group */

    /* If this is the slave side of a PTY, points to the master */
    struct pty* pty;
} tty_t;

/* PTY pair */
typedef struct pty {
    bool      valid;
    int       index;
    tty_t*    slave;        /* Slave TTY seen by the application */
    tty_ring_t m2s;         /* Master → Slave (simulated keyboard) */
    tty_ring_t s2m;         /* Slave → Master (terminal output) */
} pty_t;

/* ── Subsystem init ──────────────────────────────────────────────────── */
void tty_init(void);

/* ── Console TTY (backed by keyboard + framebuffer text) ─────────────── */
tty_t* tty_get(int index);          /* Get VT by index */
void   tty_switch(int index);       /* Switch active VT */

/* ── I/O ─────────────────────────────────────────────────────────────── */
void   tty_input_char(tty_t* tty, uint8_t c);  /* Called by keyboard driver */
int    tty_read(tty_t* tty, char* buf, size_t n);
int    tty_write(tty_t* tty, const char* buf, size_t n);

/* ── PTY ─────────────────────────────────────────────────────────────── */
int   pty_open(int* master_fd, int* slave_fd);   /* Open PTY pair */
int   pty_master_write(pty_t* pty, const char* buf, size_t n);
int   pty_master_read(pty_t* pty, char* buf, size_t n);

/* ── Ring buffer helpers (inlined) ───────────────────────────────────── */
static inline bool tty_ring_empty(const tty_ring_t* r) {
    return r->head == r->tail;
}
static inline bool tty_ring_full(const tty_ring_t* r) {
    return ((r->tail + 1) % TTY_BUF_SIZE) == r->head;
}
static inline void tty_ring_push(tty_ring_t* r, uint8_t c) {
    if (!tty_ring_full(r)) {
        r->buf[r->tail] = c;
        r->tail = (r->tail + 1) % TTY_BUF_SIZE;
    }
}
static inline int tty_ring_pop(tty_ring_t* r) {
    if (tty_ring_empty(r)) return -1;
    uint8_t c = r->buf[r->head];
    r->head = (r->head + 1) % TTY_BUF_SIZE;
    return c;
}

#endif /* KERNEL_TTY_H */
