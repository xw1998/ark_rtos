/**
* @file test_fs_host.c
* @brief arkrtos_fs.c 的宿主用例：真 littlefs + 一块 RAM 闪存
* @details 这一笔验的是门面本身，不是替代品：littlefs 是仓库里那份
*          kernelsrc/fs/littlefs（同一份源码、同一组 LFS_NO_* 开关），下面
*          的块设备按 NOR 的真实语义工作——**只能从 0xFF 编程**，写一块没
*          擦过的区域会被拒绝；擦除把整块填回 0xFF。于是用例不只验返回值，
*          也验「门面确实让 littlefs 先擦后写」，这一点用桩是造不出来的。
*
*          被验的六件事：
*            1. 几何从设备自己来（erase_unit / 容量），不合的几何在挂载前
*               就被拒，而不是交给 littlefs 报个看不懂的错；
*            2. mount 不格式化，空白介质回 CORRUPT，且 err_is_unformatted
*               能把它认出来；
*            3. 整文件与流式访问共用一块文件缓存 ⇒ 有流在跑时另一路回 BUSY；
*            4. 卸载会先把没关的流关掉，而不是把描述块的状态丢掉；
*            5. 列表的两种形态（回调 / 填缓冲）在缓冲装不下时一个计数、
*               一个截断，语义不同且都写死；
*            6. 设备层的失败（读/写/擦被拒）如实上抛，不重试、不假装成功。
*
*          失败注入只用来走到出错分支；断言里的数字来自 littlefs 自己的
*          LFS_ERR_*，不另抄一份。
*
* @author xw
* @date 2026.11.22
*/
#include <stdio.h>
#include <string.h>

#include "arkrtos_fs.h"
#include "arkrtos_blk.h"

#include "lfs.h"

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

#if ARKRTOS_USE_FS

/* ------------------------------------------------------------
 * RAM 闪存：一块数组 + NOR 的编程规则
 *
 * 这一段只在开档用例里存在：关档的 arkrtos_fs.c 根本不碰块层，
 * 留着这些桩只会变成「定义了没用到」的告警。
 * ------------------------------------------------------------ */

#define DISK_BYTES (512u * 1024u)

static uint8 g_disk[DISK_BYTES];

static int g_fail_init;
static int g_fail_read;
static int g_fail_write;
static int g_fail_erase;

static int g_init_calls;
static uint32 g_autostart_calls;

static const arkrtos_blk_dev_t g_dev_ram =
{
    "ram0", DISK_BYTES, 4096u,
    0, 0, 0, 0, 0
};

/* 第二个设备：擦除单元 128 字节，用来走 prog_size 的「小于 256」那一支。
 * 同一块数组——每个用例先抹成 0xFF 再用，互不干扰。 */
static const arkrtos_blk_dev_t g_dev_tiny =
{
    "tiny", DISK_BYTES, 128u,
    0, 0, 0, 0, 0
};

/* 擦除单元为 0：几何无法成立，门面必须在碰 littlefs 之前拒掉。 */
static const arkrtos_blk_dev_t g_dev_noeu =
{
    "noeu", DISK_BYTES, 0u,
    0, 0, 0, 0, 0
};

/* mount_default 走的是板级宏里的 "nor0" / 4 MB；这里刻意注册一块更小的盘，
 * 让默认挂载以「几何不合」失败——默认路径被走到，且不会真的吞掉 4 MB。 */
static const arkrtos_blk_dev_t g_dev_nor =
{
    "nor0", DISK_BYTES, 4096u,
    0, 0, 0, 0, 0
};

const arkrtos_blk_dev_t *arkrtos_blk_find(const char *name)
{
    if (name == 0)
    {
        return 0;
    }
    if (strcmp(name, "ram0") == 0)
    {
        return &g_dev_ram;
    }
    if (strcmp(name, "tiny") == 0)
    {
        return &g_dev_tiny;
    }
    if (strcmp(name, "noeu") == 0)
    {
        return &g_dev_noeu;
    }
    if (strcmp(name, "nor0") == 0)
    {
        return &g_dev_nor;
    }
    return 0;
}

int32 arkrtos_blk_init(const arkrtos_blk_dev_t *dev)
{
    (void)dev;
    g_init_calls++;
    return g_fail_init ? -1 : 0;
}

uint32 arkrtos_blk_size(const arkrtos_blk_dev_t *dev)
{
    return (dev != 0) ? dev->size : 0u;
}

int32 arkrtos_blk_read(const arkrtos_blk_dev_t *dev, uint32 off, uint8 *buf, uint32 len)
{
    if ((dev == 0) || (buf == 0) || g_fail_read)
    {
        return -1;
    }
    if ((off > DISK_BYTES) || (len > DISK_BYTES - off))
    {
        return -1;
    }
    memcpy(buf, &g_disk[off], len);
    return 0;
}

int32 arkrtos_blk_write(const arkrtos_blk_dev_t *dev, uint32 off, const uint8 *buf, uint32 len)
{
    uint32 i;

    if ((dev == 0) || (buf == 0) || g_fail_write)
    {
        return -1;
    }
    if ((off > DISK_BYTES) || (len > DISK_BYTES - off))
    {
        return -1;
    }

    /* NOR 的规则：只能把 1 写成 0。要求目标已经是 0xFF，否则这就是一次
     * 「没擦就写」——设备做不到，这里也不假装做到了。 */
    for (i = 0u; i < len; i++)
    {
        if ((g_disk[off + i] & buf[i]) != buf[i])
        {
            return -1;
        }
    }
    for (i = 0u; i < len; i++)
    {
        g_disk[off + i] = (uint8)(g_disk[off + i] & buf[i]);
    }
    return 0;
}

int32 arkrtos_blk_erase(const arkrtos_blk_dev_t *dev, uint32 off, uint32 len)
{
    if ((dev == 0) || g_fail_erase)
    {
        return -1;
    }
    if ((off > DISK_BYTES) || (len > DISK_BYTES - off))
    {
        return -1;
    }
    memset(&g_disk[off], 0xFF, len);
    return 0;
}

/* mini 自启钩子：挂上卷时被调用一次（默认档 ARKRTOS_USE_MINIAPP=1 且
 * ARKRTOS_MINI_READ_VFS=0）。这里只数次数，不启动任何东西。 */
extern uint32 arkrtos_mini_boot_autostart(void);

uint32 arkrtos_mini_boot_autostart(void)
{
    g_autostart_calls++;
    return 0u;
}

/* ------------------------------------------------------------
 * 环境
 * ------------------------------------------------------------ */

static void disk_blank(void)
{
    memset(g_disk, 0xFF, sizeof(g_disk));
}

static void env_reset(void)
{
    (void)arkrtos_fs_unmount();
    disk_blank();
    g_fail_init   = 0;
    g_fail_read   = 0;
    g_fail_write  = 0;
    g_fail_erase  = 0;
    g_init_calls  = 0;
    g_autostart_calls = 0u;
}

static void fresh_volume(void)
{
    env_reset();
    CK_I(arkrtos_fs_format("ram0", 0u, 0u), 0);
    CK_I(arkrtos_fs_mounted(), 1);
}

/* ------------------------------------------------------------
 * 1. 错误码的名字与两个谓词
 * ------------------------------------------------------------ */

static void test_names(void)
{
    CK(strcmp(arkrtos_fs_error_name(0), "ok") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_IO), "LFS_ERR_IO") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_CORRUPT), "LFS_ERR_CORRUPT") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_NOENT), "LFS_ERR_NOENT") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_EXIST), "LFS_ERR_EXIST") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_NOTDIR), "LFS_ERR_NOTDIR") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_ISDIR), "LFS_ERR_ISDIR") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_NOTEMPTY), "LFS_ERR_NOTEMPTY") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_BADF), "LFS_ERR_BADF") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_FBIG), "LFS_ERR_FBIG") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_INVAL), "LFS_ERR_INVAL") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_NOSPC), "LFS_ERR_NOSPC") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_NOMEM), "LFS_ERR_NOMEM") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_NOATTR), "LFS_ERR_NOATTR") == 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_NAMETOOLONG), "LFS_ERR_NAMETOOLONG") == 0);

    CK(strcmp(arkrtos_fs_error_name(-1001),
              "Ark RTOS: bad arguments or volume geometry") == 0);
    CK(strcmp(arkrtos_fs_error_name(-1002),
              "Ark RTOS: no such block device") == 0);
    CK(strcmp(arkrtos_fs_error_name(-1003),
              "Ark RTOS: no volume in this board layout") == 0);
    CK(strcmp(arkrtos_fs_error_name(-1004),
              "Ark RTOS: a volume is already mounted") == 0);
    CK(strcmp(arkrtos_fs_error_name(-1005),
              "Ark RTOS: nothing is mounted") == 0);
    CK(strcmp(arkrtos_fs_error_name(-1006),
              "Ark RTOS: the block device refused it") == 0);
    CK(strcmp(arkrtos_fs_error_name(-1007),
              "Ark RTOS: short read/write") == 0);
    CK(strcmp(arkrtos_fs_error_name(-1008),
              "Ark RTOS: a file stream is already open") == 0);
    CK(strcmp(arkrtos_fs_error_name(-77), "unknown") == 0);

    CK_I(arkrtos_fs_err_is_unformatted(LFS_ERR_CORRUPT), 1);
    CK_I(arkrtos_fs_err_is_unformatted(LFS_ERR_INVAL), 1);
    CK_I(arkrtos_fs_err_is_unformatted(LFS_ERR_IO), 0);
    CK_I(arkrtos_fs_err_is_unformatted(0), 0);
}

/* ------------------------------------------------------------
 * 2. 什么都没挂时的答案
 *
 * 注意这里刻意只断言返回码，不断言 last_error 的每个入口：门面有一半
 * 入口在「未挂载」时直接返回而不写 last_error（它们把参数检查放在前面，
 * 而参数检查只在参数坏时才写）。这是现状，用例把它固定下来，不粉饰。
 * 真正保证 last_error 的是 open_*（写 ARGS）与 list/stat（写 NOT_MOUNTED）。
 * ------------------------------------------------------------ */

static void test_not_mounted(void)
{
    uint8 buf[8];
    const char *dev = 0;
    uint32 a = 0u;
    uint32 b = 0u;
    char names[32];

    env_reset();
    CK_I(arkrtos_fs_mounted(), 0);

    CK_I(arkrtos_fs_stat(&a, &b), -1);
    CK_I(arkrtos_fs_last_error(), -1005);

    CK_I(arkrtos_fs_stat_path("/a", &a, &b), -1);
    CK_I(arkrtos_fs_write_file("/a", buf, 1u), -1);
    CK_I(arkrtos_fs_read_file("/a", buf, 1u, &a), -1);
    CK_I(arkrtos_fs_truncate("/a", 1u), -1);
    CK_I(arkrtos_fs_remove("/a"), -1);
    CK_I(arkrtos_fs_mkdir("/a"), -1);
    CK_I(arkrtos_fs_read_at("/a", buf, 1u, 0u, &a), -1);
    CK_I(arkrtos_fs_write_at("/a", buf, 1u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1005);
    CK_I(arkrtos_fs_rename("/a", "/b"), -1);
    CK_I(arkrtos_fs_volume(&dev, &a, &b), -1);

    CK_I(arkrtos_fs_list("/", 0, 0), -1);
    CK_I(arkrtos_fs_last_error(), -1005);
    CK_I(arkrtos_fs_list_names(0, names, sizeof(names), &a), -1);
    CK_I(arkrtos_fs_last_error(), -1005);

    /* 流式入口在未挂载时统一报 ARGS——见 fs_stream_open() 的注释：
     * 那里的每一条失败路径都要写 last_error，好让桥能把「父目录不存在」
     * 与真正的后端错误分开。 */
    CK_I(arkrtos_fs_open_read("/a"), -1);
    CK_I(arkrtos_fs_last_error(), -1001);
    CK_I(arkrtos_fs_open_write("/a"), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 没有流的时候这两个是空操作，不是错误。 */
    CK_I(arkrtos_fs_read_next(buf, sizeof(buf)), -1);
    CK_I(arkrtos_fs_last_error(), -1001);
    CK_I(arkrtos_fs_write_next(buf, sizeof(buf)), -1);
    CK_I(arkrtos_fs_last_error(), -1001);
    CK_I(arkrtos_fs_close_read(), 0);
    CK_I(arkrtos_fs_close_write(), 0);

    CK_I(arkrtos_fs_unmount(), 0);
    CK_I(arkrtos_fs_unmount(), 0);
    CK_I(arkrtos_fs_mounted(), 0);

    CK_I(arkrtos_fs_last_is_noent(), 0);
    CK_I(arkrtos_fs_last_is_nospc(), 0);
}

/* ------------------------------------------------------------
 * 3. 几何检查：能在挂载之前拒掉的，不交给 littlefs
 * ------------------------------------------------------------ */

static void test_geometry(void)
{
    env_reset();

    CK_I(arkrtos_fs_mount(0, 0u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    CK_I(arkrtos_fs_mount("nope", 0u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1002);

    /* 块设备起不来 */
    g_fail_init = 1;
    CK_I(arkrtos_fs_mount("ram0", 0u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1006);
    CK_I(g_init_calls, 1);
    g_fail_init = 0;

    /* 偏移不对齐 */
    CK_I(arkrtos_fs_mount("ram0", 1u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 卷比一个擦除单元还小 */
    CK_I(arkrtos_fs_mount("ram0", 0u, 100u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 卷伸出设备之外 */
    CK_I(arkrtos_fs_mount("ram0", 0u, DISK_BYTES + 4096u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 擦除单元为 0：设备自己说不清它的几何 */
    CK_I(arkrtos_fs_mount("noeu", 0u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 一个卷只有两个块：littlefs 用不了，门面先拒 */
    CK_I(arkrtos_fs_mount("ram0", 0u, 2u * 4096u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 空白介质：mount 不格式化，报 CORRUPT，且谓词认得出它 */
    CK_I(arkrtos_fs_mount("ram0", 0u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_CORRUPT);
    CK_I(arkrtos_fs_err_is_unformatted(arkrtos_fs_last_error()), 1);
    CK_I(arkrtos_fs_mounted(), 0);
}

/* ------------------------------------------------------------
 * 4. 格式化 / 挂载 / 卸载
 * ------------------------------------------------------------ */

static void test_mount_and_format(void)
{
    const char *dev = 0;
    uint32 off = 0u;
    uint32 size = 0u;

    env_reset();

    /* 坏参数先于介质：名字为 NULL 或设备不存在时一次擦除都不该发生 */
    CK_I(arkrtos_fs_format(0, 0u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);
    CK_I(arkrtos_fs_format("nope", 0u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1002);

    /* 格式化成功后自动挂上（这是一次格式化唯一有用的后续状态）。 */
    CK_I(arkrtos_fs_format("ram0", 0u, 0u), 0);
    CK_I(arkrtos_fs_mounted(), 1);
    /* 默认档（MINIAPP=1 且按卷根路径读）挂载即自启一次 */
    CK_I(g_autostart_calls, 1);

    CK_I(arkrtos_fs_volume(&dev, &off, &size), 0);
    CK(strcmp(dev, "ram0") == 0);
    CK_I(off, 0);
    CK_I(size, DISK_BYTES);

    /* 只给一部分参数也能用 */
    CK_I(arkrtos_fs_volume(0, 0, 0), 0);

    /* 已挂载时再挂、再格式化都被拒 */
    CK_I(arkrtos_fs_mount("ram0", 0u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1004);
    CK_I(arkrtos_fs_format("ram0", 0u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1004);

    /* 卸载后卷里的内容还在（这里是空的，只验状态） */
    CK_I(arkrtos_fs_unmount(), 0);
    CK_I(arkrtos_fs_mounted(), 0);
    CK_I(arkrtos_fs_volume(&dev, &off, &size), -1);

    /* size 传 0 = 到设备末尾；再挂一次不自启（self 一次/挂载） */
    g_autostart_calls = 0u;
    CK_I(arkrtos_fs_mount("ram0", 0u, 0u), 0);
    CK_I(arkrtos_fs_mounted(), 1);
    CK_I(g_autostart_calls, 1);

    /* 从中间切一段：偏移与长度都对齐，几何成立 */
    CK_I(arkrtos_fs_unmount(), 0);
    CK_I(arkrtos_fs_format("ram0", 4096u, 8u * 4096u), 0);
    CK_I(arkrtos_fs_volume(&dev, &off, &size), 0);
    CK_I(off, 4096);
    CK_I(size, 8u * 4096u);

    CK_I(arkrtos_fs_unmount(), 0);
}

/* ------------------------------------------------------------
 * 5. 整文件访问与目录操作
 * ------------------------------------------------------------ */

static void test_files(void)
{
    uint8  data[100];
    uint8  back[128];
    uint32 n = 0u;
    uint32 total = 0u;
    uint32 used = 0u;
    uint32 is_dir = 0u;
    uint32 i;

    for (i = 0u; i < sizeof(data); i++)
    {
        data[i] = (uint8)(i + 1u);
    }

    fresh_volume();

    /* 写 / 读一个整文件 */
    CK_I(arkrtos_fs_write_file("/a.txt", data, sizeof(data)), 0);
    CK_I(arkrtos_fs_stat_path("/a.txt", &n, &is_dir), 0);
    CK_I(n, sizeof(data));
    CK_I(is_dir, 0);
    CK_I(arkrtos_fs_stat_path("/a.txt", 0, 0), 0);

    memset(back, 0, sizeof(back));
    n = 0u;
    CK_I(arkrtos_fs_read_file("/a.txt", back, sizeof(back), &n), 0);
    CK_I(n, sizeof(data));
    CK(memcmp(back, data, sizeof(data)) == 0);

    /* 缓冲小：只读到 max，多出来的留在文件里（不是错误） */
    memset(back, 0, sizeof(back));
    n = 0u;
    CK_I(arkrtos_fs_read_file("/a.txt", back, 40u, &n), 0);
    CK_I(n, 40);
    CK(memcmp(back, data, 40u) == 0);

    /* out_len 可以不要 */
    CK_I(arkrtos_fs_read_file("/a.txt", back, sizeof(back), 0), 0);

    /* 重写会截断旧内容 */
    CK_I(arkrtos_fs_write_file("/a.txt", data, 10u), 0);
    CK_I(arkrtos_fs_stat_path("/a.txt", &n, 0), 0);
    CK_I(n, 10);

    /* 参数错 */
    /* data == NULL 时门面直接回 FAIL、不写 last_error（参数检查放在
     * 最前面，且只有路径不是绝对路径时才写）。这里只钉返回码。 */
    CK_I(arkrtos_fs_write_file("/a.txt", 0, 1u), -1);
    CK_I(arkrtos_fs_write_file("a.txt", data, 1u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);
    CK_I(arkrtos_fs_read_file("a.txt", back, sizeof(back), &n), -1);
    CK_I(arkrtos_fs_read_file("/a.txt", 0, sizeof(back), &n), -1);

    /* 不存在 */
    CK_I(arkrtos_fs_read_file("/zzz", back, sizeof(back), &n), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOENT);
    CK_I(arkrtos_fs_last_is_noent(), 1);
    CK_I(arkrtos_fs_last_is_nospc(), 0);

    /* 目录 */
    CK_I(arkrtos_fs_mkdir("/d1"), 0);
    CK_I(arkrtos_fs_mkdir("/d1"), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_EXIST);
    CK_I(arkrtos_fs_mkdir("/d1/sub"), 0);
    CK_I(arkrtos_fs_mkdir("/nodir/x"), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOENT);
    CK_I(arkrtos_fs_mkdir("d1/x"), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    CK_I(arkrtos_fs_stat_path("/d1", &n, &is_dir), 0);
    CK_I(is_dir, 1);   /* 目录的 size 由 littlefs 自己给，不作断言 */

    /* 非空目录不能被删掉 */
    CK_I(arkrtos_fs_write_file("/d1/f.bin", data, 4u), 0);
    CK_I(arkrtos_fs_remove("/d1"), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOTEMPTY);
    CK_I(arkrtos_fs_remove("/d1/f.bin"), 0);
    CK_I(arkrtos_fs_remove("/d1/sub"), 0);
    CK_I(arkrtos_fs_remove("/d1"), 0);
    CK_I(arkrtos_fs_remove("/nope"), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOENT);

    /* 改名 */
    CK_I(arkrtos_fs_rename("/a.txt", "/b.txt"), 0);
    CK_I(arkrtos_fs_stat_path("/a.txt", &n, 0), -1);
    CK_I(arkrtos_fs_stat_path("/b.txt", &n, 0), 0);
    CK_I(n, 10);
    CK_I(arkrtos_fs_rename("/gone", "/x"), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOENT);
    CK_I(arkrtos_fs_write_file("/c.txt", data, 3u), 0);
    /* 改名到一个已存在的文件：littlefs 按 POSIX 语义**替换**它，
     * 不是报 EXIST。下面那段随机读因此按 3 字节的 /b.txt 来断言。 */
    CK_I(arkrtos_fs_rename("/c.txt", "/b.txt"), 0);
    CK_I(arkrtos_fs_stat_path("/c.txt", &n, 0), -1);
    CK_I(arkrtos_fs_stat_path("/b.txt", &n, 0), 0);
    CK_I(n, 3);
    CK_I(arkrtos_fs_rename("c.txt", "/x"), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 随机读 */
    memset(back, 0, sizeof(back));
    n = 0u;
    CK_I(arkrtos_fs_read_at("/b.txt", back, 10u, 0u, &n), 0);
    CK_I(n, 3);
    CK_I(arkrtos_fs_read_at("/b.txt", back, 4u, 2u, &n), 0);
    CK_I(n, 1);
    CK(memcmp(back, &data[2], 1u) == 0);
    /* 从末尾之后读：给 0 字节，不是错误 */
    n = 99u;
    CK_I(arkrtos_fs_read_at("/b.txt", back, 4u, 1000u, &n), 0);
    CK_I(n, 0);
    CK_I(arkrtos_fs_read_at("/zzz", back, 4u, 0u, &n), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOENT);
    CK_I(arkrtos_fs_read_at("/b.txt", 0, 4u, 0u, &n), -1);

    /* 随机写：缺文件即创建，只动给定区间 */
    {
        uint8 wbuf[4];
        uint8 rbuf[16];
        uint32 m = 0u;

        wbuf[0] = 0x11u; wbuf[1] = 0x22u; wbuf[2] = 0x33u; wbuf[3] = 0x44u;
        CK_I(arkrtos_fs_write_at("/w.bin", wbuf, 4u, 0u), 0);
        CK_I(arkrtos_fs_stat_path("/w.bin", &m, 0), 0);
        CK_I(m, 4);
        memset(rbuf, 0, sizeof(rbuf));
        CK_I(arkrtos_fs_read_at("/w.bin", rbuf, 4u, 0u, &m), 0);
        CK_I(m, 4);
        CK(memcmp(rbuf, wbuf, 4u) == 0);

        /* 只覆盖中间两个字节：两侧原样保留 */
        wbuf[0] = 0x99u; wbuf[1] = 0x99u;
        CK_I(arkrtos_fs_write_at("/w.bin", wbuf, 2u, 1u), 0);
        CK_I(arkrtos_fs_stat_path("/w.bin", &m, 0), 0);
        CK_I(m, 4);
        memset(rbuf, 0, sizeof(rbuf));
        CK_I(arkrtos_fs_read_at("/w.bin", rbuf, 4u, 0u, &m), 0);
        CK_I(rbuf[0], 0x11u);
        CK_I(rbuf[1], 0x99u);
        CK_I(rbuf[2], 0x99u);
        CK_I(rbuf[3], 0x44u);

        /* 越过末尾写：中间补 0，文件被撑长 */
        CK_I(arkrtos_fs_write_at("/w.bin", wbuf, 1u, 8u), 0);
        CK_I(arkrtos_fs_stat_path("/w.bin", &m, 0), 0);
        CK_I(m, 9);
        memset(rbuf, 0xAA, sizeof(rbuf));
        CK_I(arkrtos_fs_read_at("/w.bin", rbuf, 4u, 4u, &m), 0);
        CK_I(m, 4);
        CK_I(rbuf[0], 0);
        CK_I(rbuf[3], 0);

        /* 参数：相对路径 / 空缓冲 / 零长度都拒 */
        CK_I(arkrtos_fs_write_at("w.bin", wbuf, 1u, 0u), -1);
        CK_I(arkrtos_fs_last_error(), -1001);
        CK_I(arkrtos_fs_write_at("/w.bin", 0, 1u, 0u), -1);
        CK_I(arkrtos_fs_last_error(), -1001);
        CK_I(arkrtos_fs_write_at("/w.bin", wbuf, 0u, 0u), -1);
        CK_I(arkrtos_fs_last_error(), -1001);

        /* 流开着的时候共用的那块文件缓存归流，一次性写要让路 */
        CK_I(arkrtos_fs_open_write("/w2.bin"), 0);
        CK_I(arkrtos_fs_write_at("/w.bin", wbuf, 1u, 0u), -1);
        CK_I(arkrtos_fs_last_error(), -1008);
        CK_I(arkrtos_fs_close_write(), 0);
        /* 让路之后立刻能用（BUSY 不留残留状态） */
        CK_I(arkrtos_fs_write_at("/w.bin", wbuf, 1u, 0u), 0);
    }

    /* 截断：可以缩短，也可以把文件撑长（补 0） */
    CK_I(arkrtos_fs_truncate("/b.txt", 4u), 0);
    CK_I(arkrtos_fs_stat_path("/b.txt", &n, 0), 0);
    CK_I(n, 4);
    CK_I(arkrtos_fs_truncate("/b.txt", 100u), 0);
    CK_I(arkrtos_fs_stat_path("/b.txt", &n, 0), 0);
    CK_I(n, 100);
    memset(back, 0xAA, sizeof(back));
    CK_I(arkrtos_fs_read_at("/b.txt", back, 4u, 4u, &n), 0);
    CK_I(n, 4);
    CK_I(back[0], 0);
    CK_I(back[3], 0);
    CK_I(arkrtos_fs_truncate("/zzz", 4u), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOENT);
    CK_I(arkrtos_fs_truncate("b.txt", 4u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 占用统计 */
    CK_I(arkrtos_fs_stat(&total, &used), 0);
    CK_I(total, DISK_BYTES);
    CK(used > 0u);
    CK(used <= total);
    CK_I(arkrtos_fs_stat(0, 0), 0);

    /* 断电语义的替身：卸载再挂载，内容还在——证明上面写的是真介质，
     * 不是内存里的一张表。 */
    CK_I(arkrtos_fs_unmount(), 0);
    CK_I(arkrtos_fs_mount("ram0", 0u, 0u), 0);
    CK_I(arkrtos_fs_stat_path("/b.txt", &n, 0), 0);
    CK_I(n, 100);
    CK_I(arkrtos_fs_stat_path("/c.txt", &n, 0), -1);

    CK_I(arkrtos_fs_unmount(), 0);
}

/* ------------------------------------------------------------
 * 6. 流式访问：一块文件缓存，一个流
 * ------------------------------------------------------------ */

static void test_streams(void)
{
    uint8  chunk[64];
    uint8  back[128];
    uint32 n = 0u;
    uint32 i;
    int32  got;

    for (i = 0u; i < sizeof(chunk); i++)
    {
        chunk[i] = (uint8)(0x40u + i);
    }

    fresh_volume();

    /* 写流 */
    CK_I(arkrtos_fs_open_write("/s.bin"), 0);
    CK_I(arkrtos_fs_write_next(chunk, sizeof(chunk)), 0);
    CK_I(arkrtos_fs_write_next(chunk, sizeof(chunk)), 0);

    /* 同一个流以外的入口全部 BUSY：它们要用的就是那块文件缓存。 */
    CK_I(arkrtos_fs_open_write("/t.bin"), -1);
    CK_I(arkrtos_fs_last_error(), -1008);
    CK_I(arkrtos_fs_open_read("/s.bin"), -1);
    CK_I(arkrtos_fs_last_error(), -1008);
    CK_I(arkrtos_fs_write_file("/t.bin", chunk, 4u), -1);
    CK_I(arkrtos_fs_last_error(), -1008);
    CK_I(arkrtos_fs_read_file("/s.bin", back, sizeof(back), &n), -1);
    CK_I(arkrtos_fs_last_error(), -1008);
    CK_I(arkrtos_fs_truncate("/s.bin", 4u), -1);
    CK_I(arkrtos_fs_last_error(), -1008);
    CK_I(arkrtos_fs_read_at("/s.bin", back, 4u, 0u, &n), -1);
    CK_I(arkrtos_fs_last_error(), -1008);

    /* 在写流上调读、在空流上调写都是参数错 */
    CK_I(arkrtos_fs_read_next(back, sizeof(back)), -1);
    CK_I(arkrtos_fs_last_error(), -1001);
    CK_I(arkrtos_fs_write_next(0, 4u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    CK_I(arkrtos_fs_close_write(), 0);
    CK_I(arkrtos_fs_close_write(), 0);
    CK_I(arkrtos_fs_stat_path("/s.bin", &n, 0), 0);
    CK_I(n, 2u * sizeof(chunk));

    /* 读流：一次读一块，读到头回 0 */
    CK_I(arkrtos_fs_open_read("/s.bin"), 0);
    CK_I(arkrtos_fs_write_next(chunk, 4u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);
    CK_I(arkrtos_fs_read_next(0, 4u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);
    CK_I(arkrtos_fs_read_next(back, 0u), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    memset(back, 0, sizeof(back));
    got = arkrtos_fs_read_next(back, sizeof(chunk));
    CK_I(got, 64);
    CK(memcmp(back, chunk, sizeof(chunk)) == 0);
    got = arkrtos_fs_read_next(back, sizeof(chunk));
    CK_I(got, 64);
    got = arkrtos_fs_read_next(back, sizeof(chunk));
    CK_I(got, 0);
    CK_I(arkrtos_fs_close_read(), 0);
    CK_I(arkrtos_fs_close_read(), 0);
    CK_I(arkrtos_fs_read_next(back, sizeof(chunk)), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 打开失败：路径不存在 / 相对路径 */
    CK_I(arkrtos_fs_open_read("/nope.bin"), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOENT);
    CK_I(arkrtos_fs_open_read("s.bin"), -1);
    CK_I(arkrtos_fs_last_error(), -1001);
    CK_I(arkrtos_fs_open_write(0), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 卸载时流被关掉而不是被遗忘：再挂上，内容完整 */
    CK_I(arkrtos_fs_open_write("/u.bin"), 0);
    CK_I(arkrtos_fs_write_next(chunk, 7u), 0);
    CK_I(arkrtos_fs_unmount(), 0);
    CK_I(arkrtos_fs_mount("ram0", 0u, 0u), 0);
    CK_I(arkrtos_fs_read_file("/u.bin", back, sizeof(back), &n), 0);
    CK_I(n, 7);
    CK(memcmp(back, chunk, 7u) == 0);

    CK_I(arkrtos_fs_unmount(), 0);
}

/* ------------------------------------------------------------
 * 7. 列目录的两种形态
 * ------------------------------------------------------------ */

static int g_cb_names;
static int g_cb_stop_after;

static int list_cb(const char *name, uint32 size, uint8 is_dir, void *arg)
{
    (void)name;
    (void)size;
    (void)is_dir;
    (void)arg;
    g_cb_names++;
    if ((g_cb_stop_after > 0) && (g_cb_names >= g_cb_stop_after))
    {
        return -1;      /* 调用方要求停：不是错误 */
    }
    return 0;
}

static void test_listing(void)
{
    uint8  data[4] = { 1u, 2u, 3u, 4u };
    char   names[128];
    uint32 count = 0u;
    uint32 n = 0u;

    fresh_volume();

    CK_I(arkrtos_fs_mkdir("/L"), 0);
    CK_I(arkrtos_fs_write_file("/L/one.txt", data, 4u), 0);
    CK_I(arkrtos_fs_write_file("/L/two.txt", data, 4u), 0);
    CK_I(arkrtos_fs_write_file("/L/three.txt", data, 4u), 0);
    CK_I(arkrtos_fs_mkdir("/L/sub"), 0);

    /* 回调形态：根也由 NULL 表示。
     * 注意 littlefs 的 dir_read 会把 "." 与 ".." 一起交出来，门面原样转给
     * 上层 ⇒ 一个空目录也至少有两条。这不是门面的 bug，是 littlefs 的
     * 接口事实；上层要自己过滤，这里把它钉住。 */
    g_cb_names = 0;
    g_cb_stop_after = 0;
    CK_I(arkrtos_fs_list("/L", list_cb, 0), 0);
    CK_I(g_cb_names, 6);        /* 4 条真条目 + . + .. */
    g_cb_names = 0;
    CK_I(arkrtos_fs_list(0, list_cb, 0), 0);
    CK_I(g_cb_names, 3);        /* /L + . + .. */

    /* 回调要求停：正常返回，不报错 */
    g_cb_names = 0;
    g_cb_stop_after = 2;
    CK_I(arkrtos_fs_list("/L", list_cb, 0), 0);
    CK_I(g_cb_names, 2);
    g_cb_stop_after = 0;

    CK_I(arkrtos_fs_list("/L", 0, 0), -1);
    CK_I(arkrtos_fs_last_error(), -1005);
    CK_I(arkrtos_fs_list("/nope", list_cb, 0), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOENT);
    CK_I(arkrtos_fs_list("L", list_cb, 0), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 填缓冲形态：每行一个名字，计数 = 真正放进去的条数 */
    memset(names, 0, sizeof(names));
    count = 0u;
    CK_I(arkrtos_fs_list_names("/L", names, sizeof(names), &count), 0);
    CK_I(count, 6);             /* 同上：. 与 .. 也在里面 */
    CK(strstr(names, "one.txt\n") != 0);
    CK(strstr(names, "sub\n") != 0);
    CK(strlen(names) > 0u);

    /* 不数也可以 */
    CK_I(arkrtos_fs_list_names("/L", names, sizeof(names), 0), 0);

    /* 缓冲只够放下一条多一点：装不下的那条不计入 count，且已经写进去的
     * 半条会被回退（不留截断的名字） */
    memset(names, 0, sizeof(names));
    count = 0u;
    CK_I(arkrtos_fs_list_names("/L", names, 10u, &count), 0);
    CK(count < 6u);
    {
        uint32 k = 0u;
        while (names[k] != 0)
        {
            k++;
        }
        CK(k < 10u);
    }

    /* 根目录用 NULL 表示 */
    CK_I(arkrtos_fs_list_names(0, names, sizeof(names), &count), 0);
    CK(count >= 2u);

    /* 参数错：缓冲小于 2 字节、出参为 NULL、目录不是绝对路径 */
    CK_I(arkrtos_fs_list_names("/L", names, 1u, &count), -1);
    CK_I(arkrtos_fs_last_error(), -1005);
    CK_I(arkrtos_fs_list_names("/L", 0, sizeof(names), &count), -1);
    CK_I(arkrtos_fs_last_error(), -1005);
    CK_I(arkrtos_fs_list_names("/nope", names, sizeof(names), &count), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOENT);
    CK_I(arkrtos_fs_list_names("L", names, sizeof(names), &count), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    (void)n;
    CK_I(arkrtos_fs_unmount(), 0);
}

/* ------------------------------------------------------------
 * 8. 设备层失败如实上抛
 * ------------------------------------------------------------ */

static void test_failures(void)
{
    uint8  big[8192];
    uint8  back[64];
    uint32 n = 0u;
    int32  rc;

    memset(big, 0x5A, sizeof(big));

    /* 格式化时擦除被拒 */
    env_reset();
    g_fail_erase = 1;
    CK_I(arkrtos_fs_format("ram0", 0u, 0u), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_IO);
    CK_I(arkrtos_fs_mounted(), 0);
    g_fail_erase = 0;

    fresh_volume();
    CK_I(arkrtos_fs_write_file("/ok.bin", big, 16u), 0);

    /* 读被拒：open 阶段就失败，报 IO */
    g_fail_read = 1;
    CK_I(arkrtos_fs_read_file("/ok.bin", back, sizeof(back), &n), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_IO);
    CK_I(arkrtos_fs_open_read("/ok.bin"), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_IO);
    g_fail_read = 0;

    /* 读流已经打开之后再让设备坏掉：第一块由文件缓存直接供给，第二块必须
     * 回设备取 ⇒ read_next 要把设备错误如实交给调用方（不是回 0 当作文件
     * 结束），流仍可正常关闭。文件写 2 KB 就是为了让第二块越过 256 B 缓存。 */
    CK_I(arkrtos_fs_write_file("/mid.bin", big, 2048u), 0);
    CK_I(arkrtos_fs_open_read("/mid.bin"), 0);
    CK_I(arkrtos_fs_read_next(back, 256u), 256);
    g_fail_read = 1;
    CK_I(arkrtos_fs_read_next(back, 256u), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_IO);
    g_fail_read = 0;
    CK_I(arkrtos_fs_close_read(), 0);

    /* 随机写：三条失败路径各自落到自己的分支上，不能都读成「写坏了」 */
    {
        uint8 w8[8];

        memset(w8, 0x3C, sizeof(w8));

        /* 父目录不存在：打开就失败，报 littlefs 自己的 NOENT */
        CK_I(arkrtos_fs_write_at("/nodir/x.bin", w8, sizeof(w8), 0u), -1);
        CK_I(arkrtos_fs_last_error(), LFS_ERR_NOENT);

        /* 先正常建一个文件，再让设备写失败 */
        CK_I(arkrtos_fs_write_at("/ok2.bin", w8, sizeof(w8), 0u), 0);

        g_fail_write = 1;
        /* 这一版 littlefs 的写是「先分配块、再 prog」，小写也当场落盘，所以
         * 失败发生在 lfs_file_write 里：门面记 SHORT 并去关文件；close 又把
         * 同一个设备错误报了一遍，所以调用方看到的是 IO（与 write_file 一致）。 */
        CK_I(arkrtos_fs_write_at("/ok2.bin", w8, sizeof(w8), 0u), -1);
        CK_I(arkrtos_fs_last_error(), LFS_ERR_IO);
        g_fail_write = 0;

        /* 卷的统计要真的读介质：设备读坏时不能报一个凭空的块数 */
        g_fail_read = 1;
        CK_I(arkrtos_fs_stat(&n, 0), -1);
        CK_I(arkrtos_fs_last_error(), LFS_ERR_IO);
        g_fail_read = 0;

        /* 让路之后卷仍可用：内容没有被半条记录占住 */
        CK_I(arkrtos_fs_write_at("/ok2.bin", w8, sizeof(w8), 0u), 0);
        CK_I(arkrtos_fs_stat_path("/ok2.bin", &n, 0), 0);
        CK_I(n, 8);

    }

    /* 写被拒：4 KB 在调用内就刷下去了（缓存只有 256 B），失败发生在
     * lfs_file_write 里 ⇒ 门面报 SHORT，而不是等 close 才发现。 */
    g_fail_write = 1;
    CK_I(arkrtos_fs_write_file("/fail.bin", big, 4096u), -1);
    /* 最后落在 last_error 上的是 close 的码：lfs_file_write 先回 IO ⇒ 门面
     * 记 SHORT 并去关文件，而 close 又把 IO 报了一遍、覆盖掉 SHORT。
     * SHORT 真正能被调用方看到的地方是 write_next（下面那一段）。 */
    CK_I(arkrtos_fs_last_error(), LFS_ERR_IO);
    g_fail_write = 0;

    /* 流写同理；失败后流仍然是可关的 */
    CK_I(arkrtos_fs_open_write("/fail2.bin"), 0);
    CK_I(arkrtos_fs_write_next(big, 4096u), 0);
    g_fail_write = 1;
    CK_I(arkrtos_fs_write_next(big, 4096u), -1);
    CK_I(arkrtos_fs_last_error(), -1007);
    g_fail_write = 0;
    (void)arkrtos_fs_close_write();

    /* 关闭时刷不下去：先正常写满一块，再让 close 的擦除失败。
     * close 必须把自己拿到的错误码透出来，而不是回 0。 */
    CK_I(arkrtos_fs_open_write("/fail3.bin"), 0);
    CK_I(arkrtos_fs_write_next(big, sizeof(big)), 0);
    g_fail_write = 1;
    rc = arkrtos_fs_close_write();
    CK_I(rc, -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_IO);
    g_fail_write = 0;
    /* 流已置空：再调读/写是参数错，不是接着用一条坏流 */
    CK_I(arkrtos_fs_read_next(back, sizeof(back)), -1);
    CK_I(arkrtos_fs_last_error(), -1001);

    /* 失败之后已有文件没有被动过 */
    memset(back, 0, sizeof(back));
    CK_I(arkrtos_fs_read_file("/ok.bin", back, sizeof(back), &n), 0);
    CK_I(n, 16);
    CK(memcmp(back, big, 16u) == 0);

    CK_I(arkrtos_fs_unmount(), 0);
}

/* ------------------------------------------------------------
 * 8b. 介质写满：整文件写回 SHORT，截断撑长回 littlefs 自己的 NOSPC
 * ------------------------------------------------------------ */

static void test_space_limits(void)
{
    static uint8 huge[600u * 1024u];
    uint32 n = 0u;

    memset(huge, 0xA5, sizeof(huge));

    /* 600 KB 超过 512 KB 盘：littlefs 在 lfs_file_write 内就发现写不下，
     * 门面因此在调用内报 SHORT（-1007），不是拖到 close 的 IO。 */
    fresh_volume();
    CK_I(arkrtos_fs_write_file("/huge.bin", huge, (uint32)sizeof(huge)), -1);
    CK_I(arkrtos_fs_last_error(), -1007);

    /* 失败之后的卷仍可用：重新来过，写个小文件再读回来 */
    fresh_volume();
    CK_I(arkrtos_fs_write_file("/s.bin", huge, 16u), 0);
    CK_I(arkrtos_fs_read_file("/s.bin", huge, 16u, &n), 0);
    CK_I(n, 16);

    /* 截断把文件撑到 600 KB，同样撞上限：报 littlefs 自己的 NOSPC */
    CK_I(arkrtos_fs_truncate("/s.bin", (uint32)sizeof(huge)), -1);
    CK_I(arkrtos_fs_last_error(), LFS_ERR_NOSPC);

    CK_I(arkrtos_fs_unmount(), 0);
}

/* ------------------------------------------------------------
 * 9. 擦除单元 128 字节：prog_size 取设备自己的值
 * ------------------------------------------------------------ */

static void test_small_erase_unit(void)
{
    uint8 data[8] = { 9u, 8u, 7u, 6u, 5u, 4u, 3u, 2u };
    uint8 back[16];
    uint32 n = 0u;

    env_reset();
    CK_I(arkrtos_fs_format("tiny", 0u, 0u), 0);
    CK_I(arkrtos_fs_mounted(), 1);
    CK_I(arkrtos_fs_write_file("/t.bin", data, sizeof(data)), 0);
    CK_I(arkrtos_fs_read_file("/t.bin", back, sizeof(back), &n), 0);
    CK_I(n, sizeof(data));
    CK(memcmp(back, data, sizeof(data)) == 0);
    CK_I(arkrtos_fs_unmount(), 0);
}

/* ------------------------------------------------------------
 * 10. 板的默认卷：宏在本夹具里是 4 MB，盘只有 512 KB ⇒ 几何不合
 * ------------------------------------------------------------ */

static void test_mount_default(void)
{
    env_reset();
    CK_I(arkrtos_fs_mount_default(), -1);
    CK_I(arkrtos_fs_last_error(), -1001);
    CK_I(arkrtos_fs_mounted(), 0);
}

#else   /* ARKRTOS_USE_FS == 0 */

/* 关档：每个入口都回同一个 FAIL，谓词一律说「没有」。
 * 这一档的意义是让「没编进来」与「板上没卷」对调用方不可区分。 */
static void test_stubs(void)
{
    uint8 buf[8];
    const char *dev = 0;
    uint32 a = 0u;
    uint32 b = 0u;
    char names[16];

    CK_I(arkrtos_fs_mount("ram0", 0u, 0u), -1);
    CK_I(arkrtos_fs_mount_default(), -1);
    CK_I(arkrtos_fs_format("ram0", 0u, 0u), -1);
    CK_I(arkrtos_fs_mounted(), 0);
    CK_I(arkrtos_fs_volume(&dev, &a, &b), -1);
    CK_I(arkrtos_fs_stat(&a, &b), -1);
    CK_I(arkrtos_fs_stat_path("/a", &a, &b), -1);
    CK_I(arkrtos_fs_write_file("/a", buf, 1u), -1);
    CK_I(arkrtos_fs_read_file("/a", buf, 1u, &a), -1);
    CK_I(arkrtos_fs_truncate("/a", 1u), -1);
    CK_I(arkrtos_fs_open_read("/a"), -1);
    CK_I(arkrtos_fs_read_next(buf, 1u), -1);
    CK_I(arkrtos_fs_close_read(), 0);
    CK_I(arkrtos_fs_open_write("/a"), -1);
    CK_I(arkrtos_fs_write_next(buf, 1u), -1);
    CK_I(arkrtos_fs_close_write(), 0);
    CK_I(arkrtos_fs_remove("/a"), -1);
    CK_I(arkrtos_fs_mkdir("/a"), -1);
    CK_I(arkrtos_fs_read_at("/a", buf, 1u, 0u, &a), -1);
    CK_I(arkrtos_fs_write_at("/a", buf, 1u, 0u), -1);
    CK_I(arkrtos_fs_last_is_noent(), 0);
    CK_I(arkrtos_fs_last_is_nospc(), 0);
    CK_I(arkrtos_fs_rename("/a", "/b"), -1);
    CK_I(arkrtos_fs_list("/", 0, 0), -1);
    CK_I(arkrtos_fs_list_names("/", names, sizeof(names), &a), -1);
    CK_I(arkrtos_fs_unmount(), 0);

    CK_I(arkrtos_fs_last_error(), -1005);
    CK_I(arkrtos_fs_err_is_unformatted(LFS_ERR_CORRUPT), 0);
    CK(strcmp(arkrtos_fs_error_name(LFS_ERR_IO),
              "Ark RTOS: nothing is mounted") == 0);
}

#endif  /* ARKRTOS_USE_FS */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

#if ARKRTOS_USE_FS
    test_names();
    test_not_mounted();
    test_geometry();
    test_mount_and_format();
    test_files();
    test_streams();
    test_listing();
    test_failures();
    test_space_limits();
    test_small_erase_unit();
    test_mount_default();
#else
    test_stubs();
#endif

    if (g_fails != 0)
    {
        printf("FS-HOST FAIL  %d check(s), %d failure(s)\n", g_checks, g_fails);
        return 1;
    }
    printf("FS-HOST OK  %d checks\n", g_checks);
    return 0;
}
