/**
* @file sys/stat.h
* @brief stat()/fstat() for Ark RTOS Apps.
* @details The struct is deliberately small: the fields the VFS can actually
*          answer are here, and the ones it cannot (st_ino, st_nlink, st_uid,
*          st_mtime as a real clock) are absent rather than filled in with a
*          zero that a program would take for a fact. Ark RTOS has no RTC, so
*          st_mtime is seconds since boot - it is not a wall clock time and
*          the field name alone would suggest otherwise.
*
*          st_mode carries the POSIX S_IFMT type bits (pinned to the kernel's
*          ARKRTOS_PATH_S_IF* by a static assert) and no permission bits: the
*          kernel's model is "the App may or may not reach a path", not
*          "read for the group, write for the owner".
* @author xw
*/
#ifndef __ARKRTOS_SYS_STAT_H__
#define __ARKRTOS_SYS_STAT_H__

#include "arkrtos_posix_types.h"

struct stat
{
    arkrtos_mode_t st_mode;    /**< S_IFMT type bits only, no permissions */
    arkrtos_off_t  st_size;    /**< bytes; 0 for a directory or a device      */
    arkrtos_time_t st_mtime;   /**< seconds since boot, NOT wall clock time   */
};

#define S_IFMT   0xF000
#define S_IFCHR  0x2000
#define S_IFDIR  0x4000

/* A socket has no size and no offset. The value is this layer's own: no
 * socket descriptor is ever described to the kernel as a file, so nothing
 * depends on the kernel's S_IF* set growing a matching bit. */
#define S_IFSOCK 0xC000
#define S_IFREG  0x8000

#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)


/** Stat a path in the namespace. Returns 0, or -1 with errno set. */
int arkrtos_posix_stat(const char *path, struct stat *st);
/** Stat an open descriptor. A device reports S_IFCHR with size 0. */
int arkrtos_posix_fstat(int fd, struct stat *st);

#define stat(path, st)   arkrtos_posix_stat((path), (st))
#define fstat(fd, st)    arkrtos_posix_fstat((fd), (st))

#endif /* __ARKRTOS_SYS_STAT_H__ */
