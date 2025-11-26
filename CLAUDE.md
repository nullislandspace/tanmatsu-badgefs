# Claude Implementation Log - BadgeFS FUSE Filesystem

## Project Overview

BadgeFS is a FUSE3 virtual filesystem framework with a pluggable backend architecture. The default backend stores data in memory; custom backends can be implemented for network storage, databases, or device communication.

## Requirements

- Create/delete directories
- Upload/download files (read/write)
- Full directory hierarchy support
- Thread-safe operation
- Pluggable storage backend

## Implementation Status

**Status:** COMPLETE AND TESTED

### Phase 1: Research
- [x] Research FUSE architecture and API
- [x] Review libfuse3 documentation
- [x] Study example implementations (passthrough.c, tutorials)

### Phase 2: Framework Design
- [x] Design pluggable backend architecture
- [x] Define abstract storage interface
- [x] Plan thread-safety strategy (pthread_rwlock)

### Phase 3: Implementation
- [x] Create Makefile with pkg-config
- [x] Implement abstract backend interface (badgefs_backend.h)
- [x] Implement in-memory backend (badgefs_backend_mem.c)
- [x] Implement FUSE operations wrapper (badgefs_ops.c)
- [x] Create main entry point (badgefs.c)

### Phase 4: Documentation
- [x] Write comprehensive FUSE.md guide (~26 KB)
- [x] Create README.md quick start guide

### Phase 5: Testing
- [x] Build verification
- [x] Functional testing:
  - [x] Mount/unmount
  - [x] Create directories (mkdir)
  - [x] Nested directories (mkdir -p a/b/c)
  - [x] Create files
  - [x] Write to files
  - [x] Read from files
  - [x] List directories (ls)
  - [x] File attributes (stat)

## Architecture

```
badgefs.c              Main entry point, argument parsing
    │
    ▼
badgefs_ops.c          FUSE callbacks (delegates to backend)
    │
    ▼
badgefs_backend.h      Abstract interface (function pointers)
    │
    ▼
badgefs_backend_mem.c  Default in-memory implementation
                       (replace with custom backend)
```

## Files Created

| File | Lines | Purpose |
|------|-------|---------|
| Makefile | 51 | Build system with pkg-config |
| badgefs.c | 110 | Main entry, help, version |
| badgefs_ops.h | 65 | FUSE callback declarations |
| badgefs_ops.c | 198 | FUSE callback implementations |
| badgefs_backend.h | 81 | Abstract storage interface |
| badgefs_backend_mem.h | 41 | Memory backend declarations |
| badgefs_backend_mem.c | 590 | Full in-memory filesystem |
| FUSE.md | ~700 | Comprehensive implementation guide |
| README.md | 60 | Quick start guide |

## Implemented FUSE Operations

| Operation | Purpose |
|-----------|---------|
| init | Initialize backend on mount |
| destroy | Cleanup on unmount |
| getattr | Get file/directory attributes (stat) |
| readdir | List directory contents |
| mkdir | Create directory |
| rmdir | Remove empty directory |
| create | Create new file |
| unlink | Delete file |
| open | Open file for I/O |
| release | Close file |
| read | Read file contents |
| write | Write to file |
| truncate | Resize file |
| rename | Move/rename file or directory |
| chmod | Change permissions |
| chown | Change owner/group |
| utimens | Update timestamps |

## Technical Specifications

- **FUSE Version:** 3.1+ (FUSE_USE_VERSION 31)
- **Thread Safety:** pthread_rwlock (read/write locks)
- **Storage:** In-memory tree structure with linked-list children
- **Path Handling:** Full nested directory support
- **Error Handling:** Returns -errno (POSIX convention)

## Build & Test Results

```
Build: SUCCESS
Binary: badgefs (linked with -lfuse3 -lpthread)

Test Results:
- mkdir: PASS
- nested mkdir: PASS
- file create: PASS
- file write: PASS
- file read: PASS
- directory listing: PASS
- unmount: PASS
```

## Usage

```bash
# Build
make

# Mount
./badgefs /tmp/mnt

# Debug mode
./badgefs -d -f /tmp/mnt

# Unmount
fusermount -u /tmp/mnt
```

## Extending with Custom Backend

To add a custom storage backend (e.g., for BadgeLink communication):

1. Copy `badgefs_backend_mem.c` as template
2. Implement all functions in `struct badgefs_backend`
3. Replace backend initialization in `badgefs.c`:
   ```c
   badgefs_set_backend(my_custom_backend_get());
   ```
4. Update Makefile SRCS to include new file

## Dependencies

- libfuse3-dev (`sudo apt install libfuse3-dev`)
- libusb-1.0-0-dev (`sudo apt install libusb-1.0-0-dev`)
- pkg-config
- gcc
- pthread

---

## BadgeLink Backend Implementation (2025-11-26)

### Overview

Added a BadgeLink USB backend to connect BadgeFS to the Tanmatsu badge hardware.

### Files Added

| File | Purpose |
|------|---------|
| badgefs_backend_badgelink.c | BadgeLink backend (path routing, write buffering) |
| badgefs_backend_badgelink.h | Backend header |
| badgelink_client.c | High-level client API (stat, list, upload, download) |
| badgelink_client.h | Client header |
| badgelink_proto.c | Protocol layer (COBS framing, CRC32, protobuf) |
| badgelink_proto.h | Protocol header |
| badgelink_usb.c | USB transport layer (libusb bulk transfers) |
| badgelink_usb.h | USB header |
| cobs.c | COBS encoding/decoding |
| cobs.h | COBS header |
| badgelink.pb.c | Nanopb-generated protobuf code |
| badgelink.pb.h | Nanopb-generated protobuf header |
| pb_*.c/h | Nanopb library files |

### Architecture

```
badgefs_backend_badgelink.c   Path routing (/sd, /int, /appfs)
         │                    Write buffering (upload on close)
         ▼
badgelink_client.c            High-level operations
         │                    (stat, list, upload, download, mkdir, etc.)
         ▼
badgelink_proto.c             Protocol layer
         │                    - COBS framing
         │                    - CRC32 integrity
         │                    - Protobuf serialization
         │                    - Request/response handling
         │                    - Retry with resync on errors
         ▼
badgelink_usb.c               USB transport
                              - libusb bulk transfers
                              - 512-byte chunked writes
                              - 10ms inter-chunk delays
```

### Key Protocol Details

- **USB Device:** VID 0x16d0, PID 0x0f9a
- **Interface:** 0
- **Endpoints:** 0x01 (OUT), 0x81 (IN)
- **Frame Format:** COBS-encoded payload + CRC32 + null terminator
- **Chunk Size:** 512 bytes (USB full-speed bulk max)
- **Inter-chunk Delay:** 10ms (matches Python implementation)

### Issues Fixed

#### Issue 1: CRC Mismatches on Large Frames
**Problem:** Large directory listings (271+ bytes) consistently failed CRC check.
**Root Cause:** Stale data in receive buffer from previous operations.
**Solution:** Clear rx_len and rx_pos at start of each request in `badgelink_proto_request()`.

#### Issue 2: Badge Becomes Unresponsive After StatusNotFound
**Problem:** After receiving StatusNotFound (e.g., stat on non-existent file), subsequent requests time out.
**Root Cause:** Badge protocol state becomes desynchronized.
**Solution:** Added `badgelink_proto_resync()` function and call it on retry after timeout errors.

#### Issue 3: USB Write Timing
**Problem:** File uploads (especially >4KB) timing out with no response.
**Root Cause:** USB writes sent too fast for badge to process.
**Solution:** Chunk USB writes to 512 bytes with 10ms delay between chunks (matching Python behavior).

### Code Changes Made

**badgelink_proto.c:**
```c
// Clear stale data at start of each request
proto->rx_len = 0;
proto->rx_pos = 0;

// Resync on retry
if (tries > 0) {
    badgelink_proto_resync(proto);
    serial = proto->serial_no++;  // Get new serial after resync
}
```

**badgelink_proto.h:**
```c
// Added resync function declaration
int badgelink_proto_resync(struct badgelink_proto *proto);
```

**badgelink_usb.c:**
```c
#define USB_WRITE_CHUNK_SIZE 512

// Send data in 512-byte chunks with 10ms delays
while (total_sent < len) {
    size_t chunk_size = min(len - total_sent, USB_WRITE_CHUNK_SIZE);
    libusb_bulk_transfer(..., chunk_size, ...);
    total_sent += transferred;
    usleep(10000);  // 10ms delay
}
```

### Test Results

```
Directory listing:  PASS
Small file upload (512 bytes):  PASS
Medium file upload (10KB):  PASS
```

### Usage

```bash
# Build
make

# Mount with BadgeLink backend (single-threaded mode recommended)
./badgefs -f -s /tmp/badgefs_mnt

# Access badge filesystem
ls /tmp/badgefs_mnt/sd/
cp local_file.bin /tmp/badgefs_mnt/sd/

# Unmount
fusermount -u /tmp/badgefs_mnt
```

### Virtual Filesystem Structure

```
/tmp/badgefs_mnt/
├── sd/         → Badge SD card (/sd on device)
├── int/        → Badge internal flash (/int on device)
└── appfs/      → AppFS applications (as <slug>.bin files)
```

---

## Protocol Bug Investigation (2025-11-26)

### Problem Description

BadgeFS has severe performance and reliability issues compared to the Python badgelink.py tool:

| Operation | Python | BadgeFS (C) |
|-----------|--------|-------------|
| `ls /sd/` | 0.267s | 51s (with errors) |
| `stat /sd/init.toml` | 0.166s | ~10s (with retries) |
| `stat /sd/BADGEVMS` | 0.148s | TIMEOUT (fails completely) |

### Observed Symptoms

1. **CRC Mismatches:** Frequent CRC errors like:
   ```
   badgelink_proto: CRC mismatch: recv=3f98d3c3 calc=aefcbf55 data_len=22
   ```

2. **Timeouts:** Operations timing out after 10s, then retrying

3. **Serial Number Mismatches:** After resync, old responses rejected:
   ```
   DEBUG proto_request: mismatch - expected serial=1763853865 got=1763853864
   ```

4. **Late Sync Responses:** Badge sends old response when we expect sync:
   ```
   badgelink_proto: sync: expected sync response, got 3
   ```

### Root Cause Analysis

Comparing Python (`badgelink.py`) vs C (`badgelink_proto.c`) implementations:

#### Issue 1: USB Read Behavior (CRITICAL)

**Python** reads ALL available data in a loop:
```python
def read_all(self):
    data = b''
    while True:
        try:
            new_data = bytes(self.ep_in.read(self.ep_in.wMaxPacketSize, 5))
            data += new_data
        except usb.USBError as e:
            break
    return data
```

**C** does single reads, missing data that arrives in multiple USB packets.

#### Issue 2: Serial Number on Retry (CRITICAL)

**Python** uses SAME serial for all retries:
```python
self.serial_no = (self.serial_no + 1) % (1 << 32)  # Once
for _ in range(tries):  # Same serial
    req_packet = Packet(request=request, serial=self.serial_no)
```

**C** gets NEW serial after resync (BUG):
```c
serial = proto->serial_no++;  // Initial
if (tries > 0) {
    badgelink_proto_resync(proto);
    serial = proto->serial_no++;  // NEW serial - WRONG!
}
```

#### Issue 3: Automatic Resync (PROBLEMATIC)

**Python** only resyncs when badge explicitly requests via sync flag.

**C** automatically resyncs on every retry, which disrupts communication.

#### Issue 4: Timeout Values

**Python:** 0.25s default, 0.5s for chunks
**C:** 10s default (CLIENT_TIMEOUT_MS) - way too long

### Fixes Applied

#### Fix 1: Implement read_all() in USB layer ✓
Modified `badgelink_usb_read()` to loop reading all available data like Python:
```c
int badgelink_usb_read(...) {
    size_t total_read = 0;
    int first_read = 1;
    while (total_read < max_len) {
        int read_timeout = first_read ? timeout_ms : 5;
        first_read = 0;
        int ret = libusb_bulk_transfer(...);
        if (ret == LIBUSB_ERROR_TIMEOUT) break;
        if (transferred == 0) break;
        total_read += transferred;
    }
    return total_read;
}
```

#### Fix 2: Use same serial across retries ✓
Changed `badgelink_proto_request()` to use same serial for all retries:
```c
uint32_t serial = proto->serial_no++;  // Get serial ONCE
for (int tries = 0; tries < 3; tries++) {
    // Use same serial for all attempts
    req_packet.serial = serial;
    ...
}
```

#### Fix 3: Remove automatic resync ✓
Removed automatic resync on retry. Only resync when badge explicitly requests via sync flag.

#### Fix 4: Reduce timeouts ✓
Changed `CLIENT_TIMEOUT_MS` from 10000ms to 2000ms in `badgelink_client.c`.

#### Fix 5: CRC32 Lookup Table ✓ (CRITICAL)
**Root Cause:** The hardcoded CRC32 lookup table in `badgelink_proto.c` had an incorrect value at index 111:
- **Wrong:** `0xDD0D7D87`
- **Correct:** `0xDD0D7CC9`

**Solution:** Created `generate_crc32_table.py` to generate the CRC32 table programmatically:
```python
poly = 0xEDB88320
for i in range(256):
    crc = i
    for _ in range(8):
        if crc & 1:
            crc = (crc >> 1) ^ poly
        else:
            crc >>= 1
    table.append(crc)
```

Generated `crc32_table.h` and modified `badgelink_proto.c` to include it:
```c
/* CRC32 lookup table - auto-generated from generate_crc32_table.py */
#include "crc32_table.h"
```

### Files Modified

- `badgelink_usb.c` - Implement read_all loop
- `badgelink_proto.c` - Fix serial handling, remove auto-resync, use generated CRC table
- `badgelink_client.c` - Reduce timeout constants
- `crc32_table.h` - Auto-generated CRC32 lookup table (NEW)
- `generate_crc32_table.py` - Python script to regenerate table (NEW)

### Test Results After Fixes

```
Before: ls /sd/ took 51 seconds with CRC errors
After:  ls /sd/ takes 2.8 seconds with no errors

All operations working:
- Directory listing: PASS
- File stat: PASS
- CRC verification: PASS
```

---

## Large File Download Investigation (2025-11-26)

### Problem Description

100KB file upload works correctly, but download fails consistently on the second chunk:

| Operation | Result |
|-----------|--------|
| Upload 100KB | SUCCESS |
| Download 100KB | FAILS at second chunk |

### Observed Behavior

```
First chunk (0-4095):    Gets 4125 bytes (4096 data + 29 overhead) - SUCCESS
Second chunk (4096-8191): Gets exactly 4096 bytes, missing final 29 bytes - TIMEOUT
```

The badge sends exactly 64 full USB packets (64 × 64 = 4096 bytes) then stops, never sending the final short packet containing the COBS frame terminator + CRC.

### Python vs C Behavior

Python `badgelink.py` tool downloads successfully using:
```python
def read_all(self):
    data = b''
    while True:
        try:
            new_data = bytes(self.ep_in.read(self.ep_in.wMaxPacketSize, 5))
            data += new_data
        except usb.USBError as e:
            break
    return data
```

Key differences:
- Reads exactly `wMaxPacketSize` (64 bytes) at a time
- Uses 5ms timeout per USB read
- Loops until timeout with no data

### Changes Made

**badgelink_usb.c** - Multiple iterations of USB read logic:

1. Changed subsequent read timeout from 5ms to 100ms
2. Added overall timeout tracking with `clock_gettime`
3. Changed to read 64 bytes at a time (USB_MAX_PACKET_SIZE) like Python
4. Added retry logic when total_read is multiple of packet size:

```c
#define USB_MAX_PACKET_SIZE 64

int badgelink_usb_read(struct badgelink_usb *usb, uint8_t *buf,
                       size_t max_len, int timeout_ms)
{
    size_t total_read = 0;
    int first_read = 1;
    int timeout_count = 0;

    while (total_read < max_len) {
        int read_timeout = first_read ? timeout_ms : 5;
        first_read = 0;

        /* Read only up to USB max packet size at a time, like Python */
        size_t chunk_size = max_len - total_read;
        if (chunk_size > USB_MAX_PACKET_SIZE)
            chunk_size = USB_MAX_PACKET_SIZE;

        int ret = libusb_bulk_transfer(...);

        if (ret == LIBUSB_ERROR_TIMEOUT) {
            /* If aligned to packet size, try again with longer timeout */
            if (total_read > 0 && (total_read % USB_MAX_PACKET_SIZE) == 0) {
                timeout_count++;
                if (timeout_count < 10) {
                    /* Try again with 50ms timeout */
                    ret = libusb_bulk_transfer(..., 50);
                    if (ret == 0 && transferred > 0) {
                        total_read += transferred;
                        continue;
                    }
                }
            }
            break;
        }
        ...
    }
    return total_read;
}
```

**badgelink_proto.c** - Increased fill_rx_buffer timeout from 50ms to 500ms.

### Root Cause Analysis

The issue appears to be badge firmware behavior:
- When data to send ends exactly on a USB bulk packet boundary (4096 = 64 × 64), the badge sometimes doesn't complete the transfer
- First chunk always works because the badge likely handles it differently
- pyusb vs libusb may have different handling of bulk transfer completion

### Status

**RESOLVED** - See fix below.

---

## Download Fix (2025-11-26)

### Root Cause

Two issues were preventing large file downloads:

1. **Short-read break condition (CRITICAL):** The C implementation was breaking out of the USB read loop when receiving less than `USB_MAX_PACKET_SIZE` bytes. Python's `read_all()` does NOT break on short reads - it only breaks on USB timeout errors. A short read just means one packet was short; there may be more data (including the null byte frame terminator) in subsequent packets.

2. **Insufficient timeout for initial transfer requests:** The initial download request used `CLIENT_TIMEOUT_MS` (500ms), but Python uses `xfer_timeout = 10` seconds. Large files require more time for the badge to open.

### Fix Applied

**badgelink_usb.c** - Remove short-read break condition:
```c
/* BEFORE (WRONG): */
if ((size_t)transferred < USB_MAX_PACKET_SIZE) {
    break;  /* This caused early termination! */
}

/* AFTER (CORRECT): */
/* Do NOT break on short reads - only break on timeout or transferred==0 */
if (transferred == 0) {
    break;
}
```

**badgelink_client.c** - Add longer timeout for transfer operations:
```c
/* New timeout constant */
#define XFER_TIMEOUT_MS 10000  /* Match Python's xfer_timeout = 10s */

/* Use for initial download/upload requests */
int ret = badgelink_proto_request(..., XFER_TIMEOUT_MS);
```

### Test Results

| File Size | Before | After |
|-----------|--------|-------|
| 188 bytes | 0.2s ✓ | 0.2s ✓ |
| 10 KB | 0.9s ✓ | 0.9s ✓ |
| 41 KB | 2.7s ✓ | 2.7s ✓ |
| 50 KB | FAIL (timeout at chunk 11) | 3.6s ✓ |
| 100 KB | FAIL (timeout on initial request) | 6.7s ✓ |

### Key Insight

Python's USB read behavior:
```python
def read_all(self):
    data = b''
    while True:
        try:
            new_data = bytes(self.ep_in.read(self.ep_in.wMaxPacketSize, 5))
            data += new_data
        except usb.USBError as e:
            break  # Only break on USB error (timeout), NOT on short reads
    return data
```

The critical difference: Python only exits on USB timeout errors, not when a single read returns fewer bytes than requested.

---

## Test Script (2025-11-26)

### test_sd_cycle.sh

Full integration test for SD card operations via BadgeFS FUSE mount.

**Test Steps:**
1. Create 100KB random test file
2. Mount BadgeFS filesystem
3. Create test directory on SD card
4. Upload test file to badge
5. Download file and verify MD5 checksum matches
6. Delete test directory and cleanup

**Usage:**
```bash
./test_sd_cycle.sh
```

**Features:**
- Automatic cleanup on exit (trap handler)
- Unique test directory per run (uses PID)
- MD5 checksum verification
- Colored output for status
- Proper error handling with `set -e`

**Test Results (2025-11-26):**
```
Step 1: Create 100KB random file     - PASS
Step 2: Mount BadgeFS                - PASS
Step 3: Create test directory        - PASS
Step 4: Upload test file             - PASS
Step 5: Download and compare (MD5)   - PASS
Step 6: Delete test directory        - PASS
ALL TESTS PASSED!
```

---

## Large File Download Fix (2025-11-26)

### Problem
Files >100KB were being truncated during download. A 152KB file would download as 131072 bytes (exactly 128KB).

### Root Cause
In `bl_lookup()`, when an `open_file` entry exists, the function returns `f->size` regardless of whether the file has a buffer. For files opened read-only:
1. `bl_open` creates `open_file` with `size=0` (no download for read-only)
2. FUSE calls `getattr` (which calls `bl_lookup`)
3. `bl_lookup` finds `open_file`, returns `size=0`
4. FUSE uses the returned size to limit reads
5. Download gets truncated to FUSE's internal buffer size (128KB)

### Fix
In `bl_lookup()`, only use in-memory `open_file` state if the file has been modified (`f->modified == true`). Otherwise, query the badge for the actual file size.

**badgefs_backend_badgelink.c line 310:**
```c
/* Before (WRONG): */
if (f) {

/* After (CORRECT): */
if (f && f->modified) {
```

### Test Results
- 152KB binary file upload/download: PASS (MD5 match)
- 100KB random file test cycle: PASS (all 6 steps)

---

## AppFS Extended Attributes (xattr) Implementation (2025-11-26)

### Overview

Added support for reading and writing AppFS application metadata (title, version) via POSIX extended attributes. This allows users to manage app metadata using standard tools like `getfattr` and `setfattr`.

### Supported Attributes

| Attribute | Description | Example |
|-----------|-------------|---------|
| `user.appfs.title` | Application display name | "Nofrendo NES emulator" |
| `user.appfs.version` | Application version number | "1" |

### Files Modified

**badgefs_backend.h** - Added xattr function pointers to backend interface:
```c
/* Extended attributes */
int (*getxattr)(const char *path, const char *name, char *value, size_t size);
int (*setxattr)(const char *path, const char *name, const char *value, size_t size, int flags);
int (*listxattr)(const char *path, char *list, size_t size);
```

**badgefs_ops.h/c** - Added FUSE xattr callbacks and registered them in fuse_operations structure.

**badgefs_backend_badgelink.c** - Implemented xattr operations:
- `bl_getxattr()` - Reads title/version from badge via `badgelink_appfs_stat()`
- `bl_setxattr()` - Sets title/version with immediate upload to badge
- `bl_listxattr()` - Returns list of supported attributes

### Key Implementation Details

#### Immediate Upload on setxattr

The initial implementation stored pending xattr values but didn't upload until `release()` - which never happens with `setfattr`. The fix performs immediate upload:

```c
static int bl_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags)
{
    /* 1. Download existing file content to preserve it */
    uint8_t *data = NULL;
    size_t file_size = 0;
    badgelink_appfs_download(&state.client, slug, &data, &file_size);

    /* 2. Set the new attribute value */
    if (strcmp(name, XATTR_APPFS_TITLE) == 0) {
        strncpy(f->pending_title, value, size);
        f->has_pending_title = true;
    } else if (strcmp(name, XATTR_APPFS_VERSION) == 0) {
        f->pending_version = atoi(ver_str);
        f->has_pending_version = true;
    }

    /* 3. Immediately upload with new metadata */
    struct badgelink_app app;
    badgelink_appfs_stat(&state.client, slug, &existing);
    /* Apply pending values */
    if (f->has_pending_title) strncpy(app.title, f->pending_title, ...);
    if (f->has_pending_version) app.version = f->pending_version;

    badgelink_appfs_upload(&state.client, &app, f->buffer, f->size);

    /* 4. Clean up */
    remove_open_file(f);
    return 0;
}
```

#### Attribute Preservation on Overwrite

When a file is overwritten (e.g., `cp new.bin /appfs/existing.bin`), the existing title and version are automatically preserved. This is handled in `bl_release()`:

```c
/* Check if app already exists to preserve title/version */
struct badgelink_app existing;
ret = badgelink_appfs_stat(&state.client, f->slug, &existing);
if (ret == 0) {
    /* App exists - keep existing title/version */
    strncpy(app.title, existing.title, sizeof(app.title) - 1);
    app.version = existing.version;
}
```

### Usage Examples

```bash
# Read all attributes
getfattr -d /tmp/badgefs_mnt/appfs/team.badge.nofrendo.bin

# Read specific attribute
getfattr -n user.appfs.title /tmp/badgefs_mnt/appfs/myapp.bin

# Set title
setfattr -n user.appfs.title -v "My Application" /tmp/badgefs_mnt/appfs/myapp.bin

# Set version
setfattr -n user.appfs.version -v "42" /tmp/badgefs_mnt/appfs/myapp.bin
```

### Test Results

| Test | Result |
|------|--------|
| getxattr title | PASS |
| getxattr version | PASS |
| setxattr title | PASS |
| setxattr version | PASS |
| Persistence after remount | PASS |
| File content preservation | PASS |
| Overwrite preserves attrs | PASS |

---
**Last Updated:** 2025-11-26
**Status:** FULLY FUNCTIONAL - Upload, download, mkdir, rmdir, delete, xattr all working
