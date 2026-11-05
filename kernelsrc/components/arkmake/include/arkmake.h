/* ============================================================
 * arkmake.h - Ark Make：板上可用的"简版 make"
 *
 * 为什么需要它
 * ------------
 * 板上已经有 arkc（cc）、arkvim、arkshell、arkvfs，但把"多份 .c 编成一个
 * 可运行镜像"这件事仍然要人手敲：`cc --pack a.c b.c -o out.svcm` 一条命令
 * 能做完，可一旦源码变成四份、要带 -D/-I、产物要落到某个卷上，命令行的
 * 重复劳动就开始出错（少一份源码是看不见的：镜像照样链接成功，只是少了
 * 那个编译单元）。arkmake 把这件事固化成一份文件的解析 + 一次驱动。
 *
 * 它刻意不是 GNU make
 * -------------------
 * 没有隐含规则、没有时间戳依赖、没有 shell。本工具解析的是一份**工程描述**：
 *
 *     # 注释
 *     TARGET = app
 *     OUT    = /mnt/nor/app.svcm
 *     SRCS   = a.c b.c
 *     DEFS   = LOG_LEVEL=2
 *     INCS   = inc include
 *
 * 支持的语法只有三样，全部有明确语义：
 *   1) `NAME = value`（覆盖赋值）与 `NAME += value`（追加）；
 *   2) `$(NAME)` 展开（递归展开，深度上限 8，防自引用）；
 *   3) `#` 开头到行尾是注释。
 * 其余任何写法都**报错**，不静默忽略——"读不懂但照样构建出一份东西"正是
 * 这个工具最该避免的失败方式。
 *
 * 为什么没有时间戳
 * ----------------
 * 板上的卷（littlefs/ramfs）不保证可靠的 mtime，用"比较时间"决定要不要重编
 * 会得出随机结论。所以 arkmake 永远全量构建，并把这个代价写在文档里；需要
 * 增量时该改的是存储层，而不是在判据上打补丁。
 * ============================================================ */
#ifndef ARKMAKE_H
#define ARKMAKE_H

#include <stddef.h>

/* 与 `cc --pack` 的对象槽数一致（CC_PACK_MAX_SRCS）。超过就是编译器那边
 * 装不下，提前在这里拒绝，而不是等四遍都跑完再失败。 */
#define ARKMAKE_MAX_SRCS   4
#define ARKMAKE_MAX_DEFS   16
#define ARKMAKE_MAX_INCS   8
#define ARKMAKE_MAX_VARS   16
#define ARKMAKE_NAME_MAX   48
#define ARKMAKE_PATH_MAX   96
#define ARKMAKE_EXPAND_DEPTH 8

/* 错误码（全部为负）。名字是给人看的，映射只有一处
 * （arkmake_error_name）。 */
#define ARKMAKE_OK               0
#define ARKMAKE_ERR_ARG         (-1)   /* 空指针 / 容量 0 */
#define ARKMAKE_ERR_SYNTAX      (-2)   /* 既不是注释、也不是 NAME=value */
#define ARKMAKE_ERR_NAME        (-3)   /* 变量名非法或过长 */
#define ARKMAKE_ERR_VALUE_LONG  (-4)   /* 值超出字段容量（不截断） */
#define ARKMAKE_ERR_UNDEF       (-5)   /* 用了没定义的变量 */
#define ARKMAKE_ERR_RECURSE     (-6)   /* $(A) 里又有 $(A)，超过深度上限 */
#define ARKMAKE_ERR_NO_TARGET   (-7)   /* 没有 TARGET，产物名无从确定 */
#define ARKMAKE_ERR_NO_SRCS     (-8)   /* 没有 SRCS */
#define ARKMAKE_ERR_TOO_MANY    (-9)   /* 某项超出上限（源码/宏/头文件目录） */
#define ARKMAKE_ERR_SIZE        (-10)  /* 构建阶段：镜像超过窗口 */
#define ARKMAKE_ERR_COMPILE     (-11)  /* 构建阶段：编译失败 */
#define ARKMAKE_ERR_LINK        (-12)  /* 构建阶段：链接失败 */
#define ARKMAKE_ERR_WRITE       (-13)  /* 构建阶段：产物写不下去 */
#define ARKMAKE_ERR_UNSUPPORTED (-14)  /* 构建阶段：这个 build 不认识 */
#define ARKMAKE_ERR_SETUP       (-15)  /* 构建阶段：打包器拿不到工作区，还没开编 */
#define ARKMAKE_ERR_PACK        (-16)  /* 构建阶段：四份镜像打不成一张表（布局/向量表） */

typedef struct {
    char name[ARKMAKE_NAME_MAX];
    char value[ARKMAKE_PATH_MAX];
} arkmake_var_t;

typedef struct {
    char target[ARKMAKE_NAME_MAX];              /* TARGET，缺省 "app" 不做：
                                                 * 没有就报 ARKMAKE_ERR_NO_TARGET */
    char out[ARKMAKE_PATH_MAX];                 /* OUT，空 = 由 default_out() 推 */
    char srcs[ARKMAKE_MAX_SRCS][ARKMAKE_PATH_MAX];
    int  n_srcs;
    char defs[ARKMAKE_MAX_DEFS][ARKMAKE_NAME_MAX];
    int  n_defs;
    char incs[ARKMAKE_MAX_INCS][ARKMAKE_PATH_MAX];
    int  n_incs;
} arkmake_project_t;

/* 解析一份 makefile 文本。成功返回 0，失败返回 ARKMAKE_ERR_* 并在 err 里
 * 写一条带行号的说明（errcap 至少 1；写不下就截断说明，不截断判断）。
 * 解析失败时 p 的内容无意义，调用方不得使用。 */
int arkmake_parse_text(const char *text, arkmake_project_t *p,
                       char *err, size_t errcap);

/* 读文件 + 解析。read 由调用方给（板上是 vfs，主机是 stdio），
 * 这样这一层不依赖任何文件系统。返回 0 或 ARKMAKE_ERR_*。 */
typedef int (*arkmake_read_fn)(void *ctx, const char *path,
                               char *buf, size_t cap, size_t *out_len);
int arkmake_parse_file(arkmake_read_fn read, void *ctx, const char *path,
                       char *buf, size_t cap,
                       arkmake_project_t *p, char *err, size_t errcap);

/* "<target>.svcm"（不含目录）。返回 0 或 ARKMAKE_ERR_VALUE_LONG。 */
int arkmake_default_out(const arkmake_project_t *p, char *buf, size_t cap);

/* 构建驱动：把工程交给调用方的 pack 回调。两条口令都要打出来——
 * 构建是一次几十秒的操作，"它到底读了哪几份源码"必须在屏幕上。 */
typedef struct {
    /* 打一行日志（实现自己补换行）。返回 0 表示打印成功，非 0 忽略。 */
    int (*log)(void *ctx, const char *line);
    /* 把这份工程打成 out（四遍差分的打包器）。传的是整个 project_t 而不是
     * 只是 srcs：DEFS/INCS 改变的是**编译结果**，漏传就是打出另一份镜像。
     * 返回 0 成功，负值为实现自定义错误码；arkmake_build 原样上传。 */
    int (*pack)(void *ctx, const arkmake_project_t *p, const char *out);
} arkmake_ops_t;

/* 构建一次。ops->pack 为 NULL 或 srcs 为空都返回 ARKMAKE_ERR_ARG。 */
int arkmake_build(const arkmake_project_t *p, const arkmake_ops_t *ops, void *ctx);

const char *arkmake_error_name(int code);

#endif /* ARKMAKE_H */
