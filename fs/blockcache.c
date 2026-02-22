/*
 * fs/blockcache.c — Write-back LRU block cache
 *
 * Uses a direct-mapped + LRU eviction strategy:
 * - 128 slots, each holding one 512-byte sector
 * - LRU counter: 32-bit monotonic tick incremented on each access
 * - Dirty bit: set on writes, cleared after write-back
 * - On cache miss: evict LRU dirty slot (write back), load new sector
 */
#include <fs/blockcache.h>
#include <drivers/ata.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <types.h>

typedef struct {
    uint64_t lba;
    int      drive;
    uint8_t  data[BCACHE_SECTOR_SIZE];
    bool     valid;
    bool     dirty;
    uint32_t last_use;    /* LRU counter */
} bcache_slot_t;

static bcache_slot_t g_slots[BCACHE_SLOTS];
static uint32_t      g_lru_clock = 0;
static uint32_t      g_hits   = 0;
static uint32_t      g_misses = 0;
static uint32_t      g_writes = 0;

/* =========================================================
 * Find a slot matching (drive, lba) or return NULL
 * ========================================================= */
static bcache_slot_t* find_slot(int drive, uint64_t lba)
{
    for (int i = 0; i < BCACHE_SLOTS; i++) {
        if (g_slots[i].valid &&
            g_slots[i].drive == drive &&
            g_slots[i].lba   == lba)
            return &g_slots[i];
    }
    return NULL;
}

/* =========================================================
 * Evict one slot: prefer clean, fall back to dirty (write-back)
 * ========================================================= */
static bcache_slot_t* evict_slot(void)
{
    /* Find LRU clean slot first */
    bcache_slot_t* best = NULL;
    uint32_t oldest = 0xFFFFFFFF;

    for (int i = 0; i < BCACHE_SLOTS; i++) {
        if (!g_slots[i].valid) return &g_slots[i];  /* Free slot */
        if (!g_slots[i].dirty && g_slots[i].last_use < oldest) {
            oldest = g_slots[i].last_use;
            best   = &g_slots[i];
        }
    }
    if (best) { best->valid = false; return best; }

    /* All dirty — find LRU dirty and flush it */
    oldest = 0xFFFFFFFF;
    for (int i = 0; i < BCACHE_SLOTS; i++) {
        if (g_slots[i].last_use < oldest) {
            oldest = g_slots[i].last_use;
            best   = &g_slots[i];
        }
    }
    if (best && best->dirty) {
        extern ata_drive_t ata_drives[];
        if (best->drive < 4 && ata_drives[best->drive].present) {
            ata_write(&ata_drives[best->drive], best->lba, 1, best->data);
        }
        g_writes++;
        best->dirty = false;
    }
    best->valid = false;
    return best;
}

/* =========================================================
 * Public API
 * ========================================================= */
uint8_t* bcache_get(int drive_idx, uint64_t lba)
{
    extern ata_drive_t ata_drives[];
    if (drive_idx < 0 || drive_idx >= 4) return NULL;

    bcache_slot_t* s = find_slot(drive_idx, lba);
    if (s) {
        g_hits++;
        s->last_use = ++g_lru_clock;
        return s->data;
    }

    /* Cache miss — evict and load */
    g_misses++;
    s = evict_slot();
    if (!s) return NULL;

    if (!ata_drives[drive_idx].present) return NULL;
    if (ata_read(&ata_drives[drive_idx], lba, 1, s->data) != 0)
        return NULL;

    s->valid    = true;
    s->dirty    = false;
    s->drive    = drive_idx;
    s->lba      = lba;
    s->last_use = ++g_lru_clock;
    return s->data;
}

void bcache_dirty(int drive_idx, uint64_t lba)
{
    bcache_slot_t* s = find_slot(drive_idx, lba);
    if (s) s->dirty = true;
}

int bcache_write_through(int drive_idx, uint64_t lba,
                          const uint8_t* data)
{
    extern ata_drive_t ata_drives[];
    if (drive_idx < 0 || drive_idx >= 4) return -1;

    /* Update cache if present */
    bcache_slot_t* s = find_slot(drive_idx, lba);
    if (!s) {
        s = evict_slot();
        if (!s) return -1;
        s->valid = true;
        s->drive = drive_idx;
        s->lba   = lba;
    }
    memcpy(s->data, data, BCACHE_SECTOR_SIZE);
    s->dirty    = false;
    s->last_use = ++g_lru_clock;

    /* Write to disk immediately */
    if (!ata_drives[drive_idx].present) return -1;
    int rc = ata_write(&ata_drives[drive_idx], lba, 1, (void*)data);
    g_writes++;
    return rc;
}

int bcache_flush(int drive_idx)
{
    extern ata_drive_t ata_drives[];
    if (drive_idx < 0 || drive_idx >= 4) return -1;
    if (!ata_drives[drive_idx].present)  return -1;

    int errors = 0;
    for (int i = 0; i < BCACHE_SLOTS; i++) {
        bcache_slot_t* s = &g_slots[i];
        if (!s->valid || !s->dirty || s->drive != drive_idx) continue;
        if (ata_write(&ata_drives[drive_idx], s->lba, 1, s->data) != 0)
            errors++;
        else { s->dirty = false; g_writes++; }
    }
    return errors ? -1 : 0;
}

int bcache_flush_all(void)
{
    int rc = 0;
    for (int d = 0; d < 4; d++) rc |= bcache_flush(d);
    return rc;
}

void bcache_invalidate(int drive_idx)
{
    for (int i = 0; i < BCACHE_SLOTS; i++) {
        if (g_slots[i].valid && g_slots[i].drive == drive_idx)
            g_slots[i].valid = false;
    }
}

void bcache_stats(uint32_t* hits, uint32_t* misses, uint32_t* writes)
{
    if (hits)   *hits   = g_hits;
    if (misses) *misses = g_misses;
    if (writes) *writes = g_writes;
}
