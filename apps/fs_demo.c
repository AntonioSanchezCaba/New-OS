/*
 * apps/fs_demo.c — AetherFS read/write demonstration
 *
 * Exercises the full persistent filesystem stack:
 *   VFS → EXT2 driver → (ATA block device at runtime)
 *
 * Called from the kernel shell or as a standalone test surface.
 * Runs after diskman_init() has mounted partitions under /mnt/.
 */
#include <fs/vfs.h>
#include <kernel.h>
#include <string.h>

/* =========================================================
 * Demo entry point
 * Call this after diskman_init() has run.
 * ========================================================= */
void fs_demo_run(void)
{
    klog_info("=== fs_demo: AetherFS read/write demo ===");

    /* --------------------------------------------------
     * 1. Create a directory
     * -------------------------------------------------- */
    int rc = vfs_mkdir("/mnt/hd0p0/testdir");
    if (rc == 0)
        klog_info("fs_demo: mkdir /mnt/hd0p0/testdir  OK");
    else
        klog_warn("fs_demo: mkdir failed (rc=%d) — check diskman mounted hd0p0", rc);

    /* --------------------------------------------------
     * 2. Write a file
     * -------------------------------------------------- */
    vfs_node_t* f = vfs_open("/mnt/hd0p0/testdir/hello.txt",
                              O_WRONLY | O_CREAT);
    if (!f) {
        klog_warn("fs_demo: open for write failed");
        return;
    }

    const char* msg  = "Hello from AetherFS!\nPersistent write test.\n";
    uint32_t    wlen = (uint32_t)strlen(msg);
    ssize_t     wrote = vfs_write(f, 0, wlen, msg);
    vfs_close(f);

    if ((uint32_t)wrote == wlen)
        klog_info("fs_demo: wrote %u bytes to hello.txt  OK", wlen);
    else
        klog_warn("fs_demo: partial write %ld/%u", (long)wrote, wlen);

    /* --------------------------------------------------
     * 3. Read the file back and verify
     * -------------------------------------------------- */
    f = vfs_open("/mnt/hd0p0/testdir/hello.txt", O_RDONLY);
    if (!f) {
        klog_warn("fs_demo: open for read failed");
        return;
    }

    char buf[128];
    memset(buf, 0, sizeof(buf));
    ssize_t got = vfs_read(f, 0, wlen, buf);
    vfs_close(f);

    if ((uint32_t)got == wlen && memcmp(buf, msg, wlen) == 0)
        klog_info("fs_demo: read-back verified  OK");
    else
        klog_warn("fs_demo: read-back mismatch (got %ld bytes)", (long)got);

    /* --------------------------------------------------
     * 4. List the directory
     * -------------------------------------------------- */
    klog_info("fs_demo: listing /mnt/hd0p0/testdir:");
    vfs_node_t* dir = vfs_resolve_path("/mnt/hd0p0/testdir");
    if (dir) {
        vfs_dirent_t ent;
        for (uint32_t idx = 0; ; idx++) {
            int found = vfs_readdir(dir, idx, &ent);
            if (found != 0) break;
            klog_info("  [%u] inode=%-5u  %s", idx, ent.inode, ent.name);
        }
    }

    /* --------------------------------------------------
     * 5. Unlink (delete) the file
     * -------------------------------------------------- */
    rc = vfs_unlink("/mnt/hd0p0/testdir/hello.txt");
    klog_info("fs_demo: unlink hello.txt  %s", rc == 0 ? "OK" : "FAIL");

    /* --------------------------------------------------
     * 6. Write a second, larger file (multi-block test)
     * -------------------------------------------------- */
    f = vfs_open("/mnt/hd0p0/bigfile.bin", O_WRONLY | O_CREAT);
    if (f) {
        /* Write 4 KB across 4 blocks */
        uint8_t pattern[4096];
        for (int i = 0; i < 4096; i++)
            pattern[i] = (uint8_t)(i & 0xFF);
        ssize_t bw = vfs_write(f, 0, sizeof(pattern), pattern);
        vfs_close(f);
        klog_info("fs_demo: multi-block write  %ld/4096 bytes  %s",
                  (long)bw, bw == 4096 ? "OK" : "FAIL");

        /* Read back and spot-check */
        f = vfs_open("/mnt/hd0p0/bigfile.bin", O_RDONLY);
        if (f) {
            uint8_t rbuf[4096];
            ssize_t rb = vfs_read(f, 0, sizeof(rbuf), rbuf);
            vfs_close(f);
            bool ok = (rb == 4096);
            for (int i = 0; i < 4096 && ok; i++)
                if (rbuf[i] != (uint8_t)(i & 0xFF)) ok = false;
            klog_info("fs_demo: multi-block read-back  %s", ok ? "OK" : "FAIL");
        }
    }

    klog_info("=== fs_demo: done ===");
}
