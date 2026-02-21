/*
 * kernel/cap.c — Aether OS Capability Security System
 *
 * Implements the global capability table and all cap operations.
 * This is the trusted core of Aether's security model: every resource
 * access is gated by a valid capability token.
 *
 * Design notes:
 *   - The table is a flat array; linear scan is acceptable for the
 *     current table size (4096 entries, <100 µs at boot frequency).
 *   - cap_id_t 0 is permanently reserved as "invalid".
 *   - Revocation cascades: revoking cap X also revokes every cap
 *     derived from X (those with parent == X).
 */
#include <kernel/cap.h>
#include <kernel/secmon.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * Global state
 * ========================================================= */

static cap_t    cap_table[CAP_TABLE_SIZE];
static uint32_t cap_next_id      = 1;   /* 0 = invalid, never reused */
static uint32_t cap_active_count = 0;

/* =========================================================
 * Internal helpers
 * ========================================================= */

static cap_t* _alloc_slot(void)
{
    for (int i = 0; i < CAP_TABLE_SIZE; i++) {
        if (!cap_table[i].valid)
            return &cap_table[i];
    }
    return NULL;
}

static uint32_t _next_id(void)
{
    uint32_t id = cap_next_id++;
    if (cap_next_id == 0) cap_next_id = 1; /* Skip 0 */
    return id;
}

/* =========================================================
 * Initialisation
 * ========================================================= */

void cap_table_init(void)
{
    memset(cap_table, 0, sizeof(cap_table));
    cap_next_id      = 1;
    cap_active_count = 0;

    kinfo("CAP: capability table ready — %u slots × %u bytes = %u KB",
          CAP_TABLE_SIZE,
          (uint32_t)sizeof(cap_t),
          (uint32_t)(CAP_TABLE_SIZE * sizeof(cap_t) / 1024));
}

/* =========================================================
 * cap_create — create a root capability
 * Only the kernel or a privileged service should call this.
 * ========================================================= */

cap_id_t cap_create(cap_type_t type, cap_rights_t rights,
                    void* object, uint32_t owner_tid)
{
    cap_t* slot = _alloc_slot();
    if (!slot) {
        klog_warn("CAP: table full — cannot create capability (type=%u)", type);
        return CAP_INVALID_ID;
    }

    cap_id_t id    = _next_id();
    slot->id        = id;
    slot->type      = type;
    slot->rights    = rights;
    slot->object    = object;
    slot->parent    = CAP_INVALID_ID;
    slot->ref_count = 1;
    slot->owner_tid = owner_tid;
    slot->valid     = true;

    cap_active_count++;

    secmon_audit_cap(AUDIT_CAP_CREATE, owner_tid, id,
                     (uint32_t)type, (uint32_t)rights);
    return id;
}

/* =========================================================
 * cap_derive — create a sub-capability with reduced rights
 * ========================================================= */

cap_id_t cap_derive(cap_id_t parent_id, cap_rights_t reduced_rights)
{
    cap_t* parent = cap_get(parent_id);
    if (!parent) return CAP_INVALID_ID;

    /* Cannot escalate beyond parent's rights */
    if ((reduced_rights & ~parent->rights) != 0) {
        klog_warn("CAP: derive attempted rights escalation "
                  "(parent rights=0x%x, requested=0x%x)",
                  parent->rights, reduced_rights);
        secmon_audit_cap(AUDIT_POLICY_DENY, parent->owner_tid,
                         parent_id, (uint32_t)reduced_rights, 0);
        return CAP_INVALID_ID;
    }

    if (!(parent->rights & CAP_RIGHT_MANAGE)) {
        klog_warn("CAP: derive requires CAP_RIGHT_MANAGE on parent cap %u",
                  parent_id);
        return CAP_INVALID_ID;
    }

    cap_t* slot = _alloc_slot();
    if (!slot) return CAP_INVALID_ID;

    cap_id_t id     = _next_id();
    slot->id         = id;
    slot->type       = parent->type;
    slot->rights     = reduced_rights;
    slot->object     = parent->object;
    slot->parent     = parent_id;
    slot->ref_count  = 1;
    slot->owner_tid  = parent->owner_tid;
    slot->valid      = true;

    cap_addref(parent_id);  /* Derived cap keeps parent alive */
    cap_active_count++;

    secmon_audit_cap(AUDIT_CAP_DERIVE, parent->owner_tid, id,
                     parent_id, (uint32_t)reduced_rights);
    return id;
}

/* =========================================================
 * cap_revoke — invalidate a capability (cascades to children)
 * ========================================================= */

bool cap_revoke(cap_id_t id)
{
    cap_t* c = cap_get(id);
    if (!c) return false;

    secmon_audit_cap(AUDIT_CAP_REVOKE, c->owner_tid, id, 0, 0);

    c->valid  = false;
    c->object = NULL;
    cap_active_count--;

    /* Release parent reference */
    if (c->parent != CAP_INVALID_ID)
        cap_release(c->parent);

    /* Cascade: revoke all caps derived from this one */
    for (int i = 0; i < CAP_TABLE_SIZE; i++) {
        if (cap_table[i].valid && cap_table[i].parent == id)
            cap_revoke(cap_table[i].id);
    }

    return true;
}

/* =========================================================
 * cap_get — look up a capability by token ID
 * ========================================================= */

cap_t* cap_get(cap_id_t id)
{
    if (id == CAP_INVALID_ID) return NULL;

    for (int i = 0; i < CAP_TABLE_SIZE; i++) {
        if (cap_table[i].valid && cap_table[i].id == id)
            return &cap_table[i];
    }
    return NULL;
}

/* =========================================================
 * cap_check — does a capability hold the requested rights?
 * ========================================================= */

bool cap_check(cap_id_t id, cap_rights_t required)
{
    cap_t* c = cap_get(id);
    if (!c) return false;
    return (c->rights & required) == required;
}

/* =========================================================
 * Reference counting
 * ========================================================= */

void cap_addref(cap_id_t id)
{
    cap_t* c = cap_get(id);
    if (c) c->ref_count++;
}

void cap_release(cap_id_t id)
{
    cap_t* c = cap_get(id);
    if (!c) return;

    if (c->ref_count > 1) {
        c->ref_count--;
        return;
    }

    /* Last reference: free the slot */
    c->ref_count = 0;
    c->valid     = false;
    c->object    = NULL;

    if (c->parent != CAP_INVALID_ID)
        cap_release(c->parent);

    cap_active_count--;
}

/* =========================================================
 * cap_transfer — change ownership to another task
 * ========================================================= */

cap_id_t cap_transfer(cap_id_t id, uint32_t new_owner_tid)
{
    cap_t* c = cap_get(id);
    if (!c) return CAP_INVALID_ID;

    if (!(c->rights & CAP_RIGHT_GRANT)) {
        klog_warn("CAP: transfer requires CAP_RIGHT_GRANT on cap %u", id);
        return CAP_INVALID_ID;
    }

    uint32_t old = c->owner_tid;
    c->owner_tid = new_owner_tid;

    secmon_audit_cap(AUDIT_CAP_TRANSFER, old, id, new_owner_tid, 0);
    return id;
}

/* =========================================================
 * Diagnostics
 * ========================================================= */

uint32_t cap_count(void) { return cap_active_count; }

void cap_dump_table(void)
{
    kinfo("CAP: %u active capabilities:", cap_active_count);
    for (int i = 0; i < CAP_TABLE_SIZE; i++) {
        if (!cap_table[i].valid) continue;
        cap_t* c = &cap_table[i];
        kinfo("  cap[%u] type=%u rights=0x%02x owner=%u refs=%u%s",
              c->id, (uint32_t)c->type, c->rights, c->owner_tid,
              c->ref_count,
              c->parent != CAP_INVALID_ID ? " (derived)" : "");
    }
}
