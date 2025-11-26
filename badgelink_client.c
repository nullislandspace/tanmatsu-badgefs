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

    int ret = badgelink_proto_init(&client->proto);
    if (ret < 0)
        return ret;

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

    *data = NULL;
    *size = 0;

    fprintf(stderr, "DEBUG fs_download: path=%s\n", path);

    /* Start download */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_fs_action_tag;
    req.req.fs_action.type = badgelink_FsActionType_FsActionDownload;
    strncpy(req.req.fs_action.path, path, sizeof(req.req.fs_action.path) - 1);

    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      XFER_TIMEOUT_MS);
    if (ret < 0) {
        fprintf(stderr, "DEBUG fs_download: initial request failed: %d\n", ret);
        return ret;
    }

    if (resp.status_code != badgelink_StatusCode_StatusOk) {
        fprintf(stderr, "DEBUG fs_download: status=%d\n", resp.status_code);
        return badgelink_status_to_errno(resp.status_code);
    }

    /* Get file size from response */
    uint32_t file_size = resp.resp.fs_resp.size;
    fprintf(stderr, "DEBUG fs_download: file_size=%u\n", file_size);

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

    /* Receive chunks */
    while (received < file_size) {
        fprintf(stderr, "DEBUG fs_download: chunk received=%u/%u\n", received, file_size);

        /* Request next chunk - no delay needed, badge handles pacing */
        badgelink_Request cont_req = badgelink_Request_init_zero;
        cont_req.which_req = badgelink_Request_xfer_ctrl_tag;
        cont_req.req.xfer_ctrl = badgelink_XferReq_XferContinue;

        badgelink_Response chunk_resp;
        ret = badgelink_proto_request(&client->proto, &cont_req, &chunk_resp,
                                      TRANSFER_TIMEOUT_MS);
        if (ret < 0) {
            fprintf(stderr, "DEBUG fs_download: chunk request failed: %d\n", ret);
            free(buffer);
            /* Drain any stale USB data before cleanup */
            badgelink_proto_resync(&client->proto);
            /* Send XferFinish to clean up state */
            badgelink_Request finish_req = badgelink_Request_init_zero;
            finish_req.which_req = badgelink_Request_xfer_ctrl_tag;
            finish_req.req.xfer_ctrl = badgelink_XferReq_XferFinish;
            badgelink_proto_request(&client->proto, &finish_req, &resp, CLIENT_TIMEOUT_MS);
            return ret;
        }

        if (chunk_resp.status_code != badgelink_StatusCode_StatusOk) {
            fprintf(stderr, "DEBUG fs_download: chunk status=%d\n", chunk_resp.status_code);
            free(buffer);
            return badgelink_status_to_errno(chunk_resp.status_code);
        }

        /* Check response type */
        if (chunk_resp.which_resp != badgelink_Response_download_chunk_tag) {
            fprintf(stderr, "DEBUG fs_download: unexpected response type=%d\n", chunk_resp.which_resp);
            free(buffer);
            return -EINVAL;
        }

        badgelink_Chunk *chunk = &chunk_resp.resp.download_chunk;

        /* Verify chunk position */
        if (chunk->position != received) {
            fprintf(stderr, "badgelink_client: chunk position mismatch: "
                    "expected %u got %u\n", received, chunk->position);
            free(buffer);
            return -EIO;
        }

        /* Copy chunk data */
        size_t chunk_size = chunk->data.size;
        if (received + chunk_size > file_size)
            chunk_size = file_size - received;

        memcpy(buffer + received, chunk->data.bytes, chunk_size);
        received += chunk_size;
    }

    fprintf(stderr, "DEBUG fs_download: transfer complete, sending XferFinish\n");

    /* Send XferFinish to complete transfer (like Python does) */
    badgelink_Request finish_req = badgelink_Request_init_zero;
    finish_req.which_req = badgelink_Request_xfer_ctrl_tag;
    finish_req.req.xfer_ctrl = badgelink_XferReq_XferFinish;
    ret = badgelink_proto_request(&client->proto, &finish_req, &resp, CLIENT_TIMEOUT_MS);
    if (ret < 0) {
        fprintf(stderr, "DEBUG fs_download: XferFinish failed: %d\n", ret);
        /* Don't fail the download if we already got all data */
    }

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

    fprintf(stderr, "DEBUG fs_upload: path=%s size=%zu\n", path, size);

    /* Calculate CRC32 of file */
    uint32_t crc = badgelink_crc32(data, size);
    fprintf(stderr, "DEBUG fs_upload: CRC32=0x%08x\n", crc);

    /* Start upload */
    badgelink_Request req = badgelink_Request_init_zero;
    req.which_req = badgelink_Request_fs_action_tag;
    req.req.fs_action.type = badgelink_FsActionType_FsActionUpload;
    strncpy(req.req.fs_action.path, path, sizeof(req.req.fs_action.path) - 1);
    req.req.fs_action.size = size;
    req.req.fs_action.crc32 = crc;

    fprintf(stderr, "DEBUG fs_upload: sending upload request\n");
    badgelink_Response resp;
    int ret = badgelink_proto_request(&client->proto, &req, &resp,
                                      XFER_TIMEOUT_MS);
    if (ret < 0) {
        fprintf(stderr, "DEBUG fs_upload: upload request failed: %d\n", ret);
        return ret;
    }

    if (resp.status_code != badgelink_StatusCode_StatusOk) {
        fprintf(stderr, "DEBUG fs_upload: upload request status: %d\n", resp.status_code);
        return badgelink_status_to_errno(resp.status_code);
    }
    fprintf(stderr, "DEBUG fs_upload: upload request accepted\n");

    /* Send chunks */
    uint32_t sent = 0;
    int chunk_num = 0;
    while (sent < size) {
        size_t chunk_size = size - sent;
        if (chunk_size > BADGELINK_CHUNK_MAX)
            chunk_size = BADGELINK_CHUNK_MAX;

        fprintf(stderr, "DEBUG fs_upload: sending chunk %d, pos=%u size=%zu\n",
                chunk_num++, sent, chunk_size);

        badgelink_Request chunk_req = badgelink_Request_init_zero;
        chunk_req.which_req = badgelink_Request_upload_chunk_tag;
        chunk_req.req.upload_chunk.position = sent;
        chunk_req.req.upload_chunk.data.size = chunk_size;
        memcpy(chunk_req.req.upload_chunk.data.bytes, data + sent, chunk_size);

        badgelink_Response chunk_resp;
        ret = badgelink_proto_request(&client->proto, &chunk_req, &chunk_resp,
                                      TRANSFER_TIMEOUT_MS);
        if (ret < 0) {
            fprintf(stderr, "DEBUG fs_upload: chunk failed: %d\n", ret);
            return ret;
        }

        if (chunk_resp.status_code != badgelink_StatusCode_StatusOk) {
            fprintf(stderr, "DEBUG fs_upload: chunk status: %d\n", chunk_resp.status_code);
            return badgelink_status_to_errno(chunk_resp.status_code);
        }

        sent += chunk_size;
    }

    fprintf(stderr, "DEBUG fs_upload: all chunks sent, finishing\n");

    /* Finish upload */
    badgelink_Request finish_req = badgelink_Request_init_zero;
    finish_req.which_req = badgelink_Request_xfer_ctrl_tag;
    finish_req.req.xfer_ctrl = badgelink_XferReq_XferFinish;

    ret = badgelink_proto_request(&client->proto, &finish_req, &resp,
                                  TRANSFER_TIMEOUT_MS);
    if (ret < 0) {
        fprintf(stderr, "DEBUG fs_upload: finish failed: %d\n", ret);
        return ret;
    }

    if (resp.status_code != badgelink_StatusCode_StatusOk) {
        fprintf(stderr, "DEBUG fs_upload: finish status: %d\n", resp.status_code);
        return badgelink_status_to_errno(resp.status_code);
    }

    fprintf(stderr, "DEBUG fs_upload: upload complete\n");
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

    /* Get app size from response */
    uint32_t app_size = resp.resp.appfs_resp.size;
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

    /* Receive chunks */
    while (received < app_size) {
        badgelink_Request cont_req = badgelink_Request_init_zero;
        cont_req.which_req = badgelink_Request_xfer_ctrl_tag;
        cont_req.req.xfer_ctrl = badgelink_XferReq_XferContinue;

        badgelink_Response chunk_resp;
        ret = badgelink_proto_request(&client->proto, &cont_req, &chunk_resp,
                                      TRANSFER_TIMEOUT_MS);
        if (ret < 0) {
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
            fprintf(stderr, "badgelink_client: chunk position mismatch\n");
            free(buffer);
            return -EIO;
        }

        size_t chunk_size = chunk->data.size;
        if (received + chunk_size > app_size)
            chunk_size = app_size - received;

        memcpy(buffer + received, chunk->data.bytes, chunk_size);
        received += chunk_size;
    }

    /* Send XferFinish to properly terminate transfer */
    badgelink_Request finish_req = badgelink_Request_init_zero;
    finish_req.which_req = badgelink_Request_xfer_ctrl_tag;
    finish_req.req.xfer_ctrl = badgelink_XferReq_XferFinish;
    badgelink_proto_request(&client->proto, &finish_req, &resp, CLIENT_TIMEOUT_MS);

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

    /* Finish upload */
    badgelink_Request finish_req = badgelink_Request_init_zero;
    finish_req.which_req = badgelink_Request_xfer_ctrl_tag;
    finish_req.req.xfer_ctrl = badgelink_XferReq_XferFinish;

    ret = badgelink_proto_request(&client->proto, &finish_req, &resp,
                                  TRANSFER_TIMEOUT_MS);
    if (ret < 0)
        return ret;

    if (resp.status_code != badgelink_StatusCode_StatusOk)
        return badgelink_status_to_errno(resp.status_code);

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
