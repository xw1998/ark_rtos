/**
* @file test_shell_auth_host.c
* @brief Host-side unit test for arkrtos_shell_auth.c (SEC-G01).
*
* What this proves: the console gate's decisions and its accounting.
*   - boot starts locked, and only login/logout/help get through the gate;
*   - a wrong password counts up and every attempt costs a full digest
*     compare (that part is code shape, see the source) -- what is asserted
*     here is the counter and the lock-hard threshold;
*   - after ARKRTOS_SHELL_AUTH_TRIES wrong passwords the console is locked
*     until reset: login itself refuses, logout reports the same thing, and
*     the gate's message names the count;
*   - the correct password unlocks exactly once and resets the counter;
*   - every entry point behaves the same way whether the console is locked,
*     unlocked, locked-hard, or the name is NULL.
*
* What it does NOT prove: that the password does not leak over the serial
* line, that the digest is not brute-forceable offline, real timing
* constancy (only the code shape is visible here), or anything about the
* board's serial console.  This is ILP32 host code -- nothing here depends
* on pointer width.
*
* The salt and digest below are a fixture: salt 0x00..0x0F, password
* "s3cret!", digest = sha256(salt || "s3cret!") computed offline.
*
* @author xw
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "arkrtos_shell_auth.h"

/* ------------------------------------------------------------ assertions */

static int g_checks;
static int g_failed;

#define CK(cond)                                                       \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            printf("FAIL line %d: %s\n", __LINE__, #cond);             \
        }                                                              \
    } while (0)

#define CK_I(got, want)                                                \
    do {                                                               \
        long g_ = (long)(got);                                         \
        long w_ = (long)(want);                                        \
        g_checks++;                                                    \
        if (g_ != w_) {                                                \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got %ld want %ld)\n",            \
                   __LINE__, #got, g_, w_);                            \
        }                                                              \
    } while (0)

/* ------------------------------------------------------- console capture */

static int  g_printf_calls;
static char g_last[256];

void ark_shell_printf(const char *fmt, ...);

void ark_shell_printf(const char *fmt, ...)
{
    va_list ap;

    g_printf_calls++;
    va_start(ap, fmt);
    vsnprintf(g_last, sizeof(g_last), fmt, ap);
    va_end(ap);
}

static void reset_capture(void)
{
    g_printf_calls = 0;
    g_last[0] = '\0';
}

static int last_has(const char *needle)
{
    return strstr(g_last, needle) != NULL;
}

/* ---------------------------------------------------------------- helpers */

static char s_login_name[] = "login";
static char s_pw[] = "s3cret!";
static char s_bad[] = "wrong";

static int do_login(char *pw)
{
    char *argv[2];

    argv[0] = s_login_name;
    argv[1] = pw;
    return arkrtos_shell_auth_cmd_login(2, argv);
}

static int do_logout(void)
{
    return arkrtos_shell_auth_cmd_logout(0, NULL);
}

/* -------------------------------------------------------------- testcases */

static void test_boot_state(void)
{
    arkrtos_shell_auth_init();
    CK_I(arkrtos_shell_auth_locked(), 1);
    reset_capture();
    /* 空名字：拒绝，但**不打印**（门禁返回 -1 之前没有输出） */
    CK_I(arkrtos_shell_auth_gate(NULL), -1);
    CK_I(g_printf_calls, 0);
}

static void test_gate_exempt(void)
{
    arkrtos_shell_auth_init();
    reset_capture();
    CK_I(arkrtos_shell_auth_gate("login"), 0);
    CK_I(arkrtos_shell_auth_gate("logout"), 0);
    CK_I(arkrtos_shell_auth_gate("help"), 0);
    CK_I(g_printf_calls, 0);          /* 豁免词不打印、不计错 */
}

static void test_gate_locked(void)
{
    arkrtos_shell_auth_init();
    reset_capture();
    CK_I(arkrtos_shell_auth_gate("ps"), -1);
    CK_I(g_printf_calls, 1);
    CK(last_has("console is locked, type 'login <password>'"));
    CK_I(arkrtos_shell_auth_locked(), 1);
}

static void test_login_usage(void)
{
    char *argv[1];

    arkrtos_shell_auth_init();
    argv[0] = s_login_name;
    reset_capture();
    CK_I(arkrtos_shell_auth_cmd_login(1, argv), 1);
    CK(last_has("usage: login <password>"));
    CK_I(arkrtos_shell_auth_locked(), 1);
    /* argc 太大也走同一条路 */
    reset_capture();
    CK_I(arkrtos_shell_auth_cmd_login(3, argv), 1);
    CK(last_has("usage: login <password>"));
}

static void test_login_ok(void)
{
    arkrtos_shell_auth_init();
    reset_capture();
    CK_I(do_login(s_pw), 0);
    CK(last_has("login: ok, console unlocked"));
    CK_I(arkrtos_shell_auth_locked(), 0);

    /* 解锁后门禁放行任何名字，且不打印 */
    reset_capture();
    CK_I(arkrtos_shell_auth_gate("ps"), 0);
    CK_I(g_printf_calls, 0);

    /* 已登录时再 login：不重算、不计数，直接 0 */
    reset_capture();
    CK_I(do_login(s_bad), 0);
    CK(last_has("already logged in"));
    CK_I(arkrtos_shell_auth_locked(), 0);
}

static void test_login_wrong_counts(void)
{
    int i;

    arkrtos_shell_auth_init();
    for (i = 1; i < ARKRTOS_SHELL_AUTH_TRIES; i++) {
        reset_capture();
        CK_I(do_login(s_bad), 1);
        CK_I(arkrtos_shell_auth_locked(), 1);
        CK_I(g_printf_calls, 1);
        CK(last_has("wrong password"));
        /* 第 i 次：文案里带 (i/TRIES) */
        CK(last_has("("));
    }
    /* 最后一次：锁定到底 */
    reset_capture();
    CK_I(do_login(s_bad), 1);
    CK(last_has("locked until reset"));
    CK_I(arkrtos_shell_auth_locked(), 1);

    /* 锁到底之后：正确密码也不放行，且文案说清"重启才能再试" */
    reset_capture();
    CK_I(do_login(s_pw), 1);
    CK(last_has("locked until reset"));
    CK(last_has("reboot"));
    CK_I(arkrtos_shell_auth_locked(), 1);

    /* 门禁此时报告"锁定到底"，而不是"请输入密码" */
    reset_capture();
    CK_I(arkrtos_shell_auth_gate("ps"), -1);
    CK_I(g_printf_calls, 1);
    CK(last_has("console locked until reset"));
    CK(last_has("wrong passwords"));

    /* 豁免词在锁到底时仍然放行 */
    reset_capture();
    CK_I(arkrtos_shell_auth_gate("help"), 0);
    CK_I(g_printf_calls, 0);
}

static void test_logout(void)
{
    /* 已锁：logout 拒绝并说清 */
    arkrtos_shell_auth_init();
    reset_capture();
    CK_I(do_logout(), 0);
    CK(last_has("already locked"));
    CK_I(arkrtos_shell_auth_locked(), 1);

    /* 解锁后 logout：真正锁回去，并清掉失败计数 */
    CK_I(do_login(s_pw), 0);
    CK_I(arkrtos_shell_auth_locked(), 0);
    reset_capture();
    CK_I(do_logout(), 0);
    CK(last_has("logout: console locked"));
    CK_I(arkrtos_shell_auth_locked(), 1);

    /* 失败一次再 logout：计数被清，下次错误重新从 1 开始 */
    CK_I(do_login(s_bad), 1);
    CK_I(do_login(s_pw), 0);
    reset_capture();
    CK_I(do_logout(), 0);
    CK_I(do_login(s_bad), 1);
    CK(last_has("(1/"));

    /* 锁到底后 logout 报的是"已经锁定到底"，不做别的事 */
    arkrtos_shell_auth_init();
    {
        int i;
        for (i = 0; i < ARKRTOS_SHELL_AUTH_TRIES; i++) {
            (void)do_login(s_bad);
        }
    }
    reset_capture();
    CK_I(do_logout(), 0);
    CK(last_has("already locked until reset"));
    CK_I(arkrtos_shell_auth_locked(), 1);
}

int main(void)
{
    test_boot_state();
    test_gate_exempt();
    test_gate_locked();
    test_login_usage();
    test_login_ok();
    test_login_wrong_counts();
    test_logout();

    if (g_failed != 0) {
        printf("SHELL-AUTH-HOST FAILED  %d of %d\n", g_failed, g_checks);
        return 1;
    }
    printf("SHELL-AUTH-HOST OK  %d checks\n", g_checks);
    return 0;
}
