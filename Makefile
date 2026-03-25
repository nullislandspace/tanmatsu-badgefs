# BadgeFS - FUSE Filesystem Framework with BadgeLink backend
# Requires: libfuse3-dev libusb-1.0-0-dev
# Install: sudo apt install libfuse3-dev libusb-1.0-0-dev

CC = gcc
CFLAGS = -Wall -Wextra -g -D_FILE_OFFSET_BITS=64 \
         $(shell pkg-config fuse3 --cflags) \
         $(shell pkg-config libusb-1.0 --cflags)
LDFLAGS = $(shell pkg-config fuse3 --libs) \
          $(shell pkg-config libusb-1.0 --libs) \
          -lpthread

# Core FUSE framework
CORE_SRCS = badgefs.c badgefs_ops.c

# BadgeLink backend (for connecting to badge)
BADGELINK_SRCS = badgefs_backend_badgelink.c \
                 badgelink_client.c \
                 badgelink_proto.c \
                 badgelink_usb.c \
                 badgelink_tcp.c \
                 cobs.c

# Nanopb (Protocol Buffers for C)
NANOPB_SRCS = pb_common.c pb_encode.c pb_decode.c badgelink.pb.c

# All sources
SRCS = $(CORE_SRCS) $(BADGELINK_SRCS) $(NANOPB_SRCS)
OBJS = $(SRCS:.c=.o)

# Headers
HEADERS = badgefs_ops.h badgefs_backend.h \
          badgefs_backend_badgelink.h badgelink_client.h \
          badgelink_proto.h badgelink_usb.h badgelink_tcp.h badgelink.pb.h \
          cobs.h pb.h pb_common.h pb_encode.h pb_decode.h

TARGET = badgefs

# BadgeLink proxy (TCP-to-USB, no FUSE dependency)
PROXY_TARGET = badgelinkproxy
PROXY_CFLAGS = -Wall -Wextra -g $(shell pkg-config libusb-1.0 --cflags)
PROXY_LDFLAGS = $(shell pkg-config libusb-1.0 --libs)

# RFC2217 proxy (telnet COM port to serial, no external dependencies)
RFC2217_TARGET = rfc2217proxy
RFC2217_CFLAGS = -Wall -Wextra -g

# ESP32-P4 reset/bootloader test tool (for development)
TEST_RESET_TARGET = test_reset
TEST_RESET_CFLAGS = -Wall -g

.PHONY: all clean install uninstall help

all: $(TARGET) $(PROXY_TARGET) $(RFC2217_TARGET) $(TEST_RESET_TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(PROXY_TARGET): badgelinkproxy.c
	$(CC) $(PROXY_CFLAGS) $< -o $@ $(PROXY_LDFLAGS)

$(RFC2217_TARGET): rfc2217proxy.c
	$(CC) $(RFC2217_CFLAGS) $< -o $@

$(TEST_RESET_TARGET): test_reset.c stub_esp32p4.h
	$(CC) $(TEST_RESET_CFLAGS) $< -o $@

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(PROXY_TARGET) $(RFC2217_TARGET) $(TEST_RESET_TARGET)

# Install to /usr/local/bin (requires sudo)
install: $(TARGET) $(PROXY_TARGET) $(RFC2217_TARGET) $(TEST_RESET_TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 755 $(PROXY_TARGET) /usr/local/bin/
	install -m 755 $(RFC2217_TARGET) /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/$(TARGET)
	rm -f /usr/local/bin/$(PROXY_TARGET)
	rm -f /usr/local/bin/$(RFC2217_TARGET)

mount: $(TARGET)
	mkdir -p /tmp/mnt
	./badgefs /tmp/mnt

umount: $(TARGET)
	./badgefs -u /tmp/mnt

# Usage help
help:
	@echo "BadgeFS - FUSE Filesystem for Tanmatsu Badge"
	@echo ""
	@echo "Build targets:"
	@echo "  make          - Build the filesystem"
	@echo "  make clean    - Remove build files"
	@echo "  make install  - Install to /usr/local/bin"
	@echo ""
	@echo "Prerequisites:"
	@echo "  sudo apt install libfuse3-dev libusb-1.0-0-dev"
	@echo ""
	@echo "Usage:"
	@echo "  mkdir /tmp/mnt"
	@echo "  ./badgefs /tmp/mnt            # Mount filesystem"
	@echo "  ./badgefs -f /tmp/mnt         # Mount in foreground"
	@echo "  ./badgefs -d -f /tmp/mnt      # Mount with debug output"
	@echo "  fusermount -u /tmp/mnt        # Unmount filesystem"
	@echo ""
	@echo "Filesystem layout:"
	@echo "  /sd     - SD card on badge"
	@echo "  /int    - Internal memory"
	@echo "  /appfs  - Application filesystem (apps as <slug>.bin files)"
