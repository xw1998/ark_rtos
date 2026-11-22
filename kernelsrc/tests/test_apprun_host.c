/**
* @file test_apprun_host.c
* @brief 板上直跑（arkrtos_apprun.c）的宿主用例
* @details 这条路径只有五步，每一步都在拿「窗口是借来的、地址是写死的」做判断：
*
*            1. 判并发 —— 同一时刻只允许一段窗口在跑；
*            2. 借窗   —— 向 SLOT_RAM 池借**指定的**那两块，借回来的基址必须
*                         正好等于窗口常量；首适配给了别处就当场拒绝，绝不
*                         悄悄换地址去跑一份按固定地址链接的代码；
*            3. 读文件 —— 整份读进代码窗口，超出窗口一个字节都不截断；
*            3b. 认向量表 —— +0 是初始 SP（必须等于数据窗口顶端）、+4 是
*                         Reset_Handler（必须落在代码窗口内、首半字是 arkc 的
*                         函数序言）。两者对不上就说明这份镜像不是给这个窗口
*                         链出来的，跑了等于执行任意字节；
*            4. 起任务 —— 非特权、栈取数据窗口末尾那 4 KB、TCB 两个窗口各自
*                         一块并交给 MPU；
*            5. 退出   —— 只认自己的主任务；别人退出原样放行，绝不还别人的窗。
*
*          宿主上必须解决两件事：模块按**绝对地址**读写窗口（ARKRTOS_APPRUN_BASE
*          来自板级分区头），所以 main() 先真映射 [SLOT_RAM_BASE, +SLOT_RAM_TOTAL)；
*          以及窗口常量本身（APPRUN_* 在 f427 的配置里没有定义，那是 h743 的
*          板级地址声明），由编译命令行按板级数值给出，用例再核对它们与映射
*          区间一致——这条断言保证「测的地址就是板上会用的地址形状」。
*
*          只依赖这些外部符号：VFS 四个入口、ptable 的 mini_alloc/free 与
*          set_slot、task_register、task_table、mpu_build_task、调度器锁与
*          activate、port 临界区/DMB。全部由本文件提供桩或真身。
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
#include "arkrtos_apprun.h"
#include "arkrtos_cfg.h"
#include "arkrtos_ptable.h"
#include "arkrtos_task.h"
#include "arkrtos_mpu.h"
#include "arkrtos_spin.h"
#include "arkrtos_vfs.h"
#include "arkrtos_share.h"
#include "arkrtos_loader.h"
#include "arkrtos_mini.h"

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

/* ---------------- VFS 桩 ---------------- */

#define FILE_CAP (2 * 1024 * 1024)

static uint8  g_file[FILE_CAP];
static uint32 g_file_len;            /* 文件真实长度 */
static uint32 g_read_pos;
static uint32 g_read_chunk;          /* 每次 read 最多给多少字节（0 = 全给） */
static int32  g_read_fail_at;        /* 第 n 次 read 返回 -1（从 1 起，0 = 不注错） */
static unsigned long g_read_calls;
static uint8  g_vfs_ready;
static int32  g_open_rc;
static unsigned long g_open_calls;
static char   g_open_path[64];
static unsigned long g_close_calls;

uint8 arkrtos_vfs_ready(void)
{
    return g_vfs_ready;
}

int32 arkrtos_vfs_open_read(const char *path)
{
    g_open_calls++;
    (void)snprintf(g_open_path, sizeof(g_open_path), "%s", path);
    if(g_open_rc < 0)
    {
        return g_open_rc;
    }
    g_read_pos = 0u;
    return g_open_rc;
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
    if(g_read_pos >= g_file_len)
    {
        return 0;
    }
    avail = g_file_len - g_read_pos;
    n = (avail < len) ? avail : len;
    if((g_read_chunk != 0u) && (n > g_read_chunk))
    {
        n = g_read_chunk;
    }
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

/* ---------------- 分区表桩 ---------------- */

static int32  g_alloc_rc;
static uint32 g_alloc_calls;
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

#define WIN_CODE_BASE ((uint32)ARKRTOS_APPRUN_BASE)
#define WIN_CODE_SIZE ((uint32)ARKRTOS_APPRUN_CODE_SIZE)
#define WIN_RAM_BASE  ((uint32)ARKRTOS_APPRUN_RAM_BASE)
#define WIN_RAM_SIZE  ((uint32)ARKRTOS_APPRUN_RAM_SIZE)
#define WIN_TOP       (WIN_RAM_BASE + WIN_RAM_SIZE)

static void reset_all(void)
{
    /* 模块是单实例状态（g_apprun_task），没有公开的重置接口。上一段用例
     * 跑起来的程序必须先按真实路径退出，否则本段一进 run 就拿到 BUSY。 */
    {
        uint32 running = arkrtos_apprun_task_id();

        if(running != 0u)
        {
            (void)arkrtos_apprun_on_task_exit(running);
        }
    }

    g_file_len   = 0u;
    g_read_pos   = 0u;
    g_read_chunk = 0u;
    g_read_fail_at = 0;
    g_read_calls = 0ul;
    g_vfs_ready  = 1u;
    g_open_rc    = 3;
    g_open_calls = 0ul;
    g_open_path[0] = '\0';
    g_close_calls = 0ul;
    memset(g_file, 0, sizeof(g_file));

    g_alloc_rc = 0;
    g_alloc_calls = 0u;
    g_alloc_code_bytes = 0u;
    g_alloc_ram_bytes  = 0u;
    g_alloc_pt     = 7u;
    g_alloc_c_base = WIN_CODE_BASE;
    g_alloc_c_size = WIN_CODE_SIZE;
    g_alloc_r_base = WIN_RAM_BASE;
    g_alloc_r_size = WIN_RAM_SIZE;

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
    g_crit_enters = 0ul;
    g_crit_exits  = 0ul;
    g_dmb_calls   = 0ul;

    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    memset((void *)(uintptr_t)WIN_CODE_BASE, 0,
           (size_t)(WIN_CODE_SIZE + WIN_RAM_SIZE));
}

/* 造一份「按本窗口链出来」的镜像：向量表 + 入口序言。 */
static void build_image(uint32 size, uint32 sp, uint32 entry_off)
{
    volatile uint32 *vt = (volatile uint32 *)(uintptr_t)WIN_CODE_BASE;
    volatile uint16 *insn;

    g_file_len = size;
    vt[0] = sp;
    vt[1] = (WIN_CODE_BASE + entry_off) | 1u;    /* Thumb 位 */
    insn = (volatile uint16 *)(uintptr_t)(WIN_CODE_BASE + entry_off);
    *insn = (uint16)ARKRTOS_APPRUN_PROLOGUE;
    /* 文件内容：向量表那 8 字节 + 入口处的序言也要跟着进文件（读文件会覆盖
     * 窗口，所以先写文件、再由 run 读进来；这里同步写一份进 g_file）。 */
    memcpy(g_file, (const void *)(uintptr_t)WIN_CODE_BASE, size);
}

/* ---------------- 用例 ---------------- */

static void test_geometry(void)
{
    /* 板级常量与映射区间必须是同一形状：窗口整段落在 SLOT_RAM 池里，
     * 两块各自是 2 的幂、都够一个 MPU region。 */
    ck((WIN_CODE_BASE >= (uint32)SLOT_RAM_BASE), "code window starts inside SLOT_RAM");
    ck((uint32)(WIN_CODE_BASE + WIN_CODE_SIZE + WIN_RAM_SIZE) <=
       (uint32)(SLOT_RAM_BASE + SLOT_RAM_TOTAL), "both windows fit the pool");
    ck(((WIN_CODE_SIZE & (WIN_CODE_SIZE - 1u)) == 0u), "code window is a power of two");
    ck(((WIN_RAM_SIZE & (WIN_RAM_SIZE - 1u)) == 0u), "ram window is a power of two");
    ck((WIN_RAM_BASE == (WIN_CODE_BASE + WIN_CODE_SIZE)), "windows are adjacent");
    ck(((uint32)ARKRTOS_APPRUN_STACK_SIZE <= WIN_RAM_SIZE), "stack fits the data window");
    ck(((uint32)ARKRTOS_APPRUN_MAX_IMAGE <= WIN_CODE_SIZE), "image cap fits the code window");
}

static void test_params_and_state(void)
{
    /* 空路径：一个外部调用都不该发生。 */
    reset_all();
    ck_u((unsigned long)arkrtos_apprun_run_ex(0, 0), (unsigned long)ARKRTOS_LOADER_ERR_PARAM,
         "null path refused");
    ck_u(g_alloc_calls, 0ul, "null path does not borrow the window");
    ck_u(g_open_calls, 0ul, "null path does not touch the file system");

    /* VFS 没起来：报 STATE，窗还没借，闸门必须放回去（否则一次失败就把
     * 直跑永久锁死）。 */
    reset_all();
    g_vfs_ready = 0u;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_LOADER_ERR_STATE, "no VFS -> STATE");
    ck_u(g_alloc_calls, 0ul, "no VFS does not borrow the window");
    ck_u(arkrtos_apprun_task_id(), 0ul, "no VFS leaves the gate free");

    /* 失败之后还能再跑：闸门确实被放回去了。 */
    reset_all();
    build_image(64u, WIN_TOP, 0x40u);
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"), 0ul, "retry after a failed call works");
    (void)arkrtos_apprun_on_task_exit(arkrtos_apprun_task_id());
}

static void test_window(void)
{
    /* 池里没有这块地：报窗口不可用，别拿 NOSPACE 糊过去。 */
    reset_all();
    g_alloc_rc = -1;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_APPRUN_ERR_WINDOW, "alloc failure -> WINDOW");
    ck_u(g_open_calls, 0ul, "no window means no file read");
    ck_u(g_free_calls, 0ul, "nothing to give back when the borrow failed");
    ck_u(arkrtos_apprun_task_id(), 0ul, "gate released after the borrow failed");

    /* 首适配给了别的地方：必须归还并拒绝，绝不挪地址去跑。 */
    reset_all();
    g_alloc_c_base = WIN_CODE_BASE + 0x1000u;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_APPRUN_ERR_WINDOW, "misplaced block -> WINDOW");
    ck_u(g_free_calls, 1ul, "misplaced block is given back");
    ck_u((unsigned long)g_free_index, 7ul, "the returned credential is the one we borrowed");
    ck_u(g_open_calls, 0ul, "no read on a misplaced block");

    reset_all();
    g_alloc_r_size = WIN_RAM_SIZE / 2u;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_APPRUN_ERR_WINDOW, "short data block -> WINDOW");
    ck_u(g_free_calls, 1ul, "short data block is given back");

    /* 借窗的入参就是窗口大小，不是「够大就用」。 */
    reset_all();
    build_image(64u, WIN_TOP, 0x40u);
    (void)arkrtos_apprun_run("/tmp/a.bin");
    ck_u((unsigned long)g_alloc_code_bytes, (unsigned long)WIN_CODE_SIZE,
         "borrow asks for the code window size");
    ck_u((unsigned long)g_alloc_ram_bytes, (unsigned long)WIN_RAM_SIZE,
         "borrow asks for the data window size");
    (void)arkrtos_apprun_on_task_exit(arkrtos_apprun_task_id());
}

static void test_read_file(void)
{
    /* 打不开：报文件系统错误，窗要还。 */
    reset_all();
    g_open_rc = -1;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_MINI_ERR_FS, "open failure -> FS error");
    ck_u(g_free_calls, 1ul, "open failure gives the window back");
    ck_u(g_read_calls, 0ul, "open failure never reads");

    /* 路径原样传给 VFS（不做前缀改写）。 */
    reset_all();
    g_open_rc = -1;
    (void)arkrtos_apprun_run("/tmp/a.bin");
    ck(strcmp(g_open_path, "/tmp/a.bin") == 0, "path is passed through unchanged");

    /* 读到一半失败。 */
    reset_all();
    build_image(64u, WIN_TOP, 0x40u);
    g_read_fail_at = 1;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_MINI_ERR_FS, "read failure -> FS error");
    ck_u(g_free_calls, 1ul, "read failure gives the window back");
    ck_u(g_close_calls, 1ul, "read failure still closes the file");

    /* 空文件不是可执行镜像。 */
    reset_all();
    g_file_len = 0u;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_LOADER_ERR_SIZE, "empty file -> SIZE");
    ck_u(g_close_calls, 1ul, "empty file is closed");

    /* 刚好读满窗口还不够：必须再问一次，才能把「正好这么大」和「比窗口大」
     * 分开。这里文件比窗口大 1 字节。 */
    reset_all();
    build_image(64u, WIN_TOP, 0x40u);
    g_read_chunk = 1024u;
    g_file_len   = WIN_CODE_SIZE + 1u;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_LOADER_ERR_SIZE, "one byte over the window -> SIZE");
    ck_u(g_free_calls, 1ul, "oversize image gives the window back");

    /* 恰好等于窗口大小：probe 读到 0 就通过大小这一关（后面会因向量表/序言
     * 被拒，那是另一回事，但错误码必须是 LAYOUT 而不是 SIZE）。 */
    reset_all();
    g_read_chunk = 4096u;
    g_file_len   = WIN_CODE_SIZE;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_APPRUN_ERR_LAYOUT,
         "exactly the window size passes the size gate");

    /* 那一次 probe 自己读失败（不是读到更多字节，而是 I/O 错）：报文件系统
     * 错误而不是 SIZE——「读不下去」与「文件太大」是两种完全不同的现场。 */
    reset_all();
    g_file_len     = WIN_CODE_SIZE;
    g_read_fail_at = 2;                 /* 第 1 次读满窗口，第 2 次是 probe */
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_MINI_ERR_FS, "probe read failure -> FS error");
    ck_u(g_read_calls, 2ul, "the probe really was issued");
    ck_u(g_close_calls, 1ul, "probe failure still closes the file");
    ck_u(g_free_calls, 1ul, "probe failure gives the window back");
}

static void test_vectors(void)
{
    /* 太短：连向量表都不够。 */
    reset_all();
    g_file_len = 4u;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_APPRUN_ERR_LAYOUT, "payload shorter than the vector table");

    /* SP 不是数据窗口顶端：这份镜像不是按这个窗口链出来的。 */
    reset_all();
    build_image(64u, WIN_TOP - 4u, 0x40u);
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_APPRUN_ERR_LAYOUT, "SP != window top -> LAYOUT");

    /* 入口落在代码窗口之外（两种方向）。 */
    reset_all();
    build_image(64u, WIN_TOP, 0x40u);
    {
        volatile uint32 *vt = (volatile uint32 *)(uintptr_t)WIN_CODE_BASE;
        vt[1] = (WIN_CODE_BASE - 8u) | 1u;
        memcpy(g_file, (const void *)(uintptr_t)WIN_CODE_BASE, g_file_len);
    }
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_APPRUN_ERR_LAYOUT, "entry below the code window -> LAYOUT");

    reset_all();
    build_image(64u, WIN_TOP, 0x40u);
    {
        volatile uint32 *vt = (volatile uint32 *)(uintptr_t)WIN_CODE_BASE;
        /* 入口就是窗口顶端：去掉 Thumb 位后仍然一个字节都不在窗口里。
         * （贴最后一个半字不算越界——它还装得下一条 16 位指令。） */
        vt[1] = (WIN_CODE_BASE + WIN_CODE_SIZE) | 1u;
        memcpy(g_file, (const void *)(uintptr_t)WIN_CODE_BASE, g_file_len);
    }
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_APPRUN_ERR_LAYOUT, "entry with no room for an instruction -> LAYOUT");

    /* 入口不在窗口里 —— 文件头两个字节是字符串也一样能识别，因为核的是
     * 入口那一条指令，不是文件首字节。 */
    reset_all();
    build_image(64u, WIN_TOP, 0x40u);
    ((volatile uint16 *)(uintptr_t)(WIN_CODE_BASE + 0x40u))[0] = 0x1234u;
    memcpy(g_file, (const void *)(uintptr_t)WIN_CODE_BASE, g_file_len);
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_APPRUN_ERR_PROLOGUE, "bad prologue -> PROLOGUE");
    ck_u(g_task_calls, 0ul, "no task is registered for a rejected image");
}

static void test_success(void)
{
    uint32 task_out = 0u;
    uint32 entry = 0u;
    uint32 code = 0u;
    uint32 ram = 0u;

    reset_all();
    build_image(64u, WIN_TOP, 0x40u);

    ck_u((unsigned long)arkrtos_apprun_run_ex("/tmp/a.bin", &task_out), 0ul,
         "well-formed image starts");
    ck_u((unsigned long)task_out, (unsigned long)g_task_rc,
         "task id is written out before activate");
    ck_u((unsigned long)g_task_calls, 1ul, "exactly one task is registered");
    ck(g_task_entry == (void *)(uintptr_t)((WIN_CODE_BASE + 0x40u) | 1u),
       "entry is the Reset_Handler with its Thumb bit");
    ck_u((unsigned long)g_task_stack,
         (unsigned long)(WIN_RAM_BASE + WIN_RAM_SIZE - (uint32)ARKRTOS_APPRUN_STACK_SIZE),
         "stack bottom is the top of the data window minus the stack");
    ck_u((unsigned long)g_task_stack_size, (unsigned long)ARKRTOS_APPRUN_STACK_SIZE,
         "stack size is the board constant");
    ck_u((unsigned long)g_task_prio, (unsigned long)ARKRTOS_APPRUN_PRIORITY,
         "priority is the board constant");
    ck_u((unsigned long)g_task_period, (unsigned long)ARKRTOS_APPRUN_PERIOD_MS,
         "period is the board constant");

    ck_u((unsigned long)arkrtos_task_table[g_task_rc - 1].rom_start,
         (unsigned long)WIN_CODE_BASE, "TCB rom window is the code window");
    ck_u((unsigned long)arkrtos_task_table[g_task_rc - 1].rom_size,
         (unsigned long)WIN_CODE_SIZE, "TCB rom size is the code window size");
    ck_u((unsigned long)arkrtos_task_table[g_task_rc - 1].ram_start,
         (unsigned long)WIN_RAM_BASE, "TCB ram window is the data window");
    ck_u((unsigned long)arkrtos_task_table[g_task_rc - 1].ram_size,
         (unsigned long)WIN_RAM_SIZE, "TCB ram size is the data window size");
    ck_u((unsigned long)arkrtos_task_table[g_task_rc - 1].is_priv, 0ul,
         "the program runs unprivileged");
    ck_u(g_mpu_calls, 1ul, "MPU windows are built for the new task");
    ck(g_mpu_task == &arkrtos_task_table[g_task_rc - 1], "MPU sees the TCB we filled in");

    ck_u(g_set_slot_calls, 1ul, "the pool record is updated once");
    ck_u((unsigned long)g_set_slot_index, 7ul, "pool record index is the borrowed one");
    ck_u((unsigned long)g_set_slot_state, (unsigned long)ARKRTOS_APP_SLOT_RUNNING,
         "pool record says running");
    ck_u((unsigned long)g_set_slot_entry, (unsigned long)((WIN_CODE_BASE + 0x40u) | 1u),
         "pool record carries the entry");
    ck_u((unsigned long)g_set_slot_task, (unsigned long)g_task_rc,
         "pool record carries the task id");

    ck_u(g_sched_lock_calls, 1ul, "the file read holds the scheduler lock");
    ck_u(g_sched_unlock_calls, 1ul, "the lock is released before the task is started");
    ck_u(g_activate_calls, 1ul, "activate is called once");
    ck_u((unsigned long)g_activate_prio, (unsigned long)ARKRTOS_APPRUN_PRIORITY,
         "activate uses the program priority");
    ck_u((unsigned long)g_close_calls, 1ul, "the file is closed");

    ck_u((unsigned long)arkrtos_apprun_task_id(), (unsigned long)g_task_rc,
         "task_id reports the running program");
    ck_u((unsigned long)arkrtos_apprun_info(&entry, &code, &ram), 0ul,
         "info succeeds while the program runs");
    ck_u((unsigned long)entry, (unsigned long)((WIN_CODE_BASE + 0x40u) | 1u),
         "info reports the entry");
    ck_u((unsigned long)code, (unsigned long)WIN_CODE_BASE, "info reports the code window");
    ck_u((unsigned long)ram, (unsigned long)WIN_RAM_BASE, "info reports the data window");
    ck_u((unsigned long)arkrtos_apprun_info(0, 0, 0), 0ul,
         "info tolerates null out-parameters");

    /* 同一时刻只允许一个：第二次调用必须 BUSY，而且不碰窗口与文件。 */
    reset_all();
    build_image(64u, WIN_TOP, 0x40u);
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"), 0ul, "first program starts");
    {
        unsigned long allocs = g_alloc_calls;
        unsigned long opens = g_open_calls;

        ck_u((unsigned long)arkrtos_apprun_run("/tmp/other.bin"),
             (unsigned long)ARKRTOS_APPRUN_ERR_BUSY, "second program is refused");
        ck_u(g_alloc_calls, allocs, "busy path does not borrow the window");
        ck_u(g_open_calls, opens, "busy path does not read a file");
    }

    /* 退出：窗口还回去，状态清空，info 报 NOSTATE。 */
    {
        uint32 running = arkrtos_apprun_task_id();

        ck_u((unsigned long)arkrtos_apprun_on_task_exit(running), 0ul,
             "the owner exits cleanly");
        ck_u(g_free_calls, 1ul, "the window is returned on exit");
        ck_u((unsigned long)g_free_index, 7ul, "the credential is the one we borrowed");
        ck_u(arkrtos_apprun_task_id(), 0ul, "task_id is clear after exit");
        ck_u((unsigned long)arkrtos_apprun_info(0, 0, 0),
             (unsigned long)ARKRTOS_APPRUN_ERR_NOSTATE, "info says NOSTATE after exit");
        ck_u((unsigned long)arkrtos_apprun_on_task_exit(running), 0ul,
             "a second exit for the same task is a no-op");
        ck_u(g_free_calls, 1ul, "a second exit does not return the window twice");
    }
}

static void test_task_failure(void)
{
    /* 任务表满：窗还在手里，必须还回去；返回的是任务错误而不是窗口错误。 */
    reset_all();
    build_image(64u, WIN_TOP, 0x40u);
    g_task_rc = 0;
    ck_u((unsigned long)arkrtos_apprun_run("/tmp/a.bin"),
         (unsigned long)ARKRTOS_LOADER_ERR_TASK, "task registration failure -> TASK");
    ck_u(g_free_calls, 1ul, "the window is returned when the task cannot be created");
    ck_u(g_mpu_calls, 0ul, "no MPU windows without a task");
    ck_u(g_set_slot_calls, 0ul, "the pool record is not published without a task");
    ck_u(g_activate_calls, 0ul, "activate is not called without a task");
    ck_u(arkrtos_apprun_task_id(), 0ul, "gate released after task failure");
}

static void test_exit_contract(void)
{
    /* 别人的退出不许还窗。 */
    reset_all();
    build_image(64u, WIN_TOP, 0x40u);
    (void)arkrtos_apprun_run("/tmp/a.bin");
    ck_u((unsigned long)arkrtos_apprun_on_task_exit(0u), 0ul, "task 0 exit is ignored");
    ck(arkrtos_apprun_on_task_exit((uint32)g_task_rc + 1u) == 0,
       "another task's exit is ignored");
    ck_u(g_free_calls, 0ul, "foreign exits do not return the window");
    ck_u((unsigned long)arkrtos_apprun_task_id(), (unsigned long)g_task_rc,
         "the program is still running after foreign exits");

    /* 归还失败（账本已经乱了）：退出本身照常返回 0，但必须留下可见记录。
     * 这里只能断言返回值与调用次数——日志在本档被编掉。 */
    g_free_rc = -1;
    ck(arkrtos_apprun_on_task_exit((uint32)g_task_rc) == 0,
       "a failed return still reports a clean exit");
    ck_u(g_free_calls, 1ul, "the return was attempted");
    ck_u(arkrtos_apprun_task_id(), 0ul, "the state is cleared even when the return fails");
}

static void test_error_names(void)
{
    ck(strcmp(arkrtos_apprun_error_name(ARKRTOS_APPRUN_ERR_BUSY),
              "a program is already running") == 0, "name(BUSY)");
    ck(strcmp(arkrtos_apprun_error_name(ARKRTOS_APPRUN_ERR_WINDOW),
              "the run window could not be taken") == 0, "name(WINDOW)");
    ck(strcmp(arkrtos_apprun_error_name(ARKRTOS_APPRUN_ERR_LAYOUT),
              "image was not linked for this window") == 0, "name(LAYOUT)");
    ck(strcmp(arkrtos_apprun_error_name(ARKRTOS_APPRUN_ERR_PROLOGUE),
              "not an arkc image (bad prologue)") == 0, "name(PROLOGUE)");
    ck(arkrtos_apprun_error_name(ARKRTOS_APPRUN_ERR_NOSTATE) == 0,
       "name(NOSTATE) is not ours: the caller prints its own text");
    ck(arkrtos_apprun_error_name(0) == 0, "name(0) is not ours");
}

int main(void)
{
    /* 先真映射窗口所在的池：模块按绝对地址读写它。映射失败直接判 FAIL，
     * 绝不在空指针上跑一套看起来通过的断言。 */
    if(map_at((unsigned long)SLOT_RAM_BASE, (unsigned long)SLOT_RAM_TOTAL) != 0)
    {
        printf("APPRUN-HOST FAIL: cannot map %u bytes at 0x%08X\n",
               (unsigned)SLOT_RAM_TOTAL, (unsigned)SLOT_RAM_BASE);
        return 1;
    }

    test_geometry();
    test_params_and_state();
    test_window();
    test_read_file();
    test_vectors();
    test_success();
    test_task_failure();
    test_exit_contract();
    test_error_names();

    printf("checks=%d fails=%d\n", g_checks, g_fails);
    if(g_fails != 0)
    {
        printf("APPRUN-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("APPRUN-HOST OK  %d checks\n", g_checks);
    return 0;
}
