/**
* @file test_shell_pmap_host.c
* @brief Host-side unit test for kernelsrc/shell/arkrtos_shell_pmap.c.
*
* What this proves: the pmap command family on the host, with every refusal
* path executed and every printed branch checked:
*   - the bare command (and an unknown subcommand) prints the paging audit
*     counters and the usage block;
*   - map / check / unmap / rd / rb / wrb / dyn / frame / rel / space /
*     pairs / look each accept their own argument shape, reject a malformed
*     one with their own usage line, and report the backend rc unchanged;
*   - rd clamps the word count at 8 instead of reading past the ask;
*   - pairs separates three different "nothing to show" cases: no paged
*     window (total=0), space out of range, and the per-space listing with
*     empty slots skipped and the tail folded into "N more pair(s)";
*   - ftest allocates 1 + 3 frames, writes and reads back a 64-word pattern
*     in all four frames, refuses an out-of-pool free with -1, a double free
*     with -3, and returns the frame count to where it started;
*   - test maps the kernel ROM page outside the static mapping, compares 16
*     words through the new translation against the direct address, then
*     proves unmap really drops the entry (lookup, not just rc=0), rebinds
*     the same VA to the next physical page and compares again;
*   - u1 writes two different words through the same VA in two address
*     spaces and reads them back in both directions;
*   - elf prints the per-space instance ledger (live entry vs empty);
*   - u2 loads one ELF path into two spaces and checks three things from the
*     same VA: each load only touches its own frame, the second instance gets
*     a different space, and both instances are live with distinct task ids
*     bound to their own space;
*   - cu drives all four refusal codes of the controlled user access
*     (-1 range wrap, -2 no user window / kernel caller, -3 outside span /
*     kernel pointer / unmapped / write to ROM, -4 wrong space), plus the two
*     success directions, and shows the kernel survived all of them.
*
* What it does NOT prove:
*   - that any of this works on LoongArch: every paging / port / ELF entry
*     point here is a stub.  The TLB is modelled by a per-space shadow copy
*     of the window (switching a space copies the shadow in, leaving it
*     copies it out), so "same VA, two frames" is exercised as a memory
*     model, not as hardware translation;
*   - that the frame pattern mismatch branch of ftest can ever fire: plain
*     host RAM always reads back what was just written, so that one branch is
*     left uncovered on purpose rather than faked with a fault injector;
*   - the swap (U-16), ASID (U-7) and invop / asidinval families:
*     qemu_loongarch64 builds with ARKRTOS_USE_PAGING_SWAP=0 and
*     ARKRTOS_USE_PAGING_ASID=0, so those subcommands are not compiled into
*     the module measured here (they are exercised by the board readings in
*     docs/MMU*.md, not by this file);
*   - the direct-dereference nature of rd / rb / wrb: on the host those
*     addresses are plain allocated bytes, so the test shows the command's
*     decode and print, not what a real unmapped access does.
*
* Address note: the module turns the address it prints into a pointer through
* unsigned long, which is 32-bit under MinGW.  Every buffer handed to it is
* therefore VirtualAlloc'ed below 4 GB at a fixed base, not a static array.
*
* @author xw
*/

#if defined(_WIN32)
#include <windows.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_shell_internal.h"

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

#define CK_S(got, want)                                                \
    do {                                                               \
        const char *g_ = (got);                                        \
        const char *w_ = (want);                                       \
        g_checks++;                                                    \
        if ((g_ == NULL) || (strcmp(g_, w_) != 0)) {                    \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got \"%s\" want \"%s\")\n",      \
                   __LINE__, #got, g_ ? g_ : "(null)", w_);            \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------ capture */

static char g_out[32768];
static size_t g_out_len;

static void cap_reset(void)
{
    g_out[0] = '\0';
    g_out_len = 0u;
}

static void cap_add(const char *s, size_t n)
{
    size_t room = sizeof(g_out) - 1u - g_out_len;

    if (n > room) {
        n = room;
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
    g_out[g_out_len] = '\0';
}

static int out_has(const char *needle)
{
    return strstr(g_out, needle) != NULL;
}

static int out_count(const char *needle)
{
    const char *p = g_out;
    size_t n = strlen(needle);
    int c = 0;

    if (n == 0u) {
        return 0;
    }
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += n;
    }
    return c;
}

/* Writable copy: the command entry points take char *argv[] and the literals
 * are const char[] under -Wwrite-strings. */
static char *W(const char *s)
{
    static char bufs[8][48];
    static unsigned idx;
    char *b = bufs[idx % 8u];
    size_t n = strlen(s);

    idx++;
    if (n >= sizeof(bufs[0])) {
        n = sizeof(bufs[0]) - 1u;
    }
    memcpy(b, s, n);
    b[n] = '\0';
    return b;
}

/* ------------------------------------------------------------ shell glue */

void ark_shell_printf(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        cap_add(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1u);
    }
}

void sh_out(const char *s)
{
    cap_add(s, strlen(s));
}

/* Decimal only, matching every other shell case (the module uses it for
 * counts, never for addresses). */
int parse_u32(const char *s, uint32 *out)
{
    uint32 v = 0u;

    if ((s == NULL) || (out == NULL) || (*s == '\0')) {
        return -1;
    }
    while (*s != '\0') {
        if ((*s < '0') || (*s > '9')) {
            return -1;
        }
        v = (v * 10u) + (uint32)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

/* ------------------------------------------------------------ low memory */

#define WIN_VA        0x01000000u    /* the App window (pmap u1 / cu poke) */
#define WIN_SIZE      0x101000u      /* must cover win + 1 MB + 0x800 + 16 */
#define MEM_VA        0x01200000u    /* rd / rb / wrb scratch */
#define FRAME_VA      0x01300000u    /* frame pool handed out by ftest */
#define FRAME_COUNT   64u
#define TEST_VA       0x03000000u    /* mirrors PMAP_TEST_VA in the module */

static unsigned char *g_win;
static unsigned char *g_mem;
static unsigned char *g_frame_pool;
static unsigned char *g_rom;

static unsigned char *g_shadow[52];          /* per-space window image */

/* ------------------------------------------------------------ paging state */

static uint32 g_space_count = 2u;
static uint32 g_current;
static uint32 g_window_va;                   /* 32-bit VA of g_win */
static int32  g_activate_rc[52];
static int    g_activate_calls;     /* how many activate calls so far */
static int    g_activate_fail_from; /* 1..N: calls from here on return -2 */
static int    g_switch_broken;               /* model a switch that lost data */

static uint32 g_map_pages;
static uint32 g_dyn_used;
static int32  g_map_rc;
static int32  g_map_rc_2nd;      /* used for the 2nd and later map calls */
static int32  g_unmap_rc;
static int32  g_check_rc;
static int32  g_stats_rc;
static uint32 g_checked;
static uint32 g_bad;
static uint32 g_bad_base;
static uint32 g_pair_total;
static uint32 g_pair_count;
static uint32 g_pair_pa[16];
static uint32 g_lookup_hit;
static int    g_lookup_hit_from;  /* 1-based call number from which hit=1 */
static int    g_lookup_calls;
static int32  g_space_of_bias;    /* shift the reported task binding */
static uint32 g_last_lookup_idx;
static uint32 g_last_lookup_ppn;
static uint32 g_last_lookup_ps;
static uint32 g_last_map_va;
static uint32 g_last_map_pa;
static uint32 g_last_map_size;
static arkrtos_mpu_mem_t g_last_map_mem;
static int    g_map_calls;
static int    g_unmap_calls;
static int    g_check_calls;
static uint32 g_release_rc;
static uint32 g_release_last;
static int    g_release_calls;

int32 arkrtos_paging_stats(uint32 *p_checked, uint32 *p_bad, uint32 *p_bad_base)
{
    if (p_checked != NULL) {
        *p_checked = g_checked;
    }
    if (p_bad != NULL) {
        *p_bad = g_bad;
    }
    if (p_bad_base != NULL) {
        *p_bad_base = g_bad_base;
    }
    return g_stats_rc;
}

int32 arkrtos_paging_map_span(uint32 va, uint32 pa, uint32 size,
                              arkrtos_mpu_mem_t mem)
{
    int32 rc = g_map_rc;

    g_map_calls++;
    if ((g_map_calls > 1) && (g_map_rc_2nd != 0)) {
        rc = g_map_rc_2nd;
    }
    g_last_map_va = va;
    g_last_map_pa = pa;
    g_last_map_size = size;
    g_last_map_mem = mem;
    if (rc == 0) {
        g_map_pages++;
        g_dyn_used++;
    }
    return rc;
}

uint32 arkrtos_paging_map_pages(void)
{
    return g_map_pages;
}

int32 arkrtos_paging_unmap_span(uint32 va, uint32 size)
{
    g_unmap_calls++;
    g_last_map_va = va;
    g_last_map_size = size;
    if (g_unmap_rc == 0) {
        if (g_map_pages > 0u) {
            g_map_pages--;
        }
        if (g_dyn_used > 0u) {
            g_dyn_used--;
        }
    }
    return g_unmap_rc;
}

uint32 arkrtos_paging_dyn_used(void)
{
    return g_dyn_used;
}

int32 arkrtos_port_paging_check(unsigned long base, uint32 size,
                                arkrtos_mpu_mem_t mem)
{
    (void)base;
    (void)size;
    (void)mem;
    g_check_calls++;
    return g_check_rc;
}

uint32 arkrtos_port_paging_lookup(unsigned long va, uint32 *index,
                                 uint32 *ppn, uint32 *ps)
{
    uint32 idx = (uint32)((unsigned long)va - g_window_va) >> 12;
    uint32 hit = g_lookup_hit;

    g_lookup_calls++;
    if ((g_lookup_hit_from > 0) && (g_lookup_calls >= g_lookup_hit_from)) {
        hit = 1u;
    }

    g_last_lookup_idx = idx;
    g_last_lookup_ppn = (uint32)(va & 0xFFFFF000u) | 0x100u;
    g_last_lookup_ps = 22u;
    if (index != NULL) {
        *index = idx;
    }
    if (ppn != NULL) {
        *ppn = g_last_lookup_ppn;
    }
    if (ps != NULL) {
        *ps = g_last_lookup_ps;
    }
    return hit;
}

unsigned long arkrtos_paging_window_va(void)
{
    return (unsigned long)g_window_va;
}

uint32 arkrtos_paging_space_count(void)
{
    return g_space_count;
}

uint32 arkrtos_paging_space_frame(uint32 space)
{
    if (space >= 52u) {
        return 0u;
    }
    return 0x40000000u + (space * 0x100000u);
}

uint32 arkrtos_paging_space_current(void)
{
    return g_current;
}

int32 arkrtos_paging_space_activate(uint32 space)
{
    g_activate_calls++;
    if ((g_activate_fail_from > 0) && (g_activate_calls >= g_activate_fail_from)) {
        return -2;
    }
    if (space >= g_space_count) {
        return -1;
    }
    if (space < 52u && g_activate_rc[space] != 0) {
        return g_activate_rc[space];
    }
    if (space == g_current) {
        return 0;
    }
    if (g_switch_broken == 0) {
        if (g_current < 52u) {
            memcpy(g_shadow[g_current], g_win, WIN_SIZE);
        }
        g_current = space;
        memcpy(g_win, g_shadow[g_current], WIN_SIZE);
    } else {
        g_current = space;
    }
    return 0;
}

/* The kernel task table: only its address arithmetic matters here. */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];

static uint32 g_task_space[ARKRTOS_TASK_MAX_NUM];

uint32 arkrtos_paging_space_of(arkrtos_task_t *p_task)
{
    int32 idx;

    if ((p_task == NULL) || (p_task < arkrtos_task_table)) {
        return 0u;
    }
    idx = ARKRTOS_TASK_IDX(p_task);
    if ((idx < 0) || (idx >= (int32)ARKRTOS_TASK_MAX_NUM)) {
        return 0u;
    }
    return (uint32)((int32)g_task_space[idx] + g_space_of_bias);
}

int32 arkrtos_paging_space_release(uint32 space)
{
    g_release_calls++;
    g_release_last = space;
    return (int32)g_release_rc;
}

uint32 arkrtos_paging_pair_total(void)
{
    return g_pair_total;
}

uint32 arkrtos_paging_pair_count(uint32 space)
{
    (void)space;
    return g_pair_count;
}

uint32 arkrtos_paging_pair_pa(uint32 space, uint32 idx)
{
    (void)space;
    if (idx >= 16u) {
        return 0u;
    }
    return g_pair_pa[idx];
}

/* ---- frames --------------------------------------------------------- */

typedef struct {
    uint32 base;
    uint32 count;
    int    used;
} frame_rec_t;

static frame_rec_t g_frames[16];
static uint32 g_frame_used;
static uint32 g_frame_total = FRAME_COUNT;
static int32  g_frame_alloc_rc;
static int32  g_frame_free_rc;

int32 arkrtos_paging_frame_alloc(uint32 count, uint32 *p_base)
{
    uint32 i;

    if ((g_frame_alloc_rc != 0) || (count == 0u) ||
        (g_frame_used + count > g_frame_total)) {
        return (g_frame_alloc_rc != 0) ? g_frame_alloc_rc : -1;
    }
    for (i = 0u; i < 16u; i++) {
        if (g_frames[i].used == 0) {
            g_frames[i].used = 1;
            g_frames[i].base = FRAME_VA + (g_frame_used * 0x1000u);
            g_frames[i].count = count;
            g_frame_used += count;
            if (p_base != NULL) {
                *p_base = g_frames[i].base;
            }
            return 0;
        }
    }
    return -1;
}

int32 arkrtos_paging_frame_free(uint32 base, uint32 count)
{
    uint32 i;

    if (g_frame_free_rc != 0) {
        return g_frame_free_rc;
    }
    if ((base < FRAME_VA) || (base >= (FRAME_VA + (FRAME_COUNT * 0x1000u)))) {
        return -1;
    }
    for (i = 0u; i < 16u; i++) {
        if ((g_frames[i].used != 0) && (g_frames[i].base == base) &&
            (g_frames[i].count == count)) {
            g_frames[i].used = 0;
            g_frame_used -= count;
            return 0;
        }
    }
    return -3;
}

uint32 arkrtos_paging_frame_total(void)
{
    return g_frame_total;
}

uint32 arkrtos_paging_frame_used(void)
{
    return g_frame_used;
}

/* ---- controlled user access ----------------------------------------- */

static int32 g_ucopy_force;      /* non-zero: return this for every call */
static int g_ucopy_starve;       /* 1 = the RAM read succeeds without copying */
static int g_ucopy_rom_wrong;    /* 1 = the ROM read reports ok, copies nothing */
static uint32 g_ucopy_calls;

int32 arkrtos_paging_user_copy(unsigned long rom_start, uint32 rom_size,
                               unsigned long ram_start, uint32 ram_size,
                               uint32 space, uint32 installed,
                               void *kern, const void *user, uint32 len,
                               uint8 write)
{
    uint8 *dst = (uint8 *)kern;
    const uint8 *src = (const uint8 *)(uintptr_t)user;
    uint32 i;

    g_ucopy_calls++;

    if (g_ucopy_force != 0) {
        return g_ucopy_force;
    }
    if ((len == 0u) || (user == NULL) || (kern == NULL)) {
        return -2;
    }
    if ((rom_size == 0u) && (ram_size == 0u)) {
        return -2;
    }
    /* 32-bit wrap of user + len is refused before any range check */
    if (((uint32)(uintptr_t)user + len) < (uint32)(uintptr_t)user) {
        return -1;
    }
    if (space != installed) {
        return -4;
    }
    if ((uintptr_t)user == 0x00200000u) {
        return -3;       /* kernel image address */
    }
    if (((uintptr_t)user >= 0x80000000u) ||
        ((uintptr_t)user < 0x00100000u)) {
        return -3;       /* unmapped */
    }

    if (((uintptr_t)user >= (uintptr_t)rom_start) &&
        ((uintptr_t)user + len <= (uintptr_t)rom_start + rom_size)) {
        if (write != 0u) {
            return -3;   /* ROM window is read-only */
        }
        if (g_ucopy_rom_wrong != 0) {
            return 0;    /* says ok, but the kernel buffer is untouched */
        }
        for (i = 0u; i < len; i++) {
            dst[i] = src[i];
        }
        return 0;
    }
    if (((uintptr_t)user >= (uintptr_t)ram_start) &&
        ((uintptr_t)user + len <= (uintptr_t)ram_start + ram_size)) {
        if ((g_ucopy_starve != 0) && (write == 0u)) {
            return 0;    /* says ok, but the kernel buffer stays as it was */
        }
        if (write != 0u) {
            for (i = 0u; i < len; i++) {
                ((uint8 *)(uintptr_t)user)[i] = dst[i];
            }
        } else {
            for (i = 0u; i < len; i++) {
                dst[i] = src[i];
            }
        }
        return 0;
    }
    return -3;
}

int32 arkrtos_copy_from_user(void *dst, const void *usrc, uint32 len)
{
    (void)dst;
    (void)usrc;
    (void)len;
    return -2;           /* the shell is a kernel task */
}

/* ---- scheduler lock ------------------------------------------------- */

static int g_lock_depth;
static int g_lock_calls;
static int g_unlock_calls;

void arkrtos_sched_lock(void)
{
    g_lock_depth++;
    g_lock_calls++;
}

void arkrtos_sched_unlock(void)
{
    if (g_lock_depth > 0) {
        g_lock_depth--;
    }
    g_unlock_calls++;
}

/* ---- ELF loader ----------------------------------------------------- */

static arkrtos_elf_info_t g_info[52];
static int32 g_elf_running;
static int32 g_elf_running_in[52];
static int32 g_elf_space_count;
static int32 g_elf_last_space = -1;
static int32 g_exec_rc[8];
static uint32 g_exec_space[8];
static int g_exec_idx;
static int g_exec_calls;
static int g_exec_write_sentinel;    /* 1 = do not touch the frame */
static int g_exec_space_reuse;       /* 1 = reuse the first space */
static int g_exec_no_live;           /* 1 = leave running_in at 0 */
static int g_exec_touch_other;
static int g_exec_dup_task;        /* 1 = every load reports task 6 */       /* 1 = clobber the other space too */
static int g_exec_clobber_first;     /* 1 = second load clobbers space 1 */

const char *arkrtos_elf_error_name(int32 rc)
{
    static char buf[32];

    (void)snprintf(buf, sizeof(buf), "elf-err-%d", (int)rc);
    return buf;
}

/* The frame of the space that is currently mapped *is* the window: the
 * stub loader writes through this pointer so that a load into the active
 * space is visible without a switch. */
static unsigned char *frame_ptr(uint32 space)
{
    if (space == g_current) {
        return g_win;
    }
    return g_shadow[space < 52u ? space : 0u];
}

int32 arkrtos_elf_exec_space(const char *path, int32 argc, char *argv[],
                             uint32 *p_space)
{
    int32 rc;
    uint32 sp;

    (void)path;
    (void)argc;
    (void)argv;
    rc = g_exec_rc[g_exec_idx % 8];
    g_exec_idx++;
    g_exec_calls++;
    if (rc != 0) {
        return rc;
    }
    sp = g_exec_space[(g_exec_idx - 1) % 8];
    if (g_exec_space_reuse != 0) {
        sp = g_exec_space[0];
    }
    if (p_space != NULL) {
        *p_space = sp;
    }
    if (sp >= 52u) {
        return -1;
    }
    if (g_exec_write_sentinel == 0) {
        *(uint32 *)(void *)frame_ptr(sp) = 0xEE000000u | sp;
        g_info[sp].bytes = 4096u;
        g_info[sp].entry = (unsigned long)g_window_va;
        g_info[sp].task = (g_exec_dup_task != 0) ? 6 : (int32)(5u + sp);
        g_task_space[4u + sp] = sp;
    }
    if (g_exec_touch_other != 0) {
        uint32 other = (sp == 0u) ? 1u : 0u;

        *(uint32 *)(void *)frame_ptr(other) = 0x12345678u;
    }
    if ((g_exec_clobber_first != 0) && (g_exec_calls >= 2)) {
        *(uint32 *)(void *)frame_ptr(g_exec_space[0]) = 0xDEADBEEFu;
    }
    if (g_exec_no_live == 0) {
        g_elf_running_in[sp] = 1;
        g_elf_running = 1;
    }
    g_elf_last_space = (int32)sp;
    return 0;
}

int32 arkrtos_elf_info_space(uint32 space, arkrtos_elf_info_t *p_out)
{
    if ((space >= 52u) || (p_out == NULL)) {
        return -1;
    }
    *p_out = g_info[space];
    return g_info[space].task > 0 ? ARKRTOS_ELF_OK : -1;
}

int32 arkrtos_elf_running(void)
{
    return g_elf_running;
}

int32 arkrtos_elf_running_in(uint32 space)
{
    return (space < 52u) ? g_elf_running_in[space] : 0;
}

int32 arkrtos_elf_space_count(void)
{
    return g_elf_space_count;
}

int32 arkrtos_elf_last_space(void)
{
    return g_elf_last_space;
}

/* ------------------------------------------------------------ fixtures */

static void reset_all(void)
{
    uint32 i;

    cap_reset();
    g_current = 0u;
    g_space_count = 2u;
    g_activate_calls = 0;
    g_activate_fail_from = 0;
    for (i = 0u; i < 52u; i++) {
        g_activate_rc[i] = 0;
        g_elf_running_in[i] = 0;
        g_info[i].task = 0;
        g_info[i].bytes = 0u;
        g_info[i].entry = 0u;
        g_task_space[i < ARKRTOS_TASK_MAX_NUM ? i : 0u] = 0u;
    }
    for (i = 0u; i < ARKRTOS_TASK_MAX_NUM; i++) {
        g_task_space[i] = 0u;
    }
    g_switch_broken = 0;
    g_map_pages = 0u;
    g_dyn_used = 0u;
    g_map_rc = 0;
    g_map_rc_2nd = 0;
    g_unmap_rc = 0;
    g_check_rc = 0;
    g_stats_rc = 0;
    g_checked = 7u;
    g_bad = 1u;
    g_bad_base = 0x1234u;
    g_pair_total = 0u;
    g_pair_count = 0u;
    memset(g_pair_pa, 0, sizeof(g_pair_pa));
    g_lookup_hit = 0u;
    g_lookup_hit_from = 0;
    g_lookup_calls = 0;
    g_space_of_bias = 0;
    g_map_calls = 0;
    g_unmap_calls = 0;
    g_check_calls = 0;
    g_release_rc = 0u;
    g_release_last = 99u;
    g_release_calls = 0;
    memset(g_frames, 0, sizeof(g_frames));
    g_frame_used = 0u;
    g_frame_total = FRAME_COUNT;
    g_frame_alloc_rc = 0;
    g_frame_free_rc = 0;
    g_ucopy_force = 0;
    g_ucopy_starve = 0;
    g_ucopy_rom_wrong = 0;
    g_ucopy_calls = 0u;
    g_lock_depth = 0;
    g_lock_calls = 0;
    g_unlock_calls = 0;
    g_elf_running = 0;
    g_elf_space_count = 2;
    g_elf_last_space = -1;
    memset(g_exec_rc, 0, sizeof(g_exec_rc));
    g_exec_idx = 0;
    g_exec_calls = 0;
    g_exec_write_sentinel = 0;
    g_exec_space_reuse = 0;
    g_exec_no_live = 0;
    g_exec_touch_other = 0;
    g_exec_dup_task = 0;
    g_exec_clobber_first = 0;
    memset(g_shadow[0], 0, WIN_SIZE);
    memset(g_shadow[1], 0, WIN_SIZE);
    memset(g_win, 0, WIN_SIZE);
}

/* ------------------------------------------------------------ helpers */

static int pmap_cmd(int argc, const char *a0, const char *a1,
                    const char *a2, const char *a3, const char *a4,
                    const char *a5)
{
    const char *args[6];
    char *argv[6];
    int i;
    int rc;

    args[0] = a0;
    args[1] = a1;
    args[2] = a2;
    args[3] = a3;
    args[4] = a4;
    args[5] = a5;
    for (i = 0; i < argc; i++) {
        argv[i] = W(args[i]);
    }
    rc = cmd_pmap(argc, argv);
    return rc;
}

#define PMAP0()                     pmap_cmd(1, "pmap", 0, 0, 0, 0, 0)
#define PMAP1(a)                    pmap_cmd(2, "pmap", a, 0, 0, 0, 0)
#define PMAP2(a, b)                 pmap_cmd(3, "pmap", a, b, 0, 0, 0)
#define PMAP3(a, b, c)              pmap_cmd(4, "pmap", a, b, c, 0, 0)
#define PMAP4(a, b, c, d)           pmap_cmd(5, "pmap", a, b, c, d, 0)
#define PMAP5(a, b, c, d, e)        pmap_cmd(6, "pmap", a, b, c, d, e)

/* ------------------------------------------------------------ cases */

static void t_default(void)
{
    reset_all();
    CK_I(PMAP0(), 0);
    CK(out_has("pmap: init_rc=0 checked=7 bad=1 bad_base=0x00001234"));
    CK(out_has("usage: pmap [map"));

    reset_all();
    CK_I(PMAP1("bogus"), 0);
    CK(out_has("usage: pmap [map"));

    /* argc == 2 with a word that is not a subcommand still prints the audit */
    reset_all();
    CK_I(PMAP2("nope", "x"), 0);
    CK(out_has("usage: pmap [map"));
}

static void t_map(void)
{
    reset_all();
    CK_I(PMAP1("map"), 1);
    CK(out_has("usage: pmap map <va> <pa> <size> <ram|rom|ro|rw>"));

    reset_all();
    CK_I(PMAP4("map", "0x1000", "0x2000", "0x1000"), 1);
    CK(out_has("usage: pmap map"));

    reset_all();
    CK_I(PMAP5("map", "0xzz", "0x2000", "0x1000", "ram"), 1);
    CK(out_has("usage: pmap map"));

    /* the three other ways pmap_hex refuses: empty, bare prefix, no digits */
    reset_all();
    CK_I(PMAP5("map", "", "0x2000", "0x1000", "ram"), 1);
    CK(out_has("usage: pmap map"));

    reset_all();
    CK_I(PMAP5("map", "0x", "0x2000", "0x1000", "ram"), 1);
    CK(out_has("usage: pmap map"));

    reset_all();
    CK_I(PMAP5("map", "0x1a", "0x2000", "0x10", "ram"), 0);
    CK_I(g_last_map_va, 0x1Au);

    reset_all();
    CK_I(PMAP5("map", "0x1000", "0x2000", "0x10", "bogus"), 1);
    CK(out_has("usage: pmap map"));

    reset_all();
    CK_I(PMAP5("map", "0x1000", "0x2000", "0x10", "rom"), 0);
    CK_I(g_map_calls, 1);
    CK_I(g_last_map_va, 0x1000u);
    CK_I(g_last_map_pa, 0x2000u);
    CK_I(g_last_map_size, 0x10u);
    CK_I((long)g_last_map_mem, (long)ARKRTOS_MPU_MEM_ROM);
    CK(out_has("pmap: map 0x00001000 <- 0x00002000 size=16 rc=0 pages=1"));

    reset_all();
    CK_I(PMAP5("map", "1000", "2000", "3000", "ro"), 0);
    CK_I((long)g_last_map_mem, (long)ARKRTOS_MPU_MEM_PERIPH_RO);

    reset_all();
    CK_I(PMAP5("map", "0X1000", "0x2000", "0x10", "rw"), 0);
    CK_I((long)g_last_map_mem, (long)ARKRTOS_MPU_MEM_PERIPH_RW);

    reset_all();
    g_map_rc = -5;
    CK_I(PMAP5("map", "0x1000", "0x2000", "0x10", "ram"), 1);
    CK(out_has("rc=-5"));
}

static void t_check(void)
{
    reset_all();
    CK_I(PMAP1("check"), 0);        /* argc < 3: falls through to the audit */
    CK(out_has("usage: pmap [map"));

    reset_all();
    CK_I(PMAP2("check", "0x1000"), 1);
    CK(out_has("usage: pmap check <va> <size> <ram|rom|ro|rw>"));

    reset_all();
    CK_I(PMAP3("check", "0x1000", "bad"), 1);
    CK(out_has("usage: pmap check"));

    reset_all();
    CK_I(PMAP4("check", "0x1000", "0x20", "ram"), 0);
    CK_I(g_check_calls, 1);
    CK(out_has("pmap: check 0x00001000 +32 rc=0"));

    reset_all();
    CK_I(PMAP4("check", "0x1000", "0x20", "bogus"), 1);
    CK(out_has("usage: pmap check"));

    reset_all();
    g_check_rc = -1;
    CK_I(PMAP4("check", "0x1000", "0x20", "ro"), 1);
    CK(out_has("rc=-1"));
}

static void t_rd_rb_wrb(void)
{
    uint32 *p = (uint32 *)(void *)g_mem;

    p[0] = 0x11111111u;
    p[1] = 0x22222222u;

    reset_all();
    CK_I(PMAP1("rd"), 0);
    CK(out_has("usage: pmap [map"));

    reset_all();
    CK_I(PMAP3("rd", "zz", "2"), 1);
    CK(out_has("usage: pmap rd <va> [count]"));

    reset_all();
    CK_I(PMAP3("rd", "0x1000", "two"), 1);
    CK(out_has("usage: pmap rd <va> [count]"));

    {
        char va[24];

        (void)snprintf(va, sizeof(va), "0x%08X", (unsigned)MEM_VA);
        reset_all();
        CK_I(PMAP3("rd", va, "2"), 0);
        CK(out_has("= 0x11111111"));
        CK(out_has("= 0x22222222"));
        CK(out_count("pmap: [0x") == 2);

        /* the clamp: ask for 20 words, get 8 */
        reset_all();
        CK_I(PMAP3("rd", va, "20"), 0);
        CK(out_count("pmap: [0x") == 8);

        reset_all();
        CK_I(PMAP2("rb", va), 0);
        CK(out_has("= 0x11"));

        reset_all();
        CK_I(PMAP2("rb", "0xzz"), 1);
        CK(out_has("usage: pmap rb <va>"));

        reset_all();
        CK_I(PMAP3("wrb", va, "0x5A"), 0);
        CK_I(p[0], 0x1111115Au);
        p[0] = 0x11111111u;

        reset_all();
        CK_I(PMAP3("wrb", va, "zz"), 1);
        CK(out_has("usage: pmap wrb <va> <byte>"));

        reset_all();
        CK_I(PMAP3("wrb", "zz", "0x5A"), 1);
        CK(out_has("usage: pmap wrb"));

        /* argc == 3 never reaches wrb: it falls through to the audit */
        reset_all();
        CK_I(PMAP2("wrb", va), 0);
        CK(out_has("usage: pmap [map"));
    }
}

static void t_unmap_dyn(void)
{
    reset_all();
    CK_I(PMAP1("unmap"), 0);
    CK(out_has("usage: pmap [map"));

    reset_all();
    CK_I(PMAP2("unmap", "0x1000"), 1);
    CK(out_has("usage: pmap unmap <va> <size>"));

    reset_all();
    CK_I(PMAP2("unmap", "0xzz"), 1);
    CK(out_has("usage: pmap unmap <va> <size>"));

    reset_all();
    g_map_pages = 3u;
    g_dyn_used = 2u;
    CK_I(PMAP3("unmap", "0x1000", "0x1000"), 0);
    CK_I(g_unmap_calls, 1);
    CK(out_has("pmap: unmap 0x00001000 size=4096 rc=0 pages=2 dyn=1"));

    reset_all();
    g_unmap_rc = -4;
    CK_I(PMAP3("unmap", "0x1000", "0x1000"), 1);
    CK(out_has("rc=-4"));

    reset_all();
    g_dyn_used = 4u;
    g_map_pages = 9u;
    CK_I(PMAP1("dyn"), 0);
    CK(out_has("pmap: dyn slot(s) in use=4 pages=9"));
}

static void t_frame(void)
{
    reset_all();
    g_frame_used = 5u;
    CK_I(PMAP1("frame"), 0);
    CK(out_has("pmap: frames total=64 used=5 free=59"));

    reset_all();
    g_frame_total = 0u;
    CK_I(PMAP1("frame"), 0);
    CK(out_has("free=0"));      /* used > total does not underflow */
    g_frame_total = FRAME_COUNT;

    /* PASS: two allocations, pattern in all four frames, free back */
    reset_all();
    CK_I(PMAP1("ftest"), 0);
    CK(out_has("pmap ftest: PASS (alloc/free/double-free/pattern, used back to 0)"));
    CK_I(g_frame_used, 0u);

    /* alloc 1 refused */
    reset_all();
    g_frame_alloc_rc = -1;
    CK_I(PMAP1("ftest"), 1);
    CK(out_has("pmap ftest: FAIL (alloc 1)"));

    /* alloc 3 refused: the first frame is handed back */
    reset_all();
    g_frame_total = 1u;
    CK_I(PMAP1("ftest"), 1);
    CK(out_has("pmap ftest: FAIL (alloc 3)"));
    CK_I(g_frame_used, 0u);

    /* free of the first frame fails */
    reset_all();
    g_frame_free_rc = -2;
    CK_I(PMAP1("ftest"), 1);
    CK(out_has("pmap ftest: FAIL"));
}

static void t_map_test(void)
{
    unsigned char *rom = g_rom;

    /* same pattern on both sides: map / compare / unmap / rebind / unmap */
    memset((void *)TEST_VA, 0x3C, 0x1000);
    memset(rom, 0x3C, 0x2000);

    reset_all();
    CK_I(PMAP1("test"), 0);
    CK(out_has("pmap: map ROM 0x03000000 <- 0x02000000 rc=0"));
    CK(out_has("pmap: compare 16 words, mismatch=0"));
    CK(out_has("left_entry=0"));
    CK(out_has("pmap test: PASS (map / unmap / rebind / unmap)"));

    /* the first comparison only reports: one differing word still passes,
     * as long as the rebind page matches.  This is the module's own reading,
     * not an endorsement - it is why the rebind loop is the one that gates. */
    memset((void *)TEST_VA, 0x3C, 0x1000);
    memset(rom, 0x3C, 0x2000);
    *(volatile uint32 *)(void *)rom = 0x00000000u;
    reset_all();
    CK_I(PMAP1("test"), 0);
    CK(out_has("pmap: compare 16 words, mismatch=1"));
    CK(out_has("pmap: rebind compare 16 words, mismatch=0"));
    CK(out_has("pmap test: PASS"));
    memset(rom, 0x3C, 0x2000);

    /* map refused */
    reset_all();
    g_map_rc = -2;
    CK_I(PMAP1("test"), 1);
    CK(out_has("pmap test: FAIL (map refused)"));

    /* unmap leaves an entry: lookup still hits */
    reset_all();
    g_lookup_hit = 1u;
    CK_I(PMAP1("test"), 1);
    CK(out_has("pmap test: FAIL (unmap left an entry)"));

    /* unmap returns an error */
    reset_all();
    g_unmap_rc = -3;
    CK_I(PMAP1("test"), 1);
    CK(out_has("pmap test: FAIL (unmap left an entry)"));

    /* rebind refused: the dynamic slot was not recycled */
    reset_all();
    g_map_rc_2nd = -5;
    CK_I(PMAP1("test"), 1);
    CK(out_has("pmap test: FAIL (slot not recycled)"));

    /* rebind translation mismatch: second ROM page differs */
    memset(rom + 0x1000, 0x77, 0x1000);
    reset_all();
    CK_I(PMAP1("test"), 1);
    CK(out_has("pmap test: FAIL (rebind translation)"));
    memset(rom + 0x1000, 0x3C, 0x1000);

    /* final unmap leaves an entry: the hit appears on the second lookup */
    reset_all();
    g_lookup_hit_from = 2;
    CK_I(PMAP1("test"), 1);
    CK(out_has("pmap test: FAIL (final unmap left an entry)"));
}

static void t_test_branch_extra(void)
{
    /* A clean reboot of the two mapped pages: the command must be
     * repeatable, not a one-shot.  Both runs must land on PASS. */
    memset((void *)TEST_VA, 0x3C, 0x1000);
    memset(g_rom, 0x3C, 0x2000);
    reset_all();
    CK_I(PMAP1("test"), 0);
    CK(out_has("pmap test: PASS"));

    memset((void *)TEST_VA, 0x3C, 0x1000);
    memset(g_rom, 0x3C, 0x2000);
    reset_all();
    CK_I(PMAP1("test"), 0);
    CK(out_has("pmap test: PASS"));
    CK_I(g_dyn_used, 0u);
}

static void t_rel_space(void)
{
    reset_all();
    CK_I(PMAP2("rel", "zz"), -1);
    CK(out_has("usage: pmap rel <space>"));

    reset_all();
    g_frame_used = 9u;
    g_release_rc = 0u;
    CK_I(PMAP2("rel", "1"), 0);
    CK_I(g_release_calls, 1);
    CK_I(g_release_last, 1u);
    CK(out_has("pmap rel: space 1 rc=0 frames 9 -> 9"));
    CK(out_has("current=0"));

    reset_all();
    g_release_rc = 4u;
    CK_I(PMAP2("rel", "3"), 1);
    CK(out_has("space 3 rc=4"));

    /* space: listing */
    reset_all();
    g_space_count = 3u;
    g_current = 2u;
    CK_I(PMAP1("space"), 0);
    CK(out_has("pmap: spaces=3 current=2"));
    CK(out_count("-> frame 0x") == 3);

    /* space: switch */
    reset_all();
    CK_I(PMAP2("space", "1"), 0);
    CK(out_has("pmap: space 1 rc=0 (current=1)"));
    CK_I(g_current, 1u);

    reset_all();
    g_activate_rc[1] = -2;
    CK_I(PMAP2("space", "1"), 1);
    CK(out_has("rc=-2"));

    reset_all();
    CK_I(PMAP2("space", "zz"), -1);
    CK(out_has("usage: pmap space [n]"));
}

static void t_pairs_look(void)
{
    reset_all();
    g_pair_total = 0u;
    CK_I(PMAP1("pairs"), 1);
    CK(out_has("this build has no paged window"));

    reset_all();
    g_pair_total = 16u;
    g_space_count = 2u;
    CK_I(PMAP2("pairs", "7"), 1);
    CK(out_has("space 7 out of range"));

    reset_all();
    g_pair_total = 16u;
    g_pair_count = 10u;
    g_pair_pa[0] = 0u;          /* skipped */
    g_pair_pa[1] = 0x1000u;
    g_pair_pa[2] = 0x2000u;
    CK_I(PMAP1("pairs"), 0);
    CK(out_has("pmap pairs: space 0 mapped=10/16 pair(s) of 8 KB (80 KB)"));
    CK(out_has("pair 1: va="));
    CK(out_count("  pair ") == 2);
    CK(out_has("... 8 more pair(s) not shown"));

    reset_all();
    g_pair_total = 16u;
    g_pair_count = 2u;
    g_pair_pa[3] = 0x3000u;
    g_pair_pa[4] = 0x4000u;
    CK_I(PMAP1("pairs"), 0);
    CK_I(out_count("  pair "), 2);
    CK(out_has("... 0 more pair(s) not shown") == 0);

    reset_all();
    CK_I(PMAP2("pairs", "zz"), -1);
    CK(out_has("usage: pmap pairs [space]"));

    /* look */
    reset_all();
    CK_I(PMAP1("look"), -1);
    CK(out_has("usage: pmap look <va>"));

    reset_all();
    CK_I(PMAP2("look", "zz"), -1);
    CK(out_has("usage: pmap look <va>"));

    reset_all();
    g_lookup_hit = 0u;
    CK_I(PMAP2("look", "0x01000000"), 1);
    CK(out_has("no entry"));

    reset_all();
    g_lookup_hit = 1u;
    CK_I(PMAP2("look", "0x01000000"), 0);
    CK(out_has("pmap look: va=0x01000000 entry=0 ps=22 ppn=0x"));
}

static void t_u1(void)
{
    reset_all();
    g_elf_running = 1;
    CK_I(PMAP1("u1"), 1);
    CK(out_has("refused - an ELF program is using the window"));

    reset_all();
    g_space_count = 1u;
    CK_I(PMAP1("u1"), 1);
    CK(out_has("refused - board provides only one address space"));

    reset_all();
    CK_I(PMAP1("u1"), 0);
    CK(out_has("pmap u1: va=0x01100000 space0=0xA5A50001 space1=0xA5A50002"));
    CK(out_has("pmap u1: PASS - same VA, two frames"));
    CK_I(g_current, 0u);

    /* only the switch back to space 1 fails: space 0 still reads right,
     * so the report says one of the two reads is wrong */
    reset_all();
    g_activate_fail_from = 4;
    CK_I(PMAP1("u1"), 1);
    CK(out_has("pmap u1: FAIL (1 of 2 reads wrong)"));

    /* a switch that does not move the frame: the two reads collide */
    reset_all();
    g_switch_broken = 1;
    CK_I(PMAP1("u1"), 1);
    CK(out_has("pmap u1: FAIL"));
}

static void t_elf(void)
{
    reset_all();
    g_elf_space_count = 2;
    g_elf_last_space = 1;
    g_info[1].task = 4;
    g_info[1].bytes = 8192u;
    g_info[1].entry = 0x01000000u;
    g_elf_running_in[1] = 1;
    CK_I(PMAP1("elf"), 0);
    CK(out_has("pmap elf: spaces=2 last=1"));
    CK(out_has("space 0 frame=0x40000000 empty"));
    CK(out_has("space 1 frame=0x40100000 task=4 live=1 bytes=8192"));
    CK(out_has("entry=0x01000000"));
    CK(out_has("pmap elf: 1 live instance(s)"));

    /* no spaces at all: only the two summary lines */
    reset_all();
    g_elf_space_count = 0;
    g_elf_last_space = -1;
    CK_I(PMAP1("elf"), 0);
    CK(out_has("spaces=0 last=-1"));
    CK(out_has("pmap elf: 0 live instance(s)"));
    CK(out_count("  space ") == 0);

    /* the loop is capped by the task table, not by the space count */
    reset_all();
    g_elf_space_count = 50;
    CK_I(PMAP1("elf"), 0);
    CK_I(out_count("  space "), (int)ARKRTOS_TASK_MAX_NUM);
}

static void t_u2(void)
{
    char pathbuf[24];

    (void)snprintf(pathbuf, sizeof(pathbuf), "%s", "/mnt/ram/hello");

    reset_all();
    g_space_count = 1u;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("pmap u2: refused - board provides only one address space"));

    reset_all();
    g_elf_running = 1;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("refused - an ELF program is already running"));

    /* PASS: two loads, two spaces, sentinels intact where they should be */
    reset_all();
    g_exec_space[0] = 1u;
    g_exec_space[1] = 0u;
    CK_I(PMAP2("u2", pathbuf), 0);
    CK(out_has("pmap u2: PASS - two instances, two spaces"));
    CK(out_has("same VA 0x01000000 reads 0xEE000001 / 0xEE000000"));
    CK_I(g_lock_calls, 1);
    CK_I(g_unlock_calls, 1);
    CK_I(g_lock_depth, 0);

    /* default path when argv[2] is absent */
    reset_all();
    g_exec_space[0] = 1u;
    g_exec_space[1] = 0u;
    CK_I(PMAP1("u2"), 0);
    CK(out_has("/mnt/ram/hello -> space 1 task 6, space 0 task 5"));

    /* first exec fails */
    reset_all();
    g_exec_rc[0] = -7;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("first exec failed: elf-err--7"));
    CK(out_has("FAIL (1 checks failed)"));

    /* second exec fails */
    reset_all();
    g_exec_space[0] = 1u;
    g_exec_rc[1] = -6;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("second exec failed"));

    /* second instance reuses the first space */
    reset_all();
    g_exec_space[0] = 1u;
    g_exec_space[1] = 1u;
    g_exec_space_reuse = 1;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("second instance reused space 1"));

    /* the load did not overwrite its own sentinel */
    reset_all();
    g_exec_space[0] = 1u;
    g_exec_space[1] = 0u;
    g_exec_write_sentinel = 1;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("still holds its sentinel after the load"));

    /* no live task after the load, and a shared task id */
    reset_all();
    g_exec_space[0] = 1u;
    g_exec_space[1] = 0u;
    g_exec_dup_task = 1;
    g_exec_write_sentinel = 0;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("both instances share task 6"));

    /* no live task right after the load */
    reset_all();
    g_exec_space[0] = 1u;
    g_exec_space[1] = 0u;
    g_exec_no_live = 1;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("has no live task"));

    /* the first load also wrote into the other space's frame */
    reset_all();
    g_exec_space[0] = 1u;
    g_exec_space[1] = 0u;
    g_exec_touch_other = 1;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("wrote into space"));

    /* the second load clobbered the first instance's frame */
    reset_all();
    g_exec_space[0] = 1u;
    g_exec_space[1] = 0u;
    g_exec_clobber_first = 1;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("the second load changed space 1"));

    /* both tasks are alive but bound to the wrong space */
    reset_all();
    g_exec_space[0] = 1u;
    g_exec_space[1] = 0u;
    g_space_of_bias = 1;
    CK_I(PMAP1("u2"), 1);
    CK(out_has("is not bound to space"));
}

static void t_cu(void)
{
    unsigned long uram = (unsigned long)g_window_va + 0x100000u;
    unsigned long urom = uram + 0x800u;

    /* the controlled-copy stub picks its answer from the arguments, so one
     * run walks all eleven calls of the self-test. */
    reset_all();
    CK_I(PMAP1("cu"), 0);
    CK(out_has("pmap cu: PASS - from/to user, refusals -1/-2/-3/-4"));
    CK_I(g_ucopy_calls, 10u);

    /* the ROM read reports ok but copies nothing: the byte check catches it */
    reset_all();
    g_ucopy_rom_wrong = 1;
    CK_I(PMAP1("cu"), 1);
    CK(out_has("pmap cu: FAIL from_user ROM bytes 0x40..0x4F"));
    CK(out_has("pmap cu: FAIL (1 checks failed)"));

    /* refused: an ELF program owns the window */
    reset_all();
    g_elf_running = 1;
    CK_I(PMAP1("cu"), 1);
    CK(out_has("pmap cu: refused - an ELF program is using the window"));

    /* refused: space 0 cannot be installed */
    reset_all();
    g_activate_rc[0] = -1;
    CK_I(PMAP1("cu"), 1);
    CK(out_has("pmap cu: refused - cannot install space 0"));

    /* a read says ok but the kernel buffer was not filled */
    reset_all();
    g_ucopy_starve = 1;
    CK_I(PMAP1("cu"), 1);
    CK(out_has("pmap cu: FAIL from_user RAM bytes"));

    /* one refusal code comes back wrong */
    reset_all();
    g_ucopy_force = -9;
    CK_I(PMAP1("cu"), 1);
    CK(out_has("pmap cu: FAIL from_user RAM rc=-9 want=0"));

    /* only one address space: the wrong-space step is skipped */
    reset_all();
    g_space_count = 1u;
    CK_I(PMAP1("cu"), 0);
    CK(out_has("pmap cu: PASS"));

    /* space 1 cannot be installed for the wrong-space step */
    reset_all();
    g_activate_rc[1] = -1;
    CK_I(PMAP1("cu"), 1);
    CK(out_has("pmap cu: FAIL cannot install space 1"));

    (void)urom;
}

/* ------------------------------------------------------------ main */

int main(void)
{
#if defined(_WIN32)
    void *p;

    p = VirtualAlloc((void *)(uintptr_t)WIN_VA, WIN_SIZE,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_win = (unsigned char *)p;
    p = VirtualAlloc((void *)(uintptr_t)MEM_VA, 0x1000u,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_mem = (unsigned char *)p;
    p = VirtualAlloc((void *)(uintptr_t)FRAME_VA, FRAME_COUNT * 0x1000u,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_frame_pool = (unsigned char *)p;
    p = VirtualAlloc((void *)(uintptr_t)TEST_VA, 0x1000u,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CK(p != NULL);
    if (p == NULL) {
        printf("FAIL: cannot reserve the self-test VA 0x%08lX\n",
               (unsigned long)TEST_VA);
        return 1;
    }
    p = VirtualAlloc((void *)(uintptr_t)ARKRTOS_MPU_KERNEL_ROM_BASE, 0x2000u,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CK(p != NULL);
    if (p == NULL) {
        printf("FAIL: cannot reserve the kernel ROM base 0x%08lX "
               "(see host_board_la64.h)\n",
               (unsigned long)ARKRTOS_MPU_KERNEL_ROM_BASE);
        return 1;
    }
    g_rom = (unsigned char *)p;
#else
    g_win = NULL;
#endif

    CK(g_win != NULL);
    CK(g_mem != NULL);
    CK(g_rom != NULL);
    if ((g_win == NULL) || (g_mem == NULL) || (g_rom == NULL)) {
        printf("FAIL: cannot reserve the low host buffers\n");
        return 1;
    }
    g_window_va = WIN_VA;

    {
        uint32 i;

        for (i = 0u; i < 52u; i++) {
            g_shadow[i] = (unsigned char *)calloc(1u, WIN_SIZE);
            CK(g_shadow[i] != NULL);
        }
    }

    t_default();
    t_map();
    t_check();
    t_rd_rb_wrb();
    t_unmap_dyn();
    t_frame();
    t_map_test();
    t_test_branch_extra();
    t_rel_space();
    t_pairs_look();
    t_u1();
    t_elf();
    t_u2();
    t_cu();

    printf("SHELL-PMAP-HOST OK  %d checks\n", g_checks);
    if (g_failed != 0) {
        printf("SHELL-PMAP-HOST FAILED  %d of %d\n", g_failed, g_checks);
        return 1;
    }
    return 0;
}
