/**
* @file test_vfs_host.c
* @brief arkrtos_vfs.c 的宿主用例：命名空间门面 + App 句柄层
* @details 被测对象是 kernelsrc/src/arkrtos_vfs.c 一个文件。为了让它的
*          行为是真的（而不是桩对桩），这里链**真的** ark_vfs 核心
*          （ark_vfs_core.c）与**真的** ramfs（ark_vfs_ramfs.c）：
*          路径规范化、最长前缀匹配、挂载表、fd 表都是上游那一份。
*
*          被替换的只有「内核侧」：端口层（块设备窗口、/dev 挂载）、
*          arkrtos_fs 卷门面、littlefs 桥的 fsdrv、日志出口、任务状态。
*          这些是板级知识，宿主上没有也不该有。
*
*          因此本用例能证明的：门面的参数校验与错误码、命名空间的装配
*          顺序（/dev 先挂、/ 后挂、标准目录）、持久卷的唯一性约束、
*          App 句柄层的归属与序号回收、seek 位置的镜像记账。
*          不能证明的：真 littlefs / 真块设备 / 真任务切换下的并发。
*
*          【为什么「未初始化」的断言全部挤在进程最前面】
*          g_vfs_ready 一旦置 1 就没有回头路（门面故意不提供 deinit：
*          拆掉命名空间是板级关机该做的事，不是运行期 API）。而
*          ark_vfs_deinit() 只拆核心、不会把门面的标志清回去。所以
*          「未初始化时的回答」必须在第一次成功 init 之前测完，否则
*          测出来的就只是「核心被拆了但门面还以为自己在跑」——那是
*          本用例自己的造假，不是被测代码的行为。
*
* @author xw
* @date 2026.11.22
*/
#include <stdio.h>
#include <string.h>

#include "arkrtos_vfs.h"
#include "arkrtos_features.h"
#include "arkrtos_log.h"
#include "arkrtos_task.h"
#include "arkrtos_fs.h"
#include "ark_vfs.h"
#include "ark_vfs_ramfs.h"
#include "ark_vfs_port_arkrtos.h"

/* ============================================================
 * 断言计数
 * ============================================================ */
static int g_checks;
static int g_fails;

#define CK(cond)                                                            \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fails++;                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                   \
    } while (0)

/* 比值一律经 long 打印：MinGW 的 printf 不认 %lld，而这些断言里的数
 * 都在 32 位范围内（含 0x7FFFFF00 这种「正数但接近上限」的偏移）。 */
#define CK_I(a, b)                                                          \
    do {                                                                    \
        long _a = (long)(a);                                                 \
        long _b = (long)(b);                                                 \
        g_checks++;                                                         \
        if (_a != _b) {                                                     \
            g_fails++;                                                      \
            printf("FAIL %s:%d  %s=%ld 期望 %s=%ld\n", __FILE__, __LINE__,   \
                   #a, _a, #b, _b);                                         \
        }                                                                   \
    } while (0)

/* ============================================================
 * 任务侧桩：句柄归属与「任务是否还活着」
 * ============================================================ */
int32 arkrtos_current_task_id = 1;

#define STUB_TASK_MAX 16
static uint8 g_task_dead[STUB_TASK_MAX];

int32 arkrtos_task_status_get_internal(int32 task_id)
{
    if ((task_id <= 0) || (task_id >= STUB_TASK_MAX)) { return ARKRTOS_TASK_NONE; }
    return (g_task_dead[task_id] != 0u) ? ARKRTOS_TASK_NONE : ARKRTOS_TASK_READY;
}

/* ============================================================
 * 日志桩：只数条数，分级别
 * ============================================================ */
static int g_log_error;
static int g_log_warn;
static int g_log_info;

uint32 arkrtos_log_get_level(void) { return (uint32)ARKRTOS_LOG_DEBUG; }

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)tag; (void)line; (void)msg;
    if (level == (uint32)ARKRTOS_LOG_ERROR)        { g_log_error++; }
    else if (level == (uint32)ARKRTOS_LOG_WARNING) { g_log_warn++; }
    else                                           { g_log_info++; }
}

#if (ARKRTOS_USE_MINIAPP == 1) && (ARKRTOS_MINI_READ_VFS == 1)
#include "arkrtos_mini.h"

static int g_autostart_calls;
uint32 arkrtos_mini_boot_autostart(void)
{
    g_autostart_calls++;
    return 0u;
}
#endif

/* ============================================================
 * arkrtos_fs 卷门面桩
 * ============================================================ */
static uint8 g_fs_mounted;
static int   g_fs_mount_rc;
static int   g_fs_last_err;
static int   g_fs_mount_calls;
static int   g_fs_unmount_calls;
static const char *g_fs_dev = "nor0";
static uint32      g_fs_off;
static uint32      g_fs_size;

int32 arkrtos_fs_mount(const char *dev, uint32 offset, uint32 size)
{
    g_fs_mount_calls++;
    if (g_fs_mount_rc != 0)
    {
        g_fs_last_err = -28;        /* 假装是「卷没格式化」 */
        return -1;
    }
    g_fs_mounted = 1u;
    g_fs_dev  = dev;
    g_fs_off  = offset;
    g_fs_size = size;
    return 0;
}

int32 arkrtos_fs_unmount(void)
{
    g_fs_unmount_calls++;
    g_fs_mounted = 0u;
    return 0;
}

uint8 arkrtos_fs_mounted(void) { return (g_fs_mounted != 0u) ? 1u : 0u; }

int32 arkrtos_fs_volume(const char **dev, uint32 *offset, uint32 *size)
{
#if (ARKRTOS_USE_VFS_LFS == 1)
    if (g_fs_mounted == 0u) { return -1; }
    if (dev != NULL)    { *dev = g_fs_dev; }
    if (offset != NULL) { *offset = g_fs_off; }
    if (size != NULL)   { *size = g_fs_size; }
    return 0;
#else
    (void)dev; (void)offset; (void)size;
    return -1;
#endif
}

int32 arkrtos_fs_last_error(void) { return g_fs_last_err; }

const char *arkrtos_fs_error_name(int32 err) { (void)err; return "stub-fs-error"; }

/* ============================================================
 * 块设备窗口桩
 * ============================================================ */
static int32 g_blk_rc;
static uint32 g_blk_size = 4096u;

int32_t ark_vfs_port_blk_init(ark_vfs_port_blk_t *pb, const char *dev_name,
                              uint32_t base, uint32_t size, const char *label)
{
    (void)dev_name; (void)base; (void)label;
    if (g_blk_rc != 0) { return g_blk_rc; }
    if (pb != NULL) { pb->size = (size == 0u) ? g_blk_size : size; }
    return 0;
}

/* ============================================================
 * 两个替身文件系统
 *
 * devfs：挂在 /dev，节点类型 ARK_S_IFCHR。它存在的意义是让「/dev 先挂、
 *        / 后挂、最长前缀各走各的」这条装配顺序被真的走一遍。
 * lfs：  arkrtos_vfs_lfs_fsdrv 的替身。它故意把 seek 的返回值当 0
 *        （littlefs 桥就是这么回），好让 App 层「自己算偏移」的记账
 *        分支被走到；它的读是 EOF，写只回长度。
 * ============================================================ */
typedef struct
{
    int used;
    int is_dir;
    int is_chr;
} stub_file_t;

static stub_file_t g_dev_files[4];
static stub_file_t g_lfs_files[4];

static struct ark_vfs_file *stub_open_pool(stub_file_t *pool, int count,
                                           int chr)
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (pool[i].used == 0)
        {
            pool[i].used   = 1;
            pool[i].is_dir = 0;
            pool[i].is_chr = chr;
            return (struct ark_vfs_file *)&pool[i];
        }
    }
    return NULL;
}

static void stub_close_pool(struct ark_vfs_file *f)
{
    stub_file_t *s = (stub_file_t *)f;

    if (s != NULL) { s->used = 0; }
}

/* ---- devfs ---- */
static int32_t devfs_mount(struct ark_vfs_mnt *mnt) { (void)mnt; return 0; }

static int32_t devfs_open(struct ark_vfs_mnt *mnt, const char *rel, int flags,
                          struct ark_vfs_file **out)
{
    (void)mnt; (void)flags;
    if (rel[0] == '\0') { return ARK_E_ISDIR; }
    *out = stub_open_pool(g_dev_files, 4, 1);
    return (*out != NULL) ? 0 : ARK_E_FULL;
}

static int32_t devfs_close(struct ark_vfs_mnt *mnt, struct ark_vfs_file *f)
{
    (void)mnt;
    stub_close_pool(f);
    return 0;
}

static int32_t devfs_stat(struct ark_vfs_mnt *mnt, const char *rel,
                          struct ark_vfs_stat *st)
{
    (void)mnt;
    st->mode  = (rel[0] == '\0') ? (uint32_t)(ARK_S_IFDIR | ARK_S_DEFAULT_DIR)
                                 : (uint32_t)(ARK_S_IFCHR | ARK_S_DEFAULT_CHR);
    st->size  = 0;
    st->mtime = 0u;
    st->uid   = 0u;
    st->gid   = 0u;
    return 0;
}

static int32_t devfs_readdir(struct ark_vfs_file *d, struct ark_vfs_dirent *ent)
{
    (void)d; (void)ent;
    return ARK_E_NOENT;
}

static const ark_vfs_fsdrv_t g_devfs = {
    "devfs", ARK_FS_CAP_STAT | ARK_FS_CAP_DIR,
    devfs_mount, NULL,
    devfs_open, devfs_close, NULL, NULL, NULL,
    devfs_stat, NULL, NULL, NULL, NULL, devfs_readdir, NULL,
    NULL, NULL, NULL
};

/* ---- lfs 替身 ---- */
static int g_lfs_seek_rc;
static int g_lfs_readdir_rc;
static int g_lfs_open_rc;
static int g_lfs_trunc_rc;
static int g_lfs_read_rc;

static int32_t lfs_mount(struct ark_vfs_mnt *mnt) { (void)mnt; return ARK_VFS_OK; }
static int32_t lfs_umount(struct ark_vfs_mnt *mnt) { (void)mnt; return ARK_VFS_OK; }

static int32_t lfs_open(struct ark_vfs_mnt *mnt, const char *rel, int flags,
                        struct ark_vfs_file **out)
{
    (void)mnt; (void)flags; (void)rel;
    if (g_lfs_open_rc != 0) { return g_lfs_open_rc; }
    *out = stub_open_pool(g_lfs_files, 4, 0);
    return (*out != NULL) ? 0 : ARK_E_FULL;
}

static int32_t lfs_close(struct ark_vfs_mnt *mnt, struct ark_vfs_file *f)
{
    (void)mnt;
    stub_close_pool(f);
    return 0;
}

static ark_ssize_t lfs_read(struct ark_vfs_file *f, void *buf, ark_size_t n)
{
    (void)f; (void)buf; (void)n;
    if (g_lfs_read_rc != 0) { return (ark_ssize_t)g_lfs_read_rc; }
    return 0;                           /* 空文件：读就是 EOF */
}

static ark_ssize_t lfs_write(struct ark_vfs_file *f, const void *buf, ark_size_t n)
{
    (void)f; (void)buf;
    return (ark_ssize_t)n;
}

static int32_t lfs_seek(struct ark_vfs_file *f, ark_off_t off, int whence)
{
    (void)f; (void)off; (void)whence;
    return g_lfs_seek_rc;               /* 真桥回 0，App 层不许拿它当偏移 */
}

static int32_t lfs_stat(struct ark_vfs_mnt *mnt, const char *rel,
                        struct ark_vfs_stat *st)
{
    (void)mnt;
    st->mode  = (rel[0] == '\0') ? (uint32_t)(ARK_S_IFDIR | ARK_S_DEFAULT_DIR)
                                 : (uint32_t)(ARK_S_IFREG | ARK_S_DEFAULT_FILE);
    st->size  = 0;
    st->mtime = 0u;
    st->uid   = 0u;
    st->gid   = 0u;
    return 0;
}

static int32_t lfs_unlink(struct ark_vfs_mnt *mnt, const char *rel)
{
    (void)mnt; (void)rel;
    return 0;
}

static int32_t lfs_mkdir(struct ark_vfs_mnt *mnt, const char *rel)
{
    (void)mnt; (void)rel;
    return 0;
}

static int32_t lfs_rename(struct ark_vfs_mnt *mnt, const char *a, const char *b)
{
    (void)mnt; (void)a; (void)b;
    return 0;
}

static int32_t lfs_opendir(struct ark_vfs_mnt *mnt, const char *rel,
                           struct ark_vfs_file **out)
{
    (void)mnt; (void)rel;
    if (g_lfs_readdir_rc == 0) { return ARK_E_NOENT; }   /* 空目录 */
    *out = stub_open_pool(g_lfs_files, 4, 0);
    return (*out != NULL) ? 0 : ARK_E_FULL;
}

static int32_t lfs_readdir(struct ark_vfs_file *d, struct ark_vfs_dirent *ent)
{
    (void)d; (void)ent;
    return g_lfs_readdir_rc;
}

static int32_t lfs_closedir(struct ark_vfs_mnt *mnt, struct ark_vfs_file *d)
{
    (void)mnt;
    stub_close_pool(d);
    return 0;
}

static int32_t lfs_truncate(struct ark_vfs_mnt *mnt, struct ark_vfs_file *f,
                            ark_off_t size)
{
    (void)mnt; (void)f; (void)size;
    return g_lfs_trunc_rc;
}

const ark_vfs_fsdrv_t arkrtos_vfs_lfs_fsdrv = {
    "lfs",
    ARK_FS_CAP_READ | ARK_FS_CAP_WRITE | ARK_FS_CAP_CREATE | ARK_FS_CAP_UNLINK |
    ARK_FS_CAP_MKDIR | ARK_FS_CAP_RENAME | ARK_FS_CAP_DIR | ARK_FS_CAP_STAT |
    ARK_FS_CAP_TRUNC,
    lfs_mount, lfs_umount,
    lfs_open, lfs_close, lfs_read, lfs_write, lfs_seek,
    lfs_stat, lfs_unlink, lfs_mkdir, lfs_rename,
    lfs_opendir, lfs_readdir, lfs_closedir,
    NULL, NULL, lfs_truncate
};

/* ============================================================
 * 端口层桩：装 /dev、按需制造装配事故
 * ============================================================ */
static int  g_port_rc;              /* 非 0：/dev 装不上 */
static int  g_port_claim_root;      /* 1：端口层自己把 / 占了，门面再挂就 EXIST */
static int  g_port_fill_dirs;       /* >0：先占掉这么多个 ramfs 目录槽 */
static int  g_port_calls;

int32_t ark_vfs_port_init(void)
{
    ark_vfs_hooks_t hooks;
    int32_t rc;
    int i;

    g_port_calls++;
    if (g_port_rc != 0) { return g_port_rc; }

    memset(&hooks, 0, sizeof(hooks));
    ark_vfs_init(&hooks);

    if (g_port_fill_dirs > 0)
    {
        /* ramfs 目录表 16 项、根占 1 项。先建 N 个孩子再卸掉 /：内容按
         * ramfs 的约定在重挂后仍在，于是门面挂上 / 之后建标准目录时，
         * 前 16-1-N 个成功、其余的拿到 FULL——正是「命名空间不完整
         * 要出声」那条分支。两侧都能被走到，才算测了那条 if。 */
        char name[16];

        if (ark_vfs_mount("ram", "/", ark_ramfs_fs(), NULL, 0u, NULL) == ARK_VFS_OK)
        {
            for (i = 0; i < g_port_fill_dirs; i++)
            {
                (void)snprintf(name, sizeof(name), "/fill%d", i);
                (void)ark_vfs_mkdir(name);
            }
            (void)ark_vfs_umount("/");
        }
    }

    rc = ark_vfs_mount("devfs", "/dev", &g_devfs, NULL, 0u, NULL);
    if (rc != ARK_VFS_OK) { return rc; }

    if (g_port_claim_root != 0)
    {
        (void)ark_vfs_mount("ram", "/", ark_ramfs_fs(), NULL, 0u, NULL);
    }
    return ARK_VFS_OK;
}

/* ============================================================
 * 环境复位
 *
 * 两个层次：
 *   env_boot_reset()  只在第一次成功 init 之前用：连核心一起拆掉。
 *   env_reset()       成功 init 之后用：核心保持挂着（门面的 ready 位
 *                     已经回不去了），只把内容与桩状态清干净。
 * ============================================================ */
static void stub_reset(void)
{
    arkrtos_vfs_app_task_exit(1u);
    arkrtos_vfs_app_task_exit(2u);
    g_task_dead[1] = 0u;
    g_task_dead[2] = 0u;
    arkrtos_current_task_id = 1;
    memset(g_dev_files, 0, sizeof(g_dev_files));
    memset(g_lfs_files, 0, sizeof(g_lfs_files));
    g_port_rc = 0;
    g_port_claim_root = 0;
    g_port_fill_dirs = 0;
    g_port_calls = 0;
    g_blk_rc = 0;
    g_blk_size = 4096u;
    g_fs_mounted = 0u;
    g_fs_mount_rc = 0;
    g_fs_last_err = 0;
    g_fs_mount_calls = 0;
    g_fs_unmount_calls = 0;
    g_fs_dev = "nor0";
    g_fs_off = 0u;
    g_fs_size = 0u;
    g_lfs_seek_rc = 0;
    g_lfs_readdir_rc = 0;
    g_lfs_open_rc = 0;
    g_lfs_trunc_rc = 0;
    g_lfs_read_rc = 0;
    g_log_error = 0;
    g_log_warn = 0;
    g_log_info = 0;
#if (ARKRTOS_USE_MINIAPP == 1) && (ARKRTOS_MINI_READ_VFS == 1)
    g_autostart_calls = 0;
#endif
}

static void env_boot_reset(void)
{
    stub_reset();
    ark_vfs_deinit();
    ark_ramfs_reset();
}

static void env_reset(void)
{
    /* 挂点之外的（测试自己加的）先摘掉，否则下一个用例会遇到 EXIST */
    (void)arkrtos_vfs_umount("/ro");
    (void)arkrtos_vfs_umount("/mnt/lfs");
    stub_reset();
    ark_ramfs_reset();      /* 文件与目录全清；/tmp 由用例自己再建 */
}

/* ============================================================
 * 0. 还没初始化：每条门面都必须说 INVAL，而不是假装成功
 * ============================================================ */
static void test_not_ready(void)
{
    arkrtos_vfs_mount_info_t mi;

    env_boot_reset();
    CK_I(arkrtos_vfs_ready(), 0);
    CK_I(arkrtos_vfs_mount_count(), 0);
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), ARK_E_INVAL);
    CK_I(arkrtos_vfs_umount("/mnt/lfs"), ARK_E_INVAL);

    memset(&mi, 0, sizeof(mi));
    CK_I(arkrtos_vfs_mount_info(0u, NULL), ARKRTOS_VFS_ERR_STATE);
    CK_I(arkrtos_vfs_mount_info(0u, &mi), ARKRTOS_VFS_ERR_STATE);
    CK_I((int)mi.target[0], 0);

    CK_I(arkrtos_vfs_app_open("/tmp/x", ARKRTOS_VFS_O_RDONLY), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_stat("/tmp/x", NULL, NULL), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_mkdir("/tmp/d"), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_rmdir("/tmp/d"), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_unlink("/tmp/f"), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_rename("/a", "/b"), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_truncate_path("/tmp/f", 0u), ARK_E_INVAL);
}

/* ============================================================
 * 1. 初始化：两条失败路径 + 一次成功（含标准目录告警）
 * ============================================================ */
static void test_init_failures(void)
{
    /* /dev 装不上：原样把原因交回去，门面不许说自己就绪 */
    env_boot_reset();
    g_port_rc = -5;
    CK_I(arkrtos_vfs_init(), -5);
    CK_I(arkrtos_vfs_ready(), 0);
    CK_I(g_port_calls, 1);
    CK(g_log_error >= 1);            /* 失败必须出声 */

    /* 根挂载失败：端口层抢先把 / 占了 → 门面 EXIST，并把核心拆干净 */
    env_boot_reset();
    g_port_claim_root = 1;
    CK_I(arkrtos_vfs_init(), ARK_E_EXIST);
    CK_I(arkrtos_vfs_ready(), 0);
    CK(g_log_error >= 1);
    CK_I(ark_vfs_is_ready(), 0);
}

static void test_init_ok(void)
{
    /* 根本身不占目录槽：13 个 fill 用掉 13 个槽，还剩 3 个 —— 
     * /tmp /etc /home 建成，/mnt /media 拿 FULL 并各告警一次。 */
    env_boot_reset();
    g_port_fill_dirs = 13;
    CK_I(arkrtos_vfs_init(), 0);
    CK_I(arkrtos_vfs_ready(), 1);
    CK_I(g_port_calls, 1);
    CK_I(arkrtos_vfs_init(), 0);     /* 幂等：不重复装 /dev */
    CK_I(g_port_calls, 1);

    CK_I(arkrtos_vfs_mount_count(), 2);      /* /dev 先、/ 后 */
    CK_I(arkrtos_vfs_app_stat("/tmp", NULL, NULL), 0);
    CK_I(arkrtos_vfs_app_stat("/etc", NULL, NULL), 0);
    CK_I(arkrtos_vfs_app_stat("/home", NULL, NULL), 0);
    CK_I(arkrtos_vfs_app_stat("/mnt", NULL, NULL), ARK_E_NOENT);
    CK_I(arkrtos_vfs_app_stat("/media", NULL, NULL), ARK_E_NOENT);
    CK_I(g_log_warn, 2);
}

/* ============================================================
 * 2. 挂载信息
 * ============================================================ */
static void test_mount_info(void)
{
    arkrtos_vfs_mount_info_t mi;

    env_reset();
    CK_I(arkrtos_vfs_mount_count(), 2);

    memset(&mi, 0, sizeof(mi));
    CK_I(arkrtos_vfs_mount_info(0u, &mi), 0);
    CK(strcmp(mi.target, "/dev") == 0);
    CK(strcmp(mi.fs, "devfs") == 0);
    CK(mi.source == NULL);                /* 无后端：来源照实报 NULL */
    CK_I(mi.ro, 0);

    memset(&mi, 0, sizeof(mi));
    CK_I(arkrtos_vfs_mount_info(1u, &mi), 0);
    CK(strcmp(mi.target, "/") == 0);
    CK(strcmp(mi.fs, "ramfs") == 0);

    memset(&mi, 0, sizeof(mi));
    CK_I(arkrtos_vfs_mount_info(2u, &mi), ARKRTOS_VFS_ERR_RANGE);

    /* 只读挂载在列表里必须看得出来 */
    CK_I(ark_vfs_mount("ro", "/ro", ark_ramfs_fs(), NULL, ARK_MNT_RO, NULL),
         ARK_VFS_OK);
    memset(&mi, 0, sizeof(mi));
    CK_I(arkrtos_vfs_mount_info(2u, &mi), 0);
    CK(strcmp(mi.target, "/ro") == 0);
    CK_I(mi.ro, 1);
    CK_I(arkrtos_vfs_mount_count(), 3);
}

/* ============================================================
 * 3. 持久卷挂载：校验面 + 成功 / 失败分支
 * ============================================================ */
static void test_mount_volume(void)
{
    arkrtos_vfs_mount_info_t mi;
    int32 h;

#if (ARKRTOS_USE_VFS_LFS == 0)
    /* 本档没有 littlefs 桥：门面没有任何能做成的事，必须是 NOSYS */
    (void)mi; (void)h;      /* 只在这一档用不到，别让编译器替我们猜 */
    env_reset();
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), ARK_E_NOSYS);
    CK_I(arkrtos_vfs_mount_count(), 2);
#else
    env_reset();
    CK_I(arkrtos_vfs_mount_volume(NULL, 0u, 0u, "/mnt/lfs"), ARK_E_INVAL);
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, NULL), ARK_E_INVAL);
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, ""), ARK_E_INVAL);

    /* 窗口被块层拒绝：原因是它给的，原样透出去 */
    g_blk_rc = ARK_E_INVAL;
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 100u, "/mnt/lfs"), ARK_E_INVAL);
    CK_I(g_fs_mount_calls, 0);
    g_blk_rc = 0;

    /* 卷挂不上：报 IO，且不能留下「卷挂了但挂点没挂」 */
    g_fs_mount_rc = -1;
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), ARK_E_IO);
    CK_I(g_fs_mount_calls, 1);
    CK_I(g_fs_unmount_calls, 0);
    CK(g_log_error >= 1);
    g_fs_mount_rc = 0;

    /* 成功：卷用块层归一化后的长度 */
    CK_I(arkrtos_vfs_mount_volume("nor0", 1024u, 0u, "/mnt/lfs"), 0);
    CK_I(g_fs_mount_calls, 2);
    CK_I(g_fs_off, 1024u);
    CK_I(g_fs_size, g_blk_size);
    CK_I(arkrtos_vfs_mount_count(), 3);
    memset(&mi, 0, sizeof(mi));
    CK_I(arkrtos_vfs_mount_info(2u, &mi), 0);
    CK(strcmp(mi.target, "/mnt/lfs") == 0);
    CK(strcmp(mi.fs, "lfs") == 0);

    /* 第二个卷：arkrtos_fs 只有一个卷，直接 EXIST（早于块层） */
    CK_I(arkrtos_vfs_mount_volume("nor0", 1024u, 0u, "/mnt/lfs"), ARK_E_EXIST);

    /* 有文件开着：卸卷必须 BUSY，且卷目标不被清掉 */
    h = arkrtos_vfs_app_open("/mnt/lfs/f", ARKRTOS_VFS_O_RDONLY);
    CK(h > 0);
    CK_I(arkrtos_vfs_umount("/mnt/lfs"), ARK_E_BUSY);
    CK_I(arkrtos_vfs_mount_volume("nor0", 1024u, 0u, "/mnt/lfs"), ARK_E_EXIST);
    CK_I(arkrtos_vfs_app_close(h), 0);

    /* 卸掉别的挂点不该动持久卷的状态 */
    CK_I(arkrtos_vfs_umount("/dev"), 0);
    CK_I(arkrtos_vfs_mount_count(), 2);
    CK_I(arkrtos_vfs_mount_volume("nor0", 1024u, 0u, "/mnt/lfs"), ARK_E_EXIST);

    CK_I(arkrtos_vfs_umount("/mnt/lfs"), 0);
    CK_I(g_fs_unmount_calls, 1);
    CK_I(arkrtos_vfs_mount_count(), 1);

    /* 卸干净后可以再挂：证明卷目标确实被清了 */
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), 0);
    CK_I(arkrtos_vfs_umount("/mnt/lfs"), 0);

    /* 卷已在别处挂好（同几何）：不重复 mount，直接复用 */
    g_fs_mounted = 1u;
    g_fs_dev = "nor0";
    g_fs_off = 0u;
    g_fs_size = g_blk_size;
    g_fs_mount_calls = 0;
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), 0);
    CK_I(g_fs_mount_calls, 0);
    CK_I(arkrtos_vfs_umount("/mnt/lfs"), 0);

    /* 几何不同：EXIST，绝不静默复用别人的窗口 */
    g_fs_mounted = 1u;
    g_fs_off = 512u;
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), ARK_E_EXIST);
    /* 设备名不同 */
    g_fs_off = 0u;
    g_fs_dev = "nor1";
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), ARK_E_EXIST);
    /* 问不出几何也是 EXIST（不知道就别复用） */
    g_fs_dev = "nor0";
    g_fs_mounted = 0u;
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), 0);
    CK_I(arkrtos_vfs_umount("/mnt/lfs"), 0);

    /* 卷挂上了但挂点装不上（/ 已被占）：必须把卷也卸掉 */
    g_fs_mounted = 1u;
    g_fs_dev = "nor0";
    g_fs_off = 0u;
    g_fs_size = g_blk_size;
    g_fs_unmount_calls = 0;
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/"), ARK_E_EXIST);
    CK_I(g_fs_unmount_calls, 1);
    CK_I(g_fs_mounted, 0);

#if (ARKRTOS_USE_MINIAPP == 1) && (ARKRTOS_MINI_READ_VFS == 1)
    /* 自启只能放在卷挂好之后跑，且整机只跑一次 */
    g_autostart_calls = 0;
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), 0);
    CK_I(g_autostart_calls, 1);
    CK_I(arkrtos_vfs_umount("/mnt/lfs"), 0);
#endif
#endif
}

/* ============================================================
 * 4. 门面：读写 / 大小 / 列目录 / 权限
 * ============================================================ */
static int list_cb_stop(const char *name, const arkrtos_vfs_stat_t *st, void *arg)
{
    int *n = (int *)arg;

    (void)st;
    (*n)++;
    if (strcmp(name, "a.txt") == 0) { return -1; }   /* 提前收手 */
    return 0;
}

static int list_cb_count(const char *name, const arkrtos_vfs_stat_t *st, void *arg)
{
    int *n = (int *)arg;

    (void)name; (void)st;
    (*n)++;
    return 0;
}

static void test_facade_io(void)
{
    uint8 buf[16];
    uint32 len;
    uint32 size;
    int32 fd;
    int32 rc;

    env_reset();
    CK_I(arkrtos_vfs_write_file("/tmp/a.txt", (const uint8 *)"hello", 5u), 0);

    len = 99u;
    rc = arkrtos_vfs_read_file("/tmp/a.txt", buf, (uint32)sizeof(buf), &len);
    CK_I(rc, 0);
    CK_I(len, 5u);
    CK(memcmp(buf, "hello", 5) == 0);

    /* 短缓冲不是「截断成功」：下层探到还有剩就报 NOSPC，
     * 绝不把半个文件当作整个文件交回去 */
    memset(buf, 0, sizeof(buf));
    len = 99u;
    CK_I(arkrtos_vfs_read_file("/tmp/a.txt", buf, 3u, &len), ARK_E_NOSPC);
    CK_I(len, 0u);
    CK(memcmp(buf, "hel", 3) == 0);   /* 已读到的部分照样在缓冲里 */

    /* out_len 可以为 NULL；参数空要报 INVAL */
    CK_I(arkrtos_vfs_read_file("/tmp/a.txt", buf, (uint32)sizeof(buf), NULL), 0);
    CK_I(arkrtos_vfs_read_file(NULL, buf, 2u, &len), ARK_E_INVAL);
    CK_I(arkrtos_vfs_read_file("/tmp/a.txt", NULL, 2u, &len), ARK_E_INVAL);
    CK_I(arkrtos_vfs_write_file(NULL, buf, 2u), ARK_E_INVAL);
    CK_I(arkrtos_vfs_write_file("/tmp/x", NULL, 2u), ARK_E_INVAL);
    CK_I(arkrtos_vfs_write_file("/tmp/x", NULL, 0u), 0);   /* 空写合法 */
    CK_I(arkrtos_vfs_size("/tmp/x", &size), 0);
    CK_I(size, 0u);

    /* 偏移读：跨一次 EOF 也要把短读当正常结束 */
    memset(buf, 0, sizeof(buf));
    len = 99u;
    CK_I(arkrtos_vfs_read_at("/tmp/a.txt", buf, 4u, 2u, &len), 0);
    CK_I(len, 3u);
    CK(memcmp(buf, "llo", 3) == 0);

    len = 99u;
    CK_I(arkrtos_vfs_read_at("/tmp/a.txt", buf, 4u, 100u, &len), 0);
    CK_I(len, 0u);
    CK_I(arkrtos_vfs_read_at(NULL, buf, 4u, 0u, &len), ARK_E_INVAL);
    CK_I(arkrtos_vfs_read_at("/tmp/a.txt", NULL, 4u, 0u, &len), ARK_E_INVAL);
    CK_I(arkrtos_vfs_read_at("/nope", buf, 4u, 0u, &len), ARK_E_NOENT);

    /* 大小：量不到就报 IO，不编一个数 */
    CK_I(arkrtos_vfs_size("/tmp/a.txt", &size), 0);
    CK_I(size, 5u);
    CK_I(arkrtos_vfs_size("/nope", &size), ARK_E_IO);
    CK_I(size, 0u);
    CK_I(arkrtos_vfs_size(NULL, &size), ARK_E_INVAL);
    CK_I(arkrtos_vfs_size("/tmp/a.txt", NULL), ARK_E_INVAL);

    /* 直接 fd 面 */
    fd = arkrtos_vfs_open_read("/tmp/a.txt");
    CK(fd >= 0);
    CK_I(arkrtos_vfs_read(fd, buf, 2u), 2);
    CK_I(arkrtos_vfs_read(fd, buf, 0u), 0);
    CK_I(arkrtos_vfs_read(fd, NULL, 2u), ARK_E_INVAL);
    CK_I(arkrtos_vfs_close(fd), 0);
    CK_I(arkrtos_vfs_read(fd, buf, 2u), ARK_E_BADF);
    CK_I(arkrtos_vfs_close(fd), ARK_E_BADF);
    CK_I(arkrtos_vfs_open_read(NULL), ARK_E_INVAL);
    CK_I(arkrtos_vfs_open_read("/nope"), ARK_E_NOENT);

    /* 删除 */
    CK_I(arkrtos_vfs_remove(NULL), ARK_E_INVAL);
    CK_I(arkrtos_vfs_remove("/nope"), ARK_E_NOENT);
    CK_I(arkrtos_vfs_remove("/tmp/x"), 0);

    /* 列目录：走到底的 NOENT 是「列完了」，不是错 */
    {
        int n = 0;

        CK_I(arkrtos_vfs_write_file("/tmp/b.txt", (const uint8 *)"b", 1u), 0);
        CK_I(arkrtos_vfs_list("/tmp", list_cb_count, &n), 0);
        CK(n >= 2);
        n = 0;
        CK_I(arkrtos_vfs_list("/tmp", list_cb_stop, &n), 0);   /* 回调打断 */
        CK(n >= 1);
        CK_I(arkrtos_vfs_list("/nope", list_cb_count, &n), ARK_E_NOENT);
        CK_I(arkrtos_vfs_list("/tmp/a.txt", list_cb_count, &n), ARK_E_NOTDIR);
        CK_I(arkrtos_vfs_list(NULL, list_cb_count, &n), ARK_E_INVAL);
        CK_I(arkrtos_vfs_list("/tmp", NULL, &n), ARK_E_INVAL);
    }

    /* 错误名：数字永远保留，符号只是附加 */
    CK(arkrtos_vfs_error_name(ARK_VFS_OK) != NULL);
    CK(arkrtos_vfs_error_name(ARK_E_NOENT) != NULL);
    CK(strcmp(arkrtos_vfs_error_name(ARK_E_NOENT),
              arkrtos_vfs_error_name(ARK_E_INVAL)) != 0);

    /* 权限 */
    CK_I(arkrtos_vfs_chmod(NULL, 0600u), ARK_E_INVAL);
    CK_I(arkrtos_vfs_chmod("/tmp/a.txt", 0600u), 0);
    CK_I(arkrtos_vfs_chmod("/nope", 0600u), ARK_E_NOENT);
    CK_I(arkrtos_vfs_chown(NULL, 1u, 1u), ARK_E_INVAL);
    CK_I(arkrtos_vfs_chown("/tmp/a.txt", 1u, 2u), 0);
    CK_I(arkrtos_vfs_chown("/nope", 1u, 2u), ARK_E_NOENT);
}

/* ============================================================
 * 5. App 句柄层：打开 / 归属 / 满表回收
 * ============================================================ */
static void test_app_open(void)
{
    int32 h;
    uint8 buf[8];
    int i;

    env_reset();
    CK_I(arkrtos_vfs_write_file("/tmp/a.txt", (const uint8 *)"hello", 5u), 0);

    CK_I(arkrtos_vfs_app_open(NULL, ARKRTOS_VFS_O_RDONLY), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_open("", ARKRTOS_VFS_O_RDONLY), ARK_E_INVAL);
    /* 目录必须只读打开：写目录没有意义 */
    CK_I(arkrtos_vfs_app_open("/tmp",
                              ARKRTOS_VFS_O_DIRECTORY | ARKRTOS_VFS_O_WRONLY),
         ARK_E_INVAL);

    h = arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDONLY);
    CK(h > 0);
    CK(((uint32)h & ARKRTOS_VFS_HANDLE_FLAG) == ARKRTOS_VFS_HANDLE_FLAG);
    CK_I(arkrtos_vfs_app_read(h, buf, 5u), 5);
    CK(memcmp(buf, "hello", 5) == 0);
    /* 位置顺推：第二次读就是 EOF */
    CK_I(arkrtos_vfs_app_read(h, buf, 1u), 0);
    CK_I(arkrtos_vfs_app_read(h, buf, 0u), 0);
    CK_I(arkrtos_vfs_app_read(h, NULL, 1u), ARK_E_INVAL);
    /* 另一个任务拿到同一个号码也不能用 */
    arkrtos_current_task_id = 2;
    CK_I(arkrtos_vfs_app_read(h, buf, 1u), ARK_E_BADF);
    arkrtos_current_task_id = 1;
    CK_I(arkrtos_vfs_app_close(h), 0);
    CK_I(arkrtos_vfs_app_close(h), ARK_E_BADF);   /* 序号已推进 */

    /* 换一任持有者也不能用旧号码 */
    h = arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDONLY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_close(h), 0);

    /* 号码面：tag 不对、槽号越界，一律 BADF —— 不给「猜一个号码」留余地 */
    CK_I(arkrtos_vfs_app_close((int32)0x12345678), ARK_E_BADF);
    CK_I(arkrtos_vfs_app_close((int32)(ARKRTOS_VFS_HANDLE_FLAG | 0x20u)),
         ARK_E_BADF);

    /* 读写打开：两个方向都要在存，写进去的要读得回来 */
    h = arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDWR);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_write(h, (const uint8 *)"J", 1u), 1);
    CK_I(arkrtos_vfs_app_seek(h, 0, ARKRTOS_VFS_SEEK_SET), 0);
    CK_I(arkrtos_vfs_app_read(h, buf, 1u), 1);
    CK(buf[0] == 'J');
    CK_I(arkrtos_vfs_app_close(h), 0);

    /* 目录句柄：读/写/seek/truncate 一律 ISDIR，readdir 才是它的路 */
    h = arkrtos_vfs_app_open("/tmp",
                             ARKRTOS_VFS_O_RDONLY | ARKRTOS_VFS_O_DIRECTORY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_read(h, buf, 1u), ARK_E_ISDIR);
    CK_I(arkrtos_vfs_app_write(h, buf, 1u), ARK_E_ISDIR);
    CK_I(arkrtos_vfs_app_seek(h, 0, ARKRTOS_VFS_SEEK_SET), ARK_E_ISDIR);
    CK_I(arkrtos_vfs_app_truncate(h, 0u), ARK_E_ISDIR);
    CK_I(arkrtos_vfs_app_close(h), 0);

    /* 写：参数、追加、大小随写更新 */
    h = arkrtos_vfs_app_open("/tmp/w.txt",
                             ARKRTOS_VFS_O_CREAT | ARKRTOS_VFS_O_TRUNC |
                             ARKRTOS_VFS_O_WRONLY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_write(h, NULL, 1u), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_write(h, NULL, 0u), 0);
    CK_I(arkrtos_vfs_app_write(h, (const uint8 *)"abc", 3u), 3);
    CK_I(arkrtos_vfs_app_write(h, (const uint8 *)"de", 2u), 2);
    CK_I(arkrtos_vfs_app_seek(h, 0, ARKRTOS_VFS_SEEK_END), 5);
    CK_I(arkrtos_vfs_app_close(h), 0);

    h = arkrtos_vfs_app_open("/tmp/w.txt",
                             ARKRTOS_VFS_O_APPEND | ARKRTOS_VFS_O_WRONLY);
    CK(h > 0);
    /* 追加起点=当前大小，所以先 seek 到 0 也不影响写的位置 */
    CK_I(arkrtos_vfs_app_seek(h, 0, ARKRTOS_VFS_SEEK_SET), 0);
    CK_I(arkrtos_vfs_app_write(h, (const uint8 *)"F", 1u), 1);
    CK_I(arkrtos_vfs_app_close(h), 0);
    {
        uint32 size = 0u;

        CK_I(arkrtos_vfs_app_stat("/tmp/w.txt", &size, NULL), 0);
        CK_I(size, 6u);
    }

    /* 满表：8 个槽用满后第 9 个是 FULL；任务还活着就回收不了。
     * 同一个文件反复打开也各占一个槽 —— 槽数是句柄数，不是文件数。 */
    for (i = 0; i < (int)ARKRTOS_VFS_HANDLE_MAX; i++)
    {
        CK(arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDONLY) > 0);
    }
    CK_I(arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDONLY), ARK_E_FULL);

    /* 任务死了：下一次要句柄时顺手回收，号码也不会撞上旧的 */
    g_task_dead[1] = 1u;
    h = arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDONLY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_close(h), 0);
    g_task_dead[1] = 0u;

    /* 被回收的旧号码一律 BADF（序号已被推进） */
    CK_I(arkrtos_vfs_app_close((int32)(ARKRTOS_VFS_HANDLE_FLAG | 1u)), ARK_E_BADF);
}

/* ============================================================
 * 6. App 层 seek：位置由自己记账，不信下层的返回值
 * ============================================================ */
static void test_app_seek(void)
{
    int32 h;
    uint8 buf[4];
    uint32 size;

    env_reset();
    CK_I(arkrtos_vfs_write_file("/tmp/a.txt", (const uint8 *)"hello", 5u), 0);

    h = arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDONLY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_seek(h, 1, ARKRTOS_VFS_SEEK_SET), 1);
    CK_I(arkrtos_vfs_app_seek(h, 2, ARKRTOS_VFS_SEEK_CUR), 3);
    CK_I(arkrtos_vfs_app_read(h, buf, 2u), 2);
    CK(memcmp(buf, "lo", 2) == 0);
    CK_I(arkrtos_vfs_app_seek(h, -1, ARKRTOS_VFS_SEEK_END), 4);
    /* 越过 EOF 是合法的（POSIX），但下层 ramfs 只到容量上限，负数要被拒 */
    CK_I(arkrtos_vfs_app_seek(h, -100, ARKRTOS_VFS_SEEK_SET), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_seek(h, 0, 3u), ARK_E_INVAL);   /* 坏 whence */
    CK_I(arkrtos_vfs_app_close(h), 0);

#if (ARKRTOS_USE_VFS_LFS == 1)
    /* 下层回 0（littlefs 桥的约定）：偏移必须还是对的 */
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), 0);
    h = arkrtos_vfs_app_open("/mnt/lfs/f", ARKRTOS_VFS_O_RDONLY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_seek(h, 0, ARKRTOS_VFS_SEEK_SET), 0);
    CK_I(arkrtos_vfs_app_seek(h, 0x100, ARKRTOS_VFS_SEEK_CUR), 0x100);
    CK_I(arkrtos_vfs_app_seek(h, 0, ARKRTOS_VFS_SEEK_END), 0);

    /* 记出来的位置溢出 int32：拒绝，而不是回一个负数偏移 */
    CK_I(arkrtos_vfs_app_seek(h, (int32)0x7FFFFF00, ARKRTOS_VFS_SEEK_SET),
         (int32)0x7FFFFF00);
    CK_I(arkrtos_vfs_app_seek(h, 0x200, ARKRTOS_VFS_SEEK_CUR), ARK_E_INVAL);
    /* 往回越过 0 同理 */
    CK_I(arkrtos_vfs_app_seek(h, 0, ARKRTOS_VFS_SEEK_SET), 0);
    CK_I(arkrtos_vfs_app_seek(h, -1000, ARKRTOS_VFS_SEEK_CUR), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_close(h), 0);

    /* 下层拒绝时位置不动 */
    g_lfs_seek_rc = ARK_E_IO;
    h = arkrtos_vfs_app_open("/mnt/lfs/f", ARKRTOS_VFS_O_RDONLY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_seek(h, 0, ARKRTOS_VFS_SEEK_SET), ARK_E_IO);
    CK_I(arkrtos_vfs_app_close(h), 0);
    g_lfs_seek_rc = 0;

    /* 下层拒绝的偏移读：把原因原样带出去 */
    {
        uint32 len = 0u;

        g_lfs_seek_rc = ARK_E_IO;
        CK_I(arkrtos_vfs_read_at("/mnt/lfs/f", buf, 1u, 1u, &len), ARK_E_IO);
        CK_I(len, 0u);
        g_lfs_seek_rc = 0;

        /* 定位成功、读失败：一样原样带出去，不把错误当 EOF */
        g_lfs_read_rc = ARK_E_IO;
        CK_I(arkrtos_vfs_read_at("/mnt/lfs/f", buf, 1u, 0u, &len), ARK_E_IO);
        CK_I(len, 0u);
        g_lfs_read_rc = 0;
    }
    CK_I(arkrtos_vfs_umount("/mnt/lfs"), 0);
#endif

    /* 截断：句柄、目录、卷满各一条 */
    h = arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDONLY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_truncate(h, 3u), 0);
    CK_I(arkrtos_vfs_app_stat("/tmp/a.txt", &size, NULL), 0);
    CK_I(size, 3u);
    CK_I(arkrtos_vfs_app_seek(h, 0, ARKRTOS_VFS_SEEK_END), 3);
    CK_I(arkrtos_vfs_app_truncate(h, 600u), ARK_E_NOSPC);   /* ramfs 容量 512 */
    CK_I(arkrtos_vfs_app_truncate(h, 0u), 0);
    CK_I(arkrtos_vfs_app_close(h), 0);
    CK_I(arkrtos_vfs_app_truncate((int32)ARKRTOS_VFS_HANDLE_FLAG, 0u), ARK_E_BADF);

    CK_I(arkrtos_vfs_app_truncate_path(NULL, 0u), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_truncate_path("", 0u), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_truncate_path("/nope", 0u), ARK_E_NOENT);
    CK_I(arkrtos_vfs_write_file("/tmp/b.txt", (const uint8 *)"abcd", 4u), 0);
    CK_I(arkrtos_vfs_app_truncate_path("/tmp/b.txt", 2u), 0);
    CK_I(arkrtos_vfs_app_stat("/tmp/b.txt", &size, NULL), 0);
    CK_I(size, 2u);

    /* 只读挂载：截断必须 ROFS。ramfs 的内容跨挂载点共享，所以先在
     * 一个可写挂载点上把 b.txt 建出来，再从 /ro 以只读身份看见它。 */
    CK_I(ark_vfs_mount("rw", "/rw", ark_ramfs_fs(), NULL, 0u, NULL), ARK_VFS_OK);
    CK_I(arkrtos_vfs_write_file("/rw/b.txt", (const uint8 *)"abcd", 4u), 0);
    CK_I(ark_vfs_umount("/rw"), ARK_VFS_OK);
    CK_I(ark_vfs_mount("ro", "/ro", ark_ramfs_fs(), NULL, ARK_MNT_RO, NULL),
         ARK_VFS_OK);
    CK_I(arkrtos_vfs_app_truncate_path("/ro/b.txt", 0u), ARK_E_ROFS);
    h = arkrtos_vfs_app_open("/ro/b.txt", ARKRTOS_VFS_O_RDONLY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_truncate(h, 0u), ARK_E_ROFS);
    CK_I(arkrtos_vfs_app_close(h), 0);
}

/* ============================================================
 * 7. App 层 readdir / stat / 目录操作
 * ============================================================ */
static void test_app_dirs(void)
{
    int32 h;
    char name[ARK_VFS_NAME_MAX];
    uint32 mode;
    uint32 size;
    int n = 0;
    int32 rc;

    env_reset();
    CK_I(arkrtos_vfs_write_file("/tmp/a.txt", (const uint8 *)"hello", 5u), 0);
    CK_I(arkrtos_vfs_write_file("/tmp/a_very_long_name_1234567890.txt",
                                (const uint8 *)"x", 1u), 0);

    h = arkrtos_vfs_app_open("/tmp",
                             ARKRTOS_VFS_O_RDONLY | ARKRTOS_VFS_O_DIRECTORY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_readdir(h, NULL, 4u, NULL, NULL), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_readdir(h, name, 0u, NULL, NULL), ARK_E_INVAL);
    /* 名字装不下就报错，不回半截 */
    CK_I(arkrtos_vfs_app_readdir(h, name, 4u, NULL, NULL),
         ARK_E_NAMETOOLONG);
    CK_I((int)name[0], 0);

    while ((rc = arkrtos_vfs_app_readdir(h, name, (uint32)sizeof(name),
                                        &mode, &size)) == 0)
    {
        if (strcmp(name, "a.txt") == 0)
        {
            CK(((mode & ARKRTOS_VFS_S_IFMT) == ARKRTOS_VFS_S_IFREG) != 0u);
            CK_I(size, 5u);
        }
        n++;
    }
    CK_I(rc, 1);            /* 列完是 1，不是错误 */
    CK(n >= 2);
    CK_I(arkrtos_vfs_app_close(h), 0);

    /* 对普通文件 readdir */
    h = arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDONLY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_readdir(h, name, (uint32)sizeof(name), NULL, NULL),
         ARK_E_NOTDIR);
    CK_I(arkrtos_vfs_app_close(h), 0);

    /* stat：类型位与大小，空输出参数可以不给 */
    CK_I(arkrtos_vfs_app_stat("/tmp/a.txt", &size, &mode), 0);
    CK_I(size, 5u);
    CK(((mode & ARKRTOS_VFS_S_IFMT) == ARKRTOS_VFS_S_IFREG) != 0u);
    CK_I(arkrtos_vfs_app_stat("/tmp", NULL, &mode), 0);
    CK(((mode & ARKRTOS_VFS_S_IFMT) == ARKRTOS_VFS_S_IFDIR) != 0u);
    CK_I(arkrtos_vfs_app_stat("/nope", &size, &mode), ARK_E_NOENT);
    CK_I(size, 0u);
    CK_I(mode, 0u);
    CK_I(arkrtos_vfs_app_stat(NULL, &size, NULL), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_stat("", &size, NULL), ARK_E_INVAL);

    /* 目录操作 */
    CK_I(arkrtos_vfs_app_mkdir("/tmp/d1"), 0);
    CK_I(arkrtos_vfs_app_mkdir("/tmp/d1"), ARK_E_EXIST);
    CK_I(arkrtos_vfs_app_mkdir(NULL), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_mkdir(""), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_rmdir("/tmp/d1"), 0);
    CK_I(arkrtos_vfs_app_rmdir("/tmp/d1"), ARK_E_NOENT);
    CK_I(arkrtos_vfs_app_rmdir("/tmp/a.txt"), ARK_E_NOTDIR);
    CK_I(arkrtos_vfs_app_rmdir(NULL), ARK_E_INVAL);

    CK_I(arkrtos_vfs_app_rename("/tmp/a.txt", "/tmp/b.txt"), 0);
    CK_I(arkrtos_vfs_app_stat("/tmp/b.txt", &size, NULL), 0);
    CK_I(arkrtos_vfs_app_rename("/tmp/b.txt", NULL), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_rename(NULL, "/tmp/c.txt"), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_rename("", "/tmp/c.txt"), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_rename("/tmp/b.txt", ""), ARK_E_INVAL);

    CK_I(arkrtos_vfs_app_unlink("/tmp/b.txt"), 0);
    CK_I(arkrtos_vfs_app_unlink("/tmp/b.txt"), ARK_E_NOENT);
    CK_I(arkrtos_vfs_app_unlink(NULL), ARK_E_INVAL);
    CK_I(arkrtos_vfs_app_unlink(""), ARK_E_INVAL);

#if (ARKRTOS_USE_VFS_LFS == 1)
    /* readdir 的下层错误要原样带出去（不是「列完了」） */
    CK_I(arkrtos_vfs_mount_volume("nor0", 0u, 0u, "/mnt/lfs"), 0);
    g_lfs_readdir_rc = ARK_E_IO;
    h = arkrtos_vfs_app_open("/mnt/lfs/d",
                             ARKRTOS_VFS_O_RDONLY | ARKRTOS_VFS_O_DIRECTORY);
    CK(h > 0);
    CK_I(arkrtos_vfs_app_readdir(h, name, (uint32)sizeof(name), NULL, NULL),
         ARK_E_IO);
    CK_I(arkrtos_vfs_app_close(h), 0);
    g_lfs_readdir_rc = 0;
    CK_I(arkrtos_vfs_umount("/mnt/lfs"), 0);
#endif
}

/* ============================================================
 * 8. 任务退出与死槽回收
 * ============================================================ */
static void test_task_exit(void)
{
    int32 h1;
    int32 h2;
    uint8 buf[4];

    env_reset();
    CK_I(arkrtos_vfs_write_file("/tmp/a.txt", (const uint8 *)"hello", 5u), 0);

    arkrtos_current_task_id = 1;
    h1 = arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDONLY);
    CK(h1 > 0);
    arkrtos_current_task_id = 2;
    h2 = arkrtos_vfs_app_open("/tmp/a.txt", ARKRTOS_VFS_O_RDONLY);
    CK(h2 > 0);
    CK(h1 != h2);

    /* 只收自己的 */
    arkrtos_vfs_app_task_exit(1u);
    arkrtos_current_task_id = 1;
    CK_I(arkrtos_vfs_app_read(h1, buf, 1u), ARK_E_BADF);
    arkrtos_current_task_id = 2;
    CK_I(arkrtos_vfs_app_read(h2, buf, 1u), 1);

    /* 0 号不是合法任务：什么都不做 */
    arkrtos_vfs_app_task_exit(0u);
    CK_I(arkrtos_vfs_app_read(h2, buf, 1u), 1);

    arkrtos_vfs_app_task_exit(2u);
    CK_I(arkrtos_vfs_app_read(h2, buf, 1u), ARK_E_BADF);
    arkrtos_current_task_id = 1;
}

/* ============================================================
 * main
 * ============================================================ */
int main(void)
{
    /* 断言失败行必须落在 stdout 上就能看见：崩溃时缓冲里的内容会丢，
     * 而丢一条 FAIL 会让人以为「全过」。 */
    setvbuf(stdout, NULL, _IONBF, 0);

    test_not_ready();
    test_init_failures();
    test_init_ok();

    test_mount_info();
    test_mount_volume();
    test_facade_io();
    test_app_open();
    test_app_seek();
    test_app_dirs();
    test_task_exit();

    if (g_fails == 0)
    {
        printf("VFS-HOST OK  %d checks\n", g_checks);
        return 0;
    }
    printf("VFS-HOST FAIL  %d/%d\n", g_fails, g_checks);
    return 1;
}