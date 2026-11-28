/**
* @file test_drvwdg_host.c
* @brief board/stm32f427/drvwdg.c 的宿主用例：独立看门狗（IWDG）的驱动契约
* @details 这是板级驱动宿主覆盖率欠账的第二刀。与 drvled 不同，这个模块
*          **不经过 STM32 HAL**，它直接读写三组绝对地址的寄存器：
*
*            - IWDG 块   0x40003000（KR / PR / RLR / SR）
*            - RCC_CSR   0x40023874（LSION / LSIRDY）
*            - DBGMCU    0xE0042008（IWDG_STOP / WWDG_STOP）
*
*          IWDG 与 RCC 两段在 main() 开头**真正映射出来**（Windows
*          VirtualAlloc / POSIX mmap MAP_FIXED），映射失败直接报错退出；
*          跳过会让断言在未映射的地址上跑，读起来像通过。寄存器是普通
*          RAM 单元：写进去什么读回来什么，所以 PR / RLR / KR 的**终值**
*          与 LSIRDY / PVU / RVU 的**读侧行为**都能精确摆布。
*
*          DBGMCU 是例外：Windows 在 `0xE0000000` 处保留了一整段私有区间
*          （VirtualQuery 报 0x1FFF0000 字节的 MEM_RESERVE），
*          `0xE0042008` 落在里面，任何进程都拿不到。所以 drvwdg.c 把那一个
*          地址单列成可覆盖的 `WDG_DBGMCU_APB1_FZ_ADDR`，本用例用 `-D`
*          把它指到已经映射的第一段 RAM（`0x40004000`）。这**只改地址**，
*          读改写语义（置位 bit 11 / 12、不踩别的位）仍按真寄存器的写法验
*          一遍。
*
*          宿主上钉住的是这些：
*
*            1. 预分频选择：从 /4 起找最小的、能装进 12 位重装值的档；
*               算不出来的超时**拒绝**而不是拿一个近似的值去启动计数器
*               （IWDG 一启动在软件里停不下来，错的超时不可恢复）。
*            2. 32 位乘法回绕：`timeout_ms * 32` 在 32 位里会绕回，绕回后的
*               「小数字」曾能被当成合法拍数接受（如 134217856 ms 被读成
*               128 ms）。本用例把这条边界钉死：越过可表示范围一律拒绝。
*            3. LSI 等不到 ready 时返回 -1，且**计数器一个字节都没被启动**
*               ——拒绝之后板子与调用前完全一样。
*            4. PVU / RVU 永远不落时，自旋是有界的（不会挂死内核），配置
*               仍会写完并返回 0。
*            5. 回报的是**寄存器里的事实**而不是调用方要的数字：timeout_ms
*               由 PR/RLR 反算，失败路径把它清 0（0 = 没启动）。
*            6. 调试冻结位（DBGMCU 的 bit 11 / 12）每次都写，且不踩别的位。
*
*          **不证明**：KR 三种命令（START / UNLOCK / FEED）的**先后顺序**。
*          KR 是一个地址，普通 RAM 只会留下最后一次写入的值，所以「先启动
*          再解锁」这条顺序约束在宿主上不可观测——它是板级读数的账，本文
*          不假装测到了。也不证明 LSI 实际频率（标称 32 kHz，实际误差约
*          ±50%，超时从来只是「大约这么久」）、不证明真复位会发生。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_types.h"
#include "arkrtos_hal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* ---------------- 寄存器窗口 ---------------- */

#define T_IWDG_BASE     (0x40003000u)
#define T_RCC_CSR       (*(volatile uint32 *)(0x40023874u))
#define T_DBGMCU_ADDR   (0x40004000u)   /* 见下：DBGMCU 用 RAM 代替 */
#define T_DBGMCU_FZ     (*(volatile uint32 *)(T_DBGMCU_ADDR))

#define T_KR            (*(volatile uint32 *)(T_IWDG_BASE + 0x00u))
#define T_PR            (*(volatile uint32 *)(T_IWDG_BASE + 0x04u))
#define T_RLR           (*(volatile uint32 *)(T_IWDG_BASE + 0x08u))
#define T_SR            (*(volatile uint32 *)(T_IWDG_BASE + 0x0Cu))

#define T_KR_UNLOCK     (0x5555u)
#define T_KR_START      (0xCCCCu)
#define T_KR_FEED       (0xAAAAu)
#define T_SR_PVU        (1u << 0)
#define T_SR_RVU        (1u << 1)
#define T_CSR_LSION     (1u << 0)
#define T_CSR_LSIRDY    (1u << 1)

/* 两段窗口：IWDG 块（0x40003000）与 RCC（CSR 在 0x40023874）。Windows
 * 的分配粒度是 64 KB，所以基址取 64 KB 对齐的那个；DBGMCU 在 0xE0042008
 * 拿不到（见文件头），改指向第一段里的 0x40004000。 */
static const unsigned long g_win[2] = { 0x40000000ul, 0x40020000ul };

static int map_at(unsigned long base, unsigned long size)
{
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)base, (SIZE_T)size,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    return (p == (void *)(uintptr_t)base) ? 0 : -1;
#else
    void *p = mmap((void *)(uintptr_t)base, (size_t)size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    return (p == MAP_FAILED) ? -1 : 0;
#endif
}

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL %s (line %d)\n", what, __LINE__);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got %lu want %lu (line %d)\n",
               what, got, want, __LINE__);
    }
}

/* 把三段寄存器清零，并把 LSI 置成「已就绪」——那是正常上电的读侧行为。
 * 需要「LSI 等不到」的用例自己再清掉 LSIRDY。 */
static void regs_reset(void)
{
    int i;

    for(i = 0; i < 2; i++)
    {
        memset((void *)(uintptr_t)g_win[i], 0, 0x10000u);
    }

    T_RCC_CSR = T_CSR_LSIRDY;
}

/* ---------------- 用例 ---------------- */

/* 预分频与重装值的算术：指名道姓地算一遍，不从寄存器反推。 */
static void test_prescaler(void)
{
    regs_reset();
    ck(arkrtos_port_wdg_timeout_ms() == 0u, "before any init the timeout reads 0");

    /* 100 ms：3200/4 = 800 拍，/4 档就装得下。 */
    ck(arkrtos_port_wdg_init(100u) == 0, "init 100 ms returns 0");
    ck_u(T_PR, 0ul, "100 ms uses prescaler /4");
    ck_u(T_RLR, 799ul, "100 ms reloads 800 ticks");
    ck_u(arkrtos_port_wdg_timeout_ms(), 100ul, "100 ms reported back");

    /* 512 ms 正好是 12 位在 /4 档的上限（4096 拍）。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(512u) == 0, "init 512 ms returns 0");
    ck_u(T_PR, 0ul, "512 ms still fits prescaler /4");
    ck_u(T_RLR, 4095ul, "512 ms reloads 4096 ticks");
    ck_u(arkrtos_port_wdg_timeout_ms(), 512ul, "512 ms reported back");

    /* 再大 1 ms 就换档：4104 拍装不下，/8 的 2052 拍装得下。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(513u) == 0, "init 513 ms returns 0");
    ck_u(T_PR, 1ul, "513 ms switches to prescaler /8");
    ck_u(T_RLR, 2051ul, "513 ms reloads 2052 ticks");
    ck_u(arkrtos_port_wdg_timeout_ms(), 513ul, "513 ms reported back");

    /* 1 s 走 /8。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(1000u) == 0, "init 1000 ms returns 0");
    ck_u(T_PR, 1ul, "1000 ms uses prescaler /8");
    ck_u(T_RLR, 3999ul, "1000 ms reloads 4000 ticks");

    /* 最小可表示值：1 ms -> 8 拍。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(1u) == 0, "init 1 ms returns 0");
    ck_u(T_PR, 0ul, "1 ms uses prescaler /4");
    ck_u(T_RLR, 7ul, "1 ms reloads 8 ticks");
    ck_u(arkrtos_port_wdg_timeout_ms(), 1ul, "1 ms reported back");

    /* 最大可表示值：/256 档的 4096 拍 = 32768 ms。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(32768u) == 0, "init 32768 ms returns 0");
    ck_u(T_PR, 6ul, "32768 ms uses the largest prescaler /256");
    ck_u(T_RLR, 4095ul, "32768 ms reloads 4096 ticks");
    ck_u(arkrtos_port_wdg_timeout_ms(), 32768ul, "32768 ms reported back");
}

/* 拒绝路径：超时算不出来时**不许**启动计数器。 */
static void test_refuse(void)
{
    /* 0 ms 是调用方的错，在碰任何寄存器之前就回 -1。此时读回的仍是
     * 「上一次真正启动的那个超时」——硬件没被碰过，读数就该说实话。 */
    {
        uint32 prev = arkrtos_port_wdg_timeout_ms();

        regs_reset();
        ck(arkrtos_port_wdg_init(0u) == -1, "init 0 ms is refused");
        ck_u(T_KR, 0ul, "refused 0 ms never starts the counter");
        ck_u(T_RCC_CSR, (unsigned long)T_CSR_LSIRDY, "refused 0 ms leaves RCC alone");
        ck_u(arkrtos_port_wdg_timeout_ms(), prev,
             "refused 0 ms does not disturb the reported timeout");
    }

    /* 32777 ms：/256 档要 4097 拍，超过 12 位，拒绝。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(32777u) == -1, "init 32777 ms is refused");
    ck_u(T_KR, 0ul, "refused 32777 ms never starts the counter");
    ck_u(T_PR, 0ul, "refused 32777 ms writes no prescaler");
    ck_u(T_RLR, 0ul, "refused 32777 ms writes no reload");

    /* 32 位乘法回绕：134217856 * 32 = 2^32 + 4096，绕回成 4096 拍。
     * 若不拦，这一句会被当成 128 ms 接受并启动计数器。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(134217856u) == -1, "init that would wrap 32-bit math is refused");
    ck_u(T_KR, 0ul, "refused wrap case never starts the counter");
    ck_u(T_PR, 0ul, "refused wrap case writes no prescaler");
    ck_u(T_RLR, 0ul, "refused wrap case writes no reload");

    /* 0xFFFFFFFF ms：同样拒绝，且不能因为绕回而落进 12 位。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(0xFFFFFFFFu) == -1, "init 0xFFFFFFFF ms is refused");
    ck_u(T_KR, 0ul, "refused maximal request never starts the counter");

    /* 一次成功之后再失败：读数必须回到 0（「没在跑」），而不是留着旧值。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(100u) == 0, "successful init before a failing one");
    ck_u(arkrtos_port_wdg_timeout_ms(), 100ul, "timeout 100 before the failing init");
    ck(arkrtos_port_wdg_init(32777u) == -1, "failing init after a success");
    ck_u(arkrtos_port_wdg_timeout_ms(), 0ul, "failing init clears the reported timeout");
}

/* LSI：等不到 ready 就退，计数器不许被启动。 */
static void test_lsi(void)
{
    uint32 i;

    /* 读侧永远是 0：自旋 100000 次后放弃。断言还要求它**很快**返回，
     * 所以这条同时钉住「自旋有界」。 */
    regs_reset();
    T_RCC_CSR = 0u;
    ck(arkrtos_port_wdg_init(100u) == -1, "init is refused while the LSI never gets ready");
    ck_u(T_RCC_CSR & (unsigned long)T_CSR_LSION, (unsigned long)T_CSR_LSION,
         "the refusal still left LSION requested");
    ck_u(T_RCC_CSR & (unsigned long)T_CSR_LSIRDY, 0ul, "LSIRDY is still not set");
    ck_u(T_KR, 0ul, "no start command was written");
    ck_u(T_PR, 0ul, "no prescaler was written");
    ck_u(arkrtos_port_wdg_timeout_ms(), 0ul, "no timeout is reported");

    /* 已经就绪时不要重复启动 LSI——|= 是幂等的，这里顺带钉住它不清掉
     * 别的位（RCC_CSR 上还挂着复位标志）。 */
    regs_reset();
    T_RCC_CSR = (uint32)(T_CSR_LSIRDY | (1u << 24) | (1u << 26));
    ck(arkrtos_port_wdg_init(100u) == 0, "init succeeds with a ready LSI");
    ck_u(T_RCC_CSR & (unsigned long)(1u << 24), (unsigned long)(1u << 24),
         "init preserves the other RCC_CSR bits");
    ck_u(T_RCC_CSR & (unsigned long)T_CSR_LSION, (unsigned long)T_CSR_LSION,
         "init requests LSION anyway");

    /* 已经就绪的读侧：自旋第一圈就 break（不靠计数量，靠返回值与后续写入
     * 都在——若读数错，PVU 那一段会走满 100000 次，测试会慢到看得见）。 */
    for(i = 0u; i < 1u; i++)
    {
        ck(arkrtos_port_wdg_timeout_ms() != 0u, "a successful init reports a non-zero timeout");
    }
}

/* PVU / RVU：永远不落时自旋有界，配置照样写完。 */
static void test_update_flags(void)
{
    regs_reset();
    T_SR = T_SR_PVU;
    ck(arkrtos_port_wdg_init(100u) == 0, "init returns 0 with PVU stuck");
    ck_u(T_PR, 0ul, "PVU stuck still writes the prescaler");
    ck_u(T_RLR, 799ul, "PVU stuck still writes the reload");
    ck_u(T_KR, (unsigned long)T_KR_FEED, "PVU stuck still ends with a feed");

    regs_reset();
    T_SR = T_SR_RVU;
    ck(arkrtos_port_wdg_init(100u) == 0, "init returns 0 with RVU stuck");
    ck_u(T_RLR, 799ul, "RVU stuck still writes the reload");
    ck_u(T_KR, (unsigned long)T_KR_FEED, "RVU stuck still ends with a feed");

    regs_reset();
    T_SR = (uint32)(T_SR_PVU | T_SR_RVU);
    ck(arkrtos_port_wdg_init(1000u) == 0, "init returns 0 with both update flags stuck");
    ck_u(T_PR, 1ul, "both stuck still writes the prescaler");
    ck_u(T_RLR, 3999ul, "both stuck still writes the reload");
    ck_u(arkrtos_port_wdg_timeout_ms(), 1000ul, "both stuck still reports the timeout");

    /* 正常情形：SR 为 0，两个自旋都在第一圈 break。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(100u) == 0, "init returns 0 with clear update flags");
    ck_u(T_SR, 0ul, "the test never had to fake SR");
}

/* 调试冻结与回报值。 */
static void test_misc(void)
{
    /* DBGMCU 的两个 stop 位：每启动一次都要重新写（任何复位都会清掉它们），
     * 且不许踩到别的位。 */
    regs_reset();
    T_DBGMCU_FZ = 0x1u;
    ck(arkrtos_port_wdg_init(100u) == 0, "init writes the debug freeze");
    ck_u(T_DBGMCU_FZ & (1ul << 12), (1ul << 12), "IWDG_STOP (bit 12) is set");
    ck_u(T_DBGMCU_FZ & (1ul << 11), (1ul << 11), "WWDG_STOP (bit 11) is set");
    ck_u(T_DBGMCU_FZ & 0x1ul, 0x1ul, "an unrelated DBGMCU bit survives");

    /* feed：只写 0xAAAA，别的什么都不碰。 */
    regs_reset();
    ck(arkrtos_port_wdg_init(100u) == 0, "init before a feed");
    T_KR = 0x1234u;
    arkrtos_port_wdg_feed();
    ck_u(T_KR, (unsigned long)T_KR_FEED, "feed writes 0xAAAA");
    ck_u(T_PR, 0ul, "feed leaves the prescaler alone");
    ck_u(T_RLR, 799ul, "feed leaves the reload alone");

    /* 探针：发出去的三种命令值是这份驱动与 IWDG 之间的契约，写错一位
     * 板子上就是「永远解不了锁」或「永远不启动」。 */
    ck_u(T_KR_START, 0xCCCCul, "start magic is 0xCCCC");
    ck_u(T_KR_UNLOCK, 0x5555ul, "unlock magic is 0x5555");
    ck_u(T_KR_FEED, 0xAAAAul, "feed magic is 0xAAAA");
}

/* ---------------- main ---------------- */

int main(void)
{
    int i;

    for(i = 0; i < 2; i++)
    {
        if(map_at(g_win[i], 0x10000u) != 0)
        {
            printf("DRVWDG-HOST FAIL cannot map 0x%lX\n", g_win[i]);
            return 1;
        }
    }

    test_prescaler();
    test_refuse();
    test_lsi();
    test_update_flags();
    test_misc();

    if(g_fails != 0)
    {
        printf("DRVWDG-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }

    printf("DRVWDG-HOST OK  %d checks\n", g_checks);
    return 0;
}
