/**
* @file pthread.h
* @brief POSIX threads for Ark RTOS Apps.
* @details Ark RTOS has threads, a scheduler and synchronization, but no user
*          mode way to create a thread: a thread needs a TCB and a scheduler
*          slot, and only the kernel owns those. pthread_create therefore goes
*          through the thread service (SVC 0x1B) added for this purpose.
*
*          Two things a POSIX program may expect are absent on purpose,
*          because the kernel cannot honour them and pretending would be a
*          silent lie:
*            - no priority inheritance on mutexes (the kernel mutex does
*              inherit, but a POSIX program cannot observe or set it),
*            - no condition variables (they need a kernel wait queue with a
*              signal/broadcast primitive that does not exist yet).
*
* @author xw
*/
#ifndef __ARKRTOS_PTHREAD_H__
#define __ARKRTOS_PTHREAD_H__

#include "arkrtos_posix_types.h"

/* Thread-specific data (pthread_key_*) and pthread_once.  Kept behind a
 * switch because a program that never uses them should not pay for the
 * static pointer table arkrtos_posix.c keeps for them. */
#ifndef ARKRTOS_POSIX_PTHREAD_KEY
#define ARKRTOS_POSIX_PTHREAD_KEY  1
#endif

typedef uint32 arkrtos_pthread_t;
#define ARKRTOS_PTHREAD_NULL  ((arkrtos_pthread_t)0)

/** Thread attributes. Anything left at 0 takes the built-in default, so a
 *  plain pthread_create(t, NULL, fn, arg) call needs no attributes at all. */
typedef struct
{
    void  *stackaddr;   /* 0 = take a stack from the SDK pool            */
    uint32 stacksize;   /* bytes, 0 = ARKRTOS_POSIX_STACK_WORDS * 4        */
    uint32 priority;    /* kernel priority, 0 = ARKRTOS_POSIX_DEFAULT_PRIO */
    uint32 period_ms;   /* 0 = event driven (blocks until it waits)      */
} arkrtos_pthread_attr_t;

typedef arkrtos_pthread_attr_t pthread_attr_t;
typedef arkrtos_pthread_t      pthread_t;

typedef struct
{
    int32 handle;       /* kernel mutex handle, -1 = not created yet */
} arkrtos_pthread_mutex_t;

typedef arkrtos_pthread_mutex_t pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER  { -1 }

int  arkrtos_posix_pthread_attr_init(arkrtos_pthread_attr_t *attr);
int  arkrtos_posix_pthread_create(arkrtos_pthread_t *thread,
                                const arkrtos_pthread_attr_t *attr,
                                void *(*start_routine)(void *), void *arg);
int  arkrtos_posix_pthread_join(arkrtos_pthread_t thread, void **retval);
void arkrtos_posix_pthread_exit(void *retval);
arkrtos_pthread_t arkrtos_posix_pthread_self(void);

int  arkrtos_posix_mutex_init(arkrtos_pthread_mutex_t *m);
int  arkrtos_posix_mutex_destroy(arkrtos_pthread_mutex_t *m);
int  arkrtos_posix_mutex_lock(arkrtos_pthread_mutex_t *m);
int  arkrtos_posix_mutex_trylock(arkrtos_pthread_mutex_t *m);
int  arkrtos_posix_mutex_unlock(arkrtos_pthread_mutex_t *m);

#define pthread_attr_init(a)        arkrtos_posix_pthread_attr_init(a)
#define pthread_create(t, a, f, g)  arkrtos_posix_pthread_create((t), (a), (f), (g))
#define pthread_join(t, r)          arkrtos_posix_pthread_join((t), (r))
#define pthread_exit(r)             arkrtos_posix_pthread_exit(r)
#define pthread_self()              arkrtos_posix_pthread_self()
#define pthread_mutex_init(m, a)    arkrtos_posix_mutex_init(m)
#define pthread_mutex_destroy(m)    arkrtos_posix_mutex_destroy(m)
#define pthread_mutex_lock(m)       arkrtos_posix_mutex_lock(m)
#define pthread_mutex_trylock(m)    arkrtos_posix_mutex_trylock(m)
#define pthread_mutex_unlock(m)     arkrtos_posix_mutex_unlock(m)

#define pthread_cond_init(c, a)         arkrtos_posix_cond_init(c)
#define pthread_cond_destroy(c)         arkrtos_posix_cond_destroy(c)
#define pthread_cond_wait(c, m)         arkrtos_posix_cond_wait((c), (m))
#define pthread_cond_timedwait(c, m, t) arkrtos_posix_cond_timedwait((c), (m), (t))
#define pthread_cond_signal(c)          arkrtos_posix_cond_signal(c)
#define pthread_cond_broadcast(c)       arkrtos_posix_cond_broadcast(c)

/* Condition variable: a kernel object bound on first use, so
 * PTHREAD_COND_INITIALIZER works exactly like the mutex one. */
typedef struct
{
    int32 handle;       /* kernel cond handle, -1 = not created yet */
} arkrtos_pthread_cond_t;

typedef arkrtos_pthread_cond_t pthread_cond_t;

#define PTHREAD_COND_INITIALIZER  { -1 }

int  arkrtos_posix_cond_init(arkrtos_pthread_cond_t *c);
int  arkrtos_posix_cond_destroy(arkrtos_pthread_cond_t *c);
int  arkrtos_posix_cond_wait(arkrtos_pthread_cond_t *c, arkrtos_pthread_mutex_t *m);
int  arkrtos_posix_cond_timedwait(arkrtos_pthread_cond_t *c, arkrtos_pthread_mutex_t *m,
                                const struct timespec *abstime);
int  arkrtos_posix_cond_signal(arkrtos_pthread_cond_t *c);
int  arkrtos_posix_cond_broadcast(arkrtos_pthread_cond_t *c);

/* One row per POSIX thread slot, for the App side shell dump.  Plain data,
 * no printing: the App decides how to show it. */
typedef struct
{
    uint32 in_use;
    int32  task_id;          /* kernel task id, -1 once the thread left */
    int32  gate;             /* start gate handle                        */
    int32  done;             /* completion handle                        */
} arkrtos_pth_dbg_t;

/* Copies ARKRTOS_POSIX_THREAD_MAX rows into out.  Returns 0 on success. */
int32 arkrtos_posix_pth_dbg_slots(arkrtos_pth_dbg_t *out);

extern volatile int32 arkrtos_posix_pth_dbg_claim;   /* slot claimed by task id */
extern volatile int32 arkrtos_posix_pth_dbg_boot;    /* claimed via boot_slot   */
extern volatile int32 arkrtos_posix_pth_dbg_lost;    /* gave up: no slot to be  */
extern volatile int32 arkrtos_posix_pth_dbg_post;    /* trampoline posted done  */
extern volatile int32 arkrtos_posix_pth_dbg_exit;    /* pthread_exit() posted   */


#if (ARKRTOS_POSIX_PTHREAD_KEY == 1)

/* ---- thread-specific data (TLS) -----------------------------------------
 * pthread_key_* maps onto a static per-App table, keyed by the thread slot
 * this layer hands out (pthread_self() returns slot + 1; 0 is the App main
 * task).  No kernel object is involved, so no new SVC either.
 */
typedef uint32 arkrtos_pthread_key_t;
typedef arkrtos_pthread_key_t pthread_key_t;
#ifndef ARKRTOS_POSIX_KEY_MAX
#define ARKRTOS_POSIX_KEY_MAX   (8u)
#endif

int  arkrtos_posix_key_create(pthread_key_t *key, void (*destructor)(void *));
int  arkrtos_posix_key_delete(pthread_key_t key);
int  arkrtos_posix_setspecific(pthread_key_t key, const void *value);
void *arkrtos_posix_getspecific(pthread_key_t key);

#define pthread_key_create(k, d)   arkrtos_posix_key_create((k), (d))
#define pthread_key_delete(k)      arkrtos_posix_key_delete(k)
#define pthread_setspecific(k, v)  arkrtos_posix_setspecific((k), (v))
#define pthread_getspecific(k)     arkrtos_posix_getspecific(k)

/* pthread_once: runs init_routine exactly once, on the first caller.  The
 * body runs with the scheduler lock held, so on a single core a second
 * caller cannot observe "started but not finished".  init_routine must not
 * block (it may neither sleep nor wait on a kernel object). */
typedef uint32 arkrtos_pthread_once_t;
typedef arkrtos_pthread_once_t pthread_once_t;
#define PTHREAD_ONCE_INIT  0u

int  arkrtos_posix_once(pthread_once_t *once_control, void (*init_routine)(void));
#define pthread_once(o, f)  arkrtos_posix_once((o), (f))

#endif /* ARKRTOS_POSIX_PTHREAD_KEY */


/* ==========================================================================
 * 读写锁与屏障
 *
 * 两者都不是内核对象，而是**架在已有的内核互斥锁 + 条件变量上**的用户态
 * 组合：读写锁 = 1 把互斥锁 + 2 个条件变量（读者等一个、写者等一个），
 * 屏障 = 1 把互斥锁 + 1 个条件变量。这样做的好处是不新增 SVC、不改内核，
 * 代价是**每个对象要占内核的锁/条件变量表名额**（ARKRTOS_MTX_NUM /
 * ARKRTOS_COND_NUM），表满了 init 返回 ENOMEM，不会静默降级。
 *
 * 如实写明的边界：
 *   - 读写锁是**写者优先**：只要有人在等写锁，新来的读者也会排队。读优先
 *     会让写者饿死，而"读多写少"的常见负载里写者饿死是最难查的故障。
 *     pthread_rwlockattr_setkind_np(PTHREAD_PREFER_READER_NP) 因此返回
 *     ENOTSUP，而不是接受参数再按写者优先跑（那是给了个假选项）。
 *   - 读写锁**不可递归**：持读锁再去拿写锁、持写锁再拿读锁都会阻塞到死，
 *     与 POSIX 允许的实现一致，但本实现不做 EDEADLK 检测。
 *   - 屏障可重复使用（内部有 generation 计数），唤醒时计数与代次在同一把
 *     互斥锁下更新，所以"最后到达者清零、其余人看到代次变化"不会丢唤醒。
 *     销毁一个还有人等着的屏障是未定义行为，POSIX 也不保证。
 * ========================================================================== */

typedef struct
{
    arkrtos_pthread_mutex_t mtx;    /* 保护下面几个计数 */
    arkrtos_pthread_cond_t  rcond;  /* 读者在这里排队 */
    arkrtos_pthread_cond_t  wcond;  /* 写者在这里排队 */
    int32 readers;                  /* 当前持读锁的线程数 */
    int32 writer;                   /* 1 = 写锁被持有 */
    int32 wait_writers;             /* 正在等写锁的线程数（写者优先的门槛） */
    int32 bound;                    /* 0 = 三个内核对象还没建 */
} arkrtos_pthread_rwlock_t;

typedef arkrtos_pthread_rwlock_t pthread_rwlock_t;

#define PTHREAD_RWLOCK_INITIALIZER  { { -1 }, { -1 }, { -1 }, 0, 0, 0, 0 }

int arkrtos_posix_rwlock_init(arkrtos_pthread_rwlock_t *rw);
int arkrtos_posix_rwlock_destroy(arkrtos_pthread_rwlock_t *rw);
int arkrtos_posix_rwlock_rdlock(arkrtos_pthread_rwlock_t *rw);
int arkrtos_posix_rwlock_tryrdlock(arkrtos_pthread_rwlock_t *rw);
int arkrtos_posix_rwlock_timedrdlock(arkrtos_pthread_rwlock_t *rw,
                                     const struct timespec *abstime);
int arkrtos_posix_rwlock_wrlock(arkrtos_pthread_rwlock_t *rw);
int arkrtos_posix_rwlock_trywrlock(arkrtos_pthread_rwlock_t *rw);
int arkrtos_posix_rwlock_timedwrlock(arkrtos_pthread_rwlock_t *rw,
                                     const struct timespec *abstime);
int arkrtos_posix_rwlock_unlock(arkrtos_pthread_rwlock_t *rw);

#define pthread_rwlock_init(r, a)          arkrtos_posix_rwlock_init(r)
#define pthread_rwlock_destroy(r)          arkrtos_posix_rwlock_destroy(r)
#define pthread_rwlock_rdlock(r)           arkrtos_posix_rwlock_rdlock(r)
#define pthread_rwlock_tryrdlock(r)        arkrtos_posix_rwlock_tryrdlock(r)
#define pthread_rwlock_timedrdlock(r, t)   arkrtos_posix_rwlock_timedrdlock((r), (t))
#define pthread_rwlock_wrlock(r)           arkrtos_posix_rwlock_wrlock(r)
#define pthread_rwlock_trywrlock(r)        arkrtos_posix_rwlock_trywrlock(r)
#define pthread_rwlock_timedwrlock(r, t)   arkrtos_posix_rwlock_timedwrlock((r), (t))
#define pthread_rwlock_unlock(r)           arkrtos_posix_rwlock_unlock(r)

/* 属性对象只是为了不打断移植过来的代码：本实现只有一种行为（进程内、
 * 写者优先），所以属性里能设的只有"私有"这一档，其余一律 ENOTSUP。 */
typedef struct
{
    int32 pshared;      /* PTHREAD_PROCESS_PRIVATE / SHARED */
    int32 kind;         /* PTHREAD_PREFER_READER_NP / WRITER_NONRECURSIVE_NP */
} arkrtos_pthread_rwlockattr_t;

typedef arkrtos_pthread_rwlockattr_t pthread_rwlockattr_t;

#define PTHREAD_PROCESS_PRIVATE              0
#define PTHREAD_PROCESS_SHARED               1
#define PTHREAD_PREFER_READER_NP             0
#define PTHREAD_PREFER_WRITER_NONRECURSIVE_NP 1

int arkrtos_posix_rwlockattr_init(arkrtos_pthread_rwlockattr_t *a);
int arkrtos_posix_rwlockattr_destroy(arkrtos_pthread_rwlockattr_t *a);
int arkrtos_posix_rwlockattr_setpshared(arkrtos_pthread_rwlockattr_t *a, int32 v);
int arkrtos_posix_rwlockattr_getpshared(const arkrtos_pthread_rwlockattr_t *a, int32 *v);
int arkrtos_posix_rwlockattr_setkind(arkrtos_pthread_rwlockattr_t *a, int32 v);
int arkrtos_posix_rwlockattr_getkind(const arkrtos_pthread_rwlockattr_t *a, int32 *v);

#define pthread_rwlockattr_init(a)          arkrtos_posix_rwlockattr_init(a)
#define pthread_rwlockattr_destroy(a)       arkrtos_posix_rwlockattr_destroy(a)
#define pthread_rwlockattr_setpshared(a, v) arkrtos_posix_rwlockattr_setpshared((a), (v))
#define pthread_rwlockattr_getpshared(a, v) arkrtos_posix_rwlockattr_getpshared((a), (v))
#define pthread_rwlockattr_setkind_np(a, v) arkrtos_posix_rwlockattr_setkind((a), (v))
#define pthread_rwlockattr_getkind_np(a, v) arkrtos_posix_rwlockattr_getkind((a), (v))

/* 屏障 */
typedef struct
{
    arkrtos_pthread_mutex_t mtx;
    arkrtos_pthread_cond_t  cond;
    uint32 count;       /* 本轮已到达数 */
    uint32 gen;         /* 代次：每放行一轮 +1，唤醒条件用它，避免丢唤醒 */
    uint32 n;           /* 阈值（参与线程数） */
    int32  bound;       /* 0 = 两个内核对象还没建 */
} arkrtos_pthread_barrier_t;

typedef arkrtos_pthread_barrier_t pthread_barrier_t;

#define PTHREAD_BARRIER_SERIAL_THREAD  (-1)

int arkrtos_posix_barrier_init(arkrtos_pthread_barrier_t *b, uint32 count);
int arkrtos_posix_barrier_destroy(arkrtos_pthread_barrier_t *b);
int arkrtos_posix_barrier_wait(arkrtos_pthread_barrier_t *b);

#define pthread_barrier_init(b, a, n)  arkrtos_posix_barrier_init((b), (n))
#define pthread_barrier_destroy(b)     arkrtos_posix_barrier_destroy(b)
#define pthread_barrier_wait(b)        arkrtos_posix_barrier_wait(b)

/* 屏障属性：本实现只有"进程内屏障"一种，setpshared(SHARED) 返回 ENOTSUP */
typedef struct
{
    int32 pshared;
} arkrtos_pthread_barrierattr_t;

typedef arkrtos_pthread_barrierattr_t pthread_barrierattr_t;

int arkrtos_posix_barrierattr_init(arkrtos_pthread_barrierattr_t *a);
int arkrtos_posix_barrierattr_destroy(arkrtos_pthread_barrierattr_t *a);
int arkrtos_posix_barrierattr_setpshared(arkrtos_pthread_barrierattr_t *a, int32 v);
int arkrtos_posix_barrierattr_getpshared(const arkrtos_pthread_barrierattr_t *a, int32 *v);

#define pthread_barrierattr_init(a)           arkrtos_posix_barrierattr_init(a)
#define pthread_barrierattr_destroy(a)        arkrtos_posix_barrierattr_destroy(a)
#define pthread_barrierattr_setpshared(a, v)  arkrtos_posix_barrierattr_setpshared((a), (v))
#define pthread_barrierattr_getpshared(a, v)  arkrtos_posix_barrierattr_getpshared((a), (v))

#endif /* __ARKRTOS_PTHREAD_H__ */
