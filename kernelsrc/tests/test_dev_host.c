/**
* @file test_dev_host.c
* @brief 设备框架（arkrtos_dev.c）的宿主单元测试
* @details 这一层是"名字 → 槽位 → 实例"三张表加一本引用计数，全部记账都在
*          宿主上走得通，而记错的后果都属于**静默**的那一类：
*
*            - 槽位搬移会让已发放的句柄指向另一个设备（注销必须留墓碑）；
*            - 重复 open 如果重新初始化硬件，会把第一个持有者的实例变成孤儿
*              （App 打开控制台 UART，内核 shell 从此收不到输入）；
*            - 一个实例多个持有者时，非最后一个 close 就去关硬件，会把别人的
*              设备一起关掉；
*            - 句柄用槽位下标编码，越界句柄必须当场拒，不能拿它去索引表。
*
*          被测的是真实的 arkrtos_dev.c；驱动侧用会记账的假驱动。两档都编：
*          ARKRTOS_HW_SAFE=1（走所有权台账 claim/release）与 =0（与引入该机制
*          前完全一致）。两档是两段代码，必须分别编一次才看得见。
*
*          覆盖范围（如实说明）：
*            已验证：注册/重名/名字截断/表满、注销的墓碑语义、槽位不连续时的
*                    遍历、句柄标志与越界、实例复用与引用计数上限、多个持有者
*                    下的收尾顺序、read/write/ctrl 的实现缺失与未打开、按实例
*                    指针的 ctrl（遗言路径）、所有权台账的调用时机。
*            未验证：真硬件时序、并发（两个任务同时 open/close 的竞争）、
*                    驱动内部行为；这些要板子与真调度。
* @note 用法：由 tools/coverage_gate.py 的两条 case 编译并运行，输出
*       DEV-HOST OK（HW_SAFE=1）或 DEV-HOST NOSAFE OK（HW_SAFE=0）。
*/

#include "arkrtos_dev.h"

#if ARKRTOS_HW_SAFE
#include "arkrtos_hwsafe.h"
#endif

#include <stdio.h>

#if (ARKRTOS_DEV_MAX_NUM < 8)
#error "test_dev_host.c expects ARKRTOS_DEV_MAX_NUM >= 8"
#endif

/* ------------------------------------------------------------------
 * 断言
 * ------------------------------------------------------------------ */
static int g_checks = 0;
static int g_fails  = 0;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

#define CK_EQ(a, b, msg)                                                     \
    do {                                                                     \
        long va = (long)(a);                                                 \
        long vb = (long)(b);                                                 \
        g_checks++;                                                          \
        if(va != vb)                                                         \
        {                                                                    \
            g_fails++;                                                       \
            printf("FAIL %s:%d %s (got %ld, want %ld)\n",                    \
                   __FILE__, __LINE__, msg, va, vb);                         \
        }                                                                    \
    } while(0)

/* ------------------------------------------------------------------
 * 假驱动
 * ------------------------------------------------------------------ */
static int      g_open_calls;
static int      g_open_null;        /* 1 = drv_open 交不出实例            */
static int      g_close_calls;
static int      g_close_null;       /* 收尾时拿到空实例的次数（双关路径） */
static int      g_read_calls;
static int      g_write_calls;
static int      g_ctrl_calls;
static uint32   g_open_dev_num;
static uint32   g_open_param;
static int32    g_close_rc;
static int32    g_read_rc;
static int32    g_write_rc;
static int32    g_ctrl_rc;
static uint32   g_ctrl_code;
static uint32   g_ctrl_value;
static int32    g_rw_len;
static uint8   *g_rw_data;
static int      g_rw_which;         /* 1 = read，2 = write               */

static arkrtos_dev_hdr_t g_inst[2];

static arkrtos_dev_hdr_t *fake_open(uint32 dev_id, uint32 param)
{
    g_open_calls++;
    g_open_dev_num = dev_id;
    g_open_param   = param;
    if(g_open_null != 0)
    {
        return 0;
    }
    return &g_inst[(param != 0u) ? 1u : 0u];
}

static int32 fake_close(arkrtos_dev_hdr_t *obj)
{
    g_close_calls++;
    if(obj == 0)
    {
        g_close_null++;
    }
    return g_close_rc;
}

static int32 fake_read(arkrtos_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    (void)obj;
    g_read_calls++;
    g_rw_which = 1;
    g_rw_data  = pdata;
    g_rw_len   = len;
    return g_read_rc;
}

static int32 fake_write(arkrtos_dev_hdr_t *obj, uint8 *pdata, int32 len)
{
    (void)obj;
    g_write_calls++;
    g_rw_which = 2;
    g_rw_data  = pdata;
    g_rw_len   = len;
    return g_write_rc;
}

static int32 fake_ctrl(arkrtos_dev_hdr_t *obj, uint32 code, uint32 value)
{
    (void)obj;
    g_ctrl_calls++;
    g_ctrl_code  = code;
    g_ctrl_value = value;
    return g_ctrl_rc;
}

/* 全功能驱动 / 只有 open（其余入口缺失，走 NOIMPL）/ 连 open 都没有 */
static arkrtos_dev_drv_t g_drv_full =
    { fake_open, fake_close, fake_read, fake_write, fake_ctrl };
static arkrtos_dev_drv_t g_drv_open_only = { fake_open, 0, 0, 0, 0 };
static arkrtos_dev_drv_t g_drv_no_open   = { 0, 0, 0, 0, 0 };

/* ------------------------------------------------------------------
 * 所有权台账桩（HW_SAFE 档）
 * ------------------------------------------------------------------ */
#if ARKRTOS_HW_SAFE
/* 内核里的同名全局变量（arkrtos_task.c:405），这里给出宿主定义。 */
int32 arkrtos_current_task_id = 0;

static int   g_resets;
static int   g_claims;
static int   g_releases;
static int   g_unregisters;
static char  g_claim_name[16];
static int32 g_claim_owner;
static arkrtos_dev_hdr_t *g_claim_obj;

void arkrtos_hwsafe_reset(void)
{
    g_resets++;
    g_claims   = 0;
    g_releases = 0;
    g_unregisters = 0;
    g_claim_name[0] = 0;
    g_claim_owner = 0;
    g_claim_obj   = 0;
}

void arkrtos_hwsafe_claim(const char *dev_name, int32 owner,
                          arkrtos_dev_hdr_t *obj)
{
    int i;

    g_claims++;
    for(i = 0; (i < 15) && (dev_name[i] != 0); i++)
    {
        g_claim_name[i] = dev_name[i];
    }
    g_claim_name[i] = 0;
    g_claim_owner   = owner;
    g_claim_obj     = obj;
}

void arkrtos_hwsafe_release(const char *dev_name, int32 owner)
{
    (void)dev_name;
    (void)owner;
    g_releases++;
}

int32 arkrtos_hwsafe_unregister(const char *dev_name)
{
    (void)dev_name;
    g_unregisters++;
    return 0;
}
#endif /* ARKRTOS_HW_SAFE */

/* ------------------------------------------------------------------
 * 复位
 * ------------------------------------------------------------------ */
static void reset_all(void)
{
    arkrtos_dev_module_init();

    g_open_calls    = 0;
    g_open_null     = 0;
    g_close_calls   = 0;
    g_close_null    = 0;
    g_read_calls    = 0;
    g_write_calls   = 0;
    g_ctrl_calls    = 0;
    g_open_dev_num  = 0xFFFFFFFFu;
    g_open_param    = 0xFFFFFFFFu;
    g_close_rc      = 0;
    g_read_rc       = 0;
    g_write_rc      = 0;
    g_ctrl_rc       = 0;
    g_ctrl_code     = 0xFFFFFFFFu;
    g_ctrl_value    = 0xFFFFFFFFu;
    g_rw_len        = 0;
    g_rw_data       = 0;
    g_rw_which      = 0;

#if ARKRTOS_HW_SAFE
    g_resets = 0;
    g_claims = 0;
    g_releases = 0;
    g_unregisters = 0;
    g_claim_name[0] = 0;
    g_claim_owner = 0;
    g_claim_obj = 0;
#endif
}

/* ------------------------------------------------------------------
 * 1) 注册的参数闸门
 * ------------------------------------------------------------------ */
static void test_register_rejects(void)
{
    reset_all();

    CK_EQ(arkrtos_dev_register(0, &g_drv_full, 0u), ARKRTOS_DEV_ERR_ARG,
          "a null name is refused");
    CK_EQ(arkrtos_dev_register("x", 0, 0u), ARKRTOS_DEV_ERR_ARG,
          "a null driver is refused");
    CK_EQ(arkrtos_dev_get_count(), 0, "a refused registration leaves no trace");

#if ARKRTOS_HW_SAFE
    /* reset_all() 先把计数清零再调 module_init，所以这里要自己再造一次
     * init，才看得见它确实去复位了台账。 */
    arkrtos_dev_module_init();
    CK_EQ(g_resets, 1, "module init resets the ownership ledger");
#endif
}

/* ------------------------------------------------------------------
 * 2) 注册 / 重名 / 名字截断 / 表满
 * ------------------------------------------------------------------ */
static void test_register_table(void)
{
    int i;
    char nm[16];
    char small[4];

    reset_all();

    CK_EQ(arkrtos_dev_register("uart0", &g_drv_full, 7u), 0, "first device");
    CK_EQ(arkrtos_dev_get_count(), 1, "count follows registration");
    CK_EQ(arkrtos_dev_register("uart0", &g_drv_full, 7u),
          ARKRTOS_DEV_ERR_EXIST, "the same name is refused");

    /* 名字只有 8 字节（含结尾 0）：注册时截到 7 个字符 */
    CK_EQ(arkrtos_dev_register("abcdefghij", &g_drv_full, 0u), 0,
          "a long name is stored truncated");
    CK_EQ(arkrtos_dev_name_at(1u, nm, (uint32)sizeof nm), 0, "read it back");
    ck(nm[0] == 'a' && nm[6] == 'g' && nm[7] == 0,
       "the stored name is exactly the first seven characters");
    CK_EQ(arkrtos_dev_register("abcdefg", &g_drv_full, 0u),
          ARKRTOS_DEV_ERR_EXIST,
          "a name that truncates onto an existing one is refused");

    /* 表满：当前 2 台，再注册 6 台正好满，第 9 台拒绝 */
    for(i = 0; i < (ARKRTOS_DEV_MAX_NUM - 2); i++)
    {
        char name[8];

        name[0] = 'd';
        name[1] = (char)('0' + i);
        name[2] = 0;
        CK_EQ(arkrtos_dev_register(name, &g_drv_full, (uint32)i), 0,
              "fill the table");
    }
    CK_EQ(arkrtos_dev_get_count(), ARKRTOS_DEV_MAX_NUM, "the table is full");
    CK_EQ(arkrtos_dev_register("over", &g_drv_full, 0u), ARKRTOS_DEV_ERR_NOSLOT,
          "one more device does not fit");

    /* 腾出一个槽位就能再注册 */
    CK_EQ(arkrtos_dev_unregister("uart0"), 0, "free one slot");
    CK_EQ(arkrtos_dev_get_count(), ARKRTOS_DEV_MAX_NUM - 1, "count drops");
    CK_EQ(arkrtos_dev_register("late", &g_drv_full, 0u), 0,
          "the freed slot is reusable");
    /* 空槽位按最低下标分配：被 uart0 腾出来的 0 号位就是新设备的位置。
     * 把它读出来确认，再注销掉，才能看到"空槽位报 STATE"。 */
    CK_EQ(arkrtos_dev_name_at(0u, nm, (uint32)sizeof nm), 0,
          "the new device took the lowest free slot");
    ck(nm[0] == 'l' && nm[1] == 'a' && nm[2] == 't' && nm[3] == 'e' &&
       nm[4] == 0, "and it is the name we registered");
    CK_EQ(arkrtos_dev_unregister("late"), 0, "free that slot again");
    CK_EQ(arkrtos_dev_get_count(), ARKRTOS_DEV_MAX_NUM - 1,
          "count drops again");

    /* name_at 的四道闸门 */
    CK_EQ(arkrtos_dev_name_at(0u, 0, 8u), ARKRTOS_DEV_ERR_PARAM,
          "a null output buffer is refused");
    CK_EQ(arkrtos_dev_name_at(0u, nm, 0u), ARKRTOS_DEV_ERR_PARAM,
          "a zero length is refused");
    CK_EQ(arkrtos_dev_name_at((uint32)ARKRTOS_DEV_MAX_NUM, nm, 8u),
          ARKRTOS_DEV_ERR_HANDLE, "an index past the table is refused");
    nm[0] = 'Z';
    CK_EQ(arkrtos_dev_name_at(0u, nm, 8u), ARKRTOS_DEV_ERR_STATE,
          "the freed slot reports STATE");
    ck(nm[0] == 0, "a failed name_at never leaves a stale string behind");

    /* 缓冲太短：截断且以 0 结尾；max=1 只能是空串 */
    CK_EQ(arkrtos_dev_name_at(1u, small, (uint32)sizeof small), 0,
          "a short buffer still succeeds");
    ck(small[0] == 'a' && small[1] == 'b' && small[2] == 'c' && small[3] == 0,
       "a short buffer truncates to max-1 characters");
    CK_EQ(arkrtos_dev_name_at(1u, small, 1u), 0, "max=1 still succeeds");
    ck(small[0] == 0, "max=1 yields the empty string");
}

/* ------------------------------------------------------------------
 * 3) 注销：墓碑语义与相邻槽位不受影响
 * ------------------------------------------------------------------ */
static void test_unregister(void)
{
    int32 h_a;
    int32 h_b;
    uint8 buf[4];

    reset_all();

    CK_EQ(arkrtos_dev_unregister(0), ARKRTOS_DEV_ERR_PARAM,
          "a null name is refused");
    CK_EQ(arkrtos_dev_unregister("ghost"), ARKRTOS_DEV_ERR_NOTFOUND,
          "an unknown name is refused");

    CK_EQ(arkrtos_dev_register("a", &g_drv_full, 1u), 0, "register a");
    CK_EQ(arkrtos_dev_register("b", &g_drv_full, 2u), 0, "register b");

    h_a = arkrtos_dev_open_internal("a", 0u);
    h_b = arkrtos_dev_open_internal("b", 1u);
    ck(h_a != h_b, "two devices get two handles");
    CK_EQ(h_a & 0x000FFFFF, 0, "the first device sits in slot 0");

    /* 注销 a：b 的句柄必须一位不变（槽位不搬移） */
    CK_EQ(arkrtos_dev_unregister("a"), 0, "unregister a");
    CK_EQ(arkrtos_dev_get_count(), 1, "count follows unregister");
    CK_EQ(arkrtos_dev_read_internal(h_b, buf, (int32)sizeof buf), 0,
          "the other device's handle still works");
    CK_EQ(g_read_calls, 1, "and it reached the driver");

    /* 墓碑：旧句柄还指向原槽位，但表里已经没有设备，读写必须当场拒 */
    CK_EQ(arkrtos_dev_read_internal(h_a, buf, (int32)sizeof buf),
          ARKRTOS_DEV_ERR_STATE, "the stale handle is refused, not redirected");
    CK_EQ(g_read_calls, 1, "and it never reached the driver");
    CK_EQ(arkrtos_dev_unregister("a"), ARKRTOS_DEV_ERR_NOTFOUND,
          "a second unregister finds nothing");

    /* 再注册同名设备会占用同一槽位，旧句柄因此"复活"指向新实例。
     * 这是"句柄=槽位下标"的固有代价，不是这次要改的行为——钉住它，
     * 免得以后有人以为旧句柄是安全的。 */
    CK_EQ(arkrtos_dev_register("a", &g_drv_full, 3u), 0, "register a again");
    CK_EQ(arkrtos_dev_open_internal("a", 0u), h_a,
          "the reused slot yields the same handle value");
}

/* ------------------------------------------------------------------
 * 4) 打开：实例复用、引用计数、上限
 * ------------------------------------------------------------------ */
static void test_open_refcount(void)
{
    int32 h0;
    int32 h1;
    int i;

    reset_all();

    CK_EQ(arkrtos_dev_register("con", &g_drv_full, 5u), 0, "register");
    h0 = arkrtos_dev_open_internal("con", 0u);
    ck(h0 >= 0, "open succeeds");
    CK_EQ(h0, ARKRTOS_DEV_HANDLE_FLAG, "the handle is slot 0 plus the flag");
    CK_EQ(g_open_calls, 1, "the driver was opened once");
    CK_EQ(g_open_dev_num, 5u, "the driver got its device number");
    CK_EQ(g_open_param, 0u, "and the caller's parameter");
#if ARKRTOS_HW_SAFE
    CK_EQ(g_claim_obj, &g_inst[0],
          "the instance reaches the ownership ledger");
    CK_EQ(g_claims, 1, "one claim per open");
    CK_EQ(g_claim_owner, arkrtos_current_task_id, "claimed for the caller");
#endif

    /* 重复 open：同一个实例、同一个句柄，不许再叫一次 drv_open */
    h1 = arkrtos_dev_open_internal("con", 1u);
    CK_EQ(h1, h0, "a second opener gets the same handle");
    CK_EQ(g_open_calls, 1, "the hardware is not re-initialised");
    CK_EQ(g_open_param, 0u, "the second param never reached the driver");
#if ARKRTOS_HW_SAFE
    CK_EQ(g_claims, 2, "the second opener is still recorded");
#endif

    /* 两个持有者：第一次 close 只减计数，第二次才关硬件 */
    CK_EQ(arkrtos_dev_close_internal(h0), 0, "first close succeeds");
    CK_EQ(g_close_calls, 0, "the hardware is left alone");
#if ARKRTOS_HW_SAFE
    CK_EQ(g_releases, 1, "the first holder withdraws its claim");
#endif
    g_close_rc = -7;
    CK_EQ(arkrtos_dev_close_internal(h0), -7,
          "the last close returns the driver's own code");
    CK_EQ(g_close_calls, 1, "the hardware is closed exactly once");
    CK_EQ(g_close_null, 0, "with a live instance");

    /* 引用计数上限 255：再开 300 次不该把计数绕回去 */
    reset_all();
    CK_EQ(arkrtos_dev_register("cap", &g_drv_full, 0u), 0, "register");
    h0 = arkrtos_dev_open_internal("cap", 0u);
    for(i = 0; i < 300; i++)
    {
        CK_EQ(arkrtos_dev_open_internal("cap", 0u), h0,
              "repeated open keeps the same handle");
    }
    CK_EQ(g_open_calls, 1, "300 opens still open the hardware once");
    for(i = 0; i < 254; i++)
    {
        CK_EQ(arkrtos_dev_close_internal(h0), 0, "close the extra openers");
    }
    CK_EQ(g_close_calls, 0, "254 closes are all shared-opener closes");
    CK_EQ(arkrtos_dev_close_internal(h0), 0, "the 255th close is the last one");
    CK_EQ(g_close_calls, 1, "and it really closes the hardware");
}

/* ------------------------------------------------------------------
 * 5) 打开失败面
 * ------------------------------------------------------------------ */
static void test_open_failures(void)
{
    reset_all();

    CK_EQ(arkrtos_dev_open_internal("ghost", 0u), ARKRTOS_DEV_ERR_NOTFOUND,
          "an unknown name cannot be opened");

    CK_EQ(arkrtos_dev_register("nul", &g_drv_full, 0u), 0, "register");
    g_open_null = 1;
    CK_EQ(arkrtos_dev_open_internal("nul", 0u), ARKRTOS_DEV_ERR_NOTFOUND,
          "a driver that cannot hand out an instance fails the open");
    CK_EQ(g_open_calls, 1, "the driver was asked once");
    g_open_null = 0;

    CK_EQ(arkrtos_dev_register("bare", &g_drv_open_only, 0u), 0, "register");
    /* open 存在的驱动照常打开，方便后面测 NOIMPL */
    ck(arkrtos_dev_open_internal("bare", 0u) >= 0, "open-only driver opens");

    CK_EQ(arkrtos_dev_register("none", &g_drv_no_open, 0u), 0, "register");
    CK_EQ(arkrtos_dev_open_internal("none", 0u), ARKRTOS_DEV_ERR_NOTFOUND,
          "a driver without drv_open cannot be opened");
}

/* ------------------------------------------------------------------
 * 6) read / write / ctrl 的闸门与转发
 * ------------------------------------------------------------------ */
static void test_io(void)
{
    int32 h;
    int32 h_bare;
    uint8 buf[4];
    int32 bad_flag;

    reset_all();

    CK_EQ(arkrtos_dev_register("io", &g_drv_full, 0u), 0, "register");
    h = arkrtos_dev_open_internal("io", 0u);
    ck(h >= 0, "open");

    g_read_rc  = 3;
    g_write_rc = 5;
    g_ctrl_rc  = 9;
    CK_EQ(arkrtos_dev_read_internal(h, buf, (int32)sizeof buf), 3,
          "read returns the driver's code");
    CK_EQ(g_rw_which, 1, "and reached drv_read");
    ck(g_rw_data == buf && g_rw_len == (int32)sizeof buf,
       "with the caller's buffer and length");
    CK_EQ(arkrtos_dev_write_internal(h, buf, (int32)sizeof buf), 5,
          "write returns the driver's code");
    CK_EQ(g_rw_which, 2, "and reached drv_write");
    CK_EQ(arkrtos_dev_ctrl_internal(h, 0xA5A5u, 0x5A5Au), 9,
          "ctrl returns the driver's code");
    CK_EQ(g_ctrl_code, 0xA5A5u, "ctrl code forwarded");
    CK_EQ(g_ctrl_value, 0x5A5Au, "ctrl value forwarded");

    /* 标志位不对 / 槽位越界 / 未打开 */
    bad_flag = (int32)((uint32)h & ~(uint32)ARKRTOS_HANDLE_MASK);
    CK_EQ(arkrtos_dev_read_internal(bad_flag, buf, 1), ARKRTOS_DEV_ERR_HANDLE,
          "a handle without the device flag is refused");
    CK_EQ(arkrtos_dev_read_internal((int32)(ARKRTOS_DEV_HANDLE_FLAG
                                            | (ARKRTOS_DEV_MAX_NUM + 3u)),
                                    buf, 1),
          ARKRTOS_DEV_ERR_HANDLE, "a handle past the table is refused");
    CK_EQ(arkrtos_dev_read_internal(-1, buf, 1), ARKRTOS_DEV_ERR_HANDLE,
          "a negative handle is refused");
    /* write / ctrl 各自也有同一对闸门，不能只测 read 那一条 */
    CK_EQ(arkrtos_dev_write_internal(bad_flag, buf, 1), ARKRTOS_DEV_ERR_HANDLE,
          "write refuses a handle without the device flag");
    CK_EQ(arkrtos_dev_write_internal(-1, buf, 1), ARKRTOS_DEV_ERR_HANDLE,
          "write refuses a negative handle");
    CK_EQ(arkrtos_dev_write_internal((int32)(ARKRTOS_DEV_HANDLE_FLAG
                                             | (ARKRTOS_DEV_MAX_NUM + 3u)),
                                     buf, 1),
          ARKRTOS_DEV_ERR_HANDLE, "write refuses a handle past the table");
    CK_EQ(arkrtos_dev_ctrl_internal(bad_flag, 0u, 0u), ARKRTOS_DEV_ERR_HANDLE,
          "ctrl refuses a handle without the device flag");
    CK_EQ(arkrtos_dev_ctrl_internal(-1, 0u, 0u), ARKRTOS_DEV_ERR_HANDLE,
          "ctrl refuses a negative handle");
    CK_EQ(arkrtos_dev_ctrl_internal((int32)(ARKRTOS_DEV_HANDLE_FLAG
                                            | (ARKRTOS_DEV_MAX_NUM + 3u)),
                                    0u, 0u),
          ARKRTOS_DEV_ERR_HANDLE, "ctrl refuses a handle past the table");

    CK_EQ(arkrtos_dev_register("idle", &g_drv_full, 0u), 0, "register");
    {
        int32 h_idle = (int32)((ARKRTOS_DEV_HANDLE_FLAG | 1u));

        CK_EQ(arkrtos_dev_read_internal(h_idle, buf, 1), ARKRTOS_DEV_ERR_STATE,
              "a device that was never opened reports STATE");
        CK_EQ(arkrtos_dev_write_internal(h_idle, buf, 1), ARKRTOS_DEV_ERR_STATE,
              "write too");
        CK_EQ(arkrtos_dev_ctrl_internal(h_idle, 0u, 0u), ARKRTOS_DEV_ERR_STATE,
              "ctrl too");
    }

    /* 驱动缺入口：三个都报 NOIMPL。reset_all() 已经把 test_open_failures
     * 那份表清掉了，这里要自己注册一台只有 drv_open 的设备。 */
    CK_EQ(arkrtos_dev_register("bare", &g_drv_open_only, 0u), 0,
          "register an open-only device");
    h_bare = arkrtos_dev_open_internal("bare", 0u);
    ck(h_bare >= 0, "open the open-only device");
    CK_EQ(arkrtos_dev_read_internal(h_bare, buf, 1), ARKRTOS_DEV_ERR_NOIMPL,
          "read without drv_read is NOIMPL");
    CK_EQ(arkrtos_dev_write_internal(h_bare, buf, 1), ARKRTOS_DEV_ERR_NOIMPL,
          "write without drv_write is NOIMPL");
    CK_EQ(arkrtos_dev_ctrl_internal(h_bare, 0u, 0u), ARKRTOS_DEV_ERR_NOIMPL,
          "ctrl without drv_ctrl is NOIMPL");
    CK_EQ(g_read_calls + g_write_calls + g_ctrl_calls, 3,
          "no missing entry ever reached a driver");

    /* 驱动没有 drv_close：收尾只清句柄，不报错，也不叫硬件 */
    CK_EQ(arkrtos_dev_close_internal(h_bare), 0,
          "closing a device whose driver has no drv_close succeeds");
    CK_EQ(g_close_calls, 0, "and it never called a close entry");
    CK_EQ(arkrtos_dev_read_internal(h_bare, buf, 1), ARKRTOS_DEV_ERR_STATE,
          "the handle from that device is now closed");

    /* 关闭后再用同一句柄：必须 STATE */
    CK_EQ(arkrtos_dev_close_internal(h), 0, "close the io device");
    CK_EQ(arkrtos_dev_read_internal(h, buf, 1), ARKRTOS_DEV_ERR_STATE,
          "the closed handle reports STATE");

    /* close 自己的闸门 */
    CK_EQ(arkrtos_dev_close_internal(bad_flag), ARKRTOS_DEV_ERR_HANDLE,
          "close refuses a handle without the flag");
    CK_EQ(arkrtos_dev_close_internal((int32)(ARKRTOS_DEV_HANDLE_FLAG
                                             | (ARKRTOS_DEV_MAX_NUM + 1u))),
          ARKRTOS_DEV_ERR_HANDLE, "close refuses a handle past the table");
}

/* ------------------------------------------------------------------
 * 7) 遗言路径：按实例指针找设备图 12（仅 HW_SAFE 档有这段代码）
 * ------------------------------------------------------------------ */
#if ARKRTOS_HW_SAFE
static void test_ctrl_obj(void)
{
    int32 h;
    uint8 buf[2];

    reset_all();

    CK_EQ(arkrtos_dev_ctrl_obj(0, 0u, 0u), ARKRTOS_DEV_ERR_PARAM,
          "a null instance is refused");

    CK_EQ(arkrtos_dev_register("m", &g_drv_full, 0u), 0, "register");
    h = arkrtos_dev_open_internal("m", 0u);
    ck(h >= 0, "open");

    g_ctrl_rc = 11;
    CK_EQ(arkrtos_dev_ctrl_obj(&g_inst[0], 0x11u, 0x22u), 11,
          "a live instance reaches its driver");
    CK_EQ(g_ctrl_code, 0x11u, "code forwarded by pointer");
    CK_EQ(g_ctrl_value, 0x22u, "value forwarded by pointer");

    CK_EQ(arkrtos_dev_ctrl_obj(&g_inst[1], 0u, 0u), ARKRTOS_DEV_ERR_NOTFOUND,
          "an instance that is not in the table is refused");

    /* 驱动没有 ctrl：报 NOIMPL，不假装成功。
     * 假驱动按 param 选实例，两台都传 0 会拿到同一个 g_inst[0]；
     * 那样 ctrl_obj 会先命中 slot 0 的全功能驱动，测不到这一支。
     * 所以先把 m 关掉，让 m2 成为表里唯一持有该实例的设备。 */
    CK_EQ(arkrtos_dev_close_internal(h), 0, "close m first");
    CK_EQ(arkrtos_dev_register("m2", &g_drv_open_only, 0u), 0, "register");
    ck(arkrtos_dev_open_internal("m2", 0u) >= 0, "open the open-only device");
    CK_EQ(arkrtos_dev_ctrl_obj(&g_inst[0], 0u, 0u), ARKRTOS_DEV_ERR_NOIMPL,
          "a device without drv_ctrl is reported as such");

    /* 注销时台账也要跟着撤（回调里指针会指向即将被替换的镜像） */
    CK_EQ(arkrtos_dev_unregister("m"), 0, "unregister");
    CK_EQ(g_unregisters, 1, "the ledger hears about the unregister");
    CK_EQ(arkrtos_dev_ctrl_obj(&g_inst[0], 0u, 0u), ARKRTOS_DEV_ERR_NOTFOUND,
          "and the instance is gone with it");

    (void)buf;
}
#endif

/* ------------------------------------------------------------------
 * 8) 双关：现在的行为是把空实例交给驱动（登记为待定问题，不钉成"正确"）
 * ------------------------------------------------------------------ */
static void test_double_close(void)
{
    int32 h;

    reset_all();

    CK_EQ(arkrtos_dev_register("dc", &g_drv_full, 0u), 0, "register");
    h = arkrtos_dev_open_internal("dc", 0u);
    ck(h >= 0, "open");
    CK_EQ(arkrtos_dev_close_internal(h), 0, "close");
    CK_EQ(g_close_calls, 1, "the driver saw one close");

    /* 第二次 close：句柄仍合法、引用计数已是 0，代码会再叫一次 drv_close，
     * 但实例指针已经被清空。这是**现有行为**，本用例只把它记成读数，
     * 不声称它是对的（见 docs/单元测试框架.md 的同名条目）。 */
    CK_EQ(arkrtos_dev_close_internal(h), g_close_rc,
          "a second close is still forwarded to the driver");
    CK_EQ(g_close_calls, 2, "so the driver sees two closes");
    CK_EQ(g_close_null, 1, "the second one carries a null instance");
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int main(void)
{
    test_register_rejects();
    test_register_table();
    test_unregister();
    test_open_refcount();
    test_open_failures();
    test_io();
#if ARKRTOS_HW_SAFE
    test_ctrl_obj();
#endif
    test_double_close();

    if(g_fails != 0)
    {
        printf("DEV-HOST FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
#if ARKRTOS_HW_SAFE
    printf("DEV-HOST OK %d checks\n", g_checks);
#else
    printf("DEV-HOST NOSAFE OK %d checks\n", g_checks);
#endif
    return 0;
}
