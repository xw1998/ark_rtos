/**
* @file semaphore.h
* @brief POSIX unnamed semaphores for Ark RTOS Apps.
* @details Backed by the kernel semaphore service (SVC 0x15). sem_t holds the
*          kernel handle, so sem_init / sem_destroy are the only lifecycle
*          calls and no static storage is needed.
* @author xw
*/
#ifndef __ARKRTOS_SEMAPHORE_H__
#define __ARKRTOS_SEMAPHORE_H__

#include "arkrtos_posix_types.h"

#define SEM_FAILED  ((arkrtos_sem_t *)0)
#define SEM_VALUE_MAX (0x7fffffff)

/** Opaque to the caller: only arkrtos_posix.c knows what is inside. */
typedef struct
{
    int32 handle;
} arkrtos_sem_t;

/* The POSIX spelling, so `sem_t s;` compiles as written. */
typedef arkrtos_sem_t sem_t;

int arkrtos_posix_sem_init(arkrtos_sem_t *sem, int pshared, uint32 value);
int arkrtos_posix_sem_destroy(arkrtos_sem_t *sem);
int arkrtos_posix_sem_wait(arkrtos_sem_t *sem);
int arkrtos_posix_sem_trywait(arkrtos_sem_t *sem);
int arkrtos_posix_sem_timedwait(arkrtos_sem_t *sem, uint32 timeout_ms);
int arkrtos_posix_sem_post(arkrtos_sem_t *sem);
int arkrtos_posix_sem_getvalue(arkrtos_sem_t *sem, int *value);

#define sem_init(s, pshared, v)     arkrtos_posix_sem_init((s), (pshared), (v))
#define sem_destroy(s)              arkrtos_posix_sem_destroy(s)
#define sem_wait(s)                 arkrtos_posix_sem_wait(s)
#define sem_trywait(s)              arkrtos_posix_sem_trywait(s)
#define sem_timedwait(s, ms)        arkrtos_posix_sem_timedwait((s), (ms))
#define sem_post(s)                 arkrtos_posix_sem_post(s)
#define sem_getvalue(s, v)          arkrtos_posix_sem_getvalue((s), (v))

#endif /* __ARKRTOS_SEMAPHORE_H__ */
