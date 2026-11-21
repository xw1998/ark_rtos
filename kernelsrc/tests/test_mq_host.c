/**
* @file test_mq_host.c
* @brief 消息队列（arkrtos_mq）的宿主单元测试（不依赖板子、不依赖调度器）
* @details 为什么需要它：mq 的缺陷几乎都在"槽位账目"里——头尾索引回绕、msglen
*          与实际拷贝长度错位、满了还写、删了还能收。这些在板子上只能靠串口
*          观察，在宿主上可以逐字断言。
*
*          覆盖范围（如实说明）：
*            已验证：创建/存活探测、变长消息按声明长度收发、接收缓冲更小时截断
*                    （不串下一条）、队列满时非阻塞发送失败、清空后再写（头尾回绕）、
*                    send_from_isr 的满/不满两态、删除后句柄失效、非法句柄被拒。
*                    阻塞发送与阻塞接收的登记、唤醒后重排队、腾出空间/取到消息、
*                    超时用尽、等待对象被删除、用户态轮询六类收尾（可编程桩，
*                    第 10/11 段）。
*            未验证：真睡下与真被 ISR 唤醒、真节拍推进、真超时——桩只回答「唤醒原因
*                    给成这样时代码走哪条路」，不回答「调度器会不会按时唤醒它」。
* @note 用法：由 tools/mq_host_gate.py 编译并运行，输出 MQ-HOST OK。
*/

#include "arkrtos_mq.h"
#include "arkrtos_cfg.h"
#include "arkrtos.h"   /* arkrtos_mq_recv_from_isr 的公开声明在这里（mq.h 只有 *_internal） */

#include <stdio.h>
#include <string.h>

/* 宿主档里对象表不做 static（源里由 ARKRTOS_MQ_HOST_TEST 打开）：
 * 清盘与防御分支只有摆得出等待表与任务状态才够得到。 */
extern arkrtos_mq_obj_t arkrtos_mqs[ARKRTOS_MQ_NUM];

/* ------------------------------------------------------------------
 * 内核桩：只提供链接所需的符号，行为尽量"什么都不做"
 * ------------------------------------------------------------------ */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32  arkrtos_task_count      = 0;
int32  arkrtos_current_task_id = 0;
uint32 arkrtos_kernel_tick     = 0u;

void arkrtos_port_disable_irq(void) { }
void arkrtos_port_enable_irq(void)  { }
void arkrtos_port_switch_task(void) { }
uint32 arkrtos_port_enter_critical(void) { return 0u; }
void arkrtos_port_exit_critical(uint32 state) { (void)state; }

/* 可编程的阻塞桩。默认值与拆分前逐字一致（当前任务为 0、阻塞恒失败），
 * 因此前面八段用例的路径完全不变；只有第 10/11 段才临时打开 g_block_mode。
 *   mode 0 = 恒失败（默认）
 *   mode 2 = 第一次"令牌交接"、第二次超时
 *   mode 3 = 先从 g_block_free_handle 收走一条腾出空间，再返回"令牌交接"
 *   mode 4 = 返回"令牌交接"前把节拍推过超时上限
 *   mode 5 = 返回"等待对象被删除"
 *   mode 6 = 由 g_in_handler 声明"处在用户态轮询里"
 *   mode 7 = 阻塞期间往 g_block_free_handle 投一条消息，再返回"令牌交接" */
static arkrtos_task_t  g_fake_task;
static arkrtos_task_t *g_cur_task         = 0;
static int32  g_block_mode       = 0;
static int32  g_block_steps      = 0;
static int32  g_block_free_handle = -1;
static uint8  g_in_handler        = 0u;
static uint32 g_block_free_out[ARKRTOS_MQ_MSG_WORDS];
static arkrtos_mq_obj_t *g_block_mq = 0;   /* 重试窗口里要摆等待表的对象 */

arkrtos_task_t *arkrtos_task_get_current(void) { return g_cur_task; }

int32 arkrtos_task_block_in_critical(uint32 timeout_ms)
{
    int32 i;

    (void)timeout_ms;
    g_block_steps++;

    if(g_block_mode == 2)
    {
        return (g_block_steps == 1) ? ARKRTOS_WAKE_HANDOFF : ARKRTOS_WAKE_TIMEOUT;
    }
    if(g_block_mode == 3)
    {
        /* 模拟"接收者腾出一格"：在阻塞期间把队头取走 */
        (void)arkrtos_mq_recv_internal(g_block_free_handle, g_block_free_out, 1, 0);
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
        /* 模拟"发送者在阻塞期间投进一条消息" */
        (void)arkrtos_mq_send_internal(g_block_free_handle, g_block_free_out, 1, 0);
        return ARKRTOS_WAKE_HANDOFF;
    }
    if(g_block_mode == 8)
    {
        /* 第一次交接但不腾空间，第二次超时 */
        return (g_block_steps == 1) ? ARKRTOS_WAKE_HANDOFF : ARKRTOS_WAKE_TIMEOUT;
    }
    if(g_block_mode == 9)
    {
        /* 第一次交接，第二次"没能睡下" */
        return (g_block_steps == 1) ? ARKRTOS_WAKE_HANDOFF : -1;
    }
    if(g_block_mode == 10)
    {
        /* 第一次就超时 */
        return ARKRTOS_WAKE_TIMEOUT;
    }
    if(g_block_mode == 11)
    {
        /* 交接时把发送等待表塞满别的任务 */
        for(i = 0; i < ARKRTOS_WQ_SLOTS; i++)
        {
            g_block_mq->send_waiters[i] = &arkrtos_task_table[10 + i];
        }
        return ARKRTOS_WAKE_HANDOFF;
    }
    if(g_block_mode == 12)
    {
        /* 同上，接收等待表 */
        for(i = 0; i < ARKRTOS_WQ_SLOTS; i++)
        {
            g_block_mq->recv_waiters[i] = &arkrtos_task_table[10 + i];
        }
        return ARKRTOS_WAKE_HANDOFF;
    }
    return -1;
}
static int32 g_ready_calls = 0;
void  arkrtos_ready_add(int32 task_idx) { (void)task_idx; g_ready_calls++; }
uint8 arkrtos_sync_in_handler(void) { return g_in_handler; }

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

int main(void)
{
    int32 h;
    int32 h2;
    int32 h3;
    int32 h4;
    uint32 in[ARKRTOS_MQ_MSG_WORDS + 2];
    uint32 out[ARKRTOS_MQ_MSG_WORDS + 2];
    int32 i;
    int32 n;

    for(i = 0; i < (int32)(ARKRTOS_MQ_MSG_WORDS + 2); i++)
    {
        in[i] = 0xA5A50000u + (uint32)i;
        out[i] = 0xDEADBEEFu;
    }

    /* ---- 1. 创建与存活探测 ---- */
    h = arkrtos_mq_create_internal("mq1");
    check(h >= 0, "create returns a handle");
    check(arkrtos_mq_is_alive(h) == 1, "fresh handle is alive");
    h2 = arkrtos_mq_create_internal("mq2");
    check((h2 >= 0) && (h2 != h), "second queue gets a different handle");

    /* ---- 2. 变长消息按声明长度收发 ---- */
    n = arkrtos_mq_send_internal(h, in, 3, 0);
    check(n == 0, "send 3 words succeeds");
    memset(out, 0xEE, sizeof(out));
    n = arkrtos_mq_recv_internal(h, out, (int32)ARKRTOS_MQ_MSG_WORDS, 0);
    check(n == 3, "recv reports the 3 words the sender declared");
    check((out[0] == in[0]) && (out[1] == in[1]) && (out[2] == in[2]),
          "payload matches word for word");
    check(out[3] == 0xEEEEEEEEu, "recv does not write past the declared length");

    /* ---- 3. 接收缓冲更小：截断到缓冲容量，消息整条消费 ---- */
    n = arkrtos_mq_send_internal(h, in, 4, 0);
    check(n == 0, "send 4 words succeeds");
    n = arkrtos_mq_recv_internal(h, out, 2, 0);
    check(n == 2, "recv with a 2-word buffer reports 2");
    check((out[0] == in[0]) && (out[1] == in[1]), "truncated copy keeps the first words");
    n = arkrtos_mq_recv_internal(h, out, (int32)ARKRTOS_MQ_MSG_WORDS, 0);
    check(n == -1, "the truncated message was consumed whole (queue is empty)");

    /* ---- 4. 非法长度被拒，不留下半条 ---- */
    n = arkrtos_mq_send_internal(h, in, 0, 0);
    check(n == -1, "send 0 words is rejected");
    n = arkrtos_mq_send_internal(h, in, (int32)ARKRTOS_MQ_MSG_WORDS + 1, 0);
    check(n == -1, "send more words than a slot holds is rejected");
    n = arkrtos_mq_recv_internal(h, out, 0, 0);
    check(n == -1, "recv 0 words is rejected");
    n = arkrtos_mq_recv_internal(h, out, (int32)ARKRTOS_MQ_MSG_WORDS, 0);
    check(n == -1, "rejected sends left nothing behind");

    /* ---- 5. 队列满：非阻塞发送失败，且不覆盖已有消息 ---- */
    for(i = 0; i < (int32)ARKRTOS_MQ_DEPTH; i++)
    {
        in[0] = (uint32)i;
        n = arkrtos_mq_send_internal(h, in, 1, 0);
        check(n == 0, "fill slot succeeds");
    }
    in[0] = 0xFFFFFFFFu;
    n = arkrtos_mq_send_internal(h, in, 1, 0);
    check(n == -1, "send on a full queue fails without waiting");
    n = arkrtos_mq_send_from_isr_internal(h, in, 1);
    check(n == -1, "send_from_isr on a full queue fails");

    /* ---- 6. 清空后每条都按 FIFO 顺序取出（头尾索引回绕） ---- */
    for(i = 0; i < (int32)ARKRTOS_MQ_DEPTH; i++)
    {
        n = arkrtos_mq_recv_internal(h, out, 1, 0);
        check(n == 1, "drain returns one word");
        check(out[0] == (uint32)i, "drain keeps FIFO order");
    }
    n = arkrtos_mq_recv_internal(h, out, 1, 0);
    check(n == -1, "queue is empty after a full drain");

    /* 头尾都已回绕一圈：再写再读必须仍然正常 */
    in[0] = 0x12345678u;
    n = arkrtos_mq_send_internal(h, in, 1, 0);
    check(n == 0, "send after a wrap succeeds");
    n = arkrtos_mq_recv_internal(h, out, 1, 0);
    check((n == 1) && (out[0] == 0x12345678u), "the wrapped slot holds the new message");

    /* ---- 7. send_from_isr 的正常路径 ---- */
    in[0] = 0xFEEDFACEu;
    n = arkrtos_mq_send_from_isr_internal(h, in, 1);
    check(n == 0, "send_from_isr on a non-full queue succeeds");
    n = arkrtos_mq_recv_internal(h, out, 1, 0);
    check((n == 1) && (out[0] == 0xFEEDFACEu), "the ISR-posted message is readable");

    /* ---- 7b. 中断里直接收：空队列不登记、不等待；有消息就取走 ---- */
    n = arkrtos_mq_recv_from_isr_internal(h, out, 1);
    check(n == -1, "recv_from_isr on an empty queue fails instead of waiting");
    in[0] = 0xA5A5A5A5u;
    n = arkrtos_mq_send_from_isr_internal(h, in, 2);
    check(n == 0, "send_from_isr feeds a 2-word message");
    n = arkrtos_mq_recv_from_isr_internal(h, out, 2);
    check(n == 2, "recv_from_isr reports the declared word count");
    check(out[0] == 0xA5A5A5A5u, "recv_from_isr copies the payload");
    n = arkrtos_mq_recv_from_isr_internal(h, out, 2);
    check(n == -1, "the second recv_from_isr finds the queue empty");
    n = arkrtos_mq_recv_from_isr(h, out, 1);
    check(n == -1, "the public wrapper agrees with the internal entry point");

    /* ---- 8. 非法句柄 ---- */
    n = arkrtos_mq_send_internal(h + 0x00100000, in, 1, 0);   /* 错误的高位标志 */
    check(n == -1, "a handle with the wrong type flag is rejected");
    n = arkrtos_mq_send_internal(h + (int32)ARKRTOS_MQ_NUM, in, 1, 0);
    check(n == -1, "an out-of-range index is rejected");

    /* ---- 9. 删除 ---- */
    n = arkrtos_mq_delete_internal(h);
    check(n == 0, "delete succeeds");
    check(arkrtos_mq_is_alive(h) == 0, "the handle is not alive after delete");
    n = arkrtos_mq_send_internal(h, in, 1, 0);
    check(n == -1, "send on a deleted queue is rejected");
    n = arkrtos_mq_recv_internal(h, out, 1, 0);
    check(n == -1, "recv on a deleted queue is rejected");
    n = arkrtos_mq_delete_internal(h);
    check(n == -1, "double delete is rejected");

    /* 另一个队列不受影响 */
    n = arkrtos_mq_send_internal(h2, in, 1, 0);
    check(n == 0, "the other queue still works");
    n = arkrtos_mq_delete_internal(h2);
    check(n == 0, "the other queue deletes cleanly");

    /* ---- 10. 阻塞发送：登记、重排队、腾出空间、超时用尽、对象删除、用户态轮询 ---- */
    h3 = arkrtos_mq_create_internal("mq3");
    check(h3 >= 0, "a third queue is created for the blocking paths");
    for(i = 0; i < (int32)ARKRTOS_MQ_DEPTH; i++)
    {
        in[0] = 0x6000u + (uint32)i;
        n = arkrtos_mq_send_internal(h3, in, 1, 0);
        check(n == 0, "the third queue is filled");
    }

    /* A. 有任务但进不了阻塞（block 返回负值）：撤销登记、按上下文错误收尾。
     *    连做 5 次都必须是同一结论——若登记没被撤销，第二次起会撞上重复登记。 */
    g_cur_task    = &g_fake_task;
    g_block_mode  = 0;
    g_block_steps = 0;
    for(i = 0; i < 5; i++)
    {
        n = arkrtos_mq_send_internal(h3, in, 1, 100);
        check(n == ARKRTOS_MQ_ERR_CONTEXT, "a send that cannot block is refused");
    }
    check(g_block_steps == 5, "every attempt really reached the blocking call");
    check(arkrtos_mq_send_from_isr_internal(h3, in, 1) == -1,
          "nothing was delivered by the refused sends");

    /* B. 被唤醒时空间又被别人占走：按剩余时间重新排队，最终超时 */
    g_block_mode  = 2;
    g_block_steps = 0;
    in[0] = 0x6666u;
    n = arkrtos_mq_send_internal(h3, in, 1, 100);
    check(n == ARKRTOS_MQ_ERR_TIMEOUT, "a wake-up that finds no space times out");
    check(g_block_steps == 2, "the sender re-queued itself exactly once");

    /* C. 被唤醒时空间已腾出：消息投递成功，且先进先出 */
    g_block_mode       = 3;
    g_block_steps      = 0;
    g_block_free_handle = h3;
    in[0] = 0x7777u;
    n = arkrtos_mq_send_internal(h3, in, 1, 100);
    check(n == 0, "a wake-up with free space delivers the message");
    check(g_block_steps == 1, "the sender blocked exactly once");
    n = arkrtos_mq_recv_internal(h3, out, 1, 0);
    check((n == 1) && (out[0] == (0x6000u + 1u)),
          "the message that was freed first is the one that left first");
    g_block_free_handle = -1;
    check(arkrtos_mq_send_internal(h3, in, 1, 0) == 0,
          "the queue is filled again before the next case");

    /* D. 剩余时间已用尽：不再排队，直接报超时 */
    g_block_mode  = 4;
    g_block_steps = 0;
    n = arkrtos_mq_send_internal(h3, in, 1, 100);
    check(n == ARKRTOS_MQ_ERR_TIMEOUT, "an exhausted deadline ends the retry loop");
    check(g_block_steps == 1, "no second block happens once the deadline is gone");
    arkrtos_kernel_tick = 0u;

    /* E. 等待期间队列被删除 */
    g_block_mode  = 5;
    g_block_steps = 0;
    n = arkrtos_mq_send_internal(h3, in, 1, 100);
    check(n == ARKRTOS_MQ_ERR_DELETED, "a queue deleted while waiting fails the send");

    /* F. 用户态轮询（处在 handler 里）：只登记不睡，报 would-block */
    g_block_mode  = 6;
    g_in_handler  = 1u;
    n = arkrtos_mq_send_internal(h3, in, 1, 100);
    check(n == ARKRTOS_SYNC_ERR_WOULDBLOCK, "a user-mode poll reports would-block");
    g_in_handler = 0u;

    g_cur_task    = 0;
    g_block_mode  = 0;
    g_block_steps = 0;
    n = arkrtos_mq_delete_internal(h3);
    check(n == 0, "the blocking-path queue deletes cleanly");

    /* ---- 11. 阻塞接收：登记、重排队、消息到达、超时用尽、对象删除、用户态轮询 ---- */
    h4 = arkrtos_mq_create_internal("mq4");
    check(h4 >= 0, "a fourth queue is created for the blocking receive paths");

    /* A0. 队列空且不等：走「瞬时失败」分支（当前任务为 0，没有可撤的登记） */
    n = arkrtos_mq_recv_internal(h4, out, 1, 0);
    check(n == ARKRTOS_MQ_ERR_EMPTY, "an empty queue refuses a non-blocking receive");

    /* A1. timeout>0 但没有当前任务：按上下文错误收尾 */
    n = arkrtos_mq_recv_internal(h4, out, 1, 100);
    check(n == ARKRTOS_MQ_ERR_CONTEXT, "a receive without a current task is refused");

    /* B. 有任务但进不了阻塞（block 返回负值）：撤销登记、按上下文错误收尾。
     *    连做 5 次都必须是同一结论——若登记没被撤销，第二次起会撞上重复登记。 */
    g_cur_task    = &g_fake_task;
    g_block_mode  = 0;
    g_block_steps = 0;
    for(i = 0; i < 5; i++)
    {
        n = arkrtos_mq_recv_internal(h4, out, 1, 100);
        check(n == ARKRTOS_MQ_ERR_CONTEXT, "a receive that cannot block is refused");
    }
    check(g_block_steps == 5, "every receive attempt really reached the blocking call");
    check(arkrtos_mq_send_internal(h4, in, 1, 0) == 0,
          "the refused receives left the queue usable");
    check(arkrtos_mq_recv_internal(h4, out, 1, 0) == 1,
          "the message put in afterwards comes straight back out");

    /* C. 被唤醒时仍然没有消息：按剩余时间重新排队，最终超时 */
    g_block_mode  = 2;
    g_block_steps = 0;
    n = arkrtos_mq_recv_internal(h4, out, 1, 100);
    check(n == ARKRTOS_SYNC_ERR_TIMEOUT, "a wake-up that finds no message times out");
    check(g_block_steps == 2, "the receiver re-queued itself exactly once");

    /* D. 被唤醒时消息已到：取走它，且只睡了一次 */
    g_block_mode        = 7;
    g_block_steps       = 0;
    g_block_free_handle = h4;
    g_block_free_out[0] = 0x8888u;
    n = arkrtos_mq_recv_internal(h4, out, 1, 100);
    check((n == 1) && (out[0] == 0x8888u),
          "a wake-up with a message takes that message");
    check(g_block_steps == 1, "the receiver blocked exactly once");
    g_block_free_handle = -1;

    /* E. 剩余时间已用尽：不再排队，直接报超时 */
    g_block_mode  = 4;
    g_block_steps = 0;
    n = arkrtos_mq_recv_internal(h4, out, 1, 100);
    check(n == ARKRTOS_SYNC_ERR_TIMEOUT, "an exhausted deadline ends the receive retry loop");
    check(g_block_steps == 1, "no second block happens once the receive deadline is gone");
    arkrtos_kernel_tick = 0u;

    /* F. 等待期间队列被删除 */
    g_block_mode  = 5;
    g_block_steps = 0;
    n = arkrtos_mq_recv_internal(h4, out, 1, 100);
    check(n == ARKRTOS_MQ_ERR_DELETED, "a queue deleted while waiting fails the receive");

    /* G. 用户态轮询（处在 handler 里）：只登记不睡，报 would-block */
    g_block_mode = 6;
    g_in_handler = 1u;
    n = arkrtos_mq_recv_internal(h4, out, 1, 100);
    check(n == ARKRTOS_SYNC_ERR_WOULDBLOCK, "a user-mode receive reports would-block");
    g_in_handler = 0u;

    g_cur_task    = 0;
    g_block_mode  = 0;
    g_block_steps = 0;
    n = arkrtos_mq_delete_internal(h4);
    check(n == 0, "the blocking-receive queue deletes cleanly");


    /* ---- 12..23 \u7b2c\u4e8c\u6279\uff1a\u6a21\u5757\u521d\u59cb\u5316\u3001\u4e0b\u7ebf\u6536\u5c38\u3001\u53e5\u67c4\u95f8\u95e8\u4e0e\u91cd\u8bd5\u5faa\u73af\u7684\u9632\u5fa1\u5206\u652f ---- */
    {
        int32 hq;
        int32 hk;
        int32 he;
        int32 hs[ARKRTOS_MQ_NUM];
        int32 j;

        /* 12. \u6a21\u5757\u521d\u59cb\u5316\uff1a\u6574\u5f20\u8868\u56de\u5230\u521d\u6001 */
        hq = arkrtos_mq_create_internal("reinit");
        check(hq >= 0, "a queue is created before re-initialising the module");
        check(arkrtos_mq_send_internal(hq, in, 1, 0) == 0,
              "the queue holds one message before init");
        arkrtos_mq_module_init();
        check(arkrtos_mq_is_alive(hq) == 0, "module init clears every queue slot");
        hq = arkrtos_mq_create_internal("reinit2");
        check(hq == (0 | ARKRTOS_MQ_HANDLE_FLAG),
              "after init the lowest slot is free again");
        (void)arkrtos_mq_delete_internal(hq);

        /* 13. \u5185\u6838\u961f\u5217\u4e0d\u968f\u521b\u5efa\u8005\u4e0b\u7ebf\u6d88\u5931\uff1b\u666e\u901a\u961f\u5217\u968f\u4e4b\u4e0b\u7ebf */
        arkrtos_task_count      = 6;
        arkrtos_current_task_id = 2;
        hq = arkrtos_mq_create_internal("taskq");
        hk = arkrtos_mq_create_kernel_internal("kernq");
        check((hq >= 0) && (hk >= 0) && (hq != hk),
              "a task queue and a kernel queue live side by side");
        check(arkrtos_mqs[hq & (int32)ARKRTOS_HANDLE_RELMASK].creator_id == 2,
              "the task queue remembers its creator");
        check(arkrtos_mqs[hk & (int32)ARKRTOS_HANDLE_RELMASK].creator_id == 0,
              "the kernel queue belongs to the kernel");
        arkrtos_mqs[hq & (int32)ARKRTOS_HANDLE_RELMASK].send_waiters[0] =
            &arkrtos_task_table[1];
        arkrtos_mqs[hk & (int32)ARKRTOS_HANDLE_RELMASK].recv_waiters[0] =
            &arkrtos_task_table[1];
        arkrtos_mq_release_task(2);
        check(arkrtos_mq_is_alive(hq) == 0, "the dead task's queue is collected");
        check(arkrtos_mq_is_alive(hk) == 1, "the kernel queue survives the teardown");
        check(arkrtos_mqs[hk & (int32)ARKRTOS_HANDLE_RELMASK].recv_waiters[0] == 0,
              "the dead task is detached from the live queue's wait list");

        /* 14. \u574f\u4efb\u52a1\u53f7\u76f4\u63a5\u8fd4\u56de\uff1b\u4e0b\u7ebf\u9014\u4e2d\u7684\u4efb\u52a1\u4e0d\u56de\u5c31\u7eea\u961f\u5217 */
        arkrtos_mq_release_task(0);
        arkrtos_mq_release_task(arkrtos_task_count + 1);
        check(arkrtos_mq_is_alive(hk) == 1, "an out-of-range task id is ignored");
        arkrtos_task_table[3].status = ARKRTOS_TASK_INVALID;
        arkrtos_mqs[hk & (int32)ARKRTOS_HANDLE_RELMASK].send_waiters[0] =
            &arkrtos_task_table[3];
        j = g_ready_calls;
        check(arkrtos_mq_delete_internal(hk) == 0, "deleting the kernel queue succeeds");
        check(arkrtos_task_table[3].status == ARKRTOS_TASK_INVALID,
              "a task being torn down is not re-queued by wake_all");
        check(g_ready_calls == j,
              "wake_all dropped the invalid waiter instead of waking it");
        arkrtos_task_table[3].status = ARKRTOS_TASK_READY;
        arkrtos_task_count      = 0;
        arkrtos_current_task_id = 0;

        /* 15. \u53e5\u67c4\u95f8\u95e8\uff1aISR \u4e24\u4e2a\u5165\u53e3\u3001\u516c\u5f00\u5305\u88c5\u3001delete\u3001is_alive\u3001recv */
        hq = arkrtos_mq_create_internal("gates");
        check(hq >= 0, "a queue is created for the handle gates");
        check(arkrtos_mq_send_from_isr_internal(hq + 0x00100000, in, 1) == -1,
              "send_from_isr rejects a wrong type flag");
        check(arkrtos_mq_send_from_isr_internal(hq + (int32)ARKRTOS_MQ_NUM, in, 1) == -1,
              "send_from_isr rejects an out-of-range index");
        check(arkrtos_mq_send_from_isr_internal(hq, 0, 1) == -1,
              "send_from_isr rejects a null buffer");
        check(arkrtos_mq_send_from_isr_internal(hq, in, 0) == -1,
              "send_from_isr rejects a zero length");
        check(arkrtos_mq_recv_from_isr_internal(hq + 0x00100000, out, 1) == -1,
              "recv_from_isr rejects a wrong type flag");
        check(arkrtos_mq_recv_from_isr_internal(hq + (int32)ARKRTOS_MQ_NUM, out, 1) == -1,
              "recv_from_isr rejects an out-of-range index");
        check(arkrtos_mq_recv_from_isr_internal(hq, 0, 1) == -1,
              "recv_from_isr rejects a null buffer");
        check(arkrtos_mq_recv_from_isr_internal(hq, out, 0) == -1,
              "recv_from_isr rejects a zero length");
        check(arkrtos_mq_send_from_isr(hq, in, 1) == 0,
              "the public ISR send wrapper delivers");
        check(arkrtos_mq_recv_from_isr(hq, out, 1) == 1,
              "the public ISR receive wrapper takes it back");
        check(arkrtos_mq_recv_internal(hq + 0x00100000, out, 1, 0) == -1,
              "recv rejects a wrong type flag");
        check(arkrtos_mq_delete_internal(hq + 0x00100000) == -1,
              "delete rejects a wrong type flag");
        check(arkrtos_mq_delete_internal(hq + (int32)ARKRTOS_MQ_NUM) == -1,
              "delete rejects an out-of-range index");
        check(arkrtos_mq_is_alive(hq + 0x00100000) == 0,
              "is_alive rejects a wrong type flag");
        check(arkrtos_mq_is_alive(hq + (int32)ARKRTOS_MQ_NUM) == 0,
              "is_alive rejects an out-of-range index");

        /* 16. \u7b49\u5f85\u8868\u6ee1\uff1a\u7b2c\u4e00\u6b21\u767b\u8bb0\u5c31\u62a5 NOSPACE\uff0c\u4e0d\u7761 */
        for(j = 0; j < (int32)ARKRTOS_MQ_DEPTH; j++)
        {
            in[0] = 0x9100u + (uint32)j;
            check(arkrtos_mq_send_internal(hq, in, 1, 0) == 0, "the gate queue is filled");
        }
        g_cur_task    = &g_fake_task;
        g_block_mode  = 0;
        g_block_steps = 0;
        for(j = 0; j < (int32)ARKRTOS_WQ_SLOTS; j++)
        {
            arkrtos_mqs[hq & (int32)ARKRTOS_HANDLE_RELMASK].send_waiters[j] =
                &arkrtos_task_table[10 + j];
        }
        check(arkrtos_mq_send_internal(hq, in, 1, 100) == ARKRTOS_MQ_ERR_NOSPACE,
              "a full send-waiter table is reported as NOSPACE");
        check(g_block_steps == 0, "a send that cannot register never blocks");
        for(j = 0; j < (int32)ARKRTOS_WQ_SLOTS; j++)
        {
            arkrtos_mqs[hq & (int32)ARKRTOS_HANDLE_RELMASK].send_waiters[j] = 0;
        }

        he = arkrtos_mq_create_internal("emptyq");
        check(he >= 0, "an empty queue is created for the receive paths");
        g_block_steps = 0;
        for(j = 0; j < (int32)ARKRTOS_WQ_SLOTS; j++)
        {
            arkrtos_mqs[he & (int32)ARKRTOS_HANDLE_RELMASK].recv_waiters[j] =
                &arkrtos_task_table[10 + j];
        }
        check(arkrtos_mq_recv_internal(he, out, 1, 100) == ARKRTOS_MQ_ERR_NOSPACE,
              "a full recv-waiter table is reported as NOSPACE");
        check(g_block_steps == 0, "a receive that cannot register never blocks");
        for(j = 0; j < (int32)ARKRTOS_WQ_SLOTS; j++)
        {
            arkrtos_mqs[he & (int32)ARKRTOS_HANDLE_RELMASK].recv_waiters[j] = 0;
        }

        /* 17. \u6ee1\u961f\u5217 + \u65e0\u5f53\u524d\u4efb\u52a1\uff1aCONTEXT\uff08\u4e0d\u767b\u8bb0\u3001\u4e0d\u7761\uff09 */
        g_cur_task = 0;
        check(arkrtos_mq_send_internal(hq, in, 1, 100) == ARKRTOS_MQ_ERR_CONTEXT,
              "a blocking send without a current task is refused");

        /* 18. \u7b2c\u4e00\u6b21\u963b\u585e\u5c31\u8d85\u65f6\uff1a\u76f4\u63a5\u6536\u5c3e */
        g_cur_task    = &g_fake_task;
        g_block_mode  = 10;
        g_block_steps = 0;
        check(arkrtos_mq_send_internal(hq, in, 1, 100) == ARKRTOS_MQ_ERR_TIMEOUT,
              "a first-block timeout ends the send");
        check(g_block_steps == 1, "the timeout is classified right after the first block");
        check(arkrtos_mq_recv_internal(he, out, 1, 100) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "a first-block timeout ends the receive");

        /* 19. \u65e0\u9650\u7b49\u5f85\uff08timeout<0\uff09\u5728\u91cd\u8bd5\u5faa\u73af\u91cc\u62ff\u4e0d\u5230\u7a7a\u95f4/\u6d88\u606f */
        g_block_mode  = 8;
        g_block_steps = 0;
        check(arkrtos_mq_send_internal(hq, in, 1, -1) == ARKRTOS_MQ_ERR_TIMEOUT,
              "an infinite send gives up when the second wake-up is a timeout");
        check(g_block_steps == 2, "the infinite send re-queued itself once");
        g_block_steps = 0;
        check(arkrtos_mq_recv_internal(he, out, 1, -1) == ARKRTOS_SYNC_ERR_TIMEOUT,
              "an infinite receive gives up when the second wake-up is a timeout");
        check(g_block_steps == 2, "the infinite receive re-queued itself once");

        /* 20. \u91cd\u8bd5\u7a97\u53e3\u91cc"\u6ca1\u80fd\u7761\u4e0b"\uff1a\u6309\u5bf9\u8c61\u6d88\u5931\u6536\u5c3e */
        g_block_mode  = 9;
        g_block_steps = 0;
        check(arkrtos_mq_send_internal(hq, in, 1, 100) == ARKRTOS_MQ_ERR_DELETED,
              "a send retry that cannot block ends as deleted");
        g_block_steps = 0;
        check(arkrtos_mq_recv_internal(he, out, 1, 100) == ARKRTOS_MQ_ERR_DELETED,
              "a receive retry that cannot block ends as deleted");

        /* 21. \u91cd\u8bd5\u7a97\u53e3\u91cc\u7b49\u5f85\u8868\u88ab\u522b\u4eba\u5360\u6ee1\uff1aNOSPACE */
        g_block_mode  = 11;
        g_block_steps = 0;
        g_block_mq    = &arkrtos_mqs[hq & (int32)ARKRTOS_HANDLE_RELMASK];
        check(arkrtos_mq_send_internal(hq, in, 1, 100) == ARKRTOS_MQ_ERR_NOSPACE,
              "a send retry that finds the waiter table full reports NOSPACE");
        check(g_block_steps == 1, "the NOSPACE is decided before the second block");
        for(j = 0; j < (int32)ARKRTOS_WQ_SLOTS; j++)
        {
            arkrtos_mqs[hq & (int32)ARKRTOS_HANDLE_RELMASK].send_waiters[j] = 0;
        }
        g_block_mode  = 12;
        g_block_steps = 0;
        g_block_mq    = &arkrtos_mqs[he & (int32)ARKRTOS_HANDLE_RELMASK];
        check(arkrtos_mq_recv_internal(he, out, 1, 100) == ARKRTOS_MQ_ERR_NOSPACE,
              "a receive retry that finds the waiter table full reports NOSPACE");
        for(j = 0; j < (int32)ARKRTOS_WQ_SLOTS; j++)
        {
            arkrtos_mqs[he & (int32)ARKRTOS_HANDLE_RELMASK].recv_waiters[j] = 0;
        }

        /* 22. \u7528\u6237\u6001\u8f6e\u8be2\u7559\u4e0b\u7684\u767b\u8bb0\uff0c\u88ab timeout 0 \u7684\u6536\u5c3e\u64a4\u6389 */
        g_block_mode  = 6;
        g_block_steps = 0;
        g_block_mq    = 0;
        g_in_handler  = 1u;
        check(arkrtos_mq_send_internal(hq, in, 1, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
              "a user-mode send poll reports would-block");
        check(arkrtos_mqs[hq & (int32)ARKRTOS_HANDLE_RELMASK].send_waiters[0] == &g_fake_task,
              "the send poll left its registration behind");
        check(arkrtos_mq_send_internal(hq, in, 1, 0) == ARKRTOS_MQ_ERR_FULL,
              "the non-blocking send reports the queue is full");
        check(arkrtos_mqs[hq & (int32)ARKRTOS_HANDLE_RELMASK].send_waiters[0] == 0,
              "the non-blocking send removed its stale registration");
        g_block_steps = 0;
        check(arkrtos_mq_recv_internal(he, out, 1, 100) == ARKRTOS_SYNC_ERR_WOULDBLOCK,
              "a user-mode receive poll reports would-block");
        check(arkrtos_mqs[he & (int32)ARKRTOS_HANDLE_RELMASK].recv_waiters[0] == &g_fake_task,
              "the receive poll left its registration behind");
        check(arkrtos_mq_recv_internal(he, out, 1, 0) == ARKRTOS_MQ_ERR_EMPTY,
              "the non-blocking receive reports the queue is empty");
        check(arkrtos_mqs[he & (int32)ARKRTOS_HANDLE_RELMASK].recv_waiters[0] == 0,
              "the non-blocking receive removed its stale registration");
        g_in_handler  = 0u;
        g_cur_task    = 0;
        g_block_mode  = 0;
        g_block_steps = 0;

        check(arkrtos_mq_delete_internal(hq) == 0, "the gate queue deletes cleanly");
        check(arkrtos_mq_delete_internal(he) == 0, "the empty queue deletes cleanly");

        /* 23. \u961f\u5217\u8868\u6ee1\uff1a\u4e24\u4e2a\u521b\u5efa\u5165\u53e3\u90fd\u62a5 NOSLOT */
        for(j = 0; j < (int32)ARKRTOS_MQ_NUM; j++)
        {
            hs[j] = arkrtos_mq_create_internal("fill");
            check(hs[j] >= 0, "the queue table fills up");
        }
        check(arkrtos_mq_create_internal("over") == ARKRTOS_MQ_ERR_NOSLOT,
              "creating past the table size reports NOSLOT");
        check(arkrtos_mq_create_kernel_internal("over") == ARKRTOS_MQ_ERR_NOSLOT,
              "the kernel create entry reports NOSLOT too");
        for(j = 0; j < (int32)ARKRTOS_MQ_NUM; j++)
        {
            (void)arkrtos_mq_delete_internal(hs[j]);
        }
    }
    if(g_fail != 0)
    {
        printf("MQ-HOST FAIL: %d of %d checks failed\n", g_fail, g_checks);
        return 1;
    }
    printf("MQ-HOST OK (%d checks)\n", g_checks);
    return 0;
}
