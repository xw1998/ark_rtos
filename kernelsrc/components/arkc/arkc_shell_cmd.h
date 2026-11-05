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
/* 失败发生在哪一段。cc_pack 只知道"成功/失败"，而一次 --pack 里有编译、链接、
 * 布局校验、差分打包、写盘五段，同一句"失败了"落在不同段上意味着完全不同的
 * 下一步。调用方（ark make）要把这个差别翻成自己的错误码，不能让它们在半路
 * 变成一个笼统的"link failed"——那是错答案，不是概括。 */
#define ARKC_PACK_PHASE_NONE     0
#define ARKC_PACK_PHASE_SETUP    1   /* 借工作区 / 注册对象槽 / 写 .sct */
#define ARKC_PACK_PHASE_COMPILE  2   /* 编译某个对象（含池耗尽） */
#define ARKC_PACK_PHASE_LINK     3   /* 链接某一遍 */
#define ARKC_PACK_PHASE_LAYOUT   4   /* 四遍尺寸不一致 / 装不进窗口 / 越界的落点 */
#define ARKC_PACK_PHASE_PACK     5   /* 四份镜像差分不出表（含没有向量表） */
#define ARKC_PACK_PHASE_WRITE    6   /* 产物写不下去 */

/* 多源差分打包，"cc --pack" 的实现本体。
 *
 * ark make 走同一条路而不是另写一份：镜像的生成方式只允许有一种，否则
 * "在板上验证过的"和"make 打出来的"就不是同一份东西，而这一点在板上
 * 只能靠偶发的运行失败才发现。返回 0 成功，非 0 失败（细节已打到控制台）。
 *
 * fail_phase 可为 NULL；非 NULL 时在失败时写入 ARKC_PACK_PHASE_*，成功时写
 * ARKC_PACK_PHASE_NONE。 */
int arkc_cc_pack(const char *const *srcs, int n_srcs, const char *out,
                 const char *const *defs, int n_defs,
                 const char *const *incs, int n_incs,
                 int opt_level, int verbose, int *fail_phase);
#endif

#endif /* ARKC_SHELL_CMD_H */
