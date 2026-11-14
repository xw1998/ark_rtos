/**
* @file arkrtos_fs.h
* @brief Ark RTOS file system facade - littlefs over a registered block device
* @details One volume at a time, mounted explicitly. That is a deliberate
*          limit, not an oversight: the kernel has one caller (the shell today,
*          an SVC handler later) and a single mount point keeps the RAM cost
*          (two caches + lookahead, all static) and the failure modes small.
*          Multiple volumes can be added later by turning g_fs into an array.
*
*          The volume is described by three numbers - device name, offset,
*          length - so the same code mounts a file system on the external NOR,
*          on the internal flash pool, or on anything a future board registers
*          with arkrtos_blk. Offset 0 of the volume is offset `offset` of the
*          device: the FS never sees absolute addresses.
*
*          littlefs is asked for one block = one erase unit of the underlying
*          device. That keeps "erase" a single device erase and avoids the
*          classic bug of programming a 4 KiB file block inside a 128 KiB
*          sector that a later erase wipes.
*
*          Every entry point returns 0 on success and -1 on failure. The raw
*          littlefs error is kept in arkrtos_fs_last_error() so the caller can
*          report what the file system actually said instead of a guess.
*
* @author xw
* @date 2026.09.21
*/
#ifndef ARKRTOS_FS_H
#define ARKRTOS_FS_H

#include "arkrtos_types.h"

/* 返回码：失败一律回 -1，具体原因看 arkrtos_fs_last_error()。
 * 关闭档（ARKRTOS_USE_FS=0）的桩函数故意回同一个值：调用方不该能区分
 * "没编进来"和"板上没卷"，这两件事对本模块是同一回事。值不得单独修改。 */
#define ARKRTOS_FS_FAIL          (-1)


#ifdef __cplusplus
extern "C" {
#endif

/** Longest path accepted, including the leading '/' and the terminator */
#define ARKRTOS_FS_PATH_MAX   (64u)

/** Bytes buffered for metadata/reads and for one open file */
#define ARKRTOS_FS_CACHE_SIZE (256u)

/**
 * @brief Mount the volume described by (dev, offset, size).
 * @param dev    name of a registered block device, e.g. "nor0"
 * @param offset byte offset of the volume inside that device
 * @param size   volume length in bytes; 0 = to the end of the device
 * @return 0 on success, -1 on failure (not mounted, nothing in flash, or a
 *         block device that does not exist / is not up)
 * @note Mounting does NOT format. An unformatted volume fails here, which is
 *       the honest answer - use arkrtos_fs_format() first, on purpose.
 */
int32 arkrtos_fs_mount(const char *dev, uint32 offset, uint32 size);

/**
 * @brief Mount the board's default volume (see ARKRTOS_FS_* in the partition
 *        header). Returns -1 with last_error = -2 when no volume is configured.
 */
int32 arkrtos_fs_mount_default(void);

/** @brief Drop the mount. Safe to call when nothing is mounted. */
int32 arkrtos_fs_unmount(void);

/** @brief 1 while a volume is mounted */
uint8 arkrtos_fs_mounted(void);

/**
 * @brief Erase and create an empty file system on the volume.
 * @note  Destructive: everything on the volume is lost. Works on a volume that
 *        is not mounted yet, and mounts it on success.
 */
int32 arkrtos_fs_format(const char *dev, uint32 offset, uint32 size);

/** @brief Total and used bytes of the mounted volume (used = blocks in use) */
int32 arkrtos_fs_stat(uint32 *total, uint32 *used);

/** @brief Create/truncate path and write len bytes */
/**
 * @brief Size and type of one entry (file or directory).
 * @param path   absolute path inside the mounted volume
 * @param size   receives the size in bytes, 0 for a directory (may be NULL)
 * @param is_dir receives 1 for a directory, 0 for a file (may be NULL)
 * @return 0 on success, -1 on failure (nothing mounted, bad path, not found)
 */
int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_dir);

int32 arkrtos_fs_write_file(const char *path, const uint8 *data, uint32 len);

/**
 * @brief Set the length of one file, leaving its content alone up to @p size.
 * @details Shrinking drops the tail, growing fills the new bytes with zeros
 *          (POSIX truncate). The file must already exist: this call never
 *          creates one. It shares the single file cache with the streaming
 *          entry points and refuses with ARKRTOS_FS_ERR_BUSY while a stream is
 *          open, because resizing underneath an open stream is exactly the
 *          half-true state this facade does not hand out.
 * @return 0 on success, -1 on failure (nothing mounted, bad path, not found,
 *         or a stream already open); arkrtos_fs_last_error() has the reason.
 */
int32 arkrtos_fs_truncate(const char *path, uint32 size);

/** @brief Read at most max bytes; *out_len gets the real length */
int32 arkrtos_fs_read_file(const char *path, uint8 *buf, uint32 max, uint32 *out_len);

/**
 * @brief Open one file for chunked (streaming) reading.
 * @details A 20 KiB image does not fit a fixed scratch buffer, so the file
 *          stays open across calls and the caller drives it. While a stream
 *          is open, the one-shot entry points that share the single file cache
 *          (arkrtos_fs_read_file / arkrtos_fs_write_file) refuse to run instead of
 *          silently corrupting whichever is in flight - that refusal is
 *          ARKRTOS_FS_ERR_BUSY.
 * @return 0 on success, -1 on failure (nothing mounted, bad path, not found,
 *         a directory, or another stream already open)
 */
int32 arkrtos_fs_open_read(const char *path);

/**
 * @brief Read the next chunk of the open read stream.
 * @param buf destination, at least max bytes
 * @param max largest chunk to ask for
 * @return bytes read (1..max), 0 at end of file, -1 on failure
 * @note 0 means "the file ended", never "nothing right now": the call is
 *       synchronous, so a caller loop may stop on it.
 */
int32 arkrtos_fs_read_next(uint8 *buf, uint32 max);

/** @brief Close the read stream. Closing when none is open is not an error. */
int32 arkrtos_fs_close_read(void);

/**
 * @brief Open one file for chunked writing (creates or truncates).
 * @return 0 on success, -1 on failure; the rules of arkrtos_fs_open_read apply
 */
int32 arkrtos_fs_open_write(const char *path);

/** @brief Append one chunk to the open write stream. @return 0 on success */
int32 arkrtos_fs_write_next(const uint8 *buf, uint32 len);

/**
 * @brief Close the write stream, which is what commits the file.
 * @return 0 on success, -1 on failure (littlefs reports the reason); closing
 *         when none is open is not an error
 */
int32 arkrtos_fs_close_write(void);

/** @brief Remove a file. @return 0 on success, -1 (not found / is a dir) */
int32 arkrtos_fs_remove(const char *path);

/** @brief Create a directory. @return 0 on success, -1 (exists / bad path) */
int32 arkrtos_fs_mkdir(const char *path);

/** @brief Callback for arkrtos_fs_list(); returns 0 to continue, -1 to stop */
typedef int (*arkrtos_fs_list_cb_t)(const char *name, uint32 size, uint8 is_dir, void *arg);

/** @brief List one directory ("/" for the root) */
int32 arkrtos_fs_list(const char *dir, arkrtos_fs_list_cb_t cb, void *arg);

/**
 * @brief Error of the last failed call (0 when none)
 * @details Whatever littlefs returned is passed through unchanged; failures
 *          raised by this facade itself use codes <= -1000 (ARKRTOS_FS_ERR_*).
 *          Print the number, and arkrtos_fs_error_name() adds the symbol.
 */
/** @brief Read len bytes starting at byte offset off (random access read).
 *  A partial read at end of file returns fewer bytes than len, with that
 *  number in *out_len - never an error. */
int32 arkrtos_fs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off,
                      uint32 *out_len);

/** @brief Move/rename one entry. Both paths are inside the volume. */
int32 arkrtos_fs_rename(const char *old_path, const char *new_path);

/** @brief Copy the names of one directory into a caller buffer, one per line.
 *  Only complete entries are copied: when the buffer runs out, the last
 *  partial name is dropped and *count counts what actually fits, so the
 *  caller can tell a full listing from a truncated one. */
int32 arkrtos_fs_list_names(const char *dir, char *out, uint32 out_size,
                         uint32 *count);

/**
 * @brief Geometry of the volume that is mounted, so a caller can check a
 *        request against it instead of silently reusing whatever is up.
 * @param dev/offset/size receive the values the mount actually used; a
 *        requested size of 0 is already resolved to "to the end of the device".
 * @return 0 while a volume is mounted, -1 when there is nothing to report.
 */
int32 arkrtos_fs_volume(const char **dev, uint32 *offset, uint32 *size);

int32 arkrtos_fs_last_error(void);

/**
 * @brief 1 = the last call failed only because the entry does not exist
 * @details "No such entry" is a normal answer, not a fault: a caller that has
 *          to choose between opening and creating (the VFS bridge does) must
 *          be able to tell it apart from a real I/O failure.  It is a separate
 *          predicate instead of "stat failures leave last_error alone",
 *          because clearing the recorded cause would erase the one number an
 *          operator is told to look at.
 * @return 1 when arkrtos_fs_last_error() is LFS_ERR_NOENT, 0 otherwise.
 */
uint8 arkrtos_fs_last_is_noent(void);

/**
 * @brief 1 = the last call failed only because the backend is out of space
 * @details Resizing a file is the one call where "too big" is a normal answer
 *          a caller can act on (it may retry smaller). littlefs reports it as
 *          LFS_ERR_NOSPC, and this predicate is the single place that number
 *          is turned into a decision, so no other file has to know it.
 * @return 1 when arkrtos_fs_last_error() is LFS_ERR_NOSPC, 0 otherwise.
 */
uint8 arkrtos_fs_last_is_nospc(void);

/**
 * @brief 1 = this error number means "there is no littlefs volume here"
 * @details A mount over blank NOR, over another file system, or over a volume
 *          whose superblock is damaged returns LFS_ERR_CORRUPT (or INVAL) -
 *          littlefs never formats on its own, and neither does this layer.
 *          The predicate exists so a caller can print the *next step*
 *          ("format it on purpose") instead of guessing a cause from the
 *          number.  It is a diagnosis of the failure, never a licence to
 *          write: nothing in this file formats except arkrtos_fs_format().
 * @return 1 when lfs_err is LFS_ERR_CORRUPT or LFS_ERR_INVAL, 0 otherwise.
 */
uint8 arkrtos_fs_err_is_unformatted(int32 lfs_err);

/**
 * @brief Human readable name of an error code, for reports.
 * @details Callers must print the number the file system returned, not a
 *          guessed cause; this only adds the short symbol on top of it.
 */
const char *arkrtos_fs_error_name(int32 lfs_err);

#ifdef __cplusplus
}
#endif

#endif /* ARKRTOS_FS_H */
