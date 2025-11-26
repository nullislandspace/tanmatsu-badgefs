/*
 * badgelink_usb.h - USB transport layer for BadgeLink protocol
 *
 * This provides low-level USB communication with the Tanmatsu badge
 * using libusb.
 */

#ifndef BADGELINK_USB_H
#define BADGELINK_USB_H

#include <stdint.h>
#include <stddef.h>
#include <libusb-1.0/libusb.h>

/* USB device identifiers for Tanmatsu badge */
#define BADGELINK_VID 0x16d0  /* OpenMoko */
#define BADGELINK_PID 0x0f9a  /* Tanmatsu badge */

/* USB interface and endpoint configuration for Tanmatsu */
#define BADGELINK_INTERFACE 0
#define BADGELINK_EP_OUT    0x01
#define BADGELINK_EP_IN     0x81

/* Default timeout for USB operations (milliseconds) */
#define BADGELINK_USB_TIMEOUT 5000

/* USB connection state */
struct badgelink_usb {
    libusb_context *ctx;
    libusb_device_handle *handle;
    int interface_claimed;
};

/*
 * Initialize USB transport layer.
 * Must be called before any other USB functions.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_usb_init(struct badgelink_usb *usb);

/*
 * Open connection to the badge.
 * Searches for a connected Tanmatsu badge and opens it.
 * Returns 0 on success, negative error code on failure.
 */
int badgelink_usb_open(struct badgelink_usb *usb);

/*
 * Close connection to the badge.
 * Releases the interface and closes the device.
 */
void badgelink_usb_close(struct badgelink_usb *usb);

/*
 * Cleanup USB transport layer.
 * Must be called after closing the connection.
 */
void badgelink_usb_cleanup(struct badgelink_usb *usb);

/*
 * Write data to the badge.
 * Returns number of bytes written on success, negative error code on failure.
 */
int badgelink_usb_write(struct badgelink_usb *usb, const uint8_t *data,
                        size_t len, int timeout_ms);

/*
 * Read data from the badge.
 * Returns number of bytes read on success, negative error code on failure.
 * May return 0 if no data is available within timeout.
 */
int badgelink_usb_read(struct badgelink_usb *usb, uint8_t *buf,
                       size_t max_len, int timeout_ms);

/*
 * Check if USB connection is open.
 * Returns 1 if connected, 0 otherwise.
 */
int badgelink_usb_is_connected(struct badgelink_usb *usb);

#endif /* BADGELINK_USB_H */
