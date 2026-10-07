/**
* @file arkrtos_posix.h
* @brief Ark RTOS POSIX compatibility layer - umbrella header.
* @details Lets an ordinary POSIX C program run as an Ark RTOS App with as few
*          source edits as possible. Everything maps onto SVC services the App
*          SDK already exposes, so nothing here knows an address, a partition
*          or a slot and the same source builds for any board.
*
*          Include this file instead of <pthread.h> / <semaphore.h> and the
*          rest; the individual headers stay usable on their own when a
*          project only needs one of them.
*
*          Tunables - define before including, or in the project options:
*            ARKRTOS_POSIX_THREAD_MAX   concurrent pthreads        (default 2)
*            ARKRTOS_POSIX_STACK_WORDS  stack words per pthread    (default 256)
*            ARKRTOS_POSIX_HEAP_SIZE    App side heap bytes        (default 1024)
*
*          The defaults are sized to fit the 8 KB RAM window a development
*          App slot gets out of the box: 2 * 1 KB of thread stack plus a 1 KB
*          arena leaves room for the App's own data and its main stack. Raise
*          them per project when the App slot has more RAM; the arenas are
*          static, so the cost is paid whether they are used or not.
*            ARKRTOS_POSIX_DEFAULT_PRIO default thread priority    (default 10)
*            ARKRTOS_POSIX_NO_TIMESPEC  platform already has struct timespec
*            ARKRTOS_POSIX_WRAP_STDLIB  also #define malloc/free/... over ours
*            ARKRTOS_POSIX_PTHREAD_KEY  thread-specific data (TLS)  (default 1)
*
* @note  Declarations only. arkrtos_posix.c has to be added to the App project
*        once; the memory it costs is the two static arenas below.
* @author xw
*/
#ifndef __ARKRTOS_POSIX_H__
#define __ARKRTOS_POSIX_H__

#include "arkrtos.h"

/* ------------------------------------------------------------- tunables */
#ifndef ARKRTOS_POSIX_THREAD_MAX
#define ARKRTOS_POSIX_THREAD_MAX    (2)
#endif
#ifndef ARKRTOS_POSIX_STACK_WORDS
#define ARKRTOS_POSIX_STACK_WORDS   (256u)
#endif
#ifndef ARKRTOS_POSIX_HEAP_SIZE
#define ARKRTOS_POSIX_HEAP_SIZE     (1024u)
#endif
#ifndef ARKRTOS_POSIX_DEFAULT_PRIO
#define ARKRTOS_POSIX_DEFAULT_PRIO  (10u)
#endif

/* Thread-specific data (pthread_key_*) and pthread_once.  Set to 0 to drop
 * both the declarations and the static pointer table arkrtos_posix.c keeps for
 * them (ARKRTOS_POSIX_KEY_MAX x (ARKRTOS_POSIX_THREAD_MAX + 1) pointers). */
#ifndef ARKRTOS_POSIX_PTHREAD_KEY
#define ARKRTOS_POSIX_PTHREAD_KEY   1
#endif

#include "arkrtos_posix_types.h"
#include "errno.h"
#include "fcntl.h"
#include "unistd.h"
#include "time.h"
#include "pthread.h"
#include "semaphore.h"
#include "mqueue.h"
#include "sys/stat.h"
#include "dirent.h"
#include "sys/socket.h"
#include "netinet/in.h"
#include "arpa/inet.h"


/* ----------------------------------------------------------- App memory.
 * A small first fit arena inside the App's own RAM. It is intentionally not
 * a kernel service: the kernel has no heap, the MPU already confines the App
 * to its own window, and an allocator that lives on the same side of the
 * privilege boundary as its callers is one less thing to get wrong.
 */
void *arkrtos_posix_malloc(uint32 size);
void  arkrtos_posix_free(void *ptr);
void *arkrtos_posix_calloc(uint32 nmemb, uint32 size);
void *arkrtos_posix_realloc(void *ptr, uint32 size);
uint32 arkrtos_posix_heap_total(void);
uint32 arkrtos_posix_heap_free_bytes(void);

/* Convenience helpers, same reason as the allocator: no libc dependency. */
void  *arkrtos_posix_memset(void *dst, int value, uint32 len);
void  *arkrtos_posix_memcpy(void *dst, const void *src, uint32 len);
uint32 arkrtos_posix_strlen(const char *s);

#if defined(ARKRTOS_POSIX_WRAP_STDLIB)
#define malloc(n)      arkrtos_posix_malloc(n)
#define free(p)        arkrtos_posix_free(p)
#define calloc(n, s)   arkrtos_posix_calloc((n), (s))
#define realloc(p, n)  arkrtos_posix_realloc((p), (n))
#endif

#endif /* __ARKRTOS_POSIX_H__ */
