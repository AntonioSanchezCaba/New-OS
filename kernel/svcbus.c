/*
 * kernel/svcbus.c — Aether OS Service Bus
 *
 * The service bus is the kernel name registry for Aether services.
 * A service registers once; clients look up by name and receive
 * a capability-protected port reference.
 */
#include <kernel/svcbus.h>
#include <kernel/secmon.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * State
 * ========================================================= */

static svc_entry_t svc_table[SVCBUS_MAX_SERVICES];
static uint32_t    svc_count = 0;

/* =========================================================
 * Initialisation
 * ========================================================= */

void svcbus_init(void)
{
    memset(svc_table, 0, sizeof(svc_table));
    svc_count = 0;
    kinfo("SVCBUS: service registry ready (capacity %d)", SVCBUS_MAX_SERVICES);
}

/* =========================================================
 * svcbus_register
 * ========================================================= */

svc_err_t svcbus_register(const char* name, port_id_t port,
                           uint32_t owner_tid, uint32_t version)
{
    if (!name || port == PORT_INVALID) return SVC_ERR_INVALID;

    /* Reject duplicates */
    for (int i = 0; i < SVCBUS_MAX_SERVICES; i++) {
        if (svc_table[i].active &&
            strncmp(svc_table[i].name, name, SVCBUS_NAME_LEN) == 0) {
            klog_warn("SVCBUS: duplicate registration attempt for '%s'", name);
            return SVC_ERR_EXISTS;
        }
    }

    /* Find free slot */
    for (int i = 0; i < SVCBUS_MAX_SERVICES; i++) {
        if (!svc_table[i].active) {
            strncpy(svc_table[i].name, name, SVCBUS_NAME_LEN - 1);
            svc_table[i].name[SVCBUS_NAME_LEN - 1] = '\0';
            svc_table[i].port      = port;
            svc_table[i].owner_tid = owner_tid;
            svc_table[i].version   = version;
            svc_table[i].active    = true;

            /* Create a service capability (READ + WRITE for clients) */
            svc_table[i].cap = cap_create(CAP_TYPE_SERVICE,
                                          CAP_RIGHT_READ | CAP_RIGHT_WRITE |
                                          CAP_RIGHT_GRANT,
                                          &svc_table[i], owner_tid);

            svc_count++;
            kinfo("SVCBUS: registered '%s' v%u (port=%u tid=%u)",
                  name, version, port, owner_tid);
            secmon_audit_svc(AUDIT_SVC_REGISTER, owner_tid, name);
            return SVC_OK;
        }
    }

    return SVC_ERR_FULL;
}

/* =========================================================
 * svcbus_lookup
 * ========================================================= */

svc_err_t svcbus_lookup(const char* name,
                         port_id_t* out_port, cap_id_t* out_cap)
{
    if (!name || !out_port) return SVC_ERR_INVALID;

    for (int i = 0; i < SVCBUS_MAX_SERVICES; i++) {
        if (svc_table[i].active &&
            strncmp(svc_table[i].name, name, SVCBUS_NAME_LEN) == 0) {
            *out_port = svc_table[i].port;
            if (out_cap) *out_cap = svc_table[i].cap;
            secmon_audit_svc(AUDIT_SVC_LOOKUP, 0, name);
            return SVC_OK;
        }
    }

    return SVC_ERR_NOTFOUND;
}

/* =========================================================
 * svcbus_unregister
 * ========================================================= */

svc_err_t svcbus_unregister(const char* name, uint32_t owner_tid)
{
    for (int i = 0; i < SVCBUS_MAX_SERVICES; i++) {
        if (!svc_table[i].active) continue;
        if (strncmp(svc_table[i].name, name, SVCBUS_NAME_LEN) != 0) continue;

        if (svc_table[i].owner_tid != owner_tid) return SVC_ERR_NOPERM;

        cap_release(svc_table[i].cap);
        svc_table[i].active = false;
        svc_count--;

        kinfo("SVCBUS: unregistered '%s'", name);
        secmon_audit_svc(AUDIT_SVC_UNREGISTER, owner_tid, name);
        return SVC_OK;
    }
    return SVC_ERR_NOTFOUND;
}

/* =========================================================
 * Diagnostics
 * ========================================================= */

uint32_t svcbus_count(void) { return svc_count; }

void svcbus_dump(void)
{
    kinfo("SVCBUS: %u registered services:", svc_count);
    for (int i = 0; i < SVCBUS_MAX_SERVICES; i++) {
        if (!svc_table[i].active) continue;
        kinfo("  [%2d] %-30s v%-3u port=%-5u tid=%-4u cap=%u",
              i,
              svc_table[i].name,
              svc_table[i].version,
              svc_table[i].port,
              svc_table[i].owner_tid,
              svc_table[i].cap);
    }
}
