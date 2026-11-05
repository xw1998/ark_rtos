/* ============================================================
 * arkc_shell_cmd.h - "cc" command for the Ark RTOS shell
 *
 * arkvim proved the pattern: the editor has no idea a shell exists, and
 * the shell gains one command by calling arkvim_shell_register().  The
 * compiler follows the same two functions.
 * ============================================================ */
#ifndef ARKC_SHELL_CMD_H
#define ARKC_SHELL_CMD_H

/* Install the "cc" command at g_cmd_table[idx]; returns the next free
 * index (unchanged when the command table is full). */
int arkc_shell_register(int idx);

#if (ARKRTOS_USE_APPPACK == 1)
/* 多源差分打包，"cc --pack" 的实现本体。
 *
 * ark make 走同一条路而不是另写一份：镜像的生成方式只允许有一种，否则
 * "在板上验证过的"和"make 打出来的"就不是同一份东西，而这一点在板上
 * 只能靠偶发的运行失败才发现。返回 0 成功，非 0 失败（细节已打到控制台）。 */
int arkc_cc_pack(const char *const *srcs, int n_srcs, const char *out,
                 const char *const *defs, int n_defs,
                 const char *const *incs, int n_incs,
                 int opt_level, int verbose);
#endif

#endif /* ARKC_SHELL_CMD_H */
