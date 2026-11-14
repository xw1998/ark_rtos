/**
* @file arkrtos_mini.c
* @brief Ark RTOS 小程序（MiniApp）：从文件系统按需 load 进 RAM 执行
* @details 装载全程只有六步，每一步都在处理「RAM 是借来的、随时要还」这件事：
*            1. 读 256 字节镜像头 → 只校字段；
*            2. 读重定位表（<= 4KB）→ 再校一次表体；
*            3. 向 RAM 池借两块 pow2 的块：代码块 + （RW/ZI + 栈）块；
*            4. 负载「边读边落 RAM + 边打补丁」，同一份字节只过一遍
*               （头与表不落 RAM，也没有第二份中转缓冲）；
*            5. CRC32 复核（只算标称形态，与打包工具算的一致）；
*            6. 注册任务 → 回填 TCB 的代码/数据窗口为这两块 → 建 MPU 上下文 → 起调度。
*          退出/停止时把两块还回池：下一次运行重新从文件系统读一遍。
*          两块而不是一块的理由见 arkrtos_mini.h：池的分配粒度是 2 的幂，把代码与
*          RAM 塞进同一个 pow2 块会让「代码远小于 RAM」的常见情形接近翻倍。
*
*          **同一时刻可以有多个**（数量编译期 ARKRTOS_MINI_MAX，运行期只能收紧）：
*          实例表 g_mini[] 一项一个小程序，每项自己一组块（借还都带 ptable 下标，
*          互不共用）。但**装载是串行的**——由 g_mini_busy 这道全局闸门保证同一时刻
*          只有一次装载/拆除在跑，所以「多个」是同时**运行**多个，不是同时装载多个。
*          文件系统只有一份缓存，所以**单次读**（arkrtos_fs_read_at）必须在调度器锁内
*          完成、不能被打断；但两次读之间没有需要保护的中间状态，故装载循环每读满
*          ARKRTOS_MINI_YIELD_CHUNKS 个分块就解锁一次再重新加锁，让控制回路等更高优先级
*          的任务在装载途中也能拿到 CPU（详见该开关的注释）。
*
*          崩溃禁用：与带槽位的 App 用**同一套**阈值（分区表 cfg_restart_max），
*          不新造数字。小程序没有槽位，身份改用**镜像路径的 FNV-1a 哈希**，
*          记录落在 arkrtos_crash.c 的日记里（跨复位存活）。到上限后 arkrtos_mini_run
*          直接回 ARKRTOS_MINI_ERR_DISABLED，直到 `mini forget <路径>` 放行或换镜像。
*
*          校验与重定位不在此另写一份：字段规则（arkrtos_loader_check_header）、
*          MOVW/MOVT 的编码细节（arkrtos_loader_reloc_apply）、头部 CRC 的算法
*          （arkrtos_loader_crc_header）都只有 loader 那一份实现。两条装载路径
*          （串口→池、文件系统→RAM）解析的是同一种容器，判定必须一致。
*
* @author xw
* @date 2026.09.23
*/

#include "arkrtos_mini.h"
#include "arkrtos_config.h"
#include "arkrtos_partition.h"    /* 内核专属：APP_TASK_* / SLOT_RAM_* 的唯一地址源头 */
#include "arkrtos_app_image.h"
#include "arkrtos_loader.h"
#include "arkrtos_ptable.h"
#include "arkrtos_fs.h"
#if (ARKRTOS_MINI_READ_VFS == 1)
#include "arkrtos_vfs.h"
#endif
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"
#include "arkrtos_mpu.h"
#include "arkrtos_crash.h"
#include "arkrtos_log.h"
#include "arkrtos_spin.h"

#if (ARKRTOS_USE_MINIAPP == 1)

/* 负载一次读进来的字节数：与文件系统的单文件读缓存（ARKRTOS_FS_CACHE_SIZE）
 * 同宽，每段恰好喂满一次小缓存，不制造第二个中转缓冲。 */
#define ARKRTOS_MINI_CHUNK        (ARKRTOS_FS_CACHE_SIZE)

/* 装载循环每读多少个分块让出一次调度器（解锁→重新加锁）：默认值与完整缘由
 * 见 arkrtos_features.h 的 ARKRTOS_MINI_YIELD_CHUNKS（0 = 关闭）。这里只做一次
 * 存在性检查——若哪天包含顺序变了，宁可编不过，也不要让未定义悄悄等于 0
 * 把让出静默关掉。 */
#ifndef ARKRTOS_MINI_YIELD_CHUNKS
#error "ARKRTOS_MINI_YIELD_CHUNKS must be defined by arkrtos_features.h"
#endif

/* 小程序任务的优先级：默认沿用 App 的那一个（APP_TASK_PRIORITY），因此不定义
 * 这个宏时行为与体积逐字节不变。留一个覆盖口的理由是实测教训，与 M1 的
 * apprun 同一条：小程序是「现读现跑、最容易死循环」的形态，一旦它死循环且
 * 优先级在控制台之上，控制台会被永久饿死——实测第二次 `mini run` 连回显都
 * 没有，只能复位。把优先级降到控制台之下，就能在「它占着块不放」的同时继续
 * 用控制台（判据 B「同一镜像两个落点」正是靠这个跑出来的）。
 * 取值与出处见 docs/arkc板上直跑-M2可重定位镜像.md §5.3。 */
#ifndef ARKRTOS_MINI_TASK_PRIORITY
#define ARKRTOS_MINI_TASK_PRIORITY   APP_TASK_PRIORITY
#endif

/* 实例状态。LOAD 有两层含义，都归在「已占项但不在跑」里：
 *   - 正在装载（已占项，尚未起任务）；
 *   - 正在拆除（停止/回收中，任务已被摘除、块待归还）。
 * 两者都不出现在 arkrtos_mini_running() / info_at() / stop_at() 的「在跑」集合里，
 * 但都占着一个并发名额，所以别的装载不会挑中它。 */
#define ARKRTOS_MINI_ST_FREE      (0u)
#define ARKRTOS_MINI_ST_LOAD      (1u)
#define ARKRTOS_MINI_ST_RUN       (2u)

/** @brief 一个在跑（或在装载/拆除）的小程序 */
typedef struct {
    uint32 state;                       /* ARKRTOS_MINI_ST_x */
    uint32 pt_index;                    /* 共享表记账下标（state != FREE 时有效） */
    uint32 ram_block;                   /* RAM 块字节数（含栈）—— 故障归属的任务窗口匹配 */
    char   path[ARKRTOS_MINI_PATH_MAX];   /* 镜像路径：身份（哈希）与展示都用它 */
    arkrtos_mini_info_t info;             /* 对外的状态快照（state == RUN 时有效） */
} arkrtos_mini_slot_t;

static arkrtos_mini_slot_t g_mini[ARKRTOS_MINI_MAX];
static uint32 g_mini_busy;              /* 1 = 有一次装载/拆除在进行（装载串行） */
static uint32 g_mini_max_count = ARKRTOS_MINI_MAX;
static arkrtos_spinlock_t g_mini_lock = ARKRTOS_SPINLOCK_INIT;

/* 重定位表：ABI 的上限就是 ARKRTOS_APP_RELOC_MAX 条（4KB），一次读完再校验，
 * 不做滑动窗口。窗口能省的静态内存只有几 KB，却要让「一次 reloc_apply 会碰到
 * 多少条目」这类边界推理跟着负载分段走——省下的内存远小于推理出错的代价。
 * 这两块缓冲是**装载期**的（装载串行共用一份），与小程序的运行期占用无关。 */
static uint32 g_mini_reloc[ARKRTOS_APP_RELOC_MAX];

/* 镜像头：要当结构体用就必须 4 字节对齐，所以按 uint32 数组开 */
static uint32 g_mini_hdr[ARKRTOS_APP_HEADER_SIZE / 4u];

/* 单个小程序的体积上限（块的字节数）。默认取编译期 SLOT_RAM_MAX_BLOCK，运行期
 * 可以在 [SLOT_RAM_MIN_BLOCK, SLOT_RAM_MAX_BLOCK] 之间收紧（shell: mini limit）。
 * 只能收紧不能放宽的理由写在 arkrtos_mini.h 的 arkrtos_mini_max_bytes() 上：
 * SLOT_RAM_MAX_BLOCK 同时是池单块上限与 MPU 数据窗口上限。 */
static uint32 g_mini_max_block = SLOT_RAM_MAX_BLOCK;

/* 自启清单的两个缓冲：一份装「读到的原文」，一份装「重建后的新文本」。
 * 各 1KB，只在控制台/挂载这类非装载路径上用，不与装载期的 g_mini_hdr /
 * g_mini_reloc 混用（那两块在装载串行闸门内，语义是「正在装的这一份」）。 */
static uint8  g_mini_auto_in[ARKRTOS_MINI_AUTOSTART_MAX];
static uint8  g_mini_auto_out[ARKRTOS_MINI_AUTOSTART_MAX];

/* 本次开机是否已经做过自启。先置位再干活：启动过程中若有东西又把卷挂一遍，
 * 不会递归回来再启动一轮。 */
static uint32 g_mini_auto_boot_done;

/* 不小于 v 的最小 2 的幂。只在本模块用，所以不去动公共头。 */
static uint32 arkrtos_mini_pow2_ceil(uint32 v)
{
    uint32 r = 1u;

    while(r < v)
    {
        r <<= 1u;
    }

    return r;
}

/* 清空一个实例。不用 memset，避免为一个结构体再引一层依赖。 */
static void arkrtos_mini_slot_clear(arkrtos_mini_slot_t *s)
{
    uint32 i;

    s->state     = ARKRTOS_MINI_ST_FREE;
    s->pt_index  = 0u;
    s->ram_block = 0u;

    for(i = 0u; i < (uint32)ARKRTOS_MINI_PATH_MAX; i++)
    {
        s->path[i] = '\0';
    }

    s->info.task_id    = 0u;
    s->info.code_base  = 0u;
    s->info.code_block = 0u;
    s->info.code_size  = 0u;
    s->info.ram_base   = 0u;
    s->info.ram_size   = 0u;
    s->info.entry      = 0u;
    s->info.crc        = 0u;
}

/* 把 path 抄进实例（长度已在 arkrtos_mini_run 入口限过） */
static void arkrtos_mini_slot_set_path(arkrtos_mini_slot_t *s, const char *path)
{
    uint32 i;

    for(i = 0u; (i + 1u) < (uint32)ARKRTOS_MINI_PATH_MAX; i++)
    {
        s->path[i] = path[i];
        if(path[i] == '\0')
        {
            return;
        }
    }
    s->path[ARKRTOS_MINI_PATH_MAX - 1u] = '\0';
}

/* 取第 index 个「在跑」的实例（密集编号，跳过空项 / 装载中 / 拆除中）。
 * 调用者必须持 g_mini_lock。 */
static arkrtos_mini_slot_t *arkrtos_mini_nth_running(uint32 index)
{
    uint32 seen = 0u;
    uint32 i;

    for(i = 0u; i < (uint32)ARKRTOS_MINI_MAX; i++)
    {
        if(g_mini[i].state == ARKRTOS_MINI_ST_RUN)
        {
            if(seen == index)
            {
                return &g_mini[i];
            }
            seen++;
        }
    }

    return 0;
}

/* 从镜像文件里精确读 len 字节。少读一个字节就算失败：短文件就是短文件，
 * 不能靠块里的残留内容把一次装载"补全"。 */
#if (ARKRTOS_MINI_READ_VFS == 1)
/* 最近一次读镜像的失败原因；0 表示没失败。见 arkrtos_mini.h。
 * 只在走 VFS 那条路上有意义：那条路的 arkrtos_fs_last_error() 恒为 0，
 * 照它报错就是「失败 + ok」这种错答案。默认关，默认构建逐字节不变。 */
static int32 g_mini_read_err = 0;

int32 arkrtos_mini_last_read_error(void)
{
    return g_mini_read_err;
}
#endif

static int32 arkrtos_mini_read_exact(const char *path, uint8 *buf, uint32 len, uint32 off)
{
    uint32 got = 0u;
    int32  rc;

#if (ARKRTOS_MINI_READ_VFS == 1)
    g_mini_read_err = 0;
#endif
    if(len == 0u)
    {
        return 0;
    }

#if (ARKRTOS_MINI_READ_VFS == 1)
    rc = arkrtos_vfs_read_at(path, buf, len, off, &got);
#else
    rc = arkrtos_fs_read_at(path, buf, len, off, &got);
#endif
    if(rc != 0)
    {
#if (ARKRTOS_MINI_READ_VFS == 1)
        g_mini_read_err = rc;
#endif
        return ARKRTOS_MINI_ERR_READ;
    }
    if(got != len)
    {
        /* 文件比声明的短：不是读错误，但半份镜像同样不能跑，单独记一个码，
         * 免得 shell 把它报成「底层读失败」而把人引去查文件系统。 */
#if (ARKRTOS_MINI_READ_VFS == 1)
        g_mini_read_err = ARKRTOS_MINI_ERR_SHORT_READ;
#endif
        return ARKRTOS_MINI_ERR_READ;
    }

    return 0;
}

/* 一次「只装载、不起任务」的结果：一对新块 + 打完补丁的入口 + CRC。
 * arkrtos_mini_run() 与 arkrtos_mini_swap_at() 共用这一份装载实现——两者
 * 的差别只在装载完成之后做什么（起任务 / 就地热切换）。装载过程各写一份，
 * 迟早有一条会漏掉一处校验。 */
typedef struct {
    uint32 pt_index;      /* 借据：归还交给 arkrtos_ptable_mini_free */
    uint32 code_base;
    uint32 code_block;
    uint32 code_size;
    uint32 ram_base;
    uint32 ram_block;
    uint32 ram_size;      /* 镜像头声明的 RW/ZI 需求（不是池上取整后的块大小） */
    uint32 entry;
    uint32 crc;
} arkrtos_mini_image_t;

/* 读镜像 -> 借两块 -> 打补丁 -> 校 CRC，**不注册任务**。
 * 成功时 p_img 有效且两块已借到（归还由调用者负责）；失败时不留下任何借出的块。
 * 进入时调用者必须已经占好实例项、并置了 g_mini_busy。 */
static int32 arkrtos_mini_load_image(const char *path, uint32 max_block,
                                     arkrtos_mini_image_t *p_img)
{
    arkrtos_app_header_t *p_hdr = (arkrtos_app_header_t *)(void *)g_mini_hdr;
    uint32 code_size;
    uint32 code_span;
    uint32 ram_size_hdr;
    uint32 code_base = 0u;
    uint32 code_block = 0u;
    uint32 code_want;
    uint32 ram_base = 0u;
    uint32 ram_block = 0u;
    uint32 pt_index = 0u;
    uint32 entry;
    uint32 total;
    uint32 cur;
    uint32 crc_done;
    uint32 idx = 0u;
    uint32 crc = 0u;
    uint32 delta_rom;
    uint32 delta_ram;
#if (ARKRTOS_MINI_YIELD_CHUNKS > 0)
    uint32 yield_credits = 0u;      /* 装载循环里的分块计数 */
#endif
    int32  ret;

#if (ARKRTOS_MINI_READ_VFS == 1)
    if(arkrtos_vfs_ready() == 0u)
    {
        return ARKRTOS_MINI_ERR_FS;
    }
#else
    if(arkrtos_fs_mounted() == 0u)
    {
        return ARKRTOS_MINI_ERR_FS;
    }
#endif

    /* 文件系统只有一份缓存，读镜像的整个过程不能与别的任务的读写交错：
     * 与 SVC 层访问文件系统用同一套约定（调度器锁）。 */
    arkrtos_sched_lock_internal();

    if(arkrtos_mini_read_exact(path, (uint8 *)(void *)g_mini_hdr,
                             ARKRTOS_APP_HEADER_SIZE, 0u) != 0)
    {
        ret = ARKRTOS_MINI_ERR_FS;
        goto unlock_out;
    }

    /* 先只校字段：此刻重定位表还没读进来，p_rel 传 0。
     * 掩码传 MASK_MINI：池安装器拒收的 type，小程序路径也只认小程序，两个
     * 方向都不放宽——让"能装进池"与"能当小程序跑"是同一套类型判定。 */
    ret = arkrtos_loader_check_header(p_hdr, 0, ARKRTOS_APP_TYPE_MASK_MINI);
    if(ret != 0)
    {
        goto unlock_out;
    }

    if(arkrtos_mini_read_exact(path, (uint8 *)(void *)g_mini_reloc,
                             p_hdr->reloc_count * ARKRTOS_APP_RELOC_SIZE,
                             ARKRTOS_APP_RELOC_OFFSET) != 0)
    {
        ret = ARKRTOS_MINI_ERR_FS;
        goto unlock_out;
    }

    /* 表体到手再校一次：字段自洽不等于表合法。越界/乱序/未对齐的表会让
     * 后面的打补丁静默漏改，必须在这里挡掉，不能带着它去执行。 */
    ret = arkrtos_loader_check_header(p_hdr, g_mini_reloc, ARKRTOS_APP_TYPE_MASK_MINI);
    if(ret != 0)
    {
        goto unlock_out;
    }

    code_size    = p_hdr->image_size;
    code_span    = (code_size + 7u) & ~7u;      /* 负载按 8 字节上取整 */
    ram_size_hdr = p_hdr->ram_size;
    code_want    = arkrtos_mini_pow2_ceil(code_span);

    /* 回绕自证：image_size 与 ram_size 各自都在 check_header 里被限过，按说不该
     * 回绕；但「不该」不是证据。pow2_ceil 的结果若比负载还小，只可能是加法回绕
     * ——那时块会小于负载，后面的分段读取会写到块外。宁可在这里报 SIZE。 */
    if(code_want < code_span)
    {
        ret = ARKRTOS_LOADER_ERR_SIZE;
        goto unlock_out;
    }

    /* 代码块与 RAM 块各自取 2 的幂（池的分配粒度），各自超过上限就明确报错：
     * 不截断、不缩 RAM 区——悄悄缩掉的那块 RAM 正是小程序自己的栈；把代码块
     * 截到负载以下则会让分段读取写到块外。上限是运行期可配的
     * （默认 = 编译期 SLOT_RAM_MAX_BLOCK，见 g_mini_max_block）。 */
    if((code_want > max_block) || (ram_size_hdr > max_block) ||
       (ram_size_hdr < APP_TASK_STACK_SIZE))
    {
        ret = ARKRTOS_LOADER_ERR_SIZE;
        goto unlock_out;
    }

    /* 两块一起借（同一把锁、同生同死）：借代码块、再借 RAM 块，任一步失败都
     * 不做「半借」——归还路径只认一种状态，少一种状态就少一类现场查不出来的错。
     * 归还凭据是 ptable 交回来的下标，多实例下每项各记一组。 */
    if(arkrtos_ptable_mini_alloc(code_span, ram_size_hdr, &pt_index,
                               &code_base, &code_block,
                               &ram_base, &ram_block) != 0)
    {
        ret = ARKRTOS_LOADER_ERR_NOSPACE;
        goto unlock_out;
    }

    /* 增量就是「运行时地址 − 标称链接地址」：负载落在代码块基址，RAM 区就是
     * RAM 块本身。两个 delta 里的 payload_offset 都已抵消——标称链接基址
     * nominal_base 就是负载自己的链接基址（pack_app 写的是 payload_link_base），
     * 而 nominal_ram_base 就是镜像链接 RW/ZI 时用的 RAM 起点。 */
    delta_rom = code_base - p_hdr->nominal_base;
    delta_ram = ram_base - p_hdr->nominal_ram_base;

    crc = arkrtos_loader_crc_header(p_hdr);
    if(p_hdr->reloc_count != 0u)
    {
        crc = arkrtos_crc32((const void *)g_mini_reloc,
                          p_hdr->reloc_count * ARKRTOS_APP_RELOC_SIZE, crc);
    }

    total    = ARKRTOS_APP_TOTAL_LEN(p_hdr);
    cur      = p_hdr->payload_offset;
    crc_done = cur;

    while(cur < total)
    {
        uint32 n = total - cur;
        uint32 submit;
        uint32 next;

        if(n > ARKRTOS_MINI_CHUNK)
        {
            n = ARKRTOS_MINI_CHUNK;
        }

        /* 负载直接读进它在块里的最终位置（镜像偏移 X 的字节 = 块地址
         * code_base + (X - payload_offset)），因此没有中转缓冲。 */
        if(arkrtos_mini_read_exact(path,
                                 (uint8 *)(void *)(code_base + (cur - p_hdr->payload_offset)),
                                 n, cur) != 0)
        {
            ret = ARKRTOS_MINI_ERR_FS;
            goto free_out;
        }

        /* CRC 只算「标称形态」（打包工具算的那个）：一段字节刚读进来、还没打
         * 补丁时算一次。段尾被切开时下一轮会回退重读几个字节，那部分已经算过，
         * 不能重复计入，否则 CRC 必然对不上。 */
        if((cur + n) > crc_done)
        {
            uint32 from = (cur > crc_done) ? cur : crc_done;

            crc = arkrtos_crc32((const void *)(code_base + (from - p_hdr->payload_offset)),
                              (cur + n) - from, crc);
            crc_done = cur + n;
        }

        /* 补丁缓冲的基点要满足「镜像偏移 X 的字节在 buf[X - buf_off]」 */
        submit = arkrtos_loader_reloc_apply((uint8 *)(void *)(code_base + (cur - p_hdr->payload_offset)),
                                          cur, n, p_hdr, g_mini_reloc, &idx,
                                          delta_rom, delta_ram);

        next = (submit < (cur + n)) ? submit : (cur + n);

        if(next <= cur)
        {
            /* 表体校验保证每条表项完整落在负载内，正常走不到这里。真到了就报错：
             * 带着"少打了一次补丁"的镜像去执行，比装载失败危险得多。 */
            ret = ARKRTOS_LOADER_ERR_RELOC;
            goto free_out;
        }

        /* 段尾切开了一条 8 字节指令类表项：回退到该表项起点重读一次（<= 8 字节），
         * 让它在这一轮里完整地被打上补丁。块里尾部那几个字节此刻还是标称形态，
         * 重读覆盖它没有副作用。 */
        cur = next;

#if (ARKRTOS_MINI_YIELD_CHUNKS > 0)
        /* 分块之间没有要保护的中间状态：单次 read_at 已经原子完成，补丁只写
         * 自己借来的代码块。所以可以安全地放开调度器一小会儿：解锁会立即触发
         * 一次任务切换（arkrtos_sched_unlock_internal），让控制回路这类更高
         * 优先级的任务跑一轮，装载停顿因此被切成分块大小的小片。下一次 read_at
         * 的原子性由本轮末尾的重新加锁保证。 */
        yield_credits++;
        if(yield_credits >= (uint32)ARKRTOS_MINI_YIELD_CHUNKS)
        {
            yield_credits = 0u;
            (void)arkrtos_sched_unlock_internal();
            arkrtos_sched_lock_internal();
        }
#endif
    }

    if(crc != p_hdr->crc32)
    {
        ret = ARKRTOS_LOADER_ERR_CRC;
        goto free_out;
    }

    entry = code_base + p_hdr->entry_offset;    /* 入口偏移是相对「负载起始」的 */
    #if (ARKRTOS_ARCH_IS_ARM == 1)
    entry |= 1u;                            /* Cortex-M：Thumb 指令集标志位 */
    #endif

    /* 调度器锁必须在返回之前放开：调用者随后可能起任务 / 挂热切换，
     * 那两步都不该在锁里做。 */
    arkrtos_sched_unlock_internal();

    p_img->pt_index   = pt_index;
    p_img->code_base  = code_base;
    p_img->code_block = code_block;
    p_img->code_size  = code_size;
    p_img->ram_base   = ram_base;
    p_img->ram_block  = ram_block;
    p_img->ram_size   = ram_size_hdr;
    p_img->entry      = entry;
    p_img->crc        = crc;
    return 0;

free_out:
    (void)arkrtos_ptable_mini_free(pt_index);     /* 两块还在我们手里，原样还回去 */
unlock_out:
    arkrtos_sched_unlock_internal();
    return ret;
}

int32 arkrtos_mini_run(const char *path)
{
    arkrtos_mini_slot_t *slot = 0;
    arkrtos_mini_image_t img;
    uint32 irq;
    uint32 path_len = 0u;
    uint32 hash;
    uint32 in_use = 0u;
    uint32 max_block = 0u;
    uint32 i;
    int32  free_i = -1;
    int32  ret;
    int32  task_id;

    if(path == 0)
    {
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    /* 路径长度先量出来：实例表按 ARKRTOS_MINI_PATH_MAX 存路径，超了就不能
     * 悄悄截断——截断会让两个不同的路径撞成同一个身份。 */
    while((path_len < (uint32)ARKRTOS_MINI_PATH_MAX) && (path[path_len] != '\0'))
    {
        path_len++;
    }
    if((path_len == 0u) || (path_len >= (uint32)ARKRTOS_MINI_PATH_MAX))
    {
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    hash = arkrtos_crash_hash_path(path);

    /* 先占位：装载期间项就要被占住，而不是等文件读完才发现没名额——那时
     * 两块 RAM 已经借出去了。同时把「装载串行」和「并发数上限」一次判掉。 */
    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);

    if(arkrtos_crash_mini_disabled(hash) != 0u)
    {
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
        return ARKRTOS_MINI_ERR_DISABLED;     /* 连续故障到上限，已被禁用 */
    }

    if(g_mini_busy != 0u)
    {
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
        return ARKRTOS_LOADER_ERR_BUSY;       /* 另一次装载/拆除还在进行 */
    }

    for(i = 0u; i < (uint32)ARKRTOS_MINI_MAX; i++)
    {
        if(g_mini[i].state != ARKRTOS_MINI_ST_FREE)
        {
            in_use++;
        }
        else if(free_i < 0)
        {
            free_i = (int32)i;
        }
    }

    /* 没有空项，或已经到运行期并发上限：都是 BUSY，不挤掉任何一个在跑的。 */
    if((free_i < 0) || (in_use >= g_mini_max_count))
    {
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
        return ARKRTOS_LOADER_ERR_BUSY;
    }

    slot = &g_mini[free_i];
    arkrtos_mini_slot_clear(slot);
    slot->state = ARKRTOS_MINI_ST_LOAD;
    arkrtos_mini_slot_set_path(slot, path);
    max_block  = g_mini_max_block;          /* 本次装载用哪条上限，此刻定下来 */
    g_mini_busy = 1u;

    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    /* 读镜像 + 借两块 + 打补丁 + 校 CRC：与热切换共用同一份实现。
     * 调度器锁在它内部放开，此处已经不在锁里。 */
    ret = arkrtos_mini_load_image(path, max_block, &img);
    if(ret != 0)
    {
        goto out;
    }

    task_id = arkrtos_task_register((void (*)(void))img.entry,
                                  (uint32 *)(void *)(img.ram_base + img.ram_block - APP_TASK_STACK_SIZE),
                                  (uint32)APP_TASK_STACK_SIZE,
                                  (uint8)ARKRTOS_MINI_TASK_PRIORITY,
                                  (uint32)APP_TASK_PERIOD_MS);
    if(task_id <= 0)
    {
        (void)arkrtos_ptable_mini_free(img.pt_index);     /* 任务表满了：两块还在我们手里 */
        ret = ARKRTOS_LOADER_ERR_TASK;
        goto out;
    }

    /* TCB 的区域描述就是这两块：rom_* = 代码块（在池里 -> 代码窗口用可执行的
     * RAMX 属性），ram_* = RAM 块（RW/ZI + 栈，走常规数据窗口，不可执行）。
     * 两块基址不同、大小不同，因此 region0 / region1 是两个独立的精确窗口，
     * 不需要「两个窗口必须同属性」那种妥协（见 arkrtos_mpu.c）。 */
    arkrtos_task_table[task_id - 1].rom_start = img.code_base;
    arkrtos_task_table[task_id - 1].rom_size  = img.code_block;
    arkrtos_task_table[task_id - 1].ram_start = img.ram_base;
    arkrtos_task_table[task_id - 1].ram_size  = img.ram_block;
    arkrtos_task_table[task_id - 1].is_priv   = 0u;   /* 与 App 同级：非特权 */

    arkrtos_mpu_build_task(&arkrtos_task_table[task_id - 1]);

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    slot->state     = ARKRTOS_MINI_ST_RUN;
    slot->pt_index  = img.pt_index;
    slot->ram_block = img.ram_block;
    slot->info.task_id    = (uint32)task_id;
    slot->info.code_base  = img.code_base;
    slot->info.code_block = img.code_block;
    slot->info.code_size  = img.code_size;
    slot->info.ram_base   = img.ram_base;
    slot->info.ram_size   = img.ram_size;
    slot->info.entry      = img.entry;
    slot->info.crc        = img.crc;
    g_mini_busy = 0u;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    ARKRTOS_LOGI("MINI", "%s: %u B payload -> code 0x%08X (%u B block), RW/stack 0x%08X (%u B), task %d",
               path, (unsigned)img.code_size, (unsigned)img.code_base, (unsigned)img.code_block,
               (unsigned)img.ram_base, (unsigned)img.ram_size, (int)task_id);

    arkrtos_sched_activate_higher((uint8)ARKRTOS_MINI_TASK_PRIORITY);
    return 0;

out:
    {
        uint32 irq2;

        arkrtos_spin_lock_irqsave(&g_mini_lock, &irq2);
        if(slot != 0)
        {
            arkrtos_mini_slot_clear(slot);
        }
        g_mini_busy = 0u;
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq2);
    }
    return ret;
}

#if (ARKRTOS_USE_TASK_SWAP == 1)
/* ============================================================
 * 小程序的无缝升级：A/B 两块 RAM 就地换
 *
 * 用户问的是「再加一级缓冲很浪费内存」，所以这里不做第三份：
 *   - 旧的一对块（代码 + RAM）在跑；
 *   - 新的一对块是**第二个版本**的落点，装载完就挂上切换；
 *   - 切换在目标任务的**下一次系统调用**上落地（内核拿走那一帧），落地那一刻
 *     旧的一对块立刻还池。
 * 峰值就是两套，不会再出现「旧 + 新 + 中转」三套。
 *
 * 内核那一侧只认 task_id 与两块地址，不认实例表：旧块什么时候还、实例表怎么
 * 改是这里的事。因此这个文件里只有「谁在换」这本账。
 * ============================================================ */

typedef struct {
    uint32 active;                      /* 1 = 有一次切换在途 */
    uint32 slot_i;                      /* g_mini[] 下标 */
    char   path[ARKRTOS_MINI_PATH_MAX]; /* 新镜像的路径（成功后成为实例身份） */
    arkrtos_mini_image_t img;           /* 新的一对块（已装载、未起任务） */
} arkrtos_mini_swap_t;

static arkrtos_mini_swap_t g_mini_swap;

/* 撤掉在途的那一份，返回要归还的借据（0 = 本来就没有在途的）。
 * 必须在持 g_mini_lock 时调用；真正的归还（会进 ptable 自己的锁）留在锁外做。 */
static uint32 arkrtos_mini_swap_drop(void)
{
    uint32 pt;

    if(g_mini_swap.active == 0u)
    {
        return 0u;
    }

    pt = g_mini_swap.img.pt_index;
    g_mini_swap.active = 0u;
    g_mini_swap.img.pt_index = 0u;
    return pt;
}

int32 arkrtos_mini_swap_at(uint32 index, const char *path)
{
    arkrtos_mini_slot_t *slot;
    uint32 irq;
    uint32 path_len = 0u;
    uint32 hash;
    uint32 max_block;
    uint32 drop_pt = 0u;
    int32  ret;
    uint32 old_ram_block;
    uint32 old_ram_size;
    uint32 old_code_block;
    int32  task_id;

    if(path == 0)
    {
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    while((path_len < (uint32)ARKRTOS_MINI_PATH_MAX) && (path[path_len] != '\0'))
    {
        path_len++;
    }
    if((path_len == 0u) || (path_len >= (uint32)ARKRTOS_MINI_PATH_MAX))
    {
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    hash = arkrtos_crash_hash_path(path);

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);

    if(arkrtos_crash_mini_disabled(hash) != 0u)
    {
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
        return ARKRTOS_MINI_ERR_DISABLED;
    }

    /* 三条互斥：装载串行、本模块只允许一份在途、内核那边也只允许一份在途。
     * 第三条不能省——内核只有一个挂起槽，两个模块同时挂上去会互相覆盖。 */
    if((g_mini_busy != 0u) || (g_mini_swap.active != 0u) ||
       (arkrtos_task_swap_pending() != 0))
    {
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
        return ARKRTOS_LOADER_ERR_BUSY;
    }

    slot = arkrtos_mini_nth_running(index);
    if(slot == 0)
    {
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
        return ARKRTOS_LOADER_ERR_STATE;
    }

    /* 旧版本的四样特征先拍下来：装载会放开锁（读文件），回读时它可能已经变了。 */
    task_id       = (int32)slot->info.task_id;
    old_ram_block = slot->ram_block;
    old_ram_size  = slot->info.ram_size;
    old_code_block = slot->info.code_block;

    g_mini_busy = 1u;
    g_mini_swap.active = 1u;
    g_mini_swap.slot_i = (uint32)(slot - g_mini);
    g_mini_swap.img.pt_index = 0u;
    {
        uint32 k;

        for(k = 0u; (k + 1u) < (uint32)ARKRTOS_MINI_PATH_MAX; k++)
        {
            g_mini_swap.path[k] = path[k];
            if(path[k] == '\0')
            {
                break;
            }
        }
        g_mini_swap.path[ARKRTOS_MINI_PATH_MAX - 1u] = '\0';
    }
    max_block = g_mini_max_block;

    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    ret = arkrtos_mini_load_image(path, max_block, &g_mini_swap.img);
    if(ret != 0)
    {
        goto load_fail;
    }

    /* 布局必须一致：整块搬运的前提。四样都得对上——代码块大小、RAM 块大小、
     * 镜像头声明的 RAM 需求，以及目标任务自己的 RAM 块大小。任何一条不等都说
     * 明这两份镜像不是「同一布局的两个版本」，那种升级只能走交接协议，
     * 不能在这里假装无缝。 */
    if((g_mini_swap.img.code_block != old_code_block) ||
       (g_mini_swap.img.ram_block != old_ram_block) ||
       (g_mini_swap.img.ram_size != old_ram_size))
    {
        ret = ARKRTOS_SWAP_ERR_LAYOUT;
        goto refuse;
    }

    ret = arkrtos_task_swap_arm(task_id,
                                g_mini_swap.img.code_base, g_mini_swap.img.code_block,
                                g_mini_swap.img.ram_base, g_mini_swap.img.ram_block,
                                g_mini_swap.img.entry);
    if(ret != 0)
    {
        goto refuse;
    }

    /* 装载完成、切换已挂上：装载闸门放开（别的 `mini run` 可以继续），
     * 「同时只有一份在途」由 g_mini_swap.active 继续守着。 */
    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    g_mini_busy = 0u;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    ARKRTOS_LOGI("MINI", "%s: armed, task %d switches at its next syscall"
               " (code 0x%08X, RW 0x%08X)",
               path, (int)task_id, (unsigned)g_mini_swap.img.code_base,
               (unsigned)g_mini_swap.img.ram_base);
    return 0;

refuse:
    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    drop_pt = arkrtos_mini_swap_drop();
    g_mini_busy = 0u;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
    if(drop_pt != 0u)
    {
        (void)arkrtos_ptable_mini_free(drop_pt);
    }
    return ret;

load_fail:
    /* 装载失败时它自己没留下借出的块，这里只清账。 */
    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    g_mini_swap.active = 0u;
    g_mini_busy = 0u;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
    return ret;
}

/* SVC 入口的落地钩子。返回值：0 = 与本模块无关（或已拒绝并清账）；
 * 1 = 已切换（这一帧已经变成新镜像的入口，本次系统调用不再执行下去）。 */
int32 arkrtos_mini_swap_take(void *p_exc_ctx, int32 task_id)
{
    arkrtos_mini_slot_t *slot;
    uint32 irq;
    uint32 old_pt = 0u;
    int32  rc;

    if((g_mini_swap.active == 0u) || (task_id <= 0) ||
       (g_mini_swap.slot_i >= (uint32)ARKRTOS_MINI_MAX))
    {
        return 0;
    }

    slot = &g_mini[g_mini_swap.slot_i];
    if((int32)slot->info.task_id != task_id)
    {
        return 0;
    }

    /* 内核侧：整块拷到新块 -> 改这一帧 -> 挪 PSP -> 换 TCB/MPU 窗口。 */
    rc = arkrtos_task_swap_take(p_exc_ctx, task_id);

    if(rc == 0)
    {
        /* 这一帧不在旧块里（例如中断上下文发起的调用走 MSP）：等下一次。 */
        return 0;
    }

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);

    if(rc > 0)
    {
        old_pt = slot->pt_index;
        slot->pt_index  = g_mini_swap.img.pt_index;
        slot->ram_block = g_mini_swap.img.ram_block;
        slot->info.code_base  = g_mini_swap.img.code_base;
        slot->info.code_block = g_mini_swap.img.code_block;
        slot->info.code_size  = g_mini_swap.img.code_size;
        slot->info.ram_base   = g_mini_swap.img.ram_base;
        slot->info.ram_size   = g_mini_swap.img.ram_size;
        slot->info.entry      = g_mini_swap.img.entry;
        slot->info.crc        = g_mini_swap.img.crc;
        /* 身份换成新镜像：崩溃记账按路径归属，跑的是谁就记在谁头上。 */
        arkrtos_mini_slot_set_path(slot, g_mini_swap.path);
        g_mini_swap.active = 0u;
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

        /* 旧的一对块在切换完成这一刻归还：帧已在新块、TCB 与 MPU 都指着新块，
         * 此刻没有任何执行流还指着旧块。 */
        (void)arkrtos_ptable_mini_free(old_pt);
        ARKRTOS_LOGI("MINI", "%s: hot swap done (task %d), old blocks returned",
                   g_mini_swap.path, (int)task_id);
        return 1;
    }

    /* 内核拒绝（帧形态不是基本帧 / 布局变了）：新块还回去，实例继续跑旧镜像。 */
    old_pt = arkrtos_mini_swap_drop();
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
    if(old_pt != 0u)
    {
        (void)arkrtos_ptable_mini_free(old_pt);
    }
    ARKRTOS_LOGE("MINI", "%s: hot swap refused (%d), still on the old image",
               g_mini_swap.path, (int)rc);
    return 0;
}
#endif /* ARKRTOS_USE_TASK_SWAP */

/* 停掉一个实例：先把镜像（含它自建的线程）踢出调度，再还块。顺序不能反——
 * 还完块，那块 RAM 可能立刻被下一个装载者拿去写代码，而原任务还在跑。
 * 不持 g_mini_lock（会做 halt_image / ptable_free）。 */
static void arkrtos_mini_teardown(arkrtos_mini_slot_t *slot)
{
    uint32 task_id;

#if (ARKRTOS_USE_TASK_SWAP == 1)
    /* 停的是正在换的那个实例：在途的那份新块要一起撤掉，否则它永远留在池里，
     * 而且内核那侧还挂着一个指向已经不在的任务号的切换。 */
    {
        uint32 irq;
        uint32 drop_pt = 0u;

        arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
        if((g_mini_swap.active != 0u) &&
           (g_mini_swap.slot_i == (uint32)(slot - g_mini)))
        {
            (void)arkrtos_task_swap_cancel(0);
            drop_pt = arkrtos_mini_swap_drop();
        }
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

        if(drop_pt != 0u)
        {
            (void)arkrtos_ptable_mini_free(drop_pt);
        }
    }
#endif

    task_id = slot->info.task_id;

    if(task_id != 0u)
    {
        arkrtos_loader_halt_image(task_id);
    }

    (void)arkrtos_ptable_mini_free(slot->pt_index);
}

int32 arkrtos_mini_stop_at(uint32 index)
{
    uint32 irq;
    arkrtos_mini_slot_t *slot;

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    slot = arkrtos_mini_nth_running(index);
    if(slot == 0)
    {
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
        return ARKRTOS_LOADER_ERR_STATE;
    }
    /* 立刻离开「在跑」集合：并发进来的 stop / run 都不会再选中它 */
    slot->state = ARKRTOS_MINI_ST_LOAD;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    arkrtos_mini_teardown(slot);

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    {
        uint32 task_id = slot->info.task_id;

        arkrtos_mini_slot_clear(slot);
        ARKRTOS_LOGI("MINI", "stopped: task %u, code and RAM blocks returned to the pool",
                   (unsigned)task_id);
    }
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    return 0;
}

int32 arkrtos_mini_stop(void)
{
    int32 stopped = 0;

    /* 逐个停：每次都停「当前第 0 个」，停掉后原来的第 1 个就变成第 0 个。
     * 上限用 ARKRTOS_MINI_MAX 兜底，避免状态异常时在这里空转。 */
    while((arkrtos_mini_running() > 0u) && (stopped < (int32)ARKRTOS_MINI_MAX))
    {
        if(arkrtos_mini_stop_at(0u) != 0)
        {
            break;
        }
        stopped++;
    }

    return (stopped > 0) ? 0 : ARKRTOS_LOADER_ERR_STATE;
}

uint32 arkrtos_mini_running(void)
{
    uint32 irq;
    uint32 n = 0u;
    uint32 i;

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    for(i = 0u; i < (uint32)ARKRTOS_MINI_MAX; i++)
    {
        if(g_mini[i].state == ARKRTOS_MINI_ST_RUN)
        {
            n++;
        }
    }
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    return n;
}

int32 arkrtos_mini_info_at(uint32 index, arkrtos_mini_info_t *p_out)
{
    uint32 irq;
    arkrtos_mini_slot_t *slot;

    if(p_out == 0)
    {
        return ARKRTOS_MINI_ERR_PARAM;
    }

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    slot = arkrtos_mini_nth_running(index);
    if(slot == 0)
    {
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
        return ARKRTOS_MINI_ERR_NOSTATE;
    }
    *p_out = slot->info;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    return 0;
}

int32 arkrtos_mini_info(arkrtos_mini_info_t *p_out)
{
    if(p_out == 0)
    {
        return ARKRTOS_MINI_ERR_PARAM;
    }

    if(arkrtos_mini_info_at(0u, p_out) != 0)
    {
        return ARKRTOS_MINI_ERR_NOSTATE;
    }

    return 0;
}

uint32 arkrtos_mini_max_count(void)
{
    uint32 irq;
    uint32 v;

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    v = g_mini_max_count;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    return v;
}

int32 arkrtos_mini_set_max_count(uint32 n)
{
    uint32 irq;

    /* 超范围一律报 PARAM：不夹取。「设了 8 却只跑 2 个」是看不出来的谎话，
     * 上限本来就是为了「说不清就拒绝」才存在的。 */
    if((n < 1u) || (n > (uint32)ARKRTOS_MINI_MAX))
    {
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    g_mini_max_count = n;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    return 0;
}

uint32 arkrtos_mini_max_bytes(void)
{
    uint32 irq;
    uint32 v;

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    v = g_mini_max_block;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    return v;
}

int32 arkrtos_mini_set_max_bytes(uint32 bytes)
{
    uint32 irq;

    if(bytes == 0u)
    {
        bytes = SLOT_RAM_MAX_BLOCK;         /* 0 = 恢复编译期默认 */
    }

    /* 不是 2 的幂 / 越界一律报 PARAM：不夹取。夹取会让「我设了 64K 却拿到 32K」
     * 变成一句看不出来的谎话——上限本来就是为了「说不清就拒绝」才存在的。 */
    if((bytes < SLOT_RAM_MIN_BLOCK) || (bytes > SLOT_RAM_MAX_BLOCK) ||
       ((bytes & (bytes - 1u)) != 0u))
    {
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    g_mini_max_block = bytes;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    return 0;
}

int32 arkrtos_mini_forget(const char *path)
{
    if((path == 0) || (path[0] == '\0'))
    {
        return ARKRTOS_MINI_ERR_PARAM;
    }

    arkrtos_crash_mini_forget(arkrtos_crash_hash_path(path));

    return 0;
}

int32 arkrtos_mini_task_is_main(uint32 task_id)
{
    uint32 irq;
    uint32 i;
    int32  found = 0;

    if(task_id == 0u)
    {
        return 0;
    }

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    for(i = 0u; i < (uint32)ARKRTOS_MINI_MAX; i++)
    {
        if((g_mini[i].state == ARKRTOS_MINI_ST_RUN) &&
           (g_mini[i].info.task_id == task_id))
        {
            found = 1;
            break;
        }
    }
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    return found;
}

int32 arkrtos_mini_on_task_exit(uint32 task_id)
{
    uint32 irq;
    uint32 i;
    uint32 task_id_saved;
    arkrtos_mini_slot_t *slot = 0;

    if(task_id == 0u)
    {
        return 0;
    }

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    for(i = 0u; i < (uint32)ARKRTOS_MINI_MAX; i++)
    {
        /* 只认主任务：子线程退出不动块——小程序可能只是收掉了一个 worker，
         * 主任务还要继续跑。 */
        if((g_mini[i].state == ARKRTOS_MINI_ST_RUN) && (g_mini[i].info.task_id == task_id))
        {
            slot = &g_mini[i];
            break;
        }
    }
    if(slot == 0)
    {
        /* 不是小程序的主任务（内核任务、App 任务，或小程序内部的子线程） */
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
        return 0;
    }
    task_id_saved = slot->info.task_id;
    slot->state = ARKRTOS_MINI_ST_LOAD;       /* 离开「在跑」集合 */
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    /* 主任务退出 = 整个小程序结束：它自己创建的线程也必须一起停，停完才还块。
     * 此刻调用者自身已被置为 INVALID，因此 halt_image 不会重复收尸它。 */
    arkrtos_mini_teardown(slot);

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    arkrtos_mini_slot_clear(slot);
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    ARKRTOS_LOGI("MINI", "task %u returned: MiniApp finished, code and RAM blocks back in the pool",
               (unsigned)task_id_saved);
    return 1;
}

int32 arkrtos_mini_on_fault(int32 task_id)
{
    uint32 irq;
    uint32 i;
    uint32 hash;
    uint32 disabled;
    uint32 task_id_saved;
    arkrtos_mini_slot_t *slot = 0;

    if((task_id <= 0) || ((uint32)task_id > (uint32)arkrtos_task_count))
    {
        return 0;
    }

    /* 归属判定用 RAM 窗口：小程序的子线程也跑在这块 RAM 里，任何一个线程的
     * 故障都记在整个小程序名下——与 arkrtos_loader_halt_image 停整个窗口是
     * 同一套身份。内核任务的窗口在别处，永不匹配。 */
    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    for(i = 0u; i < (uint32)ARKRTOS_MINI_MAX; i++)
    {
        uint32 rb;
        uint32 re;
        uint32 tb;
        uint32 te;

        if(g_mini[i].state != ARKRTOS_MINI_ST_RUN)
        {
            continue;
        }

        rb = g_mini[i].info.ram_base;
        re = rb + g_mini[i].ram_block;
        tb = arkrtos_task_table[(uint32)task_id - 1u].ram_start;
        te = tb + arkrtos_task_table[(uint32)task_id - 1u].ram_size;

        if((rb != 0u) && (re > rb) && (tb >= rb) && (te <= re))
        {
            slot = &g_mini[i];
            break;
        }
    }
    if(slot == 0)
    {
        arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);
        return 0;                   /* 与本模块无关：调用方按默认路径处理 */
    }

    hash = arkrtos_crash_hash_path(slot->path);
    task_id_saved = slot->info.task_id;
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    /* 与带槽位的 App 同一套阈值（分区表 cfg_restart_max）：计数落在跨复位的
     * 日记里。这一步可能从异常上下文调用，只用临界区，不做文件 I/O。 */
    (void)arkrtos_crash_mini_fault(hash, ARKRTOS_CRASH_REASON_FAULT);

    disabled = arkrtos_crash_mini_disabled(hash);
    if(disabled == 0u)
    {
        return 0;                   /* 未到上限：交给默认故障路径（原地重启） */
    }

    /* 到上限：不再让它跑。停掉整个小程序、归还两块，并记一条禁用。
     * 此后 arkrtos_mini_run 对同一路径（同一哈希）直接回 DISABLED。 */
    ARKRTOS_LOGE("MINI", "disabled: %s, %u consecutive faults (retry with 'mini forget %s')",
               arkrtos_crash_reason_name(disabled),
               (unsigned)arkrtos_crash_mini_count(hash), slot->path);

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    if(slot->state == ARKRTOS_MINI_ST_RUN)
    {
        slot->state = ARKRTOS_MINI_ST_LOAD;       /* 离开「在跑」集合 */
    }
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    arkrtos_mini_teardown(slot);

    /* 这个任务再也不会被调度：立刻撤掉它的 MPU 窗口，免得硬件里留着一份
     * 过期的上下文直到下次任务切换（与带槽位的禁用路径同一处理）。 */
    arkrtos_mpu_reset();

    arkrtos_spin_lock_irqsave(&g_mini_lock, &irq);
    arkrtos_mini_slot_clear(slot);
    arkrtos_spin_unlock_irqrestore(&g_mini_lock, irq);

    (void)task_id_saved;
    return 1;                       /* 已按小程序规则处理：不要恢复它 */
}

/* ============================================================================
 * 开机自启清单（卷内文本文件 ARKRTOS_MINI_AUTOSTART_FILE）
 *
 * 解析只写一份：arkrtos_mini_auto_next() 是「什么算一个有效条目」的唯一判据
 * （行尾分隔、行内去首尾空白、空行跳过、'#' 注释），遍历、比对、重建都从它取
 * 行区间。两份解析迟早会分歧，而分歧的表现是「清单里明明有、却没启起来」。
 * ==========================================================================*/

/* 读清单到 g_mini_auto_in。冷启动时文件不存在是正常状态（= 一条自启都没有），
 * 但那与「存在却读不了」不是同一件事，所以返回值两者都报 -1、由调用方决定怎么措辞。 */
static int32 arkrtos_mini_auto_read(uint32 *out_len)
{
    uint32 len = 0u;

    *out_len      = 0u;
    g_mini_auto_in[0] = '\0';

    if(arkrtos_fs_read_file(ARKRTOS_MINI_AUTOSTART_FILE, g_mini_auto_in,
                          (uint32)ARKRTOS_MINI_AUTOSTART_MAX - 1u, &len) != 0)
    {
        return ARKRTOS_MINI_ERR_READ;
    }

    g_mini_auto_in[len] = '\0';
    *out_len = len;

    return 0;
}

/* 从「读到的原文」里取下一条有效条目。cursor 是字节游标（调用方从 0 开始）。
 * 返回 1=取到（[s,e) 是去空白后的区间），0=没有更多。 */
static uint32 arkrtos_mini_auto_next(uint32 *cursor, uint32 len, uint32 *s, uint32 *e)
{
    uint32 i = *cursor;

    while(i < len)
    {
        uint32 a = i;
        uint32 b;

        while((i < len) && (g_mini_auto_in[i] != '\n'))
        {
            i++;                        /* a..i 是一整行 */
        }
        b = i;
        if(i < len)
        {
            i++;                        /* 吃掉 '\n' */
        }
        *cursor = i;

        /* 行内去首尾空白。'\r' 也算空白：清单文件可能被 Windows 侧编辑器
         * 存成 CRLF，那样的行尾不该被当成路径的一部分。 */
        while((a < b) && ((g_mini_auto_in[a] == ' ') ||
                          (g_mini_auto_in[a] == '\t') ||
                          (g_mini_auto_in[a] == '\r')))
        {
            a++;
        }
        while((b > a) && ((g_mini_auto_in[b - 1u] == ' ') ||
                          (g_mini_auto_in[b - 1u] == '\t') ||
                          (g_mini_auto_in[b - 1u] == '\r')))
        {
            b--;
        }

        if(b <= a)
        {
            continue;                   /* 空行 */
        }
        if(g_mini_auto_in[a] == '#')
        {
            continue;                   /* 注释 */
        }

        *s = a;
        *e = b;

        return 1u;
    }

    return 0u;
}

/* [s,e) 是否就是 path（逐字节比，两边都要求刚好结束）。 */
static uint32 arkrtos_mini_auto_line_is(uint32 s, uint32 e, const char *path)
{
    uint32 n = e - s;
    uint32 k;

    for(k = 0u; k < n; k++)
    {
        if(path[k] == '\0')
        {
            return 0u;
        }
        if((char)g_mini_auto_in[s + k] != path[k])
        {
            return 0u;
        }
    }

    return (path[n] == '\0') ? 1u : 0u;
}

/* 由遍历统一回调。超长条目在这里被拦下：不截断、也不回调，只报一条错误日志。 */
static int32 arkrtos_mini_auto_walk(arkrtos_mini_auto_cb_t cb, void *arg)
{
    char   path[ARKRTOS_MINI_PATH_MAX];
    uint32 len = 0u;
    uint32 cur = 0u;
    uint32 s   = 0u;
    uint32 e   = 0u;

    if(cb == 0)
    {
        return ARKRTOS_MINI_ERR_PARAM;
    }

    if(arkrtos_mini_auto_read(&len) != 0)
    {
        return ARKRTOS_MINI_ERR_READ;
    }

    while(arkrtos_mini_auto_next(&cur, len, &s, &e) != 0u)
    {
        uint32 n = e - s;
        uint32 k;

        if(n > ((uint32)ARKRTOS_MINI_PATH_MAX - 1u))
        {
            /* 截断会让两个不同路径共享同一个身份（崩溃记账会串号），
             * 静默丢一条会让「清单里写了却没启起来」无从解释。都不做。 */
            ARKRTOS_LOGE("MINI", "%s: entry longer than %u bytes, skipped",
                       ARKRTOS_MINI_AUTOSTART_FILE,
                       (unsigned)(ARKRTOS_MINI_PATH_MAX - 1u));
            continue;
        }

        for(k = 0u; k < n; k++)
        {
            path[k] = (char)g_mini_auto_in[s + k];
        }
        path[n] = '\0';

        if(cb(path, arg) != 0)
        {
            break;
        }
    }

    return 0;
}

int32 arkrtos_mini_autostart_foreach(arkrtos_mini_auto_cb_t cb, void *arg)
{
    return arkrtos_mini_auto_walk(cb, arg);
}

/* 启动一条：判据全部复用 arkrtos_mini_run（并发上限、崩溃禁用、体积上限都不另写），
 * 成功与否逐条报出来——自启是「没人看着的时候发生的事」，日志就是它唯一的交代。 */
static int arkrtos_mini_auto_start_one(const char *path, void *arg)
{
    uint32 *started = (uint32 *)arg;
    int32   rc      = arkrtos_mini_run(path);

    if(rc == 0)
    {
        (*started)++;
        ARKRTOS_LOGI("MINI", "autostart: %s started", path);
    }
    else
    {
        ARKRTOS_LOGE("MINI", "autostart: %s not started, rc=%d", path, (int)rc);
    }

    return 0;
}

int32 arkrtos_mini_autostart_now(void)
{
    uint32 started = 0u;

    if(arkrtos_mini_auto_walk(arkrtos_mini_auto_start_one, &started) != 0)
    {
        return ARKRTOS_MINI_ERR_FS;
    }

    return (int32)started;
}

uint32 arkrtos_mini_boot_autostart(void)
{
    int32 rc;

    if(g_mini_auto_boot_done != 0u)
    {
        return 0u;
    }
    g_mini_auto_boot_done = 1u;         /* 先置位：启动过程中再次挂载不会递归 */

    rc = arkrtos_mini_autostart_now();
    if(rc < 0)
    {
        /* 「没有清单」是启动的常见状态（开机不启动才是默认），用 info 而不是
         * error：它不是故障，只是没配。 */
        ARKRTOS_LOGI("MINI", "no autostart list (%s)", ARKRTOS_MINI_AUTOSTART_FILE);
        return 0u;
    }

    ARKRTOS_LOGI("MINI", "boot autostart: %d started from %s",
               (int)rc, ARKRTOS_MINI_AUTOSTART_FILE);

    return (uint32)rc;
}

int32 arkrtos_mini_autostart_set(const char *path, uint32 on)
{
    uint32 len    = 0u;
    uint32 cur    = 0u;
    uint32 s      = 0u;
    uint32 e      = 0u;
    uint32 o      = 0u;
    uint32 plen   = 0u;
    uint32 listed = 0u;
    uint32 k;

    if((path == 0) || (path[0] != '/'))
    {
        /* 与 `mini run` 必须是同一种写法：带不带 '/' 是两条不同的身份串，
         * 这里自作主张补一个，会让清单里的路径与手工运行的那条哈希不一致。 */
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    while(path[plen] != '\0')
    {
        plen++;
    }
    if(plen > ((uint32)ARKRTOS_MINI_PATH_MAX - 1u))
    {
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    (void)arkrtos_mini_auto_read(&len);   /* 读不到 = 空清单，仍然可以新增 */

    while(arkrtos_mini_auto_next(&cur, len, &s, &e) != 0u)
    {
        if(arkrtos_mini_auto_line_is(s, e, path) != 0u)
        {
            listed = 1u;
        }
    }

    /* 空改动不落盘：本来就在清单里再加、本来就不在再删，都不去动那个文件。 */
    if((listed != 0u) == (on != 0u))
    {
        return 0;
    }

    cur = 0u;
    while(arkrtos_mini_auto_next(&cur, len, &s, &e) != 0u)
    {
        if(arkrtos_mini_auto_line_is(s, e, path) != 0u)
        {
            continue;                   /* 旧行丢掉，后面按需要重新追加 */
        }
        if((o + (e - s) + 1u) >= (uint32)ARKRTOS_MINI_AUTOSTART_MAX)
        {
            return ARKRTOS_LOADER_ERR_NOSPACE;
        }
        for(k = s; k < e; k++)
        {
            g_mini_auto_out[o++] = g_mini_auto_in[k];
        }
        g_mini_auto_out[o++] = '\n';
    }

    if(on != 0u)
    {
        if((o + plen + 1u) >= (uint32)ARKRTOS_MINI_AUTOSTART_MAX)
        {
            return ARKRTOS_LOADER_ERR_NOSPACE;
        }
        for(k = 0u; k < plen; k++)
        {
            g_mini_auto_out[o++] = (uint8)path[k];
        }
        g_mini_auto_out[o++] = '\n';
    }

    if(o == 0u)
    {
        /* 清单空了：删掉文件，而不是留一个 0 字节文件。两者「一条自启都没有」
         * 的语义一样，但一个残留的空文件会让人以为自己配过。 */
        (void)arkrtos_fs_remove(ARKRTOS_MINI_AUTOSTART_FILE);
        return 0;
    }

    if(arkrtos_fs_write_file(ARKRTOS_MINI_AUTOSTART_FILE, g_mini_auto_out, o) != 0)
    {
        return ARKRTOS_MINI_ERR_FS;
    }

    return 0;
}

#endif /* ARKRTOS_USE_MINIAPP */
