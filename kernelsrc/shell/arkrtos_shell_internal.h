/**
* @file arkrtos_shell_internal.h
* @brief 内核 Shell 各命令族之间共享的东西（内部头，不对外）
* @details 拆文件之后唯一需要跨文件可见的只有三样：
*            - 共享的小工具（sh_out / parse_u32 / 各种名字表）；
*            - 各族的命令入口（命令注册表在 arkrtos_shell.c 里一次性填）；
*            - 用户命令族的两块静态缓冲（控制台任务卸载镜像时要清）。
*          除这三样外，各族的 helper 依旧是文件内 static。
* @note 只许 kernelsrc/shell/arkrtos_shell*.c 包含本头。
*/

#ifndef ARKRTOS_SHELL_INTERNAL_H
#define ARKRTOS_SHELL_INTERNAL_H

#include "arkrtos_shell.h"

#include "ark_shell.h"
#include "ark_shell_commands.h"
#include "ark_shell_eval.h"
#include "platform.h"

#include "arkrtos_config.h"
#include "arkrtos_cfg.h"
#include "arkrtos_fault.h"
#include "arkrtos_installer.h"
#include "arkrtos_loader.h"
#include "arkrtos_mini.h"
#include "arkrtos_apprun.h"
#include "arkrtos_elf.h"
#include "arkrtos_export.h"
#include "arkrtos_modld.h"
#if (ARKRTOS_USE_FLASHAPP == 1)
#include "arkrtos_flashapp.h"
#endif
#include "arkrtos_dev.h"
#include "arkrtos_layout.h"
#include "arkrtos_ptable.h"
#include "arkrtos_log.h"
#include "arkrtos_fifo.h"
#include "arkrtos_sync.h"
#include "arkrtos_share.h"
#include "arkrtos_task.h"
#include "arkrtos_trace.h"
#include "arkrtos_blk.h"
#include "arkrtos_fs.h"
#include "arkrtos_vfs.h"
#include "ark_vfs.h"
#include "arkrtos_shell_auth.h"
#include "arkrtos_audit.h"
#if (ARKRTOS_USE_AUDIT_LOG == 1)
#include "arkrtos_audit_log.h"
#endif
#include "arkrtos_guard.h"
#if ARKRTOS_HW_SAFE
#include "arkrtos_hwsafe.h"
#endif
#if ARKRTOS_USE_SOFTBUS
#include "arkrtos_softbus.h"
#endif
#if ARKRTOS_USE_MEMPOOL
#include "arkrtos_mempool.h"
#endif
/* utest 曾经被塞在 MEMPOOL 的条件块里 —— 于是「关内存池、开单元测试」
 * 这种合法组合会在 cmd_utest() 里报 ark_utest_summary_t 未定义（实测最小档
 * 1 Error）。它的门控必须只看自己的开关。 */
#if (ARKRTOS_USE_UTEST == 1)
#include "arkrtos_utest.h"
#endif
#include "arkrtos_crash.h"
#include "arkrtos_partition.h"
#include "arkrtos_hal.h"
#include "arkrtos_mpu.h"
#include "arkrtos_version.h"
#include "arkrtos_rt.h"
#if (ARKRTOS_USE_TICKLESS == 1)
#include "arkrtos_tickless.h"
#endif
#if (ARKRTOS_USE_SECCHECK == 1)
#include "arkrtos_sec.h"
#endif
#include "arkrtos_dt.h"
#if (ARKRTOS_USE_FDT == 1)
#include "arkrtos_fdt.h"
#endif

#if (ARKRTOS_USE_ARKVIM == 1)
#include "arkvim_shell_cmd.h"
#endif

#if (ARKRTOS_USE_ARKC == 1)
#include "arkc_shell_cmd.h"
#endif
#if (ARKRTOS_USE_ARKMAKE == 1)
#include "arkmake_shell_cmd.h"
#endif

#include <string.h>

/* Feature-gate fallbacks -------------------------------------------------
 * Three always-on commands read a value that belongs to an optional module:
 * `app` asks whether a volume is mounted, `app` / `drv` ask why a slot was
 * held back, and `cfg show` prints the log level.  Rather than scattering
 * #if through the readers, the call is rewritten to a constant when the
 * module is switched off.  Only the value changes; the command still runs
 * and still reports something true ("not mounted", "disabled").
 * ---------------------------------------------------------------------- */
#if !ARKRTOS_USE_FS
#define arkrtos_fs_mounted()          0u
#endif
#if !ARKRTOS_USE_CRASH_LOG
#define arkrtos_crash_disabled(i)     0u
#define arkrtos_crash_reason_name(r)  "crash journal disabled"
#endif
#if !ARKRTOS_USE_LOG
#define arkrtos_log_get_level()       0u
#endif

/* ---- 用户命令族的私有类型与宏（原样从 arkrtos_shell_ushell.c 搬入） ---- */
#define ARKRTOS_USHELL_MAX_CMDS   (8)

/* 控制台分段输出缓冲：SVC 处理跑在用户任务的 PSP 上，栈很浅 */
#define ARKRTOS_USHELL_OUT_CHUNK  (64)

typedef struct
{
    char     name[ARKRTOS_USHELL_NAME_MAX + 1];
    char     help[ARKRTOS_USHELL_HELP_MAX + 1];
    int    (*func)(int argc, char **argv);
    uint32   max_args;
    uint32   in_use;
    uint32   owner_task;    /* task that registered it; 0 = kernel, kept */
} arkrtos_ushell_entry_t;

#if (SHELL_ENABLE == 1)

/* ---- 静态缓冲（定义在 arkrtos_shell_ushell.c） ---- */
extern ark_shell_t g_shell;
extern arkrtos_ushell_entry_t g_ushell[ARKRTOS_USHELL_MAX_CMDS];

/* ---- 跨文件的 helper 与命令入口 ---- */
const char *cfg_mode_text(uint32 mode);
const char *cfg_reclaim_text(uint32 rm);
int32 cfg_recv_exact(int32 dev, uint8 *buf, uint32 len, uint32 timeout_ms);
const char *cfg_source_text(uint32 src);
const char *cfg_type_text(uint32 t);
int cmd_app(int argc, char *argv[]);
int cmd_audit(int argc, char *argv[]);
#if (ARKRTOS_USE_AUDIT_LOG == 1)
int cmd_auditlog(int argc, char *argv[]);
#endif
#if (ARKRTOS_USE_ROLLBACK == 1)
int cmd_rollback(int argc, char *argv[]);
#endif
int cmd_blk(int argc, char *argv[]);
int cmd_cd(int argc, char *argv[]);
int cmd_cfg(int argc, char *argv[]);
int cmd_crash(int argc, char *argv[]);
int cmd_df(int argc, char *argv[]);
int cmd_dmesg(int argc, char *argv[]);
int cmd_drv(int argc, char *argv[]);
int cmd_dt(int argc, char *argv[]);
#if (ARKRTOS_USE_FDT == 1)
int cmd_fdt(int argc, char *argv[]);
#endif
int cmd_exec(int argc, char *argv[]);
int cmd_fault(int argc, char *argv[]);
int cmd_free(int argc, char *argv[]);
int cmd_fs(int argc, char *argv[]);
int cmd_guard(int argc, char *argv[]);
int cmd_history(int argc, char *argv[]);
int cmd_hwsafe(int argc, char *argv[]);
int cmd_info(int argc, char *argv[]);
int cmd_install(int argc, char *argv[]);
int cmd_ld(int argc, char *argv[]);
int cmd_lkup(int argc, char *argv[]);
int cmd_log(int argc, char *argv[]);
#if (ARKRTOS_USE_LINUX_ABI == 1) && (ARKRTOS_LINUX_ABI_SYSSTAT == 1)
int cmd_lxstat(int argc, char *argv[]);
#endif
int cmd_man(int argc, char *argv[]);
int cmd_mini(int argc, char *argv[]);
int cmd_modules(int argc, char *argv[]);
int cmd_mpool(int argc, char *argv[]);
int cmd_pmap(int argc, char *argv[]);
int cmd_pool(int argc, char *argv[]);
int cmd_ps(int argc, char *argv[]);
int cmd_pwd(int argc, char *argv[]);
int cmd_rt(int argc, char *argv[]);
int cmd_sched(int argc, char *argv[]);
int cmd_sec(int argc, char *argv[]);
#if (ARKRTOS_USE_SELFTEST == 1)
int cmd_selftest(int argc, char *argv[]);
#endif
int cmd_stack(int argc, char *argv[]);
#if (ARKRTOS_USE_TASK_SWAP == 1)
int cmd_swap(int argc, char *argv[]);
#endif
int cmd_syncinfo(int argc, char *argv[]);
int cmd_task(int argc, char *argv[]);
int cmd_uname(int argc, char *argv[]);
int cmd_unld(int argc, char *argv[]);
int cmd_utest(int argc, char *argv[]);
int cmd_vfs_cat(int argc, char *argv[]);
int cmd_vfs_chmod(int argc, char *argv[]);
int cmd_vfs_chown(int argc, char *argv[]);
int cmd_vfs_cp(int argc, char *argv[]);
int cmd_vfs_ls(int argc, char *argv[]);
int cmd_vfs_mkdir(int argc, char *argv[]);
int cmd_vfs_mount(int argc, char *argv[]);
int cmd_vfs_mv(int argc, char *argv[]);
int cmd_vfs_rm(int argc, char *argv[]);
int cmd_vfs_rmdir(int argc, char *argv[]);
int cmd_vfs_stat(int argc, char *argv[]);
int cmd_vfs_touch(int argc, char *argv[]);
int cmd_vfs_umount(int argc, char *argv[]);
int cmd_wait(int argc, char *argv[]);
int cmd_which(int argc, char *argv[]);
const char *fault_type_name(uint32 t);
int parse_u32(const char *s, uint32 *out);
void sh_out(const char *s);
const char *slot_state_name(uint32 st);
const char *slot_type_name(uint32 t);
const char *task_state_name(uint32 st);
/* Unknown-word fallback into the ELF path. Defined by arkrtos_shell_mod.c when
 * ARKRTOS_USE_ELF is on; ark_shell_eval.c is its only caller. */
int ark_shell_exec_fallback(int argc, char *argv[]);
void ushell_drop(uint32 i);
int vfs_complete(struct ark_shell_s *shell, const char *word, int is_first, char cand[][ARK_SHELL_LINE_SIZE], int max);

#endif /* SHELL_ENABLE */

#endif /* ARKRTOS_SHELL_INTERNAL_H */
