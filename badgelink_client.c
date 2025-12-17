/*
 * badgelink_client.c - High-level BadgeLink client implementation
 *
 * Provides filesystem and AppFS operations for interacting with
 * the badge over BadgeLink.
 */

#include "badgelink_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* Timeout for most operations - match Python's def_timeout = 0.25s */
#define CLIENT_TIMEOUT_MS 500

/* Timeout for file transfers (per chunk) - match Python's chunk_timeout = 0.5s */
#define TRANSFER_TIMEOUT_MS 500

/* Timeout for starting file transfers - match Python's xfer_timeout = 10s */
#define XFER_TIMEOUT_MS 10000

/* Extra timeout for XferFinish per MB (badge needs time to sync large files) */
#define FINISH_TIMEOUT_PER_MB_MS 2000

/* Maximum finish timeout for very large files */
#define FINISH_TIMEOUT_MAX_MS 120000

/*
 * Map BadgeLink status code to errno.
 */
int badgelink_status_to_errno(badgelink_StatusCode status)
{
    switch (status) {
    case badgelink_StatusCode_StatusOk:
        return 0;
    case badgelink_StatusCode_StatusNotFound:
        return -ENOENT;
    case badgelink_StatusCode_StatusExists:
        return -EEXIST;
    case badgelink_StatusCode_StatusNotEmpty:
        return -ENOTEMPTY;
    case badgelink_StatusCode_StatusIsFile:
        return -ENOTDIR;
    case badgelink_StatusCode_StatusIsDir:
        return -EISDIR;
    case badgelink_StatusCode_StatusNoSpace:
        return -ENOSPC;
    case badgelink_StatusCode_StatusMalformed:
        return -EINVAL;
    case badgelink_StatusCode_StatusInternalError:
        return -EIO;
    case badgelink_StatusCode_StatusNotSupported:
        return -ENOSYS;
    case badgelink_StatusCode_StatusIllegalState:
        return -EBUSY;
    default:
        return -EIO;
    }
}

/*
 * Initialize the client.
 */
int badgelink_client_init(struct badgelink_client *client)
{
    if (!client)
        return -EINVAL;

    memset(client, 0, sizeof(*client));

    /* Default to V1 until version negotiation completes */
    client->protocol_version = BADGELINK_PROTOCOL_V1;
    client->force_v1 = false;

    int ret = badgelink_proto_init(&client->proto);
    if (ret < 0)
        return ret;

    return 0;
}

/*
 * Negotiate protocol version with badge.
 * Called after sync. Server resets to v1 on every sync, so this must be
 * called every time sync occurs.
 *
 * Returns 0 on success (protocol_version is set).
 * On failure, falls back to v1.
 */
static int negotiate_protocol_version(struct badgelink_client *client)
{
    /* If force_v1 is set, skip negotiation */
    if (client->force_v1) {
        client->protocol_version = BADGELINK_PROTOCOL_V1;
        return 0;
    }

    /* Send VersionReq with our highest supported version */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_version_req_tag;
    req.req.version_req.client_version = BADGELINK_CLIENT_VERSION;

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      CLIENT_TIMEOUT_MS);
    if (ret < 0) {
        /* Timeout or error - fall back to v1 */
        client->protocol_version = BADGELINK_PROTOCOL_V1;
        return 0;
    }

    /* Check if server supports version negotiation */
    if (resp.status_code == badgelink_StatusCode_StatusNotSupported) {
        /* Server is v1 only */
        client->protocol_version = BADGELINK_PROTOCOL_V1;
        return 0;
    }

    if (resp.status_code != badgelink_StatusCode_StatusOk) {
        /* Unexpected status - fall back to v1 */
        client->protocol_version = BADGELINK_PROTOCOL_V1;
        return 0;
    }

    /* Check response type */
    if (resp.which_resp != badgelink_Response_version_resp_tag) {
        /* Unexpected response type - fall back to v1 */
        client->protocol_version = BADGELINK_PROTOCOL_V1;
        return 0;
    }

    /* Use negotiated version */
    client->protocol_version = resp.resp.version_resp.negotiated_version;

    return 0;
}

/*
 * Connect to the badge.
 */
int badgelink_client_connect(struct badgelink_client *client)
{
    if (!client)
        return -EINVAL;

    if (client->connected)
        return 0;

    int ret = badgelink_proto_connect(&client->proto);
    if (ret < 0)
        return ret;

    client->connected = true;

    /* Sync with badge */
    ret = badgelink_proto_sync(&client->proto);
    if (ret < 0) {
        badgelink_proto_disconnect(&client->proto);
        client->connected = false;
        return ret;
    }

    client->synced = true;

    /* Negotiate protocol version after sync */
    negotiate_protocol_version(client);

    return 0;
}

/*
 * Disconnect from the badge.
 */
void badgelink_client_disconnect(struct badgelink_client *client)
{
    if (client && client->connected) {
        badgelink_proto_disconnect(&client->proto);
        client->connected = false;
        client->synced = false;
    }
}

/*
 * Cleanup the client.
 */
void badgelink_client_cleanup(struct badgelink_client *client)
{
    if (client) {
        badgelink_client_disconnect(client);
        badgelink_proto_cleanup(&client->proto);
    }
}

/*
 * Check if connected to badge.
 */
bool badgelink_client_is_connected(struct badgelink_client *client)
{
    return client && client->connected && client->synced;
}

/*
 * Get negotiated protocol version.
 */
uint32_t badgelink_client_get_protocol_version(struct badgelink_client *client)
{
    if (!client)
        return BADGELINK_PROTOCOL_V1;
    return client->protocol_version;
}

/*
 * Force protocol version 1 (call before connect).
 */
void badgelink_client_force_v1(struct badgelink_client *client)
{
    if (client) {
        client->force_v1 = true;
        client->protocol_version = BADGELINK_PROTOCOL_V1;
    }
}

/* ============================================================================
 * Filesystem Operations
 * ============================================================================ */

/*
 * List directory contents.
 */
int badgelink_fs_list(struct badgelink_client *client, const char *path,
                      struct badgelink_dirent **entries, size_t *count)
{
    if (!client || !path || !entries || !count)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    *entries = NULL;
    *count = 0;

    struct badgelink_dirent *result = NULL;
    size_t result_count = 0;
    size_t result_capacity = 0;
    uint32_t offset = 0;

    do {
        /* Build list request */
        badgelink_Request req = badgelink_Request_init_zero;
        req.which_req = badgelink_Request_fs_action_tag;
        req.req.fs_action.type = badgelink_FsActionType_FsActionList;
        strncpy(req.req.fs_action.path, path, sizeof(req.req.fs_action.path) - 1);
        req.req.fs_action.list_offset = offset;

        badgelink_Response resp;
        int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                          CLIENT_TIMEOUT_MS);
        if (ret < 0) {
            free(result);
            return ret;
        }

        if (resp.status_code != badgelink_StatusCode_StatusOk) {
            free(result);
            return badgelink_status_to_errno(resp.status_code);
        }

        /* Check response type */
        if (resp.which_resp != badgelink_Response_fs_resp_tag ||
            resp.resp.fs_resp.which_val != badgelink_FsActionResp_list_tag) {
            free(result);
            return -EINVAL;
        }

        badgelink_FsDirentList *list = &resp.resp.fs_resp.val.list;

        /* Expand result array if needed */
        size_t needed = result_count + list->list_count;
        if (needed > result_capacity) {
            size_t new_cap = (needed + 16) & ~15;
            struct badgelink_dirent *new_result = realloc(result,
                new_cap * sizeof(struct badgelink_dirent));
            if (!new_result) {
                free(result);
                return -ENOMEM;
            }
            result = new_result;
            result_capacity = new_cap;
        }

        /* Copy entries */
        for (size_t i = 0; i < list->list_count; i++) {
            strncpy(result[result_count].name, list->list[i].name,
                    sizeof(result[result_count].name) - 1);
            result[result_count].name[sizeof(result[result_count].name) - 1] = '\0';
            result[result_count].is_dir = list->list[i].is_dir;
            result_count++;
        }

        offset += list->list_count;

        /* Check if we got all entries */
        if (offset >= list->total_size)
            break;

    } while (1);

    *entries = result;
    *count = result_count;
    return 0;
}

/*
 * Get file/directory info.
 */
int badgelink_fs_stat(struct badgelink_client *client, const char *path,
                      struct badgelink_stat *st)
{
    if (!client || !path || !st)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Build stat request */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_fs_action_tag;
    req.req.fs_action.type = badgelink_FsActionType_FsActionStat;
    strncpy(req.req.fs_action.path, path, sizeof(req.req.fs_action.path) - 1);

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      CLIENT_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    /* Check response type */
    if (resp.which_resp != badgelink_Response_fs_resp_tag ||
        resp.resp.fs_resp.which_val != badgelink_FsActionResp_stat_tag) {
        return -EINVAL;
    }

    badgelink_FsStat *fs_stat = &resp.resp.fs_resp.val.stat;
    st->size = fs_stat->size;
    st->mtime = fs_stat->mtime;
    st->ctime = fs_stat->ctime;
    st->atime = fs_stat->atime;
    st->is_dir = fs_stat->is_dir;

    return 0;
}

/*
 * Create a directory.
 */
int badgelink_fs_mkdir(struct badgelink_client *client, const char *path)
{
    if (!client || !path)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Build mkdir request */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_fs_action_tag;
    req.req.fs_action.type = badgelink_FsActionType_FsActionMkdir;
    strncpy(req.req.fs_action.path, path, sizeof(req.req.fs_action.path) - 1);

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      CLIENT_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    return 0;
}

/*
 * Remove an empty directory.
 */
int badgelink_fs_rmdir(struct badgelink_client *client, const char *path)
{
    if (!client || !path)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Build rmdir request */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_fs_action_tag;
    req.req.fs_action.type = badgelink_FsActionType_FsActionRmdir;
    strncpy(req.req.fs_action.path, path, sizeof(req.req.fs_action.path) - 1);

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      CLIENT_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    return 0;
}

/*
 * Delete a file.
 */
int badgelink_fs_delete(struct badgelink_client *client, const char *path)
{
    if (!client || !path)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Build delete request */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_fs_action_tag;
    req.req.fs_action.type = badgelink_FsActionType_FsActionDelete;
    strncpy(req.req.fs_action.path, path, sizeof(req.req.fs_action.path) - 1);

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      CLIENT_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    return 0;
}

/*
 * Download a file.
 */
int badgelink_fs_download(struct badgelink_client *client, const char *path,
                          uint8_t **data, size_t *size)
{
    if (!client || !path || !data || !size)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Always negotiate protocol version before transfer */
    negotiate_protocol_version(client);

    *data = NULL;
    *size = 0;

    /* Start download */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_fs_action_tag;
    req.req.fs_action.type = badgelink_FsActionType_FsActionDownload;
    strncpy(req.req.fs_action.path, path, sizeof(req.req.fs_action.path) - 1);

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      XFER_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    /* Get file size and CRC from response
     * V1: crc32 is the actual CRC (server read entire file)
     * V2: crc32 is 0 (server used stat, will send CRC at XferFinish)
     */
    uint32_t file_size = resp.resp.fs_resp.size;
    uint32_t initial_crc = 0;
    if (resp.resp.fs_resp.which_val == badgelink_FsActionResp_crc32_tag) {
        initial_crc = resp.resp.fs_resp.val.crc32;
    }

    fprintf(stderr, "fs_download: %s (%u bytes, v%u)\n", path, file_size,
            client->protocol_version);

    if (file_size == 0) {
        *data = malloc(1);  /* Empty file */
        if (!*data)
            return -ENOMEM;
        *size = 0;
        return 0;
    }

    /* Allocate buffer */
    uint8_t *buffer = malloc(file_size);
    if (!buffer)
        return -ENOMEM;

    uint32_t received = 0;
    uint32_t running_crc = 0xFFFFFFFF;  /* Initialize for streaming CRC */

    /* Receive chunks */
    while (received < file_size) {
        /* Request next chunk */
        badgelink_Request cont_req = badgelink_Request_init_zero;
        cont_req.which_req = badgelink_Request_xfer_ctrl_tag;
        cont_req.req.xfer_ctrl = badgelink_XferReq_XferContinue;

        badgelink_Response chunk_resp;
        ret = badgelink_proto_request(&client->proto, &cont_req, &chunk_resp,
                                      TRANSFER_TIMEOUT_MS);
        if (ret < 0) {
            fprintf(stderr, "fs_download: %s failed at %u/%u bytes\n",
                    path, received, file_size);
            free(buffer);
            /* Drain any stale USB data before cleanup */
            badgelink_proto_resync(&client->proto);
            /* Re-negotiate protocol version after resync (badge resets to v1) */
            negotiate_protocol_version(client);
            /* Send XferFinish to clean up state */
            badgelink_Request finish_req = badgelink_Request_init_zero;
            finish_req.which_req = badgelink_Request_xfer_ctrl_tag;
            finish_req.req.xfer_ctrl = badgelink_XferReq_XferFinish;
            badgelink_proto_request(&client->proto, &finish_req, &resp, CLIENT_TIMEOUT_MS);
            return ret;
        }

        if (chunk_resp.status_code != badgelink_StatusCode_StatusOk) {
            free(buffer);
            return badgelink_status_to_errno(chunk_resp.status_code);
        }

        /* Check response type */
        if (chunk_resp.which_resp != badgelink_Response_download_chunk_tag) {
            free(buffer);
            return -EINVAL;
        }

        badgelink_Chunk *chunk = &chunk_resp.resp.download_chunk;

        /* Verify chunk position */
        if (chunk->position != received) {
            fprintf(stderr, "fs_download: %s chunk position mismatch\n", path);
            free(buffer);
            return -EIO;
        }

        /* Copy chunk data */
        size_t chunk_size = chunk->data.size;
        if (received + chunk_size > file_size)
            chunk_size = file_size - received;

        memcpy(buffer + received, chunk->data.bytes, chunk_size);

        /* Update running CRC */
        running_crc = badgelink_crc32_update(running_crc, chunk->data.bytes, chunk_size);

        received += chunk_size;
    }

    /* Finalize running CRC */
    running_crc = badgelink_crc32_final(running_crc);

    /* Send XferFinish to complete transfer */
    badgelink_Request finish_req = badgelink_Request_init_zero;
    finish_req.which_req = badgelink_Request_xfer_ctrl_tag;
    finish_req.req.xfer_ctrl = badgelink_XferReq_XferFinish;
    ret = badgelink_proto_request(&client->proto, &finish_req, &resp, CLIENT_TIMEOUT_MS);

    /* Get expected CRC based on protocol version */
    uint32_t expected_crc;
    if (client->protocol_version >= BADGELINK_PROTOCOL_V2) {
        /* V2: XferFinish returns FsActionResp with crc32 */
        if (ret < 0) {
            fprintf(stderr, "fs_download: %s finish failed\n", path);
            free(buffer);
            return ret;
        }
        if (resp.which_resp == badgelink_Response_fs_resp_tag &&
            resp.resp.fs_resp.which_val == badgelink_FsActionResp_crc32_tag) {
            expected_crc = resp.resp.fs_resp.val.crc32;
        } else {
            fprintf(stderr, "fs_download: %s unexpected finish response\n", path);
            free(buffer);
            return -EIO;
        }
    } else {
        /* V1: XferFinish returns StatusOk, use CRC from initial response */
        expected_crc = initial_crc;
    }

    /* Verify CRC */
    if (running_crc != expected_crc) {
        fprintf(stderr, "fs_download: %s CRC mismatch\n", path);
        free(buffer);
        return -EIO;
    }

    fprintf(stderr, "fs_download: %s complete\n", path);

    *data = buffer;
    *size = file_size;
    return 0;
}

/*
 * Upload a file.
 */
int badgelink_fs_upload(struct badgelink_client *client, const char *path,
                        const uint8_t *data, size_t size)
{
    if (!client || !path || (!data && size > 0))
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Always negotiate protocol version before transfer */
    negotiate_protocol_version(client);

    fprintf(stderr, "fs_upload: %s (%zu bytes, v%u)\n", path, size,
            client->protocol_version);

    /* Calculate CRC32 of file */
    uint32_t crc = badgelink_crc32(data, size);

    /* Start upload */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_fs_action_tag;
    req.req.fs_action.type = badgelink_FsActionType_FsActionUpload;
    strncpy(req.req.fs_action.path, path, sizeof(req.req.fs_action.path) - 1);
    req.req.fs_action.size = size;
    req.req.fs_action.crc32 = crc;

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      XFER_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    /* Send chunks */
    uint32_t sent = 0;
    while (sent < size) {
        size_t chunk_size = size - sent;
        if (chunk_size > BADGELINK_CHUNK_MAX)
            chunk_size = BADGELINK_CHUNK_MAX;

        badgelink_Request chunk_req = badgelink_Request_init_zero;
        chunk_req.which_req = badgelink_Request_upload_chunk_tag;
        chunk_req.req.upload_chunk.position = sent;
        chunk_req.req.upload_chunk.data.size = chunk_size;
        memcpy(chunk_req.req.upload_chunk.data.bytes, data + sent, chunk_size);

        badgelink_Response chunk_resp;
        ret = badgelink_proto_request(&client->proto, &chunk_req, &chunk_resp,
                                      TRANSFER_TIMEOUT_MS);
        if (ret < 0) {
            fprintf(stderr, "fs_upload: %s failed at %u/%zu bytes\n",
                    path, sent, size);
            return ret;
        }

        if (chunk_resp.status_code != badgelink_StatusCode_StatusOk)
            return badgelink_status_to_errno(chunk_resp.status_code);

        sent += chunk_size;

        /* Small delay between chunks to avoid overwhelming the badge */
        usleep(5000);  /* 5ms */
    }

    /* Finish upload - use dynamic timeout based on file size */
    /* Badge needs time to sync large files to SD card */
    int finish_timeout = XFER_TIMEOUT_MS + (size / (1024 * 1024)) * FINISH_TIMEOUT_PER_MB_MS;
    if (finish_timeout > FINISH_TIMEOUT_MAX_MS)
        finish_timeout = FINISH_TIMEOUT_MAX_MS;

    badgelink_Request finish_req = badgelink_Request_init_zero;
    finish_req.which_req = badgelink_Request_xfer_ctrl_tag;
    finish_req.req.xfer_ctrl = badgelink_XferReq_XferFinish;

    ret = badgelink_proto_request(&client->proto, &finish_req, &resp,
                                  finish_timeout);
    if (ret < 0) {
        fprintf(stderr, "fs_upload: %s finish failed\n", path);
        return ret;
    }

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    fprintf(stderr, "fs_upload: %s complete\n", path);

    /* Small delay after upload to let badge settle before next operation */
    usleep(50000);  /* 50ms */

    return 0;
}

/*
 * Get filesystem usage.
 */
int badgelink_fs_usage(struct badgelink_client *client, const char *path,
                       struct badgelink_usage *usage)
{
    if (!client || !path || !usage)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Build usage request */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_fs_action_tag;
    req.req.fs_action.type = badgelink_FsActionType_FsActionGetUsage;
    strncpy(req.req.fs_action.path, path, sizeof(req.req.fs_action.path) - 1);

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      CLIENT_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    /* Check response type */
    if (resp.which_resp != badgelink_Response_fs_resp_tag ||
        resp.resp.fs_resp.which_val != badgelink_FsActionResp_usage_tag) {
        return -EINVAL;
    }

    usage->total = resp.resp.fs_resp.val.usage.size;
    usage->used = resp.resp.fs_resp.val.usage.used;

    return 0;
}

/* ============================================================================
 * AppFS Operations
 * ============================================================================ */

/*
 * List installed applications.
 */
int badgelink_appfs_list(struct badgelink_client *client,
                         struct badgelink_app **apps, size_t *count)
{
    if (!client || !apps || !count)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    *apps = NULL;
    *count = 0;

    struct badgelink_app *result = NULL;
    size_t result_count = 0;
    size_t result_capacity = 0;
    uint32_t offset = 0;

    do {
        /* Build list request */
        badgelink_Request req = badgelink_Request_init_zero;
        req.which_req = badgelink_Request_appfs_action_tag;
        req.req.appfs_action.type = badgelink_FsActionType_FsActionList;
        req.req.appfs_action.list_offset = offset;

        badgelink_Response resp;
        int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                          CLIENT_TIMEOUT_MS);
        if (ret < 0) {
            free(result);
            return ret;
        }

        if (resp.status_code != badgelink_StatusCode_StatusOk) {
            free(result);
            return badgelink_status_to_errno(resp.status_code);
        }

        /* Check response type */
        if (resp.which_resp != badgelink_Response_appfs_resp_tag ||
            resp.resp.appfs_resp.which_val != badgelink_AppfsActionResp_list_tag) {
            free(result);
            return -EINVAL;
        }

        badgelink_AppfsList *list = &resp.resp.appfs_resp.val.list;

        /* Expand result array if needed */
        size_t needed = result_count + list->list_count;
        if (needed > result_capacity) {
            size_t new_cap = (needed + 16) & ~15;
            struct badgelink_app *new_result = realloc(result,
                new_cap * sizeof(struct badgelink_app));
            if (!new_result) {
                free(result);
                return -ENOMEM;
            }
            result = new_result;
            result_capacity = new_cap;
        }

        /* Copy entries */
        for (size_t i = 0; i < list->list_count; i++) {
            strncpy(result[result_count].slug, list->list[i].slug,
                    sizeof(result[result_count].slug) - 1);
            result[result_count].slug[sizeof(result[result_count].slug) - 1] = '\0';
            strncpy(result[result_count].title, list->list[i].title,
                    sizeof(result[result_count].title) - 1);
            result[result_count].title[sizeof(result[result_count].title) - 1] = '\0';
            result[result_count].version = list->list[i].version;
            result[result_count].size = list->list[i].size;
            result_count++;
        }

        offset += list->list_count;

        /* Check if we got all entries */
        if (offset >= list->total_size)
            break;

    } while (1);

    *apps = result;
    *count = result_count;
    return 0;
}

/*
 * Get app metadata.
 */
int badgelink_appfs_stat(struct badgelink_client *client, const char *slug,
                         struct badgelink_app *app)
{
    if (!client || !slug || !app)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Build stat request */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_appfs_action_tag;
    req.req.appfs_action.type = badgelink_FsActionType_FsActionStat;
    req.req.appfs_action.which_id = badgelink_AppfsActionReq_slug_tag;
    strncpy(req.req.appfs_action.id.slug, slug,
            sizeof(req.req.appfs_action.id.slug) - 1);

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      CLIENT_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    /* Check response type */
    if (resp.which_resp != badgelink_Response_appfs_resp_tag ||
        resp.resp.appfs_resp.which_val != badgelink_AppfsActionResp_metadata_tag) {
        return -EINVAL;
    }

    badgelink_AppfsMetadata *meta = &resp.resp.appfs_resp.val.metadata;
    strncpy(app->slug, meta->slug, sizeof(app->slug) - 1);
    app->slug[sizeof(app->slug) - 1] = '\0';
    strncpy(app->title, meta->title, sizeof(app->title) - 1);
    app->title[sizeof(app->title) - 1] = '\0';
    app->version = meta->version;
    app->size = meta->size;

    return 0;
}

/*
 * Download an app.
 */
int badgelink_appfs_download(struct badgelink_client *client, const char *slug,
                             uint8_t **data, size_t *size)
{
    if (!client || !slug || !data || !size)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Always negotiate protocol version before transfer */
    negotiate_protocol_version(client);

    *data = NULL;
    *size = 0;

    /* Start download */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_appfs_action_tag;
    req.req.appfs_action.type = badgelink_FsActionType_FsActionDownload;
    req.req.appfs_action.which_id = badgelink_AppfsActionReq_slug_tag;
    strncpy(req.req.appfs_action.id.slug, slug,
            sizeof(req.req.appfs_action.id.slug) - 1);

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      XFER_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    /* Get app size and CRC from response
     * V1: crc32 is the actual CRC (server read entire file)
     * V2: crc32 is 0 (server used stat, will send CRC at XferFinish)
     */
    uint32_t app_size = resp.resp.appfs_resp.size;
    uint32_t initial_crc = 0;
    if (resp.resp.appfs_resp.which_val == badgelink_AppfsActionResp_crc32_tag) {
        initial_crc = resp.resp.appfs_resp.val.crc32;
    }

    fprintf(stderr, "appfs_download: %s (%u bytes, v%u)\n", slug, app_size,
            client->protocol_version);

    if (app_size == 0) {
        *data = malloc(1);
        if (!*data)
            return -ENOMEM;
        *size = 0;
        return 0;
    }

    /* Allocate buffer */
    uint8_t *buffer = malloc(app_size);
    if (!buffer)
        return -ENOMEM;

    uint32_t received = 0;
    uint32_t running_crc = 0xFFFFFFFF;  /* Initialize for streaming CRC */

    /* Receive chunks */
    while (received < app_size) {
        badgelink_Request cont_req = badgelink_Request_init_zero;
        cont_req.which_req = badgelink_Request_xfer_ctrl_tag;
        cont_req.req.xfer_ctrl = badgelink_XferReq_XferContinue;

        badgelink_Response chunk_resp;
        ret = badgelink_proto_request(&client->proto, &cont_req, &chunk_resp,
                                      TRANSFER_TIMEOUT_MS);
        if (ret < 0) {
            fprintf(stderr, "appfs_download: %s failed at %u/%u bytes\n",
                    slug, received, app_size);
            free(buffer);
            return ret;
        }

        if (chunk_resp.status_code != badgelink_StatusCode_StatusOk) {
            free(buffer);
            return badgelink_status_to_errno(chunk_resp.status_code);
        }

        if (chunk_resp.which_resp != badgelink_Response_download_chunk_tag) {
            free(buffer);
            return -EINVAL;
        }

        badgelink_Chunk *chunk = &chunk_resp.resp.download_chunk;

        if (chunk->position != received) {
            fprintf(stderr, "appfs_download: %s chunk position mismatch\n", slug);
            free(buffer);
            return -EIO;
        }

        size_t chunk_size = chunk->data.size;
        if (received + chunk_size > app_size)
            chunk_size = app_size - received;

        memcpy(buffer + received, chunk->data.bytes, chunk_size);

        /* Update running CRC */
        running_crc = badgelink_crc32_update(running_crc, chunk->data.bytes, chunk_size);

        received += chunk_size;
    }

    /* Finalize running CRC */
    running_crc = badgelink_crc32_final(running_crc);

    /* Send XferFinish to properly terminate transfer */
    badgelink_Request finish_req = badgelink_Request_init_zero;
    finish_req.which_req = badgelink_Request_xfer_ctrl_tag;
    finish_req.req.xfer_ctrl = badgelink_XferReq_XferFinish;
    ret = badgelink_proto_request(&client->proto, &finish_req, &resp, CLIENT_TIMEOUT_MS);

    /* Get expected CRC based on protocol version */
    uint32_t expected_crc;
    if (client->protocol_version >= BADGELINK_PROTOCOL_V2) {
        /* V2: XferFinish returns AppfsActionResp with crc32 */
        if (ret < 0) {
            fprintf(stderr, "appfs_download: %s finish failed\n", slug);
            free(buffer);
            return ret;
        }
        if (resp.which_resp == badgelink_Response_appfs_resp_tag &&
            resp.resp.appfs_resp.which_val == badgelink_AppfsActionResp_crc32_tag) {
            expected_crc = resp.resp.appfs_resp.val.crc32;
        } else {
            fprintf(stderr, "appfs_download: %s unexpected finish response\n", slug);
            free(buffer);
            return -EIO;
        }
    } else {
        /* V1: XferFinish returns StatusOk, use CRC from initial response */
        expected_crc = initial_crc;
    }

    /* Verify CRC */
    if (running_crc != expected_crc) {
        fprintf(stderr, "appfs_download: %s CRC mismatch\n", slug);
        free(buffer);
        return -EIO;
    }

    fprintf(stderr, "appfs_download: %s complete\n", slug);

    *data = buffer;
    *size = app_size;
    return 0;
}

/*
 * Upload an app.
 */
int badgelink_appfs_upload(struct badgelink_client *client,
                           const struct badgelink_app *metadata,
                           const uint8_t *data, size_t size)
{
    if (!client || !metadata || (!data && size > 0))
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Always negotiate protocol version before transfer */
    negotiate_protocol_version(client);

    fprintf(stderr, "appfs_upload: %s (%zu bytes, v%u)\n", metadata->slug, size,
            client->protocol_version);

    /* Calculate CRC32 */
    uint32_t crc = badgelink_crc32(data, size);

    /* Start upload */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_appfs_action_tag;
    req.req.appfs_action.type = badgelink_FsActionType_FsActionUpload;
    req.req.appfs_action.which_id = badgelink_AppfsActionReq_metadata_tag;
    strncpy(req.req.appfs_action.id.metadata.slug, metadata->slug,
            sizeof(req.req.appfs_action.id.metadata.slug) - 1);
    strncpy(req.req.appfs_action.id.metadata.title, metadata->title,
            sizeof(req.req.appfs_action.id.metadata.title) - 1);
    req.req.appfs_action.id.metadata.version = metadata->version;
    req.req.appfs_action.id.metadata.size = size;
    req.req.appfs_action.crc32 = crc;

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      XFER_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    /* Send chunks */
    uint32_t sent = 0;
    while (sent < size) {
        size_t chunk_size = size - sent;
        if (chunk_size > BADGELINK_CHUNK_MAX)
            chunk_size = BADGELINK_CHUNK_MAX;

        badgelink_Request chunk_req = badgelink_Request_init_zero;
        chunk_req.which_req = badgelink_Request_upload_chunk_tag;
        chunk_req.req.upload_chunk.position = sent;
        chunk_req.req.upload_chunk.data.size = chunk_size;
        memcpy(chunk_req.req.upload_chunk.data.bytes, data + sent, chunk_size);

        badgelink_Response chunk_resp;
        ret = badgelink_proto_request(&client->proto, &chunk_req, &chunk_resp,
                                      TRANSFER_TIMEOUT_MS);
        if (ret < 0)
            return ret;

        if (chunk_resp.status_code != badgelink_StatusCode_StatusOk)
            return badgelink_status_to_errno(chunk_resp.status_code);

        sent += chunk_size;
    }

    /* Finish upload - use xfer_timeout like Python, badge needs time to write to storage */
    badgelink_Request finish_req = badgelink_Request_init_zero;
    finish_req.which_req = badgelink_Request_xfer_ctrl_tag;
    finish_req.req.xfer_ctrl = badgelink_XferReq_XferFinish;

    ret = badgelink_proto_request(&client->proto, &finish_req, &resp,
                                  XFER_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    /* Small delay after upload to let badge settle before next operation */
    usleep(50000);  /* 50ms */

    fprintf(stderr, "appfs_upload: %s complete\n", metadata->slug);
    return 0;
}

/*
 * Delete an app.
 */
int badgelink_appfs_delete(struct badgelink_client *client, const char *slug)
{
    if (!client || !slug)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Build delete request */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_appfs_action_tag;
    req.req.appfs_action.type = badgelink_FsActionType_FsActionDelete;
    req.req.appfs_action.which_id = badgelink_AppfsActionReq_slug_tag;
    strncpy(req.req.appfs_action.id.slug, slug,
            sizeof(req.req.appfs_action.id.slug) - 1);

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      CLIENT_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    return 0;
}

/*
 * Get AppFS usage.
 */
int badgelink_appfs_usage(struct badgelink_client *client,
                          struct badgelink_usage *usage)
{
    if (!client || !usage)
        return -EINVAL;

    if (!client->connected)
        return -ENODEV;

    /* Build usage request */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_appfs_action_tag;
    req.req.appfs_action.type = badgelink_FsActionType_FsActionGetUsage;

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      CLIENT_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

    /* Check response type */
    if (resp.which_resp != badgelink_Response_appfs_resp_tag ||
        resp.resp.appfs_resp.which_val != badgelink_AppfsActionResp_usage_tag) {
        return -EINVAL;
    }

    usage->total = resp.resp.appfs_resp.val.usage.size;
    usage->used = resp.resp.appfs_resp.val.usage.used;

    return 0;
}
