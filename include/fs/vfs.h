/*
 * fs/vfs.h - Virtual File System (VFS) layer interface
 *
 * The VFS provides a unified interface for multiple filesystem types.
 * All filesystem operations go through this layer.
 */
#ifndef FS_VFS_H
#define FS_VFS_H

#include <types.h>
#include <errno.h>

/* File type flags */
#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_CHARDEVICE  0x04
#define VFS_BLOCKDEVICE 0x08
#define VFS_PIPE        0x10
#define VFS_SYMLINK     0x20
#define VFS_MOUNTPOINT  0x40

/* File open flags */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x0200000

/* Seek origins */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Maximum filename length */
#define VFS_NAME_MAX  255

/* Inode number type */
typedef uint64_t ino_t;

/* Forward declarations */
struct vfs_node;
struct vfs_dirent;

/* VFS operations function pointers */
typedef struct vfs_ops {
    /* Read/write data */
    ssize_t (*read)(struct vfs_node* node, off_t offset, size_t size, void* buf);
    ssize_t (*write)(struct vfs_node* node, off_t offset, size_t size, const void* buf);

    /* Directory operations */
    struct vfs_node* (*finddir)(struct vfs_node* node, const char* name);
    struct vfs_dirent* (*readdir)(struct vfs_node* node, uint32_t index);
    int (*mkdir)(struct vfs_node* node, const char* name, uint32_t mode);
    int (*create)(struct vfs_node* node, const char* name, uint32_t mode);
    int (*unlink)(struct vfs_node* node, const char* name);

    /* Node management */
    void (*open)(struct vfs_node* node, int flags);
    void (*close)(struct vfs_node* node);

    /* Special */
    int (*ioctl)(struct vfs_node* node, uint64_t request, void* arg);
} vfs_ops_t;

/* VFS node (inode equivalent) */
typedef struct vfs_node {
    char         name[VFS_NAME_MAX + 1];
    uint32_t     flags;     /* VFS_FILE, VFS_DIRECTORY, etc. */
    uint32_t     mode;      /* Permissions */
    uint32_t     uid;
    uint32_t     gid;
    uint64_t     size;
    uint64_t     inode;
    uint64_t     atime;     /* Access time */
    uint64_t     mtime;     /* Modification time */
    uint64_t     ctime;     /* Creation time */
    int          refcount;

    vfs_ops_t*   ops;       /* Operations for this node */
    void*        impl;      /* Filesystem-specific data */

    struct vfs_node* mountpoint; /* If this is a mount point */
    struct vfs_node* mount;      /* Mounted filesystem root */
} vfs_node_t;

/* File type constants for vfs_dirent.type and vfs_stat.type */
#define VFS_TYPE_FILE  1
#define VFS_TYPE_DIR   2
#define VFS_TYPE_LINK  3
#define VFS_TYPE_DEV   4

/* Directory entry (used when listing directory contents) */
typedef struct vfs_dirent {
    char     name[VFS_NAME_MAX + 1];
    uint64_t inode;
    uint32_t type;   /* VFS_TYPE_FILE, VFS_TYPE_DIR, … */
    uint64_t size;
} vfs_dirent_t;

/* Stat result */
typedef struct vfs_stat {
    uint64_t size;
    uint32_t type;   /* VFS_TYPE_FILE etc. */
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
} vfs_stat_t;

/* Open file handle */
typedef struct file {
    vfs_node_t* node;
    off_t       offset;
    int         flags;
    int         refcount;
} file_t;

/* Extra open flags (aliases for compatibility) */
#define VFS_O_RDONLY   O_RDONLY
#define VFS_O_WRONLY   O_WRONLY
#define VFS_O_RDWR     O_RDWR
#define VFS_O_CREAT    O_CREAT
#define VFS_O_TRUNC    O_TRUNC
#define VFS_O_APPEND   O_APPEND

/* Short-form aliases used in apps/services */
#define VFS_O_READ     VFS_O_RDONLY
#define VFS_O_WRITE    VFS_O_WRONLY
#define VFS_O_CREATE   VFS_O_CREAT

/* Convenience: return size of a VFS node (0 if NULL) */
static inline uint64_t vfs_size(const vfs_node_t* node)
{
    return node ? node->size : 0;
}

/* VFS API */
void       vfs_init(void);
vfs_node_t* vfs_root(void);
void       vfs_mount_root(vfs_node_t* root);
int        vfs_mount(const char* path, vfs_node_t* node, void* fs_data);

/* Path-based operations */
vfs_node_t* vfs_open(const char* path, int flags);
void        vfs_close(vfs_node_t* node);
ssize_t     vfs_read(vfs_node_t* node, off_t offset, size_t size, void* buf);
ssize_t     vfs_write(vfs_node_t* node, off_t offset, size_t size, const void* buf);
vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name);
int         vfs_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out);
int         vfs_stat(vfs_node_t* node, vfs_stat_t* st);
int         vfs_mkdir(const char* path);
int         vfs_rmdir(const char* path);
int         vfs_create(const char* path, uint32_t mode);
int         vfs_unlink(const char* path);
vfs_node_t* vfs_resolve_path(const char* path);

#endif /* FS_VFS_H */
