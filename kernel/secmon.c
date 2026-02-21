/*
 * kernel/secmon.c — Aether OS Security Monitor
 *
 * Passive audit and policy enforcement.
 * All capability and IPC operations call into secmon so every
 * security-relevant event is recorded in the audit ring buffer.
 *
 * The monitor does NOT perform any I/O itself at event time (to avoid
 * re-entrancy). It stores events in a ring buffer; the kernel may
 * flush the buffer to the serial console or a log service at lower
 * priority.
 */
#include <kernel/secmon.h>
#include <kernel/cap.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <drivers/timer.h>

/* =========================================================
 * State
 * ========================================================= */

static audit_event_t log_buf[SECMON_LOG_SIZE];
static uint32_t      log_head  = 0;    /* Next write position */
static uint32_t      log_count = 0;    /* Total events ever recorded */
static uint32_t      policy    = SECPOL_DENY_IRQCAP_USERLAND;

/* =========================================================
 * Initialisation
 * ========================================================= */

void secmon_init(void)
{
    memset(log_buf, 0, sizeof(log_buf));
    log_head  = 0;
    log_count = 0;
    policy    = SECPOL_DENY_IRQCAP_USERLAND;

    /* Record the boot event */
    audit_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq       = log_count;
    ev.timestamp = (uint64_t)timer_get_ticks();
    ev.type      = AUDIT_BOOT;
    log_buf[log_head % SECMON_LOG_SIZE] = ev;
    log_head++;
    log_count++;

    kinfo("SECMON: audit ring buffer ready (%u entries × %u bytes = %u KB)",
          SECMON_LOG_SIZE,
          (uint32_t)sizeof(audit_event_t),
          (uint32_t)(SECMON_LOG_SIZE * sizeof(audit_event_t) / 1024));
}

/* =========================================================
 * Internal: append one event
 * ========================================================= */

static void _record(audit_event_type_t type, uint32_t actor_tid,
                    cap_id_t cap, port_id_t port,
                    uint32_t d0, uint32_t d1)
{
    audit_event_t* ev = &log_buf[log_head % SECMON_LOG_SIZE];
    ev->seq        = log_count;
    ev->timestamp  = (uint64_t)timer_get_ticks();
    ev->type       = type;
    ev->actor_tid  = actor_tid;
    ev->cap        = cap;
    ev->port       = port;
    ev->detail[0]  = d0;
    ev->detail[1]  = d1;

    log_head++;
    log_count++;
}

/* =========================================================
 * Public audit hooks
 * ========================================================= */

void secmon_audit_cap(audit_event_type_t type, uint32_t actor_tid,
                      cap_id_t cap, uint32_t d0, uint32_t d1)
{
    _record(type, actor_tid, cap, 0, d0, d1);
}

void secmon_audit_ipc(audit_event_type_t type, uint32_t actor_tid,
                      port_id_t port, uint32_t msg_type)
{
    /* Only log all IPC if the verbose policy flag is set */
    if (type == AUDIT_IPC_SEND && !(policy & SECPOL_LOG_ALL_IPC)) return;
    _record(type, actor_tid, 0, port, msg_type, 0);
}

void secmon_audit_svc(audit_event_type_t type, uint32_t actor_tid,
                      const char* name)
{
    /* Store first 8 bytes of name in detail fields */
    uint32_t d0 = 0, d1 = 0;
    if (name) {
        size_t len = strlen(name);
        if (len > 4) len = 4;
        for (size_t i = 0; i < len; i++)
            d0 |= ((uint32_t)(uint8_t)name[i]) << (i * 8);
        if (strlen(name) > 4) {
            size_t rem = strlen(name) - 4;
            if (rem > 4) rem = 4;
            for (size_t i = 0; i < rem; i++)
                d1 |= ((uint32_t)(uint8_t)name[4+i]) << (i * 8);
        }
    }
    _record(type, actor_tid, 0, 0, d0, d1);
}

/* =========================================================
 * Policy enforcement
 * ========================================================= */

bool secmon_check_cap_create(uint32_t actor_tid, cap_type_t type)
{
    if (type == CAP_TYPE_IRQLINE &&
        (policy & SECPOL_DENY_IRQCAP_USERLAND) &&
        actor_tid != 0) {
        /* tid 0 = kernel; everyone else denied IRQ caps */
        _record(AUDIT_POLICY_DENY, actor_tid, 0, 0,
                (uint32_t)type, 0);
        klog_warn("SECMON: denied IRQ capability request from task %u",
                  actor_tid);
        return false;
    }
    return true;
}

bool secmon_check_irq_access(uint32_t actor_tid)
{
    return secmon_check_cap_create(actor_tid, CAP_TYPE_IRQLINE);
}

/* =========================================================
 * Policy configuration
 * ========================================================= */

void     secmon_set_policy(uint32_t flags) { policy = flags; }
uint32_t secmon_get_policy(void)           { return policy;  }

/* =========================================================
 * Diagnostics
 * ========================================================= */

uint32_t secmon_event_count(void) { return log_count; }

void secmon_dump_log(int last_n)
{
    if (last_n <= 0 || last_n > (int)SECMON_LOG_SIZE)
        last_n = SECMON_LOG_SIZE;

    uint32_t total = log_count;
    uint32_t start = (total > (uint32_t)last_n)
                     ? (total - (uint32_t)last_n) : 0;

    kinfo("SECMON: audit log — last %d events (total=%u):", last_n, total);

    for (uint32_t i = start; i < total; i++) {
        audit_event_t* ev = &log_buf[i % SECMON_LOG_SIZE];
        const char* type_str = "?";
        switch (ev->type) {
        case AUDIT_CAP_CREATE:    type_str = "CAP_CREATE";    break;
        case AUDIT_CAP_DERIVE:    type_str = "CAP_DERIVE";    break;
        case AUDIT_CAP_REVOKE:    type_str = "CAP_REVOKE";    break;
        case AUDIT_CAP_TRANSFER:  type_str = "CAP_TRANSFER";  break;
        case AUDIT_IPC_SEND:      type_str = "IPC_SEND";      break;
        case AUDIT_IPC_RECV:      type_str = "IPC_RECV";      break;
        case AUDIT_SVC_REGISTER:  type_str = "SVC_REGISTER";  break;
        case AUDIT_SVC_LOOKUP:    type_str = "SVC_LOOKUP";    break;
        case AUDIT_SVC_UNREGISTER:type_str = "SVC_UNREG";     break;
        case AUDIT_POLICY_DENY:   type_str = "POLICY_DENY";   break;
        case AUDIT_BOOT:          type_str = "BOOT";           break;
        case AUDIT_PANIC:         type_str = "PANIC";          break;
        }
        kinfo("  [%5u] t=%-8llu %-14s actor=%-4u cap=%-4u port=%-4u",
              ev->seq, (unsigned long long)ev->timestamp,
              type_str, ev->actor_tid, ev->cap, ev->port);
    }
}
