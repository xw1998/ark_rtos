/**
* @file drvblk.c
* @brief QEMU `virt`（LoongArch64）板级块设备：RAM 里模拟 NOR 语义的一卷存储
* @details 设计边界写在 drvblk.h 顶部。这里只强调实现上的三个决定：
*
*          1) **存储放在 .bss**（4 MB）。QEMU 的 DRAM 有 128 MB，内核 RAM
*             段 8 MB，链接脚本末尾有 ASSERT 兜住溢出——所以"够不够放"由链接期
*             回答，不在运行期猜。
*          2) **写只允许 1→0**：和 NOR 一样，任何一个目标字节不是 0xFF 就整笔
*             拒绝（不是部分写入），并把写重试计数加一。这样上层的
*             "先擦后写" 约定一旦被破坏，会当场变成 -1 而不是静默的坏数据。
*          3) **不做读-改-写**：块设备契约里写明了"写落在已擦除字节上"，这一层
*             不替调用者补这一步。补了就等于把调用者的 bug 吃进去。
*
* @author xw
*/

#include "arkrtos_features.h"

#if (ARKRTOS_USE_BLK == 1)

#include "arkrtos_blk.h"
#include "drvblk.h"

/* 4 MB 的"介质"。对齐到擦除单位，便于将来换成 DMA 直连的窗口。
 * 尺寸按"装得下一份真实静态二进制（623728 B）+ 一份 libc（1571208 B）"
 * 定，不是随手取的圆整值。 */
static uint8 s_ram_nor[ARKRTOS_QEMU_BLK_SIZE]
    __attribute__((aligned(ARKRTOS_QEMU_BLK_ERASE)));

static uint32 s_written;
static uint32 s_rejected;
static uint32 s_erased;

static int32 blk_ram_bringup(void)
{
    /* 上电语义：非易失介质的内容是未知的；这里第一次 bring-up 时整卷填 0xFF，
     * 相当于"出厂已擦除"。第二次调用直接返回 0（契约要求幂等），不会把
     * 已经写进去的内容抹掉。 */
    static uint8 s_one_shot;

    if(s_one_shot == 0u)
    {
        uint32 i;

        for(i = 0u; i < ARKRTOS_QEMU_BLK_SIZE; i++)
        {
            s_ram_nor[i] = 0xFFu;
        }
        s_one_shot = 1u;
    }

    return 0;
}

static int32 blk_ram_read(uint32 off, uint8 *buf, uint32 len)
{
    uint32 i;

    if((off > ARKRTOS_QEMU_BLK_SIZE) || (len > (ARKRTOS_QEMU_BLK_SIZE - off)))
    {
        return -1;
    }

    for(i = 0u; i < len; i++)
    {
        buf[i] = s_ram_nor[off + i];
    }

    return 0;
}

static int32 blk_ram_write(uint32 off, const uint8 *buf, uint32 len)
{
    uint32 i;

    if((off > ARKRTOS_QEMU_BLK_SIZE) || (len > (ARKRTOS_QEMU_BLK_SIZE - off)))
    {
        return -1;
    }

    /* 先整体检查再落笔：NOR 的"只能 1→0"意味着覆写已编程字节是物理上做不到
     * 的事，不是"数据会坏"。所以整笔拒绝，不做部分写入。 */
    for(i = 0u; i < len; i++)
    {
        if(s_ram_nor[off + i] != 0xFFu)
        {
            s_rejected++;
            return -1;
        }
    }

    for(i = 0u; i < len; i++)
    {
        s_ram_nor[off + i] = buf[i];
    }

    s_written += len;

    return 0;
}

static int32 blk_ram_erase(uint32 off, uint32 len)
{
    uint32 i;

    if((off > ARKRTOS_QEMU_BLK_SIZE) || (len > (ARKRTOS_QEMU_BLK_SIZE - off)))
    {
        return -1;
    }

    for(i = 0u; i < len; i++)
    {
        s_ram_nor[off + i] = 0xFFu;
    }

    s_erased++;

    return 0;
}

static const arkrtos_blk_dev_t g_blk_ram =
{
    ARKRTOS_QEMU_BLK_NAME,
    (uint32)ARKRTOS_QEMU_BLK_SIZE,
    (uint32)ARKRTOS_QEMU_BLK_ERASE,
    blk_ram_bringup,        /* 首次调用整卷填 0xFF，之后幂等 */
    0,                      /* size_fn：编译期常量就够 */
    blk_ram_read,
    blk_ram_write,
    blk_ram_erase
};

int32 arkrtos_board_blk_init(void)
{
    return arkrtos_blk_register(&g_blk_ram);
}

uint32 arkrtos_board_blk_bytes_written(void)
{
    return s_written;
}

uint32 arkrtos_board_blk_write_rejected(void)
{
    return s_rejected;
}

uint32 arkrtos_board_blk_erase_count(void)
{
    return s_erased;
}

#else /* ARKRTOS_USE_BLK == 0 */

#include "drvblk.h"

int32 arkrtos_board_blk_init(void)
{
    return 0;
}

uint32 arkrtos_board_blk_bytes_written(void)
{
    return 0u;
}

uint32 arkrtos_board_blk_write_rejected(void)
{
    return 0u;
}

uint32 arkrtos_board_blk_erase_count(void)
{
    return 0u;
}

#endif /* ARKRTOS_USE_BLK */
