/*
 * kernel/ipc.c — Aether OS IPC Engine
 *
 * Implements message-port IPC.  All cross-task communication in
 * Aether OS flows through these typed message ports.
 *
 * Implementation notes:
 *   - Port table is a flat array; port_id_t is the unique token.
 *   - Message queues are fixed-size circular buffers inside each port.
 *   - ipc_call() creates a transient "reply port", sends the message,
 *     then blocks waiting for the server to call ipc_reply().
 *   - Capability tokens inside messages are validated on send.
 */
#include <kernel/ipc.h>
#include <kernel/cap.h>
#include <kernel/secmon.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <scheduler.h>

/* =========================================================
 * Global state
 * ========================================================= */

static ipc_port_t  port_table[IPC_MAX_PORTS];
static uint32_t    port_next_id     = 1;
static uint32_t    port_active      = 0;
static uint32_t    msg_seq          = 1;  /* Monotonic message counter */

/* =========================================================
 * Internal helpers
 * ========================================================= */

static ipc_port_t* _find_port(port_id_t id)
{
    if (id == PORT_INVALID) return NULL;
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (port_table[i].valid && port_table[i].id == id)
            return &port_table[i];
    }
    return NULL;
}

static ipc_port_t* _alloc_port(void)
{
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (!port_table[i].valid)
            return &port_table[i];
    }
    return NULL;
}

/* =========================================================
 * Initialisation
 * ========================================================= */

void ipc_init(void)
{
    memset(port_table, 0, sizeof(port_table));
    port_next_id = 1;
    port_active  = 0;
    msg_seq      = 1;

    kinfo("IPC: engine ready — %u ports × queue=%u msgs/port "
          "(%u KB total)",
          IPC_MAX_PORTS, IPC_PORT_QUEUE_SIZE,
          (uint32_t)(IPC_MAX_PORTS * sizeof(ipc_port_t) / 1024));
}

/* =========================================================
 * Port lifecycle
 * ========================================================= */

port_id_t ipc_port_create(uint32_t owner_tid)
{
    ipc_port_t* p = _alloc_port();
    if (!p) {
        klog_warn("IPC: port table full");
        return PORT_INVALID;
    }

    port_id_t id = port_next_id++;
    if (port_next_id == 0) port_next_id = 1;

    p->id        = id;
    p->owner_tid = owner_tid;
    p->q_head    = 0;
    p->q_tail    = 0;
    p->q_count   = 0;
    p->valid     = true;
    p->closed    = false;

    /* Create a capability for this port */
    p->cap = cap_create(CAP_TYPE_PORT,
                        CAP_RIGHT_READ | CAP_RIGHT_WRITE | CAP_RIGHT_GRANT,
                        p, owner_tid);

    port_active++;
    return id;
}

void ipc_port_destroy(port_id_t port_id)
{
    ipc_port_t* p = _find_port(port_id);
    if (!p) return;

    p->closed = true;
    p->valid  = false;

    if (p->cap != CAP_INVALID_ID) {
        cap_release(p->cap);
        p->cap = CAP_INVALID_ID;
    }

    port_active--;
}

cap_id_t ipc_port_cap(port_id_t port_id)
{
    ipc_port_t* p = _find_port(port_id);
    return p ? p->cap : CAP_INVALID_ID;
}

/* =========================================================
 * ipc_send — enqueue a message (non-blocking)
 * ========================================================= */

ipc_err_t ipc_send(port_id_t port_id, const ipc_msg_t* msg)
{
    if (!msg) return IPC_ERR_INVALID;

    ipc_port_t* p = _find_port(port_id);
    if (!p)          return IPC_ERR_INVALID;
    if (p->closed)   return IPC_ERR_CLOSED;
    if (p->q_count >= IPC_PORT_QUEUE_SIZE) return IPC_ERR_FULL;

    /* Validate any capability transfers */
    uint32_t ncaps = msg->cap_count < IPC_MSG_CAPS_MAX
                     ? msg->cap_count : IPC_MSG_CAPS_MAX;
    for (uint32_t i = 0; i < ncaps; i++) {
        if (!cap_check(msg->caps[i], CAP_RIGHT_GRANT)) {
            klog_warn("IPC: send with non-grantable cap %u", msg->caps[i]);
            return IPC_ERR_NOPERM;
        }
    }

    /* Enqueue */
    ipc_msg_t* slot  = &p->queue[p->q_tail];
    *slot            = *msg;
    slot->msg_id     = msg_seq++;

    p->q_tail = (p->q_tail + 1) % IPC_PORT_QUEUE_SIZE;
    p->q_count++;

    secmon_audit_ipc(AUDIT_IPC_SEND, msg->sender_tid, port_id, msg->type);
    return IPC_OK;
}

/* =========================================================
 * ipc_receive — dequeue a message (blocking with timeout)
 * ========================================================= */

ipc_err_t ipc_receive(port_id_t port_id, ipc_msg_t* out, uint32_t timeout_ms)
{
    ipc_port_t* p = _find_port(port_id);
    if (!p || !out) return IPC_ERR_INVALID;

    uint32_t waited = 0;

    for (;;) {
        if (p->q_count > 0) {
            *out    = p->queue[p->q_head];
            p->q_head = (p->q_head + 1) % IPC_PORT_QUEUE_SIZE;
            p->q_count--;
            secmon_audit_ipc(AUDIT_IPC_RECV, out->sender_tid,
                             port_id, out->type);
            return IPC_OK;
        }

        if (p->closed)                               return IPC_ERR_CLOSED;
        if (timeout_ms == IPC_TIMEOUT_NONE)          return IPC_ERR_AGAIN;
        if (timeout_ms != IPC_TIMEOUT_FOREVER &&
            waited >= timeout_ms)                    return IPC_ERR_TIMEOUT;

        scheduler_yield();
        waited += 2;  /* Approximate: each yield ≈ 2ms at 500Hz */
    }
}

/* =========================================================
 * ipc_call — synchronous request/reply (send + wait)
 * ========================================================= */

ipc_err_t ipc_call(port_id_t port_id, ipc_msg_t* msg, uint32_t timeout_ms)
{
    /* Allocate a temporary port for the reply */
    port_id_t reply = ipc_port_create(msg->sender_tid);
    if (reply == PORT_INVALID) return IPC_ERR_INVALID;

    msg->reply_port = reply;

    ipc_err_t err = ipc_send(port_id, msg);
    if (err != IPC_OK) {
        ipc_port_destroy(reply);
        return err;
    }

    /* Block until reply arrives */
    err = ipc_receive(reply, msg, timeout_ms);
    ipc_port_destroy(reply);
    return err;
}

/* =========================================================
 * ipc_reply — send reply back to a message's originator
 * ========================================================= */

ipc_err_t ipc_reply(const ipc_msg_t* request, const ipc_msg_t* reply_data)
{
    if (!request || request->reply_port == PORT_INVALID)
        return IPC_ERR_INVALID;

    ipc_msg_t reply = *reply_data;
    reply.flags  |= MSG_FLAG_REPLY;
    reply.msg_id  = msg_seq++;

    return ipc_send(request->reply_port, &reply);
}

/* =========================================================
 * ipc_pending — how many messages are waiting?
 * ========================================================= */

uint32_t ipc_pending(port_id_t port_id)
{
    ipc_port_t* p = _find_port(port_id);
    return p ? p->q_count : 0;
}

/* =========================================================
 * Diagnostics
 * ========================================================= */

void ipc_dump_ports(void)
{
    kinfo("IPC: %u active ports:", port_active);
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        ipc_port_t* p = &port_table[i];
        if (!p->valid) continue;
        kinfo("  port[%u] owner=%u q=%u/%u cap=%u%s",
              p->id, p->owner_tid, p->q_count,
              IPC_PORT_QUEUE_SIZE, p->cap,
              p->closed ? " CLOSED" : "");
    }
}
