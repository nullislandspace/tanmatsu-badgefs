/*
 * badgelink_tcp.c - TCP transport layer for BadgeLink protocol
 *
 * Connects to a badgelinkproxy instance over TCP, providing the same
 * read/write interface as badgelink_usb.c so the protocol layer can
 * use either transport.
 */

#include "badgelink_tcp.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

/*
 * Initialize TCP transport layer.
 */
int badgelink_tcp_init(struct badgelink_tcp *tcp, const char *host, int port)
{
    if (!tcp || !host)
        return -EINVAL;

    memset(tcp, 0, sizeof(*tcp));
    tcp->fd = -1;
    snprintf(tcp->host, sizeof(tcp->host), "%s", host);
    tcp->port = port;

    return 0;
}

/*
 * Open connection to the proxy.
 */
int badgelink_tcp_open(struct badgelink_tcp *tcp)
{
    if (!tcp)
        return -EINVAL;

    if (tcp->fd >= 0)
        return 0;

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", tcp->port);

    int ret = getaddrinfo(tcp->host, port_str, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "badgelink_tcp: getaddrinfo(%s:%d): %s\n",
                tcp->host, tcp->port, gai_strerror(ret));
        return -EIO;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        perror("badgelink_tcp: socket");
        freeaddrinfo(res);
        return -EIO;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "badgelink_tcp: connect(%s:%d): %s\n",
                tcp->host, tcp->port, strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -ENODEV;
    }

    freeaddrinfo(res);

    /* Disable Nagle's algorithm for low-latency request/response */
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    tcp->fd = fd;
    return 0;
}

/*
 * Close connection to the proxy.
 */
void badgelink_tcp_close(struct badgelink_tcp *tcp)
{
    if (!tcp)
        return;

    if (tcp->fd >= 0) {
        close(tcp->fd);
        tcp->fd = -1;
    }
}

/*
 * Cleanup TCP transport layer.
 */
void badgelink_tcp_cleanup(struct badgelink_tcp *tcp)
{
    if (!tcp)
        return;

    badgelink_tcp_close(tcp);
}

/*
 * Write data to the proxy.
 */
int badgelink_tcp_write(struct badgelink_tcp *tcp, const uint8_t *data,
                        size_t len, int timeout_ms)
{
    if (!tcp || tcp->fd < 0 || !data)
        return -EINVAL;

    if (len == 0)
        return 0;

    size_t total_sent = 0;

    while (total_sent < len) {
        struct pollfd pfd = {
            .fd = tcp->fd,
            .events = POLLOUT,
        };

        int poll_ret = poll(&pfd, 1, timeout_ms);
        if (poll_ret == 0)
            return -ETIMEDOUT;
        if (poll_ret < 0)
            return -EIO;

        ssize_t n = send(tcp->fd, data + total_sent, len - total_sent,
                         MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -EIO;
        }
        if (n == 0)
            return -EIO;

        total_sent += n;
    }

    return total_sent;
}

/*
 * Read data from the proxy.
 *
 * Mirrors badgelink_usb_read() behavior: reads in a loop with short
 * timeouts, returning all available data.
 */
#define TCP_READ_TIMEOUT_MS 100
#define TCP_READ_CHUNK_SIZE 4096

int badgelink_tcp_read(struct badgelink_tcp *tcp, uint8_t *buf,
                       size_t max_len, int timeout_ms)
{
    if (!tcp || tcp->fd < 0 || !buf)
        return -EINVAL;

    if (max_len == 0)
        return 0;

    (void)timeout_ms; /* Use short polling like USB transport */

    size_t total_read = 0;

    while (total_read < max_len) {
        struct pollfd pfd = {
            .fd = tcp->fd,
            .events = POLLIN,
        };

        int poll_ret = poll(&pfd, 1, TCP_READ_TIMEOUT_MS);

        if (poll_ret == 0)
            break; /* Timeout - return what we have */

        if (poll_ret < 0) {
            if (errno == EINTR)
                continue;
            return -EIO;
        }

        if (pfd.revents & (POLLERR | POLLHUP))
            return total_read > 0 ? (int)total_read : -EIO;

        size_t chunk = max_len - total_read;
        if (chunk > TCP_READ_CHUNK_SIZE)
            chunk = TCP_READ_CHUNK_SIZE;

        ssize_t n = recv(tcp->fd, buf + total_read, chunk, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return total_read > 0 ? (int)total_read : -EIO;
        }
        if (n == 0)
            break; /* Connection closed */

        total_read += n;
    }

    return total_read;
}

/*
 * Check if TCP connection is open.
 */
int badgelink_tcp_is_connected(struct badgelink_tcp *tcp)
{
    return tcp && tcp->fd >= 0;
}
