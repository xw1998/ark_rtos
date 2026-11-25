/**
* @file test_mpu_host.c
* @brief MPU 窗口构建（arkrtos_mpu.c）的宿主用例
* @details 这一层不写寄存器：它决定「一个任务允许碰哪几段地址」，把结果放进
*          TCB 里的架构无关上下文，再由 port 层编码成硬件寄存器。所以用例
*          不能去看 MPU 硬件，而要钉住**放进上下文的每一段窗口几何与属性**，
*          以及那些「宁可失败关闭、绝不失败打开」的判定。
*
*          五个 port 入口在宿主上由本文件提供：init / reset / set_idle /
*          set_app 只计数，encode 记录 (idx, base, size, mem) 并回填上下文，
*          返回码可编程，用来证明坏参数不会把整个上下文搞乱。
*
*          几条容易读错的语义，用例逐条钉住：
*            - 内核任务（rom_start == 0）落到内核 Flash 窗口；数据侧用整块
*              RAM 的上取整 2 的幂，共享内存被它覆盖时不再单列一段；
*            - App 的代码窗口是「包住镜像的最小对齐 2 的幂」，落点紧贴时
*              会向低地址扩，但绝不越过镜像池；
*            - 镜像池之外 / 形状不合法的 RAM 窗口**失败关闭**（size 0，区域
*              留空），只有「根本不是镜像窗口」的任务才拿到整块芯片 RAM；
*            - MiniApp 的代码在 RAM 里 ⇒ 区域 0 必须是可执行 RAM（RAMX），
*              而不是只读 Flash；
*            - 每次 build 前先清空整组区域，坏参数不会残留上一轮的窗口。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_mpu.h"
#include "arkrtos_partition.h"
#include "arkrtos_arch.h"
#include "arkrtos_hal.h"
#include "arkrtos_task.h"

#if (ARKRTOS_USE_MPU == 1)

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL: %s (got 0x%lX want 0x%lX)\n", what, got, want);
    }
}

/* ---------------- port 桩 ---------------- */

typedef struct {
    int              idx;
    uint32           base;
    uint32           size;
    arkrtos_mpu_mem_t mem;
} cap_t;

static cap_t  g_caps[ARKRTOS_MPU_REGION_MAX];
static int    g_ncap;
static int    g_init_calls;
static int    g_idle_calls;
static int    g_reset_calls;
static int    g_setapp_calls;
static const arkrtos_arch_mpu_t *g_setapp_arg;
static int32  g_encode_ret;

void arkrtos_port_mpu_init(void)
{
    g_init_calls++;
}

void arkrtos_port_mpu_set_region(uint32 rom_addr, uint32 rom_size,
                                uint32 ram_addr, uint32 ram_size)
{
    (void)rom_addr; (void)rom_size; (void)ram_addr; (void)ram_size;
}

void arkrtos_port_mpu_set_app(const arkrtos_arch_mpu_t *p_mpu)
{
    g_setapp_calls++;
    g_setapp_arg = p_mpu;
}

void arkrtos_port_mpu_reset(void)
{
    g_reset_calls++;
}

void arkrtos_port_mpu_set_idle(void)
{
    g_idle_calls++;
}

int32 arkrtos_port_mpu_encode(arkrtos_arch_mpu_t *p_mpu, uint32 idx,
                             uint32 base, uint32 size, arkrtos_mpu_mem_t mem)
{
    if(g_ncap < (int)ARKRTOS_MPU_REGION_MAX)
    {
        g_caps[g_ncap].idx  = (int)idx;
        g_caps[g_ncap].base = base;
        g_caps[g_ncap].size = size;
        g_caps[g_ncap].mem  = mem;
        g_ncap++;
    }

    if(g_encode_ret == 0)
    {
        p_mpu->region_base[idx] = base;
        p_mpu->region_attr[idx] = (uint32)mem;
    }

    return g_encode_ret;
}

/* ---------------- 工具 ---------------- */

static arkrtos_task_t g_task;

static void reset_all(void)
{
    memset(g_caps, 0, sizeof(g_caps));
    g_ncap = 0;
    g_init_calls = 0;
    g_idle_calls = 0;
    g_reset_calls = 0;
    g_setapp_calls = 0;
    g_setapp_arg = 0;
    g_encode_ret = 0;
    memset(&g_task, 0, sizeof(g_task));
    /* 先摆一块非零垃圾，证明 build_task 会先清空整组区域。 */
    {
        uint32 i;
        for(i = 0u; i < ARKRTOS_MPU_REGION_MAX; i++)
        {
            g_task.mpu.region_base[i] = 0xDEADBEEFu;
            g_task.mpu.region_attr[i] = 0x5A5A5A5Au;
        }
    }
}

static const cap_t *cap_find(int idx)
{
    int i;
    for(i = 0; i < g_ncap; i++)
    {
        if(g_caps[i].idx == idx)
        {
            return &g_caps[i];
        }
    }
    return 0;
}

static void set_rom(unsigned long start, uint32 span)
{
    g_task.rom_start = start;
    g_task.rom_size  = span;
}

static void set_ram(unsigned long start, uint32 size)
{
    g_task.ram_start = start;
    g_task.ram_size  = size;
}

/* ---------------- 用例 ---------------- */

static void test_init_idle_reset(void)
{
    reset_all();

    arkrtos_mpu_module_init();
    ck_u((unsigned long)g_init_calls, 1ul, "module_init forwards to port_mpu_init once");
    ck(g_init_calls == 1, "module_init does not touch other port entry points");
    ck_u((unsigned long)g_setapp_calls, 0ul, "module_init does not install an app context");

    arkrtos_mpu_set_idle();
    ck_u((unsigned long)g_idle_calls, 1ul, "set_idle forwards to port_mpu_set_idle");

    arkrtos_mpu_reset();
    ck_u((unsigned long)g_reset_calls, 1ul, "reset forwards to port_mpu_reset");
}

static void test_null_task(void)
{
    reset_all();

    arkrtos_mpu_build_task(0);
    ck_u((unsigned long)g_ncap, 0ul, "build_task(NULL) encodes nothing");
    ck(g_task.mpu.region_base[0] == 0xDEADBEEFu,
       "build_task(NULL) leaves the context untouched");
}

static void test_kernel_task(void)
{
    const cap_t *c;

    reset_all();
    /* rom_start == 0 是内核内部任务的标记，两个窗口字段都为 0。 */
    arkrtos_mpu_build_task(&g_task);

    ck_u((unsigned long)g_ncap, 2ul, "kernel task gets two regions (code + RAM fallback)");

    c = cap_find((int)ARKRTOS_MPU_RGN_CODE);
    ck(c != 0, "kernel task has a code region");
    if(c != 0)
    {
        ck_u(c->base, (unsigned long)ARKRTOS_MPU_KERNEL_ROM_BASE, "kernel code base is the kernel flash window");
        ck_u(c->size, (unsigned long)ARKRTOS_MPU_KERNEL_ROM_SIZE, "kernel code size is the kernel size");
        ck(c->mem == ARKRTOS_MPU_MEM_ROM, "kernel code is read-only executable flash");
    }

    c = cap_find((int)ARKRTOS_MPU_RGN_DATA);
    ck(c != 0, "kernel task has a data region");
    if(c != 0)
    {
        ck_u(c->base, (unsigned long)ARKRTOS_MPU_KERNEL_RAM_FALLBACK, "kernel data base is the chip RAM base");
        ck_u(c->size, (unsigned long)CHIP_RAM_SIZE, "kernel data window rounds up to a power of two covering the chip RAM");
        ck(c->mem == ARKRTOS_MPU_MEM_RAM, "kernel data is read/write non-executable");
    }

    ck(cap_find((int)ARKRTOS_MPU_RGN_SHARE) == 0,
       "the kernel RAM fallback already covers shared RAM, so no extra share region");
}

static void test_context_is_cleared(void)
{
    reset_all();
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram(0u, 0u);

    arkrtos_mpu_build_task(&g_task);

    /* 未用到的区域必须被清空，不能留着上一轮的窗口。 */
    ck_u(g_task.mpu.region_base[6], 0ul, "region 6 base is cleared before building");
    ck_u(g_task.mpu.region_attr[6], 0ul, "region 6 attr is cleared before building");
    ck_u(g_task.mpu.region_base[7], 0ul, "region 7 base is cleared before building");
    ck_u(g_task.mpu.region_attr[7], 0ul, "region 7 attr is cleared before building");
}

static void test_app_exact_window(void)
{
    const cap_t *c;

    reset_all();
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram(0u, 0u);

    arkrtos_mpu_build_task(&g_task);

    c = cap_find((int)ARKRTOS_MPU_RGN_CODE);
    ck(c != 0, "app has a code region");
    if(c != 0)
    {
        ck_u(c->base, (unsigned long)IMAGE_POOL_BASE, "aligned image yields an exact window base");
        ck_u(c->size, 0x1000ul, "aligned image yields an exact window size");
        ck(c->mem == ARKRTOS_MPU_MEM_ROM, "app code is read-only executable");
    }

    c = cap_find((int)ARKRTOS_MPU_RGN_DATA);
    ck(c != 0, "app without a RAM window still gets the whole-chip fallback");
    if(c != 0)
    {
        ck_u(c->base, (unsigned long)ARKRTOS_MPU_KERNEL_RAM_FALLBACK,
             "no RAM window falls back to chip RAM");
        ck(c->mem == ARKRTOS_MPU_MEM_RAM, "fallback RAM is read/write non-executable");
    }
}

static void test_app_tight_window(void)
{
    const cap_t *c;
    unsigned long start = (unsigned long)IMAGE_POOL_BASE + 0x3000ul;

    reset_all();
    set_rom(start, 0x3000u);
    set_ram(0u, 0u);

    arkrtos_mpu_build_task(&g_task);

    c = cap_find((int)ARKRTOS_MPU_RGN_CODE);
    ck(c != 0, "tight image still gets a code region");
    if(c != 0)
    {
        ck_u(c->base, (unsigned long)IMAGE_POOL_BASE,
             "tight landing expands the window down to an aligned base");
        ck_u(c->size, 0x8000ul, "window grows until it covers the image tail");
        ck((c->base + c->size) >= (start + 0x3000ul), "the expanded window covers the image");
    }
}

static void test_app_window_ending_at_pool_end(void)
{
    const cap_t *c;
    unsigned long start = (unsigned long)IMAGE_POOL_BASE
                        + (unsigned long)IMAGE_POOL_SIZE - 0x2000ul;

    reset_all();
    set_rom(start, 0x2000u);
    set_ram(0u, 0u);

    arkrtos_mpu_build_task(&g_task);

    c = cap_find((int)ARKRTOS_MPU_RGN_CODE);
    ck(c != 0, "image ending exactly at the pool end is accepted");
    if(c != 0)
    {
        ck_u(c->base, start, "exact base kept");
        ck_u(c->base + c->size, (unsigned long)IMAGE_POOL_BASE + (unsigned long)IMAGE_POOL_SIZE,
             "window ends exactly at the pool end");
    }
}

static void test_app_outside_pool(void)
{
    const cap_t *c;

    /* 起点在池内但跨度越过池尾 ⇒ 拒绝（落回内核 Flash 窗口）。 */
    reset_all();
    set_rom((unsigned long)IMAGE_POOL_BASE + (unsigned long)IMAGE_POOL_SIZE - 0x1000ul, 0x2000u);
    set_ram(0u, 0u);
    arkrtos_mpu_build_task(&g_task);
    c = cap_find((int)ARKRTOS_MPU_RGN_CODE);
    ck(c != 0, "image crossing the pool end still gets a code region");
    if(c != 0)
    {
        ck_u(c->base, (unsigned long)ARKRTOS_MPU_KERNEL_ROM_BASE,
             "an image crossing the pool end is refused, not expanded over the boundary");
        ck_u(c->size, (unsigned long)ARKRTOS_MPU_KERNEL_ROM_SIZE, "refusal falls back to the kernel window");
    }

    /* 完全在池外的镜像地址同样落回内核窗口。 */
    reset_all();
    set_rom((unsigned long)ARKRTOS_MPU_KERNEL_ROM_BASE, 0x1000u);
    set_ram(0u, 0u);
    arkrtos_mpu_build_task(&g_task);
    c = cap_find((int)ARKRTOS_MPU_RGN_CODE);
    ck(c != 0 && c->base == (unsigned long)ARKRTOS_MPU_KERNEL_ROM_BASE,
       "a non-pool code address falls back to the kernel window");
}

static void test_pool_ram_exact(void)
{
    const cap_t *c;

    reset_all();
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram((unsigned long)SLOT_RAM_BASE + 0x1000ul, 0x1000u);

    arkrtos_mpu_build_task(&g_task);

    c = cap_find((int)ARKRTOS_MPU_RGN_DATA);
    ck(c != 0, "valid pool RAM block yields a data region");
    if(c != 0)
    {
        ck_u(c->base, (unsigned long)SLOT_RAM_BASE + 0x1000ul, "pool RAM base is exact");
        ck_u(c->size, 0x1000ul, "pool RAM size is exact");
        ck(c->mem == ARKRTOS_MPU_MEM_RAM, "pool RAM is read/write non-executable");
    }

    c = cap_find((int)ARKRTOS_MPU_RGN_SHARE);
    ck(c != 0, "a small pool block does not cover shared RAM, so the share region is added");
    if(c != 0)
    {
        ck_u(c->base, (unsigned long)ARKRTOS_MPU_SHARE_RAM_BASE, "share region base");
        ck_u(c->size, (unsigned long)ARKRTOS_MPU_SHARE_RAM_SIZE, "share region size");
        /* SEC-G03: the kernel keeps the write, an unprivileged task does
         * not get it. Asserting the exact kind (not just "not RAM")
         * is what would catch a future change that quietly widens it
         * back to read/write. */
        ck(c->mem == ARKRTOS_MPU_MEM_RAM_RO,
           "share region is privileged read/write, unprivileged read-only");
    }
}

static void test_pool_ram_bad_shapes(void)
{
    /* 非 2 的幂大小：失败关闭（区域留空），不向外取整。 */
    reset_all();
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram((unsigned long)SLOT_RAM_BASE + 0x1000ul, 0x1800u);
    arkrtos_mpu_build_task(&g_task);
    ck(cap_find((int)ARKRTOS_MPU_RGN_DATA) == 0,
       "a non-power-of-two pool block is refused (data region left disabled)");

    /* 起点未按自身大小对齐。 */
    reset_all();
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram((unsigned long)SLOT_RAM_BASE + 0x800ul, 0x1000u);
    arkrtos_mpu_build_task(&g_task);
    ck(cap_find((int)ARKRTOS_MPU_RGN_DATA) == 0,
       "a misaligned pool block is refused");

    /* 跨度越过池尾。 */
    reset_all();
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram((unsigned long)SLOT_RAM_BASE + (unsigned long)SLOT_RAM_TOTAL - 0x1000ul, 0x2000u);
    arkrtos_mpu_build_task(&g_task);
    ck(cap_find((int)ARKRTOS_MPU_RGN_DATA) == 0,
       "a block crossing the pool end is refused");

    /* 小于最小块 / 大于最大块。 */
    reset_all();
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram((unsigned long)SLOT_RAM_BASE, 0x200u);
    arkrtos_mpu_build_task(&g_task);
    ck(cap_find((int)ARKRTOS_MPU_RGN_DATA) == 0, "a block below the minimum is refused");

    reset_all();
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram((unsigned long)SLOT_RAM_BASE, (uint32)ARKRTOS_MPU_RAM_BLOCK_MAX * 2u);
    arkrtos_mpu_build_task(&g_task);
    ck(cap_find((int)ARKRTOS_MPU_RGN_DATA) == 0, "a block above the maximum is refused");

    /* 被拒绝的池内窗口绝不能退化成整块芯片 RAM（那会放开所有任务的数据）。 */
    reset_all();
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram((unsigned long)SLOT_RAM_BASE + 0x800ul, 0x1000u);
    arkrtos_mpu_build_task(&g_task);
    ck(g_task.mpu.region_base[ARKRTOS_MPU_RGN_DATA] == 0u,
       "a refused pool window leaves region 1 disabled rather than opening chip RAM");
}

static void test_ram_outside_pool(void)
{
    const cap_t *c;

    reset_all();
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram((unsigned long)CHIP_RAM_BASE + 0x1000ul, 0x1000u);

    arkrtos_mpu_build_task(&g_task);

    c = cap_find((int)ARKRTOS_MPU_RGN_DATA);
    ck(c != 0, "a task whose RAM is not a pool block gets the chip fallback");
    if(c != 0)
    {
        ck_u(c->base, (unsigned long)ARKRTOS_MPU_KERNEL_RAM_FALLBACK, "fallback base");
        ck_u(c->size, (unsigned long)CHIP_RAM_SIZE, "fallback size covers the chip");
    }
}

static void test_miniapp_code_in_ram(void)
{
    const cap_t *c;

    reset_all();
    set_rom((unsigned long)SLOT_RAM_BASE + 0x2000ul, 0x2000u);
    set_ram((unsigned long)SLOT_RAM_BASE + 0x4000ul, 0x1000u);

    arkrtos_mpu_build_task(&g_task);

    c = cap_find((int)ARKRTOS_MPU_RGN_CODE);
    ck(c != 0, "miniapp has a code region");
    if(c != 0)
    {
        ck_u(c->base, (unsigned long)SLOT_RAM_BASE + 0x2000ul, "miniapp code base is its RAM block");
        ck_u(c->size, 0x2000ul, "miniapp code size is its RAM block");
        ck(c->mem == ARKRTOS_MPU_MEM_RAMX, "RAM code must be executable (RAMX), not read-only flash");
    }

    c = cap_find((int)ARKRTOS_MPU_RGN_DATA);
    ck(c != 0, "miniapp has a data region");
    if(c != 0)
    {
        ck(c->mem == ARKRTOS_MPU_MEM_RAM,
           "a separate data block stays non-executable even for a miniapp");
    }
}

static void test_miniapp_code_equals_data(void)
{
    const cap_t *c;

    reset_all();
    set_rom((unsigned long)SLOT_RAM_BASE + 0x2000ul, 0x2000u);
    set_ram((unsigned long)SLOT_RAM_BASE + 0x2000ul, 0x2000u);

    arkrtos_mpu_build_task(&g_task);

    c = cap_find((int)ARKRTOS_MPU_RGN_DATA);
    ck(c != 0, "same-block miniapp still has a data region");
    if(c != 0)
    {
        ck(c->mem == ARKRTOS_MPU_MEM_RAMX,
           "when the data window IS the code window it must keep execute permission");
    }
}

static void test_miniapp_bad_block(void)
{
    const cap_t *c;

    /* MiniApp 的块形状坏了：代码窗口失败关闭，绝不向外扩成邻居可执行。 */
    reset_all();
    set_rom((unsigned long)SLOT_RAM_BASE + 0x1000ul, 0x1800u);
    set_ram(0u, 0u);

    arkrtos_mpu_build_task(&g_task);

    c = cap_find((int)ARKRTOS_MPU_RGN_CODE);
    ck(c == 0, "an inexact miniapp block is refused (code region left disabled)");
    ck(g_task.mpu.region_base[ARKRTOS_MPU_RGN_CODE] == 0u,
       "refused miniapp code does not fall back to kernel flash either");
}

static void test_encode_reject_is_ignored(void)
{
    reset_all();
    g_encode_ret = -1;
    set_rom(IMAGE_POOL_BASE, 0x1000u);
    set_ram(0u, 0u);

    arkrtos_mpu_build_task(&g_task);

    /* 坏参数只让那一段缺失，构建流程继续；上下文里没有被写脏。 */
    ck(g_ncap == 2, "both regions are still offered to the port even when encode refuses");
    ck_u(g_task.mpu.region_base[ARKRTOS_MPU_RGN_CODE], 0ul,
         "a refused encode leaves the context clean instead of half-written");
}

static void test_set_app(void)
{
    reset_all();

    arkrtos_mpu_set_app(0);
    ck_u((unsigned long)g_setapp_calls, 0ul, "set_app(NULL) installs nothing");

    arkrtos_mpu_set_app(&g_task);
    ck_u((unsigned long)g_setapp_calls, 1ul, "set_app forwards one context");
    ck(g_setapp_arg == &g_task.mpu, "set_app forwards this task's own mpu context");
}

/* ---------------- 主 ---------------- */

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);

    test_init_idle_reset();
    test_null_task();
    test_kernel_task();
    test_context_is_cleared();
    test_app_exact_window();
    test_app_tight_window();
    test_app_window_ending_at_pool_end();
    test_app_outside_pool();
    test_pool_ram_exact();
    test_pool_ram_bad_shapes();
    test_ram_outside_pool();
    test_miniapp_code_in_ram();
    test_miniapp_code_equals_data();
    test_miniapp_bad_block();
    test_encode_reject_is_ignored();
    test_set_app();

    if(g_fails != 0)
    {
        printf("MPU-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }

    printf("MPU-HOST OK  %d checks\n", g_checks);
    return 0;
}

#else  /* ARKRTOS_USE_MPU == 0 */

/* 关档时整个模块编成空 TU，头文件把五个入口收缩成空宏。这里证明的是
 * 「关档调用点照常编译、并且什么都不做」，不是任何窗口被正确构建。 */
int main(void)
{
    int checks = 0;
    int fails = 0;

    setvbuf(stdout, 0, _IONBF, 0);

#if (ARKRTOS_USE_MPU != 0)
#error "MPU test built for the OFF grade but ARKRTOS_USE_MPU is not 0"
#endif

    arkrtos_mpu_module_init();
    arkrtos_mpu_set_idle();
    arkrtos_mpu_reset();
    arkrtos_mpu_set_app(0);
    arkrtos_mpu_build_task(0);
    checks++;

    /* 关档下取地址是不可能的（宏不是函数），能编过就说明收缩确实发生了。 */
    checks++;

    if(fails != 0)
    {
        printf("MPU-HOST OFF FAILED %d/%d\n", fails, checks);
        return 1;
    }
    printf("MPU-HOST OFF OK  %d checks\n", checks);
    return 0;
}

#endif /* ARKRTOS_USE_MPU */
