/**
* @file arkrtos_shell_ushell.c
* @brief 用户命令（SVC 0x1A）：App 侧命令注册表与内核跳板。
*          注册表、输出缓冲、跳板与卸载清理都在这里；控制台任务只调用
*          ushell_drop() 清理某个镜像留下的命令。
* @note 本文件属于内核特权态代码；由 arkrtos_shell.c 按命令族拆出，
*       只做搬运，命令逻辑与拆分前逐字相同。
*/

#include "arkrtos_shell_internal.h"
#include "arkrtos_ushell_upcall.h"

#if (SHELL_ENABLE == 1)

/* ============================================================
 * 特权级策略（SEC-G08）：特权级分离 + 分页的构建上 App 侧注册一律拒绝
 *
 * 现状：跳板在控制台任务栈上直接 `func(argc, argv)`，运行在 PLV0。特权级
 * 分离（ARKRTOS_USE_PRIV=1）且开分页（ARKRTOS_USE_PAGING=1）时，这段用户
 * 代码就拿到了内核特权——不是「App 的命令」能做的事，是内核替它做的。
 * 隔离执行（起一个 PLV3 worker + 返回蹦床 syscall）尚未落地，所以这里
 * fail-closed：拒绝 App 注册，把功能缺口显式化，而不是留一个静默提权。
 *
 * 内核任务（is_priv != 0）注册不受影响；没有特权级分离的构建（Cortex-M /
 * MPU 档）本来就没有这个洞，也保持原语义。
 * 显式定义 ARKRTOS_USE_USHELL_APP_CB=1 可恢复旧语义（仅供宿主用例）。
 * ============================================================ */
#if !defined(ARKRTOS_USE_USHELL_APP_CB)
#  if (ARKRTOS_USE_PRIV == 1) && (ARKRTOS_USE_PAGING == 1)
#    define ARKRTOS_USE_USHELL_APP_CB 0
#  else
#    define ARKRTOS_USE_USHELL_APP_CB 1
#  endif
#endif

/* ============================================================
 * 用户命令（SVC 0x1A）：注册表 + 内核跳板
 *
 * 内核不改 ark_shell（上游原样引入），而是在命令表里追加一条指向本文件的
 * 跳板命令：
 *   - 命令名与帮助文本注册时复制进内核 RAM，镜像卸载后不留下悬垂指针；
 *   - 跳板执行前先确认处理器仍落在某个「已装载」镜像的区间内，
 *     镜像被卸载后命令自动失效并给出提示，而不是跳到已擦除的 Flash 上。
 * ============================================================ */



arkrtos_ushell_entry_t g_ushell[ARKRTOS_USHELL_MAX_CMDS];
static char g_ushell_out[ARKRTOS_USHELL_OUT_CHUNK + 1];

/* 命令名比较：不区分大小写，且要求两边同时结束 */
static int ushell_name_eq(const char *a, const char *b)
{
    if((a == 0) || (b == 0))
    {
        return 0;
    }

    while((*a != '\0') && (*b != '\0'))
    {
        uint32 ca = (uint32)(uint8)*a;
        uint32 cb = (uint32)(uint8)*b;

        if((ca >= 'A') && (ca <= 'Z'))
        {
            ca += 32u;
        }
        if((cb >= 'A') && (cb <= 'Z'))
        {
            cb += 32u;
        }
        if(ca != cb)
        {
            return 0;
        }
        a++;
        b++;
    }

    return ((*a == '\0') && (*b == '\0')) ? 1 : 0;
}

static void ushell_copy(char *dst, uint32 cap, const char *src)
{
    uint32 i = 0u;

    if((dst == 0) || (cap == 0u))
    {
        return;
    }

    if(src != 0)
    {
        while((src[i] != '\0') && ((i + 1u) < cap))
        {
            dst[i] = src[i];
            i++;
        }
    }

    dst[i] = '\0';
}

/* Defined below, next to the dispatch itself: the drop helper needs the
 * trampoline address to find its row in the command table. */
static int arkrtos_shell_ext_trampoline(int argc, char **argv);

/* 一行隔离执行（M-08-4）：起一次 upcall、把返回码翻成一行日志。
 * 实体定义在文件末尾的 ISO 段（它要用那里的记账与几何查询）；关档时
 * 这里给一个 fail-closed 的定义——关档的注册路径根本不会产生
 * has_space 行，真有也算「handler 不可用」，绝不回落到同栈直调。 */
#if (ARKRTOS_USE_USHELL_ISO == 1)
static int ushell_run_isolated(uint32 i, int argc, char **argv);
#else
static int ushell_run_isolated(uint32 i, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ark_shell_printf("%s: handler no longer available (image unloaded?)\r\n",
                     g_ushell[i].name);
    return -1;
}
#endif

/* Remove one entry: the trampoline row is deleted by shifting the later rows
 * down (their order is the help display order, so no hole is left), then the
 * slot itself is cleared. Register / unregister / task-exit all go through
 * here, so the bookkeeping cannot drift between the three.
 * Runs with interrupts off (called from a critical section); it only touches
 * this file's tables and performs no blocking call. */
void ushell_drop(uint32 i)
{
    uint32 k;

    for(k = 0u; (k < (uint32)g_cmd_count) && (k < (uint32)ARK_SHELL_MAX_COMMANDS); k++)
    {
        if((g_cmd_table[k].func == arkrtos_shell_ext_trampoline) &&
           (g_cmd_table[k].name == g_ushell[i].name))
        {
            uint32 j;

            for(j = k; (j + 1u) < (uint32)g_cmd_count; j++)
            {
                g_cmd_table[j] = g_cmd_table[j + 1u];
            }
            g_cmd_count--;
            break;
        }
    }

    g_ushell[i].in_use = 0u;
    g_ushell[i].func = 0;
    g_ushell[i].owner_task = 0u;
    g_ushell[i].space = 0u;
    g_ushell[i].has_space = 0u;
    g_ushell[i].name[0] = '\0';
    g_ushell[i].help[0] = '\0';
}

/* 处理器是否还活着：必须仍落在某个已装载镜像的 Flash 区间内 */
static int ushell_func_alive(const void *p)
{
    const arkrtos_partition_table_t *pt = arkrtos_ptable_get();
    uint32 addr = (uint32)(arkrtos_uptr_t)p;
    uint32 i;

    if((p == 0) || (pt == 0))
    {
        return 0;
    }

    for(i = 0u; (i < pt->slot_max) && (i < ARKRTOS_SLOT_ARRAY_MAX); i++)
    {
        if((pt->slot_type[i] == ARKRTOS_SLOT_FREE) || (pt->slot_base[i] == 0u))
        {
            continue;
        }
        if((pt->slot_state[i] != ARKRTOS_APP_SLOT_LOADED) &&
           (pt->slot_state[i] != ARKRTOS_APP_SLOT_RUNNING))
        {
            continue;
        }
        if((addr >= pt->slot_base[i]) &&
           (addr < (pt->slot_base[i] + pt->slot_size[i])))
        {
            return 1;
        }
    }

    return 0;
}

/* 所有用户命令共用同一个跳板：用 argv[0] 反查注册表项 */
static int arkrtos_shell_ext_trampoline(int argc, char **argv)
{
    uint32 i;

    for(i = 0u; i < ARKRTOS_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use == 0u) || (argv == 0) || (argc <= 0) || (argv[0] == 0))
        {
            continue;
        }

        if(ushell_name_eq(g_ushell[i].name, argv[0]) == 0)
        {
            continue;
        }

        /* SEC-G08 M-08-4：App 注册的行改走隔离执行。失败一律报错，绝不
         * 降级成下面的同栈直调——「在 PLV0 跑一下算了」正是本缺陷。 */
        if(g_ushell[i].has_space != 0u)
        {
            return ushell_run_isolated(i, argc, argv);
        }

        if(ushell_func_alive((const void *)g_ushell[i].func) == 0)
        {
            ark_shell_printf("%s: handler no longer available (image unloaded?)\r\n",
                             g_ushell[i].name);
            return -1;
        }

        {
            /* The callback belongs to an App but runs on the kernel shell task
             * stack: the buffers it renders match no user RAM window, so every
             * buffered SVC service call from inside the callback would be
             * rejected. Raise the flag for the call and drop it right after.
             *
             * On a privilege-separated + paging build no App registration can
             * reach here (see the fail-closed policy at the top of this file),
             * so the flag only covers kernel-side rows; the real fix is the
             * isolated trampoline tracked as SEC-G08. */
            extern uint32 arkrtos_kernel_ushell_active;
            int rc;

            arkrtos_kernel_ushell_active = 1u;
            rc = g_ushell[i].func(argc, argv);
            arkrtos_kernel_ushell_active = 0u;
            return rc;
        }
    }

    sh_out("user command not available\r\n");
    return -1;
}

int32 arkrtos_shell_ext_register(const arkrtos_ushell_cmd_t *cmd)
{
    uint32 i;
    uint32 len;
    uint32 reg_space = 0u;      /* 只在 is_priv == 0 的放行分支里写 */
    uint32 reg_has_space = 0u;  /* 同上；内核侧注册恒为 0（直调） */

    if((cmd == 0) || (cmd->name == 0) || (cmd->func == 0))
    {
        return ARKRTOS_USHELL_ERR_PARAM;
    }

#if (ARKRTOS_USE_USHELL_APP_CB == 0)
    {
        /* 见文件头的「特权级策略」。判据是调用者当前任务的 is_priv——
         * 内核任务注册（板级 / 驱动在内核线程里注册）仍然是 1，只有
         * App 是 0。
         *
         * M-08-4：拒绝从「恒拒绝」收窄成「有条件放行」。App 注册放行的
         * 唯一条件是**它自己那个空间**现在能隔离执行（有 upcall 区 +
         * port 会写桩，判据在 arkrtos_ushell_upcall_ready()）。放行时
         * 记下空间号与 has_space，跳板据此走 upcall 而不是同栈直调；
         * 不满足就仍然回 -6，注册表一个字节都不动。 */
        arkrtos_task_t *me = arkrtos_task_get_current();

        if((me != 0) && (me->is_priv == 0u))
        {
            reg_space = arkrtos_paging_space_of(me);
            if(arkrtos_ushell_upcall_ready(reg_space) == 0)
            {
                return ARKRTOS_USHELL_ERR_PRIV;
            }
            reg_has_space = 1u;
        }
    }
#endif

    len = (uint32)strlen(cmd->name);
    if((len == 0u) || (len > (uint32)ARKRTOS_USHELL_NAME_MAX))
    {
        return ARKRTOS_USHELL_ERR_PARAM;
    }

    if(g_cmd_count >= ARK_SHELL_MAX_COMMANDS)
    {
        return ARKRTOS_USHELL_ERR_FULL;
    }

    for(i = 0u; i < ARKRTOS_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use != 0u) && (ushell_name_eq(g_ushell[i].name, cmd->name) != 0))
        {
            return ARKRTOS_USHELL_ERR_DUP;
        }
    }

    for(i = 0u; i < ARKRTOS_USHELL_MAX_CMDS; i++)
    {
        if(g_ushell[i].in_use == 0u)
        {
            break;
        }
    }
    if(i >= ARKRTOS_USHELL_MAX_CMDS)
    {
        return ARKRTOS_USHELL_ERR_FULL;
    }

    ushell_copy(g_ushell[i].name, (uint32)sizeof(g_ushell[i].name), cmd->name);
    ushell_copy(g_ushell[i].help, (uint32)sizeof(g_ushell[i].help), cmd->help);
    g_ushell[i].func = cmd->func;
    g_ushell[i].max_args = cmd->max_args;
    g_ushell[i].in_use = 1u;
    /* Remember who owns it. A kernel side registration (task id 0) is not tied
     * to any task lifetime and must survive every App stopping. */
    g_ushell[i].owner_task = (arkrtos_current_task_id > 0)
                             ? (uint32)arkrtos_current_task_id : 0u;
    g_ushell[i].space = reg_space;
    g_ushell[i].has_space = reg_has_space;

    g_cmd_table[g_cmd_count].name = g_ushell[i].name;
    g_cmd_table[g_cmd_count].func = arkrtos_shell_ext_trampoline;
    g_cmd_table[g_cmd_count].help = g_ushell[i].help;
    g_cmd_table[g_cmd_count].max_args = (int)cmd->max_args;
    g_cmd_count++;

    return ARKRTOS_USHELL_OK;
}

int32 arkrtos_shell_ext_unregister(const char *name)
{
    uint32 i;

    if(name == 0)
    {
        return ARKRTOS_USHELL_ERR_PARAM;
    }

    for(i = 0u; i < ARKRTOS_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use == 0u) || (ushell_name_eq(g_ushell[i].name, name) == 0))
        {
            continue;
        }

        /* 摘掉命令表里的跳板条目：表内顺序即 help 的显示顺序，
         * 所以用后项前移而不是留空洞 */
        ushell_drop(i);

        return ARKRTOS_USHELL_OK;
    }

    return ARKRTOS_USHELL_ERR_NOTFND;
}

int32 arkrtos_shell_print_n(const char *msg, uint32 len)
{
    uint32 off = 0u;

    if(msg == 0)
    {
        return ARKRTOS_USHELL_ERR_PARAM;
    }

    while(off < len)
    {
        uint32 n = len - off;
        uint32 i;

        if(n > (uint32)ARKRTOS_USHELL_OUT_CHUNK)
        {
            n = (uint32)ARKRTOS_USHELL_OUT_CHUNK;
        }

        for(i = 0u; i < n; i++)
        {
            g_ushell_out[i] = msg[off + i];
        }
        g_ushell_out[n] = '\0';

        sh_out(g_ushell_out);
        off += n;
    }

    return ARKRTOS_USHELL_OK;
}

/* ============================================================
 * SEC-G08 隔离执行：upcall 核心（ready / call / ret）
 *
 * 这里是「App 命令以 PLV3 在它自己的空间里跑」那条路的内核半边。三个入口
 * 分别对上三个时刻：
 *   ready()  注册时：这个空间能不能隔离执行（有 upcall 区 + port 会写桩）
 *   call()   跳板：起一条临时线程、等它返回、把 rc 拿回来
 *   ret()    SVC 0x23：线程自己回来，记账后终止自己
 *
 * 与注册表本体同处一个翻译单元，是因为仓库里除 QEMU 板级以外的四个工程都是
 * **显式文件清单**（F427 GCC sources.mk + F427/F401 .uvprojx + H743
 * sources.mk），新增一个独立 .c 必须同时改这四处，否则那些平台链接失败；
 * 而本文件已经被所有工程包含。取舍与判据见 docs/SEC-G08-隔离执行-设计.md
 * §7。
 * ============================================================ */
#if (ARKRTOS_USE_USHELL_ISO == 1)

/* 每个空间一份并发记账。**只在这里改**：三个入口各看一处，不出现
 * 「两处各记一份、迟早对不上」。索引就是地址空间号。 */
static uint8 s_up_busy[ARKRTOS_USHELL_UP_MAX_SPACES];
static int32 s_up_rc[ARKRTOS_USHELL_UP_MAX_SPACES];
static int32 s_up_sem[ARKRTOS_USHELL_UP_MAX_SPACES];
static int32 s_up_tid[ARKRTOS_USHELL_UP_MAX_SPACES];
/* 最近一次**成功**隔离执行的现场：ret() 记账时就写，跳板在"等到了
 * 返回"之后用它打一行读数（`rc / tid / space / plv`）。tid 必须在
 * ret() 终止任务之前取走 —— TCB 会被复用，事后再读到的可能是别人。 */
static int32 s_up_last_tid[ARKRTOS_USHELL_UP_MAX_SPACES];
static uint32 s_up_last_plv[ARKRTOS_USHELL_UP_MAX_SPACES];

/* 处理器是否还在**这个空间**的镜像里（M-08-4）。与 ushell_func_alive()
 * 的分区表槽位判据不同，这里问的是「这个 App 此刻装着的那个镜像」——
 * 隔离执行只在自己那块空间里有意义：
 *   ① 空间里必须有一个活着的 ELF 实例（镜像被卸 / 任务退了就不可执行）；
 *   ② 它的装载结果里必须有 upcall 区，否则 call() 只会回 NOAREA；
 *   ③ 处理器地址必须落在该空间的可执行区 [win_base, upcall_base) 内。
 * ③ 不逐段比对（只按可执行区判），两点理由写在这里免得后来者以为漏了：
 *   - `arkrtos_elf_info_t` 只记段数（segs），不记段区间；逐段判要改这个
 *     对外结构并动装载器，代价与收益不成比例；
 *   - 这个区间整体就是 App 自己的 PLV3 内存，代码段与数据段同权：判据要
 *     挡的是「悬垂指针 / 别的空间」，不是「数据页里的字节」。upcall 桩页
 *     与桩栈是内核写的，所以用 upcall_base 把它们排除在外。 */
static int ushell_func_alive_space(uint32 space, const void *p)
{
    arkrtos_elf_info_t info;
    unsigned long addr = (unsigned long)p;

    if((p == 0) || (space >= ARKRTOS_USHELL_UP_MAX_SPACES))
    {
        return 0;
    }
    if(arkrtos_elf_running_in(space) == 0)
    {
        return 0;
    }
    if(arkrtos_elf_info_space(space, &info) != (int32)ARKRTOS_ELF_OK)
    {
        return 0;
    }
    if((info.upcall_base == 0ul) || (info.upcall_sp == 0ul))
    {
        return 0;
    }
    if((addr < (unsigned long)ARKRTOS_ELF_WINDOW_BASE) ||
       (addr >= info.upcall_base))
    {
        return 0;
    }
    return 1;
}

/* 取该空间的 upcall 几何；返回 0 = 没有这块区（没装过 ELF，或者
 * ARKRTOS_ELF_UPCALL_SIZE=0 的回退档）。判据落在**每个 App 自己的装载结果**
 * 上，不是全局常量。 */
static int upcall_area(uint32 space, unsigned long *p_stub_va,
                       unsigned long *p_up_sp)
{
    arkrtos_elf_info_t info;

    if(space >= ARKRTOS_USHELL_UP_MAX_SPACES)
    {
        return 0;
    }
    if(arkrtos_elf_info_space(space, &info) != ARKRTOS_ELF_OK)
    {
        return 0;
    }
    if((info.upcall_base == 0ul) || (info.stub_va == 0ul) ||
       (info.upcall_sp == 0ul))
    {
        return 0;
    }

    *p_stub_va = info.stub_va;
    *p_up_sp   = info.upcall_sp;
    return 1;
}

int32 arkrtos_ushell_upcall_ready(uint32 space)
{
    unsigned long stub_va = 0ul;
    unsigned long up_sp = 0ul;

    if(upcall_area(space, &stub_va, &up_sp) == 0)
    {
        return 0;
    }
    if(arkrtos_port_ushell_stub_supported() != 1)
    {
        return 0;
    }

    /* 不看忙闲：注册那一刻恰好有一次回调在跑，不该把注册拒掉。
     * 忙闲由 call() 判。 */
    return 1;
}

/* 半配置的 upcall 任务不许留在就绪集里跑垃圾：摘链 + 置 INVALID，与
 * kill_internal 的收尸前半段同形。新任务还没拥有任何资源（没有信号量、
 * 文件、共享内存），所以不必走 release_resources。调用者必须已经关中断。 */
static void upcall_abort_task(int32 id)
{
    /* 调用者已在建任务成功后判过 id > 0；这里不再重复判，少一条永远走
     * 不到的 return，覆盖率读数也就不会为一条防御分支打折。 */
    arkrtos_ready_del(id - 1);
    arkrtos_task_table[id - 1].status = ARKRTOS_TASK_INVALID;
}

/* ---- 参数搬家（M-08-4）：argv 必须是 App 窗口里的一份副本 --------------
 * 跳板手里拿的 argv 是**内核侧**的指针（shell 的行缓冲），而回调在 App 的
 * 空间里以 PLV3 跑：内核的 DMW 直映区是 PLV0 的，PLV3 读一下就地址错；就算
 * 读得到，「App 能读内核内存」本身也已经把隔离执行否掉了。所以参数在起跑
 * 之前必须搬进 App 自己的窗口，argv 数组里的指针也必须是窗口内的地址。
 *
 * 落点 = upcall 栈**顶**往下的一块，不是栈底：搬完把回调的 sp 移到参数区
 * 之下，栈从那里继续往下长，压栈永远压不到参数区；反过来（放栈底）则会在
 * 回调栈用得深时被静默盖掉，那是最难查的一类缺陷。代价是可用栈少了参数区
 * 那么多字节（≤ ARKRTOS_USHELL_UP_ARGS_MAX），写进 已知限制。
 *
 * 只用 8 字节字写（arkrtos_port_paging_write_space 的粒度）：字符串按 8 字节
 * 补齐、指针数组本来就是 8 字节，于是整块要么全写进去、要么一个字节都不写。
 * 返回 0 = 成功；否则是 ARKRTOS_USHELL_UP_ERR_*，调用方原样回给操作员。 */
/* 往 8 字节槽里写一个小端无符号数：不按 unsigned long 落字，因为宿主的
 * long 只有 4 字节——布局必须与目标机一致，而不是与编译它的机器一致。 */
static void ushell_put_u64(unsigned char *p, unsigned long long v)
{
    unsigned i;

    for(i = 0u; i < 8u; i++)
    {
        p[i] = (unsigned char)((v >> (i * 8u)) & 0xFFull);
    }
}

static int32 ushell_marshal_args(uint32 space, unsigned long up_sp,
                                 unsigned long stack_bottom,
                                 int argc, char **argv,
                                 unsigned long *p_sp, char ***p_argv)
{
    /* 暂存区按字节算，再按 sizeof(unsigned long) 交给 port —— 目标机上
     * 一个字就是 8 字节，等于 total/8 个字；宿主上 long 是 4 字节，等于
     * total/4 个字，覆盖的字节数两档相同。 */
    static unsigned long s_buf[ARKRTOS_USHELL_UP_ARGS_MAX / sizeof(unsigned long)];
    unsigned char *p_bytes;
    unsigned long base;
    uint32 strings = 0u;
    uint32 total;
    uint32 off;
    uint32 k;
    int i;

    /* 不在这里重复参数判据：唯一调用点是 call()，它在搬到这一步之前已经
     * 把 argc/argv/p_sp/p_argv 全验过一遍（回 ERR_PARAM）。留一份永远走
     * 不到的防御，只会让覆盖率变成噪声，读的人还以为是真分支。 */
    if(argc == 0)
    {
        *p_sp   = up_sp;
        *p_argv = 0;
        return ARKRTOS_USHELL_UP_OK;
    }

    /* 先量长度再写：越界的参数不许在窗口里留下半份现场。 */
    for(i = 0; i < argc; i++)
    {
        uint32 n = 0u;

        if(argv[i] == 0)
        {
            return ARKRTOS_USHELL_UP_ERR_PARAM;
        }
        while(argv[i][n] != '\0')
        {
            n++;
            if((strings + n) >= (uint32)ARKRTOS_USHELL_UP_ARGS_MAX)
            {
                return ARKRTOS_USHELL_UP_ERR_PARAM;
            }
        }
        strings += (n + 1u + 7u) & ~7u;
    }
    total = strings + (((uint32)argc + 1u) * 8u);
    if(total > (uint32)ARKRTOS_USHELL_UP_ARGS_MAX)
    {
        return ARKRTOS_USHELL_UP_ERR_PARAM;
    }
    /* 参数区留在 up_sp 之下、初始帧之上，之下还要留够真栈：几何不成立就
     * 一个字节都不写。2048 是「初始帧（LA64 = 1040 字节）+ 一段真实可用的
     * 栈」的下限，帧本身由 port 落在 call_sp - 帧长。 */
    if(up_sp < (stack_bottom + (unsigned long)total + 2048ul))
    {
        return ARKRTOS_USHELL_UP_ERR_NOAREA;
    }
    base = (up_sp - (unsigned long)total) & ~7ul;

    /* 整块按**字节**铺：字符串与指针数组在窗口里的位置是 ABI 的一部分
     * （8 字节槽、小端），不能跟着宿主上 `sizeof(unsigned long)` 变。往
     * 宿主编译时 long 是 4 字节，若按「一个字 = 8 字节」去索引就会把指针
     * 数组盖到第二个字符串上——这正是第一版读出来的假红。 */
    p_bytes = (unsigned char *)(void *)s_buf;
    for(k = 0u; k < (uint32)ARKRTOS_USHELL_UP_ARGS_MAX; k++)
    {
        p_bytes[k] = 0u;
    }
    off = 0u;
    for(i = 0; i < argc; i++)
    {
        const char *p_s = argv[i];
        uint32 n = 0u;

        while(p_s[n] != '\0')
        {
            p_bytes[off + n] = (unsigned char)p_s[n];
            n++;
        }
        /* 指针槽：8 字节小端，与目标机的一个字一一对应 */
        ushell_put_u64(&p_bytes[strings + ((uint32)i * 8u)],
                       (unsigned long long)(base + (unsigned long)off));
        off += (n + 1u + 7u) & ~7ul;
    }
    /* 末尾 NULL：回调里 `while(argv[i])` 这种写法走不到头就会读过参数区。 */
    ushell_put_u64(&p_bytes[strings + ((uint32)argc * 8u)], 0ull);

    if(arkrtos_port_paging_write_space(space, base,
                                       (const unsigned long *)(const void *)p_bytes,
                                       total / (uint32)sizeof(unsigned long)) != 0)
    {
        return ARKRTOS_USHELL_UP_ERR_EXEC;
    }

    *p_sp   = base;
    *p_argv = (char **)(unsigned long)(base + (unsigned long)strings);
    return ARKRTOS_USHELL_UP_OK;
}

int32 arkrtos_ushell_upcall_call(uint32 space, uint32 owner_task,
                                 int (*func)(int, char **argv),
                                 int argc, char **argv, int32 *p_rc)
{
    arkrtos_task_t *p_task;
    unsigned long stub_va = 0ul;
    unsigned long up_sp = 0ul;
    unsigned long stack_bottom;
    unsigned long call_sp = 0ul;    /* 回调真正用的栈顶（参数区之下） */
    unsigned long frame;
    char **app_argv = 0;            /* 窗口内的 argv；入口帧里传的是它 */
    uint32 stack_bytes;
    uint32 crit;
    int32 id;
    int32 sem;
    int32 rc = ARKRTOS_USHELL_UP_OK;
    int32 wrc;
#if (ARKRTOS_USE_PAGING == 1)
    uint32 prev_space;
#endif

    if((p_rc == 0) || (func == 0) || (argc < 0) ||
       ((argc > 0) && (argv == 0)))
    {
        return ARKRTOS_USHELL_UP_ERR_PARAM;
    }
    /* owner_task 是 1 基任务号；0 = 内核侧注册，它不该走隔离执行。 */
    if((owner_task == 0u) || (owner_task > (uint32)ARKRTOS_TASK_MAX_NUM))
    {
        return ARKRTOS_USHELL_UP_ERR_PARAM;
    }
    if(space >= ARKRTOS_USHELL_UP_MAX_SPACES)
    {
        return ARKRTOS_USHELL_UP_ERR_PARAM;
    }
    if(upcall_area(space, &stub_va, &up_sp) == 0)
    {
        return ARKRTOS_USHELL_UP_ERR_NOAREA;
    }
    if(arkrtos_port_ushell_stub_supported() != 1)
    {
        return ARKRTOS_USHELL_UP_ERR_NOAREA;
    }
    if(s_up_busy[space] != 0u)
    {
        return ARKRTOS_USHELL_UP_ERR_BUSY;
    }

    /* 桩页之上才是 upcall 栈。几何不成立时任何一步都不许开始：往窗口里写
     * 半份现场比报错贵得多。 */
    stack_bottom = stub_va + (unsigned long)ARKRTOS_USHELL_UP_PAGE_SIZE;
    if((up_sp < (stack_bottom + 64ul)) || ((up_sp + 16ul) <= stack_bottom))
    {
        return ARKRTOS_USHELL_UP_ERR_NOAREA;
    }

    /* 参数搬家：回调在 PLV3 读不到内核内存，argv 必须在起跑前变成窗口里的
     * 一份副本；搬完 sp 下移到参数区之下（栈从那里往下长）。这一步在任何
     * 资源被占用之前做完，失败就干净地回错误码。 */
    rc = ushell_marshal_args(space, up_sp, stack_bottom, argc, argv,
                             &call_sp, &app_argv);
    if(rc != ARKRTOS_USHELL_UP_OK)
    {
        return rc;
    }
    /* 任务栈到 call_sp 为止，**不含**参数区：注册会按「栈顶 = 栈底 + 栈长」
     * 造一份初始帧（还会按 STACK_USAGE 整段填图案），栈顶若给到 up_sp，
     * 那份帧正好压到参数区上——实测症状是 argc/argv 指针全对、argv[i] 全 0。
     * 所以栈顶必须停在 call_sp：参数区在 call_sp 之上，初始帧在其下。 */
    stack_bytes = (uint32)(call_sp - stack_bottom);

    sem = arkrtos_sem_create_internal("upcall", 0);
    if(sem < 0)
    {
        return ARKRTOS_USHELL_UP_ERR_TASK;
    }

    crit = ARKRTOS_ENTER_CRITICAL();
    /* 登记 busy 与建任务放在同一个临界区：同一空间的两条 shell 命令不能
     * 同时看到「不忙」然后各建一条线程。 */
    s_up_busy[space] = 1u;
    s_up_rc[space]   = 0;
    s_up_tid[space]  = 0;
    s_up_sem[space]  = sem;

#if (ARKRTOS_USE_PAGING == 1)
    /* 窗口必须指着目标空间：task_register() 会往 upcall 栈底写保护字、建
     * 一份内核帧，写的是 VA —— 装着别人的帧就写到别人窗口里去了。与装载器
     * 同一做法（elf_exec_space），退出前一定装回去。 */
    prev_space = arkrtos_paging_space_current();
    (void)arkrtos_paging_space_activate(space);
#endif

    id = arkrtos_task_register((void (*)(void))stub_va,
                               (uint32 *)stack_bottom, stack_bytes,
                               (uint8)ARKRTOS_USHELL_UP_TASK_PRIORITY, 0u);
    if(id <= 0)
    {
#if (ARKRTOS_USE_PAGING == 1)
        (void)arkrtos_paging_space_activate(prev_space);
#endif
        s_up_busy[space] = 0u;
        s_up_tid[space]  = 0;
        s_up_sem[space]  = -1;
        ARKRTOS_EXIT_CRITICAL(crit);
        (void)arkrtos_sem_delete_internal(sem);
        return ARKRTOS_USHELL_UP_ERR_TASK;
    }

    p_task = &arkrtos_task_table[id - 1];
    p_task->is_priv = 0u;
    /* 代码窗口 = 数据窗口 = 该 App 的窗口：copy_from_user / copy_to_user
     * 与 MPU 都按这两个区间判，漏写就是「所有 write 都回 -EFAULT」。 */
    p_task->rom_start = (unsigned long)ARKRTOS_ELF_WINDOW_BASE;
    p_task->rom_size  = (uint32)ARKRTOS_ELF_WINDOW_SIZE;
    p_task->ram_start = (unsigned long)ARKRTOS_ELF_WINDOW_BASE;
    p_task->ram_size  = (uint32)ARKRTOS_ELF_WINDOW_SIZE;
#if (ARKRTOS_USE_MPU == 1)
    arkrtos_mpu_build_task(p_task);
#endif
#if (ARKRTOS_USE_APP_QUOTA == 1)
    /* 继承 App 的配额身份（SEC-G07）：不这么做，一个 App 起无限多回调
     * 线程就能把任务表吃光，绕过分区配额。 */
    p_task->app_owner = arkrtos_task_table[owner_task - 1u].app_owner;
#endif
#if (ARKRTOS_USE_PAGING == 1)
    (void)arkrtos_paging_bind_task(p_task, space);
#endif
    s_up_tid[space] = id;

    if(arkrtos_port_ushell_stub(space, stub_va) != 0)
    {
        rc = ARKRTOS_USHELL_UP_ERR_EXEC;
    }
    else
    {
        frame = arkrtos_port_ushell_start(space, stub_va, call_sp, func, argc,
                                          app_argv);
        if(frame == 0ul)
        {
            rc = ARKRTOS_USHELL_UP_ERR_EXEC;
        }
        else
        {
            /* 帧的位置由 port 定（落在 upcall 栈里），这里把它登记成这条
             * 任务的「栈顶现场」。放在 bind 之后是故意的：SMP 档的绑定会按
             * 旧 stack_ptr 重造一次帧，我们这份才是最终现场。 */
            p_task->stack_top = call_sp;
            p_task->stack_ptr = frame;
        }
    }

#if (ARKRTOS_USE_PAGING == 1)
    (void)arkrtos_paging_space_activate(prev_space);
#endif

    if(rc != ARKRTOS_USHELL_UP_OK)
    {
        upcall_abort_task(id);
        s_up_busy[space] = 0u;
        s_up_tid[space]  = 0;
        s_up_sem[space]  = -1;
        ARKRTOS_EXIT_CRITICAL(crit);
        (void)arkrtos_sem_delete_internal(sem);
        return rc;
    }
    ARKRTOS_EXIT_CRITICAL(crit);

    /* 等桩返回。超时不强杀（与「内核不擅自杀任务」的口径一致）：把 busy
     * 位清掉并报错，下一次调用能继续；那条还在跑的回调线程由现有故障/
     * 退出路径收尸。 */
    wrc = arkrtos_sem_wait_internal(sem, (int32)ARKRTOS_USHELL_UP_TIMEOUT_MS);

    crit = ARKRTOS_ENTER_CRITICAL();
    if(s_up_busy[space] == 0u)
    {
        /* ret() 已经记账：rc 槽有效。它与 post 的顺序是「先写槽、后 post」，
         * 所以 busy == 0 就意味着值已经在槽里。 */
        *p_rc = s_up_rc[space];
        rc = ARKRTOS_USHELL_UP_OK;
    }
    else
    {
        s_up_busy[space] = 0u;
        s_up_tid[space]  = 0;
        *p_rc = 0;
        rc = (wrc == ARKRTOS_SYNC_ERR_TIMEOUT) ? ARKRTOS_USHELL_UP_ERR_TIMEOUT
                                               : ARKRTOS_USHELL_UP_ERR_EXEC;
    }
    s_up_sem[space] = -1;
    ARKRTOS_EXIT_CRITICAL(crit);

    (void)arkrtos_sem_delete_internal(sem);
    return rc;
}

int32 arkrtos_ushell_upcall_ret(int32 rc, uint32 plv)
{
    int32 tid = arkrtos_current_task_id;
    uint32 i;

    if(tid <= 0)
    {
        return ARKRTOS_USHELL_UP_ERR_CALLER;
    }

    /* 冒充者一律拒绝，且不做任何状态变更：只有 call() 亲手登记过的那条
     * 任务号能对上，普通 App 直接 syscall 0x23 找不到自己的行。 */
    for(i = 0u; i < ARKRTOS_USHELL_UP_MAX_SPACES; i++)
    {
        if((s_up_tid[i] != tid) || (s_up_busy[i] == 0u))
        {
            continue;
        }

        s_up_rc[i]       = rc;
        s_up_last_tid[i] = tid;
        /* 这次 syscall 是从哪个特权级发出来的（trap 帧保存的 PPLV）。
         * 隔离执行成立与否的硬证据就在这里：App 在 PLV3 跑，值必须是 3。 */
        s_up_last_plv[i] = plv;
        s_up_busy[i]     = 0u;
        s_up_tid[i]      = 0;
        if(s_up_sem[i] >= 0)
        {
            (void)arkrtos_sem_post_internal(s_up_sem[i]);
        }

        /* 记账之后立刻终止自己：rc 已经在内核手里，桩不需要再往下走。
         * kill_internal 负责摘就绪链、置 INVALID 并切走；正常路径永远回
         * 不到下一行。 */
        arkrtos_task_kill_internal();
        return ARKRTOS_USHELL_UP_OK;   /* 只有宿主桩（切不走）会走到这里 */
    }

    return ARKRTOS_USHELL_UP_ERR_CALLER;
}

/* 跳板的一行隔离执行（M-08-4）。每条失败都落到**不同**的日志行上：
 * 操作员看到 "busy" 与看到 "cannot start isolated worker" 要能分辨
 * 是「上一次还没回来」还是「内核没把线程建起来」，两者处理方式不同。
 * 返回码语义与直调一致：回调的 int 原样回给 shell。 */
static int ushell_run_isolated(uint32 i, int argc, char **argv)
{
    int32 rc = 0;
    int32 crc;

    if(ushell_func_alive_space(g_ushell[i].space,
                               (const void *)g_ushell[i].func) == 0)
    {
        ark_shell_printf("%s: handler no longer available (image unloaded?)\r\n",
                         g_ushell[i].name);
        return -1;
    }

    crc = arkrtos_ushell_upcall_call(g_ushell[i].space, g_ushell[i].owner_task,
                                     g_ushell[i].func, argc, argv, &rc);

    switch(crc)
    {
        case ARKRTOS_USHELL_UP_OK:
            /* 读数行：这一条是 QEMU 门禁的锚点。tid 与 plv 来自 ret() 记账，
             * 不是猜的；plv 必须是 3（PLV3 才算「隔离执行」成立）。 */
            ark_shell_printf("[ushell] isolated rc=%d tid=%d space=%u plv=%u\r\n",
                             (int)rc,
                             (int)s_up_last_tid[g_ushell[i].space],
                             (unsigned)g_ushell[i].space,
                             (unsigned)s_up_last_plv[g_ushell[i].space]);
            return (int)rc;

        case ARKRTOS_USHELL_UP_ERR_BUSY:
            ark_shell_printf("%s: busy (another upcall in this space)\r\n",
                             g_ushell[i].name);
            return -1;

        case ARKRTOS_USHELL_UP_ERR_TIMEOUT:
            ark_shell_printf("%s: isolated worker timed out after %u ms\r\n",
                             g_ushell[i].name,
                             (unsigned)ARKRTOS_USHELL_UP_TIMEOUT_MS);
            return -1;

        case ARKRTOS_USHELL_UP_ERR_TASK:
        case ARKRTOS_USHELL_UP_ERR_EXEC:
            ark_shell_printf("%s: cannot start isolated worker (%d)\r\n",
                             g_ushell[i].name, (int)crc);
            return -1;

        default:
            /* NOAREA / PARAM / CALLER：注册时的门禁已经拦过一遍，走到这里
             * 说明装载结果在注册之后变了（镜像被卸、空间被回收）。按
             * 「handler 不可用」报，仍然不直调。 */
            ark_shell_printf("%s: handler no longer available (image unloaded?)\r\n",
                             g_ushell[i].name);
            return -1;
    }
}

#endif /* ARKRTOS_USE_USHELL_ISO */

#endif /* SHELL_ENABLE */
