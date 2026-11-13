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

/* 恒返回 0：阻塞路径因此走不到。这不是"覆盖了阻塞"，是刻意不覆盖。 */
arkrtos_task_t *arkrtos_task_get_current(void) { return 0; }
int32 arkrtos_task_block_in_critical(uint32 timeout_ms) { (void)timeout_ms; return -1; }
void  arkrtos_ready_add(int32 task_idx) { (void)task_idx; }
uint8 arkrtos_sync_in_handler(void) { return 0u; }

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

    if(g_fail != 0)
    {
        printf("MQ-HOST FAIL: %d of %d checks failed\n", g_fail, g_checks);
        return 1;
    }
    printf("MQ-HOST OK (%d checks)\n", g_checks);
    return 0;
}
