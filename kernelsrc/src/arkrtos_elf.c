/**
* @file arkrtos_elf.c
* @brief ELF 装载器（32/64 位）：把 VFS 里的可执行文件搬进 App 窗口并降权起跑
* @details 装载是**流式**的：先读 64 字节 ELF 头，再按 e_phoff 读程序头表，
*          最后按每个 PT_LOAD 的 p_offset 用 arkrtos_vfs_read_at() 直接读进
*          目标地址。整份镜像从不进临时缓冲——窗口本身就只有 2 MB，
*          再备一份同尺寸的拷件既没必要，也会在 8 MB 内核 RAM 上挤掉别的东西。
*
*          为什么入口约定用 Linux 那一套（sp 指向 argc）而不是传 a0/a1：
*          任务创建路径最后落到 arkrtos_task_stack_init()，它把初始 sp 设成
*          stack_bottom + stack_size 并按 16 字节对齐。所以只要让
*          「stack_size = argv 区起点 - 栈底」，初始 sp 就正好指向 argc。
*          这样任何按 Linux crt 写的静态程序都不用为 Ark 改一行。
*
*          三条边界写死在这里，不做「尽力而为」：
*            1. 只收 ET_EXEC 静态 ELF64（PT_INTERP 一律拒绝：本仓没有动态链接器，
*               半跑起来的动态程序会在第一条 PLT 指令上给一个看不懂的 refill）；
*            2. 段必须整段落在 [window_base, stack_bottom)：越过栈底就是覆盖
*               自己的栈，那不是一个可以「再想想」的情况；
*            3. p_filesz > p_memsz 直接拒绝——按 memsz 清零会把文件外的字节
*               当成数据，按 filesz 拷又会写过段尾，两种猜法都是错的。
*
* @note 开关：ARKRTOS_USE_ELF。关闭时本文件是空目标（零体积零符号），
*       打开时要求板级给出 ARKRTOS_ELF_WINDOW_BASE/SIZE，缺一个就 #error。
* @author xw
*/

#include "arkrtos_config.h"

#if (ARKRTOS_USE_ELF == 1)

#include "arkrtos_elf.h"
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"
#include "arkrtos_vfs.h"
#if (ARKRTOS_USE_EXEC_SIGN == 1)
#include "arkrtos_sign.h"
#endif
#if ((ARKRTOS_USE_PAGING == 1) || (ARKRTOS_USE_MPU == 1))
#include "arkrtos_mpu.h"
#endif

/* ---------------------------------------------------------------- 编译期 */

#ifndef ARKRTOS_ELF_WINDOW_BASE
#error "ARKRTOS_USE_ELF=1 需要板级给出 ARKRTOS_ELF_WINDOW_BASE（可执行文件能落进去的地址）"
#endif
#ifndef ARKRTOS_ELF_WINDOW_SIZE
#error "ARKRTOS_USE_ELF=1 需要板级给出 ARKRTOS_ELF_WINDOW_SIZE（这块窗口有多大）"
#endif
#if (ARKRTOS_USE_PRIV != 1)
#error "ARKRTOS_USE_ELF=1 需要 ARKRTOS_USE_PRIV=1：ELF 程序必须以非特权任务起跑，否则隔离为空"
#endif

/* 栈与参数区从窗口顶端往下切。两者都可由板级覆盖，因为窗口大小是板级事实。 */
#ifndef ARKRTOS_ELF_STACK_SIZE
#define ARKRTOS_ELF_STACK_SIZE      (32u * 1024u)
#endif
#ifndef ARKRTOS_ELF_ARGV_SIZE
#define ARKRTOS_ELF_ARGV_SIZE       (16u * 1024u)
#endif
#ifndef ARKRTOS_ELF_TASK_PRIORITY
#define ARKRTOS_ELF_TASK_PRIORITY   (12u)
#endif

#define ARKRTOS_ELF_MAX_ARGS        (16u)
#define ARKRTOS_ELF_MAX_ARG_LEN     (256u)
#define ARKRTOS_ELF_MAX_PHNUM       (16u)

/* ELF 常量。只用得到的这几个，不引 <elf.h>：内核不依赖宿主头。 */
#define ELF_EI_CLASS32              (1u)
#define ELF_EI_CLASS64              (2u)
#define ELF_EI_DATA_LSB             (1u)
#define ELF_TYPE_EXEC               (2u)
#define ELF_MACHINE_LOONGARCH       (258u)
#define ELF_MACHINE_RISCV           (243u)
#define ELF_PT_LOAD                 (1u)
#define ELF_PT_INTERP               (3u)

/* ---- 字段宽度随 class 变，语义不变 ----
 *
 * ELF32 与 ELF64 的差别只在这几个数上：头 52/64 字节、程序头 32/56 字节、
 * 地址字段 4/8 字节、argv 槽 4/8 字节。**装载流程一行都不分叉**，所以这里
 * 用一份 sizeof 宏表把差别收进来，下面的代码全部按 elf_word_t 写。
 *
 * class 的判据是 port 的 long 宽度：64 位 port 装 ELF64（LA64），32 位 port
 * 装 ELF32（RV32）。机器号另行给：32 位默认 RISC-V(243)，板级可覆盖。 */
#if (defined(__SIZEOF_LONG__) && (__SIZEOF_LONG__ > 4))
#define ELF_HOST_64                 (1)
typedef unsigned long elf_word_t;
#define ELF_EHDR_SIZE               (64u)
#define ELF_PHDR_SIZE               (56u)
#define ELF_WORD_SIZE               (8u)
#define ELF_EH_ENTRY                (24u)
#define ELF_EH_PHOFF                (32u)
#define ELF_EH_PHENTSIZE            (54u)
#define ELF_EH_PHNUM                (56u)
#define ELF_PH_OFFSET               (8u)
#define ELF_PH_VADDR                (16u)
#define ELF_PH_FILESZ               (32u)
#define ELF_PH_MEMSZ                (40u)
#else
#define ELF_HOST_64                 (0)
typedef uint32 elf_word_t;
#define ELF_EHDR_SIZE               (52u)
#define ELF_PHDR_SIZE               (32u)
#define ELF_WORD_SIZE               (4u)
#define ELF_EH_ENTRY                (24u)
#define ELF_EH_PHOFF                (28u)
#define ELF_EH_PHENTSIZE            (42u)
#define ELF_EH_PHNUM                (44u)
#define ELF_PH_OFFSET               (4u)
#define ELF_PH_VADDR                (8u)
#define ELF_PH_FILESZ               (16u)
#define ELF_PH_MEMSZ                (20u)
#endif

/* 期望的机器号。32 位的 LoongArch（LA32）是 258 号 + 32 位 class，
 * 板级把 ARKRTOS_ELF_MACHINE 覆盖过去即可，不改这里。 */
#ifndef ARKRTOS_ELF_MACHINE
#if (ELF_HOST_64 == 1)
#define ARKRTOS_ELF_MACHINE         ELF_MACHINE_LOONGARCH
#else
#define ARKRTOS_ELF_MACHINE         ELF_MACHINE_RISCV
#endif
#endif

#if (ELF_HOST_64 == 1)
#define ELF_EI_CLASS_OK             ELF_EI_CLASS64
#else
#define ELF_EI_CLASS_OK             ELF_EI_CLASS32
#endif

/* 小端读。字段宽度由 class 决定（见上面的 ELF_WORD_SIZE 表）：
 * 32 位 port 上 elf_rd64/elf_wr64 整个不参与编译 —— 那两处 `<< 32`
 * 在 32 位 unsigned long 上是未定义行为，不能只是"不调用"。 */
/* 16/32 位的读写**两档都要**：ELF32 的字段、SVC 参数块、程序头里的
 * e_phentsize 全是 32 位。它们与 port 宽度无关，所以放在条件外。
 * （曾经整段包在 ELF_HOST_64 里，32 位 port 上 elf_rd32 变成隐式声明，
 *  编译不报错、链接期才报 undefined reference —— 症状看起来像"函数忘了
 *  写"，实际是条件编译把定义吃掉了。） */
static uint32 elf_rd32(const uint8 *p)
{
    return ((uint32)p[0])
         | ((uint32)p[1] << 8)
         | ((uint32)p[2] << 16)
         | ((uint32)p[3] << 24);
}

static uint16 elf_rd16(const uint8 *p)
{
    return (uint16)(((uint16)p[0]) | ((uint16)p[1] << 8));
}

static void elf_wr32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
    p[2] = (uint8)((v >> 16) & 0xFFu);
    p[3] = (uint8)((v >> 24) & 0xFFu);
}

/* 64 位读写在 32 位 port 上必须整个不参与编译：`<< 32` 在 32 位
 * unsigned long 上是未定义行为，不能只是"不调用"。 */
#if (ELF_HOST_64 == 1)

static elf_word_t elf_rd64(const uint8 *p)
{
    return ((unsigned long)p[0])
         | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16)
         | ((unsigned long)p[3] << 24)
         | ((unsigned long)p[4] << 32)
         | ((unsigned long)p[5] << 40)
         | ((unsigned long)p[6] << 48)
         | ((unsigned long)p[7] << 56);
}

static void elf_wr64(uint8 *p, elf_word_t v)
{
    elf_wr32(p, (uint32)(v & 0xFFFFFFFFu));
    elf_wr32(p + 4, (uint32)((v >> 32) & 0xFFFFFFFFu));
}

#endif /* ELF_HOST_64 */

/* 一个 elf_word_t 的读写：64 位 port 走上面两个，32 位 port 走 elf_rd32/
 * elf_wr32。下面所有代码只认这两个名字。 */
static elf_word_t elf_rdword(const uint8 *p)
{
#if (ELF_HOST_64 == 1)
    return elf_rd64(p);
#else
    return (elf_word_t)elf_rd32(p);
#endif
}

static void elf_wrword(uint8 *p, elf_word_t v)
{
#if (ELF_HOST_64 == 1)
    elf_wr64(p, v);
#else
    elf_wr32(p, (uint32)v);
#endif
}

/* 内核侧没有 libc：这三个小工具只在本文件用得到，不往外放。 */
static uint32 elf_strlen(const char *s)
{
    uint32 n = 0u;

    while(s[n] != '\0')
    {
        n++;
    }
    return n;
}

static void elf_memzero(uint8 *p, uint32 n)
{
    uint32 i;

    for(i = 0u; i < n; i++)
    {
        p[i] = 0u;
    }
}

static void elf_memcpy(uint8 *dst, const uint8 *src, uint32 n)
{
    uint32 i;

    for(i = 0u; i < n; i++)
    {
        dst[i] = src[i];
    }
}

/* ---------------------------------------------------------------- 状态 */

/* 实例台账，下标是地址空间号。
 *
 * U-2（2026-11-17）之前这里只有一个 s_elf_task，因为窗口只有一块，两个程序
 * 同时在一个地址空间里跑没有意义。分页后端给每块窗口一个地址空间之后，
 * 「窗口只有一块」这个前提不再成立：同一 VA 在每个空间里各指向一块物理帧，
 * 所以每个空间各自记一份实例，互不覆盖。
 *
 * 不开分页的构建里空间数就是 1，台账退化成原来的单槽，行为与以前逐位一致。 */
#if (ARKRTOS_USE_PAGING == 1)
#ifndef ARKRTOS_PAGING_APPS_MAX
#define ARKRTOS_PAGING_APPS_MAX     1u
#endif
#define ELF_INST_MAX                ARKRTOS_PAGING_APPS_MAX
#else
#define ELF_INST_MAX                1u
#endif

static int32 s_elf_task[ELF_INST_MAX];              /* 0 = 该空间空着 */
static arkrtos_elf_info_t s_elf_info[ELF_INST_MAX]; /* 该空间最后一次装载的读数 */
static int32 s_elf_last = -1;                       /* 最近一次装载落在哪个空间 */

/* ------------------------------------------------------------ 实例台账 */

/* 任务退出后 status 回到 INVALID。懒清理：不要求退出路径回调进来，
 * 代价是一次数组读，和原来的单实例版本同量级。 */
static int32 elf_slot_live(uint32 space)
{
    int32 id;

    if(space >= (uint32)ELF_INST_MAX)
    {
        return 0;
    }

    id = s_elf_task[space];
    if(id <= 0)
    {
        return 0;
    }

    if(arkrtos_task_table[id - 1].status == ARKRTOS_TASK_INVALID)
    {
#if (ARKRTOS_USE_EXIT_CODE == 1)
        /* U-8：任务死了但 TCB 还没被复用，这是唯一能安全取退出码的时刻；
         * 取走之后再清槽位。没上报过就保持 exit_code_set=0，不编 0。 */
        {
            int32 code = 0;

            if(arkrtos_task_exit_code(id, &code) != 0)
            {
                s_elf_info[space].exit_code = code;
                s_elf_info[space].exit_code_set = 1u;
            }
        }
#endif
        s_elf_task[space] = 0;
        return 0;
    }

    return 1;
}

/* 可用空间数 = min(编译期台账长度, 端口真正提供的空间数)。板级把
 * ARKRTOS_PAGING_APPS_MAX 写大了而端口只有一块窗口时，按端口算，不猜。 */
static uint32 elf_space_capacity(void)
{
#if (ARKRTOS_USE_PAGING == 1)
    uint32 n = arkrtos_paging_space_count();

    return (n < (uint32)ELF_INST_MAX) ? n : (uint32)ELF_INST_MAX;
#else
    return 1u;
#endif
}

/* ---------------------------------------------------------------- 内部 */

static int32 elf_read_exact(const char *path, uint8 *buf, uint32 len, uint32 off)
{
    uint32 got = 0u;
    int32 rc;

    if(len == 0u)
    {
        return ARKRTOS_ELF_OK;
    }

    rc = arkrtos_vfs_read_at(path, buf, len, off, &got);
    if(rc < 0)
    {
        return ARKRTOS_ELF_ERR_FS;
    }
    /* 短读 = 文件比程序头声明的短。这不是「容错一下」的场景：
     * 少读的字节会被当成上一个程序留下的数据执行。 */
    if(got != len)
    {
        return ARKRTOS_ELF_ERR_FS;
    }
    return ARKRTOS_ELF_OK;
}

#if (ARKRTOS_USE_PAGING == 1)
/* 往窗口里落字节必须「先装对空间、再落笔」。
 *
 * 为什么不能像以前那样把窗口地址直接交给 VFS 读：拷贝期间可能被抢占，
 * 调度器切到别的 App 时会把这个 VA 重装成那个任务的帧，回来以后同一个 VA
 * 指的是别人的内存。所以数据先读进内核暂存（完全不碰窗口），装好空间之后
 * 再 memcpy 进去；memcpy 这段没有阻塞调用，中途不会被切走。 */
#define ELF_COPY_CHUNK              (4096u)

static uint8 s_elf_stage[ELF_COPY_CHUNK];

static int32 elf_copy_to_window(uint32 space, const char *path, uint32 va,
                                uint32 len, uint32 off)
{
    uint32 done = 0u;

    while(done < len)
    {
        uint32 n = len - done;
        int32 rc;

        if(n > (uint32)ELF_COPY_CHUNK)
        {
            n = (uint32)ELF_COPY_CHUNK;
        }

        rc = elf_read_exact(path, s_elf_stage, n, off + done);
        if(rc != ARKRTOS_ELF_OK)
        {
            return rc;
        }

        (void)arkrtos_paging_space_activate(space);
        elf_memcpy((uint8 *)(unsigned long)(va + done), s_elf_stage, n);
        done += n;
    }

    return ARKRTOS_ELF_OK;
}

static void elf_zero_window(uint32 space, uint32 va, uint32 len)
{
    if(len == 0u)
    {
        return;
    }

    (void)arkrtos_paging_space_activate(space);
    elf_memzero((uint8 *)(unsigned long)va, len);
}
#endif /* ARKRTOS_USE_PAGING */


#if (ARKRTOS_USE_EXEC_SIGN == 1)
/* 尾部 32 字节签名 tag：tag 覆盖 [0, size-32)。
 * ELF 装载只按 p_offset 读段，尾部这 32 字节永远不参与装载；但它们必须在
 * 这里被"读过并按 MAC 核过"——否则一个改过的程序照样能起跑。 */
static int32 elf_verify_signature(const char *path)
{
    uint8  tag[ARKRTOS_SIGN_LEN];
    uint8  buf[512];
    uint32 size = 0u;
    uint32 body;
    uint32 off = 0u;
    int32  rc;

    rc = arkrtos_vfs_size(path, &size);
    if(rc < 0)
    {
        return ARKRTOS_ELF_ERR_FS;
    }
    if(size <= (uint32)ARKRTOS_SIGN_LEN)
    {
        /* 比 tag 还短：不可能既装得下 tag 又装得下 ELF。只能说"没有签名"。 */
        return ARKRTOS_ELF_ERR_SIGN;
    }
    body = size - (uint32)ARKRTOS_SIGN_LEN;

    rc = elf_read_exact(path, tag, (uint32)ARKRTOS_SIGN_LEN, body);
    if(rc != ARKRTOS_ELF_OK)
    {
        return rc;
    }

    arkrtos_sign_payload_begin();
    while(off < body)
    {
        uint32 n = body - off;

        if(n > (uint32)sizeof(buf))
        {
            n = (uint32)sizeof(buf);
        }
        rc = elf_read_exact(path, buf, n, off);
        if(rc != ARKRTOS_ELF_OK)
        {
            return rc;
        }
        arkrtos_sign_payload_update(buf, n);
        off += n;
    }
    if(arkrtos_sign_payload_end(tag) != 0)
    {
        return ARKRTOS_ELF_ERR_SIGN;
    }
    return ARKRTOS_ELF_OK;
}
#endif /* ARKRTOS_USE_EXEC_SIGN */

/**
* @brief 校验 ELF 头，把 entry / phoff / phnum / phentsize 取出来
* @note  e_machine 单独返回 ERR_ARCH，是为了让「这是 x86 的 ELF」和
*        「这不是 ELF」在日志里能分开——两者的修法完全不同。
*/
static int32 elf_check_header(const uint8 *h, elf_word_t *p_entry,
                              elf_word_t *p_phoff, uint16 *p_phnum,
                              uint16 *p_phentsize)
{
    if((h[0] != 0x7Fu) || (h[1] != 'E') || (h[2] != 'L') || (h[3] != 'F'))
    {
        return ARKRTOS_ELF_ERR_FORMAT;
    }
    if((h[4] != ELF_EI_CLASS_OK) || (h[5] != ELF_EI_DATA_LSB))
    {
        /* class 与本 port 的宽度不匹配：64 位 port 装 ELF32 会按 8 字节
         * 读字段，得到的是两个字段拼起来的垃圾 —— 必须在写任何字节之前拒。 */
        return ARKRTOS_ELF_ERR_FORMAT;
    }
    if(elf_rd16(h + 18) != (uint16)ARKRTOS_ELF_MACHINE)
    {
        return ARKRTOS_ELF_ERR_ARCH;
    }
    if(elf_rd16(h + 16) != (uint16)ELF_TYPE_EXEC)
    {
        /* ET_DYN 需要按加载基址重定位所有绝对地址，本仓没有重定位器；
         * 混进来只能得到一堆指向 0 的调用。 */
        return ARKRTOS_ELF_ERR_FORMAT;
    }

    *p_entry     = elf_rdword(h + ELF_EH_ENTRY);
    *p_phoff     = elf_rdword(h + ELF_EH_PHOFF);
    *p_phentsize = elf_rd16(h + ELF_EH_PHENTSIZE);
    *p_phnum     = elf_rd16(h + ELF_EH_PHNUM);

    if((*p_phentsize != (uint16)ELF_PHDR_SIZE) ||
       (*p_phnum == 0u) || (*p_phnum > (uint16)ARKRTOS_ELF_MAX_PHNUM))
    {
        return ARKRTOS_ELF_ERR_FORMAT;
    }
    return ARKRTOS_ELF_OK;
}

/**
* @brief 把 argv 铺成 Linux 的形状，返回初始 sp
* @details 布局（地址从高到低）：
*            字符串区（argv[i] 的字节）
*            NULL(envp)、NULL(argv 结束)、argv[argc-1..0]、argc  <- sp
*          字符串从窗口顶端往下写，数组紧贴在 argv_start 之上。
* @return 0 = 成功（*p_sp 有效）；否则为负错误码
*/
static int32 elf_build_stack(uint32 win_top, uint32 argv_start,
                             int32 argc, char *argv[], uint32 *p_sp)
{
    uint32 str_low = win_top;
    uint32 addrs[ARKRTOS_ELF_MAX_ARGS];
    uint32 array_bytes;
    uint32 i;
    uint8 *p_array;

    /* 先放字符串，并把每个字符串的最终地址记下来。
     * 不能事后用「win_top 减长度累加」重算：8 字节对齐会额外下移，
     * 重算出的地址与真正写入的位置差几个字节，程序读到的 argv[i]
     * 就是别人字符串的后半截。 */
    for(i = 0u; i < (uint32)argc; i++)
    {
        uint32 n = elf_strlen(argv[i]);

        if(n >= ARKRTOS_ELF_MAX_ARG_LEN)
        {
            return ARKRTOS_ELF_ERR_PARAM;
        }
        str_low -= (n + 1u);
        str_low &= ~0x7u;
        elf_memcpy((uint8 *)(unsigned long)str_low, (const uint8 *)argv[i], n + 1u);
        addrs[i] = str_low;
    }

    /* 数组：argc + argv[argc] + envp[0] + 一个冗余槽，按本 class 的字宽。 */
    array_bytes = (uint32)ELF_WORD_SIZE * ((uint32)argc + 3u);
    if((argv_start + array_bytes) > str_low)
    {
        return ARKRTOS_ELF_ERR_LAYOUT;
    }

    p_array = (uint8 *)(unsigned long)argv_start;
    elf_wrword(p_array, (elf_word_t)(uint32)argc);
    for(i = 0u; i < (uint32)argc; i++)
    {
        elf_wrword(p_array + ELF_WORD_SIZE + ((uint32)ELF_WORD_SIZE * i),
                   (elf_word_t)addrs[i]);
    }
    elf_wrword(p_array + ELF_WORD_SIZE + ((uint32)ELF_WORD_SIZE * (uint32)argc), 0);
    elf_wrword(p_array + ELF_WORD_SIZE +
               ((uint32)ELF_WORD_SIZE * ((uint32)argc + 1u)), 0);

    *p_sp = argv_start;
    return ARKRTOS_ELF_OK;
}

/* ---------------------------------------------------------------- 对外 */

/**
* @brief 把一份 ELF64 装进指定地址空间并起一个降权任务
* @param space   目标地址空间（0 .. 空间数-1）；窗口 VA 在每个空间里相同
* @param p_info  成功时填装载读数（不接收失败值）
* @details 空间选择、台账登记、以及"装载完把内核任务原来的视图装回去"
*          都在 arkrtos_elf_exec_space() 里；这里只做"把这份文件装进这一块帧"。
*/
static int32 elf_load(uint32 space, const char *path, int32 argc,
                      char *argv[], arkrtos_elf_info_t *p_info)
{
    uint8 hdr[ELF_EHDR_SIZE];
    uint8 phdr[ELF_PHDR_SIZE];
    elf_word_t entry = 0;
    elf_word_t phoff = 0;
    uint16 phnum = 0u;
    uint16 phentsize = 0u;
    uint32 win_base = (uint32)ARKRTOS_ELF_WINDOW_BASE;
    uint32 win_top;
    uint32 argv_start;
    uint32 stack_bottom;
    uint32 stack_bytes;
    uint32 bytes = 0u;
    uint32 segs = 0u;
    uint32 sp = 0u;
    uint32 i;
    int32 rc;
    int32 id;
    uint32 crit;
    char *argv_auto[1];

    if((path == 0) || (path[0] == '\0'))
    {
        return ARKRTOS_ELF_ERR_PARAM;
    }

#if (ARKRTOS_USE_EXEC_SIGN == 1)
    /* 签名先于一切：格式都还没看就先确认这份文件是我们签过的。
     * 顺序有讲究——放在参数检查之后、读 ELF 头之前，避免"格式错误"的
     * 报错把"文件没签名"这件事盖掉。 */
    rc = elf_verify_signature(path);
    if(rc != ARKRTOS_ELF_OK)
    {
        return rc;
    }
#endif
    if(argc < 0)
    {
        return ARKRTOS_ELF_ERR_PARAM;
    }
    if(argc > (int32)ARKRTOS_ELF_MAX_ARGS)
    {
        return ARKRTOS_ELF_ERR_PARAM;
    }
    /* argv[0] 缺省 = 程序路径：Linux 的 shell 也这么补，
     * 而程序自己按 argv[0] 判断「我是谁」是常见写法，不能给 0。 */
    if((argc == 0) || (argv == 0))
    {
        argv_auto[0] = (char *)path;
        argv = argv_auto;
        argc = 1;
    }
    for(i = 0u; i < (uint32)argc; i++)
    {
        if(argv[i] == 0)
        {
            return ARKRTOS_ELF_ERR_PARAM;
        }
    }

    win_top      = win_base + (uint32)ARKRTOS_ELF_WINDOW_SIZE;
    argv_start   = (win_top - (uint32)ARKRTOS_ELF_ARGV_SIZE) & ~0xFu;
    stack_bottom = argv_start - (uint32)ARKRTOS_ELF_STACK_SIZE;
    if((stack_bottom <= win_base) ||
       (argv_start <= stack_bottom) ||
       (win_top <= argv_start))
    {
        /* 窗口小到连栈+参数区都放不下：这是板级配置错误，不是程序问题。 */
        return ARKRTOS_ELF_ERR_LAYOUT;
    }

    rc = elf_read_exact(path, hdr, ELF_EHDR_SIZE, 0u);
    if(rc != ARKRTOS_ELF_OK)
    {
        return rc;
    }
    rc = elf_check_header(hdr, &entry, &phoff, &phnum, &phentsize);
    if(rc != ARKRTOS_ELF_OK)
    {
        return rc;
    }


    /* 入口必须落在窗口内且 4 字节对齐：LoongArch 的指令永远是 4 字节。 */
/* 入口必须落在窗口内且 4 字节对齐：RV32 与 LoongArch 的指令都是 4 字节
 * （RV32 的压缩扩展只让指令可能变短，不会让入口变半字对齐）。 */
    if((entry < (elf_word_t)win_base) ||
       (entry > ((elf_word_t)stack_bottom - 4u)) ||
       ((entry & 3u) != 0u))
    {
        return ARKRTOS_ELF_ERR_LAYOUT;
    }

    /* 第一遍：只校验。任何一段不合法都在**写坏字节之前**返回，
     * 否则一个坏镜像会留下半个程序，下一次装载读到的是混合内容。 */
    for(i = 0u; i < (uint32)phnum; i++)
    {
        elf_word_t p_type;
        elf_word_t p_vaddr;
        elf_word_t p_filesz;
        elf_word_t p_memsz;

        rc = elf_read_exact(path, phdr, ELF_PHDR_SIZE,
                            (uint32)phoff + (i * (uint32)phentsize));
        if(rc != ARKRTOS_ELF_OK)
        {
            return rc;
        }

        p_type   = (elf_word_t)elf_rd32(phdr + 0);
        p_vaddr  = elf_rdword(phdr + ELF_PH_VADDR);
        p_filesz = elf_rdword(phdr + ELF_PH_FILESZ);
        p_memsz  = elf_rdword(phdr + ELF_PH_MEMSZ);

        if(p_type == (unsigned long)ELF_PT_INTERP)
        {
            /* 有解释器 = 动态链接。没有 ld.so，跑起来只会跳到 PLT 桩。 */
            return ARKRTOS_ELF_ERR_FORMAT;
        }
        if(p_type != (unsigned long)ELF_PT_LOAD)
        {
            continue;
        }
        if(p_filesz > p_memsz)
        {
            return ARKRTOS_ELF_ERR_LAYOUT;
        }
        /* 用减法写这个判据，不用 p_vaddr + p_memsz：32 位 port 上那个加法
         * 会在 0xFFFFFFxx 处回绕，一段"跨过 4 GB 边界"的坏镜像就能通过。 */
        if((p_vaddr < (elf_word_t)win_base) ||
           (p_vaddr > (elf_word_t)stack_bottom) ||
           (p_memsz > ((elf_word_t)stack_bottom - p_vaddr)))
        {
            return ARKRTOS_ELF_ERR_LAYOUT;
        }
        segs++;
    }

    if(segs == 0u)
    {
        /* 没有 PT_LOAD 的 ELF 是合法的（目标文件/调试信息），但没有东西可跑。 */
        return ARKRTOS_ELF_ERR_FORMAT;
    }

    /* 第二遍：真写。先 filesz 拷数据，再 memsz-filesz 清零（BSS）。
     * 顺序不能反：清零写在拷贝之前会被后面的数据覆盖，写在之后才是对的。 */
    bytes = 0u;
    for(i = 0u; i < (uint32)phnum; i++)
    {
        elf_word_t p_type;
        elf_word_t p_offset;
        elf_word_t p_vaddr;
        elf_word_t p_filesz;
        elf_word_t p_memsz;
        uint8 *dst;

        rc = elf_read_exact(path, phdr, ELF_PHDR_SIZE,
                            (uint32)phoff + (i * (uint32)phentsize));
        if(rc != ARKRTOS_ELF_OK)
        {
            return rc;
        }

        p_type   = elf_rd32(phdr + 0);
        if(p_type != (unsigned long)ELF_PT_LOAD)
        {
            continue;
        }
        p_offset = elf_rdword(phdr + ELF_PH_OFFSET);
        p_vaddr  = elf_rdword(phdr + ELF_PH_VADDR);
        p_filesz = elf_rdword(phdr + ELF_PH_FILESZ);
        p_memsz  = elf_rdword(phdr + ELF_PH_MEMSZ);
        dst      = (uint8 *)(unsigned long)p_vaddr;
#if (ARKRTOS_USE_PAGING == 1)
        (void)dst;   /* 分页档不直接用窗口指针：统一走 elf_copy_to_window() */
#endif

        if(p_filesz > 0ul)
        {
#if (ARKRTOS_USE_PAGING == 1)
            /* 这一段必须落在 space 的那块帧里：elf_copy_to_window() 每写
             * 4 KB 之前重装一次空间，抢占把窗口换走也不会写错帧。 */
            rc = elf_copy_to_window(space, path, (uint32)p_vaddr,
                                    (uint32)p_filesz, (uint32)p_offset);
#else
            rc = elf_read_exact(path, dst, (uint32)p_filesz, (uint32)p_offset);
#endif
            if(rc != ARKRTOS_ELF_OK)
            {
                return rc;
            }
            bytes += (uint32)p_filesz;
        }
        if(p_memsz > p_filesz)
        {
#if (ARKRTOS_USE_PAGING == 1)
            elf_zero_window(space, (uint32)p_vaddr + (uint32)p_filesz,
                            (uint32)(p_memsz - p_filesz));
#else
            elf_memzero(dst + p_filesz, (uint32)(p_memsz - p_filesz));
#endif
        }
    }

#if (ARKRTOS_USE_PAGING == 1)
    /* 用户栈与 argv 也在窗口里，铺之前同样把空间装对。elf_build_stack()
     * 里没有阻塞调用，中途不会被切走。 */
    (void)arkrtos_paging_space_activate(space);
#endif
    rc = elf_build_stack(win_top, argv_start, argc, argv, &sp);
    if(rc != ARKRTOS_ELF_OK)
    {
        return rc;
    }

    stack_bytes = argv_start - stack_bottom;

    /* 关键一步：stack_size 取到 argv 区起点，初始 sp 就会落在 argc 上。
     * 这里若改成「传栈大小 32 KB 再多给 16 KB」，sp 会指向参数数组中段，
     * 程序读到的 argc 是某个字符串的字节——一个只在运行期才暴露的错答案。 */
    /* 装配期竞态（本轮实测根因之一）：arkrtos_task_register() 内部会把任务
     * 直接挂进就绪集，而「降权」与「MPU 窗口写回」是它之后才发生的两步。
     * 若在这中间被 tick 切走，任务会以半配置状态被切进：跑在 M 模式看到的是
     * 特权，降权后窗口未写回看到的就是 mcause=1 mtval=0x02000000 的取指 fault
     * （5 连跑 2 败的间歇签名）。因此把 注册 → 降权 → MPU 写回 整段做成原子段。 */
    crit = ARKRTOS_ENTER_CRITICAL();
    id = arkrtos_task_register((void (*)(void))(unsigned long)entry,
                               (uint32 *)(unsigned long)stack_bottom,
                               stack_bytes,
                               (uint8)ARKRTOS_ELF_TASK_PRIORITY, 0u);
    if(id <= 0)
    {
        ARKRTOS_EXIT_CRITICAL(crit);
        return ARKRTOS_ELF_ERR_NOMEM;
    }

    /* 注册出来的任务默认是特权任务；ELF 程序必须降权，否则隔离为空。 */
    arkrtos_task_table[id - 1].is_priv = 0u;

#if (ARKRTOS_USE_MPU == 1)
    /* 无分页档（RV32 的 PMP）：U 模式任务能碰的只有 TCB 记的这几个窗口。
     * 装载器必须把「代码窗口 = 数据窗口 = 这一块可执行 RAM」写回 TCB，
     * 再重建区域集 —— 否则任务拿着内核默认窗口起跑：轻则一取指就 fault，
     * 重则整块内核 RAM 对它可读可写，隔离等于没开。 */
    {
        arkrtos_task_t *p_task = &arkrtos_task_table[id - 1];

        p_task->rom_start = win_base;
        p_task->rom_size  = (uint32)ARKRTOS_ELF_WINDOW_SIZE;
        p_task->ram_start = win_base;
        p_task->ram_size  = (uint32)ARKRTOS_ELF_WINDOW_SIZE;
        arkrtos_mpu_build_task(p_task);
    }
#endif

#if (ARKRTOS_USE_PAGING == 1)
    /* 任务必须绑到「段被写进去的那个空间」：否则写的是 frame A、跑的是
     * frame B —— 程序一取指就落进别人的帧。绑定只记一个号，真正切换
     * 发生在下一次调度到它的时候。 */
    (void)arkrtos_paging_bind_task(&arkrtos_task_table[id - 1], space);
#endif

    /* 原子段落在这里收口：此后任务已降权、窗口已写回、帧已绑定，
     * 无论调度器在下一条指令切进来，看到的都是一个完整可跑的上下文。 */
    ARKRTOS_EXIT_CRITICAL(crit);

    p_info->entry      = (uint32)entry;
    p_info->bytes      = bytes;
    p_info->segs       = segs;
    p_info->argc       = (uint32)argc;
    p_info->sp         = sp;
    p_info->stack_size = stack_bytes;
    p_info->task       = id;

    return ARKRTOS_ELF_OK;
}

/**
* @brief 装一个 ELF 程序，返回它落到哪个地址空间
* @param path     VFS 路径
* @param argc     argv 个数（0 或缺省 argv 时按 Linux 习惯填成 path）
* @param argv     argv 数组
* @param p_space  可为 0；成功时写回这次装载用的空间号
* @return ARKRTOS_ELF_OK / 负错误码（含 ARKRTOS_ELF_ERR_BUSY：空间全占着）
* @details U-2（2026-11-17）：装载不再假设"只有一块窗口"。先挑一个没有
*          活实例的空间，把它装进窗口，把段写进那块帧，任务绑上去，然后把
*          内核任务原来的视图装回去。挑空间只看台账，不看地址：窗口 VA
*          在所有空间里都是同一个值，这个值本身不携带任何身份。
*/
int32 arkrtos_elf_exec_space(const char *path, int32 argc, char *argv[],
                             uint32 *p_space)
{
    uint32 space;
    uint32 prev = 0u;
    int32 rc;
    arkrtos_elf_info_t info;

    if(p_space != 0)
    {
        *p_space = 0u;
    }

    /* 空间全占着时返回 BUSY —— 与 U-1 之前"窗口被占"是同一个语义，
     * 只是判据从"有没有一个实例"变成"还有没有空空间"。 */
    for(space = 0u; space < elf_space_capacity(); space++)
    {
        if(elf_slot_live(space) == 0)
        {
            break;
        }
    }
    if(space >= elf_space_capacity())
    {
        return ARKRTOS_ELF_ERR_BUSY;
    }

    info.entry      = 0u;
    info.bytes      = 0u;
    info.segs       = 0u;
    info.argc       = 0u;
    info.sp         = 0u;
    info.stack_size = 0u;
    info.task       = 0;
#if (ARKRTOS_USE_EXIT_CODE == 1)
    info.exit_code  = 0;
    info.exit_code_set = 0u;
#endif

#if (ARKRTOS_USE_PAGING == 1)
    /* 空间回收（U-3 遗留）：挑中的空间一定没有活实例（上面按台账挑的），
     * 所以先把上次用过的帧还回池子，让 activate() 重新要一块。两个作用：
     * 装载永远落在没有旧内容的帧上，且反复 exec 不会把帧池一格一格吃干。 */
    (void)arkrtos_paging_space_release(space);

    /* 装载期间窗口必须指着目标空间；失败也要装回去，否则 shell 之后的
     * 每一条内存命令读到的都是别人的帧。 */
    prev = arkrtos_paging_space_current();
    (void)arkrtos_paging_space_activate(space);
#endif

    rc = elf_load(space, path, argc, argv, &info);

#if (ARKRTOS_USE_PAGING == 1)
    (void)arkrtos_paging_space_activate(prev);
#else
    (void)prev;
#endif

    if(rc != ARKRTOS_ELF_OK)
    {
        /* 失败的装载不留台账：那块帧里可能是半个程序，但空间仍算空着，
         * 下一次用一个合法镜像覆盖掉它。 */
        return rc;
    }

    s_elf_task[space] = info.task;
    s_elf_info[space] = info;
    s_elf_last        = (int32)space;

    if(p_space != 0)
    {
        *p_space = space;
    }

    return ARKRTOS_ELF_OK;
}

/**
* @brief 兼容入口：装一个 ELF 程序，不关心它落到哪个空间
*/
int32 arkrtos_elf_exec(const char *path, int32 argc, char *argv[])
{
    return arkrtos_elf_exec_space(path, argc, argv, 0);
}

int32 arkrtos_elf_running_in(uint32 space)
{
    return elf_slot_live(space);
}

int32 arkrtos_elf_running(void)
{
    uint32 i;

    for(i = 0u; i < elf_space_capacity(); i++)
    {
        if(elf_slot_live(i) != 0)
        {
            return 1;
        }
    }
    return 0;
}

int32 arkrtos_elf_space_count(void)
{
    return (int32)elf_space_capacity();
}

int32 arkrtos_elf_last_space(void)
{
    return s_elf_last;
}

int32 arkrtos_elf_info_space(uint32 space, arkrtos_elf_info_t *p_out)
{
    if(p_out == 0)
    {
        return ARKRTOS_ELF_ERR_PARAM;
    }
    if(space >= (uint32)ELF_INST_MAX)
    {
        return ARKRTOS_ELF_ERR_PARAM;
    }
    if(s_elf_info[space].task <= 0)
    {
        return ARKRTOS_ELF_ERR_PARAM;
    }
    *p_out = s_elf_info[space];
    return ARKRTOS_ELF_OK;
}

int32 arkrtos_elf_info(arkrtos_elf_info_t *p_out)
{
    if((p_out == 0) || (s_elf_last < 0))
    {
        return ARKRTOS_ELF_ERR_PARAM;
    }
    if(s_elf_info[s_elf_last].task <= 0)
    {
        return ARKRTOS_ELF_ERR_PARAM;
    }
    *p_out = s_elf_info[s_elf_last];
    return ARKRTOS_ELF_OK;
}

const char *arkrtos_elf_error_name(int32 rc)
{
    switch(rc)
    {
    case ARKRTOS_ELF_OK:            return "ok";
    case ARKRTOS_ELF_ERR_PARAM:     return "bad parameter";
    case ARKRTOS_ELF_ERR_FS:        return "cannot read file";
    case ARKRTOS_ELF_ERR_FORMAT:    return "not a static ELF executable";
    case ARKRTOS_ELF_ERR_ARCH:      return "wrong machine (need LoongArch)";
    case ARKRTOS_ELF_ERR_LAYOUT:    return "segment outside exec window";
    case ARKRTOS_ELF_ERR_NOMEM:     return "no task slot for the program";
    case ARKRTOS_ELF_ERR_BUSY:      return "exec window already in use";
    case ARKRTOS_ELF_ERR_SIGN:      return "missing or wrong signature tag (see tools/sign_payload.py)";
    default:                        return "unknown";
    }
}

#endif /* ARKRTOS_USE_ELF */
