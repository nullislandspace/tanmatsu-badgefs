/*
 * badgelinkproxy.c - TCP-to-USB proxy for BadgeLink protocol
 *
 * Bridges a TCP socket to the Tanmatsu badge's USB bulk endpoints,
 * allowing remote access to the BadgeLink protocol over the network.
 *
 * The proxy is transparent: it forwards raw bytes in both directions
 * without interpreting the BadgeLink protocol. The COBS framing,
 * protobuf messages, and CRC32 checksums pass through unchanged.
 *
 * Based on the USB transport layer from badgefs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <libusb-1.0/libusb.h>

/* USB device identifiers for Tanmatsu badge */
#define BADGELINK_VID       0x16d0
#define BADGELINK_PID       0x0f9a
#define BADGELINK_INTERFACE 0
#define BADGELINK_EP_OUT    0x01
#define BADGELINK_EP_IN     0x81

/* Proxy settings */
#define DEFAULT_PORT        4003
#define USB_TIMEOUT_MS      5
#define USB_MAX_PACKET_SIZE 512
#define TCP_BUF_SIZE        65536

static volatile sig_atomic_t running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

/*
 * Open the Tanmatsu USB device.
 * Returns 0 on success, -1 on failure.
 */
static int usb_open(libusb_context *ctx, libusb_device_handle **handle)
{
    *handle = libusb_open_device_with_vid_pid(ctx, BADGELINK_VID, BADGELINK_PID);
    if (!*handle) {
        fprintf(stderr, "proxy: Tanmatsu not found (VID=%04x PID=%04x)\n",
                BADGELINK_VID, BADGELINK_PID);
        return -1;
    }

    if (libusb_kernel_driver_active(*handle, BADGELINK_INTERFACE) == 1) {
        if (libusb_detach_kernel_driver(*handle, BADGELINK_INTERFACE) < 0) {
            fprintf(stderr, "proxy: failed to detach kernel driver\n");
            libusb_close(*handle);
            *handle = NULL;
            return -1;
        }
    }

    int ret = libusb_claim_interface(*handle, BADGELINK_INTERFACE);
    if (ret < 0) {
        fprintf(stderr, "proxy: failed to claim interface: %s\n",
                libusb_strerror(ret));
        libusb_close(*handle);
        *handle = NULL;
        return -1;
    }

    return 0;
}

static void usb_close(libusb_device_handle *handle)
{
    if (handle) {
        libusb_release_interface(handle, BADGELINK_INTERFACE);
        libusb_close(handle);
    }
}

/*
 * Create a listening TCP socket.
 * Returns the socket fd, or -1 on failure.
 */
static int tcp_listen(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("proxy: socket");
        return -1;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("proxy: bind");
        close(sock);
        return -1;
    }

    if (listen(sock, 1) < 0) {
        perror("proxy: listen");
        close(sock);
        return -1;
    }

    return sock;
}

/*
 * Drain any stale data from the USB IN endpoint.
 */
static void usb_drain(libusb_device_handle *handle)
{
    uint8_t buf[USB_MAX_PACKET_SIZE];
    int transferred;

    for (int i = 0; i < 100; i++) {
        int ret = libusb_bulk_transfer(handle, BADGELINK_EP_IN,
                                       buf, sizeof(buf),
                                       &transferred, USB_TIMEOUT_MS);
        if (ret == LIBUSB_ERROR_TIMEOUT || transferred == 0)
            break;
    }
}

/*
 * Handle a single client connection.
 * Proxies data bidirectionally between the TCP client and USB device.
 */
static void handle_client(int client_fd, libusb_context *ctx)
{
    libusb_device_handle *handle = NULL;

    if (usb_open(ctx, &handle) < 0) {
        fprintf(stderr, "proxy: no USB device, closing client\n");
        close(client_fd);
        return;
    }

    fprintf(stderr, "proxy: USB device opened, proxying...\n");
    usb_drain(handle);

    uint8_t tcp_buf[TCP_BUF_SIZE];
    uint8_t usb_buf[TCP_BUF_SIZE];

    while (running) {
        struct pollfd pfd = {
            .fd = client_fd,
            .events = POLLIN,
        };

        /* Poll TCP with short timeout so we can also poll USB */
        int poll_ret = poll(&pfd, 1, USB_TIMEOUT_MS);

        /* TCP -> USB */
        if (poll_ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = recv(client_fd, tcp_buf, sizeof(tcp_buf), 0);
            if (n <= 0) {
                if (n == 0)
                    fprintf(stderr, "proxy: client disconnected\n");
                else
                    perror("proxy: recv");
                break;
            }

            /* Write to USB in chunks with delays, matching badgefs behavior */
            size_t sent = 0;
            while (sent < (size_t)n) {
                int transferred = 0;
                size_t chunk = (size_t)n - sent;
                if (chunk > 512)
                    chunk = 512;

                int ret = libusb_bulk_transfer(handle, BADGELINK_EP_OUT,
                                               tcp_buf + sent, chunk,
                                               &transferred, 5000);
                if (ret < 0) {
                    fprintf(stderr, "proxy: USB write failed: %s\n",
                            libusb_strerror(ret));
                    goto done;
                }
                sent += transferred;
            }
        }

        if (poll_ret > 0 && (pfd.revents & (POLLERR | POLLHUP)))
            break;

        /* USB -> TCP */
        size_t total_read = 0;
        for (;;) {
            int transferred = 0;
            size_t chunk = sizeof(usb_buf) - total_read;
            if (chunk > USB_MAX_PACKET_SIZE)
                chunk = USB_MAX_PACKET_SIZE;

            int ret = libusb_bulk_transfer(handle, BADGELINK_EP_IN,
                                           usb_buf + total_read, chunk,
                                           &transferred, USB_TIMEOUT_MS);
            if (transferred > 0)
                total_read += transferred;

            if (ret == LIBUSB_ERROR_TIMEOUT || transferred == 0)
                break;
            if (ret < 0) {
                fprintf(stderr, "proxy: USB read failed: %s\n",
                        libusb_strerror(ret));
                goto done;
            }
        }

        if (total_read > 0) {
            size_t sent = 0;
            while (sent < total_read) {
                ssize_t n = send(client_fd, usb_buf + sent,
                                 total_read - sent, MSG_NOSIGNAL);
                if (n <= 0) {
                    if (n == 0)
                        fprintf(stderr, "proxy: client disconnected\n");
                    else
                        perror("proxy: send");
                    goto done;
                }
                sent += n;
            }
        }
    }

done:
    usb_close(handle);
    close(client_fd);
    fprintf(stderr, "proxy: session ended\n");
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-p port] [-h]\n", prog);
    fprintf(stderr, "  -p port  TCP port to listen on (default: %d)\n",
            DEFAULT_PORT);
    fprintf(stderr, "  -h       Show this help\n");
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    int opt;

    while ((opt = getopt(argc, argv, "p:h")) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "proxy: invalid port: %s\n", optarg);
                return 1;
            }
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    libusb_context *ctx = NULL;
    int ret = libusb_init(&ctx);
    if (ret < 0) {
        fprintf(stderr, "proxy: libusb_init failed: %s\n",
                libusb_strerror(ret));
        return 1;
    }

    int listen_fd = tcp_listen(port);
    if (listen_fd < 0) {
        libusb_exit(ctx);
        return 1;
    }

    fprintf(stderr, "proxy: listening on 127.0.0.1:%d\n", port);

    while (running) {
        struct pollfd pfd = {
            .fd = listen_fd,
            .events = POLLIN,
        };

        int poll_ret = poll(&pfd, 1, 1000);
        if (poll_ret <= 0)
            continue;

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("proxy: accept");
            break;
        }

        fprintf(stderr, "proxy: client connected\n");
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        handle_client(client_fd, ctx);
    }

    close(listen_fd);
    libusb_exit(ctx);
    fprintf(stderr, "proxy: shutdown\n");
    return 0;
}
