# BadgeFS - FUSE Filesystem for Tanmatsu Badge

BadgeFS mounts your Tanmatsu badge's storage as a local filesystem via USB. Access the SD card, internal flash, and AppFS applications using standard file operations.

## Quick Start

```bash
# Install dependencies
sudo apt install libfuse3-dev libusb-1.0-0-dev attr

# Build
make

# Connect your Tanmatsu badge via USB

# Mount (single-threaded foreground mode recommended)
mkdir -p /tmp/badge
./badgefs -f -s /tmp/badge

# Access badge storage
ls /tmp/badge/sd/          # SD card
ls /tmp/badge/int/         # Internal flash
ls /tmp/badge/appfs/       # Installed applications

# Unmount
fusermount -u /tmp/badge
```

## Virtual Filesystem Structure

```
/tmp/badge/
├── sd/         SD card (/sd on device)
├── int/        Internal flash (/int on device)
└── appfs/      AppFS applications (as <slug>.bin files)
```

## Supported Operations

| Operation | SD/Int | AppFS |
|-----------|--------|-------|
| List files | Yes | Yes |
| Read files | Yes | Yes |
| Write files | Yes | Yes |
| Create files | Yes | Yes |
| Delete files | Yes | Yes |
| Create directories | Yes | No |
| Delete directories | Yes | No |
| Read attributes (xattr) | No | Yes |
| Set attributes (xattr) | No | Yes |

## AppFS Extended Attributes

AppFS applications have metadata (title, version) accessible via extended attributes:

```bash
# Read application metadata
getfattr -d /tmp/badge/appfs/team.badge.nofrendo.bin
# Output:
# user.appfs.title="Nofrendo NES emulator"
# user.appfs.version="1"

# Set application title
setfattr -n user.appfs.title -v "My Custom Title" /tmp/badge/appfs/myapp.bin

# Set application version
setfattr -n user.appfs.version -v "2" /tmp/badge/appfs/myapp.bin
```

### Attribute Behavior

- **On new upload:** Default title="Application", version=0
- **On overwrite:** Existing title/version are preserved
- **On setxattr:** Changes are immediately uploaded to badge

## Examples

```bash
# Copy file to SD card
cp myfile.txt /tmp/badge/sd/

# Download file from badge
cp /tmp/badge/sd/config.toml ./

# Upload new application
cp myapp.bin /tmp/badge/appfs/com.example.myapp.bin

# Update existing app (preserves title/version)
cp myapp_v2.bin /tmp/badge/appfs/com.example.myapp.bin

# Change app title after upload
setfattr -n user.appfs.title -v "My Application" /tmp/badge/appfs/com.example.myapp.bin

# Delete application
rm /tmp/badge/appfs/com.example.myapp.bin

# Create directory on SD
mkdir /tmp/badge/sd/mydir

# Delete directory
rmdir /tmp/badge/sd/mydir
```

## Command Line Options

```
./badgefs [options] <mountpoint>

Options:
  -f          Foreground mode (recommended)
  -s          Single-threaded mode (recommended)
  -d          Debug mode (verbose output)
  -o <opts>   FUSE mount options
  -h          Show help
```

## Dependencies

- libfuse3-dev
- libusb-1.0-0-dev
- attr (for getfattr/setfattr tools)

## Troubleshooting

**Permission denied on USB:**
```bash
# Add udev rule for Tanmatsu badge
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="16d0", ATTR{idProduct}=="0f9a", MODE="0666"' | \
    sudo tee /etc/udev/rules.d/99-tanmatsu.rules
sudo udevadm control --reload-rules
# Reconnect badge
```

**Mount hangs or times out:**
- Ensure badge is connected and powered on
- Try unplugging and reconnecting the USB cable
- Check `dmesg` for USB errors

**Files not appearing:**
- Use `-f -s` flags for single-threaded foreground mode
- Check `/tmp/badgefs_debug.log` for errors

## Technical Details

- **USB Device:** VID 0x16d0, PID 0x0f9a
- **Protocol:** BadgeLink (COBS framing, CRC32, protobuf)
- **FUSE Version:** 3.1+
