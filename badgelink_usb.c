/*
 * badgelink_usb.c - USB transport layer for BadgeLink protocol
 *
 * Implements low-level USB communication with the Tanmatsu badge
 * using libusb bulk transfers.
 */

#include "badgelink_usb.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

/*
 * Initialize USB transport layer.
 */
int badgelink_usb_init(struct badgelink_usb *usb)
{
    if (!usb)
        return -EINVAL;

    memset(usb, 0, sizeof(*usb));

    int ret = libusb_init(&usb->ctx);
    if (ret < 0) {
        fprintf(stderr, "badgelink_usb: libusb_init failed: %s\n",
                libusb_strerror(ret));
        return -EIO;
    }

    return 0;
}

/*
 * Open connection to the badge.
 */
int badgelink_usb_open(struct badgelink_usb *usb)
{
    if (!usb || !usb->ctx)
        return -EINVAL;

    /* Already open? */
    if (usb->handle) {
        return 0;
    }

    /* Find and open the device */
    usb->handle = libusb_open_device_with_vid_pid(usb->ctx,
                                                   BADGELINK_VID,
                                                   BADGELINK_PID);
    if (!usb->handle) {
        fprintf(stderr, "badgelink_usb: device not found (VID=%04x PID=%04x)\n",
                BADGELINK_VID, BADGELINK_PID);
        return -ENODEV;
    }

    /* Detach kernel driver if attached */
    if (libusb_kernel_driver_active(usb->handle, BADGELINK_INTERFACE) == 1) {
        int ret = libusb_detach_kernel_driver(usb->handle, BADGELINK_INTERFACE);
        if (ret < 0) {
            fprintf(stderr, "badgelink_usb: failed to detach kernel driver: %s\n",
                    libusb_strerror(ret));
            libusb_close(usb->handle);
            usb->handle = NULL;
            return -EIO;
        }
    }

    /* Claim the interface */
    int ret = libusb_claim_interface(usb->handle, BADGELINK_INTERFACE);
    if (ret < 0) {
        fprintf(stderr, "badgelink_usb: failed to claim interface %d: %s\n",
                BADGELINK_INTERFACE, libusb_strerror(ret));
        libusb_close(usb->handle);
        usb->handle = NULL;
        return -EIO;
    }

    usb->interface_claimed = 1;
    return 0;
}

/*
 * Close connection to the badge.
 */
void badgelink_usb_close(struct badgelink_usb *usb)
{
    if (!usb)
        return;

    if (usb->handle) {
        if (usb->interface_claimed) {
            libusb_release_interface(usb->handle, BADGELINK_INTERFACE);
            usb->interface_claimed = 0;
        }
        libusb_close(usb->handle);
        usb->handle = NULL;
    }
}

/*
 * Cleanup USB transport layer.
 */
void badgelink_usb_cleanup(struct badgelink_usb *usb)
{
    if (!usb)
        return;

    badgelink_usb_close(usb);

    if (usb->ctx) {
        libusb_exit(usb->ctx);
        usb->ctx = NULL;
    }
}

/*
 * Write data to the badge.
 *
 * Match Python's behavior: write all data in a loop, with 10ms delay
 * after each write. Python's pyUSB handles bulk transfer segmentation
 * internally, so we let libusb do the same.
 */
int badgelink_usb_write(struct badgelink_usb *usb, const uint8_t *data,
                        size_t len, int timeout_ms)
{
    if (!usb || !usb->handle || !data)
        return -EINVAL;

    if (len == 0)
        return 0;

    size_t total_sent = 0;

    fprintf(stderr, "DEBUG usb_write: sending %zu bytes\n", len);
    while (total_sent < len) {
        int transferred = 0;
        int ret = libusb_bulk_transfer(usb->handle, BADGELINK_EP_OUT,
                                       (unsigned char *)data + total_sent,
                                       len - total_sent, &transferred, timeout_ms);

        if (ret < 0) {
            if (ret == LIBUSB_ERROR_TIMEOUT)
                return -ETIMEDOUT;
            fprintf(stderr, "badgelink_usb: write failed at offset %zu: %s\n",
                    total_sent, libusb_strerror(ret));
            return -EIO;
        }

        total_sent += transferred;

        /* 10ms delay after each write like Python does */
        usleep(10000);
    }
    fprintf(stderr, "DEBUG usb_write: sent %zu bytes total\n", total_sent);

    return total_sent;
}

/*
 * Read data from the badge.
 *
 * IMPORTANT: This must match Python's read_all() behavior exactly:
 * - Read exactly wMaxPacketSize (32 bytes on this device) at a time
 * - Use very short timeout (5ms) for each read
 * - Loop until timeout with no data
 *
 * The badge's USB endpoint has wMaxPacketSize=32, not 64!
 */
#define USB_MAX_PACKET_SIZE 32  /* Actual wMaxPacketSize from device descriptor */
#define USB_READ_TIMEOUT_MS 5   /* Match Python's 5ms timeout */

int badgelink_usb_read(struct badgelink_usb *usb, uint8_t *buf,
                       size_t max_len, int timeout_ms)
{
    if (!usb || !usb->handle || !buf)
        return -EINVAL;

    if (max_len == 0)
        return 0;

    size_t total_read = 0;

    /*
     * Match Python's read_all() EXACTLY:
     *   while True:
     *       try:
     *           new_data = bytes(ep_in.read(wMaxPacketSize, 5))
     *           data += new_data
     *       except usb.USBError:
     *           break
     *   return data
     *
     * CRITICAL: Python uses 5ms timeout for EVERY read, not just subsequent ones.
     * This allows fast polling when no data is available.
     * The timeout_ms parameter is ignored - we always use USB_READ_TIMEOUT_MS.
     */
    (void)timeout_ms;  /* Ignored - always use 5ms like Python */

    while (total_read < max_len) {
        int transferred = 0;

        /* Read exactly one packet's worth */
        size_t chunk_size = max_len - total_read;
        if (chunk_size > USB_MAX_PACKET_SIZE)
            chunk_size = USB_MAX_PACKET_SIZE;

        /* Always use 5ms timeout like Python's read_all() */
        int ret = libusb_bulk_transfer(usb->handle, BADGELINK_EP_IN,
                                       buf + total_read,
                                       chunk_size,
                                       &transferred,
                                       USB_READ_TIMEOUT_MS);

        /* Always count transferred bytes, even on timeout */
        if (transferred > 0) {
            total_read += transferred;
        }

        if (ret == LIBUSB_ERROR_TIMEOUT) {
            /* Match Python: timeout means return what we have */
            break;
        }

        if (ret < 0) {
            fprintf(stderr, "badgelink_usb: read failed: %s\n",
                    libusb_strerror(ret));
            return -EIO;
        }

        /*
         * IMPORTANT: Do NOT break on short reads!
         * Python's read_all() only breaks on USB timeout errors, not short reads.
         * A short read just means one packet was short, there may be more data
         * (including the null byte frame terminator) in subsequent packets.
         *
         * Only break conditions:
         * 1. LIBUSB_ERROR_TIMEOUT (handled above)
         * 2. transferred == 0 with no error (shouldn't happen, but safe)
         */
        if (transferred == 0) {
            break;
        }
    }

    return total_read;
}

/*
 * Check if USB connection is open.
 */
int badgelink_usb_is_connected(struct badgelink_usb *usb)
{
    return usb && usb->handle && usb->interface_claimed;
}
