/**
* @file  main.c
* @brief 32 位事件集（SVC 0x21）的非特权通道自检程序。
* @details 这份程序跑在 App 自己的非特权任务里（ARKRTOS_USE_PRIV=1），
*          每一次 arkrtos_eset_* 调用都穿一次 SVC 边界进内核。它验证三件事：
*
*            1. 用户态封装能穿过 SVC 0x21 拿到正确结果——包括返回值和
*               **写回调用方缓冲的位**（SVC 里参数是按地址传的，写回错了
*               只有板端才看得出来）；
*            2. 事件集的四条语义：set 累加式且不丢事件（没有等待者时位留在
*               对象里）、OR / AND 必须二选一、CLEAR 由拿到位的这一方清除、
*               非法参数与坏句柄都要被拒且不改动对象状态；
*            3. 生命周期：delete 之后句柄立刻失效，任何接口都不许再报成功。
*
*          全部判据都打印 PASS/FAIL，最后一行汇总；任何一个 FAIL 都会让
*          `main` 返回非 0，串口上看最后两行就够。
*
* @note 本程序**不阻塞**：所有 wait 都用 timeout=0（只试一次），
*       所以它跑完就退场，不会把槽位占着。
*/

#include <stdio.h>
#include <stdint.h>

#include "arkrtos.h"

#define BIT0    (1u << 0)
#define BIT1    (1u << 1)
#define BIT2    (1u << 2)
#define BIT3    (1u << 3)

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

int main(void)
{
    int32_t  h;
    int32_t  h2;
    int32_t  rc;
    uint32 got;
    uint32 flags;

    printf("--- eset demo (SVC 0x21) ---\n");

    /* ---- 创建与初值 ---- */
    h = arkrtos_eset_create("eset");
    chk(h >= 0, "create -> handle");
    if (h < 0) {
        printf("ESET: cannot create, abort\n");
        return 1;
    }
    printf("handle = %d\n", (int)h);

    flags = 0xFFFFFFFFu;
    chk(arkrtos_eset_get(h, &flags) == 0, "get -> 0");
    chk(flags == 0u, "fresh set has no bits");

    /* ---- set 是累加，且没有等待者时位留在对象里 ---- */
    chk(arkrtos_eset_set(h, BIT0 | BIT2) == 0, "set bit0|bit2 -> 0");
    chk(arkrtos_eset_get(h, &flags) == 0 && flags == (BIT0 | BIT2),
        "get -> bit0|bit2");
    chk(arkrtos_eset_set(h, BIT1) == 0, "set bit1 -> 0 (accumulate)");
    chk(arkrtos_eset_get(h, &flags) == 0 && flags == (BIT0 | BIT1 | BIT2),
        "get -> bit0|bit1|bit2");

    /* ---- OR：任一位成立即命中，p_got 只含当时成立的位 ---- */
    got = 0u;
    rc = arkrtos_eset_wait(h, BIT1 | BIT3, ARKRTOS_ESET_OPT_OR, &got, 0);
    chk(rc == 0, "wait OR(bit1|bit3) -> 0");
    chk(got == BIT1, "wait OR writes back only the bits that were set");
    chk(arkrtos_eset_get(h, &flags) == 0 && flags == (BIT0 | BIT1 | BIT2),
        "OR without CLEAR leaves the bits in the object");

    /* ---- AND：少一位就不命中，返回 TIMEOUT 且不写回假值成 0 以外的东西 ---- */
    got = 0u;
    rc = arkrtos_eset_wait(h, BIT0 | BIT3, ARKRTOS_ESET_OPT_AND, &got, 0);
    chk(rc == ARKRTOS_SYNC_ERR_TIMEOUT, "wait AND(bit0|bit3) -> TIMEOUT(-2)");
    chk(got == 0u, "AND miss writes back 0");

    got = 0u;
    rc = arkrtos_eset_wait(h, BIT0 | BIT2, ARKRTOS_ESET_OPT_AND, &got, 0);
    chk(rc == 0 && got == (BIT0 | BIT2), "wait AND(bit0|bit2) -> 0, got == mask");

    /* ---- CLEAR：拿到位的这一方清掉它，剩下的位原样保留 ---- */
    chk(arkrtos_eset_set(h, BIT3) == 0, "set bit3 -> 0");
    got = 0u;
    rc = arkrtos_eset_wait(h, BIT0 | BIT3, ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_CLEAR,
                          &got, 0);
    chk(rc == 0 && got == (BIT0 | BIT3), "wait OR|CLEAR(bit0|bit3) -> got both");
    chk(arkrtos_eset_get(h, &flags) == 0 && flags == (BIT1 | BIT2),
        "CLEAR removed exactly the handed-over bits");

    /* ---- clear：按掩码清，未命中的位不受影响 ---- */
    chk(arkrtos_eset_clear(h, BIT1) == 0, "clear bit1 -> 0");
    chk(arkrtos_eset_get(h, &flags) == 0 && flags == BIT2, "get -> bit2");
    chk(arkrtos_eset_clear(h, ~BIT2) == 0, "clear everything but bit2 -> 0");
    chk(arkrtos_eset_get(h, &flags) == 0 && flags == BIT2, "bit2 survived");

    /* ---- 非法参数：OR / AND 必须二选一；mask 不能为 0；输出指针不能为空 ---- */
    got = 0u;
    chk(arkrtos_eset_wait(h, BIT2, 0u, &got, 0) == -1 /* ERR_OPT */,
        "wait with neither OR nor AND -> -1");
    chk(arkrtos_eset_wait(h, BIT2, ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_AND,
                          &got, 0) == -1 /* ERR_OPT */,
        "wait with both OR and AND -> -1");
    chk(arkrtos_eset_wait(h, 0u, ARKRTOS_ESET_OPT_OR, &got, 0) == -1 /* ERR_PARAM */,
        "wait mask 0 -> -1");
    chk(arkrtos_eset_wait(h, BIT2, ARKRTOS_ESET_OPT_OR, (uint32 *)0, 0) == -1 /* ERR_PARAM */,
        "wait p_got NULL -> -1");
    chk(arkrtos_eset_set(h, 0u) == -1 /* ERR_PARAM */, "set mask 0 -> -1");
    chk(arkrtos_eset_get(h, (uint32 *)0) == -1 /* ERR_PARAM */, "get NULL -> -1");
    chk(arkrtos_eset_get(h, &flags) == 0 && flags == BIT2,
        "rejected calls did not touch the object");

    /* ---- 第二个对象独立：位不串台 ---- */
    h2 = arkrtos_eset_create("eset2");
    chk(h2 >= 0 && h2 != h, "second create -> distinct handle");
    if (h2 < 0) {
        printf("ESET: second create failed\n");
        s_fail++;
    } else {
        chk(arkrtos_eset_set(h2, BIT3) == 0, "set bit3 on the second object");
        chk(arkrtos_eset_get(h2, &flags) == 0 && flags == BIT3, "second object -> bit3");
        chk(arkrtos_eset_get(h, &flags) == 0 && flags == BIT2,
            "first object untouched by the second");
    }

    /* ---- 生命周期：删除后句柄立刻失效 ---- */
    chk(arkrtos_eset_delete(h) == 0, "delete -> 0");
    flags = 0xFFFFFFFFu;
    chk(arkrtos_eset_get(h, &flags) == -1 /* ERR_STATE */, "get after delete -> -1");
    chk(arkrtos_eset_set(h, BIT0) == -1 /* ERR_STATE */, "set after delete -> -1");
    got = 0u;
    chk(arkrtos_eset_wait(h, BIT0, ARKRTOS_ESET_OPT_OR, &got, 0) == -1 /* ERR_STATE */,
        "wait after delete -> -1");
    chk(arkrtos_eset_delete(h) == -1 /* ERR_STATE */, "delete twice -> -1");
    if (h2 >= 0) {
        chk(arkrtos_eset_delete(h2) == 0, "delete the second object -> 0");
    }

    printf("ESET: %u pass, %u fail\n", s_pass, s_fail);
    printf("ESET: %s\n", (s_fail == 0u) ? "ALL PASS" : "FAILED");
    return (s_fail == 0u) ? 0 : 1;
}
