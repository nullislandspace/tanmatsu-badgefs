/*
 * badgefs_backend.h - Abstract storage backend interface
 *
 * This header defines the interface that any storage backend must implement.
 * The default implementation is badgefs_backend_mem.c (in-memory storage).
 * You can create alternative backends for network storage, databases, etc.
 *
 * All functions return 0 on success or -errno on failure.
 */

#ifndef BADGEFS_BACKEND_H
#define BADGEFS_BACKEND_H

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stddef.h>
#include <time.h>

/*
 * Backend interface structure
 *
 * Implement all these function pointers to create a custom storage backend.
 * See badgefs_backend_mem.c for a reference implementation.
 */
struct badgefs_backend {
    /* Initialization and cleanup */
    int (*init)(void *config);
    void (*destroy)(void);

    /* File/directory lookup and attributes */
    int (*lookup)(const char *path, struct stat *st);

    /* Directory operations */
    int (*readdir)(const char *path, void *buf, fuse_fill_dir_t filler,
                   off_t offset, struct fuse_file_info *fi,
                   enum fuse_readdir_flags flags);
    int (*mkdir)(const char *path, mode_t mode);
    int (*rmdir)(const char *path);

    /* File creation and deletion */
    int (*create)(const char *path, mode_t mode, struct fuse_file_info *fi);
    int (*unlink)(const char *path);

    /* File I/O */
    int (*open)(const char *path, struct fuse_file_info *fi);
    int (*release)(const char *path, struct fuse_file_info *fi);
    int (*read)(const char *path, char *buf, size_t size, off_t offset,
                struct fuse_file_info *fi);
    int (*write)(const char *path, const char *buf, size_t size, off_t offset,
                 struct fuse_file_info *fi);
    int (*truncate)(const char *path, off_t size, struct fuse_file_info *fi);
    int (*fsync)(const char *path, int datasync, struct fuse_file_info *fi);
    int (*flush)(const char *path, struct fuse_file_info *fi);

    /* File/directory metadata */
    int (*rename)(const char *from, const char *to, unsigned int flags);
    int (*chmod)(const char *path, mode_t mode, struct fuse_file_info *fi);
    int (*chown)(const char *path, uid_t uid, gid_t gid,
                 struct fuse_file_info *fi);
    int (*utimens)(const char *path, const struct timespec ts[2],
                   struct fuse_file_info *fi);

    /* Extended attributes */
    int (*getxattr)(const char *path, const char *name, char *value,
                    size_t size);
    int (*setxattr)(const char *path, const char *name, const char *value,
                    size_t size, int flags);
    int (*listxattr)(const char *path, char *list, size_t size);
};

/*
 * Get the current backend implementation
 *
 * Returns a pointer to the active backend. By default, this returns
 * the in-memory backend from badgefs_backend_mem.c
 */
struct badgefs_backend *badgefs_get_backend(void);

/*
 * Set a custom backend implementation
 *
 * Call this before mounting the filesystem to use a different storage backend.
 * Pass NULL to restore the default in-memory backend.
 */
void badgefs_set_backend(struct badgefs_backend *backend);

#endif /* BADGEFS_BACKEND_H */
