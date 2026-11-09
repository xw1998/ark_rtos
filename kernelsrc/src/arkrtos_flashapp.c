/**
* @file arkrtos_flashapp.c
* @brief 阶段 3c：产物烧进片内 Flash 应用区，由内核引导器跳转执行
* @details 与 app run 的差别只有一句话：代码从哪里取指。
*            - app run：文件从 /tmp 读进 8 KB RAM 代码窗，掉电即失；
*            - flash app：产物已经烧在片内 Flash 应用区，掉电不丢，复位后
*              不必重烧。内核只做「校验 → 借数据窗口 → 起任务」三件事。
*
*          产物形状（宿主：cc --scatter flash.sct --vector-table -o x.bin）：
*            text/rodata 链到 Flash 应用区，data/bss/stack 链到 RAM 数据窗口，
*            文件 +0 是向量表（+0 = 初始 SP = 数据窗口顶，+4 = Reset_Handler）。
*          Flash 应用区末尾固定 32 字节是元数据 flash word（魔数/长度/CRC32/版本），
*          见分区头 ARKRTOS_FLASHAPP_META_*。元数据放区尾不放负载头：Cortex-M
*          要求向量表在 +0，任何头都会把 SP 推后，而本路径正是靠 SP 判断
*          「这份镜像是不是给这条地址链链接的」。
*
*          写路径是「先整份验 → 再擦 → 分块写 → 写元数据 → 回读逐字节比对」。
*          先验后擦：一份明显不对的文件不该把已经能跑的那份擦掉。擦除一旦
*          开始就没有回滚——失败就是「区里现在是空的」，这点在 shell 输出里
*          明说，不做「假装失败没发生」的补偿。
*
* @author xw
* @date 2026.11.09
*/

#include "arkrtos_flashapp.h"
#include "arkrtos_config.h"
#include "arkrtos_partition.h"    /* ARKRTOS_FLASHAPP_*：本路径唯一的地址源头 */
#include "arkrtos_loader.h"       /* 错误码：只借数字，不解析镜像 */
#include "arkrtos_cfg.h"          /* arkrtos_task_table（回填 rom/ram 两窗口） */
#include "arkrtos_mini.h"         /* ARKRTOS_MINI_ERR_FS */
#include "arkrtos_ptable.h"
#include "arkrtos_vfs.h"
#include "arkrtos_task.h"
#include "arkrtos_mpu.h"
#include "arkrtos_spin.h"
#include "arkrtos_log.h"
#include "drvflash.h"             /* arkrtos_port_flash_erase/write */

#if (ARKRTOS_USE_FLASHAPP == 1)

/* 分块搬运的临时缓冲：擦写期间不能与别的任务交错（调度器锁 + 单实例闸门），
 * 所以一份静态缓冲就够。32 字节对齐是 H7 flash word 编程的硬要求。 */
#define FLASHAPP_IO_CHUNK   (1024u)

static uint8 g_flashapp_io[FLASHAPP_IO_CHUNK] __attribute__((aligned(32)));

/* 元数据：区尾固定 32 字节，正好一个 flash word。 */
typedef struct
{
    uint32 magic;
    uint32 len;
    uint32 crc;
    uint32 version;
    uint32 rsv[4];
} flashapp_meta_t;

/* 单实例状态：与 apprun 一样三样东西一起读改，共用一把锁。 */
static arkrtos_spinlock_t g_flashapp_lock = ARKRTOS_SPINLOCK_INIT;
static uint32 g_flashapp_task  = 0u;     /* 主任务号；0 = 没在跑；0xFFFFFFFF = 装载中 */
static uint32 g_flashapp_pt    = 0u;     /* 借窗凭据（ptable 下标） */
static uint32 g_flashapp_entry = 0u;     /* 入口地址（已带 Thumb 位），仅供展示 */

/* CRC32（IEEE 802.3，反射多项式 0xEDB88320）。
 * 不用 1 KB 查表：内核 RAM 只剩十来 KB，扫 128 KB 不到 10 ms，这点空间不值。 */
static uint32 flashapp_crc32(uint32 crc, const uint8 *p, uint32 n)
{
    uint32 i;
    uint32 b;
    uint32 k;

    crc = ~crc;
    for(i = 0u; i < n; i++)
    {
        crc ^= (uint32)p[i];
        for(k = 0u; k < 8u; k++)
        {
            b = crc & 1u;
            crc >>= 1;
            if(b != 0u)
            {
                crc ^= 0xEDB88320u;
            }
        }
    }
    return ~crc;
}

/* 从头到尾把文件流一遍：算长度、CRC32、取向量表两项、取入口处的序言半字。
 * 一次读完就够——不在 RAM 里缓存整份（区是 128 KB，缓存等于把 Flash 白扩了）。 */
static int32 flashapp_scan(const char *path, uint32 *p_len, uint32 *p_crc,
                           uint32 *p_sp, uint32 *p_vec, uint16 *p_pro)
{
    int32  fd;
    int32  n;
    uint32 total = 0u;
    uint32 crc   = 0u;
    uint32 ent   = 0u;
    uint32 pro_off = 0xFFFFFFFFu;   /* 入口在负载里的偏移；拿到向量表后才知道 */
    uint8  vec[8];
    uint32 i;

    fd = arkrtos_vfs_open_read(path);
    if(fd < 0)
    {
        return ARKRTOS_MINI_ERR_FS;
    }

    for(;;)
    {
        uint32 base = total;

        n = arkrtos_vfs_read(fd, g_flashapp_io, FLASHAPP_IO_CHUNK);
        if(n < 0)
        {
            (void)arkrtos_vfs_close(fd);
            return ARKRTOS_MINI_ERR_FS;
        }
        if(n == 0)
        {
            break;
        }

        crc = flashapp_crc32(crc, g_flashapp_io, (uint32)n);

        /* 向量表：只认负载前 8 字节。 */
        for(i = 0u; (i < (uint32)n) && ((base + i) < 8u); i++)
        {
            vec[base + i] = g_flashapp_io[i];
        }
        if((base == 0u) && ((uint32)n >= 8u))
        {
            uint32 v = ((uint32)vec[0]) | ((uint32)vec[1] << 8) |
                       ((uint32)vec[2] << 16) | ((uint32)vec[3] << 24);
            uint32 e = ((uint32)vec[4]) | ((uint32)vec[5] << 8) |
                       ((uint32)vec[6] << 16) | ((uint32)vec[7] << 24);

            *p_sp  = v;
            *p_vec = e;
            ent    = e & ~1u;
            /* 入口必须落在本区内，否则偏移无意义（越界的由 check_shape 拒收）。 */
            if((ent >= (uint32)ARKRTOS_FLASHAPP_BASE) &&
               (ent <  (uint32)ARKRTOS_FLASHAPP_END))
            {
                pro_off = ent - (uint32)ARKRTOS_FLASHAPP_BASE;
            }
        }

        /* 序言半字：入口那一条指令，不是文件首字节——arkc 把字符串字面量
         * 排在所有函数之前，带字符串的程序首字节是字符、不是指令。 */
        if((pro_off != 0xFFFFFFFFu) &&
           (pro_off >= base) && ((pro_off + 2u) <= (base + (uint32)n)))
        {
            *p_pro = (uint16)(((uint16)g_flashapp_io[pro_off - base]) |
                              ((uint16)g_flashapp_io[pro_off - base + 1u] << 8));
        }

        total += (uint32)n;

        /* 超上限就地拒收：继续读下去只是把时间花在一个已经不合格的文件上。 */
        if(total > (uint32)ARKRTOS_FLASHAPP_MAX_IMAGE)
        {
            (void)arkrtos_vfs_close(fd);
            return ARKRTOS_LOADER_ERR_SIZE;
        }
    }

    (void)arkrtos_vfs_close(fd);

    if(total < 8u)
    {
        return ARKRTOS_LOADER_ERR_SIZE;    /* 空文件/目录都不是可执行镜像 */
    }

    *p_len = total;
    *p_crc = crc;
    return 0;
}

/* 形状校验：这份文件是不是给「Flash 应用区 + RAM 数据窗口」这条链链接的。
 * 三条全对才放行——SP 对上窗口顶、入口落在区内、入口首半字是 arkc 序言。 */
static int32 flashapp_check_shape(uint32 len, uint32 sp, uint32 vec, uint16 pro)
{
    uint32 ent = vec & ~1u;

    if((len < 8u) || (len > (uint32)ARKRTOS_FLASHAPP_MAX_IMAGE))
    {
        return ARKRTOS_FLASHAPP_ERR_LAYOUT;
    }

    /* SP 必须正好是数据窗口顶端：拿别处布局链出来的镜像，这一步当场拒收。 */
    if(sp != (uint32)(ARKRTOS_FLASHAPP_RAM_BASE + ARKRTOS_FLASHAPP_RAM_SIZE))
    {
        return ARKRTOS_FLASHAPP_ERR_LAYOUT;
    }

    /* 入口必须落在「本区已写入的那一段」里：落在区外等于执行任意字节，
     * 落在本区但超出负载长度等于执行擦过的 0xFFFFFFFF。 */
    if((ent < (uint32)ARKRTOS_FLASHAPP_BASE) ||
       ((ent + 2u) > ((uint32)ARKRTOS_FLASHAPP_BASE + len)))
    {
        return ARKRTOS_FLASHAPP_ERR_LAYOUT;
    }

    if(pro != (uint16)ARKRTOS_FLASHAPP_PROLOGUE)
    {
        return ARKRTOS_FLASHAPP_ERR_PROLOGUE;
    }

    return 0;
}

int32 arkrtos_flashapp_write(const char *path, uint32 *p_len)
{
    uint32 irq;
    uint32 len  = 0u;
    uint32 crc  = 0u;
    uint32 sp   = 0u;
    uint32 vec  = 0u;
    uint16 pro  = 0u;
    uint32 total;
    int32  fd;
    int32  n;
    int32  ret;

    if(path == 0)
    {
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    if(arkrtos_vfs_ready() == 0u)
    {
        return ARKRTOS_LOADER_ERR_STATE;
    }

    /* 单实例：正在跑的那份代码就在这条路径要擦的区里，边跑边擦等于把
     * 正在取指的扇区抹掉。这里直接读状态字，不走 arkrtos_flashapp_task_id()
     * ——后者内部要取同一把锁，在已持锁的上下文里会自死锁。 */
    arkrtos_spin_lock_irqsave(&g_flashapp_lock, &irq);
    if(g_flashapp_task != 0u)
    {
        arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);
        return ARKRTOS_FLASHAPP_ERR_BUSY;
    }
    g_flashapp_task = 0xFFFFFFFFu;          /* 占位：写入中 */
    arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);

    /* 第一步：整份验。验不过就原地返回，区里那份还能跑的程序分毫未动。 */
    ret = flashapp_scan(path, &len, &crc, &sp, &vec, &pro);
    if(ret != 0)
    {
        goto out_busy;
    }

    ret = flashapp_check_shape(len, sp, vec, pro);
    if(ret != 0)
    {
        goto out_busy;
    }

    /* 文件系统只有一份缓存：从这一步起不再与别的任务交错读写。 */
    arkrtos_sched_lock_internal();

    /* 第二步：擦整区。失败也是「区里现在是空的」，如实上报。 */
    if(arkrtos_port_flash_erase((uint32)ARKRTOS_FLASHAPP_BASE,
                                (uint32)ARKRTOS_FLASHAPP_SIZE) != 0)
    {
        arkrtos_sched_unlock_internal();
        ret = ARKRTOS_FLASHAPP_ERR_FLASH;
        goto out_busy;
    }

    /* 第三步：分块读 + 写。源缓冲 32 字节对齐、且同一次写入内按顺序单调
     * 推进——H7 的 flash word 编程是读-改-写，同一 32 字节块只能编程一次。 */
    fd = arkrtos_vfs_open_read(path);
    if(fd < 0)
    {
        arkrtos_sched_unlock_internal();
        ret = ARKRTOS_MINI_ERR_FS;
        goto out_busy;
    }

    total = 0u;
    while(total < len)
    {
        uint32 want = len - total;

        if(want > FLASHAPP_IO_CHUNK)
        {
            want = FLASHAPP_IO_CHUNK;
        }

        n = arkrtos_vfs_read(fd, g_flashapp_io, want);
        if(n <= 0)
        {
            /* 前面刚扫过一遍都读得通，这里读不到说明文件被改了或被卸了卷。
             * 区已经被擦，没有回滚——如实报「写入失败、区里现在是空的」。 */
            (void)arkrtos_vfs_close(fd);
            arkrtos_sched_unlock_internal();
            ret = ARKRTOS_MINI_ERR_FS;
            goto out_busy;
        }

        if(arkrtos_port_flash_write((uint32)ARKRTOS_FLASHAPP_BASE + total,
                                    g_flashapp_io, (uint32)n) != 0)
        {
            (void)arkrtos_vfs_close(fd);
            arkrtos_sched_unlock_internal();
            ret = ARKRTOS_FLASHAPP_ERR_FLASH;
            goto out_busy;
        }

        total += (uint32)n;
    }

    (void)arkrtos_vfs_close(fd);

    /* 第四步：回读逐字节比对。写入返回 0 只说明「下发成功」，不说明
     * 「落进去的是这些字节」——这一步才是「写对了」的唯一证据。
     * 顺带重算 CRC，与扫描时那一份比较。 */
    fd = arkrtos_vfs_open_read(path);
    if(fd < 0)
    {
        arkrtos_sched_unlock_internal();
        ret = ARKRTOS_MINI_ERR_FS;
        goto out_busy;
    }

    total = 0u;
    while(total < len)
    {
        uint32 want = len - total;
        uint32 i;

        if(want > FLASHAPP_IO_CHUNK)
        {
            want = FLASHAPP_IO_CHUNK;
        }

        n = arkrtos_vfs_read(fd, g_flashapp_io, want);
        if(n <= 0)
        {
            (void)arkrtos_vfs_close(fd);
            arkrtos_sched_unlock_internal();
            ret = ARKRTOS_MINI_ERR_FS;
            goto out_busy;
        }

        for(i = 0u; i < (uint32)n; i++)
        {
            if(g_flashapp_io[i] !=
               *(volatile const uint8 *)((uint32)ARKRTOS_FLASHAPP_BASE + total + i))
            {
                (void)arkrtos_vfs_close(fd);
                arkrtos_sched_unlock_internal();
                ret = ARKRTOS_FLASHAPP_ERR_VERIFY;
                goto out_busy;
            }
        }

        total += (uint32)n;
    }

    (void)arkrtos_vfs_close(fd);

    /* 第五步：写元数据（区尾固定 32 字节）。放在最后：中途掉电时元数据
     * 缺失 ⇒ info 判「这里没有产物」，而不是让一份半截镜像看起来能跑。 */
    {
        static flashapp_meta_t meta __attribute__((aligned(32)));
        uint32 i;

        meta.magic   = (uint32)ARKRTOS_FLASHAPP_MAGIC;
        meta.len     = len;
        meta.crc     = crc;
        meta.version = (uint32)ARKRTOS_FLASHAPP_VERSION;
        for(i = 0u; i < 4u; i++)
        {
            meta.rsv[i] = 0xFFFFFFFFu;
        }

        if(arkrtos_port_flash_write((uint32)ARKRTOS_FLASHAPP_META_BASE,
                                    (const uint8 *)(const void *)&meta,
                                    (uint32)sizeof(meta)) != 0)
        {
            arkrtos_sched_unlock_internal();
            ret = ARKRTOS_FLASHAPP_ERR_FLASH;
            goto out_busy;
        }

        /* 元数据也要回读：写接口返回 0 不等于落进去的是这 32 字节。 */
        for(i = 0u; i < (uint32)sizeof(meta); i++)
        {
            if(((const volatile uint8 *)(const void *)&meta)[i] !=
               *(volatile const uint8 *)((uint32)ARKRTOS_FLASHAPP_META_BASE + i))
            {
                arkrtos_sched_unlock_internal();
                ret = ARKRTOS_FLASHAPP_ERR_VERIFY;
                goto out_busy;
            }
        }
    }

    arkrtos_sched_unlock_internal();

    if(p_len != 0)
    {
        *p_len = len;
    }

    ARKRTOS_LOGI("FLASHAPP", "%s: %u B, crc 0x%08X -> 0x%08X..0x%08X (verified)",
                 path, (unsigned)len, (unsigned)crc,
                 (unsigned)ARKRTOS_FLASHAPP_BASE,
                 (unsigned)((uint32)ARKRTOS_FLASHAPP_BASE + len));

    ret = 0;

out_busy:
    arkrtos_spin_lock_irqsave(&g_flashapp_lock, &irq);
    g_flashapp_task = 0u;
    arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);

    return ret;
}

int32 arkrtos_flashapp_erase(void)
{
    uint32 irq;
    int32  ret;

    arkrtos_spin_lock_irqsave(&g_flashapp_lock, &irq);
    if(g_flashapp_task != 0u)
    {
        arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);
        return ARKRTOS_FLASHAPP_ERR_BUSY;
    }
    arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);

    if(arkrtos_port_flash_erase((uint32)ARKRTOS_FLASHAPP_BASE,
                                (uint32)ARKRTOS_FLASHAPP_SIZE) != 0)
    {
        return ARKRTOS_FLASHAPP_ERR_FLASH;
    }

    ARKRTOS_LOGI("FLASHAPP", "area 0x%08X..0x%08X erased",
                 (unsigned)ARKRTOS_FLASHAPP_BASE,
                 (unsigned)ARKRTOS_FLASHAPP_END);
    ret = 0;
    return ret;
}

int32 arkrtos_flashapp_info(uint32 *p_len, uint32 *p_crc, uint32 *p_entry)
{
    const volatile flashapp_meta_t *meta =
        (const volatile flashapp_meta_t *)(const void *)ARKRTOS_FLASHAPP_META_BASE;
    uint32 len;
    uint32 crc;
    uint32 version;
    uint32 sp;
    uint32 vec;

    /* 元数据不取锁：它只是一次从 Flash 读 32 字节，且 write 期间元数据
     * 还没写（老的那份已经随擦除消失），读到的任何状态都是自洽的。 */
    if(meta->magic != (uint32)ARKRTOS_FLASHAPP_MAGIC)
    {
        return ARKRTOS_FLASHAPP_ERR_META;
    }

    version = meta->version;
    len     = meta->len;
    crc     = meta->crc;

    if(version != (uint32)ARKRTOS_FLASHAPP_VERSION)
    {
        return ARKRTOS_FLASHAPP_ERR_META;
    }

    if((len < 8u) || (len > (uint32)ARKRTOS_FLASHAPP_MAX_IMAGE))
    {
        return ARKRTOS_FLASHAPP_ERR_META;
    }

    /* 向量表也核一遍：元数据说是这幅形状，控点却是内存里那 8 个字节。
     * 两者不一致（比如烧写只成功了一半）时以内存为准，判「没有可用产物」。 */
    sp  = *(const volatile uint32 *)(const void *)((uint32)ARKRTOS_FLASHAPP_BASE);
    vec = *(const volatile uint32 *)(const void *)((uint32)ARKRTOS_FLASHAPP_BASE + 4u);
    if(sp != (uint32)(ARKRTOS_FLASHAPP_RAM_BASE + ARKRTOS_FLASHAPP_RAM_SIZE))
    {
        return ARKRTOS_FLASHAPP_ERR_META;
    }
    if((vec & ~1u) == 0u)
    {
        return ARKRTOS_FLASHAPP_ERR_META;
    }

    if(p_len != 0)
    {
        *p_len = len;
    }
    if(p_crc != 0)
    {
        *p_crc = crc;
    }
    if(p_entry != 0)
    {
        *p_entry = vec;
    }

    return 0;
}

int32 arkrtos_flashapp_run(uint32 *task_out)
{
    uint32 irq;
    uint32 code_base = 0u;
    uint32 code_size = 0u;
    uint32 ram_base = 0u;
    uint32 ram_size = 0u;
    uint32 pt_index = 0u;
    uint32 len = 0u;
    uint32 vec = 0u;
    uint32 entry;
    int32  task_id;
    int32  ret;

    /* 单实例闸门：先占位再干活（装载期间进来的第二个请求不能被放行）。 */
    arkrtos_spin_lock_irqsave(&g_flashapp_lock, &irq);
    if(g_flashapp_task != 0u)
    {
        arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);
        return ARKRTOS_FLASHAPP_ERR_BUSY;
    }
    g_flashapp_task = 0xFFFFFFFFu;
    arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);

    ret = arkrtos_flashapp_info(&len, 0, &vec);
    if(ret != 0)
    {
        goto release_slot;
    }

    /* 入口复核：info 只核了 SP，这里再核「入口在区内且首半字是序言」——
     * 从 Flash 取指时，这两条是唯一能在起任务前发现「区里是别的字节」的机会。 */
    entry = vec & ~1u;
    if((entry < (uint32)ARKRTOS_FLASHAPP_BASE) ||
       ((entry + 2u) > ((uint32)ARKRTOS_FLASHAPP_BASE + len)))
    {
        ret = ARKRTOS_FLASHAPP_ERR_LAYOUT;
        goto release_slot;
    }
    if(*(const volatile uint16 *)(const void *)entry !=
       (uint16)ARKRTOS_FLASHAPP_PROLOGUE)
    {
        ret = ARKRTOS_FLASHAPP_ERR_PROLOGUE;
        goto release_slot;
    }

    /* 借窗：代码块借来是空占（产物在 Flash，不用 RAM 代码窗），作用是让
     * 「app run 与 flash app 抢同一段数据窗口」由池分配器自己拒绝，不需要
     * 再加一道只在这一处生效的闸门。ptable_mini_alloc 要求两个尺寸都非零。 */
    arkrtos_sched_lock_internal();
    {
        uint32 c_base = 0u, c_size = 0u, r_base = 0u, r_size = 0u;
        int32  slot;

        slot = arkrtos_ptable_mini_alloc((uint32)ARKRTOS_APPRUN_CODE_SIZE,
                                         (uint32)ARKRTOS_FLASHAPP_RAM_SIZE,
                                         &pt_index,
                                         &c_base, &c_size, &r_base, &r_size);
        if(slot < 0)
        {
            arkrtos_sched_unlock_internal();
            ret = ARKRTOS_FLASHAPP_ERR_WINDOW;
            goto release_slot;
        }

        /* 数据窗口必须正好是 APPRUN 那一块：产物按这个地址链好了 stack 与
         * .data，搬到别处跑等于让它跳进虚空。 */
        if((c_base != (uint32)ARKRTOS_APPRUN_BASE) ||
           (c_size != (uint32)ARKRTOS_APPRUN_CODE_SIZE) ||
           (r_base != (uint32)ARKRTOS_FLASHAPP_RAM_BASE) ||
           (r_size != (uint32)ARKRTOS_FLASHAPP_RAM_SIZE))
        {
            (void)arkrtos_ptable_mini_free(pt_index);
            arkrtos_sched_unlock_internal();
            ret = ARKRTOS_FLASHAPP_ERR_WINDOW;
            goto release_slot;
        }

        code_base = c_base;
        code_size = c_size;
        ram_base  = r_base;
        ram_size  = r_size;
    }
    arkrtos_sched_unlock_internal();

    /* 栈取数据窗口末尾的 4 KB，向下增长；.data/.bss 落在窗口前段，由程序的
     * __arkcc_init_globals() 按链接地址初始化/清零。 */
    task_id = arkrtos_task_register((void (*)(void))entry,
                                    (uint32 *)(void *)(ram_base +
                                        (uint32)ARKRTOS_FLASHAPP_RAM_SIZE -
                                        (uint32)ARKRTOS_FLASHAPP_STACK_SIZE),
                                    (uint32)ARKRTOS_FLASHAPP_STACK_SIZE,
                                    (uint8)ARKRTOS_FLASHAPP_PRIORITY,
                                    (uint32)ARKRTOS_APPRUN_PERIOD_MS);
    if(task_id <= 0)
    {
        (void)arkrtos_ptable_mini_free(pt_index);
        ret = ARKRTOS_LOADER_ERR_TASK;
        goto release_slot;
    }

    /* TCB 的两窗口：rom 记成 Flash 应用区（只读可执行），ram 记成借到的
     * 数据块。这是本路径与 app run 唯一不同的地方——app run 的 rom 是池里
     * 那块可执行 RAM。 */
    arkrtos_task_table[task_id - 1].rom_start = (uint32)ARKRTOS_FLASHAPP_BASE;
    arkrtos_task_table[task_id - 1].rom_size  = (uint32)ARKRTOS_FLASHAPP_SIZE;
    arkrtos_task_table[task_id - 1].ram_start = ram_base;
    arkrtos_task_table[task_id - 1].ram_size  = ram_size;
    arkrtos_task_table[task_id - 1].is_priv   = 0u;    /* 非特权：与 App 同级 */

    arkrtos_mpu_build_task(&arkrtos_task_table[task_id - 1]);

    (void)arkrtos_ptable_set_slot(pt_index, ARKRTOS_APP_SLOT_RUNNING,
                                  entry, (uint32)task_id);

    arkrtos_spin_lock_irqsave(&g_flashapp_lock, &irq);
    g_flashapp_pt    = pt_index;
    g_flashapp_entry = entry;
    g_flashapp_task  = (uint32)task_id;
    if(task_out != 0)
    {
        *task_out = (uint32)task_id;    /* activate 之前带出去 */
    }
    arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);

    ARKRTOS_LOGI("FLASHAPP", "%u B from flash 0x%08X, entry 0x%08X, data 0x%08X (%u B), task %d",
                 (unsigned)len, (unsigned)ARKRTOS_FLASHAPP_BASE,
                 (unsigned)entry, (unsigned)ram_base,
                 (unsigned)ram_size, (int)task_id);

    arkrtos_sched_activate_higher((uint8)ARKRTOS_FLASHAPP_PRIORITY);
    return 0;

release_slot:
    arkrtos_spin_lock_irqsave(&g_flashapp_lock, &irq);
    g_flashapp_task = 0u;
    arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);
    return ret;
}

int32 arkrtos_flashapp_on_task_exit(uint32 task_id)
{
    uint32 irq;
    uint32 pt_index = 0u;

    if(task_id == 0u)
    {
        return 0;
    }

    /* 只认自己的主任务：内核任务、App、小程序、直跑程序自己开的线程退出时
     * 都原样返回 0——还一次窗就是在池里漏两块 RAM。 */
    arkrtos_spin_lock_irqsave(&g_flashapp_lock, &irq);
    if((g_flashapp_task == 0u) || (g_flashapp_task != task_id))
    {
        arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);
        return 0;
    }
    pt_index = g_flashapp_pt;
    g_flashapp_task  = 0u;
    g_flashapp_pt    = 0u;
    g_flashapp_entry = 0u;
    arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);

    /* 调用者此刻已被内核置为 INVALID，没有任何上下文还在执行那块数据窗口。 */
    if(arkrtos_ptable_mini_free(pt_index) != 0)
    {
        ARKRTOS_LOGE("FLASHAPP", "task %u exited but window %u was already free",
                     (unsigned)task_id, (unsigned)pt_index);
    }

    ARKRTOS_LOGI("FLASHAPP", "task %u exited, data window returned to the pool",
                 (unsigned)task_id);

    return 0;
}

uint32 arkrtos_flashapp_task_id(void)
{
    uint32 irq;
    uint32 id;

    arkrtos_spin_lock_irqsave(&g_flashapp_lock, &irq);
    id = g_flashapp_task;
    arkrtos_spin_unlock_irqrestore(&g_flashapp_lock, irq);

    return (id == 0xFFFFFFFFu) ? 0u : id;   /* 装载中的占位不算「在跑」 */
}

const char *arkrtos_flashapp_error_name(int32 rc)
{
    switch(rc)
    {
    case ARKRTOS_FLASHAPP_ERR_BUSY:     return "a direct-run program is already running";
    case ARKRTOS_FLASHAPP_ERR_WINDOW:   return "the data window could not be taken";
    case ARKRTOS_FLASHAPP_ERR_LAYOUT:   return "image was not linked for the flash app area";
    case ARKRTOS_FLASHAPP_ERR_PROLOGUE: return "entry does not start with an arkc prologue";
    case ARKRTOS_FLASHAPP_ERR_META:     return "no image in the flash app area";
    case ARKRTOS_FLASHAPP_ERR_VERIFY:   return "read-back mismatch: flash content differs from the file";
    case ARKRTOS_FLASHAPP_ERR_FLASH:    return "flash erase/program failed";
    default:                            return 0;    /* 不是本路径独有的错误码 */
    }
}

#endif /* ARKRTOS_USE_FLASHAPP */
