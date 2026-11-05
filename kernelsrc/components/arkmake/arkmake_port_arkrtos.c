/* ============================================================
 * arkmake_port_arkrtos.c - 把 arkmake 的三个回调接到板上
 *
 *   读   -> arkc 端口的 fs_read_file（VFS 命名空间 + 内存对象表 + ramfs/NOR）
 *   日志 -> ark_shell_printf
 *   打包 -> arkc_cc_pack（就是 `cc --pack` 的本体）
 *
 * 第三条是这份接线的全部理由。板上已经有一条被验证过的"多份源码 -> 一份
 * 可运行镜像"的实现（四遍基址差分），arkmake 不能再写第二条：两条路迟早
 * 会打出不一样的镜像，而且只有一条被上板跑过。所以这里不做任何编译工作，
 * 只把 arkmake_project_t 摊成 arkc_cc_pack() 的参数。
 *
 * 摊平不是多余的：arkmake 的 srcs/defs/incs 是定长二维数组（每项一个
 * PATH_MAX 宽的槽），cc_pack 要的是 char* 指针数组。少摊一项（比如忘记
 * defs）不会报错——只会打出一份"没带这个 -D"的镜像，而那正是最不该静默
 * 发生的事。
 * ============================================================ */
#include "arkmake_port.h"

#include "include/flash_fs.h"      /* fs_read_file / fs_last_error */
#include "arkc_shell_cmd.h"        /* arkc_cc_pack */

#include "ark_shell.h"             /* ark_shell_printf */

#include <stddef.h>

/* 与 `cc --pack` 的缺省优化档一致。四遍必须同档，否则四份镜像的字节差
 * 里混进了优化差异，差分会把它当成"地址造成的位移"补掉——错得很安静。 */
#define ARKMAKE_OPT_LEVEL 1

static int make_read(void *ctx, const char *path,
                     char *buf, size_t cap, size_t *out_len)
{
    int n;

    (void)ctx;
    if ((buf == NULL) || (path == NULL) || (out_len == NULL) || (cap < 2u)) {
        return -1;
    }
    /* fs_read_file 的第三个参数是 int 且要求 max_len >= 1：传 cap 意味着
     * 最多读 cap-1 字节、末端留一个 '\0'，正好是 arkmake 期望的形态
     * （它随后自己再写一次 buf[len] = '\0'）。 */
    n = fs_read_file(path, buf, (int)cap);
    if (n < 0) {
        return -1;
    }
    *out_len = (size_t)n;
    return 0;
}

int arkmake_port_read(void *ctx, const char *path,
                      char *buf, size_t cap, size_t *out_len)
{
    return make_read(ctx, path, buf, cap, out_len);
}

static int make_log(void *ctx, const char *line)
{
    (void)ctx;
    if (line == NULL) {
        return -1;
    }
    ark_shell_printf("%s\r\n", line);
    return 0;
}

/* arkc 的失败阶段 -> arkmake 的错误词汇。
 *
 * 这一步不是美化措辞，而是为了不给出错答案：一次 --pack 里编译、链接、布局
 * 校验、差分打包、写盘五段都可能失败，而"哪一段"决定了下一步该看什么。
 * 全都翻成 link failed 会把人送去查链接器，而真正的问题是池子不够。
 *
 * 返回 0 表示成功。 */
static int make_phase_to_code(int phase)
{
    switch (phase) {
    case ARKC_PACK_PHASE_COMPILE: return ARKMAKE_ERR_COMPILE;
    case ARKC_PACK_PHASE_LINK:    return ARKMAKE_ERR_LINK;
    case ARKC_PACK_PHASE_LAYOUT:  return ARKMAKE_ERR_SIZE;
    case ARKC_PACK_PHASE_PACK:    return ARKMAKE_ERR_PACK;
    case ARKC_PACK_PHASE_WRITE:   return ARKMAKE_ERR_WRITE;
    case ARKC_PACK_PHASE_SETUP:   return ARKMAKE_ERR_SETUP;
    default:                      return ARKMAKE_ERR_LINK;
    }
}

static int make_pack(void *ctx, const arkmake_project_t *p, const char *out)
{
    const arkmake_ctx_t *c = (const arkmake_ctx_t *)ctx;
    int phase = ARKC_PACK_PHASE_NONE;
    int rc;
    const char *srcs[ARKMAKE_MAX_SRCS];
    const char *defs[ARKMAKE_MAX_DEFS];
    const char *incs[ARKMAKE_MAX_INCS];
    int i;

    if ((p == NULL) || (out == NULL)) {
        return -1;
    }
    if ((p->n_srcs <= 0) || (p->n_srcs > ARKMAKE_MAX_SRCS) ||
        (p->n_defs > ARKMAKE_MAX_DEFS) || (p->n_incs > ARKMAKE_MAX_INCS)) {
        /* 解析层已经挡过上限；这里是第二道，因为一份越界的 project_t
         * 会把栈上的指针数组读穿，而那是内存破坏，不是"参数不对"。 */
        return -1;
    }
    for (i = 0; i < p->n_srcs; i++) {
        srcs[i] = p->srcs[i];
    }
    for (i = 0; i < p->n_defs; i++) {
        defs[i] = p->defs[i];
    }
    for (i = 0; i < p->n_incs; i++) {
        incs[i] = p->incs[i];
    }

    rc = arkc_cc_pack(srcs, p->n_srcs, out,
                      defs, p->n_defs, incs, p->n_incs,
                      ARKMAKE_OPT_LEVEL, (c != NULL) ? c->verbose : 0, &phase);
    if (rc == 0) {
        return 0;
    }
    /* cc_pack 已经把具体原因打在屏幕上（哪一遍、哪个对象、哪个尺寸不符），
     * 这里只负责把阶段翻成 arkmake 的错误码，让上层那句话说得准。 */
    return make_phase_to_code(phase);
}

void arkmake_port_ops(arkmake_ops_t *ops)
{
    if (ops == NULL) {
        return;
    }
    ops->log  = make_log;
    ops->pack = make_pack;
}
