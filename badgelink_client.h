/*
 * badgelink_client.h - High-level BadgeLink client interface
 *
 * Provides filesystem and AppFS operations for interacting with
 * the badge over BadgeLink.
 */

#ifndef BADGELINK_CLIENT_H
#define BADGELINK_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "badgelink_proto.h"

/* Protocol version constants */
#define BADGELINK_PROTOCOL_V1 1
#define BADGELINK_PROTOCOL_V2 2
#define BADGELINK_CLIENT_VERSION BADGELINK_PROTOCOL_V2

/* Directory entry for fs_list */
struct badgelink_dirent {
    char name[256];
    bool is_dir;
};

/* File stat info */
struct badgelink_stat {
    uint32_t size;
    uint64_t mtime;  /* Milliseconds since epoch */
    uint64_t ctime;
    uint64_t atime;
    bool is_dir;
};

/* AppFS metadata */
struct badgelink_app {
    char slug[48];
    char title[64];
    uint32_t version;
    uint32_t size;
};

/* Filesystem usage info */
struct badgelink_usage {
    uint32_t total;
    uint32_t used;
};

/* Client connection structure */
struct badgelink_client {
    struct badgelink_proto proto;
    bool connected;
    bool synced;
    uint32_t protocol_version;  /* Negotiated protocol version (1 or 2) */
    bool force_v1;              /* Skip negotiation, force v1 */
};

/*
 * Initialize the client.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_client_init(struct badgelink_client *client);

/*
 * Connect to the badge.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_client_connect(struct badgelink_client *client);

/*
 * Disconnect from the badge.
 */
void badgelink_client_disconnect(struct badgelink_client *client);

/*
 * Cleanup the client.
 */
void badgelink_client_cleanup(struct badgelink_client *client);

/*
 * Check if connected to badge.
 */
bool badgelink_client_is_connected(struct badgelink_client *client);

/*
 * Get negotiated protocol version.
 * Returns BADGELINK_PROTOCOL_V1 or BADGELINK_PROTOCOL_V2.
 */
uint32_t badgelink_client_get_protocol_version(struct badgelink_client *client);

/*
 * Force protocol version 1 (call before connect).
 * Skips version negotiation and uses legacy behavior.
 */
void badgelink_client_force_v1(struct badgelink_client *client);

/* ============================================================================
 * Filesystem Operations (for /sd and /int paths)
 * ============================================================================ */

/*
 * List directory contents.
 * Returns array of dirents in *entries (caller must free).
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_fs_list(struct badgelink_client *client, const char *path,
                      struct badgelink_dirent **entries, size_t *count);

/*
 * Get file/directory info.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_fs_stat(struct badgelink_client *client, const char *path,
                      struct badgelink_stat *st);

/*
 * Create a directory.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_fs_mkdir(struct badgelink_client *client, const char *path);

/*
 * Remove an empty directory.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_fs_rmdir(struct badgelink_client *client, const char *path);

/*
 * Delete a file.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_fs_delete(struct badgelink_client *client, const char *path);

/*
 * Download a file.
 * Returns file data in *data (caller must free).
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_fs_download(struct badgelink_client *client, const char *path,
                          uint8_t **data, size_t *size);

/*
 * Upload a file.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_fs_upload(struct badgelink_client *client, const char *path,
                        const uint8_t *data, size_t size);

/*
 * Get filesystem usage.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_fs_usage(struct badgelink_client *client, const char *path,
                       struct badgelink_usage *usage);

/* ============================================================================
 * AppFS Operations (for /appfs)
 * ============================================================================ */

/*
 * List installed applications.
 * Returns array of apps in *apps (caller must free).
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_appfs_list(struct badgelink_client *client,
                         struct badgelink_app **apps, size_t *count);

/*
 * Get app metadata.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_appfs_stat(struct badgelink_client *client, const char *slug,
                         struct badgelink_app *app);

/*
 * Download an app.
 * Returns app binary in *data (caller must free).
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_appfs_download(struct badgelink_client *client, const char *slug,
                             uint8_t **data, size_t *size);

/*
 * Upload an app.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_appfs_upload(struct badgelink_client *client,
                           const struct badgelink_app *metadata,
                           const uint8_t *data, size_t size);

/*
 * Delete an app.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_appfs_delete(struct badgelink_client *client, const char *slug);

/*
 * Get AppFS usage.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_appfs_usage(struct badgelink_client *client,
                          struct badgelink_usage *usage);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/*
 * Map BadgeLink status code to errno.
 */
int badgelink_status_to_errno(badgelink_StatusCode status);

#endif /* BADGELINK_CLIENT_H */
