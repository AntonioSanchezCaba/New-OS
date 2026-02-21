/*
 * include/kernel/svcbus.h — Aether OS Service Bus
 *
 * The service bus is a kernel-managed name registry.
 * Services register by a well-known name and expose an IPC port.
 * Clients look up services by name and receive a port capability.
 *
 * The service bus is the ONLY way to discover a service.
 * There is no global variable, no header-defined address.
 * Every connection goes through a capability-validated lookup.
 */
#ifndef KERNEL_SVCBUS_H
#define KERNEL_SVCBUS_H

#include <types.h>
#include <kernel/ipc.h>
#include <kernel/cap.h>

/* =========================================================
 * Well-known service names
 * ========================================================= */

#define SVC_DISPLAY  "aether.display"
#define SVC_INPUT    "aether.input"
#define SVC_VFS      "aether.vfs"
#define SVC_NETWORK  "aether.network"
#define SVC_AUDIO    "aether.audio"
#define SVC_LAUNCHER "aether.launcher"

/* =========================================================
 * Error codes
 * ========================================================= */

typedef enum {
    SVC_OK          =  0,
    SVC_ERR_INVALID = -1,   /* NULL name or invalid port */
    SVC_ERR_EXISTS  = -2,   /* Service already registered */
    SVC_ERR_FULL    = -3,   /* Registry is at capacity */
    SVC_ERR_NOTFOUND= -4,   /* No service with that name */
    SVC_ERR_NOPERM  = -5,   /* Unregister by non-owner */
} svc_err_t;

#define SVCBUS_MAX_SERVICES  64
#define SVCBUS_NAME_LEN      64

/* =========================================================
 * Service entry (internal — exposed for diagnostics)
 * ========================================================= */

typedef struct {
    char      name[SVCBUS_NAME_LEN]; /* Registered name */
    port_id_t port;                  /* Service's main IPC port */
    cap_id_t  cap;                   /* Service capability token */
    uint32_t  owner_tid;             /* Task ID of the service */
    uint32_t  version;               /* API version */
    bool      active;
} svc_entry_t;

/* =========================================================
 * API
 * ========================================================= */

void       svcbus_init(void);

/* Register a service (idempotent per name; returns error if name taken) */
svc_err_t  svcbus_register(const char* name, port_id_t port,
                            uint32_t owner_tid, uint32_t version);

/* Discover a service — fills *out_port (and optionally *out_cap) */
svc_err_t  svcbus_lookup(const char* name,
                          port_id_t* out_port, cap_id_t* out_cap);

/* Unregister — only the owning task may do this */
svc_err_t  svcbus_unregister(const char* name, uint32_t owner_tid);

/* Diagnostics */
uint32_t   svcbus_count(void);
void       svcbus_dump(void);

#endif /* KERNEL_SVCBUS_H */
