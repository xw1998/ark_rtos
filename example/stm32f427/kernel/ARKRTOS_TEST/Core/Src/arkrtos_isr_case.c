/**
 * @file arkrtos_isr_case.c
 * @brief 板级 utest 用例：在真正的中断上下文里直接收发消息（EXTI0 软件触发）
 * @details 这里要证明的是「ISR 侧能直接收发消息」这条承诺在真硬件上成立，
 *          而不是只在宿主机上把等待队列手工塞满后读一遍数字。
 *
 *          为什么单独放在板级工程、不放进 kernelsrc：
 *          - 触发中断必须依赖具体芯片的 NVIC / EXTI，内核源码是板无关的；
 *          - 用例只需要「一次真的进 handler」，用 EXTI0 的软件触发（SWIER）
 *            即可，不必占用任何 GPIO，也不必让某个外部引脚真的翻转。
 *          - startup_stm32f427xx.s 里 EXTI0_IRQHandler 是 [WEAK] 的空桩，
 *            本文件定义同名函数即覆盖它（向量表已经指向该符号）。
 *
 *          判据（缺一条都不算过）：
 *          1. handler 真的被执行（hits == 1），且执行时 IPSR != 0（自证在
 *             异常上下文，而不是被谁直接当普通函数调了一下）；
 *          2. arkrtos_mq_send_from_isr 在 ISR 里返回 0，任务侧随后能取到
 *             同一条消息、同样的字数与内容；
 *          3. arkrtos_mq_recv_from_isr 在队列有数据时返回实际字数，内容正确，
 *             取走后队列真的空了；
 *          4. 队列空时 recv_from_isr 返回 ARKRTOS_MQ_ERR_EMPTY，不登记等待者、
 *             不阻塞（handler 立刻返回，用例不挂死）；
 *          5. 队列满时 send_from_isr 返回 ARKRTOS_MQ_ERR_FULL，不覆盖、不丢旧数据。
 *
 *          门控：ARKRTOS_USE_UTEST 与 ARKRTOS_USE_MQ 都为 1 才编入；任一为 0
 *          时本文件整体不参与编译，体积与行为零变化。
 *
 * @note 本文件属于板级工程（example/），不属于内核。
 */
#include "main.h"

#include "arkrtos.h"
#include "arkrtos_mq.h"
#include "arkrtos_utest.h"

#if (ARKRTOS_USE_UTEST == 1) && (ARKRTOS_USE_MQ == 1)

/* 队列名：内部只保留 7 字符 + 结束符，超长会被截断，这里主动取短名 */
#define ISR_MQ_NAME      "isrmq"
/* 等 handler 跑起来的自旋上限：handler 只是几条内存写，
 * 真跑起来是「下一个指令边界」的事，这么多轮足够；超时即判失败，
 * 不无限等待（坏掉时必须几百微秒内报红，不能挂死控制台）。 */
#define ISR_WAIT_SPIN    2000000u
/* 队列深度（与 ARKRTOS_MQ_DEPTH 对齐，用例用它来造「满」）*/
#define ISR_MQ_FILL      ARKRTOS_MQ_DEPTH

/* handler 与用例共享的取证量：全部 volatile，handler 只写、用例只读 */
static volatile uint32 s_isr_hits;             /* handler 执行次数 */
static volatile uint32 s_isr_in_handler;       /* 1 = 执行时 IPSR != 0 */
static volatile int32  s_isr_send_ret = -99;   /* send_from_isr 的返回值 */
static volatile int32  s_isr_recv_ret = -99;   /* recv_from_isr 的返回值 */
static volatile uint32 s_isr_recv_words[2];    /* recv_from_isr 取到的内容 */
static volatile uint8  s_isr_mode;             /* 0 = 只发；1 = 只收 */

static int32 s_isr_mq = -1;

/* 覆盖 startup 里的弱符号空桩：向量表 DCD EXTI0_IRQHandler 指向这里 */
void EXTI0_IRQHandler(void)
{
    if((EXTI->PR & 0x1u) == 0u)
    {
        /* 不是本线触发：不动全局量，直接返回（宁可少记一次，也不记假账） */
        return;
    }
    EXTI->PR = 0x1u;                            /* 先清挂起，避免返回后立刻重入 */

    s_isr_in_handler = (__get_IPSR() != 0u) ? 1u : 0u;

    if(s_isr_mode == 0u)
    {
        uint32 msg[2];
        msg[0] = 0xA5A50001u;
        msg[1] = 0x5A5A0002u;
        s_isr_send_ret = arkrtos_mq_send_from_isr(s_isr_mq, msg, 2);
    }
    else
    {
        uint32 out[2];
        out[0] = 0u;
        out[1] = 0u;
        s_isr_recv_ret = arkrtos_mq_recv_from_isr(s_isr_mq, out, 2);
        s_isr_recv_words[0] = out[0];
        s_isr_recv_words[1] = out[1];
    }
    s_isr_hits++;
}

/* EXTI0 的软件触发（不占引脚、不需要 EXTICR 映射）。
 * 触发前先把计数量归零，避免上一次的残留被当成本次证据。 */
static void isr_test_arm(void)
{
    s_isr_hits       = 0u;
    s_isr_in_handler = 0u;
    s_isr_send_ret   = -99;
    s_isr_recv_ret   = -99;
    s_isr_recv_words[0] = 0u;
    s_isr_recv_words[1] = 0u;

    EXTI->IMR |= 0x1u;                          /* 打开 line0 中断屏蔽 */
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI0_IRQn);
}

/* 返回 1 = handler 在自旋上限内跑过；0 = 超时（中断没进来） */
static uint8 isr_test_fire(void)
{
    uint32 i;

    EXTI->SWIER = 0x1u;                         /* 软件置位 line0 挂起 */
    __DSB();
    __ISB();                                    /* 让挂起立刻被取走 */

    for(i = 0u; i < ISR_WAIT_SPIN; i++)
    {
        if(s_isr_hits != 0u)
        {
            return 1u;
        }
        __NOP();
    }
    return 0u;
}

static void isr_test_disarm(void)
{
    NVIC_DisableIRQ(EXTI0_IRQn);
    EXTI->IMR    &= ~0x1u;
    EXTI->SWIER   = 0x1u;                       /* 吃掉可能残留的挂起 */
    EXTI->PR      = 0x1u;
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    s_isr_mode = 0u;
}

/* 把队列灌满（每条 1 字），返回成功塞进去的条数 */
static int32 isr_test_fill(void)
{
    int32 i;
    int32 ret = 0;

    for(i = 0; i < (int32)ISR_MQ_FILL; i++)
    {
        uint32 one = 0xC0DE0000u + (uint32)i;
        if(arkrtos_mq_send_internal(s_isr_mq, &one, 1, 0) != 0)
        {
            break;
        }
        ret++;
    }
    return ret;
}

/* 把队列抽空，返回取出来的条数（顺带核对内容，坏一条记一条账） */
static int32 isr_test_drain(void)
{
    int32 got;
    int32 n = 0;

    for(;;)
    {
        uint32 out[1];
        got = arkrtos_mq_recv_internal(s_isr_mq, out, 1, 0);
        if(got <= 0)
        {
            break;
        }
        n++;
    }
    return n;
}

UTEST_CASE(mq_from_isr_contract)
{
    uint32 out[4];
    int32  got;
    int32  n;
    uint32 i;

    s_isr_mq = arkrtos_mq_create_internal((char *)ISR_MQ_NAME);
    if(s_isr_mq < 0)
    {
        UTEST_FAIL("mq create rejected a legal queue (queue table full?)");
        s_isr_mq = -1;
        return;
    }

    isr_test_arm();

    /* ---- 1) ISR 里发：真的进了 handler，真的塞进了队列 ---- */
    s_isr_mode = 0u;
    {
        uint8 fired = isr_test_fire();
        UTEST_CHECK_EQ(fired, 1u);              /* 中断确实进来了 */
        UTEST_CHECK_EQ(s_isr_hits, 1u);         /* 而且只进来一次 */
        UTEST_CHECK_EQ(s_isr_in_handler, 1u);   /* 执行时 IPSR != 0 */
        UTEST_CHECK_EQ(s_isr_send_ret, 0);
    }

    out[0] = 0u; out[1] = 0u; out[2] = 0u; out[3] = 0u;
    got = arkrtos_mq_recv_internal(s_isr_mq, out, 4, 0);
    UTEST_CHECK_EQ(got, 2);                     /* 任务侧取到 2 个字 */
    UTEST_CHECK_EQ(out[0], 0xA5A50001u);
    UTEST_CHECK_EQ(out[1], 0x5A5A0002u);
    UTEST_CHECK_EQ(arkrtos_mq_recv_internal(s_isr_mq, out, 4, 0), -1);  /* 已空 */

    /* ---- 2) ISR 里收：队列有数据时返回字数与内容，且取走后真的空 ---- */
    {
        uint32 in[2];
        in[0] = 0x11112222u;
        in[1] = 0x33334444u;
        UTEST_CHECK_EQ(arkrtos_mq_send_internal(s_isr_mq, in, 2, 0), 0);

        s_isr_mode = 1u;
        UTEST_CHECK_NE(isr_test_fire(), 0u);
        UTEST_CHECK_EQ(s_isr_recv_ret, 2);
        UTEST_CHECK_EQ(s_isr_recv_words[0], 0x11112222u);
        UTEST_CHECK_EQ(s_isr_recv_words[1], 0x33334444u);
        UTEST_CHECK_EQ(arkrtos_mq_recv_internal(s_isr_mq, out, 4, 0), -1);
    }

    /* ---- 3) 队列空时 ISR 里收：返回 EMPTY(-1)，不登记、不阻塞 ---- */
    s_isr_mode = 1u;
    UTEST_CHECK_NE(isr_test_fire(), 0u);
    UTEST_CHECK_EQ(s_isr_recv_ret, ARKRTOS_MQ_ERR_EMPTY);
    UTEST_CHECK_EQ(s_isr_recv_ret, -1);

    /* ---- 4) 队列满时 ISR 里发：返回 FULL(-1)，旧数据一条不少 ---- */
    n = isr_test_fill();
    UTEST_CHECK_EQ(n, (int32)ISR_MQ_FILL);      /* 先证明真的满了 */
    s_isr_mode = 0u;
    UTEST_CHECK_NE(isr_test_fire(), 0u);
    UTEST_CHECK_EQ(s_isr_send_ret, ARKRTOS_MQ_ERR_FULL);
    UTEST_CHECK_EQ(s_isr_send_ret, -1);

    /* 满队列里那条「被拒」的消息不得挤掉任何旧数据：逐条核对内容 */
    n = 0;
    for(i = 0u; i < ISR_MQ_FILL; i++)
    {
        got = arkrtos_mq_recv_internal(s_isr_mq, out, 1, 0);
        if(got <= 0)
        {
            break;
        }
        UTEST_CHECK_EQ(out[0], 0xC0DE0000u + i);
        n++;
    }
    UTEST_CHECK_EQ(n, (int32)ISR_MQ_FILL);
    UTEST_CHECK_EQ(arkrtos_mq_recv_internal(s_isr_mq, out, 1, 0), -1);
    UTEST_CHECK_EQ(isr_test_drain(), 0);

    /* ---- 收尾：关中断源、删队列，让 utest run 可以反复跑 ---- */
    isr_test_disarm();
    {
        int32 dead = s_isr_mq;
        UTEST_CHECK_EQ(arkrtos_mq_delete_internal(dead), 0);
        s_isr_mq = -1;
        /* 删掉之后句柄不能再被当成活对象：任何一步都不许返回「成功」 */
        UTEST_CHECK_EQ(arkrtos_mq_is_alive(dead), 0);
        UTEST_CHECK(arkrtos_mq_recv_internal(dead, out, 1, 0) < 0);
        UTEST_CHECK_EQ(arkrtos_mq_delete_internal(dead), ARKRTOS_MQ_ERR_STATE);
    }
}

#endif /* (ARKRTOS_USE_UTEST == 1) && (ARKRTOS_USE_MQ == 1) */
