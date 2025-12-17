/*
 * badgelink_proto.h - BadgeLink protocol layer
 *
 * Handles COBS framing, CRC32 calculation, and protobuf packet
 * serialization/deserialization.
 */

#ifndef BADGELINK_PROTO_H
#define BADGELINK_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "badgelink_usb.h"
#include "badgelink.pb.h"

/* Protocol constants */
#define BADGELINK_CHUNK_MAX     4096
#define BADGELINK_PATH_MAX      1023
#define BADGELINK_FRAME_MAX     8192
#define BADGELINK_RX_BUF_SIZE   16384

/* Protocol connection state */
struct badgelink_proto {
    struct badgelink_usb usb;
    uint64_t serial_no;     /* Next serial number to use */
    uint8_t rx_buf[BADGELINK_RX_BUF_SIZE];
    size_t rx_len;          /* Bytes currently in rx_buf */
    size_t rx_pos;          /* Current read position in rx_buf */
    bool sync_occurred;     /* Set when badge-initiated sync answered */
};

/*
 * Calculate CRC32 of data.
 * Uses standard CRC-32 polynomial (0xEDB88320).
 */
uint32_t badgelink_crc32(const uint8_t *data, size_t len);

/*
 * Update running CRC32 calculation.
 * Use with initial crc value of 0xFFFFFFFF.
 */
uint32_t badgelink_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/*
 * Finalize CRC32 calculation.
 */
uint32_t badgelink_crc32_final(uint32_t crc);

/*
 * Initialize protocol layer.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_proto_init(struct badgelink_proto *proto);

/*
 * Connect to the badge.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_proto_connect(struct badgelink_proto *proto);

/*
 * Disconnect from the badge.
 */
void badgelink_proto_disconnect(struct badgelink_proto *proto);

/*
 * Cleanup protocol layer.
 */
void badgelink_proto_cleanup(struct badgelink_proto *proto);

/*
 * Check if connected to badge.
 */
bool badgelink_proto_is_connected(struct badgelink_proto *proto);

/*
 * Synchronize with the badge.
 * Sends a sync packet and waits for acknowledgment.
 * This resets the serial number sequence.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_proto_sync(struct badgelink_proto *proto);

/*
 * Re-sync with badge if connection seems stale.
 * Called after errors or before new operations.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_proto_resync(struct badgelink_proto *proto);

/*
 * Send a request packet and receive response.
 * Automatically handles serial numbers and framing.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_proto_request(struct badgelink_proto *proto,
                            badgelink_Request *req,
                            badgelink_Response *resp,
                            int timeout_ms);

/*
 * Send a raw packet.
 * For advanced use (e.g., sending sync packets).
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_proto_send_packet(struct badgelink_proto *proto,
                                badgelink_Packet *packet);

/*
 * Receive a raw packet.
 * For advanced use.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_proto_recv_packet(struct badgelink_proto *proto,
                                badgelink_Packet *packet,
                                int timeout_ms);

#endif /* BADGELINK_PROTO_H */
