/*
 * include/kernel/ipc.h — Aether OS IPC Engine
 *
 * Message-passing is the ONLY communication primitive between tasks
 * and services in Aether OS.  There is no shared global state.
 *
 * Design overview:
 *   - Tasks own "ports" — bidirectional message-queue endpoints
 *   - Ports are protected by CAP_TYPE_PORT capabilities
 *   - Messages carry typed headers + inline payload (≤ 256 bytes)
 *   - Capability tokens can be transferred alongside messages
 *   - For large data: send a CAP_TYPE_MEMORY capability (zero-copy)
 *   - Sync call: ipc_call() blocks until a reply arrives
 *   - Async send: ipc_send() returns immediately
 */
#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include <types.h>
#include <kernel/cap.h>

/* =========================================================
 * Configuration constants
 * ========================================================= */

#define IPC_MSG_DATA_MAX    256     /* Maximum inline payload bytes */
#define IPC_MSG_CAPS_MAX    4       /* Maximum caps per message */
#define IPC_PORT_QUEUE_SIZE 8       /* Depth of per-port receive queue */
#define IPC_MAX_PORTS       64      /* System-wide port limit */
#define IPC_TIMEOUT_FOREVER UINT32_MAX
#define IPC_TIMEOUT_NONE    0       /* Non-blocking receive */

typedef uint32_t port_id_t;
#define PORT_INVALID  ((port_id_t)0)

/* =========================================================
 * Message-type namespace
 *   High 16 bits = protocol family
 *   Low  16 bits = operation code within family
 * ========================================================= */

#define MSG_TYPE_SYSTEM     0x00000000  /* Kernel/system messages */
#define MSG_TYPE_SERVICE    0x00010000  /* Generic service protocol */
#define MSG_TYPE_DISPLAY    0x00030000  /* Compositor protocol */
#define MSG_TYPE_INPUT      0x00040000  /* Input event protocol */
#define MSG_TYPE_FS         0x00050000  /* Filesystem operations */
#define MSG_TYPE_NET        0x00060000  /* Network stack */

/* System messages */
#define MSG_SYS_PING        (MSG_TYPE_SYSTEM | 0x0001)
#define MSG_SYS_PONG        (MSG_TYPE_SYSTEM | 0x0002)
#define MSG_SYS_SHUTDOWN    (MSG_TYPE_SYSTEM | 0x0003)
#define MSG_SYS_RESTART_SVC (MSG_TYPE_SYSTEM | 0x0004)

/* Display compositor messages */
#define MSG_DISP_CREATE     (MSG_TYPE_DISPLAY | 0x0001)
#define MSG_DISP_DESTROY    (MSG_TYPE_DISPLAY | 0x0002)
#define MSG_DISP_SET_GEOM   (MSG_TYPE_DISPLAY | 0x0003)
#define MSG_DISP_SET_VIS    (MSG_TYPE_DISPLAY | 0x0004)
#define MSG_DISP_COMMIT     (MSG_TYPE_DISPLAY | 0x0005)
#define MSG_DISP_SET_TITLE  (MSG_TYPE_DISPLAY | 0x0006)
#define MSG_DISP_RAISE      (MSG_TYPE_DISPLAY | 0x0007)
#define MSG_DISP_INFO       (MSG_TYPE_DISPLAY | 0x0008)
#define MSG_DISP_DAMAGE     (MSG_TYPE_DISPLAY | 0x0009) /* Notify dirty rect */

/* Input service messages */
#define MSG_INPUT_KEY_DOWN  (MSG_TYPE_INPUT | 0x0001)
#define MSG_INPUT_KEY_UP    (MSG_TYPE_INPUT | 0x0002)
#define MSG_INPUT_MOUSE_MOV (MSG_TYPE_INPUT | 0x0003)
#define MSG_INPUT_MOUSE_BTN (MSG_TYPE_INPUT | 0x0004)
#define MSG_INPUT_FOCUS_IN  (MSG_TYPE_INPUT | 0x0005)
#define MSG_INPUT_FOCUS_OUT (MSG_TYPE_INPUT | 0x0006)
#define MSG_INPUT_SUBSCRIBE (MSG_TYPE_INPUT | 0x0007) /* Register for events */

/* =========================================================
 * Message flags
 * ========================================================= */

#define MSG_FLAG_ASYNC   0x01    /* Don't wait for reply */
#define MSG_FLAG_URGENT  0x02    /* Jump to front of receive queue */
#define MSG_FLAG_NOCOPY  0x04    /* Payload is a memory capability */
#define MSG_FLAG_REPLY   0x08    /* This message is a reply */

/* =========================================================
 * The IPC message structure
 * ========================================================= */

typedef struct ipc_msg {
    /* Fixed header — always present */
    uint32_t    msg_id;                     /* Kernel-assigned sequence number */
    uint32_t    type;                       /* MSG_TYPE_* protocol identifier */
    uint32_t    flags;                      /* MSG_FLAG_* bitmask */
    port_id_t   reply_port;                 /* Where to send the reply (0=none) */
    uint32_t    sender_tid;                 /* Sending task ID (kernel-filled) */

    /* Capability transfers (0..IPC_MSG_CAPS_MAX-1 valid caps) */
    uint32_t    cap_count;
    cap_id_t    caps[IPC_MSG_CAPS_MAX];

    /* Inline payload */
    uint32_t    data_len;
    uint8_t     data[IPC_MSG_DATA_MAX];
} ipc_msg_t;

/* =========================================================
 * IPC port state
 * ========================================================= */

typedef struct ipc_port {
    port_id_t   id;
    cap_id_t    cap;                        /* Capability token for this port */
    uint32_t    owner_tid;

    /* Circular message queue */
    ipc_msg_t   queue[IPC_PORT_QUEUE_SIZE];
    uint32_t    q_head;
    uint32_t    q_tail;
    uint32_t    q_count;

    bool        valid;
    bool        closed;
} ipc_port_t;

/* =========================================================
 * Error codes
 * ========================================================= */

typedef enum {
    IPC_OK          =  0,
    IPC_ERR_INVALID = -1,   /* Invalid port or null pointer */
    IPC_ERR_NOPERM  = -2,   /* Missing capability / rights */
    IPC_ERR_FULL    = -3,   /* Queue is at capacity */
    IPC_ERR_CLOSED  = -4,   /* Port has been destroyed */
    IPC_ERR_TIMEOUT = -5,   /* Blocking receive timed out */
    IPC_ERR_AGAIN   = -6,   /* No message ready (non-blocking) */
} ipc_err_t;

/* =========================================================
 * API
 * ========================================================= */

void       ipc_init(void);

/* Port lifecycle */
port_id_t  ipc_port_create(uint32_t owner_tid);
void       ipc_port_destroy(port_id_t port);
cap_id_t   ipc_port_cap(port_id_t port);

/* Non-blocking enqueue */
ipc_err_t  ipc_send(port_id_t port, const ipc_msg_t* msg);

/* Blocking: sends then waits on a temporary reply port */
ipc_err_t  ipc_call(port_id_t port, ipc_msg_t* msg, uint32_t timeout_ms);

/* Dequeue — blocks until message arrives or timeout */
ipc_err_t  ipc_receive(port_id_t port, ipc_msg_t* out, uint32_t timeout_ms);

/* Send a reply back to the originator of a request */
ipc_err_t  ipc_reply(const ipc_msg_t* request, const ipc_msg_t* reply);

/* How many messages are waiting in the queue */
uint32_t   ipc_pending(port_id_t port);

/* Diagnostics */
void       ipc_dump_ports(void);

#endif /* KERNEL_IPC_H */
