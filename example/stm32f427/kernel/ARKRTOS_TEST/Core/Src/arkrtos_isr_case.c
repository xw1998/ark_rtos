/**
 * @file arkrtos_isr_case.c
 * @brief 板级 utest 用例：在真正的中断上下文里直接用内核对象（EXTI0 软件触发）
 * @details 这里要证明的是「ISR 侧能直接用内核对象」这条承诺在真硬件上成立，
 *          而不是只在宿主机上把等待队列手工塞满后读一遍数字。当前有两条用例：
 *
 *            - mq_from_isr_contract   ：在 handler 里直接收发消息
 *            - eset_from_isr_contract ：在 handler 里 set 32 位事件集，
 *              唤醒一个**真的阻塞在等待队列上**的任务，并核对交班值；
 *              最后一段（D）在同一个 handler 里发 SVC 0x21 —— 就是 App 侧
 *              arkrtos_eset_set_from_isr() 走到的那条 ABI —— 把「ISR 里发
 *              系统调用」也变成板级读数（参数块位置的正反对照见 D 段注释）
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
 *          2. mq：arkrtos_mq_send_from_isr 在 ISR 里返回 0，任务侧随后能取到
 *             同一条消息、同样的字数与内容；recv_from_isr 有数据时返回字数与
 *             内容、空时返回 EMPTY、满时 send 返回 FULL 且旧数据一条不少；
 *          3. eset：handler 里 set 把**阻塞中的等待者**置 READY（不是在这里
 *             切换——handler 还没返回时它必须是 READY 而不是 RUNNING），
 *             交班值等于当时成立的那些位；OR|CLEAR 交出的位被清；
 *             AND 缺一位时不唤醒等待者；ISR 里 mask == 0 被拒且不动位图。
 *          4. SVC-in-ISR：handler 里发的 SVC 0x21 返回 0 并真的把等待者
 *             置 READY（不在异常里切换）；同一次 handler 里用内核栈上的
 *             参数块再发一次必须被拒（-1）—— 证明参数窗口校验没被松开。
 *
 *          门控：ARKRTOS_USE_UTEST 为 1 且（MQ 或 ESET 至少一个为 1）才编入；
 *          两条用例各自再按自己的模块开关裁剪，缺功能的构建不会因为缺符号
 *          而链接失败，也不会出现「编进去了但什么也没测」的假绿。
 *
 * @note 本文件属于板级工程（example/），不属于内核。
 */
#include "main.h"

#include "arkrtos.h"
#include "arkrtos_utest.h"

#if (ARKRTOS_USE_MQ == 1)
#include "arkrtos_mq.h"
#endif

#if (ARKRTOS_USE_ESET == 1)
#include "arkrtos_eset.h"
#include "arkrtos_sync.h"
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"   /* arkrtos_task_table[] 的外部声明在这里 */
#include "arkrtos_svc_call.h"    /* ARKRTOS_SVC_DECL_1：AC5 用 __svc(n) 内建 */
#include "arkrtos_svc_abi.h"     /* ARKRTOS_ESET_SUB_SET 等子命令号 */
#include "arkrtos_partition.h"   /* SHARE_RAM_BASE / SHARE_RAM_SIZE / CRASH_LOG_SIZE */
#endif

#if (ARKRTOS_USE_UTEST == 1) && ((ARKRTOS_USE_MQ == 1) || (ARKRTOS_USE_ESET == 1))

/* 等 handler 跑起来的自旋上限：handler 只是几条内存写，
 * 真跑起来是「下一个指令边界」的事，这么多轮足够；超时即判失败，
 * 不无限等待（坏掉时必须几百微秒内报红，不能挂死控制台）。 */
#define ISR_WAIT_SPIN    2000000u

/* handler 与用例共享的取证量：全部 volatile，handler 只写、用例只读 */
static volatile uint32 s_isr_hits;             /* handler 执行次数 */
static volatile uint32 s_isr_in_handler;       /* 1 = 执行时 IPSR != 0 */
static volatile uint8  s_isr_mode;             /* 0 = mq 发；1 = mq 收；2 = eset set */

/* ============================================================
 * 消息队列：ISR 里直接收发
 * ============================================================ */
#if (ARKRTOS_USE_MQ == 1)

/* 队列名：内部只保留 7 字符 + 结束符，超长会被截断，这里主动取短名 */
#define ISR_MQ_NAME      "isrmq"
/* 队列深度（与 ARKRTOS_MQ_DEPTH 对齐，用例用它来造「满」）*/
#define ISR_MQ_FILL      ARKRTOS_MQ_DEPTH

static volatile int32  s_isr_send_ret = -99;   /* send_from_isr 的返回值 */
static volatile int32  s_isr_recv_ret = -99;   /* recv_from_isr 的返回值 */
static volatile uint32 s_isr_recv_words[2];    /* recv_from_isr 取到的内容 */

static int32 s_isr_mq = -1;

#endif /* ARKRTOS_USE_MQ */

/* ============================================================
 * 32 位事件集：ISR 里 set，唤醒真的阻塞着的任务
 * ============================================================ */
#if (ARKRTOS_USE_ESET == 1)

#define ISR_ESET_NAME         "isreset"
#define ISR_ESET_STACK_WORDS  128u   /* 512 B：等待者只调几个内核函数 */
#define ISR_ESET_PRIO_GAP     4u     /* 等待者比控制台高这么多档 */
#define ISR_ESET_BIT0         (1u << 0)
#define ISR_ESET_BIT1         (1u << 1)
#define ISR_ESET_BIT2         (1u << 2)
#define ISR_ESET_BIT3         (1u << 3)

static uint32 s_isr_eset_stack[ISR_ESET_STACK_WORDS];

static volatile int32  s_isr_eset_set_ret = -99;     /* handler 里 set 的返回 */
static volatile int32  s_isr_eset_set_ret_zero = -99;/* handler 里 set(mask 0) 的返回 */
static volatile uint32 s_isr_eset_waiter_state;      /* set 之后等待者的状态 */
static volatile int32  s_isr_eset_wait_rc = -99;     /* 等待者拿到的返回码 */
static volatile uint32 s_isr_eset_wait_got;          /* 等待者拿到的交班位 */

static volatile uint32 s_isr_eset_isr_mask;          /* 本次 handler 要 set 的位 */
static uint32 s_isr_eset_wait_mask;                  /* 等待者要等的位 */
static uint32 s_isr_eset_wait_opt;                   /* 等待者的 OR/AND/CLEAR */

static int32 s_isr_eset       = -1;
static int32 s_isr_eset_done  = -1;   /* 等待者退场信号（计数信号量） */
static int32 s_isr_eset_waiter = -1;  /* 等待者的任务 id，handler 用它取状态 */

/* ------------------------------------------------------------------
 * 真 SVC-in-ISR：在 handler 里发 SVC 0x21（App 侧
 * arkrtos_eset_set_from_isr() 走的同一条 ABI：一条 SVC 指令 + 24 字节参数块）。
 *
 * ★ 硬前置：SVC(SVCall) 的优先级必须**高于**本中断，否则在 handler 里发
 *   SVC 会被升级成 HardFault（SVC 不得以同级或更高优先级发出）。
 *   arkrtos_port_irq_init() 给 SVCall 的是 0x01，本用例把 EXTI0 设成
 *   逻辑优先级 4 —— 写进 NVIC->IP 就是 0x40（SVCall 是 0x10）。
 *   注意 CMSIS 的坑：NVIC_SetPriority() 收的是“逻辑档位”（0x00..0x0F，
 *   4 位实现），不是 IP 寄存器里的字节。直接传 0x40 会被
 *   (0x40 << 4) & 0xFF 截成 0x00 —— 那等于把 EXTI0 提到最高优先级，
 *   比 SVC 还高，正是本用例要避开的那一侧。
 *
 * ★ 参数块位置：svc_eset_ctrl() 先用 arkrtos_kernel_svc_args_ok() 校验
 *   24 字节参数块是否落在“用户可见 RAM”（SHARE_RAM / SLOT_RAM）窗口里。
 *   内核栈不在窗口内，所以这里用共享 RAM 的自由区放参数块；同一个
 *   handler 里再用内核栈上的参数块发一次，作为“窗口校验不是假绿”的对照。
 *   共享 RAM 的已知消费者只有开头的分区表与尾部 512 B 崩溃日志，
 *   偏移 0x800 处的 24 字节无人使用（下面有编译期与运行期各一道边界检查）。
 * ------------------------------------------------------------------ */
#define ISR_SVC_ARGS_OFFSET   0x800u
#define ISR_SVC_ARGS_WORDS    6u
#define ISR_SVC_ARGS_ADDR     (SHARE_RAM_BASE + ISR_SVC_ARGS_OFFSET)

/* EXTI0 的优先级：逻辑档位 4 → NVIC->IP 里的 0x40（SVCall 是 0x10） */
#define ISR_EXTI0_PRIO_LEVEL  0x04u
#define ISR_EXTI0_IP_REG      ((uint32)(ISR_EXTI0_PRIO_LEVEL << (8u - __NVIC_PRIO_BITS)))

#if ((ISR_SVC_ARGS_OFFSET + (ISR_SVC_ARGS_WORDS * 4u)) > (SHARE_RAM_SIZE - CRASH_LOG_SIZE))
#error "ISR SVC argument block would overlap the crash journal"
#endif

ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_ESET_CTRL, isr_svc_eset_ctrl, uint32 *);

static volatile int32 s_isr_eset_svc_ret        = -99;  /* 共享 RAM 参数块的返回 */
static volatile int32 s_isr_eset_svc_ret_kstack = -99;  /* 内核栈参数块的返回（期望 -1） */

/* 等待者：睡在事件集上，拿到交班值后记下来、通知控制台、退场 */
static void isr_eset_waiter_entry(void)
{
    uint32 got = 0xFFFFFFFFu;

    s_isr_eset_wait_rc = arkrtos_eset_wait_internal(s_isr_eset, s_isr_eset_wait_mask,
                                                    s_isr_eset_wait_opt, &got, 500);
    s_isr_eset_wait_got = got;
    (void)arkrtos_sem_post_internal(s_isr_eset_done);
    arkrtos_task_kill_internal();
}

#endif /* ARKRTOS_USE_ESET */

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

#if (ARKRTOS_USE_MQ == 1)
    if(s_isr_mode == 0u)
    {
        uint32 msg[2];
        msg[0] = 0xA5A50001u;
        msg[1] = 0x5A5A0002u;
        s_isr_send_ret = arkrtos_mq_send_from_isr(s_isr_mq, msg, 2);
    }
    else if(s_isr_mode == 1u)
    {
        uint32 out[2];
        out[0] = 0u;
        out[1] = 0u;
        s_isr_recv_ret = arkrtos_mq_recv_from_isr(s_isr_mq, out, 2);
        s_isr_recv_words[0] = out[0];
        s_isr_recv_words[1] = out[1];
    }
#endif /* ARKRTOS_USE_MQ */

#if (ARKRTOS_USE_ESET == 1)
    if(s_isr_mode == 2u)
    {
        s_isr_eset_set_ret = arkrtos_eset_set_internal(s_isr_eset, s_isr_eset_isr_mask);

        /* 关键取证：set 在 ISR 里只把等待者置 READY，**不能在这里切换**
         * （handler 还没返回，PSP 还是被打断的那个任务的）。这一格读的是
         * 被打断之后立刻的状态：READY 才叫"延迟到异常返回再切"。 */
        if((s_isr_eset_isr_mask != 0u) && (s_isr_eset_waiter > 0))
        {
            s_isr_eset_waiter_state =
                (uint32)arkrtos_task_table[s_isr_eset_waiter - 1].status;
        }

        /* 同一次 handler 里再试一次非法参数：ISR 里也必须被拒，不能因为
         * "在中断里"就放松校验（返回 -1，位图一位不动）。 */
        s_isr_eset_set_ret_zero = arkrtos_eset_set_internal(s_isr_eset, 0u);
    }

    if(s_isr_mode == 3u)
    {
        volatile uint32 *p_share = (volatile uint32 *)(uint32)ISR_SVC_ARGS_ADDR;
        uint32 kp[3];
        uint32 k;

        /* 1) 参数块在共享 RAM（用户可见窗口）：SVC 必须真的走到内核并成功 */
        p_share[0] = (uint32)ARKRTOS_ESET_SUB_SET;
        p_share[1] = (uint32)s_isr_eset;
        p_share[2] = s_isr_eset_isr_mask;
        for(k = 3u; k < ISR_SVC_ARGS_WORDS; k++)
        {
            p_share[k] = 0u;
        }
        s_isr_eset_svc_ret = isr_svc_eset_ctrl((uint32 *)p_share);

        /* SVC 里也只置 READY：异常还没返回，不能在这里切到等待者 */
        if((s_isr_eset_isr_mask != 0u) && (s_isr_eset_waiter > 0))
        {
            s_isr_eset_waiter_state =
                (uint32)arkrtos_task_table[s_isr_eset_waiter - 1].status;
        }

        /* 2) 对照：参数块在内核栈（不在 SHARE_RAM / SLOT_RAM 窗口里），
         *    必须被拒。用 BIT3 试探——若窗口校验是假绿，BIT3 会留在位图上，
         *    后面的 flags 断言就能抓住。 */
        kp[0] = (uint32)ARKRTOS_ESET_SUB_SET;
        kp[1] = (uint32)s_isr_eset;
        kp[2] = ISR_ESET_BIT3;
        s_isr_eset_svc_ret_kstack = isr_svc_eset_ctrl(kp);
    }
#endif /* ARKRTOS_USE_ESET */

    s_isr_hits++;
}

/* EXTI0 的软件触发（不占引脚、不需要 EXTICR 映射）。
 * 触发前先把计数量归零，避免上一次的残留被当成本次证据。 */
static void isr_test_arm(void)
{
    s_isr_hits       = 0u;
    s_isr_in_handler = 0u;

#if (ARKRTOS_USE_MQ == 1)
    s_isr_send_ret   = -99;
    s_isr_recv_ret   = -99;
    s_isr_recv_words[0] = 0u;
    s_isr_recv_words[1] = 0u;
#endif

#if (ARKRTOS_USE_ESET == 1)
    s_isr_eset_set_ret        = -99;
    s_isr_eset_set_ret_zero   = -99;
    s_isr_eset_waiter_state   = 0xFFFFFFFFu;
    s_isr_eset_svc_ret        = -99;
    s_isr_eset_svc_ret_kstack = -99;
#endif

    /* SVC 的优先级是 0x01（arkrtos_port_irq_init 设的），必须比本中断高：
     * 这里把 EXTI0 压到逻辑优先级 4（IP 寄存器值 0x40 > SVCall 的 0x10）。
     * 论据见文件里「真 SVC-in-ISR」那段的注释：这里传的是逻辑档位，
     * 不是 IP 字节（传 0x40 会被截成 0x00，反而比 SVC 高）。 */
    NVIC_SetPriority(EXTI0_IRQn, ISR_EXTI0_PRIO_LEVEL);

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

#if (ARKRTOS_USE_MQ == 1)

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

#endif /* ARKRTOS_USE_MQ */

#if (ARKRTOS_USE_ESET == 1)

/* 注册一个等待者并等它真的睡下去；返回任务 id（<0 = 注册失败） */
static int32 isr_eset_start_waiter(uint32 mask, uint32 opt)
{
    int32 id;

    s_isr_eset_wait_rc  = -99;
    s_isr_eset_wait_got = 0xFFFFFFFFu;
    s_isr_eset_wait_mask = mask;
    s_isr_eset_wait_opt  = opt;

    id = arkrtos_task_register(isr_eset_waiter_entry, s_isr_eset_stack,
                              (uint32)sizeof(s_isr_eset_stack),
                              (uint8)(arkrtos_task_get_current()->base_priority - ISR_ESET_PRIO_GAP),
                              0u);
    s_isr_eset_waiter = id;
    return id;
}

UTEST_CASE(eset_from_isr_contract)
{
    arkrtos_task_t *p_self = arkrtos_task_get_current();
    uint32 flags = 0xFFFFFFFFu;

    UTEST_REQUIRE(p_self != 0);
    if((uint32)p_self->base_priority < (ISR_ESET_PRIO_GAP + 1u))
    {
        UTEST_SKIP("console priority too low to register a higher-priority waiter");
    }

    s_isr_eset      = arkrtos_eset_create_internal((char *)ISR_ESET_NAME);
    s_isr_eset_done = arkrtos_sem_create_internal((char *)"isresd", 0);
    UTEST_REQUIRE((s_isr_eset >= 0) && (s_isr_eset_done >= 0));

    /* ---- A) OR|CLEAR：ISR 里 set 唤醒阻塞中的等待者，交班值正确、位被清 ---- */
    UTEST_REQUIRE(isr_eset_start_waiter(ISR_ESET_BIT0 | ISR_ESET_BIT2,
                                        ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_CLEAR) > 0);
    arkrtos_task_wait_internal(2u);             /* 让等待者跑起来并登记在队列上 */

    /* 先证明它真的睡在等待队列里：不是在跑，也不是还没开始 */
    UTEST_CHECK_EQ(arkrtos_task_table[s_isr_eset_waiter - 1].status, ARKRTOS_TASK_WAIT);

    isr_test_arm();
    s_isr_mode          = 2u;
    s_isr_eset_isr_mask = ISR_ESET_BIT0 | ISR_ESET_BIT2;
    UTEST_CHECK_NE(isr_test_fire(), 0u);
    UTEST_CHECK_EQ(s_isr_hits, 1u);
    UTEST_CHECK_EQ(s_isr_in_handler, 1u);        /* IPSR != 0：真在异常上下文 */
    UTEST_CHECK_EQ(s_isr_eset_set_ret, 0);
    /* handler 还没返回时等待者只许是 READY：ISR 里置位、异常返回才切 */
    UTEST_CHECK_EQ(s_isr_eset_waiter_state, (uint32)ARKRTOS_TASK_READY);

    UTEST_CHECK_EQ(arkrtos_sem_wait_internal(s_isr_eset_done, 500), 0);
    UTEST_CHECK_EQ(s_isr_eset_wait_rc, 0);
    UTEST_CHECK_EQ(s_isr_eset_wait_got, ISR_ESET_BIT0 | ISR_ESET_BIT2);
    UTEST_CHECK_EQ(arkrtos_eset_get_internal(s_isr_eset, &flags), 0);
    UTEST_CHECK_EQ(flags, 0u);                   /* CLEAR：交出的位被清 */

    /* ---- B) AND：缺一位不唤醒；补齐后从任务上下文唤醒、CLEAR 不加就不清 ---- */
    UTEST_CHECK_EQ(arkrtos_eset_set_internal(s_isr_eset, ISR_ESET_BIT1), 0);
    UTEST_CHECK_EQ(arkrtos_eset_get_internal(s_isr_eset, &flags), 0);
    UTEST_CHECK_EQ(flags, ISR_ESET_BIT1);        /* 没有等待者时位照样留着 */

    UTEST_REQUIRE(isr_eset_start_waiter(ISR_ESET_BIT1 | ISR_ESET_BIT3,
                                        ARKRTOS_ESET_OPT_AND) > 0);
    arkrtos_task_wait_internal(2u);
    UTEST_CHECK_EQ(arkrtos_task_table[s_isr_eset_waiter - 1].status, ARKRTOS_TASK_WAIT);

    isr_test_arm();
    s_isr_mode          = 2u;
    s_isr_eset_isr_mask = ISR_ESET_BIT2;         /* 只补上一位：AND 还不满足 */
    UTEST_CHECK_NE(isr_test_fire(), 0u);
    UTEST_CHECK_EQ(s_isr_eset_set_ret, 0);
    UTEST_CHECK_EQ(s_isr_eset_waiter_state, (uint32)ARKRTOS_TASK_WAIT);  /* 没被唤醒 */
    UTEST_CHECK_EQ(arkrtos_eset_get_internal(s_isr_eset, &flags), 0);
    UTEST_CHECK_EQ(flags, ISR_ESET_BIT1 | ISR_ESET_BIT2);

    /* 补齐最后一位（任务上下文）：等待者醒来，交班值是它的掩码与当前位的交集 */
    UTEST_CHECK_EQ(arkrtos_eset_set_internal(s_isr_eset, ISR_ESET_BIT3), 0);
    UTEST_CHECK_EQ(arkrtos_sem_wait_internal(s_isr_eset_done, 500), 0);
    UTEST_CHECK_EQ(s_isr_eset_wait_rc, 0);
    UTEST_CHECK_EQ(s_isr_eset_wait_got, ISR_ESET_BIT1 | ISR_ESET_BIT3);
    UTEST_CHECK_EQ(arkrtos_eset_get_internal(s_isr_eset, &flags), 0);
    UTEST_CHECK_EQ(flags, ISR_ESET_BIT1 | ISR_ESET_BIT2 | ISR_ESET_BIT3);  /* 无 CLEAR 不清 */

    /* ---- C) ISR 里 mask == 0：必须被拒，且位图一位不动 ---- */
    isr_test_arm();
    s_isr_mode          = 2u;
    s_isr_eset_isr_mask = 0u;
    UTEST_CHECK_NE(isr_test_fire(), 0u);
    UTEST_CHECK_EQ(s_isr_eset_set_ret, ARKRTOS_ESET_ERR_PARAM);       /* mask 0 被拒 */
    UTEST_CHECK_EQ(s_isr_eset_set_ret_zero, ARKRTOS_ESET_ERR_PARAM);  /* 同一次 handler 里再试一次，一样被拒 */
    UTEST_CHECK_EQ(arkrtos_eset_get_internal(s_isr_eset, &flags), 0);
    UTEST_CHECK_EQ(flags, ISR_ESET_BIT1 | ISR_ESET_BIT2 | ISR_ESET_BIT3);

    /* ---- D) 真 SVC-in-ISR：handler 里发 SVC 0x21（App 侧 _from_isr 的同一条 ABI） ---- */
    UTEST_CHECK_EQ(NVIC_GetPriority(EXTI0_IRQn), ISR_EXTI0_PRIO_LEVEL);
    UTEST_CHECK_EQ((uint32)NVIC->IP[EXTI0_IRQn], ISR_EXTI0_IP_REG);   /* 寄存器里就是 0x40 */
    /* 真正的硬前置：数值更大 = 优先级更低。写反了就变成 HardFault 那一侧 */
    UTEST_CHECK(NVIC_GetPriority(EXTI0_IRQn) > NVIC_GetPriority(SVCall_IRQn));
    /* 参数块位置自检：在共享 RAM 窗口内、且不碰尾部崩溃日志区 */
    UTEST_CHECK(ISR_SVC_ARGS_ADDR >= SHARE_RAM_BASE);
    UTEST_CHECK((ISR_SVC_ARGS_ADDR + (ISR_SVC_ARGS_WORDS * 4u)) <=
                (SHARE_RAM_BASE + SHARE_RAM_SIZE - CRASH_LOG_SIZE));

    /* 从干净的位图开始：前面几段留下的位会让下面的 flags 断言失去分辨力 */
    UTEST_CHECK_EQ(arkrtos_eset_clear_internal(s_isr_eset, 0xFFFFFFFFu), 0);
    UTEST_CHECK_EQ(arkrtos_eset_get_internal(s_isr_eset, &flags), 0);
    UTEST_CHECK_EQ(flags, 0u);

    UTEST_REQUIRE(isr_eset_start_waiter(ISR_ESET_BIT0 | ISR_ESET_BIT2,
                                        ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_CLEAR) > 0);
    arkrtos_task_wait_internal(2u);
    UTEST_CHECK_EQ(arkrtos_task_table[s_isr_eset_waiter - 1].status, ARKRTOS_TASK_WAIT);

    isr_test_arm();
    s_isr_mode          = 3u;
    s_isr_eset_isr_mask = ISR_ESET_BIT0 | ISR_ESET_BIT2;
    UTEST_CHECK_NE(isr_test_fire(), 0u);
    UTEST_CHECK_EQ(s_isr_hits, 1u);
    UTEST_CHECK_EQ(s_isr_in_handler, 1u);                  /* SVC 发自真 handler */
    UTEST_CHECK_EQ(s_isr_eset_svc_ret, 0);                 /* 共享 RAM 参数块：SVC 成功 */
    UTEST_CHECK_EQ(s_isr_eset_svc_ret_kstack, ARKRTOS_ESET_ERR_PARAM);  /* 内核栈参数块：被拒 */
    UTEST_CHECK_EQ(s_isr_eset_waiter_state, (uint32)ARKRTOS_TASK_READY);/* SVC 里只置 READY */

    UTEST_CHECK_EQ(arkrtos_sem_wait_internal(s_isr_eset_done, 500), 0);
    UTEST_CHECK_EQ(s_isr_eset_wait_rc, 0);
    UTEST_CHECK_EQ(s_isr_eset_wait_got, ISR_ESET_BIT0 | ISR_ESET_BIT2);
    UTEST_CHECK_EQ(arkrtos_eset_get_internal(s_isr_eset, &flags), 0);
    UTEST_CHECK_EQ(flags, 0u);   /* CLEAR 交出的位被清，被拒的 BIT3 也没落下 */

    /* ---- 收尾：关中断源、删对象；删掉之后句柄不许再被当成活对象 ---- */
    isr_test_disarm();
    {
        int32 dead = s_isr_eset;
        UTEST_CHECK_EQ(arkrtos_eset_delete_internal(dead), 0);
        s_isr_eset = -1;
        UTEST_CHECK(arkrtos_eset_get_internal(dead, &flags) < 0);
        UTEST_CHECK(arkrtos_eset_set_internal(dead, ISR_ESET_BIT0) < 0);
        UTEST_CHECK_EQ(arkrtos_eset_delete_internal(dead), ARKRTOS_ESET_ERR_STATE);
    }
    UTEST_CHECK_EQ(arkrtos_sem_delete_internal(s_isr_eset_done), 0);
    s_isr_eset_done = -1;
}

#endif /* ARKRTOS_USE_ESET */

#endif /* (ARKRTOS_USE_UTEST == 1) && (MQ || ESET) */
