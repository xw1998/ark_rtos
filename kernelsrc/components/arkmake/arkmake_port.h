/* ============================================================
 * arkmake_port.h - Ark RTOS 侧的 arkmake 接线（文件读 + 日志 + 打包）
 *
 * 上游 arkmake（gitee xw19981010/arkmake）不认识文件系统、不认识 shell、也不
 * 认识编译器：arkmake_build() 只把"这几份源码打成这个产物"交给三个回调。
 * 本文件声明的就是这三个回调在 Ark RTOS 上的实现。
 *
 * 为什么单独一个头：shell 命令层（arkmake_shell_cmd.c）只该看到"一次构建"，
 * 不该看到 fs_read_file / arkc_cc_pack。反过来，端口实现不该看见 argv。
 * 两边唯一的共用物就是这里的 ctx 与 ops 装配函数。
 * ============================================================ */
#ifndef ARKMAKE_PORT_H
#define ARKMAKE_PORT_H

#include "arkmake.h"

/* 一次构建的可调项。
 *
 * verbose 直接透给 arkc 的 cc_pack：它控制每一遍、每一份源、每个对象的
 * 读数（"obj0 2590 B code at +0x0"）。默认关，因为一次四遍打包本来就要
 * 三十多秒，全开时串口那点带宽会把它拖得更久；但失败时必须能开。 */
typedef struct {
    int verbose;
} arkmake_ctx_t;

/* 把三个回调（log / pack；read 由命令层单独用）填进 ops。
 * 传入的 ops 必须非空；函数不分配、不失败。 */
void arkmake_port_ops(arkmake_ops_t *ops);

/* 读一份文件（makefile 用）。契约与 arkmake_read_fn 一致：
 * 成功返回 0 并回填 *out_len，失败返回非 0。
 *
 * 不截断：装不下就是失败。半份 makefile 会被解析成"少了 SRCS 第三项"，
 * 而那种错误在镜像上是看不见的（链接照样成功），所以宁可在这里停。 */
int arkmake_port_read(void *ctx, const char *path,
                      char *buf, size_t cap, size_t *out_len);

#endif /* ARKMAKE_PORT_H */
