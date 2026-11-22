/**
* @file test_flashapp_host.c
* @brief 产物落 Flash 并引导执行（arkrtos_flashapp.c）的宿主用例
* @details 这条路径要证明的是「烧进去的那份，和文件逐字节一样」，以及
*          「起任务前把不属于这条地址链的镜像挡在门外」。三段职责：
*
*            写：先整份读一遍验形状（长度 / 向量表 / CRC）→ 擦整区 →
*                分块写 → 回读逐字节比 → 最后写区尾元数据。先验后擦，
*                验不过时区里那份还能跑的程序分毫未动。
*            查：元数据（魔数 / 版本 / 长度）与向量表（SP / 入口非零）
*                同时自洽才算「这里有产物」。
*            跑：判并发 → 查元数据 → 复核入口（区内 + arkc 序言）→
*                借数据窗口（首适配必须正好是那一块）→ 起任务（rom 记
*                Flash 区、ram 记借到的块）→ 发布并激活。
*
*          宿主上必须解决两件事：模块按**绝对地址**读写 Flash 应用区与
*          RAM 数据窗口，所以 main() 先真映射 [FLASHAPP_BASE, +SIZE) 与
*          [SLOT_RAM_BASE, +SLOT_RAM_TOTAL)；以及这些板级常量在 f427 的
*          配置里没有定义（那是 h743 的板级地址声明），由编译命令行按
*          h743 的数值注入，用例再核对注入的形状自洽——这条断言保证
*          「测的地址就是板上会用的地址形状」。
*
*          Flash 端口用映射出来的 RAM 假装：擦 = 填 0xFF，写 = memcpy，
*          外加「第 n 次写失败 / 写后篡改一个字节 / 第 n 次读返回 0」
*          几个旋钮，专门打各条失败出口。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "arkrtos_partition.h"
#include "arkrtos_flashapp.h"
#include "arkrtos_cfg.h"
#include "arkrtos_ptable.h"
#include "arkrtos_task.h"
#include "arkrtos_mpu.h"
#include "arkrtos_spin.h"
#include "arkrtos_vfs.h"
#include "arkrtos_share.h"
#include "arkrtos_loader.h"
#include "arkrtos_mini.h"
#include "arkrtos_log.h"

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
        printf("FAIL: %s (got %lu want %lu)\n", what, got, want);
    }
}

/* ---------------- 绝对地址映射 ---------------- */

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

/* ---------------- 端口 ---- */

static unsigned long g_crit_enters;
static unsigned long g_crit_exits;
static unsigned long g_dmb_calls;

uint32 arkrtos_port_enter_critical(void)
{
    return (uint32)(++g_crit_enters);
}

void arkrtos_port_exit_critical(uint32 state)
{
    (void)state;
    g_crit_exits++;
}

void arkrtos_port_dmb(void)
{
    g_dmb_calls++;
}

/* 自旋锁的 fallback 档（ARKRTOS_USE_SPINLOCK=0）除了临界区与屏障之外，
 * 还会问一次核号、并在等待时打一个提示：单核恒为 0，提示是空操作。 */
uint32 arkrtos_port_cpu_id(void)
{
    return 0u;
}

void arkrtos_port_spin_hint(void)
{
}

/* ---------------- 日志桩 ---------------- */

/* 不把日志编译掉（那样 ARKRTOS_LOGI 的参数就没人用，三个参数会在
 * -Wextra 下报未使用）；改为把级别压到最低，一条也不输出。 */
static unsigned long g_log_emits;

uint32 arkrtos_log_get_level(void)
{
    return 0u;
}

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level;
    (void)tag;
    (void)line;
    (void)msg;
    g_log_emits++;
}

/* ---------------- VFS 桩 ---------------- */

#define FILE_CAP (256 * 1024)

static uint8  g_file[FILE_CAP];
static uint32 g_file_len;            /* 文件真实长度 */
static uint32 g_read_pos;
static int32  g_open_fail_at;        /* 第 n 次 open 返回 -1（从 1 起，0 = 不注错） */
static int32  g_read_stop_at;        /* 第 n 次 read 返回 0（从 1 起，0 = 不注错） */
static int32  g_read_fail_at;        /* 第 n 次 read 返回 -1（从 1 起，0 = 不注错） */
static unsigned long g_open_calls;
static unsigned long g_read_calls;
static unsigned long g_close_calls;
static uint8  g_vfs_ready;

uint8 arkrtos_vfs_ready(void)
{
    return g_vfs_ready;
}

int32 arkrtos_vfs_open_read(const char *path)
{
    (void)path;
    g_open_calls++;
    if((g_open_fail_at != 0) && (g_open_calls == (unsigned long)g_open_fail_at))
    {
        return -1;
    }
    g_read_pos = 0u;
    return 3;
}

int32 arkrtos_vfs_read(int32 fd, uint8 *buf, uint32 len)
{
    uint32 avail;
    uint32 n;

    (void)fd;
    g_read_calls++;
    if((g_read_fail_at != 0) && (g_read_calls == (unsigned long)g_read_fail_at))
    {
        return -1;
    }
    if((g_read_stop_at != 0) && (g_read_calls == (unsigned long)g_read_stop_at))
    {
        return 0;
    }
    if(g_read_pos >= g_file_len)
    {
        return 0;
    }
    avail = g_file_len - g_read_pos;
    n = (avail < len) ? avail : len;
    memcpy(buf, &g_file[g_read_pos], n);
    g_read_pos += n;
    return (int32)n;
}

int32 arkrtos_vfs_close(int32 fd)
{
    (void)fd;
    g_close_calls++;
    return 0;
}

/* ---------------- Flash 端口桩（把映射出来的 RAM 当 Flash） ---------------- */

#define WRITE_LOG_MAX 32

static int32  g_erase_rc;
static int32  g_write_fail_at;      /* 第 n 次 write 返回 -1（从 1 起） */
static int32  g_write_flip;         /* 1 = 写入后把该块最后一个字节取反；2 = 只动元数据 */
static unsigned long g_erase_calls;
static uint32 g_erase_addr;
static uint32 g_erase_size;
static unsigned long g_write_calls;
static uint32 g_write_addr[WRITE_LOG_MAX];
static uint32 g_write_len[WRITE_LOG_MAX];

int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    g_erase_calls++;
    g_erase_addr = addr;
    g_erase_size = size;
    if(g_erase_rc != 0)
    {
        return g_erase_rc;
    }
    memset((void *)(uintptr_t)addr, 0xFF, (size_t)size);
    return 0;
}

int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    g_write_calls++;
    if(g_write_calls <= (unsigned long)WRITE_LOG_MAX)
    {
        g_write_addr[g_write_calls - 1u] = addr;
        g_write_len[g_write_calls - 1u]  = len;
    }
    if((g_write_fail_at != 0) && (g_write_calls == (unsigned long)g_write_fail_at))
    {
        return -1;
    }
    memcpy((void *)(uintptr_t)addr, data, (size_t)len);
    if((g_write_flip == 1) && (len != 0u) && (addr < (uint32)ARKRTOS_FLASHAPP_META_BASE))
    {
        ((volatile uint8 *)(uintptr_t)(addr + len - 1u))[0] ^= 0xFFu;
    }
    if((g_write_flip == 2) && (addr == (uint32)ARKRTOS_FLASHAPP_META_BASE))
    {
        ((volatile uint8 *)(uintptr_t)addr)[0] ^= 0xFFu;
    }
    return 0;
}

/* ---------------- 分区表桩 ---------------- */

static int32  g_alloc_rc;
static unsigned long g_alloc_calls;
static uint32 g_alloc_code_bytes;
static uint32 g_alloc_ram_bytes;
static uint32 g_alloc_pt;
static uint32 g_alloc_c_base;
static uint32 g_alloc_c_size;
static uint32 g_alloc_r_base;
static uint32 g_alloc_r_size;
static int32  g_free_rc;
static unsigned long g_free_calls;
static uint32 g_free_index;
static unsigned long g_set_slot_calls;
static uint32 g_set_slot_index;
static uint32 g_set_slot_state;
static uint32 g_set_slot_entry;
static uint32 g_set_slot_task;

int32 arkrtos_ptable_mini_alloc(uint32 code_bytes, uint32 ram_bytes,
                              uint32 *p_index,
                              uint32 *p_code_base, uint32 *p_code_size,
                              uint32 *p_ram_base, uint32 *p_ram_size)
{
    g_alloc_calls++;
    g_alloc_code_bytes = code_bytes;
    g_alloc_ram_bytes  = ram_bytes;
    if(g_alloc_rc < 0)
    {
        return g_alloc_rc;
    }
    *p_index     = g_alloc_pt;
    *p_code_base = g_alloc_c_base;
    *p_code_size = g_alloc_c_size;
    *p_ram_base  = g_alloc_r_base;
    *p_ram_size  = g_alloc_r_size;
    return (int32)g_alloc_pt;
}

int32 arkrtos_ptable_mini_free(uint32 index)
{
    g_free_calls++;
    g_free_index = index;
    return g_free_rc;
}

int32 arkrtos_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    g_set_slot_calls++;
    g_set_slot_index = slot;
    g_set_slot_state = state;
    g_set_slot_entry = entry;
    g_set_slot_task  = task_id;
    return 0;
}

/* ---------------- 任务表 / 注册 / MPU ---------------- */

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];

static int32 g_task_rc;
static unsigned long g_task_calls;
static void (*g_task_entry)(void);
static uint32 *g_task_stack;
static uint32 g_task_stack_size;
static uint32 g_task_prio;
static uint32 g_task_period;

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom, uint32 stack_size,
                          uint8 priority, uint32 period_ms)
{
    g_task_calls++;
    g_task_entry      = entry;
    g_task_stack      = stack_bottom;
    g_task_stack_size = stack_size;
    g_task_prio       = (uint32)priority;
    g_task_period     = period_ms;
    return g_task_rc;
}

static unsigned long g_mpu_calls;
static arkrtos_task_t *g_mpu_task;

void arkrtos_mpu_build_task(arkrtos_task_t *p_task)
{
    g_mpu_calls++;
    g_mpu_task = p_task;
}

/* ---------------- 调度器桩 ---------------- */

static unsigned long g_sched_lock_calls;
static unsigned long g_sched_unlock_calls;
static unsigned long g_activate_calls;
static uint32 g_activate_prio;

void arkrtos_sched_lock_internal(void)
{
    g_sched_lock_calls++;
}

uint32 arkrtos_sched_unlock_internal(void)
{
    g_sched_unlock_calls++;
    return 0u;
}

void arkrtos_sched_activate_higher(uint8 ck_pri)
{
    g_activate_calls++;
    g_activate_prio = (uint32)ck_pri;
}

/* ---------------- 夹具 ---------------- */

#define FA_BASE   ((uint32)ARKRTOS_FLASHAPP_BASE)
#define FA_SIZE   ((uint32)ARKRTOS_FLASHAPP_SIZE)
#define FA_END    ((uint32)ARKRTOS_FLASHAPP_END)
#define FA_META   ((uint32)ARKRTOS_FLASHAPP_META_BASE)
#define FA_MAX    ((uint32)ARKRTOS_FLASHAPP_MAX_IMAGE)
#define FA_RAM    ((uint32)ARKRTOS_FLASHAPP_RAM_BASE)
#define FA_RAMSZ  ((uint32)ARKRTOS_FLASHAPP_RAM_SIZE)
#define FA_STACK  ((uint32)ARKRTOS_FLASHAPP_STACK_SIZE)
#define FA_TOP    (FA_RAM + FA_RAMSZ)

#define WIN_CODE_BASE ((uint32)ARKRTOS_APPRUN_BASE)
#define WIN_CODE_SIZE ((uint32)ARKRTOS_APPRUN_CODE_SIZE)

#define IMG_SMALL  (64u)
#define IMG_BIG    (2048u)
#define IMG_ENTRY  (0x10u)

/* 用独立实现（Python zlib）算出来的已知答案：镜像形状固定，所以 CRC 可核。 */
#define CRC_SMALL  (0xA2598A98u)
#define CRC_BIG    (0xDBCFF55Bu)

typedef struct
{
    uint32 magic;
    uint32 len;
    uint32 crc;
    uint32 version;
    uint32 rsv[4];
} t_meta_t;

static void flash_set(uint32 off, const void *p, uint32 n)
{
    memcpy((void *)(uintptr_t)(FA_BASE + off), p, (size_t)n);
}

static void flash_fill(uint8 v)
{
    memset((void *)(uintptr_t)FA_BASE, v, (size_t)FA_SIZE);
}

static void flash_meta(uint32 magic, uint32 len, uint32 crc, uint32 version)
{
    t_meta_t m;

    memset(&m, 0, sizeof(m));
    m.magic   = magic;
    m.len     = len;
    m.crc     = crc;
    m.version = version;
    flash_set(FA_META - FA_BASE, &m, (uint32)sizeof(m));
}

/* 造文件：+0 = SP，+4 = 入口（带 Thumb 位），入口处 = 序言半字，其余填 0。 */
static void build_file(uint32 len, uint32 entry_off, uint32 sp, uint16 pro)
{
    memset(g_file, 0, sizeof(g_file));
    g_file_len = len;
    if(len >= 4u)
    {
        g_file[0] = (uint8)(sp & 0xFFu);
        g_file[1] = (uint8)((sp >> 8) & 0xFFu);
        g_file[2] = (uint8)((sp >> 16) & 0xFFu);
        g_file[3] = (uint8)((sp >> 24) & 0xFFu);
    }
    if(len >= 8u)
    {
        uint32 vec = (FA_BASE + entry_off) | 1u;

        g_file[4] = (uint8)(vec & 0xFFu);
        g_file[5] = (uint8)((vec >> 8) & 0xFFu);
        g_file[6] = (uint8)((vec >> 16) & 0xFFu);
        g_file[7] = (uint8)((vec >> 24) & 0xFFu);
    }
    if((entry_off + 2u) <= len)
    {
        g_file[entry_off]      = (uint8)(pro & 0xFFu);
        g_file[entry_off + 1u] = (uint8)((pro >> 8) & 0xFFu);
    }
}

/* 位级改写向量表：让入口落在想测的位置（文件内容跟着改，CRC 才自洽）。 */
static void set_vec(uint32 vec)
{
    g_file[4] = (uint8)(vec & 0xFFu);
    g_file[5] = (uint8)((vec >> 8) & 0xFFu);
    g_file[6] = (uint8)((vec >> 16) & 0xFFu);
    g_file[7] = (uint8)((vec >> 24) & 0xFFu);
}

/* 只清调用计数，不动模块状态（正在跑的任务要继续跑）。 */
static void reset_counters_only(void)
{
    g_open_calls  = 0ul;
    g_read_calls  = 0ul;
    g_close_calls = 0ul;
    g_erase_calls = 0ul;
    g_erase_addr  = 0u;
    g_erase_size  = 0u;
    g_write_calls = 0ul;
    memset(g_write_addr, 0, sizeof(g_write_addr));
    memset(g_write_len, 0, sizeof(g_write_len));
    g_alloc_calls = 0ul;
    g_free_calls  = 0ul;
    g_free_index  = 0u;
    g_set_slot_calls = 0ul;
    g_sched_lock_calls   = 0ul;
    g_sched_unlock_calls = 0ul;
    g_activate_calls     = 0ul;
    g_activate_prio      = 0u;
    g_task_calls      = 0ul;
    g_mpu_calls       = 0ul;
}

/* 直接摆一份「已经在 Flash 里」的产物（跳过写路径，专测查/跑）。 */
static void flash_image(uint32 len, uint32 entry_off, uint32 sp, uint16 pro)
{
    uint8 hdr[8];
    uint32 vec = (FA_BASE + entry_off) | 1u;
    uint16 p = pro;
    uint32 i;

    flash_fill(0xFFu);
    hdr[0] = (uint8)(sp & 0xFFu);
    hdr[1] = (uint8)((sp >> 8) & 0xFFu);
    hdr[2] = (uint8)((sp >> 16) & 0xFFu);
    hdr[3] = (uint8)((sp >> 24) & 0xFFu);
    hdr[4] = (uint8)(vec & 0xFFu);
    hdr[5] = (uint8)((vec >> 8) & 0xFFu);
    hdr[6] = (uint8)((vec >> 16) & 0xFFu);
    hdr[7] = (uint8)((vec >> 24) & 0xFFu);
    flash_set(0u, hdr, 8u);
    for(i = 8u; i < len; i++)
    {
        ((volatile uint8 *)(uintptr_t)(FA_BASE + i))[0] = 0x00u;
    }
    flash_set(entry_off, &p, 2u);
    flash_meta(ARKRTOS_FLASHAPP_MAGIC, len, 0x12345678u,
               ARKRTOS_FLASHAPP_VERSION);
}

static void reset_all(void)
{
    /* 模块是单实例状态（g_flashapp_task），没有公开的重置接口。上一段用例
     * 跑起来的程序必须先按真实路径退出，否则本段一进 run/write 就吃到 BUSY。 */
    {
        uint32 running = arkrtos_flashapp_task_id();

        if(running != 0u)
        {
            (void)arkrtos_flashapp_on_task_exit(running);
        }
    }

    memset(g_file, 0, sizeof(g_file));
    g_file_len = 0u;
    g_read_pos = 0u;
    g_open_fail_at = 0;
    g_read_stop_at = 0;
    g_read_fail_at = 0;
    g_open_calls  = 0ul;
    g_read_calls  = 0ul;
    g_close_calls = 0ul;
    g_vfs_ready   = 1u;

    g_erase_rc    = 0;
    g_write_fail_at = 0;
    g_write_flip  = 0;
    g_erase_calls = 0ul;
    g_erase_addr  = 0u;
    g_erase_size  = 0u;
    g_write_calls = 0ul;
    memset(g_write_addr, 0, sizeof(g_write_addr));
    memset(g_write_len, 0, sizeof(g_write_len));

    g_alloc_rc = 0;
    g_alloc_calls = 0ul;
    g_alloc_code_bytes = 0u;
    g_alloc_ram_bytes  = 0u;
    g_alloc_pt     = 7u;
    g_alloc_c_base = WIN_CODE_BASE;
    g_alloc_c_size = WIN_CODE_SIZE;
    g_alloc_r_base = FA_RAM;
    g_alloc_r_size = FA_RAMSZ;

    g_free_rc    = 0;
    g_free_calls = 0ul;
    g_free_index = 0u;
    g_set_slot_calls = 0ul;
    g_set_slot_index = 0u;
    g_set_slot_state = 0u;
    g_set_slot_entry = 0u;
    g_set_slot_task  = 0u;

    g_task_rc         = 5;
    g_task_calls      = 0ul;
    g_task_entry      = 0;
    g_task_stack      = 0;
    g_task_stack_size = 0u;
    g_task_prio       = 0u;
    g_task_period     = 1u;
    g_mpu_calls       = 0ul;
    g_mpu_task        = 0;
    g_sched_lock_calls   = 0ul;
    g_sched_unlock_calls = 0ul;
    g_activate_calls     = 0ul;
    g_activate_prio      = 0u;
    g_log_emits   = 0ul;
    g_crit_enters = 0ul;
    g_crit_exits  = 0ul;
    g_dmb_calls   = 0ul;

    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    flash_fill(0xFFu);
}

/* ---------------- 用例 ---------------- */

static void test_geometry(void)
{
    /* 注入的板级常量必须与 h743 那张表同形：区顶到 Flash 末尾、元数据占
     * 最后一个 flash word、数据窗口就是 APPRUN 那块。 */
    ck_u(FA_END, FA_BASE + FA_SIZE, "area end = base + size");
    ck_u(FA_META + 32u, FA_END, "metadata is the last 32 bytes");
    ck_u(FA_MAX + 64u, FA_SIZE, "image cap leaves room for two metadata words");
    ck_u(FA_RAM, (uint32)ARKRTOS_APPRUN_RAM_BASE, "flash app shares the apprun data window");
    ck_u(FA_RAMSZ, (uint32)ARKRTOS_APPRUN_RAM_SIZE, "same window size as apprun");
    ck_u(FA_STACK, (uint32)ARKRTOS_APPRUN_STACK_SIZE, "same stack size as apprun");
    ck((FA_STACK <= FA_RAMSZ), "stack fits the data window");
    ck((WIN_CODE_BASE + WIN_CODE_SIZE) == FA_RAM, "code window is right before the data window");
    ck((FA_BASE >= 0x08100000u), "flash app area is in bank 2 (read-while-write)");
    ck(((FA_BASE % FA_SIZE) == 0u), "area starts on an erase-unit boundary");
    ck((FA_END == 0x08200000u), "area ends at the end of the 2 MB flash");
    ck((FA_RAM >= (uint32)SLOT_RAM_BASE), "data window starts inside SLOT_RAM");
    ck(((FA_RAM + FA_RAMSZ) <= ((uint32)SLOT_RAM_BASE + (uint32)SLOT_RAM_TOTAL)),
       "data window fits the pool");
}

static void test_write_param_gates(void)
{
    uint32 len = 0u;

    /* 空路径：一个外部调用都不该发生。 */
    reset_all();
    ck_u((unsigned long)arkrtos_flashapp_write(0, &len),
         (unsigned long)ARKRTOS_LOADER_ERR_PARAM, "null path refused");
    ck_u(g_open_calls, 0ul, "null path does not touch the filesystem");
    ck_u(g_erase_calls, 0ul, "null path does not erase");

    /* 文件系统没挂：直接 STATE，不动 Flash。 */
    reset_all();
    g_vfs_ready = 0u;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_LOADER_ERR_STATE, "no filesystem -> STATE");
    ck_u(g_erase_calls, 0ul, "no filesystem does not erase");

    /* 打开失败：FS，区里那份还能跑的东西分毫未动（先验后擦）。 */
    reset_all();
    flash_fill(0x5Au);
    g_open_fail_at = 1;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_MINI_ERR_FS, "open failure -> FS");
    ck_u(g_erase_calls, 0ul, "open failure does not erase");
    ck_u(g_close_calls, 0ul, "nothing to close when open failed");
    ck(((volatile uint8 *)(uintptr_t)FA_BASE)[0] == 0x5Au,
       "existing content survived an open failure");

    /* 读失败：FS，同样不擦。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_read_fail_at = 1;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_MINI_ERR_FS, "read failure -> FS");
    ck_u(g_erase_calls, 0ul, "read failure does not erase");
    ck_u(g_close_calls, 1ul, "the file is closed after a read failure");

    /* 空文件 / 太短：都判 SIZE，且都不擦。 */
    reset_all();
    g_file_len = 0u;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_LOADER_ERR_SIZE, "empty file -> SIZE");
    ck_u(g_erase_calls, 0ul, "empty file does not erase");

    reset_all();
    build_file(4u, 0u, FA_TOP, 0u);
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_LOADER_ERR_SIZE, "4-byte file -> SIZE");
    ck_u(g_erase_calls, 0ul, "short file does not erase");

    /* 超过区上限：边扫边拒，读到越界就停，不擦。 */
    reset_all();
    memset(g_file, 0, sizeof(g_file));
    g_file_len = FA_MAX + 1u;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_LOADER_ERR_SIZE, "oversized file -> SIZE");
    ck_u(g_erase_calls, 0ul, "oversized file does not erase");

    /* 形状四道闸：SP 不对 / 入口在区外 / 入口超出负载 / 序言不对。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP + 4u, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_LAYOUT, "wrong stack pointer -> LAYOUT");
    ck_u(g_erase_calls, 0ul, "wrong SP does not erase");

    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    set_vec((ARKRTOS_FLASHAPP_BASE - 8u) | 1u);
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_LAYOUT, "entry before the area -> LAYOUT");
    ck_u(g_erase_calls, 0ul, "entry before the area does not erase");

    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    set_vec((FA_BASE + IMG_SMALL) | 1u);
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_LAYOUT, "entry past the payload -> LAYOUT");
    ck_u(g_erase_calls, 0ul, "entry past the payload does not erase");

    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, 0x1234u);
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_PROLOGUE, "wrong prologue -> PROLOGUE");
    ck_u(g_erase_calls, 0ul, "wrong prologue does not erase");
}

static void test_write_success(void)
{
    uint32 len = 0u;

    /* 小镜像：一条 payload 写 + 一条元数据写。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len), 0ul,
         "small image written");
    ck_u(len, IMG_SMALL, "length reported back");
    ck_u(g_erase_calls, 1ul, "area erased exactly once");
    ck_u(g_erase_addr, FA_BASE, "erased from the area base");
    ck_u(g_erase_size, FA_SIZE, "erased the whole area");
    ck_u(g_write_calls, 2ul, "one payload write + one metadata write");
    ck_u(g_write_addr[0], FA_BASE, "payload lands at the area base");
    ck_u(g_write_len[0], IMG_SMALL, "payload length");
    ck_u(g_write_addr[1], FA_META, "metadata lands at the area tail");
    ck_u(g_write_len[1], 32ul, "metadata is one flash word");
    ck((memcmp((const void *)(uintptr_t)FA_BASE, g_file, IMG_SMALL) == 0),
       "flash content equals the file");
    ck_u(g_sched_lock_calls, g_sched_unlock_calls, "scheduler lock is balanced");

    {
        uint32 got_len = 0u;
        uint32 got_crc = 0u;
        uint32 got_entry = 0u;

        ck_u((unsigned long)arkrtos_flashapp_info(&got_len, &got_crc, &got_entry), 0ul,
             "info accepts what write just produced");
        ck_u(got_len, IMG_SMALL, "info length");
        ck_u(got_crc, CRC_SMALL, "CRC matches the independent zlib answer");
        ck_u(got_entry, (FA_BASE + IMG_ENTRY) | 1u, "info entry keeps the Thumb bit");
        ck_u((unsigned long)arkrtos_flashapp_info(0, 0, 0), 0ul,
             "info with all outputs null is fine");
    }

    /* 跨 chunk 的镜像：入口在第一个 chunk，第二个 chunk 里 pro_off 已经
     * 落在读窗口之前——这条分支不跨块是走不到的。 */
    reset_all();
    build_file(IMG_BIG, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    len = 0u;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/big.bin", 0), 0ul,
         "chunked image written (null length out is allowed)");
    ck_u(g_write_calls, 3ul, "two payload writes + one metadata write");
    ck_u(g_write_addr[1], FA_BASE + 1024u, "second payload chunk follows the first");
    ck((memcmp((const void *)(uintptr_t)FA_BASE, g_file, IMG_BIG) == 0),
       "chunked flash content equals the file");
    ck_u((unsigned long)arkrtos_flashapp_info(&len, 0, 0), 0ul, "info after chunked write");
    ck_u(len, IMG_BIG, "chunked length");
    {
        uint32 got_crc = 0u;

        ck_u((unsigned long)arkrtos_flashapp_info(0, &got_crc, 0), 0ul, "info crc");
        ck_u(got_crc, CRC_BIG, "chunked CRC matches the independent zlib answer");
    }

    /* 正在跑的时候不许再写：那条路径要擦的正是正在取指的区。 */
    {
        uint32 task = 0u;

        ck_u((unsigned long)arkrtos_flashapp_run(&task), 0ul, "image starts");
        reset_counters_only();
        ck_u((unsigned long)arkrtos_flashapp_write("/tmp/big.bin", &len),
             (unsigned long)ARKRTOS_FLASHAPP_ERR_BUSY, "write while running -> BUSY");
        ck_u(g_erase_calls, 0ul, "busy write does not erase");
        ck_u(g_open_calls, 0ul, "busy write does not even open the file");
        (void)arkrtos_flashapp_on_task_exit(task);
    }
}

static void test_write_flash_failures(void)
{
    uint32 len = 0u;

    /* 擦失败：如实报 FLASH，且不假装区里还有东西。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_erase_rc = -1;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_FLASH, "erase failure -> FLASH");
    ck_u(g_write_calls, 0ul, "no programming after a failed erase");
    ck_u(g_sched_lock_calls, g_sched_unlock_calls, "scheduler lock is balanced on failure");
    ck_u(arkrtos_flashapp_task_id(), 0ul, "gate released after a failed erase");

    /* 写入时打开失败（擦已经发生，不回滚）。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_open_fail_at = 2;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_MINI_ERR_FS, "reopen for programming fails -> FS");
    ck_u(g_erase_calls, 1ul, "the area was already erased");
    ck_u(arkrtos_flashapp_task_id(), 0ul, "gate released");

    /* 写入时读不到数据（文件被改了 / 卷被卸了）。扫描两次读（数据 + EOF），
     * 写入路径的第一次读是第 3 次。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_read_stop_at = 3;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_MINI_ERR_FS, "short read while programming -> FS");

    /* 底层写失败。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_write_fail_at = 1;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_FLASH, "program failure -> FLASH");
    ck_u(arkrtos_flashapp_task_id(), 0ul, "gate released after a program failure");

    /* 回读阶段打不开文件：open#1 扫描、open#2 写入、open#3 回读。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_open_fail_at = 3;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_MINI_ERR_FS, "reopen for verify fails -> FS");

    /* 回读阶段读不到数据：扫描 2 次、写入 1 次，回读是第 4 次。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_read_stop_at = 4;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_MINI_ERR_FS, "short read while verifying -> FS");

    /* 回读不一致：写入成功但落进去的字节被改了。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_write_flip = 1;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_VERIFY, "payload mismatch -> VERIFY");
    ck_u(g_write_calls, 1ul, "metadata is not written after a payload mismatch");

    /* 元数据写失败。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_write_fail_at = 2;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_FLASH, "metadata write failure -> FLASH");

    /* 元数据回读不一致。 */
    reset_all();
    build_file(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_write_flip = 2;
    ck_u((unsigned long)arkrtos_flashapp_write("/tmp/a.bin", &len),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_VERIFY, "metadata mismatch -> VERIFY");
    ck_u((unsigned long)arkrtos_flashapp_info(0, 0, 0),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_META,
         "a corrupted metadata word leaves no usable image");
}

static void test_erase(void)
{
    reset_all();
    memset((void *)(uintptr_t)FA_BASE, 0x00, (size_t)FA_SIZE);
    ck_u((unsigned long)arkrtos_flashapp_erase(), 0ul, "erase succeeds");
    ck_u(g_erase_calls, 1ul, "one erase");
    ck_u(g_erase_addr, FA_BASE, "erased at the area base");
    ck_u(g_erase_size, FA_SIZE, "erased the whole area");
    ck(((volatile uint8 *)(uintptr_t)FA_BASE)[0] == 0xFFu, "first byte is erased");
    ck(((volatile uint8 *)(uintptr_t)FA_META)[0] == 0xFFu, "metadata word is erased");
    ck_u((unsigned long)arkrtos_flashapp_info(0, 0, 0),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_META, "erased area has no image");

    reset_all();
    g_erase_rc = -1;
    ck_u((unsigned long)arkrtos_flashapp_erase(),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_FLASH, "erase failure -> FLASH");

    /* 正在跑的时候不许擦：那块 Flash 正在被取指。 */
    reset_all();
    flash_image(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    {
        uint32 task = 0u;

        ck_u((unsigned long)arkrtos_flashapp_run(&task), 0ul, "image starts");
        reset_counters_only();
        ck_u((unsigned long)arkrtos_flashapp_erase(),
             (unsigned long)ARKRTOS_FLASHAPP_ERR_BUSY, "erase while running -> BUSY");
        ck_u(g_erase_calls, 0ul, "busy erase does not touch the flash");
        (void)arkrtos_flashapp_on_task_exit(task);
    }
}

static void test_info(void)
{
    reset_all();
    ck_u((unsigned long)arkrtos_flashapp_info(0, 0, 0),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_META, "blank area has no image");

    flash_image(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    flash_meta(0x12345678u, IMG_SMALL, 1u, ARKRTOS_FLASHAPP_VERSION);
    ck_u((unsigned long)arkrtos_flashapp_info(0, 0, 0),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_META, "wrong magic -> META");

    flash_meta(ARKRTOS_FLASHAPP_MAGIC, IMG_SMALL, 1u, 99u);
    ck_u((unsigned long)arkrtos_flashapp_info(0, 0, 0),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_META, "wrong version -> META");

    flash_meta(ARKRTOS_FLASHAPP_MAGIC, 4u, 1u, ARKRTOS_FLASHAPP_VERSION);
    ck_u((unsigned long)arkrtos_flashapp_info(0, 0, 0),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_META, "length below 8 -> META");

    flash_meta(ARKRTOS_FLASHAPP_MAGIC, FA_MAX + 1u, 1u, ARKRTOS_FLASHAPP_VERSION);
    ck_u((unsigned long)arkrtos_flashapp_info(0, 0, 0),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_META, "length above the cap -> META");

    /* 元数据说没问题，但向量表里的 SP 不是数据窗口顶——以内存为准。 */
    flash_meta(ARKRTOS_FLASHAPP_MAGIC, IMG_SMALL, 1u, ARKRTOS_FLASHAPP_VERSION);
    {
        uint32 bad_sp = FA_TOP + 4u;

        flash_set(0u, &bad_sp, 4u);
    }
    ck_u((unsigned long)arkrtos_flashapp_info(0, 0, 0),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_META, "wrong vector SP -> META");

    /* SP 对，但入口是 0（半个字都没烧进去）。 */
    {
        uint32 sp = FA_TOP;
        uint32 vec = 0u;

        flash_set(0u, &sp, 4u);
        flash_set(4u, &vec, 4u);
    }
    ck_u((unsigned long)arkrtos_flashapp_info(0, 0, 0),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_META, "zero entry -> META");
    ck_u((unsigned long)arkrtos_flashapp_task_id(), 0ul, "info never leaves a gate held");
}

static void test_run_failures(void)
{
    uint32 task = 0u;

    reset_all();
    ck_u((unsigned long)arkrtos_flashapp_run(&task),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_META, "nothing burned -> META");
    ck_u(arkrtos_flashapp_task_id(), 0ul, "gate released after META");
    ck_u(g_alloc_calls, 0ul, "no window borrowed without an image");

    /* 入口在区外。 */
    reset_all();
    flash_image(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    {
        uint32 vec = (ARKRTOS_FLASHAPP_BASE - 8u) | 1u;

        flash_set(4u, &vec, 4u);
    }
    ck_u((unsigned long)arkrtos_flashapp_run(&task),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_LAYOUT, "entry before the area -> LAYOUT");

    /* 入口超出负载长度：落在本区但已经擦过了。 */
    reset_all();
    flash_image(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    {
        uint32 vec = (FA_BASE + IMG_SMALL) | 1u;

        flash_set(4u, &vec, 4u);
    }
    ck_u((unsigned long)arkrtos_flashapp_run(&task),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_LAYOUT, "entry past the payload -> LAYOUT");

    /* 入口首半字不是 arkc 序言。 */
    reset_all();
    flash_image(IMG_SMALL, IMG_ENTRY, FA_TOP, 0x1234u);
    ck_u((unsigned long)arkrtos_flashapp_run(&task),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_PROLOGUE, "wrong prologue -> PROLOGUE");
    ck_u(arkrtos_flashapp_task_id(), 0ul, "gate released after PROLOGUE");

    /* 池里拿不到窗。 */
    reset_all();
    flash_image(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_alloc_rc = -1;
    ck_u((unsigned long)arkrtos_flashapp_run(&task),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_WINDOW, "allocation failure -> WINDOW");
    ck_u(g_free_calls, 0ul, "nothing to free when allocation failed");
    ck_u(g_sched_lock_calls, g_sched_unlock_calls, "scheduler lock is balanced");

    /* 首适配给了别的地址：绝不在换过的地址上跑一份按固定地址链的产物。 */
    reset_all();
    flash_image(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_alloc_c_base = WIN_CODE_BASE + 0x100u;
    ck_u((unsigned long)arkrtos_flashapp_run(&task),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_WINDOW, "wrong code window -> WINDOW");
    ck_u(g_free_calls, 1ul, "the mismatched window is returned");
    ck_u(g_free_index, g_alloc_pt, "the returned window is the one that was borrowed");

    /* 数据窗口尺寸不对，同样拒绝。 */
    reset_all();
    flash_image(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_alloc_r_size = FA_RAMSZ / 2u;
    ck_u((unsigned long)arkrtos_flashapp_run(&task),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_WINDOW, "wrong data window size -> WINDOW");
    ck_u(g_free_calls, 1ul, "the mismatched window is returned");

    /* 任务注册失败：借来的窗必须在本地还掉，调用方不必再还。 */
    reset_all();
    flash_image(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    g_task_rc = 0;
    ck_u((unsigned long)arkrtos_flashapp_run(&task),
         (unsigned long)ARKRTOS_LOADER_ERR_TASK, "task registration failure -> TASK");
    ck_u(g_free_calls, 1ul, "the window is returned when the task cannot be created");
    ck_u(g_mpu_calls, 0ul, "no MPU windows without a task");
    ck_u(g_set_slot_calls, 0ul, "the pool record is not published without a task");
    ck_u(g_activate_calls, 0ul, "activate is not called without a task");
    ck_u(arkrtos_flashapp_task_id(), 0ul, "gate released after task failure");
}

static void test_run_success(void)
{
    uint32 task = 0u;

    reset_all();
    flash_image(IMG_BIG, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    ck_u((unsigned long)arkrtos_flashapp_run(&task), 0ul, "image starts");
    ck((task != 0u), "task id is reported");
    ck_u(task, (uint32)g_task_rc, "the reported task is the one registered");
    ck_u(arkrtos_flashapp_task_id(), task, "the module remembers its main task");

    ck_u(g_alloc_calls, 1ul, "one window borrowed");
    ck_u(g_alloc_code_bytes, (unsigned long)WIN_CODE_SIZE, "code window size requested");
    ck_u(g_alloc_ram_bytes, (unsigned long)FA_RAMSZ, "data window size requested");
    ck_u(g_task_calls, 1ul, "one task registered");
    ck(((uintptr_t)g_task_entry == (uintptr_t)(FA_BASE + IMG_ENTRY)),
       "entry is the vector-table reset handler (Thumb bit cleared)");
    ck(((uintptr_t)g_task_stack == (uintptr_t)(FA_RAM + FA_RAMSZ - FA_STACK)),
       "stack is the top 4 KB of the data window");
    ck_u(g_task_stack_size, (unsigned long)FA_STACK, "stack size");
    ck_u(g_task_prio, (unsigned long)ARKRTOS_FLASHAPP_PRIORITY, "priority");
    ck_u(g_task_period, (unsigned long)ARKRTOS_APPRUN_PERIOD_MS, "period");

    /* TCB 两窗口：rom 是 Flash 应用区，ram 是借到的数据块，非特权。 */
    ck_u(g_task_rc > 0 ? (uint32)arkrtos_task_table[g_task_rc - 1].rom_start : 0u,
         (unsigned long)FA_BASE, "rom window starts at the flash app area");
    ck_u(g_task_rc > 0 ? (uint32)arkrtos_task_table[g_task_rc - 1].rom_size : 0u,
         (unsigned long)FA_SIZE, "rom window covers the whole area");
    ck_u(g_task_rc > 0 ? (uint32)arkrtos_task_table[g_task_rc - 1].ram_start : 0u,
         (unsigned long)FA_RAM, "ram window starts at the borrowed data block");
    ck_u(g_task_rc > 0 ? (uint32)arkrtos_task_table[g_task_rc - 1].ram_size : 0u,
         (unsigned long)FA_RAMSZ, "ram window size");
    ck_u(g_task_rc > 0 ? (unsigned long)arkrtos_task_table[g_task_rc - 1].is_priv : 1ul,
         0ul, "flash app runs unprivileged");

    ck_u(g_mpu_calls, 1ul, "MPU windows are rebuilt for the new task");
    ck((g_mpu_task == &arkrtos_task_table[g_task_rc - 1]), "MPU was given this task's TCB");
    ck_u(g_set_slot_calls, 1ul, "the pool record is published");
    ck_u(g_set_slot_index, g_alloc_pt, "published against the borrowed slot");
    ck_u(g_set_slot_state, (unsigned long)ARKRTOS_APP_SLOT_RUNNING, "published as running");
    ck_u(g_set_slot_entry, (unsigned long)(FA_BASE + IMG_ENTRY), "published entry");
    ck_u(g_set_slot_task, task, "published task id");
    ck_u(g_activate_calls, 1ul, "scheduler asked to switch once");
    ck_u(g_activate_prio, (unsigned long)ARKRTOS_FLASHAPP_PRIORITY, "switched for the app priority");

    /* 第二次起跑必须被拒。 */
    reset_counters_only();
    ck_u((unsigned long)arkrtos_flashapp_run(0),
         (unsigned long)ARKRTOS_FLASHAPP_ERR_BUSY, "second run -> BUSY");
    ck_u(g_alloc_calls, 0ul, "a busy run does not borrow another window");

    /* 退出契约。 */
    ck_u((unsigned long)arkrtos_flashapp_on_task_exit(0u), 0ul, "task 0 exit is ignored");
    ck(arkrtos_flashapp_on_task_exit(task + 1u) == 0, "another task's exit is ignored");
    ck_u(g_free_calls, 0ul, "foreign exits do not return the window");
    ck_u(arkrtos_flashapp_task_id(), task, "still running after foreign exits");

    ck(arkrtos_flashapp_on_task_exit(task) == 0, "own exit is clean");
    ck_u(g_free_calls, 1ul, "the data window is returned");
    ck_u(g_free_index, g_alloc_pt, "the right window is returned");
    ck_u(arkrtos_flashapp_task_id(), 0ul, "state is cleared on exit");

    /* 归还失败（账本已经乱了）：退出照常返回 0，但状态必须清掉，否则
     * 这条路径会永远卡在 BUSY。 */
    reset_all();
    flash_image(IMG_SMALL, IMG_ENTRY, FA_TOP, (uint16)ARKRTOS_FLASHAPP_PROLOGUE);
    task = 0u;
    ck_u((unsigned long)arkrtos_flashapp_run(&task), 0ul, "second image starts");
    g_free_rc = -1;
    ck(arkrtos_flashapp_on_task_exit(task) == 0, "a failed return still reports a clean exit");
    ck_u(g_free_calls, 1ul, "the return was attempted");
    ck_u(arkrtos_flashapp_task_id(), 0ul, "the state is cleared even when the return fails");
}

static void test_error_names(void)
{
    ck(strcmp(arkrtos_flashapp_error_name(ARKRTOS_FLASHAPP_ERR_BUSY),
              "a direct-run program is already running") == 0, "name(BUSY)");
    ck(strcmp(arkrtos_flashapp_error_name(ARKRTOS_FLASHAPP_ERR_WINDOW),
              "the data window could not be taken") == 0, "name(WINDOW)");
    ck(strcmp(arkrtos_flashapp_error_name(ARKRTOS_FLASHAPP_ERR_LAYOUT),
              "image was not linked for the flash app area") == 0, "name(LAYOUT)");
    ck(strcmp(arkrtos_flashapp_error_name(ARKRTOS_FLASHAPP_ERR_PROLOGUE),
              "entry does not start with an arkc prologue") == 0, "name(PROLOGUE)");
    ck(strcmp(arkrtos_flashapp_error_name(ARKRTOS_FLASHAPP_ERR_META),
              "no image in the flash app area") == 0, "name(META)");
    ck(strcmp(arkrtos_flashapp_error_name(ARKRTOS_FLASHAPP_ERR_VERIFY),
              "read-back mismatch: flash content differs from the file") == 0, "name(VERIFY)");
    ck(strcmp(arkrtos_flashapp_error_name(ARKRTOS_FLASHAPP_ERR_FLASH),
              "flash erase/program failed") == 0, "name(FLASH)");
    ck(arkrtos_flashapp_error_name(ARKRTOS_LOADER_ERR_TASK) == 0,
       "name(TASK) is not ours: the caller prints its own text");
    ck(arkrtos_flashapp_error_name(0) == 0, "name(0) is not ours");
}

int main(void)
{
    /* 两条绝对地址都要真映射：模块按板级地址读写 Flash 应用区与数据窗口。
     * 映射失败直接判 FAIL，绝不在空指针上跑一套看起来通过的断言。 */
    if(map_at((unsigned long)ARKRTOS_FLASHAPP_BASE,
              (unsigned long)ARKRTOS_FLASHAPP_SIZE) != 0)
    {
        printf("FLASHAPP-HOST FAIL: cannot map %u bytes at 0x%08X\n",
               (unsigned)ARKRTOS_FLASHAPP_SIZE, (unsigned)ARKRTOS_FLASHAPP_BASE);
        return 1;
    }
    if(map_at((unsigned long)SLOT_RAM_BASE, (unsigned long)SLOT_RAM_TOTAL) != 0)
    {
        printf("FLASHAPP-HOST FAIL: cannot map %u bytes at 0x%08X\n",
               (unsigned)SLOT_RAM_TOTAL, (unsigned)SLOT_RAM_BASE);
        return 1;
    }

    test_geometry();
    test_write_param_gates();
    test_write_success();
    test_write_flash_failures();
    test_erase();
    test_info();
    test_run_failures();
    test_run_success();
    test_error_names();

    printf("checks=%d fails=%d\n", g_checks, g_fails);
    if(g_fails != 0)
    {
        printf("FLASHAPP-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("FLASHAPP-HOST OK  %d checks\n", g_checks);
    return 0;
}
