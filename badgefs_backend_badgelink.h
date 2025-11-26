/*
 * badgefs_backend_badgelink.h - BadgeLink backend for BadgeFS
 *
 * This backend provides access to a Tanmatsu badge filesystem
 * via the BadgeLink USB protocol.
 *
 * Filesystem layout:
 *   /sd     -> SD card on badge
 *   /int    -> Internal memory on badge
 *   /appfs  -> Application filesystem (flat directory of apps)
 */

#ifndef BADGEFS_BACKEND_BADGELINK_H
#define BADGEFS_BACKEND_BADGELINK_H

#include "badgefs_backend.h"

/*
 * Get the BadgeLink backend instance.
 * Returns pointer to static backend structure.
 */
struct badgefs_backend *badgefs_backend_badgelink_get(void);

/*
 * Test connection to badge before mounting.
 * This should be called before fuse_main() to verify the badge is connected.
 * Returns 0 on success, negative error code on failure.
 */
int badgefs_backend_badgelink_test_connection(void);

#endif /* BADGEFS_BACKEND_BADGELINK_H */
