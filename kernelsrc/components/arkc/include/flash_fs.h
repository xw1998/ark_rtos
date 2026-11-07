/* Ark C Compiler - flash_fs.h
 * Filesystem abstraction layer: the compiler reads source code uniformly via fs_read_file.
 * Host uses stdio; STM32 uses W25Q SPI Flash.
 */
#ifndef ARK_FLASH_FS_H
#define ARK_FLASH_FS_H

#include "common.h"

/* Read the entire file into buf. Returns byte count read; -1 on failure.
 * If file is larger than max_len-1, it is truncated and buf is guaranteed NUL-terminated.
 */
int fs_read_file(const char *path, char *buf, int max_len);

/* Size of the file in bytes, or -1 if it cannot be opened / is unknown.
 * Callers use it to tell "the buffer was big enough" from "the file was cut
 * off": fs_read_file() truncates silently, so comparing sizes is the only way
 * to avoid compiling or linking half a file. */
int fs_file_size(const char *path);

/* Write binary data to output file/Flash. Returns bytes written; -1 on failure.
 * path "-" means stdout (host) or callback (STM32).
 */
int fs_write_file(const char *path, const unsigned char *data, int len);

/* Why the most recent fs_read_file / fs_write_file failed, as a short phrase
 * ("No such file or directory"). Never NULL; backends that cannot know the
 * reason return a generic phrase rather than an empty string. Diagnostics
 * print this verbatim, so it must read like the C library's strerror(). */
const char *fs_last_error(void);

/* List all files on the flash FS (STM32 only; prints via ark_log).
 * Returns the number of files found, or -1 if the backing store cannot be
 * listed at all (e.g. the host port, which has no flash FS).
 */
int fs_list_files(void);

#endif /* ARK_FLASH_FS_H */

/* --- RAM-resident files (STM32 port) ----------------------------------
 * The board has to compile sources it produced itself (and feed the
 * resulting objects back into -link) without a flash filesystem, so the
 * port keeps a few flat-name slots in RAM.  Lookups try RAM first, then
 * the flash table, which keeps the flash path working unchanged.
 * fs_ram_put("name", data, -1) stores data as a NUL-terminated string. */
int  fs_ram_put(const char *name, const char *data, int len);
int  fs_ram_count(void);
const char *fs_ram_name_at(int i);
int  fs_ram_size_at(int i);
void fs_ram_clear(void);
