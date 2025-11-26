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
                 cobs.c

# Nanopb (Protocol Buffers for C)
NANOPB_SRCS = pb_common.c pb_encode.c pb_decode.c badgelink.pb.c

# All sources
SRCS = $(CORE_SRCS) $(BADGELINK_SRCS) $(NANOPB_SRCS)
OBJS = $(SRCS:.c=.o)

# Headers
HEADERS = badgefs_ops.h badgefs_backend.h \
          badgefs_backend_badgelink.h badgelink_client.h \
          badgelink_proto.h badgelink_usb.h badgelink.pb.h \
          cobs.h pb.h pb_common.h pb_encode.h pb_decode.h

TARGET = badgefs

.PHONY: all clean install uninstall help

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

# Install to /usr/local/bin (requires sudo)
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/$(TARGET)

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
