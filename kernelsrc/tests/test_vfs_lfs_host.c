/**
* @file test_vfs_lfs_host.c
* @brief arkrtos_vfs_lfs.c 的宿主用例：在 RAM 里搭一个假 littlefs，逐个回调验语义
* @details 这里不链接真的 littlefs，也不起 ark_vfs 核心：直接把
*          arkrtos_vfs_lfs_fsdrv 的每个回调当普通函数调用，配合 15 个
*          arkrtos_fs_* 桩。被验的是桥本身的三条承诺，不是 littlefs：
*
*            1. 读走 read_at（随机访问，可多 fd 同时读），写走单一写流；
*            2. 同一时刻只允许一个写句柄，有写流时读/截断回 BUSY；
*            3. 不做得到的事（写句柄随机 seek、非零 truncate）报 NOSYS，
*               不静默降级。
*
*          桩的失败注入只用来走到「出错分支」，返回码由桥自己决定，
*          不用 littlefs 的具体数字去猜。
*
* @author xw
* @date 2026.11.22
*/
#include <stdio.h>
#include <string.h>

#include "ark_vfs.h"
#include "arkrtos_fs.h"
#include "arkrtos_log.h"

/* 被验模块的驱动表（源文件只有定义，没有对外头文件声明） */
extern const ark_vfs_fsdrv_t arkrtos_vfs_lfs_fsdrv;

/* ------------------------------------------------------------
 * 计数
 * ------------------------------------------------------------ */

static int g_checks;
static int g_fails;

#define CK(cond)                                                          \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            g_fails++;                                                    \
        }                                                                 \
    } while (0)

#define CK_I(got, want)                                                   \
    do {                                                                  \
        long g_ = (long)(got);                                            \
        long w_ = (long)(want);                                           \
        g_checks++;                                                       \
        if (g_ != w_) {                                                   \
            printf("FAIL %s:%d  got %ld want %ld\n", __FILE__, __LINE__,  \
                   g_, w_);                                               \
            g_fails++;                                                    \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------
 * 假 littlefs：一张定长节点表 + 一条写流
 * ------------------------------------------------------------ */

#define FS_MAX_NODES 24
#define FS_DATA_MAX  512
#define FS_ERR_NOENT (-2)
#define FS_ERR_NOSPC (-28)

typedef struct
{
    uint8  used;
    uint8  is_dir;
    uint32 size;
    char   path[ARKRTOS_FS_PATH_MAX];
    uint8  data[FS_DATA_MAX];
} fs_node_t;

static fs_node_t g_nodes[FS_MAX_NODES];
static uint8     g_mounted;
static int32     g_last_err;
static uint8     g_last_noent;
static uint8     g_last_nospc;
static uint8     g_writer_open;
static int       g_writer_node;         /* -1 = 没有写流 */
static uint32    g_writer_pos;

/* 失败注入 */
static uint8 g_f_stat_io;
static uint8 g_f_open_write;
static uint8 g_f_open_write_noent;
static uint8 g_f_write_next;
static uint8 g_f_close_write;
static uint8 g_f_read_at;
static uint8 g_f_remove;
static uint8 g_f_rename;
static uint8 g_f_mkdir;
static uint8 g_f_list;
static uint8 g_f_trunc_nospc;
static uint8 g_f_trunc_io;

static void fs_fail(int32 err, uint8 noent, uint8 nospc)
{
    g_last_err   = err;
    g_last_noent = noent;
    g_last_nospc = nospc;
}

static int fs_find(const char *path)
{
    int i;

    for (i = 0; i < FS_MAX_NODES; i++)
    {
        if ((g_nodes[i].used != 0u) && (strcmp(g_nodes[i].path, path) == 0))
        {
            return i;
        }
    }
    return -1;
}

static int fs_create(const char *path, uint8 is_dir)
{
    int i;

    for (i = 0; i < FS_MAX_NODES; i++)
    {
        if (g_nodes[i].used == 0u)
        {
            memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
            g_nodes[i].used   = 1u;
            g_nodes[i].is_dir = is_dir;
            strncpy(g_nodes[i].path, path, ARKRTOS_FS_PATH_MAX - 1u);
            g_nodes[i].path[ARKRTOS_FS_PATH_MAX - 1u] = '\0';
            return i;
        }
    }
    return -1;
}

/* path 的父目录是否等于 dir（dir 形如 "/" 或 "/d"，无尾斜杠） */
static int fs_parent_is(const char *path, const char *dir)
{
    const char *slash = strrchr(path, '/');
    size_t      dlen  = strlen(dir);

    if (slash == NULL)
    {
        return 0;
    }
    if (slash == path)
    {
        return (strcmp(dir, "/") == 0) ? 1 : 0;
    }
    if (strlen(dir) != (size_t)(slash - path))
    {
        return 0;
    }
    return (strncmp(path, dir, dlen) == 0) ? 1 : 0;
}

static void fs_reset(void)
{
    memset(g_nodes, 0, sizeof(g_nodes));
    g_mounted     = 1u;
    g_last_err    = 0;
    g_last_noent  = 0u;
    g_last_nospc  = 0u;
    g_writer_open = 0u;
    g_writer_node = -1;
    g_writer_pos  = 0u;

    g_f_stat_io = 0u;
    g_f_open_write = 0u;
    g_f_open_write_noent = 0u;
    g_f_write_next = 0u;
    g_f_close_write = 0u;
    g_f_read_at = 0u;
    g_f_remove = 0u;
    g_f_rename = 0u;
    g_f_mkdir = 0u;
    g_f_list = 0u;
    g_f_trunc_nospc = 0u;
    g_f_trunc_io = 0u;

    (void)fs_create("/", 1u);
}

/* 造一个文件，返回节点下标 */
static int fs_seed_file(const char *path, const char *text)
{
    int    i = fs_create(path, 0u);
    size_t n = strlen(text);

    if (i >= 0)
    {
        if (n > FS_DATA_MAX) { n = FS_DATA_MAX; }
        memcpy(g_nodes[i].data, text, n);
        g_nodes[i].size = (uint32)n;
    }
    return i;
}

/* ------------------------------------------------------------
 * arkrtos_fs_* 桩（15 个门面入口）
 * ------------------------------------------------------------ */

uint8 arkrtos_fs_mounted(void)
{
    return (g_mounted != 0u) ? 1u : 0u;
}

/* 日志：默认级别下会被调用，桩只计数，不输出 */
static int g_log_calls;

uint32 arkrtos_log_get_level(void)
{
    return 4u;                      /* ARKRTOS_LOG_DEBUG */
}

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line,
                      const char *msg)
{
    (void)level;
    (void)tag;
    (void)line;
    (void)msg;
    g_log_calls++;
}

int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_dir)
{
    int i;

    if (g_f_stat_io != 0u)
    {
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    if ((g_mounted == 0u) || (path == NULL))
    {
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    i = fs_find(path);
    if (i < 0)
    {
        fs_fail(FS_ERR_NOENT, 1u, 0u);
        return -1;
    }
    if (size != NULL) { *size = g_nodes[i].size; }
    if (is_dir != NULL) { *is_dir = (uint32)g_nodes[i].is_dir; }
    return 0;
}

int32 arkrtos_fs_open_write(const char *path)
{
    int i;

    if (g_f_open_write != 0u)
    {
        fs_fail(-5, g_f_open_write_noent, 0u);
        return -1;
    }
    i = fs_find(path);
    if (i < 0)
    {
        i = fs_create(path, 0u);
        if (i < 0)
        {
            fs_fail(FS_ERR_NOSPC, 0u, 1u);
            return -1;
        }
    }
    if (g_nodes[i].is_dir != 0u)
    {
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    g_nodes[i].size = 0u;
    g_writer_open = 1u;
    g_writer_node = i;
    g_writer_pos  = 0u;
    return 0;
}

int32 arkrtos_fs_write_next(const uint8 *buf, uint32 len)
{
    fs_node_t *nd;

    if (g_f_write_next != 0u)
    {
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    if ((g_writer_open == 0u) || (g_writer_node < 0))
    {
        return -1;
    }
    nd = &g_nodes[g_writer_node];
    if ((g_writer_pos + len) > FS_DATA_MAX)
    {
        fs_fail(FS_ERR_NOSPC, 0u, 1u);
        return -1;
    }
    memcpy(nd->data + g_writer_pos, buf, len);
    g_writer_pos += len;
    if (g_writer_pos > nd->size) { nd->size = g_writer_pos; }
    return 0;
}

int32 arkrtos_fs_close_write(void)
{
    if (g_f_close_write != 0u)
    {
        g_writer_open = 0u;
        g_writer_node = -1;
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    g_writer_open = 0u;
    g_writer_node = -1;
    g_writer_pos  = 0u;
    return 0;
}

int32 arkrtos_fs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off,
                         uint32 *out_len)
{
    int    i;
    uint32 n;

    if (g_f_read_at != 0u)
    {
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    i = fs_find(path);
    if (i < 0)
    {
        fs_fail(FS_ERR_NOENT, 1u, 0u);
        return -1;
    }
    if (off >= g_nodes[i].size)
    {
        *out_len = 0u;
        return 0;
    }
    n = g_nodes[i].size - off;
    if (n > len) { n = len; }
    memcpy(buf, g_nodes[i].data + off, n);
    *out_len = n;
    return 0;
}

int32 arkrtos_fs_last_error(void)
{
    return g_last_err;
}

const char *arkrtos_fs_error_name(int32 err)
{
    (void)err;
    return "stub";
}

uint8 arkrtos_fs_last_is_noent(void)
{
    return g_last_noent;
}

uint8 arkrtos_fs_last_is_nospc(void)
{
    return g_last_nospc;
}

int32 arkrtos_fs_remove(const char *path)
{
    int i;

    if (g_f_remove != 0u)
    {
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    i = fs_find(path);
    if (i < 0)
    {
        fs_fail(FS_ERR_NOENT, 1u, 0u);
        return -1;
    }
    g_nodes[i].used = 0u;
    return 0;
}

int32 arkrtos_fs_rename(const char *old_path, const char *new_path)
{
    int i;

    if (g_f_rename != 0u)
    {
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    i = fs_find(old_path);
    if (i < 0)
    {
        fs_fail(FS_ERR_NOENT, 1u, 0u);
        return -1;
    }
    if (fs_find(new_path) >= 0)
    {
        fs_fail(-6, 0u, 0u);
        return -1;
    }
    strncpy(g_nodes[i].path, new_path, ARKRTOS_FS_PATH_MAX - 1u);
    g_nodes[i].path[ARKRTOS_FS_PATH_MAX - 1u] = '\0';
    return 0;
}

int32 arkrtos_fs_mkdir(const char *path)
{
    int i;

    if (g_f_mkdir != 0u)
    {
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    i = fs_find(path);
    if (i >= 0)
    {
        fs_fail(-7, 0u, 0u);
        return -1;
    }
    i = fs_create(path, 1u);
    if (i < 0)
    {
        fs_fail(FS_ERR_NOSPC, 0u, 1u);
        return -1;
    }
    return 0;
}

int32 arkrtos_fs_list(const char *dir, arkrtos_fs_list_cb_t cb, void *arg)
{
    int i;

    if (g_f_list != 0u)
    {
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    if (cb == NULL) { return -1; }
    if (cb(".", 0u, 1u, arg) != 0) { return 0; }
    if (cb("..", 0u, 1u, arg) != 0) { return 0; }
    for (i = 0; i < FS_MAX_NODES; i++)
    {
        const char *base;

        if (g_nodes[i].used == 0u) { continue; }
        if (!fs_parent_is(g_nodes[i].path, dir)) { continue; }
        base = strrchr(g_nodes[i].path, '/');
        base = (base == NULL) ? g_nodes[i].path : (base + 1);
        if (base[0] == '\0') { continue; }      /* 根自己 */
        if (cb(base, g_nodes[i].size, g_nodes[i].is_dir, arg) != 0) { return 0; }
    }
    return 0;
}

int32 arkrtos_fs_truncate(const char *path, uint32 size)
{
    int i;

    if (g_f_trunc_nospc != 0u)
    {
        fs_fail(FS_ERR_NOSPC, 0u, 1u);
        return -1;
    }
    if (g_f_trunc_io != 0u)
    {
        fs_fail(-5, 0u, 0u);
        return -1;
    }
    i = fs_find(path);
    if (i < 0)
    {
        fs_fail(FS_ERR_NOENT, 1u, 0u);
        return -1;
    }
    if (size > FS_DATA_MAX)
    {
        fs_fail(FS_ERR_NOSPC, 0u, 1u);
        return -1;
    }
    if (size < g_nodes[i].size)
    {
        memset(g_nodes[i].data + size, 0, g_nodes[i].size - size);
    }
    else
    {
        memset(g_nodes[i].data + g_nodes[i].size, 0, size - g_nodes[i].size);
    }
    g_nodes[i].size = size;
    return 0;
}

/* ------------------------------------------------------------
 * 用例
 * ------------------------------------------------------------ */

static struct ark_vfs_mnt g_mnt;

static void mnt_reset(void)
{
    memset(&g_mnt, 0, sizeof(g_mnt));
    g_mnt.flags = 0u;
}

/* 打开一个只读句柄（文件须已存在），失败直接计入检查 */
static struct ark_vfs_file *open_ro(const char *rel)
{
    struct ark_vfs_file *f = NULL;
    int32_t rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, rel, ARK_O_RDONLY, &f);

    CK_I(rc, ARK_VFS_OK);
    return f;
}

static void test_driver_table(void)
{
    mnt_reset();
    fs_reset();

    CK(strcmp(arkrtos_vfs_lfs_fsdrv.name, "lfs") == 0);
    CK((arkrtos_vfs_lfs_fsdrv.caps & ARK_FS_CAP_READ) != 0u);
    CK((arkrtos_vfs_lfs_fsdrv.caps & ARK_FS_CAP_WRITE) != 0u);
    CK((arkrtos_vfs_lfs_fsdrv.caps & ARK_FS_CAP_CREATE) != 0u);
    CK((arkrtos_vfs_lfs_fsdrv.caps & ARK_FS_CAP_UNLINK) != 0u);
    CK((arkrtos_vfs_lfs_fsdrv.caps & ARK_FS_CAP_RENAME) != 0u);
    CK((arkrtos_vfs_lfs_fsdrv.caps & ARK_FS_CAP_MKDIR) != 0u);
    CK((arkrtos_vfs_lfs_fsdrv.caps & ARK_FS_CAP_DIR) != 0u);
    CK((arkrtos_vfs_lfs_fsdrv.caps & ARK_FS_CAP_STAT) != 0u);
    CK((arkrtos_vfs_lfs_fsdrv.caps & ARK_FS_CAP_TRUNC) != 0u);
    CK(arkrtos_vfs_lfs_fsdrv.chmod == NULL);
    CK(arkrtos_vfs_lfs_fsdrv.chown == NULL);
    CK(arkrtos_vfs_lfs_fsdrv.truncate != NULL);
    CK(arkrtos_vfs_lfs_fsdrv.opendir != NULL);
    CK(arkrtos_vfs_lfs_fsdrv.closedir != NULL);
}

static void test_mount(void)
{
    int32_t rc;

    mnt_reset();
    fs_reset();
    g_mounted = 0u;
    rc = arkrtos_vfs_lfs_fsdrv.mount(&g_mnt);
    CK_I(rc, ARK_E_IO);                  /* 没挂卷：明说，不自己去挂 */

    g_mounted = 1u;
    rc = arkrtos_vfs_lfs_fsdrv.mount(&g_mnt);
    CK_I(rc, ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.umount(&g_mnt), ARK_VFS_OK);
}

static void test_open(void)
{
    struct ark_vfs_file *f = NULL;
    struct ark_vfs_file *g = NULL;
    struct ark_vfs_file *h[ARK_VFS_MAX_FDS];
    uint32 size = 0u;
    int    i;
    int32_t rc;

    /* 挂载根：目录，不能用 open 打开 */
    mnt_reset();
    fs_reset();
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "", ARK_O_RDONLY, &f);
    CK_I(rc, ARK_E_ISDIR);

    /* 只读打开不存在 */
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "nofile", ARK_O_RDONLY, &f);
    CK_I(rc, ARK_E_NOENT);

    /* 写意图 + stat 失败但不是 NOENT：真错，报 IO 而不是「没有」 */
    g_f_stat_io = 1u;
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "x.txt", ARK_O_WRONLY | ARK_O_CREAT,
                                    &f);
    CK_I(rc, ARK_E_IO);
    g_f_stat_io = 0u;

    /* 写意图 + NOENT：新建成功 */
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "new.txt", ARK_O_WRONLY | ARK_O_CREAT,
                                    &f);
    CK_I(rc, ARK_VFS_OK);
    CK(f != NULL);
    if (f != NULL)
    {
        CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK);
    }
    CK_I(arkrtos_fs_stat_path("/new.txt", &size, NULL), 0);
    CK_I(size, 0);

    /* 父目录不存在：open_write 回 NOENT，桥不应报成 IO */
    g_f_open_write = 1u;
    g_f_open_write_noent = 1u;
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "nodir/x.txt",
                                    ARK_O_WRONLY | ARK_O_CREAT, &f);
    CK_I(rc, ARK_E_NOENT);
    g_f_open_write = 0u;
    g_f_open_write_noent = 0u;

    /* 写流打开失败且不是 NOENT：IO */
    g_f_open_write = 1u;
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "io.txt",
                                    ARK_O_WRONLY | ARK_O_CREAT, &f);
    CK_I(rc, ARK_E_IO);
    g_f_open_write = 0u;

    /* 目录：不带 O_DIRECTORY 拒绝，带上则成功 */
    CK(arkrtos_fs_mkdir("/d") == 0);
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "d", ARK_O_RDONLY, &f);
    CK_I(rc, ARK_E_ISDIR);
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "d", ARK_O_RDONLY | ARK_O_DIRECTORY,
                                    &f);
    CK_I(rc, ARK_VFS_OK);
    if (f != NULL) { CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK); }

    /* 只读挂载上的写意图 */
    (void)fs_seed_file("/ro.txt", "r");
    g_mnt.flags = ARK_MNT_RO;
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "ro.txt", ARK_O_WRONLY, &f);
    CK_I(rc, ARK_E_ROFS);
    g_mnt.flags = 0u;

    /* 第二个写句柄 -> BUSY */
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "w1.txt",
                                    ARK_O_WRONLY | ARK_O_CREAT, &f);
    CK_I(rc, ARK_VFS_OK);
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "w2.txt",
                                    ARK_O_WRONLY | ARK_O_CREAT, &g);
    CK_I(rc, ARK_E_BUSY);
    CK(g == NULL);                      /* 失败时 *out 不被写 */
    if (f != NULL) { CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK); }

    /* 写句柄失败后不留下半开的表项：还能再开一个写句柄 */
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "w3.txt",
                                    ARK_O_WRONLY | ARK_O_CREAT, &f);
    CK_I(rc, ARK_VFS_OK);
    if (f != NULL) { CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK); }

    /* fd 表满 */
    (void)arkrtos_vfs_lfs_fsdrv.mount(&g_mnt);      /* 清句柄表 */
    for (i = 0; i < ARK_VFS_MAX_FDS; i++)
    {
        h[i] = NULL;
        rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "d",
                                        ARK_O_RDONLY | ARK_O_DIRECTORY, &h[i]);
        CK_I(rc, ARK_VFS_OK);
    }
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "d", ARK_O_RDONLY | ARK_O_DIRECTORY,
                                    &f);
    CK_I(rc, ARK_E_FULL);
    for (i = 0; i < ARK_VFS_MAX_FDS; i++)
    {
        if (h[i] != NULL)
        {
            CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, h[i]), ARK_VFS_OK);
        }
    }

    /* O_APPEND：起点在文件尾（读 1 字节得 0 即证明 pos==size） */
    (void)fs_seed_file("/ap.txt", "hello");
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "ap.txt",
                                    ARK_O_RDONLY | ARK_O_APPEND, &f);
    CK_I(rc, ARK_VFS_OK);
    if (f != NULL)
    {
        uint8 b = 0u;
        CK_I(arkrtos_vfs_lfs_fsdrv.read(f, &b, 1u), 0);   /* 已在尾，读到 0 */
        CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK);
    }
}

static void test_close(void)
{
    struct ark_vfs_file *f = NULL;
    int32_t rc;

    mnt_reset();
    fs_reset();

    CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, NULL), ARK_E_INVAL);

    /* 写句柄关闭失败 -> IO；句柄仍归还，写流标记清掉 */
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "c.txt", ARK_O_WRONLY | ARK_O_CREAT,
                                    &f);
    CK_I(rc, ARK_VFS_OK);
    g_f_close_write = 1u;
    CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_E_IO);
    g_f_close_write = 0u;
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "c2.txt",
                                    ARK_O_WRONLY | ARK_O_CREAT, &f);
    CK_I(rc, ARK_VFS_OK);
    if (f != NULL) { CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK); }

    /* 只读句柄关闭恒成功 */
    (void)fs_seed_file("/r.txt", "abc");
    f = open_ro("r.txt");
    if (f != NULL) { CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK); }
}

static void test_read(void)
{
    struct ark_vfs_file *f = NULL;
    struct ark_vfs_file *w = NULL;
    struct ark_vfs_file *d = NULL;
    uint8  buf[16];
    int32_t rc;

    mnt_reset();
    fs_reset();
    (void)fs_seed_file("/a.txt", "abcdef");

    /* NULL 句柄 */
    CK_I(arkrtos_vfs_lfs_fsdrv.read(NULL, buf, 4u), ARK_E_BADF);

    /* 目录句柄 */
    CK(arkrtos_fs_mkdir("/dd") == 0);
    (void)arkrtos_vfs_lfs_fsdrv.opendir(&g_mnt, "dd", &d);
    CK(d != NULL);
    if (d != NULL)
    {
        CK_I(arkrtos_vfs_lfs_fsdrv.read(d, buf, 1u), ARK_E_BADF);
        CK_I(arkrtos_vfs_lfs_fsdrv.closedir(&g_mnt, d), ARK_VFS_OK);
    }

    f = open_ro("a.txt");
    if (f == NULL) { return; }

    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, NULL, 1u), ARK_E_INVAL);
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, buf, 0u), 0);
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, buf, 2u), 2);
    CK(buf[0] == 'a' && buf[1] == 'b');   /* 第二次读拿到剩余 4 字节，证明 pos 已前进 */
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, buf, 16u), 4);
    CK(buf[0] == 'c' && buf[3] == 'f');
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, buf, 16u), 0);   /* 文件尾不是错误 */

    /* 读失败 -> IO */
    g_f_read_at = 1u;
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, buf, 1u), ARK_E_IO);
    g_f_read_at = 0u;

    /* 有写流时读 -> BUSY */
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "w.txt", ARK_O_WRONLY | ARK_O_CREAT,
                                    &w);
    CK_I(rc, ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, buf, 1u), ARK_E_BUSY);
    if (w != NULL) { CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, w), ARK_VFS_OK); }
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, buf, 1u), 0);

    /* 写句柄上读 -> INVAL */
    rc = arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "w2.txt", ARK_O_WRONLY | ARK_O_CREAT,
                                    &w);
    CK_I(rc, ARK_VFS_OK);
    if (w != NULL)
    {
        CK_I(arkrtos_vfs_lfs_fsdrv.read(w, buf, 1u), ARK_E_INVAL);
        CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, w), ARK_VFS_OK);
    }

    CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK);
}

static void test_write(void)
{
    struct ark_vfs_file *f = NULL;
    struct ark_vfs_file *r = NULL;
    uint8 buf[8];

    mnt_reset();
    fs_reset();
    (void)fs_seed_file("/r.txt", "abc");

    CK_I(arkrtos_vfs_lfs_fsdrv.write(NULL, buf, 1u), ARK_E_BADF);

    r = open_ro("r.txt");
    if (r != NULL)
    {
        CK_I(arkrtos_vfs_lfs_fsdrv.write(r, buf, 1u), ARK_E_BADF);
        CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, r), ARK_VFS_OK);
    }

    CK(arkrtos_fs_mkdir("/wd") == 0);
    (void)arkrtos_vfs_lfs_fsdrv.opendir(&g_mnt, "wd", &f);
    if (f != NULL)
    {
        CK_I(arkrtos_vfs_lfs_fsdrv.write(f, buf, 1u), ARK_E_BADF);
        (void)arkrtos_vfs_lfs_fsdrv.closedir(&g_mnt, f);
    }

    CK_I(arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "out.txt",
                                    ARK_O_WRONLY | ARK_O_CREAT, &f), ARK_VFS_OK);
    if (f == NULL) { return; }

    CK_I(arkrtos_vfs_lfs_fsdrv.write(f, NULL, 1u), ARK_E_INVAL);
    CK_I(arkrtos_vfs_lfs_fsdrv.write(f, "xy", 0u), 0);
    CK_I(arkrtos_vfs_lfs_fsdrv.write(f, "xy", 2u), 2);
    g_f_write_next = 1u;
    CK_I(arkrtos_vfs_lfs_fsdrv.write(f, "z", 1u), ARK_E_IO);
    g_f_write_next = 0u;
    CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK);

    CK_I(arkrtos_fs_stat_path("/out.txt", NULL, NULL), 0);
}

static void test_seek(void)
{
    struct ark_vfs_file *f = NULL;
    struct ark_vfs_file *w = NULL;
    uint8  b = 0u;

    mnt_reset();
    fs_reset();
    (void)fs_seed_file("/s.txt", "abcdef");

    CK_I(arkrtos_vfs_lfs_fsdrv.seek(NULL, 0, ARK_SEEK_SET), ARK_E_BADF);

    f = open_ro("s.txt");
    if (f == NULL) { return; }

    CK_I(arkrtos_vfs_lfs_fsdrv.seek(f, 2, ARK_SEEK_SET), ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, &b, 1u), 1);
    CK(b == 'c');
    CK_I(arkrtos_vfs_lfs_fsdrv.seek(f, -1, ARK_SEEK_CUR), ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, &b, 1u), 1);
    CK(b == 'c');

    CK_I(arkrtos_vfs_lfs_fsdrv.seek(f, -2, ARK_SEEK_END), ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, &b, 1u), 1);
    CK(b == 'e');

    CK_I(arkrtos_vfs_lfs_fsdrv.seek(f, 0, 99), ARK_E_INVAL);

    /* SEEK_END 时 stat 失败 -> IO */
    g_f_stat_io = 1u;
    CK_I(arkrtos_vfs_lfs_fsdrv.seek(f, 0, ARK_SEEK_END), ARK_E_IO);
    g_f_stat_io = 0u;

    /* 写句柄是顺序流，不随机定位 */
    CK_I(arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "sw.txt",
                                    ARK_O_WRONLY | ARK_O_CREAT, &w), ARK_VFS_OK);
    if (w != NULL)
    {
        CK_I(arkrtos_vfs_lfs_fsdrv.seek(w, 0, ARK_SEEK_SET), ARK_E_NOSYS);
        CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, w), ARK_VFS_OK);
    }

    CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK);
}

static void test_stat(void)
{
    struct ark_vfs_stat st;
    char   longname[80];
    int32_t rc;

    mnt_reset();
    fs_reset();
    (void)fs_seed_file("/f.dat", "12345");
    CK(arkrtos_fs_mkdir("/sd") == 0);

    CK_I(arkrtos_vfs_lfs_fsdrv.stat(&g_mnt, "f.dat", NULL), ARK_E_INVAL);

    rc = arkrtos_vfs_lfs_fsdrv.stat(&g_mnt, "f.dat", &st);
    CK_I(rc, ARK_VFS_OK);
    CK_I(st.mode, ARK_S_IFREG | ARK_S_DEFAULT_FILE);
    CK_I(st.size, 5);
    CK_I(st.mtime, 0u);
    CK_I(st.uid, 0u);
    CK_I(st.gid, 0u);

    rc = arkrtos_vfs_lfs_fsdrv.stat(&g_mnt, "sd", &st);
    CK_I(rc, ARK_VFS_OK);
    CK_I(st.mode, ARK_S_IFDIR | ARK_S_DEFAULT_DIR);
    CK_I(st.size, 0);

    CK_I(arkrtos_vfs_lfs_fsdrv.stat(&g_mnt, "nope", &st), ARK_E_NOENT);

    /* 卷内绝对路径超过 64 字节 */
    memset(longname, 'q', sizeof(longname));
    longname[sizeof(longname) - 1u] = '\0';
    CK_I(arkrtos_vfs_lfs_fsdrv.stat(&g_mnt, longname, &st), ARK_E_NAMETOOLONG);
}

static void test_unlink_mkdir_rename(void)
{
    struct ark_vfs_stat st;

    mnt_reset();
    fs_reset();

    CK_I(arkrtos_vfs_lfs_fsdrv.unlink(&g_mnt, "gone"), ARK_E_NOENT);

    (void)fs_seed_file("/u.txt", "u");
    g_f_remove = 1u;
    CK_I(arkrtos_vfs_lfs_fsdrv.unlink(&g_mnt, "u.txt"), ARK_E_IO);
    g_f_remove = 0u;
    CK_I(arkrtos_vfs_lfs_fsdrv.unlink(&g_mnt, "u.txt"), ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.stat(&g_mnt, "u.txt", &st), ARK_E_NOENT);

    /* mkdir：已存在 -> EXIST，失败 -> IO，成功 */
    CK(arkrtos_fs_mkdir("/m") == 0);
    CK_I(arkrtos_vfs_lfs_fsdrv.mkdir(&g_mnt, "m"), ARK_E_EXIST);
    g_f_mkdir = 1u;
    CK_I(arkrtos_vfs_lfs_fsdrv.mkdir(&g_mnt, "m2"), ARK_E_IO);
    g_f_mkdir = 0u;
    CK_I(arkrtos_vfs_lfs_fsdrv.mkdir(&g_mnt, "m2"), ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.stat(&g_mnt, "m2", &st), 0);

    /* rename：源不存在 -> NOENT，目标已存在 -> EXIST，失败 -> IO，成功 */
    (void)fs_seed_file("/a.txt", "a");
    (void)fs_seed_file("/b.txt", "b");
    CK_I(arkrtos_vfs_lfs_fsdrv.rename(&g_mnt, "none.txt", "x.txt"),
         ARK_E_NOENT);
    CK_I(arkrtos_vfs_lfs_fsdrv.rename(&g_mnt, "a.txt", "b.txt"), ARK_E_EXIST);
    g_f_rename = 1u;
    CK_I(arkrtos_vfs_lfs_fsdrv.rename(&g_mnt, "a.txt", "c.txt"), ARK_E_IO);
    g_f_rename = 0u;
    CK_I(arkrtos_vfs_lfs_fsdrv.rename(&g_mnt, "a.txt", "c.txt"), ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.stat(&g_mnt, "c.txt", &st), 0);
    CK_I(arkrtos_vfs_lfs_fsdrv.stat(&g_mnt, "a.txt", &st), ARK_E_NOENT);
}

static void test_opendir_readdir(void)
{
    struct ark_vfs_file   *d = NULL;
    struct ark_vfs_file   *f = NULL;
    struct ark_vfs_dirent  ent;
    char longname[48];
    int32_t rc;

    mnt_reset();
    fs_reset();
    CK(arkrtos_fs_mkdir("/d") == 0);
    (void)fs_seed_file("/d/a.txt", "aa");
    (void)fs_seed_file("/d/b.txt", "bbb");
    memset(longname, 'L', sizeof(longname));
    longname[sizeof(longname) - 1u] = '\0';
    {
        char path[ARKRTOS_FS_PATH_MAX];
        snprintf(path, sizeof(path), "/d/%s", longname);
        (void)fs_seed_file(path, "");
    }

    CK_I(arkrtos_vfs_lfs_fsdrv.opendir(&g_mnt, "nope", &d), ARK_E_NOENT);

    (void)fs_seed_file("/plain.txt", "p");
    CK_I(arkrtos_vfs_lfs_fsdrv.opendir(&g_mnt, "plain.txt", &d), ARK_E_NOTDIR);

    rc = arkrtos_vfs_lfs_fsdrv.opendir(&g_mnt, "d", &d);
    CK_I(rc, ARK_VFS_OK);
    if (d == NULL) { return; }

    CK_I(arkrtos_vfs_lfs_fsdrv.readdir(NULL, &ent), ARK_E_NOTDIR);
    CK_I(arkrtos_vfs_lfs_fsdrv.readdir(d, NULL), ARK_E_INVAL);

    memset(&ent, 0, sizeof(ent));
    CK_I(arkrtos_vfs_lfs_fsdrv.readdir(d, &ent), ARK_VFS_OK);
    CK(strcmp(ent.name, "a.txt") == 0);       /* "." 与 ".." 不占名额 */
    CK_I(ent.mode, ARK_S_IFREG | ARK_S_DEFAULT_FILE);
    CK_I(ent.size, 2);
    CK_I(arkrtos_vfs_lfs_fsdrv.readdir(d, &ent), ARK_VFS_OK);
    CK(strcmp(ent.name, "b.txt") == 0);
    CK_I(arkrtos_vfs_lfs_fsdrv.readdir(d, &ent), ARK_E_NAMETOOLONG);
    CK_I(arkrtos_vfs_lfs_fsdrv.readdir(d, &ent), ARK_E_NAMETOOLONG);
    CK_I(arkrtos_vfs_lfs_fsdrv.closedir(&g_mnt, d), ARK_VFS_OK);

    /* 目录列举失败 -> IO */
    rc = arkrtos_vfs_lfs_fsdrv.opendir(&g_mnt, "d", &d);
    CK_I(rc, ARK_VFS_OK);
    if (d != NULL)
    {
        g_f_list = 1u;
        CK_I(arkrtos_vfs_lfs_fsdrv.readdir(d, &ent), ARK_E_IO);
        g_f_list = 0u;
        CK_I(arkrtos_vfs_lfs_fsdrv.closedir(&g_mnt, d), ARK_VFS_OK);
    }

    /* 空目录：. / .. 之后没有条目 -> NOENT */
    CK(arkrtos_fs_mkdir("/empty") == 0);
    rc = arkrtos_vfs_lfs_fsdrv.opendir(&g_mnt, "empty", &d);
    CK_I(rc, ARK_VFS_OK);
    if (d != NULL)
    {
        CK_I(arkrtos_vfs_lfs_fsdrv.readdir(d, &ent), ARK_E_NOENT);
        CK_I(arkrtos_vfs_lfs_fsdrv.readdir(d, &ent), ARK_E_NOENT);
        CK_I(arkrtos_vfs_lfs_fsdrv.closedir(&g_mnt, d), ARK_VFS_OK);
    }

    /* fd 表满 */
    {
        struct ark_vfs_file *h[ARK_VFS_MAX_FDS];
        int i;

        (void)arkrtos_vfs_lfs_fsdrv.mount(&g_mnt);
        for (i = 0; i < ARK_VFS_MAX_FDS; i++)
        {
            h[i] = NULL;
            CK_I(arkrtos_vfs_lfs_fsdrv.opendir(&g_mnt, "d", &h[i]), ARK_VFS_OK);
        }
        CK_I(arkrtos_vfs_lfs_fsdrv.opendir(&g_mnt, "d", &d), ARK_E_FULL);
        for (i = 0; i < ARK_VFS_MAX_FDS; i++)
        {
            if (h[i] != NULL)
            {
                CK_I(arkrtos_vfs_lfs_fsdrv.closedir(&g_mnt, h[i]), ARK_VFS_OK);
            }
        }
    }

    /* 文件句柄走 readdir -> NOTDIR */
    f = open_ro("plain.txt");
    if (f != NULL)
    {
        CK_I(arkrtos_vfs_lfs_fsdrv.readdir(f, &ent), ARK_E_NOTDIR);
        CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK);
    }
}

static void test_truncate(void)
{
    struct ark_vfs_file *f = NULL;
    struct ark_vfs_file *w = NULL;
    struct ark_vfs_file *d = NULL;
    uint8  buf[16];
    int32_t rc;

    mnt_reset();
    fs_reset();
    (void)fs_seed_file("/t.txt", "0123456789");
    CK(arkrtos_fs_mkdir("/td") == 0);

    CK_I(arkrtos_vfs_lfs_fsdrv.truncate(&g_mnt, NULL, 0), ARK_E_BADF);

    rc = arkrtos_vfs_lfs_fsdrv.opendir(&g_mnt, "td", &d);
    CK_I(rc, ARK_VFS_OK);
    if (d != NULL)
    {
        CK_I(arkrtos_vfs_lfs_fsdrv.truncate(&g_mnt, d, 0), ARK_E_ISDIR);
        CK_I(arkrtos_vfs_lfs_fsdrv.closedir(&g_mnt, d), ARK_VFS_OK);
    }

    f = open_ro("t.txt");
    if (f == NULL) { return; }

    CK_I(arkrtos_vfs_lfs_fsdrv.truncate(&g_mnt, f, -1), ARK_E_INVAL);
    g_mnt.flags = ARK_MNT_RO;
    CK_I(arkrtos_vfs_lfs_fsdrv.truncate(&g_mnt, f, 0), ARK_E_ROFS);
    g_mnt.flags = 0u;

    /* 有别的写流 -> BUSY */
    CK_I(arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "w.txt",
                                    ARK_O_WRONLY | ARK_O_CREAT, &w), ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.truncate(&g_mnt, f, 4), ARK_E_BUSY);
    if (w != NULL) { CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, w), ARK_VFS_OK); }

    /* 只读句柄走门面 truncate：内容只剩前 4 字节 */
    CK_I(arkrtos_vfs_lfs_fsdrv.seek(f, 0, ARK_SEEK_END), ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.truncate(&g_mnt, f, 4), ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, buf, sizeof(buf)), 0);
    CK_I(arkrtos_vfs_lfs_fsdrv.seek(f, 0, ARK_SEEK_SET), ARK_VFS_OK);
    CK_I(arkrtos_vfs_lfs_fsdrv.read(f, buf, sizeof(buf)), 4);
    CK(buf[0] == '0' && buf[3] == '3');

    /* NOSPC / IO */
    g_f_trunc_nospc = 1u;
    CK_I(arkrtos_vfs_lfs_fsdrv.truncate(&g_mnt, f, 8), ARK_E_NOSPC);
    g_f_trunc_nospc = 0u;
    g_f_trunc_io = 1u;
    CK_I(arkrtos_vfs_lfs_fsdrv.truncate(&g_mnt, f, 8), ARK_E_IO);
    g_f_trunc_io = 0u;

    CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, f), ARK_VFS_OK);

    /* 写句柄：只认 0（打开时已清空），其余 NOSYS */
    CK_I(arkrtos_vfs_lfs_fsdrv.open(&g_mnt, "tw.txt",
                                    ARK_O_WRONLY | ARK_O_CREAT, &w), ARK_VFS_OK);
    if (w != NULL)
    {
        CK_I(arkrtos_vfs_lfs_fsdrv.truncate(&g_mnt, w, 0), ARK_VFS_OK);
        CK_I(arkrtos_vfs_lfs_fsdrv.truncate(&g_mnt, w, 8), ARK_E_NOSYS);
        CK_I(arkrtos_vfs_lfs_fsdrv.close(&g_mnt, w), ARK_VFS_OK);
    }
}

int main(void)
{
    test_driver_table();
    test_mount();
    test_open();
    test_close();
    test_read();
    test_write();
    test_seek();
    test_stat();
    test_unlink_mkdir_rename();
    test_opendir_readdir();
    test_truncate();

    if (g_fails != 0)
    {
        printf("VFS-LFS-HOST FAIL  %d check(s), %d failure(s)\n", g_checks,
               g_fails);
        return 1;
    }
    printf("VFS-LFS-HOST OK  %d checks\n", g_checks);
    return 0;
}
