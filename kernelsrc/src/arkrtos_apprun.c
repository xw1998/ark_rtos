/**
* @file arkrtos_apprun.c
* @brief 板上直跑（app run）：arkc 编出来的裸文件直接进固定窗口执行
* @details 整条路径只有五步，每一步都围绕「窗口是借来的、地址是写死的」：
*            1. 判并发：这一条路径只有一段窗口，跑着就不再收第二个；
*            2. 借窗：向 SLOT_RAM 池借**指定的**那两块（代码 8 KB / 数据 8 KB）。
*               借回来的基址必须正好等于窗口基址——裸文件是按固定地址链接的，
*               换到别的地址去跑等于让它跳进虚空，所以宁可不跑；
*            3. 读文件：整份读进代码窗口，大小必须在代码窗口内；
*            3b. 认向量表：arkc 的 `-link` 会在文件开头放一张 256 项向量表，
*               +0 是初始栈指针、+4 是 Reset_Handler。本文件只认这三件事——
*               表够长、SP 正好等于数据窗口顶、入口落在代码窗口内且首半字是序言。
*               不认「文件首半字是序言」：arkc 把字符串字面量排在所有函数之前，
*               带字符串的程序首字节是字符、不是指令。
*            4. 起任务：入口 = 向量表 +4 的 Reset_Handler，栈取数据窗口末尾的 4 KB，
*               非特权（is_priv=0），TCB 的 rom/ram 两窗口各自一块 → 建 MPU；
*            5. 退出：主任务自己退出时由 arkrtos_task.c 调本文件的钩子还窗。
*
*          为什么不复用 arkrtos_mini_run()：那条路径要吃 256 字节镜像头与重定位表，
*          而 arkc 的 `-o` 只吐一串裸字节；为了跑一个已经在板上编出来的文件去写
*          一个打包器，等于把「编译到执行」的链路再加一环。这里明说不做重定位：
*          程序用 `-link --scatter` 链接到窗口地址，编译期就把地址定死了。
*
*          入口契约为什么是向量表而不是「首半字序言」：见 3b。宿主侧实测过一份带两个
*          字符串的程序，2260 字节里 0xB5E0 首次出现在 offset 0x30（字符串在 0x00-0x2F），
*          所以「首字节是指令」只对不含字面量的程序碰巧成立。向量表是 `-link` 本来
*          就会生成的东西（SP 取 scatter 里 +RW 区的 load_addr+size），不需要新造约定；
*          它还白送一条强校验：拿别处布局链出来的镜像，SP 对不上就当场拒收。
*
*          与 App / MiniApp 的关系：三者都是「借池里的块 + 起非特权任务 + 建 MPU」，
*          所以这里不另写分配器、不另写 MPU 逻辑、不另写任务注册——只做窗口校验。
*
* @author xw
* @date 2026.11.05
*/

#include "arkrtos_apprun.h"
#include "arkrtos_config.h"
#include "arkrtos_partition.h"    /* 内核专属：SLOT_RAM_* / ARKRTOS_APPRUN_* 的唯一地址源头 */
#include "arkrtos_loader.h"     /* 错误码与镜像字段规则：本文件只用错误码，不解析镜像 */
#include "arkrtos_cfg.h"        /* arkrtos_task_table 的声明（回填 rom/ram 两窗口） */
#include "arkrtos_mini.h"       /* 沿用小程序那一套错误码（ARKRTOS_MINI_ERR_FS），不新造数字 */
#include "arkrtos_ptable.h"
#include "arkrtos_vfs.h"      /* /tmp 这类易失目录挂在 VFS 的 ramfs 上，与 littlefs 卷是两层 */
#include "arkrtos_task.h"
#include "arkrtos_mpu.h"
#include "arkrtos_spin.h"
#include "arkrtos_log.h"

#if (ARKRTOS_USE_APPRUN == 1)

/* 单实例状态。三样东西一起读、一起改，所以用一把锁；不复用 mini 的实例表，
 * 因为两者名额与生命周期都不一样（直跑只有一个窗口，谈不上并发数）。 */
static arkrtos_spinlock_t g_apprun_lock = ARKRTOS_SPINLOCK_INIT;
static uint32 g_apprun_task   = 0u;      /* 主任务号；0 = 没在跑 */
static uint32 g_apprun_pt     = 0u;      /* 借窗凭据（ptable 下标），还窗时必须原样交回 */
static uint32 g_apprun_entry  = 0u;      /* 入口地址（已带 Thumb 位），仅供展示 */

static int32 apprun_read_file(const char *path, uint32 *p_size)
{
    int32  fd;
    int32  n;
    uint32 total = 0u;
    uint32 cap   = (uint32)ARKRTOS_APPRUN_CODE_SIZE;

    /* 走 VFS 而不是 arkrtos_fs：板上 cc 的产物落在 /tmp，那是 VFS 的易失
     * ramfs，与 littlefs 卷是两层。用 arkrtos_fs 读这里只会得到「没有文件
     * 系统」，而文件明明就在那儿——第 5 节就是按这条实测改过来的。 */
    fd = arkrtos_vfs_open_read(path);
    if(fd < 0)
    {
        return ARKRTOS_MINI_ERR_FS;
    }

    /* 整份读进代码窗口。上限传窗口大小：多一个字节都不读进来——越过窗口顶
     * 就是数据窗口，多读一个字节就是在踩别人的 .data。 */
    while(total < cap)
    {
        n = arkrtos_vfs_read(fd, (uint8 *)(void *)((uint32)ARKRTOS_APPRUN_BASE + total),
                             cap - total);
        if(n < 0)
        {
            (void)arkrtos_vfs_close(fd);
            return ARKRTOS_MINI_ERR_FS;
        }
        if(n == 0)
        {
            break;                          /* 文件尾 */
        }
        total += (uint32)n;
    }

    /* 正好读满还必须再问一次，否则「刚好等于窗口的镜像」和「比窗口大」分不
     * 开：超窗口不截断——截断会把一条跳转指令切成半条，跑起来是随机行为。 */
    if(total >= cap)
    {
        uint8 probe;

        n = arkrtos_vfs_read(fd, &probe, 1u);
        if(n < 0)
        {
            (void)arkrtos_vfs_close(fd);
            return ARKRTOS_MINI_ERR_FS;
        }
        if(n > 0)
        {
            (void)arkrtos_vfs_close(fd);
            return ARKRTOS_LOADER_ERR_SIZE;
        }
    }

    (void)arkrtos_vfs_close(fd);

    if((total == 0u) || (total > (uint32)ARKRTOS_APPRUN_MAX_IMAGE))
    {
        return ARKRTOS_LOADER_ERR_SIZE;     /* 空文件/目录都不是可执行镜像 */
    }

    *p_size = total;
    return 0;
}

int32 arkrtos_apprun_run_ex(const char *path, uint32 *task_out)
{
    uint32 irq;
    uint32 code_base = 0u;
    uint32 code_size = 0u;
    uint32 ram_base = 0u;
    uint32 ram_size = 0u;
    uint32 pt_index = 0u;
    uint32 entry;
    uint32 payload = 0u;
    int32 task_id;
    int32 ret;

    if(path == 0)
    {
        return ARKRTOS_LOADER_ERR_PARAM;
    }

    /* 单实例闸门：先占位再干活。装载可能耗时（读 8 KB + 建 MPU），期间进来的
     * 第二个请求不能只看 g_apprun_task（那时还没起任务号）就放行。 */
    arkrtos_spin_lock_irqsave(&g_apprun_lock, &irq);
    if(g_apprun_task != 0u)
    {
        arkrtos_spin_unlock_irqrestore(&g_apprun_lock, irq);
        return ARKRTOS_APPRUN_ERR_BUSY;
    }
    g_apprun_task = 0xFFFFFFFFu;            /* 占位：装载中 */
    arkrtos_spin_unlock_irqrestore(&g_apprun_lock, irq);

    /* 前置条件是「VFS 起来了」，不是「littlefs 卷挂上了」：镜像从 /tmp 读，
     * 而 /tmp 是 VFS 的 ramfs。用错一层的症状是编译成功、运行报「没有文件
     * 系统」，第 5 节记了这次实测。 */
    if(arkrtos_vfs_ready() == 0u)
    {
        ret = ARKRTOS_LOADER_ERR_STATE;
        goto release_slot;
    }

    /* 文件系统只有一份缓存：读文件的整个过程不能与别的任务读写交错。
     * 与 mini / SVC 层同一套约定（调度器锁）。 */
    arkrtos_sched_lock_internal();

    /* 借窗：走小程序那套 RAM 池首适配（mini_alloc），**但没有**「够大就用」
     * 这种自由度——裸镜像是按固定窗口地址链出来的，换地址跑等于让它跳进
     * 虚空。所以借完必须逐字段核对：拿到的那一块必须与窗口常量完全相同，
     * 否则当场归还并报窗口不可用。
     *
     * 为什么不用 arkrtos_ptable_alloc_raw()：它核的是 **Flash** 的镜像池
     * （CONFIG_BASE + CONFIG_SIZE），不是这块 RAM 窗口，用它登记 RAM 区间
     * 必然失败（板上实测 rc=-21）。 */
    {
        uint32 c_base = 0u, c_size = 0u, r_base = 0u, r_size = 0u;
        int32  slot;

        slot = arkrtos_ptable_mini_alloc((uint32)ARKRTOS_APPRUN_CODE_SIZE,
                                        (uint32)ARKRTOS_APPRUN_RAM_SIZE,
                                        &pt_index,
                                        &c_base, &c_size, &r_base, &r_size);
        if(slot < 0)
        {
            /* 只可能是「窗口已被占」（上一份还没退出）或池表已满：
             * 两种都报窗口不可用，别拿 NOSPACE 糊过去。 */
            arkrtos_sched_unlock_internal();
            ret = ARKRTOS_APPRUN_ERR_WINDOW;
            goto release_slot;
        }

        /* slot 与 pt_index 是同一个下标（mini_alloc 两个都写）；拿 slot 做
         * 成功/失败判断，下标一律用 pt_index，避免两个名字被读成两件事。 */
        (void)slot;

        if((c_base != (uint32)ARKRTOS_APPRUN_BASE) ||
           (c_size != (uint32)ARKRTOS_APPRUN_CODE_SIZE) ||
           (r_base != (uint32)ARKRTOS_APPRUN_RAM_BASE) ||
           (r_size != (uint32)ARKRTOS_APPRUN_RAM_SIZE))
        {
            /* 首适配给的不是窗口本身（池里在这个尺寸上有更靠前的空块）：
             * 这块地址上跑不了按窗口链出来的镜像，如实拒绝，别悄悄挪位置。 */
            (void)arkrtos_ptable_mini_free(pt_index);
            arkrtos_sched_unlock_internal();
            ret = ARKRTOS_APPRUN_ERR_WINDOW;
            goto release_slot;
        }

        code_base = c_base;
        code_size = c_size;
        ram_base  = r_base;
        ram_size  = r_size;
    }

    ret = apprun_read_file(path, &payload);
    if(ret != 0)
    {
        (void)arkrtos_ptable_mini_free(pt_index);
        arkrtos_sched_unlock_internal();
        goto release_slot;
    }

    /* 入口与栈顶取自镜像自带的向量表，不是猜出来的：arkc 的 -link 把 256 项
     * 向量表放在负载偏移 0（+0 = 初始 SP、+4 = Reset_Handler | Thumb 位），
     * 这是 Cortex-M 的装载约定，与「入口函数排在文件第几个字节」无关。
     * 两条都拿本次借到的窗口去核：
     *   - SP 必须正好是数据窗口顶端：说明这份镜像就是按这个窗口链的；
     *   - 入口必须落在代码窗口内，且入口首半字是 arkc 的函数序言。
     * 对不上就拒绝：跑一份为别的布局链出来的镜像，等于执行任意字节。 */
    {
        const volatile uint32 *vt = (const volatile uint32 *)(const void *)ARKRTOS_APPRUN_BASE;
        uint32 sp  = vt[0];
        uint32 vec = vt[1];
        uint32 ent = vec & ~1u;

        if((payload < 8u) ||
           (sp != (uint32)(ARKRTOS_APPRUN_RAM_BASE + ARKRTOS_APPRUN_RAM_SIZE)) ||
           (ent < code_base) ||
           ((ent + 2u) > (code_base + code_size)))
        {
            (void)arkrtos_ptable_mini_free(pt_index);
            arkrtos_sched_unlock_internal();
            ret = ARKRTOS_APPRUN_ERR_LAYOUT;
            goto release_slot;
        }

        /* 序言校验：arkc 的每个函数都以 PUSH {r5,r6,r7,lr}（0xB5E0）开头。
         * 这里核的是**入口那一条**指令，不是文件首字节——字符串字面量排在
         * 代码前面，文件首字节本来就不是指令。 */
        if(*(const volatile uint16 *)(const void *)ent != (uint16)ARKRTOS_APPRUN_PROLOGUE)
        {
            (void)arkrtos_ptable_mini_free(pt_index);
            arkrtos_sched_unlock_internal();
            ret = ARKRTOS_APPRUN_ERR_PROLOGUE;
            goto release_slot;
        }

        entry = vec;
    }

    /* 调度器锁必须在起任务前放开：arkrtos_sched_activate_higher() 在锁内只记账。 */
    arkrtos_sched_unlock_internal();

    /* 栈取数据窗口末尾那 4 KB，向下增长；.data/.bss 落在窗口前段。程序的
     * __arkcc_init_globals() 会按链接地址初始化/清零，窗口里的旧内容不影响它。 */
    task_id = arkrtos_task_register((void (*)(void))entry,
                                    (uint32 *)(void *)(ram_base +
                                        (uint32)ARKRTOS_APPRUN_RAM_SIZE -
                                        (uint32)ARKRTOS_APPRUN_STACK_SIZE),
                                    (uint32)ARKRTOS_APPRUN_STACK_SIZE,
                                    (uint8)ARKRTOS_APPRUN_PRIORITY,
                                    (uint32)ARKRTOS_APPRUN_PERIOD_MS);
    if(task_id <= 0)
    {
        (void)arkrtos_ptable_mini_free(pt_index);   /* 任务表满：两块还在手里 */
        ret = ARKRTOS_LOADER_ERR_TASK;
        goto release_slot;
    }

    /* TCB 的两窗口就是这两块：代码块在池里 → region0 用可执行 RAM 属性；
     * 数据块走常规 RW、不可执行。两块基址不同、大小不同，所以是两个独立的
     * 精确窗口（region0 / region1），不需要「两窗必须同属性」的妥协。 */
    arkrtos_task_table[task_id - 1].rom_start = code_base;
    arkrtos_task_table[task_id - 1].rom_size  = code_size;
    arkrtos_task_table[task_id - 1].ram_start = ram_base;
    arkrtos_task_table[task_id - 1].ram_size  = ram_size;
    arkrtos_task_table[task_id - 1].is_priv   = 0u;    /* 非特权：与 App 同级 */

    arkrtos_mpu_build_task(&arkrtos_task_table[task_id - 1]);

    /* 入口要等向量表读出来才知道，所以 mini_alloc 先记 0，这里补上：
     * 池里这条记录是「谁占着这 16 KB」的唯一凭据，入口为空会让现场
     * 对着 app list 猜是哪份镜像。 */
    (void)arkrtos_ptable_set_slot(pt_index, ARKRTOS_APP_SLOT_RUNNING,
                                  entry, (uint32)task_id);

    arkrtos_spin_lock_irqsave(&g_apprun_lock, &irq);
    g_apprun_pt    = pt_index;
    g_apprun_entry = entry;
    g_apprun_task  = (uint32)task_id;
    if(task_out != 0)
    {
        *task_out = (uint32)task_id;    /* activate 之前带出去，见头文件 */
    }
    arkrtos_spin_unlock_irqrestore(&g_apprun_lock, irq);

    ARKRTOS_LOGI("APPRUN", "%s: %u B -> code 0x%08X (%u B), data/stack 0x%08X (%u B), task %d",
                 path, (unsigned)payload, (unsigned)code_base, (unsigned)code_size,
                 (unsigned)ram_base, (unsigned)ram_size, (int)task_id);

    arkrtos_sched_activate_higher((uint8)ARKRTOS_APPRUN_PRIORITY);
    return 0;

release_slot:
    arkrtos_spin_lock_irqsave(&g_apprun_lock, &irq);
    g_apprun_task = 0u;
    arkrtos_spin_unlock_irqrestore(&g_apprun_lock, irq);
    return ret;
}

int32 arkrtos_apprun_run(const char *path)
{
    return arkrtos_apprun_run_ex(path, 0);
}

int32 arkrtos_apprun_on_task_exit(uint32 task_id)
{
    uint32 irq;
    uint32 pt_index = 0u;
    int32 ret;

    if(task_id == 0u)
    {
        return 0;
    }

    /* 只认自己的主任务：内核任务、App、小程序，或直跑程序自己开的线程退出时
     * 都原样返回 0——还一次窗就是在池里漏两块 RAM。 */
    arkrtos_spin_lock_irqsave(&g_apprun_lock, &irq);
    if((g_apprun_task == 0u) || (g_apprun_task != task_id))
    {
        arkrtos_spin_unlock_irqrestore(&g_apprun_lock, irq);
        return 0;
    }
    pt_index = g_apprun_pt;
    g_apprun_task  = 0u;
    g_apprun_pt    = 0u;
    g_apprun_entry = 0u;
    arkrtos_spin_unlock_irqrestore(&g_apprun_lock, irq);

    /* 此刻调用者自身已被内核置为 INVALID，所以不必先踢任务再还块：没有任何
     * 上下文还在执行那块代码。仍按 mini 的顺序记账——先离开「在跑」集合
     * （上面已做），再还块。 */
    /* 归还失败必须能被看见：mini_free 返回 -1 说明这条记录本来就空，
     * 账已经乱了（重复归还或下标错），下一次借窗会莫名其妙拿到别的地方。
     * 这里不改变返回值（任务退出已经发生），但把 -1 打出来。 */
    if(arkrtos_ptable_mini_free(pt_index) != 0)
    {
        ARKRTOS_LOGE("APPRUN", "task %u exited but window %u was already free",
                     (unsigned)task_id, (unsigned)pt_index);
    }

    ARKRTOS_LOGI("APPRUN", "task %u exited, window returned to the pool",
               (unsigned)task_id);

    return 0;
}

uint32 arkrtos_apprun_task_id(void)
{
    uint32 irq;
    uint32 id;

    arkrtos_spin_lock_irqsave(&g_apprun_lock, &irq);
    id = g_apprun_task;
    arkrtos_spin_unlock_irqrestore(&g_apprun_lock, irq);

    return (id == 0xFFFFFFFFu) ? 0u : id;   /* 装载中的占位不算「在跑」 */
}

int32 arkrtos_apprun_info(uint32 *p_entry, uint32 *p_code_base, uint32 *p_ram_base)
{
    uint32 irq;

    arkrtos_spin_lock_irqsave(&g_apprun_lock, &irq);
    if((g_apprun_task == 0u) || (g_apprun_task == 0xFFFFFFFFu))
    {
        arkrtos_spin_unlock_irqrestore(&g_apprun_lock, irq);
        return -1;
    }
    if(p_entry != 0)
    {
        *p_entry = g_apprun_entry;
    }
    if(p_code_base != 0)
    {
        *p_code_base = (uint32)ARKRTOS_APPRUN_BASE;
    }
    if(p_ram_base != 0)
    {
        *p_ram_base = (uint32)ARKRTOS_APPRUN_RAM_BASE;
    }
    arkrtos_spin_unlock_irqrestore(&g_apprun_lock, irq);

    return 0;
}

const char *arkrtos_apprun_error_name(int32 rc)
{
    switch(rc)
    {
    case ARKRTOS_APPRUN_ERR_BUSY:     return "a program is already running";
    case ARKRTOS_APPRUN_ERR_WINDOW:   return "the run window could not be taken";
    case ARKRTOS_APPRUN_ERR_LAYOUT:   return "image was not linked for this window";
    case ARKRTOS_APPRUN_ERR_PROLOGUE: return "not an arkc image (bad prologue)";
    default:                          return 0;    /* 不是本路径独有的错误码 */
    }
}

#endif /* ARKRTOS_USE_APPRUN */
