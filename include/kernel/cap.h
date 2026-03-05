/*
 * include/kernel/cap.h — Aether OS Capability Security System
 *
 * Every resource in Aether OS is guarded by a capability token.
 * A capability is an unforgeable kernel-managed token that grants
 * specific rights on a specific object.
 *
 * There is no ambient authority. A task that lacks a capability for
 * a resource simply cannot access it — there is no "root" override.
 *
 * Capabilities can be:
 *   Created  — only the kernel or a delegated privileged service
 *   Derived  — holder creates a subset-rights copy
 *   Granted  — transferred to another task via IPC
 *   Revoked  — invalidated, cascades to all derived capabilities
 */
#ifndef KERNEL_CAP_H
#define KERNEL_CAP_H

#include <types.h>

/* =========================================================
 * Capability object types
 * ========================================================= */

typedef enum {
    CAP_TYPE_NULL    = 0,   /* Null / placeholder */
    CAP_TYPE_MEMORY  = 1,   /* Physical/virtual memory region */
    CAP_TYPE_PORT    = 2,   /* IPC message-port endpoint */
    CAP_TYPE_SERVICE = 3,   /* Service reference (service bus entry) */
    CAP_TYPE_DEVICE  = 4,   /* Hardware device (PCI BAR, etc.) */
    CAP_TYPE_FILE    = 5,   /* File or directory object */
    CAP_TYPE_TASK    = 6,   /* Task / process reference */
    CAP_TYPE_DISPLAY = 7,   /* Display surface pixel buffer */
    CAP_TYPE_IRQLINE = 8,   /* Hardware interrupt line */
} cap_type_t;

/* =========================================================
 * Rights bitmask
 * ========================================================= */

typedef uint32_t cap_rights_t;

#define CAP_RIGHT_NONE      ((cap_rights_t)0x00000000)
#define CAP_RIGHT_READ      ((cap_rights_t)0x00000001)  /* Receive / read */
#define CAP_RIGHT_WRITE     ((cap_rights_t)0x00000002)  /* Send / write */
#define CAP_RIGHT_EXECUTE   ((cap_rights_t)0x00000004)  /* Invoke / execute */
#define CAP_RIGHT_MAP       ((cap_rights_t)0x00000008)  /* Map into addr space */
#define CAP_RIGHT_GRANT     ((cap_rights_t)0x00000010)  /* Transfer to others */
#define CAP_RIGHT_REVOKE    ((cap_rights_t)0x00000020)  /* Revoke this cap */
#define CAP_RIGHT_MANAGE    ((cap_rights_t)0x00000040)  /* Derive sub-caps */
#define CAP_RIGHT_ALL       ((cap_rights_t)0x0000007F)

/* =========================================================
 * Capability identifier and table
 * ========================================================= */

typedef uint32_t cap_id_t;
#define CAP_INVALID_ID   ((cap_id_t)0)
#define CAP_TABLE_SIZE   256     /* System-wide capability slots */

/* One entry in the global capability table */
typedef struct cap {
    cap_id_t     id;          /* This capability's unique token */
    cap_type_t   type;        /* What kind of resource */
    cap_rights_t rights;      /* Permitted operations */
    void*        object;      /* Pointer to the managed resource */
    cap_id_t     parent;      /* Parent cap (0 = root; revoke cascades down) */
    uint32_t     ref_count;   /* How many things hold this cap alive */
    uint32_t     owner_tid;   /* Task ID that currently holds this cap */
    bool         valid;       /* False = revoked or not yet allocated */
} cap_t;

/* =========================================================
 * API
 * ========================================================= */

/* One-time initialisation (called from kernel_main) */
void      cap_table_init(void);

/* Create a root capability (kernel or privileged service only) */
cap_id_t  cap_create(cap_type_t type, cap_rights_t rights,
                     void* object, uint32_t owner_tid);

/* Derive a capability with equal-or-lesser rights */
cap_id_t  cap_derive(cap_id_t parent, cap_rights_t reduced_rights);

/* Revoke: invalidate this cap and all caps derived from it */
bool      cap_revoke(cap_id_t id);

/* Look up a capability by ID (NULL if invalid/revoked) */
cap_t*    cap_get(cap_id_t id);

/* Check whether a cap holds the specified rights */
bool      cap_check(cap_id_t id, cap_rights_t required);

/* Reference counting */
void      cap_addref(cap_id_t id);
void      cap_release(cap_id_t id);

/* Transfer ownership to another task (requires CAP_RIGHT_GRANT) */
cap_id_t  cap_transfer(cap_id_t id, uint32_t new_owner_tid);

/* Diagnostics */
uint32_t  cap_count(void);
void      cap_dump_table(void);

#endif /* KERNEL_CAP_H */
