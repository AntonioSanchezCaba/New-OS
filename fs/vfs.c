/*
 * fs/vfs.c - Virtual File System layer
 *
 * The VFS provides a uniform interface for all filesystem operations.
 * All kernel and user-space I/O goes through this layer.
 *
 * The root filesystem is mounted during kernel initialization.
 * A simple ramfs is used as the initial root filesystem.
 */
#include <fs/vfs.h>
#include <kernel.h>
#include <memory.h>
#include <types.h>
#include <string.h>

/* Root of the VFS tree */
static vfs_node_t* vfs_root_node = NULL;

/* ============================================================
 * VFS initialization
 * ============================================================ */

void vfs_init(void)
{
    vfs_root_node = NULL;
    kinfo("VFS initialized");
}

void vfs_mount_root(vfs_node_t* root)
{
    vfs_root_node = root;
    kinfo("VFS: root filesystem mounted at '/'");
}

vfs_node_t* vfs_root(void)
{
    return vfs_root_node;
}

/* ============================================================
 * Path resolution
 * ============================================================ */

/*
 * vfs_canonicalize - resolve '.' and '..' components in a path.
 * Input must be absolute (starts with '/').
 * Output is written to @out (size @out_len); always starts with '/'.
 */
static void vfs_canonicalize(const char* path, char* out, int out_len)
{
    /* Tokenize into up to 64 components (pointers into a scratch buffer) */
    char scratch[VFS_NAME_MAX * 16];
    strncpy(scratch, path, sizeof(scratch) - 1);
    scratch[sizeof(scratch) - 1] = '\0';

    const char* parts[64];
    int depth = 0;

    char* p = scratch;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char* start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = '\0';

        if (start[0] == '.' && start[1] == '\0') {
            /* '.' — stay */
        } else if (start[0] == '.' && start[1] == '.' && start[2] == '\0') {
            /* '..' — go up one level */
            if (depth > 0) depth--;
        } else {
            if (depth < 64) parts[depth++] = start;
        }
    }

    /* Reconstruct */
    int pos = 0;
    if (out_len < 2) { if (out_len > 0) out[0] = '\0'; return; }
    out[pos++] = '/';
    for (int i = 0; i < depth && pos < out_len - 1; i++) {
        if (i > 0 && pos < out_len - 1) out[pos++] = '/';
        int len = (int)strlen(parts[i]);
        if (pos + len < out_len - 1) {
            memcpy(out + pos, parts[i], (size_t)len);
            pos += len;
        }
    }
    out[pos] = '\0';
}

/*
 * vfs_resolve_path - walk the VFS tree to find the node at @path.
 *
 * Supports absolute paths starting with '/'.
 * Path components are split by '/'.
 * '.' and '..' are resolved before walking the tree.
 * Returns the vfs_node_t for the final component, or NULL if not found.
 */
vfs_node_t* vfs_resolve_path(const char* path)
{
    if (!path || !vfs_root_node) return NULL;

    /* Canonicalize to resolve . and .. */
    char canon[VFS_NAME_MAX * 16];
    if (path[0] == '/') {
        vfs_canonicalize(path, canon, (int)sizeof(canon));
    } else {
        /* Relative path: prepend '/' for simple cases */
        char abs_path[VFS_NAME_MAX * 16];
        abs_path[0] = '/';
        strncpy(abs_path + 1, path, sizeof(abs_path) - 2);
        abs_path[sizeof(abs_path) - 1] = '\0';
        vfs_canonicalize(abs_path, canon, (int)sizeof(canon));
    }

    /* Start from root */
    vfs_node_t* node = vfs_root_node;
    const char* p    = canon + 1; /* skip leading '/' */

    if (*p == '\0') return vfs_root_node; /* Bare "/" */

    /* Copy so we can tokenize */
    char tmp[VFS_NAME_MAX * 16];
    strncpy(tmp, p, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char* saveptr = NULL;
    char* token   = strtok_r(tmp, "/", &saveptr);

    while (token && node) {
        if (!(node->flags & VFS_DIRECTORY)) return NULL;
        node = vfs_finddir(node, token);
        token = strtok_r(NULL, "/", &saveptr);
    }

    return node;
}

/* ============================================================
 * VFS operations (dispatch to filesystem-specific implementations)
 * ============================================================ */

vfs_node_t* vfs_open(const char* path, int flags)
{
    vfs_node_t* node = vfs_resolve_path(path);

    if (!node && (flags & O_CREAT)) {
        /* Try to create the file */
        /* Find the parent directory */
        char dir_path[VFS_NAME_MAX * 16];
        char name[VFS_NAME_MAX + 1];

        strncpy(dir_path, path, sizeof(dir_path) - 1);
        const char* slash = strrchr(path, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - path);
            memcpy(dir_path, path, dir_len);
            dir_path[dir_len] = '\0';
            strncpy(name, slash + 1, VFS_NAME_MAX);
        } else {
            dir_path[0] = '\0';
            strncpy(name, path, VFS_NAME_MAX);
        }

        vfs_node_t* parent = dir_path[0] ? vfs_resolve_path(dir_path)
                                          : vfs_root_node;
        if (!parent || !(parent->flags & VFS_DIRECTORY)) return NULL;

        if (!parent->ops || !parent->ops->create) return NULL;
        if (parent->ops->create(parent, name, 0644) != 0) return NULL;

        node = vfs_finddir(parent, name);
    }

    if (!node) return NULL;

    /* O_TRUNC: reset file size to 0 before returning */
    if ((flags & O_TRUNC) && !(node->flags & VFS_DIRECTORY))
        node->size = 0;

    /* Call the filesystem's open handler if present */
    if (node->ops && node->ops->open) {
        node->ops->open(node, flags);
    }

    node->refcount++;
    return node;
}

void vfs_close(vfs_node_t* node)
{
    if (!node) return;

    if (node->ops && node->ops->close) {
        node->ops->close(node);
    }

    if (node->refcount > 0) node->refcount--;
}

ssize_t vfs_read(vfs_node_t* node, off_t offset, size_t size, void* buf)
{
    if (!node || !buf) return -1;
    if (!node->ops || !node->ops->read) return -1;
    return node->ops->read(node, offset, size, buf);
}

ssize_t vfs_write(vfs_node_t* node, off_t offset, size_t size, const void* buf)
{
    if (!node || !buf) return -1;
    if (!node->ops || !node->ops->write) return -1;
    return node->ops->write(node, offset, size, buf);
}

vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name)
{
    if (!node || !name) return NULL;

    /* Follow mountpoint into the mounted filesystem root */
    if (node->mount) node = node->mount;

    if (!(node->flags & VFS_DIRECTORY)) return NULL;
    if (!node->ops || !node->ops->finddir) return NULL;
    return node->ops->finddir(node, name);
}

/* Internal helper: returns raw pointer (used by fat32 etc.) */
static vfs_dirent_t* _vfs_readdir_raw(vfs_node_t* node, uint32_t index)
{
    if (!node) return NULL;
    /* Follow mountpoint into mounted filesystem */
    if (node->mount) node = node->mount;
    if (!(node->flags & VFS_DIRECTORY)) return NULL;
    if (!node->ops || !node->ops->readdir) return NULL;
    return node->ops->readdir(node, index);
}

int vfs_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out)
{
    if (!node || !out) return -EINVAL;
    if (!(node->flags & VFS_DIRECTORY)) return -ENOTDIR;
    vfs_dirent_t* de = _vfs_readdir_raw(node, index);
    if (!de) return -ENOENT;
    *out = *de;
    return 0;
}

int vfs_mkdir(const char* path)
{
    /* Find parent directory */
    char dir_path[VFS_NAME_MAX * 16];
    char name[VFS_NAME_MAX + 1];

    const char* slash = strrchr(path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - path);
        memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';
        strncpy(name, slash + 1, VFS_NAME_MAX);
    } else {
        dir_path[0] = '\0';
        strncpy(name, path, VFS_NAME_MAX);
    }

    vfs_node_t* parent = dir_path[0] ? vfs_resolve_path(dir_path)
                                      : vfs_root_node;
    if (!parent) return -ENOENT;

    if (!parent->ops || !parent->ops->mkdir) return -ENOSYS;
    return parent->ops->mkdir(parent, name, 0755);
}

int vfs_create(const char* path, uint32_t mode)
{
    char dir_path[VFS_NAME_MAX * 16];
    char name[VFS_NAME_MAX + 1];

    const char* slash = strrchr(path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - path);
        memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';
        strncpy(name, slash + 1, VFS_NAME_MAX);
    } else {
        dir_path[0] = '\0';
        strncpy(name, path, VFS_NAME_MAX);
    }

    vfs_node_t* parent = dir_path[0] ? vfs_resolve_path(dir_path)
                                      : vfs_root_node;
    if (!parent) return -ENOENT;

    if (!parent->ops || !parent->ops->create) return -ENOSYS;
    return parent->ops->create(parent, name, mode);
}

int vfs_unlink(const char* path)
{
    char dir_path[VFS_NAME_MAX * 16];
    char name[VFS_NAME_MAX + 1];

    const char* slash = strrchr(path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - path);
        memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';
        strncpy(name, slash + 1, VFS_NAME_MAX);
    } else {
        dir_path[0] = '\0';
        strncpy(name, path, VFS_NAME_MAX);
    }

    vfs_node_t* parent = dir_path[0] ? vfs_resolve_path(dir_path)
                                      : vfs_root_node;
    if (!parent) return -ENOENT;

    if (!parent->ops || !parent->ops->unlink) return -ENOSYS;
    return parent->ops->unlink(parent, name);
}

int vfs_mount(const char* path, vfs_node_t* node, void* fs_data)
{
    (void)fs_data;

    if (!path || !node) return -EINVAL;

    /* Special case: mount at root "/" */
    if (path[0] == '/' && path[1] == '\0') {
        vfs_root_node = node;
        node->mountpoint = NULL;
        kinfo("VFS: mounted root filesystem");
        return 0;
    }

    /* Find the directory that will become the mount point */
    vfs_node_t* mp = vfs_resolve_path(path);
    if (!mp) {
        /* Try to create the mount point directory if it doesn't exist */
        vfs_mkdir(path);
        mp = vfs_resolve_path(path);
        if (!mp) return -ENOENT;
    }
    if (!(mp->flags & VFS_DIRECTORY)) return -ENOTDIR;

    mp->mount       = node;
    node->mountpoint = mp;

    kinfo("VFS: filesystem mounted at '%s'", path);
    return 0;
}

/* ── vfs_stat ────────────────────────────────────────────────────────── */
int vfs_stat(vfs_node_t* node, vfs_stat_t* st)
{
    if (!node || !st) return -EINVAL;
    memset(st, 0, sizeof(*st));
    st->size  = node->size;
    st->mode  = node->mode;
    st->uid   = node->uid;
    st->gid   = node->gid;
    st->atime = node->atime;
    st->mtime = node->mtime;
    st->ctime = node->ctime;
    st->type  = (node->flags & VFS_DIRECTORY) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    return 0;
}

/* ── vfs_rmdir ───────────────────────────────────────────────────────── */
int vfs_rmdir(const char* path)
{
    if (!path) return -EINVAL;
    /* For now delegate to unlink — a proper impl checks it's an empty dir */
    return vfs_unlink(path);
}


