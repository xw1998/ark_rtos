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

/* 任务栈用量统计：填充图案 + 最低栈指针双手段，算每任务峰值 */
#ifndef ARKRTOS_USE_STACK_USAGE
#define ARKRTOS_USE_STACK_USAGE         1
#endif

/* 栈溢出检测：靠栈尾标志被踩判定 */
#ifndef ARKRTOS_USE_STACK_CHECK
#define ARKRTOS_USE_STACK_CHECK         1
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

/* MPU 上下文差异应用缓存：同一任务重复进入时不重写 MPU 寄存器 */
#ifndef ARKRTOS_USE_MPU_APPLY_CACHE
#define ARKRTOS_USE_MPU_APPLY_CACHE     1
#endif

/* ============================================================
 * 二、同步原语与 IPC
 * @note 信号量 / 互斥锁 / 条件变量 / 事件组属于内核核心，不提供关断：
 *       设备框架、POSIX 层、加载器都建立在它们之上，关掉会连锁。
 * ============================================================ */

/* 消息队列（SVC 0x16） */
#ifndef ARKRTOS_USE_MQ
#define ARKRTOS_USE_MQ                  1
#endif

/* 软件定时器（SVC 0x17） */
#ifndef ARKRTOS_USE_TIMER
#define ARKRTOS_USE_TIMER               1
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

/* 内核 Shell 控制台（SVC 0x1A）的开关不在这里，而是沿用既有的
 * SHELL_ENABLE（config/arkrtos_partition.h）；arkrtos_shell.c 的整文件门控
 * 与关闭时的空实现早已就位。不要在此再定义第二个 shell 开关：两个名字
 * 管一件事，迟早会出现「看着关了其实还开着」。 */

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

/* 小程序装载时，每读 ARKRTOS_MINI_YIELD_CHUNKS 个分块就放开一次调度器锁、
 * 再重新拿回，让更高优先级的任务（例如正在跑的控制回路）在两次分块读之间
 * 拿到 CPU。单次分块读（open/read/close 一整段）本身在锁内是原子的，所以
 * 放开锁不会让两个装载者交错读镜像；两次读之间也没有需要保护的中间状态。
 * 0 = 关闭：整段装载期间一直持锁，体积与行为与本开关引入之前完全一致。
 * 默认 1：每个分块让出一次——这是 mini run 期间控制回路不再被整段冻结的原因。*/
#ifndef ARKRTOS_MINI_YIELD_CHUNKS
#define ARKRTOS_MINI_YIELD_CHUNKS       1u
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
#if (ARKRTOS_USE_IMAGE_SIGN == 1) && !defined(ARKRTOS_IMAGE_SIGN_KEY)
#error "ARKRTOS_USE_IMAGE_SIGN=1 requires ARKRTOS_IMAGE_SIGN_KEY (32 byte initialiser list)"
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

/* 心跳合同与看门狗监督（arkrtos_guard.c）。需要板级 ARKRTOS_WDG_ENABLE 才真咬人 */
#ifndef ARKRTOS_USE_KERNEL_GUARD
#define ARKRTOS_USE_KERNEL_GUARD        1
#endif

/* 结构自检（arkrtos_audit.c）：分区表 / 槽位 / 镜像头的一致性检查与审计记录。
 * 关掉后加载器仍会做最低限度的合法性判断，只是不再逐项记账 */
#ifndef ARKRTOS_USE_AUDIT
#define ARKRTOS_USE_AUDIT               1
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

/* lwIP 端口层建在消息队列上（sys_arch.c 的邮箱就是 arkrtos_mq），
 * 关掉 ARKRTOS_USE_MQ 会让它到链接期才炸；这里先挡住。 */
#if ARKRTOS_USE_LWIP && !ARKRTOS_USE_MQ
#error "ARKRTOS_USE_LWIP=1 requires ARKRTOS_USE_MQ=1 (sys_arch.c maps lwIP mailboxes onto arkrtos_mq)"
#endif

#endif /* __ARKRTOS_FEATURES_H__ */
