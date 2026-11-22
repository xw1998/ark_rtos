/**
* @file test_elf_host.c
* @brief arkrtos_elf.c 静态装载路径的宿主用例：解析 / 校验 / 布局 / 铺栈
* @details 被验的是「一份文件被搬进窗口并起跑」这条链上的判据，不是
*          「程序跑起来做了什么事」——后者要真 CPU。整条链按执行顺序是：
*            1. 参数关（空路径 / argc 范围 / argv 元素 / 签名位置不进这一档）；
*            2. 头关（魔数、class、大小端、机器号、e_type、段表尺寸）；
*            3. 段关（filesz>memsz、窗口外、覆盖栈、0 段、地址回绕），
*               **全部在写第一个字节之前返回**；
*            4. 写关（filesz 拷数据、memsz-filesz 清零），落点是绝对 VA；
*            5. 栈关（argc/argv/auxv 的布局、AT_RANDOM 的 16 字节、字符串
*               的 8 字节对齐、sp 指向 argc）；
*            6. 任务关（注册、降权、窗口写回 TCB、MPU 重建）；
*            7. 台账（running / running_in / info / last_space / BUSY /
*               任务死后懒清理）；
*            8. exec_replace（不起新任务、任务号不变）。
*
*          宿主上必须真占住 ARKRTOS_ELF_WINDOW_BASE：装载是**直接 memcpy 到
*          VA**（这是设计，不是实现细节），窗口没映射就是往随机地址写。所以
*          main() 开头用 VirtualAlloc / mmap(MAP_FIXED) 把整块窗口映射出来，
*          失败直接 FAIL 退出，绝不「跳过」——跳过会让所有断言在未映射地址
*          上跑，读起来像通过。
*
*          宿主 gcc 是 32 位 MinGW（ILP32，unsigned long = 4 字节），所以本
*          文件编的是 ELF32 路径：头 52 字节、程序头 32 字节、字宽 4 字节、
*          默认机器号 RISC-V(243)。64 位那条路径（LA64 的 ELF64、TLS、分页
*          空间、动态链接）在宿主上不可达，由 QEMU LA64 上板覆盖，此处不
*          伪装覆盖。
*
*          明确的开关前提（与 ci_gate 的编译宏一致）：
*            ARKRTOS_USE_ELF=1 / ARKRTOS_USE_PRIV=1（ELF 程序的隔离前提）
*            ARKRTOS_USE_MPU=1（默认档：TCB 带 MPU 快照，起跑时要重建）
*            ARKRTOS_USE_DYNLOAD=0 / ARKRTOS_USE_ELF_TLS=0 /
*            ARKRTOS_USE_EXEC_SIGN=0 / ARKRTOS_USE_EXIT_CODE=0 /
*            ARKRTOS_USE_PAGING=0
*
* @author xw
* @date 2026.11.22
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_config.h"
#include "arkrtos_elf.h"

#if (ARKRTOS_USE_ELF == 0)

/* 关档：头文件把每个入口都折成常量。这里只证明这一档仍可编可跑，且每个
 * 入口都回答「没有这件事」，而不是空成功。 */
int main(void)
{
    int fails = 0;

    if(arkrtos_elf_exec("/prog", 0, 0) != (int32)(-1))
    {
        fails++;
    }
    if(arkrtos_elf_exec_space("/prog", 0, 0, 0) != (int32)(-1))
    {
        fails++;
    }
    if(arkrtos_elf_running() != 0)
    {
        fails++;
    }
    if(arkrtos_elf_running_in(0) != 0)
    {
        fails++;
    }
    if(arkrtos_elf_space_count() != 1)
    {
        fails++;
    }
    if(arkrtos_elf_last_space() != (int32)(-1))
    {
        fails++;
    }
    if(arkrtos_elf_info(0) != (int32)(-1))
    {
        fails++;
    }
    if(arkrtos_elf_info_space(0, 0) != (int32)(-1))
    {
        fails++;
    }
    if(arkrtos_elf_exec_replace("/prog", 0, 0, 0, 0) != (int32)(-1))
    {
        fails++;
    }
    if(strcmp(arkrtos_elf_error_name(0), "elf support not compiled in") != 0)
    {
        fails++;
    }

    if(fails != 0)
    {
        printf("ELF-HOST OFF FAIL %d\n", fails);
        return 1;
    }
    printf("ELF-HOST OFF OK 0 failed\n");
    return 0;
}

#else  /* ARKRTOS_USE_ELF == 1 */

#include "arkrtos_task.h"
#include "arkrtos_cfg.h"
#include "arkrtos_hal.h"
#include "arkrtos_vfs.h"
#if (ARKRTOS_USE_MPU == 1)
#include "arkrtos_mpu.h"
#endif

#include <stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* 板级事实由编译宏给（与 ci_gate / coverage_gate 的两档对齐）。缺一个就
 * 在编译期停：这里是用例，不是「猜一个地址」的地方。 */
#if !defined(ARKRTOS_ELF_WINDOW_BASE) || !defined(ARKRTOS_ELF_WINDOW_SIZE)
#error "test_elf_host 需要 ARKRTOS_ELF_WINDOW_BASE / ARKRTOS_ELF_WINDOW_SIZE"
#endif
#if !defined(ARKRTOS_ELF_ARGV_SIZE) || !defined(ARKRTOS_ELF_STACK_SIZE)
#error "test_elf_host 需要 ARKRTOS_ELF_ARGV_SIZE / ARKRTOS_ELF_STACK_SIZE"
#endif
#if (ARKRTOS_USE_PRIV != 1)
#error "test_elf_host 需要 ARKRTOS_USE_PRIV=1（ELF 程序必须降权起跑）"
#endif
#if (ARKRTOS_USE_PAGING == 1)
#error "test_elf_host 只覆盖不分页档：分页路径的窗口写入要走 elf_*_window()"
#endif
#if (ARKRTOS_USE_DYNLOAD == 1) || (ARKRTOS_USE_ELF_TLS == 1)
#error "test_elf_host 只覆盖静态路径；动态/TLS 档需要单独的用例"
#endif
#if (ARKRTOS_USE_EXIT_CODE == 1) || (ARKRTOS_USE_EXEC_SIGN == 1)
#error "test_elf_host 的判据不覆盖退出码与签名两档"
#endif

/* ELF32 常量（与 arkrtos_elf.c 同值，但这里**另写一份**：用例不该因为被测
 * 实现改了常量就跟着改，那样两边一起错也测不出来）。 */
#define T_ELF_EI_CLASS32    (1u)
#define T_ELF_EI_CLASS64    (2u)
#define T_ELF_EI_DATA_LSB   (1u)
#define T_ELF_TYPE_REL      (1u)
#define T_ELF_TYPE_EXEC     (2u)
#define T_ELF_TYPE_DYN      (3u)
#define T_ELF_MACHINE_RISCV (243u)
#define T_ELF_MACHINE_X86   (62u)
#define T_ELF_PT_LOAD       (1u)
#define T_ELF_PT_INTERP     (3u)
#define T_ELF_PT_NOTE       (4u)
#define T_ELF_EHDR_SIZE     (52u)
#define T_ELF_PHDR_SIZE     (32u)
#define T_ELF_WORD_SIZE     (4u)

#define T_AT_NULL           (0u)
#define T_AT_PHDR           (3u)
#define T_AT_PHENT          (4u)
#define T_AT_PHNUM          (5u)
#define T_AT_PAGESZ         (6u)
#define T_AT_ENTRY          (9u)
#define T_AT_RANDOM         (25u)
#define T_AT_EXECFN         (31u)
#define T_AT_CLKTCK         (17u)

#define T_MAX_PHNUM         (16u)
#define T_MAX_ARGS          (16u)
#define T_MAX_ARG_LEN       (256u)

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
        printf("FAIL %s: got 0x%lx want 0x%lx (line %d)\n",
               what, got, want, __LINE__);
    }
}

static void ck_rc(int32 got, int32 want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got %d (%s) want %d (line %d)\n",
               what, (int)got, arkrtos_elf_error_name(got), (int)want, __LINE__);
    }
}

/* ---------------- 绝对地址映射 ---------------- */

static void *g_win;

static int map_window(unsigned long base, unsigned long size)
{
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)base, (SIZE_T)size,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    return (p == (void *)(uintptr_t)base) ? 0 : -1;
#else
    void *p = mmap((void *)(uintptr_t)base, (size_t)size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    return (p == MAP_FAILED) ? -1 : 0;
#endif
}

/* ---------------- 端口与内核桩 ---------------- */

static uint32 g_crit_depth;

uint32 arkrtos_port_enter_critical(void)
{
    return ++g_crit_depth;
}

void arkrtos_port_exit_critical(uint32 state)
{
    (void)state;
    g_crit_depth--;
}

uint32 arkrtos_kernel_tick = 0u;
int32  arkrtos_current_task_id = 0;

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];

/* 任务注册桩：按次序发 1..N，超过 g_task_max 返回 0（=任务表满）。
 * 真实的 arkrtos_task_register 还会挂就绪集、初始化栈帧；这里只需要
 * 「装载器有没有把身份/窗口写对」，所以只裁出被测代码真正读写的字段。 */
static int32 g_task_max = 4;
static int32 g_task_used;
static int32 g_task_stack_checked;

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom,
                            uint32 stack_size, uint8 priority, uint32 period_ms)
{
    int32 id;

    if(g_task_used >= g_task_max)
    {
        return 0;
    }
    g_task_used++;
    id = g_task_used;
    memset(&arkrtos_task_table[id - 1], 0, sizeof(arkrtos_task_table[0]));
    arkrtos_task_table[id - 1].entry = entry;
    arkrtos_task_table[id - 1].stack_bottom = stack_bottom;
    arkrtos_task_table[id - 1].stack_size = stack_size;
    arkrtos_task_table[id - 1].priority = priority;
    arkrtos_task_table[id - 1].period = (int32)period_ms;
    arkrtos_task_table[id - 1].status = ARKRTOS_TASK_READY;
    arkrtos_task_table[id - 1].is_priv = 1u;
    g_task_stack_checked = (stack_size != 0u) ? 1 : 0;
    return id;
}

#if (ARKRTOS_USE_MPU == 1)
static int g_mpu_calls;
static unsigned long g_mpu_rom;
static unsigned long g_mpu_ram;

void arkrtos_mpu_build_task(arkrtos_task_t *p_task)
{
    g_mpu_calls++;
    g_mpu_rom = p_task->rom_start;
    g_mpu_ram = p_task->ram_start;
}
#endif

/* ---------------- 假 VFS：一个文件 /prog ---------------- */

#define T_IMG_CAP (4096u)

static uint8 g_img[T_IMG_CAP];
static uint32 g_img_len;

int32 arkrtos_vfs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off,
                          uint32 *out_len)
{
    uint32 n;

    if(strcmp(path, "/prog") != 0)
    {
        return -1;
    }
    if(off >= g_img_len)
    {
        *out_len = 0u;
        return 0;
    }
    n = g_img_len - off;
    if(n > len)
    {
        n = len;
    }
    memcpy(buf, &g_img[off], (size_t)n);
    *out_len = n;
    return 0;
}

/* ---------------- ELF32 镜像构造器 ---------------- */

typedef struct
{
    uint32 type;
    uint32 vaddr;
    uint32 filesz;
    uint32 memsz;
    uint32 fill;
    uint32 offset;      /* 有效值见 offset_set */
    uint32 offset_set;  /* 1 = 文件里这段内容已存在（如段表本身），不重复写 */
    const char *str;    /* 非 0 时文件内容取这个字符串（含 NUL），长度覆盖 filesz */
} seg_def_t;

typedef struct
{
    uint8  bad_magic;
    uint8  cls;
    uint8  data;
    uint16 etype;
    uint16 machine;
    uint32 entry;
    uint32 phoff;       /* 0 = 标准 52 */
    uint16 phnum_decl;  /* 0 = 实际段数 */
    uint16 phentsize;   /* 0 = 32 */
} hdr_opt_t;

static void put16(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
}

static void put32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
    p[2] = (uint8)((v >> 16) & 0xFFu);
    p[3] = (uint8)((v >> 24) & 0xFFu);
}

static uint32 rd32(unsigned long va)
{
    const uint8 *p = (const uint8 *)(uintptr_t)va;

    return ((uint32)p[0])
         | ((uint32)p[1] << 8)
         | ((uint32)p[2] << 16)
         | ((uint32)p[3] << 24);
}

static uint32 align4(uint32 v)
{
    return (v + 3u) & ~3u;
}

/* 造一份 ELF32。返回文件长度；内容落在 g_img / g_img_len。 */
static uint32 build_elf(const hdr_opt_t *opt, const seg_def_t *segs,
                        uint32 nsegs)
{
    seg_def_t tmp[T_MAX_PHNUM];
    uint32 phnum = (opt->phnum_decl != 0u) ? opt->phnum_decl : nsegs;
    uint32 phentsize = (opt->phentsize != 0u) ? opt->phentsize : T_ELF_PHDR_SIZE;
    uint32 phoff = (opt->phoff != 0u) ? opt->phoff : T_ELF_EHDR_SIZE;
    uint32 off = T_ELF_EHDR_SIZE + (T_ELF_PHDR_SIZE * nsegs);
    uint32 i;

    if(nsegs > T_MAX_PHNUM)
    {
        g_img_len = 0u;
        return 0u;
    }
    memcpy(tmp, segs, sizeof(tmp[0]) * (size_t)nsegs);
    segs = tmp;

    memset(g_img, 0, sizeof(g_img));
    g_img[0] = 0x7Fu;
    if(opt->bad_magic == 0u)
    {
        g_img[1] = 'E';
        g_img[2] = 'L';
        g_img[3] = 'F';
    }
    g_img[4] = opt->cls;
    g_img[5] = opt->data;
    g_img[6] = 1u;
    put16(&g_img[16], opt->etype);
    put16(&g_img[18], opt->machine);
    put32(&g_img[20], 1u);
    put32(&g_img[24], opt->entry);
    put32(&g_img[28], phoff);
    put16(&g_img[40], T_ELF_EHDR_SIZE);
    put16(&g_img[42], phentsize);
    put16(&g_img[44], phnum);
    put16(&g_img[46], 0u);
    put16(&g_img[48], 0u);
    put16(&g_img[50], 0u);

    /* 段内容先追加到段表之后；offset_set 的段表示「文件里已经有了」。 */
    for(i = 0u; i < nsegs; i++)
    {
        uint32 n = segs[i].filesz;

        if(segs[i].offset_set != 0u)
        {
            continue;
        }
        if(segs[i].str != 0)
        {
            n = (uint32)strlen(segs[i].str) + 1u;
        }
        off = align4(off);
        if((off + n) > T_IMG_CAP)
        {
            return 0u;
        }
        if(segs[i].str != 0)
        {
            memcpy(&g_img[off], segs[i].str, (size_t)n);
        }
        else if(n != 0u)
        {
            memset(&g_img[off], (int)(segs[i].fill & 0xFFu), (size_t)n);
        }
        /* 回写实际文件偏移与长度，供段表使用（写的是本地副本）。 */
        tmp[i].offset = off;
        tmp[i].filesz = n;
        off += n;
    }

    for(i = 0u; i < nsegs; i++)
    {
        uint8 *ph;

        /* 头里的 phoff 可能故意指到镜像容量之外（用例 8）：此时没有
           字节可落，只保留头里的偏移，让被验代码在短读上返回。 */
        if((phoff + (T_ELF_PHDR_SIZE * (i + 1u))) > T_IMG_CAP)
        {
            break;
        }
        ph = &g_img[phoff + (T_ELF_PHDR_SIZE * i)];

        put32(ph + 0u, segs[i].type);
        put32(ph + 4u, segs[i].offset);
        put32(ph + 8u, segs[i].vaddr);
        put32(ph + 12u, segs[i].vaddr);
        put32(ph + 16u, segs[i].filesz);
        put32(ph + 20u, segs[i].memsz);
        put32(ph + 24u, 5u);
        put32(ph + 28u, 0x1000u);
    }

    if(off < (T_ELF_EHDR_SIZE + (T_ELF_PHDR_SIZE * nsegs)))
    {
        off = T_ELF_EHDR_SIZE + (T_ELF_PHDR_SIZE * nsegs);
    }
    g_img_len = off;
    return off;
}

/* ---------------- 窗口几何 ---------------- */

static unsigned long win_base(void)
{
    return (unsigned long)ARKRTOS_ELF_WINDOW_BASE;
}

static unsigned long win_top(void)
{
    return win_base() + (unsigned long)ARKRTOS_ELF_WINDOW_SIZE;
}

static unsigned long argv_start(void)
{
    return (win_top() - (unsigned long)ARKRTOS_ELF_ARGV_SIZE) & ~(unsigned long)0xF;
}

static unsigned long stack_bottom(void)
{
    return argv_start() - (unsigned long)ARKRTOS_ELF_STACK_SIZE;
}

/* ---------------- 辅助 ---------------- */

static hdr_opt_t ok_hdr(uint32 entry)
{
    hdr_opt_t h;

    memset(&h, 0, sizeof(h));
    h.cls = T_ELF_EI_CLASS32;
    h.data = T_ELF_EI_DATA_LSB;
    h.etype = T_ELF_TYPE_EXEC;
    h.machine = T_ELF_MACHINE_RISCV;
    h.entry = entry;
    h.phoff = T_ELF_EHDR_SIZE;
    h.phentsize = T_ELF_PHDR_SIZE;
    return h;
}

/* 两个 LOAD：第一个覆盖文件头与段表（AT_PHDR 有对应内存），第二个是
 * 8 字节数据 + 16 字节 BSS。 */
static uint32 build_two_loads(uint32 entry)
{
    hdr_opt_t h = ok_hdr(entry);
    seg_def_t segs[2];

    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = T_ELF_EHDR_SIZE + (T_ELF_PHDR_SIZE * 2u);
    segs[0].memsz = segs[0].filesz;
    segs[0].offset = 0u;
    segs[0].offset_set = 1u;
    segs[1].type = T_ELF_PT_LOAD;
    segs[1].vaddr = (uint32)(win_base() + 0x1000u);
    segs[1].filesz = 8u;
    segs[1].memsz = 24u;
    segs[1].fill = 0x5Au;
    return build_elf(&h, segs, 2u);
}

static void release_slot(int32 id)
{
    if((id > 0) && (id <= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        arkrtos_task_table[id - 1].status = ARKRTOS_TASK_INVALID;
    }
}

static void reset_task_stub(void)
{
    g_task_used = 0;
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
}

static void wipe_window(void)
{
    memset(g_win, 0xAA, (size_t)(unsigned long)ARKRTOS_ELF_WINDOW_SIZE);
}

/* 从 sp 开始沿 Linux 的布局找 auxv 键。返回 1 = 找到，0 = 没找到。 */
static int find_aux(unsigned long sp, uint32 type, uint32 *p_val)
{
    uint32 argc = rd32(sp);
    unsigned long p = sp + (4UL * ((unsigned long)argc + 3UL));
    uint32 i;

    for(i = 0u; i < 32u; i++)
    {
        uint32 t = rd32(p);

        if(t == T_AT_NULL)
        {
            return 0;
        }
        if(t == type)
        {
            *p_val = rd32(p + 4UL);
            return 1;
        }
        p += 8UL;
    }
    return 0;
}

/* ---------------- 用例 ---------------- */

static void test_initial_reads(void)
{
    arkrtos_elf_info_t info;

    ck_rc(arkrtos_elf_last_space(), -1, "上电后 last_space 是 -1");
    ck_rc(arkrtos_elf_running(), 0, "上电后没有 ELF 任务");
    ck_rc(arkrtos_elf_running_in(0), 0, "上电后空间 0 是空的");
    ck_rc(arkrtos_elf_running_in(1), 0, "台账长度之外的空间恒为 0");
    ck_rc(arkrtos_elf_space_count(), 1, "不分页档只有 1 个空间");
    ck_rc(arkrtos_elf_info(&info), ARKRTOS_ELF_ERR_PARAM,
          "没有成功装载时 info 明确报错");
    ck_rc(arkrtos_elf_info_space(0u, &info), ARKRTOS_ELF_ERR_PARAM,
          "没有成功装载时 info_space 明确报错");
    ck_rc(arkrtos_elf_info(0), ARKRTOS_ELF_ERR_PARAM, "info(NULL) 是参数错误");
    ck_rc(arkrtos_elf_info_space(0u, 0), ARKRTOS_ELF_ERR_PARAM,
          "info_space(NULL) 是参数错误");
    ck_rc(arkrtos_elf_info_space(9u, &info), ARKRTOS_ELF_ERR_PARAM,
          "不存在的空间号是参数错误");
}

static void test_params(void)
{
    char *argv_null[2];

    build_two_loads((uint32)(win_base() + 0x1000u));

    ck_rc(arkrtos_elf_exec(0, 0, 0), ARKRTOS_ELF_ERR_PARAM, "空路径指针");
    ck_rc(arkrtos_elf_exec("", 0, 0), ARKRTOS_ELF_ERR_PARAM, "空路径串");
    ck_rc(arkrtos_elf_exec("/prog", -1, 0), ARKRTOS_ELF_ERR_PARAM, "argc 为负");
    ck_rc(arkrtos_elf_exec("/prog", (int32)T_MAX_ARGS + 1, 0),
          ARKRTOS_ELF_ERR_PARAM, "argc 超过上限");
    {
        static char s_prog[] = "/prog";
        argv_null[0] = s_prog;
    }
    argv_null[1] = 0;
    ck_rc(arkrtos_elf_exec("/prog", 2, argv_null), ARKRTOS_ELF_ERR_PARAM,
          "argv 里有空元素");
    ck_rc(arkrtos_elf_exec("/nope", 0, 0), ARKRTOS_ELF_ERR_FS,
          "文件不存在是 FS 不是格式错误");
}

static void test_header(void)
{
    hdr_opt_t h;
    seg_def_t segs[1];

    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 16u;
    segs[0].memsz = 16u;
    segs[0].fill = 0x11u;

    h = ok_hdr((uint32)win_base());
    h.bad_magic = 1u;
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FORMAT, "坏魔数");

    h = ok_hdr((uint32)win_base());
    h.cls = T_ELF_EI_CLASS64;
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FORMAT,
          "class 与 port 宽度不符");

    h = ok_hdr((uint32)win_base());
    h.data = 2u;
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FORMAT, "非小端");

    h = ok_hdr((uint32)win_base());
    h.machine = T_ELF_MACHINE_X86;
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_ARCH,
          "机器号不对时单独报 ARCH");

    h = ok_hdr((uint32)win_base());
    h.etype = T_ELF_TYPE_REL;
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FORMAT,
          "ET_REL 没有入口");

    h = ok_hdr((uint32)win_base());
    h.phentsize = 40u;
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FORMAT,
          "段表条目尺寸不对");

    h = ok_hdr((uint32)win_base());
    h.phnum_decl = 17u;
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FORMAT,
          "段表条数超上限");

    h = ok_hdr((uint32)win_base());
    h.phoff = 0x10000u;
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FS,
          "段表偏移在文件之外是短读");

    /* 文件连 ELF 头都不够长。 */
    memset(g_img, 0x7F, sizeof(g_img));
    g_img_len = 20u;
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FS,
          "文件短于 ELF 头");
}

static void test_segments(void)
{
    hdr_opt_t h;
    seg_def_t segs[2];

    /* filesz > memsz */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 32u;
    segs[0].memsz = 16u;
    h = ok_hdr((uint32)win_base());
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_LAYOUT,
          "filesz 大于 memsz");

    /* 段低于窗口 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base() - 0x100u;
    segs[0].filesz = 16u;
    segs[0].memsz = 16u;
    h = ok_hdr((uint32)win_base());
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_LAYOUT,
          "段落在窗口起点之前");

    /* 段越过栈底（=覆盖自己的栈） */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)(stack_bottom() - 0x10u);
    segs[0].filesz = 16u;
    segs[0].memsz = 0x40u;
    h = ok_hdr((uint32)(stack_bottom() - 0x10u));
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_LAYOUT,
          "段越过栈底");

    /* 地址回绕：vaddr + memsz 越过 32 位顶 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = 0xFFFFFFF0u;
    segs[0].filesz = 0u;
    segs[0].memsz = 0x20u;
    h = ok_hdr(0xFFFFFFF0u);
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_LAYOUT,
          "段地址回绕");

    /* 一个 PT_LOAD 都没有 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_NOTE;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 8u;
    segs[0].memsz = 8u;
    h = ok_hdr((uint32)win_base());
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FORMAT,
          "没有 PT_LOAD");

    /* 段数据读不全（文件里没有那些字节） */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 0x40u;
    segs[0].memsz = 0x40u;
    segs[0].offset = 0x2000u;
    segs[0].offset_set = 1u;
    h = ok_hdr((uint32)win_base());
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FS,
          "段数据短读");

    /* 入口判据：越界、不对齐、边界值 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 0x100u;
    segs[0].memsz = 0x100u;

    h = ok_hdr((uint32)win_base() + 2u);
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_LAYOUT,
          "入口未 4 字节对齐");

    h = ok_hdr((uint32)win_base() - 4u);
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_LAYOUT,
          "入口在窗口之前");

    h = ok_hdr((uint32)stack_bottom());
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_LAYOUT,
          "入口落在栈底之外");

    h = ok_hdr((uint32)(stack_bottom() - 2u));
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_LAYOUT,
          "入口贴着栈底但没对齐");

    h = ok_hdr((uint32)(stack_bottom() - 4u));
    build_elf(&h, segs, 1u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_OK,
          "入口恰好在 stack_bottom-4 是合法边界");
    reset_task_stub();
}

static void test_interp(void)
{
    hdr_opt_t h;
    seg_def_t segs[2];
    uint32 n;

    /* 正常路径：没有解释器文件、但段表里有 PT_INTERP —— 关档明确拒绝。 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 0x100u;
    segs[0].memsz = 0x100u;
    segs[1].type = T_ELF_PT_INTERP;
    segs[1].str = "/lib/ld.so";
    h = ok_hdr((uint32)win_base());
    build_elf(&h, segs, 2u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FORMAT,
          "关掉动态档时 PT_INTERP 按格式错误拒绝");

    /* 解释器路径长度不合法（<2 字节）。 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 0x100u;
    segs[0].memsz = 0x100u;
    segs[1].type = T_ELF_PT_INTERP;
    segs[1].filesz = 1u;
    segs[1].memsz = 1u;
    h = ok_hdr((uint32)win_base());
    build_elf(&h, segs, 2u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_INTERP,
          "PT_INTERP 路径过短");

    /* 解释器路径超出内部缓冲（64）。 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 0x100u;
    segs[0].memsz = 0x100u;
    segs[1].type = T_ELF_PT_INTERP;
    segs[1].filesz = 100u;
    segs[1].memsz = 100u;
    h = ok_hdr((uint32)win_base());
    build_elf(&h, segs, 2u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_INTERP,
          "PT_INTERP 路径超长");

    /* 解释器路径读不出来。 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 0x100u;
    segs[0].memsz = 0x100u;
    segs[1].type = T_ELF_PT_INTERP;
    segs[1].filesz = 8u;
    segs[1].memsz = 8u;
    segs[1].offset = 0x2000u;
    segs[1].offset_set = 1u;
    h = ok_hdr((uint32)win_base());
    build_elf(&h, segs, 2u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FS,
          "PT_INTERP 内容短读");

    /* ET_DYN + PT_INTERP：同样是格式错误（关档）。 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 0x100u;
    segs[0].memsz = 0x100u;
    segs[1].type = T_ELF_PT_INTERP;
    segs[1].str = "/lib/ld.so";
    h = ok_hdr((uint32)win_base());
    h.etype = T_ELF_TYPE_DYN;
    build_elf(&h, segs, 2u);
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_FORMAT,
          "ET_DYN + PT_INTERP 在关档时拒绝");

    /* ET_DYN 但没有解释器：静态档照装（vaddr 被当成绝对地址）。 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 0x100u;
    segs[0].memsz = 0x100u;
    h = ok_hdr((uint32)win_base());
    h.etype = T_ELF_TYPE_DYN;
    n = build_elf(&h, segs, 1u);
    ck(n > 0u, "ET_DYN 镜像造得出来");
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_OK,
          "没有 PT_INTERP 的 ET_DYN 走静态路径");
    release_slot(1);
    reset_task_stub();
}

static void test_load_and_stack(void)
{
    arkrtos_elf_info_t info;
    unsigned long sp;
    unsigned long a0;
    uint32 argv_null_end;
    uint32 val;
    uint32 i;
    int seen_random;

    reset_task_stub();
    wipe_window();
    build_two_loads((uint32)(win_base() + 0x1000u));

    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_OK, "正常装载");

    /* 段内容：第一个 LOAD 是文件头 + 段表，第二个 8 字节数据 + 16 字节 BSS。 */
    ck(rd32(win_base()) == 0x464C457Fu, "窗口起点是 ELF 魔数");
    ck(rd32(win_base() + 0x1000u) == 0x5A5A5A5Au, "数据段按文件内容落位");
    ck(rd32(win_base() + 0x1008u) == 0u, "BSS 区被清零");
    ck(rd32(win_base() + 0x1014u) == 0u, "BSS 末尾也是 0");
    ck(rd32(win_base() + 0x1018u) == 0xAAAAAAAAu, "BSS 之外一个字节不动");

    ck_rc(arkrtos_elf_info(&info), ARKRTOS_ELF_OK, "装载后 info 可读");
    ck_u(info.entry, win_base() + 0x1000u, "info.entry 是 e_entry");
    ck_u(info.bytes, (T_ELF_EHDR_SIZE + (T_ELF_PHDR_SIZE * 2u)) + 8u,
         "info.bytes 是 filesz 之和");
    ck_u(info.segs, 2u, "info.segs 是 PT_LOAD 个数");
    ck_u(info.argc, 1u, "argc=0 时按 Linux 习惯补 argv[0]=path");
    ck_u(info.sp, argv_start(), "sp 是 argv 区起点");
    ck_u(info.stack_size, (unsigned long)ARKRTOS_ELF_STACK_SIZE,
         "分给任务的栈是 ARKRTOS_ELF_STACK_SIZE");
    ck(info.task > 0, "info.task 是注册出来的任务号");
    ck_u(info.dynamic, 0u, "静态档 dynamic=0");
    ck_u(info.interp_base, 0u, "静态档没有解释器基址");
    ck_u(info.interp_entry, 0u, "静态档没有解释器入口");

    /* 任务对象：降权 + 窗口写回 + MPU 重建。 */
    {
        arkrtos_task_t *p = &arkrtos_task_table[info.task - 1];

        ck_u(p->status, ARKRTOS_TASK_READY, "任务已就绪");
        ck_u(p->is_priv, 0u, "ELF 任务被降权");
        ck_u(p->rom_start, win_base(), "代码窗口写回 TCB");
        ck_u(p->rom_size, (unsigned long)ARKRTOS_ELF_WINDOW_SIZE,
             "代码窗口大小写回 TCB");
        ck_u(p->ram_start, win_base(), "数据窗口写回 TCB");
        ck_u(p->ram_size, (unsigned long)ARKRTOS_ELF_WINDOW_SIZE,
             "数据窗口大小写回 TCB");
        ck_u((unsigned long)(uintptr_t)p->stack_bottom, stack_bottom(),
             "栈底是窗口内的 stack_bottom");
        ck_u(p->stack_size, (unsigned long)ARKRTOS_ELF_STACK_SIZE, "栈大小");
        ck_u(p->priority, 12u, "ELF 任务优先级");
        ck(g_crit_depth == 0u, "注册/降权/写回整段在临界区里收口");
#if (ARKRTOS_USE_MPU == 1)
        ck(g_mpu_calls > 0, "MPU 上下文被重建");
        ck_u(g_mpu_rom, win_base(), "MPU 拿到的是写回后的代码窗口");
        ck_u(g_mpu_ram, win_base(), "MPU 拿到的是写回后的数据窗口");
#endif
    }

    /* 台账。 */
    ck_rc(arkrtos_elf_running(), 1, "有任务在跑");
    ck_rc(arkrtos_elf_running_in(0), 1, "空间 0 有活实例");
    ck_rc(arkrtos_elf_last_space(), 0, "最近一次落在空间 0");
    ck_rc(arkrtos_elf_info_space(0u, &info), ARKRTOS_ELF_OK, "按空间读读数");

    /* 用户栈：sp -> argc, argv[], NULL, NULL, auxv..., AT_NULL */
    sp = info.sp;
    ck_u(rd32(sp), 1u, "sp 指向 argc");
    a0 = rd32(sp + 4UL);
    ck(a0 >= sp, "argv[0] 指针在参数区内");
    ck(a0 < win_top(), "argv[0] 指针在窗口内");
    ck_u(a0 & 7u, 0u, "argv 字符串 8 字节对齐");
    ck(strcmp((const char *)(uintptr_t)a0, "/prog") == 0,
       "argc=0 时 argv[0] 就是路径");
    argv_null_end = rd32(sp + 8UL);
    ck_u(argv_null_end, 0u, "argv 以 NULL 结束");
    ck_u(rd32(sp + 12UL), 0u, "envp 是空表");

    /* auxv：至少要有 PAGESZ / ENTRY / RANDOM / EXECFN / CLKTCK。
     * AT_RANDOM 必须真的指向一段 16 字节、不是全零的区（glibc 会立刻解引用）。 */
    ck(find_aux(sp, T_AT_PAGESZ, &val) == 1, "auxv 有 AT_PAGESZ");
    ck_u(val, 4096u, "AT_PAGESZ=4096");
    ck(find_aux(sp, T_AT_ENTRY, &val) == 1, "auxv 有 AT_ENTRY");
    ck_u(val, win_base() + 0x1000u, "AT_ENTRY 是入口");
    ck(find_aux(sp, T_AT_PHDR, &val) == 1, "段表落在 LOAD 里时给出 AT_PHDR");
    ck_u(val, win_base() + T_ELF_EHDR_SIZE, "AT_PHDR 指向段表");
    ck(find_aux(sp, T_AT_PHENT, &val) == 1, "auxv 有 AT_PHENT");
    ck_u(val, T_ELF_PHDR_SIZE, "AT_PHENT=32");
    ck(find_aux(sp, T_AT_PHNUM, &val) == 1, "auxv 有 AT_PHNUM");
    ck_u(val, 2u, "AT_PHNUM=2");
    ck(find_aux(sp, T_AT_CLKTCK, &val) == 1, "auxv 有 AT_CLKTCK");
    ck_u(val, 100u, "AT_CLKTCK=100");
    ck(find_aux(sp, T_AT_EXECFN, &val) == 1, "auxv 有 AT_EXECFN");
    ck_u(val, a0, "AT_EXECFN 指向 argv[0]");
    seen_random = find_aux(sp, T_AT_RANDOM, &val);
    ck(seen_random == 1, "auxv 有 AT_RANDOM");
    ck_u((unsigned long)val & 0xFUL, 0u, "AT_RANDOM 16 字节对齐");
    ck(val >= sp, "AT_RANDOM 在参数区里");
    ck((unsigned long)val + 16UL <= win_top(), "AT_RANDOM 不越窗口顶");
    {
        const uint8 *r = (const uint8 *)(uintptr_t)val;
        uint32 nz = 0u;

        for(i = 0u; i < 16u; i++)
        {
            nz += (r[i] != 0u) ? 1u : 0u;
        }
        ck(nz > 0u, "AT_RANDOM 不是全零（否则 canary 退化成常量）");
    }
}

static void test_second_exec_and_busy(void)
{
    char *argv[3];
    arkrtos_elf_info_t info;
    unsigned long sp;
    unsigned long a0;
    unsigned long a1;
    unsigned long a2;
    int32 first;

    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_BUSY,
          "上一个还在跑时窗口是单实例");

    ck_rc(arkrtos_elf_info(&info), ARKRTOS_ELF_OK, "读数仍是上一次的");
    first = info.task;
    release_slot(first);
    ck_rc(arkrtos_elf_running(), 0, "任务死后懒清理，台账回到空");

    {
        static char s0[] = "prog";
        static char s1[] = "alpha";
        static char s2[] = "a-longer-argument";

        argv[0] = s0;
        argv[1] = s1;
        argv[2] = s2;
    }
    wipe_window();
    build_two_loads((uint32)(win_base() + 0x1000u));
    ck_rc(arkrtos_elf_exec("/prog", 3, argv), ARKRTOS_ELF_OK,
          "空间空出来之后可以再装一次");
    ck_rc(arkrtos_elf_info(&info), ARKRTOS_ELF_OK, "第二次装载的读数");
    ck_u(info.argc, 3u, "argc 原样透出");

    sp = info.sp;
    ck_u(rd32(sp), 3u, "sp 指向 argc=3");
    a0 = rd32(sp + 4UL);
    a1 = rd32(sp + 8UL);
    a2 = rd32(sp + 12UL);
    ck(strcmp((const char *)(uintptr_t)a0, "prog") == 0, "argv[0]");
    ck(strcmp((const char *)(uintptr_t)a1, "alpha") == 0, "argv[1]");
    ck(strcmp((const char *)(uintptr_t)a2, "a-longer-argument") == 0, "argv[2]");
    ck(a1 < a0, "字符串从高到低排：argv[1] 比 argv[0] 低");
    ck(a2 < a1, "第三个字符串更低");
    ck_u(a0 & 7u, 0u, "argv[0] 8 字节对齐");
    ck_u(a1 & 7u, 0u, "argv[1] 8 字节对齐");
    ck_u(a2 & 7u, 0u, "argv[2] 8 字节对齐");
    ck_u(rd32(sp + 16UL), 0u, "argv 结束");
    ck_u(rd32(sp + 20UL), 0u, "envp 结束");
    ck(rd32(sp + 24UL) != 0u, "auxv 从 envp 之后开始");
    release_slot(info.task);
    reset_task_stub();
}

static void test_bss_only_and_no_phdr(void)
{
    hdr_opt_t h;
    seg_def_t segs[1];
    arkrtos_elf_info_t info;
    uint32 val;

    /* 一个 filesz=0 的 LOAD：整段都是 BSS；而且段表不在任何文件的
     * file range 里 ⇒ 不能给 AT_PHDR（给了就是一个指向别人的地址）。 */
    memset(segs, 0, sizeof(segs));
    segs[0].type = T_ELF_PT_LOAD;
    segs[0].vaddr = (uint32)win_base();
    segs[0].filesz = 0u;
    segs[0].memsz = 0x20u;
    segs[0].offset = 0x200u;
    segs[0].offset_set = 1u;
    h = ok_hdr((uint32)win_base());
    build_elf(&h, segs, 1u);

    wipe_window();
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_OK, "纯 BSS 段可装载");
    ck_rc(arkrtos_elf_info(&info), ARKRTOS_ELF_OK, "读数可读");
    ck_u(info.bytes, 0u, "没有从文件读进任何字节");
    ck_u(info.segs, 1u, "一个 LOAD 也算一段");
    ck_u(rd32(win_base()), 0u, "BSS 首字被清零");
    ck_u(rd32(win_base() + 0x1Cu), 0u, "BSS 末字被清零");
    ck_u(rd32(win_base() + 0x20u), 0xAAAAAAAAu, "BSS 之后不动");
    ck(find_aux(info.sp, T_AT_PHDR, &val) == 0,
       "段表不在 LOAD 的 file range 里时不给 AT_PHDR");
    ck(find_aux(info.sp, T_AT_ENTRY, &val) == 1, "AT_ENTRY 仍然给");
    release_slot(info.task);
    reset_task_stub();
}

static void test_max_args(void)
{
    char *argv[T_MAX_ARGS];
    char names[T_MAX_ARGS][8];
    arkrtos_elf_info_t info;
    unsigned long sp;
    uint32 i;

    for(i = 0u; i < T_MAX_ARGS; i++)
    {
        snprintf(names[i], sizeof(names[i]), "a%u", (unsigned)i);
        argv[i] = names[i];
    }
    wipe_window();
    build_two_loads((uint32)(win_base() + 0x1000u));
    ck_rc(arkrtos_elf_exec("/prog", (int32)T_MAX_ARGS, argv), ARKRTOS_ELF_OK,
          "argc 恰好到上限可装");
    ck_rc(arkrtos_elf_info(&info), ARKRTOS_ELF_OK, "读数可读");
    ck_u(info.argc, T_MAX_ARGS, "argc 是上限值");
    sp = info.sp;
    ck_u(rd32(sp), (unsigned long)T_MAX_ARGS, "sp 指向的 argc 是 16");
    for(i = 0u; i < T_MAX_ARGS; i++)
    {
        unsigned long p = rd32(sp + 4UL + (4UL * (unsigned long)i));

        ck(strcmp((const char *)(uintptr_t)p, names[i]) == 0, "每个 argv 都对");
    }
    release_slot(info.task);
    reset_task_stub();
}

static void test_long_arg_and_nomem(void)
{
    char big[T_MAX_ARG_LEN + 8];
    char *argv[1];
    arkrtos_elf_info_t info;

    memset(big, 'x', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';
    argv[0] = big;

    wipe_window();
    build_two_loads((uint32)(win_base() + 0x1000u));
    ck_rc(arkrtos_elf_exec("/prog", 1, argv), ARKRTOS_ELF_ERR_PARAM,
          "单个参数超过 MAX_ARG_LEN 是参数错误");
    ck_rc(arkrtos_elf_running(), 0, "失败的装载不登记台账");

    /* 任务表满：镜像已经写进窗口，但没有任务可注册。 */
    g_task_max = 0;
    ck_rc(arkrtos_elf_exec("/prog", 0, 0), ARKRTOS_ELF_ERR_NOMEM,
          "任务表满时报 NOMEM");
    ck_rc(arkrtos_elf_running(), 0, "NOMEM 也不登记台账");
    g_task_max = 4;
    reset_task_stub();
    ck_rc(arkrtos_elf_info(&info), ARKRTOS_ELF_OK,
          "失败不清掉上一次成功装载的读数");
}

static void test_exec_replace(void)
{
    arkrtos_elf_info_t info;
    unsigned long tp = 0xDEADBEEFUL;

    /* 参数检查在动任何字节之前。 */
    ck_rc(arkrtos_elf_exec_replace("/prog", 0, 0, 0, &tp), ARKRTOS_ELF_ERR_PARAM,
          "exec_replace 要求 info 出参");

    arkrtos_current_task_id = 0;
    ck_rc(arkrtos_elf_exec_replace("/prog", 0, 0, &info, &tp),
          ARKRTOS_ELF_ERR_PARAM, "没有当前任务时拒绝");

    arkrtos_current_task_id = 7;
    ck_rc(arkrtos_elf_exec_replace("/prog", 0, 0, &info, 0), ARKRTOS_ELF_ERR_PARAM,
          "exec_replace 要求 tp 出参");

    wipe_window();
    build_two_loads((uint32)(win_base() + 0x1000u));
    /* exec_replace 的调用者本来就是活任务：TCB 状态必须先是 READY，
       否则台账的懒清理会把它当成已退出（真实路径上这一位由调度器维护）。 */
    arkrtos_task_table[7 - 1].status = ARKRTOS_TASK_READY;
    ck_rc(arkrtos_elf_exec_replace("/prog", 0, 0, &info, &tp), ARKRTOS_ELF_OK,
          "execve 路径装载成功");
    ck_rc(info.task, 7, "任务号沿用调用者，不起新任务");
    ck_u(tp, 0u, "静态档没有预置 TLS 块");
    ck_rc(arkrtos_elf_running_in(0), 1, "台账记的是这个任务");
    ck(g_task_used == 0, "exec_replace 一个任务都没注册");
    release_slot(7);
    arkrtos_current_task_id = 0;
    reset_task_stub();
}

static void test_error_names(void)
{
    int32 codes[10];
    uint32 i;
    uint32 j;

    codes[0] = ARKRTOS_ELF_OK;
    codes[1] = ARKRTOS_ELF_ERR_PARAM;
    codes[2] = ARKRTOS_ELF_ERR_FS;
    codes[3] = ARKRTOS_ELF_ERR_FORMAT;
    codes[4] = ARKRTOS_ELF_ERR_ARCH;
    codes[5] = ARKRTOS_ELF_ERR_LAYOUT;
    codes[6] = ARKRTOS_ELF_ERR_NOMEM;
    codes[7] = ARKRTOS_ELF_ERR_BUSY;
    codes[8] = ARKRTOS_ELF_ERR_SIGN;
    codes[9] = ARKRTOS_ELF_ERR_INTERP;

    ck(strcmp(arkrtos_elf_error_name(codes[0]), "ok") == 0, "OK 的短名");
    for(i = 1u; i < 10u; i++)
    {
        const char *name = arkrtos_elf_error_name(codes[i]);

        ck(name != 0, "每个错误码都有名字");
        ck(strcmp(name, "unknown") != 0, "已定义的错误码不落到 unknown");
        for(j = 1u; j < i; j++)
        {
            if(strcmp(name, arkrtos_elf_error_name(codes[j])) == 0)
            {
                ck(0, "不同错误码的名字不能重合");
            }
        }
    }
    ck(strcmp(arkrtos_elf_error_name(-99), "unknown") == 0, "未知码落到 unknown");
}

int main(void)
{
    if(map_window(win_base(), (unsigned long)ARKRTOS_ELF_WINDOW_SIZE) != 0)
    {
        printf("ELF-HOST FAIL cannot map window at 0x%lx\n", win_base());
        return 1;
    }
    g_win = (void *)(uintptr_t)win_base();

    test_initial_reads();
    test_params();
    test_header();
    test_segments();
    test_interp();
    test_load_and_stack();
    test_second_exec_and_busy();
    test_bss_only_and_no_phdr();
    test_max_args();
    test_long_arg_and_nomem();
    test_exec_replace();
    test_error_names();

    ck_rc(arkrtos_elf_running(), 0, "收尾时没有遗留的活实例");
    ck(g_crit_depth == 0u, "收尾时临界区深度回到 0");

    if(g_fails != 0)
    {
        printf("ELF-HOST FAIL %d checks, %d failed\n", g_checks, g_fails);
        return 1;
    }
    printf("ELF-HOST OK %d checks, 0 failed\n", g_checks);
    return 0;
}

#endif /* ARKRTOS_USE_ELF */
