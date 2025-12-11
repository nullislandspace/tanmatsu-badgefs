/*
 * badgelink_proto.c - BadgeLink protocol layer implementation
 *
 * Handles COBS framing, CRC32 calculation, and protobuf packet
 * serialization/deserialization.
 */

#include "badgelink_proto.h"
#include "cobs.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/* CRC32 lookup table - auto-generated from generate_crc32_table.py */
#include "crc32_table.h"

/*
 * Calculate CRC32 of data.
 */
uint32_t badgelink_crc32(const uint8_t *data, size_t len)
{
    return badgelink_crc32_final(badgelink_crc32_update(0xFFFFFFFF, data, len));
}

/*
 * Update running CRC32 calculation.
 */
uint32_t badgelink_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

/*
 * Finalize CRC32 calculation.
 */
uint32_t badgelink_crc32_final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFF;
}

/*
 * Initialize protocol layer.
 */
int badgelink_proto_init(struct badgelink_proto *proto)
{
    if (!proto)
        return -EINVAL;

    memset(proto, 0, sizeof(*proto));
    proto->serial_no = 1;

    return badgelink_usb_init(&proto->usb);
}

/*
 * Connect to the badge.
 */
int badgelink_proto_connect(struct badgelink_proto *proto)
{
    if (!proto)
        return -EINVAL;

    int ret = badgelink_usb_open(&proto->usb);
    if (ret < 0)
        return ret;

    /* Clear receive buffer */
    proto->rx_len = 0;
    proto->rx_pos = 0;

    /*
     * Send a null byte to delimit from any previous data the badge might
     * have seen. Then discard any pending data from the badge.
     */
    uint8_t null_byte = 0;
    badgelink_usb_write(&proto->usb, &null_byte, 1, 100);

    /* Drain any pending data from badge (ignore errors/timeouts) */
    uint8_t drain_buf[256];
    while (badgelink_usb_read(&proto->usb, drain_buf, sizeof(drain_buf), 50) > 0) {
        /* Keep reading until no more data */
    }

    return 0;
}

/*
 * Disconnect from the badge.
 */
void badgelink_proto_disconnect(struct badgelink_proto *proto)
{
    if (proto) {
        badgelink_usb_close(&proto->usb);
    }
}

/*
 * Cleanup protocol layer.
 */
void badgelink_proto_cleanup(struct badgelink_proto *proto)
{
    if (proto) {
        badgelink_usb_cleanup(&proto->usb);
    }
}

/*
 * Check if connected to badge.
 */
bool badgelink_proto_is_connected(struct badgelink_proto *proto)
{
    return proto && badgelink_usb_is_connected(&proto->usb);
}

/*
 * Send a COBS-encoded frame.
 * Frame format: [COBS-encoded payload][CRC32][0x00]
 */
static int send_frame(struct badgelink_proto *proto, const uint8_t *payload,
                      size_t payload_len)
{
    uint8_t frame[BADGELINK_FRAME_MAX];
    uint8_t cobs_input[BADGELINK_FRAME_MAX];
    size_t cobs_input_len;

    /* Build frame: payload + CRC32 */
    if (payload_len + 4 > sizeof(cobs_input))
        return -ENOMEM;

    memcpy(cobs_input, payload, payload_len);

    /* Append CRC32 (little-endian) */
    uint32_t crc = badgelink_crc32(payload, payload_len);
    cobs_input[payload_len + 0] = (crc >> 0) & 0xFF;
    cobs_input[payload_len + 1] = (crc >> 8) & 0xFF;
    cobs_input[payload_len + 2] = (crc >> 16) & 0xFF;
    cobs_input[payload_len + 3] = (crc >> 24) & 0xFF;
    cobs_input_len = payload_len + 4;

    /* COBS encode (adds trailing 0x00) */
    size_t frame_len = cobs_encode(frame, cobs_input, cobs_input_len);
    if (frame_len == 0 || frame_len > sizeof(frame))
        return -ENOMEM;

    fprintf(stderr, "DEBUG send_frame: payload_len=%zu frame_len=%zu crc=0x%08x\n",
            payload_len, frame_len, crc);
    /* Dump first bytes of protobuf payload for debugging */
    fprintf(stderr, "DEBUG send_frame: payload[%zu]:", payload_len);
    for (size_t i = 0; i < payload_len && i < 32; i++)
        fprintf(stderr, " %02x", payload[i]);
    if (payload_len > 32) fprintf(stderr, " ...");
    fprintf(stderr, "\n");

    /* Send frame */
    int ret = badgelink_usb_write(&proto->usb, frame, frame_len,
                                  BADGELINK_USB_TIMEOUT);
    if (ret < 0) {
        fprintf(stderr, "DEBUG send_frame: write failed: %d\n", ret);
        return ret;
    }

    if ((size_t)ret != frame_len) {
        fprintf(stderr, "badgelink_proto: short write: %d/%zu\n",
                ret, frame_len);
        return -EIO;
    }

    return 0;
}

/*
 * Read more data into receive buffer.
 */
static int fill_rx_buffer(struct badgelink_proto *proto, int timeout_ms)
{
    /* Compact buffer if needed */
    if (proto->rx_pos > 0) {
        if (proto->rx_len > proto->rx_pos) {
            memmove(proto->rx_buf, proto->rx_buf + proto->rx_pos,
                    proto->rx_len - proto->rx_pos);
        }
        proto->rx_len -= proto->rx_pos;
        proto->rx_pos = 0;
    }

    /* Read more data */
    size_t space = sizeof(proto->rx_buf) - proto->rx_len;
    if (space == 0)
        return -ENOMEM;

    int ret = badgelink_usb_read(&proto->usb,
                                 proto->rx_buf + proto->rx_len,
                                 space, timeout_ms);
    if (ret < 0) {
        fprintf(stderr, "DEBUG fill_rx_buffer: read failed: %d\n", ret);
        return ret;
    }

    if (ret > 0) {
        fprintf(stderr, "DEBUG fill_rx_buffer: read %d bytes, total now=%zu\n",
                ret, proto->rx_len + ret);
    }
    proto->rx_len += ret;
    return ret;
}

/*
 * Find a complete frame in receive buffer.
 * Returns pointer to frame start, sets frame_len, or NULL if no complete frame.
 */
static uint8_t *find_frame(struct badgelink_proto *proto, size_t *frame_len)
{
    /* Look for null terminator (end of COBS frame) */
    for (size_t i = proto->rx_pos; i < proto->rx_len; i++) {
        if (proto->rx_buf[i] == 0) {
            *frame_len = i - proto->rx_pos + 1;
            return proto->rx_buf + proto->rx_pos;
        }
    }
    return NULL;
}

/*
 * Receive a COBS-encoded frame.
 * Uses short read timeouts like the Python implementation to handle
 * badges that respond slowly or in multiple chunks.
 *
 * Python's recv_frame behavior:
 * - Overall timeout (e.g., 0.5s for chunk_timeout, 3s for normal)
 * - Loops calling read_all() which uses 5ms timeouts
 * - read_all() returns QUICKLY when no data, allowing fast polling
 *
 * We match this by using short read timeouts (50ms) and looping rapidly.
 */
static int recv_frame(struct badgelink_proto *proto, uint8_t *payload,
                      size_t max_len, size_t *payload_len, int timeout_ms)
{
    uint8_t *frame;
    size_t frame_len;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /*
     * Try to find a complete frame, reading more data if needed.
     *
     * Match Python's tight polling behavior:
     * - Use short read timeouts (100ms max) for rapid polling
     * - Rely on overall timeout to control total wait time
     * - Python's read_all() uses 5ms per USB read and returns quickly,
     *   then recv_frame loops rapidly checking for complete frames
     *
     * IMPORTANT: Don't use the full timeout_ms for reads! That would
     * consume the entire timeout budget on a single failed read, leaving
     * no time for retries.
     */
    while ((frame = find_frame(proto, &frame_len)) == NULL) {
        /* Check overall timeout */
        clock_gettime(CLOCK_MONOTONIC, &now);
        int elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                        (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= timeout_ms)
            return -ETIMEDOUT;

        /* Use very short read timeout - max 20ms for rapid polling like Python */
        int remaining_ms = timeout_ms - elapsed_ms;
        int read_timeout = remaining_ms < 20 ? remaining_ms : 20;
        if (read_timeout < 5)
            read_timeout = 5;

        int ret = fill_rx_buffer(proto, read_timeout);
        if (ret < 0 && ret != -ETIMEDOUT)
            return ret;
        /* ret == 0 or -ETIMEDOUT just means no data yet, keep trying */
    }

    fprintf(stderr, "DEBUG recv_frame: found frame at pos=%zu len=%zu, rx_len=%zu\n",
            proto->rx_pos, frame_len, proto->rx_len);
    /* Debug: show first and last few bytes of the frame */
    if (frame_len > 8) {
        fprintf(stderr, "DEBUG recv_frame: first 8: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7]);
        fprintf(stderr, "DEBUG recv_frame: last 8:  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                frame[frame_len-8], frame[frame_len-7], frame[frame_len-6], frame[frame_len-5],
                frame[frame_len-4], frame[frame_len-3], frame[frame_len-2], frame[frame_len-1]);
    }
    /* Dump full frame (up to 64 bytes) for CRC debug */
    fprintf(stderr, "DEBUG recv_frame: raw[%zu]:", frame_len);
    for (size_t i = 0; i < frame_len && i < 64; i++)
        fprintf(stderr, " %02x", frame[i]);
    if (frame_len > 64) fprintf(stderr, " ...");
    fprintf(stderr, "\n");

    /* COBS decode */
    uint8_t decoded[BADGELINK_FRAME_MAX];
    size_t decoded_len = cobs_decode(decoded, frame, frame_len);
    fprintf(stderr, "DEBUG recv_frame: decoded_len=%zu\n", decoded_len);
    /* Dump decoded data for CRC debug */
    fprintf(stderr, "DEBUG recv_frame: decoded[%zu]:", decoded_len);
    for (size_t i = 0; i < decoded_len && i < 64; i++)
        fprintf(stderr, " %02x", decoded[i]);
    if (decoded_len > 64) fprintf(stderr, " ...");
    fprintf(stderr, "\n");

    /* Consume frame from buffer */
    proto->rx_pos += frame_len;

    /* Check minimum length (at least CRC32) */
    if (decoded_len < 4) {
        fprintf(stderr, "badgelink_proto: frame too short: %zu\n", decoded_len);
        return -EINVAL;
    }

    /* Verify CRC32 */
    size_t data_len = decoded_len - 4;
    uint32_t recv_crc = ((uint32_t)decoded[data_len + 0] << 0) |
                        ((uint32_t)decoded[data_len + 1] << 8) |
                        ((uint32_t)decoded[data_len + 2] << 16) |
                        ((uint32_t)decoded[data_len + 3] << 24);
    uint32_t calc_crc = badgelink_crc32(decoded, data_len);

    if (recv_crc != calc_crc) {
        fprintf(stderr, "badgelink_proto: CRC mismatch: recv=%08x calc=%08x data_len=%zu\n",
                recv_crc, calc_crc, data_len);
        fprintf(stderr, "badgelink_proto: CRC bytes at decoded[%zu]: %02x %02x %02x %02x\n",
                data_len, decoded[data_len], decoded[data_len+1],
                decoded[data_len+2], decoded[data_len+3]);
        fprintf(stderr, "badgelink_proto: first 16 decoded: ");
        for (size_t i = 0; i < 16 && i < decoded_len; i++)
            fprintf(stderr, "%02x ", decoded[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "badgelink_proto: last 16 decoded: ");
        for (size_t i = decoded_len > 16 ? decoded_len - 16 : 0; i < decoded_len; i++)
            fprintf(stderr, "%02x ", decoded[i]);
        fprintf(stderr, "\n");
        return -EIO;
    }

    /* Copy payload */
    if (data_len > max_len)
        return -ENOMEM;

    memcpy(payload, decoded, data_len);
    *payload_len = data_len;

    return 0;
}

/*
 * Send a raw packet.
 */
int badgelink_proto_send_packet(struct badgelink_proto *proto,
                                badgelink_Packet *packet)
{
    if (!proto || !packet)
        return -EINVAL;

    /* Encode packet to protobuf */
    uint8_t pb_buf[badgelink_Packet_size];
    pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));

    if (!pb_encode(&stream, badgelink_Packet_fields, packet)) {
        fprintf(stderr, "badgelink_proto: pb_encode failed: %s\n",
                PB_GET_ERROR(&stream));
        return -EINVAL;
    }

    /* Send as frame */
    return send_frame(proto, pb_buf, stream.bytes_written);
}

/*
 * Receive a raw packet.
 */
int badgelink_proto_recv_packet(struct badgelink_proto *proto,
                                badgelink_Packet *packet,
                                int timeout_ms)
{
    if (!proto || !packet)
        return -EINVAL;

    /* Receive frame */
    uint8_t pb_buf[badgelink_Packet_size];
    size_t pb_len;

    int ret = recv_frame(proto, pb_buf, sizeof(pb_buf), &pb_len, timeout_ms);
    if (ret < 0)
        return ret;

    /* Decode packet from protobuf */
    pb_istream_t stream = pb_istream_from_buffer(pb_buf, pb_len);

    memset(packet, 0, sizeof(*packet));
    if (!pb_decode(&stream, badgelink_Packet_fields, packet)) {
        fprintf(stderr, "badgelink_proto: pb_decode failed: %s\n",
                PB_GET_ERROR(&stream));
        return -EINVAL;
    }

    return 0;
}

/*
 * Synchronize with the badge.
 * Retries up to 3 times like the Python implementation.
 */
int badgelink_proto_sync(struct badgelink_proto *proto)
{
    if (!proto)
        return -EINVAL;

    /* Use a random-ish serial number like Python does */
    proto->serial_no = (uint32_t)(time(NULL) ^ getpid()) & 0xFFFFFFFF;

    int last_ret = -ETIMEDOUT;
    for (int tries = 0; tries < 3; tries++) {
        /* Clear receive buffer before each attempt */
        proto->rx_len = 0;
        proto->rx_pos = 0;

        /* Drain any pending data */
        uint8_t drain_buf[256];
        while (badgelink_usb_read(&proto->usb, drain_buf, sizeof(drain_buf), 50) > 0) {
            /* Keep reading */
        }

        /* Send sync packet */
        badgelink_Packet sync_packet = badgelink_Packet_init_zero;
        sync_packet.serial = proto->serial_no;
        sync_packet.which_packet = badgelink_Packet_sync_tag;
        sync_packet.packet.sync = true;

        int ret = badgelink_proto_send_packet(proto, &sync_packet);
        if (ret < 0) {
            last_ret = ret;
            continue;
        }

        /* Wait for sync response with short timeout */
        badgelink_Packet resp_packet;
        ret = badgelink_proto_recv_packet(proto, &resp_packet, 500);
        if (ret < 0) {
            last_ret = ret;
            continue;
        }

        /* Verify it's a sync response with matching serial */
        if (resp_packet.which_packet != badgelink_Packet_sync_tag) {
            fprintf(stderr, "badgelink_proto: sync: expected sync response, got %d\n",
                    resp_packet.which_packet);
            last_ret = -EINVAL;
            continue;
        }

        if (resp_packet.serial != proto->serial_no) {
            fprintf(stderr, "badgelink_proto: sync: serial mismatch: got %lu, expected %lu\n",
                    (unsigned long)resp_packet.serial, (unsigned long)proto->serial_no);
            last_ret = -EINVAL;
            continue;
        }

        /* Success - update serial number */
        proto->serial_no = resp_packet.serial + 1;
        return 0;
    }

    fprintf(stderr, "badgelink_proto: sync failed after 3 attempts\n");
    return last_ret;
}

/*
 * Send a request and receive response.
 * Uses same serial number for all retries (like Python implementation).
 * Does NOT resync on retry - late responses are handled by serial matching.
 */
int badgelink_proto_request(struct badgelink_proto *proto,
                            badgelink_Request *req,
                            badgelink_Response *resp,
                            int timeout_ms)
{
    if (!proto || !req || !resp)
        return -EINVAL;

    int last_ret = -EIO;

    /* Use same serial for ALL retries (like Python) */
    uint32_t serial = proto->serial_no++;

    /*
     * Match Python's simple_request behavior:
     * - Send request, wait for response
     * - On timeout, just resend (don't drain USB - response might still be coming)
     * - Badge ignores duplicate requests or sends same response
     * - Keep any buffered data - it might contain the response
     */
    for (int tries = 0; tries < 5; tries++) {
        if (tries > 0) {
            fprintf(stderr, "DEBUG proto_request: retry %d (serial=%lu)\n",
                    tries, (unsigned long)serial);
            /*
             * DON'T drain USB on retry! The response might still be in transit.
             * Python doesn't drain - it just resends and waits.
             * By draining, we throw away the response that was coming.
             *
             * Small delay before retry to give badge time to finish processing
             * the previous request if it was just slow.
             */
            usleep(50000);  /* 50ms delay before retry */
        }

        /* Build request packet with consistent serial */
        badgelink_Packet req_packet = badgelink_Packet_init_zero;
        req_packet.serial = serial;
        req_packet.which_packet = badgelink_Packet_request_tag;
        memcpy(&req_packet.packet.request, req, sizeof(*req));

        fprintf(stderr, "DEBUG proto_request: sending req serial=%lu which_req=%d try=%d\n",
                (unsigned long)req_packet.serial, req->which_req, tries);

        /* Send request */
        int ret = badgelink_proto_send_packet(proto, &req_packet);
        if (ret < 0) {
            fprintf(stderr, "DEBUG proto_request: send failed: %d\n", ret);
            last_ret = ret;
            continue;
        }
        fprintf(stderr, "DEBUG proto_request: send succeeded\n");

        /* Receive response - keep trying until we get the right one or timeout */
        badgelink_Packet resp_packet;
        for (int recv_tries = 0; recv_tries < 10; recv_tries++) {
            fprintf(stderr, "DEBUG proto_request: waiting for response (recv_try=%d timeout=%dms)\n",
                    recv_tries, timeout_ms);
            ret = badgelink_proto_recv_packet(proto, &resp_packet, timeout_ms);
            if (ret < 0) {
                fprintf(stderr, "DEBUG proto_request: recv failed: %d\n", ret);
                last_ret = ret;
                break;  /* Timeout - will retry sending */
            }
            fprintf(stderr, "DEBUG proto_request: recv got packet serial=%lu which=%d\n",
                    (unsigned long)resp_packet.serial, resp_packet.which_packet);

            /* Check if badge wants us to re-sync */
            if (resp_packet.which_packet == badgelink_Packet_sync_tag) {
                fprintf(stderr, "DEBUG proto_request: badge requested sync\n");
                /* Answer the sync and try receiving again */
                badgelink_Packet sync_resp = badgelink_Packet_init_zero;
                sync_resp.serial = resp_packet.serial;
                sync_resp.which_packet = badgelink_Packet_sync_tag;
                sync_resp.packet.sync = true;
                badgelink_proto_send_packet(proto, &sync_resp);
                continue;
            }

            /* Got a response - check if it matches our serial */
            if (resp_packet.which_packet == badgelink_Packet_response_tag &&
                resp_packet.serial == serial) {
                /* Success - copy response */
                fprintf(stderr, "DEBUG proto_request: success! status=%d\n",
                        resp_packet.packet.response.status_code);
                memcpy(resp, &resp_packet.packet.response, sizeof(*resp));
                return 0;
            }

            /* Wrong serial - stale response from previous request, skip it */
            fprintf(stderr, "DEBUG proto_request: stale response serial=%lu (expected %lu), skipping\n",
                    (unsigned long)resp_packet.serial, (unsigned long)serial);
            /* Continue receiving - our response might be next */
        }
    }

    fprintf(stderr, "DEBUG proto_request: failed after all retries: %d\n", last_ret);
    return last_ret;
}

/*
 * Re-sync with badge if connection seems stale.
 * Called after errors or before new operations.
 */
int badgelink_proto_resync(struct badgelink_proto *proto)
{
    fprintf(stderr, "DEBUG proto_resync: re-syncing with badge\n");
    return badgelink_proto_sync(proto);
}
