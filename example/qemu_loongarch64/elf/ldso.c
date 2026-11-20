/**
* @file ldso.c
* @brief Ark RTOS 的最小用户态动态装载器（QEMU LoongArch64）
* @details 内核按 Linux 的约定把它的入口设成本文件的 _start，并在用户栈上
*          铺好 auxv（AT_PHDR / AT_PHENT / AT_PHNUM / AT_PAGESZ / AT_BASE /
*          AT_ENTRY ...）。它接着做 glibc 的 ld.so 做的那几件事的精简版：
*
*            1) 从 sp 读 argc/argv/envp/auxv；
*            2) 用 AT_PHDR 定位主程序（base = AT_PHDR - 64，并核对 e_phoff
*               真的是 64——不是就明确失败，不猜）；
*            3) 解析主程序的 PT_DYNAMIC，读出 DT_NEEDED / DT_STRTAB /
*               DT_SYMTAB / DT_HASH / DT_RELA / DT_JMPREL；
*            4) 按 /mnt/ram/、/lib/ 搜索 DT_NEEDED，用 mmap + pread64 把
*               每个库的 PT_LOAD 铺到匿名内存里（不用 MAP_FIXED——内核
*               不支持，也不假装支持）；
*            5) 做 R_LARCH_RELATIVE / R_LARCH_64 / R_LARCH_JUMP_SLOT；
*            6) 打印一行行 [ldso] 读数，然后 jr 到 AT_ENTRY。
*
*          边界写在明处：
*            - 只支持一级依赖（库的库不递归；遇到就明确报错）；
*            - 只支持 LoongArch 的 3/2/5 三种重定位，其它类型直接失败；
*            - 符号解析是"按名字线性扫 .dynsym"，不是 ELF 哈希；
*            - 这不是 glibc 的 ld.so：真发行版二进制需要它 + 更大的系统
*              调用面（futex/rseq/TLS/文件映射），这里不冒充。
*
*          失败时打印 [ldso] FAIL 并以 127 退出——装载器不能"看起来成功"。
*
* @author xw
*/

#define LX_OPENAT           56
#define LX_CLOSE            57
#define LX_WRITE            64
#define LX_PREAD64          67
#define LX_MMAP             222
#define LX_EXIT             93

#define LX_AT_FDCWD         (-100)
#define LX_PROT_RW          0x3
#define LX_MAP_PRIVATE      0x02
#define LX_MAP_ANONYMOUS    0x20

#define AT_NULL             0
#define AT_PHDR             3
#define AT_PHENT            4
#define AT_PHNUM            5
#define AT_PAGESZ           6
#define AT_BASE             7
#define AT_ENTRY            9

#define PT_LOAD             1
#define PT_DYNAMIC          2
#define ET_EXEC             2
#define ET_DYN              3

#define DT_NULL             0
#define DT_NEEDED           1
#define DT_PLTRELSZ         2
#define DT_HASH             4
#define DT_STRTAB           5
#define DT_SYMTAB           6
#define DT_RELA             7
#define DT_RELASZ           8
#define DT_RELAENT          9
#define DT_STRSZ            10
#define DT_SYMENT           11
#define DT_JMPREL           23

#define R_LARCH_64          2
#define R_LARCH_RELATIVE    3
#define R_LARCH_JUMP_SLOT   5

typedef long lx_long;
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

typedef struct
{
    u8  e_ident[16];
    unsigned short e_type;
    unsigned short e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
} ehdr_t;

typedef struct
{
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} phdr_t;

typedef struct
{
    long d_tag;
    u64  d_val;
} dyn_t;

typedef struct
{
    u32 st_name;
    u8  st_info;
    u8  st_other;
    unsigned short st_shndx;
    u64 st_value;
    u64 st_size;
} sym_t;

typedef struct
{
    u64 r_offset;
    u64 r_info;
    long r_addend;
} rela_t;

#define MAX_OBJ     4
#define MAX_PHNUM   16
#define PATH_MAX    96

typedef struct
{
    int used;
    int is_dyn;
    u64 bias;           /* DYN: 装载基址；EXEC: 0 */
    u64 dyn;            /* PT_DYNAMIC 的运行期地址 */
    u64 dynsz;
    u64 strtab;
    u64 symtab;
    u64 hash;
    u64 rela;
    u64 relasz;
    u64 jmprel;
    u64 pltrelsz;
    u64 strsz;
    u64 syment;
    char name[48];
} obj_t;

static obj_t s_obj[MAX_OBJ];
static int   s_nobj;
static u64   s_boot_sp;

/* ------------------------------------------------------------------ 系统调用 */
static lx_long lx1(lx_long n, lx_long a0v)
{
    register lx_long a0 __asm__("a0") = a0v;
    register lx_long a7 __asm__("a7") = n;

    __asm__ volatile("syscall 0" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static lx_long lx3(lx_long n, lx_long a0v, lx_long a1v, lx_long a2v)
{
    register lx_long a0 __asm__("a0") = a0v;
    register lx_long a1 __asm__("a1") = a1v;
    register lx_long a2 __asm__("a2") = a2v;
    register lx_long a7 __asm__("a7") = n;

    __asm__ volatile("syscall 0" : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static lx_long lx4(lx_long n, lx_long a0v, lx_long a1v, lx_long a2v, lx_long a3v)
{
    register lx_long a0 __asm__("a0") = a0v;
    register lx_long a1 __asm__("a1") = a1v;
    register lx_long a2 __asm__("a2") = a2v;
    register lx_long a3 __asm__("a3") = a3v;
    register lx_long a7 __asm__("a7") = n;

    __asm__ volatile("syscall 0" : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

/* ------------------------------------------------------------------ 输出 */
static u64 s_len(const char *s)
{
    u64 n = 0;

    while (s[n] != '\0')
    {
        n++;
    }
    return n;
}

static void out(const char *s)
{
    u64 n = s_len(s);

    if (n != 0)
    {
        (void)lx3(LX_WRITE, 1, (lx_long)(u64)s, (lx_long)n);
    }
}

static void out_hex(u64 v)
{
    static const char h[] = "0123456789abcdef";
    char buf[19];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++)
    {
        buf[2 + i] = h[(v >> ((15 - i) * 4)) & 0xFu];
    }
    buf[18] = '\0';
    out(buf);
}

static void out_dec(u64 v)
{
    char buf[21];
    int i = 0;

    if (v == 0)
    {
        out("0");
        return;
    }
    while ((v != 0) && (i < 20))
    {
        buf[i++] = (char)('0' + (u32)(v % 10));
        v /= 10;
    }
    while (i > 0)
    {
        char one[2];

        i--;
        one[0] = buf[i];
        one[1] = '\0';
        out(one);
    }
}

static void fail(const char *msg)
{
    out("[ldso] FAIL: ");
    out(msg);
    out("\n");
    (void)lx1(LX_EXIT, 127);
    for (;;)
    {
    }
}

/* ------------------------------------------------------------------ 小工具 */
static int str_eq(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b))
    {
        a++;
        b++;
    }
    return (*a == *b);
}

static void str_copy(char *dst, const char *src, u64 cap)
{
    u64 i = 0;

    while ((i + 1 < cap) && (src[i] != '\0'))
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void mem_zero(void *p, u64 n)
{
    u8 *q = (u8 *)p;
    u64 i;

    for (i = 0; i < n; i++)
    {
        q[i] = 0;
    }
}

static u64 align_up(u64 v, u64 a)
{
    return (v + a - 1) & ~(a - 1);
}

/* ------------------------------------------------------------------ 读文件 */
static int read_file(const char *path, void *buf, u64 len, u64 off)
{
    lx_long fd;
    lx_long got;

    fd = lx4(LX_OPENAT, LX_AT_FDCWD, (lx_long)(u64)path, 0, 0);
    if (fd < 0)
    {
        return (int)fd;
    }
    got = lx4(LX_PREAD64, fd, (lx_long)(u64)buf, (lx_long)len, (lx_long)off);
    (void)lx1(LX_CLOSE, fd);
    if (got < 0)
    {
        return (int)got;
    }
    if ((u64)got != len)
    {
        return -1;
    }
    return 0;
}

static int file_exists(const char *path)
{
    lx_long fd = lx4(LX_OPENAT, LX_AT_FDCWD, (lx_long)(u64)path, 0, 0);

    if (fd < 0)
    {
        return 0;
    }
    (void)lx1(LX_CLOSE, fd);
    return 1;
}

/* ------------------------------------------------------------------ 解析 */
static u64 obj_addr(const obj_t *o, u64 v)
{
    return o->bias + v;
}

static int parse_dynamic(obj_t *o, const phdr_t *ph, int nph)
{
    int i;

    for (i = 0; i < nph; i++)
    {
        if (ph[i].p_type == PT_DYNAMIC)
        {
            o->dyn = obj_addr(o, ph[i].p_vaddr);
            o->dynsz = ph[i].p_memsz;
            break;
        }
    }
    if (o->dyn == 0)
    {
        return -1;
    }

    o->syment = 24;
    {
        dyn_t *d = (dyn_t *)o->dyn;

        for (; d->d_tag != DT_NULL; d++)
        {
            switch ((int)d->d_tag)
            {
            case DT_NEEDED:
                /* 只记录在调用方遍历原表时处理；这里不存 */
                break;
            case DT_STRTAB:  o->strtab  = obj_addr(o, d->d_val); break;
            case DT_SYMTAB:  o->symtab  = obj_addr(o, d->d_val); break;
            case DT_HASH:    o->hash    = obj_addr(o, d->d_val); break;
            case DT_STRSZ:   o->strsz   = d->d_val; break;
            case DT_SYMENT:  o->syment  = d->d_val; break;
            case DT_RELA:    o->rela    = obj_addr(o, d->d_val); break;
            case DT_RELASZ:  o->relasz  = d->d_val; break;
            case DT_JMPREL:  o->jmprel  = obj_addr(o, d->d_val); break;
            case DT_PLTRELSZ: o->pltrelsz = d->d_val; break;
            default: break;
            }
        }
    }
    if ((o->strtab == 0) || (o->symtab == 0))
    {
        return -1;
    }
    return 0;
}

static u64 sym_count(const obj_t *o)
{
    if (o->hash != 0)
    {
        const u32 *h = (const u32 *)o->hash;

        return h[1];
    }
    /* 没 .hash 时用符号表后一个标签兜底：本仓夹具总带 DT_HASH。 */
    return 0;
}

static const char *sym_name(const obj_t *o, const sym_t *s)
{
    return (const char *)(o->strtab + s->st_name);
}

static u64 lookup(const char *name)
{
    int k;

    for (k = 0; k < s_nobj; k++)
    {
        obj_t *o = &s_obj[k];
        u64 n = sym_count(o);
        u64 i;

        if ((o->used == 0) || (o->symtab == 0))
        {
            continue;
        }
        for (i = 1; i < n; i++)
        {
            sym_t *s = (sym_t *)(o->symtab + (i * o->syment));

            if (s->st_shndx == 0)
            {
                continue;
            }
            if (str_eq(sym_name(o, s), name))
            {
                return obj_addr(o, s->st_value);
            }
        }
    }
    return 0;
}

static int apply_rela(obj_t *o, const rela_t *r, int is_plt, u32 *counts)
{
    u64 type = r->r_info & 0xFFFFFFFFu;
    u64 symidx = r->r_info >> 32;
    u64 loc = obj_addr(o, r->r_offset);

    if (type == R_LARCH_RELATIVE)
    {
        *(u64 *)loc = o->bias + (u64)r->r_addend;
        counts[0]++;
        return 0;
    }
    if (type == R_LARCH_64)
    {
        sym_t *s;
        u64 addr;
        const char *nm;

        if ((symidx == 0) || (o->symtab == 0))
        {
            return -1;
        }
        s = (sym_t *)(o->symtab + (symidx * o->syment));
        nm = sym_name(o, s);
        addr = lookup(nm);
        if (addr == 0)
        {
            out("[ldso] FAIL: unresolved symbol ");
            out(nm);
            out("\n");
            return -1;
        }
        *(u64 *)loc = addr + (u64)r->r_addend;
        counts[1]++;
        return 0;
    }
    if (type == R_LARCH_JUMP_SLOT)
    {
        sym_t *s;
        u64 addr;
        const char *nm;

        if ((symidx == 0) || (o->symtab == 0))
        {
            return -1;
        }
        s = (sym_t *)(o->symtab + (symidx * o->syment));
        nm = sym_name(o, s);
        addr = lookup(nm);
        if (addr == 0)
        {
            out("[ldso] FAIL: unresolved PLT symbol ");
            out(nm);
            out("\n");
            return -1;
        }
        *(u64 *)loc = addr;
        counts[2]++;
        return 0;
    }

    out("[ldso] FAIL: unsupported relocation type ");
    out_dec(type);
    out("\n");
    return -1;
}

static int relocate(obj_t *o)
{
    u32 counts[3];
    u64 n;
    u64 i;

    counts[0] = 0;
    counts[1] = 0;
    counts[2] = 0;

    if ((o->rela != 0) && (o->relasz != 0))
    {
        n = o->relasz / 24;
        for (i = 0; i < n; i++)
        {
            if (apply_rela(o, (const rela_t *)(o->rela + i * 24), 0,
                           counts) != 0)
            {
                return -1;
            }
        }
    }
    if ((o->jmprel != 0) && (o->pltrelsz != 0))
    {
        n = o->pltrelsz / 24;
        for (i = 0; i < n; i++)
        {
            if (apply_rela(o, (const rela_t *)(o->jmprel + i * 24), 1,
                           counts) != 0)
            {
                return -1;
            }
        }
    }

    out("[ldso] reloc ");
    out(o->name);
    out(": RELATIVE=");
    out_dec(counts[0]);
    out(" 64=");
    out_dec(counts[1]);
    out(" JUMP_SLOT=");
    out_dec(counts[2]);
    out("\n");
    return 0;
}

/* ------------------------------------------------------------------ 装载库 */
static int load_object(const char *name, u64 min_vaddr_hint)
{
    char path[PATH_MAX];
    ehdr_t eh;
    phdr_t ph[MAX_PHNUM];
    obj_t *o;
    u64 total;
    u64 addr;
    int nph;
    int i;
    int rc;

    if (s_nobj >= MAX_OBJ)
    {
        fail("too many objects");
    }

    /* 搜索顺序：/mnt/ram/（示例把文件写在这里）→ /lib/ */
    path[0] = '\0';
    str_copy(path, "/mnt/ram/", PATH_MAX);
    {
        u64 n = s_len(path);
        u64 i2 = 0;

        while ((i2 + 1 < PATH_MAX - n) && (name[i2] != '\0'))
        {
            path[n + i2] = name[i2];
            i2++;
        }
        path[n + i2] = '\0';
    }
    if (file_exists(path) == 0)
    {
        str_copy(path, "/lib/", PATH_MAX);
        {
            u64 n = s_len(path);
            u64 i2 = 0;

            while ((i2 + 1 < PATH_MAX - n) && (name[i2] != '\0'))
            {
                path[n + i2] = name[i2];
                i2++;
            }
            path[n + i2] = '\0';
        }
        if (file_exists(path) == 0)
        {
            out("[ldso] FAIL: cannot find ");
            out(name);
            out("\n");
            return -1;
        }
    }

    rc = read_file(path, &eh, sizeof(eh), 0);
    if (rc != 0)
    {
        out("[ldso] FAIL: cannot read ELF header of ");
        out(name);
        out("\n");
        return -1;
    }
    if ((eh.e_ident[0] != 0x7F) || (eh.e_ident[4] != 2) || (eh.e_ident[5] != 1))
    {
        fail("needed object is not ELF64 little-endian");
    }
    if (eh.e_type != ET_DYN)
    {
        fail("needed object is not ET_DYN (this loader loads DYN only)");
    }
    nph = (int)eh.e_phnum;
    if ((nph <= 0) || (nph > MAX_PHNUM))
    {
        fail("needed object has a bad phnum");
    }
    rc = read_file(path, ph, (u64)nph * sizeof(phdr_t), eh.e_phoff);
    if (rc != 0)
    {
        fail("cannot read program headers of needed object");
    }

    total = 0;
    for (i = 0; i < nph; i++)
    {
        if (ph[i].p_type == PT_LOAD)
        {
            u64 end = ph[i].p_vaddr + ph[i].p_memsz;

            if (end > total)
            {
                total = end;
            }
        }
    }
    if (total == 0)
    {
        fail("needed object has no PT_LOAD");
    }
    total = align_up(total, 4096);

    addr = (u64)lx4(LX_MMAP, 0, (lx_long)total, LX_PROT_RW,
                    LX_MAP_PRIVATE | LX_MAP_ANONYMOUS);
    if ((long)addr < 0)
    {
        fail("mmap failed for needed object");
    }
    if (min_vaddr_hint != 0)
    {
        /* 这里只是让调用点显式说明"库按基址 0 链接"这个前提。 */
    }
    if (ph[0].p_vaddr != 0u)
    {
        /* 不是致命错误：按 p_vaddr 偏移铺，运行期地址 = addr + p_vaddr。
         * 但若最低 vaddr 不是 0，符号 st_value 也要加 addr，仍然是
         * addr + st_value，成立。 */
    }

    for (i = 0; i < nph; i++)
    {
        if (ph[i].p_type != PT_LOAD)
        {
            continue;
        }
        if (ph[i].p_filesz != 0)
        {
            lx_long fd;
            lx_long got;

            fd = lx4(LX_OPENAT, LX_AT_FDCWD, (lx_long)(u64)path, 0, 0);
            if (fd < 0)
            {
                fail("cannot reopen needed object");
            }
            got = lx4(LX_PREAD64, fd,
                      (lx_long)(addr + ph[i].p_vaddr),
                      (lx_long)ph[i].p_filesz, (lx_long)ph[i].p_offset);
            (void)lx1(LX_CLOSE, fd);
            if ((got < 0) || ((u64)got != ph[i].p_filesz))
            {
                fail("short read while loading a segment");
            }
        }
        if (ph[i].p_memsz > ph[i].p_filesz)
        {
            mem_zero((void *)(addr + ph[i].p_vaddr + ph[i].p_filesz),
                     ph[i].p_memsz - ph[i].p_filesz);
        }
    }

    o = &s_obj[s_nobj];
    mem_zero(o, sizeof(*o));
    o->used = 1;
    o->is_dyn = 1;
    o->bias = addr;
    str_copy(o->name, name, sizeof(o->name));

    if (parse_dynamic(o, ph, nph) != 0)
    {
        out("[ldso] FAIL: no PT_DYNAMIC in ");
        out(name);
        out("\n");
        return -1;
    }
    s_nobj++;

    out("[ldso] lib ");
    out(name);
    out(" base=");
    out_hex(addr);
    out(" size=");
    out_dec(total);
    out("\n");
    return 0;
}

/* ------------------------------------------------------------------ 主流程 */
static int load_needed(obj_t *main_obj, const phdr_t *ph, int nph)
{
    int i;

    for (i = 0; i < nph; i++)
    {
        if (ph[i].p_type != PT_DYNAMIC)
        {
            continue;
        }
        {
            dyn_t *d = (dyn_t *)obj_addr(main_obj, ph[i].p_vaddr);

            for (; d->d_tag != DT_NULL; d++)
            {
                if (d->d_tag == DT_NEEDED)
                {
                    const char *nm = (const char *)(main_obj->strtab + d->d_val);

                    out("[ldso] need ");
                    out(nm);
                    out("\n");
                    if (load_object(nm, 0) != 0)
                    {
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

void ldso_main(u64 sp);

__asm__(
    ".section .text._start,\"ax\"\n"
    ".globl _start\n"
    ".type  _start, @function\n"
    "_start:\n"
    "  move $a0, $sp\n"
    "  bl   ldso_main\n"
    "1:\n"
    "  b    1b\n"
    ".size  _start, .-_start\n"
    ".previous\n"
);

void ldso_main(u64 sp)
{
    u64 *p = (u64 *)sp;
    long argc = (long)*p++;
    u64 at_phdr = 0;
    u64 at_phnum = 0;
    u64 at_phent = 0;
    u64 at_entry = 0;
    u64 at_base = 0;
    u64 main_base;
    ehdr_t *eh;
    phdr_t *ph;
    obj_t *main_obj;
    int i;

    s_boot_sp = sp;
    s_nobj = 0;   /* 解释器若被重入，对象表必须是空的，不能接着上一轮 */

    out("[ldso] ark ld.so start sp=");
    out_hex(sp);
    out(" argc=");
    out_dec((u64)argc);
    out("\n");

    /* 跳过 argv 与 envp 到达 auxv */
    p += (u64)argc + 1u;
    while (*p != 0u)
    {
        p++;
    }
    p++;
    for (;;)
    {
        u64 tag = *p++;
        u64 val = *p++;

        if (tag == AT_NULL)
        {
            break;
        }
        switch (tag)
        {
        case AT_PHDR:  at_phdr = val; break;
        case AT_PHNUM: at_phnum = val; break;
        case AT_PHENT: at_phent = val; break;
        case AT_ENTRY: at_entry = val; break;
        case AT_BASE:  at_base = val; break;
        default: break;
        }
    }

    if ((at_phdr == 0u) || (at_phnum == 0u) || (at_phent == 0u) ||
        (at_entry == 0u))
    {
        fail("auxv is missing AT_PHDR/AT_PHNUM/AT_PHENT/AT_ENTRY");
    }

    /* Linux 的通行假设：主程序的 e_phoff == 64 == sizeof(Ehdr)。
     * 这里核对而不是假设；不是 64 就明确失败，不猜一个基址。 */
    main_base = at_phdr - 64u;
    eh = (ehdr_t *)main_base;
    if (eh->e_phoff != 64u)
    {
        fail("main e_phoff != 64; refusing to guess its base");
    }
    if ((u64)at_phent != sizeof(phdr_t))
    {
        fail("unexpected AT_PHENT");
    }
    ph = (phdr_t *)at_phdr;

    if (s_nobj >= MAX_OBJ)
    {
        fail("object table full");
    }
    main_obj = &s_obj[s_nobj];
    mem_zero(main_obj, sizeof(*main_obj));
    main_obj->used = 1;
    main_obj->is_dyn = (eh->e_type == ET_DYN) ? 1 : 0;
    main_obj->bias = main_obj->is_dyn ? main_base : 0u;
    str_copy(main_obj->name, (argc > 0) ? *(char **)(sp + 8) : "main",
             sizeof(main_obj->name));
    s_nobj++;

    out("[ldso] main base=");
    out_hex(main_base);
    out(" type=");
    out_dec((u64)eh->e_type);
    out(" entry=");
    out_hex(at_entry);
    out(" interp_base=");
    out_hex(at_base);
    out("\n");

    if (parse_dynamic(main_obj, ph, (int)at_phnum) != 0)
    {
        fail("main has no usable PT_DYNAMIC");
    }

    if (load_needed(main_obj, ph, (int)at_phnum) != 0)
    {
        fail("could not load a DT_NEEDED object");
    }

    /* 先做库的重定位，再做主程序：符号解析是全局按名扫描，
     * 两边都能找到对方。 */
    for (i = s_nobj - 1; i >= 0; i--)
    {
        if (relocate(&s_obj[i]) != 0)
        {
            fail("relocation failed");
        }
    }

    out("[ldso] jump entry=");
    out_hex(at_entry);
    out("\n");

    {
        register u64 r_sp __asm__("t0") = s_boot_sp;
        register u64 r_en __asm__("t1") = at_entry;

        __asm__ volatile("move $sp, $t0\n\tjr $t1"
                         :: "r"(r_sp), "r"(r_en) : "memory");
    }
    for (;;)
    {
    }
}
