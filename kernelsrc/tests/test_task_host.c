/**
* @file test_task_host.c
* @brief 任务调度与 SVC 分发（arkrtos_task.c）的宿主单元测试
* @details 被测的是真实的 arkrtos_task.c（4400 多行）。它是内核的枢纽：
*          调度决策、就绪位图、延时链、阻塞/唤醒、线程创建、任务下线、
*          故障恢复、栈守卫、任务热换入，以及 19 个 SVC 号的参数校验与分发。
*          这些路径的失效方式大多是「安静」的：
*
*            - 位图与任务表脱钩 → 某个 READY 任务永远排不上，不报错；
*            - 延时链排序错了 → 刚 armed 的睡眠者排到链头，把别人饿到超时；
*            - 阻塞接口在调度锁里照睡 → 当前任务永久让出 CPU；
*            - 唤醒原因不区分交接位 → 调用方把超时当成拿到了对象；
*            - SVC 参数块不校验 → 用户态借内核读写任意地址；
*            - 栈守卫报一次就不再报 → 故障环里只有一条，看着像偶发；
*            - 换入整块搬运少搬一个字节 → 新 main() 在错地址上取指。
*
*          本文件把任务表、port 层与全部外部子系统都换成可编程桩：
*          桩只回答「内核问的那一个问题」，并用计数/字节流把答案留下来，
*          所以这里证明的是状态机与账目，不是实时性，也不是真机时序。
*
*          覆盖范围（如实说明）：
*            已验证：指针校验的窗口/回绕/ushell 旁路、就绪位的增删改转与
*                    自愈、延时链按有符号差值排序与僵尸节点丢弃、调度锁嵌套、
*                    wait/wait_period/block/block_in_critical 的每一条返回路径、
*                    线程创建的三道闸门与窗口继承、sched_activate 的保存与
*                    摘除、栈用量两路读数、上下文栈守卫的布防与报警、
*                    task_swap 的七类拒绝/整块搬运/cancel/selftest、
*                    19 个 SVC 号的每一条校验拒绝与 dispatch、tick 的
*                    CPU_LOAD 窗口与时间片轮转、kill/recover/recover_pending、
*                    sched_is_switching 的三条分支、fault handler 的两条上下文、
*                    sched_next 的一次性 rebuild 与探针快照。
*            未验证：真实节拍下的时序（谁先跑、切换开销、ISR 抢占窗口）、
*                    两个任务真的并发、端口层的帧改写（只验证「被调用了」）。
*
* @note 用法：由 tools/coverage_gate.py 的 task case 编译并运行，
*       输出 TASK HOST OK <checks> checks。
* @author xw
* @date 2026.11.22
*/

#include "arkrtos_config.h"
#include "arkrtos_partition.h"
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"
#include "arkrtos_fault.h"
#include "arkrtos_dev.h"
#include "arkrtos_ushell.h"
#include "arkrtos_svc_abi.h"
#include "arkrtos_hal.h"
#include "arkrtos_kernel_check.h"
#include "arkrtos_event.h"
#include "arkrtos_sync.h"
#include "arkrtos_mq.h"
#include "arkrtos_timer.h"
#include "arkrtos_loader.h"
#include "arkrtos_mpu.h"
#include "arkrtos_trace.h"
#include "arkrtos_guard.h"
#include "arkrtos_log.h"
#include "arkrtos_shell.h"
#include "arkrtos_net.h"
#include "arkrtos_fs.h"
#include "arkrtos_vfs.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>

/* arkrtos_task.c 自己定义、但没有在头文件里 extern 的内部全局量。
 * 用例只读/清它们，不重复定义；重复定义会在链接期撞成 MULTIPLE DEFINITION。 */
extern volatile uint32 arkrtos_kill_ret_cnt;
extern volatile uint32 arkrtos_sched_probe[6u * 16u];
extern volatile uint32 arkrtos_sched_probe_pos;
extern volatile uint32 arkrtos_ready_heal_cnt;
extern volatile uint32 arkrtos_ready_heal_head;
extern volatile uint8  arkrtos_dbg_wake_last[ARKRTOS_TASK_MAX_NUM];
extern volatile uint8  arkrtos_dbg_wake_hits[ARKRTOS_TASK_MAX_NUM];
extern uint32 arkrtos_kernel_ushell_active;
#if (ARKRTOS_USE_CPU_LOAD == 1)
extern uint16 arkrtos_cpu_load_counter;
#endif

/* ------------------------------------------------------------------
 * 板级裸输出桩（docs/QEMU模拟器移植与验证.md §5.34）
 *
 * 内核在上下文切换路径上不能拿日志锁（拿了可能当场再触发一次调度），
 * 所以栈溢出这条「无声死亡」走的是一行裸的 arkrtos_board_puts/putu。
 * 宿主这里把这两条收进缓冲：既让链接成立，也让用例能断言「死亡是有声的」，
 * 而不是只看到任务表里多了一行 INVALID。
 * ------------------------------------------------------------------ */
/* 先声明再定义：warn_gate 的 -Wmissing-prototypes 也管用例文件 */
void arkrtos_board_puts(const char *s);
void arkrtos_board_putu(unsigned int v);

static char   g_board_out[256];
static uint32 g_board_out_len;

void arkrtos_board_puts(const char *s)
{
    if(s == 0)
    {
        return;
    }
    while((*s != '\0') && (g_board_out_len < (uint32)(sizeof(g_board_out) - 1u)))
    {
        g_board_out[g_board_out_len] = *s;
        g_board_out_len++;
        s++;
    }
    g_board_out[g_board_out_len] = '\0';
}

void arkrtos_board_putu(unsigned int v)
{
    char tmp[12];
    int  n = 0;

    if(v == 0u)
    {
        tmp[n] = '0';
        n++;
    }
    while((v != 0u) && (n < (int)sizeof(tmp)))
    {
        tmp[n] = (char)('0' + (v % 10u));
        n++;
        v /= 10u;
    }
    while(n > 0)
    {
        char one[2];
        n--;
        one[0] = tmp[n];
        one[1] = '\0';
        arkrtos_board_puts(one);
    }
}

static void board_out_reset(void)
{
    g_board_out_len = 0u;
    g_board_out[0] = '\0';
}

static int board_out_has(const char *s)
{
    return (strstr(g_board_out, s) != 0) ? 1 : 0;
}

/* ------------------------------------------------------------------
 * 断言
 * ------------------------------------------------------------------ */
static int g_checks = 0;
static int g_fails  = 0;

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
        printf("FAIL: %s (got %d, want %d)\n", what, (int)got, (int)want);
    }
}

static void ck_u32(uint32 got, uint32 want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL: %s (got %u, want %u)\n", what,
               (unsigned)got, (unsigned)want);
    }
}

/* ------------------------------------------------------------------
 * port 层桩
 * ------------------------------------------------------------------ */
static int  g_switch_n = 0;
static int  g_wfe_n    = 0;
static int  g_irq_off  = 0;
static int  g_irq_on   = 0;
static int  g_in_isr   = 0;
static uint32 g_priv_last = 0u;
static int  g_priv_n   = 0;
static int  g_crit_n   = 0;
static uint32 g_delay_us_sum = 0u;

/* switch_task 桩：可编程的「唤醒者」。真实系统里是 PendSV 之后对象层
 * 把 wake_reason 写进 TCB；这里由测试指定这次切换要不要带回一个原因，
 * 才能覆盖 block_in_critical 的两条返回路径。 */
static int  g_switch_sets_wake = -1;   /* -1 = 不动 */

void arkrtos_port_disable_irq(void)  { g_irq_off++; }
void arkrtos_port_enable_irq(void)   { g_irq_on++; }
uint8 arkrtos_port_in_isr(void)      { return (uint8)g_in_isr; }
uint32 arkrtos_port_enter_critical(void) { g_crit_n++; return 0x5Au; }
void  arkrtos_port_exit_critical(uint32 state) { (void)state; }
void  arkrtos_port_set_thread_priv(uint32 is_priv)
{
    g_priv_n++;
    g_priv_last = is_priv;
}
void arkrtos_port_delay_us(uint32 us) { g_delay_us_sum += us; }
void arkrtos_port_wfe(void)           { g_wfe_n++; }

/* entry_return 的 wfi 桩：真实实现是"停在本任务上下文里死等"，宿主里没法
 * 真的停住，所以用 setjmp/longjmp 截住。只验「返回即收尸、确实落进等待」，
 * 死循环本身不可测（也不需要测）。 */
static jmp_buf g_wfi_jmp;
static int     g_wfi_jmp_armed = 0;
static int     g_wfi_hits = 0;

void arkrtos_port_wfi(void)
{
    if(g_wfi_jmp_armed == 0)
    {
        return;
    }
    /* 第一次返回让退出循环的那条回边走一遍（死等本身没内容，但回边是
     * 别的东西"被卡住"时的唯一证据），第二次再跳出，用例才不会挂死。 */
    g_wfi_hits++;
    if(g_wfi_hits >= 2)
    {
        g_wfi_jmp_armed = 0;
        longjmp(g_wfi_jmp, 1);
    }
}

void arkrtos_port_switch_task(void)
{
    g_switch_n++;
    if((g_switch_sets_wake >= 0) && (arkrtos_current_task_id > 0))
    {
        arkrtos_task_table[arkrtos_current_task_id - 1].wake_reason =
            (uint8)g_switch_sets_wake;
    }
}

/* ------------------------------------------------------------------
 * SVC 上下文桩：寄存器数组 + 返回值
 * ------------------------------------------------------------------ */
static uint32 g_svc_num = 0u;
static unsigned long g_svc_args[8];
static unsigned long g_svc_ret;
static int g_svc_ret_set = 0;

unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx)
{
    (void)p_exc_ctx;
    return (idx < 8u) ? g_svc_args[idx] : 0u;
}

void arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value)
{
    (void)p_exc_ctx;
    g_svc_ret = value;
    g_svc_ret_set = 1;
}

uint32 arkrtos_port_syscall_num(void *p_exc_ctx)
{
    (void)p_exc_ctx;
    return g_svc_num;
}

unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*entry)(void))
{
    (void)entry;
    return stack_top;
}

/* 换入端口层：只记「被调用过」，帧内容由内核的整块搬运负责。 */
static int g_swap_bytes = 32;
static int g_frame_ok = 1;
static void *g_swap_activate_old = 0;
static void *g_swap_activate_new = 0;
static int g_swap_activate_n = 0;
static void *g_swap_rewrite_frame = 0;
static uint32 g_swap_rewrite_entry = 0u;
static int g_swap_rewrite_n = 0;

uint32 arkrtos_port_swap_frame_bytes(void) { return (uint32)g_swap_bytes; }
int32  arkrtos_port_swap_frame_ok(const void *p) { (void)p; return g_frame_ok; }
void   arkrtos_port_swap_rewrite(void *p_exc_ctx, uint32 entry)
{
    g_swap_rewrite_n++;
    g_swap_rewrite_frame = p_exc_ctx;
    g_swap_rewrite_entry = entry;
}
void arkrtos_port_swap_activate(void *p_old_ctx, void *p_new_ctx)
{
    g_swap_activate_n++;
    g_swap_activate_old = p_old_ctx;
    g_swap_activate_new = p_new_ctx;
}

/* ------------------------------------------------------------------
 * 控制台 / trace / guard / log / mpu
 * ------------------------------------------------------------------ */
static uint8 g_console[1024];
static uint32 g_console_len = 0u;

int32 arkrtos_console_is_handle(int32 handle) { return (handle == 0x55) ? 1 : 0; }

int32 arkrtos_console_write(const uint8 *p_data, uint32 len)
{
    uint32 i;

    for(i = 0u; (i < len) && (g_console_len < sizeof(g_console)); i++)
    {
        g_console[g_console_len++] = p_data[i];
    }
    return (int32)len;
}

static void console_reset(void)
{
    g_console_len = 0u;
    memset(g_console, 0, sizeof(g_console));
}

static int console_has(const char *s)
{
    uint32 n = (uint32)strlen(s);
    uint32 i;

    if(n > g_console_len)
    {
        return 0;
    }
    for(i = 0u; i + n <= g_console_len; i++)
    {
        if(memcmp(&g_console[i], s, n) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int g_trace_isr_enter = 0;
static int g_trace_isr_exit  = 0;
static int g_trace_switch_n  = 0;
static int g_trace_wait_n    = 0;
static uint16 g_trace_obj = 0u;

void arkrtos_trace_isr(uint8 irq, uint8 kind)
{
    (void)irq;
    if(kind == ARKRTOS_TR_ISR_ENTER)
    {
        g_trace_isr_enter++;
    }
    else
    {
        g_trace_isr_exit++;
    }
}

void arkrtos_trace_switch(uint8 from, uint8 to) { (void)from; (void)to; g_trace_switch_n++; }
void arkrtos_trace_wait(uint8 task_id)          { (void)task_id; g_trace_wait_n++; }
uint16 arkrtos_trace_take_wait_obj(void)        { uint16 v = g_trace_obj; g_trace_obj = 0u; return v; }
uint32 arkrtos_trace_last_cycles(void)          { return 0u; }
void arkrtos_trace_record(uint8 ev, uint8 arg)  { (void)ev; (void)arg; }

static int g_obj_wait_n    = 0;
static int g_obj_timeout_n = 0;
static uint16 g_obj_wait_obj = 0u;
static uint8  g_obj_wait_cls = 0u;
static uint16 g_obj_timeout_obj = 0u;

void mdk_trace_arkrtos_obj_wait(uint16_t obj, uint8_t cls)
{
    g_obj_wait_n++;
    g_obj_wait_obj = obj;
    g_obj_wait_cls = cls;
}

void mdk_trace_arkrtos_obj_timeout(uint16_t obj, uint8_t cls)
{
    (void)cls;
    g_obj_timeout_n++;
    g_obj_timeout_obj = obj;
}

static int g_task_create_trace_n = 0;
static int g_task_exit_trace_n   = 0;
static uint32 g_task_create_slot = 0u;
static uint32 g_task_create_entry = 0u;

void mdk_trace_arkrtos_task_create(uint32_t slot, uint32_t entry)
{
    g_task_create_trace_n++;
    g_task_create_slot = slot;
    g_task_create_entry = entry;
}

void mdk_trace_arkrtos_task_exit(uint32_t slot) { (void)slot; g_task_exit_trace_n++; }

static int g_guard_tick_n = 0;
static int g_guard_svc_n  = 0;
static int g_guard_beat_n = 0;
static int32 g_guard_beat_ret = 0;

void  arkrtos_guard_tick(void) { g_guard_tick_n++; }
void  arkrtos_guard_svc(int32 task_id) { (void)task_id; g_guard_svc_n++; }
int32 arkrtos_guard_beat(int32 task_id, uint32 period_ms)
{
    (void)task_id;
    (void)period_ms;
    g_guard_beat_n++;
    return g_guard_beat_ret;
}

static int g_log_emit_n = 0;
static uint32 g_log_level = 3u;
static int g_log_svc_n = 0;

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level; (void)tag; (void)line; (void)msg;
    g_log_emit_n++;
}
uint32 arkrtos_log_get_level(void) { return g_log_level; }
int32 arkrtos_log_svc(uint32 level, const char *tag, const char *msg)
{
    (void)level; (void)tag; (void)msg;
    g_log_svc_n++;
    return 7;
}

/* 这三个钩子没有公共头文件，定义前先补原型（-Wmissing-prototypes）。 */
void arkrtos_sched_stat_close(uint32 t0);
void arkrtos_rt_tick_enter(void);
void arkrtos_rt_tick_exit(void);

void arkrtos_sched_stat_close(uint32 t0) { (void)t0; }
void arkrtos_rt_tick_enter(void) { }
void arkrtos_rt_tick_exit(void)  { }

static int g_mpu_build_n = 0;
static int g_mpu_app_n   = 0;
static int g_mpu_idle_n  = 0;

void arkrtos_mpu_build_task(arkrtos_task_t *p_task) { (void)p_task; g_mpu_build_n++; }
void arkrtos_mpu_set_app(arkrtos_task_t *p_app)     { (void)p_app; g_mpu_app_n++; }
void arkrtos_mpu_set_idle(void)                     { g_mpu_idle_n++; }

/* ------------------------------------------------------------------
 * 故障环桩
 * ------------------------------------------------------------------ */
static arkrtos_fault_record_t g_fault[32];
static int g_fault_n = 0;

void arkrtos_fault_record(uint32 type, int32 task_id)
{
    if(g_fault_n < (int)(sizeof(g_fault) / sizeof(g_fault[0])))
    {
        g_fault[g_fault_n].type = type;
        g_fault[g_fault_n].task_id = task_id;
        g_fault[g_fault_n].tick = arkrtos_kernel_tick;
        g_fault_n++;
    }
}

const arkrtos_fault_record_t *arkrtos_fault_record_get(int32 index)
{
    if((index < 0) || (index >= g_fault_n))
    {
        return 0;
    }
    return &g_fault[index];
}

int32 arkrtos_fault_record_count_internal(void) { return g_fault_n; }

static int fault_has(uint32 type, int32 task_id)
{
    int i;

    for(i = 0; i < g_fault_n; i++)
    {
        if((g_fault[i].type == type) && (g_fault[i].task_id == task_id))
        {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------
 * 其它子系统桩：返回值按「能不能看出分派到了谁」来设
 * ------------------------------------------------------------------ */
static int g_event_set_n = 0;
int32 arkrtos_event_create_internal(const char *name) { (void)name; return 101; }
int32 arkrtos_event_wait_internal(int32 handle, int32 timeout_ms)
{
    (void)handle; (void)timeout_ms;
    return 3;
}
void arkrtos_event_set_internal(int32 handle) { (void)handle; g_event_set_n++; }

static int g_release_n = 0;
static int g_sync_release_n = 0;
static int g_mq_release_n = 0;
static int g_net_release_n = 0;
static int g_shell_release_n = 0;
static uint32 g_shell_release_id = 0u;

void arkrtos_event_release_task(int32 task_id) { (void)task_id; g_release_n++; }
void arkrtos_sync_release_task(int32 task_id)  { (void)task_id; g_sync_release_n++; }
void arkrtos_mq_release_task(int32 task_id)    { (void)task_id; g_mq_release_n++; }
void arkrtos_net_release_task(int32 task_id)   { (void)task_id; g_net_release_n++; }
int32 arkrtos_shell_release_task(uint32 task_id) { g_shell_release_n++; g_shell_release_id = task_id; return 0; }

int32 arkrtos_sem_create_internal(const char *name, int32 init_count)
{
    (void)name; (void)init_count;
    return 201;
}
int32 arkrtos_sem_wait_internal(int32 handle, int32 timeout_ms)
{
    (void)handle; (void)timeout_ms;
    return 0;
}
int32 arkrtos_sem_post_internal(int32 handle)   { (void)handle; return 0; }
int32 arkrtos_sem_delete_internal(int32 handle) { (void)handle; return 0; }

int32 arkrtos_mtx_create_internal(const char *name) { (void)name; return 202; }
int32 arkrtos_mtx_lock_internal(int32 handle, int32 timeout_ms)
{
    (void)handle; (void)timeout_ms;
    return 0;
}
int32 arkrtos_mtx_unlock_internal(int32 handle) { (void)handle; return 0; }
int32 arkrtos_mtx_delete_internal(int32 handle) { (void)handle; return 0; }

int32 arkrtos_cond_create_internal(const char *name) { (void)name; return 203; }
int32 arkrtos_cond_wait_internal(int32 c, int32 m, int32 t)
{
    (void)c; (void)m; (void)t;
    return 0;
}
int32 arkrtos_cond_signal_internal(int32 handle)    { (void)handle; return 0; }
int32 arkrtos_cond_broadcast_internal(int32 handle) { (void)handle; return 0; }
int32 arkrtos_cond_delete_internal(int32 handle)    { (void)handle; return 0; }
int32 arkrtos_cond_enqueue_internal(int32 handle)   { (void)handle; return 0; }
int32 arkrtos_cond_poll_internal(int32 handle)      { (void)handle; return 0; }
int32 arkrtos_cond_abort_internal(int32 handle)     { (void)handle; return 0; }

int32 arkrtos_mq_create_internal(const char *name) { (void)name; return 204; }
int32 arkrtos_mq_send_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms)
{
    (void)handle; (void)buf; (void)len_words; (void)timeout_ms;
    return 0;
}
int32 arkrtos_mq_recv_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms)
{
    (void)handle; (void)buf; (void)len_words; (void)timeout_ms;
    return 0;
}
int32 arkrtos_mq_delete_internal(int32 handle) { (void)handle; return 0; }

int32 arkrtos_timer_create_internal(const char *name) { (void)name; return 205; }
int32 arkrtos_timer_start_internal(int32 handle, uint32 period_ms, uint8 mode,
                                   void (*cb)(void *), void *arg)
{
    (void)handle; (void)period_ms; (void)mode; (void)cb; (void)arg;
    return 0;
}
int32 arkrtos_timer_stop_internal(int32 handle)   { (void)handle; return 0; }
int32 arkrtos_timer_delete_internal(int32 handle) { (void)handle; return 0; }

static int g_timer_tick_n = 0;
void arkrtos_timer_tick_handler(void) { g_timer_tick_n++; }

static int g_dev_open_n = 0;
static int g_dev_write_n = 0;
static int g_dev_close_n = 0;
static int g_dev_ctrl_n = 0;
static int g_dev_read_n = 0;
int32 g_dev_open_ret = 6;

int32 arkrtos_dev_open_internal(const char *name, uint32 param)
{
    (void)name; (void)param;
    g_dev_open_n++;
    return g_dev_open_ret;
}
int32 arkrtos_dev_close_internal(int32 handle) { (void)handle; g_dev_close_n++; return 0; }
int32 arkrtos_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    (void)handle; (void)pdata; (void)len;
    g_dev_read_n++;
    return 0;
}
int32 arkrtos_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    (void)handle; (void)pdata; (void)len;
    g_dev_write_n++;
    return 0;
}
int32 arkrtos_dev_ctrl_internal(int32 handle, uint32 code, uint32 value)
{
    (void)handle; (void)code; (void)value;
    g_dev_ctrl_n++;
    return 0;
}
int32 arkrtos_dev_register(const char *name, arkrtos_dev_drv_t *drv, uint32 dev_num)
{
    (void)name; (void)drv; (void)dev_num;
    return 0;
}
int32 arkrtos_dev_unregister(const char *name) { (void)name; return 0; }
int32 arkrtos_dev_get_count(void) { return 3; }

static int g_loader_fault_ret = 0;
static int g_loader_fault_n = 0;
static int32 g_loader_swap_take_ret = 0;

int32 arkrtos_loader_load_dev(int32 dev, uint32 image_len) { (void)dev; (void)image_len; return 0; }
int32 arkrtos_loader_load_driver(int32 dev, uint32 image_len) { (void)dev; (void)image_len; return 0; }
int32 arkrtos_loader_start(uint32 slot) { (void)slot; return 0; }
int32 arkrtos_loader_stop(uint32 slot) { (void)slot; return 0; }
uint32 arkrtos_loader_state(uint32 slot) { (void)slot; return 1u; }
uint32 arkrtos_loader_pool_free(uint32 *p_largest)
{
    if(p_largest != 0)
    {
        *p_largest = 0x1234u;
    }
    return 0x5678u;
}
int32 arkrtos_loader_load_dev_hdr_unused(void);
int32 arkrtos_loader_on_fault(int32 task_id)
{
    g_loader_fault_n++;
    if(g_loader_fault_ret == 1)
    {
        /* 契约（见 arkrtos_loader.h）：达到重启上限时 loader 自己把该任务
         * 置 INVALID、清掉恢复挂起并摘出就绪链；本桩照此办理，否则测的是
         * 「桩什么都不做」而不是内核的禁用分支。 */
        int32 i = task_id - 1;

        if((i >= 0) && (i < arkrtos_task_count))
        {
            arkrtos_ready_del(i);
            arkrtos_task_table[i].recover_pending = 0;
            arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
        }
    }
    return g_loader_fault_ret;
}
int32 arkrtos_loader_swap_take(void *p_exc_ctx, int32 task_id)
{
    (void)p_exc_ctx; (void)task_id;
    return g_loader_swap_take_ret;
}

static int g_shell_print_n = 0;
int32 arkrtos_shell_ext_register(const arkrtos_ushell_cmd_t *cmd) { (void)cmd; return 0; }
int32 arkrtos_shell_ext_unregister(const char *name) { (void)name; return 0; }
int32 arkrtos_shell_print_n_calls(const char *msg, uint32 len);
int32 arkrtos_shell_print_n(const char *msg, uint32 len)
{
    (void)msg; (void)len;
    g_shell_print_n++;
    return (int32)len;
}

static int g_net_svc_n = 0;
int32 arkrtos_net_svc(void *p_svc_ctx) { (void)p_svc_ctx; g_net_svc_n++; return -3; }

int32 arkrtos_fs_mount_default(void) { return 0; }
uint8 arkrtos_fs_mounted(void) { return 1u; }
int32 arkrtos_fs_unmount(void) { return 0; }
int32 arkrtos_fs_stat(uint32 *total, uint32 *used)
{
    if(total != 0) { *total = 4096u; }
    if(used != 0)  { *used = 1024u; }
    return 0;
}
int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_dir)
{
    (void)path;
    if(size != 0)   { *size = 11u; }
    if(is_dir != 0) { *is_dir = 0u; }
    return 0;
}
int32 arkrtos_fs_write_file(const char *path, const uint8 *data, uint32 len)
{
    (void)path; (void)data;
    return (int32)len;
}
int32 arkrtos_fs_read_file(const char *path, uint8 *buf, uint32 max, uint32 *out_len)
{
    (void)path; (void)buf; (void)max;
    if(out_len != 0) { *out_len = 6u; }
    return 0;
}
int32 arkrtos_fs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off,
                         uint32 *out_len)
{
    (void)path; (void)buf; (void)len; (void)off;
    if(out_len != 0) { *out_len = 3u; }
    return 0;
}
int32 arkrtos_fs_remove(const char *path) { (void)path; return 0; }
int32 arkrtos_fs_rename(const char *old_path, const char *new_path)
{
    (void)old_path; (void)new_path;
    return 0;
}
int32 arkrtos_fs_list_names(const char *dir, char *out, uint32 out_size, uint32 *count)
{
    (void)dir; (void)out; (void)out_size;
    if(count != 0) { *count = 2u; }
    return 0;
}

int32 arkrtos_vfs_app_open(const char *path, uint32 flags) { (void)path; (void)flags; return 9; }
int32 arkrtos_vfs_app_read(int32 handle, uint8 *buf, uint32 len)
{
    (void)handle; (void)buf;
    return (int32)len;
}
int32 arkrtos_vfs_app_write(int32 handle, const uint8 *buf, uint32 len)
{
    (void)handle; (void)buf;
    return (int32)len;
}
int32 arkrtos_vfs_app_close(int32 handle) { (void)handle; return 0; }
int32 arkrtos_vfs_app_stat(const char *path, uint32 *size, uint32 *mode)
{
    (void)path;
    if(size != 0) { *size = 5u; }
    if(mode != 0) { *mode = 1u; }
    return 0;
}
int32 arkrtos_vfs_app_seek(int32 handle, int32 off, uint32 whence)
{
    (void)handle; (void)off; (void)whence;
    return 0;
}
int32 arkrtos_vfs_app_readdir(int32 handle, char *name, uint32 cap,
                              uint32 *mode, uint32 *size)
{
    (void)handle;
    if(cap > 0u) { name[0] = 'x'; }
    if(mode != 0) { *mode = 2u; }
    if(size != 0) { *size = 3u; }
    return 0;
}
int32 arkrtos_vfs_app_unlink(const char *path) { (void)path; return 0; }
int32 arkrtos_vfs_app_mkdir(const char *path)  { (void)path; return 0; }
int32 arkrtos_vfs_app_rmdir(const char *path)  { (void)path; return 0; }
int32 arkrtos_vfs_app_rename(const char *old_path, const char *new_path)
{
    (void)old_path; (void)new_path;
    return 0;
}
int32 arkrtos_vfs_app_truncate(int32 handle, uint32 size) { (void)handle; (void)size; return 0; }
int32 arkrtos_vfs_app_truncate_path(const char *path, uint32 size)
{
    (void)path; (void)size;
    return 0;
}

/* ------------------------------------------------------------------
 * 本地任务表与注册（按 arkrtos_cfg.c 的真实语义实现）
 * ------------------------------------------------------------------ */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32 arkrtos_task_count = 0;

static void stack_fill(uint32 *bottom, uint32 size)
{
    uint32 words = size / 4u;
    uint32 i;

    for(i = 1u; i < words; i++)
    {
        bottom[i] = (uint32)ARKRTOS_STACK_FILL_PATTERN;
    }
}

void arkrtos_task_stack_init(arkrtos_task_t *p_task, void (*entry)(void),
                             uint32 *stack_bottom, uint32 stack_size)
{
    unsigned long stack_top = (unsigned long)stack_bottom + stack_size;

    stack_top &= ~0x7UL;
    stack_fill(stack_bottom, stack_size);

    p_task->stack_bottom = stack_bottom;
    p_task->stack_top    = stack_top;
    p_task->stack_peak_low = stack_top;
    p_task->entry        = entry;
    p_task->stack_ptr    = arkrtos_port_stack_init(stack_top, entry);
}

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom, uint32 stack_size,
                            uint8 priority, uint32 period_ms)
{
    arkrtos_task_t *p_task;
    int32 free_idx = -1;
    int32 i;

    for(i = 0; i < arkrtos_task_count; i++)
    {
        if((arkrtos_task_table[i].status == ARKRTOS_TASK_INVALID) &&
           (arkrtos_task_table[i].recover_pending == 0))
        {
            free_idx = i;
            break;
        }
    }

    if(free_idx < 0)
    {
        if(arkrtos_task_count < (int32)ARKRTOS_TASK_MAX_NUM)
        {
            free_idx = arkrtos_task_count;
            arkrtos_task_count++;
        }
        else
        {
            arkrtos_fault_record(ARKRTOS_FAULT_NOSLOT, 0);
            return -1;
        }
    }

    if((entry == 0) || (stack_bottom == 0) || (stack_size < 64u))
    {
        arkrtos_fault_record(ARKRTOS_FAULT_NOSLOT, 0);
        return -1;
    }

    stack_bottom[0] = (uint32)ARKRTOS_STACK_END_FLAG_VAL;

    p_task = &arkrtos_task_table[free_idx];
    p_task->ram_start     = (uint32)stack_bottom;
    p_task->ram_size      = stack_size;
    p_task->stack_size    = stack_size;
    p_task->rom_start     = 0;
    p_task->rom_size      = 0;
    p_task->period        = (int32)ARKRTOS_MS_TO_TICK(period_ms);
    p_task->priority      = priority;
    p_task->base_priority = priority;
    p_task->shm_attri     = 0;
    p_task->stack_guard_armed    = 1u;
    p_task->stack_guard_reported = 0u;
    p_task->is_priv       = 1u;
    p_task->status        = ARKRTOS_TASK_READY;
    p_task->period_time   = p_task->period;
    p_task->wait_time     = 0;
    p_task->tim_tick      = 0;
    p_task->touch_tick    = 0;
    p_task->wake_reason   = 0;
    p_task->recover_pending = 0;
    p_task->ready_next    = ARKRTOS_TASK_NIL;
    p_task->ready_prev    = ARKRTOS_TASK_NIL;
    p_task->delay_next    = ARKRTOS_TASK_NIL;
    p_task->delay_prev    = ARKRTOS_TASK_NIL;
    p_task->delay_queued  = 0u;
    p_task->delay_tick    = 0u;
    p_task->slice_tick    = (int32)ARKRTOS_TIME_SLICE_TICKS;

    arkrtos_mpu_build_task(p_task);
    arkrtos_task_stack_init(p_task, entry, stack_bottom, stack_size);
    arkrtos_ready_add(free_idx);

    return (free_idx + 1);
}

/* ------------------------------------------------------------------
 * 夹具
 * ------------------------------------------------------------------ */
static uint32 g_stack[16][256];        /* 每个任务 1 KB 栈 */
static int g_stack_next = 0;

static void sample_entry(void) { }

static void reset_all(void)
{
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    arkrtos_task_count = 0;
    arkrtos_current_task_id = 0;
    arkrtos_kernel_tick = 0;
    arkrtos_kill_ret_cnt = 0;
    arkrtos_sched_lock_nest = 0;
    arkrtos_cpu_load_counter = 0;
    arkrtos_cpu_busy_ticks = 0;
    arkrtos_idle_ticks = 0;
    arkrtos_kernel_ushell_active = 0;
    arkrtos_sched_probe_pos = 0u;

    arkrtos_ready_reset();

    g_switch_n = 0;
    g_wfe_n = 0;
    g_irq_off = 0;
    g_irq_on = 0;
    g_in_isr = 0;
    g_priv_last = 0u;
    g_priv_n = 0;
    g_crit_n = 0;
    g_delay_us_sum = 0u;
    g_switch_sets_wake = -1;

    g_svc_num = 0u;
    memset(g_svc_args, 0, sizeof(g_svc_args));
    g_svc_ret = 0u;
    g_svc_ret_set = 0;

    g_swap_activate_n = 0;
    g_swap_rewrite_n = 0;
    g_swap_activate_old = 0;
    g_swap_activate_new = 0;
    g_swap_rewrite_frame = 0;
    g_swap_rewrite_entry = 0u;
    g_swap_bytes = 32;
    g_frame_ok = 1;

    console_reset();
    board_out_reset();
    g_trace_isr_enter = 0;
    g_trace_isr_exit = 0;
    g_trace_switch_n = 0;
    g_trace_wait_n = 0;
    g_trace_obj = 0u;
    g_obj_wait_n = 0;
    g_obj_timeout_n = 0;
    g_obj_wait_obj = 0u;
    g_obj_wait_cls = 0u;
    g_obj_timeout_obj = 0u;
    g_task_create_trace_n = 0;
    g_task_exit_trace_n = 0;
    g_task_create_slot = 0u;
    g_task_create_entry = 0u;

    g_guard_tick_n = 0;
    g_guard_svc_n = 0;
    g_guard_beat_n = 0;
    g_guard_beat_ret = 0;
    g_log_emit_n = 0;
    g_log_svc_n = 0;
    g_log_level = 3u;

    g_mpu_build_n = 0;
    g_mpu_app_n = 0;
    g_mpu_idle_n = 0;

    g_fault_n = 0;

    g_event_set_n = 0;
    g_release_n = 0;
    g_sync_release_n = 0;
    g_mq_release_n = 0;
    g_net_release_n = 0;
    g_shell_release_n = 0;
    g_shell_release_id = 0u;
    g_timer_tick_n = 0;
    g_dev_open_n = 0;
    g_dev_write_n = 0;
    g_dev_close_n = 0;
    g_dev_ctrl_n = 0;
    g_dev_read_n = 0;
    g_dev_open_ret = 6;
    g_loader_fault_ret = 0;
    g_loader_fault_n = 0;
    g_loader_swap_take_ret = 0;
    g_shell_print_n = 0;
    g_net_svc_n = 0;

    g_stack_next = 0;
}

/* 注册一个新任务并返回任务号（栈从静态池里分）。 */
static int32 make_task(uint8 prio, uint32 period_ms)
{
    uint32 *stk = g_stack[g_stack_next % 16];

    g_stack_next++;
    return arkrtos_task_register(sample_entry, stk, 1024u, prio, period_ms);
}

/* 把当前任务切到 task_id（1 基）。 */
static void use_task(int32 task_id)
{
    arkrtos_current_task_id = task_id;
}

/* ------------------------------------------------------------------
 * T0：首次 sched_next 自动 rebuild（必须是全程序第一次）
 *
 * ready_built 是 task.c 内 static，ready_reset() 不清它，所以这条只能
 * 在「一次都没调过 sched_next」时测。手工填 TCB 不挂链正是 rebuild
 * 要兜住的旧式写法：不 rebuild 的话这个 READY 任务永远选不出来。
 * ------------------------------------------------------------------ */
static void test_first_sched_rebuild(void)
{
    arkrtos_task_t *p;

    reset_all();

    /* 手工填两个任务：只写 status/priority，不让 ready_add 挂链 */
    arkrtos_task_count = 2;
    p = &arkrtos_task_table[0];
    p->status = ARKRTOS_TASK_READY;
    p->priority = 5u;
    p->ready_next = ARKRTOS_TASK_NIL;
    p->ready_prev = ARKRTOS_TASK_NIL;
    p->delay_next = ARKRTOS_TASK_NIL;
    p->delay_prev = ARKRTOS_TASK_NIL;
    p->slice_tick = (int32)ARKRTOS_TIME_SLICE_TICKS;

    p = &arkrtos_task_table[1];
    p->status = ARKRTOS_TASK_READY;
    p->priority = 2u;
    p->ready_next = ARKRTOS_TASK_NIL;
    p->ready_prev = ARKRTOS_TASK_NIL;
    p->delay_next = ARKRTOS_TASK_NIL;
    p->delay_prev = ARKRTOS_TASK_NIL;
    p->slice_tick = (int32)ARKRTOS_TIME_SLICE_TICKS;

    ck_u32(arkrtos_sched_ready_count(), 0u, "unlinked tasks start outside the ready set");
    ck_i32(arkrtos_sched_next(), 1, "first sched_next rebuilds from status and picks prio 2");
    ck_u32(arkrtos_sched_ready_count(), 2u, "rebuild linked both READY tasks");
    ck(arkrtos_ready_contains(0) == 1u, "rebuild links task 1");
    ck(arkrtos_ready_contains(1) == 1u, "rebuild links task 2");
    ck_u32(arkrtos_sched_probe_pos, 1u, "each decision bumps the probe cursor");
    ck_u32(arkrtos_sched_probe[0], 1u, "probe[0] is the decision result");
    ck_u32(arkrtos_sched_probe[4], 2u, "probe[4] is the ready total");
}

/* ------------------------------------------------------------------
 * T1：用户指针校验（SVC 可信边界）
 * ------------------------------------------------------------------ */
static uint32 g_blk[8];

static void test_pointer_checks(void)
{
    reset_all();

    /* 宿主静态缓冲不在任何用户窗口内：默认必须全部拒绝 */
    ck(arkrtos_kernel_svc_args_ok(&g_blk[0], 16u) == 0u,
       "host static memory is not a user window");
    ck(arkrtos_kernel_svc_args_ok(0, 16u) == 0u, "NULL argument block is refused");
    ck(arkrtos_kernel_svc_args_ok(&g_blk[0], 0xFFFFFFF0u) == 0u,
       "a length that wraps the address is refused");
    ck(arkrtos_kernel_dev_buf_ok(&g_blk[0], 0) == 0u, "zero length write buffer refused");
    ck(arkrtos_kernel_dev_buf_ok(&g_blk[0], -1) == 0u, "negative write buffer refused");
    ck(arkrtos_kernel_user_ro_ok(&g_blk[0], 0u) == 0u, "zero length read buffer refused");
    ck(arkrtos_kernel_user_name_ok("abc", 4u) == 0u, "host string literal is not a user window");
    ck(arkrtos_kernel_user_ro_ok(0, 4u) == 0u, "NULL read buffer refused");

    /* App shell 回调期间：调用者栈在内核 RAM，必须放行，否则 print/log/queue
     * 一从回调里调用就全被拒。 */
    arkrtos_kernel_ushell_active = 1u;
    ck(arkrtos_kernel_svc_args_ok(&g_blk[0], 16u) == 1u,
       "ushell callback may pass its own kernel-RAM block");
    ck(arkrtos_kernel_dev_buf_ok(&g_blk[0], 16) == 1u,
       "ushell callback may pass a write buffer");
    ck(arkrtos_kernel_user_ro_ok(&g_blk[0], 16u) == 1u,
       "ushell callback may pass a read buffer");
    ck(arkrtos_kernel_user_name_ok("abc", 4u) == 1u,
       "ushell callback may pass a name");
    arkrtos_kernel_ushell_active = 0u;

    /* 窗口边界只做算术断言（用宏计算，不写死地址） */
    ck(SHARE_RAM_SIZE > 0u, "share window is non-empty");
    ck(SLOT_RAM_BASE > SHARE_RAM_BASE, "slot RAM sits above the share window");
    ck(IMAGE_POOL_SIZE > 0u, "image pool is non-empty");

    ck(arkrtos_kernel_svc_args_ok((const void *)0x10u, 4u) == 0u,
       "a low invalid address is refused");

    /* dev_buf_ok 的正长度要求：ushell 旁路也不能让 len<=0 过关 */
    arkrtos_kernel_ushell_active = 1u;
    ck(arkrtos_kernel_dev_buf_ok(&g_blk[0], 0) == 0u,
       "ushell bypass does not accept a zero length");
    arkrtos_kernel_ushell_active = 0u;
}

/* ------------------------------------------------------------------
 * T2：就绪位图（增删改转、自愈、重建、复位）
 * ------------------------------------------------------------------ */
static void test_ready_bitmap(void)
{
    int32 t1;
    int32 t2;
    int32 t3;

    reset_all();
    t1 = make_task(5u, 0u);      /* 1 */
    t2 = make_task(3u, 0u);      /* 2 */
    t3 = make_task(3u, 0u);      /* 3 */

    ck_i32(t1, 1, "first registration returns task 1");
    ck_i32(t2, 2, "second registration returns task 2");
    ck_i32(t3, 3, "third registration returns task 3");
    ck_u32(arkrtos_sched_ready_count(), 3u, "three tasks are ready");
    ck_i32(arkrtos_ready_top(), 1, "lowest priority number wins");
    ck(arkrtos_ready_contains(0) == 1u, "task 1 is on the ready chain");

    arkrtos_ready_del(1);
    ck(arkrtos_ready_contains(1) == 0u, "ready_del unlinks");
    ck_i32(arkrtos_ready_top(), 2, "the remaining prio-3 head is task 3");

    arkrtos_ready_add(1);
    ck(arkrtos_ready_contains(1) == 1u, "ready_add relinks");
    arkrtos_ready_add(1);
    ck_u32(arkrtos_sched_ready_count(), 3u, "ready_add is idempotent");

    /* 同优先级轮转：rotate 的入参是要让位的那个 idx（内核里传当前任务）。
     * 此时 prio-3 桶是 2 -> 1，队首是 idx 2，所以先转 2 才看得到队首变化。 */
    ck_i32(arkrtos_ready_top(), 2, "prio-3 head starts at idx 2");
    arkrtos_ready_rotate(2);
    ck_i32(arkrtos_ready_top(), 1, "rotate moves the head to the next task");
    arkrtos_ready_rotate(1);
    ck_i32(arkrtos_ready_top(), 2, "rotating the other one swaps back");

    /* 单节点桶：rotate 不动 */
    arkrtos_ready_rotate(0);
    ck_i32(arkrtos_ready_top(), 2, "a single-node bucket is not disturbed");

    /* 改优先级 */
    arkrtos_task_table[1].priority = 1u;
    arkrtos_ready_reprio(1, 3u);
    /* 优先级号更小者胜：搬到 prio 1 之后它就应当成为队首。 */
    ck_i32(arkrtos_ready_top(), 1, "reprio relinks into the new bucket");
    ck(arkrtos_ready_contains(1) == 1u, "reprio keeps the task on the chain");
    arkrtos_task_table[1].priority = 3u;
    arkrtos_ready_reprio(1, 1u);

    /* 不在就绪集时 reprio 什么都不做 */
    arkrtos_ready_del(1);
    arkrtos_task_table[1].priority = 2u;
    arkrtos_ready_reprio(1, 3u);
    ck(arkrtos_ready_contains(1) == 0u, "reprio on an unlinked task does nothing");

    /* 越界必须安全 */
    arkrtos_ready_add(-1);
    arkrtos_ready_add(99);
    arkrtos_ready_del(-1);
    arkrtos_ready_del(99);
    arkrtos_ready_rotate(99);
    arkrtos_ready_reprio(99, 3u);
    ck(arkrtos_ready_contains(-1) == 0u, "out-of-range contains is false");
    ck(arkrtos_ready_contains(99) == 0u, "out-of-range contains is false (high)");

    /* 重建：从 status 派生，不信任旧链 */
    arkrtos_ready_reset();
    ck_u32(arkrtos_sched_ready_count(), 0u, "reset clears the ready set");
    arkrtos_ready_rebuild();
    ck_u32(arkrtos_sched_ready_count(), 3u, "rebuild restores READY tasks");
    ck_i32(arkrtos_sched_check(), 0, "rebuild leaves an internally consistent set");

    /* 自愈：位图/桶头指向一个已经越界的下标（槽位被回收、位图没跟着清，
     * 正是 LoongArch64 上偶发 ADEM 的那个现场），ready_top 必须清位而不是
     * 把越界值当任务号返回。桶头是 task.c 内部 static，这里通过收缩
     * 任务表长度合法地造出「桶头 >= task_count」这一条件。 */
    {
        uint32 heal0 = arkrtos_ready_heal_cnt;

        /* 把 idx 2 搬到 prio 1，让它成为编号最小的桶的队首 */
        arkrtos_task_table[2].priority = 1u;
        arkrtos_ready_reprio(2, 3u);
        ck_i32(arkrtos_ready_top(), 2, "the moved task heads the lowest bucket");

        arkrtos_task_count = 2;              /* 下标 2 越界：模拟槽位被回收 */
        ck_i32(arkrtos_ready_top(), 1, "ready_top drops the out-of-range head");
        ck(arkrtos_ready_heal_cnt == heal0 + 1u, "the heal is recorded in the counter");
        ck_u32(arkrtos_ready_heal_head, 2u, "the heal keeps the offending head for diagnosis");

        arkrtos_task_count = 3;              /* 恢复现场 */
        arkrtos_ready_reset();
        arkrtos_ready_rebuild();
        ck_i32(arkrtos_ready_top(), 2, "rebuild after the heal picks the best task again");
        ck(arkrtos_sched_check() == 0, "after the heal the set is consistent again");
    }
}

/* ------------------------------------------------------------------
 * T3：延时链（有符号排序、跨回绕、僵尸节点、disarm）
 * ------------------------------------------------------------------ */
static void test_delay_chain(void)
{
    int32 t1;
    int32 t2;
    int32 t3;

    reset_all();
    t1 = make_task(4u, 0u);
    t2 = make_task(4u, 0u);
    t3 = make_task(4u, 0u);
    ck_i32(t1, 1, "the delay fixture registers task 1");
    ck_i32(t2, 2, "the delay fixture registers task 2");
    ck_i32(t3, 3, "the delay fixture registers task 3");

    /* 三个任务都置 WAIT 并 armed：10 / 20 / 30 拍 */
    arkrtos_task_table[0].status = ARKRTOS_TASK_WAIT;
    arkrtos_task_table[1].status = ARKRTOS_TASK_WAIT;
    arkrtos_task_table[2].status = ARKRTOS_TASK_WAIT;
    arkrtos_task_table[0].wait_time = 10;
    arkrtos_task_table[1].wait_time = 20;
    arkrtos_task_table[2].wait_time = 30;

    arkrtos_delay_arm(1, 20u);
    arkrtos_delay_arm(2, 30u);
    arkrtos_delay_arm(0, 10u);          /* 乱序插入 */

    arkrtos_kernel_tick = 9u;
    arkrtos_delay_tick();
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_WAIT, "nothing expires before its tick");

    arkrtos_kernel_tick = 10u;
    arkrtos_delay_tick();
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_READY, "the shortest sleep expires first");
    ck_i32(arkrtos_task_table[0].wake_reason, 1, "timeout wake reason is 1");
    ck_i32(arkrtos_task_table[0].wait_time, 0, "a one-shot wait clears wait_time");
    ck_i32(arkrtos_task_table[1].status, ARKRTOS_TASK_WAIT, "the later sleepers stay asleep");

    arkrtos_kernel_tick = 20u;
    arkrtos_delay_tick();
    ck_i32(arkrtos_task_table[1].status, ARKRTOS_TASK_READY, "the second sleep expires on time");

    /* 周期等待：wait_time<=0 时把下一次间隔复位成配置周期。
     * 这里曾经断言的是 period_time 30 -> 35（"补回一个周期"），而
     * wait_period_internal 拿 period_time 当"从现在起再睡多久"来 arm，
     * 于是周期变成 5/10/15/20… 逐次累加 —— 一个周期任务会越跑越慢。
     * 正确的不变量是"每次到期后间隔回到 period"。 */
    arkrtos_task_table[2].wait_time = 0;
    arkrtos_task_table[2].period = 5;
    arkrtos_task_table[2].period_time = 30;
    arkrtos_kernel_tick = 30u;
    arkrtos_delay_tick();
    ck_i32(arkrtos_task_table[2].status, ARKRTOS_TASK_READY, "periodic wait expires");
    ck_i32(arkrtos_task_table[2].period_time, 5,
           "periodic wait resets the interval to the configured period");

    /* 僵尸节点：已被提前唤醒（status != WAIT）的节点到期直接丢弃 */
    arkrtos_task_table[0].status = ARKRTOS_TASK_READY;
    arkrtos_delay_arm(0, 5u);
    arkrtos_task_table[0].status = ARKRTOS_TASK_READY;   /* 模拟 post 提前唤醒 */
    arkrtos_kernel_tick = 40u;
    arkrtos_delay_tick();
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_READY, "a zombie node is dropped, not re-added");
    ck_i32(arkrtos_task_table[0].delay_queued, 0, "the zombie node is unlinked");

    /* disarm 幂等 */
    arkrtos_delay_arm(1, 5u);
    ck_i32(arkrtos_task_table[1].delay_queued, 1, "arm marks the node queued");
    arkrtos_delay_arm(1, 7u);
    ck_i32(arkrtos_task_table[1].delay_queued, 1, "re-arm unlinks then relinks");
    arkrtos_delay_disarm(1);
    ck_i32(arkrtos_task_table[1].delay_queued, 0, "disarm unlinks");
    arkrtos_delay_disarm(1);
    ck_i32(arkrtos_task_table[1].delay_queued, 0, "disarm is idempotent");
    arkrtos_delay_disarm(99);
    arkrtos_delay_arm(99, 1u);
    ck(1, "out-of-range delay calls are safe");

    /* 跨 32 位回绕：接近回绕的超时仍必须排在远端 */
    reset_all();
    (void)make_task(6u, 0u);
    (void)make_task(6u, 0u);
    arkrtos_kernel_tick = 0xFFFFFFF0u;
    arkrtos_task_table[0].status = ARKRTOS_TASK_WAIT;
    arkrtos_task_table[1].status = ARKRTOS_TASK_WAIT;
    arkrtos_task_table[0].wait_time = 0x20;
    arkrtos_task_table[1].wait_time = 0x10;
    arkrtos_delay_arm(0, 0x20u);        /* 0x10 拍后回绕 */
    arkrtos_delay_arm(1, 0x10u);
    arkrtos_kernel_tick = 0u;           /* 回绕后 */
    arkrtos_delay_tick();
    ck_i32(arkrtos_task_table[1].status, ARKRTOS_TASK_READY,
           "the near deadline (before wrap) fires first");
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_WAIT,
           "the far deadline stays asleep across the wrap");
    arkrtos_kernel_tick = 0x10u;
    arkrtos_delay_tick();
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
           "the far deadline fires after the wrap");
}
/* ------------------------------------------------------------------
 * T4：调度器锁
 *
 * 锁的作用只有一条：把「切换决策」推迟到解锁那一刻。锁期间调阻塞接口
 * 属于编程错误——若照常睡下，负责解锁的任务自己就永久让出了 CPU。
 * 所以这里同时验证计数、可见性、解锁补偿，以及「锁定期阻塞被拒且记一条
 * 故障」。
 * ------------------------------------------------------------------ */
static void test_sched_lock(void)
{
    reset_all();
    (void)make_task(3u, 0u);
    use_task(1);

    arkrtos_sched_lock_internal();
    ck_u32(arkrtos_sched_lock_nest, 1u, "lock raises the nesting count");
    ck_i32(arkrtos_sched_lock_count_internal(), 1, "the count is readable");
    arkrtos_sched_lock_internal();
    ck_u32(arkrtos_sched_lock_nest, 2u, "lock nests");

    ck_u32(arkrtos_sched_unlock_internal(), 1u, "unlock returns the remaining depth");
    ck_i32(g_switch_n, 0, "unlocking with depth left does not switch");
    ck_u32(arkrtos_sched_unlock_internal(), 0u, "unlock reaches zero");
    ck_i32(g_switch_n, 1, "reaching zero triggers the deferred switch");

    /* 锁定期阻塞：三条路径各自被拒，任务状态不变，各留一条 SCHEDLOCK */
    arkrtos_sched_lock_internal();
    g_fault_n = 0;
    arkrtos_task_wait_internal(100u);
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
           "wait under the lock leaves the task READY");
    ck(fault_has(ARKRTOS_FAULT_SCHEDLOCK, 1), "wait under the lock records SCHEDLOCK");

    g_fault_n = 0;
    arkrtos_task_wait_period_internal();
    ck(fault_has(ARKRTOS_FAULT_SCHEDLOCK, 1),
       "wait_period under the lock records SCHEDLOCK");
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
           "wait_period under the lock leaves the task READY");

    g_fault_n = 0;
    arkrtos_task_block_internal();
    ck(fault_has(ARKRTOS_FAULT_SCHEDLOCK, 1),
       "block under the lock records SCHEDLOCK");
    ck_i32((int)arkrtos_task_table[0].wait_time, 0,
           "block under the lock does not mark an infinite wait");

    ck_i32(arkrtos_task_block_in_critical(10u), ARKRTOS_TASK_ERR_CONTEXT,
           "block_in_critical refuses while the scheduler is locked");
    arkrtos_sched_unlock_internal();

    /* 内核上下文（id == 0）也不允许阻塞 */
    use_task(0);
    ck_i32(arkrtos_task_block_in_critical(10u), ARKRTOS_TASK_ERR_CONTEXT,
           "block_in_critical refuses in the kernel context");
}

/* ------------------------------------------------------------------
 * T5：阻塞接口的每一条返回路径
 *
 * wait / wait_period / block / block_in_critical 四个入口的差别全在
 * wait_time 的取值和是否上延时链上；共同点是「先从就绪集摘出、再置 WAIT、
 * 再切换」。block_in_critical 另外要多验「交接令牌」与「诊断账」。
 * ------------------------------------------------------------------ */
static void test_blocking(void)
{
    reset_all();
    (void)make_task(3u, 50u);          /* period = 50 ms -> 100 ticks */
    use_task(1);

    arkrtos_task_wait_internal(100u);
    ck_i32((int)arkrtos_task_table[0].wait_time, 200, "wait converts ms to ticks");
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_WAIT, "wait parks the task");
    ck_u32(arkrtos_ready_contains(0), 0u, "wait removes the task from the ready set");
    ck(arkrtos_task_table[0].delay_queued == 1u, "wait arms the delay chain");
    ck_i32(g_trace_wait_n, 1, "wait emits the wait trace event");
    ck_i32(g_switch_n, 1, "wait asks for a switch");

    reset_all();
    (void)make_task(3u, 50u);
    use_task(1);
    arkrtos_task_table[0].period_time = 0;
    arkrtos_task_wait_period_internal();
    ck_i32((int)arkrtos_task_table[0].period_time, 100,
           "wait_period re-derives a zero period_time from period");
    ck_u32(arkrtos_task_table[0].delay_tick == 100u ? 1u : 0u, 1u,
           "wait_period arms the chain with the period");
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_WAIT,
           "wait_period parks the task");

    reset_all();
    (void)make_task(3u, 50u);
    use_task(1);
    arkrtos_task_block_internal();
    ck_i32((int)arkrtos_task_table[0].wait_time, -1,
           "block marks an infinite wait");
    ck_u32(arkrtos_task_table[0].delay_queued, 0u,
           "block does not arm the delay chain");
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_WAIT,
           "block parks the task");

    /* block_in_critical：显式唤醒（交接令牌）与超时两条返回路径 */
    reset_all();
    (void)make_task(3u, 0u);
    use_task(1);
    g_switch_sets_wake = ARKRTOS_WAKE_HANDOFF;
    ck_i32(arkrtos_task_block_in_critical(0u), ARKRTOS_WAKE_HANDOFF,
           "block_in_critical returns the hand-off reason");
    ck_i32((int)arkrtos_task_table[0].wait_time, -1,
           "timeout 0 means wait forever");
    ck_u32(arkrtos_task_table[0].delay_queued, 0u,
           "an infinite wait is not armed on the delay chain");
    ck_u32(arkrtos_dbg_wake_hits[0], 1u, "the wake account counts one wake");
    ck_u32(arkrtos_dbg_wake_last[0], (uint32)(ARKRTOS_WAKE_HANDOFF & 0x7Fu),
           "the wake account keeps the reason bits");

    reset_all();
    (void)make_task(3u, 0u);
    use_task(1);
    g_switch_sets_wake = -1;
    ck_i32(arkrtos_task_block_in_critical(100u), 0,
           "block_in_critical returns 0 when the switch brought no reason");
    ck_i32((int)arkrtos_task_table[0].wait_time, 200,
           "a finite timeout is converted to ticks");
    ck(arkrtos_task_table[0].delay_queued == 1u,
       "a finite timeout is armed on the delay chain");

    /* 对象等待账：声明了对象却没拿到交接令牌 -> 记一条 timeout */
    reset_all();
    (void)make_task(3u, 0u);
    use_task(1);
    g_trace_obj = 0x0123u;
    g_switch_sets_wake = -1;
    (void)arkrtos_task_block_in_critical(50u);
    ck_i32(g_obj_wait_n, 1, "the object registration is reported as a wait");
    ck_u32(g_obj_wait_obj, 0x0123u, "the object id travels as a sync record");
    ck_i32(g_obj_timeout_n, 1,
           "a wait without a hand-off reason closes with a timeout record");

    reset_all();
    (void)make_task(3u, 0u);
    use_task(1);
    g_trace_obj = 0x0456u;
    g_switch_sets_wake = ARKRTOS_WAKE_HANDOFF;
    (void)arkrtos_task_block_in_critical(0u);
    ck_i32(g_obj_timeout_n, 0,
           "a hand-off wake does not record an orphan timeout");

    /* delay 只透传到 port 层 */
    reset_all();
    arkrtos_task_delay_internal(1234u);
    ck_u32(g_delay_us_sum, 1234u, "delay forwards the microsecond count");
}/* ------------------------------------------------------------------
 * T5b：跨任务唤醒（arkrtos_task_wakeup_internal）
 *
 * 只认 WAIT：命中的任务打交接令牌、清 wait_time、置 READY、挂回就绪链
 * （ready_add 幂等，顺带把延时链摘掉）；READY / INVALID / 越界号一律 -1
 * 且一位不改——迟到的唤醒不能把一个已经在跑或已被收尸的任务叫醒。
 * ------------------------------------------------------------------ */
static void test_wakeup_internal(void)
{
    reset_all();
    (void)make_task(3u, 50u);
    use_task(1);
    arkrtos_task_wait_internal(100u);
    ck_u32(arkrtos_task_table[0].delay_queued, 1u,
         "wakeup fixture arms the delay chain");

    ck_i32(arkrtos_task_wakeup_internal(1), 0, "wakeup accepts a WAIT task");
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
           "wakeup makes the task READY");
    ck_i32((int)arkrtos_task_table[0].wake_reason, ARKRTOS_WAKE_HANDOFF,
           "wakeup sets the hand-off token");
    ck_i32((int)arkrtos_task_table[0].wait_time, 0,
           "wakeup clears the wait timeout");
    ck_u32(arkrtos_ready_contains(0), 1u,
           "wakeup puts the task back in the ready set");
    ck_u32(arkrtos_task_table[0].delay_queued, 0u,
           "wakeup detaches the delay chain");

    /* 迟到的唤醒：已经 READY，什么都不改 */
    ck_i32(arkrtos_task_wakeup_internal(1), -1,
           "wakeup refuses a task that is not WAIT");

    /* 越界任务号：0 与「大于任务数」，都在碰表之前就被拒 */
    ck_i32(arkrtos_task_wakeup_internal(0), -1,
           "wakeup refuses task id 0");
    ck_i32(arkrtos_task_wakeup_internal((int32)arkrtos_task_count + 1), -1,
           "wakeup refuses an id past the task table");
}

/* ------------------------------------------------------------------
 * T6：线程创建 / 自身 / 退出
 *
 * 创建有三道闸门：入口在调用方自己的代码区、栈在调用方自己的 RAM 区、
 * 优先级不是保留哨兵 255。通过之后还要继承创建者的窗口与特权——
 * 不继承的话新线程拿到的是「内核 Flash + 整片芯片 RAM + 特权」，
 * 恰好是 App 线程最不该有的那一组。
 * ------------------------------------------------------------------ */
static void test_thread_lifecycle(void)
{
    unsigned long fn = (unsigned long)(void (*)(void))sample_entry;
    int32 tid;

    reset_all();
    (void)make_task(3u, 0u);
    use_task(1);
    arkrtos_task_table[0].rom_start = (uint32)(fn & ~0xFFFUL);
    arkrtos_task_table[0].rom_size  = 0x4000u;
    arkrtos_task_table[0].ram_start = (uint32)(unsigned long)g_stack[1];
    arkrtos_task_table[0].ram_size  = 1024u;
    arkrtos_task_table[0].is_priv   = 0u;

    tid = arkrtos_thread_create_internal(sample_entry, g_stack[1], 1024u, 4u, 0u);
    ck_i32(tid, 2, "a valid thread is registered in the next slot");
    ck_i32((int)arkrtos_task_table[1].status, ARKRTOS_TASK_READY,
           "the new thread starts READY");
    ck_i32(g_task_create_trace_n, 1, "creation emits one trace record");
    ck_u32(g_task_create_slot, 1u, "the trace record uses the 0-based slot");
    ck_u32(g_task_create_entry, (uint32)fn, "the trace record carries the entry");
    ck_u32(arkrtos_task_table[1].rom_start, arkrtos_task_table[0].rom_start,
           "the thread inherits the creator's code window");
    ck_u32(arkrtos_task_table[1].ram_start, arkrtos_task_table[0].ram_start,
           "the thread inherits the creator's RAM window");
    ck_u32(arkrtos_task_table[1].is_priv, 0u,
           "the thread inherits the creator's privilege");

    /* 入口不在调用方窗口内 */
    ck_i32(arkrtos_thread_create_internal((void (*)(void))(fn + 0x100000UL),
                                          g_stack[2], 1024u, 4u, 0u),
           ARKRTOS_TASK_ERR_ENTRY, "an entry outside the caller window is refused");
    /* 栈越过调用方 RAM 窗口 */
    ck_i32(arkrtos_thread_create_internal(sample_entry, g_stack[1] + 512, 1024u, 4u, 0u),
           ARKRTOS_TASK_ERR_STACK, "a stack crossing the caller window is refused");
    /* 255 是调度器的「无候选」哨兵 */
    ck_i32(arkrtos_thread_create_internal(sample_entry, g_stack[1], 1024u, 255u, 0u),
           ARKRTOS_TASK_ERR_PRIORITY, "priority 255 is refused");

    /* self */
    ck_i32(arkrtos_thread_self_internal(), 1, "self reports the current task id");
    use_task(0);
    ck_i32(arkrtos_thread_self_internal(), 0, "self reports 0 in the kernel context");
    use_task(1);

    /* 带退出码自杀：码先落进 TCB，再走 kill 收尸 */
    arkrtos_thread_exit_code_internal(42);
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_INVALID,
           "exit_code marks the task INVALID");
    ck_i32((int)arkrtos_task_table[0].exit_code, 42, "exit_code stores the code");
    ck_u32(arkrtos_task_table[0].exit_code_set, 1u, "exit_code marks the code valid");
    ck_i32(g_task_exit_trace_n, 1, "exit emits one trace record");
    ck_i32(g_sync_release_n, 1, "exit releases sync registrations");
    ck_i32(g_mq_release_n, 1, "exit releases mq registrations");
    ck_i32(g_release_n, 1, "exit releases event registrations");
    ck_i32(g_net_release_n, 1, "exit releases net registrations");
    ck_i32(g_shell_release_n, 1, "exit gives shell command names back");
    ck_u32(g_shell_release_id, 1u, "the shell release names the dying task");
    ck_u32(arkrtos_kill_ret_cnt, 1u,
           "the stub switch does not take, so the diagnostic counts one return");

    {
        int32 code = 0;

        ck_i32(arkrtos_task_exit_code(1, &code), 1, "the exit code is readable");
        ck_i32(code, 42, "the exit code reads back");
        ck_i32(arkrtos_task_exit_code(0, &code), 0, "task id 0 has no exit code");
        ck_i32(arkrtos_task_exit_code((int32)ARKRTOS_TASK_MAX_NUM + 1, &code), 0,
               "an out-of-range id has no exit code");
    }

    /* 没设过退出码的任务读出 0 */
    reset_all();
    (void)make_task(3u, 0u);
    {
        int32 code = -7;

        ck_i32(arkrtos_task_exit_code(1, &code), 0,
               "a task that never set a code reports none");
        ck_i32(code, -7, "the output is untouched when there is no code");
    }

    /* 内核上下文里 exit 什么都不做 */
    reset_all();
    use_task(0);
    arkrtos_thread_exit_internal();
    ck_i32(g_task_exit_trace_n, 0, "exit in the kernel context emits no task record");
    ck_u32(arkrtos_kill_ret_cnt, 0u, "exit in the kernel context does not loop");
}

/* ------------------------------------------------------------------
 * T7：切换落地 arkrtos_sched_activate
 *
 * 它是 PendSV 的后半段：保存旧任务的栈指针/峰值、按状态决定要不要降级
 * 为 READY、在 RUNNING 上做两道越界判定（栈底保护字 + PSP < 栈底），
 * 再把新任务置 RUNNING 并写入 MPU / 特权。越界任务被摘除，而不是
 * 「带着坏现场继续跑」。
 * ------------------------------------------------------------------ */
static void test_sched_activate(void)
{
    unsigned long bottom;
    unsigned long old_psp;

    reset_all();
    (void)make_task(3u, 0u);
    (void)make_task(5u, 0u);
    use_task(1);
    bottom  = (unsigned long)arkrtos_task_table[0].stack_bottom;
    old_psp = bottom + 512u;
    arkrtos_task_table[0].status = ARKRTOS_TASK_RUNNING;

    ck_u32((uint32)arkrtos_sched_activate(2, old_psp),
           (uint32)arkrtos_task_table[1].stack_ptr,
           "activate returns the incoming task's stack pointer");
    ck_u32((uint32)arkrtos_task_table[0].stack_ptr, (uint32)old_psp,
           "activate stores the outgoing stack pointer");
    ck_u32((uint32)arkrtos_task_table[0].stack_peak_low, (uint32)old_psp,
           "activate records the new peak low-water mark");
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
           "a running task is downgraded to READY");
    ck_i32(arkrtos_current_task_id, 2, "the current task becomes the new one");
    ck_i32((int)arkrtos_task_table[1].status, ARKRTOS_TASK_RUNNING,
           "the incoming task is RUNNING");
    ck_u32(arkrtos_task_table[1].touch_tick, arkrtos_kernel_tick,
           "the incoming task's touch tick is refreshed");
    ck_i32(g_trace_switch_n, 1, "activate emits one switch record");
    ck(arkrtos_task_table[1].stack_peak_low <= arkrtos_task_table[1].stack_top,
       "peak never exceeds the top");

    /* 峰值只降不升 */
    reset_all();
    (void)make_task(3u, 0u);
    (void)make_task(5u, 0u);
    use_task(1);
    arkrtos_task_table[0].status = ARKRTOS_TASK_RUNNING;
    arkrtos_task_table[0].stack_peak_low = (unsigned long)arkrtos_task_table[0].stack_top - 100u;
    (void)arkrtos_sched_activate(2, (unsigned long)arkrtos_task_table[0].stack_top - 200u);
    ck_u32((uint32)arkrtos_task_table[0].stack_peak_low,
           (uint32)((unsigned long)arkrtos_task_table[0].stack_top - 200u),
           "a deeper low-water mark replaces the old one");
    reset_all();
    (void)make_task(3u, 0u);
    (void)make_task(5u, 0u);
    use_task(1);
    arkrtos_task_table[0].status = ARKRTOS_TASK_RUNNING;
    arkrtos_task_table[0].stack_peak_low = (unsigned long)arkrtos_task_table[0].stack_top - 200u;
    (void)arkrtos_sched_activate(2, (unsigned long)arkrtos_task_table[0].stack_top - 100u);
    ck_u32((uint32)arkrtos_task_table[0].stack_peak_low,
           (uint32)((unsigned long)arkrtos_task_table[0].stack_top - 200u),
           "a shallower sample keeps the historical low-water mark");

    /* PSP 掉到栈底以下：记故障并摘除 */
    reset_all();
    (void)make_task(3u, 0u);
    (void)make_task(5u, 0u);
    use_task(1);
    arkrtos_task_table[0].status = ARKRTOS_TASK_RUNNING;
    (void)arkrtos_sched_activate(2, (unsigned long)arkrtos_task_table[0].stack_bottom - 4u);
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_INVALID,
           "a PSP below the stack bottom kills the task");
    ck(fault_has(ARKRTOS_FAULT_STACKOVF, 1), "the stack overflow is recorded");
    ck_u32(arkrtos_ready_contains(0), 0u, "the overflowed task leaves the ready set");
    ck(board_out_has("[STKOVF] task 1"),
       "the overflow is announced on the board channel (not a silent INVALID)");
    ck(board_out_has(" bottom ") && board_out_has(" size "),
       "the announcement carries the boundary readings a reader needs");

    /* 栈界未知：clone(2) 线程只把栈顶交给内核（arkrtos_elf_thread），
     * 既没有栈底也没有长度。此时「psp < 栈底」恒真，任何一次 RUNNING
     * 期间被节拍抢占都会被静默判成溢出（§5.34 的根因）。未知边界不判。 */
    reset_all();
    (void)make_task(3u, 0u);
    (void)make_task(5u, 0u);
    use_task(1);
    arkrtos_task_table[0].status = ARKRTOS_TASK_RUNNING;
    arkrtos_task_table[0].stack_bottom = (uint32 *)0;
    arkrtos_task_table[0].stack_size = 0u;
    (void)arkrtos_sched_activate(2, 0x1000u);
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
           "an unknown stack boundary is not read as an overflow");
    ck(fault_has(ARKRTOS_FAULT_STACKOVF, 1) == 0,
       "no overflow is recorded when the boundary is unknown");
    ck(board_out_has("[STKOVF]") == 0, "and nothing is printed either");

    /* 半已知（有栈底但长度为 0）同样不判：长度 0 说明「不知道有多长」 */
    reset_all();
    (void)make_task(3u, 0u);
    (void)make_task(5u, 0u);
    use_task(1);
    arkrtos_task_table[0].status = ARKRTOS_TASK_RUNNING;
    arkrtos_task_table[0].stack_size = 0u;
    (void)arkrtos_sched_activate(2, (unsigned long)arkrtos_task_table[0].stack_bottom - 4u);
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
           "a zero length is not read as an overflow either");

    /* 栈底保护字被踩：同样摘除 */
    reset_all();
    (void)make_task(3u, 0u);
    (void)make_task(5u, 0u);
    use_task(1);
    arkrtos_task_table[0].status = ARKRTOS_TASK_RUNNING;
    arkrtos_task_table[0].stack_bottom[0] = 0u;
    (void)arkrtos_sched_activate(2, (unsigned long)arkrtos_task_table[0].stack_top - 16u);
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_INVALID,
           "a clobbered guard word kills the task");
    ck(fault_has(ARKRTOS_FAULT_STACKOVF, 1),
       "the guard hit is recorded as a stack overflow");

    /* old_psp == 0（故障恢复路径）：不写回、不做越界判定、状态不降级 */
    reset_all();
    (void)make_task(3u, 0u);
    use_task(1);
    arkrtos_task_table[0].status = ARKRTOS_TASK_RUNNING;
    arkrtos_task_table[0].stack_ptr = 0x1234u;
    (void)arkrtos_sched_activate(0, 0u);
    ck_u32((uint32)arkrtos_task_table[0].stack_ptr, 0x1234u,
           "old_psp 0 leaves the stored frame alone");
    ck_i32((int)arkrtos_task_table[0].status, ARKRTOS_TASK_RUNNING,
           "old_psp 0 does not downgrade the faulting task");

    /* 切到 idle：保存 idle 帧、恢复 idle MPU、把特权归给内核 */
    reset_all();
    use_task(0);
    ck_u32((uint32)arkrtos_sched_activate(0, 0x2000u), 0x2000u,
           "the idle path returns the saved idle frame");
    ck_i32(g_mpu_idle_n, 1, "the idle path restores the idle MPU context");
    ck_u32(g_priv_last, 1u, "the idle task runs privileged");
    ck_i32(g_mpu_app_n, 0, "the idle path does not install a task MPU context");

    /* 越界任务号：拒绝并留自愈计数 */
    reset_all();
    (void)make_task(3u, 0u);
    arkrtos_ready_heal_cnt = 0u;
    ck_u32((uint32)arkrtos_sched_activate(-1, 0x100u), 0u,
           "a negative task id is refused");
    ck_u32(arkrtos_ready_heal_cnt, 1u, "the refusal is counted");
    ck_u32((uint32)arkrtos_sched_activate(99, 0x100u), 0u,
           "a task id past task_count is refused");
}

/* ------------------------------------------------------------------
 * T8：栈用量（扫描 + 峰值两路）
 *
 * 两路读数分开报：只给合成值分不清是「栈真用满了」还是「记录/扫描坏了」，
 * 这两种结论对应完全不同的处置。
 * ------------------------------------------------------------------ */
static void test_stack_usage(void)
{
    uint32 out4[4];
    uint32 out3[4];

    reset_all();
    (void)make_task(3u, 0u);

    out4[0] = out4[1] = out4[2] = out4[3] = 0u;
    ck_i32(arkrtos_task_stack_info_raw(1, out4), 0, "stack_info_raw succeeds");
    ck_u32(out4[0], 1024u, "total stack bytes");
    ck_u32(out4[2], 4u, "only the guard word is touched, so the scan reports 4 bytes");
    ck_u32(out4[3], 0u, "no switch has happened, so the PSP side reports 0");

    /* 触碰 10 个字 + 让峰值掉 100 字节：合成值取较大者 */
    arkrtos_task_table[0].stack_bottom[10] = 0xA5A5A5A5u;
    arkrtos_task_table[0].stack_peak_low =
        (unsigned long)arkrtos_task_table[0].stack_top - 100u;
    (void)arkrtos_task_stack_info_raw(1, out4);
    ck_u32(out4[2], 1024u - 9u * 4u, "the scan counts words up to the first dirty one");
    ck_u32(out4[3], 100u, "the PSP side reports the peak distance");
    ck_u32(out4[1], 1024u - 36u, "the combined value takes the larger of the two");

    /* 峰值落到栈底以下：合成值封顶为总大小 */
    arkrtos_task_table[0].stack_peak_low =
        (unsigned long)arkrtos_task_table[0].stack_bottom - 500u;
    (void)arkrtos_task_stack_info_raw(1, out4);
    ck_u32(out4[1], 1024u, "the combined value is capped at the stack size");

    ck_i32(arkrtos_task_stack_info_raw(1, 0), ARKRTOS_TASK_ERR_PARAM,
           "a null output is refused");
    ck_i32(arkrtos_task_stack_info_raw(0, out4), ARKRTOS_TASK_ERR_PARAM,
           "task id 0 is refused");
    ck_i32(arkrtos_task_stack_info_raw((int32)ARKRTOS_TASK_MAX_NUM + 1, out4),
           ARKRTOS_TASK_ERR_PARAM, "an out-of-range id is refused");
    arkrtos_task_table[0].stack_size = 0u;
    ck_i32(arkrtos_task_stack_info_raw(1, out4), ARKRTOS_TASK_ERR_NOSTATE,
           "a task without a stack has no state to report");

    /* 3 字版本：剩余 = 总 - 合成 */
    reset_all();
    (void)make_task(3u, 0u);
    out3[0] = out3[1] = out3[2] = out3[3] = 0u;
    ck_i32(arkrtos_task_stack_info_internal(1, out3), 0, "stack_info succeeds");
    ck_u32(out3[0], 1024u, "stack_info reports the total");
    ck_u32(out3[1], 4u, "stack_info reports the used bytes");
    ck_u32(out3[2], 1020u, "stack_info derives the free bytes");
    ck_i32(arkrtos_task_stack_info_internal(1, 0), ARKRTOS_TASK_ERR_PARAM,
           "stack_info refuses a null output");
}

/* ------------------------------------------------------------------
 * T9：栈守卫（任务槽位 + 启动/idle 上下文）
 *
 * 任务侧看的是「栈底那个字还是不是 END_FLAG」——这与「保存的 PSP 是否
 * 低于栈底」不是同一件事：陷阱帧、野指针、DMA 写穿都能改掉栈底字而
 * 保存下来的 PSP 仍在上方。上下文侧看的是「最低的非图案字」，因为那块
 * 栈不属于任何槽位。
 * ------------------------------------------------------------------ */
static void test_stack_guard(void)
{
    uint32 info[4];
    static uint32 ctx[128];            /* 512 字节，字对齐 */

    reset_all();
    (void)make_task(3u, 0u);

    ck_i32(arkrtos_stack_guard_check(-1), 0, "a negative slot is ignored");
    ck_i32(arkrtos_stack_guard_check(0), 0, "an intact guard word reports clean");

    arkrtos_task_table[0].stack_bottom[0] = 0xDEADu;
    console_reset();
    ck_i32(arkrtos_stack_guard_check(0), 1, "a clobbered guard word is reported");
    ck_i32(g_fault_n, 1, "the first hit records one fault");
    ck(fault_has(ARKRTOS_FAULT_STACKOVF, 1), "the fault names the owning task");
    ck(console_has("[STK] task 1: stack bottom flag overwritten"),
       "the console carries the task number");
    ck_i32(arkrtos_stack_guard_check(0), 1, "the hit keeps reporting");
    ck_i32(g_fault_n, 1, "but only one fault record is produced");

    /* 未布防的任务不参与判定 */
    arkrtos_task_table[0].stack_bottom[0] = 0u;
    arkrtos_task_table[0].stack_guard_armed = 0u;
    ck_i32(arkrtos_stack_guard_check(0), 0, "an unarmed task is not checked");
    arkrtos_task_table[0].stack_guard_armed = 1u;
    arkrtos_task_table[0].stack_guard_reported = 0u;
    arkrtos_task_table[0].stack_bottom[0] = 0u;

    /* check_current 走当前任务号 */
    use_task(1);
    ck_i32(arkrtos_stack_guard_check(0), 1, "the task is still reported");
    use_task(0);
    arkrtos_stack_guard_check_current();
    ck_i32(g_fault_n, 2, "the current-task helper does nothing in the kernel context");

    /* 上下文守卫参数不成立：不布防（半截守卫比没有守卫更坏） */
    ck_i32(arkrtos_stack_guard_context_info(info), -1, "nothing is armed yet");
    arkrtos_stack_guard_watch_context((uint32)(unsigned long)ctx, 128u,
                                      (uint32)(unsigned long)ctx + 512u);
    ck_i32(arkrtos_stack_guard_context_info(info), -1,
           "too small a stack is not armed");
    arkrtos_stack_guard_watch_context(0x20000002u, 512u, 0x20000202u);
    ck_i32(arkrtos_stack_guard_context_info(info), -1,
           "a misaligned stack is not armed");
    arkrtos_stack_guard_watch_context(0x20001000u, 512u, 0x20001000u);
    ck_i32(arkrtos_stack_guard_context_info(info), -1,
           "an empty range is not armed");
    arkrtos_stack_guard_watch_context(0x20001000u, 256u, 0x20001200u);
    ck_i32(arkrtos_stack_guard_context_info(info), -1,
           "a window larger than the declared size is not armed");

    /* 正常布防：info 报告四元组 */
    memset(ctx, 0, sizeof(ctx));
    arkrtos_stack_guard_watch_context((uint32)(unsigned long)ctx, 512u,
                                      (uint32)(unsigned long)ctx + 512u);
    ck_i32(arkrtos_stack_guard_context_info(info), 0, "a valid stack is armed");
    ck_u32(info[0], (uint32)(unsigned long)ctx, "info reports the bottom");
    ck_u32(info[1], (uint32)(unsigned long)ctx + 512u, "info reports the top");
    ck_u32(info[2], (uint32)(unsigned long)ctx + 512u, "the low-water mark starts at the top");
    /* info[3] = 已知水位距栈底的距离（即仍可用的字节数）：刚填完图案、
     * 一个字都没被写过，它就等于整块大小。随着栈被压低这个数只会变小，
     * 触底为 0（下面 overflow 那条）。 */
    ck_u32(info[3], 512u, "the free bytes start at the whole window");

    /* 顶部写一个非图案字：还差 508 字节，不报警 */
    g_fault_n = 0;
    ctx[127] = 0xA5A5A5A5u;
    arkrtos_stack_guard_check_context();
    ck_i32(g_fault_n, 0, "a shallow touch stays quiet");
    ck_i32(arkrtos_stack_guard_context_info(info), 0, "still armed");
    ck_u32(info[2], (uint32)(unsigned long)&ctx[127], "the low-water mark follows the touch");

    /* 离栈底只剩 8 字节：低余量报警 */
    console_reset();
    ctx[2] = 0x5A5A5A5Au;
    arkrtos_stack_guard_check_context();
    ck(fault_has(ARKRTOS_FAULT_STACKOVF, 0), "a low margin is recorded");
    ck(console_has("[STK] context stack low margin"), "the console says low margin");
    ck(console_has("margin=8B"), "the console reports the measured margin");

    /* 只报一次 */
    g_fault_n = 0;
    ctx[1] = 0x5A5A5A5Au;
    arkrtos_stack_guard_check_context();
    ck_i32(g_fault_n, 0, "the context guard reports only once");

    /* 重新布防后触底：报 overflow */
    arkrtos_stack_guard_watch_context((uint32)(unsigned long)ctx, 512u,
                                      (uint32)(unsigned long)ctx + 512u);
    console_reset();
    g_fault_n = 0;
    ctx[0] = 0x5A5A5A5Au;
    arkrtos_stack_guard_check_context();
    ck(fault_has(ARKRTOS_FAULT_STACKOVF, 0), "an overflow is recorded");
    ck(console_has("[STK] context stack overflow"), "the console says overflow");
    ck_i32(arkrtos_stack_guard_context_info(info), 0, "it stays armed after a hit");
    ck_u32(info[3], 0u, "the measured margin is zero");
}
/* ------------------------------------------------------------------
 * T10：任务热切换（A/B 双缓冲无缝升级的内核一侧）
 *
 * 这里量的是 task.c 自己的四步：整块搬、端口层改帧、端口层落帧、
 * TCB/MPU 跟上。端口层全部打桩，所以「改对了没有」由桩收到的指针
 * 与长度来判，而不是看某个真寄存器。
 * ------------------------------------------------------------------ */
/* 块按字节算：1024 个 uint32 = 4096 字节，MPU region 要按 4096 对齐。 */
static uint32 g_old_blk[1024] __attribute__((aligned(4096)));
static uint32 g_new_blk[1024] __attribute__((aligned(4096)));
static uint32 g_small_blk[512] __attribute__((aligned(2048)));

static void setup_swap_task(void)
{
    arkrtos_task_t *p;

    reset_all();
    arkrtos_task_count = 1;
    p = &arkrtos_task_table[0];
    memset(p, 0, sizeof(*p));
    p->status        = ARKRTOS_TASK_READY;
    p->ram_start     = (unsigned long)g_old_blk;
    p->ram_size      = 4096u;
    p->stack_size    = 4096u;
    p->stack_top     = (unsigned long)g_old_blk + 4096u;
    p->stack_bottom  = g_old_blk;
    p->stack_peak_low = p->stack_top;
    p->rom_start     = 0x00020000ul;
    p->rom_size      = 0x1000u;
    p->entry         = sample_entry;
    p->priority      = 5u;
    p->base_priority = 5u;
    p->slice_tick    = (int32)ARKRTOS_TIME_SLICE_TICKS;
    arkrtos_ready_reset();
    arkrtos_ready_add(0);
}

static void test_task_swap(void)
{
    uint32 *p_frame = (uint32 *)(void *)((uint8 *)g_old_blk + 0x400);
    uint32 i;
    int32 rc;

    /* ---- arm 的拒绝分支 ---- */
    setup_swap_task();

    ck_i32(arkrtos_task_swap_arm(0, 0x40000ul, 0x1000u,
                                 (unsigned long)g_new_blk, 4096u, 0x1234u),
           ARKRTOS_SWAP_ERR_PARAM, "task 0 is rejected");
    ck_i32(arkrtos_task_swap_arm(2, 0x40000ul, 0x1000u,
                                 (unsigned long)g_new_blk, 4096u, 0x1234u),
           ARKRTOS_SWAP_ERR_PARAM, "a task beyond the count is rejected");
    ck_i32(arkrtos_task_swap_arm(1, 0ul, 0x1000u,
                                 (unsigned long)g_new_blk, 4096u, 0x1234u),
           ARKRTOS_SWAP_ERR_PARAM, "a zero rom window is rejected");
    ck_i32(arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                                 (unsigned long)g_new_blk, 4096u, 0ul),
           ARKRTOS_SWAP_ERR_PARAM, "a zero entry is rejected");
    ck_i32(arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                                 (unsigned long)g_new_blk + 4u, 4096u, 0x1234u),
           ARKRTOS_SWAP_ERR_PARAM, "a misaligned ram block is rejected");
    ck_i32(arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                                 (unsigned long)g_new_blk, 1000u, 0x1234u),
           ARKRTOS_SWAP_ERR_PARAM, "a block size that is not a power of two is rejected");
    ck_i32(arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                                 (unsigned long)g_small_blk, 2048u, 0x1234u),
           ARKRTOS_SWAP_ERR_LAYOUT, "a different block size is a layout mismatch");

    /* 目标任务已被判无效 */
    arkrtos_task_table[0].status = ARKRTOS_TASK_INVALID;
    ck_i32(arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                                 (unsigned long)g_new_blk, 4096u, 0x1234u),
           ARKRTOS_SWAP_ERR_PARAM, "an invalid task is rejected");
    arkrtos_task_table[0].status = ARKRTOS_TASK_READY;

    /* 同窗口还有活线程 */
    arkrtos_task_count = 2;
    memset(&arkrtos_task_table[1], 0, sizeof(arkrtos_task_table[1]));
    arkrtos_task_table[1].status    = ARKRTOS_TASK_READY;
    arkrtos_task_table[1].rom_start = 0x00020000ul;
    arkrtos_task_table[1].rom_size  = 0x1000u;
    ck_i32(arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                                 (unsigned long)g_new_blk, 4096u, 0x1234u),
           ARKRTOS_SWAP_ERR_THREAD, "a sibling thread in the same window is rejected");
    arkrtos_task_count = 1;

    /* 挂起 / 排队 / 取消 */
    ck_i32(arkrtos_task_swap_pending(), 0, "nothing is pending at first");
    ck_i32(arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                                 (unsigned long)g_new_blk, 4096u, 0x1234u),
           0, "a well formed swap is armed");
    ck_i32(arkrtos_task_swap_pending(), 1, "the pending swap names task 1");
    ck_i32(arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                                 (unsigned long)g_new_blk, 4096u, 0x1234u),
           ARKRTOS_SWAP_ERR_BUSY, "a second swap is refused while one is in flight");
    ck_i32(arkrtos_task_swap_cancel(2), ARKRTOS_SWAP_ERR_PARAM,
           "cancel for another task is refused");
    ck_i32(arkrtos_task_swap_cancel(0), 0, "cancel with 0 drops the pending swap");
    ck_i32(arkrtos_task_swap_pending(), 0, "nothing is pending after cancel");
    ck_i32(arkrtos_task_swap_cancel(0), ARKRTOS_SWAP_ERR_PARAM,
           "cancel with nothing pending is refused");

    /* ---- take 的分支 ---- */
    ck_i32(arkrtos_task_swap_take(p_frame, 1), 0, "take with nothing pending is a no-op");

    arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                         (unsigned long)g_new_blk, 4096u, 0x1234u);
    ck_i32(arkrtos_task_swap_take(0, 1), 0, "take without a frame is a no-op");
    ck_i32(arkrtos_task_swap_pending(), 1, "the no-op keeps the swap pending");
    ck_i32(arkrtos_task_swap_take(p_frame, 2), 0, "take for another task is a no-op");
    ck_i32(arkrtos_task_swap_pending(), 1, "still pending after a mismatched take");
    ck_i32(arkrtos_task_swap_take((uint32 *)(void *)g_new_blk, 1), 0,
           "a frame outside the old block is a no-op");
    ck_i32(arkrtos_task_swap_pending(), 1, "still pending after an out of block frame");

    /* 块大小在 take 时又对不上：arm 自己按 TCB 尺寸挡掉不同大小的块，
     * 所以这条只能让窗口在「arm 之后、take 之前」变化（目标任务被换成了
     * 另一份窗口尺寸的块），由 take 再复核一次并清账。 */
    arkrtos_task_swap_cancel(0);
    arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                         (unsigned long)g_new_blk, 4096u, 0x1234u);
    arkrtos_task_table[0].ram_size = 2048u;      /* arm 之后窗口变了 */
    ck_i32(arkrtos_task_swap_take(p_frame, 1), ARKRTOS_SWAP_ERR_LAYOUT,
           "take notices a block size mismatch");
    ck_i32(arkrtos_task_swap_pending(), 0, "a layout mismatch clears the pending swap");
    arkrtos_task_table[0].ram_size = 4096u;      /* 恢复现场 */

    /* 帧形状被端口层拒绝 */
    arkrtos_task_swap_arm(1, 0x40000ul, 0x1000u,
                         (unsigned long)g_new_blk, 4096u, 0x1234u);
    g_frame_ok = 0;
    ck_i32(arkrtos_task_swap_take(p_frame, 1), ARKRTOS_SWAP_ERR_FRAME,
           "a frame the port rejects is refused");
    ck_i32(arkrtos_task_swap_pending(), 0, "a frame refusal clears the pending swap");
    g_frame_ok = 1;

    /* ---- 成功路径 ---- */
    setup_swap_task();
    for(i = 0u; i < 1024u; i++)
    {
        g_old_blk[i] = 0xA0000000u + i;
        g_new_blk[i] = 0xDEADBEEFu;
    }
    g_swap_bytes = 32;

    arkrtos_task_swap_arm(1, 0x00040000ul, 0x2000u,
                         (unsigned long)g_new_blk, 4096u, 0x00001001u);
    rc = arkrtos_task_swap_take(p_frame, 1);
    ck_i32(rc, 1, "the take reports a completed swap");
    ck_i32(arkrtos_task_swap_pending(), 0, "the pending swap is cleared");

    ck_u32(g_new_blk[0], 0xA0000000u, "the whole old block is copied (head)");
    ck_u32(g_new_blk[256], 0xA0000000u + 256u, "the whole old block is copied (frame slot)");
    ck_u32(g_new_blk[1023], 0xA0000000u + 1023u, "the whole old block is copied (tail)");

    ck_i32(g_swap_rewrite_n, 1, "the port rewrites exactly one frame");
    ck_u32((uint32)(unsigned long)g_swap_rewrite_frame,
           (uint32)(unsigned long)&g_new_blk[256],
           "the frame is rewritten in the new block at the same offset");
    ck_u32(g_swap_rewrite_entry, 0x00001001u, "the entry handed over is the new one");

    ck_i32(g_swap_activate_n, 1, "the port is told to resume on the new frame");
    ck_u32((uint32)(unsigned long)g_swap_activate_old,
           (uint32)(unsigned long)p_frame, "the old frame is passed to activate");
    ck_u32((uint32)(unsigned long)g_swap_activate_new,
           (uint32)(unsigned long)&g_new_blk[256], "the new frame is passed to activate");

    ck_u32((uint32)arkrtos_task_table[0].rom_start, 0x00040000u, "the TCB takes the new rom window");
    ck_u32(arkrtos_task_table[0].rom_size, 0x2000u, "the TCB takes the new rom size");
    ck_u32((uint32)arkrtos_task_table[0].ram_start, (uint32)(unsigned long)g_new_blk,
           "the TCB takes the new ram block");
    ck_u32(arkrtos_task_table[0].ram_size, 4096u, "the TCB keeps the block size");
    ck_u32((uint32)arkrtos_task_table[0].stack_top,
           (uint32)(unsigned long)g_new_blk + 4096u, "the stack top follows the block");
    ck_u32((uint32)(unsigned long)arkrtos_task_table[0].stack_bottom,
           (uint32)(unsigned long)((uint8 *)g_new_blk + 0u), "the stack bottom follows the block");
    ck_u32((uint32)(unsigned long)arkrtos_task_table[0].entry, 0x00001001u,
           "the TCB entry is the new image");
    ck_u32((uint32)arkrtos_task_table[0].stack_ptr,
           (uint32)(unsigned long)&g_new_blk[256], "the resume frame points into the new block");
    ck_u32((uint32)arkrtos_task_table[0].stack_peak_low,
           (uint32)(unsigned long)g_new_blk + 4096u, "the peak watermark restarts at the top");
    ck_i32(g_mpu_build_n > 0, 1, "the MPU context is rebuilt");
    ck_i32(g_mpu_app_n, 1, "the MPU context is loaded right away");

    /* ---- 内核自检直连钩子 ---- */
    setup_swap_task();
    arkrtos_current_task_id = 2;
    arkrtos_task_swap_selftest_arm(1);
    ck_i32(arkrtos_task_swap_selftest_take(p_frame), 0,
           "the self test stays out of another task's SVC");
    arkrtos_current_task_id = 1;
    ck_i32(arkrtos_task_swap_selftest_take(p_frame), 0,
           "without a pending swap the self test is a no-op");

    arkrtos_task_swap_arm(1, 0x00040000ul, 0x1000u,
                         (unsigned long)g_new_blk, 4096u, 0x2000u);
    ck_i32(arkrtos_task_swap_selftest_take(p_frame), 1,
           "the self test drives the pending swap on its own task");
    ck_i32(arkrtos_task_swap_selftest_take(p_frame), 0,
           "the self test disarms itself after a completed swap");

    /* 负值也算终局：拒绝后注销，不重复尝试 */
    setup_swap_task();
    arkrtos_current_task_id = 1;
    arkrtos_task_swap_selftest_arm(1);
    arkrtos_task_swap_arm(1, 0x00040000ul, 0x1000u,
                         (unsigned long)g_new_blk, 4096u, 0x2000u);
    arkrtos_task_table[0].ram_size = 2048u;      /* arm 之后窗口变了 */
    ck_i32(arkrtos_task_swap_selftest_take(p_frame), ARKRTOS_SWAP_ERR_LAYOUT,
           "a refused self test swap reports the refusal");
    arkrtos_task_table[0].ram_size = 4096u;
    arkrtos_task_swap_arm(1, 0x00040000ul, 0x1000u,
                         (unsigned long)g_new_blk, 4096u, 0x2000u);
    ck_i32(arkrtos_task_swap_selftest_take(p_frame), 0,
           "a refused self test does not arm itself again");

    arkrtos_current_task_id = 0;
}

/* ------------------------------------------------------------------
 * T11：SVC 号表与每个服务的子命令分派
 *
 * 口径：这是「号 → 入口」的映射与参数块检查，不是各子系统内部语义
 * （那些在各自的宿主用例里）。每个号至少走一条成功路径和一条拒绝
 * 路径，才能把 switch 的两个方向都压到。
 * ------------------------------------------------------------------ */
static uint32 g_svcb[16];
static arkrtos_dev_drv_t g_drv_tbl;
static arkrtos_ushell_cmd_t g_shell_cmd;

static int cmd_stub(int argc, char **argv)
{
    (void)argc; (void)argv;
    return 0;
}

static int32 svc_with_block(uint32 num, uint32 sub)
{
    /* 参数块由调用者先填好（g_svcb[0] 是子命令，其余是参数）：
     * 这里清空会把调用者刚写进去的参数一起抹掉。 */
    memset(g_svc_args, 0, sizeof(g_svc_args));
    g_svcb[0] = sub;
    g_svc_args[0] = (unsigned long)(void *)g_svcb;
    g_svc_num = num;
    g_svc_ret = 0u;
    g_svc_ret_set = 0;
    SVC_Server(0);
    return (int32)g_svc_ret;
}

static int32 svc_direct(uint32 num, unsigned long a0, unsigned long a1,
                        unsigned long a2)
{
    memset(g_svc_args, 0, sizeof(g_svc_args));
    g_svc_args[0] = a0;
    g_svc_args[1] = a1;
    g_svc_args[2] = a2;
    g_svc_num = num;
    g_svc_ret = 0u;
    g_svc_ret_set = 0;
    SVC_Server(0);
    return (int32)g_svc_ret;
}

static void test_svc_dispatch(void)
{
    uint32 out[8];
    int32 rc;
    uint32 sub;
    int32 calls = 0;

    reset_all();
    /* 让栈上的参数块通过用户内存校验：这正是「App shell 回调在
     * 内核栈上跑」那一条的真实形态。 */
    arkrtos_kernel_ushell_active = 1;
    arkrtos_current_task_id = 0;
    memset(g_svcb, 0, sizeof(g_svcb));

    /* ---- EVENT ---- */
    memset(out, 0, sizeof(out));
    ck_i32(svc_with_block(ARKRTOS_SVC_EVENT_CTRL, ARKRTOS_EVENT_SUB_CREATE), -1,
           "an event create without a name is refused");
    g_svcb[1] = (unsigned long)(void *)out;
    ck_i32(svc_with_block(ARKRTOS_SVC_EVENT_CTRL, ARKRTOS_EVENT_SUB_CREATE), 101,
           "event create reaches the event module");
    g_svcb[1] = (unsigned long)(void *)out;
    g_svcb[2] = 50u;
    ck_i32(svc_with_block(ARKRTOS_SVC_EVENT_CTRL, ARKRTOS_EVENT_SUB_WAIT), 3,
           "event wait returns the module's answer");
    g_svcb[1] = (unsigned long)(void *)out;
    ck_i32(svc_with_block(ARKRTOS_SVC_EVENT_CTRL, ARKRTOS_EVENT_SUB_SET), 0,
           "event set returns 0");
    ck_i32(g_event_set_n, 1, "event set really reached the module");
    g_svc_args[0] = 0u;
    g_svc_num = ARKRTOS_SVC_EVENT_CTRL;
    SVC_Server(0);
    ck_i32((int32)g_svc_ret, -1, "a NULL parameter block is refused");
    ck_i32(svc_with_block(ARKRTOS_SVC_EVENT_CTRL, 9u), 0, "an unknown event subcommand is silent");
    calls += 6;

    /* ---- SYS ---- */
    arkrtos_kernel_tick = 100u;
    ck_i32(svc_direct(ARKRTOS_SVC_SYS_INFO, ARKRTOS_SYS_SUB_TIME, 0, 0), 50,
           "SYS time converts ticks to milliseconds");
    arkrtos_cpu_busy_ticks = 7u;
    ck_i32(svc_direct(ARKRTOS_SVC_SYS_INFO, ARKRTOS_SYS_SUB_CPU_BUSY, 0, 0), 7,
           "SYS cpu busy reports the last window");
    arkrtos_fault_record(ARKRTOS_FAULT_HARDFAULT, 3);
    ck_i32(svc_direct(ARKRTOS_SVC_SYS_INFO, ARKRTOS_SYS_SUB_FAULT_COUNT, 0, 0), 1,
           "SYS fault count sees the record");
    ck_i32(svc_direct(ARKRTOS_SVC_SYS_INFO, ARKRTOS_SYS_SUB_FAULT_GET, 0,
                      (unsigned long)(void *)out), 0, "SYS fault get copies a record");
    ck_u32(out[0], ARKRTOS_FAULT_HARDFAULT, "the fault type is copied");
    ck_u32(out[1], 3u, "the fault task id is copied");
    ck_i32(svc_direct(ARKRTOS_SVC_SYS_INFO, ARKRTOS_SYS_SUB_FAULT_GET, 5,
                      (unsigned long)(void *)out), -1, "an out of range record is refused");
    ck_i32(svc_direct(ARKRTOS_SVC_SYS_INFO, ARKRTOS_SYS_SUB_POOL_FREE,
                      (unsigned long)(void *)out, 0), 0x5678,
           "SYS pool free reports the total");
    ck_u32(out[0], 0x1234u, "SYS pool free reports the largest run");
    calls += 7;

    /* ---- TASK ---- */
    arkrtos_task_count = 1;
    memset(&arkrtos_task_table[0], 0, sizeof(arkrtos_task_table[0]));
    arkrtos_task_table[0].status = ARKRTOS_TASK_READY;
    ck_i32(svc_direct(ARKRTOS_SVC_TASK_CTRL, ARKRTOS_TASK_SUB_STATUS, 1, 0),
           (int32)ARKRTOS_TASK_READY, "TASK status returns the slot status");
    ck_i32(svc_direct(ARKRTOS_SVC_TASK_CTRL, ARKRTOS_TASK_SUB_STATUS, 9, 0),
           ARKRTOS_TASK_ERR_PARAM, "TASK status refuses an invalid slot");
    ck_i32(svc_direct(ARKRTOS_SVC_TASK_CTRL, ARKRTOS_TASK_SUB_SCHED_LOCK, 0, 0), 0,
           "TASK sched lock returns 0");
    ck_i32((int32)arkrtos_sched_lock_nest, 1, "the scheduler lock is held");
    ck_i32(svc_direct(ARKRTOS_SVC_TASK_CTRL, ARKRTOS_TASK_SUB_SCHED_LOCK_COUNT, 0, 0), 1,
           "TASK lock count reports one level");
    ck_i32(svc_direct(ARKRTOS_SVC_TASK_CTRL, ARKRTOS_TASK_SUB_SCHED_UNLOCK, 0, 0), 0,
           "TASK sched unlock reports zero levels left");
    ck_i32(g_switch_n, 1, "the unlock compensates with one switch");
    arkrtos_kernel_tick = 0u;
    /* 上面那个槽位的 TCB 是被 memset 清零的（stack_size == 0）：
     * 没有栈就没有读数，必须按参数错拒绝，而不是返回一堆 0。 */
    ck_i32(svc_direct(ARKRTOS_SVC_TASK_CTRL, ARKRTOS_TASK_SUB_STACK_INFO, 1,
                      (unsigned long)(void *)out), ARKRTOS_TASK_ERR_PARAM,
           "a slot without a stack is refused");
    {
        int32 tid = make_task(4u, 0u);       /* 真正注册：带 1 KB 栈 */
        memset(out, 0, sizeof(out));
        ck_i32(tid, 2, "the stacked task lands in the next slot");
        ck_i32(svc_direct(ARKRTOS_SVC_TASK_CTRL, ARKRTOS_TASK_SUB_STACK_INFO,
                          (unsigned long)tid, (unsigned long)(void *)out), 0,
               "TASK stack info fills the buffer");
        ck_u32(out[0], 1024u, "the total is the registered stack size");
        ck(out[1] <= 8u, "a fresh stack reports almost nothing used");
    }
    ck_i32(svc_direct(ARKRTOS_SVC_TASK_CTRL, ARKRTOS_TASK_SUB_WAIT, 10, 0), 0,
           "TASK wait from the kernel context is a no-op");
    calls += 8;

    /* ---- DEV ---- */
    memset(out, 0, sizeof(out));
    memset(g_svcb, 0, sizeof(g_svcb));
    g_svcb[0] = ARKRTOS_DEV_SUB_OPEN;
    g_svcb[1] = (unsigned long)(void *)out;
    g_svcb[2] = 1u;
    ck_i32(svc_with_block(ARKRTOS_SVC_DEV_IO, ARKRTOS_DEV_SUB_OPEN), 6,
           "DEV open reaches the device module");
    ck_i32(g_dev_open_n, 1, "DEV open is counted");
    g_svcb[1] = 6u;
    g_svcb[2] = (unsigned long)(void *)out;
    g_svcb[3] = 4u;
    ck_i32(svc_with_block(ARKRTOS_SVC_DEV_IO, ARKRTOS_DEV_SUB_READ), 0, "DEV read returns 0");
    ck_i32(g_dev_read_n, 1, "DEV read is counted");
    g_svcb[1] = 0x55u;   /* console handle */
    g_svcb[2] = (unsigned long)(const void *)"hi";
    g_svcb[3] = 2u;
    ck_i32(svc_with_block(ARKRTOS_SVC_DEV_IO, ARKRTOS_DEV_SUB_WRITE), 2,
           "DEV write to the console returns the byte count");
    ck(g_console_len == 2u, "the console really received the bytes");
    g_svcb[1] = 6u;
    g_svcb[2] = (unsigned long)(void *)out;
    g_svcb[3] = 4u;
    ck_i32(svc_with_block(ARKRTOS_SVC_DEV_IO, ARKRTOS_DEV_SUB_WRITE), 0,
           "DEV write to a plain handle returns 0");
    ck_i32(g_dev_write_n, 1, "DEV write is counted");
    g_svcb[1] = 6u;
    g_svcb[2] = 1u;
    g_svcb[3] = 2u;
    ck_i32(svc_with_block(ARKRTOS_SVC_DEV_IO, ARKRTOS_DEV_SUB_CTRL), 0, "DEV ctrl returns 0");
    ck_i32(g_dev_ctrl_n, 1, "DEV ctrl is counted");
    g_svcb[1] = 6u;
    ck_i32(svc_with_block(ARKRTOS_SVC_DEV_IO, ARKRTOS_DEV_SUB_CLOSE), 0, "DEV close returns 0");
    ck_i32(g_dev_close_n, 1, "DEV close is counted");
    g_svcb[1] = (unsigned long)(void *)out;
    ck_i32(svc_with_block(ARKRTOS_SVC_DEV_IO, ARKRTOS_DEV_SUB_SET_WILL), -1,
           "DEV set will is refused when hardware safe mode is off");
    ck_i32(svc_with_block(ARKRTOS_SVC_DEV_IO, 9u), 0, "an unknown DEV subcommand answers 0");
    calls += 10;

    /* ---- DRV ---- */
    memset(&g_drv_tbl, 0, sizeof(g_drv_tbl));
    memset(g_svcb, 0, sizeof(g_svcb));
    g_svcb[0] = ARKRTOS_DRV_SUB_REGISTER;
    g_svcb[1] = (unsigned long)(void *)out;
    g_svcb[2] = (unsigned long)(void *)&g_drv_tbl;
    g_svcb[3] = 1u;
    ck_i32(svc_with_block(ARKRTOS_SVC_DRV_MGR, ARKRTOS_DRV_SUB_REGISTER), 0,
           "DRV register reaches the driver table");
    g_svcb[1] = (unsigned long)(void *)out;
    ck_i32(svc_with_block(ARKRTOS_SVC_DRV_MGR, ARKRTOS_DRV_SUB_UNREGISTER), 0,
           "DRV unregister returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_DRV_MGR, ARKRTOS_DRV_SUB_COUNT), 3,
           "DRV count reports the module's number");
    ck_i32(svc_with_block(ARKRTOS_SVC_DRV_MGR, 9u), -1, "an unknown DRV subcommand is refused");
    calls += 4;

    /* ---- SYNC：16 条子命令各走一次 ---- */
    memset(g_svcb, 0, sizeof(g_svcb));
    g_svcb[1] = (unsigned long)(void *)out;
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_SEM_CREATE), 201,
           "SYNC sem create reaches the module");
    g_svcb[1] = 1u; g_svcb[2] = 10u;
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_SEM_WAIT), 0,
           "SYNC sem wait returns 0");
    g_svcb[1] = 1u;
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_SEM_POST), 0,
           "SYNC sem post returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_SEM_DELETE), 0,
           "SYNC sem delete returns 0");
    g_svcb[1] = (unsigned long)(void *)out;
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_MTX_CREATE), 202,
           "SYNC mutex create reaches the module");
    g_svcb[1] = 1u; g_svcb[2] = 10u;
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_MTX_LOCK), 0,
           "SYNC mutex lock returns 0");
    g_svcb[1] = 1u;
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_MTX_UNLOCK), 0,
           "SYNC mutex unlock returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_MTX_DELETE), 0,
           "SYNC mutex delete returns 0");
    g_svcb[1] = (unsigned long)(void *)out;
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_COND_CREATE), 203,
           "SYNC cond create reaches the module");
    g_svcb[1] = 1u; g_svcb[2] = 2u; g_svcb[3] = 3u;
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_COND_WAIT), 0,
           "SYNC cond wait returns 0");
    g_svcb[1] = 1u;
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_COND_SIGNAL), 0,
           "SYNC cond signal returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_COND_BROADCAST), 0,
           "SYNC cond broadcast returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_COND_DELETE), 0,
           "SYNC cond delete returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_COND_ENQUEUE), 0,
           "SYNC cond enqueue returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_COND_POLL), 0,
           "SYNC cond poll returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, ARKRTOS_SYNC_SUB_COND_ABORT), 0,
           "SYNC cond abort returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_SYNC_CTRL, 30u), -1, "an unknown SYNC subcommand is refused");
    calls += 17;

    /* ---- MQ ---- */
    memset(g_svcb, 0, sizeof(g_svcb));
    g_svcb[1] = (unsigned long)(void *)out;
    ck_i32(svc_with_block(ARKRTOS_SVC_MQ_CTRL, ARKRTOS_MQ_SUB_CREATE), 204,
           "MQ create reaches the module");
    g_svcb[1] = 1u;
    g_svcb[2] = (unsigned long)(void *)out;
    g_svcb[3] = 1u;
    g_svcb[4] = 5u;
    ck_i32(svc_with_block(ARKRTOS_SVC_MQ_CTRL, ARKRTOS_MQ_SUB_SEND), 0, "MQ send returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_MQ_CTRL, ARKRTOS_MQ_SUB_RECV), 0, "MQ recv returns 0");
    g_svcb[1] = 1u;
    ck_i32(svc_with_block(ARKRTOS_SVC_MQ_CTRL, ARKRTOS_MQ_SUB_DELETE), 0, "MQ delete returns 0");
    /* 消息长度 0 与超上限都被拒 */
    g_svcb[1] = 1u;
    g_svcb[2] = (unsigned long)(void *)out;
    g_svcb[3] = 0u;
    ck_i32(svc_with_block(ARKRTOS_SVC_MQ_CTRL, ARKRTOS_MQ_SUB_SEND), -1,
           "a zero length message is refused");
    g_svcb[3] = (unsigned long)(ARKRTOS_MQ_MSG_WORDS + 1u);
    ck_i32(svc_with_block(ARKRTOS_SVC_MQ_CTRL, ARKRTOS_MQ_SUB_SEND), -1,
           "a message longer than the queue slot is refused");
    ck_i32(svc_with_block(ARKRTOS_SVC_MQ_CTRL, 9u), -1, "an unknown MQ subcommand is refused");
    calls += 7;

    /* ---- SBUF / MEMPOOL：本档未编入，必须拒绝而不是假装成功 ---- */
    ck_i32(svc_with_block(ARKRTOS_SVC_SBUF_CTRL, 1u), -1,
           "the stream buffer service is refused when it is not built in");
    ck_i32(svc_with_block(ARKRTOS_SVC_MEMPOOL_CTRL, 1u), -1,
           "the memory pool service is refused when it is not built in");
    calls += 2;

    /* ---- TIMER ---- */
    memset(g_svcb, 0, sizeof(g_svcb));
    g_svcb[1] = (unsigned long)(void *)out;
    ck_i32(svc_with_block(ARKRTOS_SVC_TIMER_CTRL, ARKRTOS_TIMER_SUB_CREATE), 205,
           "TIMER create reaches the module");
    g_svcb[1] = 1u; g_svcb[2] = 100u; g_svcb[3] = 1u;
    g_svcb[4] = (unsigned long)(void *)sample_entry;
    g_svcb[5] = 0u;
    ck_i32(svc_with_block(ARKRTOS_SVC_TIMER_CTRL, ARKRTOS_TIMER_SUB_START), 0,
           "TIMER start reaches the module");
    g_svcb[1] = 1u;
    ck_i32(svc_with_block(ARKRTOS_SVC_TIMER_CTRL, ARKRTOS_TIMER_SUB_STOP), 0,
           "TIMER stop returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_TIMER_CTRL, ARKRTOS_TIMER_SUB_DELETE), 0,
           "TIMER delete returns 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_TIMER_CTRL, 9u), -1, "an unknown TIMER subcommand is refused");
    calls += 5;

    /* ---- THREAD ---- */
    ck_i32(svc_with_block(ARKRTOS_SVC_THREAD_CTRL, ARKRTOS_THREAD_SUB_SELF), 0,
           "THREAD self reports the kernel context");
    ck_i32(svc_with_block(ARKRTOS_SVC_THREAD_CTRL, ARKRTOS_THREAD_SUB_EXIT_CODE), 0,
           "THREAD exit code is accepted");
    memset(g_svcb, 0, sizeof(g_svcb));
    g_svcb[0] = ARKRTOS_THREAD_SUB_CREATE;
    g_svcb[3] = 1024u;
    ck_i32(svc_with_block(ARKRTOS_SVC_THREAD_CTRL, ARKRTOS_THREAD_SUB_CREATE), -1,
           "THREAD create without an entry is refused");
    ck_i32(svc_with_block(ARKRTOS_SVC_THREAD_CTRL, 9u), -1, "an unknown THREAD subcommand is refused");
    calls += 4;

    /* ---- LOG ---- */
    ck_i32(svc_direct(ARKRTOS_SVC_LOG, 3u, (unsigned long)(void *)out,
                      (unsigned long)(void *)out), 7, "LOG reaches the logger");
    ck_i32(g_log_svc_n, 1, "LOG is counted");
    ck_i32(svc_direct(ARKRTOS_SVC_LOG, 3u, 0, (unsigned long)(void *)out), -1,
           "a NULL log tag is refused");
    calls += 2;

    /* ---- SHELL ---- */
    memset(&g_shell_cmd, 0, sizeof(g_shell_cmd));
    g_shell_cmd.name = (const char *)out;
    g_shell_cmd.help = (const char *)out;
    g_shell_cmd.func = cmd_stub;
    memset(g_svcb, 0, sizeof(g_svcb));
    g_svcb[1] = (unsigned long)(void *)&g_shell_cmd;
    ck_i32(svc_with_block(ARKRTOS_SVC_SHELL, ARKRTOS_SHELL_SUB_REGISTER), 0,
           "SHELL register reaches the shell module");
    g_svcb[1] = (unsigned long)(void *)out;
    ck_i32(svc_with_block(ARKRTOS_SVC_SHELL, ARKRTOS_SHELL_SUB_UNREGISTER), 0,
           "SHELL unregister returns 0");
    g_svcb[1] = (unsigned long)(const void *)"hello";
    ck_i32(svc_with_block(ARKRTOS_SVC_SHELL, ARKRTOS_SHELL_SUB_PRINT), 5,
           "SHELL print returns the length");
    ck_i32(g_shell_print_n, 1, "SHELL print is counted");
    ck_i32(svc_with_block(ARKRTOS_SVC_SHELL, 9u), -1, "an unknown SHELL subcommand is refused");
    calls += 4;

    /* ---- HEARTBEAT ---- */
    memset(g_svcb, 0, sizeof(g_svcb));
    g_svcb[0] = ARKRTOS_HB_SUB_REPORT;
    g_svcb[1] = 1000u;
    ck_i32(svc_with_block(ARKRTOS_SVC_HEARTBEAT, ARKRTOS_HB_SUB_REPORT), 0,
           "HEARTBEAT report reaches the guard");
    ck_i32(g_guard_beat_n, 1, "HEARTBEAT report is counted");
    ck_i32(svc_with_block(ARKRTOS_SVC_HEARTBEAT, 9u), -1, "an unknown HEARTBEAT subcommand is refused");
    calls += 2;

    /* ---- FILE_SYS：卷 1..10 / 命名空间 11..23 / 越界 ---- */
    for(sub = ARKRTOS_FILE_SUB_WRITE; sub <= ARKRTOS_FILE_SUB_LIST; sub++)
    {
        uint32 size_out = 0u;
        uint32 dir_out = 0u;
        uint32 cnt_out = 0u;

        memset(g_svcb, 0, sizeof(g_svcb));
        g_svcb[0] = sub;
        g_svcb[1] = (unsigned long)(const void *)"/a.txt";
        /* 每个子命令的参数语义不同：长度值不能当输出指针用，
         * 否则内核会按调用者给的地址写回（sub=4 的 p[3] 是 is_dir 输出）。 */
        switch(sub)
        {
        case ARKRTOS_FILE_SUB_WRITE:
            g_svcb[2] = (unsigned long)(void *)out;
            g_svcb[3] = 4u;
            break;
        case ARKRTOS_FILE_SUB_READ:
            g_svcb[2] = (unsigned long)(void *)out;
            g_svcb[3] = 8u;
            break;
        case ARKRTOS_FILE_SUB_READ_AT:
            g_svcb[2] = (unsigned long)(void *)out;
            g_svcb[3] = 4u;
            g_svcb[4] = 0u;
            break;
        case ARKRTOS_FILE_SUB_STAT:
            g_svcb[2] = (unsigned long)(void *)&size_out;
            g_svcb[3] = (unsigned long)(void *)&dir_out;
            break;
        case ARKRTOS_FILE_SUB_INFO:
            g_svcb[1] = (unsigned long)(void *)&size_out;
            g_svcb[2] = (unsigned long)(void *)&dir_out;
            break;
        case ARKRTOS_FILE_SUB_RENAME:
            g_svcb[2] = (unsigned long)(const void *)"/b.txt";
            break;
        case ARKRTOS_FILE_SUB_LIST:
            g_svcb[1] = (unsigned long)(const void *)"/";
            g_svcb[2] = (unsigned long)(void *)out;
            g_svcb[3] = 32u;
            g_svcb[4] = (unsigned long)(void *)&cnt_out;
            break;
        default:
            /* remove / mount / unmount 只吃路径 */
            break;
        }
        g_svc_num = ARKRTOS_SVC_FILE_SYS;
        g_svc_ret = 0u;
        g_svc_ret_set = 0;
        SVC_Server(0);
        calls++;
        ck(g_svc_ret_set == 1, "every volume subcommand answers");
    }
    ck_i32((int32)g_svc_ret, 0, "the volume list answered 0");
    ck_i32(svc_with_block(ARKRTOS_SVC_FILE_SYS, 0u), ARKRTOS_VFS_EINVAL,
           "an unknown volume subcommand is EINVAL");

    for(sub = ARKRTOS_PATH_SUB_OPEN; sub <= 30u; sub++)
    {
        uint32 mode_out = 0u;
        uint32 size_out = 0u;

        memset(g_svcb, 0, sizeof(g_svcb));
        g_svcb[0] = sub;
        switch(sub)
        {
        case ARKRTOS_PATH_SUB_OPEN:
            g_svcb[1] = (unsigned long)(const void *)"/mnt/x";
            g_svcb[2] = 0u;
            break;
        case ARKRTOS_PATH_SUB_READ:
            g_svcb[1] = 1u;
            g_svcb[2] = (unsigned long)(void *)out;
            g_svcb[3] = 8u;
            break;
        case ARKRTOS_PATH_SUB_WRITE:
            g_svcb[1] = 1u;
            g_svcb[2] = (unsigned long)(void *)out;
            g_svcb[3] = 4u;
            break;
        case ARKRTOS_PATH_SUB_CLOSE:
            g_svcb[1] = 1u;
            break;
        case ARKRTOS_PATH_SUB_STAT:
            g_svcb[1] = (unsigned long)(const void *)"/mnt/x";
            g_svcb[2] = (unsigned long)(void *)&size_out;
            g_svcb[3] = (unsigned long)(void *)&mode_out;
            break;
        case ARKRTOS_PATH_SUB_SEEK:
            g_svcb[1] = 1u;
            g_svcb[2] = 0u;
            g_svcb[3] = 0u;
            break;
        case ARKRTOS_PATH_SUB_READDIR:
            g_svcb[1] = 1u;
            g_svcb[2] = (unsigned long)(void *)out;
            g_svcb[3] = 32u;
            g_svcb[4] = (unsigned long)(void *)&mode_out;
            g_svcb[5] = (unsigned long)(void *)&size_out;
            break;
        case ARKRTOS_PATH_SUB_UNLINK:
        case ARKRTOS_PATH_SUB_MKDIR:
        case ARKRTOS_PATH_SUB_RMDIR:
            g_svcb[1] = (unsigned long)(const void *)"/mnt/x";
            break;
        case ARKRTOS_PATH_SUB_RENAME:
            g_svcb[1] = (unsigned long)(const void *)"/mnt/a";
            g_svcb[2] = (unsigned long)(const void *)"/mnt/b";
            break;
        case ARKRTOS_PATH_SUB_TRUNCATE:
            g_svcb[1] = (unsigned long)(const void *)"/mnt/x";
            g_svcb[2] = 0u;
            break;
        case ARKRTOS_PATH_SUB_FTRUNCATE:
            g_svcb[1] = 1u;
            g_svcb[2] = 0u;
            break;
        default:
            /* 24..30：未知子命令，处理函数必须答 EINVAL */
            break;
        }
        g_svc_num = ARKRTOS_SVC_FILE_SYS;
        g_svc_ret = 0u;
        g_svc_ret_set = 0;
        SVC_Server(0);
        calls++;
        ck(g_svc_ret_set == 1, "every namespace subcommand answers");
        if(sub > ARKRTOS_PATH_SUB_FTRUNCATE)
        {
            ck_i32((int32)g_svc_ret, ARKRTOS_VFS_EINVAL,
                   "a namespace subcommand past ftruncate is EINVAL");
        }
    }

    /* ---- NET ---- */
    ck_i32(svc_with_block(ARKRTOS_SVC_NET, 1u), -3, "NET hands the call to the network module");
    ck_i32(g_net_svc_n, 1, "NET is counted");
    calls += 1;

    /* ---- APP ---- */
    ck_i32(svc_with_block(ARKRTOS_SVC_APP_MGR, ARKRTOS_APP_SUB_LOAD_DEV), 0,
           "APP load dev reaches the loader");
    ck_i32(svc_with_block(ARKRTOS_SVC_APP_MGR, ARKRTOS_APP_SUB_START), 0,
           "APP start reaches the loader");
    ck_i32(svc_with_block(ARKRTOS_SVC_APP_MGR, ARKRTOS_APP_SUB_STOP), 0,
           "APP stop reaches the loader");
    ck_i32(svc_with_block(ARKRTOS_SVC_APP_MGR, ARKRTOS_APP_SUB_STATUS), 1,
           "APP status reaches the loader");
    ck_i32(svc_with_block(ARKRTOS_SVC_APP_MGR, ARKRTOS_APP_SUB_DRIVER_LOAD), 0,
           "APP driver load reaches the loader");
    ck_i32(svc_with_block(ARKRTOS_SVC_APP_MGR, ARKRTOS_APP_SUB_MINI_RUN), -1,
           "APP mini run is refused when the mini app is not built in");
    ck_i32(svc_with_block(ARKRTOS_SVC_APP_MGR, 99u), -1, "an unknown APP subcommand is refused");
    calls += 7;

    /* ---- 未知 SVC 号与早期返回 ---- */
    memset(g_svc_args, 0, sizeof(g_svc_args));
    g_svc_num = 0x77u;
    g_svc_ret_set = 0;
    SVC_Server(0);
    ck_i32(g_svc_ret_set, 0, "an unknown SVC number writes no return value");

    g_guard_svc_n = 0;
    g_trace_isr_enter = 0;
    g_trace_isr_exit = 0;
    g_loader_swap_take_ret = 1;
    SVC_Server(0);
    ck_i32(g_guard_svc_n, 0, "a swap take short circuits before the liveness evidence");
    ck_i32(g_trace_isr_enter, 1, "the SVC enter trace is still paired");
    ck_i32(g_trace_isr_exit, 1, "the SVC exit trace is still paired");
    g_loader_swap_take_ret = 0;

    rc = (int32)g_guard_svc_n;
    ck_i32(rc, 0, "the short circuit leaves guard untouched");
    (void)calls;

    /* 全程 enter/exit 必须成对 */
    g_trace_isr_enter = 0;
    g_trace_isr_exit = 0;
    SVC_Server(0);
    ck_i32(g_trace_isr_enter, 1, "a plain SVC enters the trace once");
    ck_i32(g_trace_isr_exit, 1, "and exits it once");
    ck_i32(g_guard_svc_n, 1, "a plain SVC reports liveness");

    arkrtos_kernel_ushell_active = 0;
}

/* ------------------------------------------------------------------
 * T12：节拍体与时间片
 * ------------------------------------------------------------------ */
static void test_tick_and_slice(void)
{
    reset_all();
    make_task(5u, 0u);
    make_task(5u, 0u);
    use_task(1);
    arkrtos_task_table[0].slice_tick = 1;

    arkrtos_kernel_tick_handler();
    ck_u32(arkrtos_kernel_tick, 1u, "the tick advances");
    ck_i32(g_guard_tick_n, 1, "the guard sees every tick first");
    ck_i32(g_timer_tick_n, 1, "the timer module sees every tick");
    ck_u32(arkrtos_cpu_load_counter, 1u, "a real task on the CPU counts as busy");
    ck_i32(arkrtos_task_table[0].slice_tick, (int32)ARKRTOS_TIME_SLICE_TICKS,
           "the time slice is reset when it runs out");
    ck_i32(arkrtos_ready_top(), 1, "the rotating task moved to the tail of its bucket");
    ck_i32(g_switch_n, 1, "the rotation asks for a switch");

    use_task(0);
    arkrtos_cpu_load_counter = 0;
    arkrtos_kernel_tick_handler();
    ck_u32(arkrtos_cpu_load_counter, 0u, "idle does not count as busy");
    ck_u32(arkrtos_idle_ticks, 1u, "idle ticks are counted separately");

    /* 延时到期：当拍就绪、原因记为超时 */
    use_task(1);
    arkrtos_task_table[1].status = ARKRTOS_TASK_WAIT;
    arkrtos_task_table[1].wait_time = 5;
    arkrtos_ready_del(1);
    arkrtos_delay_arm(1, 1u);
    arkrtos_kernel_tick_handler();
    ck_i32(arkrtos_task_table[1].status, ARKRTOS_TASK_READY, "the sleeper wakes on time");
    ck_i32(arkrtos_task_table[1].wait_time, 0, "the wait budget is cleared");
    ck_i32(arkrtos_task_table[1].wake_reason, 1, "the wake reason is timeout");
    ck(arkrtos_ready_contains(1), "the woken task is back on the ready chain");

    /* 调度器锁定期：时间片照走，但不切 */
    arkrtos_sched_lock_nest = 1;
    g_switch_n = 0;
    arkrtos_task_table[0].slice_tick = 1;
    arkrtos_kernel_tick_handler();
    ck_i32(g_switch_n, 0, "a locked scheduler does not switch");
    arkrtos_sched_lock_nest = 0;

    /* 1024 拍边界关窗口 */
    arkrtos_kernel_tick = 0x3ffu;
    arkrtos_cpu_load_counter = 3u;
    arkrtos_cpu_busy_ticks = 0u;
    use_task(1);
    arkrtos_kernel_tick_handler();
    ck_u32(arkrtos_cpu_busy_ticks, 4u, "the busy window closes with the count");
    ck_u32(arkrtos_cpu_load_counter, 0u, "the live counter restarts");

    ck_u32(arkrtos_kernel_get_tick(), 0x400u, "get tick reports the raw tick");
    ck_u32(arkrtos_kernel_get_time(), 0x200u, "get time divides by the tick period");
    ck_u32(arkrtos_kernel_get_cpu_busy_ticks(), 4u, "get cpu busy reports the window");
}

/* ------------------------------------------------------------------
 * T13：收尸、自杀与故障恢复的两个阶段
 * ------------------------------------------------------------------ */
static void test_reap_and_recover(void)
{
    reset_all();
    make_task(5u, 0u);
    make_task(7u, 0u);

    /* 非法号是静默的 */
    arkrtos_task_release_resources(0);
    arkrtos_task_release_resources(9);
    ck_i32(g_sync_release_n, 0, "an invalid task releases nothing");

    arkrtos_task_release_resources(1);
    ck_i32(g_sync_release_n, 1, "the sync module is told");
    ck_i32(g_mq_release_n, 1, "the message queue module is told");
    ck_i32(g_release_n, 1, "the event module is told");
    ck_i32(g_net_release_n, 1, "the network module is told");
    ck_i32(g_shell_release_n, 1, "the shell module is told");
    ck_u32(g_shell_release_id, 1u, "the shell is given the task id");
    arkrtos_task_release_resources_handover(2);
    ck_i32(g_sync_release_n, 2, "the hand-over path reaps the same way");

    /* 自杀：状态、链、开关与诊断 */
    reset_all();
    make_task(5u, 0u);
    make_task(5u, 0u);
    use_task(1);
    arkrtos_task_table[0].status = ARKRTOS_TASK_WAIT;
    arkrtos_task_table[0].delay_queued = 0u;
    arkrtos_delay_arm(0, 100u);
    arkrtos_task_kill_internal();
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_INVALID, "a self kill invalidates the slot");
    ck(arkrtos_ready_contains(0) == 0, "the killed task leaves the ready chain");
    ck_i32(arkrtos_task_table[0].delay_queued, 0, "the killed task leaves the delay chain");
    ck_i32(g_switch_n, 1, "the kill asks for a switch");
    ck_i32(g_wfe_n, 1, "the kill waits for it");
    ck_i32((int32)arkrtos_kill_ret_cnt, 1, "an ineffective switch is counted as evidence");
    ck_i32(g_sync_release_n, 1, "the kill reaps the task's objects");

    /* 状态查询 */
    ck_i32(arkrtos_task_status_get_internal(1), ARKRTOS_TASK_INVALID,
           "the killed slot reports invalid");
    ck_i32(arkrtos_task_status_get_internal(0), ARKRTOS_TASK_ERR_PARAM,
           "task 0 is not a valid slot");
    ck_i32(arkrtos_task_status_get_internal(9), ARKRTOS_TASK_ERR_PARAM,
           "a slot beyond the count is not valid");

    /* 恢复阶段 1：标记 */
    reset_all();
    make_task(5u, 0u);
    use_task(1);
    /* 模拟优先级继承：内核里唯一改 priority 的路径是 reprio，它会把
     * 节点从旧桶摘出再挂到新桶。只改字段不搬桶会造出「桶头指向已脱链
     * 任务」的悬垂链，随后 recover 按新优先级摘链时就摘错了桶。 */
    arkrtos_task_table[0].priority = 9u;
    arkrtos_ready_reprio(0, 5u);
    arkrtos_task_table[0].status = ARKRTOS_TASK_READY;
    ck_i32(arkrtos_task_recover(0), ARKRTOS_TASK_ERR_PARAM, "recover refuses task 0");
    ck_i32(arkrtos_task_recover(9), ARKRTOS_TASK_ERR_PARAM, "recover refuses a slot beyond the count");
    ck_i32(arkrtos_task_recover(1), 0, "recover accepts a real slot");
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_INVALID, "the task is parked as invalid");
    ck_i32(arkrtos_task_table[0].recover_pending, 1, "the recovery is pending");
    ck_i32(arkrtos_task_table[0].priority, 5, "the inherited priority is reset to the base");
    ck(fault_has(ARKRTOS_FAULT_RECOVER, 1), "the recovery is recorded");
    ck_i32(g_switch_n, 1, "recover asks for a switch");

    /* 恢复阶段 2：下一次调度决策把栈帧重建好 */
    arkrtos_task_table[0].stack_bottom[1] = 0x11111111u;
    arkrtos_task_table[0].stack_bottom[0] = 0u;
    arkrtos_sched_next();
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_READY, "the pending task is rebuilt as ready");
    ck_i32(arkrtos_task_table[0].recover_pending, 0, "the pending flag is cleared");
    ck(arkrtos_ready_contains(0), "the rebuilt task is back on the ready chain");
    ck_u32(arkrtos_task_table[0].stack_bottom[1], (uint32)ARKRTOS_STACK_FILL_PATTERN,
           "the stack is re-initialised");
    ck_i32(arkrtos_task_table[0].wake_reason, 0, "the wake reason is reset");
    ck_i32(arkrtos_task_table[0].period_time, arkrtos_task_table[0].period,
           "the period budget is reset");

    /* 没有待恢复任务时这一段是空转 */
    arkrtos_sched_next();
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_READY, "a second decision leaves it alone");
}

/* ------------------------------------------------------------------
 * T13b：任务入口返回（旧实现里 ra=0 -> 地址 0 取指 -> 硬件 INE）
 * ------------------------------------------------------------------ */
static void test_entry_return(void)
{
    /* 有当前任务：返回即按自我结束收尸，然后停在 wfi 等待调度 */
    reset_all();
    make_task(5u, 0u);
    use_task(1);

    g_wfi_jmp_armed = 1;
    g_wfi_hits = 0;
    if(setjmp(g_wfi_jmp) == 0)
    {
        arkrtos_task_entry_return();
        ck(0, "entry return must not fall through");
    }
    ck_i32(g_wfi_jmp_armed, 0, "the returned entry lands in the wfi loop");
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_INVALID,
           "a returned entry is reclaimed as a self kill");
    ck(arkrtos_ready_contains(0) == 0,
       "the returned task leaves the ready chain");
    ck_u32(arkrtos_kill_ret_cnt, 1u,
           "the ineffective switch after the exit is still counted");

    /* 没有当前任务：不收尸任何槽（本不该发生，但不能因此改别人的状态），
     * 仍然停在 wfi。 */
    reset_all();
    make_task(5u, 0u);
    use_task(0);

    g_wfi_jmp_armed = 1;
    g_wfi_hits = 0;
    if(setjmp(g_wfi_jmp) == 0)
    {
        arkrtos_task_entry_return();
        ck(0, "entry return must not fall through without a current task");
    }
    ck_i32(g_wfi_jmp_armed, 0, "the wfi loop is entered either way");
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
           "no current task means no slot is reclaimed");
}

/* ------------------------------------------------------------------
 * T14：切换判定 arkrtos_sched_is_switching
 * ------------------------------------------------------------------ */
static void test_sched_is_switching(void)
{
    reset_all();

    /* 没有任务：越界哨兵 + 探针留证 */
    arkrtos_sched_probe_pos = 0u;
    ck_i32(arkrtos_sched_is_switching(), ARKRTOS_TASK_NONE,
           "an empty system asks for no switch");
    /* 空系统里 ready_top 返回哨兵，sched_next 只把决策计数推一格。
     * 「越界任务号 +0x10000」那条分支是防御性的：ready_top 会自己把
     * 越界桶头自愈掉，公开路径到不了那里。 */
    ck_u32(arkrtos_sched_probe_pos, 1u,
           "one decision is recorded in the probe counter");

    /* 高优先级就绪：要切 */
    make_task(5u, 0u);
    make_task(3u, 0u);
    use_task(1);
    ck_i32(arkrtos_sched_is_switching(), 2, "a higher priority task asks for a switch");

    /* 锁定期：不切（返回上下文错误） */
    arkrtos_sched_lock_nest = 1;
    ck_i32(arkrtos_sched_is_switching(), ARKRTOS_TASK_ERR_CONTEXT,
           "a locked scheduler reports the context error");
    arkrtos_sched_lock_nest = 0;

    /* 选中的还是自己：不切 */
    use_task(2);
    ck_i32(arkrtos_sched_is_switching(), ARKRTOS_TASK_NONE,
           "the same task is not a switch");
}

/* ------------------------------------------------------------------
 * T15：CPU 异常入口
 * ------------------------------------------------------------------ */
static void test_fault_handler(void)
{
    unsigned long ret;

    reset_all();
    make_task(5u, 0u);
    make_task(5u, 0u);
    use_task(1);
    ret = arkrtos_cpu_fault_handler(ARKRTOS_FAULT_HARDFAULT);
    ck(fault_has(ARKRTOS_FAULT_HARDFAULT, 1), "the fault is recorded against the task");
    ck_i32(g_loader_fault_n, 1, "the loader is asked whether the app is disabled");
    /* handler 末尾自己调 sched_next 选下一个任务，而 sched_next 会把
     * recover_pending 当场走完（重建栈帧、置 READY），所以这里看到的是
     * 「已经重建好、等下一拍重新跑」的状态，而不是挂起态。 */
    ck_i32(arkrtos_task_table[0].recover_pending, 0,
           "the pending recovery is consumed inside the handler");
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_READY,
           "the faulted task is rebuilt ready to restart");
    ck_u32((uint32)arkrtos_task_table[0].stack_peak_low,
           (uint32)arkrtos_task_table[0].stack_top,
           "the rebuilt stack restarts its watermark at the top");
    ck_i32(arkrtos_current_task_id, 2, "control lands on the next ready task");
    ck(ret != 0ul, "the next stack pointer is returned");
    ck_i32(g_mpu_app_n, 1, "the next task's MPU context is loaded");

    /* 已达到重启上限：不再标记恢复 */
    reset_all();
    make_task(5u, 0u);
    use_task(1);
    g_loader_fault_ret = 1;
    arkrtos_cpu_fault_handler(ARKRTOS_FAULT_BUSFAULT);
    ck_i32(arkrtos_task_table[0].recover_pending, 0, "a disabled app is not marked for recovery");
    ck_i32(arkrtos_task_table[0].status, ARKRTOS_TASK_INVALID, "a disabled app ends up invalid");
    ck(fault_has(ARKRTOS_FAULT_BUSFAULT, 1), "the fault is still recorded");

    /* 内核上下文：记录后停机 */
    reset_all();
    use_task(0);
    ret = arkrtos_cpu_fault_handler(ARKRTOS_FAULT_USGFAULT);
    ck_u32((uint32)ret, 0u, "the kernel context is not restartable");
    ck(fault_has(ARKRTOS_FAULT_USGFAULT, 0), "the fault is recorded against the kernel");

    /* hardfault 转调同一入口 */
    reset_all();
    use_task(0);
    ret = arkrtos_hardfault_handler();
    ck_u32((uint32)ret, 0u, "hardfault from the kernel is not restartable");
    ck(fault_has(ARKRTOS_FAULT_HARDFAULT, 0), "hardfault is recorded with its own type");
}

/* ------------------------------------------------------------------
 * T16：调度自检与决策快照
 * ------------------------------------------------------------------ */
static void test_sched_check(void)
{
    reset_all();
    make_task(5u, 0u);
    make_task(5u, 0u);
    ck_i32(arkrtos_sched_check(), 0, "a consistent ready set passes the self check");

    /* 手工把状态改成无效但留在链上：自检必须看得出来 */
    arkrtos_task_table[1].status = ARKRTOS_TASK_INVALID;
    ck_i32(arkrtos_sched_check() > 0, 1, "a status that drifted from the chain is detected");
    arkrtos_task_table[1].status = ARKRTOS_TASK_READY;
    ck_i32(arkrtos_sched_check(), 0, "the self check recovers with the status");

    /* 快照六字 */
    reset_all();
    make_task(5u, 0u);
    arkrtos_sched_probe_pos = 0u;
    ck_i32(arkrtos_sched_next(), 0, "the decision picks the only task");
    ck_u32(arkrtos_sched_probe_pos, 1u, "the probe position advances");
    ck_u32(arkrtos_sched_probe[0], 0u, "the snapshot records the chosen index");
    ck_u32(arkrtos_sched_probe[4], 1u, "the snapshot records the ready total");
    ck_u32(arkrtos_sched_probe[5], 0u, "the snapshot records the current task");

    /* 环形：第 17 次写回第 0 槽 */
    arkrtos_sched_probe_pos = 16u;
    arkrtos_sched_next();
    ck_u32(arkrtos_sched_probe_pos, 17u, "the probe position keeps counting");
    ck_u32(arkrtos_sched_probe[0], 0u, "the ring wraps onto the first slot");
}

/* ------------------------------------------------------------------
 * T17：就绪即切（activate_higher）
 * ------------------------------------------------------------------ */
static void test_activate_higher(void)
{
    reset_all();
    make_task(5u, 0u);
    make_task(3u, 0u);
    use_task(1);

    arkrtos_sched_lock_nest = 1;
    g_switch_n = 0;
    arkrtos_sched_activate_higher(3u);
    ck_i32(g_switch_n, 0, "a locked scheduler defers the switch");
    arkrtos_sched_lock_nest = 0;

    arkrtos_sched_activate_higher(3u);
    ck_i32(g_switch_n, 1, "a higher priority wake switches at once");

    arkrtos_sched_activate_higher(7u);
    ck_i32(g_switch_n, 1, "a lower priority wake does not switch");

    use_task(0);
    arkrtos_sched_activate_higher(31u);
    ck_i32(g_switch_n, 2, "from the kernel context any wake switches");
}

/* ------------------------------------------------------------------
 * main：T0 必须最先（它吃掉「首次调度自动重建」这条一次性路径）
 * ------------------------------------------------------------------ */
int main(void)
{
    /* 与其它宿主用例同口径：只在最后打一行汇总，不加逐例噪音。 */
    #define RUN(fn) fn()
    RUN(test_first_sched_rebuild);
    RUN(test_pointer_checks);
    RUN(test_ready_bitmap);
    RUN(test_delay_chain);
    RUN(test_sched_lock);
    RUN(test_blocking);
    RUN(test_wakeup_internal);
    RUN(test_thread_lifecycle);
    RUN(test_sched_activate);
    RUN(test_stack_usage);
    RUN(test_stack_guard);
    RUN(test_task_swap);
    RUN(test_svc_dispatch);
    RUN(test_tick_and_slice);
    RUN(test_reap_and_recover);
    RUN(test_entry_return);
    RUN(test_sched_is_switching);
    RUN(test_fault_handler);
    RUN(test_sched_check);
    RUN(test_activate_higher);

    if(g_fails != 0)
    {
        printf("TASK HOST FAIL %d/%d checks\n", g_fails, g_checks);
        return 1;
    }
    printf("TASK HOST OK %d checks\n", g_checks);
    return 0;
}
