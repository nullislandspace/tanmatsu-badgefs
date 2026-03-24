/*
 * badgefs_backend_badgelink.c - BadgeLink backend for BadgeFS
 *
 * This backend provides access to a Tanmatsu badge filesystem
 * via the BadgeLink USB protocol.
 *
 * Features:
 * - Path routing to /sd, /int, /appfs virtual directories
 * - Write buffering (files are buffered in memory, uploaded on close)
 * - Thread-safe with mutex protection
 */

#define FUSE_USE_VERSION 31

#include "badgefs_backend_badgelink.h"
#include "badgelink_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fuse3/fuse.h>

/* Path types */
enum path_type {
    PATH_ROOT,      /* / */
    PATH_SD,        /* /sd or /sd/... */
    PATH_INT,       /* /int or /int/... */
    PATH_APPFS,     /* /appfs or /appfs/... */
    PATH_INVALID
};

/* Open file entry for write buffering */
struct open_file {
    char *path;             /* FUSE path */
    uint8_t *buffer;        /* Write buffer */
    size_t size;            /* Current data size */
    size_t capacity;        /* Buffer capacity */
    bool modified;          /* Has been written to */
    bool is_appfs;          /* Is an AppFS file */
    char slug[48];          /* AppFS slug (if is_appfs) */
    /* Pending xattr for appfs (set via setxattr, applied on release) */
    char pending_title[64]; /* Pending title (empty = not set) */
    int pending_version;    /* Pending version (-1 = not set) */
    bool has_pending_title;
    bool has_pending_version;
    struct open_file *next;
};

/* Backend state */
static struct {
    struct badgelink_client client;
    pthread_mutex_t lock;
    struct open_file *open_files;
    bool initialized;
    bool force_v1;  /* Force protocol version 1 (set before init) */
} state;

/* ============================================================================
 * Path Parsing Utilities
 * ============================================================================ */

/*
 * Determine path type from FUSE path.
 */
static enum path_type get_path_type(const char *path)
{
    if (!path || path[0] != '/')
        return PATH_INVALID;

    if (strcmp(path, "/") == 0)
        return PATH_ROOT;

    if (strcmp(path, "/sd") == 0 || strncmp(path, "/sd/", 4) == 0)
        return PATH_SD;

    if (strcmp(path, "/int") == 0 || strncmp(path, "/int/", 5) == 0)
        return PATH_INT;

    if (strcmp(path, "/appfs") == 0 || strncmp(path, "/appfs/", 7) == 0)
        return PATH_APPFS;

    return PATH_INVALID;
}

/*
 * Translate FUSE path to device path.
 * Returns pointer to device path (static or within fuse_path).
 */
static const char *translate_path(const char *fuse_path)
{
    enum path_type type = get_path_type(fuse_path);

    switch (type) {
    case PATH_SD:
        /* /sd -> /sd, /sd/foo -> /sd/foo */
        return fuse_path;

    case PATH_INT:
        /* /int -> /int, /int/foo -> /int/foo */
        return fuse_path;

    default:
        return NULL;
    }
}

/*
 * Extract app slug from /appfs/<slug>.bin path.
 * Copies slug to provided buffer.
 * Returns 0 on success, negative error on failure.
 */
static int extract_app_slug(const char *fuse_path, char *slug, size_t slug_size)
{
    if (!fuse_path || !slug || slug_size == 0)
        return -EINVAL;

    /* Must be /appfs/<something>.bin */
    if (strncmp(fuse_path, "/appfs/", 7) != 0)
        return -EINVAL;

    const char *name = fuse_path + 7;
    size_t name_len = strlen(name);

    /* Must end in .bin */
    if (name_len < 5 || strcmp(name + name_len - 4, ".bin") != 0)
        return -EINVAL;

    /* Extract slug (name without .bin extension) */
    size_t slug_len = name_len - 4;
    if (slug_len >= slug_size)
        return -ENAMETOOLONG;

    memcpy(slug, name, slug_len);
    slug[slug_len] = '\0';

    return 0;
}

/* ============================================================================
 * Open File Management
 * ============================================================================ */

/*
 * Find open file by path.
 * Must be called with lock held.
 */
static struct open_file *find_open_file(const char *path)
{
    for (struct open_file *f = state.open_files; f; f = f->next) {
        if (strcmp(f->path, path) == 0)
            return f;
    }
    return NULL;
}

/*
 * Create new open file entry.
 * Must be called with lock held.
 */
static struct open_file *create_open_file(const char *path)
{
    struct open_file *f = calloc(1, sizeof(struct open_file));
    if (!f)
        return NULL;

    f->path = strdup(path);
    if (!f->path) {
        free(f);
        return NULL;
    }

    /* Initialize pending xattr */
    f->pending_version = -1;
    f->has_pending_title = false;
    f->has_pending_version = false;

    /* Check if it's an AppFS file */
    if (strncmp(path, "/appfs/", 7) == 0) {
        f->is_appfs = true;
        if (extract_app_slug(path, f->slug, sizeof(f->slug)) < 0) {
            free(f->path);
            free(f);
            return NULL;
        }
    }

    /* Add to list */
    f->next = state.open_files;
    state.open_files = f;

    return f;
}

/*
 * Remove open file entry.
 * Must be called with lock held.
 */
static void remove_open_file(struct open_file *f)
{
    struct open_file **pp = &state.open_files;
    while (*pp) {
        if (*pp == f) {
            *pp = f->next;
            free(f->path);
            free(f->buffer);
            free(f);
            return;
        }
        pp = &(*pp)->next;
    }
}

/*
 * Ensure open file has write buffer with at least min_size capacity.
 */
static int ensure_buffer(struct open_file *f, size_t min_size)
{
    if (f->capacity >= min_size)
        return 0;

    /* Round up to 4K */
    size_t new_cap = (min_size + 4095) & ~4095UL;
    uint8_t *new_buf = realloc(f->buffer, new_cap);
    if (!new_buf)
        return -ENOMEM;

    f->buffer = new_buf;
    f->capacity = new_cap;
    return 0;
}

/* ============================================================================
 * Backend Callbacks
 * ============================================================================ */

static int bl_init(void *config)
{
    (void)config;

    if (state.initialized)
        return 0;

    pthread_mutex_init(&state.lock, NULL);

    int ret = badgelink_client_init(&state.client);
    if (ret < 0) {
        fprintf(stderr, "badgefs_badgelink: failed to init client: %s\n",
                strerror(-ret));
        return ret;
    }

    /* Apply force_v1 flag if set (before connect) */
    if (state.force_v1) {
        badgelink_client_force_v1(&state.client);
    }

    ret = badgelink_client_connect(&state.client);
    if (ret < 0) {
        fprintf(stderr, "badgefs_badgelink: failed to connect: %s\n",
                strerror(-ret));
        badgelink_client_cleanup(&state.client);
        return ret;
    }

    state.initialized = true;
    printf("badgefs_badgelink: connected to badge (protocol v%u)\n",
           badgelink_client_get_protocol_version(&state.client));
    return 0;
}

static void bl_destroy(void)
{
    if (!state.initialized)
        return;

    pthread_mutex_lock(&state.lock);

    /* Clean up open files */
    while (state.open_files) {
        struct open_file *f = state.open_files;
        state.open_files = f->next;
        free(f->path);
        free(f->buffer);
        free(f);
    }

    badgelink_client_disconnect(&state.client);
    badgelink_client_cleanup(&state.client);

    pthread_mutex_unlock(&state.lock);
    pthread_mutex_destroy(&state.lock);

    state.initialized = false;
    printf("badgefs_badgelink: disconnected\n");
}

static int bl_lookup(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));

    enum path_type type = get_path_type(path);

    switch (type) {
    case PATH_ROOT:
        /* Virtual root directory */
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 5;  /* . + .. + sd + int + appfs */
        st->st_uid = getuid();
        st->st_gid = getgid();
        return 0;

    case PATH_SD:
    case PATH_INT:
        pthread_mutex_lock(&state.lock);
        {
            /*
             * Check if file is open AND has been modified (written to).
             * Only use in-memory state for files we've created/modified.
             * For files opened read-only, we must query the badge for size.
             */
            struct open_file *f = find_open_file(path);
            if (f && f->modified) {
                /* Return info from our in-memory state */
                st->st_mode = S_IFREG | 0644;
                st->st_nlink = 1;
                st->st_size = f->size;
                st->st_mtime = time(NULL);
                st->st_ctime = time(NULL);
                st->st_atime = time(NULL);
                st->st_uid = getuid();
                st->st_gid = getgid();
                pthread_mutex_unlock(&state.lock);
                return 0;
            }

            const char *device_path = translate_path(path);
            struct badgelink_stat bl_st;
            int ret = badgelink_fs_stat(&state.client, device_path, &bl_st);
            pthread_mutex_unlock(&state.lock);

            if (ret < 0)
                return ret;

            st->st_mode = bl_st.is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
            st->st_nlink = bl_st.is_dir ? 2 : 1;
            st->st_size = bl_st.size;
            st->st_mtime = bl_st.mtime / 1000;
            st->st_ctime = bl_st.ctime / 1000;
            st->st_atime = bl_st.atime / 1000;
            st->st_uid = getuid();
            st->st_gid = getgid();
        }
        return 0;

    case PATH_APPFS:
        if (strcmp(path, "/appfs") == 0) {
            /* AppFS root directory */
            st->st_mode = S_IFDIR | 0755;
            st->st_nlink = 2;
            st->st_uid = getuid();
            st->st_gid = getgid();
            return 0;
        } else {
            /* AppFS file: /appfs/<slug>.bin */
            char slug[48];
            int ret = extract_app_slug(path, slug, sizeof(slug));
            if (ret < 0)
                return -ENOENT;

            pthread_mutex_lock(&state.lock);

            /* Check if file is open (newly created or being written) */
            struct open_file *f = find_open_file(path);
            if (f && f->modified) {
                /* Return info from our in-memory state */
                st->st_mode = S_IFREG | 0644;
                st->st_nlink = 1;
                st->st_size = f->size;
                st->st_mtime = time(NULL);
                st->st_ctime = time(NULL);
                st->st_atime = time(NULL);
                st->st_uid = getuid();
                st->st_gid = getgid();
                pthread_mutex_unlock(&state.lock);
                return 0;
            }

            struct badgelink_app app;
            ret = badgelink_appfs_stat(&state.client, slug, &app);
            pthread_mutex_unlock(&state.lock);

            if (ret < 0)
                return ret;

            st->st_mode = S_IFREG | 0644;
            st->st_nlink = 1;
            st->st_size = app.size;
            st->st_mtime = time(NULL);
            st->st_ctime = time(NULL);
            st->st_atime = time(NULL);
            st->st_uid = getuid();
            st->st_gid = getgid();
        }
        return 0;

    default:
        return -ENOENT;
    }
}

static int bl_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)fi;
    (void)flags;

    enum path_type type = get_path_type(path);

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    switch (type) {
    case PATH_ROOT:
        filler(buf, "sd", NULL, 0, 0);
        filler(buf, "int", NULL, 0, 0);
        filler(buf, "appfs", NULL, 0, 0);
        return 0;

    case PATH_SD:
    case PATH_INT:
        pthread_mutex_lock(&state.lock);
        {
            const char *device_path = translate_path(path);
            struct badgelink_dirent *entries = NULL;
            size_t count = 0;
            int ret = badgelink_fs_list(&state.client, device_path,
                                        &entries, &count);
            pthread_mutex_unlock(&state.lock);

            if (ret < 0)
                return ret;

            for (size_t i = 0; i < count; i++) {
                filler(buf, entries[i].name, NULL, 0, 0);
            }
            free(entries);
        }
        return 0;

    case PATH_APPFS:
        if (strcmp(path, "/appfs") != 0)
            return -ENOTDIR;

        pthread_mutex_lock(&state.lock);
        {
            struct badgelink_app *apps = NULL;
            size_t count = 0;
            int ret = badgelink_appfs_list(&state.client, &apps, &count);
            pthread_mutex_unlock(&state.lock);

            if (ret < 0)
                return ret;

            for (size_t i = 0; i < count; i++) {
                char filename[64];
                snprintf(filename, sizeof(filename), "%s.bin", apps[i].slug);
                filler(buf, filename, NULL, 0, 0);
            }
            free(apps);
        }
        return 0;

    default:
        return -ENOENT;
    }
}

static int bl_mkdir(const char *path, mode_t mode)
{
    (void)mode;

    enum path_type type = get_path_type(path);

    switch (type) {
    case PATH_SD:
    case PATH_INT:
        pthread_mutex_lock(&state.lock);
        {
            const char *device_path = translate_path(path);
            int ret = badgelink_fs_mkdir(&state.client, device_path);
            pthread_mutex_unlock(&state.lock);
            return ret;
        }

    case PATH_APPFS:
        return -EPERM;  /* Can't create directories in appfs */

    default:
        return -ENOENT;
    }
}

static int bl_rmdir(const char *path)
{
    enum path_type type = get_path_type(path);

    switch (type) {
    case PATH_SD:
    case PATH_INT:
        pthread_mutex_lock(&state.lock);
        {
            const char *device_path = translate_path(path);
            int ret = badgelink_fs_rmdir(&state.client, device_path);
            pthread_mutex_unlock(&state.lock);
            return ret;
        }

    case PATH_APPFS:
        return -EPERM;

    default:
        return -ENOENT;
    }
}

static int bl_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)mode;
    (void)fi;

    enum path_type type = get_path_type(path);

    pthread_mutex_lock(&state.lock);

    /* Check if already open */
    struct open_file *f = find_open_file(path);
    if (f) {
        pthread_mutex_unlock(&state.lock);
        return -EEXIST;
    }

    switch (type) {
    case PATH_SD:
    case PATH_INT:
    case PATH_APPFS:
        /* Create empty open file entry */
        f = create_open_file(path);
        if (!f) {
            pthread_mutex_unlock(&state.lock);
            return -ENOMEM;
        }
        f->modified = true;  /* New file, will need upload */
        pthread_mutex_unlock(&state.lock);
        return 0;

    default:
        pthread_mutex_unlock(&state.lock);
        return -ENOENT;
    }
}

static int bl_unlink(const char *path)
{
    enum path_type type = get_path_type(path);

    switch (type) {
    case PATH_SD:
    case PATH_INT:
        pthread_mutex_lock(&state.lock);
        {
            const char *device_path = translate_path(path);
            int ret = badgelink_fs_delete(&state.client, device_path);
            pthread_mutex_unlock(&state.lock);
            return ret;
        }

    case PATH_APPFS:
        {
            char slug[48];
            int ret = extract_app_slug(path, slug, sizeof(slug));
            if (ret < 0)
                return -ENOENT;

            pthread_mutex_lock(&state.lock);
            ret = badgelink_appfs_delete(&state.client, slug);
            pthread_mutex_unlock(&state.lock);
            return ret;
        }

    default:
        return -ENOENT;
    }
}

static int bl_open(const char *path, struct fuse_file_info *fi)
{
    enum path_type type = get_path_type(path);

    if (type == PATH_ROOT || type == PATH_INVALID)
        return -ENOENT;

    pthread_mutex_lock(&state.lock);

    /* Check if already open */
    struct open_file *f = find_open_file(path);
    if (!f) {
        /* Create new open file entry */
        f = create_open_file(path);
        if (!f) {
            pthread_mutex_unlock(&state.lock);
            return -ENOMEM;
        }

        /* Check for O_TRUNC - if set, skip download and start with empty buffer */
        if (fi->flags & O_TRUNC) {
            f->modified = true;  /* Will need upload on close */
        } else {
            /* Download existing content (for both read and write) */
            /* This caches the file to avoid repeated downloads during read */
            uint8_t *data = NULL;
            size_t size = 0;
            int ret = 0;

            if (f->is_appfs) {
                ret = badgelink_appfs_download(&state.client, f->slug,
                                               &data, &size);
            } else {
                const char *device_path = translate_path(path);
                ret = badgelink_fs_download(&state.client, device_path,
                                            &data, &size);
            }

            if (ret < 0 && ret != -ENOENT) {
                remove_open_file(f);
                pthread_mutex_unlock(&state.lock);
                return ret;
            }

            if (ret == 0 && data) {
                f->buffer = data;
                f->size = size;
                f->capacity = size;
            }
        }
    }

    pthread_mutex_unlock(&state.lock);
    return 0;
}

/*
 * Helper: flush pending writes to device without removing from open list.
 * Must be called with lock held.
 * Returns 0 on success, negative error code on failure.
 */
static int flush_open_file(struct open_file *f, const char *path)
{
    if (!f->modified)
        return 0;

    int ret = 0;

    if (f->is_appfs) {
        struct badgelink_app app;
        memset(&app, 0, sizeof(app));
        strncpy(app.slug, f->slug, sizeof(app.slug) - 1);

        /* Check if app already exists to preserve title/version */
        struct badgelink_app existing;
        ret = badgelink_appfs_stat(&state.client, f->slug, &existing);
        if (ret == 0) {
            strncpy(app.title, existing.title, sizeof(app.title) - 1);
            app.version = existing.version;
        } else {
            strncpy(app.title, "Application", sizeof(app.title) - 1);
            app.version = 0;
        }

        /* Override with pending xattr if set */
        if (f->has_pending_title) {
            strncpy(app.title, f->pending_title, sizeof(app.title) - 1);
        }
        if (f->has_pending_version) {
            app.version = f->pending_version;
        }
        app.size = f->size;

        ret = badgelink_appfs_upload(&state.client, &app, f->buffer, f->size);
    } else {
        const char *device_path = translate_path(path);
        ret = badgelink_fs_upload(&state.client, device_path, f->buffer, f->size);
    }

    if (ret == 0) {
        /* Mark as not modified after successful upload */
        f->modified = false;
    }

    return ret;
}

static int bl_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)datasync;
    (void)fi;

    pthread_mutex_lock(&state.lock);

    struct open_file *f = find_open_file(path);
    if (!f) {
        pthread_mutex_unlock(&state.lock);
        return 0;  /* No open file to sync */
    }

    int ret = flush_open_file(f, path);
    if (ret < 0) {
        fprintf(stderr, "badgefs_badgelink: fsync failed for %s: %s\n",
                path, strerror(-ret));
    }

    pthread_mutex_unlock(&state.lock);
    return ret;
}

static int bl_flush(const char *path, struct fuse_file_info *fi)
{
    (void)fi;

    pthread_mutex_lock(&state.lock);

    struct open_file *f = find_open_file(path);
    if (!f) {
        pthread_mutex_unlock(&state.lock);
        return 0;  /* No open file to flush */
    }

    int ret = flush_open_file(f, path);
    if (ret < 0) {
        fprintf(stderr, "badgefs_badgelink: flush failed for %s: %s\n",
                path, strerror(-ret));
    }

    pthread_mutex_unlock(&state.lock);
    return ret;
}

static int bl_release(const char *path, struct fuse_file_info *fi)
{
    (void)fi;

    pthread_mutex_lock(&state.lock);

    struct open_file *f = find_open_file(path);
    if (!f) {
        pthread_mutex_unlock(&state.lock);
        return 0;
    }

    /* If modified, upload to device */
    if (f->modified) {
        int ret = 0;

        if (f->is_appfs) {
            /* AppFS upload needs metadata */
            struct badgelink_app app;
            memset(&app, 0, sizeof(app));
            strncpy(app.slug, f->slug, sizeof(app.slug) - 1);

            /* Check if app already exists to preserve title/version */
            struct badgelink_app existing;
            ret = badgelink_appfs_stat(&state.client, f->slug, &existing);
            if (ret == 0) {
                /* App exists - keep existing title/version */
                strncpy(app.title, existing.title, sizeof(app.title) - 1);
                app.version = existing.version;
            } else {
                /* New app - use defaults */
                strncpy(app.title, "Application", sizeof(app.title) - 1);
                app.version = 0;
            }

            /* Override with pending xattr if set */
            if (f->has_pending_title) {
                strncpy(app.title, f->pending_title, sizeof(app.title) - 1);
            }
            if (f->has_pending_version) {
                app.version = f->pending_version;
            }

            app.size = f->size;

            ret = badgelink_appfs_upload(&state.client, &app,
                                         f->buffer, f->size);
        } else {
            const char *device_path = translate_path(path);
            ret = badgelink_fs_upload(&state.client, device_path,
                                      f->buffer, f->size);
        }

        if (ret < 0) {
            fprintf(stderr, "badgefs: upload failed for %s: %s\n",
                    path, strerror(-ret));
            /* Still need to clean up the open file entry */
            remove_open_file(f);
            pthread_mutex_unlock(&state.lock);
            return ret;
        }
    }

    remove_open_file(f);
    pthread_mutex_unlock(&state.lock);
    return 0;
}

static int bl_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    (void)fi;

    enum path_type type = get_path_type(path);

    if (type == PATH_ROOT || type == PATH_INVALID)
        return -ENOENT;

    pthread_mutex_lock(&state.lock);

    /* Check if file is open with buffer */
    struct open_file *f = find_open_file(path);
    if (f && f->buffer) {
        /* Read from buffer */
        if ((size_t)offset >= f->size) {
            pthread_mutex_unlock(&state.lock);
            return 0;
        }

        size_t avail = f->size - offset;
        if (size > avail)
            size = avail;

        memcpy(buf, f->buffer + offset, size);
        pthread_mutex_unlock(&state.lock);
        return size;
    }

    /* Download file if not buffered */
    uint8_t *data = NULL;
    size_t file_size = 0;
    int ret = 0;

    if (type == PATH_APPFS) {
        char slug[48];
        ret = extract_app_slug(path, slug, sizeof(slug));
        if (ret < 0) {
            pthread_mutex_unlock(&state.lock);
            return -ENOENT;
        }
        ret = badgelink_appfs_download(&state.client, slug, &data, &file_size);
    } else {
        const char *device_path = translate_path(path);
        ret = badgelink_fs_download(&state.client, device_path, &data, &file_size);
    }

    pthread_mutex_unlock(&state.lock);

    if (ret < 0)
        return ret;

    /* Read from downloaded data */
    if ((size_t)offset >= file_size) {
        free(data);
        return 0;
    }

    size_t avail = file_size - offset;
    if (size > avail)
        size = avail;

    memcpy(buf, data + offset, size);
    free(data);

    return size;
}

static int bl_write(const char *path, const char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    (void)fi;

    enum path_type type = get_path_type(path);

    if (type == PATH_ROOT || type == PATH_INVALID)
        return -ENOENT;

    pthread_mutex_lock(&state.lock);

    struct open_file *f = find_open_file(path);
    if (!f) {
        pthread_mutex_unlock(&state.lock);
        return -EBADF;
    }

    /* Ensure buffer has enough space */
    size_t needed = offset + size;
    int ret = ensure_buffer(f, needed);
    if (ret < 0) {
        pthread_mutex_unlock(&state.lock);
        return ret;
    }

    /* Zero-fill gap if writing past end */
    if ((size_t)offset > f->size) {
        memset(f->buffer + f->size, 0, offset - f->size);
    }

    /* Copy data */
    memcpy(f->buffer + offset, buf, size);

    /* Update size */
    if (needed > f->size)
        f->size = needed;

    f->modified = true;

    pthread_mutex_unlock(&state.lock);
    return size;
}

static int bl_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi;

    enum path_type type = get_path_type(path);

    if (type == PATH_ROOT || type == PATH_INVALID)
        return -ENOENT;

    pthread_mutex_lock(&state.lock);

    struct open_file *f = find_open_file(path);
    if (!f) {
        /* Create open file entry for truncate */
        f = create_open_file(path);
        if (!f) {
            pthread_mutex_unlock(&state.lock);
            return -ENOMEM;
        }
    }

    /* Resize buffer */
    if ((size_t)size > f->capacity) {
        int ret = ensure_buffer(f, size);
        if (ret < 0) {
            pthread_mutex_unlock(&state.lock);
            return ret;
        }
    }

    /* Zero-fill if growing */
    if ((size_t)size > f->size) {
        memset(f->buffer + f->size, 0, size - f->size);
    }

    f->size = size;
    f->modified = true;

    pthread_mutex_unlock(&state.lock);
    return 0;
}

static int bl_rename(const char *from, const char *to, unsigned int flags)
{
    if (flags != 0)
        return -EINVAL;

    /* Translate FUSE paths to device paths */
    const char *badge_from = translate_path(from);
    const char *badge_to = translate_path(to);
    if (!badge_from || !badge_to)
        return -EINVAL;

    pthread_mutex_lock(&state.lock);

    if (state.client.protocol_version >= BADGELINK_PROTOCOL_V3) {
        fprintf(stderr, "badgefs: rename %s -> %s\n", badge_from, badge_to);
        int ret = badgelink_fs_rename(&state.client, badge_from, badge_to);
        pthread_mutex_unlock(&state.lock);
        if (ret < 0)
            fprintf(stderr, "badgefs: rename failed: %s\n", strerror(-ret));
        else
            fprintf(stderr, "badgefs: rename complete\n");
        return ret;
    }

    /* V2 fallback: return -ENOSYS, FUSE handles via read+write+unlink */
    pthread_mutex_unlock(&state.lock);
    return -ENOSYS;
}

static int bl_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    /* FAT filesystem doesn't support Unix permissions */
    (void)path;
    (void)mode;
    (void)fi;
    return 0;  /* Silently succeed */
}

static int bl_chown(const char *path, uid_t uid, gid_t gid,
                    struct fuse_file_info *fi)
{
    /* FAT filesystem doesn't support Unix ownership */
    (void)path;
    (void)uid;
    (void)gid;
    (void)fi;
    return 0;  /* Silently succeed */
}

static int bl_utimens(const char *path, const struct timespec ts[2],
                      struct fuse_file_info *fi)
{
    /* Could potentially set mtime on device, but not critical */
    (void)path;
    (void)ts;
    (void)fi;
    return 0;  /* Silently succeed */
}

/* ============================================================================
 * Extended Attribute Operations
 * ============================================================================ */

/* AppFS xattr names */
#define XATTR_APPFS_TITLE   "user.appfs.title"
#define XATTR_APPFS_VERSION "user.appfs.version"

static int bl_getxattr(const char *path, const char *name, char *value,
                       size_t size)
{
    enum path_type type = get_path_type(path);

    /* Only appfs files have extended attributes */
    if (type != PATH_APPFS || strcmp(path, "/appfs") == 0)
        return -ENODATA;

    char slug[48];
    if (extract_app_slug(path, slug, sizeof(slug)) < 0)
        return -ENOENT;

    pthread_mutex_lock(&state.lock);

    /* Check if file is open with pending xattr */
    struct open_file *f = find_open_file(path);
    if (f) {
        if (strcmp(name, XATTR_APPFS_TITLE) == 0 && f->has_pending_title) {
            size_t len = strlen(f->pending_title);
            if (size == 0) {
                pthread_mutex_unlock(&state.lock);
                return len;
            }
            if (size < len) {
                pthread_mutex_unlock(&state.lock);
                return -ERANGE;
            }
            memcpy(value, f->pending_title, len);
            pthread_mutex_unlock(&state.lock);
            return len;
        }
        if (strcmp(name, XATTR_APPFS_VERSION) == 0 && f->has_pending_version) {
            char ver_str[16];
            int len = snprintf(ver_str, sizeof(ver_str), "%d", f->pending_version);
            if (size == 0) {
                pthread_mutex_unlock(&state.lock);
                return len;
            }
            if (size < (size_t)len) {
                pthread_mutex_unlock(&state.lock);
                return -ERANGE;
            }
            memcpy(value, ver_str, len);
            pthread_mutex_unlock(&state.lock);
            return len;
        }
    }

    /* Get current values from badge */
    struct badgelink_app app;
    int ret = badgelink_appfs_stat(&state.client, slug, &app);
    pthread_mutex_unlock(&state.lock);

    if (ret < 0)
        return -ENODATA;

    if (strcmp(name, XATTR_APPFS_TITLE) == 0) {
        size_t len = strlen(app.title);
        if (size == 0)
            return len;
        if (size < len)
            return -ERANGE;
        memcpy(value, app.title, len);
        return len;
    }
    if (strcmp(name, XATTR_APPFS_VERSION) == 0) {
        char ver_str[16];
        int len = snprintf(ver_str, sizeof(ver_str), "%d", app.version);
        if (size == 0)
            return len;
        if (size < (size_t)len)
            return -ERANGE;
        memcpy(value, ver_str, len);
        return len;
    }

    return -ENODATA;
}

static int bl_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags)
{
    (void)flags;

    enum path_type type = get_path_type(path);

    /* Only appfs files support extended attributes */
    if (type != PATH_APPFS || strcmp(path, "/appfs") == 0)
        return -ENOTSUP;

    char slug[48];
    if (extract_app_slug(path, slug, sizeof(slug)) < 0)
        return -ENOENT;

    pthread_mutex_lock(&state.lock);

    /* Find or create open_file entry */
    struct open_file *f = find_open_file(path);
    if (!f) {
        /* Create entry for xattr change */
        f = create_open_file(path);
        if (!f) {
            pthread_mutex_unlock(&state.lock);
            return -ENOMEM;
        }

        /* Download existing content so we don't lose it when uploading with new xattr */
        uint8_t *data = NULL;
        size_t file_size = 0;
        int ret = badgelink_appfs_download(&state.client, f->slug, &data, &file_size);
        if (ret == 0 && data) {
            f->buffer = data;
            f->size = file_size;
            f->capacity = file_size;
        } else if (ret != -ENOENT) {
            /* Download failed (not just file-not-found) */
            remove_open_file(f);
            pthread_mutex_unlock(&state.lock);
            return ret;
        }
        /* If ENOENT, file is new - that's fine, buffer stays empty */
    }

    if (strcmp(name, XATTR_APPFS_TITLE) == 0) {
        if (size >= sizeof(f->pending_title)) {
            pthread_mutex_unlock(&state.lock);
            return -ERANGE;
        }
        memcpy(f->pending_title, value, size);
        f->pending_title[size] = '\0';
        f->has_pending_title = true;
    } else if (strcmp(name, XATTR_APPFS_VERSION) == 0) {
        /* Parse integer from value */
        char ver_str[16];
        if (size >= sizeof(ver_str)) {
            pthread_mutex_unlock(&state.lock);
            return -ERANGE;
        }
        memcpy(ver_str, value, size);
        ver_str[size] = '\0';
        f->pending_version = atoi(ver_str);
        f->has_pending_version = true;
    } else {
        pthread_mutex_unlock(&state.lock);
        return -ENOTSUP;
    }

    /* Immediately upload to badge with new attributes */
    struct badgelink_app app;
    memset(&app, 0, sizeof(app));
    strncpy(app.slug, f->slug, sizeof(app.slug) - 1);

    /* Get existing metadata */
    struct badgelink_app existing;
    int stat_ret = badgelink_appfs_stat(&state.client, f->slug, &existing);
    if (stat_ret == 0) {
        strncpy(app.title, existing.title, sizeof(app.title) - 1);
        app.version = existing.version;
    } else {
        strncpy(app.title, "Application", sizeof(app.title) - 1);
        app.version = 0;
    }

    /* Apply pending xattr values */
    if (f->has_pending_title) {
        strncpy(app.title, f->pending_title, sizeof(app.title) - 1);
    }
    if (f->has_pending_version) {
        app.version = f->pending_version;
    }
    app.size = f->size;

    int ret = badgelink_appfs_upload(&state.client, &app, f->buffer, f->size);
    if (ret < 0) {
        fprintf(stderr, "badgefs: setxattr upload failed for %s: %s\n",
                path, strerror(-ret));
        pthread_mutex_unlock(&state.lock);
        return ret;
    }

    /* Clean up the open_file entry */
    remove_open_file(f);
    pthread_mutex_unlock(&state.lock);
    return 0;
}

static int bl_listxattr(const char *path, char *list, size_t size)
{
    enum path_type type = get_path_type(path);

    /* Only appfs files have extended attributes */
    if (type != PATH_APPFS || strcmp(path, "/appfs") == 0)
        return 0;

    char slug[48];
    if (extract_app_slug(path, slug, sizeof(slug)) < 0)
        return 0;

    /* List of supported xattrs: name\0name\0 */
    const char *attrs = XATTR_APPFS_TITLE "\0" XATTR_APPFS_VERSION "\0";
    size_t total_len = strlen(XATTR_APPFS_TITLE) + 1 + strlen(XATTR_APPFS_VERSION) + 1;

    if (size == 0)
        return total_len;

    if (size < total_len)
        return -ERANGE;

    memcpy(list, attrs, total_len);
    return total_len;
}

/* ============================================================================
 * Pre-mount Connection Test
 * ============================================================================ */

int badgefs_backend_badgelink_test_connection(void)
{
    struct badgelink_client test_client;

    int ret = badgelink_client_init(&test_client);
    if (ret < 0) {
        fprintf(stderr, "badgefs: failed to init USB: %s\n", strerror(-ret));
        return ret;
    }

    ret = badgelink_client_connect(&test_client);
    if (ret < 0) {
        fprintf(stderr, "badgefs: badge not found (is it connected?)\n");
        badgelink_client_cleanup(&test_client);
        return ret;
    }

    /* Connection successful - disconnect and let bl_init handle the real connection */
    badgelink_client_disconnect(&test_client);
    badgelink_client_cleanup(&test_client);

    return 0;
}

/*
 * Force protocol version 1 (legacy mode).
 * Must be called before init.
 */
void badgefs_backend_badgelink_force_v1(void)
{
    state.force_v1 = true;
}

/* ============================================================================
 * Backend Structure
 * ============================================================================ */

static struct badgefs_backend badgelink_backend = {
    .init       = bl_init,
    .destroy    = bl_destroy,
    .lookup     = bl_lookup,
    .readdir    = bl_readdir,
    .mkdir      = bl_mkdir,
    .rmdir      = bl_rmdir,
    .create     = bl_create,
    .unlink     = bl_unlink,
    .open       = bl_open,
    .release    = bl_release,
    .read       = bl_read,
    .write      = bl_write,
    .truncate   = bl_truncate,
    .fsync      = bl_fsync,
    .flush      = bl_flush,
    .rename     = bl_rename,
    .chmod      = bl_chmod,
    .chown      = bl_chown,
    .utimens    = bl_utimens,
    .getxattr   = bl_getxattr,
    .setxattr   = bl_setxattr,
    .listxattr  = bl_listxattr,
};

struct badgefs_backend *badgefs_backend_badgelink_get(void)
{
    return &badgelink_backend;
}
