/**
* @file  sbuf_demo.c
* @brief 流缓冲 / 消息缓冲（SVC 0x1F）的非特权通道自检程序。
* @details 这份程序跑在 App 自己的非特权任务里，缓冲由内核创建、数据放在 App
*          自己的静态数组里。所有 sbuf / mbuf 调用都经 SVC 0x1F 进内核，因此它
*          同时验证四件事：
*
*            1. 用户态封装（`arkrtos_sbuf_*` / `arkrtos_mbuf_*`）能穿过 SVC 边界
*               拿到正确结果（返回的字节数、读回的字节）；
*            2. 内核在边界上做的参数校验真的生效——长度为 0 / 负值 / 源指针为空
*               一律被拒，且拒绝后缓冲内容一点没变；
*            3. 流语义与消息语义的差别：流"写进去多少算多少"，消息"整条原子、
*               装不下就整条丢"；
*            4. 生命周期：删除后句柄立刻失效。
*
*          口径修正（2026-11-13 上板实测，与最初的设想相反）：把 `main` 的地址
*          当源缓冲**是合法的**——App 的镜像本来就在 Flash 里，内核只要求源缓冲
*          落在调用者可见的 RAM/ROM 内。所以"递一个 Flash 地址就会被拒"不成立，
*          用例里保留了这条正向读数，免得下次又照错的设想写用例。
*
*          口径说明（这条也是实测出来的）：非特权侧的头文件里
*          **没有** SBUF 的错误码枚举，所以 App 只看得到"负值=失败"，分不清
*          TIMEOUT 与 PARAM。用例因此只在能确定的地方断言具体字节数，其余一律
*          断言符号——要精确到错误码，得在内核侧（`utest` 那两个用例）看。
*/

#include <stdio.h>
#include <stdint.h>

#include "arkrtos.h"

/* App 自己的数据区：收发缓冲都落在这里，内核通过 SVC 读写它。 */
static uint8_t s_tx[256];
static uint8_t s_rx[256];

static uint32_t s_pass;
static uint32_t s_fail;

static void chk(int cond, const char *what)
{
    if (cond) {
        s_pass++;
        printf("  PASS  %s\n", what);
    } else {
        s_fail++;
        printf("  FAIL  %s\n", what);
    }
}

int main(void);

/* 把 main 的入口地址当"内核 Flash 里的地址"样本用：App 侧工程不许出现布局常量，
 * 而这个地址一定落在 App 自己的 ROM 窗口外，正好用来试"外来缓冲必须被拒"。 */
static const void *kernel_flash_sample(void)
{
    return (const void *)(uintptr_t)(void (*)(void))main;
}

static void fill(uint8_t *p, uint32_t n, uint8_t seed)
{
    uint32_t i;

    for (i = 0u; i < n; i++) {
        p[i] = (uint8_t)(seed + i);
    }
}

int main(void)
{
    int32_t  h;
    int32_t  r;
    int32_t  cap;
    uint32_t i;
    uint8_t  ok;

    printf("\n--- sbuf / mbuf demo (SVC 0x1F) ---\n");

    /* 1. 创建 + 初始状态 */
    h = arkrtos_sbuf_create("appsb");
    printf("create -> %d\n", (int)h);
    chk(h >= 0, "create returns a handle");
    if (h < 0) {
        goto done;
    }

    cap = arkrtos_sbuf_free(h);
    printf("capacity=%d used=%d\n", (int)cap, (int)arkrtos_sbuf_used(h));
    chk(cap > 0, "a fresh buffer reports free space");
    chk(arkrtos_sbuf_used(h) == 0, "a fresh buffer is empty");
    if (cap <= 0 || cap > (int32_t)sizeof(s_tx)) {
        chk(0, "the reported capacity fits the test buffers");
        goto del;
    }

    /* 2. 流语义：写进去多少算多少，读多少拿多少，顺序不变 */
    r = arkrtos_sbuf_send(h, (const void *)"hello", 5, 0);
    printf("send(\"hello\",5) -> %d\n", (int)r);
    chk(r == 5, "a short write takes the whole 5 bytes");
    chk(arkrtos_sbuf_used(h) == 5, "used() follows the write");

    r = arkrtos_sbuf_recv(h, (void *)s_rx, 2, 0);
    chk(r == 2 && s_rx[0] == 'h' && s_rx[1] == 'e', "a partial read takes the first two bytes");
    r = arkrtos_sbuf_recv(h, (void *)s_rx, 8, 0);
    chk(r == 3 && s_rx[0] == 'l' && s_rx[2] == 'o', "the rest comes out in order");
    chk(arkrtos_sbuf_used(h) == 0, "the buffer is drained");

    /* 3. 空读不等待：必须报负值，而不是返回 0 假装读到了 */
    r = arkrtos_sbuf_recv(h, (void *)s_rx, 4, 0);
    printf("recv(empty,4,0) -> %d\n", (int)r);
    chk(r < 0, "reading an empty buffer fails instead of returning 0");

    /* 4. 写满 + 部分成功 + 再写被拒；reset 后恢复 */
    fill(s_tx, (uint32_t)cap, 0x30u);
    r = arkrtos_sbuf_send(h, (const void *)s_tx, cap, 0);
    printf("send(cap=%d) -> %d\n", (int)cap, (int)r);
    chk(r == cap, "one write can fill the buffer exactly");
    chk(arkrtos_sbuf_free(h) == 0, "free() reports zero when full");
    r = arkrtos_sbuf_send(h, (const void *)s_tx, 1, 0);
    chk(r < 0, "writing to a full buffer fails instead of dropping bytes");
    chk(arkrtos_sbuf_reset(h) == 0, "reset returns 0");
    chk(arkrtos_sbuf_used(h) == 0 && arkrtos_sbuf_free(h) == cap,
        "reset gives the whole buffer back");

    /* 5. 回绕之后顺序不乱：写满、读掉前半、再写后半，读回来必须仍是 0..cap-1 */
    fill(s_tx, (uint32_t)cap, 0x00u);          /* s_tx[i] = i & 0xFF */
    (void)arkrtos_sbuf_send(h, (const void *)s_tx, cap, 0);
    (void)arkrtos_sbuf_recv(h, (void *)s_rx, cap / 2, 0);
    r = arkrtos_sbuf_send(h, (const void *)s_tx, cap / 2, 0);
    chk(r == cap / 2, "writing again after a partial read succeeds");
    r = arkrtos_sbuf_recv(h, (void *)s_rx, cap, 0);
    chk(r == cap, "the wrapped buffer still holds a full buffer worth of bytes");

    ok = 1u;
    for (i = 0u; i < (uint32_t)cap; i++) {
        if (s_rx[i] != (uint8_t)((i + (uint32_t)(cap / 2)) & 0xFFu)) {
            ok = 0u;
        }
    }
    chk(ok == 1u, "bytes stay in order across a wrap");

    /* 6. 消息语义：整条原子；接收缓冲装不下就整条丢，且不串下一条 */
    (void)arkrtos_sbuf_reset(h);
    r = arkrtos_mbuf_send(h, (const void *)"msg-1", 5, 0);
    printf("mbuf_send(\"msg-1\",5) -> %d\n", (int)r);
    chk(r == 0, "a message send returns 0 on success");
    r = arkrtos_mbuf_recv(h, (void *)s_rx, sizeof(s_rx), 0);
    chk(r == 5 && s_rx[0] == 'm' && s_rx[4] == '1', "the message comes back whole");

    (void)arkrtos_mbuf_send(h, (const void *)"0123456789", 10, 0);
    (void)arkrtos_mbuf_send(h, (const void *)"next", 4, 0);
    r = arkrtos_mbuf_recv(h, (void *)s_rx, 4, 0);
    printf("mbuf_recv(4) -> %d\n", (int)r);
    chk(r < 0, "a message longer than the receive buffer is refused");
    r = arkrtos_mbuf_recv(h, (void *)s_rx, sizeof(s_rx), 0);
    chk(r == 4 && s_rx[0] == 'n', "the refused message did not smear into the next one");

    /* 7. 边界校验：长度/指针非法必须被拒，且状态一点不变。
     * 口径修正（这次上板测出来的）：把 `main` 的地址当源缓冲**是合法的**——
     * App 的镜像本来就在 Flash 里，内核只要求源缓冲落在调用者可见的 RAM/ROM
     * 内，而 App 自己的常量区正好满足。所以「拿一个 Flash 地址去试就会被拒」
     * 这条不成立（实测返回 4），真正被拒的是长度与空指针。 */
    (void)arkrtos_sbuf_reset(h);
    r = arkrtos_sbuf_send(h, (const void *)s_tx, 0, 0);
    printf("send(len=0) -> %d\n", (int)r);
    chk(r < 0, "a zero length send is refused");
    r = arkrtos_sbuf_send(h, (const void *)s_tx, -1, 0);
    chk(r < 0, "a negative length send is refused");
    r = arkrtos_sbuf_send(h, (const void *)0, 4, 0);
    chk(r < 0, "a null source buffer is refused");
    chk(arkrtos_sbuf_used(h) == 0, "refused sends left the buffer alone");

    /* 顺带把上面那条口径测出来并打印，免得下次又按"Flash 地址会被拒"写用例 */
    r = arkrtos_sbuf_send(h, kernel_flash_sample(), 4, 0);
    printf("send(app flash ptr) -> %d (App 自己的常量区是合法源)\n", (int)r);
    chk(r == 4, "the app's own flash is a legal source buffer");
    (void)arkrtos_sbuf_reset(h);

    /* 8. 阻塞超时：App 侧没有"让出"的通道，封装是 1 ms 轮询 + 重试直到超时。
     *    所以这里能量的是"超时有没有被尊重"：早醒（没真等到）或者不醒（挂住）
     *    都会让金标失败。App 的 oslib 里有毫秒口径的 arkrtos_get_time_ms()。 */
    (void)arkrtos_sbuf_reset(h);
    {
        uint32_t t0 = arkrtos_get_time_ms();
        uint32_t dt;

        r = arkrtos_sbuf_recv(h, (void *)s_rx, 4, 100);
        dt = arkrtos_get_time_ms() - t0;
        printf("recv(empty, 100ms) -> %d in %u ms\n", (int)r, (unsigned)dt);
        chk(r < 0, "a blocking read on an empty buffer times out");
        chk(dt >= 99u && dt <= 130u, "the read waited its full 100 ms");

        /* 写满之后带超时写：没有读者，写不完就必须报超时而不是把控制台挂住 */
        fill(s_tx, sizeof(s_tx), 0);
        (void)arkrtos_sbuf_send(h, (const void *)s_tx, (int)sizeof(s_tx), 0);
        t0 = arkrtos_get_time_ms();
        r = arkrtos_sbuf_send(h, (const void *)"z", 1, 50);
        dt = arkrtos_get_time_ms() - t0;
        printf("send(full buffer, 50ms) -> %d in %u ms\n", (int)r, (unsigned)dt);
        chk(r < 0, "a blocking write on a full buffer times out");
        chk(dt >= 49u && dt <= 80u, "the write waited its full 50 ms");
        (void)arkrtos_sbuf_reset(h);
    }

    /* 9. 生命周期：删除后句柄立刻失效 */
del:
    r = arkrtos_sbuf_delete(h);
    printf("delete -> %d\n", (int)r);
    chk(r == 0, "delete returns 0");
    chk(arkrtos_sbuf_used(h) < 0, "used() on a deleted handle fails");
    chk(arkrtos_sbuf_send(h, (const void *)"x", 1, 0) < 0,
        "send on a deleted handle fails");

done:
    printf("SBUF: %u pass, %u fail\n", (unsigned)s_pass, (unsigned)s_fail);
    printf("SBUF: %s\n", (s_fail == 0u) ? "ALL PASS" : "FAILED");
    return (s_fail == 0u) ? 0 : 1;
}
