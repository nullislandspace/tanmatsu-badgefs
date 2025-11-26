# FUSE Implementation Guide

**Filesystem in Userspace (FUSE)** allows you to create custom filesystems without modifying kernel code. This guide covers everything you need to implement your own virtual filesystem.

## Table of Contents

1. [Introduction](#introduction)
2. [Architecture](#architecture)
3. [Prerequisites](#prerequisites)
4. [Core Concepts](#core-concepts)
5. [Essential Operations](#essential-operations)
6. [Building and Running](#building-and-running)
7. [Framework Overview](#framework-overview)
8. [Implementing a Custom Backend](#implementing-a-custom-backend)
9. [Error Handling](#error-handling)
10. [Debugging](#debugging)
11. [Advanced Topics](#advanced-topics)
12. [References](#references)

---

## Introduction

### What is FUSE?

FUSE (Filesystem in Userspace) is a software interface that lets non-privileged users create their own filesystems without editing kernel code. It consists of:

- **Kernel module** (`fuse.ko`): Handles the VFS interface
- **Userspace library** (`libfuse3`): Provides the API for your program
- **Your program**: Implements filesystem operations

### Why Use FUSE?

- **No kernel development required**: Write filesystems in userspace
- **Any language**: Use C, Python, Go, Rust, etc.
- **Safe**: Crashes don't bring down the system
- **Flexible**: Create virtual filesystems for any data source
- **Debuggable**: Use standard debugging tools (gdb, printf, etc.)

### Use Cases

- Network filesystems (SSHFS, S3FS)
- Archive filesystems (mount ZIP, TAR)
- Version control (mount Git repos)
- Virtual data sources (databases, APIs)
- Encrypted filesystems
- Memory-backed filesystems

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      User Application                        │
│                   (ls, cat, cp, your app)                   │
└─────────────────────────────┬───────────────────────────────┘
                              │ open(), read(), write(), etc.
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    VFS (Virtual File System)                 │
│                        Linux Kernel                          │
└─────────────────────────────┬───────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      FUSE Kernel Module                      │
│                        (fuse.ko)                             │
└─────────────────────────────┬───────────────────────────────┘
                              │ /dev/fuse
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      libfuse3 Library                        │
│              (Handles communication with kernel)             │
└─────────────────────────────┬───────────────────────────────┘
                              │ callbacks
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   Your FUSE Filesystem                       │
│                    (badgefs, sshfs, etc.)                   │
└─────────────────────────────────────────────────────────────┘
```

### Request Flow

1. Application calls `read("/mnt/fuse/file", ...)`
2. VFS routes request to FUSE kernel module
3. FUSE module sends request to userspace via `/dev/fuse`
4. libfuse receives request and calls your `read()` callback
5. Your callback returns data
6. libfuse sends response back through kernel
7. Application receives data

---

## Prerequisites

### Install libfuse3 Development Package

**Debian/Ubuntu:**
```bash
sudo apt install libfuse3-dev pkg-config
```

**Fedora/RHEL:**
```bash
sudo dnf install fuse3-devel pkgconf
```

**Arch Linux:**
```bash
sudo pacman -S fuse3
```

### Verify Installation

```bash
pkg-config --modversion fuse3
# Should output: 3.x.x
```

### User Permissions

To mount FUSE filesystems as a non-root user:

```bash
# Add yourself to the fuse group
sudo usermod -aG fuse $USER

# Log out and back in, or:
newgrp fuse
```

---

## Core Concepts

### The fuse_operations Structure

The heart of any FUSE filesystem is the `fuse_operations` structure. It contains function pointers for every filesystem operation:

```c
#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>

static const struct fuse_operations my_ops = {
    .init       = my_init,       // Called on mount
    .destroy    = my_destroy,    // Called on unmount
    .getattr    = my_getattr,    // stat() - get file attributes
    .readdir    = my_readdir,    // List directory contents
    .mkdir      = my_mkdir,      // Create directory
    .rmdir      = my_rmdir,      // Remove directory
    .create     = my_create,     // Create file
    .open       = my_open,       // Open file
    .release    = my_release,    // Close file
    .read       = my_read,       // Read from file
    .write      = my_write,      // Write to file
    .unlink     = my_unlink,     // Delete file
    .truncate   = my_truncate,   // Resize file
    .rename     = my_rename,     // Move/rename
    .chmod      = my_chmod,      // Change permissions
    .chown      = my_chown,      // Change owner
    .utimens    = my_utimens,    // Update timestamps
};
```

### FUSE_USE_VERSION

Always define `FUSE_USE_VERSION` before including FUSE headers:

```c
#define FUSE_USE_VERSION 31  // Use FUSE 3.1+ API
#include <fuse3/fuse.h>
```

Version numbers:
- **31**: FUSE 3.1+ (recommended)
- **30**: FUSE 3.0
- **26**: FUSE 2.6 (legacy)

### Error Handling Convention

**All FUSE callbacks return:**
- `0` on success
- Negative `errno` on failure (e.g., `-ENOENT`, `-EACCES`)

```c
static int my_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi)
{
    if (strcmp(path, "/") != 0)
        return -ENOENT;  // Not found

    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = 2;
    return 0;  // Success
}
```

### The struct stat

File attributes are returned via `struct stat`:

```c
struct stat {
    dev_t     st_dev;      // Device ID (ignored by FUSE)
    ino_t     st_ino;      // Inode number (optional)
    mode_t    st_mode;     // File type and permissions
    nlink_t   st_nlink;    // Number of hard links
    uid_t     st_uid;      // Owner user ID
    gid_t     st_gid;      // Owner group ID
    dev_t     st_rdev;     // Device ID (if special file)
    off_t     st_size;     // File size in bytes
    blksize_t st_blksize;  // Block size for I/O
    blkcnt_t  st_blocks;   // Number of 512-byte blocks
    struct timespec st_atim;  // Last access time
    struct timespec st_mtim;  // Last modification time
    struct timespec st_ctim;  // Last status change time
};
```

**File type flags for st_mode:**
| Flag | Meaning |
|------|---------|
| `S_IFREG` | Regular file |
| `S_IFDIR` | Directory |
| `S_IFLNK` | Symbolic link |
| `S_IFCHR` | Character device |
| `S_IFBLK` | Block device |
| `S_IFIFO` | FIFO (named pipe) |
| `S_IFSOCK` | Socket |

**Example:**
```c
// Directory with rwxr-xr-x permissions
st->st_mode = S_IFDIR | 0755;

// Regular file with rw-r--r-- permissions
st->st_mode = S_IFREG | 0644;
```

### struct fuse_file_info

Passed to file operations, contains:

```c
struct fuse_file_info {
    int flags;           // Open flags (O_RDONLY, O_WRONLY, etc.)
    uint64_t fh;         // File handle (you can store anything here)
    unsigned int writepage : 1;
    unsigned int direct_io : 1;
    unsigned int keep_cache : 1;
    // ... more fields
};
```

Use `fi->fh` to store per-open-file state:

```c
static int my_open(const char *path, struct fuse_file_info *fi)
{
    int fd = open(real_path, fi->flags);
    fi->fh = fd;  // Store file descriptor
    return 0;
}

static int my_read(const char *path, char *buf, size_t size,
                   off_t offset, struct fuse_file_info *fi)
{
    return pread(fi->fh, buf, size, offset);  // Use stored fd
}
```

---

## Essential Operations

### getattr (stat)

**Called when:** Any file/directory is accessed (very frequent!)

```c
static int my_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi)
{
    (void)fi;  // May be NULL

    memset(st, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        // Root directory
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_uid = getuid();
        st->st_gid = getgid();
        return 0;
    }

    // Look up the file in your storage...
    struct my_file *file = find_file(path);
    if (!file)
        return -ENOENT;

    st->st_mode = file->mode;
    st->st_nlink = 1;
    st->st_size = file->size;
    st->st_uid = file->uid;
    st->st_gid = file->gid;
    st->st_atim = file->atime;
    st->st_mtim = file->mtime;
    st->st_ctim = file->ctime;

    return 0;
}
```

### readdir (directory listing)

**Called when:** `ls`, `find`, or directory is opened

```c
static int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)fi;
    (void)flags;

    // Always include . and ..
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    // Add your files
    struct my_dir *dir = find_dir(path);
    if (!dir)
        return -ENOENT;

    for (struct my_entry *e = dir->entries; e; e = e->next) {
        // filler returns non-zero if buffer is full
        if (filler(buf, e->name, &e->stat, 0, 0))
            break;
    }

    return 0;
}
```

**filler function signature:**
```c
int (*fuse_fill_dir_t)(void *buf, const char *name,
                       const struct stat *stbuf, off_t off,
                       enum fuse_fill_dir_flags flags);
```

### mkdir / rmdir

```c
static int my_mkdir(const char *path, mode_t mode)
{
    // Create directory with given permissions
    struct my_dir *parent = find_parent_dir(path);
    if (!parent)
        return -ENOENT;

    const char *name = get_basename(path);
    if (find_entry(parent, name))
        return -EEXIST;

    create_directory(parent, name, mode);
    return 0;
}

static int my_rmdir(const char *path)
{
    struct my_dir *dir = find_dir(path);
    if (!dir)
        return -ENOENT;
    if (!is_directory(dir))
        return -ENOTDIR;
    if (!is_empty(dir))
        return -ENOTEMPTY;

    remove_directory(dir);
    return 0;
}
```

### create / unlink

```c
static int my_create(const char *path, mode_t mode,
                     struct fuse_file_info *fi)
{
    (void)fi;

    struct my_dir *parent = find_parent_dir(path);
    if (!parent)
        return -ENOENT;

    const char *name = get_basename(path);
    if (find_entry(parent, name))
        return -EEXIST;

    create_file(parent, name, mode);
    return 0;
}

static int my_unlink(const char *path)
{
    struct my_file *file = find_file(path);
    if (!file)
        return -ENOENT;
    if (is_directory(file))
        return -EISDIR;

    delete_file(file);
    return 0;
}
```

### open / release

```c
static int my_open(const char *path, struct fuse_file_info *fi)
{
    struct my_file *file = find_file(path);
    if (!file)
        return -ENOENT;
    if (is_directory(file))
        return -EISDIR;

    // Check access permissions based on fi->flags
    // (O_RDONLY, O_WRONLY, O_RDWR)

    // Optionally store handle in fi->fh
    fi->fh = (uint64_t)file;

    return 0;
}

static int my_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    // Called when file is closed
    // Cleanup if needed
    return 0;
}
```

### read / write

```c
static int my_read(const char *path, char *buf, size_t size,
                   off_t offset, struct fuse_file_info *fi)
{
    struct my_file *file = (struct my_file *)fi->fh;
    // Or: file = find_file(path);

    if (offset >= file->size)
        return 0;  // EOF

    size_t available = file->size - offset;
    if (size > available)
        size = available;

    memcpy(buf, file->data + offset, size);
    return size;  // Return bytes read
}

static int my_write(const char *path, const char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    struct my_file *file = (struct my_file *)fi->fh;

    // Expand file if needed
    size_t new_size = offset + size;
    if (new_size > file->capacity) {
        file->data = realloc(file->data, new_size);
        file->capacity = new_size;
    }

    memcpy(file->data + offset, buf, size);

    if (new_size > file->size)
        file->size = new_size;

    return size;  // Return bytes written
}
```

### truncate

```c
static int my_truncate(const char *path, off_t size,
                       struct fuse_file_info *fi)
{
    struct my_file *file = fi ? (struct my_file *)fi->fh
                              : find_file(path);
    if (!file)
        return -ENOENT;

    if (size < 0)
        return -EINVAL;

    // Resize file
    if ((size_t)size > file->capacity) {
        file->data = realloc(file->data, size);
        // Zero-fill new area
        memset(file->data + file->size, 0, size - file->size);
        file->capacity = size;
    }

    file->size = size;
    return 0;
}
```

### rename

```c
static int my_rename(const char *from, const char *to, unsigned int flags)
{
    // flags can be RENAME_EXCHANGE or RENAME_NOREPLACE
    if (flags)
        return -EINVAL;  // Not supported

    struct my_file *file = find_file(from);
    if (!file)
        return -ENOENT;

    // Remove from old parent
    remove_from_parent(file);

    // Update name
    free(file->name);
    file->name = strdup(get_basename(to));

    // Add to new parent
    struct my_dir *new_parent = find_parent_dir(to);
    add_to_parent(new_parent, file);

    return 0;
}
```

---

## Building and Running

### Compilation

**Simple single-file:**
```bash
gcc -Wall myfs.c -o myfs $(pkg-config fuse3 --cflags --libs)
```

**With Makefile (recommended):**
```makefile
CC = gcc
CFLAGS = -Wall -Wextra -g $(shell pkg-config fuse3 --cflags)
LDFLAGS = $(shell pkg-config fuse3 --libs) -lpthread

TARGET = badgefs
SRCS = badgefs.c badgefs_ops.c badgefs_backend_mem.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
```

### Mounting

```bash
# Create mount point
mkdir /tmp/mnt

# Mount in background (daemon mode)
./badgefs /tmp/mnt

# Mount in foreground (see output)
./badgefs -f /tmp/mnt

# Mount with debug output
./badgefs -d -f /tmp/mnt

# Single-threaded mode (easier debugging)
./badgefs -s -f /tmp/mnt
```

### Unmounting

```bash
# Normal unmount
fusermount -u /tmp/mnt

# Force unmount (if busy)
fusermount -uz /tmp/mnt

# Or use umount (as root)
sudo umount /tmp/mnt
```

### Mount Options

```bash
# Allow other users to access
./badgefs -o allow_other /tmp/mnt

# Read-only mount
./badgefs -o ro /tmp/mnt

# Multiple options
./badgefs -o allow_other,default_permissions /tmp/mnt
```

Note: `allow_other` requires `/etc/fuse.conf` to contain `user_allow_other`.

---

## Framework Overview

This BadgeFS framework provides a pluggable architecture:

```
badgefs.c              Main entry point
    │
    ▼
badgefs_ops.c/.h       FUSE callbacks (thin wrapper)
    │
    ▼
badgefs_backend.h      Abstract storage interface
    │
    ▼
badgefs_backend_mem.c  In-memory implementation (default)
```

### File Structure

| File | Purpose |
|------|---------|
| `Makefile` | Build system |
| `badgefs.c` | Main entry point, argument parsing |
| `badgefs_ops.h` | FUSE callback declarations |
| `badgefs_ops.c` | FUSE callback implementations |
| `badgefs_backend.h` | Abstract storage interface |
| `badgefs_backend_mem.h` | Memory backend declarations |
| `badgefs_backend_mem.c` | In-memory storage implementation |
| `FUSE.md` | This documentation |

---

## Implementing a Custom Backend

### Step 1: Create Backend Header

```c
// my_backend.h
#ifndef MY_BACKEND_H
#define MY_BACKEND_H

#include "badgefs_backend.h"

struct badgefs_backend *my_backend_get(void);

#endif
```

### Step 2: Implement Backend

```c
// my_backend.c
#include "my_backend.h"

static int my_init(void *config) {
    // Connect to database, network, etc.
    return 0;
}

static void my_destroy(void) {
    // Cleanup
}

static int my_lookup(const char *path, struct stat *st) {
    // Query your storage for file attributes
    return 0;
}

// Implement all other operations...

static struct badgefs_backend my_backend = {
    .init     = my_init,
    .destroy  = my_destroy,
    .lookup   = my_lookup,
    .readdir  = my_readdir,
    .mkdir    = my_mkdir,
    // ... all other operations
};

struct badgefs_backend *my_backend_get(void) {
    return &my_backend;
}
```

### Step 3: Use Your Backend

```c
// In badgefs.c, before fuse_main():
#include "my_backend.h"

int main(int argc, char *argv[]) {
    // Use custom backend instead of default
    badgefs_set_backend(my_backend_get());

    return fuse_main(argc, argv, badgefs_get_operations(), NULL);
}
```

### Step 4: Update Makefile

```makefile
SRCS = badgefs.c badgefs_ops.c my_backend.c
```

---

## Error Handling

### Common Error Codes

| Error | Value | Meaning | When to Use |
|-------|-------|---------|-------------|
| `ENOENT` | 2 | No such file or directory | Path doesn't exist |
| `EACCES` | 13 | Permission denied | Access check failed |
| `EEXIST` | 17 | File exists | Create when file exists |
| `ENOTDIR` | 20 | Not a directory | Expected directory |
| `EISDIR` | 21 | Is a directory | File op on directory |
| `EINVAL` | 22 | Invalid argument | Bad parameter |
| `ENOSPC` | 28 | No space left | Out of storage |
| `EROFS` | 30 | Read-only filesystem | Write to R/O mount |
| `ENOTEMPTY` | 39 | Directory not empty | rmdir non-empty |
| `ENOSYS` | 38 | Function not implemented | Op not supported |

### Example Error Handling

```c
static int my_write(const char *path, const char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    // Check for read-only mode
    if (read_only_mode)
        return -EROFS;

    // Find file
    struct my_file *file = find_file(path);
    if (!file)
        return -ENOENT;

    // Check permissions
    if (!can_write(file))
        return -EACCES;

    // Check for invalid offset
    if (offset < 0)
        return -EINVAL;

    // Allocate space
    char *new_data = realloc(file->data, offset + size);
    if (!new_data)
        return -ENOSPC;

    // Write data
    file->data = new_data;
    memcpy(file->data + offset, buf, size);

    return size;  // Success: return bytes written
}
```

---

## Debugging

### Debug Flags

```bash
# Maximum debug output
./badgefs -d -f -s /tmp/mnt

# -d: Enable FUSE debug messages
# -f: Foreground (don't daemonize)
# -s: Single-threaded (easier to debug)
```

### Adding Debug Output

```c
#include <stdio.h>

static int my_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi)
{
    fprintf(stderr, "getattr: path=%s\n", path);

    // ... implementation ...

    fprintf(stderr, "getattr: returning %d\n", result);
    return result;
}
```

### Using GDB

```bash
# Run under gdb
gdb --args ./badgefs -f -s /tmp/mnt

# In gdb:
(gdb) break my_getattr
(gdb) run

# In another terminal, trigger the operation:
ls /tmp/mnt
```

### Common Issues

**"Transport endpoint is not connected"**
- Filesystem crashed or was killed
- Solution: `fusermount -u /tmp/mnt`

**"Permission denied" on mount**
- User not in fuse group
- Solution: `sudo usermod -aG fuse $USER` and re-login

**"fuse: bad mount point" or "not empty"**
- Mount point doesn't exist or has files
- Solution: `mkdir /tmp/mnt` or use empty directory

**Operations return -1 instead of -errno**
- You're returning errno, not -errno
- Wrong: `return ENOENT`
- Right: `return -ENOENT`

---

## Advanced Topics

### Thread Safety

FUSE runs multi-threaded by default. Protect shared state:

```c
#include <pthread.h>

static pthread_rwlock_t fs_lock = PTHREAD_RWLOCK_INITIALIZER;

static int my_read(const char *path, char *buf, size_t size,
                   off_t offset, struct fuse_file_info *fi)
{
    pthread_rwlock_rdlock(&fs_lock);  // Read lock

    // ... read implementation ...

    pthread_rwlock_unlock(&fs_lock);
    return result;
}

static int my_write(const char *path, const char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    pthread_rwlock_wrlock(&fs_lock);  // Write lock

    // ... write implementation ...

    pthread_rwlock_unlock(&fs_lock);
    return result;
}
```

Or run single-threaded with `-s` flag.

### Extended Attributes (xattr)

```c
static int my_setxattr(const char *path, const char *name,
                       const char *value, size_t size, int flags)
{
    // Store extended attribute
    return 0;
}

static int my_getxattr(const char *path, const char *name,
                       char *value, size_t size)
{
    // Retrieve extended attribute
    // Return size of attribute, or -ENODATA if not found
    return attr_size;
}

static int my_listxattr(const char *path, char *list, size_t size)
{
    // List all extended attributes
    // Return total size of list
    return list_size;
}

static int my_removexattr(const char *path, const char *name)
{
    // Remove extended attribute
    return 0;
}
```

### Symbolic Links

```c
static int my_symlink(const char *target, const char *linkpath)
{
    // Create symbolic link
    struct my_file *link = create_file(linkpath, S_IFLNK | 0777);
    link->target = strdup(target);
    return 0;
}

static int my_readlink(const char *path, char *buf, size_t size)
{
    struct my_file *link = find_file(path);
    if (!link || !S_ISLNK(link->st.st_mode))
        return -EINVAL;

    strncpy(buf, link->target, size - 1);
    buf[size - 1] = '\0';
    return 0;
}
```

### File Locking

```c
static int my_lock(const char *path, struct fuse_file_info *fi,
                   int cmd, struct flock *lock)
{
    // Implement POSIX locking (F_GETLK, F_SETLK, F_SETLKW)
    return 0;
}

static int my_flock(const char *path, struct fuse_file_info *fi,
                    int op)
{
    // Implement BSD flock()
    return 0;
}
```

### Direct I/O

Skip kernel page cache for large files or real-time access:

```c
static int my_open(const char *path, struct fuse_file_info *fi)
{
    fi->direct_io = 1;   // Bypass page cache
    fi->keep_cache = 0;  // Don't cache
    return 0;
}
```

---

## References

### Official Documentation

- [libfuse GitHub](https://github.com/libfuse/libfuse)
- [libfuse API Docs](https://libfuse.github.io/doxygen/)
- [Linux Kernel FUSE Docs](https://docs.kernel.org/filesystems/fuse/)

### Tutorials

- [CS NMSU FUSE Tutorial](https://www.cs.nmsu.edu/~pfeiffer/fuse-tutorial/)
- [Writing a Simple Filesystem Using FUSE](https://www.maastaar.net/fuse/linux/filesystem/c/2016/05/21/writing-a-simple-filesystem-using-fuse/)
- [OSDev FUSE Wiki](https://wiki.osdev.org/FUSE)

### Example Filesystems

- [passthrough.c](https://github.com/libfuse/libfuse/blob/master/example/passthrough.c) - Official example
- [SSHFS](https://github.com/libfuse/sshfs) - SSH filesystem
- [s3fs-fuse](https://github.com/s3fs-fuse/s3fs-fuse) - Amazon S3

### API Reference

- [fuse_operations struct](https://libfuse.github.io/doxygen/structfuse__operations.html)
- [fuse_lowlevel_ops](https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html) (low-level API)

---

## Quick Reference

### Minimal FUSE Filesystem

```c
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <string.h>
#include <errno.h>

static int my_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi) {
    (void)fi;
    memset(st, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }
    return -ENOENT;
}

static int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    if (strcmp(path, "/") != 0)
        return -ENOENT;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    return 0;
}

static const struct fuse_operations ops = {
    .getattr = my_getattr,
    .readdir = my_readdir,
};

int main(int argc, char *argv[]) {
    return fuse_main(argc, argv, &ops, NULL);
}
```

**Build and run:**
```bash
gcc -Wall minimal.c -o minimal $(pkg-config fuse3 --cflags --libs)
mkdir /tmp/mnt
./minimal /tmp/mnt
ls /tmp/mnt
fusermount -u /tmp/mnt
```
