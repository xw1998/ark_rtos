/**
* @file arkrtos_shell_cfg.c
* @brief 配置：pool / cfg。池容量读数与设备端 CONFIG 区的读写。
* @note 本文件属于内核特权态代码；由 arkrtos_shell.c 按命令族拆出，
*       只做搬运，命令逻辑与拆分前逐字相同。
*/

#include "arkrtos_shell_internal.h"

#if (SHELL_ENABLE == 1)

/* ============================================================
 * pool：池内还能装下多少（与 SVC 0x12 子命令 5 同一数据源）
 * ============================================================ */
#if ARKRTOS_USE_MEMPOOL

#define MPOOL_TEST_BLOCKS      (4u)
#define MPOOL_TEST_BLOCK_SIZE  (32u)

static int mpool_show_all(void)
{
    int32 i;
    int   n = 0;
    arkrtos_mempool_stat_t st;

    ark_shell_printf("\r\nmemory pools (fixed-block)\r\n");
    for(i = 0; i < ARKRTOS_MEMPOOL_NUM; i++)
    {
        if(arkrtos_mempool_is_alive(i) == 0)
        {
            continue;
        }
        if(arkrtos_mempool_query_internal(i, &st) != 0)
        {
            continue;
        }
        n++;
        ark_shell_printf("  #%d %-8s block %u B x %u  free %u  used %u (peak %u)"
                         "  alloc %u  fail %u  bad-free %u\r\n",
                         (int)i, st.name,
                         (unsigned)st.block_size, (unsigned)st.block_count,
                         (unsigned)st.free_count,
                         (unsigned)(st.block_count - st.free_count),
                         (unsigned)st.peak_used,
                         (unsigned)st.alloc_ok, (unsigned)st.alloc_fail,
                         (unsigned)st.free_bad);
    }
    if(n == 0)
    {
        ark_shell_printf("  (no pool)\r\n");
    }
    return 0;
}

static int mpool_selftest(void)
{
    static uint8 buf[MPOOL_TEST_BLOCKS * MPOOL_TEST_BLOCK_SIZE];
    void  *blk[MPOOL_TEST_BLOCKS + 1];
    int32  h = -1;
    uint32 i;
    int    checks = 0;
    int    fails  = 0;
    arkrtos_mempool_stat_t st;

    ark_shell_printf("\r\nmempool selftest (fixed-block pool)\r\n");

    for(i = 0; i < (MPOOL_TEST_BLOCKS + 1u); i++)
    {
        blk[i] = 0;
    }

    checks++;
    h = arkrtos_mempool_create_internal("mptest", buf,
                                        MPOOL_TEST_BLOCK_SIZE, MPOOL_TEST_BLOCKS);
    if(h < 0) { fails++; ark_shell_printf("  FAIL create\r\n"); }
    else      { ark_shell_printf("  ok   create\r\n"); }

    if(h < 0)
    {
        goto done;
    }

    for(i = 0; i < MPOOL_TEST_BLOCKS; i++)
    {
        blk[i] = arkrtos_mempool_alloc_internal(h);
    }
    checks++;
    for(i = 0; i < MPOOL_TEST_BLOCKS; i++)
    {
        if(blk[i] == 0) { break; }
    }
    if(i < MPOOL_TEST_BLOCKS)
    { fails++; ark_shell_printf("  FAIL alloc all (%u/%u)\r\n", (unsigned)i, (unsigned)MPOOL_TEST_BLOCKS); }
    else
    { ark_shell_printf("  ok   alloc all %u blocks\r\n", (unsigned)MPOOL_TEST_BLOCKS); }

    checks++;
    blk[MPOOL_TEST_BLOCKS] = arkrtos_mempool_alloc_internal(h);
    if(blk[MPOOL_TEST_BLOCKS] != 0)
    { fails++; ark_shell_printf("  FAIL exhaust (empty pool handed out a block)\r\n"); }
    else
    { ark_shell_printf("  ok   exhaust -> 0\r\n"); }

    checks++;
    if(arkrtos_mempool_query_internal(h, &st) != 0)
    { fails++; ark_shell_printf("  FAIL query\r\n"); }
    else if((st.free_count != 0u) || (st.alloc_fail != 1u))
    { fails++; ark_shell_printf("  FAIL stats free=%u fail=%u\r\n", (unsigned)st.free_count, (unsigned)st.alloc_fail); }
    else
    { ark_shell_printf("  ok   stats free=0 fail=1\r\n"); }

    checks++;
    if(arkrtos_mempool_free_internal(h, blk[0]) != 0)
    { fails++; ark_shell_printf("  FAIL free\r\n"); }
    else if(arkrtos_mempool_alloc_internal(h) != blk[0])
    { fails++; ark_shell_printf("  FAIL reuse (did not hand back the same block)\r\n"); }
    else
    { ark_shell_printf("  ok   free + reuse same block\r\n"); }

    checks++;
    if(arkrtos_mempool_free_internal(h, (void *)(buf + sizeof(buf))) == 0)
    { fails++; ark_shell_printf("  FAIL reject out-of-range free\r\n"); }
    else if(arkrtos_mempool_free_internal(h, (void *)(buf + 1u)) == 0)
    { fails++; ark_shell_printf("  FAIL reject misaligned free\r\n"); }
    else
    { ark_shell_printf("  ok   reject bad free\r\n"); }

    checks++;
    if(arkrtos_mempool_create_internal("mptest", buf,
                                       MPOOL_TEST_BLOCK_SIZE, MPOOL_TEST_BLOCKS) >= 0)
    { fails++; ark_shell_printf("  FAIL reject duplicate name\r\n"); }
    else
    { ark_shell_printf("  ok   reject duplicate name\r\n"); }

    checks++;
    if(arkrtos_mempool_create_internal("mpbad", buf, 1u, 4u) >= 0)
    { fails++; ark_shell_printf("  FAIL reject tiny block\r\n"); }
    else
    { ark_shell_printf("  ok   reject tiny block\r\n"); }

done:
    if(h >= 0)
    {
        for(i = 0; i < MPOOL_TEST_BLOCKS; i++)
        {
            if(blk[i] != 0) { (void)arkrtos_mempool_free_internal(h, blk[i]); blk[i] = 0; }
        }
        checks++;
        if(arkrtos_mempool_delete_internal(h) != 0)
        { fails++; ark_shell_printf("  FAIL delete\r\n"); }
        else if(arkrtos_mempool_is_alive(h) != 0)
        { fails++; ark_shell_printf("  FAIL alive after delete\r\n"); }
        else
        { ark_shell_printf("  ok   delete\r\n"); }
    }

    ark_shell_printf("\r\nmempool selftest: checks %d, failed %d -- %s\r\n",
                     checks, fails, (fails == 0) ? "ALL PASS" : "FAIL");
    return (fails == 0) ? 0 : -1;
}

int cmd_mpool(int argc, char *argv[])
{
    const char *sub = (argc >= 2) ? argv[1] : "list";

    if(strcmp(sub, "list") == 0) { return mpool_show_all(); }
    if(strcmp(sub, "test") == 0) { return mpool_selftest(); }

    ark_shell_printf("\r\nusage: mpool [list | test]\r\n");
    return -1;
}

#endif /* ARKRTOS_USE_MEMPOOL */

/* ============================================================
 * cfg：设备端布局配置区（CONFIG 区）
 *
 * 记录格式见 kernelsrc/include/arkrtos_layout_def.h（与上位机工具共用），
 * 生成工具 tools/arkrtos_layout.py，串口下发工具 tools/arkrtos_cfg.py。
 *
 * 上位机协议（与 install 同构：窗口期间本任务不跑 ark_shell_run()，
 * 串口读权全部交给本命令，不存在两个读者分掉同一 FIFO 的问题）：
 *   1) 设备打印就绪行；
 *   2) 主机按 ARKRTOS_CFG_CHUNK_SIZE 字节分块下行，每块发完等一个流控字节：
 *      ACK(0x06) 表示「已收下，发下一块」，NAK(0x15) 表示「到此为止」；
 *   3) 第 4 块收满后设备校验并写入：成功发 ACK，失败先打印原因行再发 NAK。
 * 用二进制流而不是 hex 文本，是因为 ARK_SHELL_LINE_SIZE 只有 128 字节，
 * 512 字节的记录没法走命令行文本（见 ark_shell_config.h）。
 * ============================================================ */

#define ARKRTOS_CFG_CHUNK_SIZE     (128u)
#define ARKRTOS_CFG_CHUNK_COUNT    (ARKRTOS_CFG_RECORD_SIZE / ARKRTOS_CFG_CHUNK_SIZE)
#define ARKRTOS_CFG_CHUNK_TIMEOUT  (5000u)   /* 单块等待上限（ms） */
#define ARKRTOS_CFG_POLL_MS        (2u)      /* 空转一次让出 CPU 的步长（ms） */
#define ARKRTOS_CFG_FLOW_ACK       (0x06u)
#define ARKRTOS_CFG_FLOW_NAK       (0x15u)

/* 收满 len 字节；idle 累计到 timeout_ms 仍没收满就放弃。
 * 未收满时已收的字节就地丢弃（记录要么整条有效，要么整条不要）。 */
int32 cfg_recv_exact(int32 dev, uint8 *buf, uint32 len, uint32 timeout_ms)
{
    uint32 got = 0u;
    uint32 idle = 0u;

    while(got < len)
    {
        int32 r = arkrtos_dev_read_internal(dev, buf + got, (int32)(len - got));

        if(r > 0)
        {
            got += (uint32)r;
            idle = 0u;
            continue;
        }

        if(idle >= timeout_ms)
        {
            return -1;
        }

        arkrtos_task_wait_internal(ARKRTOS_CFG_POLL_MS);
        idle += ARKRTOS_CFG_POLL_MS;
    }

    return 0;
}

static void cfg_send_flow(int32 dev, uint8 flow)
{
    uint8 b = flow;

    (void)arkrtos_dev_write_internal(dev, &b, 1);
}

static int cmd_cfg_show(void)
{
    const arkrtos_cfg_slot_t *slots = arkrtos_layout_slots();
    uint32 reason = arkrtos_layout_reason();
    uint32 count  = arkrtos_layout_slot_count();
    uint32 i;

    ark_shell_printf("\r\nlayout    : %s (source: %s)\r\n",
                     cfg_mode_text(arkrtos_layout_mode()),
                     cfg_source_text(arkrtos_layout_source()));
    ark_shell_printf("config    : 0x%08X +%uK, %u record(s) per sector\r\n",
                     (unsigned)CONFIG_BASE,
                     (unsigned)(CONFIG_SIZE / 1024u),
                     (unsigned)(CONFIG_SIZE / ARKRTOS_CFG_RECORD_SIZE));
    ark_shell_printf("reclaim   : %s\r\n", cfg_reclaim_text(arkrtos_layout_reclaim_mode()));
    ark_shell_printf("knobs     : log %u (record %u), restart max %u\r\n",
                     arkrtos_log_get_level(),
                     arkrtos_layout_cfg_log_level(),
                     arkrtos_layout_cfg_restart_max());
    ark_shell_printf("boot      : hold %u ms before autostart, raw images %s\r\n",
                     arkrtos_layout_boot_delay_ms(),
                     (arkrtos_layout_raw_allow() != 0u) ? "allowed" : "refused");

    if(reason != ARKRTOS_CFG_OK)
    {
        ark_shell_printf("record    : rejected, %s\r\n", arkrtos_layout_reason_text(reason));
    }
    else if(arkrtos_layout_source() == ARKRTOS_LAYOUT_SOURCE_CONFIG)
    {
        sh_out("record    : accepted\r\n");
    }
    else
    {
        sh_out("record    : none, compile-time default in use\r\n");
    }

    if(count == 0u)
    {
        return 0;
    }

    sh_out("\r\nslot  type  base        size     ram         auto\r\n");

    for(i = 0u; i < count; i++)
    {
        ark_shell_printf(" %-3u  %-4s  0x%08X  %-7u  0x%08X  %u\r\n",
                         (unsigned)i,
                         cfg_type_text(slots[i].type),
                         (unsigned)slots[i].base,
                         (unsigned)slots[i].size,
                         (unsigned)slots[i].ram_base,
                         (unsigned)slots[i].autostart);
    }

    return 0;
}

static int cmd_cfg_load(void)
{
    static uint8 rec[ARKRTOS_CFG_RECORD_SIZE];
    int32 dev;
    uint32 i;
    uint32 erased = 0u;

    dev = arkrtos_shell_uart_open();

    /* Drop whatever the console still holds before the handshake starts.
     * The shell ends a command line on CR, so a CRLF terminator leaves the
     * LF behind; a stray byte left here would shift the whole record by one
     * and the record would only fail its magic check at the very end.
     * Draining before the ready line is printed (not after) keeps the
     * host's first chunk, which is sent once it sees that line, intact. */
    {
        uint8 sink[32];
        uint32 spins = 0u;

        while((spins < 64u) && (arkrtos_dev_read_internal(dev, sink, (int32)sizeof(sink)) > 0))
        {
            spins++;
        }
    }

    ark_shell_printf("\r\ncfg load: ready, send %u bytes as %u chunks of %u\r\n",
                     (unsigned)ARKRTOS_CFG_RECORD_SIZE,
                     (unsigned)ARKRTOS_CFG_CHUNK_COUNT,
                     (unsigned)ARKRTOS_CFG_CHUNK_SIZE);
    sh_out("cfg load: one flow byte per chunk (0x06 = next, 0x15 = stop)\r\n");

    /* 收块阶段串口上的行编辑输入也一并被吃掉，这正是我们要的：
     * ARK_SHELL_LINE_SIZE 装不下 512 字节，命令行文本走不通。 */
    for(i = 0u; i < ARKRTOS_CFG_CHUNK_COUNT; i++)
    {
        uint8 *p = &rec[i * ARKRTOS_CFG_CHUNK_SIZE];
        uint32 reason;
        int32  rc;

        if(cfg_recv_exact(dev, p, ARKRTOS_CFG_CHUNK_SIZE, ARKRTOS_CFG_CHUNK_TIMEOUT) != 0)
        {
            sh_out("cfg load: timed out waiting for the record\r\n");
            cfg_send_flow(dev, ARKRTOS_CFG_FLOW_NAK);
            return 0;
        }

        /* 前 3 块只回 ACK：此时记录还不完整，校验没有意义 */
        if((i + 1u) < ARKRTOS_CFG_CHUNK_COUNT)
        {
            cfg_send_flow(dev, ARKRTOS_CFG_FLOW_ACK);
            continue;
        }

        reason = arkrtos_layout_validate((const arkrtos_cfg_record_t *)rec);

        if(reason != ARKRTOS_CFG_OK)
        {
            ark_shell_printf("cfg load: record rejected, %s\r\n",
                             arkrtos_layout_reason_text(reason));
            cfg_send_flow(dev, ARKRTOS_CFG_FLOW_NAK);
            return 0;
        }

        rc = arkrtos_layout_write(rec, (uint32)ARKRTOS_CFG_RECORD_SIZE, &erased);

        if(rc < 0)
        {
            ark_shell_printf("cfg load: write failed (%d), config region not usable\r\n",
                             (int)rc);
            cfg_send_flow(dev, ARKRTOS_CFG_FLOW_NAK);
            return 0;
        }

        ark_shell_printf("cfg load: ok, record %d%s\r\n",
                         (int)rc,
                         (erased != 0u) ? " (region erased first)" : "");
        sh_out("cfg load: applied immediately; it is also what the next boot reads\r\n");

        cfg_send_flow(dev, ARKRTOS_CFG_FLOW_ACK);

        /* 记录里写的是布局策略与运行期参数：分区几何、RAM 窗口和任务表都在
         * 下一次 arkrtos_layout_init() 之后才完全一致（池扫描用的是启动时的模式），
         * 所以这里提示重启，而不是假装一切都已到位。 */
        sh_out("cfg load: reboot recommended before installing images\r\n");
    }

    return 0;
}

static int cmd_cfg_clear(void)
{
    int32 rc = arkrtos_layout_erase();

    if(rc < 0)
    {
        ark_shell_printf("cfg clear: failed (%d)\r\n", (int)rc);
        return -1;
    }

    sh_out("cfg clear: config region erased, compile-time default layout in use\r\n");
    sh_out("cfg clear: reboot recommended before installing images\r\n");
    return 0;
}

int cmd_cfg(int argc, char *argv[])
{
    if(argc <= 1)
    {
        return cmd_cfg_show();
    }

    if(strcmp(argv[1], "show") == 0)
    {
        return cmd_cfg_show();
    }
    if(strcmp(argv[1], "load") == 0)
    {
        return cmd_cfg_load();
    }
    if(strcmp(argv[1], "clear") == 0)
    {
        return cmd_cfg_clear();
    }

    sh_out("usage: cfg [show | load | clear]\r\n");
    return -1;
}

#endif /* SHELL_ENABLE */
