/**
* @file test_platform_shell_host.c
* @brief Host test for kernelsrc/shell/platform_arkrtos.c (shell platform layer).
*
* @details
* What this proves: the nine platform functions the ark-shell core calls --
* serial open/send/recv routing, the console hold pair (and the "do not take
* the lock twice" rule it implies for the senders), the pipeline capture
* diversion, the tick passthrough, the mode record and the board reboot hook.
* Every byte the layer would push to the UART is recorded, so the assertions
* are about exact bytes, exact call counts and exact lock pairing, not about
* "something happened".
*
* What this does NOT prove: that the kernel device table exists, that the
* console lock masks interrupts, that TxFIFO is real, or that a board actually
* resets. All of those are stubs here. It also does not execute the AIRCR
* branch of platform_reboot(): that branch is compiled out on this board
* (ARKRTOS_PLATFORM_REBOOT_PORT is defined by the real qemu_loongarch64 board
* config), and the trailing self-spin loop after the reset request is only
* reached if a reset does not happen -- it is a deliberate, registered
* non-coverage, not a missed case.
*
* Board view: kernelsrc/tests/host_board_la64.h (the real qemu_loongarch64
* board with CHIP_FLASH_BASE moved off a Windows loader reservation; this file
* touches no address, it is reused so the shell switches are the real ones).
* Only tools/coverage_gate.py's shellplat case points here.
*
* @author xw
*/

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "arkrtos_config.h"
#include "arkrtos_dev.h"
#include "arkrtos_log.h"
#include "arkrtos_task.h"
#include "arkrtos_shell.h"
#include "ark_shell/ark_shell_eval.h"

#include "platform.h"

static int g_fail;
static int g_checks;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fail++;
        printf("FAIL %s\n", what);
    }
}

static void ck_i(int got, int want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fail++;
        printf("FAIL %s: got %d want %d\n", what, got, want);
    }
}

/* ---------------- stubs ---------------- */

static int32  g_open_rc = 3;
static int    g_open_calls;
static char   g_open_name[24];
static uint32 g_open_param;

static int32  g_write_fail_first;     /* fail this many times, then succeed */
static int    g_write_always_fail;
static int    g_write_calls;
static unsigned char g_written[128];
static int    g_written_n;
static int32  g_write_handle_last;

static int32  g_read_rc;
static int    g_read_calls;
static uint8  g_read_byte;
static uint32 g_read_len_last;

static int32  g_lock_rc = 1;
static int    g_lock_calls;
static int    g_unlock_calls;

static uint32 g_time_val = 0x1234u;

static int    g_capture_on;
static int32  g_capture_put_rc = 1;
static int    g_capture_calls;
static unsigned char g_captured[128];
static int    g_captured_n;

static jmp_buf g_reboot_jmp;
static int     g_reboot_calls;

/* Declared by board/qemu_loongarch64/arkrtos_board.h, which the kernel shell
 * layer deliberately does not include; the host test provides the definition
 * and needs the prototype or the TU fails -Wmissing-prototypes. */
void arkrtos_board_reboot(void);

int32 arkrtos_dev_open_internal(const char *name, uint32 param)
{
    g_open_calls++;
    if(name != 0)
    {
        strncpy(g_open_name, name, sizeof(g_open_name) - 1u);
        g_open_name[sizeof(g_open_name) - 1u] = '\0';
    }
    else
    {
        g_open_name[0] = '\0';
    }
    g_open_param = param;
    return g_open_rc;
}

int32 arkrtos_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    (void)handle;
    g_read_calls++;
    g_read_len_last = (uint32)len;
    if(g_read_rc == 1)
    {
        if((pdata != 0) && (len > 0))
        {
            *pdata = g_read_byte;
        }
        return 1;
    }
    return 0;
}

int32 arkrtos_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    g_write_calls++;
    g_write_handle_last = handle;
    if(g_write_always_fail != 0)
    {
        return -1;
    }
    if(g_write_fail_first > 0)
    {
        g_write_fail_first--;
        return 0;
    }
    if((pdata != 0) && (len == 1) && (g_written_n < (int)sizeof(g_written)))
    {
        g_written[g_written_n++] = (unsigned char)*pdata;
    }
    if(g_written_n >= (int)sizeof(g_written))
    {
        printf("FAIL write buffer overflow in test harness\n");
        g_fail++;
        return 1;
    }
    return 1;
}

int32 arkrtos_console_lock(void)
{
    g_lock_calls++;
    return g_lock_rc;
}

void arkrtos_console_unlock(void)
{
    g_unlock_calls++;
}

uint32 arkrtos_kernel_get_time(void)
{
    return g_time_val;
}

int ark_shell_capture_active(void)
{
    return g_capture_on;
}

int ark_shell_capture_put(const unsigned char *p, unsigned int n)
{
    int i;
    g_capture_calls++;
    if(g_capture_put_rc == 0)
    {
        return 0;               /* not consumed: the port falls back to the UART */
    }
    for(i = 0; (unsigned int)i < n; i++)
    {
        if(g_captured_n < (int)sizeof(g_captured))
        {
            g_captured[g_captured_n++] = p[i];
        }
    }
    return 1;                   /* 1 = consumed, the port must not write it */
}

void arkrtos_board_reboot(void)
{
    g_reboot_calls++;
    longjmp(g_reboot_jmp, 1);
}

/* ---------------- helpers ---------------- */

static void reset_io(void)
{
    g_write_fail_first  = 0;
    g_write_always_fail = 0;
    g_write_calls       = 0;
    g_written_n         = 0;
    g_write_handle_last = -100;
    g_read_rc           = 0;
    g_read_calls        = 0;
    g_read_len_last     = 0u;
    g_lock_rc           = 1;
    g_lock_calls        = 0;
    g_unlock_calls      = 0;
    g_capture_on        = 0;
    g_capture_put_rc    = 1;
    g_capture_calls     = 0;
    g_captured_n        = 0;
}

/* The mode getter returns an enum; compare enums directly (casting a
 * function call result to another integer type trips -Wbad-function-cast). */
static void ck_mode(uart_mode_t want, const char *what)
{
    uart_mode_t got = platform_uart_get_mode();

    g_checks++;
    if(got != want)
    {
        g_fail++;
        printf("FAIL %s: got %d want %d\n", what, (int)got, (int)want);
    }
}

static int written_is(const char *s)
{
    int n = (int)strlen(s);
    if(n != g_written_n)
    {
        return 0;
    }
    return memcmp(g_written, s, (size_t)n) == 0;
}

static int captured_is(const char *s)
{
    int n = (int)strlen(s);
    if(n != g_captured_n)
    {
        return 0;
    }
    return memcmp(g_captured, s, (size_t)n) == 0;
}

/* ---------------- scenarios ---------------- */

/* Nothing may reach the device before the port has a handle. */
static void t_before_open(void)
{
    unsigned char b = 0x41u;

    reset_io();
    platform_uart_send(0x41u);
    platform_uart_send_string("abc");
    platform_uart_send_buf(&b, 1u);
    ck_i(g_write_calls, 0, "closed port sends nothing");
    ck_i(g_capture_calls, 0, "closed port does not capture");
    ck_i(g_lock_calls, 0, "closed port takes no console lock");
    ck_i(platform_uart_recv(), -1, "closed port reads -1");
    ck_i(g_read_calls, 0, "closed port does not touch the device");
    ck_i((int)arkrtos_shell_uart_handle(), -1, "handle is -1 before open");
}

static void t_open(void)
{
    reset_io();

    /* First attempt fails: the handle stays closed and every entry point
     * keeps reporting -1. */
    g_open_rc = -5;
    platform_uart_init(115200u);
    ck_i(g_open_calls, 1, "failed open still calls the device once");
    ck_i(strcmp(g_open_name, "ttyS0"), 0, "opens SHELL_DEV_NAME");
    ck_i((int)g_open_param, 115200, "baud rate passed through");
    ck((int)arkrtos_shell_uart_handle() < 0, "failed open keeps the handle closed");
    ck_i(platform_uart_recv(), -1, "failed open: recv is -1");

    /* shell_uart_open retries while the handle is invalid. */
    platform_uart_init(115200u);
    ck_i(g_open_calls, 2, "init retries after a failed open");
    g_open_rc = 7;
    ck_i((int)arkrtos_shell_uart_open(), 7, "uart_open returns the new handle");
    ck_i(g_open_calls, 3, "uart_open opened the device itself");
    ck_i((int)arkrtos_shell_uart_open(), 7, "uart_open is idempotent");
    ck_i(g_open_calls, 3, "already-open uart_open does not reopen");
    ck_i((int)arkrtos_shell_uart_handle(), 7, "handle is the open result");

    platform_uart_init(9600u);
    ck_i(g_open_calls, 3, "init on an open port does not reopen");
    ck_i((int)arkrtos_shell_uart_handle(), 7, "handle unchanged by second init");
    ck_mode(UART_MODE_IRQ, "init leaves the port in IRQ mode");
}

/* The 0 baud case falls back to SHELL_DEV_ARG; the handle is already open
 * in this build, so exercise the fallback through a direct re-open path:
 * platform_uart_init is a no-op now, which is itself the contract. */
static void t_open_default_arg(void)
{
    int before = g_open_calls;
    reset_io();
    platform_uart_init(0u);
    ck_i(g_open_calls - before, 0, "init on an open port never re-evaluates the baud");
}

static void t_console_hold(void)
{
    reset_io();

    platform_console_end();
    ck_i(g_unlock_calls, 0, "unbalanced end at depth 0 does nothing");

    platform_console_begin();
    ck_i(g_lock_calls, 1, "outer begin takes the lock once");
    platform_console_begin();
    ck_i(g_lock_calls, 1, "nested begin does not lock again");
    platform_console_end();
    ck_i(g_unlock_calls, 0, "inner end keeps the hold");
    platform_console_end();
    ck_i(g_unlock_calls, 1, "outer end releases the hold");
    platform_console_end();
    ck_i(g_unlock_calls, 1, "extra end after release does nothing");

    /* A lock that was refused must not be released later. */
    g_lock_rc = 0;
    platform_console_begin();
    ck_i(g_lock_calls, 2, "second hold tries the lock again");
    platform_console_end();
    ck_i(g_unlock_calls, 1, "refused lock is never unlocked");
    g_lock_rc = 1;
}

static void t_send_capture(void)
{
    unsigned char b[2] = { 'X', 'Y' };

    reset_io();
    g_capture_on = 1;

    platform_uart_send(0x5Au);
    ck_i(g_captured_n, 1, "single byte captured");
    ck_i((int)g_captured[0], 0x5A, "captured byte value");
    ck_i(g_write_calls, 0, "captured byte never reaches the UART");

    reset_io();
    g_capture_on = 1;
    platform_uart_send_string("hi");
    ck_i(captured_is("hi"), 1, "string captured as one unit");
    ck_i(g_write_calls, 0, "captured string never reaches the UART");

    reset_io();
    g_capture_on = 1;
    platform_uart_send_buf(b, 2u);
    ck_i(g_captured_n, 2, "buffer captured whole");
    ck_i((int)g_captured[1], 'Y', "buffer second byte");
    ck_i(g_write_calls, 0, "captured buffer never reaches the UART");

    /* A capture that refuses the bytes returns to the normal path. */
    reset_io();
    g_capture_on     = 1;
    g_capture_put_rc = 0;
    platform_uart_send('Q');
    ck_i(g_capture_calls, 1, "capture asked once");
    ck_i(g_write_calls, 1, "refused capture falls back to the UART");
}

static void t_send_paths(void)
{
    unsigned char b[3] = { 'a', 'b', 'c' };

    reset_io();
    platform_uart_send('A');
    ck_i(g_write_calls, 1, "one byte = one write");
    ck_i(g_lock_calls, 1, "single byte takes the lock");
    ck_i(g_unlock_calls, 1, "single byte releases the lock");
    ck_i(written_is("A"), 1, "single byte reaches the UART");

    /* Write refuses twice, then accepts: the spin loop is what makes the
     * byte appear, and it does not give up early. */
    reset_io();
    g_write_fail_first = 2;
    platform_uart_send('B');
    ck_i(g_write_calls, 3, "retried until the write was accepted");
    ck_i(written_is("B"), 1, "byte still arrives after two refusals");

    reset_io();
    platform_uart_send_string("abc");
    ck_i(g_write_calls, 3, "string is written one byte at a time");
    ck_i(g_lock_calls, 1, "a string is one output unit: one lock");
    ck_i(g_unlock_calls, 1, "a string releases the lock once");
    ck_i(written_is("abc"), 1, "string bytes in order");

    reset_io();
    g_write_fail_first = 1;
    platform_uart_send_string("z");
    ck_i(g_write_calls, 2, "string byte retried after a refusal");
    ck_i(written_is("z"), 1, "string byte arrives after refusal");

    /* Inside platform_console_begin/end the senders must not take the lock
     * again -- that would deadlock against the holder. */
    reset_io();
    platform_console_begin();
    platform_uart_send('C');
    platform_uart_send_string("d");
    platform_uart_send_buf(b, 2u);
    ck_i(g_lock_calls, 1, "held console: only the begin locked");
    ck_i(g_unlock_calls, 0, "held console: senders never unlock");
    platform_console_end();
    ck_i(g_unlock_calls, 1, "held console released by end");
    ck_i(written_is("Cdab"), 1, "held console writes are ordered");

    /* Nulls and empty units. */
    reset_io();
    platform_uart_send_string((const char *)0);
    ck_i(g_write_calls, 0, "null string writes nothing");
    ck_i(g_lock_calls, 0, "null string takes no lock");
    platform_uart_send_buf((const unsigned char *)0, 4u);
    ck_i(g_write_calls, 0, "null buffer writes nothing");
    ck_i(g_lock_calls, 0, "null buffer takes no lock");
    platform_uart_send_buf(b, 0u);
    ck_i(g_write_calls, 0, "empty buffer writes no byte");
    ck_i(g_lock_calls, 1, "empty buffer is still one output unit");
    ck_i(g_unlock_calls, 1, "empty buffer releases its own hold");
    platform_uart_send_string("");
    ck_i(g_write_calls, 0, "empty string writes no byte");
    ck_i(g_lock_calls, 2, "empty string takes a hold of its own");

    reset_io();
    platform_uart_send_buf(b, 3u);
    ck_i(written_is("abc"), 1, "buffer bytes in order");
    ck_i(g_lock_calls, 1, "buffer is one output unit");

    /* A device that never accepts the byte must not spin forever. */
    reset_io();
    g_write_always_fail = 1;
    platform_uart_send('D');
    ck_i(g_write_calls, 200001, "spin limit is 200000 retries after the first try");
    ck_i(g_written_n, 0, "nothing was written while the device refused");
    ck_i(g_unlock_calls, 1, "the lock is still released after giving up");

    reset_io();
    g_write_always_fail = 1;
    platform_uart_send_string("ab");
    ck_i(g_write_calls, 400002, "each string byte spends the full spin budget");
    reset_io();
    g_write_always_fail = 1;
    platform_uart_send_buf(b, 2u);
    ck_i(g_write_calls, 400002, "each buffer byte spends the full spin budget");
}

static void t_recv(void)
{
    reset_io();
    g_read_rc = 0;
    ck_i(platform_uart_recv(), -1, "empty FIFO is -1");
    ck_i(g_read_calls, 1, "recv asks the device once");
    ck_i((int)g_read_len_last, 1, "recv asks for exactly one byte");

    reset_io();
    g_read_rc   = 1;
    g_read_byte = 0x5Au;
    ck_i(platform_uart_recv(), 0x5A, "received byte is returned unsigned");
    g_read_byte = 0xFFu;
    ck_i(platform_uart_recv(), 0xFF, "0xFF is 255, not -1");
}

static void t_misc(void)
{
    reset_io();
    g_time_val = 0xDEADBEEFu;
    ck((uint32)platform_tick_ms() == 0xDEADBEEFu, "tick passthrough");
    g_time_val = 0u;
    ck((uint32)platform_tick_ms() == 0u, "tick passthrough at zero");

    platform_uart_set_mode(UART_MODE_DMA);
    ck_mode(UART_MODE_DMA, "mode is recorded");
    platform_uart_set_mode(UART_MODE_POLL);
    ck_mode(UART_MODE_POLL, "mode is recorded again");
    platform_uart_set_mode(UART_MODE_IRQ);
    ck_mode(UART_MODE_IRQ, "mode back to IRQ");

    platform_cleanup();
    ck_i(1, 1, "cleanup is a no-op that returns");

    /* Board reboot hook: the port must call the board, not the AIRCR. */
    g_reboot_calls = 0;
    if(setjmp(g_reboot_jmp) == 0)
    {
        platform_reboot();
        ck_i(1, 0, "reboot must not return from the board hook");
    }
    ck_i(g_reboot_calls, 1, "reboot asks the board exactly once");
}

int main(void)
{
    t_before_open();
    t_open();
    t_open_default_arg();
    t_console_hold();
    t_send_capture();
    t_send_paths();
    t_recv();
    t_misc();

    if(g_fail != 0)
    {
        printf("PLATFORM-SHELL-HOST FAIL  %d/%d checks\n", g_fail, g_checks);
        return 1;
    }

    printf("PLATFORM-SHELL-HOST OK  %d checks\n", g_checks);
    return 0;
}
