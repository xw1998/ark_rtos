/**
* @brief Ark RTOS 功能选配表（feature gate）
* @details 这个文件只回答一个问题：**这次构建要哪些功能**。
*          它与另外三层正交，写新功能前先确认自己属于哪一层：
*            - 能力层 board/<chip>/arkrtos_board_config.h：这块板/芯片**有什么**
*              （FPU / MPU / 特权级 / 时钟 / 看门狗）。硬件事实，内核只给兜底默认值。
*            - 契约层 kernelsrc/include/arkrtos_hal.h：内核要求硬件**提供什么接口**。
*            - 布局层 config/arkrtos_partition.h：东西**放在哪个地址**。
*          能力层的三个开关（ARKRTOS_USE_FPU / ARKRTOS_USE_MPU / ARKRTOS_USE_PRIV）
*          不在本文件，它们紧跟架构派生留在 arkrtos_config.h 与板级配置里。
*
*          四条规则（新增功能时必须遵守）：
*            1. 任何"不是每个用户都想要"的复杂功能，必须在这里有开关；
*            2. 默认值写在 #ifndef 里，板级或工程可用 -D 覆盖；
*            3. 关掉一个开关后 kernelsrc 必须仍能编译通过（CI 门禁会做裁剪构建）；
*            4. 开关之间的依赖用 #error 显式报出，不要让它烂成链接错误。
*
* @note 时序：arkrtos_config.h 先 include ARKRTOS_BOARD_CONFIG，再 include 本文件，
*       所以板级的 #undef/#define 对这里生效；本文件在 arkrtos_arch.h 之后被包含。
*/

#ifndef __ARKRTOS_FEATURES_H__
#define __ARKRTOS_FEATURES_H__

/* 先吃 Kconfig 生成值。本文件下面每一条开关都是 `#ifndef` 兜底默认值，
 * 而 autoconf.h 里的每一条也是 `#ifndef`——谁先定义谁赢。若这里不先包含
 * autoconf.h，任何「直接 include features.h」的翻译单元就会拿到本文件的
 * 默认值（多为 1），Kconfig 里明明写了 n 也关不掉；实测症状是关档构建
 * 编出实体代码、链接期报 L6218E 未定义符号（arkrtos_fs_mkdir /
 * ark_bus_port_lock …）。命令行 -D 仍优先，因为它在任何头文件之前生效。 */
#include <arkrtos_autoconf.h>

/* ============================================================
 * 一、内核基础机制
 * ============================================================ */

/* 内核自旋锁（arkrtos_spin.h）。关掉后不是没有锁，而是换成纯关中断的
 * 降级实现：单核下够用，省掉原子 CAS 那套 SMP 预留。
 * 不要把它当纯预留开关——对象表、设备表都用它，置 0 仍有锁语义。 */
#ifndef ARKRTOS_USE_SPINLOCK
#define ARKRTOS_USE_SPINLOCK            1
#endif

/* 用户态调度器锁：禁止任务切换的轻量临界区（SVC 0x1B 的 lock/unlock）。
 * 这不是可选项：锁语义没有诚实的降级方式——要么真的不切任务，要么整个
 * 调用作废——内核侧的调用点也就没有关闭分支。置 0 直接报错，别让它
 * 静默退化成一个「永远成功」的空锁。 */
#ifndef ARKRTOS_USE_SCHED_LOCK
#define ARKRTOS_USE_SCHED_LOCK          1
#endif
#if (ARKRTOS_USE_SCHED_LOCK != 1)
#error "ARKRTOS_USE_SCHED_LOCK must stay 1: the scheduler lock has no honest fallback."
#endif

/* SMP 骨架（arkrtos_smp.c / arkrtos_smp.h）。
 * 为什么默认关、并且不许把它读成"支持多核"：本内核的调度器是单核设计
 * （全局当前任务号、全局就绪位图、全局延迟队列），打开这个开关不会把
 * 调度器变成多核。它只提供多核的地基——每核状态块、次核启动入口、核间
 * 中断通路、在线核掩码。关档下类型与符号都不存在，体积与行为逐位不变；
 * 打开后次核只跑 WFI 空转，不参与调度。 */
#ifndef ARKRTOS_USE_SMP
#define ARKRTOS_USE_SMP                 0
#endif

/* 调度分核（AMP）：次核上跑一个**自己的**小调度器，任务可显式迁移到次核
 * 执行（arkrtos_smp_sched_task_to），迁回由 CPU0 每拍回收。
 * 它与 ARKRTOS_USE_SMP 是两层：USE_SMP 给地基（每核状态、次核启动、IPI），
 * USE_SMP_SCHED 才让次核真的跑任务。依赖（SMP=1、CPU_NUM>=2）在
 * arkrtos_config.h 里用 #error 拦下——那里才看得到 ARKRTOS_CPU_NUM。
 *
 * 为什么是 AMP 而不是对称调度：内核的当前任务号 / 就绪位图 / 延时链都是
 * 全局一份，改成每核一份要动调度器主体，风险与验证量都远超一次交付。
 * AMP 只新增每核运行队列，全局调度器一行不改，单核行为逐位不变。
 *
 * 次核上的任务受三条限制（文档写死，代码里按致命处理，不静默跑错）：
 *   1) 不得发 SVC、不得调用内核 API（同步对象/文件/设备表都没有并发保护）；
 *   2) 不支持延时/阻塞：迁移只接受 READY 任务，次核没有延时链；
 *   3) 次核节拍只累加本核计数，不推进全局 arkrtos_kernel_tick。
 */
#ifndef ARKRTOS_USE_SMP_SCHED
#define ARKRTOS_USE_SMP_SCHED           0
#endif

/* 任务栈用量统计：填充图案 + 最低栈指针双手段，算每任务峰值 */
#ifndef ARKRTOS_USE_STACK_USAGE
#define ARKRTOS_USE_STACK_USAGE         1
#endif

/* 栈溢出检测：靠栈尾标志被踩判定 */
#ifndef ARKRTOS_USE_STACK_CHECK
#define ARKRTOS_USE_STACK_CHECK         1
#endif

/* 栈底守卫字：切换与异常入口读 stack_bottom[0] 是否仍是
 * ARKRTOS_STACK_END_FLAG_VAL。与 STACK_CHECK 判的不是同一件事：陷阱帧、
 * 越界指针、DMA 都能改写栈底而保存的 PSP 仍在上面。默认关，关档时
 * 标志字照写但从不读，行为与体积逐字节不变。 */
#ifndef ARKRTOS_USE_STACK_GUARD
#define ARKRTOS_USE_STACK_GUARD         0
#endif

/* 调度器统计：每任务切换次数与耗时 */
#ifndef ARKRTOS_USE_SCHED_STAT
#define ARKRTOS_USE_SCHED_STAT          1
#endif

/* CPU 负载统计：实时空闲率 */
#ifndef ARKRTOS_USE_CPU_LOAD
#define ARKRTOS_USE_CPU_LOAD            1
#endif

/* 节拍按需拉 PendSV：没有更高优先级就绪任务时不触发切换中断 */
#ifndef ARKRTOS_USE_FAST_TICK_SWITCH
#define ARKRTOS_USE_FAST_TICK_SWITCH    1
#endif

/* 故障任务恢复：异常路径回收槽位与资源 */
#ifndef ARKRTOS_USE_FAULT_RECOVER
#define ARKRTOS_USE_FAULT_RECOVER       1
#endif

/* 隔离后端：0 = 窗口（ARM MPU / RISC-V PMP），1 = 页表（MMU）。
 * 由 Kconfig 选择（config ARKRTOS_USE_PAGING）。打开后 arkrtos_paging.c
 * 接管 arkrtos_mpu_* 这一套契约；它要求芯片真有页表部件
 * （ARKRTOS_ARCH_HAS_PAGING_HW == 1）且与 ARKRTOS_USE_MPU 互斥，
 * 三条都在 arkrtos_config.h 里以 #error 拦下，不做静默降级。 */
#ifndef ARKRTOS_USE_PAGING
#define ARKRTOS_USE_PAGING              0
#endif

/* ELF 装载器：0 = 关（空 TU，零体积），1 = 把 ELF64 可执行文件装进
 * 板级给定的用户窗口并以 PLV3 任务起跑（Linux 式入口约定：sp 指向
 * argc/argv）。要求 ARKRTOS_USE_PRIV == 1 且板级给出
 * ARKRTOS_ELF_WINDOW_BASE / ARKRTOS_ELF_WINDOW_SIZE，缺一即 #error。
 * 由 Kconfig 选择（config ARKRTOS_USE_ELF）。 */
#ifndef ARKRTOS_USE_ELF
#define ARKRTOS_USE_ELF                 0
#endif

/* Linux 系统调用 ABI（带 MMU / MPU 目标上的兼容入口，ARKRTOS_USE_LINUX_ABI）。
 * 打开后，ELF 程序可以用 Linux 的调用约定发起请求：RISC-V 上仍是 `ecall`，
 * 但号放在 a7（本移植本来就这么干，因此按号段让位给 Ark 的 0x10..0x22）；
 * LoongArch64 上用 `syscall 0` 并把号放在 $a7（Ark 自己的号写在指令立即数
 * 里，"立即数为 0" 就是两个 ABI 的分界）。号命中 Linux 表就按 Linux 语义
 * 处理，不命中则原样交给 Ark 的 SVC 号表。
 *
 * 这是**兼容层**，不是 Linux 内核：只覆盖静态链接程序启动所需的最小集合
 * （openat/close/read/write/exit/exit_group/set_tid_address/clock_gettime/
 * uname/getpid/getuid/geteuid/brk/mmap/munmap），其余一律返回 -ENOSYS 并
 * 计数，绝不静默当成功。动态链接（PT_INTERP）、fork、信号、fd 继承、真
 * mmap 语义（页级映射与权限）明确不支持，逐条写在
 * docs/Linux可执行文件兼容-系统调用ABI.md 里。
 *
 * 默认关：关档时 arkrtos_linux_abi.c 整文件不参与编译，port 层的两个新
 * 接口也不编译，镜像体积与行为与本开关引入前逐字节相同。
 * 由 Kconfig 选择（config ARKRTOS_USE_LINUX_ABI）。 */
#ifndef ARKRTOS_USE_LINUX_ABI
#define ARKRTOS_USE_LINUX_ABI           0
#endif
#if (ARKRTOS_USE_LINUX_ABI == 1) && (ARKRTOS_USE_ELF != 1)
#error "ARKRTOS_USE_LINUX_ABI=1 requires ARKRTOS_USE_ELF=1 (only ELF user tasks are routed)"
#endif


/* 退出码通道（U-8）：0 = 关（TCB 无字段、SVC 子 4 不认、shell 不打印，
 * 整个功能不占一个字节），1 = 任务可以用 EXIT_CODE 子命令上报 int32
 * 退出码，装载器在任务变 INVALID 时取走，exec 打印 exit code N。
 * 关档时 arkrtos_thread_exit_code() 仍存在（退化成 arkrtos_thread_exit），
 * 调用点不需要 #if。由 Kconfig 选择（config ARKRTOS_USE_EXIT_CODE）。 */
#ifndef ARKRTOS_USE_EXIT_CODE
#define ARKRTOS_USE_EXIT_CODE           0
#endif

/* native SVC 参数块字宽（U-18 剩余项）：0 = 参数块仍是 32 位字数组
 * （默认档，指针字段在窗口 VA 高于 4 GB 时会被截断，宽 VA 的 App 必须
 * 走 Linux ABI 传指针），1 = 参数块改成原生字宽（64 位目标上每字 8
 * 字节）并给子命令字置 bit31，宽 VA 的 App 可以用 native SVC 传指针。
 * 32 位目标上 unsigned long 就是 4 字节，开关为空档。
 * 内核与 App 必须用同一个开关编译：混用时调用会**失败**（宽档内核拒绝
 * 没有标记的块并打一行日志，窄档内核认不出带标记的子命令），不会给出
 * 看似权威的错答案。协议形状见 arkrtos_svc_abi.h。
 * 默认关：默认档的代码、体积与行为零变化。 */
#ifndef ARKRTOS_USE_WIDE_SVC_ARGS
#define ARKRTOS_USE_WIDE_SVC_ARGS       0
#endif

/* 分页帧池（U-3）：0 = 关（port 不定义帧池、四个接口退化成返回 -1/0），
 * 1 = port 在板级给定的物理区间里维护一个 4 KB 页帧位图，提供
 * alloc/free/total/used 四个接口，并在 la_mmu_init() 里给帧池补一条
 * PS=22 恒等映射（否则帧池在内核里不可触及）。要求 ARKRTOS_USE_PAGING == 1
 * 且板级给出 ARKRTOS_QEMU_FRAME_BASE / ARKRTOS_QEMU_FRAME_COUNT（8 MB 对齐、
 * 上限 2048 帧 = 8 MB）。由 Kconfig 选择（config ARKRTOS_USE_PAGING_FRAMES）。
 *
 * 它只是「页帧从哪来」这一层：**不改**现有的 App 窗口映射（那条仍是
 * 每条目 2 MB 的 PS=20），所以打开它不会让任何既有行为变一个字节。
 * 把窗口切成 4 KB 页、按需缺页是后续两阶段（各有自己的开关）。 */
#ifndef ARKRTOS_USE_PAGING_FRAMES
#define ARKRTOS_USE_PAGING_FRAMES       0
#endif

/* 页级 App 窗口（U-3 第二阶）：0 = 关（窗口仍是每个空间一条 2 MB 的 PS=20
 * 条目，与第一阶逐字节相同），1 = 窗口切成 4 KB 页 —— 每个地址空间从帧池
 * 里整块拿 512 帧（2 MB），切换地址空间时重写覆盖窗口的那 256 条 STLB
 * 条目（一条 PS=12 条目管相邻两页）。要求 ARKRTOS_USE_PAGING == 1 且
 * ARKRTOS_USE_PAGING_FRAMES == 1（帧只能从池里来）。由 Kconfig 选择
 * （config ARKRTOS_USE_PAGING_PAGES）。
 *
 * 它只换「窗口怎么映射」这一层：装载仍是 eager 的整块拷贝，退出也不还
 * 帧（空间回收仍欠），没有按需缺页（那是第三阶）。 */
#ifndef ARKRTOS_USE_PAGING_PAGES
#define ARKRTOS_USE_PAGING_PAGES        0
#endif

/* 按需缺页（U-3 第三阶 / U-5）：0 = 关（窗口在 activate 时一次铺满 256 条
 * 条目，与第二阶逐字节相同），1 = 窗口只在**第一次访问**到某一对 8 KB 时
 * 才从帧池取两帧、装一条 PS=12 条目 —— App 的 2 MB 窗口按摸到的页付费。
 * 要求 ARKRTOS_USE_PAGING_PAGES == 1 且 ARKRTOS_USE_PRIV == 1。
 * 由 Kconfig 选择（config ARKRTOS_USE_PAGING_DEMAND）。
 *
 * 它改的是「什么时候装」：打开这一档时 refill 入口先保存被打断的全部通用
 * 寄存器（关档不保存，镜像与行为逐字节不变），再由 la_refill_demand_c()
 * 装好条目并 ertn 回到**出错的那条指令**重试。仍然不还帧。 */
#ifndef ARKRTOS_USE_PAGING_DEMAND
#define ARKRTOS_USE_PAGING_DEMAND       0
#endif

/* Linux ABI 的 fork/clone + wait4（U-19）：0 = 关（ABI 层不认识 220/260，
 * 与打开本开关之前逐字节相同），1 = 给 Linux ABI 用户任务补上「复制一份
 * 地址空间继续跑」这件事：
 *
 *   - clone(220) 只接受 flags == 0 或 flags == SIGCHLD(17)，且 child_stack
 *     必须为 0 —— 这不是线程库，CLONE_VM / CLONE_THREAD / CLONE_VFORK /
 *     CLONE_SETTLS 等一律 -EINVAL；
 *   - 子进程拿到**父进程地址空间的逐字副本**（8 KB 对级拷贝），trap 帧里的
 *     a0 改成 0，于是父返回子 pid、子返回 0；
 *   - wait4(260) 只支持 options == 0 或 WNOHANG(1)。
 *
 * 三条硬边界（文档里逐条写死，不假装修好了）：
 *   1) fd 表 / 管道 / 信号表 / 堆游标仍是**内核全局单份**：子进程继承的是
 *      同一份，不是副本，也没有隔离；
 *   2) wait4 不能阻塞：子进程还活着而 options == 0 时回 -EAGAIN（本内核
 *      没有「阻塞在系统调用里等事件」这条通路）；
 *   3) 子进程必须在任务槽位被复用前 reap。TCB 带一个注册代次，靠它把
 *      「同一个任务号上的新旧两个实例」区分开，晚到的那次如实报 -ECHILD。
 *
 * 要求 ARKRTOS_USE_LINUX_ABI == 1、ARKRTOS_USE_EXIT_CODE == 1、
 * ARKRTOS_USE_PAGING_PAGES == 1、ARKRTOS_USE_PAGING_DEMAND == 1（地址空间
 * 复制只在按需分页的 LoongArch64 端口上实现），缺一即 #error。
 * 由 Kconfig 选择（config ARKRTOS_USE_LINUX_FORK）。
 * 默认关：关档时 port 的三个新接口、装载器的 fork 入口、ABI 的 220/260
 * 两个号全部不编译，镜像体积与行为零变化。 */
#ifndef ARKRTOS_USE_LINUX_FORK
#define ARKRTOS_USE_LINUX_FORK          0
#endif
#if (ARKRTOS_USE_LINUX_FORK == 1)
#if (ARKRTOS_USE_LINUX_ABI != 1)
#error "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_LINUX_ABI=1"
#endif
#if (ARKRTOS_USE_EXIT_CODE != 1)
#error "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_EXIT_CODE=1 (wait4 can only report a status the child did report)"
#endif
#if (ARKRTOS_USE_PAGING != 1) || (ARKRTOS_USE_PAGING_PAGES != 1) || (ARKRTOS_USE_PAGING_DEMAND != 1)
#error "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_PAGING_PAGES=1 and ARKRTOS_USE_PAGING_DEMAND=1"
#endif
#endif

/* U-16 页替换（swap）：0 = 关（一个字节都不编进镜像），1 = 帧池空了就把某一对
 * 8 KB 换出到内核静态交换区、把两帧还回池子，缺页时再换回来。
 * 要求 ARKRTOS_USE_PAGING_DEMAND == 1 —— 只有按需档才有「这一对还没驻留」
 * 这个状态可以换；页级窗口档（第二阶）里 2 MB 是一整块，没有可换的单位。
 * 由 Kconfig 选择（config ARKRTOS_USE_PAGING_SWAP）。
 *
 * 为什么交换区是内核 .bss 而不是文件：换出发生在重填上下文里（ISTLBR=1），
 * 那里不能调 VFS、不能睡眠、不能取锁，能做的只有内存到内存的拷贝。所以这一
 * 档验证的是**账本与搬运的正确性**（换出的内容换回来逐字不变、帧数回到原值），
 * 不是「省了内存」：静态交换区自己也占内存。要变成真的省内存，得先有可写的
 * 持久交换介质，那属于块设备那一层的活。 */
#ifndef ARKRTOS_USE_PAGING_SWAP
#define ARKRTOS_USE_PAGING_SWAP         0
#endif

/* ASID / 空间标签（U-7）：0 = 关（ASID 恒 0，切地址空间靠整段 invtlb +
 * 重铺 256 条 PS=12 条目，与 U-3 第二阶逐字节相同），1 = 每个地址空间一个
 * ASID（空间 s -> ASID s+1，内核默认视图是 space 0 的 ASID），窗口条目按
 * ASID 区分、每个空间独占一路 STLB（空间 s 用 way s % 8），切换空间只剩
 * 一次 CSR.ASID 写入 —— 关档每次切换要 512 次 invtlb + 256 次 tlbwr。
 * 要求 ARKRTOS_USE_PAGING_PAGES == 1（PS=20 的静态窗口没有「每空间一组
 * 条目」可复用）且 ARKRTOS_PAGING_APPS_MAX <= 8（一路一个空间）。
 * 由 Kconfig 选择（config ARKRTOS_USE_PAGING_ASID）。
 *
 * 这一档改的是**切换的代价与条目的归属**，不是隔离强度：关档下切空间会
 * 把旧空间的条目整段失效，开档下它们留在 TLB 里只是换了 ASID，别的空间
 * 仍然翻译不到（条目 ASID 不匹配就是未命中）。 */
#ifndef ARKRTOS_USE_PAGING_ASID
#define ARKRTOS_USE_PAGING_ASID         0
#endif

/* VxWorks 式可下载模块：0 = 关（空 TU，零体积），1 = 把 ET_REL 目标文件
 * 现场链接进内核模块区并以 PLV0 任务起跑（按名字解析内核导出符号）。
 * 这是与 ARKRTOS_USE_ELF **并列的另一种应用模型**，不是它的替代：
 * 前者是「内核插件（放弃隔离）」，后者是「用户程序（PLV3 隔离）」。
 * 两者可以同时打开（四种组合都能编）。要求 ARKRTOS_USE_PRIV == 1、
 * 板级给出 ARKRTOS_MOD_AREA_SIZE、64 位端口，
 * 缺一即 #error。由 Kconfig 选择（config ARKRTOS_USE_MODLOAD）。 */
#ifndef ARKRTOS_USE_MODLOAD
#define ARKRTOS_USE_MODLOAD             0
#endif

/* MPU 上下文差异应用缓存：同一任务重复进入时不重写 MPU 寄存器 */
#ifndef ARKRTOS_USE_MPU_APPLY_CACHE
#define ARKRTOS_USE_MPU_APPLY_CACHE     1
#endif

/* ============================================================
 * 二、同步原语与 IPC
 * @note 信号量 / 互斥锁 / 条件变量 / 事件组属于内核核心，不提供关断：
 *       设备框架、POSIX 层、加载器都建立在它们之上，关掉会连锁。
 * ============================================================ */

/* 固定块内存池（arkrtos_mempool.c）。内核不拥有堆，长时间运行的组件
 * （将来的 socket 请求、加载器暂存）需要一块"不会碎"的内存时用它。
 * 池的存储区由调用者提供，模块只维护对象表（ARKRTOS_MEMPOOL_NUM 项）。 */
#ifndef ARKRTOS_USE_MEMPOOL
#define ARKRTOS_USE_MEMPOOL             1
#endif

/* 消息队列（SVC 0x16） */
#ifndef ARKRTOS_USE_MQ
#define ARKRTOS_USE_MQ                  1
#endif

/* 软件定时器（SVC 0x17） */
#ifndef ARKRTOS_USE_TIMER
#define ARKRTOS_USE_TIMER               1
#endif

/* 流缓冲 / 消息缓冲（SVC 0x1F）。
 * 为什么默认关：它是一块按 ARKRTOS_SBUF_NUM * ARKRTOS_SBUF_BUF_SIZE 静态
 * 驻留的字节区，只有真正要跟串口字节流 / 变长帧打交道的工程才需要；
 * 默认关时既不占 RAM，也不产生任何符号。 */
#ifndef ARKRTOS_USE_SBUF
#define ARKRTOS_USE_SBUF                0
#endif

/* 32 位事件集（SVC 0x21，arkrtos_eset.c）。
 * 为什么单独一个开关、且默认关：arkrtos_event 的"单标志 + 广播 + 自动复位"
 * 已经覆盖现有全部调用点（POSIX 门面、驱动框架、小程序）；事件集是另一套
 * 契约——32 个独立位、按位掩码等待（OR / AND）、可选命中后清除。默认关时
 * 本模块完全不编：对象表、等待槽、SVC 分派项都不存在，体积与行为逐位不变。 */
#ifndef ARKRTOS_USE_ESET
#define ARKRTOS_USE_ESET                0
#endif

/* 递归互斥锁（同步服务子命令 ARKRTOS_SYNC_SUB_MTX_CREATE_RECURSIVE=17）。
 * 默认关：创建函数、SVC 子命令 case、SDK 包装都不编进来；关档下普通锁的
 * 重复加锁仍返回 RECURSIVE(-1)，体积与行为逐位不变。打开后也只有显式用
 * arkrtos_mtx_create_recursive 创建的锁可递归，普通锁语义不变。 */
#ifndef ARKRTOS_USE_MTX_RECURSIVE
#define ARKRTOS_USE_MTX_RECURSIVE       0
#endif
/* 等待者容量：默认每个对象固定 ARKRTOS_MAX_SYNC_WAITERS（4）个等待槽。
 * 打开这个开关后槽位放大到任务表容量（ARKRTOS_TASK_MAX_NUM）。理由：一个
 * 任务同一时刻只能等一个对象，所以任何对象的等待者数不可能超过任务数——
 * 对本内核来说这就是「无上限」，不需要链表也不需要堆。代价是所有对象表
 * 一起线性变大（每个槽 4 字节 × 所有对象表的排队数目），因此默认关。 */
#ifndef ARKRTOS_USE_WAIT_UNBOUNDED
#define ARKRTOS_USE_WAIT_UNBOUNDED       0
#endif

/* 多对象同时就绪探测（SVC 0x22）。
 * 一次问一组句柄「谁已就绪」，只报状态、不消费令牌；带超时的等待由
 * SDK 侧轮询完成。默认关时本模块、SVC 分派项与 SDK 包装都不编进来。 */
#ifndef ARKRTOS_USE_WAITMULTI
#define ARKRTOS_USE_WAITMULTI           0
#endif

/* tickless idle。
 * 为什么默认关：省的是 idle 期间的中断次数，代价是唤醒路径要自己重建
 * "这段时间过去了多少"，属于改调度时序的功能；默认关时 idle 循环就是
 * 一次 WFI，行为与没有这个开关时逐位相同。 */
#ifndef ARKRTOS_USE_TICKLESS
#define ARKRTOS_USE_TICKLESS            0
#endif

/* 一次最多跳过的节拍数（仅 tickless 打开时有意义）。默认 8 拍 = 4 ms
 * （500 us 一拍）：够让 idle 的 tick 中断从 2000/s 降到 ~125/s，
 * 又保证唤醒后补发的节拍只有几拍。 */
#ifndef ARKRTOS_TICKLESS_MAX_TICKS
#define ARKRTOS_TICKLESS_MAX_TICKS      8
#endif

/* 实时性度量（周期数器）。
 * 为什么默认关：它在节拍中断和临界区里多读一次周期计数器，
 * 开着它测到的关中断时长包含测量自身的开销；
 * 默认关时这些点全是死代码，体积与行为零变化。 */
#ifndef ARKRTOS_USE_RTSTAT
#define ARKRTOS_USE_RTSTAT              0
#endif

/* 安全基线自检（sec 命令）。只读：不改任何结构、不装任何策略。
 * 十二项逐条给 PASS/FAIL/FINDING/SKIP，SKIP 必须写清为什么测不了。
 * 默认关时整个 .c 不编入、shell 命令表不变：体积与行为零变化。 */
#ifndef ARKRTOS_USE_SECCHECK
#define ARKRTOS_USE_SECCHECK            0
#endif

/* 扁平设备树解析器（MMU 档）。
 * MCU 档不解析任何东西：dtc_lite.py 直接生成一张 const 平表。带 MMU 的
 * 档位按正常 Linux 形态项目走——镜像里放一块标准 .dtb，启动时解析。
 * 默认关时 arkrtos_fdt.c 整文件不产生符号，shell 命令表不变，
 * 镜像与没有这个开关时逐字节相同。 */
#ifndef ARKRTOS_USE_FDT
#define ARKRTOS_USE_FDT                 0
#endif

/* 设备树的第二件事：消费 /chosen/stdout-path。默认 0，关档时
 * arkrtos_fdt_console.c 整文件不产生符号、启动日志一行不多。 */
#ifndef ARKRTOS_USE_FDT_CONSOLE
#define ARKRTOS_USE_FDT_CONSOLE         0
#endif

/* 设备树的第三件事：dtb overlay（最小子集）。
 * 认：target-path 与 target = <&label>（经 fragment 的 __fixups__ + 基树
 * __symbols__ 还原）、属性覆盖 / 新属性、新子节点追加、长度 0 删属性、
 * __delete 标记删子节点。拒：fragment 侧的 __symbols__ / __local_fixups__、
 * 已解析的 phandle、相对路径、同名子节点。默认 0：关档时
 * arkrtos_fdt_overlay.c 整文件不产生符号。 */
#ifndef ARKRTOS_USE_FDT_OVERLAY
#define ARKRTOS_USE_FDT_OVERLAY         0
#endif

/* ============================================================
 * 三、服务与组件
 * ============================================================ */

/* 内核日志（arkrtos_log.c）。Shell 的 log 命令依赖它，关掉后该命令一起消失 */
#ifndef ARKRTOS_USE_LOG
#define ARKRTOS_USE_LOG                 1
#endif

/* 内核日志环（dmesg）：保留最近若干字节的格式化日志行，供 shell 的 dmesg
 * 命令事后回看。关掉整块不进二进制。容量见 ARKRTOS_LOG_RING_SIZE。 */
#ifndef ARKRTOS_USE_LOG_RING
#define ARKRTOS_USE_LOG_RING            1
#endif

/* 块设备层（arkrtos_blk.c）：设备注册表与文件系统之间的薄层 */
#ifndef ARKRTOS_USE_BLK
#define ARKRTOS_USE_BLK                 1
#endif

/* 文件系统门面 + littlefs（arkrtos_fs.c，SVC 0x1C）。依赖 ARKRTOS_USE_BLK */
#ifndef ARKRTOS_USE_FS
#define ARKRTOS_USE_FS                  1
#endif

/* Linux 风格路径命名空间（arkrtos_vfs.c + components/ark_vfs）：/ 用 ramfs、
 * /dev 是设备注册表的镜像、/mnt/<名字> 是持久卷。shell 的 mount/ls/cat
 * 与 App 的按路径访问共用这一层。关掉后门面本身还在（返回 NOSYS），
 * 但内核里不再有任何资源能按路径找到——所以实际上就是没有 VFS。
 * 依赖 ARKRTOS_USE_BLK：持久卷后端是把 arkrtos_blk 的一段窗口包成 ark_vfs
 * 的 backend，本构建不提供「无块设备的 VFS」这种变体。 */
#ifndef ARKRTOS_USE_VFS
#define ARKRTOS_USE_VFS                 1
#endif

/* VFS 的 littlefs 后端（arkrtos_vfs_lfs.c）：把 /mnt/<名字> 接到 arkrtos_fs。
 * 关掉后 /mnt 下仍可挂调用者自己实现的 fsdrv，只是内核不再自带持久卷桥 */
#ifndef ARKRTOS_USE_VFS_LFS
#define ARKRTOS_USE_VFS_LFS             1
#endif

/* 内核状态虚拟文件系统 /proc（components/ark_vfs 的 procfs 端口驱动）：
 * 只读、扁平，把 version/uptime/meminfo/cpuinfo/tasks 暴露成 /proc/<name>。
 * 内容每次读取时现从内核生成，不占存储、不持缓冲；由 ark_vfs_port_init()
 * 以 ARK_MNT_RO 挂到 /proc。依赖 ARKRTOS_USE_VFS：它是命名空间里的一个挂载点。 */
#ifndef ARKRTOS_USE_PROCFS
#define ARKRTOS_USE_PROCFS              1
#endif

/* 内核 Shell 控制台（SVC 0x1A）。开关本身是 Kconfig 的 ARKRTOS_USE_SHELL，
 * 但代码侧（arkrtos_shell.c 的整文件门控）与历史约定用的是 SHELL_ENABLE
 * （原先定义在 config/arkrtos_partition.h，那只是分区头的历史遗留位置）。
 * 这里把它接成**同一个开关的别名**，不是第二个开关：一个 Kconfig 符号、
 * 一个值。命令行先定义 SHELL_ENABLE 时以命令行为准（先定义者赢）。
 * 实测：关掉这一档省 33184 B Flash / 14620 B RAM，是单块最大的选配。 */
#ifndef SHELL_ENABLE
#define SHELL_ENABLE                    ARKRTOS_USE_SHELL
#endif

/* 控制台登录（SEC-G01）：串口可达就能装/停/重启镜像，所以「谁能敲命令」
 * 本身是安全边界。这一档给控制台加一道登录：开档后每条命令都要先过门，
 * 只有 login / logout / help 在锁定期间放行。
 *
 * 凭据怎么来（没有任何默认口令）：
 *   密码不落设备，落设备的是 sha256(salt || password) 的 32 字节摘要与 16
 *   字节盐，写在一个不进仓库的头文件里（.gitignore 忽略
 *   *_shell_auth_local.h），构建时用 -DARKRTOS_SHELL_AUTH_LOCAL=1 引进来：
 *     #define ARKRTOS_SHELL_AUTH_SALT    { 0x.., ... }   (16 字节)
 *     #define ARKRTOS_SHELL_AUTH_DIGEST  { 0x.., ... }   (32 字节)
 *   开档却没给摘要就直接 #error —— 不给「开了但没人能登录」或「开了但有个
 *   通用默认口令」这两种假绿。
 *   摘要由 tools/shell_auth_cred.py 生成（同一份盐与口令，Python 侧算）。
 *
 * 诚实边界：
 *   - 口令在串口上明文传输（串口没有加密层），能被抓包嗅到；
 *   - 摘要与盐都在固件里，拿到固件的人可以离线爆破弱口令；
 *   - 它不是设备身份（那是 SEC-G13），也不能阻止整片刷写 Flash。
 *   它回答的是「串口敲命令的人是不是运维」。
 *
 * 连续失败 ARKRTOS_SHELL_AUTH_TRIES 次后锁定到底（复位才恢复），不做指数
 * 退避：串口是低速链，退避只会让暴力尝试更便宜。 */
#ifndef ARKRTOS_USE_SHELL_AUTH
#define ARKRTOS_USE_SHELL_AUTH          0
#endif
#ifndef ARKRTOS_SHELL_AUTH_TRIES
#define ARKRTOS_SHELL_AUTH_TRIES        5
#endif
#if defined(ARKRTOS_SHELL_AUTH_LOCAL)
#include "arkrtos_shell_auth_local.h"
#endif
#if (ARKRTOS_USE_SHELL_AUTH == 1)
#if (SHELL_ENABLE != 1)
#error "ARKRTOS_USE_SHELL_AUTH=1 requires the shell console (SHELL_ENABLE=1): there is nothing to log into"
#endif
#if !defined(ARKRTOS_SHELL_AUTH_SALT)
#error "ARKRTOS_USE_SHELL_AUTH=1 requires ARKRTOS_SHELL_AUTH_SALT (16 byte initialiser list)"
#endif
#if !defined(ARKRTOS_SHELL_AUTH_DIGEST)
#error "ARKRTOS_USE_SHELL_AUTH=1 requires ARKRTOS_SHELL_AUTH_DIGEST (32 byte initialiser list)"
#endif
#endif

/* Shell 开着但控制台那套常量（SHELL_DEV_NAME / SHELL_TASK_STACK_SIZE …）
 * 仍在 config/arkrtos_partition.h：那是板级资源，不是功能开关。 */

/* 小程序（arkrtos_mini.c）：把文件系统里的一份负载按需 load 进 RAM 执行，
 * 退出或停止即把整块 RAM 还回档位池。它复用 App 的镜像容器与 App SDK，
 * 但**不进镜像池、不占槽位**：每次运行现从文件系统读、现借一块 RAM、
 * 跑完即还——牺牲效率换「放上就能跑、不用安装」。
 * 依赖 ARKRTOS_USE_FS：镜像就放在文件系统里，没有文件系统就没有小程序。 */
#ifndef ARKRTOS_USE_MINIAPP
#define ARKRTOS_USE_MINIAPP             1
#endif

/* 同一时刻能跑几个小程序（编译期上限）。
 * 它同时受两个硬约束，两个都是结构性的，不是调参：
 *   1. 共享表里记账用的数组长度是 ARKRTOS_MINI_ARRAY_MAX（ABI 形状常量），
 *      本值必须 <= 它；
 *   2. 每个在跑的小程序要占一块代码块 + 一块 RAM 块，而 MPU 每任务只给
 *      两个区域（代码窗/数据窗），所以并发数不影响单任务，但会影响 RAM 池。
 * 运行期还能在这个上限内再收紧（shell: mini count <n>），不能放宽——
 * 「设了 8 却只跑 2 个」会是一句看不出来的谎话，所以设超范围直接报错。
 * 默认 2：够用来跑「一个干活 + 一个被观察」，静态开销也小（每项只是一组
 * 记账字段与一组状态）。 */
#ifndef ARKRTOS_MINI_MAX
#define ARKRTOS_MINI_MAX                2u
#endif

/* 小程序从哪个命名空间读镜像。
 *   0（默认）= arkrtos_fs（littlefs 卷）——与本开关引入之前逐字节一致。
 *   1        = VFS（任意已挂载命名空间，含板上编译器写产物的 /tmp ramfs）。
 * 为什么是一个开关而不是「两个都试」：两条读路径的前置条件不同（前者要卷已挂、
 * 后者要 VFS 就绪），失败原因也不同；「先试 A 失败再试 B」会把「A 上明明有文件
 * 但读坏了」变成「悄悄按 B 解释路径」，那是一种看不出来的错答案。
 * 打开前必须 ARKRTOS_USE_VFS=1（下面的依赖校验挡住了关错）。*/
#ifndef ARKRTOS_MINI_READ_VFS
#define ARKRTOS_MINI_READ_VFS           0
#endif

/* 可重定位镜像打包（L2）。
 *   0（默认）= kernelsrc/src/arkrtos_apppack.c 根本不进编译，体积与行为逐字节不变。
 *   1        = 编入「四遍差分」打包器：同一份 C 程序在四个标称基址上各编一遍
 *              （A 标称 / B 代码+Δ / C 数据+Δ / D 代码+2Δ、数据+Δ），由差分出
 *              重定位表，产出带表、可直接被 loader 装载的 .svcm 容器。
 * 为什么放在内核侧而不是编译器侧：差分只认「同一段字节在两个基址上的差别」，
 * 与 arkc 的 IR、后端、优化档全部无关；放在这里，PC 上的 tools/pack_app.py
 * 才有可能与板上跑的是同一份算法、逐字节对拍。当前唯一调用方是 arkc 组件的
 * `cc --pack`。本模块不读文件、不分配内存（缓冲由调用方给），因此不强制依赖 VFS。*/
#ifndef ARKRTOS_USE_APPPACK
#define ARKRTOS_USE_APPPACK             0
#endif

/* 小程序装载时，每读 ARKRTOS_MINI_YIELD_CHUNKS 个分块就放开一次调度器锁、
 * 再重新拿回，让更高优先级的任务（例如正在跑的控制回路）在两次分块读之间
 * 拿到 CPU。单次分块读（open/read/close 一整段）本身在锁内是原子的，所以
 * 放开锁不会让两个装载者交错读镜像；两次读之间也没有需要保护的中间状态。
 * 0 = 关闭：整段装载期间一直持锁，体积与行为与本开关引入之前完全一致。
 * 默认 1：每个分块让出一次——这是 mini run 期间控制回路不再被整段冻结的原因。*/
#ifndef ARKRTOS_MINI_YIELD_CHUNKS
#define ARKRTOS_MINI_YIELD_CHUNKS       1u
#endif

/* 任务热切换（A/B 双缓冲无缝升级的内核一侧）。
 *   0（默认）= 一整块代码不编进来，体积与行为与引入本开关之前逐字节一致。
 *   1        = 编入 arkrtos_task_swap_arm() / arkrtos_task_swap_take()：把一个
 *              正在跑的任务在「下一次 SVC 的硬件栈帧」上就地换到另一份镜像上
 *              ——PC 改成新入口、PSP 挪到新数据块（整块先拷过去）、MPU 窗口跟上。
 *              不重启任务、不经过调度器、不写 Flash、不碰文件系统。
 *
 * 为什么默认关：它改的是「异常返回落到哪里」这一个字。没有板上验证之前，
 * 任何默认档都不该带上这种能力。打开后小程序侧（mini swap）与 App 侧
 * （app swap）两条命令才出现。
 *
 * 边界（都是刻意的）：数据块整块按字节搬过去，所以新旧镜像的 RAM 块大小
 * 必须相同（数据布局一致）；用过 FPU 的任务其栈帧是扩展帧，这里直接拒绝
 * 而不是猜——换错一个字的代价是任务在错误地址上取指。 */
#ifndef ARKRTOS_USE_TASK_SWAP
#define ARKRTOS_USE_TASK_SWAP           0
#endif

/* 架构判定要用到核心编码（ARKRTOS_CPU_CORE_*）：本文件可能被单个 .c 直接
 * include，那时 arkrtos_arch.h 还没进来，未定义的宏在 #if 里是 0，会把
 * 每个档都判成「不支持」。arch.h 自带包含守卫、只依赖 types，重复包含无
 * 代价；板级头由 -include 强制先于一切，核心推导不会走错。 */
#include "arkrtos_arch.h"

/* 三档有实现：Cortex-M 的 PSP 基本帧、RV32IMAC 的 mepc/mstatus 帧、
 * LA64 的 ERA/PRMD 帧。三者各自改自己那份帧，全部沉在 port 层
 * （arkrtos_port_swap_*），内核不出现架构分支。
 * RV64GC / LA32 的帧形状没有实现：在那里开档会改错寄存器，所以编译期
 * 拒绝，而不是给一个编得过但语义错的假答案。 */
#if (ARKRTOS_USE_TASK_SWAP == 1) && !(ARKRTOS_ARCH_IS_ARM) && \
    (ARKRTOS_ARCH_CORE != ARKRTOS_CPU_CORE_RV32IMAC) && \
    (ARKRTOS_ARCH_CORE != ARKRTOS_CPU_CORE_LA64)
#error "ARKRTOS_USE_TASK_SWAP=1 is implemented for Cortex-M, RV32IMAC (0x0201) and LA64 (0x0302) only: each one rewrites its own trap frame through the port layer. RV64GC / LA32 frames are not implemented; enabling it there would rewrite the wrong registers. See docs: cross-architecture mechanism matrix."
#endif


/* POSIX / Windows 兼容层（kernelsrc/sdk/posix）。关掉后 App 只能用
 * 原生 oslib，不能 include <pthread.h> 那一套 */
#ifndef ARKRTOS_USE_POSIX
#define ARKRTOS_USE_POSIX               1
#endif

/* POSIX 层把 malloc/free 映射到自己的堆 */
#ifndef ARKRTOS_POSIX_WRAP_STDLIB
#define ARKRTOS_POSIX_WRAP_STDLIB       0
#endif

/* lwIP TCP/IP 协议栈（components/lwip）+ 其 Ark RTOS 端口（port/）。
 * 打开后内核多出一个周期轮询任务，为 socket / select 兼容面提供回环网卡
 * （127.0.0.1/8，lwIP 2.1.3 内建）。第三方源码本身不带条件编译，本开关
 * 只门控 port/ 那一层：关掉即不编 port，socket 相关 SVC 返回 NOSYS。
 * 真实以太网网卡尚未接入——当前只能验证协议栈与 socket 语义，不能验证收发。
 * 体积不小（源码 2.5MB、静态 RAM 数十 KB），不是每个用户都要，
 * 所以默认关闭，由工程用 -D 显式开。 */
#ifndef ARKRTOS_USE_LWIP
#define ARKRTOS_USE_LWIP                0
#endif

/* MDK trace 采集（components/mdk_trace，依赖 DWT/ITM 调试硬件） */
#ifndef ARKRTOS_USE_MDK_TRACE
#define ARKRTOS_USE_MDK_TRACE           1
#endif

/* cm_backtrace 故障回溯（components/cm_backtrace，依赖 DWT/NVIC） */
#ifndef ARKRTOS_USE_CM_BACKTRACE
#define ARKRTOS_USE_CM_BACKTRACE        1
#endif

/* ============================================================
 * 四、可靠性与运维
 * ============================================================ */


/* 镜像数字签名（HMAC-SHA256）：安装时重算镜像的签名并与镜像头里的
 * signature[64] 比对，不一致直接拒绝安装。开这个开关必须提供一把 32 字节
 * 的预共享密钥 ARKRTOS_IMAGE_SIGN_KEY（32 个字节的初始化列表），否则编译期
 * 就报错，而不是等到安装时才发现「没有密钥」。
 * 这是一套**对称**方案：能读到设备里这把密钥的人就能签新镜像，所以它是
 * 「防篡改 + 认来源」，不是完整信任链——设备只持公钥的非对称方案才是，
 * 那不是这里实现的（详见 kernelsrc/include/arkrtos_sign.h）。
 * 默认关：需要密钥，不是每个用户都要。 */
#ifndef ARKRTOS_USE_IMAGE_SIGN
#define ARKRTOS_USE_IMAGE_SIGN          0
#endif
/* 密钥注入通道：32 字节初始化列表放在一个不进仓库的头文件里（.gitignore 忽略
 * *_sign_key_local.h），开关 ARKRTOS_SIGN_KEY_LOCAL=1 时把它包含进来：
 *   -DARKRTOS_USE_IMAGE_SIGN=1 -DARKRTOS_SIGN_KEY_LOCAL=1
 * 头文件里写一行： #define ARKRTOS_IMAGE_SIGN_KEY  { 0x.., ... }
 * 这里用无引号的开关，而不是 -DARKRTOS_SIGN_KEY_HEADER=\"xxx.h\"：Keil 的
 * Define 框里同一个字段放两个带引号的宏时，后面的 ARKRTOS_BOARD_CONFIG 会
 * 传不过去（实测 drvflash.c / arkrtos_port.c 因缺 HAL/CMSIS 头报 39 个
 * uint32_t/SCB 未定义错误）。
 * 不定义 ARKRTOS_SIGN_KEY_LOCAL 时本段完全不参与，默认档零变化。 */
#if defined(ARKRTOS_SIGN_KEY_LOCAL)
#include "arkrtos_sign_key_local.h"
#endif
#if (ARKRTOS_USE_IMAGE_SIGN == 1) && !defined(ARKRTOS_IMAGE_SIGN_KEY)
#error "ARKRTOS_USE_IMAGE_SIGN=1 requires ARKRTOS_IMAGE_SIGN_KEY (32 byte initialiser list)"
#endif

/* ELF 可执行文件与可下载模块的签名门（ARKRTOS_USE_EXEC_SIGN）。
 * 打开后 `exec` 与 `ld` 只接受「文件末尾追加 32 字节 HMAC tag」的输入，
 * 校验覆盖 tag 之前的全部字节（格式与理由见 kernelsrc/include/arkrtos_sign.h）。
 * 为什么单独一个开关而不是并进 IMAGE_SIGN：IMAGE_SIGN 管的是 App 安装/升级
 * 路径，已经打开它的板子不该因为我们多管了 exec/ld 而行为突变。
 * 默认关；关档时装载器里那几行不参与编译，体积与行为零变化。
 * 它依赖 IMAGE_SIGN（同一把预共享密钥、同一套 SHA-256/HMAC 实现）。 */
#ifndef ARKRTOS_USE_EXEC_SIGN
#define ARKRTOS_USE_EXEC_SIGN           0
#endif
#if (ARKRTOS_USE_EXEC_SIGN == 1) && (ARKRTOS_USE_IMAGE_SIGN != 1)
#error "ARKRTOS_USE_EXEC_SIGN=1 requires ARKRTOS_USE_IMAGE_SIGN=1 (same pre-shared key)"
#endif

/* ELF 可执行文件与可下载模块的签名门（ARKRTOS_USE_EXEC_SIGN）。
 * 打开后 `exec` 与 `ld` 只接受「文件末尾追加 32 字节 HMAC tag」的输入，
 * 校验覆盖 tag 之前的全部字节（格式与理由见 kernelsrc/include/arkrtos_sign.h）。
 * 为什么单独一个开关而不是并进 IMAGE_SIGN：IMAGE_SIGN 管的是 App 安装/升级
 * 路径，已经打开它的板子不该因为我们多管了 exec/ld 而行为突变。
 * 默认关；关档时装载器里那几行不参与编译，体积与行为零变化。
 * 它依赖 IMAGE_SIGN（同一把预共享密钥、同一套 SHA-256/HMAC 实现）。 */
#ifndef ARKRTOS_USE_EXEC_SIGN
#define ARKRTOS_USE_EXEC_SIGN           0
#endif
#if (ARKRTOS_USE_EXEC_SIGN == 1) && (ARKRTOS_USE_IMAGE_SIGN != 1)
#error "ARKRTOS_USE_EXEC_SIGN=1 requires ARKRTOS_USE_IMAGE_SIGN=1 (same pre-shared key)"
#endif

/* 跨复位崩溃日记（arkrtos_crash.c）：落在共享内存尾部的 .noinit 区 */
#ifndef ARKRTOS_USE_CRASH_LOG
#define ARKRTOS_USE_CRASH_LOG           1
#endif

/* App 崩溃时的硬件安全态（"遗言"）机制（arkrtos_hwsafe.c）。
 * 驱动可为某个设备注册一个"中和"回调，声明"它的所有者死掉时把它带回安全态"
 * （例如把电机 PWM 停掉、把继电器断开）；内核在设备被打开时按任务记一条所有权
 * 台账，任务死亡（自退、被杀、故障恢复）时由内核——而不是崩溃的 App——执行该回调。
 * 默认关：关掉时设备 open/close 与任务终止的路径与本机制引入前完全一致，
 * 体积与行为零变化。见 docs/App崩溃硬件安全与遗言机制.md。 */
#ifndef ARKRTOS_HW_SAFE
#define ARKRTOS_HW_SAFE                 0
#endif

/* 小程序「优雅退出」是否算计划内交接（无缝升级）。
 * 1（默认）= 小程序主任务正常 return 时，硬件安全态机制只摘台账、不驱动硬件——
 *            接手的新版本马上会重新 open 同一台设备，此刻把它打到安全态是抖动而非安全。
 * 0        = 与非小程序一致，正常退出也执行遗言/回调（硬件立刻回安全态）。
 * 只作用于『小程序主任务正常退出』这一条路径；崩溃 / 被强杀 / 故障恢复一律走完整中和。
 * 仅在 ARKRTOS_USE_MINIAPP=1 时有意义。见 docs/App崩溃硬件安全与遗言机制.md。 */
#ifndef ARKRTOS_MINI_EXIT_IS_HANDOVER
#define ARKRTOS_MINI_EXIT_IS_HANDOVER   1
#endif

/* 软总线（components/ark_softbus）：跨设备的事件/远程调用/数据平面总线。
 * 组件本体是 ark_softbus 源仓库的同步副本（tools/sync_ark_softbus.py），
 * 内核这一层只接板级三钩子 + 一条板内 loopback 链路 + 控制台命令 `bus`，
 * 见 kernelsrc/src/arkrtos_softbus.c 与 docs/软总线接入.md。
 * 默认关：关掉时不含任何总线代码，体积与行为零变化。
 * 打开时必须同时在工程侧给出 MCU 档 -DARK_BUS_* （清单与闸门见
 * kernelsrc/include/arkrtos_softbus.h 顶部），否则编译期 #error 而不是
 * 悄悄按默认档多占几 KB RAM。 */
#ifndef ARKRTOS_USE_SOFTBUS
#define ARKRTOS_USE_SOFTBUS             0
#endif

/* 心跳合同与看门狗监督（arkrtos_guard.c）。需要板级 ARKRTOS_WDG_ENABLE 才真咬人 */
#ifndef ARKRTOS_USE_KERNEL_GUARD
#define ARKRTOS_USE_KERNEL_GUARD        1
#endif

/* 结构自检（arkrtos_audit.c）：分区表 / 槽位 / 镜像头的一致性检查与审计记录。
 * 关掉后加载器仍会做最低限度的合法性判断，只是不再逐项记账 */
#ifndef ARKRTOS_USE_AUDIT
#define ARKRTOS_USE_AUDIT               1
#endif

/* 板端单元测试框架（arkrtos_utest.c）：用例自动注册 + 断言记账 + 汇总，
 * 对齐 RT-Thread utest / Zephyr ztest 的形态。关掉后框架与 shell 的 utest 命令都不参与编译，
 * 体积与行为零变化。自动注册需要构造函数支持（armclang/GCC）。 */
#ifndef ARKRTOS_USE_UTEST
#define ARKRTOS_USE_UTEST               1
#endif

/* Text editor component (kernelsrc/components/arkvim): the upstream arkvim
 * modal editor wired to the kernel VFS, plus the shell "vim" command.  Off by
 * default, and while it is 0 the sources are not compiled at all and the
 * command table is byte-for-byte what it was: the component cannot change
 * size or behaviour until somebody asks for it. */
#ifndef ARKRTOS_USE_ARKVIM
#define ARKRTOS_USE_ARKVIM              0
#endif

/* C compiler component (kernelsrc/components/arkc): arkc self-hosted on the
 * target, wired to the kernel VFS, plus the shell "cc" command.  Off by
 * default for the same reason as the editor: at 0 the sources are not
 * compiled and the command table does not change by a byte.  arkc needs
 * about 210 KB of RAM for its static arena/buffers, which the linker
 * script places in the NOLOAD .ark_buf section. */
#ifndef ARKRTOS_USE_ARKC
#define ARKRTOS_USE_ARKC                0
#endif

/* ark make component (kernelsrc/components/arkmake): a small project-file
 * builder that turns several .c files into one runnable image through the
 * same four-pass packer as "cc --pack".  Off by default like every other
 * component: at 0 the sources are not compiled and the command table is
 * byte-for-byte what it was. */
#ifndef ARKRTOS_USE_ARKMAKE
#define ARKRTOS_USE_ARKMAKE            0
#endif

/* External SDRAM on FMC SDRAM Bank1.  Off by default: at 0 no FMC register is
 * touched, nothing maps 0xC0000000 and the image is what it was.  The H743
 * board sets it to 1; the geometry and the two SDRAM partitions (.ark_buf for
 * the compiler arena, .extram for the ramfs file buffers) live in that board's
 * partition header, not here. */
#ifndef ARKRTOS_USE_SDRAM
#define ARKRTOS_USE_SDRAM               0
#endif
#ifndef ARKRTOS_SHELL_BOARD_CMDS
#define ARKRTOS_SHELL_BOARD_CMDS        0
#endif

/* 外置 QSPI NOR（W25Q64，8 MiB）：编入 board/<芯片>/drvqspi.c，注册块设备
 * `nor0`（bring-up 读 JEDEC ID，读到 0xEF4017 才算设备在），并带 `nor` 取证命令。
 * 默认 0：本文件不进编译、不碰 QUADSPI 时钟与引脚、不注册 nor0、命令表逐字节不变。
 * 读/写/擦走位敲（见该文件顶部：QUADSPI 的带地址命令在这块板上会时好时坏，
 * 失败后还会把外设留在 FIFO 有残留的状态）；QUADSPI 只留作取证入口。
 * 文件系统不是它的职责：卷由上层 arkrtos_fs 挂到 nor0 上（分区头 ARKRTOS_FS_*）。 */
#ifndef ARKRTOS_USE_QSPI
#define ARKRTOS_USE_QSPI                0
#endif

/* 开机就把 NOR 卷挂到 /mnt/nor（相当于板上的 fstab）。
 *
 * 为什么要它：工程文件放在 NOR 上，`cd /mnt/nor && make` 才是「Linux 体验」；
 * 每次上电先敲 `fs mount` + `mount /mnt/nor` 两条命令不是。
 *
 * 默认 0：不挂载、不打印、不碰 littlefs 与 VFS 的挂载路径——控制台的第一屏
 * 与命令表逐字节不变。板级构建把它跟 USE_QSPI 绑在一起（有 NOR 才有卷）。
 * 依赖：USE_QSPI=1（块设备 nor0）、USE_FS=1、USE_VFS=1、USE_VFS_LFS=1。 */
#ifndef ARKRTOS_AUTOMOUNT_NOR
#define ARKRTOS_AUTOMOUNT_NOR           0
#endif

/* 挂到哪里。写成一个宏是因为「板上有几个卷、各挂在哪」是布局问题，
 * 不该埋在代码里；改挂载点只改这里。 */
#ifndef ARKRTOS_AUTOMOUNT_NOR_POINT
#define ARKRTOS_AUTOMOUNT_NOR_POINT     "/mnt/nor"
#endif

/* On-board app runner: `app run <path>` takes a bare arkc-produced executable
 * out of the file system, drops it into the fixed window declared by the board
 * partition header (ARKRTOS_APPRUN_*) and starts it as an unprivileged task.
 * No image header, no relocation - the program is linked for that window.
 * Off by default: at 0 the source file compiles to nothing and the command
 * table is byte-for-byte what it was.  Needs the VFS to read the file. */
#ifndef ARKRTOS_USE_APPRUN
#define ARKRTOS_USE_APPRUN              0
#endif

/* 阶段 3c：产物烧回**片内 Flash** 并由内核引导器跳转执行。
 * `app flash write <path>` 把 /tmp 里的裸文件擦进 Flash 应用区
 * （分区头的 ARKRTOS_FLASHAPP_*），`app flash run` 校验向量表后起任务
 * 从 Flash 直接取指，`app flash info / erase` 查与擦。
 * 与 app run 的差别只有「代码从哪来」：那边从文件读进 RAM 窗口，这边代码
 * 本来就在 Flash 里；两块路径共用同一段数据窗口，所以同一个时刻只跑一个。
 * 默认 0：关掉时源文件编成空、命令表与镜像逐字节不变。需要 VFS 读文件
 * （写入那一步）与 APPRUN 的数据窗口（执行那一步）。 */
#ifndef ARKRTOS_USE_FLASHAPP
#define ARKRTOS_USE_FLASHAPP            0
#endif


/* ============================================================
 * 五、依赖校验
 * @brief 关错了开关要在这里报出来，而不是等到链接期一个 undefined symbol
 * ============================================================ */

#if ARKRTOS_USE_FS && !ARKRTOS_USE_BLK
#error "ARKRTOS_USE_FS=1 requires ARKRTOS_USE_BLK=1 (file system sits on the block layer)"
#endif

#if ARKRTOS_USE_MINIAPP && !ARKRTOS_USE_FS
#error "ARKRTOS_USE_MINIAPP=1 requires ARKRTOS_USE_FS=1 (a MiniApp is loaded from the file system)"
#endif

#if ARKRTOS_AUTOMOUNT_NOR && (!ARKRTOS_USE_QSPI || !ARKRTOS_USE_FS || !ARKRTOS_USE_VFS)
#error "ARKRTOS_AUTOMOUNT_NOR=1 requires ARKRTOS_USE_QSPI=1 (blk dev nor0), ARKRTOS_USE_FS=1 and ARKRTOS_USE_VFS=1"
#endif

#if ARKRTOS_AUTOMOUNT_NOR && !ARKRTOS_USE_VFS_LFS
#error "ARKRTOS_AUTOMOUNT_NOR=1 requires ARKRTOS_USE_VFS_LFS=1 (the littlefs -> ark_vfs bridge is what mounts a volume)"
#endif

#if ARKRTOS_MINI_READ_VFS && !ARKRTOS_USE_VFS
#error "ARKRTOS_MINI_READ_VFS=1 requires ARKRTOS_USE_VFS=1 (the image is read through the VFS namespace)"
#endif

#if ARKRTOS_USE_APPRUN && !ARKRTOS_USE_VFS
#error "ARKRTOS_USE_APPRUN=1 requires ARKRTOS_USE_VFS=1 (the program is read from the file system)"
#endif

#if ARKRTOS_USE_FLASHAPP && !ARKRTOS_USE_APPRUN
#error "ARKRTOS_USE_FLASHAPP=1 requires ARKRTOS_USE_APPRUN=1 (the data/stack window is the app-run window)"
#endif

#if ARKRTOS_USE_FLASHAPP && !ARKRTOS_USE_VFS
#error "ARKRTOS_USE_FLASHAPP=1 requires ARKRTOS_USE_VFS=1 (the image is read from the file system)"
#endif

/* 并发数要么是 1（退化成原来的单实例），要么必须落在共享表数组的容量内：
 * 开成 0 会让「一个小程序都跑不了」看起来像装载失败，开超了则会让记账
 * 写到数组外。两个都在编译期挡住。 */
#if ARKRTOS_USE_MINIAPP && ((ARKRTOS_MINI_MAX < 1u) || (ARKRTOS_MINI_MAX > 4u))
#error "ARKRTOS_MINI_MAX must be 1..4 (= ARKRTOS_MINI_ARRAY_MAX, the shared table array length)"
#endif

#if ARKRTOS_USE_VFS && !ARKRTOS_USE_BLK
#error "ARKRTOS_USE_VFS=1 requires ARKRTOS_USE_BLK=1 (the port wraps an arkrtos_blk window as the ark_vfs backend)"
#endif

#if ARKRTOS_USE_VFS_LFS && !ARKRTOS_USE_VFS
#error "ARKRTOS_USE_VFS_LFS=1 requires ARKRTOS_USE_VFS=1 (the bridge plugs into the VFS namespace)"
#endif

#if ARKRTOS_USE_VFS_LFS && !ARKRTOS_USE_FS
#error "ARKRTOS_USE_VFS_LFS=1 requires ARKRTOS_USE_FS=1 (the bridge drives the arkrtos_fs volume)"
#endif

#if ARKRTOS_USE_PROCFS && !ARKRTOS_USE_VFS
#error "ARKRTOS_USE_PROCFS=1 requires ARKRTOS_USE_VFS=1 (/proc is a mount inside the VFS namespace)"
#endif

#if ARKRTOS_USE_ARKVIM && !ARKRTOS_USE_VFS
#error "ARKRTOS_USE_ARKVIM=1 requires ARKRTOS_USE_VFS=1 (the editor opens files through the VFS namespace)"
#endif

#if ARKRTOS_USE_ARKC && !ARKRTOS_USE_VFS
#error "ARKRTOS_USE_ARKC=1 requires ARKRTOS_USE_VFS=1 (the compiler reads sources and writes output through the VFS namespace)"
#endif

/* ark make 没有自己的打包器：它调的是 arkc 的 cc_pack。两条分开的打包路
 * 迟早在板上打出不一样的镜像，而且只有一条被跑过，所以这里把依赖写成
 * 编译期错误，而不是运行期的一句"失败了"。 */
#if ARKRTOS_USE_ARKMAKE && !ARKRTOS_USE_APPPACK
#error "ARKRTOS_USE_ARKMAKE=1 requires ARKRTOS_USE_APPPACK=1 (make builds through the same four-pass packer as cc --pack)"
#endif

#if ARKRTOS_USE_ARKMAKE && !ARKRTOS_USE_ARKC
#error "ARKRTOS_USE_ARKMAKE=1 requires ARKRTOS_USE_ARKC=1 (the pack entry point lives in the compiler component)"
#endif

/* lwIP 端口层建在消息队列上（sys_arch.c 的邮箱就是 arkrtos_mq），
 * 关掉 ARKRTOS_USE_MQ 会让它到链接期才炸；这里先挡住。 */
#if ARKRTOS_USE_LWIP && !ARKRTOS_USE_MQ
#error "ARKRTOS_USE_LWIP=1 requires ARKRTOS_USE_MQ=1 (sys_arch.c maps lwIP mailboxes onto arkrtos_mq)"
#endif

#endif /* __ARKRTOS_FEATURES_H__ */
