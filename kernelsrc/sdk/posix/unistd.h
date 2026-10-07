/**
* @file unistd.h
* @brief Minimal <unistd.h> for Ark RTOS Apps.
* @details Maps the POSIX names a portable C program actually uses onto the
*          App SDK: read/write/close/lseek act on a file descriptor from
*          open() (a path or a device), sleep/usleep map onto the scheduler,
*          and there is no process model at all (fork, exec, getpid beyond a
*          stub) because an App is a single address space with threads, not
*          a process tree.
* @author xw
*/
#ifndef __ARKRTOS_UNISTD_H__
#define __ARKRTOS_UNISTD_H__

#include "arkrtos_posix_types.h"
#include "fcntl.h"

#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

/* Seek origins. Same numbers as the kernel's ARKRTOS_PATH_SEEK_*. */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

arkrtos_ssize_t arkrtos_posix_read(int fd, void *buf, uint32 count);
arkrtos_ssize_t arkrtos_posix_write(int fd, const void *buf, uint32 count);
int           arkrtos_posix_close(int fd);
/** Move the offset of an open path. Returns the new absolute offset, or -1
 *  with errno set; ESPIPE when the descriptor is a device (no offset). */
arkrtos_off_t   arkrtos_posix_lseek(int fd, arkrtos_off_t off, int whence);
/** Remove a file by path. A directory needs rmdir, never unlink. */
int           arkrtos_posix_unlink(const char *path);
/** Remove an empty directory. A file is refused, not silently taken. */
int           arkrtos_posix_rmdir(const char *path);
/** Rename or move a path inside one mount. Returns 0, or -1 with errno set. */
int           arkrtos_posix_rename(const char *old_path, const char *new_path);

/** Suspend the calling thread for whole seconds (scheduler based). */
uint32 arkrtos_posix_sleep(uint32 seconds);
/** Suspend the calling thread for microseconds. Ark RTOS waits in ms, so the
 *  value is rounded UP: a sleep never returns early. */
int    arkrtos_posix_usleep(arkrtos_useconds_t usec);
/** Suspend until the given absolute time on the chosen clock. */
int    arkrtos_posix_nanosleep(const struct timespec *req, struct timespec *rem);
/** Wall clock seconds since boot - Ark RTOS has no RTC. */
arkrtos_time_t arkrtos_posix_time(arkrtos_time_t *tloc);
int    arkrtos_posix_clock_gettime(arkrtos_clockid_t clk, struct timespec *ts);

/* Aliases so plain POSIX source compiles unchanged. */
#define read(fd, buf, n)        arkrtos_posix_read((fd), (buf), (n))
#define write(fd, buf, n)       arkrtos_posix_write((fd), (buf), (n))
#define close(fd)               arkrtos_posix_close(fd)
#define lseek(fd, off, whence)  arkrtos_posix_lseek((fd), (off), (whence))
#define unlink(path)            arkrtos_posix_unlink(path)
#define rmdir(path)             arkrtos_posix_rmdir(path)
#define rename(oldp, newp)      arkrtos_posix_rename((oldp), (newp))
#define sleep(s)                arkrtos_posix_sleep(s)
#define usleep(us)              arkrtos_posix_usleep(us)
#define nanosleep(req, rem)     arkrtos_posix_nanosleep((req), (rem))
#define time(t)                 arkrtos_posix_time(t)
#define clock_gettime(c, ts)    arkrtos_posix_clock_gettime((c), (ts))

#endif /* __ARKRTOS_UNISTD_H__ */
