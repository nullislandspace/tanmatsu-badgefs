/*
 * badgefs_backend_mem.c - In-memory storage backend implementation
 *
 * This file implements a complete in-memory filesystem using a tree structure.
 * All data is stored in RAM and lost when the filesystem is unmounted.
 *
 * Thread safety: Uses pthread_rwlock for concurrent access protection.
 */

#define FUSE_USE_VERSION 31

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "badgefs_backend.h"
#include "badgefs_backend_mem.h"

/* ============================================================================
 * Global State
 * ============================================================================ */

static struct badgefs_node *root_node = NULL;
static pthread_rwlock_t fs_lock = PTHREAD_RWLOCK_INITIALIZER;

/* Current backend pointer (for pluggable backend support) */
static struct badgefs_backend *current_backend = NULL;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Get current time for file timestamps
 */
static void get_current_time(struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
}

/*
 * Create a new node with the given name and mode
 */
static struct badgefs_node *create_node(const char *name, mode_t mode)
{
    struct badgefs_node *node = calloc(1, sizeof(struct badgefs_node));
    if (!node)
        return NULL;

    node->name = strdup(name);
    if (!node->name) {
        free(node);
        return NULL;
    }

    node->st.st_mode = mode;
    node->st.st_nlink = S_ISDIR(mode) ? 2 : 1;
    node->st.st_uid = getuid();
    node->st.st_gid = getgid();
    node->st.st_size = 0;
    node->st.st_blksize = 4096;
    node->st.st_blocks = 0;

    struct timespec now;
    get_current_time(&now);
    node->st.st_atim = now;
    node->st.st_mtim = now;
    node->st.st_ctim = now;

    return node;
}

/*
 * Free a node and all its resources (but not children)
 */
static void free_node(struct badgefs_node *node)
{
    if (!node)
        return;
    free(node->name);
    free(node->data);
    free(node);
}

/*
 * Recursively free a node and all its children
 */
static void free_node_recursive(struct badgefs_node *node)
{
    if (!node)
        return;

    struct badgefs_node *child = node->children;
    while (child) {
        struct badgefs_node *next = child->next;
        free_node_recursive(child);
        child = next;
    }

    free_node(node);
}

/*
 * Find a child node by name within a directory
 */
static struct badgefs_node *find_child(struct badgefs_node *parent,
                                       const char *name)
{
    if (!parent || !S_ISDIR(parent->st.st_mode))
        return NULL;

    struct badgefs_node *child = parent->children;
    while (child) {
        if (strcmp(child->name, name) == 0)
            return child;
        child = child->next;
    }
    return NULL;
}

/*
 * Resolve a path to a node
 * Returns NULL if path doesn't exist
 */
static struct badgefs_node *resolve_path(const char *path)
{
    if (!path || path[0] != '/')
        return NULL;

    if (strcmp(path, "/") == 0)
        return root_node;

    /* Skip leading slash and work through path components */
    struct badgefs_node *current = root_node;
    char *path_copy = strdup(path + 1);  /* Skip leading / */
    if (!path_copy)
        return NULL;

    char *saveptr;
    char *component = strtok_r(path_copy, "/", &saveptr);

    while (component && current) {
        current = find_child(current, component);
        component = strtok_r(NULL, "/", &saveptr);
    }

    free(path_copy);
    return current;
}

/*
 * Get parent directory and final component name from a path
 * Returns parent node and sets *name to point to the final component
 */
static struct badgefs_node *resolve_parent(const char *path, const char **name)
{
    if (!path || path[0] != '/' || strcmp(path, "/") == 0)
        return NULL;

    /* Find last slash */
    const char *last_slash = strrchr(path, '/');
    if (!last_slash)
        return NULL;

    *name = last_slash + 1;
    if (**name == '\0')
        return NULL;

    /* Get parent path */
    if (last_slash == path) {
        /* Parent is root */
        return root_node;
    }

    size_t parent_len = last_slash - path;
    char *parent_path = malloc(parent_len + 1);
    if (!parent_path)
        return NULL;

    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    struct badgefs_node *parent = resolve_path(parent_path);
    free(parent_path);

    if (!parent || !S_ISDIR(parent->st.st_mode))
        return NULL;

    return parent;
}

/*
 * Add a child node to a parent directory
 */
static void add_child(struct badgefs_node *parent, struct badgefs_node *child)
{
    child->parent = parent;
    child->next = parent->children;
    parent->children = child;

    /* Update parent directory timestamps */
    struct timespec now;
    get_current_time(&now);
    parent->st.st_mtim = now;
    parent->st.st_ctim = now;
}

/*
 * Remove a child node from its parent
 */
static void remove_child(struct badgefs_node *parent, struct badgefs_node *child)
{
    struct badgefs_node **pp = &parent->children;
    while (*pp) {
        if (*pp == child) {
            *pp = child->next;
            child->parent = NULL;
            child->next = NULL;

            /* Update parent directory timestamps */
            struct timespec now;
            get_current_time(&now);
            parent->st.st_mtim = now;
            parent->st.st_ctim = now;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ============================================================================
 * Backend Implementation Functions
 * ============================================================================ */

static int mem_init(void *config)
{
    (void)config;

    pthread_rwlock_wrlock(&fs_lock);

    /* Create root directory if it doesn't exist */
    if (!root_node) {
        root_node = create_node("", S_IFDIR | 0755);
        if (!root_node) {
            pthread_rwlock_unlock(&fs_lock);
            return -ENOMEM;
        }
    }

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static void mem_destroy(void)
{
    pthread_rwlock_wrlock(&fs_lock);

    if (root_node) {
        free_node_recursive(root_node);
        root_node = NULL;
    }

    pthread_rwlock_unlock(&fs_lock);
}

static int mem_lookup(const char *path, struct stat *st)
{
    pthread_rwlock_rdlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    *st = node->st;
    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)fi;
    (void)flags;

    pthread_rwlock_rdlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    if (!S_ISDIR(node->st.st_mode)) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOTDIR;
    }

    /* Add . and .. entries */
    filler(buf, ".", &node->st, 0, 0);
    if (node->parent) {
        filler(buf, "..", &node->parent->st, 0, 0);
    } else {
        filler(buf, "..", &node->st, 0, 0);  /* Root's parent is itself */
    }

    /* Add child entries */
    struct badgefs_node *child = node->children;
    while (child) {
        filler(buf, child->name, &child->st, 0, 0);
        child = child->next;
    }

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_mkdir(const char *path, mode_t mode)
{
    const char *name;

    pthread_rwlock_wrlock(&fs_lock);

    struct badgefs_node *parent = resolve_parent(path, &name);
    if (!parent) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    /* Check if name already exists */
    if (find_child(parent, name)) {
        pthread_rwlock_unlock(&fs_lock);
        return -EEXIST;
    }

    /* Create directory node */
    struct badgefs_node *dir = create_node(name, S_IFDIR | (mode & 0777));
    if (!dir) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOMEM;
    }

    add_child(parent, dir);

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_rmdir(const char *path)
{
    pthread_rwlock_wrlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    if (!S_ISDIR(node->st.st_mode)) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOTDIR;
    }

    /* Check if directory is empty */
    if (node->children) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOTEMPTY;
    }

    /* Cannot remove root */
    if (!node->parent) {
        pthread_rwlock_unlock(&fs_lock);
        return -EBUSY;
    }

    remove_child(node->parent, node);
    free_node(node);

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;
    const char *name;

    pthread_rwlock_wrlock(&fs_lock);

    struct badgefs_node *parent = resolve_parent(path, &name);
    if (!parent) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    /* Check if name already exists */
    if (find_child(parent, name)) {
        pthread_rwlock_unlock(&fs_lock);
        return -EEXIST;
    }

    /* Create file node */
    struct badgefs_node *file = create_node(name, S_IFREG | (mode & 0777));
    if (!file) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOMEM;
    }

    add_child(parent, file);

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_unlink(const char *path)
{
    pthread_rwlock_wrlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    if (S_ISDIR(node->st.st_mode)) {
        pthread_rwlock_unlock(&fs_lock);
        return -EISDIR;
    }

    if (!node->parent) {
        pthread_rwlock_unlock(&fs_lock);
        return -EBUSY;
    }

    remove_child(node->parent, node);
    free_node(node);

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_open(const char *path, struct fuse_file_info *fi)
{
    (void)fi;

    pthread_rwlock_rdlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    if (S_ISDIR(node->st.st_mode)) {
        pthread_rwlock_unlock(&fs_lock);
        return -EISDIR;
    }

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    return 0;
}

static int mem_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    (void)fi;

    pthread_rwlock_rdlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    if (S_ISDIR(node->st.st_mode)) {
        pthread_rwlock_unlock(&fs_lock);
        return -EISDIR;
    }

    /* Handle reads past end of file */
    if (offset >= (off_t)node->st.st_size) {
        pthread_rwlock_unlock(&fs_lock);
        return 0;
    }

    /* Calculate how much we can read */
    size_t available = node->st.st_size - offset;
    if (size > available)
        size = available;

    /* Copy data */
    if (node->data && size > 0) {
        memcpy(buf, node->data + offset, size);
    }

    /* Update access time */
    struct timespec now;
    get_current_time(&now);
    /* Note: We'd need write lock to update atime, skip for read-heavy workloads */

    pthread_rwlock_unlock(&fs_lock);
    return (int)size;
}

static int mem_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    (void)fi;

    pthread_rwlock_wrlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    if (S_ISDIR(node->st.st_mode)) {
        pthread_rwlock_unlock(&fs_lock);
        return -EISDIR;
    }

    /* Calculate new size needed */
    size_t new_size = offset + size;

    /* Expand buffer if needed */
    if (new_size > node->data_capacity) {
        /* Round up to 4K blocks */
        size_t new_capacity = (new_size + 4095) & ~4095UL;
        char *new_data = realloc(node->data, new_capacity);
        if (!new_data) {
            pthread_rwlock_unlock(&fs_lock);
            return -ENOSPC;
        }

        /* Zero-fill gap between old size and new write position */
        if (offset > (off_t)node->st.st_size) {
            memset(new_data + node->st.st_size, 0,
                   offset - node->st.st_size);
        }

        node->data = new_data;
        node->data_capacity = new_capacity;
    }

    /* Write data */
    memcpy(node->data + offset, buf, size);

    /* Update size if we extended the file */
    if (new_size > (size_t)node->st.st_size) {
        node->st.st_size = new_size;
        node->st.st_blocks = (new_size + 511) / 512;
    }

    /* Update timestamps */
    struct timespec now;
    get_current_time(&now);
    node->st.st_mtim = now;
    node->st.st_ctim = now;

    pthread_rwlock_unlock(&fs_lock);
    return (int)size;
}

static int mem_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi;

    pthread_rwlock_wrlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    if (S_ISDIR(node->st.st_mode)) {
        pthread_rwlock_unlock(&fs_lock);
        return -EISDIR;
    }

    if (size < 0) {
        pthread_rwlock_unlock(&fs_lock);
        return -EINVAL;
    }

    if ((size_t)size > node->data_capacity) {
        /* Need to expand */
        size_t new_capacity = (size + 4095) & ~4095UL;
        char *new_data = realloc(node->data, new_capacity);
        if (!new_data) {
            pthread_rwlock_unlock(&fs_lock);
            return -ENOSPC;
        }

        /* Zero-fill new area */
        memset(new_data + node->st.st_size, 0, size - node->st.st_size);

        node->data = new_data;
        node->data_capacity = new_capacity;
    }

    node->st.st_size = size;
    node->st.st_blocks = (size + 511) / 512;

    struct timespec now;
    get_current_time(&now);
    node->st.st_mtim = now;
    node->st.st_ctim = now;

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_rename(const char *from, const char *to, unsigned int flags)
{
    /* We don't support RENAME_EXCHANGE or RENAME_NOREPLACE for simplicity */
    if (flags)
        return -EINVAL;

    const char *new_name;

    pthread_rwlock_wrlock(&fs_lock);

    struct badgefs_node *node = resolve_path(from);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    struct badgefs_node *new_parent = resolve_parent(to, &new_name);
    if (!new_parent) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    /* Check if target already exists */
    struct badgefs_node *existing = find_child(new_parent, new_name);
    if (existing) {
        /* Remove existing target */
        if (S_ISDIR(existing->st.st_mode)) {
            if (existing->children) {
                pthread_rwlock_unlock(&fs_lock);
                return -ENOTEMPTY;
            }
        }
        remove_child(new_parent, existing);
        free_node(existing);
    }

    /* Remove from old parent */
    if (node->parent) {
        remove_child(node->parent, node);
    }

    /* Update name */
    char *old_name = node->name;
    node->name = strdup(new_name);
    if (!node->name) {
        node->name = old_name;  /* Restore on failure */
        pthread_rwlock_unlock(&fs_lock);
        return -ENOMEM;
    }
    free(old_name);

    /* Add to new parent */
    add_child(new_parent, node);

    struct timespec now;
    get_current_time(&now);
    node->st.st_ctim = now;

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;

    pthread_rwlock_wrlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    /* Preserve file type bits, only change permission bits */
    node->st.st_mode = (node->st.st_mode & S_IFMT) | (mode & ~S_IFMT);

    struct timespec now;
    get_current_time(&now);
    node->st.st_ctim = now;

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_chown(const char *path, uid_t uid, gid_t gid,
                     struct fuse_file_info *fi)
{
    (void)fi;

    pthread_rwlock_wrlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    if (uid != (uid_t)-1)
        node->st.st_uid = uid;
    if (gid != (gid_t)-1)
        node->st.st_gid = gid;

    struct timespec now;
    get_current_time(&now);
    node->st.st_ctim = now;

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

static int mem_utimens(const char *path, const struct timespec ts[2],
                       struct fuse_file_info *fi)
{
    (void)fi;

    pthread_rwlock_wrlock(&fs_lock);

    struct badgefs_node *node = resolve_path(path);
    if (!node) {
        pthread_rwlock_unlock(&fs_lock);
        return -ENOENT;
    }

    /* ts[0] = atime, ts[1] = mtime */
    if (ts) {
        if (ts[0].tv_nsec == UTIME_NOW) {
            get_current_time(&node->st.st_atim);
        } else if (ts[0].tv_nsec != UTIME_OMIT) {
            node->st.st_atim = ts[0];
        }

        if (ts[1].tv_nsec == UTIME_NOW) {
            get_current_time(&node->st.st_mtim);
        } else if (ts[1].tv_nsec != UTIME_OMIT) {
            node->st.st_mtim = ts[1];
        }
    } else {
        struct timespec now;
        get_current_time(&now);
        node->st.st_atim = now;
        node->st.st_mtim = now;
    }

    get_current_time(&node->st.st_ctim);

    pthread_rwlock_unlock(&fs_lock);
    return 0;
}

/* ============================================================================
 * Backend Structure and Accessors
 * ============================================================================ */

static struct badgefs_backend mem_backend = {
    .init     = mem_init,
    .destroy  = mem_destroy,
    .lookup   = mem_lookup,
    .readdir  = mem_readdir,
    .mkdir    = mem_mkdir,
    .rmdir    = mem_rmdir,
    .create   = mem_create,
    .unlink   = mem_unlink,
    .open     = mem_open,
    .release  = mem_release,
    .read     = mem_read,
    .write    = mem_write,
    .truncate = mem_truncate,
    .rename   = mem_rename,
    .chmod    = mem_chmod,
    .chown    = mem_chown,
    .utimens  = mem_utimens,
};

struct badgefs_backend *badgefs_backend_mem_get(void)
{
    return &mem_backend;
}

struct badgefs_backend *badgefs_get_backend(void)
{
    if (!current_backend)
        current_backend = &mem_backend;
    return current_backend;
}

void badgefs_set_backend(struct badgefs_backend *backend)
{
    if (backend)
        current_backend = backend;
    else
        current_backend = &mem_backend;
}
