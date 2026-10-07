/**
 * @file arkrtos_utest_cases.c
 * @brief 内核自带的板端单元测试用例集（utest）
 * @details 这些是「用例」，不是「自检」：模块自己的 shell 自检（mpool test /
 *          audit）证明它此刻是对的；用例则把**契约**写死——把实现改坏了就红，
 *          而且失败会精确到哪一条断言、在哪个文件哪一行。
 *
 *          设计取舍：
 *          - 用例在 shell 任务上下文里跑，**绝不能阻塞**：所有可能睡的调用一律
 *            用 timeout==0 的非阻塞形态，避免把控制台任务挂住。
 *          - 每条用例都自己收拾现场（池 / 信号量 / 互斥量 / 定时器用完即删），
 *            这样 `utest run` 可以反复跑而不会把内核的对象表吃满。
 *          - event 没有 delete API（只有 create / wait / set），所以它的句柄在
 *            文件作用域里缓存，只创建一次，反复用。
 *
 *          门控：ARKRTOS_USE_UTEST（arkrtos_features.h，默认 1）；关掉后本文件
 *          整体不参与编译。各用例再按模块特性开关（MEMPOOL / TIMER / AUDIT）
 *          分别裁剪，缺功能的构建不会因为缺少符号而链接失败。
 *
 * @note 本模块属于内核特权态代码。
 */
#include "arkrtos_utest.h"

#if (ARKRTOS_USE_UTEST == 1)

#include "arkrtos_def.h"
#include "arkrtos_config.h"

#include "arkrtos_mempool.h"
#include "arkrtos_sync.h"
#include "arkrtos_fifo.h"
#include "arkrtos_event.h"
#include "arkrtos_timer.h"
#include "arkrtos_audit.h"

/* ============================================================
 * 固定块内存池
 * ============================================================ */
#if (ARKRTOS_USE_MEMPOOL == 1)

#define MP_POOL_BLOCK_SIZE  32u
#define MP_POOL_BLOCKS      4u
#define MP_POOL_BUF_BYTES   (MP_POOL_BLOCK_SIZE * (MP_POOL_BLOCKS + 1u))

static uint32 s_mp_buf[MP_POOL_BUF_BYTES / 4u];

UTEST_CASE(mempool_contract)
{
    int32  h;
    void  *blk[MP_POOL_BLOCKS + 1u];
    void  *first;
    arkrtos_mempool_stat_t st;
    uint32 i;

    h = arkrtos_mempool_create_internal("utmp", s_mp_buf,
                                        MP_POOL_BLOCK_SIZE, MP_POOL_BLOCKS);
    if(h < 0)
    {
        UTEST_FAIL("mempool create rejected a legal pool");
        return;
    }

    /* 名字与参数的守卫：重名要拒，块小于指针宽度的要拒 */
    UTEST_CHECK(arkrtos_mempool_create_internal("utmp", s_mp_buf,
                 MP_POOL_BLOCK_SIZE, MP_POOL_BLOCKS) < 0);
    UTEST_CHECK(arkrtos_mempool_create_internal("utbad", s_mp_buf, 1u, 4u) < 0);
    UTEST_CHECK(arkrtos_mempool_create_internal("utbad", s_mp_buf,
                 MP_POOL_BLOCK_SIZE, 0u) < 0);

    /* 取空整池，再取一次必须给 0（不阻塞、不越界、不重复发同一块） */
    for(i = 0u; i < MP_POOL_BLOCKS; i++)
    {
        blk[i] = arkrtos_mempool_alloc_internal(h);
    }
    blk[MP_POOL_BLOCKS] = arkrtos_mempool_alloc_internal(h);

    for(i = 0u; i < MP_POOL_BLOCKS; i++)
    {
        UTEST_CHECK(blk[i] != 0);
    }
    UTEST_CHECK_EQ((long)blk[MP_POOL_BLOCKS], 0);

    /* 快照必须如实反映「0 空闲、失败 1 次、峰值占满」 */
    UTEST_CHECK_EQ(arkrtos_mempool_query_internal(h, &st), 0);
    UTEST_CHECK_EQ((long)st.block_size,  (long)MP_POOL_BLOCK_SIZE);
    UTEST_CHECK_EQ((long)st.block_count, (long)MP_POOL_BLOCKS);
    UTEST_CHECK_EQ((long)st.free_count,  0);
    UTEST_CHECK_EQ((long)st.alloc_fail,  1);
    UTEST_CHECK_EQ((long)st.peak_used,   (long)MP_POOL_BLOCKS);

    /* 归还一块后，再取应能拿回内存（同地址是 LIFO 空闲链的契约） */
    first = blk[0];
    UTEST_CHECK_EQ(arkrtos_mempool_free_internal(h, blk[0]), 0);
    blk[0] = arkrtos_mempool_alloc_internal(h);
    UTEST_CHECK(blk[0] == first);

    /* 越界 / 未对齐的释放必须被拒 */
    UTEST_CHECK(arkrtos_mempool_free_internal(h,
                 (void *)((unsigned char *)s_mp_buf + MP_POOL_BUF_BYTES)) != 0);
    UTEST_CHECK(arkrtos_mempool_free_internal(h,
                 (void *)((unsigned char *)s_mp_buf + 1u)) != 0);

    /* 清账 -> 删除 -> 句柄判死 */
    for(i = 0u; i < MP_POOL_BLOCKS; i++)
    {
        if(blk[i] != 0)
        {
            (void)arkrtos_mempool_free_internal(h, blk[i]);
            blk[i] = 0;
        }
    }
    UTEST_CHECK_EQ(arkrtos_mempool_delete_internal(h), 0);
    UTEST_CHECK_EQ(arkrtos_mempool_is_alive(h), 0);
}

#endif /* ARKRTOS_USE_MEMPOOL */

/* ============================================================
 * 同步对象：信号量 / 互斥量
 * ============================================================ */
UTEST_CASE(sem_token_flow)
{
    int32 h = arkrtos_sem_create_internal("utsem", 2);

    UTEST_CHECK(h >= 0);
    if(h < 0)
    {
        return;
    }

    UTEST_CHECK_EQ(arkrtos_sem_wait_internal(h, 0), 0);                 /* 令牌 1 */
    UTEST_CHECK_EQ(arkrtos_sem_wait_internal(h, 0), 0);                 /* 令牌 2 */
    /* 空时 timeout==0 只试一次：报超时，**不能**谎报成功 */
    UTEST_CHECK_EQ(arkrtos_sem_wait_internal(h, 0), ARKRTOS_SYNC_ERR_TIMEOUT);
    UTEST_CHECK_EQ(arkrtos_sem_post_internal(h), 0);
    UTEST_CHECK_EQ(arkrtos_sem_wait_internal(h, 0), 0);                 /* post 后又能拿 */
    UTEST_CHECK_EQ(arkrtos_sem_delete_internal(h), 0);
}

UTEST_CASE(sem_reject_bad_handle)
{
    /* 未加锁对象 / 编号越界：一律拒绝，且不是「就当成功」 */
    UTEST_CHECK(arkrtos_sem_wait_internal(-1, 0) < 0);
    UTEST_CHECK(arkrtos_sem_post_internal(-1) < 0);
    UTEST_CHECK(arkrtos_sem_delete_internal(-1) < 0);
}

UTEST_CASE(mtx_lock_unlock)
{
    int32 h = arkrtos_mtx_create_internal("utmtx");

    UTEST_CHECK(h >= 0);
    if(h < 0)
    {
        return;
    }

    UTEST_CHECK_EQ(arkrtos_mtx_lock_internal(h, 0), 0);   /* 空闲锁，立即拿到 */
    UTEST_CHECK_EQ(arkrtos_mtx_unlock_internal(h), 0);
    UTEST_CHECK_EQ(arkrtos_mtx_delete_internal(h), 0);

    UTEST_CHECK(arkrtos_mtx_lock_internal(-1, 0) < 0);
    UTEST_CHECK(arkrtos_mtx_unlock_internal(-1) < 0);
}

/* ============================================================
 * 环形 FIFO
 * ============================================================ */
#define FIFO_BUF_BYTES   16u
#define FIFO_USABLE      (FIFO_BUF_BYTES - 8u - 1u)   /* 头部 8 字节 + 环形留一格 */

static uint8 s_fifo_buf[FIFO_BUF_BYTES];
static uint8 s_fifo_bad_buf[FIFO_BUF_BYTES];

UTEST_CASE(fifo_capacity_and_order)
{
    arkrtos_fifo_t *f = arkrtos_fifo_create(s_fifo_buf, (int32)FIFO_BUF_BYTES);
    uint8 in[FIFO_BUF_BYTES];
    uint8 out[FIFO_BUF_BYTES];
    int32 n;
    int32 i;

    UTEST_CHECK(f != 0);
    if(f == 0)
    {
        return;
    }

    /* 尺寸非法（<=8）必须给 0，而不是返回一个用不了的句柄 */
    UTEST_CHECK(arkrtos_fifo_create(s_fifo_bad_buf, 8) == 0);

    for(i = 0; i < (int32)FIFO_BUF_BYTES; i++)
    {
        in[i] = (uint8)(0x40 + i);
    }

    /* 满时短写：写出 FIFO_USABLE 字节，其余被拒（且计入 refused） */
    n = arkrtos_fifo_write(f, in, 32);
    UTEST_CHECK_EQ(n, (long)FIFO_USABLE);
    UTEST_CHECK(arkrtos_fifo_write_refused() > 0u);

    n = arkrtos_fifo_read(f, out, 32);
    UTEST_CHECK_EQ(n, (long)FIFO_USABLE);
    UTEST_CHECK(out[0] == in[0]);
    UTEST_CHECK(out[FIFO_USABLE - 1u] == in[FIFO_USABLE - 1u]);

    /* 读空后再读必须给 0（空不是「有数据」） */
    UTEST_CHECK_EQ(arkrtos_fifo_read(f, out, sizeof(out)), 0);

    /* 环绕：写 3 字节 -> 读掉 -> 再写满，验证读写下标回绕后依然保持顺序 */
    UTEST_CHECK_EQ(arkrtos_fifo_write(f, in, 3), 3);
    UTEST_CHECK_EQ(arkrtos_fifo_read(f, out, 3), 3);
    UTEST_CHECK(out[0] == in[0]);
    UTEST_CHECK(out[2] == in[2]);

    n = arkrtos_fifo_write(f, in, 32);
    UTEST_CHECK_EQ(n, (long)FIFO_USABLE);
    n = arkrtos_fifo_read(f, out, 32);
    UTEST_CHECK_EQ(n, (long)FIFO_USABLE);
    UTEST_CHECK(out[0] == in[0]);
    UTEST_CHECK(out[FIFO_USABLE - 1u] == in[FIFO_USABLE - 1u]);
}

UTEST_CASE(fifo_bad_magic_is_counted)
{
    arkrtos_fifo_t *f = arkrtos_fifo_create(s_fifo_bad_buf, (int32)FIFO_BUF_BYTES);
    uint8 b[4];
    uint32 m0;
    uint32 by0;

    UTEST_CHECK(f != 0);
    if(f == 0)
    {
        return;
    }

    b[0] = 1; b[1] = 2; b[2] = 3; b[3] = 4;

    /* 模拟内存损坏：破坏头部 magic。此后该 FIFO 的每一次读写都必须被
     * **读出来**（计数器），而不是静默丢字节——这正是控制台"永久失聪"的根源。 */
    f->magic = 0x0000u;

    m0  = arkrtos_fifo_bad_magic();
    by0 = arkrtos_fifo_bad_magic_bytes();

    UTEST_CHECK_EQ(arkrtos_fifo_write(f, b, 4), 0);
    UTEST_CHECK(arkrtos_fifo_bad_magic() == (m0 + 1u));
    UTEST_CHECK(arkrtos_fifo_bad_magic_bytes() == (by0 + 4u));

    UTEST_CHECK_EQ(arkrtos_fifo_read(f, b, 4), 0);
    UTEST_CHECK(arkrtos_fifo_bad_magic() == (m0 + 2u));
}

/* ============================================================
 * 事件
 * ============================================================ */
static int32 s_ut_event = -1;   /* event 无 delete：只创建一次，反复使用 */

UTEST_CASE(event_set_wait_flag)
{
    if(s_ut_event < 0)
    {
        s_ut_event = arkrtos_event_create_internal("utevt");
    }
    UTEST_CHECK(s_ut_event >= 0);
    if(s_ut_event < 0)
    {
        return;
    }

    /* 未置位时 wait(0) 不阻塞，报超时 */
    UTEST_CHECK_EQ(arkrtos_event_wait_internal(s_ut_event, 0),
                   ARKRTOS_SYNC_ERR_TIMEOUT);

    /* set 之后下一次 wait(0) 必须立刻成功；置位被取走后再次 wait 又超时 */
    arkrtos_event_set_internal(s_ut_event);
    UTEST_CHECK_EQ(arkrtos_event_wait_internal(s_ut_event, 0), 0);
    UTEST_CHECK_EQ(arkrtos_event_wait_internal(s_ut_event, 0),
                   ARKRTOS_SYNC_ERR_TIMEOUT);
}

/* ============================================================
 * 软件定时器
 * ============================================================ */
#if (ARKRTOS_USE_TIMER == 1)

static void ut_timer_cb(void *arg)
{
    (void)arg;
}

UTEST_CASE(timer_lifecycle)
{
    int32 h = arkrtos_timer_create_internal("uttmr");

    UTEST_CHECK(h >= 0);
    if(h < 0)
    {
        return;
    }

    /* 非法参数一律拒绝：空回调 / 0 周期 / 非法模式 */
    UTEST_CHECK(arkrtos_timer_start_internal(h, 10u, ARKRTOS_TIMER_MODE_ONESHOT,
                 0, 0) < 0);
    UTEST_CHECK(arkrtos_timer_start_internal(h, 0u, ARKRTOS_TIMER_MODE_ONESHOT,
                 ut_timer_cb, 0) < 0);
    UTEST_CHECK(arkrtos_timer_start_internal(h, 10u, 0x5Au, ut_timer_cb, 0) < 0);

    /* 合法启动 / 停止 / 删除；删除后同一句柄必须失效 */
    UTEST_CHECK_EQ(arkrtos_timer_start_internal(h, 10u,
                   ARKRTOS_TIMER_MODE_PERIODIC, ut_timer_cb, 0), 0);
    UTEST_CHECK_EQ(arkrtos_timer_stop_internal(h), 0);
    UTEST_CHECK_EQ(arkrtos_timer_delete_internal(h), 0);
    UTEST_CHECK(arkrtos_timer_start_internal(h, 10u, ARKRTOS_TIMER_MODE_ONESHOT,
                 ut_timer_cb, 0) < 0);
}

#endif /* ARKRTOS_USE_TIMER */

/* ============================================================
 * 内核结构自审：静止态下必须自洽
 * ============================================================ */
#if (ARKRTOS_USE_AUDIT == 1)

UTEST_CASE(audit_structures_consistent)
{
    arkrtos_audit_result_t r;

    arkrtos_audit_run(&r);

    UTEST_CHECK_EQ((long)r.mask, 0);         /* 分区 / 槽 / 任务 / 调度位图自洽 */
    UTEST_CHECK(r.checks > 0u);              /* 确实检查了东西，而不是空转 */
    UTEST_CHECK_EQ((long)arkrtos_audit_last_mask(), (long)r.mask);
}

#endif /* ARKRTOS_USE_AUDIT */

#endif /* ARKRTOS_USE_UTEST */
