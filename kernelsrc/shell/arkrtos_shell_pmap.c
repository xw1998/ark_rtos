/**
* @file arkrtos_shell_pmap.c
* @brief 页表后端：pmap 一族（映射 / 审计 / 空间 / 页替换 / 受控访问）。
*          既是运行期动态映射 API 的命令行入口，也是它的实测手段。
* @note 本文件属于内核特权态代码；由 arkrtos_shell.c 按命令族拆出，
*       只做搬运，命令逻辑与拆分前逐字相同。
*/

#include "arkrtos_shell_internal.h"

#if (SHELL_ENABLE == 1)

#if (ARKRTOS_USE_PAGING == 1)
/* ------------------------------------------------------------
 * pmap：页表后端的运行期动态映射（驱动登记 MMIO 的那条 API）
 * @details 静态映射只管内核镜像 / App 页 / 两个 UART 页。驱动要用的设备
 *          窗口如果不在其中，就得在运行期登记一条。这里既是那套 API 的
 *          命令行入口，也是它的实测手段：
 *            pmap                                     读数（审计计数 + 已登记页数）
 *            pmap map  <va> <pa> <size> <ram|rom|ro|rw>
 *            pmap check <va> <size> <ram|rom|ro|rw>   审计探针（跨页/未映射都返回 -1）
 *            pmap rd   <va> [count]                    经映射读（每行一个字）
 *            pmap rb   <va>                            经映射读一个字节
 *            pmap wrb  <va> <byte>                     经映射写一个字节（MMIO 才用）
 *            pmap test                                 自测：内核 ROM 页映射后逐字比对
 *            pmap space [n]                            地址空间：读（无参）或切换
 *            pmap rel <n>                              空间回收（U-3）：把该空间
 *                                                      的帧还回池子，读数前后 frames
 *            pmap u1                                   自测：同一 VA 在两个空间里是
 *                                                      两块物理内存（U-1 的读数）
 *            pmap elf                                  每空间一份的 ELF 实例台账
 *            pmap u2 [path]                            自测：同一份 ELF 装进两个空
 *                                                      间、并发两个任务、互不覆盖
 *            pmap look <va>                            读回该 VA 命中的 TLB 条目
 *            pmap unmap <va> <size>                    撤销动态映射（U-15）
 *            pmap dyn                                  动态槽位占用读数
 *            pmap cu                                   自测：内核受控访问用户
 *                                                      地址（U-6）的四种拒绝码
 *            pmap swap                                 页替换读数（U-16）：换出/
 *                                                      换回/失败/槽位/驻留对
 *            pmap swaptest                             自测：先吃掉空闲帧，再摸
 *                                                      超驻留对的对，逐字比对
 *
 *          **注意**：`pmap rd/rb/wrb` 不做保护性检查 —— 地址没映射就是一次
 *          重填致命。这是刻意的：它们就是用来验证「这条地址到底通不通」的，
 *          替调用方兜住异常只会把「通不通」变成看不见。
 * ------------------------------------------------------------ */
#define PMAP_TEST_VA    0x03000000u     /* 静态映射之外的空地（16 MB 以上） */

#if (ARKRTOS_USE_PAGING_SWAP == 1)
/* 页替换自测（U-16）用的两个东西：占位帧的账本、每对每个字的图案。
 * 账本必须是静态的 —— 上千个帧基址放栈上，shell 的栈直接就没了一半。 */
#define PMAP_SWAPTEST_HOLD_MAX   1024u
static uint32 s_pmap_swap_hold[PMAP_SWAPTEST_HOLD_MAX];

/** 自测图案：同一对里每个字都不同，不同对之间也不可互换 */
static uint32 pmap_swaptest_word(uint32 pair, uint32 word)
{
    return (0xA5A50000u | ((pair & 0xFFu) << 8)) ^ ((word * 0x9E3779B1u) | 1u);
}
#endif

static int pmap_hex(const char *s, uint32 *out)
{
    uint32 v = 0u;
    uint32 n = 0u;
    char c;

    if((s == 0) || (*s == '\0'))
    {
        return -1;
    }

    if((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X')))
    {
        s += 2;
    }

    while(*s != '\0')
    {
        c = *s;
        if((c >= '0') && (c <= '9'))
        {
            v = (v << 4) | (uint32)(c - '0');
        }
        else if((c >= 'a') && (c <= 'f'))
        {
            v = (v << 4) | ((uint32)(c - 'a') + 10u);
        }
        else if((c >= 'A') && (c <= 'F'))
        {
            v = (v << 4) | ((uint32)(c - 'A') + 10u);
        }
        else
        {
            return -1;
        }
        n++;
        s++;
    }

    if(n == 0u)
    {
        return -1;
    }

    *out = v;
    return 0;
}

static int pmap_kind(const char *s, arkrtos_mpu_mem_t *out)
{
    if(strcmp(s, "ram") == 0) { *out = ARKRTOS_MPU_MEM_RAM;       return 0; }
    if(strcmp(s, "rom") == 0) { *out = ARKRTOS_MPU_MEM_ROM;       return 0; }
    if(strcmp(s, "ro")  == 0) { *out = ARKRTOS_MPU_MEM_PERIPH_RO; return 0; }
    if(strcmp(s, "rw")  == 0) { *out = ARKRTOS_MPU_MEM_PERIPH_RW; return 0; }
    return -1;
}

/* U-6 自测：把一个返回码与期望对上。只报错不短路，四种拒绝码各看一次。 */
static int pmap_cu_rc(const char *label, int32 got, int32 want, int *p_fails)
{
    if(got == want)
    {
        return 0;
    }

    (*p_fails)++;
    ark_shell_printf("pmap cu: FAIL %s rc=%d want=%d\r\n",
                     label, (int)got, (int)want);
    return 1;
}

int cmd_pmap(int argc, char *argv[])
{
    uint32 checked = 0u;
    uint32 bad = 0u;
    uint32 bad_base = 0u;
    int32 rc;

    if((argc >= 2) && (strcmp(argv[1], "map") == 0))
    {
        uint32 va = 0u;
        uint32 pa = 0u;
        uint32 size = 0u;
        arkrtos_mpu_mem_t kind = ARKRTOS_MPU_MEM_RAM;

        if((argc < 6) || (pmap_hex(argv[2], &va) != 0) ||
           (pmap_hex(argv[3], &pa) != 0) || (pmap_hex(argv[4], &size) != 0) ||
           (pmap_kind(argv[5], &kind) != 0))
        {
            sh_out("usage: pmap map <va> <pa> <size> <ram|rom|ro|rw>\r\n");
            return 1;
        }

        rc = arkrtos_paging_map_span(va, pa, size, kind);
        ark_shell_printf("pmap: map 0x%08X <- 0x%08X size=%u rc=%d pages=%u\r\n",
                         (unsigned)va, (unsigned)pa, (unsigned)size, (int)rc,
                         (unsigned)arkrtos_paging_map_pages());
        return (rc == 0) ? 0 : 1;
    }

    if((argc >= 3) && (strcmp(argv[1], "check") == 0))
    {
        uint32 va = 0u;
        uint32 size = 0u;
        arkrtos_mpu_mem_t kind = ARKRTOS_MPU_MEM_RAM;

        if((argc < 5) || (pmap_hex(argv[2], &va) != 0) ||
           (pmap_hex(argv[3], &size) != 0) || (pmap_kind(argv[4], &kind) != 0))
        {
            sh_out("usage: pmap check <va> <size> <ram|rom|ro|rw>\r\n");
            return 1;
        }

        rc = arkrtos_port_paging_check(va, size, kind);
        ark_shell_printf("pmap: check 0x%08X +%u rc=%d\r\n",
                         (unsigned)va, (unsigned)size, (int)rc);
        return (rc == 0) ? 0 : 1;
    }

    if((argc >= 3) && (strcmp(argv[1], "rd") == 0))
    {
        uint32 va = 0u;
        uint32 n = 1u;
        uint32 i;

        if((pmap_hex(argv[2], &va) != 0) ||
           ((argc >= 4) && (parse_u32(argv[3], &n) != 0)))
        {
            sh_out("usage: pmap rd <va> [count]\r\n");
            return 1;
        }

        if(n > 8u)
        {
            n = 8u;
        }

        for(i = 0u; i < n; i++)
        {
            uint32 addr = va + (i * 4u);
            uint32 w = *(volatile uint32 *)(unsigned long)addr;
            ark_shell_printf("pmap: [0x%08X] = 0x%08X\r\n", (unsigned)addr, (unsigned)w);
        }
        return 0;
    }

    if((argc >= 3) && (strcmp(argv[1], "rb") == 0))
    {
        uint32 va = 0u;

        if(pmap_hex(argv[2], &va) != 0)
        {
            sh_out("usage: pmap rb <va>\r\n");
            return 1;
        }

        ark_shell_printf("pmap: [0x%08X] = 0x%02X\r\n", (unsigned)va,
                         (unsigned)(*(volatile uint8 *)(unsigned long)va));
        return 0;
    }

    if((argc >= 4) && (strcmp(argv[1], "wrb") == 0))
    {
        uint32 va = 0u;
        uint32 val = 0u;

        if((pmap_hex(argv[2], &va) != 0) || (pmap_hex(argv[3], &val) != 0))
        {
            sh_out("usage: pmap wrb <va> <byte>\r\n");
            return 1;
        }

        *(volatile uint8 *)(unsigned long)va = (uint8)(val & 0xFFu);
        return 0;
    }

    if((argc >= 3) && (strcmp(argv[1], "unmap") == 0))
    {
        uint32 va = 0u;
        uint32 size = 0u;

        if((argc < 4) || (pmap_hex(argv[2], &va) != 0) ||
           (pmap_hex(argv[3], &size) != 0))
        {
            sh_out("usage: pmap unmap <va> <size>\r\n");
            return 1;
        }

        rc = arkrtos_paging_unmap_span(va, size);
        ark_shell_printf("pmap: unmap 0x%08X size=%u rc=%d pages=%u dyn=%u\r\n",
                         (unsigned)va, (unsigned)size, (int)rc,
                         (unsigned)arkrtos_paging_map_pages(),
                         (unsigned)arkrtos_paging_dyn_used());
        return (rc == 0) ? 0 : 1;
    }

    if((argc >= 2) && (strcmp(argv[1], "dyn") == 0))
    {
        ark_shell_printf("pmap: dyn slot(s) in use=%u pages=%u\r\n",
                         (unsigned)arkrtos_paging_dyn_used(),
                         (unsigned)arkrtos_paging_map_pages());
        return 0;
    }

#if (ARKRTOS_USE_PAGING_SWAP == 1)
    if((argc >= 2) && (strcmp(argv[1], "swap") == 0))
    {
        uint32 total = arkrtos_paging_frame_total();
        uint32 used  = arkrtos_paging_frame_used();

        ark_shell_printf("pmap: swap out=%u in=%u fail=%u slots=%u/%u "
                         "resident=%u frames %u/%u\r\n",
                         (unsigned)arkrtos_paging_swap_stat(0u),
                         (unsigned)arkrtos_paging_swap_stat(1u),
                         (unsigned)arkrtos_paging_swap_stat(3u),
                         (unsigned)arkrtos_paging_swap_stat(2u),
                         (unsigned)arkrtos_paging_swap_stat(5u),
                         (unsigned)arkrtos_paging_swap_stat(4u),
                         (unsigned)used, (unsigned)total);
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "swaptest") == 0))
    {
        /* 页替换（U-16）自测：造出「帧池真的满了」这个前提，再看内容对不对。
         *
         * 为什么必须先把空闲帧吃光：窗口一共 256 对 = 512 帧，而池子有上千
         * 帧 —— 光摸窗口永远摸不满，那种「全绿」什么都没验到。所以这里先把
         * 空闲帧按 2 帧一段占掉，只留 8 帧（4 对），再摸
         *   resident + 4 + slots
         * 对（上限 256）：resident 是已经付过费的对，4 是剩下能直接拿的对，
         * slots 是交换区能同时收下的对数。摸的就比「装得下的」多出来，
         * 于是必然发生换出；再回头逐字读第二遍，必然发生换回。
         *
         * 判据只有三条，全部落在「读出来是什么」上，不拿计数当证据：
         *   1) 第二遍逐字比对 0 处不符；
         *   2) 换出、换回都真的发生过（>0）—— 否则这一轮根本没测到替换；
         *   3) 收尾后槽位全空、帧数回到「测试前 - 驻留对的帧」，
         *      即账本没有泄漏。
         * ------------------------------------------------------------ */
        uint32 space   = arkrtos_paging_space_current();
        uint32 win     = arkrtos_paging_window_va();
        uint32 total   = arkrtos_paging_frame_total();
        uint32 used0   = arkrtos_paging_frame_used();
        uint32 res0    = arkrtos_paging_swap_stat(4u);
        uint32 slots   = arkrtos_paging_swap_stat(5u);
        uint32 out0    = arkrtos_paging_swap_stat(0u);
        uint32 in0     = arkrtos_paging_swap_stat(1u);
        uint32 hold_n  = 0u;
        uint32 npairs;
        uint32 nspace;
        uint32 i;
        uint32 k;
        int fails = 0;

        if((win == 0u) || (total == 0u))
        {
            sh_out("pmap swaptest: refused - no paging window on this board\r\n");
            return 1;
        }
        if(arkrtos_elf_running() != 0)
        {
            sh_out("pmap swaptest: refused - an ELF program is running "
                   "(try 'pmap elf')\r\n");
            return 1;
        }
        if(slots == 0u)
        {
            sh_out("pmap swaptest: refused - swap area has no slot\r\n");
            return 1;
        }

        /* 1) 吃空闲帧，只留 8 帧（4 对）。分配粒度是 2 帧，所以这里也按 2
         *    帧一段占 —— 免得留下奇数个碎片帧，反而挡住了本来能成的两帧分配。 */
        while((total - arkrtos_paging_frame_used()) > 8u)
        {
            uint32 base = 0u;

            if(hold_n >= PMAP_SWAPTEST_HOLD_MAX)
            {
                break;
            }
            if(arkrtos_paging_frame_alloc(2u, &base) != 0)
            {
                break;
            }
            s_pmap_swap_hold[hold_n] = base;
            hold_n++;
        }

        npairs = res0 + 4u + slots;
        if(npairs > 256u)
        {
            npairs = 256u;
        }

        ark_shell_printf("pmap swaptest: space=%u held=%u pair(s) free=%u "
                         "resident=%u slots=%u touch=%u pair(s)\r\n",
                         (unsigned)space, (unsigned)hold_n,
                         (unsigned)(total - arkrtos_paging_frame_used()),
                         (unsigned)res0, (unsigned)slots, (unsigned)npairs);

        /* 2) 逐对写图案（每对 8 KB）。写到后面就会把前面的对挤进交换区。 */
        for(i = 0u; i < npairs; i++)
        {
            volatile uint32 *p = (volatile uint32 *)(size_t)(win + (i * 0x2000u));

            for(k = 0u; k < 2048u; k++)
            {
                p[k] = pmap_swaptest_word(i, k);
            }
        }

        /* 3) 第二遍逐字比对：这一遍读的每一对，多半刚从交换区搬回来。 */
        for(i = 0u; (i < npairs) && (fails == 0); i++)
        {
            volatile uint32 *p = (volatile uint32 *)(size_t)(win + (i * 0x2000u));

            for(k = 0u; k < 2048u; k++)
            {
                if(p[k] != pmap_swaptest_word(i, k))
                {
                    ark_shell_printf("pmap swaptest: mismatch pair=%u word=%u "
                                     "got=0x%08X want=0x%08X\r\n",
                                     (unsigned)i, (unsigned)k,
                                     (unsigned)p[k],
                                     (unsigned)pmap_swaptest_word(i, k));
                    fails++;
                    break;
                }
            }
        }

        {
            uint32 out1 = arkrtos_paging_swap_stat(0u);
            uint32 in1  = arkrtos_paging_swap_stat(1u);

            ark_shell_printf("pmap swaptest: swap out %u -> %u, in %u -> %u, "
                             "mismatch=%d\r\n",
                             (unsigned)out0, (unsigned)out1,
                             (unsigned)in0, (unsigned)in1, fails);

            if(out1 == out0)
            {
                sh_out("pmap swaptest: FAIL (no swap-out happened - "
                       "the pool was never full)\r\n");
                fails++;
            }
            if(in1 == in0)
            {
                sh_out("pmap swaptest: FAIL (no swap-in happened)\r\n");
                fails++;
            }
        }

        /* 4) 收尾：每个空间都回收（驻留的对与交换槽一起放），再把占住的帧
         *    还回去。此后槽位必须全空、帧数必须回到 used0 - 2*res0。 */
        nspace = arkrtos_paging_space_count();
        for(i = 0u; i < nspace; i++)
        {
            (void)arkrtos_paging_space_release(i);
        }
        for(i = 0u; i < hold_n; i++)
        {
            (void)arkrtos_paging_frame_free(s_pmap_swap_hold[i], 2u);
        }

        {
            uint32 expect = (used0 >= (res0 * 2u)) ? (used0 - (res0 * 2u)) : 0u;
            uint32 got    = arkrtos_paging_frame_used();
            uint32 left   = arkrtos_paging_swap_stat(2u);

            ark_shell_printf("pmap swaptest: after release frames=%u expect=%u "
                             "slots=%u\r\n",
                             (unsigned)got, (unsigned)expect, (unsigned)left);

            if((got != expect) || (left != 0u))
            {
                sh_out("pmap swaptest: FAIL (accounting leaked)\r\n");
                fails++;
            }
        }

        if(fails != 0)
        {
            ark_shell_printf("pmap swaptest: FAIL (%d problem(s))\r\n", fails);
            return 1;
        }

        sh_out("pmap swaptest: PASS (swap out/in round trip, content "
               "byte-identical, accounting clean)\r\n");
        return 0;
    }
#endif /* ARKRTOS_USE_PAGING_SWAP == 1 */

    if((argc >= 2) && (strcmp(argv[1], "frame") == 0))
    {
        uint32 total = arkrtos_paging_frame_total();
        uint32 used = arkrtos_paging_frame_used();

        ark_shell_printf("pmap: frames total=%u used=%u free=%u\r\n",
                         (unsigned)total, (unsigned)used,
                         (unsigned)((total >= used) ? (total - used) : 0u));
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "ftest") == 0))
    {
        uint32 used0 = arkrtos_paging_frame_used();
        uint32 a = 0u;
        uint32 b = 0u;
        uint32 i;
        int pass = 1;

        /* 1 帧 + 3 帧两段连续的分配。帧池在板级是恒等映射的，所以拿到
         * 物理基址就能直接写图案——读不回来说明那条 PS=22 没生效，
         * 而这正是「分配器能跑但帧用不了」的那种假绿。 */
        rc = arkrtos_paging_frame_alloc(1u, &a);
        ark_shell_printf("pmap: frame alloc 1 rc=%d base=0x%08X\r\n", (int)rc, (unsigned)a);
        if((rc != 0) || (a == 0u))
        {
            sh_out("pmap ftest: FAIL (alloc 1)\r\n");
            return 1;
        }

        rc = arkrtos_paging_frame_alloc(3u, &b);
        ark_shell_printf("pmap: frame alloc 3 rc=%d base=0x%08X\r\n", (int)rc, (unsigned)b);
        if((rc != 0) || (b == 0u))
        {
            (void)arkrtos_paging_frame_free(a, 1u);
            sh_out("pmap ftest: FAIL (alloc 3)\r\n");
            return 1;
        }

        for(i = 0u; i < 4u; i++)
        {
            uint32 base = (i == 0u) ? a : (b + ((i - 1u) * 0x1000u));
            volatile uint32 *p = (volatile uint32 *)(size_t)base;
            uint32 k;

            for(k = 0u; k < 64u; k++)
            {
                p[k] = 0xA5A50000u ^ (i << 8) ^ k;
            }
            for(k = 0u; k < 64u; k++)
            {
                if(p[k] != (0xA5A50000u ^ (i << 8) ^ k))
                {
                    ark_shell_printf("pmap: pattern mismatch base=0x%08X word=%u\r\n",
                                     (unsigned)base, (unsigned)k);
                    pass = 0;
                    break;
                }
            }
        }

        rc = arkrtos_paging_frame_free(a, 1u);
        ark_shell_printf("pmap: frame free 1 rc=%d\r\n", (int)rc);
        if(rc != 0)
        {
            pass = 0;
        }

        rc = arkrtos_paging_frame_free(a, 1u);
        ark_shell_printf("pmap: frame double-free rc=%d (want -3)\r\n", (int)rc);
        if(rc != -3)
        {
            pass = 0;
        }

        rc = arkrtos_paging_frame_free(0x1000u, 1u);
        ark_shell_printf("pmap: frame free outside rc=%d (want -1)\r\n", (int)rc);
        if(rc != -1)
        {
            pass = 0;
        }

        rc = arkrtos_paging_frame_free(b, 3u);
        ark_shell_printf("pmap: frame free 3 rc=%d\r\n", (int)rc);
        if(rc != 0)
        {
            pass = 0;
        }

        if(arkrtos_paging_frame_used() != used0)
        {
            ark_shell_printf("pmap: frame used %u != %u after release\r\n",
                             (unsigned)arkrtos_paging_frame_used(), (unsigned)used0);
            pass = 0;
        }

        if(pass == 0)
        {
            sh_out("pmap ftest: FAIL\r\n");
            return 1;
        }

        ark_shell_printf("pmap ftest: PASS (alloc/free/double-free/pattern, used back to %u)\r\n",
                         (unsigned)used0);
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "test") == 0))
    {
        int i;
        int diff = 0;

        /* 把内核镜像的第一页挂到静态映射之外的空地，然后逐字比对：
         * 经新翻译读到的东西必须和直连地址一模一样，否则就是翻译错了，
         * 而不是「看起来能跑」。 */
        rc = arkrtos_paging_map_span(PMAP_TEST_VA, (uint32)ARKRTOS_MPU_KERNEL_ROM_BASE,
                                     0x1000u, ARKRTOS_MPU_MEM_ROM);
        ark_shell_printf("pmap: map ROM 0x%08X <- 0x%08X rc=%d\r\n",
                         (unsigned)PMAP_TEST_VA,
                         (unsigned)ARKRTOS_MPU_KERNEL_ROM_BASE, (int)rc);
        if(rc != 0)
        {
            sh_out("pmap test: FAIL (map refused)\r\n");
            return 1;
        }

        for(i = 0; i < 16; i++)
        {
            uint32 off = (uint32)i * 4u;
            uint32 a = *(volatile uint32 *)(unsigned long)(PMAP_TEST_VA + off);
            uint32 b = *(volatile uint32 *)(unsigned long)
                       ((uint32)ARKRTOS_MPU_KERNEL_ROM_BASE + off);

            if(a != b)
            {
                diff++;
            }
        }

        ark_shell_printf("pmap: compare 16 words, mismatch=%d\r\n", diff);

        /* U-15：撤掉这一页之后，同一条 VA 必须真的不再命中任何条目。
         * 只打印「rc=0」不够 —— 账面清了而翻译还在，就是这类改动的典型
         * 假绿。所以这里直接问 port 的读回路径。 */
        {
            uint32 idx = 0u;
            uint32 ppn = 0u;
            uint32 ps = 0u;
            uint32 again_va = PMAP_TEST_VA;
            int still = 0;
            int re_diff = 0;

            rc = arkrtos_paging_unmap_span(PMAP_TEST_VA, 0x1000u);
            still = (arkrtos_port_paging_lookup(again_va, &idx, &ppn, &ps) != 0u) ? 1 : 0;
            ark_shell_printf("pmap: unmap 0x%08X rc=%d left_entry=%d dyn=%u\r\n",
                             (unsigned)PMAP_TEST_VA, (int)rc, still,
                             (unsigned)arkrtos_paging_dyn_used());
            if((rc != 0) || (still != 0))
            {
                sh_out("pmap test: FAIL (unmap left an entry)\r\n");
                return 1;
            }

            /* 同一个 VA 改绑另一块物理页（内核 ROM 的第二页）。槽位如果没
             * 真的回收，这一步会撞上 -5（同 VA 已登记成别的 PA）—— 那正是
             * 「撤销」与「改绑」之间的分界。 */
            rc = arkrtos_paging_map_span(PMAP_TEST_VA,
                                         ((uint32)ARKRTOS_MPU_KERNEL_ROM_BASE + 0x1000u),
                                         0x1000u, ARKRTOS_MPU_MEM_ROM);
            ark_shell_printf("pmap: rebind 0x%08X <- 0x%08X rc=%d dyn=%u\r\n",
                             (unsigned)PMAP_TEST_VA,
                             (unsigned)((uint32)ARKRTOS_MPU_KERNEL_ROM_BASE + 0x1000u),
                             (int)rc, (unsigned)arkrtos_paging_dyn_used());
            if(rc != 0)
            {
                sh_out("pmap test: FAIL (slot not recycled)\r\n");
                return 1;
            }

            for(i = 0; i < 16; i++)
            {
                uint32 off = (uint32)i * 4u;
                uint32 a = *(volatile uint32 *)(unsigned long)(PMAP_TEST_VA + off);
                uint32 b = *(volatile uint32 *)(unsigned long)
                           ((uint32)ARKRTOS_MPU_KERNEL_ROM_BASE + 0x1000u + off);

                if(a != b)
                {
                    re_diff++;
                }
            }

            ark_shell_printf("pmap: rebind compare 16 words, mismatch=%d\r\n", re_diff);
            if(re_diff != 0)
            {
                sh_out("pmap test: FAIL (rebind translation)\r\n");
                return 1;
            }

            rc = arkrtos_paging_unmap_span(PMAP_TEST_VA, 0x1000u);
            still = (arkrtos_port_paging_lookup(again_va, &idx, &ppn, &ps) != 0u) ? 1 : 0;
            ark_shell_printf("pmap: final unmap rc=%d left_entry=%d dyn=%u\r\n",
                             (int)rc, still, (unsigned)arkrtos_paging_dyn_used());
            if((rc != 0) || (still != 0))
            {
                sh_out("pmap test: FAIL (final unmap left an entry)\r\n");
                return 1;
            }
        }

        sh_out("pmap test: PASS (map / unmap / rebind / unmap)\r\n");
        return 0;
    }

    if((argc >= 3) && (strcmp(argv[1], "rel") == 0))
    {
        uint32 want = 0u;
        uint32 before;
        uint32 after;
        int32  rcv;

        if(pmap_hex(argv[2], &want) != 0)
        {
            sh_out("usage: pmap rel <space>\r\n");
            return -1;
        }

        before = arkrtos_paging_frame_used();
        rcv    = arkrtos_paging_space_release(want);
        after  = arkrtos_paging_frame_used();
        ark_shell_printf("pmap rel: space %u rc=%d frames %u -> %u "
                         "(frame=0x%08X current=%u)\r\n",
                         (unsigned)want, (int)rcv,
                         (unsigned)before, (unsigned)after,
                         (unsigned)arkrtos_paging_space_frame(want),
                         (unsigned)arkrtos_paging_space_current());
        return (rcv == 0) ? 0 : 1;
    }

    if((argc >= 2) && (strcmp(argv[1], "space") == 0))
    {
        uint32 count = arkrtos_paging_space_count();

        if(argc >= 3)
        {
            uint32 want = 0u;

            if(pmap_hex(argv[2], &want) != 0)
            {
                sh_out("usage: pmap space [n]\r\n");
                return -1;
            }

            rc = arkrtos_paging_space_activate(want);
            ark_shell_printf("pmap: space %u rc=%d (current=%u)\r\n",
                             (unsigned)want, (int)rc,
                             (unsigned)arkrtos_paging_space_current());
            return (rc == 0) ? 0 : 1;
        }

        {
            uint32 i;

            ark_shell_printf("pmap: spaces=%u current=%u\r\n",
                             (unsigned)count,
                             (unsigned)arkrtos_paging_space_current());
            for(i = 0u; i < count; i++)
            {
                ark_shell_printf("  space %u -> frame 0x%08X\r\n", (unsigned)i,
                                 (unsigned)arkrtos_paging_space_frame(i));
            }
        }
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "look") == 0))
    {
        uint32 va = 0u;
        uint32 idx = 0u;
        uint32 ppn = 0u;
        uint32 ps = 0u;

        if((argc < 3) || (pmap_hex(argv[2], &va) != 0))
        {
            sh_out("usage: pmap look <va>\r\n");
            return -1;
        }

        if(arkrtos_port_paging_lookup(va, &idx, &ppn, &ps) == 0u)
        {
            ark_shell_printf("pmap look: va=0x%08X no entry\r\n", (unsigned)va);
            return 1;
        }

        ark_shell_printf("pmap look: va=0x%08X entry=%u ps=%u ppn=0x%08X\r\n",
                         (unsigned)va, (unsigned)idx, (unsigned)ps,
                         (unsigned)ppn);
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "u1") == 0))
    {
        /* 同一 VA、两个地址空间：写进去的东西必须互不相干。
         * 判据不是「PA 数字不一样」（那是我们自己说的），而是**同一个地址
         * 在两个空间里读出两个不同的值**——翻译真的换了帧。
         *
         * 下面这段在窗口内取一块空地（base + 1 MB），所以先确认没有程序
         * 正在窗口里跑：ELF 装载器与运行中的程序都在这个 2 MB 里。 */
        /* 窗口基址不写死板级宏：space 0 的帧就是窗口基址（这条对应关系由
         * port 建立），所以从它推。+1 MB 落在 2 MB 窗口的中段。 */
        uint32 va = arkrtos_paging_window_va() + 0x100000u;
        uint32 m0;
        uint32 m1;
        int fails = 0;

        #if (ARKRTOS_USE_ELF == 1)
        if(arkrtos_elf_running() != 0)
        {
            sh_out("pmap u1: refused - an ELF program is using the window\r\n");
            return 1;
        }
        #endif

        if(arkrtos_paging_space_count() < 2u)
        {
            sh_out("pmap u1: refused - board provides only one address space\r\n");
            return 1;
        }

        uint32 rc0;
        uint32 rc1;
        uint32 idx0 = 0u, ppn0 = 0u, ps0 = 0u;
        uint32 idx1 = 0u, ppn1 = 0u, ps1 = 0u;

        rc0 = (uint32)arkrtos_paging_space_activate(0u);
        (void)arkrtos_port_paging_lookup(va, &idx0, &ppn0, &ps0);
        *(volatile uint32 *)(unsigned long)va = 0xA5A50001u;
        rc1 = (uint32)arkrtos_paging_space_activate(1u);
        (void)arkrtos_port_paging_lookup(va, &idx1, &ppn1, &ps1);
        *(volatile uint32 *)(unsigned long)va = 0xA5A50002u;

        /* 回到 space 0 读，再切回 space 1 读：顺序反过来也行，两个方向都读 */
        (void)arkrtos_paging_space_activate(0u);
        m0 = *(volatile uint32 *)(unsigned long)va;
        (void)arkrtos_paging_space_activate(1u);
        m1 = *(volatile uint32 *)(unsigned long)va;

        if(m0 != 0xA5A50001u)
        {
            fails++;
        }
        if(m1 != 0xA5A50002u)
        {
            fails++;
        }

        ark_shell_printf("pmap u1: va=0x%08X space0=0x%08X space1=0x%08X\r\n",
                         (unsigned)va, (unsigned)m0, (unsigned)m1);
        ark_shell_printf("pmap u1: frame0=0x%08X frame1=0x%08X\r\n",
                         (unsigned)arkrtos_paging_space_frame(0u),
                         (unsigned)arkrtos_paging_space_frame(1u));
        ark_shell_printf("pmap u1: rc0=%d rc1=%d\r\n", (int)(int32)rc0, (int)(int32)rc1);
        ark_shell_printf("pmap u1: look0 entry=%u ps=%u ppn=0x%08X\r\n",
                         (unsigned)idx0, (unsigned)ps0, (unsigned)ppn0);
        ark_shell_printf("pmap u1: look1 entry=%u ps=%u ppn=0x%08X\r\n",
                         (unsigned)idx1, (unsigned)ps1, (unsigned)ppn1);

        /* 回到 space 0：这是这台机器默认的那一帧，shell 之后看到的窗口内容
         * 与没有这条命令时一致。 */
        (void)arkrtos_paging_space_activate(0u);

        if(fails != 0)
        {
            ark_shell_printf("pmap u1: FAIL (%d of 2 reads wrong)\r\n", fails);
            return 1;
        }

        sh_out("pmap u1: PASS - same VA, two frames\r\n");
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "elf") == 0))
    {
        int32 i;
        int32 n = arkrtos_elf_space_count();
        int32 live = 0;

        ark_shell_printf("pmap elf: spaces=%d last=%d\r\n",
                         (int)n, (int)arkrtos_elf_last_space());

        for(i = 0; (i < n) && (i < (int32)ARKRTOS_TASK_MAX_NUM); i++)
        {
            arkrtos_elf_info_t inf;
            uint32 frame = arkrtos_paging_space_frame((uint32)i);
            int32 running = arkrtos_elf_running_in((uint32)i);

            if(running != 0)
            {
                live++;
            }

            if((arkrtos_elf_info_space((uint32)i, &inf) == ARKRTOS_ELF_OK) &&
               (inf.task > 0))
            {
                ark_shell_printf("  space %d frame=0x%08X task=%d live=%d "
                                 "bytes=%u entry=0x%08X\r\n",
                                 (int)i, (unsigned)frame, (int)inf.task,
                                 (int)running, (unsigned)inf.bytes,
                                 (unsigned)inf.entry);
            }
            else
            {
                ark_shell_printf("  space %d frame=0x%08X empty\r\n",
                                 (int)i, (unsigned)frame);
            }
        }

        ark_shell_printf("pmap elf: %d live instance(s)\r\n", (int)live);
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "u2") == 0))
    {
        /* 同一份文件装进两个地址空间 -> 两个任务并发跑，各自的帧互不覆盖。
         *
         * 判据全部是"从同一 VA 读出什么"，不拿 PA 数字当证据：
         *   1) 装载前在两块帧的窗口首字各放一个不同的哨兵；一次装载只许
         *      冲掉自己那块帧的哨兵，另一块必须原封不动；
         *   2) 两次装载必须落在两个不同的空间，两次装载后各自那块帧里
         *      必须是程序自己写进去的内容（!= 哨兵）；
         *   3) 两次装载都做完的那一刻，两个空间各有活任务、任务号不同、
         *      绑定就是自己那个空间 —— 这才是"并发多实例"。
         *
         * 整段在调度器锁里做：测的是"装载 + 台账"，不是 hello 跑多久。
         * 不加锁的话一个眨眼就退出的程序会在两次装载之间把任务槽还回来，
         * 第二次装载拿到同一个任务号，读数看起来像"两个实例共用一个任务"，
         * 其实只是前者已经退场。锁外那两个任务才开始跑。 */
        const char *path = (argc >= 3) ? argv[2] : "/mnt/ram/hello";
        char *uargv[1];
        uint32 win;
        uint32 prev;
        uint32 s_first = 0u;
        uint32 s_second = 0u;
        uint32 other = 0u;
        uint32 sentinel_first = 0xA5A50201u;
        uint32 sentinel_other = 0xA5A50202u;
        uint32 w_first;
        uint32 w_other_before;
        uint32 w_first_after;
        uint32 w_second;
        arkrtos_elf_info_t inf0;
        arkrtos_elf_info_t inf1;
        int fails = 0;
        int locked = 0;
        int32 rc;

        if(arkrtos_paging_space_count() < 2u)
        {
            sh_out("pmap u2: refused - board provides only one address space\r\n");
            return 1;
        }
        if(arkrtos_elf_running() != 0)
        {
            sh_out("pmap u2: refused - an ELF program is already running "
                   "(try 'pmap elf')\r\n");
            return 1;
        }

        win  = arkrtos_paging_window_va();       /* 窗口的 VA 基址，与空间无关 */
        prev = arkrtos_paging_space_current();

        /* 两个空间各写一个不同的哨兵。装载器只许冲掉目标空间那一个。 */
        (void)arkrtos_paging_space_activate(0u);
        *(volatile uint32 *)(unsigned long)win = sentinel_first;
        (void)arkrtos_paging_space_activate(1u);
        *(volatile uint32 *)(unsigned long)win = sentinel_other;
        (void)arkrtos_paging_space_activate(prev);

        #if (ARKRTOS_USE_SCHED_LOCK == 1)
        arkrtos_sched_lock();
        locked = 1;
        #endif

        uargv[0] = (char *)path;
        rc = arkrtos_elf_exec_space(path, 1, uargv, &s_first);
        if(rc != ARKRTOS_ELF_OK)
        {
            ark_shell_printf("pmap u2: first exec failed: %s (rc=%d)\r\n",
                             arkrtos_elf_error_name(rc), (int)rc);
            fails++;
        }
        else
        {
            other = (s_first == 0u) ? 1u : 0u;

            (void)arkrtos_paging_space_activate(s_first);
            w_first = *(volatile uint32 *)(unsigned long)win;
            (void)arkrtos_paging_space_activate(other);
            w_other_before = *(volatile uint32 *)(unsigned long)win;
            (void)arkrtos_paging_space_activate(prev);

            if(w_other_before != ((other == 0u) ? sentinel_first : sentinel_other))
            {
                fails++;
                ark_shell_printf("pmap u2: FAIL - loading space %u wrote into "
                                 "space %u (0x%08X)\r\n",
                                 (unsigned)s_first, (unsigned)other,
                                 (unsigned)w_other_before);
            }
            if(w_first == ((s_first == 0u) ? sentinel_first : sentinel_other))
            {
                fails++;
                ark_shell_printf("pmap u2: FAIL - space %u still holds its "
                                 "sentinel after the load\r\n", (unsigned)s_first);
            }

            rc = arkrtos_elf_exec_space(path, 1, uargv, &s_second);
            if(rc != ARKRTOS_ELF_OK)
            {
                ark_shell_printf("pmap u2: second exec failed: %s (rc=%d)\r\n",
                                 arkrtos_elf_error_name(rc), (int)rc);
                fails++;
            }
            else if(s_second == s_first)
            {
                fails++;
                ark_shell_printf("pmap u2: FAIL - second instance reused space %u\r\n",
                                 (unsigned)s_first);
            }
            else
            {
                (void)arkrtos_paging_space_activate(s_second);
                w_second = *(volatile uint32 *)(unsigned long)win;
                (void)arkrtos_paging_space_activate(s_first);
                w_first_after = *(volatile uint32 *)(unsigned long)win;
                (void)arkrtos_paging_space_activate(prev);

                if(w_second == ((s_second == 0u) ? sentinel_first : sentinel_other))
                {
                    fails++;
                    ark_shell_printf("pmap u2: FAIL - space %u still holds its "
                                     "sentinel after the load\r\n",
                                     (unsigned)s_second);
                }
                if(w_first_after != w_first)
                {
                    fails++;
                    ark_shell_printf("pmap u2: FAIL - the second load changed "
                                     "space %u (0x%08X -> 0x%08X)\r\n",
                                     (unsigned)s_first, (unsigned)w_first,
                                     (unsigned)w_first_after);
                }

                /* 两个实例此刻都该是活的：调度器锁着，它们还没机会退出。 */
                if(arkrtos_elf_running_in(s_first) == 0)
                {
                    fails++;
                    ark_shell_printf("pmap u2: FAIL - space %u has no live task "
                                     "right after the load\r\n", (unsigned)s_first);
                }
                if(arkrtos_elf_running_in(s_second) == 0)
                {
                    fails++;
                    ark_shell_printf("pmap u2: FAIL - space %u has no live task "
                                     "right after the load\r\n", (unsigned)s_second);
                }

                if((arkrtos_elf_info_space(s_first, &inf0) != ARKRTOS_ELF_OK) ||
                   (arkrtos_elf_info_space(s_second, &inf1) != ARKRTOS_ELF_OK))
                {
                    fails++;
                }
                else
                {
                    if(inf0.task == inf1.task)
                    {
                        fails++;
                        ark_shell_printf("pmap u2: FAIL - both instances share "
                                         "task %d\r\n", (int)inf0.task);
                    }
                    if(arkrtos_paging_space_of(&arkrtos_task_table[inf0.task - 1]) != s_first)
                    {
                        fails++;
                        ark_shell_printf("pmap u2: FAIL - task %d is not bound to "
                                         "space %u\r\n", (int)inf0.task,
                                         (unsigned)s_first);
                    }
                    if(arkrtos_paging_space_of(&arkrtos_task_table[inf1.task - 1]) != s_second)
                    {
                        fails++;
                        ark_shell_printf("pmap u2: FAIL - task %d is not bound to "
                                         "space %u\r\n", (int)inf1.task,
                                         (unsigned)s_second);
                    }
                }

                ark_shell_printf("pmap u2: %s -> space %u task %d, space %u task %d\r\n",
                                 path, (unsigned)s_first, (int)inf0.task,
                                 (unsigned)s_second, (int)inf1.task);
                ark_shell_printf("pmap u2: frame%u=0x%08X frame%u=0x%08X\r\n",
                                 (unsigned)s_first,
                                 (unsigned)arkrtos_paging_space_frame(s_first),
                                 (unsigned)s_second,
                                 (unsigned)arkrtos_paging_space_frame(s_second));
                ark_shell_printf("pmap u2: same VA 0x%08X reads 0x%08X / 0x%08X, "
                                 "other slot before/after 0x%08X / 0x%08X\r\n",
                                 (unsigned)win, (unsigned)w_first,
                                 (unsigned)w_second, (unsigned)w_other_before,
                                 (unsigned)w_first_after);
            }
        }

        if(locked != 0)
        {
            #if (ARKRTOS_USE_SCHED_LOCK == 1)
            (void)arkrtos_sched_unlock();
            #endif
        }

        if(fails != 0)
        {
            ark_shell_printf("pmap u2: FAIL (%d checks failed)\r\n", fails);
            return 1;
        }

        sh_out("pmap u2: PASS - two instances, two spaces, neither frame overwritten\r\n");
        return 0;
    }

    if((argc >= 2) && (strcmp(argv[1], "cu") == 0))
    {
        /* U-6 自测：内核受控访问用户地址。
         * 判据是**返回码**，不是"读到了什么"：四种拒绝码各来一次，然后
         * 这条命令后面还要照常打印读数 —— 内核没有被任何一个非法指针
         * 带进 fault，这本身就是"不 fault 内核"的证据。
         * 用户侧那块内存取窗口里一段空地（与 pmap u1 同一位置，base+1 MB），
         * 先确认没有程序正在用窗口。 */
        uint32 win  = arkrtos_paging_window_va();
        uint32 uram = win + 0x100000u;
        uint32 urom = uram + 0x800u;
        uint32 prev = arkrtos_paging_space_current();
        uint32 i;
        uint32 cur;
        uint8 buf[16];
        int32 rc_c;
        int fails = 0;

        #if (ARKRTOS_USE_ELF == 1)
        if(arkrtos_elf_running() != 0)
        {
            sh_out("pmap cu: refused - an ELF program is using the window\r\n");
            return 1;
        }
        #endif

        if(arkrtos_paging_space_activate(0u) != 0)
        {
            sh_out("pmap cu: refused - cannot install space 0\r\n");
            return 1;
        }

        cur = arkrtos_paging_space_current();

        for(i = 0u; i < 16u; i++)
        {
            ((volatile uint8 *)(unsigned long)uram)[i] = (uint8)(0x40u + i);
            ((volatile uint8 *)(unsigned long)urom)[i] = (uint8)(0x80u + i);
        }

        for(i = 0u; i < 16u; i++)
        {
            buf[i] = 0u;
        }

        /* 1) 读数据窗口：成功，字节与写进去的一致 */
        rc_c = arkrtos_paging_user_copy(urom, 0x100u, uram, 0x100u, cur, cur,
                                       buf, (const void *)(unsigned long)uram,
                                       16u, 0u);
        (void)pmap_cu_rc("from_user RAM", rc_c, 0, &fails);
        if((rc_c == 0) && ((buf[0] != 0x40u) || (buf[15] != 0x4Fu)))
        {
            fails++;
            ark_shell_printf("pmap cu: FAIL from_user RAM bytes 0x%02X..0x%02X\r\n",
                             (unsigned)buf[0], (unsigned)buf[15]);
        }

        /* 2) 读代码窗口：允许（只读字符串常量的正常用法） */
        rc_c = arkrtos_paging_user_copy(urom, 0x100u, uram, 0x100u, cur, cur,
                                       buf, (const void *)(unsigned long)urom,
                                       16u, 0u);
        (void)pmap_cu_rc("from_user ROM", rc_c, 0, &fails);
        if((rc_c == 0) && ((buf[0] != 0x80u) || (buf[15] != 0x8Fu)))
        {
            fails++;
            ark_shell_printf("pmap cu: FAIL from_user ROM bytes 0x%02X..0x%02X\r\n",
                             (unsigned)buf[0], (unsigned)buf[15]);
        }

        /* 3) 窗口内、但不在两个跨度里 -> -3 */
        rc_c = arkrtos_paging_user_copy(urom, 0x100u, uram, 0x100u, cur, cur,
                                       buf, (const void *)(unsigned long)(uram + 0x8000u),
                                       16u, 0u);
        (void)pmap_cu_rc("outside span", rc_c, -3, &fails);

        /* 4) 内核镜像地址 -> -3（不是把内核内存读给用户） */
        rc_c = arkrtos_paging_user_copy(urom, 0x100u, uram, 0x100u, cur, cur,
                                       buf, (const void *)(unsigned long)0x00200000u,
                                       16u, 0u);
        (void)pmap_cu_rc("kernel image ptr", rc_c, -3, &fails);

        /* 5) 完全没映射的地址 -> -3，且内核活着（后面还在打印） */
        rc_c = arkrtos_paging_user_copy(urom, 0x100u, uram, 0x100u, cur, cur,
                                       buf, (const void *)(unsigned long)0x80000000u,
                                       16u, 0u);
        (void)pmap_cu_rc("unmapped ptr", rc_c, -3, &fails);

        /* 6) 长度把地址顶回 0 -> -1（不做回绕后的区间判断） */
        rc_c = arkrtos_paging_user_copy(urom, 0x100u, uram, 0x100u, cur, cur,
                                       buf, (const void *)(unsigned long)0xFFFFF000u,
                                       0x2000u, 0u);
        (void)pmap_cu_rc("range wrap", rc_c, -1, &fails);

        /* 7) 往代码窗口写 -> -3 */
        rc_c = arkrtos_paging_user_copy(urom, 0x100u, uram, 0x100u, cur, cur,
                                       buf, (void *)(unsigned long)urom, 16u, 1u);
        (void)pmap_cu_rc("to_user ROM", rc_c, -3, &fails);

        /* 8) 往数据窗口写：成功，字节真的落地 */
        buf[0] = 0xAAu;
        buf[1] = 0x55u;
        rc_c = arkrtos_paging_user_copy(urom, 0x100u, uram, 0x100u, cur, cur,
                                       buf, (void *)(unsigned long)uram, 2u, 1u);
        (void)pmap_cu_rc("to_user RAM", rc_c, 0, &fails);
        if((((volatile uint8 *)(unsigned long)uram)[0] != 0xAAu) ||
           (((volatile uint8 *)(unsigned long)uram)[1] != 0x55u))
        {
            fails++;
            sh_out("pmap cu: FAIL to_user RAM did not reach user memory\r\n");
        }

        /* 9) 没有用户窗口的任务 -> -2 */
        rc_c = arkrtos_paging_user_copy(0u, 0u, 0u, 0u, cur, cur,
                                       buf, (const void *)(unsigned long)uram,
                                       4u, 0u);
        (void)pmap_cu_rc("no user window", rc_c, -2, &fails);

        /* 10) 真正的入口、真正的调用者：shell 是内核任务 -> -2 */
        rc_c = arkrtos_copy_from_user(buf, (const void *)(unsigned long)uram, 4u);
        (void)pmap_cu_rc("kernel caller", rc_c, -2, &fails);

        /* 11) 装着的不是调用者的空间 -> -4（读到的是别的 App 的帧） */
        if(arkrtos_paging_space_count() >= 2u)
        {
            if(arkrtos_paging_space_activate(1u) == 0)
            {
                rc_c = arkrtos_paging_user_copy(urom, 0x100u, uram, 0x100u, 0u,
                                               arkrtos_paging_space_current(),
                                               buf, (const void *)(unsigned long)uram,
                                               16u, 0u);
                (void)pmap_cu_rc("wrong space", rc_c, -4, &fails);
            }
            else
            {
                fails++;
                sh_out("pmap cu: FAIL cannot install space 1\r\n");
            }
        }

        (void)arkrtos_paging_space_activate(prev);

        if(fails != 0)
        {
            ark_shell_printf("pmap cu: FAIL (%d checks failed)\r\n", fails);
            return 1;
        }

        sh_out("pmap cu: PASS - from/to user, refusals -1/-2/-3/-4, kernel survived\r\n");
        return 0;
    }

    rc = arkrtos_paging_stats(&checked, &bad, &bad_base);
    ark_shell_printf("pmap: init_rc=%d checked=%u bad=%u bad_base=0x%08X pages=%u\r\n",
                     (int)rc, (unsigned)checked, (unsigned)bad,
                     (unsigned)bad_base, (unsigned)arkrtos_paging_map_pages());
    sh_out("usage: pmap [map <va> <pa> <size> <kind> | check <va> <size> <kind> |"
           " rd <va> [n] | rb <va> | wrb <va> <byte> | test | space [n] | u1 |\r\n"
           " elf | u2 [path] | look <va>]\r\n");
    return 0;
}
#endif /* ARKRTOS_USE_PAGING == 1 */

#endif /* SHELL_ENABLE */
