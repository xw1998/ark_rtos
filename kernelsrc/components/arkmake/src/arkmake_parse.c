/* ============================================================
 * arkmake_parse.c - makefile 文本 -> arkmake_project_t
 *
 * 只认三样语法（见 arkmake.h）：`NAME = value`、`NAME += value`、
 * `#` 注释；值里的 `$(NAME)` 递归展开。其余一律报错。
 *
 * 为什么解析和构建分在两个文件
 * ----------------------------
 * 解析是纯函数（给一段文本，得一份工程描述），可以在主机上跑几百个用例；
 * 构建要真的调编译器、真的花几十秒。混在一起时"改一行解析"就得烧板验证，
 * 这个项目里已经有太多这样的代价了。
 * ============================================================ */
#include "arkmake.h"

#include <string.h>

/* ------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------ */

static int is_space(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static int is_name_char(char c)
{
    return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || (c == '_'));
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if ((dst == NULL) || (cap == 0)) {
        return;
    }
    while ((src != NULL) && (src[i] != '\0') && (i + 1 < cap)) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static size_t str_len(const char *s)
{
    size_t n = 0;

    while ((s != NULL) && (s[n] != '\0')) {
        n++;
    }
    return n;
}

/* 把 err 写成 "line N: <msg>"。写不下就截断说明，但 keep NUL 一定在。 */
static void set_err(char *err, size_t cap, int line, const char *msg)
{
    char  num[12];
    size_t i = 0;
    size_t n = 0;
    int    v = line;

    if ((err == NULL) || (cap == 0)) {
        return;
    }
    /* 不引 stdio 的 snprintf：板上这条路径不该牵出格式化库。 */
    do {
        num[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while ((v > 0) && (i < sizeof(num)));
    {
        static const char pre[] = "line ";
        size_t k;
        for (k = 0; (k < sizeof(pre) - 1) && (n + 1 < cap); k++) {
            err[n++] = pre[k];
        }
    }
    while ((i > 0) && (n + 1 < cap)) {
        err[n++] = num[--i];
    }
    if (n + 1 < cap) {
        err[n++] = ':';
        err[n++] = ' ';
    }
    if (n + 1 < cap) {
        err[n++] = ' ';
    }
    while ((msg != NULL) && (*msg != '\0') && (n + 1 < cap)) {
        err[n++] = *msg++;
    }
    err[n] = '\0';
}

/* ------------------------------------------------------------
 * 变量表
 * ------------------------------------------------------------ */

typedef struct {
    arkmake_var_t v[ARKMAKE_MAX_VARS];
    int           n;
} var_table_t;

static const arkmake_var_t *var_find(const var_table_t *t, const char *name, size_t len)
{
    int i;

    for (i = 0; i < t->n; i++) {
        if ((str_len(t->v[i].name) == len) &&
            (strncmp(t->v[i].name, name, len) == 0)) {
            return &t->v[i];
        }
    }
    return NULL;
}

/* 展开 $(NAME)。返回值：0 成功，ARKMAKE_ERR_* 失败。
 * 递归深度由 depth 控制：`A = $(A)` 必须在第 9 层被拦下，而不是把栈跑穿。 */
static int expand(const var_table_t *vt, const char *src, int depth,
                  char *dst, size_t cap)
{
    size_t o = 0;

    while ((src != NULL) && (*src != '\0')) {
        if ((src[0] == '$') && (src[1] == '(')) {
            const char *close = strchr(src + 2, ')');
            const arkmake_var_t *v;
            char tmp[ARKMAKE_PATH_MAX];
            size_t k;

            if (close == NULL) {
                return ARKMAKE_ERR_SYNTAX;      /* 少一个 ')' 不猜 */
            }
            if (depth >= ARKMAKE_EXPAND_DEPTH) {
                return ARKMAKE_ERR_RECURSE;
            }
            v = var_find(vt, src + 2, (size_t)(close - (src + 2)));
            if (v == NULL) {
                return ARKMAKE_ERR_UNDEF;
            }
            if (expand(vt, v->value, depth + 1, tmp, sizeof(tmp)) != 0) {
                return ARKMAKE_ERR_RECURSE;     /* 内层失败一并归到递归/未定义 */
            }
            for (k = 0; (tmp[k] != '\0') && (o + 1 < cap); k++) {
                dst[o++] = tmp[k];
            }
            src = close + 1;
            continue;
        }
        if (o + 1 >= cap) {
            return ARKMAKE_ERR_VALUE_LONG;      /* 不截断值 */
        }
        dst[o++] = *src++;
    }
    if (cap > 0) {
        dst[o] = '\0';
    }
    return 0;
}

/* ------------------------------------------------------------
 * 词法切分：按空白/逗号切，最多 max 个词。返回词数，-1 表示太多。
 * 每个词必须装进 width 字节，否则 ARKMAKE_ERR_VALUE_LONG。
 * ------------------------------------------------------------ */
static int split_words(const char *s, char *base, int width, int stride,
                       int max, int *err)
{
    int n = 0;

    *err = 0;
    while ((s != NULL) && (*s != '\0')) {
        size_t k = 0;
        char  *slot;

        while (is_space(*s) || (*s == ',')) {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        if (n >= max) {
            *err = ARKMAKE_ERR_TOO_MANY;
            return n;
        }
        slot = base + ((size_t)n * (size_t)stride);
        while ((*s != '\0') && !is_space(*s) && (*s != ',')) {
            if ((int)k + 1 >= width) {
                *err = ARKMAKE_ERR_VALUE_LONG;
                return n;
            }
            slot[k++] = *s++;
        }
        slot[k] = '\0';
        n++;
    }
    return n;
}

/* ------------------------------------------------------------
 * 解析
 * ------------------------------------------------------------ */

int arkmake_parse_text(const char *text, arkmake_project_t *p,
                       char *err, size_t errcap)
{
    var_table_t vt;
    const char *s = text;
    int line = 0;
    int rc;

    if ((text == NULL) || (p == NULL)) {
        return ARKMAKE_ERR_ARG;
    }
    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    memset(p, 0, sizeof(*p));
    memset(&vt, 0, sizeof(vt));

    while (*s != '\0') {
        const char *eol = strchr(s, '\n');
        const char *end = (eol != NULL) ? eol : (s + str_len(s));
        const char *q = s;
        const char *name;
        size_t      name_len;
        int         append = 0;
        char        raw[ARKMAKE_PATH_MAX];

        line++;
        while ((q < end) && is_space(*q)) {
            q++;
        }
        if ((q >= end) || (*q == '#')) {
            s = (eol != NULL) ? (eol + 1) : end;
            continue;                        /* 空行 / 注释行 */
        }

        /* NAME, then '=' or '+=' */
        name = q;
        while ((q < end) && (is_name_char(*q) || (*q == ' '))) {
            q++;
        }
        name_len = (size_t)(q - name);
        while ((name_len > 0) && (name[name_len - 1] == ' ')) {
            name_len--;                      /* `TARGET = x` 里的尾空格 */
        }
        if ((name_len == 0) || (name_len >= ARKMAKE_NAME_MAX)) {
            set_err(err, errcap, line,
                    "expected `NAME = value` (variable name missing or too long)");
            return (name_len == 0) ? ARKMAKE_ERR_SYNTAX : ARKMAKE_ERR_NAME;
        }
        if ((q < end) && (*q == '+')) {
            append = 1;
            q++;
        }
        if ((q >= end) || (*q != '=')) {
            set_err(err, errcap, line,
                    "expected `=` after the variable name (rules and recipes "
                    "are not supported)");
            return ARKMAKE_ERR_SYNTAX;
        }
        q++;
        {
            /* 值：去掉行尾注释与首尾空白。'#' 出现在值中间时也算注释起点，
             * 这与 make 一致，且路径里出现 '#' 在本板没有先例。 */
            const char *v = q;
            const char *v_end = end;
            size_t      k = 0;
            char        expanded[ARKMAKE_PATH_MAX];

            while ((v < end) && is_space(*v)) {
                v++;
            }
            {
                const char *h = v;
                while (h < end) {
                    if (*h == '#') {
                        v_end = h;
                        break;
                    }
                    h++;
                }
            }
            while ((v_end > v) && is_space(v_end[-1])) {
                v_end--;
            }
            if ((size_t)(v_end - v) >= sizeof(raw)) {
                set_err(err, errcap, line, "value is longer than the field that holds it");
                return ARKMAKE_ERR_VALUE_LONG;
            }
            for (k = 0; k < (size_t)(v_end - v); k++) {
                raw[k] = v[k];
            }
            raw[k] = '\0';

            /* 变量表里存**原文**，展开发生在使用点：这样 `SRCS = $(DIR)/a.c`
             * 在 DIR 后来被改掉时仍然按最终值展开（与 make 的递归变量一致）。 */
            {
                const arkmake_var_t *old = var_find(&vt, name, name_len);
                char merged[ARKMAKE_PATH_MAX];

                if ((append != 0) && (old != NULL)) {
                    if (str_len(old->value) + 1u + str_len(raw) >= sizeof(merged)) {
                        set_err(err, errcap, line, "appended value is too long");
                        return ARKMAKE_ERR_VALUE_LONG;
                    }
                    copy_str(merged, sizeof(merged), old->value);
                    {
                        size_t at = str_len(merged);
                        merged[at++] = ' ';
                        merged[at] = '\0';
                    }
                    copy_str(merged + str_len(merged), sizeof(merged) - str_len(merged), raw);
                    copy_str(raw, sizeof(raw), merged);
                } else if (append != 0) {
                    set_err(err, errcap, line, "`+=` on a variable that is not defined yet");
                    return ARKMAKE_ERR_UNDEF;
                }

                if (old != NULL) {
                    /* 覆盖/追加同一个变量：原地更新，名字保持第一次出现的位置 */
                    arkmake_var_t *w = (arkmake_var_t *)old;
                    copy_str(w->value, sizeof(w->value), raw);
                } else {
                    if (vt.n >= ARKMAKE_MAX_VARS) {
                        set_err(err, errcap, line, "too many variables");
                        return ARKMAKE_ERR_TOO_MANY;
                    }
                    {
                        size_t i;
                        for (i = 0; i < name_len; i++) {
                            vt.v[vt.n].name[i] = name[i];
                        }
                        vt.v[vt.n].name[name_len] = '\0';
                        copy_str(vt.v[vt.n].value, sizeof(vt.v[vt.n].value), raw);
                        vt.n++;
                    }
                }
            }

            /* 展开一次，只为确认这段值**能**展开：未定义变量/自引用在这里
             * 就报错，而不是等到构建时才炸。 */
            rc = expand(&vt, raw, 0, expanded, sizeof(expanded));
            if (rc != 0) {
                if (rc == ARKMAKE_ERR_UNDEF) {
                    set_err(err, errcap, line, "value uses a variable that is not defined");
                } else if (rc == ARKMAKE_ERR_RECURSE) {
                    set_err(err, errcap, line, "value expands into itself");
                } else {
                    set_err(err, errcap, line, "value does not expand");
                }
                return rc;
            }
        }
        s = (eol != NULL) ? (eol + 1) : end;
    }

    /* 把变量取成工程字段。全部经过同一段 expand + split，所以
     * "解析通过"就等于"字段装得下"。 */
    {
        static const char *const k_srcs = "SRCS";
        static const char *const k_defs = "DEFS";
        static const char *const k_incs = "INCS";
        const arkmake_var_t *v;
        int e;

        v = var_find(&vt, "TARGET", 6);
        if (v == NULL) {
            set_err(err, errcap, 0, "TARGET is not set (nothing to name the image after)");
            return ARKMAKE_ERR_NO_TARGET;
        }
        if (expand(&vt, v->value, 0, p->target, sizeof(p->target)) != 0) {
            set_err(err, errcap, 0, "TARGET does not expand");
            return ARKMAKE_ERR_UNDEF;
        }
        v = var_find(&vt, "OUT", 3);
        if (v != NULL) {
            if (expand(&vt, v->value, 0, p->out, sizeof(p->out)) != 0) {
                set_err(err, errcap, 0, "OUT does not expand");
                return ARKMAKE_ERR_UNDEF;
            }
        }

        v = var_find(&vt, k_srcs, 4);
        if (v == NULL) {
            set_err(err, errcap, 0, "SRCS is not set");
            return ARKMAKE_ERR_NO_SRCS;
        } else {
            char flat[ARKMAKE_PATH_MAX];
            if (expand(&vt, v->value, 0, flat, sizeof(flat)) != 0) {
                set_err(err, errcap, 0, "SRCS does not expand");
                return ARKMAKE_ERR_UNDEF;
            }
            p->n_srcs = split_words(flat, &p->srcs[0][0], ARKMAKE_PATH_MAX,
                                    ARKMAKE_PATH_MAX, ARKMAKE_MAX_SRCS, &e);
            if (e != 0) {
                set_err(err, errcap, 0,
                        (e == ARKMAKE_ERR_TOO_MANY) ?
                        "more sources than the compiler's object slots (4)" :
                        "a source path is too long");
                return e;
            }
            if (p->n_srcs == 0) {
                set_err(err, errcap, 0, "SRCS is empty");
                return ARKMAKE_ERR_NO_SRCS;
            }
        }

        v = var_find(&vt, k_defs, 4);
        if (v != NULL) {
            char flat[ARKMAKE_PATH_MAX];
            if (expand(&vt, v->value, 0, flat, sizeof(flat)) != 0) {
                set_err(err, errcap, 0, "DEFS does not expand");
                return ARKMAKE_ERR_UNDEF;
            }
            p->n_defs = split_words(flat, &p->defs[0][0], ARKMAKE_NAME_MAX,
                                    ARKMAKE_NAME_MAX, ARKMAKE_MAX_DEFS, &e);
            if (e != 0) {
                set_err(err, errcap, 0, "DEFS has too many or too long entries");
                return e;
            }
        }

        v = var_find(&vt, k_incs, 4);
        if (v != NULL) {
            char flat[ARKMAKE_PATH_MAX];
            if (expand(&vt, v->value, 0, flat, sizeof(flat)) != 0) {
                set_err(err, errcap, 0, "INCS does not expand");
                return ARKMAKE_ERR_UNDEF;
            }
            p->n_incs = split_words(flat, &p->incs[0][0], ARKMAKE_PATH_MAX,
                                    ARKMAKE_PATH_MAX, ARKMAKE_MAX_INCS, &e);
            if (e != 0) {
                set_err(err, errcap, 0, "INCS has too many or too long entries");
                return e;
            }
        }
    }
    return ARKMAKE_OK;
}

int arkmake_parse_file(arkmake_read_fn read, void *ctx, const char *path,
                       char *buf, size_t cap,
                       arkmake_project_t *p, char *err, size_t errcap)
{
    size_t len = 0;
    int    rc;

    if ((read == NULL) || (buf == NULL) || (cap < 2) || (path == NULL)) {
        return ARKMAKE_ERR_ARG;
    }
    rc = read(ctx, path, buf, cap, &len);
    if (rc != 0) {
        set_err(err, errcap, 0, "cannot read the makefile");
        return rc;
    }
    if (len > cap) {
        /* read 契约：装不下就报错（不截断），所以这里只做自检。 */
        set_err(err, errcap, 0, "the makefile is larger than the read buffer");
        return ARKMAKE_ERR_VALUE_LONG;
    }
    buf[len] = '\0';
    return arkmake_parse_text(buf, p, err, errcap);
}

int arkmake_default_out(const arkmake_project_t *p, char *buf, size_t cap)
{
    size_t n;

    if ((p == NULL) || (buf == NULL) || (cap == 0)) {
        return ARKMAKE_ERR_ARG;
    }
    n = str_len(p->target);
    if (n + 6u >= cap) {                     /* ".svcm" + NUL */
        return ARKMAKE_ERR_VALUE_LONG;
    }
    copy_str(buf, cap, p->target);
    copy_str(buf + n, cap - n, ".svcm");
    return ARKMAKE_OK;
}

const char *arkmake_error_name(int code)
{
    switch (code) {
    case ARKMAKE_OK:                return "ok";
    case ARKMAKE_ERR_ARG:           return "invalid argument";
    case ARKMAKE_ERR_SYNTAX:        return "syntax error";
    case ARKMAKE_ERR_NAME:          return "bad variable name";
    case ARKMAKE_ERR_VALUE_LONG:    return "value too long";
    case ARKMAKE_ERR_UNDEF:         return "undefined variable";
    case ARKMAKE_ERR_RECURSE:       return "recursive variable";
    case ARKMAKE_ERR_NO_TARGET:     return "TARGET missing";
    case ARKMAKE_ERR_NO_SRCS:       return "SRCS missing or empty";
    case ARKMAKE_ERR_TOO_MANY:      return "too many entries";
    case ARKMAKE_ERR_SIZE:          return "image too large";
    case ARKMAKE_ERR_COMPILE:       return "compile failed";
    case ARKMAKE_ERR_LINK:          return "link failed";
    case ARKMAKE_ERR_WRITE:         return "cannot write the image";
    case ARKMAKE_ERR_UNSUPPORTED:   return "unsupported";
    case ARKMAKE_ERR_SETUP:         return "cannot start the build";
    case ARKMAKE_ERR_PACK:          return "cannot pack the image";
    default:                        return "unknown";
    }
}
