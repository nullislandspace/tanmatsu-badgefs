/*
 * badgefs_backend_mem.h - In-memory storage backend declarations
 *
 * This backend stores the entire filesystem in RAM. Data is lost on unmount.
 * Useful for prototyping, testing, and as a template for custom backends.
 */

#ifndef BADGEFS_BACKEND_MEM_H
#define BADGEFS_BACKEND_MEM_H

#include "badgefs_backend.h"

/*
 * Filesystem node structure
 *
 * Each file or directory is represented by a badgefs_node.
 * Directories have children linked list; files have data buffer.
 */
struct badgefs_node {
    char *name;                     /* Entry name (not full path) */
    struct stat st;                 /* File attributes (mode, size, times) */

    /* File content (NULL for directories) */
    char *data;
    size_t data_capacity;           /* Allocated buffer size */

    /* Tree structure */
    struct badgefs_node *parent;    /* Parent directory (NULL for root) */
    struct badgefs_node *children;  /* First child (directories only) */
    struct badgefs_node *next;      /* Next sibling in parent's list */
};

/*
 * Get the in-memory backend implementation
 *
 * Returns a pointer to the static backend structure with all
 * function pointers initialized to the in-memory implementations.
 */
struct badgefs_backend *badgefs_backend_mem_get(void);

#endif /* BADGEFS_BACKEND_MEM_H */
