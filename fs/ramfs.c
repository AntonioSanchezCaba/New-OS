/*
 * fs/ramfs.c - RAM-based filesystem (in-memory virtual filesystem)
 *
 * Provides the initial root filesystem backed entirely by kernel heap memory.
 * Used as the root filesystem before any disk-based filesystem is mounted.
 *
 * Features:
 *   - Files: stored as malloc'd byte arrays (grow on write)
 *   - Directories: stored as linked lists of child nodes
 *   - No persistent storage
 *
 * This filesystem is populated at boot with:
 *   /bin/sh       - the shell binary
 *   /bin/init     - the init process
 *   /etc/motd     - message of the day
 *   /dev/null     - null device
 *   /proc/        - process information (stub)
 */
#include <fs/vfs.h>
#include <kernel.h>
#include <memory.h>
#include <types.h>
#include <string.h>

/* ============================================================
 * Internal ramfs node structure
 * ============================================================ */

#define RAMFS_MAX_CHILDREN 64
#define RAMFS_INITIAL_FILE_SIZE 256

typedef struct ramfs_node {
    vfs_node_t          vnode;          /* Embedded VFS node (must be first) */
    uint8_t*            data;           /* File content (NULL for directories) */
    size_t              data_capacity;  /* Allocated capacity */
    struct ramfs_node*  children[RAMFS_MAX_CHILDREN];
    int                 child_count;
    struct ramfs_node*  parent;
} ramfs_node_t;

/* Static dirent for readdir */
static vfs_dirent_t dirent_buf;

/* ============================================================
 * VFS operation implementations
 * ============================================================ */

static ssize_t ramfs_read(vfs_node_t* node, off_t offset, size_t size, void* buf)
{
    ramfs_node_t* rnode = (ramfs_node_t*)node;

    if (!rnode->data) return 0;
    if ((uint64_t)offset >= node->size) return 0;

    size_t avail = (size_t)(node->size - (uint64_t)offset);
    size_t to_read = MIN(size, avail);
    memcpy(buf, rnode->data + offset, to_read);
    return (ssize_t)to_read;
}

static ssize_t ramfs_write(vfs_node_t* node, off_t offset, size_t size,
                            const void* buf)
{
    ramfs_node_t* rnode = (ramfs_node_t*)node;

    size_t new_end = (size_t)offset + size;

    /* Grow the buffer if needed */
    if (new_end > rnode->data_capacity) {
        size_t new_cap = MAX(new_end, rnode->data_capacity * 2);
        if (new_cap < 64) new_cap = 64;

        uint8_t* new_data = (uint8_t*)krealloc(rnode->data, new_cap);
        if (!new_data) return -ENOMEM;

        /* Zero out newly allocated area */
        if (new_cap > rnode->data_capacity) {
            memset(new_data + rnode->data_capacity, 0,
                   new_cap - rnode->data_capacity);
        }

        rnode->data = new_data;
        rnode->data_capacity = new_cap;
    }

    memcpy(rnode->data + offset, buf, size);

    if (new_end > (size_t)node->size) {
        node->size = (uint64_t)new_end;
    }

    return (ssize_t)size;
}

static vfs_node_t* ramfs_finddir(vfs_node_t* node, const char* name)
{
    ramfs_node_t* rnode = (ramfs_node_t*)node;

    for (int i = 0; i < rnode->child_count; i++) {
        if (strcmp(rnode->children[i]->vnode.name, name) == 0) {
            return &rnode->children[i]->vnode;
        }
    }
    return NULL;
}

static vfs_dirent_t* ramfs_readdir(vfs_node_t* node, uint32_t index)
{
    ramfs_node_t* rnode = (ramfs_node_t*)node;

    /* Virtual entries: "." and ".." */
    if (index == 0) {
        strcpy(dirent_buf.name, ".");
        dirent_buf.inode = node->inode;
        dirent_buf.type  = VFS_DIRECTORY;
        return &dirent_buf;
    }
    if (index == 1) {
        strcpy(dirent_buf.name, "..");
        dirent_buf.inode = rnode->parent ? rnode->parent->vnode.inode : node->inode;
        dirent_buf.type  = VFS_DIRECTORY;
        return &dirent_buf;
    }

    int child_idx = (int)(index - 2);
    if (child_idx >= rnode->child_count) return NULL;

    vfs_node_t* child = &rnode->children[child_idx]->vnode;
    strncpy(dirent_buf.name, child->name, VFS_NAME_MAX);
    dirent_buf.inode = child->inode;
    dirent_buf.type  = child->flags;
    return &dirent_buf;
}

static int ramfs_mkdir(vfs_node_t* parent_vnode, const char* name, uint32_t mode)
{
    ramfs_node_t* parent = (ramfs_node_t*)parent_vnode;

    if (parent->child_count >= RAMFS_MAX_CHILDREN) return -ENOSPC;

    ramfs_node_t* child = (ramfs_node_t*)kmalloc(sizeof(ramfs_node_t));
    if (!child) return -ENOMEM;
    memset(child, 0, sizeof(ramfs_node_t));

    strncpy(child->vnode.name, name, VFS_NAME_MAX);
    child->vnode.flags   = VFS_DIRECTORY;
    child->vnode.mode    = mode;
    child->vnode.inode   = (uint64_t)(uintptr_t)child;
    child->vnode.size    = 0;
    child->vnode.ops     = parent_vnode->ops;
    child->parent        = parent;

    parent->children[parent->child_count++] = child;
    return 0;
}

static int ramfs_create(vfs_node_t* parent_vnode, const char* name, uint32_t mode)
{
    ramfs_node_t* parent = (ramfs_node_t*)parent_vnode;

    /* Return success (idempotent) if file already exists */
    if (ramfs_finddir(parent_vnode, name)) return 0;

    if (parent->child_count >= RAMFS_MAX_CHILDREN) return -ENOSPC;

    ramfs_node_t* child = (ramfs_node_t*)kmalloc(sizeof(ramfs_node_t));
    if (!child) return -ENOMEM;
    memset(child, 0, sizeof(ramfs_node_t));

    strncpy(child->vnode.name, name, VFS_NAME_MAX);
    child->vnode.flags = VFS_FILE;
    child->vnode.mode  = mode;
    child->vnode.inode = (uint64_t)(uintptr_t)child;
    child->vnode.size  = 0;
    child->vnode.ops   = parent_vnode->ops;
    child->parent      = parent;

    child->data          = NULL;
    child->data_capacity = 0;

    parent->children[parent->child_count++] = child;
    return 0;
}

static int ramfs_unlink(vfs_node_t* parent_vnode, const char* name)
{
    ramfs_node_t* parent = (ramfs_node_t*)parent_vnode;

    for (int i = 0; i < parent->child_count; i++) {
        if (strcmp(parent->children[i]->vnode.name, name) == 0) {
            ramfs_node_t* victim = parent->children[i];

            /* Free file data */
            if (victim->data) kfree(victim->data);
            kfree(victim);

            /* Shift remaining children */
            for (int j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->child_count--;
            return 0;
        }
    }
    return -ENOENT;
}

/* VFS operations table for ramfs */
static vfs_ops_t ramfs_ops = {
    .read    = ramfs_read,
    .write   = ramfs_write,
    .finddir = ramfs_finddir,
    .readdir = ramfs_readdir,
    .mkdir   = ramfs_mkdir,
    .create  = ramfs_create,
    .unlink  = ramfs_unlink,
    .open    = NULL,
    .close   = NULL,
    .ioctl   = NULL,
};

/* ============================================================
 * Public: create and populate the ramfs root
 * ============================================================ */

/*
 * Helper: create a file in parent with given content.
 */
static void ramfs_add_file(vfs_node_t* parent, const char* name,
                            const void* data, size_t size, uint32_t mode)
{
    ramfs_create(parent, name, mode);
    vfs_node_t* file = ramfs_finddir(parent, name);
    if (file && data && size > 0) {
        ramfs_write(file, 0, size, data);
    }
}

/*
 * ramfs_create_root - build the initial RAM filesystem structure.
 * Returns the root vfs_node_t.
 */
vfs_node_t* ramfs_create_root(void)
{
    ramfs_node_t* root = (ramfs_node_t*)kmalloc(sizeof(ramfs_node_t));
    if (!root) kpanic("ramfs: failed to allocate root node");
    memset(root, 0, sizeof(ramfs_node_t));

    strncpy(root->vnode.name, "/", VFS_NAME_MAX);
    root->vnode.flags = VFS_DIRECTORY;
    root->vnode.mode  = 0755;
    root->vnode.inode = 1;
    root->vnode.ops   = &ramfs_ops;
    root->parent      = root; /* Root's parent is itself */

    /* Create standard directories */
    ramfs_mkdir(&root->vnode, "bin",  0755);
    ramfs_mkdir(&root->vnode, "etc",  0755);
    ramfs_mkdir(&root->vnode, "dev",  0755);
    ramfs_mkdir(&root->vnode, "proc", 0555);
    ramfs_mkdir(&root->vnode, "tmp",  0777);
    ramfs_mkdir(&root->vnode, "home", 0755);
    vfs_node_t* home = ramfs_finddir(&root->vnode, "home");
    if (home) ramfs_mkdir(home, "user", 0755);  /* default user home */
    ramfs_mkdir(&root->vnode, "var",  0755);
    ramfs_mkdir(&root->vnode, "usr",  0755);
    ramfs_mkdir(&root->vnode, "sys",  0755);

    /* /sys/* — kernel subsystem data directories */
    vfs_node_t* sys = ramfs_finddir(&root->vnode, "sys");
    if (sys) {
        ramfs_mkdir(sys, "pkg",      0755);  /* apkg: /sys/pkg/db */
        ramfs_mkdir(sys, "users",    0700);  /* users: /sys/users/db */
        ramfs_mkdir(sys, "packages", 0755);  /* pkg:   /sys/packages/* */
        vfs_node_t* pkgs = ramfs_finddir(sys, "packages");
        if (pkgs) ramfs_mkdir(pkgs, "cache", 0755);
    }

    /* /etc/motd - message of the day */
    const char* motd =
        "\nWelcome to AetherOS!\n"
        "A 64-bit operating system written in C and Assembly.\n"
        "Type 'help' for a list of commands.\n\n";
    vfs_node_t* etc = ramfs_finddir(&root->vnode, "etc");
    if (etc) ramfs_add_file(etc, "motd", motd, strlen(motd), 0444);

    /* /etc/hostname */
    const char* hostname = "aetheros\n";
    if (etc) ramfs_add_file(etc, "hostname", hostname, strlen(hostname), 0444);

    /* /dev/null - null device */
    vfs_node_t* dev = ramfs_finddir(&root->vnode, "dev");
    if (dev) {
        ramfs_create(dev, "null", 0666);
        ramfs_create(dev, "zero", 0666);
    }

    kinfo("ramfs: root filesystem created with standard directories");
    return &root->vnode;
}
