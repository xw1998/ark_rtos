/**
* @file test_drvflash_host.c
* @brief board/stm32f427/drvflash.c 的宿主用例：片内 Flash 擦写驱动契约
* @details 这是板级驱动宿主覆盖率的第四刀。drvflash 经过 STM32 HAL 的
*          Flash 接口（`HAL_FLASH_Unlock` / `HAL_FLASH_Lock` /
*          `HAL_FLASHEx_Erase` / `HAL_FLASH_Program`），另外**直接读**
*          `FLASH->OPTCR` 与 `FLASH->OPTCR1` 两枚只读寄存器，所以本用例做
*          两件事：
*
*            1. 给 HAL 的四个 Flash 入口一份**记录式桩**：解锁/上锁的配
*               对、擦除请求的每个字段、逐字节编程的地址与数据，全部记下
*               来；桩自己维护一把"锁"，驱动若在未解锁时要求编程，桩记一
*               笔 `bad_locked`（真 HAL 会返回错误）。
*            2. 把 FLASH 寄存器窗口（0x40023C00 起，落在 0x40020000 那一
*               页里，Windows 分配粒度 64 KB）真的映射出来，OPTCR /
*               OPTCR1 是普通 RAM 单元：写进去什么读回来什么，正好用来
*               摆布保护位的读侧行为。
*
*          宿主上钉住的是这些：
*
*            1. 扇区算术：16K（0..3）/ 64K（4）/ 128K（5 起）三档边界逐
*               个地址验；片外地址（基址之前、基址 + 容量、0xFFFFFFFF）
*               一律返回 0，不返回一个"看起来合法"的扇区大小。
*            2. 擦除只交 **扇区** 给 HAL：`(addr,size)` 自动扩展到覆盖
*               所有相交的扇区，TypeErase / VoltageRange / Sector /
*               NbSectors 逐字段对；size==0 直接返回 0 且**一次 HAL 都不
*               调**。
*            3. 地址范围检查**不许被 32 位回绕骗过**：`addr + size` 绕回
*               后终点可能落回片内、甚至落到比起始更低的扇区，旧写法会
*               让 `last_sector - first_sector + 1` 下溢，把 0xFFFFFFFF
*               个扇区交给 HAL 去擦。本用例把回绕地址钉死为 -1。
*            4. 编程按字节：每个字节一次 `FLASH_TYPEPROGRAM_BYTE`，地址
*               递增、数据逐个对；中途失败立刻停手、如实返回 -1，且仍然
*               上锁。
*            5. 解锁/上锁**必须配对**：成功与失败两条路径都是 unlock →
*               工作 → lock，不留着 Flash 解锁跑完整个函数。
*            6. 保护位读数：RDP 位为 1 报 0 级，清零报 1 级；nWRP 是"1 =
*               不保护"，所以要取反再截 12 位；本函数**只读**，调用前后
*               OPTCR / OPTCR1 逐位不变。
*
*          **不证明**：真擦除/编程时间、擦除期间的取指停顿、写保护与
*          RDP 在真片上的行为、掉电窗口、以及 `HAL_FLASHEx_Erase` 内部对
*          PSIZE / SNB 的落位。这些都是板级读数的账，本文不假装测到了。
*
*          两个档位只差一个宏：默认档 `CHIP_FLASH_SIZE` = 1 MB（16K×4 +
*          64K + 128K×3，扇区 0..7），`-DDRVFLASH_TWO_MB` 档把它抬到
*          2 MB（扇区 8..11 也编进来）。分区头的 `#ifndef` 让 `-D` 生效。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"
#include "drvflash.h"
#include "arkrtos_partition.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* ---------------- Flash 寄存器窗口 ---------------- */

#define T_FLASH_WIN      (0x40020000ul)   /* 64 KB 页，含 FLASH 块 0x40023C00 */
#define T_OPTCR          (*(volatile uint32 *)(0x40023C14u))
#define T_OPTCR1         (*(volatile uint32 *)(0x40023C18u))

#define T_BASE           ((uint32)CHIP_FLASH_BASE)
#define T_SIZE           ((uint32)CHIP_FLASH_SIZE)

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL %s\n", what);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got 0x%lX want 0x%lX\n", what, got, want);
    }
}

/* ---------------- HAL Flash 记录式桩 ---------------- */

#define T_MAX_PROG   (64u)

static int      g_locked;            /* 1 = 已上锁（复位后默认上锁） */
static uint32   g_unlocks;
static uint32   g_locks;
static int      g_bad_locked;        /* 未解锁就要求擦/写 */
static uint32   g_erase_calls;
static uint32   g_prog_calls;
static uint32   g_fail_erase_at;     /* 第 n 次擦除失败；0 = 不注入 */
static uint32   g_fail_prog_at;      /* 第 n 次编程失败；0 = 不注入 */

static FLASH_EraseInitTypeDef g_last_erase;
static uint32   g_erase_sector_error;
static uint32   g_prog_addr[T_MAX_PROG];
static uint64_t g_prog_data[T_MAX_PROG];
static uint32   g_prog_type[T_MAX_PROG];

static void hal_reset(void)
{
    g_locked          = 1;
    g_unlocks         = 0u;
    g_locks           = 0u;
    g_bad_locked      = 0;
    g_erase_calls     = 0u;
    g_prog_calls      = 0u;
    g_fail_erase_at   = 0u;
    g_fail_prog_at    = 0u;
    memset(&g_last_erase, 0, sizeof(g_last_erase));
    g_erase_sector_error = 0xDEADBEEFu;
    memset(g_prog_addr, 0, sizeof(g_prog_addr));
    memset(g_prog_data, 0, sizeof(g_prog_data));
    memset(g_prog_type, 0, sizeof(g_prog_type));
}

HAL_StatusTypeDef HAL_FLASH_Unlock(void)
{
    g_unlocks++;
    g_locked = 0;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Lock(void)
{
    g_locks++;
    g_locked = 1;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit,
                                    uint32_t *SectorError)
{
    if(g_locked != 0)
    {
        g_bad_locked = 1;
    }
    g_erase_calls++;
    if(pEraseInit != 0)
    {
        g_last_erase = *pEraseInit;
    }
    if(SectorError != 0)
    {
        *SectorError = 0xFFFFFFFFu;
    }
    if((g_fail_erase_at != 0u) && (g_erase_calls == g_fail_erase_at))
    {
        if(SectorError != 0)
        {
            *SectorError = g_erase_sector_error;
        }
        return HAL_ERROR;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address,
                                    uint64_t Data)
{
    if(g_locked != 0)
    {
        g_bad_locked = 1;
    }
    if(g_prog_calls < T_MAX_PROG)
    {
        g_prog_addr[g_prog_calls] = Address;
        g_prog_data[g_prog_calls] = Data;
        g_prog_type[g_prog_calls] = TypeProgram;
    }
    g_prog_calls++;
    if((g_fail_prog_at != 0u) && (g_prog_calls == g_fail_prog_at))
    {
        return HAL_ERROR;
    }
    return HAL_OK;
}

/* ---------------- 用例 ---------------- */

static void test_guards(void)
{
    arkrtos_flash_prot_t prot;
    uint8 dummy = 0u;

    hal_reset();

    /* 片外地址一律 0（不是"某个看似合法的扇区大小"）。 */
    ck_u(arkrtos_port_flash_sector_size(0u), 0u, "sector size at 0 is 0");
    ck_u(arkrtos_port_flash_sector_size(T_BASE - 1u), 0u,
         "sector size below the part is 0");
    ck_u(arkrtos_port_flash_sector_size(T_BASE + T_SIZE), 0u,
         "sector size at base+size is 0");
    ck_u(arkrtos_port_flash_sector_size(0xFFFFFFFFu), 0u,
         "sector size 0xFFFFFFFF is 0");

    /* 空操作不碰 HAL。 */
    ck(arkrtos_port_flash_erase(T_BASE, 0u) == 0, "erase of zero bytes returns 0");
    ck(arkrtos_port_flash_write(T_BASE, &dummy, 0u) == 0,
       "write of zero bytes returns 0");
    ck(arkrtos_port_flash_write(T_BASE, 0, 4u) == 0, "write of NULL data returns 0");
    ck_u(g_unlocks, 0u, "no unlock for empty operations");
    ck_u(g_erase_calls, 0u, "no erase call for zero length");
    ck_u(g_prog_calls, 0u, "no program call for empty writes");

    /* 保护位读数：NULL 输出指针拒绝。 */
    ck(arkrtos_port_flash_protection(0) == -1, "protection(NULL) is -1");
    memset(&prot, 0x5A, sizeof(prot));
    ck(arkrtos_port_flash_protection(&prot) == 0, "protection reads OPTCR");
}

static void test_sector_size(void)
{
    /* 16K 扇区 0..3：0x00000 / 0x04000 / 0x08000 / 0x0C000 起，各 16K。 */
    ck_u(arkrtos_port_flash_sector_size(T_BASE + 0x00000u), 16u * 1024u,
         "sector 0 size");
    ck_u(arkrtos_port_flash_sector_size(T_BASE + 0x03FFFu), 16u * 1024u,
         "last byte of sector 0");
    ck_u(arkrtos_port_flash_sector_size(T_BASE + 0x04000u), 16u * 1024u,
         "sector 1 start");
    ck_u(arkrtos_port_flash_sector_size(T_BASE + 0x0BFFFu), 16u * 1024u,
         "last byte of sector 2");
    ck_u(arkrtos_port_flash_sector_size(T_BASE + 0x0C000u), 16u * 1024u,
         "sector 3 start");
    ck_u(arkrtos_port_flash_sector_size(T_BASE + 0x0FFFFu), 16u * 1024u,
         "last byte of sector 3");

    /* 64K 扇区 4。 */
    ck_u(arkrtos_port_flash_sector_size(T_BASE + 0x10000u), 64u * 1024u,
         "sector 4 start");
    ck_u(arkrtos_port_flash_sector_size(T_BASE + 0x1FFFFu), 64u * 1024u,
         "last byte of sector 4");

    /* 128K 扇区 5 起。 */
    ck_u(arkrtos_port_flash_sector_size(T_BASE + 0x20000u), 128u * 1024u,
         "sector 5 start");
    ck_u(arkrtos_port_flash_sector_size(T_BASE + 0x3FFFFu), 128u * 1024u,
         "last byte of sector 5");
    ck_u(arkrtos_port_flash_sector_size(T_BASE + T_SIZE - 1u), 128u * 1024u,
         "last byte of the part");
}

static void test_erase_single(void)
{
    hal_reset();

    /* base+0x30000 落在扇区 5（5 + (0x10000/0x20000)）。 */
    ck(arkrtos_port_flash_erase(T_BASE + 0x30000u, 0x4000u) == 0,
       "single-sector erase succeeds");
    ck_u(g_erase_calls, 1u, "exactly one HAL erase");
    ck_u((unsigned long)g_last_erase.TypeErase, (unsigned long)FLASH_TYPEERASE_SECTORS,
         "erase type is sector");
    ck_u((unsigned long)g_last_erase.VoltageRange,
         (unsigned long)FLASH_VOLTAGE_RANGE_3, "voltage range is 2.7-3.6 V");
    ck_u((unsigned long)g_last_erase.Sector, 5u, "first sector is 5");
    ck_u((unsigned long)g_last_erase.NbSectors, 1u, "one sector");
    ck_u(g_unlocks, 1u, "erase unlocks once");
    ck_u(g_locks, 1u, "erase locks once");
    ck(g_locked == 1, "flash is locked after a successful erase");
    ck(g_bad_locked == 0, "HAL never saw a locked erase");
}

static void test_erase_span(void)
{
    hal_reset();

    /* 0x1000 在扇区 0 里，0x40000 字节把终点推到 base+0x40FFF（扇区 6），
     * 中途跨过 16K / 64K / 128K 三档边界，所以 0..6 一共 7 个扇区。 */
    ck(arkrtos_port_flash_erase(T_BASE + 0x1000u, 0x40000u) == 0,
       "range erase succeeds");
    ck_u(g_erase_calls, 1u, "range erase is one HAL call");
    ck_u((unsigned long)g_last_erase.Sector, 0u, "range starts at sector 0");
    ck_u((unsigned long)g_last_erase.NbSectors, 7u,
         "range covers sectors 0..6");
    ck_u(g_unlocks, 1u, "range erase unlocks once");
    ck_u(g_locks, 1u, "range erase locks once");

    /* 起点在扇区中部、终点在同一个扇区：仍然只擦一个整扇区。 */
    hal_reset();
    ck(arkrtos_port_flash_erase(T_BASE + 0x21000u, 0x10u) == 0,
       "mid-sector erase succeeds");
    ck_u((unsigned long)g_last_erase.Sector, 5u, "mid-sector maps to sector 5");
    ck_u((unsigned long)g_last_erase.NbSectors, 1u,
         "mid-sector still erases exactly one sector");
}

static void test_erase_invalid(void)
{
    hal_reset();

    ck(arkrtos_port_flash_erase(T_BASE - 1u, 0x1000u) == -1,
       "erase below the part is refused");
    ck(arkrtos_port_flash_erase(T_BASE + T_SIZE, 0x1000u) == -1,
       "erase at base+size is refused");
    ck(arkrtos_port_flash_erase(T_BASE, T_SIZE + 1u) == -1,
       "erase past the end is refused");
    ck(arkrtos_port_flash_erase(T_BASE + T_SIZE - 0x1000u, 0x2000u) == -1,
       "erase crossing the end is refused");

    /* 32 位回绕：终点绕回片内、且落在比起始更低的扇区。旧写法算出
     * last < first，NbSectors 下溢成 0xFFFFFFFF 交给 HAL。 */
    ck(arkrtos_port_flash_erase(T_BASE + 0x90000u, 0xFFFFFFF0u) == -1,
       "erase with a wrapping size is refused");
    ck(arkrtos_port_flash_erase(T_BASE + 0xFF000u, 0xFFFFFFF0u) == -1,
       "erase with a wrapping size near the top is refused");
    ck(arkrtos_port_flash_erase(0xFFFFFFF0u, 0x20u) == -1,
       "erase at a wrapping start is refused");

    ck_u(g_unlocks, 0u, "refused erases never unlock");
    ck_u(g_erase_calls, 0u, "refused erases never reach HAL");
}

static void test_erase_fail(void)
{
    hal_reset();
    g_fail_erase_at = 1u;
    g_erase_sector_error = 7u;

    ck(arkrtos_port_flash_erase(T_BASE + 0x30000u, 0x4000u) == -1,
       "HAL erase failure is reported");
    ck_u(g_erase_calls, 1u, "failed erase still made one HAL call");
    ck_u(g_unlocks, 1u, "failed erase unlocked once");
    ck_u(g_locks, 1u, "failed erase still locks the flash");
    ck(g_locked == 1, "flash is locked after a failed erase");
}

static void test_write(void)
{
    static const uint8 payload[4] = { 'A', 'B', 'C', 'D' };

    hal_reset();

    ck(arkrtos_port_flash_write(T_BASE + 0x100u, payload, 4u) == 0,
       "byte programming succeeds");
    ck_u(g_prog_calls, 4u, "one HAL program call per byte");
    ck_u(g_unlocks, 1u, "write unlocks once");
    ck_u(g_locks, 1u, "write locks once");
    ck(g_locked == 1, "flash is locked after a write");
    ck(g_bad_locked == 0, "HAL never saw a locked program");

    ck_u(g_prog_addr[0], (unsigned long)(T_BASE + 0x100u), "first byte address");
    ck_u(g_prog_addr[1], (unsigned long)(T_BASE + 0x101u), "second byte address");
    ck_u(g_prog_addr[2], (unsigned long)(T_BASE + 0x102u), "third byte address");
    ck_u(g_prog_addr[3], (unsigned long)(T_BASE + 0x103u), "fourth byte address");
    ck_u((unsigned long)g_prog_data[0], (unsigned long)'A', "first byte data");
    ck_u((unsigned long)g_prog_data[1], (unsigned long)'B', "second byte data");
    ck_u((unsigned long)g_prog_data[2], (unsigned long)'C', "third byte data");
    ck_u((unsigned long)g_prog_data[3], (unsigned long)'D', "fourth byte data");
    ck(g_prog_type[0] == (uint32)FLASH_TYPEPROGRAM_BYTE, "program type is byte");
    ck(g_prog_type[3] == (uint32)FLASH_TYPEPROGRAM_BYTE, "last program is byte too");
}

static void test_write_fail(void)
{
    static const uint8 payload[6] = { 1, 2, 3, 4, 5, 6 };

    hal_reset();
    g_fail_prog_at = 3u;

    ck(arkrtos_port_flash_write(T_BASE + 0x200u, payload, 6u) == -1,
       "programming failure is reported");
    ck_u(g_prog_calls, 3u, "programming stops at the failing byte");
    ck_u(g_unlocks, 1u, "failed write unlocked once");
    ck_u(g_locks, 1u, "failed write still locks the flash");
    ck(g_locked == 1, "flash is locked after a failed write");
}

static void test_write_invalid(void)
{
    static const uint8 payload[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

    hal_reset();

    ck(arkrtos_port_flash_write(T_BASE - 1u, payload, 1u) == -1,
       "write below the part is refused");
    ck(arkrtos_port_flash_write(T_BASE + T_SIZE, payload, 1u) == -1,
       "write at base+size is refused");
    ck(arkrtos_port_flash_write(T_BASE + T_SIZE - 1u, payload, 2u) == -1,
       "write crossing the end is refused");
    ck(arkrtos_port_flash_write(0xFFFFFFF0u, payload, 8u) == -1,
       "write at a wrapping start is refused");

    ck_u(g_unlocks, 0u, "refused writes never unlock");
    ck_u(g_prog_calls, 0u, "refused writes never reach HAL");
}

static void test_protection(void)
{
    arkrtos_flash_prot_t prot;
    uint32 optcr_before;
    uint32 optcr1_before;

    /* nWRP 位为 1 = 不保护。置 OPTCR[19:16] = 0x5，即扇区 0、2 被保护；
     * 取反截 12 位后掩码为 0xFFA。 */
    T_OPTCR  = (0xAAu << 8) | (0x005u << 16);
    T_OPTCR1 = (0x00Fu << 16);
    optcr_before  = T_OPTCR;
    optcr1_before = T_OPTCR1;

    ck(arkrtos_port_flash_protection(&prot) == 0, "protection reads succeed");
    ck_u((unsigned long)prot.rdp_level, 0u, "RDP non-zero reads as level 0");
    ck_u((unsigned long)prot.wrp_mask_lo, 0xFFAu, "low WRP mask inverts nWRP");
    ck_u((unsigned long)prot.wrp_mask_hi, 0xFF0u, "high WRP mask inverts nWRP");

    /* 只读：调用前后寄存器逐位不变。 */
    ck_u((unsigned long)T_OPTCR, (unsigned long)optcr_before,
         "protection does not write OPTCR");
    ck_u((unsigned long)T_OPTCR1, (unsigned long)optcr1_before,
         "protection does not write OPTCR1");

    /* RDP 清零 = 1 级读保护。 */
    T_OPTCR  = (0x00u << 8) | (0xFFFu << 16);
    T_OPTCR1 = (0xFFFu << 16);
    ck(arkrtos_port_flash_protection(&prot) == 0, "protection reads succeed again");
    ck_u((unsigned long)prot.rdp_level, 1u, "RDP zero reads as level 1");
    ck_u((unsigned long)prot.wrp_mask_lo, 0u, "all-protected low mask is 0");
    ck_u((unsigned long)prot.wrp_mask_hi, 0u, "all-protected high mask is 0");
}

/* ---------------- 收尾 ---------------- */

static int map_window(void)
{
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)T_FLASH_WIN, 64u * 1024u,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return (p == (void *)(uintptr_t)T_FLASH_WIN) ? 0 : -1;
#else
    void *p = mmap((void *)(uintptr_t)T_FLASH_WIN, 64u * 1024u,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                   -1, 0);
    return (p == (void *)(uintptr_t)T_FLASH_WIN) ? 0 : -1;
#endif
}

int main(void)
{
    if(map_window() != 0)
    {
        printf("DRVFLASH-HOST FAIL cannot map 0x%lX\n",
               (unsigned long)T_FLASH_WIN);
        return 1;
    }

    T_OPTCR  = 0u;
    T_OPTCR1 = 0u;

    test_guards();
    test_sector_size();
    test_erase_single();
    test_erase_span();
    test_erase_invalid();
    test_erase_fail();
    test_write();
    test_write_fail();
    test_write_invalid();
    test_protection();

    if(g_fails != 0)
    {
        printf("DRVFLASH-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("DRVFLASH-HOST OK  %d checks\n", g_checks);
    return 0;
}
