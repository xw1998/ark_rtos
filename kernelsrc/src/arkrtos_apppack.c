/**
* @file arkrtos_apppack.c
* @brief 四遍链接结果的差分打包（纯计算，见 arkrtos_apppack.h 的设计说明）
* @details 三个「为什么这么写」值得单独留在这里：
*
*          1. **按 Thumb 指令长度扫描，而不是按 4 字节字扫描。**
*             Thumb-2 指令是半字对齐、长度 2 或 4 字节的；32 位绝对地址既可能
*             以整字形式躺在常量池里，也可能被拆成 MOVW（低 16 位）+ MOVT
*             （高 16 位）两条指令。按 4 字节字硬扫会跨过指令边界，把一个
*             立即数对拆成两半（一半落进前一条指令的尾巴），差分出来既不是
*             整字平移也不是指令平移，只能误报「不可重定位」。
*
*          2. **表项的偏移先按「相对负载」记账，最后统一加表长。**
*             重定位表本身就在负载前面，表项数决定了负载起点（表越长负载越
*             靠后），所以「绝对偏移」在差分阶段还不存在。先按相对偏移编码，
*             数完再整体加 (payload_offset / 2) << 2：编码里低 2 位是类型，
*             加一个 4 的倍数正好只动偏移字段，不会污染类型。
*
*          3. **验证不是「再算一遍」，而是把表真的应用到 A 上再逐字节比 D。**
*             同一份代码算两遍得到同样的表，只能证明算法是确定的，不能证明
*             表是对的；把表打回字节里跟 D 比，才证明「按这张表搬移后的镜像
*             就是换基址重新链接出来的那一份」。
*
* @author xw
* @date 2026.11.05
*/
#include "arkrtos_apppack.h"
#include "arkrtos_loader.h"      /* 只为 crc 的覆盖面与装载器同源 */
#include "arkrtos_partition.h"    /* ARKRTOS_HW_COMPAT_ID / SLOT_RAM_MAX_BLOCK */

#if (ARKRTOS_USE_APPPACK == 1)

#define APPPACK_KIND_ROM       ((uint32)ARKRTOS_APP_RELOC_KIND_ROM)       /* 0 */
#define APPPACK_KIND_RAM       ((uint32)ARKRTOS_APP_RELOC_KIND_RAM)       /* 1 */
#define APPPACK_KIND_ROM_MOVW  ((uint32)ARKRTOS_APP_RELOC_KIND_ROM_MOVW)  /* 2 */
#define APPPACK_KIND_RAM_MOVW  ((uint32)ARKRTOS_APP_RELOC_KIND_RAM_MOVW)  /* 3 */

/* ---------------------------------------------------------------- 字节读写 */

static uint32 apppack_rd16(const uint8 *p)
{
    return (uint32)p[0] | ((uint32)p[1] << 8);
}

static uint32 apppack_rd32(const uint8 *p)
{
    return apppack_rd16(p) | (apppack_rd16(p + 2) << 16);
}

static void apppack_wr32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
    p[2] = (uint8)((v >> 16) & 0xFFu);
    p[3] = (uint8)((v >> 24) & 0xFFu);
}

/* ---------------------------------------------------------- Thumb 指令识别 */

/* 32 位 Thumb 指令的前缀半字。三种前缀对应 Thumb-2 的三组编码空间。 */
static uint32 apppack_is32(uint32 h)
{
    if((h & 0xF800u) == 0xE800u) { return 1u; }
    if((h & 0xF800u) == 0xF000u) { return 1u; }
    if((h & 0xF800u) == 0xF800u) { return 1u; }
    return 0u;
}

/* 前缀半字是 MOVW(0x24) 还是 MOVT(0x2C)；都不是回 0。 */
static uint32 apppack_mov_kind(uint32 h)
{
    uint32 t;

    if((h & 0xF800u) != 0xF000u)
    {
        return 0u;
    }

    t = (h >> 4) & 0x3Fu;

    if(t == 0x24u) { return 1u; }   /* MOVW */
    if(t == 0x2Cu) { return 2u; }   /* MOVT */
    return 0u;
}

static void apppack_mov_decode(uint32 h1, uint32 h2, uint32 *rd, uint32 *imm)
{
    uint32 imm4 = h1 & 0xFu;
    uint32 i    = (h1 >> 10) & 1u;
    uint32 imm3 = (h2 >> 12) & 7u;
    uint32 imm8 = h2 & 0xFFu;

    *rd  = (h2 >> 8) & 0xFu;
    *imm = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
}

static void apppack_mov_encode(uint32 h1, uint32 h2, uint32 imm,
                              uint32 *o1, uint32 *o2)
{
    uint32 rd   = (h2 >> 8) & 0xFu;
    uint32 imm4 = (imm >> 12) & 0xFu;
    uint32 i    = (imm >> 11) & 1u;
    uint32 imm3 = (imm >> 8) & 7u;
    uint32 imm8 = imm & 0xFFu;

    *o1 = (h1 & 0xFBF0u) | (i << 10) | imm4;
    *o2 = (imm3 << 12) | (rd << 8) | imm8;
}

/* pos 处若是相邻的 MOVW+MOVT（同一个 Rd），拼出 32 位值。
 * 返回 1=是且 *value 有效；0=不是。 */
static uint32 apppack_read_pair(const uint8 *buf, uint32 len, uint32 pos,
                              uint32 *value)
{
    uint32 h1w;
    uint32 h1t;
    uint32 rd_w;
    uint32 rd_t;
    uint32 imm_w;
    uint32 imm_t;

    if((pos + ARKRTOS_APP_RELOC_MOV_SIZE) > len)
    {
        return 0u;
    }

    h1w = apppack_rd16(buf + pos);
    h1t = apppack_rd16(buf + pos + 4u);

    if((apppack_mov_kind(h1w) != 1u) || (apppack_mov_kind(h1t) != 2u))
    {
        return 0u;
    }

    apppack_mov_decode(h1w, apppack_rd16(buf + pos + 2u), &rd_w, &imm_w);
    apppack_mov_decode(h1t, apppack_rd16(buf + pos + 6u), &rd_t, &imm_t);

    if(rd_w != rd_t)
    {
        return 0u;
    }

    *value = (imm_t << 16) | imm_w;
    return 1u;
}

/* 把 32 位值写回 pos 处的 MOVW/MOVT 对（Rd 与指令种类保持不变）。
 * 调用前必须 apppack_read_pair 成功过。 */
static void apppack_write_pair(const uint8 *src, uint8 *dst, uint32 pos,
                             uint32 value)
{
    uint32 o1;
    uint32 o2;

    apppack_mov_encode(apppack_rd16(src + pos), apppack_rd16(src + pos + 2u),
                      value & 0xFFFFu, &o1, &o2);
    dst[pos]      = (uint8)(o1 & 0xFFu);
    dst[pos + 1u] = (uint8)((o1 >> 8) & 0xFFu);
    dst[pos + 2u] = (uint8)(o2 & 0xFFu);
    dst[pos + 3u] = (uint8)((o2 >> 8) & 0xFFu);

    apppack_mov_encode(apppack_rd16(src + pos + 4u), apppack_rd16(src + pos + 6u),
                      (value >> 16) & 0xFFFFu, &o1, &o2);
    dst[pos + 4u] = (uint8)(o1 & 0xFFu);
    dst[pos + 5u] = (uint8)((o1 >> 8) & 0xFFu);
    dst[pos + 6u] = (uint8)(o2 & 0xFFu);
    dst[pos + 7u] = (uint8)((o2 >> 8) & 0xFFu);
}

/* ---------------------------------------------------------------- 差分扫描 */

typedef struct {
    uint32 n_rom;
    uint32 n_ram;
    uint32 n_rom_movw;
    uint32 n_ram_movw;
} apppack_count_t;

static uint32 apppack_is_movw_kind(uint32 kind)
{
    return ((kind == APPPACK_KIND_ROM_MOVW) || (kind == APPPACK_KIND_RAM_MOVW))
           ? 1u : 0u;
}

/**
* @brief 差分一遍：把 (相对偏移, 类型) 编码进 table，或只计数
* @param table 非 0 时逐个写入编码后的表项（偏移相对负载，未加表长）
* @param stats 非 0 时按类型计数（table 为 0 的纯计数模式也用它）
*/
static int32 apppack_scan(const uint8 *a, const uint8 *b, const uint8 *c,
                        uint32 len, uint32 delta,
                        uint32 *table, apppack_count_t *count)
{
    uint32 pos = 0u;
    uint32 n = 0u;

    while(pos < len)
    {
        uint32 kind = 0u;
        uint32 hit  = 0u;   /* 「这一步认出了表项」的标志。不能用 kind != 0 代替：
                             * ROM 类表项的种类值本来就是 0，混用会把整字 ROM 表项
                             * 当成「没命中」静默丢掉，最后在别处报 DIFF。 */
        uint32 adv;
        uint32 ha;
        uint32 hb;
        uint32 hc;

        /* ---- 0) 先判 MOVW/MOVT 立即数对（32 位 Thumb 指令，跨两个半字）----
         * 必须排在整字判之前：MOVT 的高 16 位里只有 imm8 随基址变，站在 4 字节
         * 整字视角看恰好等于「+delta」，会被误当成一条 ROM 数据字表项；而 MOVT
         * 的起点常落在 2 mod 4 上，装载器按对齐规则本来就会拒收这种表项。
         * 只有三份里真的变了才按指令处理：三份一致的 MOVW/MOVT 对是普通常量，
         * 让它落到下面按指令长度正常跳过，免得把数据字误读成指令。 */
        if((pos + ARKRTOS_APP_RELOC_MOV_SIZE) <= len)
        {
            uint32 va = 0u;
            uint32 vb = 0u;
            uint32 vc = 0u;

            if((apppack_read_pair(a, len, pos, &va) != 0u) &&
               (apppack_read_pair(b, len, pos, &vb) != 0u) &&
               (apppack_read_pair(c, len, pos, &vc) != 0u))
            {
                uint32 db = (vb - va) & 0xFFFFFFFFu;
                uint32 dc = (vc - va) & 0xFFFFFFFFu;

                if((db != 0u) || (dc != 0u))
                {
                    if((db == delta) && (dc == 0u))
                    {
                        kind = APPPACK_KIND_ROM_MOVW;
                    }
                    else if((dc == delta) && (db == 0u))
                    {
                        kind = APPPACK_KIND_RAM_MOVW;
                    }
                    else
                    {
                        return ARKRTOS_APPPACK_ERR_DIFF;
                    }

                    if((pos % 2u) != 0u)
                    {
                        return ARKRTOS_APPPACK_ERR_DIFF;
                    }

                    if(table != 0)
                    {
                        table[n] = (pos / ARKRTOS_APP_RELOC_OFF_UNIT) << 2 | kind;
                    }
                    n++;

                    if(kind == APPPACK_KIND_ROM_MOVW) { count->n_rom_movw++; }
                    else                              { count->n_ram_movw++; }

                    pos += ARKRTOS_APP_RELOC_MOV_SIZE;
                    continue;
                }
            }
        }

        /* ---- 1) 先按 4 字节纯数据字判：常量池里的指针、常量表 ---- */
        if((pos + 4u) <= len)
        {
            uint32 wa = apppack_rd32(a + pos);
            uint32 wb = apppack_rd32(b + pos);
            uint32 wc = apppack_rd32(c + pos);
            uint32 db = (wb - wa) & 0xFFFFFFFFu;
            uint32 dc = (wc - wa) & 0xFFFFFFFFu;

            if((db != 0u) || (dc != 0u))
            {
                if((db == delta) && (dc == 0u))
                {
                    kind = APPPACK_KIND_ROM;
                    hit  = 1u;
                }
                else if((dc == delta) && (db == 0u))
                {
                    kind = APPPACK_KIND_RAM;
                    hit  = 1u;
                }
                else
                {
                    /* 差了一个非 delta 的量：可能正好是 MOVW 立即数被改写了，
                     * 留到下面按指令解释；这里不能直接判失败。 */
                    hit = 0u;
                }

                if(hit != 0u)
                {
                    /* 整字表项要 4 字节对齐（装载器的表检查会拒绝非对齐项）；
                     * 对齐不了的说明这个字不是独立的地址槽，交给指令解释。 */
                    if((pos % ARKRTOS_APP_RELOC_SIZE) == 0u)
                    {
                        if(table != 0)
                        {
                            table[n] = (pos / ARKRTOS_APP_RELOC_OFF_UNIT) << 2 | kind;
                        }
                        n++;
                        if(kind == APPPACK_KIND_ROM) { count->n_rom++; }
                        else                          { count->n_ram++; }
                        pos += 4u;
                        continue;
                    }
                    hit = 0u;
                }
            }
        }

        /* ---- 2) 三份完全一样：按指令长度走到下一条 ---- */
        ha = apppack_rd16(a + pos);
        hb = apppack_rd16(b + pos);
        hc = apppack_rd16(c + pos);

        if((ha == hb) && (ha == hc))
        {
            adv = (apppack_is32(ha) != 0u) ? 4u : 2u;

            /* 半字一样、但整字不同的情况已经被第 1 步处理；这里能确定整条
             * 指令（或半个字）三份一致，直接跳过。 */
            pos += adv;
            continue;
        }

        /* ---- 3) 指令本身不同：只能是 MOVW/MOVT 立即数对被链接期改写 ---- */
        {
            uint32 va = 0u;
            uint32 vb = 0u;
            uint32 vc = 0u;
            uint32 db;
            uint32 dc;

            if((apppack_read_pair(a, len, pos, &va) == 0u) ||
               (apppack_read_pair(b, len, pos, &vb) == 0u) ||
               (apppack_read_pair(c, len, pos, &vc) == 0u))
            {
                return ARKRTOS_APPPACK_ERR_DIFF;
            }

            db = (vb - va) & 0xFFFFFFFFu;
            dc = (vc - va) & 0xFFFFFFFFu;

            if((db == delta) && (dc == 0u))
            {
                kind = APPPACK_KIND_ROM_MOVW;
                count->n_rom_movw++;
            }
            else if((dc == delta) && (db == 0u))
            {
                kind = APPPACK_KIND_RAM_MOVW;
                count->n_ram_movw++;
            }
            else
            {
                return ARKRTOS_APPPACK_ERR_DIFF;
            }

            if((pos % 2u) != 0u)
            {
                return ARKRTOS_APPPACK_ERR_DIFF;
            }

            if(table != 0)
            {
                table[n] = (pos / ARKRTOS_APP_RELOC_OFF_UNIT) << 2 | kind;
            }
            n++;
            pos += ARKRTOS_APP_RELOC_MOV_SIZE;
        }
    }

    return (int32)n;
}

/* ------------------------------------------------------- 把表应用到负载副本 */

static void apppack_apply(uint8 *buf, uint32 len, uint32 pos, uint32 kind,
                        uint32 addend)
{
    if(apppack_is_movw_kind(kind) == 0u)
    {
        uint32 w = apppack_rd32(buf + pos);
        apppack_wr32(buf + pos, (w + addend) & 0xFFFFFFFFu);
    }
    else
    {
        uint32 v = 0u;
        (void)apppack_read_pair(buf, len, pos, &v);
        apppack_write_pair(buf, buf, pos, (v + addend) & 0xFFFFFFFFu);
    }
}

/* ---------------------------------------------------------------- 入口 */

static uint32 apppack_is_pow2(uint32 v)
{
    return ((v != 0u) && ((v & (v - 1u)) == 0u)) ? 1u : 0u;
}

const char *arkrtos_apppack_error_name(int32 code)
{
    switch(code)
    {
        case 0:                          return "ok";
        case ARKRTOS_APPPACK_ERR_PARAM:  return "invalid arguments (pointer / window / length)";
        case ARKRTOS_APPPACK_ERR_DIFF:   return "a payload offset cannot be explained by one base delta (not relocatable)";
        case ARKRTOS_APPPACK_ERR_VERIFY: return "relocating A with the built table does not reproduce D";
        case ARKRTOS_APPPACK_ERR_SIZE:   return "relocation table or output buffer too small";
        case ARKRTOS_APPPACK_ERR_ENTRY:  return "no usable entry: +0/+4 is not a vector table and "
                                                "+0 is not a mini start-stub prologue (0xB5E0)";
        default:                         return "unknown";
    }
}

int32 arkrtos_apppack_build(const uint8 *a, const uint8 *b, const uint8 *c,
                          const uint8 *d, uint32 len, uint32 delta,
                          const arkrtos_apppack_win_t *win, uint32 type,
                          uint32 version, uint8 *out, uint32 out_cap,
                          uint32 *out_len, arkrtos_apppack_stats_t *stats)
{
    arkrtos_app_header_t *p_hdr;
    uint32 *table;
    uint8  *payload;
    apppack_count_t count;
    uint32 n;
    uint32 i;
    uint32 payload_off;
    uint32 total;
    uint32 entry_value;
    uint32 entry_off;
    uint32 crc;
    uint32 sp;

    if((a == 0) || (b == 0) || (c == 0) || (d == 0) || (win == 0) ||
       (out == 0) || (out_len == 0))
    {
        return ARKRTOS_APPPACK_ERR_PARAM;
    }

    *out_len = 0u;

    if((len == 0u) || ((len % 4u) != 0u) ||
       (apppack_is_pow2(win->rom_size) == 0u) ||
       (apppack_is_pow2(win->ram_size) == 0u) ||
       (win->rom_size < 8u) || (win->ram_size < 8u))
    {
        return ARKRTOS_APPPACK_ERR_PARAM;
    }

    /* delta 小于任一窗口时，B/C 的地址段会与 A 重叠：重叠段里一个普通常量
     * 也可能「恰好差 delta」，那样差分就不再是「这是个绝对地址」的证据。
     * 这是判据成立的前提，不是保守。 */
    if((delta < win->rom_size) || (delta < win->ram_size) || (delta == 0u))
    {
        return ARKRTOS_APPPACK_ERR_PARAM;
    }

    if(len > win->rom_size)
    {
        /* 负载比 ROM 窗口还大：它根本装不进按这个窗口链出来的镜像，继续打包
         * 只会产出一份装上就撞坏邻居的镜像。 */
        return ARKRTOS_APPPACK_ERR_SIZE;
    }

    /* 入口有两种合法形态，只能靠负载自己的字节分辨——不能用「调用方说是
     * 小程序所以肯定没有向量表」这种声明：`cc --pack` 的同一条路径既打过
     * 带 Reset_Handler 的镜像（有表），也打小程序（没有表），源码里没有
     * 额外的入口标志位，多造一个参数就等于让调用方去猜。
     *
     *   (1) 带向量表：+0 = 初始 SP（落在 RAM 窗口内）、+4 = 入口（Thumb 位为 1、
     *       落在载荷内、且不落在表自己那 8 个字节上）。
     *
     *   (2) 平铺的小程序：没有复位向量——它是被内核当任务起来的（栈由 mini run
     *       从 RAM 块里给，见 arkrtos_mini.c），负载 +0 就是 MINISTART 的第一条
     *       指令，入口偏移恒为 0。这一种必须**正面认出**启动桩的序言
     *       （THUMB PUSH：0xB4xx / 0xB5xx，SDK 里实测 0xB5E0），认不出就报错：
     *       那说明 SRCS 的第一个不是启动桩，入口是错的，而错入口要到板上跑起来才炸。
     *
     * 顺序不能倒，也不能只留 (2)：先判 (1)，三个条件同时成立时把一份带表的
     * 镜像误判成平铺镜像是不可能的（RAM 窗口在 0x24xxxxxx，而 PUSH 的机器码
     * 是 0x0000B5E0 这一档）。
     */
    sp = apppack_rd32(a);
    entry_value = apppack_rd32(a + 4u);

    if((sp >= win->ram_base) && (sp <= (win->ram_base + win->ram_size)) &&
       ((entry_value & 1u) != 0u) &&
       ((entry_value & ~1u) >= win->rom_base) &&
       (((entry_value & ~1u) - win->rom_base) >= 8u) &&
       (((entry_value & ~1u) - win->rom_base) < len))
    {
        entry_off = (entry_value & ~1u) - win->rom_base;
    }
    else if((apppack_rd32(a) & 0xFE00u) == 0xB400u)
    {
        entry_off = 0u;
    }
    else
    {
        return ARKRTOS_APPPACK_ERR_ENTRY;
    }

    /* 第一遍只数：表有多大决定负载起点，也决定输出缓冲够不够。 */
    count.n_rom      = 0u;
    count.n_ram      = 0u;
    count.n_rom_movw = 0u;
    count.n_ram_movw = 0u;

    if(apppack_scan(a, b, c, len, delta, 0, &count) < 0)
    {
        return ARKRTOS_APPPACK_ERR_DIFF;
    }

    n = count.n_rom + count.n_ram + count.n_rom_movw + count.n_ram_movw;

    if((n == 0u) || (n > ARKRTOS_APP_RELOC_MAX))
    {
        /* n == 0 也是错：一份没有任何绝对地址的镜像，要么是函数里什么都没用
         * （那就不该打包），要么是差分根本没看到东西（判据失效）。 */
        return ARKRTOS_APPPACK_ERR_SIZE;
    }

    payload_off = ARKRTOS_APP_HEADER_SIZE + (n * ARKRTOS_APP_RELOC_SIZE);
    total       = payload_off + len;

    if((total > out_cap) || (total < payload_off))
    {
        return ARKRTOS_APPPACK_ERR_SIZE;
    }

    table   = (uint32 *)(void *)(out + ARKRTOS_APP_RELOC_OFFSET);
    payload = out + payload_off;

    /* 第二遍写表（偏移此时仍相对负载），顺手再做一次计数以便和第一遍对账：
     * 两次数出来不一样说明扫描不是确定性的，那种表不能用来打补丁。 */
    count.n_rom      = 0u;
    count.n_ram      = 0u;
    count.n_rom_movw = 0u;
    count.n_ram_movw = 0u;

    if((uint32)apppack_scan(a, b, c, len, delta, table, &count) != n)
    {
        return ARKRTOS_APPPACK_ERR_DIFF;
    }

    /* 偏移从「相对负载」改成「相对镜像」：表项低 2 位是类型，加一个 4 的
     * 倍数只动偏移字段，不会污染类型。 */
    for(i = 0u; i < n; i++)
    {
        table[i] += ((payload_off / ARKRTOS_APP_RELOC_OFF_UNIT) << 2);
    }

    /* 负载就是 A，原样搬过去；随后在这份副本上把表打一遍，必须等于 D。 */
    for(i = 0u; i < len; i++)
    {
        payload[i] = a[i];
    }

    for(i = 0u; i < n; i++)
    {
        uint32 entry = table[i];
        uint32 kind  = ARKRTOS_APP_RELOC_KIND(entry);
        uint32 off   = ARKRTOS_APP_RELOC_OFF(entry);
        uint32 addend;

        if((off < payload_off) || (off >= (total)))
        {
            return ARKRTOS_APPPACK_ERR_VERIFY;
        }

        /* D 是「ROM +2*delta、RAM +delta」链出来的那一份，所以复核时的加数
         * 与运行期装载时不同——这里验的是表本身对不对，不是落点。 */
        addend = apppack_is_movw_kind(kind) != 0u
                 ? ((kind == APPPACK_KIND_ROM_MOVW) ? (2u * delta) : delta)
                 : ((kind == APPPACK_KIND_ROM) ? (2u * delta) : delta);

        if((apppack_is_movw_kind(kind) != 0u) &&
           ((off + ARKRTOS_APP_RELOC_MOV_SIZE) > total))
        {
            return ARKRTOS_APPPACK_ERR_VERIFY;
        }

        apppack_apply(payload, len, off - payload_off, kind, addend);
    }

    for(i = 0u; i < len; i++)
    {
        if(payload[i] != d[i])
        {
            return ARKRTOS_APPPACK_ERR_VERIFY;
        }
    }

    /* 上面那份负载副本被打过补丁（等价于 D），而镜像里必须躺标称形态 A：
     * 镜像的负载就是「被链接在标称基址上的字节」，装载期才按落点打补丁。
     * 不还原的话，产出的镜像里装的是 D——它自己跟自己还是「一致」的，
     * 于是 CRC 能过，直到运行期把每个绝对地址又叠了一遍增量才崩。 */
    for(i = 0u; i < len; i++)
    {
        payload[i] = a[i];
    }

    /* 头：先整块清零，再逐字段填——reserved 区里残留上一轮的字节会让 CRC
     * 对不上，而且那种错只在上板时暴露。 */
    for(i = 0u; i < ARKRTOS_APP_HEADER_SIZE; i++)
    {
        out[i] = 0u;
    }

    p_hdr = (arkrtos_app_header_t *)(void *)out;

    p_hdr->magic            = ARKRTOS_APP_MAGIC;
    p_hdr->type             = type;
    p_hdr->hw_compat_id     = ARKRTOS_HW_COMPAT_ID;
    p_hdr->version          = version;
    p_hdr->image_size       = len;
    p_hdr->entry_offset     = entry_off;
    p_hdr->nominal_base     = win->rom_base;
    p_hdr->crc32            = 0u;
    p_hdr->flags            = ARKRTOS_APP_FLAG_NONE;
    p_hdr->state            = ARKRTOS_APP_STATE_VALID;
    p_hdr->image_id         = 0u;
    p_hdr->ram_size         = win->ram_size;
    p_hdr->reloc_offset     = ARKRTOS_APP_RELOC_OFFSET;
    p_hdr->reloc_count      = n;
    p_hdr->reloc_kind       = ARKRTOS_APP_RELOC_KIND_CUR;
    p_hdr->payload_offset   = payload_off;
    p_hdr->nominal_ram_base = win->ram_base;
    p_hdr->runtime_ram_base = 0u;

    /* image_id 是**负载**的 CRC（同 tools/pack_app.py / 镜像头的字段说明），
     * 不是镜像 CRC；而且它落在头部被 CRC 覆盖的区间里，所以必须**在算头 CRC
     * 之前**填好——先算 CRC 再回填 image_id，等于算完就把它改坏，
     * 装载路只会报一个 CRC 不符（-5），看不出是谁改的。 */
    p_hdr->image_id = arkrtos_crc32(payload, len, 0u);

    /* CRC 的覆盖顺序与装载器同源：头（crc32 / state / runtime_ram_base 按 0）
     * → 重定位表 → 负载。顺序不能变，变了每一次装载都会报 CRC 不符。 */
    crc = arkrtos_loader_crc_header(p_hdr);
    crc = arkrtos_crc32(table, n * ARKRTOS_APP_RELOC_SIZE, crc);
    crc = arkrtos_crc32(payload, len, crc);

    p_hdr->crc32 = crc;

    *out_len = total;

    if(stats != 0)
    {
        stats->n_rom       = count.n_rom;
        stats->n_ram       = count.n_ram;
        stats->n_rom_movw  = count.n_rom_movw;
        stats->n_ram_movw  = count.n_ram_movw;
        stats->entry_offset = entry_off;
        stats->image_size  = len;
    }

    return 0;
}

#endif /* ARKRTOS_USE_APPPACK */
