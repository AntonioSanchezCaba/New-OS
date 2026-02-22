/*
 * include/fs/blockcache.h — Block I/O cache (write-back, LRU)
 *
 * Provides a 128-slot cache of 512-byte disk sectors. All filesystem
 * drivers should go through this layer rather than calling ata_read/write
 * directly. Dramatically reduces disk I/O for sequential and repeated reads.
 */
#pragma once
#include <types.h>

#define BCACHE_SECTOR_SIZE  512
#define BCACHE_SLOTS        128   /* 128 × 512 = 64 KB cache */

/* Get a pointer to a cached sector. Reads from disk on cache miss.
 * Returns NULL on error. Do NOT free the returned pointer.
 * The buffer is valid until the next cache operation. */
uint8_t* bcache_get(int drive_idx, uint64_t lba);

/* Mark a cached sector as dirty (will be written on flush). */
void bcache_dirty(int drive_idx, uint64_t lba);

/* Write a modified buffer back immediately. */
int bcache_write_through(int drive_idx, uint64_t lba,
                          const uint8_t* data);

/* Flush all dirty sectors for a given drive to disk. */
int bcache_flush(int drive_idx);

/* Flush all drives. */
int bcache_flush_all(void);

/* Invalidate (evict) all cache entries for a drive. */
void bcache_invalidate(int drive_idx);

/* Statistics */
void bcache_stats(uint32_t* hits, uint32_t* misses, uint32_t* writes);
