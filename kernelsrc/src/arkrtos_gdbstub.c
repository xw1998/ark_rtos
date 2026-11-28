/**
* @file arkrtos_gdbstub.c
* @brief GDB 远程串行协议（RSP）目标侧代理（默认关）
* @details 支持的包与**明确不支持**的包写在 arkrtos_gdbstub.h 的文件头里。
*          这里只强调三条实现纪律：
*
*          1) 一个字节一个字节吃：状态机只有「等 $ / 包体内 / 等两个校验
*             和字符」四态，不假设底层传输一次给整包。串口、环回、宿主
*             用例喂字节走的都是同一条路径；
*          2) 超长包丢弃到 `#` 再回 `-`：不允许一个超长请求把缓冲写穿；
*          3) 不支持就回空包（`$#00`），不编造 `OK`。gdb 把空包当"不支持"，
*             而 `OK` 会被它当成功 —— 那是假绿。
*
* @note 关档（ARKRTOS_USE_GDBSTUB=0）下本文件是一个空翻译单元。
*/

#include "arkrtos_types.h"
#include "arkrtos_features.h"

#if (ARKRTOS_USE_GDBSTUB == 1)

#include "arkrtos_gdbstub.h"

/* 回包缓冲：包体上限的 hex 展开 + 帧头尾。寄存器整包（g）也走这里，
 * 所以再留一点余量。 */
#define GS_OUT_MAX      (ARKRTOS_GDBSTUB_PKT_MAX * 2u + 16u)
/* 监视命令的原始输出缓冲（hex 编码前） */
#define GS_MON_MAX      ARKRTOS_GDBSTUB_MON_MAX

/* X（二进制写）直接把包体里的数据按字节收进 s_out，不再做运行期上界
 * 检查：包体长度 n 已由 s_in 的大小（ARKRTOS_GDBSTUB_PKT_MAX）封顶，
 * 而这行编译期断言把「包体装得进输出缓冲」变成不可破坏的契约。 */
#if (ARKRTOS_GDBSTUB_PKT_MAX > GS_OUT_MAX)
#error "the packet body must fit in the output scratch buffer"
#endif

#define GS_RX_IDLE      0u
#define GS_RX_BODY      1u
#define GS_RX_CK1       2u
#define GS_RX_CK2       3u

static const arkrtos_gdbstub_ops_t *s_ops;
static arkrtos_gdbstub_stat_t s_stat;

static uint8  s_in[ARKRTOS_GDBSTUB_PKT_MAX];
static uint8  s_out[GS_OUT_MAX];
/* hex 编码单独一块：gs_send_hex 的源可能正是 s_out（m/M 的数据先读进
 * s_out），原地展开会在写到 i 之前覆盖掉还没读的源字节（实测 m 读到
 * "ABC" 发出去变成 "443433"）。 */
static uint8  s_enc[GS_OUT_MAX];
static uint8  s_bin[GS_MON_MAX];
static uint32 s_in_len;
static uint8  s_ck_hi;
static uint8  s_state;
static uint8  s_overflow;
static uint8  s_ack;

static const uint8 gs_hex_digits[] = "0123456789abcdef";

/* ---------------------------------------------------------------- 基础 */

static void gs_put(uint8 c)
{
    if (s_ops->byte_put != 0)
    {
        s_ops->byte_put(c);
    }
}

static void gs_send(const uint8 *body, uint32 n)
{
    uint32 i;
    uint8 cs = 0u;

    gs_put((uint8)'$');
    for (i = 0u; i < n; i++)
    {
        gs_put(body[i]);
        cs = (uint8)(cs + body[i]);
    }
    gs_put((uint8)'#');
    gs_put(gs_hex_digits[cs >> 4]);
    gs_put(gs_hex_digits[cs & 0x0Fu]);
    s_stat.tx_pkt++;
}

static void gs_send_cstr(const char *s)
{
    uint32 n = 0u;

    while (s[n] != '\0')
    {
        n++;
    }
    gs_send((const uint8 *)s, n);
}

/* 空包 = "不支持"。调用者负责先加 unsupported/bad_arg 计数。 */
static void gs_send_empty(void)
{
    gs_send((const uint8 *)"", 0u);
}

static void gs_send_e01(void)
{
    gs_send((const uint8 *)"E01", 3u);
}

static int32 gs_hexval(uint8 c)
{
    if ((c >= (uint8)'0') && (c <= (uint8)'9'))
    {
        return (int32)(c - (uint8)'0');
    }
    if ((c >= (uint8)'a') && (c <= (uint8)'f'))
    {
        return (int32)(c - (uint8)'a') + 10;
    }
    if ((c >= (uint8)'A') && (c <= (uint8)'F'))
    {
        return (int32)(c - (uint8)'A') + 10;
    }
    return -1;
}

/* 把 n 个 hex 字符解成 32 位值。n 必须 >=1 且全部是 hex，否则 -1。 */
static int32 gs_parse_hex_u32(const uint8 *p, uint32 n, uint32 *out)
{
    uint32 i;
    uint32 v = 0u;

    if ((n == 0u) || (n > 8u))
    {
        return -1;
    }
    for (i = 0u; i < n; i++)
    {
        int32 d = gs_hexval(p[i]);
        if (d < 0)
        {
            return -1;
        }
        v = (v << 4) | (uint32)d;
    }
    *out = v;
    return 0;
}

/* 把 hex 文本解成字节；返回字节数，坏字符回 -1。 */
static int32 gs_parse_hex_bytes(const uint8 *p, uint32 n, uint8 *out, uint32 cap)
{
    uint32 i;
    uint32 cnt = 0u;

    if ((n & 1u) != 0u)
    {
        return -1;
    }
    if ((n / 2u) > cap)
    {
        return -1;
    }
    for (i = 0u; i < n; i += 2u)
    {
        int32 hi = gs_hexval(p[i]);
        int32 lo = gs_hexval(p[i + 1u]);
        if ((hi < 0) || (lo < 0))
        {
            return -1;
        }
        out[cnt] = (uint8)(((uint32)hi << 4) | (uint32)lo);
        cnt++;
    }
    return (int32)cnt;
}

/* 把字节流编码成 hex 并作为包体发出去。编码走 s_enc，源可以是 s_out。 */
static void gs_send_hex(const uint8 *data, uint32 n)
{
    uint32 i;

    for (i = 0u; i < n; i++)
    {
        s_enc[i * 2u] = gs_hex_digits[data[i] >> 4];
        s_enc[i * 2u + 1u] = gs_hex_digits[data[i] & 0x0Fu];
    }
    gs_send(s_enc, n * 2u);
}

/* ---------------------------------------------------------------- 包 */

/* m ADDR,LEN —— 读内存 */
static void gs_m(const uint8 *p, uint32 n)
{
    uint32 addr = 0u;
    uint32 len = 0u;
    uint32 i;
    uint32 comma = 0u;

    for (i = 0u; i < n; i++)
    {
        if (p[i] == (uint8)',')
        {
            comma = i;
            break;
        }
    }
    if ((comma == 0u) ||
        (gs_parse_hex_u32(p, comma, &addr) < 0) ||
        (gs_parse_hex_u32(p + comma + 1u, n - comma - 1u, &len) < 0) ||
        (len > (ARKRTOS_GDBSTUB_PKT_MAX / 2u)))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    if (s_ops->mem_read == 0)
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    if (s_ops->mem_read(addr, s_out, len) < 0)
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    s_stat.mem_reads++;
    gs_send_hex(s_out, len);
}

/* M ADDR,LEN:HEX —— 写内存 */
static void gs_M(const uint8 *p, uint32 n)
{
    uint32 addr = 0u;
    uint32 len = 0u;
    uint32 i;
    uint32 comma = 0u;
    uint32 colon = 0u;
    int32 got;

    for (i = 0u; i < n; i++)
    {
        if (p[i] == (uint8)',')
        {
            comma = i;
            break;
        }
    }
    for (i = (comma == 0u) ? 0u : comma; i < n; i++)
    {
        if (p[i] == (uint8)':')
        {
            colon = i;
            break;
        }
    }
    if ((comma == 0u) || (colon <= comma) ||
        (gs_parse_hex_u32(p, comma, &addr) < 0) ||
        (gs_parse_hex_u32(p + comma + 1u, colon - comma - 1u, &len) < 0) ||
        (len > (ARKRTOS_GDBSTUB_PKT_MAX / 2u)))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    got = gs_parse_hex_bytes(p + colon + 1u, n - colon - 1u, s_out, GS_OUT_MAX);
    if ((got < 0) || ((uint32)got != len))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    if ((s_ops->mem_write == 0) || (s_ops->mem_write(addr, s_out, len) < 0))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    s_stat.mem_writes++;
    gs_send_cstr("OK");
}

/* X ADDR,LEN:BIN —— 二进制写内存（`}` 转义：下一个字节 XOR 0x20） */
static void gs_X(const uint8 *p, uint32 n)
{
    uint32 addr = 0u;
    uint32 len = 0u;
    uint32 i;
    uint32 comma = 0u;
    uint32 colon = 0u;
    uint32 cnt = 0u;
    uint8 esc = 0u;

    for (i = 0u; i < n; i++)
    {
        if (p[i] == (uint8)',')
        {
            comma = i;
            break;
        }
    }
    for (i = (comma == 0u) ? 0u : comma; i < n; i++)
    {
        if (p[i] == (uint8)':')
        {
            colon = i;
            break;
        }
    }
    if ((comma == 0u) || (colon <= comma) ||
        (gs_parse_hex_u32(p, comma, &addr) < 0) ||
        (gs_parse_hex_u32(p + comma + 1u, colon - comma - 1u, &len) < 0) ||
        (len > (ARKRTOS_GDBSTUB_PKT_MAX / 2u)))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    for (i = colon + 1u; i < n; i++)
    {
        uint8 c = p[i];
        if (esc != 0u)
        {
            c = (uint8)(c ^ 0x20u);
            esc = 0u;
        }
        else if (c == (uint8)'}')
        {
            esc = 1u;
            continue;
        }
        /* 上界由上面的编译期契约保证，这里不需要运行期检查。 */
        s_out[cnt] = c;
        cnt++;
    }
    if ((esc != 0u) || (cnt != len))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    if ((s_ops->mem_write == 0) || (s_ops->mem_write(addr, s_out, len) < 0))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    s_stat.mem_writes++;
    gs_send_cstr("OK");
}

/* p N —— 读一个寄存器（4 字节，小端） */
static void gs_p(const uint8 *p, uint32 n)
{
    uint32 idx = 0u;
    uint32 val = 0u;
    uint8 b[4];
    uint32 i;

    if ((gs_parse_hex_u32(p, n, &idx) < 0) ||
        (s_ops->reg_read == 0) ||
        (s_ops->reg_read(idx, &val) < 0))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    for (i = 0u; i < 4u; i++)
    {
        b[i] = (uint8)(val >> (i * 8u));
    }
    s_stat.reg_access++;
    gs_send_hex(b, 4u);
}

/* P N=V —— 写一个寄存器 */
static void gs_P(const uint8 *p, uint32 n)
{
    uint32 idx = 0u;
    uint32 val = 0u;
    uint32 i;
    uint32 eq = 0u;

    for (i = 0u; i < n; i++)
    {
        if (p[i] == (uint8)'=')
        {
            eq = i;
            break;
        }
    }
    if ((eq == 0u) ||
        (gs_parse_hex_u32(p, eq, &idx) < 0) ||
        (gs_parse_hex_u32(p + eq + 1u, n - eq - 1u, &val) < 0) ||
        (s_ops->reg_write == 0) ||
        (s_ops->reg_write(idx, val) < 0))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    s_stat.reg_access++;
    gs_send_cstr("OK");
}

/* g —— 整包寄存器（每个 4 字节小端，hex） */
static void gs_g(void)
{
    uint32 cnt;
    uint32 i;
    uint32 j;
    uint32 val = 0u;

    if ((s_ops->reg_count == 0) || (s_ops->reg_read == 0))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    cnt = s_ops->reg_count();
    if ((cnt == 0u) || ((cnt * 8u) > GS_OUT_MAX))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    for (i = 0u; i < cnt; i++)
    {
        if (s_ops->reg_read(i, &val) < 0)
        {
            s_stat.bad_arg++;
            gs_send_e01();
            return;
        }
        for (j = 0u; j < 4u; j++)
        {
            uint8 b = (uint8)(val >> (j * 8u));
            s_out[i * 8u + j * 2u] = gs_hex_digits[b >> 4];
            s_out[i * 8u + j * 2u + 1u] = gs_hex_digits[b & 0x0Fu];
        }
    }
    s_stat.reg_access++;
    gs_send(s_out, cnt * 8u);
}

/* G XX... —— 整包写寄存器 */
static void gs_G(const uint8 *p, uint32 n)
{
    uint32 cnt;
    uint32 i;
    int32 got;

    if (((n & 7u) != 0u) || (s_ops->reg_count == 0) || (s_ops->reg_write == 0))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    cnt = n / 8u;
    if (cnt > s_ops->reg_count())
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    got = gs_parse_hex_bytes(p, n, s_out, GS_OUT_MAX);
    if (got < 0)
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    for (i = 0u; i < cnt; i++)
    {
        uint32 val = (uint32)s_out[i * 4u] |
                     ((uint32)s_out[i * 4u + 1u] << 8) |
                     ((uint32)s_out[i * 4u + 2u] << 16) |
                     ((uint32)s_out[i * 4u + 3u] << 24);
        if (s_ops->reg_write(i, val) < 0)
        {
            s_stat.bad_arg++;
            gs_send_e01();
            return;
        }
    }
    s_stat.reg_access++;
    gs_send_cstr("OK");
}

/* qRcmd,HEX —— 监视命令（hex 文本进，hex 文本出） */
static void gs_qrcmd(const uint8 *p, uint32 n)
{
    int32 got;
    int32 outn;

    if (s_ops->monitor == 0)
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    got = gs_parse_hex_bytes(p, n, s_bin, GS_MON_MAX);
    if (got <= 0)
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    outn = s_ops->monitor((const char *)s_bin, (uint32)got, s_out, GS_OUT_MAX / 2u);
    if ((outn < 0) || ((uint32)outn > (GS_OUT_MAX / 2u)))
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    s_stat.monitor++;
    gs_send_hex(s_out, (uint32)outn);
}

/* q 系列 */
static void gs_q(const uint8 *p, uint32 n)
{
    if ((n >= 10u) && (p[0] == (uint8)'S') && (p[1] == (uint8)'u') &&
        (p[2] == (uint8)'p') && (p[3] == (uint8)'p') && (p[4] == (uint8)'o') &&
        (p[5] == (uint8)'r') && (p[6] == (uint8)'t') && (p[7] == (uint8)'e') &&
        (p[8] == (uint8)'d'))
    {
        gs_send_cstr("PacketSize=200;QStartNoAckMode+;qRcmd+;vContSupported-");
        return;
    }
    if ((n == 8u) && (p[0] == (uint8)'A') && (p[1] == (uint8)'t') &&
        (p[2] == (uint8)'t') && (p[3] == (uint8)'a') && (p[4] == (uint8)'c') &&
        (p[5] == (uint8)'h') && (p[6] == (uint8)'e') && (p[7] == (uint8)'d'))
    {
        gs_send_cstr("1");
        return;
    }
    if ((n == 1u) && (p[0] == (uint8)'C'))
    {
        gs_send_cstr("QC1");
        return;
    }
    if ((n == 7u) && (p[0] == (uint8)'O') && (p[1] == (uint8)'f') &&
        (p[2] == (uint8)'f') && (p[3] == (uint8)'s') && (p[4] == (uint8)'e') &&
        (p[5] == (uint8)'t') && (p[6] == (uint8)'s'))
    {
        gs_send_cstr("Text=0;Data=0;Bss=0");
        return;
    }
    if ((n >= 6u) && (p[0] == (uint8)'R') && (p[1] == (uint8)'c') &&
        (p[2] == (uint8)'m') && (p[3] == (uint8)'d') && (p[4] == (uint8)',') )
    {
        gs_qrcmd(p + 5u, n - 5u);
        return;
    }
    s_stat.unsupported++;
    gs_send_empty();
}

/* Q 系列 */
static void gs_Q(const uint8 *p, uint32 n)
{
    if ((n == 14u) && (p[0] == (uint8)'S') && (p[1] == (uint8)'t') &&
        (p[2] == (uint8)'a') && (p[3] == (uint8)'r') && (p[4] == (uint8)'t') &&
        (p[5] == (uint8)'N') && (p[6] == (uint8)'o') && (p[7] == (uint8)'A') &&
        (p[8] == (uint8)'c') && (p[9] == (uint8)'k') && (p[10] == (uint8)'M') &&
        (p[11] == (uint8)'o') && (p[12] == (uint8)'d') && (p[13] == (uint8)'e'))
    {
        /* 先回 OK（这一包自己还要带 '+')，再关 ack。 */
        gs_send_cstr("OK");
        s_ack = 0u;
        s_stat.ack = 0u;
        return;
    }
    s_stat.unsupported++;
    gs_send_empty();
}

static void gs_handle(const uint8 *p, uint32 n)
{
    if (n == 0u)
    {
        s_stat.bad_arg++;
        gs_send_e01();
        return;
    }
    switch (p[0])
    {
        case (uint8)'?':
            gs_send_cstr("S05");
            break;
        case (uint8)'q':
            gs_q(p + 1u, n - 1u);
            break;
        case (uint8)'Q':
            gs_Q(p + 1u, n - 1u);
            break;
        case (uint8)'m':
            gs_m(p + 1u, n - 1u);
            break;
        case (uint8)'M':
            gs_M(p + 1u, n - 1u);
            break;
        case (uint8)'X':
            gs_X(p + 1u, n - 1u);
            break;
        case (uint8)'p':
            gs_p(p + 1u, n - 1u);
            break;
        case (uint8)'P':
            gs_P(p + 1u, n - 1u);
            break;
        case (uint8)'g':
            gs_g();
            break;
        case (uint8)'G':
            gs_G(p + 1u, n - 1u);
            break;
        case (uint8)'H':
        case (uint8)'T':
        case (uint8)'D':
        case (uint8)'!':
            gs_send_cstr("OK");
            break;
        case (uint8)'k':
            s_stat.kill = 1u;
            gs_send_cstr("OK");
            break;
        default:
            /* c/s/C/S/v 与一切未实现的包都落在这里：回空包，不编 OK。 */
            s_stat.unsupported++;
            gs_send_empty();
            break;
    }
}

/* ---------------------------------------------------------------- 收字节 */

static void gs_finish_packet(void)
{
    uint32 i;
    uint8 cs = 0u;

    for (i = 0u; i < s_in_len; i++)
    {
        cs = (uint8)(cs + s_in[i]);
    }
    if ((s_overflow != 0u) || (cs != s_ck_hi))
    {
        if (s_overflow != 0u)
        {
            s_stat.rx_oversize++;
        }
        else
        {
            s_stat.rx_bad_cs++;
        }
        if (s_ack != 0u)
        {
            gs_put((uint8)'-');
        }
        return;
    }
    if (s_ack != 0u)
    {
        gs_put((uint8)'+');
    }
    s_stat.rx_pkt++;
    gs_handle(s_in, s_in_len);
}

static void gs_byte(uint8 c)
{
    switch (s_state)
    {
        case GS_RX_IDLE:
            if (c == (uint8)'$')
            {
                s_state = GS_RX_BODY;
                s_in_len = 0u;
                s_overflow = 0u;
            }
            /* '+'/'-'（客户端对我们回包的应答）与 0x03（中断）都丢掉：
             * 本代理没有执行流可中断，收到 0x03 也不改变任何状态。 */
            break;
        case GS_RX_BODY:
            if (c == (uint8)'#')
            {
                s_state = GS_RX_CK1;
            }
            else if (s_in_len < ARKRTOS_GDBSTUB_PKT_MAX)
            {
                s_in[s_in_len] = c;
                s_in_len++;
            }
            else
            {
                s_overflow = 1u;
            }
            break;
        case GS_RX_CK1:
            s_ck_hi = (uint8)gs_hexval(c);
            s_state = GS_RX_CK2;
            break;
        default: /* GS_RX_CK2 */
        {
            int32 lo = gs_hexval(c);
            int32 hi = (int32)s_ck_hi;
            s_state = GS_RX_IDLE;
            if ((hi < 0) || (lo < 0))
            {
                s_stat.rx_bad_cs++;
                if (s_ack != 0u)
                {
                    gs_put((uint8)'-');
                }
                break;
            }
            s_ck_hi = (uint8)(((uint32)hi << 4) | (uint32)lo);
            gs_finish_packet();
            break;
        }
    }
}

/* ---------------------------------------------------------------- 对外 */

void arkrtos_gdbstub_reset(void)
{
    uint32 i;

    s_in_len = 0u;
    s_state = GS_RX_IDLE;
    s_overflow = 0u;
    s_ack = 1u;
    s_ck_hi = 0u;
    for (i = 0u; i < GS_OUT_MAX; i++)
    {
        s_out[i] = 0u;
    }
    for (i = 0u; i < GS_MON_MAX; i++)
    {
        s_bin[i] = 0u;
    }
    s_stat.rx_pkt = 0u;
    s_stat.rx_bad_cs = 0u;
    s_stat.rx_oversize = 0u;
    s_stat.tx_pkt = 0u;
    s_stat.unsupported = 0u;
    s_stat.mem_reads = 0u;
    s_stat.mem_writes = 0u;
    s_stat.reg_access = 0u;
    s_stat.monitor = 0u;
    s_stat.bad_arg = 0u;
    s_stat.kill = 0u;
    s_stat.ack = 1u;
}

int32 arkrtos_gdbstub_init(const arkrtos_gdbstub_ops_t *ops)
{
    if ((ops == 0) || (ops->byte_put == 0))
    {
        return -1;
    }
    s_ops = ops;
    arkrtos_gdbstub_reset();
    return 0;
}

int32 arkrtos_gdbstub_feed(const uint8 *buf, uint32 len)
{
    uint32 i;

    if (s_ops == 0)
    {
        return -1;
    }
    if ((buf == 0) || (len == 0u))
    {
        return 0;
    }
    for (i = 0u; i < len; i++)
    {
        gs_byte(buf[i]);
    }
    return (int32)len;
}

int32 arkrtos_gdbstub_poll(uint32 max_bytes)
{
    uint32 i;

    if ((s_ops == 0) || (s_ops->byte_get == 0))
    {
        return -1;
    }
    for (i = 0u; i < max_bytes; i++)
    {
        uint8 b = 0u;
        if (s_ops->byte_get(&b) != 1)
        {
            break;
        }
        gs_byte(b);
    }
    return (int32)i;
}

const arkrtos_gdbstub_stat_t *arkrtos_gdbstub_stat(void)
{
    return &s_stat;
}

#endif /* ARKRTOS_USE_GDBSTUB */
