/*
 * badgefs_ops.c - FUSE operation callbacks implementation
 *
 * This file implements the FUSE callbacks by delegating to the
 * configured storage backend. Each callback acquires appropriate
 * locks and forwards the request to the backend.
 */

#define FUSE_USE_VERSION 31

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "badgefs_ops.h"
#include "badgefs_backend.h"

/* ============================================================================
 * Backend Management
 * ============================================================================ */

static struct badgefs_backend *current_backend = NULL;

struct badgefs_backend *badgefs_get_backend(void)
{
    return current_backend;
}

void badgefs_set_backend(struct badgefs_backend *backend)
{
    current_backend = backend;
}

/* ============================================================================
 * FUSE Lifecycle Callbacks
 * ============================================================================ */

void *badgefs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;

    /* Enable kernel caching */
    cfg->kernel_cache = 1;

    /* Initialize the backend */
    struct badgefs_backend *backend = badgefs_get_backend();
    if (backend && backend->init) {
        int ret = backend->init(NULL);
        if (ret < 0) {
            fprintf(stderr, "badgefs: backend init failed: %s\n",
                    strerror(-ret));
            return NULL;
        }
    }

    return NULL;
}

void badgefs_destroy(void *private_data)
{
    (void)private_data;

    struct badgefs_backend *backend = badgefs_get_backend();
    if (backend && backend->destroy) {
        backend->destroy();
    }
}

/* ============================================================================
 * File Attribute Operations
 * ============================================================================ */

int badgefs_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi;

    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->lookup)
        return -ENOSYS;

    return backend->lookup(path, st);
}

/* ============================================================================
 * Directory Operations
 * ============================================================================ */

int badgefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                    off_t offset, struct fuse_file_info *fi,
                    enum fuse_readdir_flags flags)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->readdir)
        return -ENOSYS;

    return backend->readdir(path, buf, filler, offset, fi, flags);
}

int badgefs_mkdir(const char *path, mode_t mode)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->mkdir)
        return -ENOSYS;

    return backend->mkdir(path, mode);
}

int badgefs_rmdir(const char *path)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->rmdir)
        return -ENOSYS;

    return backend->rmdir(path);
}

/* ============================================================================
 * File Creation and Deletion
 * ============================================================================ */

int badgefs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->create)
        return -ENOSYS;

    return backend->create(path, mode, fi);
}

int badgefs_unlink(const char *path)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->unlink)
        return -ENOSYS;

    return backend->unlink(path);
}

/* ============================================================================
 * File I/O Operations
 * ============================================================================ */

int badgefs_open(const char *path, struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->open)
        return -ENOSYS;

    return backend->open(path, fi);
}

int badgefs_release(const char *path, struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->release)
        return 0;  /* release is optional */

    return backend->release(path, fi);
}

int badgefs_read(const char *path, char *buf, size_t size, off_t offset,
                 struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->read)
        return -ENOSYS;

    return backend->read(path, buf, size, offset, fi);
}

int badgefs_write(const char *path, const char *buf, size_t size, off_t offset,
                  struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->write)
        return -ENOSYS;

    return backend->write(path, buf, size, offset, fi);
}

int badgefs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->truncate)
        return -ENOSYS;

    return backend->truncate(path, size, fi);
}

int badgefs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->fsync)
        return 0;  /* fsync is optional */

    return backend->fsync(path, datasync, fi);
}

int badgefs_flush(const char *path, struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->flush)
        return 0;  /* flush is optional */

    return backend->flush(path, fi);
}

/* ============================================================================
 * Metadata Operations
 * ============================================================================ */

int badgefs_rename(const char *from, const char *to, unsigned int flags)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->rename)
        return -ENOSYS;

    return backend->rename(from, to, flags);
}

int badgefs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->chmod)
        return -ENOSYS;

    return backend->chmod(path, mode, fi);
}

int badgefs_chown(const char *path, uid_t uid, gid_t gid,
                  struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->chown)
        return -ENOSYS;

    return backend->chown(path, uid, gid, fi);
}

int badgefs_utimens(const char *path, const struct timespec ts[2],
                    struct fuse_file_info *fi)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->utimens)
        return -ENOSYS;

    return backend->utimens(path, ts, fi);
}

/* ============================================================================
 * Extended Attribute Operations
 * ============================================================================ */

int badgefs_getxattr(const char *path, const char *name, char *value,
                     size_t size)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->getxattr)
        return -ENODATA;

    return backend->getxattr(path, name, value, size);
}

int badgefs_setxattr(const char *path, const char *name, const char *value,
                     size_t size, int flags)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->setxattr)
        return -ENOSYS;

    return backend->setxattr(path, name, value, size, flags);
}

int badgefs_listxattr(const char *path, char *list, size_t size)
{
    struct badgefs_backend *backend = badgefs_get_backend();
    if (!backend || !backend->listxattr)
        return 0;  /* No attributes */

    return backend->listxattr(path, list, size);
}

/* ============================================================================
 * FUSE Operations Structure
 * ============================================================================ */

static const struct fuse_operations badgefs_ops = {
    .init       = badgefs_init,
    .destroy    = badgefs_destroy,
    .getattr    = badgefs_getattr,
    .readdir    = badgefs_readdir,
    .mkdir      = badgefs_mkdir,
    .rmdir      = badgefs_rmdir,
    .create     = badgefs_create,
    .unlink     = badgefs_unlink,
    .open       = badgefs_open,
    .release    = badgefs_release,
    .read       = badgefs_read,
    .write      = badgefs_write,
    .truncate   = badgefs_truncate,
    .fsync      = badgefs_fsync,
    .flush      = badgefs_flush,
    .rename     = badgefs_rename,
    .chmod      = badgefs_chmod,
    .chown      = badgefs_chown,
    .utimens    = badgefs_utimens,
    .getxattr   = badgefs_getxattr,
    .setxattr   = badgefs_setxattr,
    .listxattr  = badgefs_listxattr,
};

const struct fuse_operations *badgefs_get_operations(void)
{
    return &badgefs_ops;
}
