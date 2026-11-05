/* ============================================================
 * arkc_port.h - what arkc_port_arkrtos.c offers to the command layer
 *
 * The compiler itself is reached through include/platform.h and
 * include/flash_fs.h.  These four are the kernel-side contract that only
 * the "cc" command needs, kept in one header so the two .c files cannot
 * drift apart on a signature.
 * ============================================================ */
#ifndef ARKC_PORT_H
#define ARKC_PORT_H

#include <setjmp.h>
#include <stdint.h>

/* Landing pad for ark_abort(): arkc treats it as a no-return function, so
 * the port longjmps back here instead of hanging the shell task.  Pass NULL
 * to disarm (the port then prints and returns, which is worse but alive). */
void        arkc_port_set_abort_target(jmp_buf *jb);

/* Did the run that just finished end through ark_abort()? */
uint8_t     arkc_port_abort_used(void);

/* Message of that fatal error, valid only when the above returns 1. */
const char *arkc_port_abort_message(void);

/* Zero the .ark_buf arena (compiler pool + source/asm/code/object buffers).
 *
 * That section is NOLOAD: it costs no image bytes and no boot time, but it
 * also is not zeroed by the C runtime, so whatever the previous reset or the
 * previous compile left there is still readable.  arkc resets what it owns
 * on entry, yet a deterministic start is cheaper than trusting that; call it
 * once before each compile.
 *
 * Safe to call even when there is no arena (the linker script is generated,
 * so a build without arkc simply has a zero-length section: the weak symbols
 * are then undefined and this returns doing nothing). */
void        arkc_port_zero_arena(void);

#if (ARKRTOS_USE_APPPACK == 1)
/* 输出捕获（只给 "cc --pack" 用）。
 *
 * 四遍差分要的是四份镜像的**字节**，不是四个文件：把它们写进 ramfs 会
 * 占满 3 个文件槽（源码 + .sct + 产物正好卡满），而且还要再读回来一次。
 * 于是给端口加一层"下一次写文件写进这块内存"的钩子：捕获激活期间
 * fs_write_file() 不改动文件系统，只往调用方给的缓冲里追加。
 *
 * 契约有三条，都是为了"宁可报错不给错答案"：
 *   1) 记不下就失败（返回 -1），绝不截断——半份镜像打出来的表是错的；
 *   2) 捕获期间**所有** fs_write_file 都进缓冲，路径不再解析。一条编译
 *      只有一次输出写（arkc 的 compiler.c 里 -o 与 -link 各只有一处），
 *      所以"全部"与"那一次"是同一件事，不必再分辨；
 *   3) 只能有一个捕获（二次 begin 报错），否则两个使用者会互相覆盖。
 *
 * begin 返回 0 成功、-1 参数非法或已有捕获；end 返回收到多少字节并关闭
 * 捕获（0 表示这一遍什么都没写，调用方应当当成失败）。 */
int      arkc_port_capture_begin(uint8_t *buf, uint32_t cap);
uint32_t arkc_port_capture_end(void);
#endif

#endif /* ARKC_PORT_H */

