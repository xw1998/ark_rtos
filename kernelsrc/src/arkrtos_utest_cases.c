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
#include "arkrtos_sbuf.h"
#include "arkrtos_audit.h"
#include "arkrtos_waitmulti.h"
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"

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
 * 流缓冲 / 消息缓冲
 * 前两条只走非阻塞形态：用例在 shell 任务里跑，绝不能把控制台挂住。
 * 第三条（sbuf_block_wake_contract）例外，它故意睡一小段，见那里的说明。
 * ============================================================ */
#if (ARKRTOS_USE_SBUF == 1)

UTEST_CASE(sbuf_stream_contract)
{
    int32 h;
    uint8 out[16];
    int32 i;

    h = arkrtos_sbuf_create_internal("utsb");
    UTEST_REQUIRE(h >= 0);
    UTEST_CHECK_EQ(arkrtos_sbuf_used_internal(h), 0);
    UTEST_CHECK_EQ(arkrtos_sbuf_free_internal(h), (int32)ARKRTOS_SBUF_BUF_SIZE);

    /* 部分成功：流语义下"写进去多少算多少" */
    UTEST_CHECK_EQ(arkrtos_sbuf_send_internal(h, "abcdef", 6, 0), 6);
    UTEST_CHECK_EQ(arkrtos_sbuf_recv_internal(h, out, 2, 0), 2);
    UTEST_CHECK_EQ(out[0], 'a');
    UTEST_CHECK_EQ(out[1], 'b');
    UTEST_CHECK_EQ(arkrtos_sbuf_recv_internal(h, out, sizeof(out), 0), 4);
    UTEST_CHECK_EQ(out[3], 'f');

    /* 空读不等待：必须报超时而不是返回 0 假装读到了 */
    UTEST_CHECK_EQ(arkrtos_sbuf_recv_internal(h, out, 4, 0), ARKRTOS_SBUF_ERR_TIMEOUT);

    /* 写满 + 回绕 + 顺序不变 */
    for(i = 0; i < (int32)sizeof(out); i++)
    {
        out[i] = (uint8)i;
    }
    UTEST_CHECK_EQ(arkrtos_sbuf_send_internal(h, out, (int32)ARKRTOS_SBUF_BUF_SIZE - 2, 0),
                   (int32)ARKRTOS_SBUF_BUF_SIZE - 2);
    UTEST_CHECK_EQ(arkrtos_sbuf_send_internal(h, out, 8, 0), 2);   /* 只剩 2 字节空间 */
    UTEST_CHECK_EQ(arkrtos_sbuf_free_internal(h), 0);
    UTEST_CHECK_EQ(arkrtos_sbuf_send_internal(h, out, 1, 0), ARKRTOS_SBUF_ERR_TIMEOUT);

    UTEST_CHECK_EQ(arkrtos_sbuf_reset_internal(h), ARKRTOS_SBUF_OK);
    UTEST_CHECK_EQ(arkrtos_sbuf_used_internal(h), 0);

    UTEST_CHECK_EQ(arkrtos_sbuf_delete_internal(h), ARKRTOS_SBUF_OK);
    UTEST_CHECK_EQ(arkrtos_sbuf_is_alive(h), 0);
    UTEST_CHECK_EQ(arkrtos_sbuf_send_internal(h, out, 1, 0), ARKRTOS_SBUF_ERR_PARAM);
}

UTEST_CASE(mbuf_framing_contract)
{
    int32 h;
    uint8 out[16];

    h = arkrtos_sbuf_create_internal("utmb");
    UTEST_REQUIRE(h >= 0);

    UTEST_CHECK_EQ(arkrtos_mbuf_send_internal(h, "hello", 5, 0), ARKRTOS_SBUF_OK);
    UTEST_CHECK_EQ(arkrtos_mbuf_peek_len_internal(h), 5);
    UTEST_CHECK_EQ(arkrtos_mbuf_recv_internal(h, out, sizeof(out), 0), 5);
    UTEST_CHECK_EQ(out[4], 'o');

    /* 接收缓冲放不下：整条丢弃，但下一条不能被串味 */
    UTEST_CHECK_EQ(arkrtos_mbuf_send_internal(h, "0123456789", 10, 0), ARKRTOS_SBUF_OK);
    UTEST_CHECK_EQ(arkrtos_mbuf_send_internal(h, "next", 4, 0), ARKRTOS_SBUF_OK);
    UTEST_CHECK_EQ(arkrtos_mbuf_recv_internal(h, out, 4, 0), ARKRTOS_SBUF_ERR_TOO_SMALL);
    UTEST_CHECK_EQ(arkrtos_mbuf_recv_internal(h, out, sizeof(out), 0), 4);
    UTEST_CHECK_EQ(out[0], 'n');

    /* 比整块缓冲还大：立刻报 NOSPC，不能无限重试 */
    UTEST_CHECK_EQ(arkrtos_mbuf_send_internal(h, out, (int32)ARKRTOS_SBUF_BUF_SIZE, 0),
                   ARKRTOS_SBUF_ERR_NOSPC);

    UTEST_CHECK_EQ(arkrtos_sbuf_delete_internal(h), ARKRTOS_SBUF_OK);
}

/* ------------------------------------------------------------
 * 阻塞 / 唤醒（这一条故意让出 CPU）
 * 上面两条用例只走非阻塞形态；「挂等待表 -> 真让出 -> 被唤醒接管 /
 * 超时重睡」这三段不真睡就看不见。所以这条用例会让出控制台任务两次
 * （约 20 ms 与约 30 ms），并且**自带上限**：即使内核的挂表或唤醒坏了，
 * 它也会在 400 ms 超时后返回并报红，不会把控制台挂死。
 *
 * 送数据的是软件定时器的回调（定时器任务上下文，**不是真 ISR**），所以
 * 这里证明的是「跨任务唤醒 + 超时重睡」；ISR 唤醒仍只有代码路径证据，
 * 要一条真中断才能作证。
 * ------------------------------------------------------------ */
#if (ARKRTOS_USE_TIMER == 1)

static int32 s_ut_blk_sb = -1;

static void ut_sbuf_blk_send_cb(void *p_arg)
{
    (void)p_arg;
    (void)arkrtos_sbuf_send_from_isr_internal(s_ut_blk_sb, (const void *)"wake", 4);
}

UTEST_CASE(sbuf_block_wake_contract)
{
    int32  h;
    int32  t;
    uint8  out[8];
    uint32 t0;
    uint32 dt;

    h = arkrtos_sbuf_create_internal("utbk");
    UTEST_CHECK(h >= 0);
    if(h < 0)
    {
        return;
    }
    s_ut_blk_sb = h;

    t = arkrtos_timer_create_internal("utbkt");
    UTEST_CHECK(t >= 0);
    if(t < 0)
    {
        (void)arkrtos_sbuf_delete_internal(h);
        s_ut_blk_sb = -1;
        return;
    }

    /* 空缓冲 + 20 ms 后由定时器任务塞 4 字节：读的人必须被唤醒，而不是睡到超时 */
    UTEST_CHECK_EQ(arkrtos_timer_start_internal(t, 20u, ARKRTOS_TIMER_MODE_ONESHOT,
                                               ut_sbuf_blk_send_cb, 0), 0);
    t0 = arkrtos_kernel_get_tick();
    UTEST_CHECK_EQ(arkrtos_sbuf_recv_internal(h, (void *)out, sizeof(out), 400), 4);
    dt = arkrtos_kernel_get_tick() - t0;
    UTEST_CHECK_EQ(out[0], 'w');
    UTEST_CHECK_EQ(out[3], 'e');
    /* 板上实测 40 拍（20.0 ms）：定时器 20 ms 到点即被唤醒，不是睡满超时 */
    UTEST_CHECK_RANGE(ARKRTOS_MS_TO_TICK(15u), dt, ARKRTOS_MS_TO_TICK(100u));

    /* 没人送数据：睡满超时后必须报 TIMEOUT，既不能立刻返回也不能永不返回 */
    t0 = arkrtos_kernel_get_tick();
    UTEST_CHECK_EQ(arkrtos_sbuf_recv_internal(h, (void *)out, sizeof(out), 30),
                   ARKRTOS_SBUF_ERR_TIMEOUT);
    dt = arkrtos_kernel_get_tick() - t0;
    /* 板上实测 60 拍（30.0 ms）：睡满声明的超时后返回，不早不晚 */
    UTEST_CHECK_RANGE(ARKRTOS_MS_TO_TICK(20u), dt, ARKRTOS_MS_TO_TICK(100u));

    UTEST_CHECK_EQ(arkrtos_timer_delete_internal(t), 0);
    UTEST_CHECK_EQ(arkrtos_sbuf_delete_internal(h), ARKRTOS_SBUF_OK);
    s_ut_blk_sb = -1;
}

#endif /* ARKRTOS_USE_TIMER */

#endif /* ARKRTOS_USE_SBUF */

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

/* ============================================================
 * 递归互斥锁（ARKRTOS_USE_MTX_RECURSIVE）
 * 契约：可重入是**对象属性**不是全局语义——只有递归入口创建的锁能重入，
 *       同一张表里的普通锁重入仍必须被拒；重入与解锁必须成对。
 * ============================================================ */
#if (ARKRTOS_USE_MTX_RECURSIVE == 1)

UTEST_CASE(mtx_recursive_contract)
{
    int32 r = arkrtos_mtx_create_recursive_internal("utrec");
    int32 p = arkrtos_mtx_create_internal("utplain");

    UTEST_CHECK(r >= 0);
    UTEST_CHECK(p >= 0);
    if((r < 0) || (p < 0))
    {
        return;
    }

    /* 递归锁：同任务连加三次都成功（普通锁第二次就会 -1） */
    UTEST_CHECK_EQ(arkrtos_mtx_lock_internal(r, 0), 0);
    UTEST_CHECK_EQ(arkrtos_mtx_lock_internal(r, 0), 0);
    UTEST_CHECK_EQ(arkrtos_mtx_lock_internal(r, 0), 0);

    /* 解锁必须逐级配对：前两次只减 nest，第三次才真正释放 */
    UTEST_CHECK_EQ(arkrtos_mtx_unlock_internal(r), 0);
    UTEST_CHECK_EQ(arkrtos_mtx_unlock_internal(r), 0);
    UTEST_CHECK_EQ(arkrtos_mtx_unlock_internal(r), 0);

    /* 释放后能重新拿到（nest 已归零，不是"永远锁死"） */
    UTEST_CHECK_EQ(arkrtos_mtx_lock_internal(r, 0), 0);
    UTEST_CHECK_EQ(arkrtos_mtx_unlock_internal(r), 0);

    /* 同一张表里的普通锁：重入仍被拒，语义没有被递归入口带跑 */
    UTEST_CHECK_EQ(arkrtos_mtx_lock_internal(p, 0), 0);
    UTEST_CHECK(arkrtos_mtx_lock_internal(p, 0) < 0);
    UTEST_CHECK_EQ(arkrtos_mtx_unlock_internal(p), 0);

    UTEST_CHECK_EQ(arkrtos_mtx_delete_internal(r), 0);
    UTEST_CHECK_EQ(arkrtos_mtx_delete_internal(p), 0);
}

#endif /* ARKRTOS_USE_MTX_RECURSIVE */

/* ============================================================
 * 多对象同时就绪探测（ARKRTOS_USE_WAITMULTI）
 * 契约：①一条句柄一个位；②ANY/ALL 判定正确；③**不消费令牌**——
 *       探测说"有令牌"，随后的 timeout==0 取用必须真的取到；
 *       ④任一句柄非法整组拒绝，不给一半答案。
 * ============================================================ */
#if (ARKRTOS_USE_WAITMULTI == 1)

UTEST_CASE(waitmulti_probe_contract)
{
    uint32 h[3];
    uint32 mask = 0u;
    int32  s = arkrtos_sem_create_internal("utwms", 0);
    int32  m = arkrtos_mtx_create_internal("utwmm");
    int32  e = arkrtos_event_create_internal("utwme");

    UTEST_CHECK(s >= 0);
    UTEST_CHECK(m >= 0);
    UTEST_CHECK(e >= 0);
    if((s < 0) || (m < 0) || (e < 0))
    {
        return;
    }

    /* 顺序固定：0=sem（空）、1=event（未置位）、2=mtx（空闲）。
     * 前两个只用来验"真的没就绪"，mtx 单独用来验"空闲锁算就绪"。 */
    h[0] = (uint32)s;
    h[1] = (uint32)e;
    h[2] = (uint32)m;

    /* 空信号量 + 未置位事件：ANY 与 ALL 都不满足，掩码为 0 */
    UTEST_CHECK_EQ(arkrtos_wm_probe_internal(h, 2u, ARKRTOS_WM_OPT_ANY, &mask), -2);
    UTEST_CHECK_EQ(mask, 0u);

    /* 把空闲互斥锁加进组：ANY 立刻满足（"可以加锁"就是就绪），ALL 仍差 sem/event */
    UTEST_CHECK_EQ(arkrtos_wm_probe_internal(h, 3u, ARKRTOS_WM_OPT_ANY, &mask), 0);
    UTEST_CHECK_EQ(mask, 4u);
    UTEST_CHECK_EQ(arkrtos_wm_probe_internal(h, 3u, ARKRTOS_WM_OPT_ALL, &mask), -2);

    /* 投一个令牌：位 0 亮，ALL 仍差事件 */
    UTEST_CHECK_EQ(arkrtos_sem_post_internal(s), 0);
    UTEST_CHECK_EQ(arkrtos_wm_probe_internal(h, 3u, ARKRTOS_WM_OPT_ANY, &mask), 0);
    UTEST_CHECK_EQ(mask, 5u);
    UTEST_CHECK_EQ(arkrtos_wm_probe_internal(h, 3u, ARKRTOS_WM_OPT_ALL, &mask), -2);
    UTEST_CHECK_EQ(mask & 2u, 0u);

    arkrtos_event_set_internal(e);
    UTEST_CHECK_EQ(arkrtos_wm_probe_internal(h, 3u, ARKRTOS_WM_OPT_ALL, &mask), 0);
    UTEST_CHECK_EQ(mask, 7u);

    /* 不消费令牌：探测了这么多次，令牌还在，取用仍成功 */
    UTEST_CHECK_EQ(arkrtos_sem_wait_internal(s, 0), 0);

    /* 非法句柄：整组拒绝，掩码清零（不给一半答案） */
    mask = 0xFFFFFFFFu;
    h[1] = 0x7FFFFFFFu;
    UTEST_CHECK_EQ(arkrtos_wm_probe_internal(h, 3u, ARKRTOS_WM_OPT_ANY, &mask), -1);
    UTEST_CHECK_EQ(mask, 0u);
    h[1] = (uint32)m;

    /* 参数边界：n==0 / 超上限 / 未知 opt 一律 -1 */
    UTEST_CHECK_EQ(arkrtos_wm_probe_internal(h, 0u, ARKRTOS_WM_OPT_ANY, &mask), -1);
    UTEST_CHECK_EQ(arkrtos_wm_probe_internal(h, 3u, 0u, &mask), -1);

    UTEST_CHECK_EQ(arkrtos_sem_delete_internal(s), 0);
    UTEST_CHECK_EQ(arkrtos_mtx_delete_internal(m), 0);
}

#endif /* ARKRTOS_USE_WAITMULTI */

/* ============================================================
 * 等待者容量（ARKRTOS_USE_WAIT_UNBOUNDED）
 * 契约：打开后等待槽数必须等于任务表容量（一个任务同时只能等一个对象），
 *       并且至少不小于原来的固定值——放大不许变成缩小。
 * ============================================================ */
#if (ARKRTOS_USE_WAIT_UNBOUNDED == 1)

UTEST_CASE(waitq_capacity_contract)
{
    UTEST_CHECK_EQ((long)ARKRTOS_WQ_SLOTS, (long)ARKRTOS_TASK_MAX_NUM);
    UTEST_CHECK((long)ARKRTOS_WQ_SLOTS >= (long)ARKRTOS_MAX_SYNC_WAITERS);
}

#endif /* ARKRTOS_USE_WAIT_UNBOUNDED */

/* ============================================================
 * 多级优先级继承（链式传播，真任务版）
 * 契约：A 等 L1（B 持有），B 又在等 L2（C 持有）⇒ 优先级必须沿链传到 C，
 *       只提一层的话 C 停在原地，A 仍在等一个被 B 挡住的锁。
 *
 * 板上怎么造这条链：控制台任务自己当链底 C——它先持有 L1；再注册两个优先级
 * 更高的任务，MID 拿住 L2 后去等 L1（控制台的优先级这时被提到 MID 的档），
 * HIGH 去等 L2（MID 被提到 HIGH 的档，并且这条提升必须沿链继续传到控制台）。
 * 所以「控制台的有效优先级 == HIGH 的档」就是「链传了两跳」的直接读数：
 * 只提一层的实现会停在 MID 的档。
 *
 * 这是本文件里唯一会睡一下的用例（每次几毫秒，加两条有界等待）：不睡的话
 * 更高优先级的任务根本没机会跑起来把链搭起来。所有等待都带超时，坏掉时是
 * 几百毫秒内报红，不会把控制台挂死。
 * ============================================================ */
#define PI_CHAIN_GAP         4u     /* 相邻两档之间留的优先级差 */
#define PI_CHAIN_STACK_WORDS 128u   /* 512 B：这些任务只调几个内核函数 */

static uint32 s_pi_chain_stack_mid[PI_CHAIN_STACK_WORDS];
static uint32 s_pi_chain_stack_high[PI_CHAIN_STACK_WORDS];

static int32 s_pi_chain_l1   = -1;
static int32 s_pi_chain_l2   = -1;
static int32 s_pi_chain_done = -1;

/* 链中间那一环：先拿住 L2，再去等控制台手里的 L1，放完两把锁后退场。 */
static void pi_chain_mid_entry(void)
{
    if(arkrtos_mtx_lock_internal(s_pi_chain_l2, 0) == 0)
    {
        if(arkrtos_mtx_lock_internal(s_pi_chain_l1, 300) == 0)
        {
            /* 先放 L1：HIGH 正是在这把锁上等的，它要先醒过来 */
            (void)arkrtos_mtx_unlock_internal(s_pi_chain_l1);
        }
        (void)arkrtos_mtx_unlock_internal(s_pi_chain_l2);
    }
    (void)arkrtos_sem_post_internal(s_pi_chain_done);
    arkrtos_task_kill_internal();
}

/* 链顶：等 L2，拿到即放，然后退场。 */
static void pi_chain_high_entry(void)
{
    if(arkrtos_mtx_lock_internal(s_pi_chain_l2, 300) == 0)
    {
        (void)arkrtos_mtx_unlock_internal(s_pi_chain_l2);
    }
    (void)arkrtos_sem_post_internal(s_pi_chain_done);
    arkrtos_task_kill_internal();
}

UTEST_CASE(mtx_pi_chain_contract)
{
    arkrtos_task_t *p_self = arkrtos_task_get_current();
    uint8 base;
    uint8 mid_prio;
    uint8 high_prio;
    int32 mid_id;
    int32 high_id;

    UTEST_REQUIRE(p_self != 0);

    /* 链要三档：控制台、MID、HIGH。优先级空间不够就明说跳过，不假通过。 */
    base = p_self->base_priority;
    if((uint32)base < (2u * PI_CHAIN_GAP + 1u))
    {
        UTEST_SKIP("console priority too low to build a 3-level chain");
    }
    mid_prio  = (uint8)((uint32)base - PI_CHAIN_GAP);
    high_prio = (uint8)((uint32)base - (2u * PI_CHAIN_GAP));

    s_pi_chain_l1   = arkrtos_mtx_create_internal("utpi1");
    s_pi_chain_l2   = arkrtos_mtx_create_internal("utpi2");
    s_pi_chain_done = arkrtos_sem_create_internal("utpid", 0);
    UTEST_REQUIRE((s_pi_chain_l1 >= 0) && (s_pi_chain_l2 >= 0) &&
                  (s_pi_chain_done >= 0));

    /* 控制台当链底：先拿住 L1；此刻它的有效优先级就是基准值 */
    UTEST_REQUIRE(arkrtos_mtx_lock_internal(s_pi_chain_l1, 0) == 0);
    UTEST_CHECK_EQ(p_self->priority, base);

    /* ① 注册 MID：它一上来自带抢占，拿住 L2，然后卡在 L1 上 */
    mid_id = arkrtos_task_register(pi_chain_mid_entry, s_pi_chain_stack_mid,
                                   (uint32)sizeof(s_pi_chain_stack_mid),
                                   mid_prio, 0u);
    UTEST_REQUIRE(mid_id > 0);
    arkrtos_task_wait_internal(5u);

    /* 一跳：控制台被提到 MID 的档，基准值一个字不动 */
    UTEST_CHECK_EQ(p_self->priority, mid_prio);
    UTEST_CHECK_EQ(p_self->base_priority, base);

    /* ② 注册 HIGH：它等 L2 ⇒ MID 继承 HIGH 的档，而且必须继续沿链传到控制台 */
    high_id = arkrtos_task_register(pi_chain_high_entry, s_pi_chain_stack_high,
                                    (uint32)sizeof(s_pi_chain_stack_high),
                                    high_prio, 0u);
    UTEST_REQUIRE(high_id > 0);
    arkrtos_task_wait_internal(5u);

    UTEST_CHECK_EQ(arkrtos_task_table[mid_id - 1].priority, high_prio);
    UTEST_CHECK_EQ(arkrtos_task_table[mid_id - 1].base_priority, mid_prio);
    UTEST_CHECK_EQ(p_self->priority, high_prio);    /* ← 两跳，不是一跳 */
    UTEST_CHECK_EQ(p_self->base_priority, base);

    /* ③ 放掉 L1：MID 拿到它再依次放出，HIGH 顺着 L2 醒过来，两个任务都退场 */
    UTEST_CHECK_EQ(arkrtos_mtx_unlock_internal(s_pi_chain_l1), 0);
    UTEST_CHECK_EQ(arkrtos_sem_wait_internal(s_pi_chain_done, 500), 0);
    UTEST_CHECK_EQ(arkrtos_sem_wait_internal(s_pi_chain_done, 500), 0);

    /* ④ 撤销：没有谁滞留在被提升的优先级上，锁与信号量都能干净删掉 */
    UTEST_CHECK_EQ(p_self->priority, base);
    UTEST_CHECK_EQ(p_self->base_priority, base);
    UTEST_CHECK_EQ(arkrtos_mtx_delete_internal(s_pi_chain_l1), 0);
    UTEST_CHECK_EQ(arkrtos_mtx_delete_internal(s_pi_chain_l2), 0);
    UTEST_CHECK_EQ(arkrtos_sem_delete_internal(s_pi_chain_done), 0);

    s_pi_chain_l1   = -1;
    s_pi_chain_l2   = -1;
    s_pi_chain_done = -1;
}

#endif /* ARKRTOS_USE_UTEST */
