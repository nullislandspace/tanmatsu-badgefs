/*
 * badgelink_tcp.h - TCP transport layer for BadgeLink protocol
 *
 * This provides network communication with the Tanmatsu badge
 * via the badgelinkproxy TCP-to-USB proxy.
 *
 * The interface mirrors badgelink_usb.h so the proto layer can
 * use either transport interchangeably.
 */

#ifndef BADGELINK_TCP_H
#define BADGELINK_TCP_H

#include <stdint.h>
#include <stddef.h>

/* TCP connection state */
struct badgelink_tcp {
    int fd;
    char host[256];
    int port;
};

/*
 * Initialize TCP transport layer.
 * Must be called before any other TCP functions.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_tcp_init(struct badgelink_tcp *tcp, const char *host, int port);

/*
 * Open connection to the proxy.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_tcp_open(struct badgelink_tcp *tcp);

/*
 * Close connection to the proxy.
 */
void badgelink_tcp_close(struct badgelink_tcp *tcp);

/*
 * Cleanup TCP transport layer.
 */
void badgelink_tcp_cleanup(struct badgelink_tcp *tcp);

/*
 * Write data to the proxy.
 * Returns number of bytes written on success, negative error code on failure.
 */
int badgelink_tcp_write(struct badgelink_tcp *tcp, const uint8_t *data,
                        size_t len, int timeout_ms);

/*
 * Read data from the proxy.
 * Returns number of bytes read on success, negative error code on failure.
 * May return 0 if no data is available within timeout.
 */
int badgelink_tcp_read(struct badgelink_tcp *tcp, uint8_t *buf,
                       size_t max_len, int timeout_ms);

/*
 * Check if TCP connection is open.
 * Returns 1 if connected, 0 otherwise.
 */
int badgelink_tcp_is_connected(struct badgelink_tcp *tcp);

#endif /* BADGELINK_TCP_H */
