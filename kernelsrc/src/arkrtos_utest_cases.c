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
#include "arkrtos_features.h"
#include "arkrtos_hal.h"      /* ARKRTOS_ENTER_CRITICAL：自审要在静止态下跑 */

/* SEC-G08：upcall 返回通道（SVC 0x23）的三处断言都在 app_sdk 之外的实体里，
 * 头文件自己按「PRIV && PAGING && PAGING_PAGES && ELF」派生 ARKRTOS_USE_USHELL_ISO，
 * 关档时只剩宏回退，所以这里无条件包含不会多一个符号。 */
#include "arkrtos_ushell_upcall.h"

#if (ARKRTOS_HW_SAFE == 1)
#include "arkrtos_hwsafe.h"
#endif

#if (ARKRTOS_USE_KERNEL_GUARD == 1)
#include "arkrtos_guard.h"
#endif

#if (ARKRTOS_USE_IMAGE_SIGN == 1)
#include "arkrtos_sign.h"
#include "arkrtos_app_image.h"
#include "arkrtos_loader.h"
#endif

#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1)
#include "arkrtos_app_image.h"    /* 容器格式 + arkrtos_crc32 */
#include "arkrtos_loader.h"       /* arkrtos_loader_crc_recheck / reloc_apply */
#include "arkrtos_sec.h"          /* 池内端到端用例要跑一遍 sec 自检 */
#endif

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
    uint32 state;

    /* 自审是一次多步遍历，不是快照：遍历途中被节拍打断，就会读到「一边
     * 已经改、一边还没改」的中间态，报出并不存在的 SCHED 不一致。
     * 实测证据：LA64 打开 KERNEL_GUARD 后首次跑这条用例时 1/3 断言失败
     * （mask=0x40），复跑即通过——不是状态坏了，是在动的东西上量静的东西。
     * 要判的是「静止态自洽」，所以整段遍历关中断；这也解释了为什么守护模块
     * 把自审放在 tick 入口、调度动作之前调用。 */
    state = ARKRTOS_ENTER_CRITICAL();
    arkrtos_audit_run(&r);
    ARKRTOS_EXIT_CRITICAL(state);

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
/* 栈要按**最宽的那个移植**算：trap 帧本身就占掉 256 B（LA64 是 32 个
 * 8 字节槽），512 B 的任务栈只剩 256 B 给调用链，实测在 LA64 上不够 ——
 * 溢出会写进相邻任务（.bss 里两块栈挨着）的保存帧，症状是那个任务被
 * resume 时 ERA 变成溢出者压在栈上的参数（实测是 mtx_lock 的 300 ms
 * 超时），于是跳到 0x12c 取指、报 INE 停机。RV32 的帧只有 144 B，
 * 512 B 正好够，所以这条只在 LA64 上现形。给 2 KB，两边都宽裕。 */
#define PI_CHAIN_STACK_WORDS 512u   /* 2 KB */

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

/* ============================================================
 * 硬件安全态（ARKRTOS_HW_SAFE）：所有者死后必须由**内核**中和
 * 契约：阶段一只标记不驱动；阶段二执行且只执行一次；正常 release 不触发；
 *       计划内交接只摘台账、不计入"已中和"。
 * ============================================================ */
#if (ARKRTOS_HW_SAFE == 1)

static uint32 s_hw_cb_calls;
static uint32 s_hw_cb_owner;
static arkrtos_dev_hdr_t *s_hw_cb_obj;

static int32 utest_hw_neutralize(arkrtos_dev_hdr_t *obj, uint32 owner)
{
    s_hw_cb_calls++;
    s_hw_cb_owner = owner;
    s_hw_cb_obj   = obj;
    return 0;
}

UTEST_CASE(hwsafe_death_neutralizes)
{
    static arkrtos_dev_hdr_t s_obj;
    /* 用一个不存在的任务号当所有者：不能拿控制台任务试，那会把它的真实
     * 设备台账一起标死（on_death 是按 owner 扫全表的）。 */
    int32 fake_owner = (int32)ARKRTOS_TASK_MAX_NUM + 1;
    uint32 will = 0u, fail = 0u, cb = 0u, none = 0u, hand = 0u;
    uint32 cb0;
    /* 台账不是空的：控制台等真实设备已经由内核认领了条目。
     * 这条用例只能判「我这一条」的增删，所以全程以基线为参照。 */
    int32 oc0;

    UTEST_REQUIRE(fake_owner > 0);
    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &hand);
    cb0 = cb;
    oc0 = arkrtos_hwsafe_owner_count();
    UTEST_CHECK(oc0 >= 0);

    UTEST_CHECK_EQ(arkrtos_hwsafe_register("utneo", utest_hw_neutralize), 0);
    UTEST_CHECK_EQ(arkrtos_hwsafe_registered("utneo"), 1);

    /* 没有 claim 就不能替别人留遗言：遗言是所有者权利，不是广播 */
    UTEST_CHECK_EQ(arkrtos_hwsafe_set_will("utneo", fake_owner, 1, 1),
                   ARKRTOS_HWSAFE_ERR_OWNER);

    arkrtos_hwsafe_claim("utneo", fake_owner, &s_obj);
    UTEST_CHECK_EQ(arkrtos_hwsafe_owner_count(), oc0 + 1);

    /* 阶段一：只打标记，回调一次都不许跑 */
    s_hw_cb_calls = 0u;
    arkrtos_hwsafe_on_death(fake_owner);
    UTEST_CHECK_EQ((long)s_hw_cb_calls, 0L);

    /* 阶段二：执行回调，参数是（实例, 所有者） */
    UTEST_CHECK_EQ((long)arkrtos_hwsafe_run_pending(), 1L);
    UTEST_CHECK_EQ((long)s_hw_cb_calls, 1L);
    UTEST_CHECK_EQ((long)s_hw_cb_owner, (long)fake_owner);
    UTEST_CHECK(s_hw_cb_obj == &s_obj);
    UTEST_CHECK_EQ(arkrtos_hwsafe_owner_count(), oc0);

    /* 幂等：再跑一次不会重复中和 */
    UTEST_CHECK_EQ((long)arkrtos_hwsafe_run_pending(), 0L);
    UTEST_CHECK_EQ((long)s_hw_cb_calls, 1L);

    arkrtos_hwsafe_path_counts(&will, &fail, &cb, &none, &hand);
    UTEST_CHECK_EQ((long)cb, (long)(cb0 + 1u));

    /* 正常 close 之后死亡：台账已经没了，不该再驱动硬件 */
    s_hw_cb_calls = 0u;
    arkrtos_hwsafe_claim("utneo", fake_owner, &s_obj);
    arkrtos_hwsafe_release("utneo", fake_owner);
    arkrtos_hwsafe_on_death(fake_owner);
    UTEST_CHECK_EQ((long)arkrtos_hwsafe_run_pending(), 0L);
    UTEST_CHECK_EQ((long)s_hw_cb_calls, 0L);

    /* 计划内交接：只摘台账，既不驱动硬件也不计入"已中和" */
    {
        uint32 n0 = arkrtos_hwsafe_neutralized_count();
        arkrtos_hwsafe_claim("utneo", fake_owner, &s_obj);
        arkrtos_hwsafe_on_death_reason(fake_owner, 1);
        UTEST_CHECK_EQ((long)arkrtos_hwsafe_run_pending(), 0L);
        UTEST_CHECK_EQ((long)arkrtos_hwsafe_neutralized_count(), (long)n0);
        UTEST_CHECK_EQ(arkrtos_hwsafe_owner_count(), oc0);
    }

    UTEST_CHECK_EQ(arkrtos_hwsafe_unregister("utneo"), 0);
    UTEST_CHECK_EQ(arkrtos_hwsafe_registered("utneo"), 0);
}

#endif /* ARKRTOS_HW_SAFE */

/* ============================================================
 * 心跳合同（ARKRTOS_USE_KERNEL_GUARD）：声明的窗口必须被判，而且判到具体任务
 * 契约：窗口小于服务周期 → 明确拒绝；按期上报 → 不违规；停报超过窗口 →
 *       BAD_BEAT 且 misses 增长；撤销合同后不再记账。
 * ============================================================ */
#if (ARKRTOS_USE_KERNEL_GUARD == 1)

UTEST_CASE(guard_heartbeat_contract)
{
    int32 self = arkrtos_current_task_id;
    arkrtos_guard_status_t st;
    arkrtos_guard_task_t e;
    uint32 slots, i, found = 0u;

    UTEST_REQUIRE(self > 0);

    /* 判不了的窗口必须被拒：接受它就等于发一张永远有效的合格证 */
    UTEST_CHECK_EQ((long)arkrtos_guard_beat(self, 10u),
                   (long)ARKRTOS_GUARD_ERR_PARAM);
    UTEST_CHECK_EQ((long)arkrtos_guard_beat(self, 100u), 0L);

    arkrtos_guard_status(&st);
    UTEST_CHECK(st.declared >= 1u);

    /* 停报 300 ms（声明窗口 100 ms）：服务周期一到必须判违规 */
    arkrtos_task_wait_internal(600u);

    arkrtos_guard_status(&st);
    UTEST_CHECK((st.verdict & ARKRTOS_GUARD_BAD_BEAT) != 0u);
    UTEST_CHECK(st.violating >= 1u);

    /* 违规要能落到具体任务，而不是只剩一个全局布尔 */
    slots = arkrtos_guard_task_slots();
    for(i = 0u; i < slots; i++)
    {
        if((arkrtos_guard_task_at(i, &e) == 0) && (e.task_id == self))
        {
            found = 1u;
            UTEST_CHECK_EQ((long)e.declared, 1L);
            UTEST_CHECK_EQ((long)e.period_ms, 100L);
            UTEST_CHECK(e.misses > 0u);
        }
    }
    UTEST_CHECK_EQ((long)found, 1L);

    /* 撤掉合同：别把这条违规留给后面的用例 */
    UTEST_CHECK_EQ((long)arkrtos_guard_beat(self, 0u), 0L);
}

#endif /* ARKRTOS_USE_KERNEL_GUARD */

/* ============================================================
 * 镜像签名（ARKRTOS_USE_IMAGE_SIGN）：算法自洽 + 安装路径真的会拒收
 * 覆盖：整块 MAC 与流式 MAC 同一口径、负载/签名翻位必须失败、
 *       CRC 对而签名错时装载器必须回 -18（不是别的失败码）。
 * 不覆盖：密钥保管（对称方案本身不解决）、非对称信任链（未实现）。
 * ============================================================ */
#if (ARKRTOS_USE_IMAGE_SIGN == 1)

#define SIGN_TEST_PAYLOAD   (32u)
#define SIGN_TEST_IMGLEN    (ARKRTOS_APP_HEADER_SIZE + SIGN_TEST_PAYLOAD)

static uint8 s_sign_img[SIGN_TEST_IMGLEN];

static uint32 utest_sign_crc(const arkrtos_app_header_t *h)
{
    static const uint8 zero4[4] = { 0u, 0u, 0u, 0u };
    const uint8 *img = s_sign_img;
    uint32 total = ARKRTOS_APP_TOTAL_LEN(h);
    uint32 c;

    c = arkrtos_crc32(img, ARKRTOS_APP_OFF_CRC32, 0u);
    c = arkrtos_crc32(zero4, 4u, c);
    c = arkrtos_crc32(img + (ARKRTOS_APP_OFF_CRC32 + 4u),
                      ARKRTOS_APP_OFF_STATE - (ARKRTOS_APP_OFF_CRC32 + 4u), c);
    c = arkrtos_crc32(zero4, 4u, c);
    c = arkrtos_crc32(img + (ARKRTOS_APP_OFF_STATE + 4u),
                      ARKRTOS_APP_OFF_RUNTIME_RAM_BASE - (ARKRTOS_APP_OFF_STATE + 4u), c);
    c = arkrtos_crc32(zero4, 4u, c);
    c = arkrtos_crc32(img + (ARKRTOS_APP_OFF_RUNTIME_RAM_BASE + 4u),
                      ARKRTOS_APP_HEADER_SIZE - (ARKRTOS_APP_OFF_RUNTIME_RAM_BASE + 4u), c);
    c = arkrtos_crc32(img + ARKRTOS_APP_HEADER_SIZE,
                      total - ARKRTOS_APP_HEADER_SIZE, c);
    return c;
}

UTEST_CASE(sign_hmac_and_loader_gate)
{
    arkrtos_app_header_t *h = (arkrtos_app_header_t *)s_sign_img;
    uint32 i;
    int32 rc;

    for(i = 0u; i < SIGN_TEST_IMGLEN; i++)
    {
        s_sign_img[i] = 0u;
    }

    h->magic          = ARKRTOS_APP_MAGIC;
    h->type           = ARKRTOS_APP_TYPE_DRIVER;
    h->hw_compat_id   = ARKRTOS_HW_COMPAT_ID;
    h->image_size     = SIGN_TEST_PAYLOAD;
    h->entry_offset   = 0u;
    h->reloc_offset   = ARKRTOS_APP_RELOC_OFFSET;
    h->reloc_count    = 0u;
    h->reloc_kind     = ARKRTOS_APP_RELOC_KIND_CUR;
    h->payload_offset = ARKRTOS_APP_PAYLOAD_OFFSET(h);
    /* 装载器在字段校验里就要求 ram_size 落在 [SLOT_RAM_MIN_BLOCK,
     * SLOT_RAM_MAX_BLOCK]：0 会在签名门之前被 ERR_SIZE(-4) 挡下，
     * 那样这条用例就变成了在测尺寸校验，而不是在测签名门。 */
    h->ram_size       = 1024u;

    for(i = 0u; i < SIGN_TEST_PAYLOAD; i++)
    {
        s_sign_img[ARKRTOS_APP_HEADER_SIZE + i] = (uint8)(0xA0u + i);
    }

    /* 顺序有讲究：签名覆盖「签名区清零」的形态，CRC 覆盖签名本身。
     * 反过来算（先 CRC 再签）会让签名后的 CRC 失效，安装永远过不去。 */
    arkrtos_sign_image(s_sign_img, h, h->signature);
    h->crc32 = utest_sign_crc(h);

    UTEST_CHECK_EQ((long)arkrtos_sign_verify(s_sign_img, h), 0L);

    /* 流式通道（安装时边走边算）必须与整块通道同一口径 */
    arkrtos_sign_stream_begin(s_sign_img);
    arkrtos_sign_stream_update(s_sign_img + ARKRTOS_APP_HEADER_SIZE,
                               (uint32)ARKRTOS_APP_RELOC_SIZE * h->reloc_count +
                               h->image_size);
    UTEST_CHECK_EQ((long)arkrtos_sign_stream_end(h), 0L);

    /* 负对照①：负载翻字节 ⇒ MAC 不符 */
    s_sign_img[ARKRTOS_APP_HEADER_SIZE] ^= 0xFFu;
    UTEST_CHECK(arkrtos_sign_verify(s_sign_img, h) != 0);
    s_sign_img[ARKRTOS_APP_HEADER_SIZE] ^= 0xFFu;

    /* 负对照②：签名自己翻一位 ⇒ MAC 不符（签名不能覆盖自己） */
    h->signature[0] ^= 0x01u;
    UTEST_CHECK(arkrtos_sign_verify(s_sign_img, h) != 0);

    /* 安装路径：把 CRC 重新算对，只留签名错 ⇒ 必须正好是 ERR_SIGN。
     * 这一步证明门真的挂在装载器上，而不是只有算法层面自洽。 */
    h->crc32 = utest_sign_crc(h);
    rc = arkrtos_loader_load_buffer(s_sign_img, SIGN_TEST_IMGLEN);
    UTEST_CHECK_EQ((long)rc, (long)ARKRTOS_LOADER_ERR_SIGN);

    /* 正对照：签名改回来 ⇒ 不再是 ERR_SIGN（能否落地取决于池里有没有干净区，
     * 那不是本条要判的事，所以只要求"不是被签名门挡下"）。 */
    h->signature[0] ^= 0x01u;
    h->crc32 = utest_sign_crc(h);
    UTEST_CHECK_EQ((long)arkrtos_sign_verify(s_sign_img, h), 0L);
    rc = arkrtos_loader_load_buffer(s_sign_img, SIGN_TEST_IMGLEN);
    UTEST_CHECK(rc != ARKRTOS_LOADER_ERR_SIGN);
    if(rc >= 0)
    {
        (void)arkrtos_loader_uninstall((uint32)rc);     /* 收拾现场 */
    }
}

#endif /* ARKRTOS_USE_IMAGE_SIGN */


/* ============================================================
 * 开机复核镜像 CRC（SEC-G17）：把「落盘形态」反算回标称形态
 * 覆盖：打过重定位补丁的字节能被反算回去并通过校验；负载翻一个字节必须
 *       变成 ERR_CRC（不是 PASS，也不是"算不出来"）；runtime_ram_base
 *       还没固化时必须报 ERR_PARAM——那是"算不出来"，不能算成 PASS。
 * 不覆盖：真 Flash 上的电离位翻转（这里在 RAM 里造等价形态）。
 *
 * 两个前提写死在这里，免得后来的读者误读：
 *   ① 这个函数只对「已经装了」的形态成立。它总是把 delta 减掉，所以拿一份
 *      没装过的标称镜像去调它，等于把 delta 多减一次——那不是 bug，是它的
 *      语义（"Flash 上躺的是打过补丁的字节"）。
 *   ② 因此用例必须先 install 再判，并且**断言 install 真的改了字节**：
 *      否则 delta 一旦意外为 0，这条用例就会变成"恒真"的假绿。
 * ============================================================ */
#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1)

#define CRCRC_TEST_PAYLOAD   (64u)
#define CRCRC_TEST_RECNT     (2u)
#define CRCRC_TEST_BASE      (0x80300000u)
#define CRCRC_TEST_NOM_RAM   (0x80400000u)
#define CRCRC_TEST_RT_RAM    (0x80400200u)
#define CRCRC_TEST_PAYOFF    (ARKRTOS_APP_HEADER_SIZE + CRCRC_TEST_RECNT * \
                              ARKRTOS_APP_RELOC_SIZE)
#define CRCRC_TEST_IMGLEN    (CRCRC_TEST_PAYOFF + CRCRC_TEST_PAYLOAD)

/* 按字声明：头里有 uint32 字段，用 uint8 数组再强转不保证 4 字节对齐，
 * LA64/ARM 开了 strict-align 就会在第一次解引用时直接异常。 */
static uint32 s_crcrc_words[(CRCRC_TEST_IMGLEN + 3u) / 4u];
#define s_crcrc_img ((uint8 *)s_crcrc_words)

/* 按打包工具的公式封口：头（三字段按 0）+ 重定位表 + 负载。
 * 签名写进头之后必须再调一次——签名在头里，头参与 CRC。 */
static void utest_crcrc_seal(arkrtos_app_header_t *h)
{
    uint32 c;

    c = arkrtos_loader_crc_header(h);
    c = arkrtos_crc32(s_crcrc_img + ARKRTOS_APP_RELOC_OFFSET,
                      CRCRC_TEST_RECNT * ARKRTOS_APP_RELOC_SIZE, c);
    c = arkrtos_crc32(s_crcrc_img + CRCRC_TEST_PAYOFF, CRCRC_TEST_PAYLOAD, c);
    h->crc32 = c;
}

/* 构造一份标称形态镜像并算好 crc32。
 * zero_delta=1 时把两个标称基址设成「落点恰好等于标称基址」，
 * 用于验证"反算量退化为 0"时函数也自洽（那时标称形态与落盘形态相同）。 */
static arkrtos_app_header_t *utest_crcrc_build(int zero_delta)
{
    arkrtos_app_header_t *h = (arkrtos_app_header_t *)s_crcrc_img;
    uint32 *p_rel = (uint32 *)(s_crcrc_img + ARKRTOS_APP_RELOC_OFFSET);
    uint32 *p_pay = (uint32 *)(s_crcrc_img + CRCRC_TEST_PAYOFF);
    uint32 i;

    for(i = 0u; i < ((CRCRC_TEST_IMGLEN + 3u) / 4u); i++)
    {
        s_crcrc_words[i] = 0u;
    }

    h->magic          = ARKRTOS_APP_MAGIC;
    h->type           = ARKRTOS_APP_TYPE_DRIVER;
    h->hw_compat_id   = ARKRTOS_HW_COMPAT_ID;
    h->image_size     = CRCRC_TEST_PAYLOAD;
    h->entry_offset   = 0u;
    h->reloc_offset   = ARKRTOS_APP_RELOC_OFFSET;
    h->reloc_count    = CRCRC_TEST_RECNT;
    h->reloc_kind     = ARKRTOS_APP_RELOC_KIND_CUR;
    h->payload_offset = CRCRC_TEST_PAYOFF;
    h->ram_size       = 1024u;

    /* ROM 增量 = (落点 + payload_offset) - nominal_base，
     * RAM 增量 = runtime_ram_base - nominal_ram_base */
    h->nominal_base     = zero_delta ? (CRCRC_TEST_BASE + CRCRC_TEST_PAYOFF)
                                     : CRCRC_TEST_BASE;
    h->nominal_ram_base = zero_delta ? CRCRC_TEST_RT_RAM : CRCRC_TEST_NOM_RAM;
    h->runtime_ram_base = CRCRC_TEST_RT_RAM;

    /* 一条 ROM 类、一条 RAM 类：两类增量不同，只测其中一类就分不出
     * 「两个 delta 有没有串用」。 */
    p_rel[0] = ARKRTOS_APP_RELOC_ENTRY(CRCRC_TEST_PAYOFF, ARKRTOS_APP_RELOC_KIND_ROM);
    p_rel[1] = ARKRTOS_APP_RELOC_ENTRY(CRCRC_TEST_PAYOFF + 4u, ARKRTOS_APP_RELOC_KIND_RAM);

    /* 标称形态里这两个字装的是**链接期地址**，安装时会被加上 delta。 */
    p_pay[0] = 0x80001000u;
    p_pay[1] = 0x80400040u;
    for(i = 2u; i < (CRCRC_TEST_PAYLOAD / 4u); i++)
    {
        p_pay[i] = 0xA5A50000u + i;
    }

    utest_crcrc_seal(h);

    return h;
}

/* 把标称形态变成「落盘形态」：与安装期完全相同的两个增量公式。 */
static void utest_crcrc_install(arkrtos_app_header_t *h)
{
    uint32 *p_rel = (uint32 *)(s_crcrc_img + ARKRTOS_APP_RELOC_OFFSET);
    uint32 idx = 0u;
    uint32 delta_rom = (CRCRC_TEST_BASE + CRCRC_TEST_PAYOFF) - h->nominal_base;
    uint32 delta_ram = h->runtime_ram_base - h->nominal_ram_base;

    (void)arkrtos_loader_reloc_apply(s_crcrc_img + CRCRC_TEST_PAYOFF,
                                     CRCRC_TEST_PAYOFF, CRCRC_TEST_PAYLOAD,
                                     h, p_rel, &idx, delta_rom, delta_ram);
}

UTEST_CASE(loader_crc_recheck_relocated)
{
    arkrtos_app_header_t *h = utest_crcrc_build(0);
    uint32 *p_pay = (uint32 *)(s_crcrc_img + CRCRC_TEST_PAYOFF);
    uint32 nominal_crc = h->crc32;
    uint32 word0_before = p_pay[0];
    uint32 word1_before = p_pay[1];
    uint32 got = 0u;
    int32 rc;

    /* 先证明这份镜像真的处在"落盘形态"：两个类别的字各被加上了不同的增量。
     * 少了这一步，delta 意外为 0 时整条用例会静默变成恒真。 */
    utest_crcrc_install(h);
    UTEST_CHECK(p_pay[0] != word0_before);
    UTEST_CHECK(p_pay[1] != word1_before);
    UTEST_CHECK_EQ((long)(p_pay[0] - word0_before), (long)CRCRC_TEST_PAYOFF);
    UTEST_CHECK_EQ((long)(p_pay[1] - word1_before),
                   (long)(CRCRC_TEST_RT_RAM - CRCRC_TEST_NOM_RAM));

    /* 主角：落盘形态（打过补丁）必须能反算回标称形态并判 0。 */
    rc = arkrtos_loader_crc_recheck(s_crcrc_img, CRCRC_TEST_BASE,
                                    CRCRC_TEST_IMGLEN, &got);
    UTEST_CHECK_EQ((long)rc, 0L);
    UTEST_CHECK_EQ((long)got, (long)nominal_crc);

    /* 负对照①：翻一个**表项覆盖不到**的负载字节 ⇒ ERR_CRC。 */
    s_crcrc_img[CRCRC_TEST_PAYOFF + 8u] ^= 0x01u;
    rc = arkrtos_loader_crc_recheck(s_crcrc_img, CRCRC_TEST_BASE,
                                    CRCRC_TEST_IMGLEN, &got);
    UTEST_CHECK_EQ((long)rc, (long)ARKRTOS_LOADER_ERR_CRC);
    s_crcrc_img[CRCRC_TEST_PAYOFF + 8u] ^= 0x01u;

    /* 负对照②：翻一个**表项覆盖**的字（反算要对它做减法）。
     * 这一条才真正证明"反算"参与校验，而不是恰好把这段跳过了。 */
    s_crcrc_img[CRCRC_TEST_PAYOFF + 1u] ^= 0x01u;
    rc = arkrtos_loader_crc_recheck(s_crcrc_img, CRCRC_TEST_BASE,
                                    CRCRC_TEST_IMGLEN, &got);
    UTEST_CHECK_EQ((long)rc, (long)ARKRTOS_LOADER_ERR_CRC);
    s_crcrc_img[CRCRC_TEST_PAYOFF + 1u] ^= 0x01u;

    /* 负对照③：容量给少了 ⇒ 是"看不全"，必须单独一种失败码，
     * 不能悄悄只算前一段然后报 PASS。 */
    UTEST_CHECK_EQ((long)arkrtos_loader_crc_recheck(s_crcrc_img, CRCRC_TEST_BASE,
                                                   CRCRC_TEST_IMGLEN - 4u, &got),
                   (long)ARKRTOS_LOADER_ERR_SIZE);

    /* 收尾：改回来之后必须重新判 0（证明上面的翻位都真的复原了） */
    UTEST_CHECK_EQ((long)arkrtos_loader_crc_recheck(s_crcrc_img, CRCRC_TEST_BASE,
                                                   CRCRC_TEST_IMGLEN, &got), 0L);
}

UTEST_CASE(loader_crc_recheck_zero_delta)
{
    arkrtos_app_header_t *h = utest_crcrc_build(1);
    uint32 *p_pay = (uint32 *)(s_crcrc_img + CRCRC_TEST_PAYOFF);
    uint32 word0_before = p_pay[0];
    uint32 word1_before = p_pay[1];
    uint32 got = 0u;

    /* 落点恰好等于标称基址：两个增量都是 0，标称形态与落盘形态相同。
     * 这是**巧合成立**的那一条路径，函数也必须给出 PASS。 */
    utest_crcrc_install(h);
    UTEST_CHECK_EQ((long)p_pay[0], (long)word0_before);
    UTEST_CHECK_EQ((long)p_pay[1], (long)word1_before);
    UTEST_CHECK_EQ((long)arkrtos_loader_crc_recheck(s_crcrc_img, CRCRC_TEST_BASE,
                                                   CRCRC_TEST_IMGLEN, &got), 0L);
    UTEST_CHECK_EQ((long)got, (long)h->crc32);

    /* 反面对照：把落点往后再挪一个字（增量不再为 0），同一份字节立刻不再是
     * 标称形态 ⇒ 必须判 ERR_CRC。若这里仍然 PASS，说明 delta 根本没参与运算。 */
    UTEST_CHECK_EQ((long)arkrtos_loader_crc_recheck(s_crcrc_img, CRCRC_TEST_BASE + 4u,
                                                   CRCRC_TEST_IMGLEN, &got),
                   (long)ARKRTOS_LOADER_ERR_CRC);
}

UTEST_CASE(loader_crc_recheck_unknown_ram_base)
{
    arkrtos_app_header_t *h = utest_crcrc_build(0);

    utest_crcrc_install(h);

    /* runtime_ram_base 还没固化（打包/传输中的形态）：RAM 类增量算不出来。
     * 这时必须报 ERR_PARAM（"算不出来"），绝不能因为 ROM 类还能算就报 PASS——
     * 那正是"看着权威的错答案"。 */
    h->runtime_ram_base = 0u;
    UTEST_CHECK_EQ((long)arkrtos_loader_crc_recheck(s_crcrc_img, CRCRC_TEST_BASE,
                                                   CRCRC_TEST_IMGLEN, (uint32 *)0),
                   (long)ARKRTOS_LOADER_ERR_PARAM);

    /* 出参指针为 0 也是参数错：能算出 CRC 却不能交出去，等于白算。 */
    h->runtime_ram_base = CRCRC_TEST_RT_RAM;
    UTEST_CHECK_EQ((long)arkrtos_loader_crc_recheck(s_crcrc_img, CRCRC_TEST_BASE,
                                                   CRCRC_TEST_IMGLEN, (uint32 *)0),
                   (long)ARKRTOS_LOADER_ERR_PARAM);
}


/* 池内端到端：真装一份镜像进池，再跑一遍 sec，看这条自检项真的给出 PASS。
 * 这一步覆盖的是「安装期写进去的 runtime_ram_base 与落盘字节是否自洽」——
 * RAM 里的等价构造测不到这一点，因为那里的 runtime_ram_base 是我自己填的。 */
#if (ARKRTOS_USE_SECCHECK == 1)

static const arkrtos_sec_item_t *utest_sec_find(const arkrtos_sec_report_t *r,
                                                const char *name)
{
    uint32 i;

    for(i = 0u; i < r->count; i++)
    {
        const char *a = r->item[i].name;
        const char *b = name;

        while((*a != '\0') && (*a == *b))
        {
            a++;
            b++;
        }
        if((*a == '\0') && (*b == '\0'))
        {
            return &r->item[i];
        }
    }
    return 0;
}

UTEST_CASE(loader_crc_recheck_pool_e2e)
{
    arkrtos_app_header_t *h = utest_crcrc_build(0);
    arkrtos_sec_report_t rep;
    const arkrtos_sec_item_t *it;
    int32 slot;

#if (ARKRTOS_USE_IMAGE_SIGN == 1)
    /* 开关开时安装路径要过签名门；签名落在头里，头参与 CRC ⇒ 签完必须重封口 */
    arkrtos_sign_image(s_crcrc_img, h, h->signature);
    utest_crcrc_seal(h);
#endif

    slot = arkrtos_loader_load_buffer(s_crcrc_img, CRCRC_TEST_IMGLEN);
    if(slot < 0)
    {
        /* 装不进去时必须说清是哪一类原因：环境问题（没空间）该跳过，
         * 构造/算法问题该报出来——两者混成一个"跳过"就等于把 bug 藏进跳过里。 */
        const char *why = "pool refused the image";

        if(slot == ARKRTOS_LOADER_ERR_SIGN)
        {
            why = "signature gate rejected the test image";
        }
        else if(slot == ARKRTOS_LOADER_ERR_CRC)
        {
            why = "crc mismatch on the constructed image";
        }
        else if((slot == ARKRTOS_LOADER_ERR_SIZE) ||
                (slot == ARKRTOS_LOADER_ERR_NOSPACE) ||
                (slot == ARKRTOS_LOADER_ERR_NO_SLOT))
        {
            why = "no space/slot for the test image";
        }

        ark_utest_skip(why);
        return;
    }

    (void)arkrtos_sec_run(&rep);
    it = utest_sec_find(&rep, "images.crc_recheck");
    UTEST_CHECK(it != 0);
    if(it != 0)
    {
        UTEST_CHECK_EQ((long)it->state, (long)ARKRTOS_SEC_PASS);
        UTEST_CHECK_EQ((long)it->v[0], 1L);     /* checked */
        UTEST_CHECK_EQ((long)it->v[1], 0L);     /* bad    */
        UTEST_CHECK_EQ((long)it->v[2], 0L);     /* unknown*/
    }

    (void)arkrtos_loader_uninstall((uint32)slot);
}

#endif /* ARKRTOS_USE_SECCHECK */

#endif /* ARKRTOS_USE_IMAGE_CRC_RECHECK */

/* ============================================================
 * SEC-G08 隔离执行：upcall 返回通道的分派（LA64 + ISO 档才有实体）
 * @details 本用例证明的是**分派接通**，不是「回调跑通了」：
 *          1. 实体真的链接进了镜像 —— call()/stub_supported() 在 ISO==0 档
 *             分别退化成宏回退（恒 -2）与「没有这个符号」，本构建里两条都在。
 *             另外 call() 因为 func==0 必须回 ERR_PARAM(-1) —— 只有真函数
 *             才会做这个判据，宏回退做不到，这就是「实体在」的证据。
 *          2. 冒充者直接发 `syscall 0x23`：$a0 里塞 0x5a5a 当哨兵，
 *             回读必须是 ERR_CALLER(-7)。若回读到 0x5a5a，说明这条系统调用
 *             根本没进内核（号被别的东西吃掉了），那条读数就是假的。
 *          3. 再发一次仍是 -7：拒绝不留痕（第一次没有把记账槽占掉）。
 *
 *          口径（本轮实测确认）：Ark 自己的 SVC 返回值是 **32 位** 的，
 *          ARKRTOS_SVC_RET 把值按 uint32 写进帧的 $a0，高位补零；负值读回来
 *          是 0x00000000FFFFFFF9 而不是符号扩展的 -7。这是既有 ABI（
 *          arkrtos_svc_arg_t 就是 32 位），App 侧按 int32 取低 32 位即可；
 *          要 64 位符号扩展的是 Linux ABI 那条路（svc_set_ret_signed）。
 *          所以这里按 (uint32) 比较，不拿 long 直接比 —— 前一版就是拿 long
 *          比，读到的 0xFFFFFFF9 被当成失败，其实是判据写错了。
 * @note 本任务不是 call() 亲手登记的那条 upcall 线程，所以这里走的正是
 *       「冒充者被拒」分支；记账不变由 ret() 的实现保证（见设计 §4.5）。
 *       ready() 的 0/1 两档由宿主用例覆盖（tools/coverage_gate 的 ushell
 *       档，两臂读数 212 / 17 条断言），这里不重复绑板上装载状态。
 */
#if (ARKRTOS_USE_USHELL_ISO == 1) && defined(__loongarch__)

UTEST_CASE(ushell_upcall_refuse_stranger)
{
    long  a0 = 0;
    int32 rc = 0;

    UTEST_CHECK_EQ((long)arkrtos_port_ushell_stub_supported(), 1L);
    UTEST_CHECK_EQ((long)arkrtos_ushell_upcall_call(
                       0u, 1u, (int (*)(int, char **))0, 0, (char **)0, &rc),
                   (long)ARKRTOS_USHELL_UP_ERR_PARAM);

    /* syscall 0x23 = 0x002B0000 | 0x23：号编在指令里，与 Ark 的号表同源。 */
    __asm__ __volatile__(
        "  li.w    $a0, 0x5a5a\n"
        "  .word   0x002b0023\n"
        "  move    %0, $a0\n"
        : "=r"(a0)
        :
        : "$a0", "$a1", "$a2", "$a3", "$a4", "$a5", "$a6", "$a7",
          "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7", "$t8",
          "memory");
    UTEST_CHECK_NE(a0, 0x5a5aL);
    UTEST_CHECK_EQ((uint32)a0, (uint32)ARKRTOS_USHELL_UP_ERR_CALLER);

    __asm__ __volatile__(
        "  li.w    $a0, 0x1234\n"
        "  .word   0x002b0023\n"
        "  move    %0, $a0\n"
        : "=r"(a0)
        :
        : "$a0", "$a1", "$a2", "$a3", "$a4", "$a5", "$a6", "$a7",
          "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7", "$t8",
          "memory");
    UTEST_CHECK_NE(a0, 0x1234L);
    UTEST_CHECK_EQ((uint32)a0, (uint32)ARKRTOS_USHELL_UP_ERR_CALLER);
}

#endif /* ARKRTOS_USE_USHELL_ISO && __loongarch__ */

#endif /* ARKRTOS_USE_UTEST */
