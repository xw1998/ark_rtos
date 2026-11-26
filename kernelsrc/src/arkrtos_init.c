/**
* @brief Ark RTOS 内核启动与初始化（单一权威实现）
* @details 本文件只提供 arkrtos_kernel_module_init()：内核各模块的初始化顺序在此唯一确定。
*          内核【不提供 main() 入口】，入口一律由工程侧 main() 提供——
*          同一个映像里存在两个 main 时，AC6 会因辅助符号 __ARM_use_no_argv
*          重复定义而链接失败（L6200E: by arkrtos_init.o and main.o），
*          即“弱 main 可被工程 main 覆盖”在 AC6 下并不成立。
*
*          应用自定义 main() 时建议遵循以下启动顺序：
*          1) HAL 初始化 + 时钟配置 + 外设初始化（CubeMX 生成部分）
*          2) arkrtos_port_board_init()
*          3) arkrtos_cfg_load()
*          4) arkrtos_port_irq_init()
*          5) arkrtos_kernel_module_init()   各内核模块 + 板级设备 + 内置服务任务
*                                          （唯一权威顺序实现，见本文件下方）
*          6) 注册应用/驱动任务（arkrtos_task_register 等，必须在调度启动前完成）
*          7) arkrtos_start_idle()（进入空闲任务，启动调度）
*
*          需要“设备自安装”能力的工程，在第 5 步之后追加：
*          arkrtos_ptable_init() → arkrtos_loader_scan_driver()/arkrtos_loader_start_driver()
*          → arkrtos_loader_scan()/arkrtos_loader_start() → arkrtos_installer_init()
*          （参考实现：example/stm32f427/kernel/ARKRTOS_TEST/Core/Src/main.c 的 arkrtos_kernel_init）
*
* @author xw
* @date 2026.05.03
*/

#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_event.h"
#include "arkrtos_hal.h"
#include "arkrtos_dev.h"
#include "arkrtos_sync.h"
#include "arkrtos_mq.h"
#include "arkrtos_mempool.h"
#include "arkrtos_timer.h"
#include "arkrtos_fault.h"
#include "arkrtos_mpu.h"
#include "arkrtos_config.h"
#include "arkrtos_sbuf.h"
#include "arkrtos_rt.h"
#include "arkrtos_eset.h"
#include "arkrtos_smp.h"
#include "arkrtos_init.h"
#include "arkrtos_fi.h"
#if (ARKRTOS_USE_SELFTEST == 1)
#include "arkrtos_selftest.h"
#endif
#if (ARKRTOS_USE_TRUST_ROOT == 1)
#include "arkrtos_trust.h"
#endif
#if (ARKRTOS_USE_DEVICE_ID == 1)
#include "arkrtos_devid.h"
#endif

/* 内核模块初始化：唯一权威顺序实现（任何工程的 main 都只调用它） */
void arkrtos_kernel_module_init(void)
{
    arkrtos_event_module_init();
    arkrtos_sync_module_init();
    arkrtos_mq_module_init();
#if (ARKRTOS_USE_SBUF == 1)
    arkrtos_sbuf_module_init();
#endif
#if (ARKRTOS_USE_ESET == 1)
    arkrtos_eset_module_init();
#endif
#if (ARKRTOS_USE_SMP == 1)
    /* SMP scaffolding: CPU0 marks itself online and hands cores 1..N-1 to
     * the port layer. It must run before the scheduler starts, because a
     * secondary core that is started later would find a running scheduler
     * it does not own. */
    arkrtos_smp_init();
#endif
    arkrtos_mempool_module_init();
    arkrtos_timer_module_init();
    arkrtos_fault_module_init();
    arkrtos_dev_module_init();
    arkrtos_dev_board_init();

    /* 内置定时器服务任务：须在 cfg_load 之后、调度启动之前注册 */
    arkrtos_timer_task_install();

    #if (ARKRTOS_USE_MPU == 1)
    arkrtos_mpu_module_init();
    #endif

    #if (ARKRTOS_USE_RTSTAT == 1)
    arkrtos_rt_init();
    #endif

    /* 元件自检（SEC-G14）：放在内核模块初始化的末尾——内存、节拍与板级钩子
     * 都已就绪，而调度还没起来，诊断读到的是一台静止的机器。失败不阻止启动：
     * 留日志与审计记录，要不要停机是产品策略。 */
#if (ARKRTOS_USE_SELFTEST == 1)
    (void)arkrtos_selftest_boot();
#endif

    /* 故障注入与篡改监视（SEC-G16）：开机清账，保证"开机时没有武装的
     * 注入点、也没有登记过的监视段"是静态事实而不是约定。
     * 关档时 arkrtos_fi_module_init 不存在（整文件是空 TU），这一步编掉。 */
#if (ARKRTOS_USE_FAULT_INJECT == 1)
    arkrtos_fi_module_init();
#endif

    /* 信任根（SEC-G12）：必须在任何镜像被认领之前跑一次——loader 的
     * accept 路径要读它的锁存结果，而锁存只能在这里建立。
     * 放在自检之后是因为自检不做任何安全决策，而这一步会决定本机能不能
     * 认领镜像；顺序反过来的话，自检失败也会连带挡住信任根判定。
     * 失败不在这里停机：锁存已经置上，认领路径自然会拒绝，而"要不要停机"
     * 是产品策略（见 arkrtos_trust.h 文件头第③条）。 */
#if (ARKRTOS_USE_TRUST_ROOT == 1)
    (void)arkrtos_trust_boot();
#endif
#if (ARKRTOS_USE_DEVICE_ID == 1)
    /* 设备身份（SEC-G13）：取 uid、载入密钥、派生公开 ID。失败不停机——
     * 失败关闭体现在 prove() 返回 ERR_NOKEY 上，而不是把整机拉死。 */
    (void)arkrtos_devid_boot();
#endif
}
