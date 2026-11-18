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
*            未验证：阻塞与唤醒（队列满时睡下、被 recv 唤醒、超时、删除时唤醒等待者）
*                    需要真实调度器与真实节拍，这里 task_get_current() 恒返回 0，
*                    timeout>0 的分支走不到——那部分只有上板用例能证明。
* @note 用法：由 tools/mq_host_gate.py 编译并运行，输出 MQ-HOST OK。
*/

#include "arkrtos_mq.h"
#include "arkrtos_cfg.h"
#include "arkrtos.h"   /* arkrtos_mq_recv_from_isr 的公开声明在这里（mq.h 只有 *_internal） */

#include <stdio.h>
#include <string.h>

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
 * 因此前面八段用例的路径完全不变；只有第 10 段才临时打开 g_block_mode。
 *   mode 0 = 恒失败（默认）
 *   mode 2 = 第一次"令牌交接"、第二次超时
 *   mode 3 = 先从 g_block_free_handle 收走一条腾出空间，再返回"令牌交接"
 *   mode 4 = 返回"令牌交接"前把节拍推过超时上限
 *   mode 5 = 返回"等待对象被删除"
 *   mode 6 = 由 g_in_handler 声明"处在用户态轮询里" */
static arkrtos_task_t  g_fake_task;
static arkrtos_task_t *g_cur_task         = 0;
static int32  g_block_mode       = 0;
static int32  g_block_steps      = 0;
static int32  g_block_free_handle = -1;
static uint8  g_in_handler        = 0u;
static uint32 g_block_free_out[ARKRTOS_MQ_MSG_WORDS];

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
    return -1;
}
void  arkrtos_ready_add(int32 task_idx) { (void)task_idx; }
uint8 arkrtos_sync_in_handler(void) { return g_in_handler; }

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

    if(g_fail != 0)
    {
        printf("MQ-HOST FAIL: %d of %d checks failed\n", g_fail, g_checks);
        return 1;
    }
    printf("MQ-HOST OK (%d checks)\n", g_checks);
    return 0;
}
