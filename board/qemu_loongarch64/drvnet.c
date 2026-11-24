/**
* @file drvnet.c
* @brief QEMU `virt`（LoongArch64）的以太网卡：PCI 上的 virtio-net（默认关）
* @details 这块板没有 PHY、没有 MDIO、没有 RMII 引脚——它唯一的"以太网"是
*          QEMU 挂在 PCI 总线上的 virtio-net。本文件就是那半边的驱动，实现
*          kernelsrc/include/arkrtos_net_dev.h 的四条契约。为什么值得写：
*          在此之前 lwIP 只跑一张回环网卡（lwip_port.c 顶部写着这件事），
*          "网络面能用"这句话因此只在 socket 语义层面成立。接上这张卡之后
*          出去的是**真实以太网帧**——ARP 请求/应答、ICMP echo/echo reply
*          都能在 QEMU 的 filter-dump 抓包里逐字节看到。
*
*          四个必须一起读的边界：
*            1) **轮询，不是中断**。本移植没有把 PCI 中断（PCH-PIC / EIOINTC）
*               接进内核，写成"中断驱动"只会给出假证据。收包发生在
*               arkrtos_net_dev_rx() 被调用的那一刻，发包只写环 + notify。
*            2) **BAR 由我们自己分配**。QEMU 的 pci-host-ecam-generic 没有
*               firmware，设备上电时 BAR 是 0；驱动按标准做法探测大小、把
*               地址落到 pcie ranges 的内存窗口（0x40000000）。落之前先
*               打开 MEM/BUS_MASTER，否则设备的 DMA 不出门。
*            3) **ECAM 与 BAR 都是静态映射**。这两段窗口由 port 在开 PG
*               之前铺好（ECAM 一条 PS=20 覆盖 bus 0 的 1 MB；MMIO 按 BAR4
*               实测的 16 KB 逐页 4 KB。见 arkrtos_port.c 的
*               la_mmu_fill_static() 与板级的 ARKRTOS_QEMU_PCI_*）。
*               驱动**不**用动态 alias：设备窗口是常驻映射，住进会被回收
*               的动态槽位，迟早被别的调用方顺手撤掉 —— 实测踩过一次，
*               第二帧 ARP 重发就 badv=0x40003004 -> KERNEL refill。
*            4) **关档零字节**：整个文件在 ARKRTOS_USE_NET_VIRTIO=0 时是空
*               翻译单元，默认档镜像与行为逐位不变。
*
*          诚实边界：这是 QEMU 的虚拟网卡，不是真 PHY。它证明的是"内核能
*          驱动一张真的以太网卡并把帧收发出去"，**不**证明 PHY/MDIO/RMII
*          那条路——真板接入仍欠账（见 docs/已知限制.md 与 docs/欠账盘点.md）。
*
* @author xw
*/

#include "arkrtos_features.h"
#include "arkrtos_types.h"

#if (ARKRTOS_USE_NET_VIRTIO == 1)

#include "arkrtos_config.h"
#include "arkrtos_hal.h"
#include "arkrtos_net_dev.h"
#include "arkrtos_board.h"

/* ============================================================
 * 1. 地址与寄存器口径
 * ============================================================ */

/* PCIe ECAM：QEMU loongarch virt 的机器设备树里 pcie@20000000 就是它
 * （`-machine virt,dumpdtb=x.dtb` 导出后看 reg = 0x20000000/0x08000000，
 *  bus-range 0..0x7f 可核）。我们只扫 bus 0，即前 1 MB。
 * 地址来自板级：port 静态映射的那一段与这里枚举的必须是同一个数，写两份
 * 常量迟早会分叉（改了板级却没改驱动 -> 读配置空间直接 refill）。 */
#define NET_ECAM_BASE           ((uint32)ARKRTOS_QEMU_PCI_ECAM_BASE)

/* pcie ranges 里的内存窗口起点（父地址）。QEMU 给的整段是
 * 0x40000000 / 1 GB，我们的 BAR 从窗口头开始放 —— 这块板上没有别的
 * 设备抢地址，所以"从头放"是确定的，不是碰运气。
 * port 已把 [ARKRTOS_QEMU_PCI_MMIO_BASE, +ARKRTOS_QEMU_PCI_MMIO_SIZE) 静态
 * 映射好，BAR 必须落在这段里（pci_bar_map() 末尾会核）。 */
#define NET_PCI_MEM_BASE        ((uint32)ARKRTOS_QEMU_PCI_MMIO_BASE)

/* virtio 1.0 现代设备的 PCI 厂商/设备号：0x1AF4:0x1041 是"只有现代接口"
 * （non-transitional）；0x1000 是 transitional，本驱动也能用，但只用它的
 * 现代 capability —— 传统 I/O BAR 需要 port I/O，LoongArch 没有这条指令。 */
#define NET_VIRTIO_VENDOR       0x1AF4u
#define NET_VIRTIO_DEV_MODERN   0x1041u
#define NET_VIRTIO_DEV_TRANS    0x1000u

/* PCI 配置空间常用偏移 */
#define PCI_CFG_VENDOR          0x00u
#define PCI_CFG_COMMAND         0x04u
#define PCI_CFG_BAR0            0x10u
#define PCI_CFG_STATUS          0x06u
#define PCI_CFG_CAP             0x34u
#define PCI_CMD_MEM_SPACE       0x0002u
#define PCI_CMD_BUS_MASTER      0x0004u
#define PCI_STATUS_CAP_LIST     0x0010u
#define PCI_CAP_ID_VNDR         0x09u

/* virtio capability 的 cfg_type */
#define VIRTIO_CFG_COMMON       1u
#define VIRTIO_CFG_NOTIFY       2u
#define VIRTIO_CFG_ISR          3u
#define VIRTIO_CFG_DEVICE       4u

/* 现代控制区（common cfg）寄存器偏移 */
#define VC_DEV_FEAT_SEL         0x00u
#define VC_DEV_FEAT             0x04u
#define VC_DRV_FEAT_SEL         0x08u
#define VC_DRV_FEAT             0x0Cu
#define VC_DEV_STATUS           0x14u
#define VC_QUEUE_SEL            0x16u
#define VC_QUEUE_SIZE           0x18u
#define VC_QUEUE_ENABLE         0x1Cu
#define VC_QUEUE_NOTIFY_OFF     0x1Eu
#define VC_QUEUE_DESC           0x20u
#define VC_QUEUE_AVAIL          0x28u
#define VC_QUEUE_USED           0x30u

/* 设备状态位 */
#define VIRTIO_STATUS_ACK       0x01u
#define VIRTIO_STATUS_DRIVER    0x02u
#define VIRTIO_STATUS_DRIVER_OK 0x04u
#define VIRTIO_STATUS_FEAT_OK   0x08u

/* 协商的特性：只买两样。
 *   bit 5  = VIRTIO_NET_F_MAC（设备告诉我们要用哪个 MAC，而不是编一个）；
 *   bit 32 = VIRTIO_F_VERSION_1（现代接口的唯一门槛，不买它设备会拒绝）。
 * 大端/校验和卸载一概不买：多一样特性就多一条要单独验的路径，而这一层
 * 只需要"把帧搬出去、搬进来"。 */
#define VIRTIO_NET_F_MAC        (1u << 5)
#define VIRTIO_F_VERSION_1_HI   0x00000001u

/* 描述符标志 */
#define VQ_DESC_F_NEXT          0x0001u
#define VQ_DESC_F_WRITE         0x0002u

/* virtio-net 的帧头：现代设备（VERSION_1）固定 12 字节，其中最后两个
 * 字节是 num_buffers。我们不用 MRG_RXBUF，所以发送侧填 0、接收侧忽略，
 * 但**必须先付这 12 个字节的偏移**，否则整帧会错 12 字节而看起来"像乱码"。 */
#define VN_HDR_LEN              12u

#define NET_QUEUE_NUM           16u     /* 两个环都用 16 个描述符 */
#define NET_RX_BUF_SIZE         2048u

/* ============================================================
 * 2. 数据结构
 * ============================================================ */

/* split virtqueue 描述符（16 字节，布局按 virtio 1.0 §2.7.5） */
typedef struct {
    uint32 addr_lo;
    uint32 addr_hi;
    uint32 len;
    uint16 flags;
    uint16 next;
} net_desc_t;

typedef struct {
    uint16 flags;
    uint16 idx;
    uint16 ring[NET_QUEUE_NUM];
    uint16 used_event;
} net_avail_t;

typedef struct {
    uint32 id;
    uint32 len;
} net_used_elem_t;

typedef struct {
    uint16 flags;
    uint16 idx;
    net_used_elem_t ring[NET_QUEUE_NUM];
    uint16 avail_event;
} net_used_t;

/* 一个环的全部状态。RX 与 TX 结构相同、方向不同，共用一份代码。 */
typedef struct {
    net_desc_t  desc[NET_QUEUE_NUM];
    net_avail_t avail;
    net_used_t  used;
    uint32      notify_off;
    uint32      seen;               /* 已消费的 used 条目数（16 位回绕后仍可用差值） */
} net_queue_t;

static net_queue_t s_rxq __attribute__((aligned(64)));
static net_queue_t s_txq __attribute__((aligned(64)));

static uint8 s_rx_buf[NET_QUEUE_NUM][NET_RX_BUF_SIZE] __attribute__((aligned(64)));
static uint8 s_tx_buf[NET_QUEUE_NUM][ARKRTOS_NET_DEV_FRAME_MAX] __attribute__((aligned(64)));

static uint32 s_common;
static uint32 s_notify;
static uint32 s_notify_mul;
static uint32 s_devcfg;
static uint32 s_isr;

static uint8  s_mac[6];
static uint32 s_ready;              /* 1 = init 成功且设备 DRIVER_OK */
static uint32 s_init_tried;
static int32  s_init_rc;

static uint32 s_tx_ok;
static uint32 s_tx_drop;
static uint32 s_rx_ok;
static uint32 s_rx_drop;
static uint32 s_tx_inflight;

/* ============================================================
 * 3. MMIO 与 PCI 配置访问
 * ============================================================ */

static uint32 net_rd32(uint32 a)
{
    return *(volatile uint32 *)(unsigned long)a;
}

static void net_wr32(uint32 a, uint32 v)
{
    *(volatile uint32 *)(unsigned long)a = v;
}

static uint16 net_rd16(uint32 a)
{
    return *(volatile uint16 *)(unsigned long)a;
}

static void net_wr16(uint32 a, uint16 v)
{
    *(volatile uint16 *)(unsigned long)a = v;
}

static uint8 net_rd8(uint32 a)
{
    return *(volatile uint8 *)(unsigned long)a;
}

static void net_wr8(uint32 a, uint8 v)
{
    *(volatile uint8 *)(unsigned long)a = v;
}

/* 找到设备后，它的那 4 KB 配置页的基址；0 = 还没找到 */
static uint32 s_cfg;
/* 命中的设备号（只为把日志里的 00:xx.0 打成真值，不再写死 00:00.0） */
static uint32 s_dev;

static uint32 pci_rd32(uint8 off)
{
    return net_rd32(s_cfg + (uint32)off);
}

static uint16 pci_rd16(uint8 off)
{
    return net_rd16(s_cfg + (uint32)off);
}

static uint8 pci_rd8(uint8 off)
{
    return net_rd8(s_cfg + (uint32)off);
}

static void pci_wr32(uint8 off, uint32 v)
{
    net_wr32(s_cfg + (uint32)off, v);
}

static void pci_wr16(uint8 off, uint16 v)
{
    net_wr16(s_cfg + (uint32)off, v);
}

/* ============================================================
 * 4. 扫描 PCI 总线 0
 * @details bus 0 的 1 MB ECAM 已由 port 静态映射（PS=20 一条），所以这里
 *          就是逐设备读一个 vendor ID —— 不登记、不撤销任何映射。
 *          用不到的那 31 个设备读到的都是 0xFFFFFFFF（ECAM 上没设备时
 *          返回全 1），落不到厂商号上。
 * ============================================================ */

static int32 pci_scan(void)
{
    uint32 dev;

    for(dev = 0u; dev < 32u; dev++)
    {
        uint32 page = NET_ECAM_BASE + (dev << 15);
        uint32 id = net_rd32(page + PCI_CFG_VENDOR);

        if(((id & 0xFFFFu) == NET_VIRTIO_VENDOR) &&
           (((id >> 16) == NET_VIRTIO_DEV_MODERN) || ((id >> 16) == NET_VIRTIO_DEV_TRANS)))
        {
            /* 命中：配置页基址留着，后面所有配置空间访问都走它。 */
            s_cfg = page;
            s_dev = dev;
            return 0;
        }
    }

    return -2;                          /* 总线上没有 virtio-net */
}

/* ============================================================
 * 5. BAR：探测大小 + 分配地址 + 映射窗口
 * ============================================================ */

/* 探查一个 BAR 的大小。返回 0 表示"这个 BAR 没实现"（全 0 或全 1）。
 * is64 非 0 时把 BAR 对当作 64 位地址处理（只探低 32 位的掩码，高 32 位
 * 在 QEMU 的 virtio 上恒为 0，写 0 即可）。 */
static uint32 pci_bar_size(uint8 off, uint32 is64, uint32 *mask_hi)
{
    uint32 orig_lo;
    uint32 orig_hi = 0u;
    uint32 mask;

    orig_lo = pci_rd32(off);
    if(is64 != 0u)
    {
        orig_hi = pci_rd32((uint8)(off + 4u));
        pci_wr32((uint8)(off + 4u), 0xFFFFFFFFu);
    }

    pci_wr32(off, 0xFFFFFFFFu);
    mask = pci_rd32(off);
    pci_wr32(off, orig_lo);

    if(is64 != 0u)
    {
        *mask_hi = pci_rd32((uint8)(off + 4u));
        pci_wr32((uint8)(off + 4u), orig_hi);
    }
    else
    {
        *mask_hi = 0u;
    }

    return mask;
}

static int32 pci_bar_map(uint8 off, uint32 *out_base, uint32 *out_size)
{
    uint32 orig_lo;
    uint32 mask;
    uint32 mask_hi = 0u;
    uint32 kind;
    uint32 is64;
    uint32 size;
    uint32 base = NET_PCI_MEM_BASE;

    orig_lo = pci_rd32(off);
    kind = (orig_lo >> 1) & 3u;
    if(kind == 1u)
    {
        return -3;                      /* I/O BAR：LoongArch 没有 port I/O */
    }

    is64 = (kind == 2u) ? 1u : 0u;
    mask = pci_bar_size(off, is64, &mask_hi);
    if((mask == 0u) && (mask_hi == 0u))
    {
        return -3;                      /* BAR 未实现 */
    }

    size = (~(mask & 0xFFFFFFF0u)) + 1u;
    if((size == 0u) || ((size & (size - 1u)) != 0u) || (size < 0x1000u))
    {
        return -3;                      /* 大小不是 2 的幂 / 比一页还小：不猜 */
    }
    if((base & (size - 1u)) != 0u)
    {
        return -3;                      /* 窗口起点没对齐到 BAR 大小：不猜 */
    }

    /* 落下地址：先写高 32 位（为 0），再写低 32 位。顺序反了会出现
     * "高半还是旧值"的中间态，QEMU 上表现为 BAR 读回对不上。 */
    if(is64 != 0u)
    {
        pci_wr32((uint8)(off + 4u), 0u);
    }
    pci_wr32(off, base);

    if((pci_rd32(off) & ~0xFu) != (base & ~0xFu))
    {
        return -4;                      /* 回读不符：地址没落上，不假装成功 */
    }

    /* 地址落上了，但还得落进 port 静态映射过的那段窗口里：窗口是板级常量
     * （ARKRTOS_QEMU_PCI_MMIO_BASE/SIZE），驱动不会为它扩映射 —— 落到窗口外
     * 就意味着第一次访问设备寄存器就 refill。宁可在 probe 阶段失败。 */
    if((base < NET_PCI_MEM_BASE) ||
       ((base + size) > (NET_PCI_MEM_BASE + (uint32)ARKRTOS_QEMU_PCI_MMIO_SIZE)))
    {
        return -5;                      /* 静态窗口装不下这个 BAR */
    }

    *out_base = base;
    *out_size = size;
    return 0;
}

/* ============================================================
 * 6. virtio capability 解析
 * ============================================================ */

/* 找到四段区域（common / notify / device / isr）。全部落在我们分配过的
 * BAR 里才继续——别的 BAR 我们没映射，读它就是读黑洞。 */
static int32 virtio_caps(uint32 bar_base, uint32 bar_size)
{
    uint16 status = pci_rd16((uint8)PCI_CFG_STATUS);
    uint8 cap;
    uint32 guard = 0u;

    s_common = 0u;
    s_notify = 0u;
    s_devcfg = 0u;
    s_isr = 0u;
    s_notify_mul = 0u;

    if((status & PCI_STATUS_CAP_LIST) == 0u)
    {
        return -6;                      /* 没有 capability 链 = 只有传统接口 */
    }

    cap = (uint8)(pci_rd8((uint8)PCI_CFG_CAP) & 0xFCu);

    while((cap != 0u) && (guard < 48u))
    {
        uint8 id = pci_rd8(cap);

        if(id == PCI_CAP_ID_VNDR)
        {
            uint8  type      = pci_rd8((uint8)(cap + 3u));
            uint8  bar       = pci_rd8((uint8)(cap + 4u));
            uint32 offset    = pci_rd32((uint8)(cap + 8u));
            uint32 length    = pci_rd32((uint8)(cap + 12u));
            uint32 mul       = pci_rd32((uint8)(cap + 16u));

            /* BAR 号必须是我们分配过的那个（本驱动只分配 BAR4，见 init）。
             * 落在别的 BAR 就明确拒绝，而不是拿另一个基址凑。 */
            if((bar == 4u) && ((offset + length) <= bar_size))
            {
                if(type == VIRTIO_CFG_COMMON)
                {
                    s_common = bar_base + offset;
                }
                else if(type == VIRTIO_CFG_NOTIFY)
                {
                    s_notify = bar_base + offset;
                    s_notify_mul = mul;
                }
                else if(type == VIRTIO_CFG_ISR)
                {
                    s_isr = bar_base + offset;
                }
                else if(type == VIRTIO_CFG_DEVICE)
                {
                    s_devcfg = bar_base + offset;
                }
            }
        }

        cap = (uint8)(pci_rd8((uint8)(cap + 1u)) & 0xFCu);
        guard++;
    }

    if((s_common == 0u) || (s_notify == 0u) || (s_devcfg == 0u) || (s_isr == 0u))
    {
        return -7;                      /* 四段缺一：没有现代接口 */
    }

    return 0;
}

/* ============================================================
 * 7. 队列
 * ============================================================ */

static void net_notify(net_queue_t *q)
{
    /* 描述符/avail 先落内存，再敲设备门。少了这道屏障，设备可能先看到
     * idx 增长、后看到描述符内容，收包侧表现为"偶尔一帧长度对、内容高考"。
     * __sync_synchronize() 在 LoongArch 上生成 dbar 0，是这里需要的东西。 */
    __sync_synchronize();
    net_wr16(s_notify + (q->notify_off * s_notify_mul),
             (uint16)((q == &s_rxq) ? 0u : 1u));
}

static int32 queue_setup(net_queue_t *q, uint16 index)
{
    uint16 max;
    uint32 addr;
    uint32 i;

    net_wr16(s_common + VC_QUEUE_SEL, index);
    max = net_rd16(s_common + VC_QUEUE_SIZE);
    if(max == 0u)
    {
        return -8;                      /* 这个队列不存在 */
    }
    net_wr16(s_common + VC_QUEUE_SIZE, (uint16)NET_QUEUE_NUM);
    q->notify_off = net_rd16(s_common + VC_QUEUE_NOTIFY_OFF);

    q->avail.flags = 0u;
    q->avail.idx   = 0u;
    q->used.flags  = 0u;
    q->used.idx    = 0u;
    q->seen        = 0u;

    /* 64 位字段按两半写：现代 PCI 传输不保证 64 位单次访问可用，两半写法
     * 在任何实现上都对。 */
    addr = (uint32)(unsigned long)q->desc;
    net_wr32(s_common + VC_QUEUE_DESC, addr);
    net_wr32(s_common + VC_QUEUE_DESC + 4u, 0u);

    addr = (uint32)(unsigned long)&q->avail;
    net_wr32(s_common + VC_QUEUE_AVAIL, addr);
    net_wr32(s_common + VC_QUEUE_AVAIL + 4u, 0u);

    addr = (uint32)(unsigned long)&q->used;
    net_wr32(s_common + VC_QUEUE_USED, addr);
    net_wr32(s_common + VC_QUEUE_USED + 4u, 0u);

    net_wr16(s_common + VC_QUEUE_ENABLE, 1u);

    for(i = 0u; i < NET_QUEUE_NUM; i++)
    {
        q->desc[i].addr_lo = 0u;
        q->desc[i].addr_hi = 0u;
        q->desc[i].len     = 0u;
        q->desc[i].flags   = 0u;
        q->desc[i].next    = 0u;
    }

    return 0;
}

/* 接收环的初始投喂：16 个描述符全部指向自己的缓冲、全部挂进 avail。 */
static void rx_prime(void)
{
    uint32 i;

    for(i = 0u; i < NET_QUEUE_NUM; i++)
    {
        uint32 addr = (uint32)(unsigned long)s_rx_buf[i];

        s_rxq.desc[i].addr_lo = addr;
        s_rxq.desc[i].addr_hi = 0u;
        s_rxq.desc[i].len     = NET_RX_BUF_SIZE;
        s_rxq.desc[i].flags   = VQ_DESC_F_WRITE;
        s_rxq.desc[i].next    = 0u;

        s_rxq.avail.ring[i] = (uint16)i;
    }
    s_rxq.avail.idx = (uint16)NET_QUEUE_NUM;
    net_notify(&s_rxq);
}

/* ============================================================
 * 8. 初始化
 * ============================================================ */

static int32 net_bringup(void)
{
    uint32 bar_base = 0u;
    uint32 bar_size = 0u;
    uint32 lo;
    uint32 hi;
    uint8  status;
    uint32 spin;
    int32  rc;

    rc = pci_scan();
    if(rc != 0)
    {
        arkrtos_board_puts("[net] virtio-net: no device on pci bus 0\n");
        return rc;
    }

    arkrtos_board_puts("[net] virtio-net: found at pci 00:");
    if(s_dev < 10u)
    {
        arkrtos_board_puts("0");
    }
    arkrtos_board_putu(s_dev);
    arkrtos_board_puts(".0\n");

    /* 先把 MEM 译码与总线主控打开：不开 BUS_MASTER，设备写不回想读的
     * 描述符，症状是"发出去的包永远不完成"。 */
    pci_wr16((uint8)PCI_CFG_COMMAND,
             (uint16)(pci_rd16((uint8)PCI_CFG_COMMAND) |
                      PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER));

    rc = pci_bar_map((uint8)(PCI_CFG_BAR0 + 16u), &bar_base, &bar_size);
    if(rc != 0)
    {
        arkrtos_board_puts("[net] virtio-net: BAR4 map failed rc=");
        arkrtos_board_putu((uint32)(-rc));
        arkrtos_board_puts("\n");
        return rc;
    }

    arkrtos_board_puts("[net] virtio-net: bar4=");
    arkrtos_board_puthex(bar_base);
    arkrtos_board_puts(" size=");
    arkrtos_board_puthex(bar_size);
    arkrtos_board_puts("\n");

    rc = virtio_caps(bar_base, bar_size);
    if(rc != 0)
    {
        arkrtos_board_puts("[net] virtio-net: modern caps missing rc=");
        arkrtos_board_putu((uint32)(-rc));
        arkrtos_board_puts("\n");
        return rc;
    }

    /* 复位 + 状态机：ACK -> DRIVER -> 特性 -> FEATURES_OK -> DRIVER_OK。
     * 顺序是规范规定的，跳一步设备就拒绝置 FEATURES_OK。 */
    net_wr8(s_common + VC_DEV_STATUS, 0u);
    for(spin = 0u; spin < 100000u; spin++)
    {
        if(net_rd8(s_common + VC_DEV_STATUS) == 0u)
        {
            break;
        }
    }
    if(net_rd8(s_common + VC_DEV_STATUS) != 0u)
    {
        arkrtos_board_puts("[net] virtio-net: reset timeout\n");
        return -9;
    }

    net_wr8(s_common + VC_DEV_STATUS, VIRTIO_STATUS_ACK);
    net_wr8(s_common + VC_DEV_STATUS,
            (uint8)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER));

    net_wr32(s_common + VC_DEV_FEAT_SEL, 0u);
    lo = net_rd32(s_common + VC_DEV_FEAT);
    net_wr32(s_common + VC_DEV_FEAT_SEL, 1u);
    hi = net_rd32(s_common + VC_DEV_FEAT);

    if((hi & VIRTIO_F_VERSION_1_HI) == 0u)
    {
        arkrtos_board_puts("[net] virtio-net: device has no VERSION_1\n");
        return -10;
    }
    if((lo & VIRTIO_NET_F_MAC) == 0u)
    {
        arkrtos_board_puts("[net] virtio-net: device has no MAC feature\n");
        return -11;
    }

    net_wr32(s_common + VC_DRV_FEAT_SEL, 0u);
    net_wr32(s_common + VC_DRV_FEAT, VIRTIO_NET_F_MAC);
    net_wr32(s_common + VC_DRV_FEAT_SEL, 1u);
    net_wr32(s_common + VC_DRV_FEAT, VIRTIO_F_VERSION_1_HI);

    status = (uint8)(net_rd8(s_common + VC_DEV_STATUS) | VIRTIO_STATUS_FEAT_OK);
    net_wr8(s_common + VC_DEV_STATUS, status);

    if((net_rd8(s_common + VC_DEV_STATUS) & VIRTIO_STATUS_FEAT_OK) == 0u)
    {
        arkrtos_board_puts("[net] virtio-net: FEATURES_OK rejected\n");
        return -12;
    }

    /* MAC 来自设备配置区（已经在特性里买了 MAC，所以这 6 字节是设备给的，
     * 不是我们编的）。 */
    s_mac[0] = net_rd8(s_devcfg + 0u);
    s_mac[1] = net_rd8(s_devcfg + 1u);
    s_mac[2] = net_rd8(s_devcfg + 2u);
    s_mac[3] = net_rd8(s_devcfg + 3u);
    s_mac[4] = net_rd8(s_devcfg + 4u);
    s_mac[5] = net_rd8(s_devcfg + 5u);

    rc = queue_setup(&s_rxq, 0u);
    if(rc != 0)
    {
        arkrtos_board_puts("[net] virtio-net: rx queue setup failed\n");
        return rc;
    }
    rc = queue_setup(&s_txq, 1u);
    if(rc != 0)
    {
        arkrtos_board_puts("[net] virtio-net: tx queue setup failed\n");
        return rc;
    }

    rx_prime();

    net_wr8(s_common + VC_DEV_STATUS,
            (uint8)(net_rd8(s_common + VC_DEV_STATUS) | VIRTIO_STATUS_DRIVER_OK));

    s_ready = 1u;

    arkrtos_board_puts("[net] virtio-net: mac=");
    {
        uint32 i;

        for(i = 0u; i < 6u; i++)
        {
            arkrtos_board_puthex((uint32)s_mac[i]);
            if(i != 5u)
            {
                arkrtos_board_putc(':');
            }
        }
    }
    arkrtos_board_puts(" queues=2 status=ok\n");

    return 0;
}

int32 arkrtos_net_dev_init(void)
{
    if(s_init_tried != 0u)
    {
        return s_init_rc;
    }

    s_init_tried = 1u;
    s_init_rc = net_bringup();

    if(s_init_rc != 0)
    {
        s_ready = 0u;
    }

    return s_init_rc;
}

/* ============================================================
 * 9. 收发
 * ============================================================ */

const uint8 *arkrtos_net_dev_mac(void)
{
    return (s_ready != 0u) ? s_mac : 0;
}

int32 arkrtos_net_dev_link_up(void)
{
    return (s_ready != 0u) ? 1 : 0;
}

const char *arkrtos_net_dev_name(void)
{
    return (s_ready != 0u) ? "virtio-net" : "none";
}

void arkrtos_net_dev_stats(uint32 *tx_ok, uint32 *tx_drop,
                           uint32 *rx_ok, uint32 *rx_drop)
{
    if(tx_ok != 0)
    {
        *tx_ok = s_tx_ok;
    }
    if(tx_drop != 0)
    {
        *tx_drop = s_tx_drop;
    }
    if(rx_ok != 0)
    {
        *rx_ok = s_rx_ok;
    }
    if(rx_drop != 0)
    {
        *rx_drop = s_rx_drop;
    }
}

int32 arkrtos_net_dev_tx(const uint8 *frame, uint16 len)
{
    uint32 i;
    uint32 n;
    uint16 used_idx;
    uint32 addr;

    if((s_ready == 0u) || (frame == 0) || (len == 0u) ||
       (len > ARKRTOS_NET_DEV_FRAME_MAX))
    {
        s_tx_drop++;
        return -1;
    }

    /* 先把设备已经还回来的描述符记账掉。used.idx 是 16 位的，比较用减法
     * 才不会在回绕处误判"环满"。 */
    used_idx = s_txq.used.idx;
    n = (uint32)(uint16)(used_idx - (uint16)s_txq.seen);
    if(n > 0u)
    {
        s_txq.seen += n;
        if(n <= s_tx_inflight)
        {
            s_tx_inflight -= n;
        }
        else
        {
            s_tx_inflight = 0u;
        }
    }

    if(s_tx_inflight >= NET_QUEUE_NUM)
    {
        s_tx_drop++;                    /* 环满：丢掉，不阻塞调用者 */
        return -1;
    }

    i = (uint32)(s_txq.avail.idx % NET_QUEUE_NUM);

    /* 12 字节 virtio-net 头全 0：不用 MRG_RXBUF 时 num_buffers 无意义，
     * 其余字段（checksum/gso）我们不买对应特性，必须为 0。 */
    for(n = 0u; n < VN_HDR_LEN; n++)
    {
        s_tx_buf[i][n] = 0u;
    }
    for(n = 0u; n < (uint32)len; n++)
    {
        s_tx_buf[i][VN_HDR_LEN + n] = frame[n];
    }

    addr = (uint32)(unsigned long)s_tx_buf[i];
    s_txq.desc[i].addr_lo = addr;
    s_txq.desc[i].addr_hi = 0u;
    s_txq.desc[i].len     = VN_HDR_LEN + (uint32)len;
    s_txq.desc[i].flags   = 0u;
    s_txq.desc[i].next    = 0u;

    s_txq.avail.ring[s_txq.avail.idx % NET_QUEUE_NUM] = (uint16)i;
    s_txq.avail.idx = (uint16)(s_txq.avail.idx + 1u);
    s_tx_inflight++;

    net_notify(&s_txq);

    s_tx_ok++;
    (void)s_isr;                        /* ISR 只在中断模式里读；轮询下不去清它 */
    return 0;
}

int32 arkrtos_net_dev_rx(uint8 *buf, uint16 max_len, uint16 *out_len)
{
    net_used_elem_t e;
    uint32 id;
    uint32 len;
    uint32 flen;
    uint32 n;

    if((s_ready == 0u) || (buf == 0) || (out_len == 0))
    {
        return -1;
    }

    n = (uint32)(uint16)(s_rxq.used.idx - (uint16)s_rxq.seen);
    if(n == 0u)
    {
        return 0;
    }

    e = s_rxq.used.ring[s_rxq.seen % NET_QUEUE_NUM];
    s_rxq.seen++;

    id = e.id;
    len = e.len;

    if(id >= NET_QUEUE_NUM)
    {
        s_rx_drop++;
        return -1;                      /* 描述符号越界：驱动状态坏了，不猜 */
    }

    flen = (len > VN_HDR_LEN) ? (len - VN_HDR_LEN) : 0u;

    if((flen == 0u) || (flen > (uint32)max_len))
    {
        s_rx_drop++;
    }
    else
    {
        for(n = 0u; n < flen; n++)
        {
            buf[n] = s_rx_buf[id][VN_HDR_LEN + n];
        }
        *out_len = (uint16)flen;
        s_rx_ok++;
    }

    /* 这一帧之后立刻把同一个描述符重新挂回去：接收环不能边收边漏，
     * 空着不补会让后续帧被设备丢掉（QEMU 上表现为"ping 三发只回一发"）。 */
    s_rxq.desc[id].len   = NET_RX_BUF_SIZE;
    s_rxq.desc[id].flags = VQ_DESC_F_WRITE;
    s_rxq.avail.ring[s_rxq.avail.idx % NET_QUEUE_NUM] = (uint16)id;
    s_rxq.avail.idx = (uint16)(s_rxq.avail.idx + 1u);
    net_notify(&s_rxq);

    return (flen == 0u || flen > (uint32)max_len) ? -1 : 1;
}

#endif /* ARKRTOS_USE_NET_VIRTIO */
