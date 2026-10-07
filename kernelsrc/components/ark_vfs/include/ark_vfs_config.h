/**
* @file ark_vfs_config.h
* @brief Compile time limits.  Override by putting your own copy earlier on
*        the include path, or by defining these on the command line.
*
* Nothing here allocates: these are array sizes, so the whole library has a
* fixed, known footprint before the first line of code runs.
*/
#ifndef __ARK_VFS_CONFIG_H__
#define __ARK_VFS_CONFIG_H__

/** @brief How many mount points can exist at once. */
#ifndef ARK_VFS_MAX_MOUNTS
#define ARK_VFS_MAX_MOUNTS      4
#endif

/** @brief How many files/directories can be open at once. */
#ifndef ARK_VFS_MAX_FDS
#define ARK_VFS_MAX_FDS         8
#endif

/** @brief Longest accepted path, including the terminating NUL. */
#ifndef ARK_VFS_PATH_MAX
#define ARK_VFS_PATH_MAX        64
#endif

/** @brief Longest single path component, including the terminating NUL. */
#ifndef ARK_VFS_NAME_MAX
#define ARK_VFS_NAME_MAX        32
#endif

/** @brief Set to 1 to compile in the argument checks that cost cycles.
 *
 *  Off by default.  Even when off, NULL and empty paths are still refused -
 *  those are cheap checks that stop a crash rather than a bug. */
#ifndef ARK_VFS_STRICT_ARGS
#define ARK_VFS_STRICT_ARGS     0
#endif

/** @brief Merge child mount points into their parent directory listing.
 *
 *  With this on, "ls /" lists the mounts whose target is a direct child of
 *  "/" (e.g. "/dev", "/proc") even though the root filesystem was never told
 *  they were mounted over it.  It costs two bytes per open directory
 *  (ARK_VFS_MAX_FDS * 2 in total) and nothing at all for directories that
 *  have no child mounts.  Set to 0 to compile the merging out completely: the
 *  library then behaves exactly as it did before this option existed. */
#ifndef ARK_VFS_MERGE_MOUNTS
#define ARK_VFS_MERGE_MOUNTS    1
#endif

#if ARK_VFS_MERGE_MOUNTS && (ARK_VFS_MAX_MOUNTS > 8)
#error "ARK_VFS_MERGE_MOUNTS packs the child-mount set into a uint8_t; widen the mask or lower ARK_VFS_MAX_MOUNTS."
#endif

#define ARK_VFS_VERSION_MAJOR   0
#define ARK_VFS_VERSION_MINOR   1
#define ARK_VFS_VERSION_PATCH   0

#endif /* __ARK_VFS_CONFIG_H__ */
