/**
* @file test_sbuf_host.c
* @brief 流缓冲 / 消息缓冲的宿主单元测试（不依赖板子、不依赖调度器）
* @details 为什么需要它：这个模块的绝大多数缺陷（回绕少算一个字节、
*          消息长度头错位、放不下时留半条）都在"字节搬运"里，而不是在
*          阻塞/唤醒里。宿主上把调度器换成桩，就能把这段搬运逻辑真正跑一遍，
*          而不是只保证"能编过"。
*
*          覆盖范围（如实说明）：
*            已验证：带回绕的流读写、部分成功语义、NOSPC、空读超时、
*                    消息整条收发、消息过小丢弃后不串下一条、peek 长度、
*                    删除后句柄失效；以及阻塞收发六类收尾——登记后无法阻塞、
*                    被唤醒但资源没到手（重新排队到超时）、被唤醒且资源到手
*                    （成功且保持先进先出）、剩余时间用尽、等待期间对象被删除、
*                    用户态轮询（只登记不睡）（第 12..15 段，可编程桩）；
*                    以及第 16 段把模块推到行覆盖 100%——模块入口与对象表耗尽、
*                    越界句柄解析、任务拆除时登记被摘净（含表满空的槽）、
*                    四张等待表各自登记满、timeout_ms<0 的无限等待、坏长度头、
*                    peek 跨回绕、ISR 侧流写/消息写（含部分成功与满仓拒绝）。
*            未验证：真睡下与真被 ISR 唤醒、真节拍推进、真超时——桩只回答
*                    「唤醒原因给成这样时代码走哪条路」，不回答「调度器会不会
*                    按时唤醒它」。那部分只有上板用例能证明；另外第 16 段里的
*                    ISR 入口是宿主直接调用、ENTER_CRITICAL 是桩，
*                    ISR 屏蔽与抢占行为同样只有上板能证明。
* @note 用法：由 tools/sbuf_host_gate.py 编译并运行，输出 SBUF-HOST OK。
*/

#include "arkrtos_sbuf.h"
#include "arkrtos_cfg.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 内核桩：只提供链接所需的符号
 * ------------------------------------------------------------------ */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32  arkrtos_task_count     = 0;
int32  arkrtos_current_task_id = 0;
uint32 arkrtos_kernel_tick    = 0u;

void arkrtos_port_disable_irq(void) { }
void arkrtos_port_enable_irq(void)  { }
void arkrtos_port_switch_task(void) { }
uint32 arkrtos_port_enter_critical(void) { return 0u; }
void arkrtos_port_exit_critical(uint32 state) { (void)state; }

/* 可编程的阻塞桩。默认值与拆分前逐字一致（当前任务为 0、阻塞恒失败、
 * 不在 handler 里），因此第 1..11 段（非阻塞路径）的走向完全没变；
 * 只有第 12..15 段才临时打开 g_block_mode。
 *   mode 0 = 恒失败（默认，等价于原来那个 return -1 的桩）
 *   mode 2 = 第一次"令牌交接"、第二次超时
 *   mode 3 = 先从 g_block_free_handle 收走 g_block_free_len 字节腾出空间，再交接
 *   mode 4 = 交接前把节拍推过超时上限
 *   mode 5 = 返回"等待对象被删除"
 *   mode 7 = 阻塞期间往 g_block_free_handle 写 g_feed 的前 g_feed_len 字节，再交接
 *   mode 8 = 第一次只写 g_feed_split 字节就交接，第二次才把剩下的写完
 *            （用来造"一条消息还没到齐"，读者不许被放走） */
static arkrtos_task_t  g_fake_task;
static arkrtos_task_t *g_cur_task         = 0;
static int32  g_block_mode       = 0;
static int32  g_block_steps      = 0;
static int32  g_block_free_handle = -1;
static int32  g_block_free_len    = 0;
static uint8  g_in_handler        = 0u;
static int    g_ready_count       = 0;
static uint8  g_feed[64];
static int32  g_feed_len          = 0;
static int32  g_feed_split        = 0;

arkrtos_task_t *arkrtos_task_get_current(void) { return g_cur_task; }

int32 arkrtos_task_block_in_critical(uint32 timeout_ms)
{
    (void)timeout_ms;
    g_block_steps++;

    if(g_block_mode == 2)
    {
        return (g_block_steps == 1) ? ARKRTOS_WAKE_HANDOFF : ARKRTOS_WAKE_TIMEOUT;
    }
    if(g_block_mode == 3)
    {
        uint8 sink[32];
        int32 left = g_block_free_len;

        while(left > 0)
        {
            int32 n = (left > (int32)sizeof(sink)) ? (int32)sizeof(sink) : left;

            if(arkrtos_sbuf_recv_internal(g_block_free_handle, sink, n, 0) <= 0)
            {
                break;
            }
            left -= n;
        }
        return ARKRTOS_WAKE_HANDOFF;
    }
    if(g_block_mode == 4)
    {
        arkrtos_kernel_tick += 5000u;      /* 睡过头：剩余时间已用尽 */
        return ARKRTOS_WAKE_HANDOFF;
    }
    if(g_block_mode == 5)
    {
        return ARKRTOS_WAKE_OBJ_DELETED;
    }
    if(g_block_mode == 7)
    {
        /* 模拟"对端在阻塞期间写入" */
        (void)arkrtos_sbuf_send_internal(g_block_free_handle, g_feed, g_feed_len, 0);
        return ARKRTOS_WAKE_HANDOFF;
    }
    if(g_block_mode == 8)
    {
        /* 先写半条，第二次才补齐：读者必须重新排队而不是拿着半条走人 */
        if(g_block_steps == 1)
        {
            (void)arkrtos_sbuf_send_internal(g_block_free_handle, g_feed, g_feed_split, 0);
        }
        else
        {
            (void)arkrtos_sbuf_send_internal(g_block_free_handle, g_feed + g_feed_split,
                                             g_feed_len - g_feed_split, 0);
        }
        return ARKRTOS_WAKE_HANDOFF;
    }
    return -1;
}
void  arkrtos_ready_add(int32 task_idx) { (void)task_idx; g_ready_count++; }
uint8 arkrtos_sync_in_handler(void) { return g_in_handler; }

/* trace 模块：宿主上没有这条链路 */
/* 桩也是接口：先声明后定义，-Wmissing-prototypes 才看得见原型。 */
void arkrtos_trace_wait_obj(uint16 obj13);
void arkrtos_trace_wait(uint8 task_id);
void mdk_trace_arkrtos_obj_signal(uint16 obj, uint8 cls);
void arkrtos_trace_wait_obj(uint16 obj13) { (void)obj13; }
void arkrtos_trace_wait(uint8 task_id) { (void)task_id; }
void mdk_trace_arkrtos_obj_signal(uint16 obj, uint8 cls) { (void)obj; (void)cls; }

/* ------------------------------------------------------------------
 * 测试脚手架
 * ------------------------------------------------------------------ */
static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fail++;
        fputs("  FAIL: ", stdout);
        fputs(what, stdout);
        fputc('\n', stdout);
    }
}

/* 把流缓冲填到一滴不剩；填的内容是 0,1,2,...，用来判先进先出 */
static void fill_stream(int32 h)
{
    uint8 fill[ARKRTOS_SBUF_BUF_SIZE];
    int32 i;

    for(i = 0; i < (int32)ARKRTOS_SBUF_BUF_SIZE; i++)
    {
        fill[i] = (uint8)(i & 0xff);
    }
    check(arkrtos_sbuf_send_internal(h, fill, (int32)ARKRTOS_SBUF_BUF_SIZE, 0)
          == (int32)ARKRTOS_SBUF_BUF_SIZE, "the buffer is filled to the brim");
    check(arkrtos_sbuf_free_internal(h) == 0, "a full buffer reports zero room");
}

/* 每次进新用例前重置桩的计数，并把假任务摆成"真的在等"的样子，
 * 这样 wake_one 往外叫人的动作就能被 g_ready_count 看见 */
static void arm_block(int32 mode)
{
    g_block_mode       = mode;
    g_block_steps      = 0;
    g_ready_count      = 0;
    g_fake_task.status = ARKRTOS_TASK_WAIT;
}

/* 把一条消息拼成"长度头 + 载荷"的裸字节（mode 7/8 直接往缓冲里灌） */
static int32 build_msg_frame(uint8 *dst, const char *body, int32 len)
{
    int32 i;

    for(i = 0; i < (int32)ARKRTOS_MBUF_HDR_BYTES; i++)
    {
        dst[i] = (uint8)(((uint32)len >> (8 * i)) & 0xffu);
    }
    memcpy(dst + ARKRTOS_MBUF_HDR_BYTES, body, (size_t)len);
    return len + (int32)ARKRTOS_MBUF_HDR_BYTES;
}

/* ---- 12. 流语义·写者阻塞 ---- */
static void case_stream_send_blocking(void)
{
    g_cur_task          = 0;
    g_block_mode        = 0;
    g_block_steps       = 0;
    g_in_handler        = 0u;
    g_ready_count       = 0;
    g_block_free_handle = -1;

    uint8 out[ARKRTOS_SBUF_BUF_SIZE];
    uint8 x[10];
    int32 h;
    int32 n;
    int32 i;

    h = arkrtos_sbuf_create_internal("s1");
    check(h >= 0, "a stream buffer is created for the blocking send paths");
    fill_stream(h);

    for(i = 0; i < 10; i++)
    {
        x[i] = (uint8)(0xa0 + i);
    }

    /* A. 没有当前任务：没有可睡的实体，按参数错误收尾（不是超时——没人会来叫醒） */
    n = arkrtos_sbuf_send_internal(h, "x", 1, 100);
    check(n == ARKRTOS_SBUF_ERR_PARAM, "a blocking send without a current task is refused");

    /* B. 有任务但进不了阻塞（阻塞调用返回负值）：撤销登记、按上下文错误收尾。
     *    连做 5 次结论必须一样——若登记没撤销，第二次起就是另一条路。 */
    g_cur_task    = &g_fake_task;
    arm_block(0);
    for(i = 0; i < 5; i++)
    {
        n = arkrtos_sbuf_send_internal(h, "x", 1, 100);
        check(n == ARKRTOS_SBUF_ERR_PARAM, "a send that cannot block is refused");
    }
    check(g_block_steps == 5, "every attempt really reached the blocking call");
    check(arkrtos_sbuf_free_internal(h) == 0, "the refused sends stored nothing");

    /* C. 被唤醒时空间又被别人占走：按剩余时间重新排队，最终超时 */
    arm_block(2);
    n = arkrtos_sbuf_send_internal(h, "x", 1, 100);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "a wake-up that finds no room times out");
    check(g_block_steps == 2, "the sender re-queued itself exactly once");

    /* D. 被唤醒时空间已腾出：写进去，且原有字节的顺序不变 */
    arm_block(3);
    g_block_free_handle = h;
    g_block_free_len    = 20;
    n = arkrtos_sbuf_send_internal(h, x, 10, 100);
    check(n == 10, "a wake-up with room delivers the bytes");
    check(g_block_steps == 1, "the sender blocked exactly once");
    check(g_ready_count == 1, "the drain really woke the registered writer");
    check(arkrtos_sbuf_free_internal(h) == 10, "exactly the drained room was reused");
    n = arkrtos_sbuf_recv_internal(h, out, (int32)sizeof(out), 0);
    check(n == (int32)ARKRTOS_SBUF_BUF_SIZE - 10, "the whole stream comes back out");
    check(out[0] == 20, "the stream starts right after the drained head");
    check(out[ARKRTOS_SBUF_BUF_SIZE - 21] == 255, "the old tail is still there");
    check(out[ARKRTOS_SBUF_BUF_SIZE - 20] == 0xa0, "the newly written bytes follow the old tail");
    check(out[ARKRTOS_SBUF_BUF_SIZE - 11] == 0xa9, "the newly written bytes keep their order");

    /* D2. 醒来只腾出一部分空间：流语义下就是部分成功，不是失败 */
    fill_stream(h);
    arm_block(3);
    g_block_free_len = 5;
    n = arkrtos_sbuf_send_internal(h, x, 10, 100);
    check(n == 5, "a wake-up with only some room reports the bytes that fit");
    check(g_block_steps == 1, "the partial success still happened after one block");
    check(arkrtos_sbuf_free_internal(h) == 0, "the buffer is full again after the partial write");

    /* E. 剩余时间已用尽：不再排队，直接报超时 */
    arm_block(4);
    n = arkrtos_sbuf_send_internal(h, "x", 1, 100);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "an exhausted deadline ends the retry loop");
    check(g_block_steps == 1, "no second block happens once the deadline is gone");
    arkrtos_kernel_tick = 0u;

    /* F. 等待期间缓冲被删除：必须报"对象已删除"，不是"参数错了" */
    arm_block(5);
    n = arkrtos_sbuf_send_internal(h, "x", 1, 100);
    check(n == ARKRTOS_SBUF_ERR_DELETED, "a buffer deleted while waiting fails the send as DELETED");

    /* G. 用户态轮询（处在 handler 里）：只登记不睡，报 would-block */
    g_in_handler = 1u;
    arm_block(6);
    n = arkrtos_sbuf_send_internal(h, "x", 1, 100);
    check(n == ARKRTOS_SYNC_ERR_WOULDBLOCK, "a user-mode send reports would-block");
    check(g_block_steps == 0, "a user-mode send never sleeps");
    g_in_handler = 0u;

    /* H. 那条登记是留给用户态轮询用的，必须真的存在；
     *    而 timeout 0 的收尾必须把它撤干净——两头都要看得见。 */
    n = arkrtos_sbuf_send_internal(h, "x", 1, 0);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "a non-blocking send on a full buffer times out");
    arm_block(0);
    (void)arkrtos_sbuf_reset_internal(h);
    check(g_ready_count == 0, "the timeout-0 send removed its stale registration");

    fill_stream(h);
    g_in_handler = 1u;
    arm_block(6);
    (void)arkrtos_sbuf_send_internal(h, "x", 1, 100);
    g_in_handler = 0u;
    arm_block(0);
    (void)arkrtos_sbuf_reset_internal(h);
    check(g_ready_count == 1, "the registration left by the user-mode poll is really there");

    /* 收尸：没有登记残留，删除时不该叫醒任何人 */
    fill_stream(h);
    arm_block(0);
    (void)arkrtos_sbuf_send_internal(h, "x", 1, 100);
    arm_block(0);
    n = arkrtos_sbuf_delete_internal(h);
    check(n == ARKRTOS_SBUF_OK, "the blocking-path stream buffer deletes cleanly");
    check(g_ready_count == 0, "no waiter was left behind in the buffer");
}

/* ---- 13. 流语义·读者阻塞 ---- */
static void case_stream_recv_blocking(void)
{
    g_cur_task          = 0;
    g_block_mode        = 0;
    g_block_steps       = 0;
    g_in_handler        = 0u;
    g_ready_count       = 0;
    g_block_free_handle = -1;

    uint8 out[ARKRTOS_SBUF_BUF_SIZE];
    int32 h;
    int32 n;
    int32 i;

    h = arkrtos_sbuf_create_internal("s2");
    check(h >= 0, "a second stream buffer is created for the blocking receive paths");

    /* A. 空且不等 vs 空且要等但没有当前任务 */
    n = arkrtos_sbuf_recv_internal(h, out, 4, 0);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "an empty buffer refuses a non-blocking receive");
    n = arkrtos_sbuf_recv_internal(h, out, 4, 100);
    check(n == ARKRTOS_SBUF_ERR_PARAM, "a blocking receive without a current task is refused");

    /* B. 进不了阻塞：撤销登记、按上下文错误收尾，连做 5 次结论一样 */
    g_cur_task = &g_fake_task;
    arm_block(0);
    for(i = 0; i < 5; i++)
    {
        n = arkrtos_sbuf_recv_internal(h, out, 4, 100);
        check(n == ARKRTOS_SBUF_ERR_PARAM, "a receive that cannot block is refused");
    }
    check(g_block_steps == 5, "every receive attempt really reached the blocking call");
    check(arkrtos_sbuf_used_internal(h) == 0, "the refused receives consumed nothing");

    /* C. 被唤醒时仍然没有数据：重新排队，最终超时 */
    arm_block(2);
    n = arkrtos_sbuf_recv_internal(h, out, 4, 100);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "a wake-up that finds no data times out");
    check(g_block_steps == 2, "the reader re-queued itself exactly once");

    /* D. 被唤醒时数据已到：取走它，且只睡了一次 */
    arm_block(7);
    g_block_free_handle = h;
    memcpy(g_feed, "abcd", 4);
    g_feed_len = 4;
    n = arkrtos_sbuf_recv_internal(h, out, (int32)sizeof(out), 100);
    check(n == 4, "a wake-up with data returns that data");
    check(memcmp(out, "abcd", 4) == 0, "the delivered bytes are the ones the writer put in");
    check(g_block_steps == 1, "the reader blocked exactly once");
    check(g_ready_count == 1, "the writer's wake reached the registered reader");
    g_block_free_handle = -1;

    /* E. 剩余时间已用尽：不再排队 */
    arm_block(4);
    n = arkrtos_sbuf_recv_internal(h, out, 4, 100);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "an exhausted deadline ends the receive retry loop");
    check(g_block_steps == 1, "no second block happens once the receive deadline is gone");
    arkrtos_kernel_tick = 0u;

    /* F. 等待期间缓冲被删除 */
    arm_block(5);
    n = arkrtos_sbuf_recv_internal(h, out, 4, 100);
    check(n == ARKRTOS_SBUF_ERR_DELETED, "a buffer deleted while waiting fails the receive as DELETED");

    /* G. 用户态轮询：只登记不睡 */
    g_in_handler = 1u;
    arm_block(6);
    n = arkrtos_sbuf_recv_internal(h, out, 4, 100);
    check(n == ARKRTOS_SYNC_ERR_WOULDBLOCK, "a user-mode receive reports would-block");
    check(g_block_steps == 0, "a user-mode receive never sleeps");
    g_in_handler = 0u;

    /* H. 登记的存与撤：timeout 0 撤掉之后，写入不该再叫醒任何人；
     *    再走一遍用户态轮询，写入就必须叫醒它。 */
    n = arkrtos_sbuf_recv_internal(h, out, 4, 0);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "a non-blocking receive on an empty buffer times out");
    arm_block(0);
    (void)arkrtos_sbuf_send_internal(h, "xy", 2, 0);
    check(g_ready_count == 0, "the timeout-0 receive removed its stale registration");
    (void)arkrtos_sbuf_recv_internal(h, out, (int32)sizeof(out), 0);

    g_in_handler = 1u;
    arm_block(6);
    (void)arkrtos_sbuf_recv_internal(h, out, 4, 100);
    g_in_handler = 0u;
    arm_block(0);
    (void)arkrtos_sbuf_send_internal(h, "xy", 2, 0);
    check(g_ready_count == 1, "the registration left by the user-mode poll is really there");
    (void)arkrtos_sbuf_recv_internal(h, out, (int32)sizeof(out), 0);

    arm_block(0);
    n = arkrtos_sbuf_delete_internal(h);
    check(n == ARKRTOS_SBUF_OK, "the blocking-receive stream buffer deletes cleanly");
    check(g_ready_count == 0, "no reader was left behind in the buffer");
}

/* ---- 14. 消息语义·发送者阻塞 ---- */
static void case_mbuf_send_blocking(void)
{
    g_cur_task          = 0;
    g_block_mode        = 0;
    g_block_steps       = 0;
    g_in_handler        = 0u;
    g_ready_count       = 0;
    g_block_free_handle = -1;

    uint8 msg[ARKRTOS_SBUF_BUF_SIZE];
    uint8 out[64];
    int32 payload = (int32)ARKRTOS_SBUF_BUF_SIZE - (int32)ARKRTOS_MBUF_HDR_BYTES;
    int32 h;
    int32 n;
    int32 i;

    h = arkrtos_sbuf_create_internal("m1");
    check(h >= 0, "a message buffer is created for the blocking send paths");
    for(i = 0; i < (int32)sizeof(msg); i++)
    {
        msg[i] = (uint8)(0x30 + (i % 60));
    }
    n = arkrtos_mbuf_send_internal(h, msg, payload, 0);
    check(n == ARKRTOS_SBUF_OK, "one message that exactly fills the buffer is accepted");
    check(arkrtos_sbuf_free_internal(h) == 0, "the message left no room at all");

    /* A. 没有当前任务 */
    n = arkrtos_mbuf_send_internal(h, "hi", 2, 100);
    check(n == ARKRTOS_SBUF_ERR_PARAM, "a blocking message send without a current task is refused");

    /* B. 进不了阻塞 */
    g_cur_task = &g_fake_task;
    arm_block(0);
    for(i = 0; i < 3; i++)
    {
        n = arkrtos_mbuf_send_internal(h, "hi", 2, 100);
        check(n == ARKRTOS_SBUF_ERR_PARAM, "a message send that cannot block is refused");
    }
    check(g_block_steps == 3, "every message send attempt reached the blocking call");
    check(arkrtos_sbuf_free_internal(h) == 0, "the refused message sends stored nothing");

    /* C. 醒来时空间还是不够：整条消息一个字节都不许留下，重新排队到超时 */
    arm_block(2);
    n = arkrtos_mbuf_send_internal(h, "hi", 2, 100);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "a wake-up that still has no room times out");
    check(g_block_steps == 2, "the message sender re-queued itself exactly once");
    check(arkrtos_sbuf_free_internal(h) == 0, "the timed-out message left no half frame behind");

    /* D. 醒来时空间够了：整条写进去（消息语义下不能部分成功） */
    arm_block(3);
    g_block_free_handle = h;
    g_block_free_len    = 8;
    n = arkrtos_mbuf_send_internal(h, "hi", 2, 100);
    check(n == ARKRTOS_SBUF_OK, "a wake-up with enough room accepts the whole message");
    check(g_block_steps == 1, "the message sender blocked exactly once");
    check(arkrtos_sbuf_free_internal(h) == 2, "exactly the room the message needed was used");
    g_block_free_handle = -1;

    /* 事件之后对象仍然健康：清空、正常收发一条 */
    check(arkrtos_sbuf_reset_internal(h) == ARKRTOS_SBUF_OK, "the buffer resets after the blocking cases");
    check(arkrtos_mbuf_send_internal(h, "ok", 2, 0) == ARKRTOS_SBUF_OK, "a plain message send still works");
    n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 0);
    check((n == 2) && (memcmp(out, "ok", 2) == 0), "and it comes back intact");

    /* E. 填满后再测超时与删除 */
    check(arkrtos_mbuf_send_internal(h, msg, payload, 0) == ARKRTOS_SBUF_OK, "the buffer is filled again");
    arm_block(4);
    n = arkrtos_mbuf_send_internal(h, "hi", 2, 100);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "an exhausted deadline ends the message send retry loop");
    check(g_block_steps == 1, "no second block happens once the message deadline is gone");
    arkrtos_kernel_tick = 0u;

    arm_block(5);
    n = arkrtos_mbuf_send_internal(h, "hi", 2, 100);
    check(n == ARKRTOS_SBUF_ERR_DELETED, "a buffer deleted while waiting fails the message send as DELETED");

    g_in_handler = 1u;
    arm_block(6);
    n = arkrtos_mbuf_send_internal(h, "hi", 2, 100);
    check(n == ARKRTOS_SYNC_ERR_WOULDBLOCK, "a user-mode message send reports would-block");
    check(g_block_steps == 0, "a user-mode message send never sleeps");
    g_in_handler = 0u;

    n = arkrtos_mbuf_send_internal(h, "hi", 2, 0);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "a non-blocking message send on a full buffer times out");
    arm_block(0);
    (void)arkrtos_sbuf_reset_internal(h);
    check(g_ready_count == 0, "the timeout-0 message send removed its stale registration");

    arm_block(0);
    n = arkrtos_sbuf_delete_internal(h);
    check(n == ARKRTOS_SBUF_OK, "the blocking-path message buffer deletes cleanly");
    check(g_ready_count == 0, "no message writer was left behind in the buffer");
}

/* ---- 15. 消息语义·接收者阻塞 ---- */
static void case_mbuf_recv_blocking(void)
{
    g_cur_task          = 0;
    g_block_mode        = 0;
    g_block_steps       = 0;
    g_in_handler        = 0u;
    g_ready_count       = 0;
    g_block_free_handle = -1;

    uint8 out[64];
    int32 h;
    int32 n;
    int32 i;

    h = arkrtos_sbuf_create_internal("m2");
    check(h >= 0, "a second message buffer is created for the blocking receive paths");

    /* A. 空且不等 / 空且要等但没有当前任务 */
    n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 0);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "an empty message buffer refuses a non-blocking receive");
    n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 100);
    check(n == ARKRTOS_SBUF_ERR_PARAM, "a blocking message receive without a current task is refused");

    /* B. 进不了阻塞 */
    g_cur_task = &g_fake_task;
    arm_block(0);
    for(i = 0; i < 3; i++)
    {
        n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 100);
        check(n == ARKRTOS_SBUF_ERR_PARAM, "a message receive that cannot block is refused");
    }
    check(g_block_steps == 3, "every message receive attempt reached the blocking call");
    check(arkrtos_sbuf_used_internal(h) == 0, "the refused message receives consumed nothing");

    /* C. 醒来时仍然没有消息：重新排队到超时 */
    arm_block(2);
    n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 100);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "a wake-up that finds no message times out");
    check(g_block_steps == 2, "the message reader re-queued itself exactly once");

    /* D. 醒来时整条消息已到：取走它 */
    g_feed_len = build_msg_frame(g_feed, "hello", 5);
    arm_block(7);
    g_block_free_handle = h;
    n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 100);
    check(n == 5, "a wake-up with a whole message returns its length");
    check(memcmp(out, "hello", 5) == 0, "the message body is intact");
    check(g_block_steps == 1, "the message reader blocked exactly once");
    g_block_free_handle = -1;

    /* E. 只到了半条消息：读者必须重新排队，等齐了才放它走 */
    g_feed_len = build_msg_frame(g_feed, "world", 5);
    g_feed_split = (int32)ARKRTOS_MBUF_HDR_BYTES + 2;   /* 头 + 两个字 */
    arm_block(8);
    g_block_free_handle = h;
    n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 100);
    check(n == 5, "a half message does not satisfy the reader; the finished one does");
    check(memcmp(out, "world", 5) == 0, "the message assembled across two wake-ups is intact");
    check(g_block_steps == 2, "the reader went back to sleep once for the half message");
    g_block_free_handle = -1;

    /* F. 剩余时间已用尽 */
    arm_block(4);
    n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 100);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "an exhausted deadline ends the message receive loop");
    check(g_block_steps == 1, "no second block happens once the message receive deadline is gone");
    arkrtos_kernel_tick = 0u;

    /* G. 等待期间缓冲被删除 */
    arm_block(5);
    n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 100);
    check(n == ARKRTOS_SBUF_ERR_DELETED, "a buffer deleted while waiting fails the message receive as DELETED");

    /* H. 用户态轮询 */
    g_in_handler = 1u;
    arm_block(6);
    n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 100);
    check(n == ARKRTOS_SYNC_ERR_WOULDBLOCK, "a user-mode message receive reports would-block");
    check(g_block_steps == 0, "a user-mode message receive never sleeps");
    g_in_handler = 0u;

    /* I. 登记的撤与存 */
    n = arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 0);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "a non-blocking message receive on an empty buffer times out");
    arm_block(0);
    (void)arkrtos_mbuf_send_internal(h, "ab", 2, 0);
    check(g_ready_count == 0, "the timeout-0 message receive removed its stale registration");
    (void)arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 0);

    g_in_handler = 1u;
    arm_block(6);
    (void)arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 100);
    g_in_handler = 0u;
    arm_block(0);
    (void)arkrtos_mbuf_send_internal(h, "ab", 2, 0);
    check(g_ready_count == 1, "the registration left by the user-mode poll is really there");
    (void)arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 0);

    arm_block(0);
    n = arkrtos_sbuf_delete_internal(h);
    check(n == ARKRTOS_SBUF_OK, "the blocking-receive message buffer deletes cleanly");
    check(g_ready_count == 0, "no message reader was left behind in the buffer");
}

/* ---- 16. 补齐未覆盖行：模块入口、对象表 / 等待表耗尽、任务拆除、
 *         ISR 发送、无限等待、坏长度头、peek 回卷 ---- */

/* 等待表（ARKRTOS_WQ_SLOTS 个槽）要被真正填满，需要几个彼此不同的任务。
 * 它们不必在内核任务表里：登记只比指针，唤醒只读 status、写 wait_time 等
 * 字段，arkrtos_ready_add 是桩。 */
static arkrtos_task_t g_wp[8];

static void case_coverage_gaps(void)
{
    uint8 out[ARKRTOS_SBUF_BUF_SIZE];
    uint8 raw[ARKRTOS_SBUF_BUF_SIZE];
    int32 bad = ARKRTOS_SBUF_HANDLE_FLAG | ARKRTOS_SBUF_NUM;   /* 标志对、槽位越界 */
    int32 hs[ARKRTOS_SBUF_NUM];
    int32 h;
    int32 n;
    int32 i;

    g_cur_task          = 0;
    g_block_mode        = 0;
    g_block_steps       = 0;
    g_in_handler        = 0u;
    g_ready_count       = 0;
    g_block_free_handle = -1;
    arkrtos_kernel_tick = 0u;
    arkrtos_task_count  = 0;

    /* ---- A. 创建入口：空名字是参数错误；内核入口的 creator 恒为 0 ---- */
    check(arkrtos_sbuf_create_internal(0) == ARKRTOS_SBUF_ERR_PARAM,
          "a stream buffer without a name is refused");
    check(arkrtos_sbuf_create_kernel_internal(0) == ARKRTOS_SBUF_ERR_PARAM,
          "a kernel stream buffer without a name is refused");
    h = arkrtos_sbuf_create_kernel_internal("k1");
    check(h >= 0, "a kernel-owned stream buffer is created");
    check(arkrtos_sbuf_is_alive(h) == 1, "the kernel buffer is alive");

    /* ---- B. 句柄解析：标志位对但槽位越过对象表 -> 无效句柄 ---- */
    check(arkrtos_sbuf_used_internal(bad) == ARKRTOS_SBUF_ERR_PARAM,
          "a handle whose slot is past the object table is refused");

    /* ---- C. 对象表耗尽：第 ARKRTOS_SBUF_NUM+1 个创建请求必须被拒 ---- */
    for(i = 0; i < (int32)ARKRTOS_SBUF_NUM - 1; i++)
    {
        hs[i] = arkrtos_sbuf_create_internal("g");
        check(hs[i] >= 0, "the object table accepts a buffer");
    }
    check(arkrtos_sbuf_create_internal("g") == ARKRTOS_SBUF_ERR_NOSPACE,
          "the object table refuses one buffer too many");
    /* 放掉一格：后面的用例还要建自己的缓冲，不能带着一张满表往下走 */
    check(arkrtos_sbuf_delete_internal(hs[0]) == ARKRTOS_SBUF_OK,
          "one object slot is released for the following cases");

    /* ---- D. 任务拆除：非法 id 不动任何状态；自己建的随任务消失，
     *         内核建的留下，但该任务的登记都要摘掉 ---- */
    arkrtos_sbuf_release_task(0);
    arkrtos_sbuf_release_task(99);
    check(arkrtos_sbuf_is_alive(h) == 1, "releasing an unknown task id touched nothing");

    arkrtos_task_count = 3;
    for(i = 0; i < 3; i++)
    {
        memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
        arkrtos_task_table[i].status = ARKRTOS_TASK_WAIT;
    }
    arkrtos_current_task_id = 1;
    check(arkrtos_sbuf_create_internal("own") >= 0, "a task-owned buffer is created");
    /* 任务 1 在这个缓冲和内核缓冲上都挂着读登记 */
    g_cur_task   = &arkrtos_task_table[0];
    g_in_handler = 1u;
    (void)arkrtos_sbuf_recv_internal(h, out, 4, 100);
    g_in_handler = 0u;
    g_cur_task   = 0;
    arkrtos_current_task_id = 0;
    arkrtos_sbuf_release_task(1);
    check(arkrtos_sbuf_is_alive(h) == 1, "a kernel-owned buffer survives the task that used it");
    /* 此刻表里有一个空槽：拆除循环必须跳过它，而不是拿 0 号对象当命中 */
    arkrtos_sbuf_release_task(2);
    check(arkrtos_sbuf_is_alive(h) == 1,
          "releasing another task skips the free slot and leaves the kernel buffer alone");
    g_ready_count = 0;
    check(arkrtos_sbuf_send_internal(h, "z", 1, 0) == 1,
          "the surviving buffer still takes bytes");
    check(g_ready_count == 0, "the dead task's registration was removed from the kernel buffer");
    (void)arkrtos_sbuf_recv_internal(h, out, (int32)sizeof(out), 0);
    /* 现在只剩两个：k1 + 三个 "g" 里被任务 1 带走的那一个已释放 */
    check(arkrtos_sbuf_create_internal("x1") >= 0,
          "the slot of the task-owned buffer is free again");

    /* ---- E. wake_one 遇到已经无效的等待者：清掉它、继续找下一个 ---- */
    (void)arkrtos_sbuf_delete_internal(h);
    h = arkrtos_sbuf_create_internal("w1");
    check(h >= 0, "a buffer for the dead-waiter case is created");
    g_cur_task      = &g_wp[0];
    g_wp[0].status  = ARKRTOS_TASK_INVALID;
    g_in_handler    = 1u;
    (void)arkrtos_sbuf_recv_internal(h, out, 4, 100);
    g_in_handler    = 0u;
    g_ready_count   = 0;
    check(arkrtos_sbuf_send_internal(h, "q", 1, 0) == 1, "a send still stores the byte");
    check(g_ready_count == 0, "a waiter whose task is already invalid is not woken");
    (void)arkrtos_sbuf_recv_internal(h, out, (int32)sizeof(out), 0);

    /* ---- F. 写者等待表耗尽（流）：第 5 个登记者是参数错误 ---- */
    for(i = 0; i < (int32)ARKRTOS_SBUF_BUF_SIZE; i++)
    {
        raw[i] = (uint8)(i & 0xff);
    }
    fill_stream(h);
    for(i = 0; i < (int32)ARKRTOS_WQ_SLOTS; i++)
    {
        g_cur_task        = &g_wp[i];
        g_wp[i].status    = ARKRTOS_TASK_WAIT;
        g_in_handler      = 1u;
        check(arkrtos_sbuf_send_internal(h, "x", 1, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
              "a user-mode writer registers and reports would-block");
        g_in_handler = 0u;
    }
    g_cur_task   = &g_wp[ARKRTOS_WQ_SLOTS];
    g_in_handler = 1u;
    check(arkrtos_sbuf_send_internal(h, "x", 1, 100) == ARKRTOS_SBUF_ERR_PARAM,
          "a full writer table refuses one more registration");
    g_in_handler = 0u;
    g_cur_task   = 0;
    g_ready_count = 0;
    check(arkrtos_sbuf_delete_internal(h) == ARKRTOS_SBUF_OK,
          "the saturated-writer buffer deletes cleanly");
    check(g_ready_count == (int32)ARKRTOS_WQ_SLOTS,
          "deleting wakes every registered writer, one by one");

    /* ---- G. 读者等待表耗尽（流） ---- */
    h = arkrtos_sbuf_create_internal("w2");
    check(h >= 0, "a buffer for the saturated reader table is created");
    for(i = 0; i < (int32)ARKRTOS_WQ_SLOTS; i++)
    {
        g_cur_task     = &g_wp[i];
        g_wp[i].status = ARKRTOS_TASK_WAIT;
        g_in_handler   = 1u;
        check(arkrtos_sbuf_recv_internal(h, out, 4, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
              "a user-mode reader registers and reports would-block");
        g_in_handler = 0u;
    }
    g_cur_task   = &g_wp[ARKRTOS_WQ_SLOTS];
    g_in_handler = 1u;
    check(arkrtos_sbuf_recv_internal(h, out, 4, 100) == ARKRTOS_SBUF_ERR_PARAM,
          "a full reader table refuses one more registration");
    g_in_handler  = 0u;
    g_cur_task    = 0;
    g_ready_count = 0;
    check(arkrtos_sbuf_delete_internal(h) == ARKRTOS_SBUF_OK,
          "the saturated-reader buffer deletes cleanly");
    check(g_ready_count == (int32)ARKRTOS_WQ_SLOTS,
          "deleting wakes every registered reader, one by one");

    /* ---- H. 写者等待表耗尽（消息）：整条放不下时同样登记 ---- */
    h = arkrtos_sbuf_create_internal("w3");
    check(h >= 0, "a message buffer for the saturated writer table is created");
    n = (int32)ARKRTOS_SBUF_BUF_SIZE - (int32)ARKRTOS_MBUF_HDR_BYTES;
    check(arkrtos_mbuf_send_internal(h, raw, n, 0) == ARKRTOS_SBUF_OK,
          "the message buffer is filled to the brim");
    for(i = 0; i < (int32)ARKRTOS_WQ_SLOTS; i++)
    {
        g_cur_task     = &g_wp[i];
        g_wp[i].status = ARKRTOS_TASK_WAIT;
        g_in_handler   = 1u;
        check(arkrtos_mbuf_send_internal(h, "hi", 2, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
              "a user-mode message writer registers and reports would-block");
        g_in_handler = 0u;
    }
    g_cur_task   = &g_wp[ARKRTOS_WQ_SLOTS];
    g_in_handler = 1u;
    check(arkrtos_mbuf_send_internal(h, "hi", 2, 100) == ARKRTOS_SBUF_ERR_PARAM,
          "a full message writer table refuses one more registration");
    g_in_handler = 0u;
    g_cur_task   = 0;
    g_ready_count = 0;
    check(arkrtos_sbuf_delete_internal(h) == ARKRTOS_SBUF_OK,
          "the saturated message-writer buffer deletes cleanly");
    check(g_ready_count == (int32)ARKRTOS_WQ_SLOTS,
          "deleting wakes every registered message writer");

    /* ---- I. 读者等待表耗尽（消息） ---- */
    h = arkrtos_sbuf_create_internal("w4");
    check(h >= 0, "a message buffer for the saturated reader table is created");
    for(i = 0; i < (int32)ARKRTOS_WQ_SLOTS; i++)
    {
        g_cur_task     = &g_wp[i];
        g_wp[i].status = ARKRTOS_TASK_WAIT;
        g_in_handler   = 1u;
        check(arkrtos_mbuf_recv_internal(h, out, 16, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
              "a user-mode message reader registers and reports would-block");
        g_in_handler = 0u;
    }
    g_cur_task   = &g_wp[ARKRTOS_WQ_SLOTS];
    g_in_handler = 1u;
    check(arkrtos_mbuf_recv_internal(h, out, 16, 100) == ARKRTOS_SBUF_ERR_PARAM,
          "a full message reader table refuses one more registration");
    g_in_handler = 0u;
    g_cur_task   = 0;
    g_ready_count = 0;
    check(arkrtos_sbuf_delete_internal(h) == ARKRTOS_SBUF_OK,
          "the saturated message-reader buffer deletes cleanly");
    check(g_ready_count == (int32)ARKRTOS_WQ_SLOTS,
          "deleting wakes every registered message reader");

    /* ---- J. 参数闸门：空指针 / 零长度 / 越界句柄 —— 四个入口各来一遍 ---- */
    h = arkrtos_sbuf_create_internal("p1");
    check(h >= 0, "a buffer for the parameter gates is created");
    check(arkrtos_sbuf_recv_internal(h, 0, 4, 0) == ARKRTOS_SBUF_ERR_PARAM,
          "a receive without a destination is refused");
    check(arkrtos_sbuf_recv_internal(h, out, 0, 0) == ARKRTOS_SBUF_ERR_PARAM,
          "a zero-length receive is refused");
    check(arkrtos_mbuf_send_internal(h, 0, 4, 0) == ARKRTOS_SBUF_ERR_PARAM,
          "a message send without a source is refused");
    check(arkrtos_mbuf_send_internal(h, raw, 0, 0) == ARKRTOS_SBUF_ERR_PARAM,
          "a zero-length message send is refused");
    check(arkrtos_mbuf_recv_internal(h, 0, 4, 0) == ARKRTOS_SBUF_ERR_PARAM,
          "a message receive without a destination is refused");
    check(arkrtos_mbuf_recv_internal(h, out, 0, 0) == ARKRTOS_SBUF_ERR_PARAM,
          "a zero-length message receive is refused");
    check(arkrtos_mbuf_send_internal(bad, raw, 4, 0) == ARKRTOS_SBUF_ERR_PARAM,
          "a message send on a past-the-table handle is refused");
    check(arkrtos_mbuf_recv_internal(bad, out, 4, 0) == ARKRTOS_SBUF_ERR_PARAM,
          "a message receive on a past-the-table handle is refused");
    check(arkrtos_mbuf_peek_len_internal(bad) == ARKRTOS_SBUF_ERR_PARAM,
          "a peek on a past-the-table handle is refused");
    check(arkrtos_sbuf_reset_internal(bad) == ARKRTOS_SBUF_ERR_PARAM,
          "a reset on a past-the-table handle is refused");
    check(arkrtos_sbuf_delete_internal(bad) == ARKRTOS_SBUF_ERR_PARAM,
          "a delete on a past-the-table handle is refused");

    /* ---- K. timeout < 0 是"永久等待"：被唤醒的每一步都不得自己给自己
     *         算出一个到期时间（remain 恒为无限） ---- */
    fill_stream(h);
    g_cur_task = &g_fake_task;
    arm_block(2);
    check(arkrtos_sbuf_send_internal(h, "x", 1, -1) == ARKRTOS_SBUF_ERR_TIMEOUT,
          "an infinite stream send still ends when the wake-up carries no hand-off");
    check(g_block_steps == 2, "the infinite send really slept twice (never timed itself out)");
    arm_block(2);
    check(arkrtos_mbuf_send_internal(h, "hi", 2, -1) == ARKRTOS_SBUF_ERR_TIMEOUT,
          "an infinite message send ends the same way");
    check(g_block_steps == 2, "the infinite message send slept twice as well");
    (void)arkrtos_sbuf_reset_internal(h);
    arm_block(2);
    check(arkrtos_sbuf_recv_internal(h, out, 4, -1) == ARKRTOS_SBUF_ERR_TIMEOUT,
          "an infinite stream receive ends when handed no data");
    check(g_block_steps == 2, "the infinite receive slept twice");
    arm_block(2);
    check(arkrtos_mbuf_recv_internal(h, out, 16, -1) == ARKRTOS_SBUF_ERR_TIMEOUT,
          "an infinite message receive ends when handed no message");
    check(g_block_steps == 2, "the infinite message receive slept twice");

    /* ---- L. 坏长度头：全零 / 荒唐长度 / 头到了身子没到 ---- */
    (void)arkrtos_sbuf_reset_internal(h);
    check(arkrtos_sbuf_send_internal(h, "\0\0\0\0", (int32)ARKRTOS_MBUF_HDR_BYTES, 0)
          == (int32)ARKRTOS_MBUF_HDR_BYTES, "four zero bytes are stored as a bogus header");
    check(arkrtos_mbuf_peek_len_internal(h) == ARKRTOS_SBUF_ERR_BADLEN,
          "a zero length header is reported as a broken header");
    (void)arkrtos_sbuf_reset_internal(h);
    check(arkrtos_sbuf_send_internal(h, "\xff\xff\xff\xff", (int32)ARKRTOS_MBUF_HDR_BYTES, 0)
          == (int32)ARKRTOS_MBUF_HDR_BYTES, "a huge header is stored too");
    check(arkrtos_mbuf_peek_len_internal(h) == ARKRTOS_SBUF_ERR_BADLEN,
          "a header longer than the whole buffer is reported as broken");
    (void)arkrtos_sbuf_reset_internal(h);
    check(arkrtos_sbuf_send_internal(h, "\x0a\x00\x00\x00", (int32)ARKRTOS_MBUF_HDR_BYTES, 0)
          == (int32)ARKRTOS_MBUF_HDR_BYTES, "a header for ten bytes is stored alone");
    check(arkrtos_mbuf_peek_len_internal(h) == -1,
          "a header without its body reports no whole message instead of a length");

    /* ---- M. peek 跨过回卷点：长度头被拆成两段写进环形区末尾 ---- */
    (void)arkrtos_sbuf_reset_internal(h);
    n = (int32)ARKRTOS_SBUF_BUF_SIZE - 2;
    check(arkrtos_sbuf_send_internal(h, raw, n, 0) == n, "the ring is advanced to two bytes before the end");
    check(arkrtos_sbuf_recv_internal(h, out, n, 0) == n, "and read back, so rd and wt sit there");
    check(arkrtos_mbuf_send_internal(h, "hello", 5, 0) == ARKRTOS_SBUF_OK,
          "a message is written across the wrap point");
    check(arkrtos_mbuf_peek_len_internal(h) == 5,
          "the length header is read correctly although it wraps");
    check(arkrtos_mbuf_recv_internal(h, out, (int32)sizeof(out), 0) == 5,
          "the wrapped message comes back whole");
    check(memcmp(out, "hello", 5) == 0, "and its body is intact");

    /* ---- N. ISR 侧写流：满就报超时，绝不写半条 ---- */
    (void)arkrtos_sbuf_reset_internal(h);
    check(arkrtos_sbuf_send_from_isr_internal(h, 0, 4) == ARKRTOS_SBUF_ERR_PARAM,
          "an ISR send without a source is refused");
    check(arkrtos_sbuf_send_from_isr_internal(h, raw, 0) == ARKRTOS_SBUF_ERR_PARAM,
          "a zero-length ISR send is refused");
    check(arkrtos_sbuf_send_from_isr_internal(bad, raw, 4) == ARKRTOS_SBUF_ERR_PARAM,
          "an ISR send on a past-the-table handle is refused");
    /* 读者先登记在空缓冲上（只登记不睡），ISR 侧一写就必须把它叫醒：
     * 这是 ISR 路径唯一的"唤醒"动作，写在空缓冲上才看得见。 */
    g_cur_task     = &g_wp[0];
    g_wp[0].status = ARKRTOS_TASK_WAIT;
    g_in_handler   = 1u;
    check(arkrtos_sbuf_recv_internal(h, out, 4, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
          "the reader registers on the empty buffer");
    g_in_handler  = 0u;
    g_cur_task    = 0;
    g_ready_count = 0;
    check(arkrtos_sbuf_send_from_isr_internal(h, raw, 3) == 3, "an ISR send stores what fits");
    check(g_ready_count == 1, "the ISR send woke the registered reader");
    (void)arkrtos_sbuf_reset_internal(h);

    /* 房间只剩 5 字节时，10 字节的 ISR 写只能落下 5 个（不写半条） */
    n = (int32)ARKRTOS_SBUF_BUF_SIZE - 5;
    check(arkrtos_sbuf_send_internal(h, raw, n, 0) == n, "the buffer is left with five bytes of room");
    check(arkrtos_sbuf_send_from_isr_internal(h, raw, 10) == 5,
          "an ISR send reports the bytes that fit, not the bytes asked for");
    check(arkrtos_sbuf_free_internal(h) == 0, "that partial ISR write filled the buffer exactly");
    check(arkrtos_sbuf_send_from_isr_internal(h, raw, 1) == ARKRTOS_SBUF_ERR_TIMEOUT,
          "an ISR send on a full buffer reports timeout instead of waiting");
    (void)arkrtos_sbuf_reset_internal(h);

    /* ---- O. ISR 侧发消息：放不下整条就报超时，绝不留下半个身子 ---- */
    check(arkrtos_mbuf_send_from_isr_internal(h, 0, 4) == ARKRTOS_SBUF_ERR_PARAM,
          "an ISR message send without a source is refused");
    check(arkrtos_mbuf_send_from_isr_internal(h, raw, 0) == ARKRTOS_SBUF_ERR_PARAM,
          "a zero-length ISR message send is refused");
    check(arkrtos_mbuf_send_from_isr_internal(bad, raw, 4) == ARKRTOS_SBUF_ERR_PARAM,
          "an ISR message send on a past-the-table handle is refused");
    check(arkrtos_mbuf_send_from_isr_internal(h, raw, (int32)ARKRTOS_SBUF_BUF_SIZE)
          == ARKRTOS_SBUF_ERR_NOSPC, "an ISR message bigger than the buffer is refused as NOSPC");
    check(arkrtos_mbuf_send_from_isr_internal(h, "abc", 3) == ARKRTOS_SBUF_OK,
          "an ISR message that fits is accepted");
    /* "abc" 占了 4+3 = 7 字节；第二条消息把剩下的房间用尽：
     * 载荷 = 容量 - 7 - 4（它自己的长度头） */
    n = (int32)ARKRTOS_SBUF_BUF_SIZE - (int32)ARKRTOS_MBUF_HDR_BYTES - 3
        - (int32)ARKRTOS_MBUF_HDR_BYTES;
    check(arkrtos_mbuf_send_from_isr_internal(h, raw, n) == ARKRTOS_SBUF_OK,
          "the rest of the buffer is filled with a second message");
    check(arkrtos_mbuf_send_from_isr_internal(h, "hi", 2) == ARKRTOS_SBUF_ERR_TIMEOUT,
          "an ISR message send without room for the whole frame reports timeout");
    check(arkrtos_mbuf_peek_len_internal(h) == 3, "the refused frame left no half header behind");

    /* ---- P. 模块初始化：整张对象表被清空 ---- */
    (void)arkrtos_sbuf_delete_internal(h);
    h = arkrtos_sbuf_create_internal("z1");
    check(h >= 0, "a buffer exists right before the module reset");
    arkrtos_sbuf_module_init();
    check(arkrtos_sbuf_is_alive(h) == 0, "module init clears the object table");

    arkrtos_task_count     = 0;
    arkrtos_current_task_id = 0;
    g_cur_task             = 0;
}

int main(void)

{
    int32 h;
    uint8 buf[512];
    uint8 out[512];
    int32 n;
    int32 i;

    memset(buf, 0x5a, sizeof(buf));
    memset(out, 0, sizeof(out));

    /* ---- 1. 创建与初始状态 ---- */
    h = arkrtos_sbuf_create_internal("t1");
    check(h >= 0, "create returns a handle");
    check(arkrtos_sbuf_is_alive(h) == 1, "fresh handle is alive");
    check(arkrtos_sbuf_used_internal(h) == 0, "fresh buffer is empty");
    check(arkrtos_sbuf_free_internal(h) == ARKRTOS_SBUF_BUF_SIZE, "fresh buffer is fully free");

    /* ---- 2. 基本读写 ---- */
    n = arkrtos_sbuf_send_internal(h, "hello", 5, 0);
    check(n == 5, "send 5 bytes reports 5");
    check(arkrtos_sbuf_used_internal(h) == 5, "used == 5 after send");
    n = arkrtos_sbuf_recv_internal(h, out, 2, 0);
    check(n == 2, "recv 2 bytes reports 2");
    check(memcmp(out, "he", 2) == 0, "recv returns the first bytes in order");
    n = arkrtos_sbuf_recv_internal(h, out, sizeof(out), 0);
    check(n == 3, "remaining 3 bytes come out in one recv");
    check(memcmp(out, "llo", 3) == 0, "remaining bytes are the tail of the stream");
    check(arkrtos_sbuf_used_internal(h) == 0, "buffer empty after draining");

    /* ---- 3. 空读 / 不等待 ---- */
    n = arkrtos_sbuf_recv_internal(h, out, 4, 0);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "recv on empty buffer with timeout 0 times out");

    /* ---- 4. 写满 + 部分成功 + 回绕 ---- */
    /* 先写 BUF_SIZE-4 字节，再写 8 字节：应当只成功 4 字节（部分成功） */
    for(i = 0; i < (int32)ARKRTOS_SBUF_BUF_SIZE; i++)
    {
        buf[i] = (uint8)(i & 0xff);
    }
    n = arkrtos_sbuf_send_internal(h, buf, (int32)ARKRTOS_SBUF_BUF_SIZE - 4, 0);
    check(n == (int32)ARKRTOS_SBUF_BUF_SIZE - 4, "send fills almost the whole buffer");
    n = arkrtos_sbuf_send_internal(h, buf, 8, 0);
    check(n == 4, "partial send reports only what fit");
    check(arkrtos_sbuf_free_internal(h) == 0, "buffer reports full");
    n = arkrtos_sbuf_send_internal(h, buf, 1, 0);
    check(n == ARKRTOS_SBUF_ERR_TIMEOUT, "send on full buffer with timeout 0 times out");

    /* 读掉 10 字节，再写 10 字节：写指针会绕过末尾，读出顺序必须仍然连续 */
    n = arkrtos_sbuf_recv_internal(h, out, 10, 0);
    check(n == 10, "drain 10 bytes");
    check(memcmp(out, buf, 10) == 0, "drained head matches the source");
    {
        uint8 tail[10];

        for(i = 0; i < 10; i++)
        {
            tail[i] = (uint8)(0xa0 + i);
        }
        n = arkrtos_sbuf_send_internal(h, tail, 10, 0);
        check(n == 10, "send 10 bytes across the wrap point");

        n = arkrtos_sbuf_recv_internal(h, out, (int32)ARKRTOS_SBUF_BUF_SIZE, 0);
        check(n == (int32)ARKRTOS_SBUF_BUF_SIZE, "drain everything after wrap");
        /* 期望：原缓冲第 10..BUF_SIZE-4 字节，后面跟着刚才那 10 字节 */
        check(memcmp(out, buf + 10, (size_t)(ARKRTOS_SBUF_BUF_SIZE - 14)) == 0,
              "post-wrap stream keeps the original order");
        /* 那块半截写入留下的 4 字节夹在中间，顺序必须是：旧流尾部 -> 那 4 字节 -> 新写 10 字节 */
        check(memcmp(out + (ARKRTOS_SBUF_BUF_SIZE - 14), buf, 4) == 0,
              "the 4 bytes of the earlier partial send sit right after the old tail");
        check(memcmp(out + (ARKRTOS_SBUF_BUF_SIZE - 14) + 4, tail, 10) == 0,
              "post-wrap stream continues with the newly written bytes");
    }

    /* ---- 5. 超过整块缓冲的写入是参数错误 ---- */
    n = arkrtos_sbuf_send_internal(h, buf, (int32)ARKRTOS_SBUF_BUF_SIZE + 1, 0);
    check(n == ARKRTOS_SBUF_ERR_NOSPC, "oversized stream write is refused, not split");

    /* ---- 6. reset ---- */
    (void)arkrtos_sbuf_send_internal(h, "abc", 3, 0);
    n = arkrtos_sbuf_reset_internal(h);
    check(n == ARKRTOS_SBUF_OK, "reset returns OK");
    check(arkrtos_sbuf_used_internal(h) == 0, "reset empties the buffer");

    /* ---- 7. 消息：整条收发 + 两条消息不混 ---- */
    n = arkrtos_mbuf_send_internal(h, "first", 5, 0);
    check(n == ARKRTOS_SBUF_OK, "message send returns OK");
    n = arkrtos_mbuf_send_internal(h, "", 1, 0);   /* 1 字节消息 */
    check(n == ARKRTOS_SBUF_OK, "second (short) message accepted");
    check(arkrtos_mbuf_peek_len_internal(h) == 5, "peek reports the first message length");

    n = arkrtos_mbuf_recv_internal(h, out, sizeof(out), 0);
    check(n == 5, "recv returns the first message length");
    check(memcmp(out, "first", 5) == 0, "first message body intact");
    check(arkrtos_mbuf_peek_len_internal(h) == 1, "peek reports the second message length");
    n = arkrtos_mbuf_recv_internal(h, out, sizeof(out), 0);
    check(n == 1, "second message length");
    check(out[0] == 0, "second message body intact");
    check(arkrtos_mbuf_peek_len_internal(h) < 0, "peek on empty buffer is negative");

    /* ---- 8. 接收缓冲太小：整条丢弃，下一条不受影响 ---- */
    n = arkrtos_mbuf_send_internal(h, "0123456789", 10, 0);
    check(n == ARKRTOS_SBUF_OK, "10-byte message accepted");
    n = arkrtos_mbuf_send_internal(h, "next", 4, 0);
    check(n == ARKRTOS_SBUF_OK, "following message accepted");
    n = arkrtos_mbuf_recv_internal(h, out, 4, 0);
    check(n == ARKRTOS_SBUF_ERR_TOO_SMALL, "small recv buffer is reported as TOO_SMALL");
    n = arkrtos_mbuf_recv_internal(h, out, sizeof(out), 0);
    check(n == 4, "the next message is still intact after a discard");
    check(memcmp(out, "next", 4) == 0, "and its body is correct (no length-header desync)");

    /* ---- 9. 消息比整块缓冲大：NOSPC，不是无限等 ---- */
    n = arkrtos_mbuf_send_internal(h, buf, (int32)ARKRTOS_SBUF_BUF_SIZE, 0);
    check(n == ARKRTOS_SBUF_ERR_NOSPC, "oversized message is refused as NOSPC");

    /* ---- 10. 删除 ---- */
    n = arkrtos_sbuf_delete_internal(h);
    check(n == ARKRTOS_SBUF_OK, "delete returns OK");
    check(arkrtos_sbuf_is_alive(h) == 0, "handle is dead after delete");
    n = arkrtos_sbuf_send_internal(h, "x", 1, 0);
    check(n == ARKRTOS_SBUF_ERR_PARAM, "dead handle is rejected");

    /* ---- 11. 参数检查 ---- */
    h = arkrtos_sbuf_create_internal("t2");
    check(arkrtos_sbuf_send_internal(h, 0, 4, 0) == ARKRTOS_SBUF_ERR_PARAM, "null src refused");
    check(arkrtos_sbuf_send_internal(h, buf, 0, 0) == ARKRTOS_SBUF_ERR_PARAM, "zero length refused");
    check(arkrtos_sbuf_recv_internal(0x1234, out, 4, 0) == ARKRTOS_SBUF_ERR_PARAM, "bogus handle refused");
    check(arkrtos_sbuf_delete_internal(h) == ARKRTOS_SBUF_OK, "the parameter-check handle is released");

    /* ---- 12..15. 阻塞收发路径（可编程桩） ---- */
    case_stream_send_blocking();
    case_stream_recv_blocking();
    case_mbuf_send_blocking();
    case_mbuf_recv_blocking();
    case_coverage_gaps();

    g_cur_task = 0;
    g_block_mode = 0;
    g_block_steps = 0;
    g_in_handler = 0u;

    if(g_fail == 0)
    {
        printf("SBUF-HOST OK (%d checks)\n", g_checks);
        return 0;
    }
    printf("SBUF-HOST FAILED (%d/%d)\n", g_fail, g_checks);
    return 1;
}
