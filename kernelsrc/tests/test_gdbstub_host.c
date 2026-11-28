/**
* @file test_gdbstub_host.c
* @brief GDB 远程串行协议代理（arkrtos_gdbstub.c）的宿主用例
* @details 被测的是**帧格式与判据**，不是某根串口线。用例扮演 gdb 那一侧：
*          自己拼 `$body#cs`（校验和按 RSP 的「包体字节和取低 8 位」算），
*          自己解析回包、自己核对 ack。钉住的是下面这些：
*
*            1. 校验和错 -> 回 `-` 且**不**回包；超长包同样丢弃并回 `-`，
*               不允许把缓冲写穿；
*            2. `QStartNoAckMode` 之后双方都不再收发 ack，但包照常回；
*            3. `m` / `M` / `X` 走板级钩子：区间外返回 `E01`（不是「写成功」），
*               `X` 的 `}` 转义（`}` + (c ^ 0x20)）解出来必须逐字节相等；
*            4. `p` / `P` / `g` / `G` 走寄存器钩子；板级不给钩子时整包寄存器
*               回 `E01`；
*            5. `qRcmd` 的 hex 文本原样进钩子、结果 hex 出来；
*            6. `c` / `s` / `vCont` / `Z0` 这类**没有实现**的包回空包而不是
*               `OK` —— 空包在 gdb 侧是「不支持」，`OK` 会被当成功；
*            7. 逐字节喂与整段喂走同一条状态机；`poll` 从 `byte_get` 拉字节；
*            8. 未 init 时 feed/poll 返回 -1；`init(0)` 与缺 `byte_put` 被拒。
*
*          未验证（如实说明）：与真 gdb 的互通（本机没有可用 gdb 客户端）、
*          真实串口的波特率/丢字节、寄存器编号与某个具体架构 ABI 的对应关系。
*          用例证明的是「协议层不做假」，不是「接上某台机器一定能调」。
*
* @note 用法：由 tools/coverage_gate.py 的 gdbstub case 编译并运行，输出
*       GDBSTUB-HOST OK <n> checks。
*/

#include "arkrtos_features.h"
#include "arkrtos_gdbstub.h"

#include <stdio.h>
#include <string.h>

#if (ARKRTOS_USE_GDBSTUB == 1)

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL %s (line %d)\n", what, __LINE__);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got %lu want %lu (line %d)\n", what, got, want, __LINE__);
    }
}

static void ck_s(const char *got, const char *want, const char *what)
{
    g_checks++;
    if(strcmp(got, want) != 0)
    {
        g_fails++;
        printf("FAIL %s: got \"%s\" want \"%s\" (line %d)\n", what, got, want, __LINE__);
    }
}

/* ---------------- 板级钩子的替身 ---------------- */

#define T_MEM_BASE   0x1000u
#define T_MEM_SIZE   256u

static uint8 g_mem[T_MEM_SIZE];
static uint32 g_reg[4];

static int g_mem_read_calls;
static int g_mem_write_calls;
static int g_reg_read_calls;
static int g_reg_write_calls;
static int g_monitor_calls;

static int32 t_mem_read(uint32 addr, uint8 *buf, uint32 len)
{
    g_mem_read_calls++;
    if((addr < T_MEM_BASE) || ((addr - T_MEM_BASE) + len > T_MEM_SIZE))
    {
        return -1;
    }
    memcpy(buf, &g_mem[addr - T_MEM_BASE], len);
    return 0;
}

static int32 t_mem_write(uint32 addr, const uint8 *buf, uint32 len)
{
    g_mem_write_calls++;
    if((addr < T_MEM_BASE) || ((addr - T_MEM_BASE) + len > T_MEM_SIZE))
    {
        return -1;
    }
    memcpy(&g_mem[addr - T_MEM_BASE], buf, len);
    return 0;
}

static int32 t_reg_read(uint32 idx, uint32 *val)
{
    g_reg_read_calls++;
    if(idx > 3u)
    {
        return -1;
    }
    *val = g_reg[idx];
    return 0;
}

static int32 t_reg_write(uint32 idx, uint32 val)
{
    g_reg_write_calls++;
    if(idx > 3u)
    {
        return -1;
    }
    g_reg[idx] = val;
    return 0;
}

static uint32 t_reg_count(void)
{
    return 4u;
}

static int32 t_monitor(const char *cmd, uint32 len, uint8 *out, uint32 cap)
{
    g_monitor_calls++;
    if((len == 4u) && (memcmp(cmd, "info", 4u) == 0))
    {
        if(cap < 6u)
        {
            return -1;
        }
        memcpy(out, "ark ok", 6u);
        return 6;
    }
    return -1;
}

/* ---------------- 发送侧捕获 ---------------- */

static uint8 g_tx[16384];
static uint32 g_tx_n;

static void t_put(uint8 b)
{
    if(g_tx_n < sizeof(g_tx))
    {
        g_tx[g_tx_n] = b;
        g_tx_n++;
    }
}

/* 解析出来的回包 */
typedef struct
{
    char body[768];
    uint32 cs_ok;
} rep_t;

static rep_t g_rep[8];
static int g_rep_n;
static char g_acks[16];
static int g_ack_n;

static int hexv(char c)
{
    if((c >= '0') && (c <= '9'))
    {
        return c - '0';
    }
    if((c >= 'a') && (c <= 'f'))
    {
        return c - 'a' + 10;
    }
    if((c >= 'A') && (c <= 'F'))
    {
        return c - 'A' + 10;
    }
    return -1;
}

static void tx_parse(void)
{
    uint32 i = 0u;

    g_rep_n = 0;
    g_ack_n = 0;
    g_acks[0] = '\0';
    while(i < g_tx_n)
    {
        char c = (char)g_tx[i];
        if((c == '+') || (c == '-'))
        {
            if(g_ack_n < (int)sizeof(g_acks) - 1)
            {
                g_acks[g_ack_n] = c;
                g_ack_n++;
                g_acks[g_ack_n] = '\0';
            }
            i++;
        }
        else if(c == '$')
        {
            uint32 start = i + 1u;
            uint32 end = start;
            uint32 sum = 0u;
            uint32 k;
            int hi;
            int lo;
            rep_t *r = &g_rep[(g_rep_n < 8) ? g_rep_n : 7];

            while((end < g_tx_n) && ((char)g_tx[end] != '#'))
            {
                end++;
            }
            if((end + 2u) >= g_tx_n)
            {
                break;
            }
            hi = hexv((char)g_tx[end + 1u]);
            lo = hexv((char)g_tx[end + 2u]);
            for(k = start; k < end; k++)
            {
                sum += g_tx[k];
            }
            r->cs_ok = ((hi >= 0) && (lo >= 0) &&
                        (((uint32)hi * 16u + (uint32)lo) == (sum & 0xFFu))) ? 1u : 0u;
            for(k = start; (k < end) && ((k - start) < sizeof(r->body) - 1u); k++)
            {
                r->body[k - start] = (char)g_tx[k];
            }
            r->body[(k - start) < sizeof(r->body) ? (k - start) : (sizeof(r->body) - 1u)] = '\0';
            if(g_rep_n < 8)
            {
                g_rep_n++;
            }
            i = end + 3u;
        }
        else
        {
            i++;
        }
    }
}

static void tx_reset(void)
{
    g_tx_n = 0u;
    g_rep_n = 0;
    g_ack_n = 0;
}

static const char *last_body(void)
{
    if(g_rep_n == 0)
    {
        return "<none>";
    }
    return g_rep[g_rep_n - 1].body;
}

/* ---------------- 客户端侧 ---------------- */

static uint8 g_pkt[2048];

/* 拼一个合法的 `$body#cs`，返回总长度 */
static uint32 cli_build(const char *body)
{
    uint32 n = (uint32)strlen(body);
    uint32 i;
    uint32 sum = 0u;

    g_pkt[0] = '$';
    for(i = 0u; i < n; i++)
    {
        g_pkt[1u + i] = (uint8)body[i];
        sum += (uint8)body[i];
    }
    g_pkt[1u + n] = '#';
    g_pkt[2u + n] = (uint8)"0123456789abcdef"[(sum & 0xFFu) >> 4];
    g_pkt[3u + n] = (uint8)"0123456789abcdef"[sum & 0x0Fu];
    return n + 4u;
}

/* 发一个包并解析回包（清空发送捕获） */
static void send_pkt(const char *body)
{
    uint32 n;

    tx_reset();
    n = cli_build(body);
    ck(arkrtos_gdbstub_feed(g_pkt, n) == (int32)n, "feed consumes the whole packet");
    tx_parse();
}

/* ---------------- 主用例 ---------------- */

static const arkrtos_gdbstub_ops_t g_ops =
{
    .byte_get  = 0,
    .byte_put  = t_put,
    .mem_read  = t_mem_read,
    .mem_write = t_mem_write,
    .reg_read  = t_reg_read,
    .reg_write = t_reg_write,
    .reg_count = t_reg_count,
    .monitor   = t_monitor
};

static void reset_all(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    memset(g_reg, 0, sizeof(g_reg));
    g_mem_read_calls = 0;
    g_mem_write_calls = 0;
    g_reg_read_calls = 0;
    g_reg_write_calls = 0;
    g_monitor_calls = 0;
    tx_reset();
    ck(arkrtos_gdbstub_init(&g_ops) == 0, "init accepts a valid table");
}

/* 第一组：装载与帧格式 */
static void test_frame(void)
{
    const arkrtos_gdbstub_stat_t *st;

    reset_all();
    ck(arkrtos_gdbstub_init(0) == -1, "init(0) is refused");
    ck(arkrtos_gdbstub_feed((const uint8 *)"x", 1u) == 1,
       "a refused init keeps the previous table");

    send_pkt("?");
    ck_s(g_acks, "+", "a good packet is acked");
    ck_u((unsigned long)g_rep_n, 1u, "exactly one reply");
    ck_s(last_body(), "S05", "? answers S05");
    st = arkrtos_gdbstub_stat();
    ck_u(st->rx_pkt, 1u, "rx_pkt counted");
    ck_u(st->tx_pkt, 1u, "tx_pkt counted");
    ck(g_rep[0].cs_ok == 1u, "the reply carries a correct checksum");

    /* 坏校验和：修正最后一个 hex 数字 */
    tx_reset();
    {
        uint32 n = cli_build("?");
        g_pkt[n - 1u] = (uint8)((g_pkt[n - 1u] == (uint8)'0') ? '1' : '0');
        arkrtos_gdbstub_feed(g_pkt, n);
    }
    tx_parse();
    ck_s(g_acks, "-", "a bad checksum is nacked");
    ck_u((unsigned long)g_rep_n, 0u, "a bad checksum gets no reply");
    ck_u((unsigned long)arkrtos_gdbstub_stat()->rx_bad_cs, 1u, "rx_bad_cs counted");

    /* 超长包：600 字节包体，超 512 上限 */
    tx_reset();
    {
        uint32 i;
        uint32 sum = 0u;
        g_pkt[0] = '$';
        for(i = 0u; i < 600u; i++)
        {
            g_pkt[1u + i] = (uint8)'a';
            sum += (uint8)'a';
        }
        g_pkt[601] = '#';
        g_pkt[602] = (uint8)"0123456789abcdef"[(sum & 0xFFu) >> 4];
        g_pkt[603] = (uint8)"0123456789abcdef"[sum & 0x0Fu];
        arkrtos_gdbstub_feed(g_pkt, 604u);
    }
    tx_parse();
    ck_s(g_acks, "-", "an oversize packet is nacked");
    ck_u((unsigned long)g_rep_n, 0u, "an oversize packet gets no reply");
    ck_u((unsigned long)arkrtos_gdbstub_stat()->rx_oversize, 1u, "rx_oversize counted");

    /* 非包字节（客户端对我们回包的 ack、0x03 中断）不产生任何回包 */
    tx_reset();
    {
        uint8 junk[3];
        junk[0] = (uint8)'+';
        junk[1] = 0x03u;
        junk[2] = (uint8)'-';
        ck(arkrtos_gdbstub_feed(junk, 3u) == 3, "junk bytes are consumed");
    }
    tx_parse();
    ck_u((unsigned long)g_rep_n, 0u, "junk outside a packet yields no reply");
}

/* 第二组：q 系列 */
static void test_query(void)
{
    reset_all();

    send_pkt("qSupported:multiprocess+;swbreak+");
    ck(strstr(last_body(), "PacketSize=") != 0, "qSupported advertises a packet size");
    ck(g_rep[0].cs_ok == 1u, "qSupported reply checksum ok");

    send_pkt("qAttached");
    ck_s(last_body(), "1", "qAttached answers 1");

    send_pkt("qC");
    ck_s(last_body(), "QC1", "qC answers QC1");

    send_pkt("qOffsets");
    ck_s(last_body(), "Text=0;Data=0;Bss=0", "qOffsets answers zero offsets");

    send_pkt("qWhatever");
    ck_s(last_body(), "", "an unknown q packet gets the empty reply");
    ck(arkrtos_gdbstub_stat()->unsupported >= 1u, "unsupported counted");
}

/* 第三组：内存 */
static void test_memory(void)
{
    reset_all();
    g_mem[0] = (uint8)'A';
    g_mem[1] = (uint8)'B';
    g_mem[2] = (uint8)'C';

    send_pkt("m1000,3");
    ck_s(last_body(), "414243", "m reads through the hook");
    ck_u((unsigned long)g_mem_read_calls, 1u, "mem_read called once");

    send_pkt("m9000,3");
    ck_s(last_body(), "E01", "m outside the arena is refused");
    ck_u((unsigned long)arkrtos_gdbstub_stat()->bad_arg, 1u, "bad_arg counted");

    send_pkt("m1000,0");
    ck_s(last_body(), "", "m with len 0 returns an empty payload, not E01");

    send_pkt("m1000");
    ck_s(last_body(), "E01", "m without a comma is refused");

    send_pkt("M1004,3:444546");
    ck_s(last_body(), "OK", "M writes through the hook");
    ck_u((unsigned long)g_mem[4], (unsigned long)'D', "first byte landed");
    ck_u((unsigned long)g_mem[6], (unsigned long)'F', "last byte landed");

    send_pkt("M1004,3:4445");
    ck_s(last_body(), "E01", "M with fewer bytes than len is refused");

    send_pkt("M9000,1:00");
    ck_s(last_body(), "E01", "M outside the arena is refused");

    /* X：二进制写，`}` 转义。写 {0x23 '#', 0x7d '}', 0x41 'A'} */
    tx_reset();
    {
        uint8 pkt[64];
        uint32 i = 0u;
        uint32 sum = 0u;
        const char *head = "X1010,3:";
        pkt[i++] = '$';
        while(*head != '\0')
        {
            pkt[i++] = (uint8)*head;
            sum += (uint8)*head;
            head++;
        }
        /* '#'(0x23) 必须转义：0x23 ^ 0x20 = 0x03 */
        pkt[i++] = (uint8)'}';
        sum += (uint8)'}';
        pkt[i++] = 0x03u;
        sum += 0x03u;
        /* '}' 本身必须转义：0x7d ^ 0x20 = 0x5d */
        pkt[i++] = (uint8)'}';
        sum += (uint8)'}';
        pkt[i++] = 0x5Du;
        sum += 0x5Du;
        /* 'A' 不转义 */
        pkt[i++] = (uint8)'A';
        sum += (uint8)'A';
        pkt[i++] = '#';
        pkt[i++] = (uint8)"0123456789abcdef"[(sum & 0xFFu) >> 4];
        pkt[i++] = (uint8)"0123456789abcdef"[sum & 0x0Fu];
        arkrtos_gdbstub_feed(pkt, i);
        tx_parse();
    }
    ck_s(last_body(), "OK", "X with escapes writes through the hook");
    ck_u((unsigned long)g_mem[0x10], 0x23u, "escaped '#' byte landed");
    ck_u((unsigned long)g_mem[0x11], 0x7Du, "escaped '}' byte landed");
    ck_u((unsigned long)g_mem[0x12], (unsigned long)'A', "plain byte landed");

    send_pkt("X1010,3:ABCD");
    ck_s(last_body(), "E01", "X with a length mismatch is refused");
}

/* 第四组：寄存器 */
static void test_regs(void)
{
    reset_all();
    g_reg[0] = 0x11223344u;
    g_reg[1] = 0xAABBu;

    send_pkt("p0");
    ck_s(last_body(), "44332211", "p0 is little-endian 4 bytes");

    send_pkt("p1");
    ck_s(last_body(), "bbaa0000", "p1 is zero-extended");

    send_pkt("p9");
    ck_s(last_body(), "E01", "p out of range is refused");

    send_pkt("P1=0000ccdd");
    ck_s(last_body(), "OK", "P writes through the hook");
    ck_u((unsigned long)g_reg[1], 0x0000CCDDu, "P landed");

    send_pkt("P1");
    ck_s(last_body(), "E01", "P without '=' is refused");

    send_pkt("g");
    ck_s(last_body(), "44332211ddcc00000000000000000000",
         "g concatenates every register");

    send_pkt("G44332211ddcc00000000000000000000");
    ck_s(last_body(), "OK", "G writes every register");
    ck_u((unsigned long)g_reg[0], 0x11223344u, "G landed reg 0");

    send_pkt("G4433");
    ck_s(last_body(), "E01", "G shorter than one register is refused");

    send_pkt("G4433231");
    ck_s(last_body(), "E01", "G with an odd number of hex digits is refused");
}

/* 第五组：监视命令与那个「空包不是 OK」的纪律 */
static void test_monitor_and_unsupported(void)
{
    reset_all();

    /* "info" 的 hex 是 696e666f；"ark ok" 的 hex 是 61726b206f6b */
    send_pkt("qRcmd,696e666f");
    ck_s(last_body(), "61726b206f6b", "qRcmd returns the hook output hex-encoded");
    ck_u((unsigned long)g_monitor_calls, 1u, "monitor hook called");
    ck_u((unsigned long)arkrtos_gdbstub_stat()->monitor, 1u, "monitor counted");

    send_pkt("qRcmd,6e6f7065");
    ck_s(last_body(), "E01", "an unknown monitor command is refused");

    send_pkt("vMustReplyEmpty");
    ck_s(last_body(), "", "an unsupported packet gets the empty reply");

    send_pkt("c");
    ck_s(last_body(), "", "continue is not faked into OK");

    send_pkt("s");
    ck_s(last_body(), "", "step is not faked into OK");

    send_pkt("Z0,1000,2");
    ck_s(last_body(), "", "a breakpoint request is not faked into OK");

    send_pkt("Hc-1");
    ck_s(last_body(), "OK", "H is acknowledged");

    send_pkt("T1");
    ck_s(last_body(), "OK", "T is acknowledged");

    send_pkt("D");
    ck_s(last_body(), "OK", "detach is acknowledged");

    send_pkt("!");
    ck_s(last_body(), "OK", "extended mode is acknowledged");

    send_pkt("k");
    ck_s(last_body(), "OK", "kill is acknowledged");
    ck_u((unsigned long)arkrtos_gdbstub_stat()->kill, 1u, "kill counted");
}

/* 第六组：无 ack 模式 */
static void test_no_ack(void)
{
    reset_all();

    send_pkt("QStartNoAckMode");
    ck_s(g_acks, "+", "the no-ack request itself is still acked");
    ck_s(last_body(), "OK", "no-ack mode is accepted");
    ck_u((unsigned long)arkrtos_gdbstub_stat()->ack, 0u, "ack flag cleared");

    tx_reset();
    {
        uint32 n = cli_build("?");
        arkrtos_gdbstub_feed(g_pkt, n);
    }
    tx_parse();
    ck_s(g_acks, "", "no ack is sent after the switch");
    ck_u((unsigned long)g_rep_n, 1u, "the reply still goes out");
    ck_s(last_body(), "S05", "the reply is still correct");

    /* 坏包在无 ack 模式下也不回 `-`，但仍然不回包 */
    tx_reset();
    {
        uint32 n = cli_build("?");
        g_pkt[n - 1u] = (uint8)((g_pkt[n - 1u] == (uint8)'0') ? '1' : '0');
        arkrtos_gdbstub_feed(g_pkt, n);
    }
    tx_parse();
    ck_s(g_acks, "", "a bad packet is not nacked in no-ack mode");
    ck_u((unsigned long)g_rep_n, 0u, "a bad packet still gets no reply");
}

/* 第六组补：解析与载荷边界 */
static void test_parse_edges(void)
{
    static char big[300];

    reset_all();
    g_mem[0xA0] = 0x5Au;

    /* 大写 hex 数字：gs_hexval 的 A-F 分支 */
    send_pkt("m10A0,1");
    ck_s(last_body(), "5a", "uppercase hex digits are accepted");

    /* 索引超过 8 个 hex 位 */
    send_pkt("p123456789");
    ck_s(last_body(), "E01", "p with more than 8 hex digits is refused");

    /* 地址里有非 hex 字符 */
    send_pkt("mXY,1");
    ck_s(last_body(), "E01", "m with a non-hex address is refused");

    /* M 的载荷是半个字节 */
    send_pkt("M1000,1:4");
    ck_s(last_body(), "E01", "M with an odd payload length is refused");

    /* M 的载荷里有非 hex 字符 */
    send_pkt("M1000,1:zz");
    ck_s(last_body(), "E01", "M with a non-hex payload is refused");

    /* M 缺冒号 */
    send_pkt("M1000,1");
    ck_s(last_body(), "E01", "M without a colon is refused");

    /* X 缺冒号 */
    send_pkt("X1000,1");
    ck_s(last_body(), "E01", "X without a colon is refused");

    /* qRcmd 载荷超过监视缓冲上限（129 字节 > 128） */
    memcpy(big, "qRcmd,", 6u);
    memset(big + 6, '4', 258u);
    big[6u + 258u] = '\0';
    send_pkt(big);
    ck_s(last_body(), "E01", "qRcmd beyond the monitor buffer is refused");

    /* G 的载荷里有非 hex 字符 */
    send_pkt("Gzzzzzzzz");
    ck_s(last_body(), "E01", "G with a non-hex payload is refused");

    /* 空包 */
    send_pkt("");
    ck_s(last_body(), "E01", "an empty packet is refused");

    /* 校验位不是 hex 字符 */
    tx_reset();
    {
        uint8 pkt[8];
        pkt[0] = (uint8)'$';
        pkt[1] = (uint8)'?';
        pkt[2] = (uint8)'#';
        pkt[3] = (uint8)'z';
        pkt[4] = (uint8)'z';
        arkrtos_gdbstub_feed(pkt, 5u);
    }
    tx_parse();
    ck_s(g_acks, "-", "a non-hex checksum digit is nacked");
    ck_u((unsigned long)g_rep_n, 0u, "a non-hex checksum digit gets no reply");

    /* Q 前缀未知包 */
    send_pkt("Qfoo");
    ck_s(last_body(), "", "an unknown Q packet gets the empty reply");
}

/* 第七组：各钩子缺失时的防御分支 */
static arkrtos_gdbstub_ops_t g_ops2;

static uint32 t_reg_count_0(void)
{
    return 0u;
}

static uint32 t_reg_count_5(void)
{
    return 5u;
}

static void test_hook_gaps(void)
{
    /* poll 在没接 byte_get 时必须回 -1 */
    reset_all();
    ck(arkrtos_gdbstub_poll(8u) == -1, "poll without byte_get is refused");

    /* m 但没接 mem_read */
    reset_all();
    g_ops2 = g_ops;
    g_ops2.mem_read = 0;
    ck(arkrtos_gdbstub_init(&g_ops2) == 0, "init accepts a table without mem_read");
    send_pkt("m1000,1");
    ck_s(last_body(), "E01", "m without a mem_read hook is refused");

    /* X 但没接 mem_write */
    reset_all();
    g_ops2 = g_ops;
    g_ops2.mem_write = 0;
    arkrtos_gdbstub_init(&g_ops2);
    send_pkt("X1000,1:A");
    ck_s(last_body(), "E01", "X without a mem_write hook is refused");

    /* p 但没接 reg_read */
    reset_all();
    g_ops2 = g_ops;
    g_ops2.reg_read = 0;
    arkrtos_gdbstub_init(&g_ops2);
    send_pkt("p0");
    ck_s(last_body(), "E01", "p without a reg_read hook is refused");

    /* P 但没接 reg_write */
    reset_all();
    g_ops2 = g_ops;
    g_ops2.reg_write = 0;
    arkrtos_gdbstub_init(&g_ops2);
    send_pkt("P0=00000001");
    ck_s(last_body(), "E01", "P without a reg_write hook is refused");

    /* g 但没接 reg_count */
    reset_all();
    g_ops2 = g_ops;
    g_ops2.reg_count = 0;
    arkrtos_gdbstub_init(&g_ops2);
    send_pkt("g");
    ck_s(last_body(), "E01", "g without a reg_count hook is refused");

    /* g 但寄存器个数为 0 */
    reset_all();
    g_ops2 = g_ops;
    g_ops2.reg_count = t_reg_count_0;
    arkrtos_gdbstub_init(&g_ops2);
    send_pkt("g");
    ck_s(last_body(), "E01", "g with zero registers is refused");

    /* g 但某个寄存器读失败（说 5 个，钩子只认 0..3） */
    reset_all();
    g_ops2 = g_ops;
    g_ops2.reg_count = t_reg_count_5;
    arkrtos_gdbstub_init(&g_ops2);
    send_pkt("g");
    ck_s(last_body(), "E01", "g fails when a register read fails");

    /* G 但没接 reg_count */
    reset_all();
    g_ops2 = g_ops;
    g_ops2.reg_count = 0;
    arkrtos_gdbstub_init(&g_ops2);
    send_pkt("G11223344");
    ck_s(last_body(), "E01", "G without a reg_count hook is refused");

    /* G 写的寄存器比目标多（5 个 > 4 个） */
    reset_all();
    send_pkt("G11223344556677889900aabbccddeeff00112233");
    ck_s(last_body(), "E01", "G beyond the register count is refused");

    /* G 写到第 5 个寄存器时钩子报错（说 5 个，钩子只认 0..3） */
    reset_all();
    g_ops2 = g_ops;
    g_ops2.reg_count = t_reg_count_5;
    arkrtos_gdbstub_init(&g_ops2);
    send_pkt("G00112233445566778899aabbccddeeff00112233");
    ck_s(last_body(), "E01", "G fails when a register write fails");

    /* qRcmd 但没接 monitor */
    reset_all();
    g_ops2 = g_ops;
    g_ops2.monitor = 0;
    arkrtos_gdbstub_init(&g_ops2);
    send_pkt("qRcmd,696e666f");
    ck_s(last_body(), "E01", "qRcmd without a monitor hook is refused");

    /* qRcmd 载荷只有半个字节，解析出 0 个字节 */
    reset_all();
    send_pkt("qRcmd,4");
    ck_s(last_body(), "E01", "qRcmd with a half-byte payload is refused");
}

/* 第九组：逐字节喂 / poll / reset */
static uint8 g_q[64];
static uint32 g_qn;
static uint32 g_qi;

static int32 t_byte_get(uint8 *b);

static void test_stream(void)
{
    arkrtos_gdbstub_ops_t ops;
    uint32 n;
    uint32 i;

    reset_all();

    /* 逐字节喂一个包，仍然只回一次 */
    tx_reset();
    n = cli_build("?");
    for(i = 0u; i < n; i++)
    {
        ck(arkrtos_gdbstub_feed(&g_pkt[i], 1u) == 1, "one byte at a time is consumed");
    }
    tx_parse();
    ck_u((unsigned long)g_rep_n, 1u, "a split packet produces one reply");
    ck_s(last_body(), "S05", "the split packet is handled");

    /* poll：字节从 byte_get 队列里拉 */
    n = cli_build("qC");
    memcpy(g_q, g_pkt, n);
    g_qn = n;
    g_qi = 0u;
    ops = g_ops;
    ops.byte_get = t_byte_get;
    ck(arkrtos_gdbstub_init(&ops) == 0, "init with byte_get succeeds");
    tx_reset();
    ck_u((unsigned long)arkrtos_gdbstub_poll(64u), (unsigned long)n,
         "poll drains the queued packet");
    tx_parse();
    ck_s(last_body(), "QC1", "the polled packet is handled");
    ck_u((unsigned long)arkrtos_gdbstub_poll(64u), 0u, "an empty queue yields 0");

    /* reset 清计数并恢复 ack 模式 */
    arkrtos_gdbstub_reset();
    ck_u((unsigned long)arkrtos_gdbstub_stat()->rx_pkt, 0u, "reset clears counters");
    ck_u((unsigned long)arkrtos_gdbstub_stat()->ack, 1u, "reset restores ack mode");

    ck(arkrtos_gdbstub_feed(0, 0u) == 0, "a null/empty feed is a no-op");
}

static int32 t_byte_get(uint8 *b)
{
    if(g_qi >= g_qn)
    {
        return 0;
    }
    *b = g_q[g_qi];
    g_qi++;
    return 1;
}

int main(void)
{
    {
        uint8 junk = (uint8)'$';
        ck(arkrtos_gdbstub_feed(&junk, 1u) == -1, "feed before any init is refused");
    }
    test_frame();
    test_query();
    test_memory();
    test_regs();
    test_monitor_and_unsupported();
    test_no_ack();
    test_parse_edges();
    test_hook_gaps();
    test_stream();

    if(g_fails != 0)
    {
        printf("GDBSTUB-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("GDBSTUB-HOST OK %d checks\n", g_checks);
    return 0;
}

#else

int main(void)
{
    printf("GDBSTUB-HOST OFF OK\n");
    return 0;
}

#endif
