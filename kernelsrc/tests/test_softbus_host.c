/**
* @file test_softbus_host.c
* @brief arkrtos_softbus.c 的宿主用例：真组件 + 板内 loopback，端到端
* @details 被验的不是"软总线好不好用"，而是这一层**接入代码**有没有把组件
*          接错。做法是让整个 ark_softbus 组件（core / frame / qos / rpc /
*          stream / discovery / transport_loopback / port 胶水）在宿主上真
*          编真链，然后用内核这一层的四个入口去驱动它：
*
*            1. 未起来时 pump 必须报 NOTUP，起来后幂等 init；
*            2. info 打出来的档位（msg<=128 topic<=12 dev<=4）就是编进去的
*               那个档——档位宏没生效时这里会先红，而不是等到上板 RAM 不够；
*            3. selftest 的十段（编解码 / 重组 / 发现 / 发布订阅 / 定向+ACK /
*               RPC / 数据平面分块 / QoS 重传超时 / 错误路径 / 主题表）
*               在宿主上逐段 PASS，且"failed 0"；
*            4. shell 命令的每条子命令与参数错误分支：info / selftest /
*               pump [n] / peers / rx / pub / rpc / stream，以及数字解析
*               失败（回落到默认值）与未知子命令（打 usage 返回 -1）。
*
*          三个钩子由本文件提供，语义与板级一致但时间来自虚拟时钟：
*          ark_bus_port_tick_ms 返回 g_now，arkrtos_task_wait_internal 把
*          g_now 往前推——于是 QoS 的 200ms 重传间隔在宿主上是确定性的，
*          用例不会真的等 12 秒。
*
*          明确没验证：物理串口上的两个节点（两块板 / 板+PC）、真节拍下
*          的重传抖动、多核并发下总线表的竞争。那些只能上板。
*
* @author xw
* @date 2026.11.22
*/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "arkrtos_softbus.h"
#include "arkrtos_task.h"           /* arkrtos_task_wait_internal */
#include "ark_bus_port_arkrtos.h"   /* ark_bus_port_tick_ms / lock / unlock */
#include "ark_shell.h"              /* ark_shell_printf */

#if (ARKRTOS_USE_SOFTBUS == 0)

/* 关档：arkrtos_softbus.c 是一个空翻译单元，四个入口连声明都不存在
 * （头文件里也在同一个 #if 里）。这里只证明"关档时用例仍可编可跑"，
 * 真正的断言在 ci_gate：关档目标里不许剩任何全局符号——否则就是
 * "开关拨到 0 了、代码还在里面"，体积与行为都不是零变化。 */
int main(void)
{
    printf("SOFTBUS-HOST OK  off\n");
    return 0;
}

#else

static int g_checks;

/* shell_cmd 收 char **（历史签名，它并不改写 argv）。字符串字面量要当实参
 * 就得跨过 const，而门禁开着 -Wcast-qual，直接 (char *) 强转会被抓；先折
 * 成 uintptr_t 再转回来，等于把"我知道这块不会被写"写成显式的两步。 */
#define LIT(s) ((char *)(uintptr_t)(s))

static int g_fails;

#define CK(cond)                                                          \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fails++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------------ */
/* 板级三钩子：虚拟时钟                                                   */
/* ------------------------------------------------------------------ */

static uint32_t g_now;

uint32_t ark_bus_port_tick_ms(void)
{
    return g_now;
}

void ark_bus_port_lock(void)
{
}

void ark_bus_port_unlock(void)
{
}

/* 板上是"让出 CPU 睡 ms 毫秒"；这里是"时间往前走 ms"。用例不依赖 CPU 真睡，
 * 只依赖组件看到的时间在前进（QoS 重传/超时按它判）。 */
void arkrtos_task_wait_internal(uint32 ms)
{
    g_now += (uint32_t)ms;
}

/* ------------------------------------------------------------------ */
/* 控制台输出捕获                                                         */
/* ------------------------------------------------------------------ */

#define CAP_MAX (64u * 1024u)

static char   g_cap[CAP_MAX];
static size_t g_cap_len;

void ark_shell_printf(const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (g_cap_len >= (CAP_MAX - 1u)) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(g_cap + g_cap_len, CAP_MAX - g_cap_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        g_cap_len += (size_t)n;
        if (g_cap_len >= CAP_MAX) {
            g_cap_len = CAP_MAX - 1u;
        }
        g_cap[g_cap_len] = '\0';
    }
}

static void cap_clear(void)
{
    g_cap_len = 0u;
    g_cap[0] = '\0';
}

static int cap_has(const char *needle)
{
    return strstr(g_cap, needle) != NULL;
}

/* 数一个子串出现多少次（PASS/FAIL 那种要数条数的地方用）。 */
static int cap_count(const char *needle)
{
    const char *p = g_cap;
    size_t      n = strlen(needle);
    int         c = 0;

    if (n == 0u) {
        return 0;
    }
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += n;
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* 用例                                                                  */
/* ------------------------------------------------------------------ */

/* 1. 未起来时的返回码，以及 init 的幂等。 */
static void test_lifecycle(void)
{
    CK(arkrtos_softbus_pump(1u) != 0);          /* NOTUP */
    CK(arkrtos_softbus_init() == 0);
    CK(arkrtos_softbus_init() == 0);            /* 再 init 一次仍是 0 */
    CK(arkrtos_softbus_pump(0u) == 0);          /* 0 轮按 1 轮算 */
    CK(arkrtos_softbus_pump(3u) == 0);
}

/* 2. info：档位、节点号、对端在线状态。档位这一行是"MCU 档真的生效了"的读数。 */
static void test_info(void)
{
    cap_clear();
    CK(arkrtos_softbus_info() == 0);
    CK(cap_has("softbus (ark_softbus "));
    CK(cap_has("node id   : 0x00001001 type 1 cap 0x00000001"));
    CK(cap_has("link      : loopback (in-board, not a physical UART)"));
    CK(cap_has("profile   : msg<=128 topic<=12 dev<=4 qos=2 rpc=4 stream=1"));
    CK(cap_has("peers     : 1 (peer 0x00001002 online)"));
#if (ARK_BUS_CRYPTO_ENABLED == 1)
    /* 加密档：这一行是"开关真的编进来了、此刻有没有钥"的唯一运行时读数。
     * 没装钥时应为 keyed=no——自检/命令装过的钥在复位后不该偷偷留着。 */
    CK(cap_has("crypto    : auth=hmac-sha256 tag="));
    CK(cap_has("keyed=no"));
#else
    CK(cap_has("crypto    : off (ARKRTOS_USE_BUS_CRYPTO=0)"));
#endif
    CK(cap_has("node tx   : frames="));
    CK(cap_has("node rx   : frames="));
    CK(cap_has("peer saw  : msgs="));
}

/* 3. selftest：十段全 PASS，failed 0。 */
static void test_selftest(void)
{
    cap_clear();
    CK(arkrtos_softbus_selftest() == 0);
    CK(cap_has("softbus selftest (loopback, two in-board nodes)"));
    CK(cap_has("checks "));
    CK(cap_has("failed 0"));
    CK(!cap_has("FAIL"));
    CK(cap_has("  PASS codec"));
    CK(cap_has("  PASS reader"));
    CK(cap_has("  PASS discovery"));
    CK(cap_has("  PASS pubsub"));
    CK(cap_has("  PASS directed+ack"));
    CK(cap_has("  PASS rpc"));
    CK(cap_has("  PASS stream"));
    CK(cap_has("  PASS qos-timeout"));
    CK(cap_has("  PASS errors"));
    CK(cap_has("  PASS topics"));
#if (ARK_BUS_CRYPTO_ENABLED == 1)
    /* 加密档多一段：未装钥时它只打一行"跳过"，装了钥才真跑（见用例 8）。 */
    CK(cap_has("  PASS crypto"));
    CK(cap_count("  PASS ") == 11);
#else
    CK(cap_count("  PASS ") == 10);
#endif
}

/* 4. shell 的信息类子命令。 */
static void test_shell_read(void)
{
    CK(arkrtos_softbus_init() == 0);            /* 自检把表填满了，复位一次 */

    cap_clear();
    CK(arkrtos_softbus_shell_cmd(1, NULL) == 0);/* argc<2 走 info */
    CK(cap_has("softbus (ark_softbus "));

    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("info")};
        CK(arkrtos_softbus_shell_cmd(2, argv) == 0);
        CK(cap_has("node id   : 0x00001001"));
    }

    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("peers")};
        CK(arkrtos_softbus_shell_cmd(2, argv) == 0);
        /* 0x20 = ARK_CAP_ACTUATOR：sb_build 给常驻对端登记的能力位。 */
        CK(cap_has("peer 0: id=0x00001002 type=2 cap=0x00000020 online"));
    }

    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("rx")};
        CK(arkrtos_softbus_shell_cmd(2, argv) == 0);
        CK(cap_has("bus rx: msgs="));
    }

    /* pump 的三种参数形态：缺省 1 轮、显式 3 轮、解析不了回落 1 轮。 */
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("pump")};
        CK(arkrtos_softbus_shell_cmd(2, argv) == 0);
        CK(cap_has("bus pump: 1 round(s)"));
    }
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("pump"), LIT("3")};
        CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
        CK(cap_has("bus pump: 3 round(s)"));
    }
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("pump"), LIT("nope")};
        CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
        CK(cap_has("bus pump: 1 round(s)"));
    }

    /* 未知子命令：打 usage 并返回 -1。 */
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("zzz")};
        CK(arkrtos_softbus_shell_cmd(2, argv) == -1);
        CK(cap_has("usage: bus [info | selftest | pump [n] | peers | rx |"));
    }

    /* shell 入口的 selftest 子命令：仍然全过，并打 ALL PASS。 */
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("selftest")};
        CK(arkrtos_softbus_shell_cmd(2, argv) == 0);
        CK(cap_has("bus selftest: ALL PASS"));
    }
}

/* 5. shell 的收发类子命令：pub / rpc / stream，含参数个数与解析错误。 */
static void test_shell_data(void)
{
    CK(arkrtos_softbus_init() == 0);

    /* pub：参数不足 → usage + -1。 */
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("pub"), LIT("only/topic")};
        CK(arkrtos_softbus_shell_cmd(3, argv) == -1);
        CK(cap_has("usage: bus ["));
    }

    /* pub 正常：对端收到 1 条，主题与长度都对。 */
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("pub"), LIT("demo/one"), LIT("hello")};
        CK(arkrtos_softbus_shell_cmd(4, argv) == 0);
        /* 第一个数字是 ark_bus_publish 的返回值＝整帧字节数（含帧头与主题），
         * 不是载荷长度；载荷长度看括号里的 len=5。 */
        CK(cap_has(" B on demo/one, peer got 1 msg (demo/one len=5)"));
        CK(cap_has("bus pub: sent "));
    }

    /* rpc：参数不足 → -1；正常 → 应答回来，主键字节各自 +1。 */
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("rpc")};
        CK(arkrtos_softbus_shell_cmd(2, argv) == -1);
        CK(cap_has("usage: bus ["));
    }
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("rpc"), LIT("AB")};
        CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
        CK(cap_has("bus rpc: status=0 result_len=2 result=\"BC\""));
    }

    /* stream：缺省 200B、超上限被夹到 256B、0 被抬到 1B、坏数字回落 200B。 */
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("stream")};
        CK(arkrtos_softbus_shell_cmd(2, argv) == 0);
        CK(cap_has("sent=200 B, peer rx_calls=1 len=200 bytes_ok=1, done=1 status=0"));
    }
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("stream"), LIT("300")};
        CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
        CK(cap_has("sent=256 B, peer rx_calls=1 len=256"));
    }
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("stream"), LIT("0")};
        CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
        CK(cap_has("sent=1 B, peer rx_calls=1 len=1"));
    }
    /* 数字后面还跟着东西：解析失败 → 回落默认 200B（不是"读 7 就发 7"）。
     * 这条钉的是「宁可报错不给错答案」——静默按前缀理解参数等于给错答案。 */
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("stream"), LIT("7x")};
        CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
        CK(cap_has("sent=200 B, peer rx_calls=1 len=200"));
    }
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("stream"), LIT("abc")};
        CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
        CK(cap_has("sent=200 B, peer rx_calls=1 len=200"));
    }
    /* 溢出（32 位装不下）同样是解析失败。 */
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("stream"), LIT("99999999999999999999")};
        CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
        CK(cap_has("sent=200 B, peer rx_calls=1 len=200"));
    }
    /* 前后空白允许（shell 引号里带进来的形态）。 */
    cap_clear();
    {
        char *argv[] = {LIT("bus"), LIT("stream"), LIT(" 64 ")};
        CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
        CK(cap_has("sent=64 B, peer rx_calls=1 len=64"));
    }
}

/* 6. 超长参数三条：载荷超上限要报失败，参数超上限要报失败，参数刚好
 *    顶到上限时回复被截断到本地 32B 缓冲（result_len 报 32）。
 *    这三条对应 sb_cmd_pub / sb_cmd_rpc 里"发不出去"的那两个分支。 */
static void test_shell_limits(void)
{
    static char big[200];
    char       *argv[4];

    CK(arkrtos_softbus_init() == 0);

    (void)memset(big, 'q', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';

    /* 130 B 载荷 > ARK_BUS_MSG_MAX_SIZE(128)：publish 返回 -1，命令报错。 */
    argv[0] = LIT("bus");
    argv[1] = LIT("pub");
    argv[2] = LIT("demo/big");
    argv[3] = big;
    cap_clear();
    CK(arkrtos_softbus_shell_cmd(4, argv) == -1);
    CK(cap_has("bus pub: publish failed (-1)"));

    /* 130 B 参数 > 128-4 的 RPC 请求上限：请求发不出去，命令报错。 */
    argv[0] = LIT("bus");
    argv[1] = LIT("rpc");
    argv[2] = big;
    cap_clear();
    CK(arkrtos_softbus_shell_cmd(3, argv) == -1);
    CK(cap_has("bus rpc: request failed"));

    /* 124 B 参数刚好装得进请求（128-4），但对端回复容量只有 128-5=123，
     * 于是 handler 把长度夹到 123；本节点的结果缓冲只有 32B，再夹一次。
     * 关键读数是 result_len=32 而不是 124——截断必须发生在明处。 */
    big[124] = '\0';
    cap_clear();
    CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
    CK(cap_has("bus rpc: status=0 result_len=32 result="));
}

/* 7. 主题表打满之后 pub 必须老实报失败，不许假装发出去。
 *    节点侧与对端侧各有一张表（各 12 槽），一直换新主题名就会撞满。 */
static void test_topic_table_full(void)
{
    char  topic[16];
    char *argv[4];
    int   i;
    int   failed = 0;
    int   saw_sub_fail = 0;

    CK(arkrtos_softbus_init() == 0);

    for (i = 0; i < 24; i++) {
        (void)snprintf(topic, sizeof(topic), "f/%d", i);
        argv[0] = LIT("bus");
        argv[1] = LIT("pub");
        argv[2] = topic;
        argv[3] = LIT("v");
        cap_clear();
        if (arkrtos_softbus_shell_cmd(4, argv) != 0) {
            failed++;
            if (cap_has("peer subscribe failed") || cap_has("publish failed")) {
                saw_sub_fail = 1;
            }
        }
    }

    /* 12 槽装不下 24 个新主题：必须至少有失败，且失败是说清原因的。 */
    CK(failed > 0);
    CK(saw_sub_fail == 1);
}

#if (ARK_BUS_CRYPTO_ENABLED == 1)
/* 8. 加密档的接入层：bus key 的三条子命令、参数错误、装钥后的自检与收发。
 *    这一节只在 ARKRTOS_USE_BUS_CRYPTO=1 的构建里存在（默认关），它的存在
 *    本身就是"开关真能打开、打开后接入层仍然完整"的证据。
 *
 *    密钥不回显是硬要求，所以这里的断言数的是字节数，不是钥本身；
 *    "装进去了"由 crypto_state() 的 keyed 位证明。 */
static void test_crypto_shell(void)
{
    char *argv[4];

    CK(arkrtos_softbus_init() == 0);
    CK((arkrtos_softbus_crypto_state() & 0x1u) == 0u);   /* 复位后无钥 */

    /* --- show：还没装钥 --- */
    cap_clear();
    argv[0] = LIT("bus");
    argv[1] = LIT("key");
    argv[2] = LIT("show");
    CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
    CK(cap_has("bus crypto: keyed=no"));
    CK(cap_has("auth=hmac-sha256 tag="));
#if (ARK_BUS_ENCRYPT_ENABLED == 1)
    CK(cap_has("enc=chacha20"));
#else
    CK(cap_has("enc=off"));
#endif
#if (ARK_BUS_REPLAY_ENABLED == 1)
    CK(cap_has("replay=on"));
#else
    CK(cap_has("replay=off"));
#endif
    CK(cap_has("key_len=0"));

    /* --- 参数错误：三种都不许装进半把钥，且都必须报错 --- */
    cap_clear();
    argv[0] = LIT("bus");
    argv[1] = LIT("key");
    argv[2] = LIT("set");
    CK(arkrtos_softbus_shell_cmd(3, argv) != 0);
    CK(cap_has("usage: bus key set <hex>"));

    cap_clear();
    argv[3] = LIT("zz");
    CK(arkrtos_softbus_shell_cmd(4, argv) != 0);
    CK(cap_has("bus key: bad hex"));

    cap_clear();
    argv[3] = LIT("abc");                       /* 奇数个十六进制字符 */
    CK(arkrtos_softbus_shell_cmd(4, argv) != 0);
    CK(cap_has("bus key: bad hex"));

    {
        /* 比 KEY_MAX 多一个字节的钥。长度不能写死：KEY_MAX 是组件档位
         * （默认 32），写死 17 在默认档下是合法长度，用例会变成假绿。 */
        char bighex[2 * (ARK_BUS_CRYPTO_KEY_MAX + 1) + 1];
        int  i;

        for (i = 0; i < (int)sizeof(bighex) - 1; i++) {
            bighex[i] = 'a';
        }
        bighex[sizeof(bighex) - 1] = '\0';
        cap_clear();
        argv[3] = bighex;
        CK(arkrtos_softbus_shell_cmd(4, argv) != 0);
        CK(cap_has("bus key: bad hex"));
        CK((arkrtos_softbus_crypto_state() & 0x1u) == 0u);   /* 没装进去 */
    }

    /* --- 正常装钥：只报字节数，绝不回显钥 --- */
    cap_clear();
    argv[3] = LIT("0a1b2c3d4e5f6071");
    CK(arkrtos_softbus_shell_cmd(4, argv) == 0);
    CK(cap_has("bus key: 8 bytes installed"));
    CK(!cap_has("0a1b2c3d"));                   /* 钥不回显 */
    CK((arkrtos_softbus_crypto_state() & 0x1u) != 0u);

    /* argc==2 等价于 show。 */
    cap_clear();
    CK(arkrtos_softbus_shell_cmd(2, argv) == 0);
    CK(cap_has("bus crypto: keyed=yes"));
    CK(cap_has("key_len=8"));

    /* --- 装了钥跑自检：crypto 段真跑（不是"跳过"），且跑完钥还在 --- */
    cap_clear();
    CK(arkrtos_softbus_selftest() == 0);
    CK(cap_has("  PASS crypto"));
    CK(cap_has("failed 0"));
    CK(!cap_has("keyed checks skipped"));       /* 这一段确实执行了 */
    CK((arkrtos_softbus_crypto_state() & 0x1u) != 0u);

    /* 自检复位了总线，重新按装了钥的状态发起一次发布：链路仍然通。 */
    cap_clear();
    argv[0] = LIT("bus");
    argv[1] = LIT("pub");
    argv[2] = LIT("crypto/live");
    argv[3] = LIT("hello");
    CK(arkrtos_softbus_shell_cmd(4, argv) == 0);
    CK(cap_has("peer got 1 msg (crypto/live len=5)"));

    /* --- 大写十六进制同样接受：控制台上贴的钥常常是大写，
     * 不接受就是一个只有人手才会碰到的「看起来成功了」白给。 --- */
    cap_clear();
    argv[0] = LIT("bus");
    argv[1] = LIT("key");
    argv[2] = LIT("set");
    argv[3] = LIT("00112233AABBCCDD");
    CK(arkrtos_softbus_shell_cmd(4, argv) == 0);
    CK(cap_has("bus key: 8 bytes installed"));
    CK((arkrtos_softbus_crypto_state() & 0x1u) != 0u);

    /* --- clear：回到无钥，明文仍能通 --- */
    cap_clear();
    argv[0] = LIT("bus");
    argv[1] = LIT("key");
    argv[2] = LIT("clear");
    CK(arkrtos_softbus_shell_cmd(3, argv) == 0);
    CK(cap_has("bus key: cleared"));
    CK((arkrtos_softbus_crypto_state() & 0x1u) == 0u);

    cap_clear();
    argv[0] = LIT("bus");
    argv[1] = LIT("pub");
    argv[2] = LIT("crypto/live");
    argv[3] = LIT("plain");
    CK(arkrtos_softbus_shell_cmd(4, argv) == 0);
    CK(cap_has("peer got 1 msg (crypto/live len=5)"));

    /* --- 未知子命令：打 usage 返回错误 --- */
    cap_clear();
    argv[0] = LIT("bus");
    argv[1] = LIT("key");
    argv[2] = LIT("zzz");
    CK(arkrtos_softbus_shell_cmd(3, argv) != 0);
    CK(cap_has("usage: bus key [show | set <hex> | clear]"));
}
#endif /* ARK_BUS_CRYPTO_ENABLED == 1 */


int main(void)
{
    test_lifecycle();
    test_info();
    test_selftest();
    test_shell_read();
    test_shell_data();
    test_shell_limits();
    test_topic_table_full();
#if (ARK_BUS_CRYPTO_ENABLED == 1)
    test_crypto_shell();
#endif

    if (g_fails != 0) {
        printf("SOFTBUS-HOST FAILED  %d/%d checks failed\n", g_fails, g_checks);
        return 1;
    }
    printf("SOFTBUS-HOST OK  %d checks (crypto=%d encrypt=%d replay=%d)\n",
           g_checks, ARK_BUS_CRYPTO_ENABLED, ARK_BUS_ENCRYPT_ENABLED,
           ARK_BUS_REPLAY_ENABLED);
    return 0;
}
#endif /* ARKRTOS_USE_SOFTBUS */
