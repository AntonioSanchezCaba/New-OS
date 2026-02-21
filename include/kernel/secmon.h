/*
 * include/kernel/secmon.h — Aether OS Security Monitor
 *
 * The security monitor is a passive audit subsystem.
 * It observes capability operations and IPC activity,
 * logs security-relevant events, and can enforce policies.
 *
 * The audit log is a fixed-size ring buffer in kernel memory.
 * All entries are append-only from the monitor's perspective.
 */
#ifndef KERNEL_SECMON_H
#define KERNEL_SECMON_H

#include <types.h>
#include <kernel/cap.h>
#include <kernel/ipc.h>

/* =========================================================
 * Audit event types
 * ========================================================= */

typedef enum {
    AUDIT_CAP_CREATE    = 0x01,  /* New capability created */
    AUDIT_CAP_DERIVE    = 0x02,  /* Capability derived (sub-rights) */
    AUDIT_CAP_REVOKE    = 0x03,  /* Capability revoked */
    AUDIT_CAP_TRANSFER  = 0x04,  /* Capability transferred to new owner */
    AUDIT_IPC_SEND      = 0x10,  /* IPC message sent */
    AUDIT_IPC_RECV      = 0x11,  /* IPC message received */
    AUDIT_SVC_REGISTER  = 0x20,  /* Service registered */
    AUDIT_SVC_LOOKUP    = 0x21,  /* Service looked up */
    AUDIT_SVC_UNREGISTER= 0x22,  /* Service unregistered */
    AUDIT_POLICY_DENY   = 0x30,  /* Policy check denied an action */
    AUDIT_BOOT          = 0x40,  /* System boot event */
    AUDIT_PANIC         = 0x41,  /* Kernel panic */
} audit_event_type_t;

/* One audit record */
typedef struct {
    uint32_t            seq;        /* Monotonic sequence number */
    uint64_t            timestamp;  /* Timer ticks at time of event */
    audit_event_type_t  type;
    uint32_t            actor_tid;  /* Task that triggered the event */
    cap_id_t            cap;        /* Related capability (or 0) */
    port_id_t           port;       /* Related port (or 0) */
    uint32_t            detail[2];  /* Type-specific payload */
} audit_event_t;

#define SECMON_LOG_SIZE  512    /* Audit ring buffer entries */

/* =========================================================
 * Policy flags
 * ========================================================= */

#define SECPOL_DENY_IRQCAP_USERLAND  0x01  /* Block IRQ caps to non-kernel tasks */
#define SECPOL_LOG_ALL_IPC           0x02  /* Log every IPC send (verbose) */
#define SECPOL_RESTRICT_CAP_DERIVE   0x04  /* Only kernel can call cap_create */

/* =========================================================
 * API
 * ========================================================= */

void  secmon_init(void);

/* Called by the kernel subsystems — do not call directly */
void  secmon_audit_cap(audit_event_type_t type, uint32_t actor_tid,
                       cap_id_t cap, uint32_t detail0, uint32_t detail1);
void  secmon_audit_ipc(audit_event_type_t type, uint32_t actor_tid,
                       port_id_t port, uint32_t msg_type);
void  secmon_audit_svc(audit_event_type_t type, uint32_t actor_tid,
                       const char* name);

/* Policy enforcement — returns true if action is permitted */
bool  secmon_check_cap_create(uint32_t actor_tid, cap_type_t type);
bool  secmon_check_irq_access(uint32_t actor_tid);

/* Policy configuration */
void  secmon_set_policy(uint32_t flags);
uint32_t secmon_get_policy(void);

/* Diagnostics — dump recent audit entries via kinfo */
void  secmon_dump_log(int last_n);

/* Count of audit events since boot */
uint32_t secmon_event_count(void);

#endif /* KERNEL_SECMON_H */
