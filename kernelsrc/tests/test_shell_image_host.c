/**
* @file test_shell_image_host.c
* @brief Host-side unit test for kernelsrc/shell/arkrtos_shell_image.c.
*
* What this proves: the four command families in that file do what they say on
* the host, with every refusal path executed:
*   - "info" prints the layout, the pool census and an address-ordered pool map
*     that skips FREE records, zero-based records and everything below the last
*     printed base (and says "(pool is empty)" when nothing printed at all);
*   - "app" lists only APP records, and start / stop / uninstall / install each
*     name their own failure (usage, loader error, unmanaged raw image, missing
*     file system) instead of sharing one "failed" line;
*   - "app swap" (compile-time switch) reports each refusal separately:
*     ERR_STATE, SWAP_ERR_LAYOUT, ERR_BUSY;
*   - "drv" has the same shape over DRIVER records;
*   - "install" refuses a bare command in fixed-slot mode, refuses to erase the
*     flash of a slot that is still executing (and stops that image first, or
*     refuses outright when the stop fails), then drains the console once the
*     one-shot window is over.
*
* What it does NOT prove:
*   - that any of these genuinely start, stop or install an image: every loader
*     and installer entry point here is a programmable return code;
*   - that the printed addresses come from a real partition table: the table is
*     a fixture struct, and arkrtos_ptable_stats() is a stub;
*   - that the console drain ends for the right reason on a real UART: the read
*     shape is "N reads of 32 bytes, then 0";
*   - the apprun / flashapp branches: qemu_loongarch64 builds with
*     ARKRTOS_USE_APPRUN=0 and ARKRTOS_USE_FLASHAPP=0 (no apprun window and no
*     flash app area exist in that layout), so those two blocks are not
*     compiled into the module measured here. swap is compiled in via
*     -DARKRTOS_USE_TASK_SWAP=1.
*
* The fixture values are arbitrary but distinctive, so a swapped field is
* caught rather than passing by coincidence.
*
* @author xw
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_shell_internal.h"

/* ------------------------------------------------------------ assertions */

static int g_checks;
static int g_failed;

#define CK(cond)                                                       \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            printf("FAIL line %d: %s\n", __LINE__, #cond);             \
        }                                                              \
    } while (0)

#define CK_I(got, want)                                                \
    do {                                                               \
        long g_ = (long)(got);                                         \
        long w_ = (long)(want);                                        \
        g_checks++;                                                    \
        if (g_ != w_) {                                                \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got %ld want %ld)\n",            \
                   __LINE__, #got, g_, w_);                            \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------ capture */

static char g_out[16384];
static size_t g_out_len;

static void cap_reset(void)
{
    g_out[0] = '\0';
    g_out_len = 0u;
}

static void cap_add(const char *s, size_t n)
{
    size_t room = sizeof(g_out) - 1u - g_out_len;

    if (n > room) {
        n = room;
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
    g_out[g_out_len] = '\0';
}

static int out_has(const char *needle)
{
    return strstr(g_out, needle) != NULL;
}

static int out_count(const char *needle)
{
    const char *p = g_out;
    size_t n = strlen(needle);
    int c = 0;

    if (n == 0u) {
        return 0;
    }
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += n;
    }
    return c;
}

static int out_before(const char *a, const char *b)
{
    const char *pa = strstr(g_out, a);
    const char *pb = strstr(g_out, b);

    return (pa != NULL) && (pb != NULL) && (pa < pb);
}

/* 可写副本：被测接口收 char *，而字面量是 const char[]（-Wwrite-strings）。 */
static char *W(const char *s)
{
    static char bufs[8][48];
    static unsigned idx;
    char *b = bufs[idx % 8u];
    size_t n = strlen(s);

    idx++;
    if (n >= sizeof(bufs[0])) {
        n = sizeof(bufs[0]) - 1u;
    }
    memcpy(b, s, n);
    b[n] = '\0';
    return b;
}

/* ------------------------------------------------------------ stubs: out */

void ark_shell_printf(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        cap_add(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1u);
    }
}

void sh_out(const char *s)
{
    cap_add(s, strlen(s));
}

const char *slot_type_name(uint32 t)
{
    if (t == ARKRTOS_SLOT_APP) {
        return "APP-S";
    }
    if (t == ARKRTOS_SLOT_DRIVER) {
        return "DRV-S";
    }
    return "FREE-S";
}

const char *slot_state_name(uint32 st)
{
    if (st == ARKRTOS_APP_SLOT_RUNNING) {
        return "RUN-S";
    }
    if (st == ARKRTOS_APP_SLOT_LOADED) {
        return "LOAD-S";
    }
    return "OTHER-S";
}

/* 只认十进制数字，别的一律拒绝（被测代码用它区分「槽号」与「子命令」）。 */
int parse_u32(const char *s, uint32 *out)
{
    uint32 v = 0u;

    if ((s == NULL) || (out == NULL) || (*s == '\0')) {
        return -1;
    }
    while (*s != '\0') {
        if ((*s < '0') || (*s > '9')) {
            return -1;
        }
        v = (v * 10u) + (uint32)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

/* ------------------------------------------------------------ stubs: kernel */

static arkrtos_partition_table_t g_pt;
static uint32 g_used;
static uint32 g_total;
static uint32 g_frag;
static uint32 g_tick;

int32 arkrtos_task_count;

arkrtos_partition_table_t *arkrtos_ptable_get(void)
{
    return &g_pt;
}

int32 arkrtos_ptable_stats(uint32 *p_used, uint32 *p_total, uint32 *p_frag)
{
    if (p_used != NULL) {
        *p_used = g_used;
    }
    if (p_total != NULL) {
        *p_total = g_total;
    }
    if (p_frag != NULL) {
        *p_frag = g_frag;
    }
    return 0;
}

uint32 arkrtos_kernel_get_time(void)
{
    return g_tick;
}

uint32 arkrtos_crash_disabled(uint32 slot)
{
    return slot + 5u;
}

const char *arkrtos_crash_reason_name(uint32 reason)
{
    if (reason == 5u) {
        return "HELD-A";
    }
    if (reason == 6u) {
        return "HELD-B";
    }
    return "HELD-X";
}

/* ------------------------------------------------------------ stubs: loader */

static int32 g_start_rc;
static int32 g_stop_rc;
static int32 g_drv_start_rc;
static int32 g_drv_stop_rc;
static int32 g_uninstall_rc;
static int32 g_swap_rc;
static int32 g_clear_rc;
static int32 g_hint;
static uint32 g_last_slot;
static uint32 g_last_old;
static uint32 g_last_new;

int32 arkrtos_loader_start_manual(uint32 slot)
{
    g_last_slot = slot;
    return g_start_rc;
}

int32 arkrtos_loader_start_driver_manual(uint32 slot)
{
    g_last_slot = slot;
    return g_drv_start_rc;
}

int32 arkrtos_loader_stop(uint32 slot)
{
    g_last_slot = slot;
    return g_stop_rc;
}

int32 arkrtos_loader_stop_driver_slot(uint32 slot)
{
    g_last_slot = slot;
    return g_drv_stop_rc;
}

int32 arkrtos_loader_uninstall(uint32 slot)
{
    g_last_slot = slot;
    return g_uninstall_rc;
}

int32 arkrtos_loader_swap(uint32 old_slot, uint32 new_slot)
{
    g_last_old = old_slot;
    g_last_new = new_slot;
    return g_swap_rc;
}

void arkrtos_loader_slot_hint_set(int32 index)
{
    g_hint = index;
}

int32 arkrtos_loader_clear_fixed_slot(int32 index)
{
    g_hint = -100;
    g_last_slot = (index < 0) ? 0xFFFFFFFFu : (uint32)index;
    return g_clear_rc;
}

/* ------------------------------------------------------------ stubs: layout */

#define TS_CFG_SLOTS 2u

static arkrtos_cfg_slot_t g_cfg_slots[TS_CFG_SLOTS];
static uint32 g_cfg_count;
static int g_cfg_valid;

uint32 arkrtos_layout_slot_count(void)
{
    return g_cfg_count;
}

const arkrtos_cfg_slot_t *arkrtos_layout_slot(uint32 index)
{
    if ((g_cfg_valid == 0) || (index >= g_cfg_count)) {
        return 0;
    }
    return &g_cfg_slots[index];
}

/* ------------------------------------------------------------ stubs: installer / dev */

static int32 g_run_once_rc;
static int32 g_from_file_rc;
static uint32 g_run_once_dev;
static uint32 g_run_once_timeout;
static const char *g_from_file_path;
static int32 g_sink_reads;
static int32 g_sink_calls;
static int32 g_uart_handle;
static uint8 g_fs_mounted;

int32 arkrtos_shell_uart_open(void)
{
    return g_uart_handle;
}

int32 arkrtos_installer_run_once(int32 dev, uint32 timeout_ms)
{
    g_run_once_dev = (uint32)dev;
    g_run_once_timeout = timeout_ms;
    return g_run_once_rc;
}

int32 arkrtos_installer_from_file(const char *path)
{
    g_from_file_path = path;
    return g_from_file_rc;
}

int32 arkrtos_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    (void)handle;
    (void)pdata;
    (void)len;
    g_sink_calls++;
    if (g_sink_reads > 0) {
        g_sink_reads--;
        return 32;
    }
    return 0;
}

uint8 arkrtos_fs_mounted(void)
{
    return g_fs_mounted;
}

/* ------------------------------------------------------------ fixture */

static void pt_reset(uint32 slot_max)
{
    memset(&g_pt, 0, sizeof(g_pt));
    memset(g_cfg_slots, 0, sizeof(g_cfg_slots));

    g_pt.version = 6u;
    g_pt.hw_compat_id = 0xA1B2C3D4u;
    g_pt.kernel_base = 0x10220000u;
    g_pt.kernel_size = 128u * 1024u;
    g_pt.kernel_ram_base = 0x33440000u;
    g_pt.kernel_ram_size = 64u * 1024u;
    g_pt.pool_base = 0x55660000u;
    g_pt.pool_size = 256u * 1024u;
    g_pt.pool_sector = 4u * 1024u;
    g_pt.pool_units = 64u;
    g_pt.pool_alloc_unit = 1024u;
    g_pt.pool_reserve = 2u;
    g_pt.slot_max = slot_max;
    g_pt.layout_mode = 0u;
    g_pt.image_ram_base = 0x77880000u;
    g_pt.image_ram_total = 32u * 1024u;
    g_pt.image_ram_min_block = 1024u;
    g_pt.image_ram_max_block = 16u * 1024u;

    g_used = 8u * 1024u;
    g_total = 256u * 1024u;
    g_frag = 2u;
    g_tick = 7u;
    arkrtos_task_count = 3;

    g_start_rc = 0;
    g_stop_rc = 0;
    g_drv_start_rc = 0;
    g_drv_stop_rc = 0;
    g_uninstall_rc = 0;
    g_swap_rc = 0;
    g_clear_rc = 0;
    g_hint = 12345;
    g_last_slot = 0xFFFFFFFFu;
    g_last_old = 0xFFFFFFFFu;
    g_last_new = 0xFFFFFFFFu;

    g_cfg_count = TS_CFG_SLOTS;
    g_cfg_valid = 1;
    /* 覆盖下标 1 的驱动（base 0x11200000，RUNNING）：install 的清槽必须先停它。 */
    g_cfg_slots[0].base = 0x11200000u;
    /* 0x11200000..0x11230000 同时罩住下标 1（RUNNING）与下标 0/2（非 RUNNING），
     * 于是 install 的清槽循环里「重叠且要停」和「重叠但不在跑」两条都走到。 */
    g_cfg_slots[0].size = 0x30000u;
    g_cfg_slots[1].base = 0x11440000u;
    g_cfg_slots[1].size = 8192u;

    g_run_once_rc = 3;
    g_from_file_rc = 2;
    g_run_once_dev = 0u;
    g_run_once_timeout = 0u;
    g_from_file_path = NULL;
    g_sink_reads = 0;
    g_sink_calls = 0;
    g_uart_handle = 9;
    g_fs_mounted = 0u;
}

/* 三条 App 记录 + 一条空记录，故意让地址顺序与下标顺序不一致。 */
static void slots_mixed(void)
{
    g_pt.slot_type[0] = ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0] = 0x11220000u;
    g_pt.slot_size[0] = 1024u;
    g_pt.slot_ram_base[0] = 0x33440000u;
    g_pt.slot_entry[0] = 0x55660000u;
    g_pt.slot_autostart[0] = 1u;
    g_pt.slot_task_id[0] = 7u;
    g_pt.slot_crash_cnt[0] = 2u;

    g_pt.slot_type[1] = ARKRTOS_SLOT_DRIVER;
    g_pt.slot_state[1] = ARKRTOS_APP_SLOT_RUNNING;
    g_pt.slot_base[1] = 0x11200000u;
    g_pt.slot_size[1] = 2048u;
    g_pt.slot_ram_base[1] = 0x33441000u;
    g_pt.slot_entry[1] = 0x55661000u;
    g_pt.slot_autostart[1] = 0u;
    g_pt.slot_task_id[1] = 0u;
    g_pt.slot_crash_cnt[1] = 0u;

    g_pt.slot_type[2] = ARKRTOS_SLOT_APP;
    g_pt.slot_state[2] = ARKRTOS_APP_SLOT_EMPTY;
    g_pt.slot_base[2] = 0x11250000u;
    g_pt.slot_size[2] = 512u;
    g_pt.slot_ram_base[2] = 0u;
    g_pt.slot_entry[2] = 0u;

    g_pt.slot_type[3] = ARKRTOS_SLOT_FREE;
    g_pt.slot_base[3] = 0x11280000u;
}

/* 统一入口：argc 由给了几个参数决定；a1 为 NULL 时 argv 只有命令名。 */
typedef int (*cmd_fn_t)(int argc, char *argv[]);

static int call_cmd(cmd_fn_t fn, const char *a1, const char *a2,
                    const char *a3, const char *a4)
{
    char *av[5];
    int argc = 1;

    av[0] = W("cmd");
    if (a1 != NULL) {
        av[argc] = W(a1);
        argc++;
    }
    if ((a1 != NULL) && (a2 != NULL)) {
        av[argc] = W(a2);
        argc++;
    }
    if ((a2 != NULL) && (a3 != NULL)) {
        av[argc] = W(a3);
        argc++;
    }
    if ((a3 != NULL) && (a4 != NULL)) {
        av[argc] = W(a4);
        argc++;
    }
    cap_reset();
    return fn(argc, av);
}

/* ------------------------------------------------------------ info */

static void t_info_layout_and_map(void)
{
    int rc;

    pt_reset(4u);
    slots_mixed();

    rc = call_cmd(cmd_info, NULL, NULL, NULL, NULL);

    CK_I(rc, 0);
    CK(out_has("Ark RTOS kernel shell"));
    CK(out_has("version   : "));
    CK(out_has("partition : ABI v6, hw 0xA1B2C3D4"));
    CK(out_has("kernel    : flash 0x10220000 +128K, ram 0x33440000 +64K"));
    CK(out_has("pool      : 0x55660000 +256K (64 sectors of 4K), unit 1024B, reserve 2"));
    CK(out_has("pool used : 8K of 256K, free 248K, fragments 2"));
    CK(out_has("image ram : 0x77880000 +32K, block 1024B..16K, 4 slots"));
    CK(out_has("tasks     : 3 used / "));
    CK(out_has("tick      : 7 ms, period "));

    CK(out_has("pool map (address order)"));
    /* 2048 = 0x11200000 那条（下标 1）排在最前，说明按地址不按下标。 */
    CK(out_has("  0x11200000 +2048 B  DRV-S RUN-S     slot 1"));
    CK(out_has("  0x11220000 +1024 B  APP-S LOAD-S    slot 0"));
    CK(out_has("  0x11250000 +512  B  APP-S OTHER-S   slot 2"));
    CK(out_before("0x11200000", "0x11220000"));
    CK(out_before("0x11220000", "0x11250000"));
    /* FREE 记录与 base==0 的记录都不出现在地图里。 */
    CK(!out_has("0x11280000"));
    CK(out_count("slot ") == 3);
}

static void t_info_empty_pool(void)
{
    pt_reset(2u);
    g_pt.slot_type[0] = ARKRTOS_SLOT_FREE;
    g_pt.slot_base[0] = 0x11290000u;
    g_pt.slot_type[1] = ARKRTOS_SLOT_APP;
    g_pt.slot_base[1] = 0u;

    CK_I(call_cmd(cmd_info, NULL, NULL, NULL, NULL), 0);
    CK(out_has("  (pool is empty)"));
    CK(!out_has("0x11290000"));
}

/* ------------------------------------------------------------ app */

static void t_app_list(void)
{
    pt_reset(4u);
    slots_mixed();

    CK_I(call_cmd(cmd_app, "list", NULL, NULL, NULL), 0);
    CK(out_has("id  type  state     auto  task  crash  held"));
    /* 下标 0：autostart=yes，task 7，crash 2，held 来自 crash_reason_name(5)。 */
    CK(out_has(" 0   APP-S  LOAD-S    yes   7     2      HELD-A     0x11220000  1024"));
    /* 下标 2：空槽位也要列出来（记录还被占着）。 */
    CK(out_has(" 2   APP-S  OTHER-S   no    0     0      HELD-X     0x11250000  512"));
    CK(out_count("APP-S") == 2);
    CK(!out_has("DRV-S"));
}

static void t_app_usage(void)
{
    pt_reset(4u);
    slots_mixed();

    CK_I(call_cmd(cmd_app, "bogus", NULL, NULL, NULL), -1);
    CK(out_has("  app swap <running slot> <new slot>: seamless switch"));
    CK(out_has("usage: app [list | install <path> [slot] | start <slot> |"
               " stop <slot> | uninstall <slot>]"));
}

static void t_app_start(void)
{
    pt_reset(4u);
    slots_mixed();

    CK_I(call_cmd(cmd_app, "start", NULL, NULL, NULL), -1);
    CK(out_has("usage: app start <slot>"));

    CK_I(call_cmd(cmd_app, "start", "zz", NULL, NULL), -1);
    CK(out_has("usage: app start <slot>"));

    /* 7 越界；1 是驱动记录，不是 App。 */
    CK_I(call_cmd(cmd_app, "start", "7", NULL, NULL), -1);
    CK(out_has("usage: app start <slot>"));

    CK_I(call_cmd(cmd_app, "start", "1", NULL, NULL), -1);
    CK(out_has("usage: app start <slot>"));

    g_start_rc = 5;
    CK_I(call_cmd(cmd_app, "start", "0", NULL, NULL), 0);
    CK(out_has("app: started"));
    CK_I(g_last_slot, 0);

    g_start_rc = 0;
    CK_I(call_cmd(cmd_app, "start", "2", NULL, NULL), 0);
    CK(out_has("app: start failed (see 'crash' for a held slot; state in 'app list')"));
    CK_I(g_last_slot, 2);
}

static void t_app_stop(void)
{
    pt_reset(4u);
    slots_mixed();

    CK_I(call_cmd(cmd_app, "stop", "1", NULL, NULL), -1);
    CK(out_has("usage: app stop <slot>"));

    g_stop_rc = 0;
    CK_I(call_cmd(cmd_app, "stop", "0", NULL, NULL), 0);
    CK(out_has("app: stopped (image kept in flash)"));
    CK_I(g_last_slot, 0);

    g_stop_rc = -9;
    CK_I(call_cmd(cmd_app, "stop", "2", NULL, NULL), 0);
    CK(out_has("app: stop failed (slot must be LOADED/RUNNING)"));
}

static void t_app_uninstall(void)
{
    pt_reset(4u);
    slots_mixed();

    CK_I(call_cmd(cmd_app, "uninstall", "1", NULL, NULL), -1);
    CK(out_has("usage: app uninstall <slot>"));

    g_uninstall_rc = 1;
    CK_I(call_cmd(cmd_app, "uninstall", "2", NULL, NULL), 0);
    CK(out_has("app: uninstalled (image invalidated, its sector was erased)"));

    g_uninstall_rc = 0;
    CK_I(call_cmd(cmd_app, "uninstall", "2", NULL, NULL), 0);
    CK(out_has("app: uninstalled (image invalidated; space returns once its sector is free)"));

    g_uninstall_rc = -5;
    CK_I(call_cmd(cmd_app, "uninstall", "2", NULL, NULL), 0);
    CK(out_has("app: uninstall failed (raw images are not managed here)"));
}

static void t_app_install_errors(void)
{
    pt_reset(4u);
    slots_mixed();

    CK_I(call_cmd(cmd_app, "install", NULL, NULL, NULL), -1);
    CK(out_has("usage: app install <path> [slot]"));

    /* 自动选址模式给槽号：直接拒绝，而不是猜一个落点。 */
    g_hint = 12345;
    CK_I(call_cmd(cmd_app, "install", "f.svcapp", "1", NULL), -1);
    CK(out_has("(a slot is only valid in fixed-slot mode)"));
    CK_I(g_hint, 12345);

    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;

    CK_I(call_cmd(cmd_app, "install", "f.svcapp", "x", NULL), -1);
    CK(out_has("(a slot is only valid in fixed-slot mode)"));

    CK_I(call_cmd(cmd_app, "install", "f.svcapp", "5", NULL), -1);
    CK(out_has("(a slot is only valid in fixed-slot mode)"));

    g_cfg_valid = 0;
    CK_I(call_cmd(cmd_app, "install", "f.svcapp", "0", NULL), -1);
    CK(out_has("app install: no such slot"));
}

static void t_app_install_fixed_ok_no_fs(void)
{
    pt_reset(4u);
    slots_mixed();
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;

    /* 三参数 + 固定槽位模式：提示清空（走 else 分支），然后卡在文件系统。 */
    CK_I(call_cmd(cmd_app, "install", "f.svcapp", NULL, NULL), -1);
    CK_I(g_hint, -1);
    CK(out_has("app install: no file system mounted (try 'fs mount')"));

    g_hint = 12345;
    CK_I(call_cmd(cmd_app, "install", "f.svcapp", "1", NULL), -1);
    CK_I(g_hint, 1);
    CK(out_has("app install: fixed slot 1 -> 0x11440000"));
}

static void t_app_install_from_file(void)
{
    pt_reset(4u);
    slots_mixed();
    g_fs_mounted = 1u;

    g_from_file_rc = 2;
    CK_I(call_cmd(cmd_app, "install", "f.svcapp", NULL, NULL), 0);
    CK(out_has("\r\napp install: f.svcapp\r\n"));
    CK(out_has("app install: ok, slot 2"));
    CK(g_from_file_path != NULL);
    CK_I((int)strcmp(g_from_file_path, "f.svcapp"), 0);
    CK_I(g_hint, -1);

    g_from_file_rc = -4;
    CK_I(call_cmd(cmd_app, "install", "g.svcapp", NULL, NULL), 0);
    CK(out_has("app install: failed (see 'fault'; check the path with 'fs ls')"));

    /* 固定槽位模式下成功安装也把槽号提示写回去。 */
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    g_from_file_rc = 1;
    g_hint = 12345;
    CK_I(call_cmd(cmd_app, "install", "h.svcapp", "0", NULL), 0);
    CK_I(g_hint, 0);
    CK(out_has("app install: fixed slot 0 -> 0x11200000"));
    CK(out_has("app install: ok, slot 1"));
}

#if (ARKRTOS_USE_TASK_SWAP == 1)
static void t_app_swap(void)
{
    pt_reset(4u);
    slots_mixed();

    CK_I(call_cmd(cmd_app, "swap", NULL, NULL, NULL), -1);
    CK(out_has("usage: app swap <running slot> <new slot>"));

    CK_I(call_cmd(cmd_app, "swap", "0", "x", NULL), -1);
    CK(out_has("usage: app swap <running slot> <new slot>"));

    g_swap_rc = 0;
    CK_I(call_cmd(cmd_app, "swap", "0", "2", NULL), 0);
    CK(out_has("app swap: slot 0 -> 2 armed; it switches at the task's next syscall"));
    CK(out_has("slot 0 keeps its image for rollback"));
    CK_I(g_last_old, 0);
    CK_I(g_last_new, 2);

    g_swap_rc = ARKRTOS_LOADER_ERR_STATE;
    CK_I(call_cmd(cmd_app, "swap", "0", "2", NULL), 1);
    CK(out_has("app swap: refused, rc=-9"));
    CK(out_has("the old slot must be RUNNING and the new one LOADED"));

    g_swap_rc = ARKRTOS_SWAP_ERR_LAYOUT;
    CK_I(call_cmd(cmd_app, "swap", "0", "2", NULL), 1);
    CK(out_has("app swap: refused, rc=-3"));
    CK(out_has("the two slots need the same RAM block size and the same slot type"));

    g_swap_rc = ARKRTOS_LOADER_ERR_BUSY;
    CK_I(call_cmd(cmd_app, "swap", "0", "2", NULL), 1);
    CK(out_has("app swap: refused, rc=-15"));
    CK(out_has("another swap is already in flight"));
}
#endif /* ARKRTOS_USE_TASK_SWAP */

/* ------------------------------------------------------------ drv */

static void t_drv_list_and_usage(void)
{
    pt_reset(4u);
    slots_mixed();

    CK_I(call_cmd(cmd_drv, NULL, NULL, NULL, NULL), 0);
    CK(out_has("id  type  state     auto  task  crash  held"));
    CK(out_has(" 1   DRV-S  RUN-S     no    0     0      HELD-B     0x11200000  2048"));
    CK(out_count("DRV-S") == 1);
    CK(!out_has("APP-S"));

    CK_I(call_cmd(cmd_drv, "bogus", NULL, NULL, NULL), -1);
    CK(out_has("usage: drv [list | start <slot> | stop <slot> | uninstall <slot>]"));
}

static void t_drv_start_stop_uninstall(void)
{
    pt_reset(4u);
    slots_mixed();

    CK_I(call_cmd(cmd_drv, "start", "0", NULL, NULL), -1);
    CK(out_has("usage: drv start <slot>"));

    g_drv_start_rc = 4;
    CK_I(call_cmd(cmd_drv, "start", "1", NULL, NULL), 0);
    CK(out_has("drv: started"));
    CK_I(g_last_slot, 1);

    g_drv_start_rc = -9;
    CK_I(call_cmd(cmd_drv, "start", "1", NULL, NULL), 0);
    CK(out_has("drv: start failed (see 'crash' for a held slot; state in 'drv list')"));

    CK_I(call_cmd(cmd_drv, "stop", "0", NULL, NULL), -1);
    CK(out_has("usage: drv stop <slot>"));

    g_drv_stop_rc = 0;
    CK_I(call_cmd(cmd_drv, "stop", "1", NULL, NULL), 0);
    CK(out_has("drv: stopped (image kept in flash)"));

    g_drv_stop_rc = -9;
    CK_I(call_cmd(cmd_drv, "stop", "1", NULL, NULL), 0);
    CK(out_has("drv: stop failed (slot must be LOADED/RUNNING)"));

    CK_I(call_cmd(cmd_drv, "uninstall", "0", NULL, NULL), -1);
    CK(out_has("usage: drv uninstall <slot>"));

    g_uninstall_rc = 1;
    CK_I(call_cmd(cmd_drv, "uninstall", "1", NULL, NULL), 0);
    CK(out_has("drv: uninstalled (image invalidated, its sector was erased)"));

    g_uninstall_rc = 0;
    CK_I(call_cmd(cmd_drv, "uninstall", "1", NULL, NULL), 0);
    CK(out_has("drv: uninstalled (image invalidated; space returns once its sector is free)"));

    g_uninstall_rc = -3;
    CK_I(call_cmd(cmd_drv, "uninstall", "1", NULL, NULL), 0);
    CK(out_has("drv: uninstall failed (raw images are not managed here)"));
}

/* ------------------------------------------------------------ install */

static void t_install_usage(void)
{
    pt_reset(4u);
    slots_mixed();

    /* 自动选址模式下不带参数：清空提示，然后进入等待窗口。 */
    g_hint = 12345;
    CK_I(call_cmd(cmd_install, NULL, NULL, NULL, NULL), 0);
    CK_I(g_hint, -1);
    CK(out_has("install: waiting for one .svcapp image on ttyS0"));
    CK(out_has("install: send the file now; console input is ignored while waiting"));

    /* 固定槽位模式下不带参数：拒绝，不让内核猜落点。 */
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    CK_I(call_cmd(cmd_install, NULL, NULL, NULL, NULL), -1);
    CK(out_has("usage: install <slot>   (fixed-slot mode needs an explicit slot)"));

    CK_I(call_cmd(cmd_install, "x", NULL, NULL, NULL), -1);
    CK(out_has("a slot number is only valid in fixed-slot mode"));

    CK_I(call_cmd(cmd_install, "5", NULL, NULL, NULL), -1);
    CK(out_has("a slot number is only valid in fixed-slot mode"));

    g_cfg_valid = 0;
    CK_I(call_cmd(cmd_install, "0", NULL, NULL, NULL), -1);
    CK(out_has("install: no such slot"));

    /* 自动选址模式下给槽号同样拒绝。 */
    g_pt.layout_mode = 0u;
    g_cfg_valid = 1;
    CK_I(call_cmd(cmd_install, "0", NULL, NULL, NULL), -1);
    CK(out_has("a slot number is only valid in fixed-slot mode"));
}

static void t_install_ok_drains(void)
{
    pt_reset(4u);
    slots_mixed();

    g_sink_reads = 3;
    g_run_once_rc = 3;
    CK_I(call_cmd(cmd_install, NULL, NULL, NULL, NULL), 0);
    CK(out_has("install: ok, slot 3"));
    CK_I(g_run_once_dev, 9);
    CK_I(g_run_once_timeout, 120000);
    /* 3 次有数据 + 1 次返回 0 结束。 */
    CK_I(g_sink_calls, 4);

    g_sink_reads = 0;
    g_sink_calls = 0;
    g_run_once_rc = -4;
    CK_I(call_cmd(cmd_install, NULL, NULL, NULL, NULL), 0);
    CK(out_has("install: failed or timed out (see 'fault')"));
    CK_I(g_sink_calls, 1);
}

static void t_install_fixed_stops_running(void)
{
    pt_reset(4u);
    slots_mixed();
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;

    /* 下标 0（RUNNING）与要清的 0x11220000+4096 重叠：必须先停再擦。
     * 下标 1 不重叠、下标 2 不 RUNNING、下标 3 是 FREE —— 三种跳过都走到。 */
    g_stop_rc = 0;
    g_hint = 12345;
    CK_I(call_cmd(cmd_install, "0", NULL, NULL, NULL), 0);
    CK(out_has("install: slot 0 still running (id 1), stopping it before the erase"));
    CK_I(g_last_slot, 0);
    CK(out_has("install: fixed slot 0 -> 0x11200000"));
    CK(out_has("install: ok, slot 3"));
    CK_I(g_hint, 0);           /* clear 之后写回槽号提示 */

    /* 停不下来就不许擦。 */
    g_stop_rc = -9;
    cap_reset();
    CK_I(call_cmd(cmd_install, "0", NULL, NULL, NULL), -1);
    CK(out_has("install: cannot stop the running image, refusing to erase its flash"));
    CK(!out_has("install: fixed slot"));

    /* 停下来了但擦失败。 */
    g_stop_rc = 0;
    g_clear_rc = -6;
    CK_I(call_cmd(cmd_install, "0", NULL, NULL, NULL), -1);
    CK(out_has("install: cannot clear the slot (flash erase failed)"));
    CK(!out_has("install: fixed slot"));
}

/* ------------------------------------------------------------ main */

int main(void)
{
    t_info_layout_and_map();
    t_info_empty_pool();

    t_app_list();
    t_app_usage();
    t_app_start();
    t_app_stop();
    t_app_uninstall();
    t_app_install_errors();
    t_app_install_fixed_ok_no_fs();
    t_app_install_from_file();
#if (ARKRTOS_USE_TASK_SWAP == 1)
    t_app_swap();
#endif

    t_drv_list_and_usage();
    t_drv_start_stop_uninstall();

    t_install_usage();
    t_install_ok_drains();
    t_install_fixed_stops_running();

    if (g_failed != 0) {
        printf("SHELL-IMAGE-HOST FAIL %d of %d checks\n", g_failed, g_checks);
        return 1;
    }
    printf("SHELL-IMAGE-HOST OK  %d checks\n", g_checks);
    return 0;
}
