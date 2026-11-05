/* ============================================================
 * arkmake_build.c - 工程描述 -> 一次构建
 *
 * 这一层不碰编译器，也不碰文件系统：它只知道"把这几份源码打成这个产物"，
 * 具体怎么做由调用方的 pack 回调决定。板上那份实现（arkrtos 的接线）就是
 * `cc --pack` 的核心函数——**同一份实现**，不是另写一遍，否则两条路会打出
 * 不同的镜像，而且只有一条被验证过。
 * ============================================================ */
#include "arkmake.h"

#include <string.h>

static size_t str_len(const char *s)
{
    size_t n = 0;

    while ((s != NULL) && (s[n] != '\0')) {
        n++;
    }
    return n;
}

int arkmake_build(const arkmake_project_t *p, const arkmake_ops_t *ops, void *ctx)
{
    const char *srcs[ARKMAKE_MAX_SRCS];
    char        out[ARKMAKE_PATH_MAX];
    char        line[ARKMAKE_PATH_MAX + ARKMAKE_NAME_MAX + 32];
    int         i;
    int         rc;
    size_t      n;

    if ((p == NULL) || (ops == NULL) || (ops->pack == NULL) ||
        (p->n_srcs <= 0) || (p->n_srcs > ARKMAKE_MAX_SRCS)) {
        return ARKMAKE_ERR_ARG;
    }
    for (i = 0; i < p->n_srcs; i++) {
        if (p->srcs[i][0] == '\0') {
            return ARKMAKE_ERR_ARG;
        }
        srcs[i] = p->srcs[i];
    }

    if (p->out[0] != '\0') {
        n = str_len(p->out);
        if (n + 1u > sizeof(out)) {
            return ARKMAKE_ERR_VALUE_LONG;
        }
        memcpy(out, p->out, n + 1u);
    } else {
        rc = arkmake_default_out(p, out, sizeof(out));
        if (rc != ARKMAKE_OK) {
            return rc;
        }
    }

    /* 先把"要做什么"整条说清楚：构建几十秒，中途没有别的读数，而
     * "少了一份源码"这件事只能在这一行里看出来。 */
    if (ops->log != NULL) {
        size_t o = 0;
        const char *parts[] = { "make: ", NULL, " <- ", NULL, NULL };
        int k;

        parts[1] = out;
        /* 第一份源也要在 parts 里：循环在遇到 NULL 时就停，把它留成 NULL
         * 会打出一行 "make: out <-  (+more)"——把"要编什么"漏掉，而这行
         * 正是唯一能看出"少了一份源码"的地方（镜像照样链接成功）。 */
        parts[3] = p->srcs[0];
        for (k = 0; (k < 5) && (parts[k] != NULL); k++) {
            const char *s = parts[k];
            while ((s != NULL) && (*s != '\0') && (o + 1 < sizeof(line))) {
                line[o++] = *s++;
            }
        }
        if (p->n_srcs > 1) {
            const char *more = " (+more)";
            while ((*more != '\0') && (o + 1 < sizeof(line))) {
                line[o++] = *more++;
            }
        }
        line[o] = '\0';
        (void)ops->log(ctx, line);

        if (p->n_srcs > 1) {
            for (i = 1; i < p->n_srcs; i++) {
                size_t q = 0;
                const char *pre = "make:   src ";
                while ((*pre != '\0') && (q + 1 < sizeof(line))) {
                    line[q++] = *pre++;
                }
                {
                    const char *s = p->srcs[i];
                    while ((*s != '\0') && (q + 1 < sizeof(line))) {
                        line[q++] = *s++;
                    }
                }
                line[q] = '\0';
                (void)ops->log(ctx, line);
            }
        }
        if (p->n_defs > 0 || p->n_incs > 0) {
            /* 宏与头文件目录也要报：它们改变的是**编译结果**，不是路径。 */
            size_t q = 0;
            const char *pre = "make:   ";
            while ((*pre != '\0') && (q + 1 < sizeof(line))) {
                line[q++] = *pre++;
            }
            for (i = 0; i < p->n_defs; i++) {
                const char *s = p->defs[i];
                if (q + 3 < sizeof(line)) {
                    line[q++] = '-';
                    line[q++] = 'D';
                }
                while ((*s != '\0') && (q + 1 < sizeof(line))) {
                    line[q++] = *s++;
                }
                if (q + 1 < sizeof(line)) {
                    line[q++] = ' ';
                }
            }
            for (i = 0; i < p->n_incs; i++) {
                const char *s = p->incs[i];
                if (q + 3 < sizeof(line)) {
                    line[q++] = '-';
                    line[q++] = 'I';
                }
                while ((*s != '\0') && (q + 1 < sizeof(line))) {
                    line[q++] = *s++;
                }
                if (q + 1 < sizeof(line)) {
                    line[q++] = ' ';
                }
            }
            line[q] = '\0';
            (void)ops->log(ctx, line);
        }
    }

    (void)srcs;
    rc = ops->pack(ctx, p, out);
    if (rc == 0) {
        return ARKMAKE_OK;
    }
    /* 打包器的负码各是各的原因（对象装不下 / 四遍尺寸不一致 / 没有向量表 /
     * 产物写不下去）。这里只翻译"最外层"的那一类，其余照原码上传，让上层
     * 能拿到真实原因而不是一个笼统的"构建失败"。 */
    return (rc < 0) ? rc : ARKMAKE_ERR_LINK;
}
