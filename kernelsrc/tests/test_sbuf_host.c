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
*                    用户态轮询（只登记不睡）（第 12..15 段，可编程桩）。
*            未验证：真睡下与真被 ISR 唤醒、真节拍推进、真超时——桩只回答
*                    「唤醒原因给成这样时代码走哪条路」，不回答「调度器会不会
*                    按时唤醒它」。那部分只有上板用例能证明。
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
