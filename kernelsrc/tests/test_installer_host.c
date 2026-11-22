/**
* @file test_installer_host.c
* @brief 内核内安装任务（arkrtos_installer.c）的宿主用例（不需要板子）
*
* 覆盖三条公开入口：arkrtos_installer_init() / run_once() / from_file()，
* 以及常驻任务的一轮循环体（-DARKRTOS_INSTALLER_HOST_TEST 暴露为
* arkrtos_installer_tick()）。
*
* 钉死的都是「装错了也看不出来」的地方：
*   - 串口字节流里的魔数同步（杂散字节、跨轮半截头、空转上限后不丢已收字节）；
*   - 收满头之后的三条拒绝（DUP / VERSION / 其它）必须写 NAK 0x15、把这一帧
*     的重定位表从 FIFO 里排掉、并且清掉已收头（否则日志每 50 ms 刷一行）；
*   - 自启标志：置位才调 loader_start，且只启刚落盘的槽；清位不启动；
*   - 固定槽模式的「先清后收」一次性闸门：未配置槽 / 非空槽 / 本上电已装过
*     一律拒绝；hint 缺失或清槽失败时在开窗阶段就返回，且不读串口；
*   - 文件入口：未挂载 / 非文件 / 尺寸越界 / 短读 / 魔数错 / 设备注册失败
*     每一档各自返回不同的错误码并记故障，成功时载荷确实从文件流里读出来。
*
* 打桩范围：设备框架、任务、分区表、布局表、loader、文件系统、故障、日志。
* 断言的是「调用序列与参数」，不是真实落盘 —— 也就是说本用例证明的是安装器
* 的决策与账目，不证明 Flash 写入、重定位、CRC 复核（那些在 loader 的用例与板上）。
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "arkrtos_installer.h"
#include "arkrtos_cfg.h"
#include "arkrtos_loader.h"
#include "arkrtos_app_image.h"
#include "arkrtos_partition.h"
#include "arkrtos_layout.h"
#include "arkrtos_layout_def.h"
#include "arkrtos_ptable.h"
#include "arkrtos_share.h"
#include "arkrtos_task.h"
#include "arkrtos_dev.h"
#include "arkrtos_fault.h"
#include "arkrtos_fs.h"
#include "arkrtos_log.h"

/* ------------------------------------------------------------------ */
/* 断言                                                                */
/* ------------------------------------------------------------------ */
static int g_checks = 0;
static int g_fails  = 0;

static void ck_true(int cond, const char *what, int line)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL line %d: %s\n", line, what);
    }
}

static void ck_u(uint32 got, uint32 want, const char *what, int line)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL line %d: %s (got 0x%X want 0x%X)\n", line, what,
               (unsigned)got, (unsigned)want);
    }
}

static void ck_i(int32 got, int32 want, const char *what, int line)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL line %d: %s (got %d want %d)\n", line, what,
               (int)got, (int)want);
    }
}

#define CK(cond)          ck_true((cond) ? 1 : 0, #cond, __LINE__)
#define CK_U(got, want)   ck_u((uint32)(got), (uint32)(want), #got, __LINE__)
#define CK_I(got, want)   ck_i((int32)(got), (int32)(want), #got, __LINE__)

/* 宿主测试钩子的声明在 arkrtos_installer.h 的 ARKRTOS_INSTALLER_HOST_TEST 块里 */

#define FIXED_UNJUDGED  (-1)
#define FIXED_READY     (0)
#define FIXED_DONE      (1)
#define FIXED_UNUSABLE  (2)

/* ------------------------------------------------------------------ */
/* 被安装器依赖的内核桩                                                */
/* ------------------------------------------------------------------ */

/* --- 分区表 --- */
static arkrtos_partition_table_t g_pt;
arkrtos_partition_table_t *arkrtos_ptable_get(void) { return &g_pt; }

/* --- 布局表 --- */
static arkrtos_cfg_slot_t g_slot;
const arkrtos_cfg_slot_t *arkrtos_layout_slot(uint32 index)
{
    if(index != 0u)
    {
        return 0;
    }
    return &g_slot;
}

/* --- 任务 --- */
static int32 g_task_register_ret = 7;
static uint32 g_task_wait_calls = 0u;
static uint32 g_task_wait_ms = 0u;
static void (*g_task_entry)(void) = 0;
static uint32 g_task_stack_size = 0u;
static uint8  g_task_prio = 0u;
static uint32 g_task_period = 0u;

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom,
                            uint32 stack_size, uint8 priority, uint32 period_ms)
{
    g_task_entry = entry;
    g_task_stack_size = stack_size;
    g_task_prio = priority;
    g_task_period = period_ms;
    if(stack_bottom == 0)
    {
        return -1;
    }
    return g_task_register_ret;
}

void arkrtos_task_wait_internal(uint32 ms)
{
    g_task_wait_calls++;
    g_task_wait_ms += ms;
}

int32 arkrtos_current_task_id = 3;

/* --- 故障 --- */
static uint32 g_fault_calls = 0u;
static uint32 g_fault_last_type = 0u;
static int32  g_fault_last_task = 0;

void arkrtos_fault_record(uint32 type, int32 task_id)
{
    g_fault_calls++;
    g_fault_last_type = type;
    g_fault_last_task = task_id;
}

/* --- 日志 --- */
static uint32 g_log_emit_calls = 0u;

uint32 arkrtos_log_get_level(void) { return 0u; }   /* 低于 ERROR：宏内直接跳过 */

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level; (void)tag; (void)line; (void)msg;
    g_log_emit_calls++;
}

/* --- 设备框架 ---
 * 两个假设备：
 *   handle 0 = "ttyS0"：从脚本缓冲按字节喂，g_gap_at 处插入一段「读不到」；
 *   handle 1 = "fsimg"：把读写转给 arkrtos_dev_register 登记的驱动，
 *              于是安装器的文件垫片（file_open/read/write/close）走真实现。
 */
#define H_TTY   (0)
#define H_FILE  (1)

static uint8  g_in[8192];
static uint32 g_in_len = 0u;
static uint32 g_in_pos = 0u;
static uint32 g_gap_at = 0xFFFFFFFFu;   /* 在此偏移处开始一段空窗 */
static uint32 g_gap_left = 0u;

static uint8  g_out[256];
static uint32 g_out_len = 0u;

static arkrtos_dev_drv_t *g_reg_drv = 0;
static char g_reg_name[8];
static int32 g_dev_register_ret = 0;
static int32 g_open_tty_ret = H_TTY;    /* -1 = 打不开 */
static uint32 g_open_tty_calls = 0u;
static uint32 g_close_calls = 0u;
static arkrtos_dev_hdr_t g_file_obj;

int32 arkrtos_dev_register(const char *name, arkrtos_dev_drv_t *drv, uint32 dev_num)
{
    (void)dev_num;
    if(g_dev_register_ret != 0)
    {
        return g_dev_register_ret;
    }
    if((name == 0) || (drv == 0))
    {
        return -1;
    }
    (void)snprintf(g_reg_name, sizeof(g_reg_name), "%s", name);
    g_reg_drv = drv;
    return 0;
}

int32 arkrtos_dev_open_internal(const char *name, uint32 param)
{
    (void)param;
    if(name == 0)
    {
        return -1;
    }
    if(strcmp(name, "ttyS0") == 0)
    {
        g_open_tty_calls++;
        return g_open_tty_ret;
    }
    if(strcmp(name, "fsimg") == 0)
    {
        if(g_reg_drv == 0)
        {
            return -1;
        }
        return H_FILE;
    }
    return -1;
}

int32 arkrtos_dev_close_internal(int32 handle)
{
    g_close_calls++;
    if((handle == H_FILE) && (g_reg_drv != 0) && (g_reg_drv->drv_close != 0))
    {
        return g_reg_drv->drv_close(&g_file_obj);
    }
    return 0;
}

int32 arkrtos_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    if(pdata == 0 || len <= 0)
    {
        return -1;
    }
    if(handle == H_FILE)
    {
        if((g_reg_drv == 0) || (g_reg_drv->drv_read == 0))
        {
            return -1;
        }
        return g_reg_drv->drv_read(&g_file_obj, pdata, len);
    }
    if(handle != H_TTY)
    {
        return -1;
    }
    if(g_gap_left > 0u)
    {
        g_gap_left--;
        return -1;
    }
    if(g_gap_at < g_in_len && g_in_pos == g_gap_at)
    {
        g_gap_at = 0xFFFFFFFFu;
        return -1;      /* 这一段空窗由 g_gap_left 单独驱动 */
    }
    if(g_in_pos >= g_in_len)
    {
        return -1;
    }
    *pdata = g_in[g_in_pos++];
    return 1;
}

int32 arkrtos_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    if(pdata == 0 || len <= 0)
    {
        return -1;
    }
    if(handle == H_FILE)
    {
        if((g_reg_drv == 0) || (g_reg_drv->drv_write == 0))
        {
            return -1;
        }
        return g_reg_drv->drv_write(&g_file_obj, pdata, len);
    }
    if(g_out_len + (uint32)len <= sizeof(g_out))
    {
        memcpy(&g_out[g_out_len], pdata, (size_t)len);
        g_out_len += (uint32)len;
    }
    return len;
}

/* --- loader --- */
static int32 g_ci_ret = 0;
static uint32 g_ci_calls = 0u;
static const arkrtos_app_header_t *g_ci_last = 0;

static int32 g_load_ret = 4;
static uint32 g_load_calls = 0u;
static uint32 g_load_driver_calls = 0u;
static uint32 g_load_last_len = 0u;
static uint32 g_payload_bytes = 0u;      /* 打桩 loader 从设备里再读走这么多字节 */

static uint32 g_start_calls = 0u;
static uint32 g_start_last = 0xFFFFFFFFu;
static int32  g_stop_ret = 0;
static uint32 g_stop_calls = 0u;
static uint32 g_stop_last = 0xFFFFFFFFu;
static int32  g_hint = -1;
static uint32 g_hint_set_calls = 0u;
static int32  g_hint_last = -99;
static int32  g_clear_ret = 0;
static uint32 g_clear_calls = 0u;
static int32  g_clear_last = -99;
static int32  g_blank = 1;
static uint32 g_blank_calls = 0u;

static void load_consume(int32 dev, const arkrtos_app_header_t *p_hdr, uint32 image_len)
{
    uint8 buf[64];
    uint32 left = g_payload_bytes;

    g_load_calls++;
    g_load_last_len = image_len;
    g_ci_last = p_hdr;

    while(left > 0u)
    {
        uint32 want = (left > sizeof(buf)) ? (uint32)sizeof(buf) : left;
        int32 n = arkrtos_dev_read_internal(dev, buf, (int32)want);

        if(n <= 0)
        {
            break;
        }
        left -= (uint32)n;
    }

    /* 模拟 loader 的一个流程字节（ACK/NAK）：文件垫片必须把它丢掉并报接受，
     * 不能把它当文件内容写回去。串口路径不写，否则会污染 NAK 账目。 */
    if(dev == H_FILE)
    {
        uint8 flow = 0x06u;
        (void)arkrtos_dev_write_internal(dev, &flow, 1);
    }
}

int32 arkrtos_loader_check_install(const arkrtos_app_header_t *p_hdr)
{
    g_ci_calls++;
    g_ci_last = p_hdr;
    return g_ci_ret;
}

int32 arkrtos_loader_load_dev_hdr(int32 dev, const arkrtos_app_header_t *p_hdr,
                                  uint32 image_len)
{
    load_consume(dev, p_hdr, image_len);
    return g_load_ret;
}

int32 arkrtos_loader_load_driver_dev(int32 dev, const arkrtos_app_header_t *p_hdr,
                                     uint32 image_len)
{
    g_load_driver_calls++;
    load_consume(dev, p_hdr, image_len);
    return g_load_ret;
}

int32 arkrtos_loader_start(uint32 slot)
{
    g_start_calls++;
    g_start_last = slot;
    return 0;
}

int32 arkrtos_loader_stop(uint32 slot)
{
    g_stop_calls++;
    g_stop_last = slot;
    return g_stop_ret;
}

void arkrtos_loader_slot_hint_set(int32 index)
{
    g_hint_set_calls++;
    g_hint_last = index;
    g_hint = index;
}

int32 arkrtos_loader_slot_hint_get(void) { return g_hint; }

int32 arkrtos_loader_clear_fixed_slot(int32 index)
{
    g_clear_calls++;
    g_clear_last = index;
    return g_clear_ret;
}

int32 arkrtos_loader_fixed_slot_blank(int32 index)
{
    (void)index;
    g_blank_calls++;
    return g_blank;
}

/* --- 文件系统 --- */
static uint8  g_file[1024];
static uint32 g_file_len = 0u;
static uint32 g_file_pos = 0u;
static int    g_fs_mounted_ret = 0;
static int32  g_stat_ret = 0;
static uint32 g_stat_size = 0u;
static uint32 g_stat_isdir = 0u;
static int32  g_open_read_ret = 0;
static int32  g_read_chunk = 0;          /* 0 = 还剩多少给多少 */
static int32  g_read_ret_zero = 0;       /* 1 = 立刻报 0（短读） */
static int32  g_fs_last_err = -1;
static uint32 g_fs_close_calls = 0u;

uint8 arkrtos_fs_mounted(void) { return (uint8)g_fs_mounted_ret; }

int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_dir)
{
    (void)path;
    if(size == 0 || is_dir == 0)
    {
        return -1;
    }
    if(g_stat_ret != 0)
    {
        return -1;
    }
    *size = g_stat_size;
    *is_dir = g_stat_isdir;
    return 0;
}

int32 arkrtos_fs_open_read(const char *path)
{
    (void)path;
    g_file_pos = 0u;
    return g_open_read_ret;
}

int32 arkrtos_fs_read_next(uint8 *buf, uint32 max)
{
    uint32 left;
    uint32 n;

    if(g_read_ret_zero != 0)
    {
        return 0;
    }
    if(g_file_pos >= g_file_len)
    {
        return 0;
    }
    left = g_file_len - g_file_pos;
    n = (left > max) ? max : left;
    if((g_read_chunk > 0) && ((uint32)g_read_chunk < n))
    {
        n = (uint32)g_read_chunk;
    }
    memcpy(buf, &g_file[g_file_pos], (size_t)n);
    g_file_pos += n;
    return (int32)n;
}

int32 arkrtos_fs_close_read(void)
{
    g_fs_close_calls++;
    return 0;
}

int32 arkrtos_fs_last_error(void) { return g_fs_last_err; }

/* ------------------------------------------------------------------ */
/* 准备与复位                                                          */
/* ------------------------------------------------------------------ */
static void reset_all(void)
{
    memset(&g_pt, 0, sizeof(g_pt));
    memset(&g_slot, 0, sizeof(g_slot));
    g_slot.base = CHIP_FLASH_BASE + 0x10000u;
    g_slot.size = 0x4000u;

    g_in_len = 0u;
    g_in_pos = 0u;
    g_gap_at = 0xFFFFFFFFu;
    g_gap_left = 0u;
    memset(g_in, 0, sizeof(g_in));
    g_out_len = 0u;

    g_open_tty_calls = 0u;
    g_close_calls = 0u;
    g_dev_register_ret = 0;
    g_open_tty_ret = H_TTY;

    g_ci_ret = 0;
    g_ci_calls = 0u;
    g_ci_last = 0;
    g_load_ret = 4;
    g_load_calls = 0u;
    g_load_driver_calls = 0u;
    g_load_last_len = 0u;
    g_payload_bytes = 0u;
    g_start_calls = 0u;
    g_start_last = 0xFFFFFFFFu;
    g_stop_ret = 0;
    g_stop_calls = 0u;
    g_stop_last = 0xFFFFFFFFu;
    g_hint = -1;
    g_hint_set_calls = 0u;
    g_hint_last = -99;
    g_clear_ret = 0;
    g_clear_calls = 0u;
    g_clear_last = -99;
    g_blank = 1;
    g_blank_calls = 0u;

    g_fault_calls = 0u;
    g_fault_last_type = 0u;
    g_fault_last_task = 0;
    g_task_wait_calls = 0u;
    g_task_wait_ms = 0u;

    g_fs_mounted_ret = 0;
    g_stat_ret = 0;
    g_stat_size = 0u;
    g_stat_isdir = 0u;
    g_open_read_ret = 0;
    g_read_chunk = 0;
    g_read_ret_zero = 0;
    g_file_len = 0u;
    g_file_pos = 0u;
    g_fs_close_calls = 0u;

    arkrtos_installer_test_reset();
}

/* 组一个 256 字节镜像头；返回写到 buf 的字节数 */
static uint32 mk_hdr(uint8 *buf, uint32 type, uint32 flags, uint32 reloc,
                     uint32 image_size, uint32 version, uint32 image_id)
{
    arkrtos_app_header_t h;

    memset(&h, 0, sizeof(h));
    h.magic = ARKRTOS_APP_MAGIC;
    h.type = type;
    h.hw_compat_id = ARKRTOS_HW_COMPAT_ID;
    h.version = version;
    h.image_size = image_size;
    h.entry_offset = 8u;
    h.flags = flags;
    h.image_id = image_id;
    h.ram_size = 0x400u;
    h.reloc_offset = ARKRTOS_APP_RELOC_OFFSET;
    h.reloc_count = reloc;
    h.reloc_kind = ARKRTOS_APP_RELOC_KIND_CUR;
    h.payload_offset = ARKRTOS_APP_HEADER_SIZE + (reloc * ARKRTOS_APP_RELOC_SIZE);

    memcpy(buf, &h, sizeof(h));
    return (uint32)sizeof(h);
}

/* 往 tty 流里追加：三字节杂散 + 镜像头 + reloc 个 4 字节表项 */
static void feed_frame(uint32 flags, uint32 reloc, int junk)
{
    uint8 hdr[ARKRTOS_APP_HEADER_SIZE];
    uint32 n;
    uint32 i;

    g_in_pos = 0u;
    g_in_len = 0u;
    if(junk != 0)
    {
        g_in[g_in_len++] = 0x00u;
        g_in[g_in_len++] = 0x11u;
        g_in[g_in_len++] = 0x22u;
    }
    n = mk_hdr(hdr, ARKRTOS_APP_TYPE_APP, flags, reloc, 0x100u,
               0x01020304u, 0xA5A5A5A5u);
    memcpy(&g_in[g_in_len], hdr, n);
    g_in_len += n;
    for(i = 0u; i < reloc; i++)
    {
        g_in[g_in_len++] = (uint8)(0x10u + i);
        g_in[g_in_len++] = (uint8)(0x20u + i);
        g_in[g_in_len++] = 0u;
        g_in[g_in_len++] = 0u;
    }
}

/* ================================================================== */
/* 用例                                                               */
/* ================================================================== */

static void test_init(void)
{
    g_task_register_ret = 7;
    CK_I(arkrtos_installer_init(), 7);
    CK(g_task_entry != 0);
    CK_U(g_task_stack_size, INSTALLER_TASK_STACK_SIZE);
    CK_U(g_task_prio, INSTALLER_TASK_PRIORITY);
    CK_U(g_task_period, INSTALLER_TASK_PERIOD_MS);
}

static void test_run_once_param(void)
{
    CK_I(arkrtos_installer_run_once(-1, 10u), ARKRTOS_LOADER_ERR_PARAM);
    CK_U(g_open_tty_calls, 0u);
    CK_U(g_fault_calls, 0u);
}

/* 空线 + timeout 0：pump 撞空转上限 → 超时分支，收头进度被清、记故障 */
static void test_run_once_timeout(void)
{
    CK_I(arkrtos_installer_run_once(H_TTY, 0u), ARKRTOS_LOADER_ERR_PARAM);
    CK_U(g_fault_calls, 1u);
    CK_U(g_fault_last_type, (uint32)ARKRTOS_FAULT_INSTALLFAIL);
    CK_I(g_fault_last_task, arkrtos_current_task_id);
    CK_U(arkrtos_installer_test_got(), 0u);
    CK_U(g_load_calls, 0u);
}

/* 杂散字节 + 完整头：同步、落盘、按 flags 自启那个槽 */
static void test_run_once_commit_autostart(void)
{
    feed_frame(ARKRTOS_APP_FLAG_AUTOSTART, 0u, 1);
    CK_I(arkrtos_installer_run_once(H_TTY, 0u), 4);
    CK_U(g_load_calls, 1u);
    CK_U(g_load_driver_calls, 0u);
    CK_U(g_load_last_len, 0u);          /* 串口路径：长度交给镜像头 */
    CK(arkrtos_installer_test_hdr()->magic == ARKRTOS_APP_MAGIC);
    CK_U(arkrtos_installer_test_hdr()->image_id, 0xA5A5A5A5u);
    CK_U(g_pt.slot_autostart[4], 1u);
    CK_U(g_start_calls, 1u);
    CK_U(g_start_last, 4u);
    CK_U(arkrtos_installer_test_got(), 0u);
    CK_U(g_out_len, 0u);                /* 成功不写 NAK */
    CK_U(g_fault_calls, 0u);
}

/* 自启位清零：照样落盘，但不启动 */
static void test_run_once_no_autostart(void)
{
    feed_frame(0u, 0u, 0);
    CK_I(arkrtos_installer_run_once(H_TTY, 0u), 4);
    CK_U(g_load_calls, 1u);
    CK_U(g_pt.slot_autostart[4], 0u);
    CK_U(g_start_calls, 0u);
}

/* 驱动镜像走 driver 入口 */
static void test_run_once_driver(void)
{
    uint8 hdr[ARKRTOS_APP_HEADER_SIZE];

    g_in_pos = 0u;
    g_in_len = mk_hdr(hdr, ARKRTOS_APP_TYPE_DRIVER, ARKRTOS_APP_FLAG_AUTOSTART,
                      0u, 0x80u, 1u, 0x1111u);
    memcpy(g_in, hdr, g_in_len);

    CK_I(arkrtos_installer_run_once(H_TTY, 0u), 4);
    CK_U(g_load_driver_calls, 1u);
    CK_U(g_load_calls, 1u);             /* load_consume 里自增，用于对账 */
    CK_U(g_pt.slot_autostart[4], 1u);
}

/* 三条「头收全了但不该装」的拒绝：DUP / VERSION / 其它。
 * 每一条都必须 NAK、把这一帧的重定位表排掉、清收头进度、记故障。 */
static void refuse_common(const char *tag)
{
    (void)tag;
    CK_U(g_load_calls, 0u);
    CK_U(g_out_len, 1u);
    CK_U(g_out[0], 0x15u);
    CK_U(arkrtos_installer_test_got(), 0u);
    CK_U(g_fault_calls, 1u);
    CK_U(g_in_pos, 3u + ARKRTOS_APP_HEADER_SIZE + 8u);   /* 表被读走 */
}

static void test_run_once_refuse(void)
{
    g_ci_ret = ARKRTOS_LOADER_ERR_DUP;
    feed_frame(0u, 2u, 1);
    CK_I(arkrtos_installer_run_once(H_TTY, 0u), ARKRTOS_LOADER_ERR_DUP);
    refuse_common("dup");

    reset_all();
    g_ci_ret = ARKRTOS_LOADER_ERR_VERSION;
    feed_frame(0u, 2u, 1);
    CK_I(arkrtos_installer_run_once(H_TTY, 0u), ARKRTOS_LOADER_ERR_VERSION);
    refuse_common("version");

    reset_all();
    g_ci_ret = ARKRTOS_LOADER_ERR_COMPAT;
    feed_frame(0u, 2u, 1);
    CK_I(arkrtos_installer_run_once(H_TTY, 0u), ARKRTOS_LOADER_ERR_COMPAT);
    refuse_common("compat");
}

/* 重定位表没收全：第一阶段等到 300 ms 上限后才放弃，再走静默收尾 */
static void test_drain_table_timeout(void)
{
    g_ci_ret = ARKRTOS_LOADER_ERR_DUP;
    feed_frame(0u, 2u, 0);              /* 8 B 表，实际只到 3 B */
    g_in_len = ARKRTOS_APP_HEADER_SIZE + 3u;

    CK_I(arkrtos_installer_run_once(H_TTY, 0u), ARKRTOS_LOADER_ERR_DUP);
    CK_U(g_out_len, 1u);
    CK_U(g_out[0], 0x15u);
    CK(g_task_wait_calls >= 300u);      /* 第一阶段按字节计的等待上限 */
    CK_U(arkrtos_installer_test_got(), 0u);
}

/* 表项为 0，但帧尾还有两个字节迟到：它们必须在静默收尾里被读走 */
static void test_drain_quiet_slack(void)
{
    g_ci_ret = ARKRTOS_LOADER_ERR_DUP;
    feed_frame(0u, 0u, 0);
    g_in[g_in_len++] = 0xAAu;
    g_in[g_in_len++] = 0xBBu;

    CK_I(arkrtos_installer_run_once(H_TTY, 0u), ARKRTOS_LOADER_ERR_DUP);
    CK_U(g_in_pos, ARKRTOS_APP_HEADER_SIZE + 2u);
    CK_U(g_task_wait_calls, 5u);        /* 只有静默收尾 */
}

/* 身份通过但落盘失败：同样 NAK + 排空 + 故障；表项为 0 时排空只剩静默收尾 */
static void test_load_failure(void)
{
    g_load_ret = ARKRTOS_LOADER_ERR_FLASH;
    feed_frame(ARKRTOS_APP_FLAG_AUTOSTART, 0u, 0);
    CK_I(arkrtos_installer_run_once(H_TTY, 0u), ARKRTOS_LOADER_ERR_FLASH);
    CK_U(g_load_calls, 1u);
    CK_U(g_out_len, 1u);
    CK_U(g_out[0], 0x15u);
    CK_U(g_pt.slot_autostart[4], 0u);
    CK_U(g_start_calls, 0u);
    CK_U(g_fault_calls, 1u);
    CK_U(g_task_wait_calls, 5u);        /* drain 的静默收尾 */
}

/* 空转上限不是终点：已收字节保留，让出 CPU 后从原位置继续收。
 * 120 B 头 + 30000 次空读（跨过一次 20000 上限）+ 剩余 136 B。 */
static void test_idle_limit_resumes(void)
{
    uint8 hdr[ARKRTOS_APP_HEADER_SIZE];
    uint32 n = mk_hdr(hdr, ARKRTOS_APP_TYPE_APP, ARKRTOS_APP_FLAG_AUTOSTART,
                      0u, 0x40u, 1u, 0x2222u);

    memcpy(g_in, hdr, n);
    g_in_len = n;
    g_gap_at = 120u;
    g_gap_left = 30000u;

    CK_I(arkrtos_installer_run_once(H_TTY, 0xFFFFFFFFu), 4);
    CK_U(g_load_calls, 1u);
    CK(arkrtos_installer_test_hdr()->image_id == 0x2222u);
    CK(g_task_wait_calls >= 1u);
    CK_U(arkrtos_installer_test_got(), 0u);
}

/* FIXED 模式 run_once：没有 hint 就在读串口之前拒绝 */
static void test_run_once_fixed_no_hint(void)
{
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    CK_I(arkrtos_installer_run_once(H_TTY, 100u), ARKRTOS_LOADER_ERR_PARAM);
    CK_U(g_clear_calls, 0u);
    CK_U(g_load_calls, 0u);
    CK_U(g_in_pos, 0u);
    CK_U(g_fault_calls, 0u);
}

/* 有 hint 但清槽失败：必须报 FLASH，且不读串口 */
static void test_run_once_fixed_clear_fail(void)
{
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    g_hint = 2;
    g_clear_ret = -1;
    feed_frame(0u, 0u, 0);
    CK_I(arkrtos_installer_run_once(H_TTY, 100u), ARKRTOS_LOADER_ERR_FLASH);
    CK_U(g_clear_calls, 1u);
    CK_I(g_clear_last, 2);
    CK_U(g_in_pos, 0u);
    CK_U(g_load_calls, 0u);
}

/* 有 hint 且清槽成功：先清后收，正常落盘 */
static void test_run_once_fixed_ok(void)
{
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    g_hint = 2;
    feed_frame(ARKRTOS_APP_FLAG_AUTOSTART, 0u, 1);
    CK_I(arkrtos_installer_run_once(H_TTY, 100u), 4);
    CK_U(g_clear_calls, 1u);
    CK_I(g_clear_last, 2);
    CK_U(g_load_calls, 1u);
    CK_U(g_pt.slot_autostart[4], 1u);
}

/* 设备打不开：常驻任务这一轮什么都不做，下一轮再试 */
static void test_tick_open_fail(void)
{
    int32 dev = -1;

    g_open_tty_ret = -1;
    arkrtos_installer_tick(&dev);
    CK_I(dev, -1);
    CK_U(g_load_calls, 0u);
    CK_I(arkrtos_installer_fixed_state, FIXED_UNJUDGED);
}

/* AUTO 模式常驻任务：一次 tick 收完并提交，state 不进入 DONE */
static void test_tick_auto_commit(void)
{
    int32 dev = -1;

    feed_frame(0u, 0u, 1);
    arkrtos_installer_tick(&dev);
    CK_I(dev, H_TTY);
    CK_U(g_load_calls, 1u);
    /* AUTO 模式下 prepare 也返回 0，一次性状态因此落在 READY；
     * 这个状态只对 FIXED 的 frame_ok 有意义。 */
    CK_I(arkrtos_installer_fixed_state, FIXED_READY);
    CK_U(g_open_tty_calls, 1u);

    /* 第二次 tick 复用同一个句柄，不再重复打开 */
    g_in_pos = 0u;
    g_in_len = 0u;
    arkrtos_installer_tick(&dev);
    CK_U(g_open_tty_calls, 1u);
}

/* FIXED 模式 + 未配置 INSTALLER_FIXED_SLOT：一次性 prepare 判 UNUSABLE，
 * 之后任何帧都被 frame_ok 拒掉并排空 —— 不是边收边猜一个槽。 */
static void test_tick_fixed_unusable(void)
{
    int32 dev = -1;

    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    feed_frame(0u, 2u, 1);
    arkrtos_installer_tick(&dev);
    CK_I(arkrtos_installer_fixed_state, FIXED_UNUSABLE);
    CK_U(g_load_calls, 0u);
    CK_U(g_clear_calls, 0u);
    CK_U(g_out_len, 0u);                /* 已在这一步排空，不写 NAK */
    CK_U(arkrtos_installer_test_got(), 0u);
    CK_U(g_in_pos, 3u + ARKRTOS_APP_HEADER_SIZE + 8u);
}

#if (INSTALLER_FIXED_SLOT >= 0)
/* 配置了固定槽：一次 tick 里停止运行中的镜像、清槽、再收 */
static void test_tick_fixed_ready(void)
{
    int32 dev = -1;

    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.slot_max = 2u;
    g_pt.slot_type[0] = ARKRTOS_SLOT_FREE;
    g_pt.slot_type[1] = 1u;
    g_pt.slot_state[1] = ARKRTOS_APP_SLOT_RUNNING;
    g_pt.slot_base[1] = g_slot.base + 0x100u;
    g_pt.slot_size[1] = 0x200u;

    feed_frame(0u, 0u, 1);
    arkrtos_installer_tick(&dev);
    CK_I(arkrtos_installer_fixed_state, FIXED_READY);
    CK_U(g_stop_calls, 1u);
    CK_U(g_stop_last, 1u);
    CK_U(g_clear_calls, 1u);
    CK_I(g_clear_last, (int32)INSTALLER_FIXED_SLOT);
    CK_I(g_hint_last, (int32)INSTALLER_FIXED_SLOT);
    CK_U(g_load_calls, 1u);

    /* 装完之后进入 DONE；再来一帧被拒，不再二次擦写 */
    g_in_pos = 0u;
    g_in_len = 0u;
    feed_frame(0u, 0u, 0);
    arkrtos_installer_tick(&dev);
    CK_I(arkrtos_installer_fixed_state, FIXED_DONE);
    CK_U(g_clear_calls, 1u);
}
#endif

/* frame_ok 的四个判定：READY+空 → 放行；非空 → 拒；DONE → 拒；UNJUDGED → 拒 */
static void test_frame_ok_states(void)
{
    int32 dev = -1;

    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    arkrtos_installer_fixed_state = FIXED_READY;
    g_blank = 1;
    feed_frame(0u, 0u, 1);
    arkrtos_installer_tick(&dev);
    CK_U(g_load_calls, 1u);
    CK_U(g_blank_calls, 1u);

    /* 非空槽：拒绝帧，绝不中途擦 */
    reset_all();
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    arkrtos_installer_fixed_state = FIXED_READY;
    g_blank = 0;
    feed_frame(0u, 0u, 1);
    arkrtos_installer_tick(&dev);
    CK_U(g_load_calls, 0u);
    CK_U(g_clear_calls, 0u);

    /* 本上电已装过：拒绝第二帧 */
    reset_all();
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    arkrtos_installer_fixed_state = FIXED_DONE;
    feed_frame(0u, 0u, 1);
    arkrtos_installer_tick(&dev);
    CK_U(g_load_calls, 0u);

    /* 一次性判定还没跑：拒绝 */
    reset_all();
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    arkrtos_installer_fixed_state = FIXED_UNJUDGED;
    feed_frame(0u, 0u, 1);
    arkrtos_installer_tick(&dev);
    CK_U(g_load_calls, 0u);
}

/* ---- 文件入口 ---- */
static void test_from_file_gates(void)
{
    CK_I(arkrtos_installer_from_file(0), ARKRTOS_LOADER_ERR_PARAM);

    g_fs_mounted_ret = 0;
    CK_I(arkrtos_installer_from_file("/a.svcapp"), ARKRTOS_LOADER_ERR_PARAM);

    g_fs_mounted_ret = 1;
    g_stat_ret = -1;
    CK_I(arkrtos_installer_from_file("/a.svcapp"), ARKRTOS_LOADER_ERR_PARAM);
    CK_U(g_fault_calls, 1u);

    reset_all();
    g_fs_mounted_ret = 1;
    g_stat_isdir = 1u;
    CK_I(arkrtos_installer_from_file("/dir"), ARKRTOS_LOADER_ERR_PARAM);

    reset_all();
    g_fs_mounted_ret = 1;
    g_stat_size = ARKRTOS_APP_HEADER_SIZE - 1u;
    CK_I(arkrtos_installer_from_file("/small"), ARKRTOS_LOADER_ERR_SIZE);

    reset_all();
    g_fs_mounted_ret = 1;
    g_stat_size = IMAGE_POOL_USABLE_SIZE + 1u;
    CK_I(arkrtos_installer_from_file("/huge"), ARKRTOS_LOADER_ERR_SIZE);

    reset_all();
    g_fs_mounted_ret = 1;
    g_stat_size = 0x400u;
    g_open_read_ret = -1;
    CK_I(arkrtos_installer_from_file("/noread"), ARKRTOS_LOADER_ERR_PARAM);
}

/* 短读：头只读到一半就结束 */
static void test_from_file_short(void)
{
    uint8 hdr[ARKRTOS_APP_HEADER_SIZE];

    g_fs_mounted_ret = 1;
    g_stat_size = 0x400u;               /* 文件头声明够长 */
    g_file_len = 128u;                  /* 实际只给到 128 B */
    (void)mk_hdr(hdr, ARKRTOS_APP_TYPE_APP, 0u, 0u, 0x10u, 1u, 9u);
    memcpy(g_file, hdr, g_file_len);
    CK_I(arkrtos_installer_from_file("/short"), ARKRTOS_LOADER_ERR_SIZE);
    CK_U(g_fs_close_calls, 1u);
    CK_U(g_load_calls, 0u);
    CK_U(g_fault_calls, 1u);
}

/* 文件长度够但魔数不对：在这里就说清楚，不交给 loader 报「头损坏」 */
static void test_from_file_bad_magic(void)
{
    memset(g_file, 0, sizeof(g_file));
    g_fs_mounted_ret = 1;
    g_stat_size = 0x400u;
    g_file_len = 0x400u;
    CK_I(arkrtos_installer_from_file("/junk"), ARKRTOS_LOADER_ERR_MAGIC);
    CK_U(g_fs_close_calls, 1u);
    CK_U(g_load_calls, 0u);
    CK_U(g_fault_calls, 1u);
}

/* 文件垫片设备注册失败：报 PARAM 并关文件流 */
static void test_from_file_dev_fail(void)
{
    uint8 hdr[ARKRTOS_APP_HEADER_SIZE];

    g_dev_register_ret = -1;
    g_fs_mounted_ret = 1;
    g_stat_size = 0x400u;
    g_file_len = mk_hdr(hdr, ARKRTOS_APP_TYPE_APP, 0u, 0u, 0x10u, 1u, 0x33u);
    memcpy(g_file, hdr, g_file_len);
    CK_I(arkrtos_installer_from_file("/ok"), ARKRTOS_LOADER_ERR_PARAM);
    CK_U(g_fs_close_calls, 1u);
    CK_U(g_load_calls, 0u);
    CK_U(g_fault_calls, 1u);
}

/* 成功路径：载荷真的从文件流里读出来，收尾关设备 + 关文件 */
static void test_from_file_ok(void)
{
    uint8 hdr[ARKRTOS_APP_HEADER_SIZE];
    uint32 n = mk_hdr(hdr, ARKRTOS_APP_TYPE_APP, ARKRTOS_APP_FLAG_AUTOSTART,
                      0u, 0x20u, 1u, 0x44u);

    memset(g_file, 0x5A, sizeof(g_file));
    memcpy(g_file, hdr, n);
    g_fs_mounted_ret = 1;
    g_stat_size = n + 64u;              /* 头 + 64 B 负载 */
    g_file_len = n + 64u;
    g_payload_bytes = 64u;
    g_read_chunk = 17;                  /* 每次只给 17 B，逼出循环读 */

    CK_I(arkrtos_installer_from_file("/APP.svcapp"), 4);
    CK_U(g_load_calls, 1u);
    CK_U(g_load_last_len, n + 64u);     /* image_len = 文件真实长度 */
    CK(arkrtos_installer_test_hdr()->image_id == 0x44u);
    CK_U(g_pt.slot_autostart[4], 1u);
    CK_U(g_start_calls, 1u);
    CK_U(g_close_calls, 1u);            /* 设备关闭 */
    CK_U(g_fs_close_calls, 1u);         /* 文件流关闭 */
    CK_U(g_fault_calls, 0u);

    /* 垫片的 open/close 两个入口（真机上由设备框架调用）直接走一遍 */
    CK(g_reg_drv != 0);
    {
        arkrtos_dev_hdr_t *obj = g_reg_drv->drv_open(0u, 0u);
        CK(obj != 0);                   /* 句柄只作占位，读写靠文件流定位 */
        CK_I(g_reg_drv->drv_close(obj), 0);
    }
}

/* 文件垫片自身的参数闸门与「流程字节丢弃」 */
static void test_file_shim(void)
{
    uint8 buf[4] = { 1u, 2u, 3u, 4u };

    CK_I(arkrtos_installer_test_file_read(0, 4), -1);
    CK_I(arkrtos_installer_test_file_read(buf, 0), -1);
    CK_I(arkrtos_installer_test_file_write(buf, 9), 9);

    g_file_len = 4u;
    g_file_pos = 0u;
    memcpy(g_file, buf, 4);
    CK_I(arkrtos_installer_test_file_read(buf, 4), 4);
    CK_I(arkrtos_installer_test_file_read(buf, 4), 0);
}

int main(void)
{
    reset_all();
    test_init();

    reset_all();
    test_run_once_param();

    reset_all();
    test_run_once_timeout();

    reset_all();
    test_run_once_commit_autostart();

    reset_all();
    test_run_once_no_autostart();

    reset_all();
    test_run_once_driver();

    reset_all();
    test_run_once_refuse();

    reset_all();
    test_drain_table_timeout();

    reset_all();
    test_drain_quiet_slack();

    reset_all();
    test_load_failure();

    reset_all();
    test_idle_limit_resumes();

    reset_all();
    test_run_once_fixed_no_hint();

    reset_all();
    test_run_once_fixed_clear_fail();

    reset_all();
    test_run_once_fixed_ok();

    reset_all();
    test_tick_open_fail();

    reset_all();
    test_tick_auto_commit();

    reset_all();
    test_tick_fixed_unusable();

#if (INSTALLER_FIXED_SLOT >= 0)
    reset_all();
    test_tick_fixed_ready();
#endif

    reset_all();
    test_frame_ok_states();

    reset_all();
    test_from_file_gates();

    reset_all();
    test_from_file_short();

    reset_all();
    test_from_file_bad_magic();

    reset_all();
    test_from_file_dev_fail();

    reset_all();
    test_from_file_ok();

    reset_all();
    test_file_shim();

    printf("INSTALLER-HOST OK  %d checks\n", g_checks);
    if(g_fails != 0)
    {
        printf("INSTALLER-HOST FAILED  %d of %d\n", g_fails, g_checks);
        return 1;
    }
    return 0;
}
