/**
* @file arkrtos_modld.c
* @brief ET_REL 目标文件在板上现场链接（节放置 + GOT + SOP 栈机重定位）
* @details 这条路径与 arkrtos_elf.c 的区别不在「读文件」，而在**谁决定地址**：
*          ET_EXEC 的地址是链接期写死的，ET_REL 的地址由装载器现在给，
*          于是所有 PC 相对与 GOT 相对的重定位都得在这里现算。
*
*          四步，顺序不能改：
*            1. 放置：按节头表把 SHF_ALLOC 的节依次摆进槽（各自对齐），
*               得到每一节的装载地址；
*            2. 建 GOT：GCC 8.3 在 -fno-pic 下访问**外部**数据仍走 GOT
*               （`pcaddu12i; ld.d $r12,$r12,slot; ld.w`），所以每个
*               被 GPREL 引用的外部符号要一个 8 字节槽，且
*               `gp = got_base + 0x800` —— 这个 0x800 不是可调参数：
*               汇编器把外部数据写成 `PCREL(_GLOBAL_OFFSET_TABLE_ + 0x800)
*               + GPREL(sym)`，两项相加后 gp 必须抵消成 0，否则
*               算出来的 hi20 会整体偏掉一页（实测差 4 MB 级）；
*            3. 复制 + 清零 BSS；
*            4. 重定位：按节顺序应用 .rela.*，SOP 用栈机求值。
*
*          为什么不支持更多重定位类型：GCC 8.3 的 LoongArch 后端只发 SOP
*          形式（`-mexplicit-relocs` 在这个版本上还不认），实测 .o 里出现的
*          类型只有 22/23/25/29/32/33/34/35/40/43/45 + .rodata 的 64 位。
*          不在支持表里的类型直接报错并点出类型号——猜一个编码会得到
*          「能跑但算错」的模块，那比装载失败更难查。
*
* @note 开关：ARKRTOS_USE_MODLOAD。关闭时本文件是空目标（零体积零符号）。
* @author xw
*/

#include "arkrtos_config.h"

#if (ARKRTOS_USE_MODLOAD == 1)

#include "arkrtos_modld.h"
#include "arkrtos_export.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_vfs.h"
#include "arkrtos_log.h"
#if (ARKRTOS_USE_EXEC_SIGN == 1)
#include "arkrtos_sign.h"
#endif

/* ---------------------------------------------------------------- 编译期 */

/* 模块窗口的地址**不**由宏决定，而是由链接器给 s_mod_pool 的那个地址决定：
 * 早期版本把 ARKRTOS_MOD_AREA_BASE（0x600000）当窗口地址，而模块实际上被拷
 * 进 .bss 里的 s_mod_pool（另在一个地址），于是任务跳进 0x600000 抓到的
 * 是一条全 0 指令 —— INE 致命异常，看不出和装载器有关。现在只用池子自己的
 * 地址；池子落在哪个可执行区由链接脚本决定（QEMU 目标：内核低 16 MB 恒等
 * 映射是 PLV0 RWX，所以 .bss 里的池子可以直接执行）。 */
#ifndef ARKRTOS_MOD_AREA_SIZE
#error "ARKRTOS_USE_MODLOAD=1 需要板级给出 ARKRTOS_MOD_AREA_SIZE（这块区有多大）"
#endif
#ifndef ARKRTOS_MOD_SLOTS
#define ARKRTOS_MOD_SLOTS           (4)         /* 同时驻留几个模块 */
#endif
#ifndef ARKRTOS_MOD_SLOT_SIZE
#define ARKRTOS_MOD_SLOT_SIZE       (16384u)    /* 每个模块最多多大 */
#endif
#ifndef ARKRTOS_MOD_TASK_STACK_WORDS
#define ARKRTOS_MOD_TASK_STACK_WORDS (1024u)    /* 模块任务栈（字） */
#endif
#ifndef ARKRTOS_MOD_TASK_PRIORITY
#define ARKRTOS_MOD_TASK_PRIORITY   (12u)
#endif
#ifndef ARKRTOS_MOD_ENTRY
#define ARKRTOS_MOD_ENTRY           "module_main"
#endif
#ifndef ARKRTOS_MOD_PRINT_MAX
#define ARKRTOS_MOD_PRINT_MAX       (4)         /* 同时能跑几个模块任务（打印缓冲按它分） */
#endif

#if (ARKRTOS_MOD_SLOTS * ARKRTOS_MOD_SLOT_SIZE) > ARKRTOS_MOD_AREA_SIZE
#error "ARKRTOS_MOD_SLOTS * ARKRTOS_MOD_SLOT_SIZE 超过 ARKRTOS_MOD_AREA_SIZE"
#endif

/* 本文件只服务 64 位 port：ELF64 的字段宽度无法在 32 位 long 上表达。 */
#if !(defined(__LP64__) || defined(_LP64) || \
      (defined(__SIZEOF_LONG__) && (__SIZEOF_LONG__ > 4)))
#error "arkrtos_modld.c 只支持 64 位 port"
#endif

/* ELF64 常量。只用得到的这几个，不引 <elf.h>。 */
#define MOD_ET_REL                  (1u)
#define MOD_EM_LOONGARCH            (258u)
#define MOD_SHT_PROGBITS            (1u)
#define MOD_SHT_SYMTAB              (2u)
#define MOD_SHT_STRTAB              (3u)
#define MOD_SHT_RELA                (4u)
#define MOD_SHT_NOBITS              (8u)
#define MOD_SHF_ALLOC               (0x2u)
#define MOD_SHN_UNDEF               (0u)
#define MOD_SHN_ABS                 (0xFFF1u)

#define MOD_EHDR_SIZE               (64u)
#define MOD_SHDR_SIZE               (64u)
#define MOD_SYM_SIZE                (24u)
#define MOD_RELA_SIZE               (24u)
#define MOD_MAX_SEC                 (32u)    /* 一个 .o 的节数上限（实测 13） */
#define MOD_MAX_GOT                 (24u)    /* 外部数据符号上限 */
#define MOD_MAX_ARGS                (8u)
#define MOD_ARG_LEN                 (64u)
#define MOD_NAME_LEN                (24u)

/* 重定位类型（LoongArch psABI） */
#define R_LA_32                     (1u)
#define R_LA_64                     (2u)
#define R_LA_SOP_PUSH_PCREL         (22u)
#define R_LA_SOP_PUSH_ABSOLUTE      (23u)
#define R_LA_SOP_PUSH_GPREL         (25u)
#define R_LA_SOP_PUSH_PLT_PCREL     (29u)
#define R_LA_SOP_ASSERT             (30u)
#define R_LA_SOP_NOT                (31u)
#define R_LA_SOP_SUB                (32u)
#define R_LA_SOP_SL                 (33u)
#define R_LA_SOP_SR                 (34u)
#define R_LA_SOP_ADD                (35u)
#define R_LA_SOP_POP_32_S_10_12     (40u)
#define R_LA_SOP_POP_32_S_10_16_S2  (42u)
#define R_LA_SOP_POP_32_S_5_20      (43u)
#define R_LA_SOP_POP_32_S_0_10_10_16_S2 (45u)
#define R_LA_SOP_POP_32_U           (46u)

/* ---------------------------------------------------------------- 小工具 */

static uint16 m_rd16(const uint8 *p)
{
    return (uint16)(((uint16)p[0]) | ((uint16)p[1] << 8));
}

static uint32 m_rd32(const uint8 *p)
{
    return ((uint32)p[0]) | ((uint32)p[1] << 8)
         | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static unsigned long m_rd64(const uint8 *p)
{
    unsigned long v = (unsigned long)m_rd32(p + 4);

    v = (v << 32) | (unsigned long)m_rd32(p);
    return v;
}

static void m_wr32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
    p[2] = (uint8)((v >> 16) & 0xFFu);
    p[3] = (uint8)((v >> 24) & 0xFFu);
}

static uint32 m_strlen(const char *s)
{
    uint32 n = 0u;

    while(s[n] != '\0')
    {
        n++;
    }
    return n;
}

static int32 m_streq(const char *a, const char *b)
{
    uint32 i = 0u;

    while((a[i] != '\0') && (a[i] == b[i]))
    {
        i++;
    }
    return (a[i] == b[i]) ? 0 : -1;
}

static void m_strcpy_n(char *dst, uint32 cap, const char *src)
{
    uint32 i = 0u;

    while((src[i] != '\0') && ((i + 1u) < cap))
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ---------------------------------------------------------------- 上下文 */

typedef struct {
    uint32 sh_name;
    uint32 sh_type;
    unsigned long sh_flags;
    unsigned long sh_offset;
    unsigned long sh_size;
    uint32 sh_link;
    uint32 sh_info;
    unsigned long sh_addralign;
} mod_shdr_t;

typedef struct {
    const uint8 *file;
    uint32       file_len;

    uint8       *img;            /* 目标槽（可写） */
    uint32       base;
    uint32       size;
    uint32       cur;            /* 已用到的最高地址（绝对地址） */

    uint32       shoff;
    uint32       shnum;
    uint32       shentsize;
    uint32       symtab;         /* .symtab 的节下标；0xFFFFFFFF = 没有 */
    uint32       strtab;

    uint32       sec_addr[MOD_MAX_SEC];

    uint32       got_base;
    uint32       gp;
    uint32       got_sym[MOD_MAX_GOT];   /* GOT 槽 → 符号表下标 */
    uint32       got_addr[MOD_MAX_GOT];  /* GOT 槽 → 槽地址 */
    uint32       got_n;

    uint32       relocs;
    uint32       imports;
    uint32       text_bytes;

    char         last_sym[MOD_NAME_LEN]; /* 最近一个没解析到的外部符号名 */
    uint32       last_reloc;             /* 最近一个不认识的重定位类型号 */
} mod_link_t;

typedef struct {
    int32  used;
    char   name[MOD_NAME_LEN];
    uint32 base;
    uint32 size;
    uint32 entry;
    uint32 text_bytes;
    uint32 relocs;
    uint32 imports;
    int32  task;
    uint32 argc;                 /* 起跑时传给 module_main 的参数个数 */
} mod_slot_t;

static uint8 s_mod_pool[ARKRTOS_MOD_AREA_SIZE] __attribute__((aligned(16)));
static uint8 s_mod_file[ARKRTOS_MOD_SLOT_SIZE] __attribute__((aligned(16)));
static uint32 s_mod_stack[ARKRTOS_MOD_SLOTS][ARKRTOS_MOD_TASK_STACK_WORDS];
/* 两份分开存：s_mod_argv 是**字符串本体**（inline 字符数组），s_mod_argp 才是
 * 交给 module_main 的那个 char* 向量。早期版本把前者强转成 char** 交出去，
 * 于是 argv[0] 变成了字符串前 8 个字节被当成指针用（模块里 argv[i] 一解引用
 * 就跳进 .rodata，报成一堆看不懂的 ADEF / 系统调用异常）。 */
static char  s_mod_argv[ARKRTOS_MOD_SLOTS][MOD_MAX_ARGS][MOD_ARG_LEN];
static char *s_mod_argp[ARKRTOS_MOD_SLOTS][MOD_MAX_ARGS + 1];
static mod_slot_t s_slots[ARKRTOS_MOD_SLOTS];

/* 最近一次装载失败的细节：让 shell 能说清「缺哪个符号 / 哪个重定位类型」，
 * 而不是只丢一个 -6。一次装载开始时清空。 */
static char   s_last_missing[MOD_NAME_LEN];
static uint32 s_last_bad_reloc;

/* ---------------------------------------------------------------- 节/符号 */

static void mod_shdr(const mod_link_t *c, uint32 i, mod_shdr_t *o)
{
    const uint8 *p = c->file + c->shoff + (i * c->shentsize);

    o->sh_name       = m_rd32(p + 0);
    o->sh_type       = m_rd32(p + 4);
    o->sh_flags      = m_rd64(p + 8);
    o->sh_offset     = m_rd64(p + 24);
    o->sh_size       = m_rd64(p + 32);
    o->sh_link       = m_rd32(p + 40);
    o->sh_info       = m_rd32(p + 44);
    o->sh_addralign  = m_rd64(p + 48);
}

/** @brief 取一个符号的 (名字, 节下标, 值)；符号表坏了返回 -1 */
static int32 mod_sym(const mod_link_t *c, uint32 idx,
                     const char **p_name, uint32 *p_shndx,
                     unsigned long *p_value)
{
    const uint8 *sym;
    uint32 name_off;

    if((c->symtab == 0xFFFFFFFFu) || (c->strtab == 0xFFFFFFFFu))
    {
        return (int32)-1;
    }
    {
        mod_shdr_t st;
        mod_shdr_t str;

        mod_shdr(c, c->symtab, &st);
        mod_shdr(c, c->strtab, &str);
        if(((idx + 1u) * MOD_SYM_SIZE) > (uint32)st.sh_size)
        {
            return (int32)-1;
        }
        sym = c->file + st.sh_offset + (idx * MOD_SYM_SIZE);
        name_off = m_rd32(sym + 0);
        if(name_off >= (uint32)str.sh_size)
        {
            return (int32)-1;
        }
        if(p_name != 0)
        {
            *p_name = (const char *)(c->file + str.sh_offset + name_off);
        }
    }
    if(p_shndx != 0)
    {
        *p_shndx = (uint32)m_rd16(sym + 6);
    }
    if(p_value != 0)
    {
        *p_value = m_rd64(sym + 8);
    }
    return 0;
}

/**
* @brief 求一个符号的最终地址
* @details 三种情况分开算，不能合并：
*            - 空名字符号 0：值是 0（汇编器用它发 PUSH_ABSOLUTE 常量）；
*            - 定义在本模块里的符号：所在节的装载地址 + st_value；
*            - 未定义符号：查内核导出表；_GLOBAL_OFFSET_TABLE_ 指向本模块的 GOT 基址。
*/
static int32 mod_sym_value(mod_link_t *c, uint32 idx, unsigned long *p_out)
{
    const char *name = 0;
    uint32 shndx = 0u;
    unsigned long value = 0ul;

    if(mod_sym(c, idx, &name, &shndx, &value) != 0)
    {
        return ARKRTOS_MOD_ERR_FORMAT;
    }
    if((name != 0) && (name[0] == '\0'))
    {
        *p_out = 0ul;
        return ARKRTOS_MOD_OK;
    }
    if(shndx == MOD_SHN_UNDEF)
    {
        if(name[0] == '_')
        {
            /* 汇编器对 PC 相对外部数据发的是 PCREL(_GLOBAL_OFFSET_TABLE_+0x800)
             * 与 GPREL(sym) 两条，两项相加时 gp 必须抵消，所以这里回 GOT 基址，
             * 而不是 0。回 0 会让每一条外部数据访问整体偏掉一个 GOT 的距离。 */
            if(m_streq(name, "_GLOBAL_OFFSET_TABLE_") == 0)
            {
                *p_out = (unsigned long)c->got_base;
                return ARKRTOS_MOD_OK;
            }
        }
        {
            unsigned long addr = 0ul;

            if(arkrtos_export_lookup(name, &addr) != 0)
            {
                m_strcpy_n(c->last_sym, MOD_NAME_LEN, name);
                return ARKRTOS_MOD_ERR_SYMBOL;
            }
            c->imports++;
            *p_out = addr;
        }
        return ARKRTOS_MOD_OK;
    }
    if(shndx == MOD_SHN_ABS)
    {
        *p_out = value;
        return ARKRTOS_MOD_OK;
    }
    if(shndx >= c->shnum)
    {
        return ARKRTOS_MOD_ERR_FORMAT;
    }
    *p_out = (unsigned long)c->sec_addr[shndx] + value;
    return ARKRTOS_MOD_OK;
}

/* ---------------------------------------------------------------- 放置 */

static uint32 mod_align_up(uint32 v, uint32 a)
{
    if(a <= 1u)
    {
        return v;
    }
    return (v + (a - 1u)) / a * a;
}

static int32 mod_place(mod_link_t *c)
{
    uint32 i;

    c->cur = c->base;
    for(i = 0u; i < c->shnum; i++)
    {
        mod_shdr_t s;
        uint32 a;

        mod_shdr(c, i, &s);
        if((s.sh_flags & (unsigned long)MOD_SHF_ALLOC) == 0ul)
        {
            c->sec_addr[i] = 0u;      /* 不装载的节：没有地址，也不会被引用 */
            continue;
        }
        a = (uint32)((s.sh_addralign != 0ul) ? s.sh_addralign : 1ul);
        /* 目标文件不会要求超过 16 的对齐；给个上界，免得一个坏值算出越界地址。 */
        if(a > 4096u)
        {
            return ARKRTOS_MOD_ERR_LAYOUT;
        }
        c->cur = mod_align_up(c->cur, a);
        c->sec_addr[i] = c->cur;
        if(((c->cur - c->base) + (uint32)s.sh_size) > c->size)
        {
            return ARKRTOS_MOD_ERR_LAYOUT;
        }
        c->cur += (uint32)s.sh_size;
    }
    return ARKRTOS_MOD_OK;
}

static int32 mod_copy(mod_link_t *c)
{
    uint32 i;

    /* 先把整槽清零：BSS 与节间空隙都靠它，免得把上一个模块的字节当数据用。 */
    for(i = 0u; i < c->size; i++)
    {
        c->img[i] = 0u;
    }

    for(i = 0u; i < c->shnum; i++)
    {
        mod_shdr_t s;
        uint32 k;

        mod_shdr(c, i, &s);
        if((s.sh_flags & (unsigned long)MOD_SHF_ALLOC) == 0ul)
        {
            continue;
        }
        if((s.sh_type == MOD_SHT_NOBITS) || (s.sh_size == 0ul))
        {
            continue;                    /* .bss 已经在清零里了 */
        }
        if((s.sh_offset + s.sh_size) > (unsigned long)c->file_len)
        {
            return ARKRTOS_MOD_ERR_FORMAT;
        }
        for(k = 0u; k < (uint32)s.sh_size; k++)
        {
            c->img[c->sec_addr[i] - c->base + k] = c->file[s.sh_offset + k];
        }
        c->text_bytes += (uint32)s.sh_size;
    }
    return ARKRTOS_MOD_OK;
}

/* ---------------------------------------------------------------- GOT */

/**
* @brief 按「首次被 GPREL 引用的顺序」给外部数据符号分 GOT 槽
* @details 只有**未定义**符号需要槽：模块里自己定义的数据，编译器发的是
*          pcalau12i 直接算地址；外部数据才走 `ld.d $r12,$r12,slot`。
*          给本地符号也开槽也能跑，但会多一层间接，且和真实链接器不同。
*/
static int32 mod_got_alloc(mod_link_t *c)
{
    uint32 i;

    c->got_n = 0u;
    for(i = 0u; i < c->shnum; i++)
    {
        mod_shdr_t rs;
        uint32 k;

        mod_shdr(c, i, &rs);
        if(rs.sh_type != MOD_SHT_RELA)
        {
            continue;
        }
        for(k = 0u; k < (uint32)(rs.sh_size / MOD_RELA_SIZE); k++)
        {
            const uint8 *e = c->file + rs.sh_offset + (k * MOD_RELA_SIZE);
            uint32 type = (uint32)(m_rd64(e + 8) & 0xFFFFFFFFul);
            uint32 symidx = (uint32)(m_rd64(e + 8) >> 32);
            uint32 shndx = 0u;
            const char *name = 0;
            uint32 g;

            if(type != R_LA_SOP_PUSH_GPREL)
            {
                continue;
            }
            if(mod_sym(c, symidx, &name, &shndx, 0) != 0)
            {
                return ARKRTOS_MOD_ERR_FORMAT;
            }
            if(shndx != MOD_SHN_UNDEF)
            {
                continue;                /* 本地定义：不需要槽 */
            }
            for(g = 0u; g < c->got_n; g++)
            {
                if(c->got_sym[g] == symidx)
                {
                    break;
                }
            }
            if(g < c->got_n)
            {
                continue;
            }
            if(c->got_n >= MOD_MAX_GOT)
            {
                return ARKRTOS_MOD_ERR_NOMEM;
            }
            c->got_sym[c->got_n] = symidx;
            c->got_n++;
        }
    }

    c->cur = mod_align_up(c->cur, 8u);
    c->got_base = c->cur;
    if(((c->got_base - c->base) + (8u * (c->got_n + 1u))) > c->size)
    {
        return ARKRTOS_MOD_ERR_LAYOUT;
    }
    c->cur = c->got_base + 8u * (c->got_n + 1u);
    /* 第 0 个槽留 0：psABI 约定 .got 头 8 字节不用，真实链接器也这么做。 */
    c->gp = c->got_base + 0x800u;
    for(i = 0u; i < c->got_n; i++)
    {
        c->got_addr[i] = c->got_base + (8u * (i + 1u));
    }
    return ARKRTOS_MOD_OK;
}

static int32 mod_got_fill(mod_link_t *c)
{
    uint32 i;

    for(i = 0u; i < c->got_n; i++)
    {
        unsigned long v = 0ul;
        uint32 at;
        int32 rc = mod_sym_value(c, c->got_sym[i], &v);

        if(rc != ARKRTOS_MOD_OK)
        {
            return rc;
        }
        at = c->got_addr[i] - c->base;
        m_wr32(c->img + at, (uint32)(v & 0xFFFFFFFFul));
        m_wr32(c->img + at + 4u, (uint32)((v >> 32) & 0xFFFFFFFFul));
    }
    return ARKRTOS_MOD_OK;
}

/* ---------------------------------------------------------------- 重定位 */

static int32 mod_read_word(mod_link_t *c, uint32 addr, uint32 *p_w)
{
    if((addr < c->base) || ((addr + 4u) > (c->base + c->size)) ||
       (((addr - c->base) + 4u) > c->size))
    {
        return ARKRTOS_MOD_ERR_LAYOUT;
    }
    *p_w = m_rd32(c->img + (addr - c->base));
    return ARKRTOS_MOD_OK;
}

static int32 mod_write_word(mod_link_t *c, uint32 addr, uint32 w)
{
    if((addr < c->base) || (addr >= (c->base + c->size)) ||
       (((addr - c->base) + 4u) > c->size))
    {
        return ARKRTOS_MOD_ERR_LAYOUT;
    }
    m_wr32(c->img + (addr - c->base), w);
    return ARKRTOS_MOD_OK;
}

/** @brief 有符号立即数是否装得下 bits 位 */
static int32 mod_fits(long v, uint32 bits)
{
    long lo = -(1l << (bits - 1u));
    long hi = (1l << (bits - 1u)) - 1l;

    return ((v >= lo) && (v <= hi)) ? 0 : -1;
}

/* 重定位分四类机器，各管一段：压栈组算值入 SOP 栈、算数组只动栈、
 * 出栈组把栈顶按指令字段编码写回、写数数组直接写数据字。
 * 每类返回 MOD_RELA_PASS 表示「这个类型不归我」，由 mod_one_rela()
 * 依次问下去；四类都不认就报错并点出类型号——不猜编码。 */
#define MOD_RELA_PASS (1)

/* SOP 栈机：压栈组（符号值已由调用方算好，这里只管入栈） */
static int32 mod_rela_push(mod_link_t *c, uint32 type, uint32 symidx,
                           long addend, uint32 P, unsigned long S,
                           unsigned long *st, uint32 *p_sp)
{
    switch(type)
    {
    case R_LA_SOP_PUSH_ABSOLUTE:
        if(*p_sp >= 8u) { return ARKRTOS_MOD_ERR_RELOC; }
        st[(*p_sp)++] = (unsigned long)(S + (unsigned long)addend);
        return ARKRTOS_MOD_OK;

    case R_LA_SOP_PUSH_PCREL:
        if(*p_sp >= 8u) { return ARKRTOS_MOD_ERR_RELOC; }
        st[(*p_sp)++] = (unsigned long)(S + (unsigned long)addend)
                        - (unsigned long)P;
        return ARKRTOS_MOD_OK;

    case R_LA_SOP_PUSH_PLT_PCREL:
        /* 本仓没有 PLT：调用直接指向被调函数本身。 */
        if(*p_sp >= 8u) { return ARKRTOS_MOD_ERR_RELOC; }
        st[(*p_sp)++] = (unsigned long)(S + (unsigned long)addend)
                        - (unsigned long)P;
        return ARKRTOS_MOD_OK;

    case R_LA_SOP_PUSH_GPREL:
    {
        uint32 shndx = 0u;

        if(*p_sp >= 8u) { return ARKRTOS_MOD_ERR_RELOC; }
        if(mod_sym(c, symidx, 0, &shndx, 0) != 0)
        {
            return ARKRTOS_MOD_ERR_FORMAT;
        }
        if(shndx != MOD_SHN_UNDEF)
        {
            /* 本地定义：符号地址直接相对 gp。 */
            st[(*p_sp)++] = (unsigned long)(S + (unsigned long)addend)
                            - (unsigned long)c->gp;
        }
        else
        {
            uint32 g;
            uint32 slot = 0u;

            for(g = 0u; g < c->got_n; g++)
            {
                if(c->got_sym[g] == symidx)
                {
                    slot = c->got_addr[g];
                    break;
                }
            }
            if(slot == 0u)
            {
                return ARKRTOS_MOD_ERR_RELOC;    /* 扫描阶段就该分好槽 */
            }
            st[(*p_sp)++] = (unsigned long)slot
                            + (unsigned long)addend
                            - (unsigned long)c->gp;
        }
        return ARKRTOS_MOD_OK;
    }

    default:
        return MOD_RELA_PASS;
    }
}

/* SOP 栈机：算数组（只看栈，不看符号与地址） */
static int32 mod_rela_alu(uint32 type, unsigned long *st, uint32 *p_sp)
{
    switch(type)
    {
    case R_LA_SOP_ADD:
        if(*p_sp < 2u) { return ARKRTOS_MOD_ERR_RELOC; }
        st[*p_sp - 2u] = st[*p_sp - 2u] + st[*p_sp - 1u];
        (*p_sp)--;
        return ARKRTOS_MOD_OK;

    case R_LA_SOP_SUB:
        if(*p_sp < 2u) { return ARKRTOS_MOD_ERR_RELOC; }
        st[*p_sp - 2u] = st[*p_sp - 2u] - st[*p_sp - 1u];
        (*p_sp)--;
        return ARKRTOS_MOD_OK;

    case R_LA_SOP_SL:
        if(*p_sp < 2u) { return ARKRTOS_MOD_ERR_RELOC; }
        st[*p_sp - 2u] = st[*p_sp - 2u] << (uint32)st[*p_sp - 1u];
        (*p_sp)--;
        return ARKRTOS_MOD_OK;

    case R_LA_SOP_SR:
        if(*p_sp < 2u) { return ARKRTOS_MOD_ERR_RELOC; }
        /* 逻辑右移：汇编器这里算的是地址的页内偏移，不能带符号位填充。 */
        st[*p_sp - 2u] = (unsigned long)(st[*p_sp - 2u]
                       >> (uint32)st[*p_sp - 1u]);
        (*p_sp)--;
        return ARKRTOS_MOD_OK;

    case R_LA_SOP_NOT:
        if(*p_sp < 1u) { return ARKRTOS_MOD_ERR_RELOC; }
        st[*p_sp - 1u] = (st[*p_sp - 1u] == 0ul) ? 1ul : 0ul;
        return ARKRTOS_MOD_OK;

    case R_LA_SOP_ASSERT:
        if(*p_sp < 1u) { return ARKRTOS_MOD_ERR_RELOC; }
        (*p_sp)--;
        if(st[*p_sp] == 0ul)
        {
            return ARKRTOS_MOD_ERR_RELOC;
        }
        return ARKRTOS_MOD_OK;

    default:
        return MOD_RELA_PASS;
    }
}

/* SOP 栈机：出栈组（把栈顶编码进 LoongArch 指令字，或写一个数据字） */
static int32 mod_rela_pop(mod_link_t *c, uint32 type, uint32 P,
                          unsigned long *st, uint32 *p_sp)
{
    int32 rc;

    switch(type)
    {
    case R_LA_SOP_POP_32_S_10_12:
    {
        uint32 w = 0u;
        long v;

        if(*p_sp < 1u) { return ARKRTOS_MOD_ERR_RELOC; }
        v = (long)st[--(*p_sp)];
        if(mod_fits(v, 12u) != 0) { return ARKRTOS_MOD_ERR_RELOC; }
        rc = mod_read_word(c, P, &w);
        if(rc != ARKRTOS_MOD_OK) { return rc; }
        w = (w & ~(0xFFFu << 10)) | (((uint32)v & 0xFFFu) << 10);
        return mod_write_word(c, P, w);
    }

    case R_LA_SOP_POP_32_S_5_20:
    {
        uint32 w = 0u;
        long v;

        if(*p_sp < 1u) { return ARKRTOS_MOD_ERR_RELOC; }
        v = (long)st[--(*p_sp)];
        if(mod_fits(v, 20u) != 0) { return ARKRTOS_MOD_ERR_RELOC; }
        rc = mod_read_word(c, P, &w);
        if(rc != ARKRTOS_MOD_OK) { return rc; }
        w = (w & ~(0xFFFFFu << 5)) | (((uint32)v & 0xFFFFFu) << 5);
        return mod_write_word(c, P, w);
    }

    case R_LA_SOP_POP_32_S_10_16_S2:
    {
        uint32 w = 0u;
        long v;

        if(*p_sp < 1u) { return ARKRTOS_MOD_ERR_RELOC; }
        v = (long)st[--(*p_sp)];
        if((v & 3l) != 0l) { return ARKRTOS_MOD_ERR_RELOC; }
        v >>= 2;
        /* B16 的偏移是 16 位字偏移：范围按「移位后」判，不是按字节判。 */
        if(mod_fits(v, 16u) != 0) { return ARKRTOS_MOD_ERR_RELOC; }
        rc = mod_read_word(c, P, &w);
        if(rc != ARKRTOS_MOD_OK) { return rc; }
        /* B16（beq/bne/bge...）：offs>>2 在 bits[25:10]，
         * 实测 `bge $r0,$r25,0x48` 的字是 0x64004819。 */
        w = (w & ~(0xFFFFu << 10)) | (((uint32)v & 0xFFFFu) << 10);
        return mod_write_word(c, P, w);
    }

    case R_LA_SOP_POP_32_S_0_10_10_16_S2:
    {
        uint32 w = 0u;
        long v;

        if(*p_sp < 1u) { return ARKRTOS_MOD_ERR_RELOC; }
        v = (long)st[--(*p_sp)];
        if((v & 3l) != 0l) { return ARKRTOS_MOD_ERR_RELOC; }
        v >>= 2;
        if(mod_fits(v, 26u) != 0) { return ARKRTOS_MOD_ERR_RELOC; }
        rc = mod_read_word(c, P, &w);
        if(rc != ARKRTOS_MOD_OK) { return rc; }
        /* B26 的字段顺序是交叉的（已用汇编器实测核对：
         *   bl target 且 offs=0xc 的指令字是 0x54000C00，
         *   0xC00 = 3 << 10，3 = offs/4 的低 16 位）
         *   bits[25:10] = offs[15:0]，bits[9:0] = offs[25:16]。
         * 写成「连续 26 位」会得到一条跳错地址的分支，而且反汇编上看不出来。 */
        w = (w & ~(0xFFFFu << 10)) | (((uint32)v & 0xFFFFu) << 10);
        w = (w & ~0x3FFu) | (((uint32)v >> 16) & 0x3FFu);
        return mod_write_word(c, P, w);
    }

    case R_LA_SOP_POP_32_U:
    {
        uint32 at;
        unsigned long v;

        if(*p_sp < 1u) { return ARKRTOS_MOD_ERR_RELOC; }
        v = st[--(*p_sp)];
        /* 无符号 32 位：高位非 0 就是装不下，不做静默截断。 */
        if((v >> 32) != 0ul)
        {
            return ARKRTOS_MOD_ERR_RELOC;
        }
        if((P < c->base) || (((P - c->base) + 4u) > c->size))
        {
            return ARKRTOS_MOD_ERR_LAYOUT;
        }
        at = P - c->base;
        m_wr32(c->img + at, (uint32)v);
        return ARKRTOS_MOD_OK;
    }

    default:
        return MOD_RELA_PASS;
    }
}

/* 直接写数据字：R_LA_32 / R_LA_64（符号值加 addend） */
static int32 mod_rela_write(mod_link_t *c, uint32 type, uint32 P,
                            unsigned long S, long addend)
{
    switch(type)
    {
    case R_LA_32:
    {
        uint32 at;
        uint32 w;

        if((P < c->base) || (((P - c->base) + 4u) > c->size))
        {
            return ARKRTOS_MOD_ERR_LAYOUT;
        }
        at = P - c->base;
        w = (uint32)(S + (unsigned long)addend);
        m_wr32(c->img + at, w);
        return ARKRTOS_MOD_OK;
    }

    case R_LA_64:
    {
        uint32 at;
        unsigned long v = S + (unsigned long)addend;

        if((P < c->base) || (((P - c->base) + 8u) > c->size))
        {
            return ARKRTOS_MOD_ERR_LAYOUT;
        }
        at = P - c->base;
        m_wr32(c->img + at, (uint32)(v & 0xFFFFFFFFul));
        m_wr32(c->img + at + 4u, (uint32)((v >> 32) & 0xFFFFFFFFul));
        return ARKRTOS_MOD_OK;
    }

    default:
        return MOD_RELA_PASS;
    }
}

/** @brief 一条 rela 记录：在 SOP 栈机上求值后写回指令字 */
static int32 mod_one_rela(mod_link_t *c, uint32 target, uint32 off,
                          uint32 type, uint32 symidx, long addend,
                          unsigned long *st, uint32 *p_sp)
{
    uint32 P = c->sec_addr[target] + off;
    unsigned long S = 0ul;
    int32 rc;

    /* 需要符号值的类型先算出来；纯运算类型不看符号。 */
    switch(type)
    {
    case R_LA_SOP_PUSH_PCREL:
    case R_LA_SOP_PUSH_ABSOLUTE:
    case R_LA_SOP_PUSH_GPREL:
    case R_LA_SOP_PUSH_PLT_PCREL:
    case R_LA_32:
    case R_LA_64:
        rc = mod_sym_value(c, symidx, &S);
        if(rc != ARKRTOS_MOD_OK)
        {
            return rc;
        }
        break;
    default:
        break;
    }

    rc = mod_rela_push(c, type, symidx, addend, P, S, st, p_sp);
    if(rc != MOD_RELA_PASS) { return rc; }

    rc = mod_rela_alu(type, st, p_sp);
    if(rc != MOD_RELA_PASS) { return rc; }

    rc = mod_rela_pop(c, type, P, st, p_sp);
    if(rc != MOD_RELA_PASS) { return rc; }

    rc = mod_rela_write(c, type, P, S, addend);
    if(rc != MOD_RELA_PASS) { return rc; }

    /* 不认识就报错，并把类型号带出去：不猜编码。 */
    c->last_reloc = type;
    return ARKRTOS_MOD_ERR_RELOC;
}

static int32 mod_reloc_all(mod_link_t *c)
{
    uint32 i;

    for(i = 0u; i < c->shnum; i++)
    {
        mod_shdr_t rs;
        mod_shdr_t ts;
        uint32 target;
        uint32 k;
        uint32 n;

        mod_shdr(c, i, &rs);
        if(rs.sh_type != MOD_SHT_RELA)
        {
            continue;
        }
        target = rs.sh_info;
        if(target >= c->shnum)
        {
            return ARKRTOS_MOD_ERR_FORMAT;
        }
        mod_shdr(c, target, &ts);
        if((ts.sh_flags & (unsigned long)MOD_SHF_ALLOC) == 0ul)
        {
            /* .rela.eh_frame 之类：目标节不装载，重定位也就没有意义。 */
            continue;
        }
        n = (uint32)(rs.sh_size / MOD_RELA_SIZE);
        /* 一条指令的 SOP 表达式由**多条** reloc 记录共同构成（实测一次
         * extern 数据访问是 7 条），所以栈必须在这几条之间活着；只有
         * offset 变了才是下一条指令，那时才要求栈已弹干净。
         * 早先把 st/sp 声明在循环体内，等于每条记录都从空栈开始，
         * 于是每一条 PUSH 之后立刻就报「栈没弹干净」。 */
        {
            unsigned long st[8];
            uint32 sp = 0u;
            uint32 prev_off = 0xFFFFFFFFu;

            for(k = 0u; k < n; k++)
            {
                const uint8 *e = c->file + rs.sh_offset + (k * MOD_RELA_SIZE);
                unsigned long info = m_rd64(e + 8);
                uint32 off = (uint32)m_rd64(e + 0);
                int32 rc;

                if(off != prev_off)
                {
                    if(sp != 0u)
                    {
                        /* 上一条指令的表达式没配平：不是我们理解的形态。 */
                        return ARKRTOS_MOD_ERR_RELOC;
                    }
                    sp = 0u;
                    prev_off = off;
                }

                rc = mod_one_rela(c, target, off,
                                  (uint32)(info & 0xFFFFFFFFul),
                                  (uint32)(info >> 32),
                                  (long)m_rd64(e + 16),
                                  st, &sp);
                if(rc != ARKRTOS_MOD_OK)
                {
                    return rc;
                }
                c->relocs++;
            }
            if(sp != 0u)
            {
                /* 最后一条指令的表达式也要配平。 */
                return ARKRTOS_MOD_ERR_RELOC;
            }
        }
    }
    return ARKRTOS_MOD_OK;
}

/* ---------------------------------------------------------------- 入口 */

static int32 mod_find_entry(mod_link_t *c, uint32 *p_entry)
{
    uint32 i;
    mod_shdr_t st;

    if(c->symtab == 0xFFFFFFFFu)
    {
        return ARKRTOS_MOD_ERR_NOENTRY;
    }
    mod_shdr(c, c->symtab, &st);
    for(i = 0u; i < (uint32)(st.sh_size / MOD_SYM_SIZE); i++)
    {
        const char *name = 0;
        uint32 shndx = 0u;
        unsigned long value = 0ul;

        if(mod_sym(c, i, &name, &shndx, &value) != 0)
        {
            return ARKRTOS_MOD_ERR_FORMAT;
        }
        if((name == 0) || (shndx == MOD_SHN_UNDEF))
        {
            continue;
        }
        if(m_streq(name, ARKRTOS_MOD_ENTRY) == 0)
        {
            *p_entry = c->sec_addr[shndx] + (uint32)value;
            return ARKRTOS_MOD_OK;
        }
    }
    return ARKRTOS_MOD_ERR_NOENTRY;
}

/* ---------------------------------------------------------------- 任务 */

static void mod_task_tramp(void)
{
    arkrtos_task_t *p_self = arkrtos_task_get_current();
    int32 i;
    int32 rc = 0;

    for(i = 0; i < ARKRTOS_MOD_SLOTS; i++)
    {
        if((s_slots[i].used != 0) && (s_slots[i].task > 0) &&
           (p_self == &arkrtos_task_table[s_slots[i].task - 1]))
        {
            int (*p_fn)(int, char **) =
                (int (*)(int, char **))(unsigned long)s_slots[i].entry;

            /* 模块的 module_main 返回值就是它的结论。有一个口子必须说清：
             * 这是「模块自己声明的码」，不是内核判定——返回 0 只表示它没
             * 报告失败。 */
            rc = (int32)p_fn((int)s_slots[i].argc, s_mod_argp[i]);
            break;
        }
    }
#if (ARKRTOS_USE_EXIT_CODE == 1)
    /* 开档：把码交给内核（与 App 走同一条 SVC 落地路径），shell 的
     * wait <task id> 能读到；关档仍走纯退出，逐位不变。 */
    arkrtos_thread_exit_code_internal(rc);
#else
    (void)rc;
    arkrtos_thread_exit_internal();
#endif
    for(;;)
    {
    }
}

/* ---------------------------------------------------------------- 对外 */

static int32 mod_slot_find_free(void)
{
    int32 i;

    for(i = 0; i < ARKRTOS_MOD_SLOTS; i++)
    {
        if(s_slots[i].used == 0)
        {
            return i;
        }
    }
    return -1;
}

static int32 mod_task_alive(const mod_slot_t *s)
{
    if(s->task <= 0)
    {
        return 0;
    }
    return (arkrtos_task_table[s->task - 1].status != ARKRTOS_TASK_INVALID) ? 1 : 0;
}

/* 参数表自检：每个 argv[i] 非空且短于 MOD_ARG_LEN。
 * 「缺省成一个参数」不在这里补——那要一个返回后仍然有效的数组，只能由
 * 调用点自己持有。 */
static int32 mod_args_valid(int32 argc, char *argv[])
{
    uint32 i;

    for(i = 0u; i < (uint32)argc; i++)
    {
        if(argv[i] == 0)
        {
            return ARKRTOS_MOD_ERR_PARAM;
        }
        if(m_strlen(argv[i]) >= MOD_ARG_LEN)
        {
            return ARKRTOS_MOD_ERR_PARAM;
        }
    }
    return ARKRTOS_MOD_OK;
}

/* 把整个 .o 读进 s_mod_file，出参是正文长度（已扣掉可选的签名 tag）。
 * 文件比槽还大就读不进来：这是容量事实，不是格式问题。 */
static int32 mod_read_image(const char *path, uint32 *p_len)
{
    uint32 len = 0u;
    int32 rc;

    rc = arkrtos_vfs_read_file(path, s_mod_file, ARKRTOS_MOD_SLOT_SIZE, &len);
    if(rc < 0)
    {
        return ARKRTOS_MOD_ERR_FS;
    }

#if (ARKRTOS_USE_EXEC_SIGN == 1)
    /* 尾部 32 字节 tag 覆盖 [0, len-32)。校验完把 len 缩回正文长度，后面的
     * 节表/重定位一律按"文件只到 tag 之前"来解析——不缩的话，一个把
     * shoff+shnum*64 顶到文件尾的坏对象会拿 tag 当节头表读。 */
    if(len <= (uint32)ARKRTOS_SIGN_LEN)
    {
        return ARKRTOS_MOD_ERR_SIGN;
    }
    len -= (uint32)ARKRTOS_SIGN_LEN;
    if(arkrtos_sign_payload_check(s_mod_file, len,
                                  s_mod_file + len) != 0)
    {
        return ARKRTOS_MOD_ERR_SIGN;
    }
#endif

    if(len < MOD_EHDR_SIZE)
    {
        return ARKRTOS_MOD_ERR_FORMAT;
    }

    *p_len = len;
    return ARKRTOS_MOD_OK;
}

/* 解析 ELF 头与节表，找出 .symtab / .strtab，并把链接上下文（c）按这个槽
 * 的落点填好。只读不写镜像。 */
static int32 mod_parse_elf(mod_link_t *c, int32 slot, uint32 len)
{
    mod_shdr_t sh;
    uint32 i;

    for(i = 0u; i < sizeof(*c); i++)
    {
        ((uint8 *)c)[i] = 0u;
    }
    c->file = s_mod_file;
    c->file_len = len;
    c->img = s_mod_pool + ((uint32)slot * ARKRTOS_MOD_SLOT_SIZE);
    /* 窗口地址 = 池子在这个槽上的真实地址（恒等映射下即可执行地址）。
     * 不能拿宏来算：宏只是「期望窗口在哪」，池子的实际落点由链接器定。 */
    c->base = (uint32)(unsigned long)c->img;
    c->size = ARKRTOS_MOD_SLOT_SIZE;
    c->symtab = 0xFFFFFFFFu;
    c->strtab = 0xFFFFFFFFu;

    /* --- ELF 头 --- */
    if((s_mod_file[0] != 0x7Fu) || (s_mod_file[1] != 'E') ||
       (s_mod_file[2] != 'L') || (s_mod_file[3] != 'F'))
    {
        return ARKRTOS_MOD_ERR_FORMAT;
    }
    if((s_mod_file[4] != 2u) || (s_mod_file[5] != 1u))
    {
        return ARKRTOS_MOD_ERR_FORMAT;
    }
    if(m_rd16(s_mod_file + 18) != (uint16)MOD_EM_LOONGARCH)
    {
        return ARKRTOS_MOD_ERR_ARCH;
    }
    if(m_rd16(s_mod_file + 16) != (uint16)MOD_ET_REL)
    {
        /* ET_EXEC 走 exec；这一条专门挡住「把可执行文件喂给 ld」。 */
        return ARKRTOS_MOD_ERR_FORMAT;
    }

    c->shoff     = (uint32)m_rd64(s_mod_file + 40);
    c->shentsize = m_rd16(s_mod_file + 58);
    c->shnum     = m_rd16(s_mod_file + 60);
    if((c->shentsize != MOD_SHDR_SIZE) || (c->shnum == 0u) ||
       (c->shnum > MOD_MAX_SEC) ||
       ((c->shoff + (c->shnum * c->shentsize)) > len))
    {
        return ARKRTOS_MOD_ERR_FORMAT;
    }

    /* --- 找 .symtab / .strtab --- */
    for(i = 0u; i < c->shnum; i++)
    {
        mod_shdr(c, i, &sh);
        if(sh.sh_type == MOD_SHT_SYMTAB)
        {
            c->symtab = i;
            c->strtab = sh.sh_link;
        }
    }
    if((c->symtab == 0xFFFFFFFFu) || (c->strtab >= c->shnum))
    {
        /* 没有符号表就既找不到入口也解析不了外部引用——不是可以「凑合跑」的情况。 */
        return ARKRTOS_MOD_ERR_FORMAT;
    }

    return ARKRTOS_MOD_OK;
}

/* 布局 -> GOT -> 搬字节 -> 回填 GOT -> 重定位 -> 找入口。
 * 失败时把「缺哪个符号 / 哪条重定位坏了」留在诊断变量里：调用者要知道缺谁，
 * 不能只拿到一个 -6/-7。 */
static int32 mod_link_image(mod_link_t *c, uint32 *p_entry)
{
    int32 rc;

    rc = mod_place(c);
    if(rc != ARKRTOS_MOD_OK)
    {
        return rc;
    }
    rc = mod_got_alloc(c);
    if(rc != ARKRTOS_MOD_OK)
    {
        return rc;
    }
    rc = mod_copy(c);
    if(rc != ARKRTOS_MOD_OK)
    {
        return rc;
    }
    rc = mod_got_fill(c);
    if(rc != ARKRTOS_MOD_OK)
    {
        return rc;
    }
    rc = mod_reloc_all(c);
    if(rc != ARKRTOS_MOD_OK)
    {
        /* 部分失败只剩诊断信息，不能只说一个 -6/-7：调用者要知道缺谁。 */
        if(rc == ARKRTOS_MOD_ERR_SYMBOL)
        {
            m_strcpy_n(s_last_missing, MOD_NAME_LEN, c->last_sym);
        }
        else if(rc == ARKRTOS_MOD_ERR_RELOC)
        {
            s_last_bad_reloc = c->last_reloc;
        }
        return rc;
    }
    rc = mod_find_entry(c, p_entry);
    if(rc != ARKRTOS_MOD_OK)
    {
        return rc;
    }

    return ARKRTOS_MOD_OK;
}

/* 链接成功之后才记槽：名字、几何、参数表。
 * 参数拷进槽自己的静态区：shell 的 argv 指向它的行缓冲，等模块任务真跑起来
 * 时那行可能已经被下一行覆盖了。 */
static void mod_commit_slot(int32 slot, const char *path, int32 argc,
                            char *argv[], const mod_link_t *c, uint32 entry)
{
    const char *base_name;
    uint32 i;

    base_name = path;
    for(i = 0u; path[i] != '\0'; i++)
    {
        if(path[i] == '/')
        {
            base_name = path + i + 1u;
        }
    }
    m_strcpy_n(s_slots[slot].name, MOD_NAME_LEN, base_name);

    s_slots[slot].used       = 1;
    s_slots[slot].base       = c->base;
    s_slots[slot].size       = c->cur - c->base;
    s_slots[slot].entry      = entry;
    s_slots[slot].text_bytes = c->text_bytes;
    s_slots[slot].relocs     = c->relocs;
    s_slots[slot].imports    = c->imports;
    s_slots[slot].argc       = (uint32)argc;

    for(i = 0u; i < (uint32)argc; i++)
    {
        m_strcpy_n(s_mod_argv[slot][i], MOD_ARG_LEN, argv[i]);
        s_mod_argp[slot][i] = s_mod_argv[slot][i];
    }
    for(i = (uint32)argc; i <= (uint32)MOD_MAX_ARGS; i++)
    {
        s_mod_argp[slot][i] = (char *)0;      /* argv[argc] == NULL，模块可以靠它收尾 */
    }
}

int32 arkrtos_mod_load(const char *path, int32 argc, char *argv[])
{
    mod_link_t c;
    uint32 len = 0u;
    uint32 entry = 0u;
    int32 slot;
    int32 rc;
    int32 id;
    char *argv_auto[1];

    if((path == 0) || (path[0] == '\0'))
    {
        return ARKRTOS_MOD_ERR_PARAM;
    }
    if((argc < 0) || (argc > (int32)MOD_MAX_ARGS))
    {
        return ARKRTOS_MOD_ERR_PARAM;
    }
    if((argc == 0) || (argv == 0))
    {
        argv_auto[0] = (char *)path;
        argv = argv_auto;
        argc = 1;
    }
    rc = mod_args_valid(argc, argv);
    if(rc != ARKRTOS_MOD_OK)
    {
        return rc;
    }

    s_last_missing[0] = '\0';
    s_last_bad_reloc  = 0u;

    slot = mod_slot_find_free();
    if(slot < 0)
    {
        return ARKRTOS_MOD_ERR_NOMEM;
    }

    rc = mod_read_image(path, &len);
    if(rc != ARKRTOS_MOD_OK)
    {
        return rc;
    }
    rc = mod_parse_elf(&c, slot, len);
    if(rc != ARKRTOS_MOD_OK)
    {
        return rc;
    }
    rc = mod_link_image(&c, &entry);
    if(rc != ARKRTOS_MOD_OK)
    {
        return rc;
    }

    mod_commit_slot(slot, path, argc, argv, &c, entry);

    id = arkrtos_task_register(mod_task_tramp, s_mod_stack[slot],
                              (uint32)sizeof(s_mod_stack[slot]),
                              (uint8)ARKRTOS_MOD_TASK_PRIORITY, 0u);
    if(id <= 0)
    {
        s_slots[slot].used = 0;
        return ARKRTOS_MOD_ERR_NOMEM;
    }
    s_slots[slot].task = id;

    return ARKRTOS_MOD_OK;
}

int32 arkrtos_mod_count(void)
{
    int32 i;
    int32 n = 0;

    for(i = 0; i < ARKRTOS_MOD_SLOTS; i++)
    {
        /* 按「已占用」计数而不是「任务还活着」：模块跑完但没卸时它仍占着
         * 一块内存，列表里把它藏起来只会让人以为槽位凭空消失。 */
        if(s_slots[i].used != 0)
        {
            n++;
        }
    }
    return n;
}

int32 arkrtos_mod_info(int32 index, arkrtos_mod_info_t *p_out)
{
    int32 i;
    int32 n = 0;

    if((p_out == 0) || (index < 0))
    {
        return (int32)-1;
    }
    for(i = 0; i < ARKRTOS_MOD_SLOTS; i++)
    {
        if(s_slots[i].used == 0)
        {
            continue;
        }
        if(n == index)
        {
            uint32 k;

            for(k = 0u; k < MOD_NAME_LEN; k++)
            {
                p_out->name[k] = s_slots[i].name[k];
            }
            p_out->base       = s_slots[i].base;
            p_out->size       = s_slots[i].size;
            p_out->entry      = s_slots[i].entry;
            p_out->text_bytes = s_slots[i].text_bytes;
            p_out->relocs     = s_slots[i].relocs;
            p_out->imports    = s_slots[i].imports;
            p_out->task       = s_slots[i].task;
            p_out->state      = mod_task_alive(&s_slots[i]);
            return 0;
        }
        n++;
    }
    return (int32)-1;
}

int32 arkrtos_mod_unload(int32 index)
{
    int32 i;
    int32 n = 0;

    if(index < 0)
    {
        return ARKRTOS_MOD_ERR_PARAM;
    }
    for(i = 0; i < ARKRTOS_MOD_SLOTS; i++)
    {
        /* 这里按「已登记」而不是「活着」来数：正在跑的模块也得能被指名，
         * 否则 unld 会把一个活模块当成不存在而误卸下一个。 */
        if(s_slots[i].used == 0)
        {
            continue;
        }
        if(n == index)
        {
            /* 任务还在跑就不能卸：它正指着这块内存执行。 */
            if(mod_task_alive(&s_slots[i]) != 0)
            {
                return ARKRTOS_MOD_ERR_BUSY;
            }
            s_slots[i].used = 0;
            s_slots[i].task = 0;
            s_slots[i].argc = 0u;
            s_mod_argp[i][0] = (char *)0;     /* 别把上一轮的指针留给下一个模块 */
            return ARKRTOS_MOD_OK;
        }
        n++;
    }
    return ARKRTOS_MOD_ERR_PARAM;
}

const char *arkrtos_mod_missing_symbol(void)
{
    return s_last_missing;
}

uint32 arkrtos_mod_bad_reloc(void)
{
    return s_last_bad_reloc;
}

const char *arkrtos_mod_error_name(int32 rc)
{
    switch(rc)
    {
    case ARKRTOS_MOD_OK:            return "ok";
    case ARKRTOS_MOD_ERR_PARAM:     return "bad parameter";
    case ARKRTOS_MOD_ERR_FS:        return "cannot read file (or larger than a slot)";
    case ARKRTOS_MOD_ERR_FORMAT:    return "not an ELF64 relocatable object";
    case ARKRTOS_MOD_ERR_ARCH:      return "wrong machine (need LoongArch)";
    case ARKRTOS_MOD_ERR_LAYOUT:    return "section does not fit the module slot";
    case ARKRTOS_MOD_ERR_SYMBOL:    return "unresolved symbol (not exported by the kernel)";
    case ARKRTOS_MOD_ERR_RELOC:     return "unsupported relocation";
    case ARKRTOS_MOD_ERR_NOENTRY:   return "no module_main entry in this object";
    case ARKRTOS_MOD_ERR_NOMEM:     return "no free module slot (or no task slot)";
    case ARKRTOS_MOD_ERR_BUSY:      return "module task still running";
    case ARKRTOS_MOD_ERR_SIGN:      return "missing or wrong signature tag (see tools/sign_payload.py)";
    default:                        return "unknown";
    }
}

#endif /* ARKRTOS_USE_MODLOAD */
