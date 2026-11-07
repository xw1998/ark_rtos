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

/* 内存文件（"--pack 多份源码" 与 ark make 用）。
 *
 * 一次 pass 里要先把 N 份源码各编成一个对象、再把它们链接成负载。对象既
 * 不该落盘（ramfs 只有 3 槽；littlefs 每次写要擦扇区，实测 ~10 s 一份），
 * 也不可能落盘（四遍 × N 份），因此给端口一张表：注册过的名字由
 * fs_read_file / fs_file_size / fs_write_file 直接从内存服务。
 *
 * 语义与真实文件一致，包括那条"宁可报错"的规矩：
 *   - 写入超过 cap 报错（"File too large"），不截断；
 *   - 读出时文件大于调用方缓冲也报错，不截断；
 *   - 同名重复注册拒绝（不静默覆盖），表满拒绝；
 *   - 名字按字面匹配，不做路径解析。
 * 用 arkc_port_vfile_clear() 在一轮结束（成功或失败）时清表，避免下一次
 * 打包拿到上一轮的对象。 */
int      arkc_port_vfile_register(const char *name, uint8_t *buf, uint32_t cap);
void     arkc_port_vfile_clear(void);
uint32_t arkc_port_vfile_size(const char *name);

/* 上一次编译（-c）产出的对象里有没有叫 name 的符号。
 *
 * 存在的原因具体且只有一条：“cc --pack” 的第一个对象要不要预留
 * 0x400 的向量表洞，取决于**它自己定没定义 Reset_Handler**——定义了就必须
 * 留洞给 -link 填表，没定义说明这是平铺的小程序（没有复位向量），留洞只是
 * 白长 0x400 而且没人填。这个事实只有刚编完的符号表能回答，去搜源码文本
 * 或者去猜都是在赌。
 *
 * 返回 1 = 有，0 = 没有（名字空/参数非法也算没有）。
 * 只在“刚编完、还没开始下一份”的窗口里有效：arkc 在下一次编译的 cc_reset_state
 * 里清表（那是它自己的约定，不是本端口造成的）。 */
int      arkc_port_last_object_has_symbol(const char *name);

/* 内存对象里代码段的长度（ARKOBJ2 头 +8 的那个 u32）。
 *
 * 为什么要这个入口，而不是让调用方自己去算「对象槽在哪儿」：那个槽位偏移在
 * 两边各算一遍（注册时一次、读长度时一次），只要有一处算法漂了，读到的就是
 * 别的槽的字节——板上实测过：三个源时第三槽被读成 0，报"对象 0 字节"，
 * 而编译器明明写了 2560 字节。槽的地址只有注册它的那个端口知道，长度就该从
 * 那条路读。（顺带也把「对象头格式」这件事收在一个文件里。）
 *
 * 返回 0 表示"读不出"：名字没注册、长度不足一个头、或头不是 ARKOBJ2。
 * 这三种都得由调用方当失败，不能当成"0 字节的对象"。 */
uint32_t arkc_port_vfile_code_size(const char *name);
#endif

#endif /* ARKC_PORT_H */

