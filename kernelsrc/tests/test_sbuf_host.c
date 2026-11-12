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
*                    删除后句柄失效。
*            未验证：阻塞/唤醒路径需要真实调度器，这里 task_get_current()
*                    返回 0，所以 timeout>0 的分支不会被走到——那部分只有
*                    上板用例能证明。
* @note 用法：由 tools/sbuf_host_gate.py 编译并运行，输出 SBUF-HOST OK。
*/

#include "arkrtos_sbuf.h"
#include "arkrtos_cfg.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 内核桩：只提供链接所需的符号，行为尽量"什么都不做"
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

arkrtos_task_t *arkrtos_task_get_current(void) { return 0; }
int32 arkrtos_task_block_in_critical(uint32 timeout_ms) { (void)timeout_ms; return -1; }
void  arkrtos_ready_add(int32 task_idx) { (void)task_idx; }
uint8 arkrtos_sync_in_handler(void) { return 0u; }

/* trace 模块：宿主上没有这条链路 */
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

    if(g_fail == 0)
    {
        fputs("SBUF-HOST OK\n", stdout);
        return 0;
    }
    printf("SBUF-HOST FAILED (%d/%d)\n", g_fail, g_checks);
    return 1;
}
