/*
 * badgefs.c - BadgeFS FUSE filesystem main entry point
 *
 * This is the main program that initializes and runs the FUSE filesystem.
 * It parses command-line arguments and starts the FUSE main loop.
 *
 * Usage:
 *   badgefs [options] <mountpoint>
 *
 * Options:
 *   -u          Unmount the filesystem
 *   -f          Run in foreground (don't daemonize)
 *   -d          Enable debug output (implies -f)
 *   -s          Run single-threaded
 *   -o <opts>   Mount options (e.g., -o allow_other)
 *
 * Examples:
 *   badgefs /mnt/badge           # Mount filesystem
 *   badgefs -f /mnt/badge        # Mount in foreground
 *   badgefs -d -f /mnt/badge     # Debug mode with output
 *   badgefs -u /mnt/badge        # Unmount filesystem
 */

#define FUSE_USE_VERSION 31

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <fuse3/fuse.h>

#include "badgefs_ops.h"
#include "badgefs_backend.h"
#include "badgefs_backend_badgelink.h"

/*
 * Print usage information
 */
static void print_usage(const char *progname)
{
    fprintf(stderr,
        "BadgeFS - FUSE Virtual Filesystem for Tanmatsu Badge\n"
        "\n"
        "Usage: %s [options] <mountpoint>\n"
        "\n"
        "Options:\n"
        "  -u              Unmount the filesystem\n"
        "  -f              Run in foreground (don't daemonize)\n"
        "  -d              Enable debug output (implies -f)\n"
        "  -s              Run single-threaded\n"
        "  -o <options>    Mount options (comma-separated)\n"
        "  --version1      Force protocol version 1 (legacy mode)\n"
        "  -h, --help      Show this help message\n"
        "  -V, --version   Show version\n"
        "\n"
        "Common mount options (-o):\n"
        "  allow_other     Allow other users to access the filesystem\n"
        "  allow_root      Allow root to access the filesystem\n"
        "  default_permissions  Enable permission checking by kernel\n"
        "\n"
        "Examples:\n"
        "  mkdir /tmp/mnt\n"
        "  %s /tmp/mnt                 # Mount filesystem\n"
        "  %s -f /tmp/mnt              # Mount in foreground\n"
        "  %s -d -f /tmp/mnt           # Debug mode\n"
        "  %s -u /tmp/mnt              # Unmount filesystem\n"
        "\n"
        "Filesystem layout:\n"
        "  /sd             SD card on badge\n"
        "  /int            Internal memory on badge\n"
        "  /appfs          Application filesystem (apps as <slug>.bin)\n"
        "\n",
        progname, progname, progname, progname, progname);
}

/*
 * Print version information
 */
static void print_version(void)
{
    printf("BadgeFS version 1.1.0 (BadgeLink support)\n");
    printf("FUSE library version %d.%d\n",
           FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);
}

/*
 * Unmount the filesystem using fusermount
 */
static int do_unmount(const char *mountpoint)
{
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "fusermount -u '%s'", mountpoint);
    int status = system(cmd);
    int ret = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (ret == 0) {
        printf("BadgeFS: Unmounted %s\n", mountpoint);
    } else {
        fprintf(stderr, "BadgeFS: Failed to unmount %s\n", mountpoint);
    }
    return ret;
}

int main(int argc, char *argv[])
{
    int do_unmount_flag = 0;
    int force_v1_flag = 0;
    const char *mountpoint = NULL;
    int new_argc = 0;
    char **new_argv = malloc((argc + 2) * sizeof(char *));  /* +2 for -s flag */
    if (!new_argv) {
        fprintf(stderr, "Error: Out of memory\n");
        return 1;
    }

    /* Parse our options and pass the rest to FUSE */
    new_argv[new_argc++] = argv[0];

    /* Force single-threaded mode for BadgeLink (USB is not thread-safe) */
    new_argv[new_argc++] = "-s";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 ||
            strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            free(new_argv);
            return 0;
        }
        if (strcmp(argv[i], "-V") == 0 ||
            strcmp(argv[i], "--version") == 0) {
            print_version();
            free(new_argv);
            return 0;
        }
        if (strcmp(argv[i], "-u") == 0) {
            do_unmount_flag = 1;
            continue;  /* Don't pass -u to FUSE */
        }
        if (strcmp(argv[i], "--version1") == 0) {
            force_v1_flag = 1;
            continue;  /* Don't pass --version1 to FUSE */
        }
        new_argv[new_argc++] = argv[i];
        /* Track mountpoint (last non-option argument) */
        if (argv[i][0] != '-') {
            mountpoint = argv[i];
        }
    }

    /* Handle unmount request */
    if (do_unmount_flag) {
        free(new_argv);
        if (!mountpoint) {
            fprintf(stderr, "Error: No mountpoint specified for unmount\n");
            return 1;
        }
        return do_unmount(mountpoint);
    }

    /* Ensure we have at least a mountpoint argument */
    if (new_argc < 2) {
        fprintf(stderr, "Error: No mountpoint specified\n\n");
        print_usage(argv[0]);
        free(new_argv);
        return 1;
    }

    /* Apply force_v1 flag if set */
    if (force_v1_flag) {
        printf("BadgeFS: Forcing protocol version 1 (legacy mode)\n");
        badgefs_backend_badgelink_force_v1();
    }

    /* Test connection before mounting to avoid invalid mount */
    printf("BadgeFS: Connecting to badge...\n");
    int conn_ret = badgefs_backend_badgelink_test_connection();
    if (conn_ret < 0) {
        fprintf(stderr, "BadgeFS: Cannot mount - badge not connected\n");
        free(new_argv);
        return 1;
    }
    printf("BadgeFS: Badge found, mounting filesystem\n");

    badgefs_set_backend(badgefs_backend_badgelink_get());

    /*
     * Get the FUSE operations structure and start FUSE.
     *
     * fuse_main() handles:
     *   - Argument parsing
     *   - Mounting the filesystem
     *   - Running the event loop
     *   - Unmounting on exit
     */
    const struct fuse_operations *ops = badgefs_get_operations();

    printf("BadgeFS: Starting FUSE filesystem...\n");
    printf("BadgeFS: Use '%s -u <mountpoint>' to unmount\n", argv[0]);

    int ret = fuse_main(new_argc, new_argv, ops, NULL);

    free(new_argv);

    if (ret != 0) {
        fprintf(stderr, "BadgeFS: fuse_main returned %d\n", ret);
    }

    return ret;
}
