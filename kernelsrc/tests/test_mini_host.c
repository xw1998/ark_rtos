/**
* @file test_mini_host.c
* @brief 小程序（arkrtos_mini.c）的宿主用例
* @details 被测模块的契约只有一句话：「块是借来的，跑完必须还」。所有用例都
*          围绕这句话展开，分四组：
*
*            A. 装载六步的每一步失败面 —— 头读不到 / 表读不到 / 头不合法 /
*               表不合法 / 超出体积上限 / 借不到块 / 负载读失败 / CRC 不符 /
*               任务注册失败。判据不只是「返回了错误码」，还要看**没有留下
*               借出的块**（mini_free 的调用与下标）与**实例项被清回 FREE**
*               （紧接着的一次 run 能成功）。
*            B. 运行期记账 —— 并发上限（编译期 ARKRTOS_MINI_MAX 与运行期
*               set_max_count 双层）、运行期体积上限（只能收紧）、info /
*               running / task_is_main / stop_at / stop、任务退出与故障归属。
*            C. 无缝升级（ARKRTOS_USE_TASK_SWAP=1）—— 三条互斥、布局核对、
*               内核挂载成功/被拒两条落地路径，以及「停掉正在换的那个实例」
*               要把在途的那份新块一起撤掉。
*            D. 开机自启清单 —— 解析（注释/空行/CRLF/去空白）、超长条目跳过、
*               空改动不落盘、清单空了删文件、NOSPACE、自启只做一次。
*
*          **这个用例不证明什么**（写在这里，免得读数被当成更多东西）：
*            1. 不证明镜像容器的合法性判定：loader_check_header /
*               reloc_apply / crc_header 在本用例里是桩，由 loader 自己的宿主
*               用例负责。这里验证的是「mini 把哪些字节、按什么顺序喂给了谁」，
*               不是「loader 判得对不对」。
*            2. 不证明 RAM 池的分配策略：ptable_mini_alloc/free 也是桩。真实
*               首适配、粒度、碎片行为在 ptable 的用例里。
*            3. 不证明 MPU 窗口真的被装上：板级档位开了 ARKRTOS_USE_PAGING，
*               所以 arkrtos_mpu_build_task / arkrtos_mpu_reset 是真实的 extern
*               引用（不是空宏），用例给它们打桩、只记「装了几次、装给谁」。
*               窗口的生效与否只有板上能看到；这里能核对的是 TCB 的 rom_ /
*               ram_ 四个字段与 is_priv —— 那是 MPU 唯一的输入。
*            4. 不证明真机上的时序：调度器锁、让出点、activate 都是桩，只统计
*               调用次数与顺序，量不到真实停顿。
*            5. 不证明崩溃日记跨复位存活：crash_* 是桩，只验证归属判定与
*               「到上限后停掉并禁用」这条链路。
*
*          宿主的两个硬约束与 apprun 用例同源：
*            - 模块把块基址当指针用（(uint8 *)(void *)code_base），所以桩返回
*              的基址必须是真能写的内存：这里用两块 4KB 对齐的静态缓冲；
*            - 32 位 MinGW 上 uint32 与指针同宽，转换不丢信息（本门禁只跑 32
*              位宿主，见 tools/coverage_gate.py 的注释）。
*
* @author xw
* @date 2026.11.22
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_config.h"
#include "arkrtos_partition.h"
#include "arkrtos_mini.h"
#include "arkrtos_app_image.h"
#include "arkrtos_loader.h"
#include "arkrtos_ptable.h"
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"
#include "arkrtos_mpu.h"
#include "arkrtos_crash.h"
#include "arkrtos_spin.h"
#include "arkrtos_fs.h"
#include "arkrtos_vfs.h"
#include "arkrtos_log.h"

/* ============================================================================
 * 关档：模块编成空目标，用例只证明「配置读数就是没有」
 * ==========================================================================*/
#if (ARKRTOS_USE_MINIAPP == 0)

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

int main(void)
{
    /* 关档的三条静态形状：开关为 0、路径上限仍是 ABI 常量、结果结构体尺寸不变。
     * 不引用任何 arkrtos_mini_* 函数——关档时它们没有定义，能链接上本身就是
     * 「这个模块一个符号都没编进来」的证据（coverage_gate 还会用 nm 再核一遍）。 */
    ck((ARKRTOS_USE_MINIAPP == 0), "USE_MINIAPP stays 0");
    ck((ARKRTOS_MINI_PATH_MAX == 64u), "PATH_MAX is the ABI constant");
    ck((sizeof(arkrtos_mini_info_t) == (8u * sizeof(uint32))),
       "info struct keeps its 8-word shape");

    printf("MINI HOST OFF OK %d checks, %d failed\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}

#else  /* ARKRTOS_USE_MINIAPP == 1 */

/* ============================================================================
 * 断言
 * ==========================================================================*/

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

static void ck_i32(int32 got, int32 want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL: %s (got %ld want %ld)\n", what, (long)got, (long)want);
    }
}

static void ck_u32(uint32 got, uint32 want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL: %s (got %lu want %lu)\n", what,
               (unsigned long)got, (unsigned long)want);
    }
}

/* ============================================================================
 * 端口层：临界区 / 屏障 / 核号 / 自旋提示
 * ==========================================================================*/

static unsigned long g_crit_enters;
static unsigned long g_crit_exits;

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
}

uint32 arkrtos_port_cpu_id(void)
{
    return 0u;
}

void arkrtos_port_spin_hint(void)
{
}

/* ============================================================================
 * 调度器桩：锁 / 让出 / activate
 * ==========================================================================*/

static unsigned long g_sched_lock_calls;
static unsigned long g_sched_unlock_calls;
static unsigned long g_activate_calls;
static uint32        g_activate_prio;

void arkrtos_sched_lock_internal(void)
{
    g_sched_lock_calls++;
}

uint32 arkrtos_sched_unlock_internal(void)
{
    g_sched_unlock_calls++;
    return 0u;
}

void arkrtos_sched_activate_higher(uint8 prio)
{
    g_activate_calls++;
    g_activate_prio = (uint32)prio;
}

/* ============================================================================
 * 镜像文件与 VFS 桩
 *
 * 镜像容器：256 字节头 + 重定位表（一般 0 条；量「表读失败」那条用例给 1 条，
 * 因为 0 字节的读不会碰 VFS）+ 负载。负载在
 * payload_offset 起的连续字节，mini 会把它按 ARKRTOS_MINI_CHUNK 一段段读进
 * 代码块，所以这里只需要一份线性缓冲 + 一个「按偏移读」的桩。
 * ==========================================================================*/

#define IMG_CAP        (64u * 1024u)   /* 上限用例要造 32KB 负载 */
#define IMG_PAYLOAD    (ARKRTOS_APP_HEADER_SIZE)

static uint8  g_img[IMG_CAP];
static uint32 g_img_len;

static uint32 g_vfs_ready   = 1u;
static int32  g_vfs_rc;                 /* 非 0 = 每次读都失败 */
static int32  g_vfs_fail_at;            /* 第 n 次读失败（从 1 起，0 = 不注错） */
static uint32 g_vfs_short;              /* 1 = 每次少给 1 字节（短读） */
static unsigned long g_vfs_calls;
static char   g_vfs_path[ARKRTOS_MINI_PATH_MAX];

/* 头部按字节写：不依赖宿主的结构体对齐，也顺带让偏移写错会立刻表现为失败。 */
static void img_w32(uint32 off, uint32 val)
{
    g_img[off + 0u] = (uint8)(val & 0xFFu);
    g_img[off + 1u] = (uint8)((val >> 8) & 0xFFu);
    g_img[off + 2u] = (uint8)((val >> 16) & 0xFFu);
    g_img[off + 3u] = (uint8)((val >> 24) & 0xFFu);
}

static uint32 img_r32(uint32 off)
{
    return ((uint32)g_img[off + 0u])
         | ((uint32)g_img[off + 1u] << 8)
         | ((uint32)g_img[off + 2u] << 16)
         | ((uint32)g_img[off + 3u] << 24);
}

/* 与被测模块喂给 arkrtos_crc32 的口径一致：crc_header 的返回值（本用例是桩，
 * 默认 0）作为种子，然后按顺序累加整段负载。 */
static uint32 g_crc_seed;

static uint32 crc_expected(void)
{
    return arkrtos_crc32(&g_img[IMG_PAYLOAD],
                         g_img_len - IMG_PAYLOAD, g_crc_seed);
}

/* 造一份自洽的镜像。payload 字节数 / 镜像声明的 RAM 需求都由调用方指定，
 * 因为「代码块与 RAM 块各取 2 的幂」这条正是要量的东西。 */
static void img_build(uint32 payload, uint32 ram_size)
{
    uint32 i;

    memset(g_img, 0, sizeof(g_img));

    img_w32(0u, ARKRTOS_APP_MAGIC);
    img_w32(4u, ARKRTOS_APP_TYPE_MINIAPP);
    img_w32(8u, 0x00010000u);              /* hw_compat_id：校验是桩，不参与判定 */
    img_w32(12u, 1u);                      /* version */
    img_w32(16u, payload);                 /* image_size */
    img_w32(20u, 8u);                      /* entry_offset */
    img_w32(24u, 0x00100000u);             /* nominal_base */
    img_w32(28u, 0u);                      /* crc32：最后再填 */
    img_w32(108u, ram_size);               /* ram_size */
    img_w32(112u, ARKRTOS_APP_RELOC_OFFSET);
    img_w32(116u, 0u);                     /* reloc_count */
    img_w32(120u, ARKRTOS_APP_RELOC_KIND_CUR);
    img_w32(124u, IMG_PAYLOAD);            /* payload_offset */
    img_w32(128u, 0x20000000u);            /* nominal_ram_base */

    for(i = 0u; i < payload; i++)
    {
        g_img[IMG_PAYLOAD + i] = (uint8)(0x5Au ^ (i * 7u));
    }

    g_img_len = IMG_PAYLOAD + payload;

    img_w32(28u, crc_expected());
}

int32 arkrtos_vfs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off,
                        uint32 *got)
{
    uint32 n;

    g_vfs_calls++;
    if(path != 0)
    {
        (void)snprintf(g_vfs_path, sizeof(g_vfs_path), "%s", path);
    }

    *got = 0u;

    if((g_vfs_fail_at != 0) && (g_vfs_calls == (unsigned long)g_vfs_fail_at))
    {
        return -5;                          /* 底层读错误（值只用于回传） */
    }
    if(g_vfs_rc != 0)
    {
        return g_vfs_rc;
    }
    if(off >= g_img_len)
    {
        return 0;                           /* 读到文件尾：got = 0，由调用方判短读 */
    }

    n = g_img_len - off;
    if(n > len)
    {
        n = len;
    }
    if((g_vfs_short != 0u) && (n != 0u))
    {
        n--;
    }

    memcpy(buf, &g_img[off], n);
    *got = n;

    return 0;
}

uint8 arkrtos_vfs_ready(void)
{
    return (uint8)g_vfs_ready;
}

/* ============================================================================
 * 分区表桩：成对借还
 *
 * 返回的基址必须是真能写的内存，而且每次借要落在不同的块上——两次 run 同时
 * 在跑时，第二份负载不能覆盖第一份的代码（覆盖了就是「都跑但只有一个是对的」，
 * 那种错在这里必须表现为可见的地址不同）。
 * ==========================================================================*/

#define POOL_N 4u

static uint8 g_code_pool[POOL_N][64u * 1024u] __attribute__((aligned(4096)));
static uint8 g_ram_pool[POOL_N][64u * 1024u] __attribute__((aligned(4096)));

static int32  g_alloc_rc;
static uint32 g_alloc_seq;                  /* 第几次借（1 起，只用于记账） */
static uint8  g_pool_used[POOL_N];          /* 首次适配：真实 ptable 会交回最低的空块 */
static unsigned long g_alloc_calls;
static uint32 g_alloc_code_bytes;
static uint32 g_alloc_ram_bytes;
static unsigned long g_free_calls;
static uint32 g_free_last;

static uint32 pow2_ceil(uint32 v)
{
    uint32 r = 1u;

    while(r < v)
    {
        r <<= 1u;
    }
    return r;
}

int32 arkrtos_ptable_mini_alloc(uint32 code_bytes, uint32 ram_bytes,
                               uint32 *p_index,
                               uint32 *p_code_base, uint32 *p_code_size,
                               uint32 *p_ram_base, uint32 *p_ram_size)
{
    uint32 i;

    g_alloc_calls++;
    g_alloc_code_bytes = code_bytes;
    g_alloc_ram_bytes  = ram_bytes;

    if(g_alloc_rc < 0)
    {
        return g_alloc_rc;
    }

    g_alloc_seq++;

    /* 首次适配：归还的块下一次会被重新借出去，而不是轮转到一个新块。
     * 这不只是「更像真实池」——否则「布局变了被拒 → 再挂一次」那条路上，
     * 新块落到池尾，用例里的 code_base_of(n) 就与真实行为不符。 */
    for(i = 0u; i < POOL_N; i++)
    {
        if(g_pool_used[i] == 0u)
        {
            break;
        }
    }
    if(i == POOL_N)
    {
        return -1;
    }
    g_pool_used[i] = 1u;

    *p_index     = i + 1u;
    *p_code_base = (uint32)(uintptr_t)&g_code_pool[i][0];
    *p_code_size = pow2_ceil(code_bytes);
    *p_ram_base  = (uint32)(uintptr_t)&g_ram_pool[i][0];
    *p_ram_size  = pow2_ceil(ram_bytes);

    /* 真实 ptable 成功时返回 0（非 0 = 失败），下标走 *p_index。 */
    return 0;
}

int32 arkrtos_ptable_mini_free(uint32 index)
{
    g_free_calls++;
    g_free_last = index;

    if((index >= 1u) && (index <= POOL_N))
    {
        g_pool_used[index - 1u] = 0u;
    }
    return 0;
}

/* ============================================================================
 * Loader 桩：只记录「mini 喂了什么」
 * ==========================================================================*/

static int32  g_chk_rc_first;               /* 第一次 check_header 的返回 */
static int32  g_chk_rc_second;              /* 第二次（带表体）的返回 */
static unsigned long g_chk_calls;
static uint32 g_chk_mask;
static uint32 g_chk_rel_present;

static uint32 g_reloc_ret_delta;            /* 0 = 返回 buf_off+len（正常）；
                                               1 = 少 4 字节（段尾切开）；
                                               2 = 返回 buf_off（不前进） */
static unsigned long g_reloc_calls;
static uint32 g_reloc_buf_off;
static uint32 g_reloc_len;
static uint32 g_reloc_off1;                 /* 第 1 / 2 次补丁的偏移，用来量回退重读 */
static uint32 g_reloc_len1;
static uint32 g_reloc_off2;

static unsigned long g_crc_header_calls;
static unsigned long g_halt_calls;
static uint32 g_halt_task;

int32 arkrtos_loader_check_header(const arkrtos_app_header_t *p_hdr,
                                  const uint32 *p_rel, uint32 types_mask)
{
    g_chk_calls++;
    g_chk_mask = types_mask;
    g_chk_rel_present = (p_rel != 0) ? 1u : 0u;

    if(p_hdr == 0)
    {
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    return (g_chk_calls == 1u) ? g_chk_rc_first : g_chk_rc_second;
}

uint32 arkrtos_loader_reloc_apply(uint8 *buf, uint32 buf_off, uint32 len,
                                  const arkrtos_app_header_t *p_hdr,
                                  const uint32 *p_rel, uint32 *p_idx,
                                  uint32 delta_rom, uint32 delta_ram)
{
    (void)buf;
    (void)p_hdr;
    (void)p_rel;
    (void)p_idx;
    (void)delta_rom;
    (void)delta_ram;

    g_reloc_calls++;
    g_reloc_buf_off = buf_off;
    g_reloc_len     = len;
    if(g_reloc_calls == 1u)
    {
        g_reloc_off1 = buf_off;
        g_reloc_len1 = len;
    }
    else if(g_reloc_calls == 2u)
    {
        g_reloc_off2 = buf_off;
    }

    /* delta=1 只切第一段：第二次读到的是被切开那条表项的尾巴，此时补丁
     * 已经完整，必须报「这一块处理完」（返回 buf_off+len）。每次都少 4 字节
     * 会让循环停在原地（next <= cur）——那测的就不是回退重读，而是死循环检测。 */
    if((g_reloc_ret_delta == 1u) && (g_reloc_calls == 1u))
    {
        return buf_off + ((len > 4u) ? (len - 4u) : 0u);
    }
    if(g_reloc_ret_delta == 2u)
    {
        return buf_off;
    }

    return buf_off + len;
}

uint32 arkrtos_loader_crc_header(const arkrtos_app_header_t *p_hdr)
{
    (void)p_hdr;
    g_crc_header_calls++;
    return g_crc_seed;
}

void arkrtos_loader_halt_image(uint32 task_id)
{
    g_halt_calls++;
    g_halt_task = task_id;
}

/* 本用例只在自己内部用 crc32 造期望值，判定对象不是算法本身。用一份逐位
 * 实现的 IEEE 802.3 反射 CRC32，与仓库里那份算法同口径，但实现独立（不链
 * loader 的 .c，避免把安装路径的依赖拖进来）。 */
uint32 arkrtos_crc32(const void *data, uint32 len, uint32 crc)
{
    const uint8 *p = (const uint8 *)data;
    uint32 i;
    uint32 k;

    for(i = 0u; i < len; i++)
    {
        crc ^= (uint32)p[i];
        for(k = 0u; k < 8u; k++)
        {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

/* ============================================================================
 * 任务表 / 注册 / MPU
 * ==========================================================================*/

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32 arkrtos_task_count;

static int32  g_task_rc;                    /* 返回给 register 调用方（0 = 下一个号） */
static uint32 g_task_seq;
static unsigned long g_task_calls;
static void (*g_task_entry)(void);
static uint32 *g_task_stack;
static uint32 g_task_stack_size;
static uint32 g_task_prio;
static uint32 g_task_period;

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom,
                            uint32 stack_size, uint8 priority, uint32 period_ms)
{
    g_task_calls++;
    g_task_entry      = entry;
    g_task_stack      = stack_bottom;
    g_task_stack_size = stack_size;
    g_task_prio       = (uint32)priority;
    g_task_period     = period_ms;

    if(g_task_rc != 0)
    {
        return g_task_rc;
    }

    g_task_seq++;
    return (int32)g_task_seq;
}

/* ============================================================================
 * 崩溃记账桩：路径 → 哈希只要求「同一条路径给出同一个值、不同路径不同」
 * ==========================================================================*/

static uint32 g_crash_disabled;             /* 1 = 已到上限 */
static uint32 g_crash_count_val;
static unsigned long g_hash_calls;
static char   g_hash_path[ARKRTOS_MINI_PATH_MAX];
static uint32 g_hash_last;

static unsigned long g_fault_calls;
static uint32 g_fault_hash;
static uint32 g_fault_reason;
static unsigned long g_forget_calls;
static uint32 g_forget_hash;

uint32 arkrtos_crash_hash_path(const char *path)
{
    uint32 h = 2166136261u;
    uint32 i;

    g_hash_calls++;
    if(path != 0)
    {
        (void)snprintf(g_hash_path, sizeof(g_hash_path), "%s", path);
        for(i = 0u; (path[i] != '\0') && (i < 4096u); i++)
        {
            h ^= (uint32)(uint8)path[i];
            h *= 16777619u;
        }
    }
    g_hash_last = h;
    return h;
}

uint32 arkrtos_crash_mini_disabled(uint32 hash)
{
    (void)hash;
    return (g_crash_disabled != 0u) ? ARKRTOS_CRASH_REASON_FAULT : 0u;
}

uint32 arkrtos_crash_mini_fault(uint32 hash, uint32 reason)
{
    g_fault_calls++;
    g_fault_hash   = hash;
    g_fault_reason = reason;
    return g_crash_disabled;
}

uint32 arkrtos_crash_mini_count(uint32 hash)
{
    (void)hash;
    return g_crash_count_val;
}

void arkrtos_crash_mini_forget(uint32 hash)
{
    g_forget_calls++;
    g_forget_hash = hash;
}

const char *arkrtos_crash_reason_name(uint32 reason)
{
    (void)reason;
    return "fault";
}

/* ============================================================================
 * 任务热切换桩（ARKRTOS_USE_TASK_SWAP=1 时才被引用）
 * ==========================================================================*/

#if (ARKRTOS_USE_TASK_SWAP == 1)

static int32  g_pending_rc;
static int32  g_arm_rc;
static int32  g_take_rc;
static unsigned long g_arm_calls;
static int32  g_arm_task;
static unsigned long g_take_calls;
static unsigned long g_cancel_calls;
static int32  g_cancel_task;

int32 arkrtos_task_swap_pending(void)
{
    return g_pending_rc;
}

int32 arkrtos_task_swap_arm(int32 task_id, unsigned long rom_start, uint32 rom_size,
                            unsigned long ram_start, uint32 ram_size,
                            unsigned long entry)
{
    (void)rom_start;
    (void)rom_size;
    (void)ram_start;
    (void)ram_size;
    (void)entry;

    g_arm_calls++;
    g_arm_task = task_id;
    return g_arm_rc;
}

int32 arkrtos_task_swap_take(void *p_exc_ctx, int32 task_id)
{
    (void)p_exc_ctx;
    (void)task_id;
    g_take_calls++;
    return g_take_rc;
}

int32 arkrtos_task_swap_cancel(int32 task_id)
{
    g_cancel_calls++;
    g_cancel_task = task_id;
    return 0;
}

#endif /* ARKRTOS_USE_TASK_SWAP */

/* ============================================================================
 * 文件系统桩（自启清单用）
 * ==========================================================================*/

static uint8  g_fs_file[2048];
static uint32 g_fs_file_len;
static int32  g_fs_read_rc;
static unsigned long g_fs_read_calls;
static unsigned long g_fs_write_calls;
static uint8  g_fs_written[2048];
static uint32 g_fs_written_len;
static int32  g_fs_write_rc;
static unsigned long g_fs_remove_calls;
static char   g_fs_remove_path[ARKRTOS_MINI_PATH_MAX];
static char   g_fs_last_path[ARKRTOS_MINI_PATH_MAX];

int32 arkrtos_fs_read_file(const char *path, uint8 *buf, uint32 max_len,
                           uint32 *p_len)
{
    uint32 n = g_fs_file_len;

    g_fs_read_calls++;
    (void)snprintf(g_fs_last_path, sizeof(g_fs_last_path), "%s",
                   (path != 0) ? path : "");

    if(g_fs_read_rc != 0)
    {
        return g_fs_read_rc;
    }
    if(n > max_len)
    {
        n = max_len;
    }

    memcpy(buf, g_fs_file, n);
    *p_len = n;

    return 0;
}

int32 arkrtos_fs_write_file(const char *path, const uint8 *buf, uint32 len)
{
    g_fs_write_calls++;
    (void)snprintf(g_fs_last_path, sizeof(g_fs_last_path), "%s",
                   (path != 0) ? path : "");

    if(g_fs_write_rc != 0)
    {
        return g_fs_write_rc;
    }

    if(len > sizeof(g_fs_written))
    {
        len = sizeof(g_fs_written);
    }
    memcpy(g_fs_written, buf, len);
    g_fs_written_len = len;

    return 0;
}

int32 arkrtos_fs_remove(const char *path)
{
    g_fs_remove_calls++;
    (void)snprintf(g_fs_remove_path, sizeof(g_fs_remove_path), "%s",
                   (path != 0) ? path : "");
    return 0;
}

/* ============================================================================
 * 日志 / 原子 / MPU 桩
 *
 * 板级档位是 QEMU LoongArch64，ARKRTOS_USE_PAGING=1 ⇒ arkrtos_mpu.h 不把
 * mpu_build_task / mpu_reset 折成空宏，而是真的 extern 引用；宿主链接没有
 * 分页后端，所以这里给桩并记账。用例只核对「装载完成后有没有按任务装一次
 * 窗口、停止后有没有复位」，窗口本身的效果只有板上能看到。
 * ==========================================================================*/

static unsigned long g_mpu_build_calls;
static unsigned long g_mpu_reset_calls;
static arkrtos_task_t *g_mpu_last_task;

uint32 arkrtos_log_get_level(void) { return 0u; }

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level; (void)tag; (void)line; (void)msg;
}

/* 返回 1 = 交换成功（本仓库 port 层的约定：LA64 的 LL/SC 在成功时
   返回 1，与「返回旧值」那种写法不同；写成旧值会让自旋锁永远抢不到）。 */
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,
                               uint32 new_value)
{
    if(*p_addr != expect)
    {
        return 0u;
    }

    *p_addr = new_value;
    return 1u;
}

void arkrtos_mpu_build_task(arkrtos_task_t *p_task)
{
    g_mpu_build_calls++;
    g_mpu_last_task = p_task;
}

void arkrtos_mpu_reset(void)
{
    g_mpu_reset_calls++;
}

/* ============================================================================
 * 夹具
 * ==========================================================================*/

static void reset_stubs(void)
{
    g_crit_enters = 0u;
    g_crit_exits  = 0u;
    g_sched_lock_calls = 0u;
    g_sched_unlock_calls = 0u;
    g_activate_calls = 0u;
    g_activate_prio  = 0u;
    g_mpu_build_calls = 0u;
    g_mpu_reset_calls = 0u;
    g_mpu_last_task   = 0;

    g_vfs_ready  = 1u;
    g_vfs_rc     = 0;
    g_vfs_fail_at = 0;
    g_vfs_short  = 0u;
    g_vfs_calls  = 0u;
    g_vfs_path[0] = '\0';

    g_crc_seed = 0u;

    g_alloc_rc   = 0;
    g_alloc_seq  = 0u;
    g_alloc_calls = 0u;
    g_alloc_code_bytes = 0u;
    g_alloc_ram_bytes  = 0u;
    g_free_calls = 0u;
    g_free_last  = 0u;
    (void)memset(g_pool_used, 0, sizeof(g_pool_used));

    g_chk_rc_first  = 0;
    g_chk_rc_second = 0;
    g_chk_calls = 0u;
    g_chk_mask  = 0u;
    g_chk_rel_present = 0u;

    g_reloc_ret_delta = 0u;
    g_reloc_calls = 0u;
    g_reloc_buf_off = 0u;
    g_reloc_len = 0u;
    g_reloc_off1 = 0u;
    g_reloc_len1 = 0u;
    g_reloc_off2 = 0u;

    g_crc_header_calls = 0u;
    g_halt_calls = 0u;
    g_halt_task  = 0u;

    g_task_rc = 0;
    g_task_seq = 0u;
    g_task_calls = 0u;
    g_task_entry = 0;
    g_task_stack = 0;
    g_task_stack_size = 0u;
    g_task_prio = 0u;
    g_task_period = 0u;

    g_crash_disabled = 0u;
    g_crash_count_val = 0u;
    g_hash_calls = 0u;
    g_hash_path[0] = '\0';
    g_hash_last = 0u;
    g_fault_calls = 0u;
    g_fault_hash = 0u;
    g_fault_reason = 0u;
    g_forget_calls = 0u;
    g_forget_hash = 0u;

#if (ARKRTOS_USE_TASK_SWAP == 1)
    g_pending_rc = 0;
    g_arm_rc = 0;
    g_take_rc = 0;
    g_arm_calls = 0u;
    g_arm_task = 0;
    g_take_calls = 0u;
    g_cancel_calls = 0u;
    g_cancel_task = 0;
#endif

    g_fs_file_len = 0u;
    g_fs_read_rc = 0;
    g_fs_read_calls = 0u;
    g_fs_write_calls = 0u;
    g_fs_written_len = 0u;
    g_fs_write_rc = 0;
    g_fs_remove_calls = 0u;
    g_fs_remove_path[0] = '\0';
    g_fs_last_path[0] = '\0';

    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    arkrtos_task_count = 8;

    memset(g_code_pool, 0, sizeof(g_code_pool));
    memset(g_ram_pool, 0, sizeof(g_ram_pool));
}

/* 每个用例开头都从「没有小程序在跑、上限回默认、镜像自洽」出发。 */
static void reset_all(void)
{
    (void)arkrtos_mini_stop();

    reset_stubs();

    (void)arkrtos_mini_set_max_count(ARKRTOS_MINI_MAX);
    (void)arkrtos_mini_set_max_bytes(0u);

    img_build(600u, 8192u);
}

/* 一条正常装载（600 字节负载 → 代码块 1KB、RAM 块 8KB）。返回 0 表示成功。 */
static int32 run_ok(const char *path)
{
    return arkrtos_mini_run(path);
}

/* 从借出记录里取第 n 次借的代码块基址（1 起；0 = 越界）。 */
static uint32 code_base_of(uint32 n)
{
    if((n == 0u) || (n > POOL_N))
    {
        return 0u;
    }
    return (uint32)(uintptr_t)&g_code_pool[(n - 1u) % POOL_N][0];
}

static uint32 ram_base_of(uint32 n)
{
    if((n == 0u) || (n > POOL_N))
    {
        return 0u;
    }
    return (uint32)(uintptr_t)&g_ram_pool[(n - 1u) % POOL_N][0];
}

/* ============================================================================
 * A1. 路径参数
 * ==========================================================================*/

static void t_path(void)
{
    char long_path[ARKRTOS_MINI_PATH_MAX + 2u];
    uint32 i;

    reset_all();

    ck_i32(arkrtos_mini_run(0), ARKRTOS_LOADER_ERR_PARAM, "run(NULL) rejected");
    ck_i32(arkrtos_mini_run(""), ARKRTOS_LOADER_ERR_PARAM, "run(\"\") rejected");
    ck_u32(g_alloc_calls, 0u, "no block borrowed for a bad path");
    ck_u32((uint32)g_vfs_calls, 0u, "no file read for a bad path");

    /* 63 字节（不含结尾 0）= 能放下的最长路径 */
    for(i = 0u; i < (ARKRTOS_MINI_PATH_MAX - 1u); i++)
    {
        long_path[i] = 'a';
    }
    long_path[ARKRTOS_MINI_PATH_MAX - 1u] = '\0';
    ck_i32(run_ok(long_path), 0, "63-byte path accepted");
    ck_u32(g_vfs_calls > 0u ? 1u : 0u, 1u, "63-byte path actually read the image");

    /* 64 字节：放不下，必须拒绝而不是截断（截断会让两条路径撞成同一身份） */
    for(i = 0u; i < ARKRTOS_MINI_PATH_MAX; i++)
    {
        long_path[i] = 'a';
    }
    long_path[ARKRTOS_MINI_PATH_MAX] = '\0';
    ck_i32(arkrtos_mini_run(long_path), ARKRTOS_LOADER_ERR_PARAM,
           "64-byte path rejected");
    ck_u32((uint32)arkrtos_mini_running(), 1u, "only the good one is running");
}

/* ============================================================================
 * A2. 装载成功面：六步全部走通，记账逐项核对
 * ==========================================================================*/

static void t_load_ok(void)
{
    arkrtos_mini_info_t info;
    uint32 stack_bottom;

    reset_all();

    ck_i32(run_ok("/mini/app.svcm"), 0, "run succeeded");
    ck_u32((uint32)arkrtos_mini_running(), 1u, "one instance running");
    ck_u32(g_alloc_calls, 1u, "one pair borrowed");
    ck_u32(g_alloc_code_bytes, 600u, "code span handed to the pool is the payload size");
    ck_u32(g_alloc_ram_bytes, 8192u, "RAM demand handed to the pool is the declared size");
    ck_u32(g_free_calls, 0u, "nothing returned while it runs");
    ck_u32(g_task_calls, 1u, "one task registered");
    ck_u32(g_task_stack_size, (uint32)APP_TASK_STACK_SIZE, "stack size is APP_TASK_STACK_SIZE");
    ck_u32(g_task_prio, (uint32)ARKRTOS_MINI_TASK_PRIORITY, "task priority is the mini default");
    ck_u32(g_task_period, (uint32)APP_TASK_PERIOD_MS, "period is APP_TASK_PERIOD_MS");

    /* 入口 = 代码块基址 + entry_offset（LA64 不加 Thumb 位） */
    ck_u32((uint32)(uintptr_t)g_task_entry, code_base_of(1u) + 8u,
           "entry = code base + entry_offset");

    /* 栈底 = RAM 块顶 - 栈大小 */
    stack_bottom = (uint32)(uintptr_t)g_task_stack;
    ck_u32(stack_bottom, ram_base_of(1u) + 8192u - (uint32)APP_TASK_STACK_SIZE,
           "stack bottom sits at the top of the RAM block");

    /* TCB 的两组窗口：代码块（整块，不是负载长度）/ RAM 块；小程序非特权 */
    ck_u32(arkrtos_task_table[0].rom_start, code_base_of(1u), "TCB rom_start = code block");
    ck_u32(arkrtos_task_table[0].rom_size, 1024u, "TCB rom_size = pow2(code span)");
    ck_u32(arkrtos_task_table[0].ram_start, ram_base_of(1u), "TCB ram_start = RAM block");
    ck_u32(arkrtos_task_table[0].ram_size, 8192u, "TCB ram_size = RAM block");
    ck_u32((uint32)arkrtos_task_table[0].is_priv, 0u, "mini task is unprivileged");

    /* 清单里的每一项都要与借还凭据对得上 */
    ck_i32(arkrtos_mini_info(&info), 0, "info() returns the running one");
    ck_u32(info.task_id, 1u, "info.task_id");
    ck_u32(info.code_base, code_base_of(1u), "info.code_base");
    ck_u32(info.code_block, 1024u, "info.code_block");
    ck_u32(info.code_size, 600u, "info.code_size is the payload length");
    ck_u32(info.ram_base, ram_base_of(1u), "info.ram_base");
    ck_u32(info.ram_size, 8192u, "info.ram_size");
    ck_u32(info.entry, code_base_of(1u) + 8u, "info.entry");
    ck_u32(info.crc, img_r32(28u), "info.crc is the computed CRC");

    /* 六步的调用账：头一次 + 表一次（0 条不发读）+ 负载 3 段 = 4 次读 */
    ck_u32((uint32)g_vfs_calls, 4u, "header + 3 payload chunks read");
    ck_u32((uint32)g_chk_calls, 2u, "header checked twice (fields then table)");
    ck_u32(g_chk_mask, ARKRTOS_APP_TYPE_MASK_MINI, "only the MiniApp type mask is accepted");
    ck_u32(g_chk_rel_present, 1u, "second check sees the reloc table");
    ck_u32((uint32)g_reloc_calls, 3u, "one patch call per payload chunk");
    ck_u32((uint32)g_crc_header_calls, 1u, "crc starts from loader_crc_header");
    ck_u32((uint32)g_activate_calls, 1u, "scheduler asked to switch to the mini priority");
    ck_u32(g_activate_prio, (uint32)ARKRTOS_MINI_TASK_PRIORITY, "activate uses mini priority");

    /* 装载期间每段之后都放开过调度器锁（YIELD_CHUNKS=1） */
    ck(g_sched_lock_calls >= 4u, "loader takes the scheduler lock per stage/chunk");
    ck(g_sched_unlock_calls >= 4u, "loader releases it again (yield between chunks)");

    /* 第二份镜像落在另一块内存上，两块的地址必须不同 */
    ck_i32(run_ok("/mini/b.svcm"), 0, "second instance starts");
    ck_u32((uint32)arkrtos_mini_running(), 2u, "two instances running");
    ck(arkrtos_mini_info_at(1u, &info) == 0, "info_at(1) reachable");
    ck_u32(info.code_base, code_base_of(2u), "second instance has its own code block");
    ck(info.code_base != code_base_of(1u), "two live instances never share a code block");
}

/* ============================================================================
 * A3. 失败面：每一步都要「报对错 + 不留块 + 不留占位」
 * ==========================================================================*/

/* 失败之后的共同断言：块已还（借了几次就还了几次）、实例项空了（再跑能成）。 */
static void ck_no_leak(uint32 borrowed, const char *what)
{
    char msg[96];

    (void)snprintf(msg, sizeof(msg), "%s: every borrowed pair returned", what);
    ck_u32(g_free_calls, borrowed, msg);

    /* 重试之前要把「一次性注错」收回来，并把镜像恢复成自洽的那份：
     * 失败用例是故意把注入点留在那儿的（CRC 用例甚至改坏了负载字节），
     * 不收回来，重试失败证明的是「你没清现场」，不是「槽位没释放」。 */
    g_alloc_rc        = 0;
    g_task_rc         = 0;
    g_reloc_ret_delta = 0u;
    img_build(600u, 8192u);

    (void)snprintf(msg, sizeof(msg), "%s: instance slot freed (a new run works)", what);
    ck_i32(run_ok("/mini/after.svcm"), 0, msg);

    (void)snprintf(msg, sizeof(msg), "%s: exactly one running after the retry", what);
    ck_u32((uint32)arkrtos_mini_running(), 1u, msg);
}

static void t_fail_reads(void)
{
    reset_all();
    g_vfs_ready = 0u;
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_MINI_ERR_FS,
           "not mounted -> MINI_ERR_FS");
    ck_u32(g_alloc_calls, 0u, "no block borrowed before the mount check");

    reset_all();
    g_vfs_rc = -3;
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_MINI_ERR_FS,
           "header read error -> MINI_ERR_FS");
    ck_u32((uint32)arkrtos_mini_last_read_error(), (uint32)-3,
           "last_read_error carries the driver code");
    ck_u32(g_alloc_calls, 0u, "no block borrowed when the header is unreadable");

    reset_all();
    g_vfs_short = 1u;
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_MINI_ERR_FS,
           "short header read -> MINI_ERR_FS");
    ck_u32((uint32)arkrtos_mini_last_read_error(), (uint32)ARKRTOS_MINI_ERR_SHORT_READ,
           "short read has its own code (not reported as a driver error)");

    /* 表读失败：头已经校过一次，第二次 check_header 还没发生。
     * 注意 reloc_count=0 时 read_exact 对 0 字节直接返回、不碰 VFS，
     * 所以这里要给一条真表项，第二次读才是「读表」。 */
    reset_all();
    img_w32(116u, 1u);              /* reloc_count = 1 */
    g_vfs_fail_at = 2;
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_MINI_ERR_FS,
           "reloc table read error -> MINI_ERR_FS");
    ck_u32((uint32)g_chk_calls, 1u, "table read fails before the second check");
    ck_u32(g_alloc_calls, 0u, "no block borrowed when the table is unreadable");

    /* 负载读失败：块已经借出，必须原样还回去 */
    reset_all();
    g_vfs_fail_at = 3;
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_MINI_ERR_FS,
           "payload read error -> MINI_ERR_FS");
    ck_u32(g_alloc_calls, 1u, "payload stage is after the borrow");
    ck_no_leak(1u, "payload read error");

    /* 成功之后 last_read_error 必须归零（不是上一次失败的残留） */
    reset_all();
    g_vfs_rc = -3;
    (void)arkrtos_mini_run("/mini/bad.svcm");
    g_vfs_rc = 0;
    ck_i32(run_ok("/mini/good.svcm"), 0, "a good run after a failed one");
    ck_i32(arkrtos_mini_last_read_error(), 0, "last_read_error cleared by a good run");
}

static void t_fail_header(void)
{
    reset_all();
    g_chk_rc_first = ARKRTOS_LOADER_ERR_MAGIC;
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_LOADER_ERR_MAGIC,
           "bad header fields -> loader's code passed through");
    ck_u32((uint32)g_chk_calls, 1u, "no second check after a bad header");
    ck_u32(g_alloc_calls, 0u, "no block borrowed for a bad header");

    reset_all();
    g_chk_rc_second = ARKRTOS_LOADER_ERR_RELOC;
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_LOADER_ERR_RELOC,
           "bad reloc table -> loader's code passed through");
    ck_u32((uint32)g_chk_calls, 2u, "table is checked after it is read");
    ck_u32(g_alloc_calls, 0u, "no block borrowed for a bad table");

    /* 第二档必须看到表体指针，否则「字段自洽」会被当成「表合法」 */
    ck_u32(g_chk_rel_present, 1u, "second check is handed the reloc table");
}

static void t_fail_size(void)
{
    /* 代码块超过运行期上限 */
    reset_all();
    img_build(20000u, 8192u);           /* pow2 → 32768 = 恰好等于默认上限 */
    ck_i32(run_ok("/mini/big.svcm"), 0, "a 32KB code block still fits the default cap");

    reset_all();
    img_build(33000u, 8192u);           /* pow2 → 65536 > 32KB */
    ck_i32(arkrtos_mini_run("/mini/big.svcm"), ARKRTOS_LOADER_ERR_SIZE,
           "code block over the cap -> SIZE");
    ck_u32(g_alloc_calls, 0u, "no block borrowed over the cap");

    /* RAM 需求超过上限 */
    reset_all();
    img_build(600u, 65536u);
    ck_i32(arkrtos_mini_run("/mini/big.svcm"), ARKRTOS_LOADER_ERR_SIZE,
           "RAM demand over the cap -> SIZE");

    /* RAM 需求装不下自己的栈 */
    reset_all();
    img_build(600u, 2048u);
    ck_i32(arkrtos_mini_run("/mini/big.svcm"), ARKRTOS_LOADER_ERR_SIZE,
           "RAM demand below the stack size -> SIZE");

    /* 回绕自证：image_size 大到 pow2_ceil 转回来比负载还小（check_header 是桩，
     * 合法上限由它负责；这里专门量 mini 自己那条回绕断言） */
    reset_all();
    img_build(600u, 8192u);
    img_w32(16u, 0xFFFFFF00u);          /* image_size：align8 后回绕 */
    ck_i32(arkrtos_mini_run("/mini/wrap.svcm"), ARKRTOS_LOADER_ERR_SIZE,
           "image_size wraparound is refused by the SIZE self-check");
    ck_u32(g_alloc_calls, 0u, "no block borrowed for a wrapped size");

    /* 运行期把上限收紧到 4KB：8KB 的 RAM 块就该被拒 */
    reset_all();
    ck_i32(arkrtos_mini_set_max_bytes(4096u), 0, "cap tightened to 4KB");
    ck_u32(arkrtos_mini_max_bytes(), 4096u, "max_bytes reads back");
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_LOADER_ERR_SIZE,
           "8KB RAM demand refused at a 4KB cap");
    ck_u32(g_alloc_calls, 0u, "no block borrowed after the tighter cap");

    /* 收紧之后装载用的是「当下」的上限：4KB RAM 的镜像能过 */
    img_build(600u, 4096u);
    ck_i32(run_ok("/mini/small.svcm"), 0, "4KB demand accepted at a 4KB cap");
    ck_u32(arkrtos_task_table[0].ram_size, 4096u, "RAM block follows the tighter cap");
}

static void t_fail_alloc(void)
{
    reset_all();
    g_alloc_rc = -1;
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_LOADER_ERR_NOSPACE,
           "pool says no -> NOSPACE");
    ck_u32(g_alloc_calls, 1u, "the pool was asked exactly once");
    ck_u32(g_free_calls, 0u, "nothing to return when the borrow itself failed");
    g_alloc_rc = 0;
    ck_i32(run_ok("/mini/after.svcm"), 0, "slot freed after NOSPACE");
}

static void t_fail_crc(void)
{
    reset_all();
    g_img[IMG_PAYLOAD + 10u] ^= 0xFFu;      /* 负载改一个字节，头里的 CRC 不变 */
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_LOADER_ERR_CRC,
           "payload CRC mismatch -> CRC");
    ck_u32(g_alloc_calls, 1u, "blocks were borrowed before the CRC check");
    ck_no_leak(1u, "CRC mismatch");

    /* CRC 的种子来自 loader_crc_header：种子变了，同一份负载就不再匹配 */
    reset_all();
    g_crc_seed = 0x12345678u;
    img_build(600u, 8192u);                 /* crc_expected() 已按新种子算 */
    ck_i32(run_ok("/mini/seed.svcm"), 0, "CRC seeded from loader_crc_header");
    ck_u32((uint32)g_crc_header_calls, 1u, "seed comes from exactly one call");
}

static void t_fail_task(void)
{
    reset_all();
    g_task_rc = ARKRTOS_LOADER_ERR_TASK;
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_LOADER_ERR_TASK,
           "task table full -> TASK");
    ck_u32(g_alloc_calls, 1u, "blocks were borrowed before the task registration");
    ck_no_leak(1u, "task registration failure");
}

/* 重定位段尾切开：mini 必须回退到表项起点重读，而不是带着半个表项往下走。 */
static void t_reloc_rewind(void)
{
    reset_all();
    g_reloc_ret_delta = 1u;
    ck_i32(run_ok("/mini/rewind.svcm"), 0, "a cut reloc entry is completed by a re-read");
    /* 回退重读的证据不是「调用次数变多」（次数不变），而是第二次补丁从
     * 第一条被切开的表项起点开始：偏移只前进了 len-4。 */
    ck_u32(g_reloc_off1, (uint32)ARKRTOS_APP_HEADER_SIZE,
           "first patch starts at the payload head (image offset)");
    ck_u32(g_reloc_off2, (uint32)(ARKRTOS_APP_HEADER_SIZE + (256u - 4u)),
           "the second patch rewinds to the cut entry's start");
    ck_u32(g_reloc_len1, 256u, "the first chunk is one full buffer");
    ck_u32(g_alloc_calls, 1u, "still exactly one pair borrowed");

    /* 补丁指针不前进：不能带着「漏打一次补丁」的镜像去执行 */
    reset_all();
    g_reloc_ret_delta = 2u;
    ck_i32(arkrtos_mini_run("/mini/stuck.svcm"), ARKRTOS_LOADER_ERR_RELOC,
           "a non-advancing reloc pointer -> RELOC");
    ck_no_leak(1u, "non-advancing reloc pointer");
}

/* ============================================================================
 * B1. 并发上限与实例记账
 * ==========================================================================*/

static void t_limits(void)
{
    arkrtos_mini_info_t info;

    reset_all();

    ck_u32(arkrtos_mini_max_count(), (uint32)ARKRTOS_MINI_MAX, "default count = compile-time cap");
    ck_i32(arkrtos_mini_set_max_count(0u), ARKRTOS_LOADER_ERR_PARAM, "count 0 rejected");
    ck_i32(arkrtos_mini_set_max_count(ARKRTOS_MINI_MAX + 1u), ARKRTOS_LOADER_ERR_PARAM,
           "count above the compile-time cap rejected");
    ck_u32(arkrtos_mini_max_count(), (uint32)ARKRTOS_MINI_MAX, "a rejected count changes nothing");

    ck_i32(arkrtos_mini_set_max_count(1u), 0, "count can be tightened to 1");
    ck_i32(run_ok("/mini/a.svcm"), 0, "first instance starts");
    ck_i32(arkrtos_mini_run("/mini/b.svcm"), ARKRTOS_LOADER_ERR_BUSY,
           "second instance rejected at count=1");
    ck_u32(g_alloc_calls, 1u, "a rejected run borrows nothing");

    ck_i32(arkrtos_mini_set_max_count(2u), 0, "count widened back to 2");
    ck_i32(run_ok("/mini/b.svcm"), 0, "second instance now starts");
    ck_i32(arkrtos_mini_run("/mini/c.svcm"), ARKRTOS_LOADER_ERR_BUSY,
           "third instance rejected: slots are full");

    ck_i32(arkrtos_mini_info_at(0u, &info), 0, "info_at(0)");
    ck_u32(info.task_id, 1u, "info_at(0) is the first task");
    ck_i32(arkrtos_mini_info_at(1u, &info), 0, "info_at(1)");
    ck_u32(info.task_id, 2u, "info_at(1) is the second task");
    ck_i32(arkrtos_mini_info_at(2u, &info), ARKRTOS_MINI_ERR_NOSTATE,
           "info_at(2) has no third instance");
    ck_i32(arkrtos_mini_info_at(0u, 0), ARKRTOS_MINI_ERR_PARAM, "info_at(NULL) rejected");
    ck_i32(arkrtos_mini_info(0), ARKRTOS_MINI_ERR_PARAM, "info(NULL) rejected");

    /* task_is_main 只认正在跑的主任务 */
    ck_i32(arkrtos_mini_task_is_main(1u), 1, "task 1 is a mini main task");
    ck_i32(arkrtos_mini_task_is_main(2u), 1, "task 2 is a mini main task");
    ck_i32(arkrtos_mini_task_is_main(3u), 0, "task 3 belongs to nobody");
    ck_i32(arkrtos_mini_task_is_main(0u), 0, "task 0 is never a mini");

    /* stop_at 停掉指定那一个，剩下的重新密集编号 */
    ck_i32(arkrtos_mini_stop_at(1u), 0, "stop_at(1) stops the second instance");
    ck_u32((uint32)arkrtos_mini_running(), 1u, "one left");
    ck_u32(g_free_calls, 1u, "its pair was returned");
    ck_u32((uint32)g_halt_calls, 1u, "its image was halted before the blocks went back");
    ck_u32(g_halt_task, 2u, "halt targeted the stopped task");
    ck_i32(arkrtos_mini_task_is_main(2u), 0, "the stopped task is no longer a mini");
    ck_i32(arkrtos_mini_stop_at(5u), ARKRTOS_LOADER_ERR_STATE, "stop_at out of range");

    ck_i32(arkrtos_mini_info(&info), 0, "the remaining one is index 0 again");
    ck_u32(info.task_id, 1u, "remaining task id unchanged");

    ck_i32(arkrtos_mini_stop(), 0, "stop() stops the last one");
    ck_u32((uint32)arkrtos_mini_running(), 0u, "nothing running");
    ck_u32(g_free_calls, 2u, "both pairs returned");
    ck_i32(arkrtos_mini_stop(), ARKRTOS_LOADER_ERR_STATE, "stop() with nothing running");
    ck_i32(arkrtos_mini_info(&info), ARKRTOS_MINI_ERR_NOSTATE, "info() with nothing running");
}

static void t_max_bytes(void)
{
    reset_all();

    ck_u32(arkrtos_mini_max_bytes(), (uint32)SLOT_RAM_MAX_BLOCK, "default cap = SLOT_RAM_MAX_BLOCK");
    ck_i32(arkrtos_mini_set_max_bytes(0u), 0, "0 restores the compile-time default");
    ck_u32(arkrtos_mini_max_bytes(), (uint32)SLOT_RAM_MAX_BLOCK, "still the default");

    ck_i32(arkrtos_mini_set_max_bytes(512u), ARKRTOS_LOADER_ERR_PARAM,
           "below SLOT_RAM_MIN_BLOCK rejected");
    ck_i32(arkrtos_mini_set_max_bytes(1000u), ARKRTOS_LOADER_ERR_PARAM,
           "not a power of two rejected");
    ck_i32(arkrtos_mini_set_max_bytes((uint32)SLOT_RAM_MAX_BLOCK * 2u),
           ARKRTOS_LOADER_ERR_PARAM, "above SLOT_RAM_MAX_BLOCK rejected");
    ck_u32(arkrtos_mini_max_bytes(), (uint32)SLOT_RAM_MAX_BLOCK, "rejections change nothing");

    ck_i32(arkrtos_mini_set_max_bytes(8192u), 0, "8KB accepted");
    ck_u32(arkrtos_mini_max_bytes(), 8192u, "reads back 8KB");
    ck_i32(run_ok("/mini/a.svcm"), 0, "8KB demand runs at an 8KB cap");
}

/* ============================================================================
 * B2. 任务退出与故障归属
 * ==========================================================================*/

static void t_exit_fault(void)
{
    arkrtos_mini_info_t info;

    reset_all();

    ck_i32(arkrtos_mini_on_task_exit(0u), 0, "exit of task 0 is ignored");
    ck_i32(arkrtos_mini_on_task_exit(7u), 0, "exit of a non-mini task is ignored");

    ck_i32(run_ok("/mini/a.svcm"), 0, "instance starts");
    ck_i32(arkrtos_mini_on_task_exit(1u), 1, "main task exit is handled");
    ck_u32((uint32)arkrtos_mini_running(), 0u, "exit takes it out of the running set");
    ck_u32(g_free_calls, 1u, "exit returns the pair");
    ck_u32(g_halt_calls, 1u, "exit halts the image (its own threads included)");
    ck_u32(g_halt_task, 1u, "halt targeted the exiting task");
    ck_i32(arkrtos_mini_info(&info), ARKRTOS_MINI_ERR_NOSTATE, "slot is free after exit");

    /* 故障：只认落在自己 RAM 窗口里的任务 */
    reset_all();
    ck_i32(run_ok("/mini/a.svcm"), 0, "instance starts for the fault test");
    ck_i32(arkrtos_mini_on_fault(0), 0, "fault of task 0 ignored");
    ck_i32(arkrtos_mini_on_fault(99), 0, "fault above the task count ignored");

    /* 一个不在自己窗口内的任务：改 TCB 的 ram 区间来模拟 */
    reset_all();
    ck_i32(run_ok("/mini/a.svcm"), 0, "instance starts again");
    arkrtos_task_table[1].ram_start = ram_base_of(1u) + 0x4000u;   /* 越出窗口 */
    arkrtos_task_table[1].ram_size  = 1024u;
    arkrtos_task_count = 8;
    ck_i32(arkrtos_mini_on_fault(2), 0, "a task outside the window is not ours");
    ck_u32((uint32)arkrtos_mini_running(), 1u, "and it is not torn down");

    /* 小程序的子线程：窗口落在同一块 RAM 里，故障归整个小程序 */
    arkrtos_task_table[1].ram_start = ram_base_of(1u) + 0x1000u;
    arkrtos_task_table[1].ram_size  = 1024u;
    g_crash_disabled = 0u;
    ck_i32(arkrtos_mini_on_fault(2), 0, "below the limit: default path keeps it running");
    ck_u32(g_fault_calls, 1u, "the fault was recorded under the mini's path hash");
    ck_u32(g_fault_hash, g_hash_last, "recorded hash is the path hash");
    ck_u32(g_fault_reason, ARKRTOS_CRASH_REASON_FAULT, "reason is FAULT");
    ck_u32((uint32)arkrtos_mini_running(), 1u, "still running below the limit");

    /* 到上限：停掉、还块、撤 MPU、清项，返回 1 表示「不要恢复它」 */
    g_crash_disabled = 1u;
    ck_i32(arkrtos_mini_on_fault(2), 1, "at the limit the mini is disabled");
    ck_u32((uint32)arkrtos_mini_running(), 0u, "disabled mini is not running");
    ck_u32(g_free_calls, 1u, "disabled mini returned its pair");
    ck_u32(g_halt_calls, 1u, "disabled mini halted its image");
    ck_i32(arkrtos_mini_info(&info), ARKRTOS_MINI_ERR_NOSTATE, "slot freed after disabling");

    /* 同一路径再跑：claim 阶段就被挡回 DISABLED */
    ck_i32(arkrtos_mini_run("/mini/a.svcm"), ARKRTOS_MINI_ERR_DISABLED,
           "a disabled path is refused at claim time");

    /* forget 放行：把哈希交给崩溃日记，之后允许再跑 */
    g_crash_disabled = 0u;
    ck_i32(arkrtos_mini_forget(0), ARKRTOS_MINI_ERR_PARAM, "forget(NULL) rejected");
    ck_i32(arkrtos_mini_forget(""), ARKRTOS_MINI_ERR_PARAM, "forget(\"\") rejected");
    ck_i32(arkrtos_mini_forget("/mini/a.svcm"), 0, "forget(path) accepted");
    ck_u32(g_forget_calls, 1u, "crash journal told to forget");
    ck_u32(g_forget_hash, g_hash_last, "forget uses the same path hash as the fault record");
    ck_i32(run_ok("/mini/a.svcm"), 0, "the path runs again after forget");
}

/* ============================================================================
 * C. 无缝升级（A/B 就地换）
 * ==========================================================================*/

#if (ARKRTOS_USE_TASK_SWAP == 1)

static void t_swap(void)
{
    arkrtos_mini_info_t info;

    reset_all();

    ck_i32(arkrtos_mini_swap_at(0u, 0), ARKRTOS_LOADER_ERR_PARAM, "swap(NULL) rejected");
    ck_i32(arkrtos_mini_swap_at(0u, ""), ARKRTOS_LOADER_ERR_PARAM, "swap(\"\") rejected");
    ck_i32(arkrtos_mini_swap_at(0u, "/mini/b.svcm"), ARKRTOS_LOADER_ERR_STATE,
           "swap with nothing running -> STATE");

    ck_i32(run_ok("/mini/a.svcm"), 0, "old version starts");

    /* 内核那边已经挂着一个切换：不许再挂（内核只有一个挂起槽） */
    g_pending_rc = 1;
    ck_i32(arkrtos_mini_swap_at(0u, "/mini/b.svcm"), ARKRTOS_LOADER_ERR_BUSY,
           "kernel already has a pending swap -> BUSY");
    g_pending_rc = 0;

    /* 布局不同（新镜像 RAM 需求不同）→ LAYOUT，并把新借的那一对还回去 */
    img_build(600u, 4096u);
    ck_i32(arkrtos_mini_swap_at(0u, "/mini/b.svcm"), ARKRTOS_SWAP_ERR_LAYOUT,
           "different layout -> SWAP_ERR_LAYOUT");
    ck_u32(g_alloc_calls, 2u, "the new pair was borrowed before the layout check");
    ck_u32(g_free_calls, 1u, "a refused swap returns the new pair");
    ck_u32(g_free_last, 2u, "the returned borrow is the new one, not the live one");
    ck_u32((uint32)arkrtos_mini_running(), 1u, "old version still running");

    /* 布局一致 → 挂上；内核拒绝一次之后再挂 */
    img_build(600u, 8192u);
    g_arm_rc = ARKRTOS_SWAP_ERR_LAYOUT;
    ck_i32(arkrtos_mini_swap_at(0u, "/mini/b.svcm"), ARKRTOS_SWAP_ERR_LAYOUT,
           "kernel refuses the arm -> its code passed through");
    ck_u32(g_arm_calls, 1u, "kernel asked to arm");
    ck_i32(g_arm_task, 1, "arm targets the live main task");
    ck_u32(g_free_calls, 2u, "a refused arm returns the new pair too");
    ck_u32((uint32)arkrtos_mini_running(), 1u, "still the old version");

    g_arm_rc = 0;
    ck_i32(arkrtos_mini_swap_at(0u, "/mini/b.svcm"), 0, "swap armed");
    ck_u32(g_alloc_calls, 4u, "one more pair for the new version");
    ck_u32(g_free_calls, 2u, "the old pair is NOT returned until the switch lands");

    /* 只有目标任务的下一次系统调用才落地 */
    ck_u32((uint32)g_take_calls, 0u, "take not called yet");
    ck_i32(arkrtos_mini_swap_take(0, 2), 0, "another task's syscall does nothing");
    ck_i32(arkrtos_mini_swap_take(0, 1), 0, "frame not in the old block -> wait for the next");
    ck_u32(g_alloc_calls, 4u, "waiting does not borrow again");
    ck_u32((uint32)arkrtos_mini_running(), 1u, "still running the old version");

    /* 内核落地成功：实例改指新块，旧块归还，RC=1 表示这一帧已经换掉 */
    g_take_rc = 1;
    ck_i32(arkrtos_mini_swap_take(0, 1), 1, "take reports the switch landed");
    ck_u32(g_free_calls, 3u, "the old pair is returned at the landing point");
    ck_u32(g_free_last, 1u, "the returned borrow is the OLD one");
    ck_i32(arkrtos_mini_info(&info), 0, "instance still readable after the switch");
    ck_u32(info.code_base, code_base_of(2u), "info points at the new code block");
    ck_u32(info.ram_base, ram_base_of(2u), "info points at the new RAM block");
    ck_u32(info.task_id, 1u, "task id is unchanged across the switch");
    ck_i32(arkrtos_mini_swap_take(0, 1), 0, "a second take is a no-op (nothing in flight)");

    /* 内核拒绝落地（帧形状/布局）：新块还回去，实例继续跑旧镜像 */
    img_build(600u, 8192u);
    ck_i32(arkrtos_mini_swap_at(0u, "/mini/c.svcm"), 0, "another swap armed");
    g_take_rc = -2;
    ck_i32(arkrtos_mini_swap_take(0, 1), 0, "refused landing reports 0 (keep executing)");
    ck_u32(g_free_calls, 4u, "the refused new pair is returned");
    ck_i32(arkrtos_mini_info(&info), 0, "instance readable after the refusal");
    ck_u32(info.code_base, code_base_of(2u), "still on the previous image");
    g_take_rc = 0;

    /* 停掉一个「正在换」的实例：在途的那份新块要一起撤掉 */
    img_build(600u, 8192u);
    ck_i32(arkrtos_mini_swap_at(0u, "/mini/d.svcm"), 0, "swap armed again");
    ck_u32(g_free_calls, 4u, "nothing returned while in flight");
    ck_i32(arkrtos_mini_stop_at(0u), 0, "stop while a swap is in flight");
    ck_u32((uint32)g_cancel_calls, 1u, "kernel-side pending swap cancelled");
    ck_u32((uint32)g_cancel_task, 0u, "cancel is asked to drop any pending swap");
    ck_u32(g_free_calls, 6u, "both the live pair and the in-flight pair returned");
    ck_u32((uint32)arkrtos_mini_running(), 0u, "nothing running");
}

#endif /* ARKRTOS_USE_TASK_SWAP */

/* ============================================================================
 * D. 开机自启清单
 * ==========================================================================*/

static char   g_seen[8][ARKRTOS_MINI_PATH_MAX];
static uint32 g_seen_n;

static int collect_cb(const char *path, void *arg)
{
    (void)arg;

    if(g_seen_n < 8u)
    {
        (void)snprintf(g_seen[g_seen_n], sizeof(g_seen[0]), "%s", path);
    }
    g_seen_n++;

    return 0;
}

static void fs_set_text(const char *txt)
{
    uint32 n = (uint32)strlen(txt);

    if(n > sizeof(g_fs_file))
    {
        n = sizeof(g_fs_file);
    }
    memcpy(g_fs_file, txt, n);
    g_fs_file_len = n;
}

static int fs_written_is(const char *want)
{
    uint32 n = (uint32)strlen(want);

    if(g_fs_written_len != n)
    {
        return 0;
    }
    return (memcmp(g_fs_written, want, n) == 0) ? 1 : 0;
}

static void t_autostart_parse(void)
{
    char long_line[ARKRTOS_MINI_PATH_MAX + 8u];
    uint32 i;

    reset_all();

    /* 注释 / 空行 / CRLF / 行内首尾空白 / 重复项 */
    fs_set_text("# comment\r\n\r\n  /mini/a.svcm  \r\n\t/mini/b.svcm\n/a.svcm\r\n/a.svcm\n");
    g_seen_n = 0u;
    ck_i32(arkrtos_mini_autostart_foreach(collect_cb, 0), 0, "foreach walks the list");
    ck_u32(g_seen_n, 4u, "four valid entries (comments and blanks skipped)");
    ck(strcmp(g_seen[0], "/mini/a.svcm") == 0, "leading/trailing blanks trimmed");
    ck(strcmp(g_seen[1], "/mini/b.svcm") == 0, "tabs trimmed");
    ck(strcmp(g_seen[2], "/a.svcm") == 0, "CRLF line ending handled");
    ck(strcmp(g_seen[3], "/a.svcm") == 0, "duplicates are reported, not merged");

    ck_i32(arkrtos_mini_autostart_foreach(0, 0), ARKRTOS_MINI_ERR_PARAM,
           "foreach(NULL) rejected");

    /* 超长条目：跳过并继续后面的条目（不截断、也不静默丢掉整份清单） */
    for(i = 0u; i < (ARKRTOS_MINI_PATH_MAX + 6u); i++)
    {
        long_line[i] = 'z';
    }
    long_line[ARKRTOS_MINI_PATH_MAX + 6u] = '\0';
    fs_set_text(long_line);
    g_fs_file[g_fs_file_len++] = '\n';
    memcpy(&g_fs_file[g_fs_file_len], "/ok.svcm\n", 9);
    g_fs_file_len += 9u;

    g_seen_n = 0u;
    ck_i32(arkrtos_mini_autostart_foreach(collect_cb, 0), 0, "overlong entry does not abort the walk");
    ck_u32(g_seen_n, 1u, "only the short entry is reported");
    ck(strcmp(g_seen[0], "/ok.svcm") == 0, "the entry after the overlong one is still seen");

    /* 读失败：foreach 报 READ，autostart_now 报 FS */
    reset_all();
    g_fs_read_rc = -1;
    ck_i32(arkrtos_mini_autostart_foreach(collect_cb, 0), ARKRTOS_MINI_ERR_READ,
           "unreadable list -> READ");
    ck_i32(arkrtos_mini_autostart_now(), ARKRTOS_MINI_ERR_FS, "autostart_now -> FS");

    /* 没有清单是正常状态：boot_autostart 返回 0 且只尝试一次 */
    reset_all();
    g_fs_read_rc = -1;
    ck_u32(arkrtos_mini_boot_autostart(), 0u, "no list: boot starts nothing");
    {
        unsigned long reads = g_fs_read_calls;

        g_fs_read_rc = 0;
        fs_set_text("/mini/a.svcm\n");
        ck_u32(arkrtos_mini_boot_autostart(), 0u, "boot autostart runs only once per boot");
        ck_u32((uint32)g_fs_read_calls, (uint32)reads, "second boot call reads nothing");
    }
}

static void t_autostart_now(void)
{
    reset_all();

    fs_set_text("/mini/a.svcm\n/mini/b.svcm\n");
    ck_i32(arkrtos_mini_autostart_now(), 2, "two entries started");
    ck_u32((uint32)arkrtos_mini_running(), 2u, "two instances running");

    /* 第三条装不下（并发达上限）：逐条报，成功的仍成功 */
    reset_all();
    fs_set_text("/mini/a.svcm\n/mini/b.svcm\n/mini/c.svcm\n");
    ck_i32(arkrtos_mini_autostart_now(), 2, "the third entry is refused, the first two start");
    ck_u32((uint32)arkrtos_mini_running(), 2u, "still two running");

    /* 一条坏镜像不影响后面的条目：装载失败只报错、不中断遍历 */
    reset_all();
    fs_set_text("/mini/bad.svcm\n/mini/good.svcm\n");
    g_chk_rc_first = 0;
    {
        /* 第一份镜像用一份 CRC 不符的：把负载改坏再跑 */
        uint8 saved = g_img[IMG_PAYLOAD + 3u];

        g_img[IMG_PAYLOAD + 3u] = (uint8)(saved ^ 0x11u);
        ck_i32(arkrtos_mini_autostart_now(), 0, "a bad image starts nothing");
        ck_u32((uint32)arkrtos_mini_running(), 0u, "nothing left running");

        g_img[IMG_PAYLOAD + 3u] = saved;
        img_build(600u, 8192u);
    }
    ck_i32(arkrtos_mini_autostart_now(), 2, "both good entries start after the repair");
}

static void t_autostart_set(void)
{
    reset_all();

    ck_i32(arkrtos_mini_autostart_set(0, 1u), ARKRTOS_LOADER_ERR_PARAM, "set(NULL) rejected");
    ck_i32(arkrtos_mini_autostart_set("rel/path", 1u), ARKRTOS_LOADER_ERR_PARAM,
           "a path without a leading slash is rejected");

    /* 空清单 + 新增：写出的是「新条目 + 换行」 */
    fs_set_text("");
    ck_i32(arkrtos_mini_autostart_set("/mini/a.svcm", 1u), 0, "add to an empty list");
    ck_u32((uint32)g_fs_write_calls, 1u, "one write");
    ck(fs_written_is("/mini/a.svcm\n"), "written text is the new entry + newline");

    /* 已经在清单里再加：空改动不落盘 */
    fs_set_text("/mini/a.svcm\n/mini/b.svcm\n");
    ck_i32(arkrtos_mini_autostart_set("/mini/a.svcm", 1u), 0, "adding an existing entry is a no-op");
    ck_u32((uint32)g_fs_write_calls, 1u, "no write for an empty change");

    /* 删除中间那条：其余行原样保留 */
    ck_i32(arkrtos_mini_autostart_set("/mini/a.svcm", 0u), 0, "remove an entry");
    ck_u32((uint32)g_fs_write_calls, 2u, "removal writes");
    ck(fs_written_is("/mini/b.svcm\n"), "the other entry survives the rewrite");

    /* 本来就不在再删：空改动不落盘 */
    ck_i32(arkrtos_mini_autostart_set("/mini/zzz.svcm", 0u), 0, "removing a missing entry is a no-op");
    ck_u32((uint32)g_fs_write_calls, 2u, "no write for an empty change (remove)");

    /* 清单空了：删文件，而不是留一个 0 字节文件 */
    fs_set_text("/mini/only.svcm\n");
    ck_i32(arkrtos_mini_autostart_set("/mini/only.svcm", 0u), 0, "removing the last entry");
    ck_u32((uint32)g_fs_remove_calls, 1u, "the empty list removes the file");
    ck(strcmp(g_fs_remove_path, ARKRTOS_MINI_AUTOSTART_FILE) == 0, "removal targets the list file");

    /* 写失败：报 FS */
    reset_all();
    fs_set_text("");
    g_fs_write_rc = -1;
    ck_i32(arkrtos_mini_autostart_set("/mini/a.svcm", 1u), ARKRTOS_MINI_ERR_FS,
           "write failure -> FS");

    /* 装不下：16 条 63 字节的条目（= 1024 字节）重建时要再加一条新路径 */
    reset_all();
    {
        static char big[1100];
        uint32 o = 0u;
        uint32 i;
        uint32 j;

        for(i = 0u; i < 16u; i++)
        {
            big[o++] = '/';
            for(j = 0u; j < 62u; j++)
            {
                big[o++] = 'x';
            }
            big[o++] = '\n';
        }
        big[o] = '\0';
        fs_set_text(big);

        ck_i32(arkrtos_mini_autostart_set("/new.svcm", 1u), ARKRTOS_LOADER_ERR_NOSPACE,
               "a full list reports NOSPACE instead of truncating");
        ck_u32((uint32)g_fs_write_calls, 0u, "nothing is written when the rebuild overflows");
    }
}

/* ============================================================================
 * main
 * ==========================================================================*/

int main(void)
{
    reset_all();

    t_path();
    reset_all();
    t_load_ok();
    reset_all();
    t_fail_reads();
    reset_all();
    t_fail_header();
    reset_all();
    t_fail_size();
    reset_all();
    t_fail_alloc();
    reset_all();
    t_fail_crc();
    reset_all();
    t_fail_task();
    reset_all();
    t_reloc_rewind();
    reset_all();
    t_limits();
    reset_all();
    t_max_bytes();
    reset_all();
    t_exit_fault();
#if (ARKRTOS_USE_TASK_SWAP == 1)
    reset_all();
    t_swap();
#endif
    reset_all();
    t_autostart_parse();
    reset_all();
    t_autostart_now();
    reset_all();
    t_autostart_set();

    (void)arkrtos_mini_stop();

    printf("MINI HOST OK %d checks, %d failed\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}

#endif /* ARKRTOS_USE_MINIAPP */
