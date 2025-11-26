/*
 * badgefs_ops.h - FUSE operation callbacks declarations
 *
 * This header declares all FUSE filesystem operations and the
 * fuse_operations structure that ties them together.
 */

#ifndef BADGEFS_OPS_H
#define BADGEFS_OPS_H

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>

/*
 * Get the FUSE operations structure
 *
 * Returns a pointer to the static fuse_operations structure containing
 * all implemented filesystem callbacks.
 */
const struct fuse_operations *badgefs_get_operations(void);

/*
 * Initialize the filesystem
 *
 * Called by FUSE when the filesystem is mounted.
 * Initializes the backend storage.
 */
void *badgefs_init(struct fuse_conn_info *conn, struct fuse_config *cfg);

/*
 * Cleanup the filesystem
 *
 * Called by FUSE when the filesystem is unmounted.
 * Frees all resources.
 */
void badgefs_destroy(void *private_data);

/* File attribute operations */
int badgefs_getattr(const char *path, struct stat *st,
                    struct fuse_file_info *fi);

/* Directory operations */
int badgefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                    off_t offset, struct fuse_file_info *fi,
                    enum fuse_readdir_flags flags);
int badgefs_mkdir(const char *path, mode_t mode);
int badgefs_rmdir(const char *path);

/* File creation and deletion */
int badgefs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int badgefs_unlink(const char *path);

/* File I/O */
int badgefs_open(const char *path, struct fuse_file_info *fi);
int badgefs_release(const char *path, struct fuse_file_info *fi);
int badgefs_read(const char *path, char *buf, size_t size, off_t offset,
                 struct fuse_file_info *fi);
int badgefs_write(const char *path, const char *buf, size_t size, off_t offset,
                  struct fuse_file_info *fi);
int badgefs_truncate(const char *path, off_t size, struct fuse_file_info *fi);

/* Metadata operations */
int badgefs_rename(const char *from, const char *to, unsigned int flags);
int badgefs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
int badgefs_chown(const char *path, uid_t uid, gid_t gid,
                  struct fuse_file_info *fi);
int badgefs_utimens(const char *path, const struct timespec ts[2],
                    struct fuse_file_info *fi);

/* Extended attribute operations */
int badgefs_getxattr(const char *path, const char *name, char *value,
                     size_t size);
int badgefs_setxattr(const char *path, const char *name, const char *value,
                     size_t size, int flags);
int badgefs_listxattr(const char *path, char *list, size_t size);

#endif /* BADGEFS_OPS_H */
