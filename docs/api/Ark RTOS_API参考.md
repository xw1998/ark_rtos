# Ark RTOS API 参考

> 由 `tools/gen_api_doc.py` 自动生成，请勿手工修改。
> 安装 [Doxygen](https://www.doxygen.nl/) 后重新运行脚本，可得到带交叉引用与调用关系的 HTML 版本。

## 目录

- [kernelsrc/include/arkrtos.h](#kernelsrcincludearkrtosh)（110 项）
- [kernelsrc/include/arkrtos_app_image.h](#kernelsrcincludearkrtos_app_imageh)（38 项）
- [kernelsrc/include/arkrtos_apppack.h](#kernelsrcincludearkrtos_apppackh)（9 项）
- [kernelsrc/include/arkrtos_apprun.h](#kernelsrcincludearkrtos_apprunh)（6 项）
- [kernelsrc/include/arkrtos_audit.h](#kernelsrcincludearkrtos_audith)（7 项）
- [kernelsrc/include/arkrtos_blk.h](#kernelsrcincludearkrtos_blkh)（13 项）
- [kernelsrc/include/arkrtos_cfg.h](#kernelsrcincludearkrtos_cfgh)（1 项）
- [kernelsrc/include/arkrtos_crash.h](#kernelsrcincludearkrtos_crashh)（22 项）
- [kernelsrc/include/arkrtos_dt.h](#kernelsrcincludearkrtos_dth)（11 项）
- [kernelsrc/include/arkrtos_elf.h](#kernelsrcincludearkrtos_elfh)（12 项）
- [kernelsrc/include/arkrtos_export.h](#kernelsrcincludearkrtos_exporth)（3 项）
- [kernelsrc/include/arkrtos_fdt.h](#kernelsrcincludearkrtos_fdth)（22 项）
- [kernelsrc/include/arkrtos_fdt_console.h](#kernelsrcincludearkrtos_fdt_consoleh)（4 项）
- [kernelsrc/include/arkrtos_fdt_overlay.h](#kernelsrcincludearkrtos_fdt_overlayh)（2 项）
- [kernelsrc/include/arkrtos_fdt_res.h](#kernelsrcincludearkrtos_fdt_resh)（4 项）
- [kernelsrc/include/arkrtos_flashapp.h](#kernelsrcincludearkrtos_flashapph)（7 项）
- [kernelsrc/include/arkrtos_fs.h](#kernelsrcincludearkrtos_fsh)（29 项）
- [kernelsrc/include/arkrtos_guard.h](#kernelsrcincludearkrtos_guardh)（11 项）
- [kernelsrc/include/arkrtos_hal.h](#kernelsrcincludearkrtos_halh)（80 项）
- [kernelsrc/include/arkrtos_hwsafe.h](#kernelsrcincludearkrtos_hwsafeh)（4 项）
- [kernelsrc/include/arkrtos_init.h](#kernelsrcincludearkrtos_inith)（1 项）
- [kernelsrc/include/arkrtos_installer.h](#kernelsrcincludearkrtos_installerh)（3 项）
- [kernelsrc/include/arkrtos_layout.h](#kernelsrcincludearkrtos_layouth)（11 项）
- [kernelsrc/include/arkrtos_layout_def.h](#kernelsrcincludearkrtos_layout_defh)（2 项）
- [kernelsrc/include/arkrtos_linux_abi.h](#kernelsrcincludearkrtos_linux_abih)（6 项）
- [kernelsrc/include/arkrtos_loader.h](#kernelsrcincludearkrtos_loaderh)（32 项）
- [kernelsrc/include/arkrtos_log.h](#kernelsrcincludearkrtos_logh)（16 项）
- [kernelsrc/include/arkrtos_mempool.h](#kernelsrcincludearkrtos_mempoolh)（9 项）
- [kernelsrc/include/arkrtos_mini.h](#kernelsrcincludearkrtos_minih)（20 项）
- [kernelsrc/include/arkrtos_modld.h](#kernelsrcincludearkrtos_modldh)（8 项）
- [kernelsrc/include/arkrtos_mpu.h](#kernelsrcincludearkrtos_mpuh)（23 项）
- [kernelsrc/include/arkrtos_net.h](#kernelsrcincludearkrtos_neth)（12 项）
- [kernelsrc/include/arkrtos_ptable.h](#kernelsrcincludearkrtos_ptableh)（23 项）
- [kernelsrc/include/arkrtos_rt.h](#kernelsrcincludearkrtos_rth)（14 项）
- [kernelsrc/include/arkrtos_sec.h](#kernelsrcincludearkrtos_sech)（8 项）
- [kernelsrc/include/arkrtos_sha256.h](#kernelsrcincludearkrtos_sha256h)（3 项）
- [kernelsrc/include/arkrtos_share.h](#kernelsrcincludearkrtos_shareh)（7 项）
- [kernelsrc/include/arkrtos_shell.h](#kernelsrcincludearkrtos_shellh)（9 项）
- [kernelsrc/include/arkrtos_shell_auth.h](#kernelsrcincludearkrtos_shell_authh)（5 项）
- [kernelsrc/include/arkrtos_sign.h](#kernelsrcincludearkrtos_signh)（10 项）
- [kernelsrc/include/arkrtos_smp.h](#kernelsrcincludearkrtos_smph)（11 项）
- [kernelsrc/include/arkrtos_smp_sched.h](#kernelsrcincludearkrtos_smp_schedh)（10 项）
- [kernelsrc/include/arkrtos_softbus.h](#kernelsrcincludearkrtos_softbush)（5 项）
- [kernelsrc/include/arkrtos_spin.h](#kernelsrcincludearkrtos_spinh)（10 项）
- [kernelsrc/include/arkrtos_svc_abi.h](#kernelsrcincludearkrtos_svc_abih)（4 项）
- [kernelsrc/include/arkrtos_svc_call.h](#kernelsrcincludearkrtos_svc_callh)（20 项）
- [kernelsrc/include/arkrtos_task.h](#kernelsrcincludearkrtos_taskh)（5 项）
- [kernelsrc/include/arkrtos_tickless.h](#kernelsrcincludearkrtos_ticklessh)（6 项）
- [kernelsrc/include/arkrtos_ulog.h](#kernelsrcincludearkrtos_ulogh)（5 项）
- [kernelsrc/include/arkrtos_ushell.h](#kernelsrcincludearkrtos_ushellh)（4 项）
- [kernelsrc/include/arkrtos_utest.h](#kernelsrcincludearkrtos_utesth)（22 项）
- [kernelsrc/include/arkrtos_version.h](#kernelsrcincludearkrtos_versionh)（2 项）
- [kernelsrc/include/arkrtos_vfs.h](#kernelsrcincludearkrtos_vfsh)（44 项）
- [kernelsrc/port/arm/cortex-m3/arkrtos_port.c](#kernelsrcportarmcortex-m3arkrtos_portc)（9 项）
- [kernelsrc/port/arm/cortex-m4/arkrtos_port.c](#kernelsrcportarmcortex-m4arkrtos_portc)（9 项）
- [kernelsrc/port/arm/cortex-m7/arkrtos_port.c](#kernelsrcportarmcortex-m7arkrtos_portc)（9 项）
- [kernelsrc/port/loongarch/la32/arkrtos_port.c](#kernelsrcportloongarchla32arkrtos_portc)（12 项）
- [kernelsrc/port/loongarch/la64/arkrtos_port.c](#kernelsrcportloongarchla64arkrtos_portc)（87 项）
- [kernelsrc/port/riscv/rv32imac/arkrtos_port.c](#kernelsrcportriscvrv32imacarkrtos_portc)（18 项）
- [kernelsrc/sdk/app_sdk/arkrtos_app_main.c](#kernelsrcsdkapp_sdkarkrtos_app_mainc)（1 项）
- [kernelsrc/sdk/app_sdk/arkrtos_stdio.c](#kernelsrcsdkapp_sdkarkrtos_stdioc)（4 项）
- [kernelsrc/sdk/app_sdk/tests/test_stdio_host.c](#kernelsrcsdkapp_sdkteststest_stdio_hostc)（2 项）
- [kernelsrc/sdk/driver_sdk/arkrtos_driver_sdk.h](#kernelsrcsdkdriver_sdkarkrtos_driver_sdkh)（1 项）
- [kernelsrc/sdk/mini_sdk/arkc/arkrtos.h](#kernelsrcsdkmini_sdkarkcarkrtosh)（1 项）
- [kernelsrc/sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c](#kernelsrcsdkmini_sdkarkcarkrtos_mini_rt_arkcc)（4 项）
- [kernelsrc/sdk/mini_sdk/arkc/stdio.h](#kernelsrcsdkmini_sdkarkcstdioh)（3 项）
- [kernelsrc/sdk/mini_sdk/arkc/unistd.h](#kernelsrcsdkmini_sdkarkcunistdh)（2 项）
- [kernelsrc/sdk/mini_sdk/arkrtos_mini_main.c](#kernelsrcsdkmini_sdkarkrtos_mini_mainc)（2 项）
- [kernelsrc/sdk/posix/arkrtos_libc_glue.c](#kernelsrcsdkposixarkrtos_libc_gluec)（13 项）
- [kernelsrc/sdk/posix/arkrtos_posix.c](#kernelsrcsdkposixarkrtos_posixc)（1 项）
- [kernelsrc/sdk/posix/arkrtos_posix_types.h](#kernelsrcsdkposixarkrtos_posix_typesh)（1 项）
- [kernelsrc/sdk/posix/arpa/inet.h](#kernelsrcsdkposixarpaineth)（4 项）
- [kernelsrc/sdk/posix/dirent.h](#kernelsrcsdkposixdirenth)（2 项）
- [kernelsrc/sdk/posix/errno.h](#kernelsrcsdkposixerrnoh)（1 项）
- [kernelsrc/sdk/posix/pthread.h](#kernelsrcsdkposixpthreadh)（1 项）
- [kernelsrc/sdk/posix/semaphore.h](#kernelsrcsdkposixsemaphoreh)（1 项）
- [kernelsrc/sdk/posix/sys/stat.h](#kernelsrcsdkposixsysstath)（3 项）
- [kernelsrc/sdk/posix/unistd.h](#kernelsrcsdkposixunistdh)（13 项）
- [kernelsrc/src/arkrtos_apppack.c](#kernelsrcsrcarkrtos_apppackc)（1 项）
- [kernelsrc/src/arkrtos_cfg.c](#kernelsrcsrcarkrtos_cfgc)（1 项）
- [kernelsrc/src/arkrtos_dev.c](#kernelsrcsrcarkrtos_devc)（1 项）
- [kernelsrc/src/arkrtos_elf.c](#kernelsrcsrcarkrtos_elfc)（18 项）
- [kernelsrc/src/arkrtos_eset.c](#kernelsrcsrcarkrtos_esetc)（1 项）
- [kernelsrc/src/arkrtos_exports.c](#kernelsrcsrcarkrtos_exportsc)（1 项）
- [kernelsrc/src/arkrtos_fs.c](#kernelsrcsrcarkrtos_fsc)（1 项）
- [kernelsrc/src/arkrtos_guard.c](#kernelsrcsrcarkrtos_guardc)（1 项）
- [kernelsrc/src/arkrtos_installer.c](#kernelsrcsrcarkrtos_installerc)（1 项）
- [kernelsrc/src/arkrtos_linux_abi.c](#kernelsrcsrcarkrtos_linux_abic)（4 项）
- [kernelsrc/src/arkrtos_loader.c](#kernelsrcsrcarkrtos_loaderc)（6 项）
- [kernelsrc/src/arkrtos_log.c](#kernelsrcsrcarkrtos_logc)（1 项）
- [kernelsrc/src/arkrtos_mini.c](#kernelsrcsrcarkrtos_minic)（1 项）
- [kernelsrc/src/arkrtos_modld.c](#kernelsrcsrcarkrtos_modldc)（5 项）
- [kernelsrc/src/arkrtos_mpu.c](#kernelsrcsrcarkrtos_mpuc)（1 项）
- [kernelsrc/src/arkrtos_paging.c](#kernelsrcsrcarkrtos_pagingc)（27 项）
- [kernelsrc/src/arkrtos_smp.c](#kernelsrcsrcarkrtos_smpc)（1 项）
- [kernelsrc/src/arkrtos_task.c](#kernelsrcsrcarkrtos_taskc)（5 项）
- [kernelsrc/src/arkrtos_tickless.c](#kernelsrcsrcarkrtos_ticklessc)（1 项）
- [kernelsrc/src/arkrtos_vfs_lfs.c](#kernelsrcsrcarkrtos_vfs_lfsc)（3 项）

---

## kernelsrc/include/arkrtos.h

**Ark RTOS 应用 API 头文件（用户态唯一需要包含的头文件）**
应用程序（用户态）包含本文件即可使用全部操作系统接口。
所有调用最终通过 SVC 指令陷入内核态执行，对用户透明，
用法与普通函数一致。

### `#ifndef ARKRTOS_SYNC_OK #define ARKRTOS_SYNC_OK             (0) #endif`

```c
#ifndef ARKRTOS_SYNC_OK #define ARKRTOS_SYNC_OK             (0) #endif #ifndef ARKRTOS_SYNC_ERR_PARAM #define ARKRTOS_SYNC_ERR_PARAM      (-1) #endif
```

与内核 arkrtos_def.h 保持同一组数值。内核头文件对应用不可见，
而这些数值是接口契约的一部分（调用方要据此判断“本次没拿到资源”），
所以在这里再给一份，避免文档里提到的名字在应用侧拿不到。
内核编译单元会同时包含两个头文件，#ifndef 保证不会重定义。

### `void   arkrtos_task_wait(uint32 ms);`

```c
void   arkrtos_task_wait(uint32 ms);
```

**当前任务睡眠指定毫秒（阻塞调度）**
- `ms`：睡眠时间，单位毫秒
> 说明：睡眠期间任务状态为等待，CPU 交给其他就绪任务。

### `void   arkrtos_task_wait_period(void);`

```c
void   arkrtos_task_wait_period(void);
```

**等待当前任务的周期结束（周期任务同步用）**
> 说明：任务须在配置表中设置了周期；未到期即阻塞，到期自动唤醒。

### `void   arkrtos_task_delay(uint32 us);`

```c
void   arkrtos_task_delay(uint32 us);
```

**微秒级忙等延时（不引起任务切换）**
- `us`：延时时间，单位微秒
> 说明：占用 CPU，只适合极短延时（如外设时序），长延时应使用 arkrtos_task_wait。

### `void   arkrtos_task_kill(void);`

```c
void   arkrtos_task_kill(void);
```

**终止当前任务**
> 说明：仅能终止自身；任务槽位转为 INVALID，可经 arkrtos_task_recover_req 恢复。

### `uint32 arkrtos_get_time_ms(void);`

```c
uint32 arkrtos_get_time_ms(void);
```

**获取系统运行时间**
**返回**：自内核启动以来的毫秒数

### `uint32 arkrtos_get_cpu_usage(void);`

```c
uint32 arkrtos_get_cpu_usage(void);
```

**获取 CPU 空闲率**
**返回**：空闲率（百分比 0~100）
> 说明：需开启 ARKRTOS_USE_CPU_LOAD 配置。

### `int32  arkrtos_event_create(const char *name);`

```c
int32  arkrtos_event_create(const char *name);
```

**创建命名事件**
- `name`：事件名（字符串）
**返回**：事件句柄，失败返回负值

### `int32  arkrtos_event_wait(int32 handle, int32 timeout);`

```c
int32  arkrtos_event_wait(int32 handle, int32 timeout);
```

**等待事件触发**
- `handle`：事件句柄
- `timeout`：超时时间（ms），0 表示不等待，负值表示永久等待

### `void   arkrtos_event_set(int32 handle);`

```c
void   arkrtos_event_set(int32 handle);
```

**触发事件，唤醒所有等待该事件的任务**
- `handle`：事件句柄

### `int32  arkrtos_eset_create(const char *name);`

```c
int32  arkrtos_eset_create(const char *name);
```

**创建命名事件集（32 位）**
- `name`：事件集名（字符串）
**返回**：事件集句柄，失败返回负值

### `int32  arkrtos_eset_wait(int32 handle, uint32 mask, uint32 opt, uint32`

```c
int32  arkrtos_eset_wait(int32 handle, uint32 mask, uint32 opt, uint32 *p_got, int32 timeout);
```

**等待事件集里的一组位**
- `handle`：事件集句柄
- `mask`：要等的位（非 0）
- `opt`：ARKRTOS_ESET_OPT_OR / _AND 二选一，可 | ARKRTOS_ESET_OPT_CLEAR
- `p_got`：成功时写入本次拿到的位（调用方缓冲）
- `timeout`：超时（ms）：0 = 只试一次，负值 = 永久等待
**返回**：0 = 拿到位（见 *p_got）；ARKRTOS_SYNC_ERR_TIMEOUT = 超时；
ARKRTOS_SYNC_ERR_DELETED = 等待期间对象被删除；负值 = 参数/句柄错误
> 说明：AND 命中时 *p_got 等于 mask；OR 命中时只含当时成立的那些位。
带 CLEAR 时，交给调用方的位同时从对象里清掉，且只交给一个等待者。

### `int32  arkrtos_eset_set(int32 handle, uint32 mask);`

```c
int32  arkrtos_eset_set(int32 handle, uint32 mask);
```

**置位（可累加）：flags |= mask**
- `handle`：事件集句柄
- `mask`：要置的位（非 0）
**返回**：0 成功，负值失败
> 说明：没有等待者时位留在对象里，下一次 wait 直接命中，不会丢事件。

### `int32  arkrtos_eset_set_from_isr(int32 handle, uint32 mask);`

```c
int32  arkrtos_eset_set_from_isr(int32 handle, uint32 mask);
```

**中断中置位（语义与 arkrtos_eset_set 相同）**
- `handle`：事件集句柄
- `mask`：要置的位（非 0）
**返回**：0 成功，负值失败

### `int32  arkrtos_eset_clear(int32 handle, uint32 mask);`

```c
int32  arkrtos_eset_clear(int32 handle, uint32 mask);
```

**清除若干位：flags &= ~mask**
- `handle`：事件集句柄
- `mask`：要清的位
**返回**：0 成功，负值失败

### `int32  arkrtos_eset_get(int32 handle, uint32 *p_flags);`

```c
int32  arkrtos_eset_get(int32 handle, uint32 *p_flags);
```

**查询当前置位情况**
- `handle`：事件集句柄
- `p_flags`：输出：当前 32 位掩码
**返回**：0 成功，负值失败

### `int32  arkrtos_eset_delete(int32 handle);`

```c
int32  arkrtos_eset_delete(int32 handle);
```

**删除事件集（等待者以 ARKRTOS_SYNC_ERR_DELETED 放走）**
- `handle`：事件集句柄
**返回**：0 成功，负值失败

### `int32  arkrtos_sem_create(const char *name, int32 init_count);`

```c
int32  arkrtos_sem_create(const char *name, int32 init_count);
```

**创建计数信号量**
- `name`：信号量名
- `init_count`：初始计数值
**返回**：信号量句柄，失败返回负值

### `int32  arkrtos_sem_wait(int32 handle, int32 timeout);`

```c
int32  arkrtos_sem_wait(int32 handle, int32 timeout);
```

**等待信号量（计数减一，计数为 0 时阻塞）**
- `handle`：信号量句柄
- `timeout`：超时时间（ms），0 表示不等待，负值表示永久等待
**返回**：0=成功；ARKRTOS_SYNC_ERR_TIMEOUT(-2)=等待超时（未获得资源）；其它负值=参数错误

### `int32  arkrtos_sem_post(int32 handle);`

```c
int32  arkrtos_sem_post(int32 handle);
```

**释放信号量（唤醒等待者或计数加一）**
- `handle`：信号量句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_sem_delete(int32 handle);`

```c
int32  arkrtos_sem_delete(int32 handle);
```

**删除信号量**
- `handle`：信号量句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_mutex_create(const char *name);`

```c
int32  arkrtos_mutex_create(const char *name);
```

**创建互斥锁（带优先级继承，防止优先级反转）**
- `name`：互斥锁名
**返回**：互斥锁句柄，失败返回负值

### `int32  arkrtos_mutex_create_recursive(const char *name);`

```c
int32  arkrtos_mutex_create_recursive(const char *name);
```

**创建递归互斥锁（同一任务可重复加锁，解锁需与加锁次数配平）**
- `name`：互斥锁名
**返回**：互斥锁句柄，失败返回负值
> 说明：普通 arkrtos_mutex_create 创建的锁仍不可递归，重复加锁返回 -1。

### `int32  arkrtos_wait_multi(const uint32 *handles, uint32 n, uint32 opt,`

```c
int32  arkrtos_wait_multi(const uint32 *handles, uint32 n, uint32 opt, int32 timeout_ms, uint32 *ready_mask);
```

**探测一组句柄里哪些已就绪（不消费令牌）**
- `handles`：句柄数组（信号量 / 互斥锁 / 消息队列 / 事件混用均可）
- `n`：句柄个数，1..ARKRTOS_WAITMULTI_MAX_OBJS
- `opt`：ARKRTOS_WM_OPT_ANY（任意一个就绪）或 ARKRTOS_WM_OPT_ALL
- `timeout_ms`：超时：0=只探一次；>0=最多等这么多毫秒；<0=一直等
- `ready_mask`：输出：bit i 对应 handles[i]，1=就绪（暂不满足时也可读）
**返回**：0=满足条件；-1=参数/句柄非法；-2=超时仍未满足
> 说明：非破坏性：就绪的对象不会被取走，调用方随后要用各自的 timeout=0
接口去取。带超时的等待是轮询实现，唤醒延迟上限是一个轮询周期；
32 位事件集需要每条目带掩码，本接口不支持。

### `int32  arkrtos_mutex_lock(int32 handle, int32 timeout);`

```c
int32  arkrtos_mutex_lock(int32 handle, int32 timeout);
```

**加锁**
- `handle`：互斥锁句柄
- `timeout`：超时时间（ms），0 表示不等待，负值表示永久等待
**返回**：0=成功；ARKRTOS_SYNC_ERR_TIMEOUT(-2)=等待超时（未获得资源）；其它负值=参数错误

### `int32  arkrtos_mutex_unlock(int32 handle);`

```c
int32  arkrtos_mutex_unlock(int32 handle);
```

**解锁（仅持有者可解锁）**
- `handle`：互斥锁句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_mutex_delete(int32 handle);`

```c
int32  arkrtos_mutex_delete(int32 handle);
```

**删除互斥锁**
- `handle`：互斥锁句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_cond_create(const char *name);`

```c
int32  arkrtos_cond_create(const char *name);
```

**创建条件变量**
- `name`：条件变量名（最长 7 个字符）
**返回**：条件变量句柄，失败返回负值

### `int32  arkrtos_cond_wait(int32 cond_handle, int32 mutex_handle, int32 `

```c
int32  arkrtos_cond_wait(int32 cond_handle, int32 mutex_handle, int32 timeout);
```

**等待条件（原子地释放互斥锁并挂起，醒来后重新持有互斥锁）**
- `cond_handle`：条件变量句柄
- `mutex_handle`：当前任务已持有的互斥锁句柄
- `timeout`：超时时间（ms），负值表示永久等待；0 不支持
（条件变量必须“释放互斥量后等待”，给 0 只会得到一个参数错误）
**返回**：0=被 signal/broadcast 唤醒；ARKRTOS_SYNC_ERR_TIMEOUT(-2)=超时；
ARKRTOS_SYNC_ERR_DELETED(-3)=等待期间对象被删除；其它负值=参数错误
> 说明：无论返回什么，返回时都已重新持有 mutex_handle（POSIX 契约）。
> 说明：调用前必须先持有 mutex_handle，否则返回参数错误；
释放锁与入等待队列在内核内部一次做完，不会丢失 signal。

### `int32  arkrtos_cond_signal(int32 handle);`

```c
int32  arkrtos_cond_signal(int32 handle);
```

**唤醒一个等待者（没有等待者时不算错误）**
- `handle`：条件变量句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_cond_broadcast(int32 handle);`

```c
int32  arkrtos_cond_broadcast(int32 handle);
```

**唤醒全部等待者**
- `handle`：条件变量句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_cond_delete(int32 handle);`

```c
int32  arkrtos_cond_delete(int32 handle);
```

**删除条件变量（唤醒全部等待者并告知对象已删除）**
- `handle`：条件变量句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_dev_open(const char *name, uint32 param);`

```c
int32  arkrtos_dev_open(const char *name, uint32 param);
```

**打开设备**
- `name`：设备名（内置驱动或独立驱动注册的名字）
- `param`：设备相关参数（由驱动解释，无参数填 0）
**返回**：设备句柄，失败返回负值

### `int32  arkrtos_dev_close(int32 handle);`

```c
int32  arkrtos_dev_close(int32 handle);
```

**关闭设备**
- `handle`：设备句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_dev_read(int32 handle, void *pdata, int32 len);`

```c
int32  arkrtos_dev_read(int32 handle, void *pdata, int32 len);
```

**从设备读取数据**
- `handle`：设备句柄
- `pdata`：接收缓冲区
- `len`：期望读取长度（字节）
**返回**：实际读取长度，负值表示错误

### `int32  arkrtos_dev_write(int32 handle, void *pdata, int32 len);`

```c
int32  arkrtos_dev_write(int32 handle, void *pdata, int32 len);
```

**向设备写入数据**
- `handle`：设备句柄
- `pdata`：发送缓冲区
- `len`：写入长度（字节）
**返回**：实际写入长度，负值表示错误

### `int32  arkrtos_dev_ctrl(int32 handle, uint32 code, uint32 value);`

```c
int32  arkrtos_dev_ctrl(int32 handle, uint32 code, uint32 value);
```

**设备控制命令**
- `handle`：设备句柄
- `code`：控制命令码（由驱动定义）
- `value`：命令参数
**返回**：0 或正值=成功，负值=失败

### `int32  arkrtos_dev_set_will(const char *dev_name, int32 cmd, int32 val`

```c
int32  arkrtos_dev_set_will(const char *dev_name, int32 cmd, int32 val);
```

**声明本任务对某设备的崩溃"遗言"（硬件安全态）**
- `dev_name`：设备名（本任务必须先 open 过它，否则内核拒绝）
- `cmd`：传给设备驱动的控制命令码
- `val`：命令参数
**返回**：0=成功；-1=本任务并未持有该设备；-2=参数非法
> 说明：本任务死亡（自退/被杀/故障恢复）时，由内核（不是本 App）对该设备执行
drv_ctrl(cmd,val)，防止 App 崩溃后电机/机床/机械臂等硬件失控。
需内核 ARKRTOS_HW_SAFE=1，否则返回 -1。

### `int32  arkrtos_mq_create(const char *name);`

```c
int32  arkrtos_mq_create(const char *name);
```

**创建消息队列**
- `name`：队列名
**返回**：消息队列句柄，失败返回负值
> 说明：队列深度与单条消息长度由 ARKRTOS_MQ_DEPTH / ARKRTOS_MQ_MSG_WORDS 配置。

### `int32  arkrtos_mq_send(int32 handle, void *buf, int32 len_words, int32`

```c
int32  arkrtos_mq_send(int32 handle, void *buf, int32 len_words, int32 timeout);
```

**发送消息（消息按字拷贝入队）**
- `handle`：消息队列句柄
- `buf`：消息缓冲区
- `len_words`：消息长度（以 32 位字为单位）
- `timeout`：队列满时的等待时间（ms），0 表示不等待，负值表示永久等待
**返回**：0=成功，负值=失败

### `int32  arkrtos_mq_recv(int32 handle, void *buf, int32 len_words, int32`

```c
int32  arkrtos_mq_recv(int32 handle, void *buf, int32 len_words, int32 timeout);
```

**接收消息（阻塞式）**
- `handle`：消息队列句柄
- `buf`：接收缓冲区
- `len_words`：缓冲区可容纳的字数
- `timeout`：超时时间（ms），0 表示不等待，负值表示永久等待
**返回**：实际收到的字数，负值=超时或错误

### `int32  arkrtos_mq_delete(int32 handle);`

```c
int32  arkrtos_mq_delete(int32 handle);
```

**删除消息队列**
- `handle`：消息队列句柄
**返回**：0=成功，负值=失败

### `#if (ARKRTOS_USE_SBUF == 1)`

```c
#if (ARKRTOS_USE_SBUF == 1)
```

### `int32  arkrtos_sbuf_create(const char *name);`

```c
int32  arkrtos_sbuf_create(const char *name);
```

**创建流缓冲**
- `name`：缓冲名（最长 7 字节）
**返回**：句柄，失败返回负值
> 说明：每个对象固定占用 ARKRTOS_SBUF_BUF_SIZE 字节静态缓冲，没有动态内存。
同一块缓冲既可以按字节流（arkrtos_sbuf_*）用，也可以按消息
（arkrtos_mbuf_*）用，但两者不要混用：消息靠长度头定界，
掺进裸字节流后长度头就错位了。

### `int32  arkrtos_sbuf_send(int32 handle, const void *buf, int32 len, int`

```c
int32  arkrtos_sbuf_send(int32 handle, const void *buf, int32 len, int32 timeout_ms);
```

**写入字节流**
- `handle`：流缓冲句柄
- `buf`：数据
- `len`：字节数（不得超过 ARKRTOS_SBUF_BUF_SIZE）
- `timeout_ms`：0=不等待，负值=永久等待
**返回**：实际写入的字节数（>0，可能小于 len）；负值为错误码
> 说明：与其他 RTOS 的流缓冲一致：空间不够时能写多少写多少，
返回实际量，调用方据此推进偏移后再写剩下的部分。

### `int32  arkrtos_sbuf_recv(int32 handle, void *buf, int32 len, int32 tim`

```c
int32  arkrtos_sbuf_recv(int32 handle, void *buf, int32 len, int32 timeout_ms);
```

**读取字节流**
**返回**：实际读出的字节数（>0）；负值为超时/参数错误

### `int32  arkrtos_sbuf_send_from_isr(int32 handle, const void *buf, int32`

```c
int32  arkrtos_sbuf_send_from_isr(int32 handle, const void *buf, int32 len);
```

**中断上下文写入（不阻塞：满了返回负值）**
**返回**：实际写入的字节数

### `int32  arkrtos_sbuf_used(int32 handle);`

```c
int32  arkrtos_sbuf_used(int32 handle);
```

**当前已存字节数，负值=句柄非法 */**

### `int32  arkrtos_sbuf_free(int32 handle);`

```c
int32  arkrtos_sbuf_free(int32 handle);
```

**当前剩余空间字节数，负值=句柄非法 */**

### `int32  arkrtos_sbuf_reset(int32 handle);`

```c
int32  arkrtos_sbuf_reset(int32 handle);
```

**清空缓冲（唤醒等待空间的写者） */**

### `int32  arkrtos_sbuf_delete(int32 handle);`

```c
int32  arkrtos_sbuf_delete(int32 handle);
```

**删除缓冲（等待者收到"对象已删除"，不会睡到超时） */**

### `int32  arkrtos_mbuf_send(int32 handle, const void *buf, int32 len, int`

```c
int32  arkrtos_mbuf_send(int32 handle, const void *buf, int32 len, int32 timeout_ms);
```

**发送一条带长度头的消息（整条原子）**
**返回**：0=整条写入；负值=超时/NOSPC(比整块缓冲还大)/参数错误

### `int32  arkrtos_mbuf_recv(int32 handle, void *buf, int32 len, int32 tim`

```c
int32  arkrtos_mbuf_recv(int32 handle, void *buf, int32 len, int32 timeout_ms);
```

**接收一条消息**
- `buf`：接收缓冲；比消息小时整条丢弃并返回 ARKRTOS_SBUF_ERR_TOO_SMALL
**返回**：消息长度（>0）；负值为错误码

### `int32  arkrtos_mbuf_send_from_isr(int32 handle, const void *buf, int32`

```c
int32  arkrtos_mbuf_send_from_isr(int32 handle, const void *buf, int32 len);
```

**中断上下文发送消息（整条写入，放不下返回负值） */**

### `int32  arkrtos_mbuf_peek_len(int32 handle);`

```c
int32  arkrtos_mbuf_peek_len(int32 handle);
```

**下一条完整消息的长度；没有完整消息返回负值（可先问长度再准备缓冲） */**

### `typedef struct {`

```c
typedef struct {
```

**池的只读快照 */**

### `int32  arkrtos_mpool_create(const char *name, void *pool, uint32 block`

```c
int32  arkrtos_mpool_create(const char *name, void *pool, uint32 block_size, uint32 block_count);
```

**在调用者提供的内存区上创建固定块池**
- `name`：池名（非空，长度 < ARKRTOS_MPOOL_NAME_MAX）
- `pool`：内存区首地址，必须整段落在本任务的可见 RAM 内
- `block_size`：每块字节数，必须 >= 指针宽度且按指针宽度对齐
- `block_count`：块数，必须 > 0
**返回**：>=0 句柄；负值=参数非法/名字重复/对象表已满/内存区不在可见 RAM
> 说明：池不加锁也不阻塞：分配与释放都在极短临界区内完成，可以被中断调用
的前提是同一池不在中断里与任务里同时释放同一个块（那本来就是错用）。
任务下线时，该任务创建的池会被内核自动回收（内存区归调用者，不会释放）。

### `void  *arkrtos_mpool_alloc(int32 handle);`

```c
void  *arkrtos_mpool_alloc(int32 handle);
```

**取一块（不阻塞）**
**返回**：块地址；0 表示当前没有空闲块或句柄非法

### `int32  arkrtos_mpool_free(int32 handle, void *p);`

```c
int32  arkrtos_mpool_free(int32 handle, void *p);
```

**归还一块**
**返回**：0=成功；负值=句柄非法或指针不属于本池（池状态保持不变）

### `int32  arkrtos_mpool_query(int32 handle, arkrtos_mpool_stat_t *out);`

```c
int32  arkrtos_mpool_query(int32 handle, arkrtos_mpool_stat_t *out);
```

**取池快照 @return 0=成功；负值=句柄非法或输出区不在可见 RAM */**

### `int32  arkrtos_mpool_delete(int32 handle);`

```c
int32  arkrtos_mpool_delete(int32 handle);
```

**销毁池（只释放对象表项，调用者提供的内存区不受影响） */**

### `int32  arkrtos_timer_create(const char *name);`

```c
int32  arkrtos_timer_create(const char *name);
```

**创建软定时器**
- `name`：定时器名
**返回**：定时器句柄，失败返回负值

### `int32  arkrtos_timer_start(int32 handle, uint32 period_ms, uint32 mode`

```c
int32  arkrtos_timer_start(int32 handle, uint32 period_ms, uint32 mode, void (*cb)(void *), void *arg);
```

**启动定时器**
- `handle`：定时器句柄
- `period_ms`：定时周期（ms）
- `mode`：0=单次触发，1=周期触发（ARKRTOS_TIMER_MODE_x）
- `cb`：到期回调函数
- `arg`：回调参数
**返回**：0=成功，负值=失败
> 说明：回调在专用定时器任务上下文中执行，不在中断内运行，
因此回调中可以安全调用其它系统调用。

### `int32  arkrtos_timer_stop(int32 handle);`

```c
int32  arkrtos_timer_stop(int32 handle);
```

**停止定时器**
- `handle`：定时器句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_timer_delete(int32 handle);`

```c
int32  arkrtos_timer_delete(int32 handle);
```

**删除定时器**
- `handle`：定时器句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_task_status_get(int32 task_id);`

```c
int32  arkrtos_task_status_get(int32 task_id);
```

**查询任务状态**
- `task_id`：任务号（从 1 开始）
**返回**：任务状态枚举值（arkrtos_task_status_t），-1 表示任务号非法

### `int32  arkrtos_task_recover_req(int32 task_id);`

```c
int32  arkrtos_task_recover_req(int32 task_id);
```

**请求恢复（重启）指定任务**
- `task_id`：任务号（从 1 开始）
**返回**：0=成功，-1=参数非法
> 说明：重建任务栈帧并重新调度，用于任务级故障恢复。

### `int32  arkrtos_fault_record_count(void);`

```c
int32  arkrtos_fault_record_count(void);
```

**获取故障记录条数**
**返回**：当前环形缓冲区中已记录的故障条数

### `int32  arkrtos_fault_record_read(int32 index, uint32 *out3);`

```c
int32  arkrtos_fault_record_read(int32 index, uint32 *out3);
```

**读取一条故障记录**
- `index`：记录序号（从 0 开始）
- `out3`：用户缓冲区（3 个字）：[0]=故障类型，[1]=任务号，[2]=发生时刻 tick
**返回**：0=成功，负值=失败

### `int32  arkrtos_sem_post_from_isr(int32 handle);`

```c
int32  arkrtos_sem_post_from_isr(int32 handle);
```

**中断中释放信号量（不阻塞、不引起切换）**
- `handle`：信号量句柄
**返回**：0=成功，负值=失败
> 说明：该接口直接在内核态执行，不经 SVC，只允许中断服务程序调用。

### `int32  arkrtos_event_set_from_isr(int32 handle);`

```c
int32  arkrtos_event_set_from_isr(int32 handle);
```

**中断中触发事件**
- `handle`：事件句柄
**返回**：0=成功，负值=失败

### `int32  arkrtos_mq_send_from_isr(int32 handle, void *buf, int32 len_wor`

```c
int32  arkrtos_mq_send_from_isr(int32 handle, void *buf, int32 len_words);
```

**中断中发送消息**
- `handle`：消息队列句柄
- `buf`：消息缓冲区
- `len_words`：消息长度（字）
**返回**：0=成功，负值=失败

### `int32  arkrtos_mq_recv_from_isr(int32 handle, void *buf, int32 len_wor`

```c
int32  arkrtos_mq_recv_from_isr(int32 handle, void *buf, int32 len_words);
```

**中断中直接从消息队列取一条消息**
- `handle`：消息队列句柄
- `buf`：接收缓冲区
- `len_words`：缓冲区容量（字）
**返回**：>0=实际取到的字数；负值=队列空/参数错（空队列不登记、不等待）
> 说明：与 send 版对称：唤醒的发送者只置 READY，切换交给下一次调度。
有了它，ISR 不必再「置标志 -> 唤醒线程 -> 线程 recv」多绕一跳。

### `void   arkrtos_sched_lock(void);`

```c
void   arkrtos_sched_lock(void);
```

**进入临界区（禁止任务切换，不关闭中断）**
相当于 RT-Thread 的 rt_enter_critical，支持嵌套调用。
适用于保护较长的共享数据访问，开销小于关中断。
> 说明：临界区内禁止调用阻塞接口（如 wait/sem_wait），
否则阻塞请求会被忽略并记录一条 ARKRTOS_FAULT_SCHEDLOCK 故障记录。

### `void   arkrtos_sched_unlock(void);`

```c
void   arkrtos_sched_unlock(void);
```

**退出临界区**
> 说明：须与 arkrtos_sched_lock 成对调用；计数归零时恢复任务切换。

### `int32  arkrtos_sched_lock_count(void);`

```c
int32  arkrtos_sched_lock_count(void);
```

**查询调度器锁嵌套层数**
**返回**：当前嵌套层数（0 表示未处于临界区）

### `int32  arkrtos_task_stack_info(int32 task_id, uint32 *out3);`

```c
int32  arkrtos_task_stack_info(int32 task_id, uint32 *out3);
```

**查询任务栈使用峰值**
- `task_id`：任务号（从 1 开始）
- `out3`：用户缓冲区（3 个字）：
[0]=栈总字节数，[1]=峰值已用字节数，[2]=剩余字节数
**返回**：0=成功，-1=参数非法
采用两种手段并取较大值：栈填充图案扫描 + 上下文切换最低栈指针记录。
建议按峰值用量保留 30% 以上余量再减小任务栈配置。
> 说明：需开启 ARKRTOS_USE_STACK_USAGE 配置。

### `int32  arkrtos_app_load(int32 dev, uint32 image_len);`

```c
int32  arkrtos_app_load(int32 dev, uint32 image_len);
```

**从设备加载 App 镜像到空闲槽位**
- `dev`：已打开的设备句柄（数据从镜像头开始）
- `image_len`：期望镜像总长（含头），传 0 表示由镜像头决定
**返回**：成功返回槽位号(>=0)，失败返回负错误码（见 arkrtos_loader.h）
> 说明：镜像会先擦后写并做 CRC 回读校验。

### `int32  arkrtos_app_start(uint32 slot);`

```c
int32  arkrtos_app_start(uint32 slot);
```

**启动槽位中的 App**
- `slot`：槽位号
**返回**：成功返回任务号(>0)，失败返回负错误码

### `int32  arkrtos_app_stop(uint32 slot);`

```c
int32  arkrtos_app_stop(uint32 slot);
```

**停止槽位中的 App（镜像保留在 Flash）**
- `slot`：槽位号
**返回**：0=成功，负值为错误码

### `uint32 arkrtos_app_status(uint32 slot);`

```c
uint32 arkrtos_app_status(uint32 slot);
```

**查询槽位状态**
- `slot`：槽位号
**返回**：0=空（EMPTY）,1=已加载（LOADED）,2=运行中（RUNNING）；
槽位非法返回 0xffffffff

### `int32  arkrtos_driver_load(int32 dev, uint32 image_len);`

```c
int32  arkrtos_driver_load(int32 dev, uint32 image_len);
```

**从设备安装驱动镜像到驱动区**
- `dev`：已打开的设备句柄（数据从镜像头开始）
- `image_len`：期望镜像总长（含头），传 0 表示由镜像头决定
**返回**：成功返回驱动槽位号（>=0），负值为错误码（见 arkrtos_loader.h）
> 说明：镜像头的 type 必须为驱动（ARKRTOS_APP_TYPE_DRIVER）；
镜像头的 load_addr 必须等于某个驱动槽位基址，写入前只擦除该槽区间。

### `int32  arkrtos_thread_create(void (*entry)(void), void *stack, uint32 `

```c
int32  arkrtos_thread_create(void (*entry)(void), void *stack, uint32 stack_size, uint32 priority, uint32 period_ms);
```

**在当前 App/驱动的 RAM 里创建一个线程**
- `entry`：线程入口，必须位于调用者自己的固件窗口内
- `stack`：栈底地址，整段栈必须位于调用者自己的 RAM 窗口内
- `stack_size`：栈字节数（最小 128）
- `priority`：内核优先级（1~254，数值越小越高；255 是调度器哨兵，会被拒绝）
- `period_ms`：0 = 事件驱动（默认）；非 0 = 周期任务
**返回**：任务号（>0），失败返回负值
> 说明：内核只接受"调用者自己的"入口与栈：不能借别人的 RAM 建栈，
也不能把入口指到内核或别的 App 里去。

### `int32  arkrtos_thread_self(void);`

```c
int32  arkrtos_thread_self(void);
```

**取当前线程的内核任务号**
**返回**：任务号（>0）；在内核自带任务上返回 0

### `void   arkrtos_thread_exit(void);`

```c
void   arkrtos_thread_exit(void);
```

**结束当前线程，不再返回**

### `void   arkrtos_thread_exit_code(int32 code);`

```c
void   arkrtos_thread_exit_code(int32 code);
```

**带退出码结束当前线程（U-8），不再返回**
- `code`：退出码；shell 的 exec 会把它打出来（exit code N）
> 说明：只有走这条路径才有可读的退出码；arkrtos_thread_exit() 不写码，
接收方据此区分「结束了」与「以 0 结束」

### `int32  arkrtos_file_write(const char *path, const void *data, uint32 l`

```c
int32  arkrtos_file_write(const char *path, const void *data, uint32 len);
```

**Write a whole file (create or truncate).**
- `path`：absolute path inside the mounted volume, e.g. "/cfg/a.txt"
- `data`：bytes to write (may be NULL when len is 0)
- `len`：byte count, 0 creates an empty file
**返回**：0 on success, negative error code on failure

### `int32  arkrtos_file_read(const char *path, void *buf, uint32 max);`

```c
int32  arkrtos_file_read(const char *path, void *buf, uint32 max);
```

**Read a whole file into a buffer of the caller's own RAM.**
- `path`：absolute path inside the mounted volume
- `buf`：destination buffer
- `max`：capacity of that buffer in bytes
**返回**：bytes read (>= 0), or a negative error code

### `int32  arkrtos_file_remove(const char *path);`

```c
int32  arkrtos_file_remove(const char *path);
```

**Remove a file.**
**返回**：0 on success, negative error code on failure

### `int32  arkrtos_file_stat(const char *path, uint32 *size, uint32 *is_di`

```c
int32  arkrtos_file_stat(const char *path, uint32 *size, uint32 *is_dir);
```

**Size and type of one entry.**
- `path`：absolute path
- `size`：receives the size in bytes (may be NULL)
- `is_dir`：receives 1 for a directory, 0 for a file (may be NULL)
**返回**：0 on success, negative error code on failure

### `int32  arkrtos_heartbeat(uint32 period_ms);`

```c
int32  arkrtos_heartbeat(uint32 period_ms);
```

**Report in, and optionally declare how often this image will do so.**
- `period_ms`：0 = withdraw the contract; otherwise the longest gap the
image promises to keep, in milliseconds. Must be >= 100 ms: a shorter
promise cannot be checked by sampling, and the kernel refuses it
rather than watch the image against numbers it cannot verify.
**返回**：0 accepted, -1 refused (bad period, or no slot to attribute it to).
> 说明：The kernel checks the window only while this task is runnable; a task
blocked in a kernel wait is exempt. A periodic task that never calls
this is reported by `guard` as "no contract" - not as healthy.

### `int32  arkrtos_file_mount(void);`

```c
int32  arkrtos_file_mount(void);
```

**Mount the default volume (the same call as `fs mount`).**
**返回**：0 on success, negative error code on failure
> 说明：A mount does not survive a reset, so an App that needs the
volume asks for it instead of assuming it is still there.

### `int32  arkrtos_file_unmount(void);`

```c
int32  arkrtos_file_unmount(void);
```

**Release the mounted volume.**
**返回**：0 on success, negative error code on failure

### `int32  arkrtos_file_read_at(const char *path, void *buf, uint32 len, u`

```c
int32  arkrtos_file_read_at(const char *path, void *buf, uint32 len, uint32 off);
```

**Read len bytes starting at off, past EOF just stops short.**
- `path`：absolute path inside the mounted volume
- `buf`：destination buffer
- `len`：bytes wanted
- `off`：byte offset to start from
**返回**：bytes read (>= 0), or a negative error code
> 说明：Same units as arkrtos_file_read(): the count comes back in the
return value, so a short read is visible without an extra out
parameter an App could leave NULL.

### `int32  arkrtos_file_rename(const char *from, const char *to);`

```c
int32  arkrtos_file_rename(const char *from, const char *to);
```

**Rename or move one entry.**
**返回**：0 on success, negative error code on failure

### `int32  arkrtos_file_list_names(const char *dir, char *out, uint32 out_`

```c
int32  arkrtos_file_list_names(const char *dir, char *out, uint32 out_size, uint32 *count);
```

**List entry names of a directory into a caller buffer.**
- `dir`：absolute path, "/" for the volume root
- `out`：destination buffer
- `out_size`：capacity of that buffer in bytes
- `count`：receives how many names were stored (may be NULL)
**返回**：0 on success, negative error code on failure
> 说明：Names are stored back to back, each NUL terminated, and a
name that does not fit in full is left out rather than cut.
The App gets a flat list of names, no types and no sizes.

### `#define ARKRTOS_PATH_MAX          (64u)`

```c
#define ARKRTOS_PATH_MAX          (64u)
```

**Longest accepted namespace path, NUL included. */**

### `int32  arkrtos_path_open(const char *path, uint32 flags);`

```c
int32  arkrtos_path_open(const char *path, uint32 flags);
```

**Open a file (or a directory) at an absolute namespace path.**
- `path`：absolute path, e.g. "/mnt/nor/a.txt" or "/dev/uart0"
- `flags`：ARKRTOS_PATH_O_* combination
**返回**：a handle (>= 0) on success, a negative error code on failure
> 说明：Directories open read only and take ARKRTOS_PATH_O_DIRECTORY; they are
there for arkrtos_path_readdir(). Writing a directory is refused
rather than quietly ignored. A path that is neither in / nor under a
mounted point fails - the namespace does not invent a volume.

### `int32  arkrtos_path_read(int32 handle, void *buf, uint32 len);`

```c
int32  arkrtos_path_read(int32 handle, void *buf, uint32 len);
```

**Read from the current offset.**
**返回**：bytes read (0 at end of file), or a negative error code

### `int32  arkrtos_path_write(int32 handle, const void *buf, uint32 len);`

```c
int32  arkrtos_path_write(int32 handle, const void *buf, uint32 len);
```

**Write at the current offset.**
**返回**：bytes written, or a negative error code
> 说明：The volume underneath allows one writer at a time; a second writer
gets -15 (busy) instead of a partial write.

### `int32  arkrtos_path_seek(int32 handle, int32 off, uint32 whence);`

```c
int32  arkrtos_path_seek(int32 handle, int32 off, uint32 whence);
```

**Move the read/write offset.**
- `whence`：ARKRTOS_PATH_SEEK_SET / _CUR / _END
**返回**：the new absolute offset (>= 0); a negative error code on failure,
in which case the offset is left where it was. Seeking past the end
of file is allowed (POSIX); whether anything can be written there is
up to the filesystem. The value is the offset the kernel tracks, so
it is the same on every volume - never read a file system's own
seek return value through this facade.

### `int32  arkrtos_path_readdir(int32 handle, char *name, uint32 name_size`

```c
int32  arkrtos_path_readdir(int32 handle, char *name, uint32 name_size, uint32 *mode, uint32 *size);
```

**Take the next entry of a directory opened with ARKRTOS_PATH_O_DIRECTORY.**
- `name`：buffer for one entry name (no path, one level only)
- `name_size`：capacity of that buffer, including the NUL
- `mode`：receives the file type bits (ARKRTOS_PATH_S_IF*), may be NULL
- `size`：receives the file size in bytes, may be NULL
**返回**：0 = got one; 1 = the directory is exhausted (NOT an error); negative
= a real error. A name that does not fit is reported as -13 rather
than handed back cut short.

### `int32  arkrtos_path_close(int32 handle);`

```c
int32  arkrtos_path_close(int32 handle);
```

**Return a handle to the kernel. Works for files and directories.**
**返回**：0 on success, a negative error code on failure

### `int32  arkrtos_path_stat(const char *path, uint32 *size, uint32 *mode)`

```c
int32  arkrtos_path_stat(const char *path, uint32 *size, uint32 *mode);
```

**Type and size of a path, without opening it.**
- `size`：receives the size in bytes (may be NULL)
- `mode`：receives the type bits (may be NULL)
**返回**：0 on success, a negative error code on failure

### `int32  arkrtos_path_unlink(const char *path);`

```c
int32  arkrtos_path_unlink(const char *path);
```

**Remove one file.**
**返回**：0 on success, a negative error code on failure
> 说明：Files only: this is not the way to remove a directory.

### `int32  arkrtos_path_mkdir(const char *path);`

```c
int32  arkrtos_path_mkdir(const char *path);
```

**Create a directory, one level deep; the parent must exist.**
**返回**：0 on success, a negative error code on failure

### `int32  arkrtos_path_rmdir(const char *path);`

```c
int32  arkrtos_path_rmdir(const char *path);
```

**Remove an empty directory. A file needs arkrtos_path_unlink, and**
a non-empty directory is refused, so nothing is lost silently.
**返回**：0 on success, a negative error code on failure

### `int32  arkrtos_path_rename(const char *old_path, const char *new_path)`

```c
int32  arkrtos_path_rename(const char *old_path, const char *new_path);
```

**Rename or move a path inside one mount.**
**返回**：0 on success, a negative error code on failure. A rename across
two mount points is refused (-12) rather than done as a copy.

### `int32  arkrtos_path_truncate(const char *path, uint32 size);`

```c
int32  arkrtos_path_truncate(const char *path, uint32 size);
```

**Set the length of a file by path (POSIX truncate).**
- `path`：absolute namespace path
- `size`：new length; shrinking drops the tail, growing fills with 0
**返回**：0 on success, a negative error code on failure
> 说明：A missing path is -1 (no such file): this call never creates one.
A directory is refused (-5). Nothing is left half done: the length
either changed or the call reported why.

### `int32  arkrtos_path_ftruncate(int32 handle, uint32 size);`

```c
int32  arkrtos_path_ftruncate(int32 handle, uint32 size);
```

**Set the length of an open file (POSIX ftruncate).**
- `handle`：a handle from arkrtos_path_open()
- `size`：new length
**返回**：0 on success, a negative error code on failure
> 说明：The offset is left where it was (POSIX); a position past the new end
is not an error, the next read just reports end of file.

## kernelsrc/include/arkrtos_app_image.h

**Ark RTOS App 镜像文件格式（打包工具与 Loader 共用）**
App 镜像 = 固定 256 字节头 + 代码/数据段原始内容（负载）+ 重定位表。
镜像由 App 工程编译产物转换而来（见 tools/pack_app.py），
Loader 按本结构解析、校验、重定位并写入镜像池。

### `#define ARKRTOS_APP_MAGIC           (0x53564341u)`

```c
#define ARKRTOS_APP_MAGIC           (0x53564341u)
```

**App 镜像头魔数 "SVCA" */**

### `#define ARKRTOS_APP_HEADER_SIZE     (256u)`

```c
#define ARKRTOS_APP_HEADER_SIZE     (256u)
```

**App 镜像头固定长度（字节） */**

### `#define ARKRTOS_APP_TYPE_APP        (1u) #define ARKRTOS_APP_TYPE_DRIV`

```c
#define ARKRTOS_APP_TYPE_APP        (1u) #define ARKRTOS_APP_TYPE_DRIVER     (2u)
```

**镜像类型 */**

### `#define ARKRTOS_APP_TYPE_MINIAPP    (3u)`

```c
#define ARKRTOS_APP_TYPE_MINIAPP    (3u)
```

**小程序（MiniApp）：文件系统里的可执行件，运行时才载入内存**
> 说明：与上两个类型的根本区别：
- **不进镜像池**。它存在文件系统里（普通文件，扩展名 .mini），
因此没有槽位记录、不参与安装/卸载/搬移/回收那一整套。
- **运行时整体载入 RAM 执行**（App 是代码留在 Flash 原地执行）。
退出即释放，再跑重新载入。这是拿 RAM 换「不用安装」。
- 容器格式与类型 1/2 完全一致（256B 头 + 重定位表 + 负载），
所以打包/校验/重定位代码完全复用；区别只在装载路径与运行位置。
池内安装器只接受 1/2，把 3 装进 Flash 池会被拒收。
详见 docs/小程序设计.md。*/

### `#define ARKRTOS_APP_TYPE_MASK(t)    (1u << (uint32)(t))`

```c
#define ARKRTOS_APP_TYPE_MASK(t)    (1u << (uint32)(t))
```

**类型位掩码：内核的头部校验函数按掩码判定「这个类型接受不接受」**
用具名掩码而不是在调用点写 `type == A || type == B`，是因为
校验函数被多条装载路径共用（安装 / 上电扫描 / 搬移 / 小程序），
每条路径接受的类型不同，而「能不能接受」必须只有一处判定。 */

### `#define ARKRTOS_APP_TYPE_MASK_POOL \ (ARKRTOS_APP_TYPE_MASK(ARKRTOS_AP`

```c
#define ARKRTOS_APP_TYPE_MASK_POOL \ (ARKRTOS_APP_TYPE_MASK(ARKRTOS_APP_TYPE_APP) | ARKRTOS_APP_TYPE_MASK(ARKRTOS_APP_TYPE_DRIVER))
```

**镜像池安装器接受的全部类型（小程序不在列：它属于文件系统） */**

### `#define ARKRTOS_APP_TYPE_MASK_MINI  (ARKRTOS_APP_TYPE_MASK(ARKRTOS_APP`

```c
#define ARKRTOS_APP_TYPE_MASK_MINI  (ARKRTOS_APP_TYPE_MASK(ARKRTOS_APP_TYPE_MINIAPP))
```

**小程序装载路径接受的类型 */**

### `#define ARKRTOS_APP_FLAG_AUTOSTART  (1u << 0)   /* 开机扫描认定后被自动启动 */`

```c
#define ARKRTOS_APP_FLAG_AUTOSTART  (1u << 0)   /* 开机扫描认定后被自动启动 */
```

**镜像头 flags 位定义（见 arkrtos_app_header_t.flags）**
> 说明：该字段落在头部 offset 96（原 reserved 区首 4 字节）。旧镜像该位置
恒为 0，等价于「不设任何标志」，因此新增本字段不破坏旧镜像。 */

### `#define ARKRTOS_APP_FLAG_NONE       (0u)`

```c
#define ARKRTOS_APP_FLAG_NONE       (0u)
```

**未设置任何标志 */**

### `#define ARKRTOS_APP_STATE_VALID       (0u)      /* 已提交，可用（旧镜像恒为该值） */ `

```c
#define ARKRTOS_APP_STATE_VALID       (0u)      /* 已提交，可用（旧镜像恒为该值） */ #define ARKRTOS_APP_STATE_UNCOMMITTED (1u)      /* 未提交，上电必须丢弃 */
```

**镜像提交状态（arkrtos_app_header_t.state）**
搬移一个镜像时先在新落点写一份 UNCOMMITTED 副本，校验通过后
只把这一个字段改写为 VALID（单字编程，原子），最后才擦除旧副本。
上电扫描见到 UNCOMMITTED 一律丢弃；见到同 image_id 的两份 VALID
则保留地址较低的那份。两条规则合起来使断电恢复幂等且确定，
因此不需要额外的日志扇区。
注意：本字段只能表达「安装/搬移的提交点」。不能拿它表达「卸载」：
镜像一旦提交，state 已经是 0，而 Flash 编程只能把 1 清成 0，
写 UNCOMMITTED（=1）要求把 0 置 1，硬件会静默保持 0——卸载因此
变成空操作，镜像在下次上电又会被扫回来。卸载走的是清 type 字段
（见 ARKRTOS_APP_OFF_TYPE）。

### `#define ARKRTOS_APP_RELOC_KIND_MASK  (0x3u) #define ARKRTOS_APP_RELOC_`

```c
#define ARKRTOS_APP_RELOC_KIND_MASK  (0x3u) #define ARKRTOS_APP_RELOC_KIND_ROM   (0u) #define ARKRTOS_APP_RELOC_KIND_RAM   (1u)
```

**重定位表条目编码**
每项是一个 32 位字：低 2 位是类型，高位是「相对镜像起始」的偏移。
表项按偏移升序排列。
为什么需要两种类型：镜像里既有指向**自身代码/只读数据**的绝对地址，
也有指向**自己的 RAM 窗口**的绝对地址。前者随 Flash 落点平移，
后者随 RAM 块基址平移，两者增量不同，必须分开标记。
kind = 0（ROM）：装载时 word += (落点 + 负载偏移 - nominal_base)
kind = 1（RAM）：装载时 word += (RAM 块基址 - nominal_ram_base)
之所以 ROM 增量还要加上「负载偏移」：负载被链接在 nominal_base 上，
运行期却落在 落点+payload_offset——重定位表本身把负载往后推了
reloc_count*4 字节，这部分位移必须计入，否则代码里的自引用会整体
偏出一个表长。
两个基址都在镜像头里声明，因此单个镜像被整体搬移时只需要
对 ROM 类表项再叠加一个增量（新落点 - 旧落点），RAM 类不动。
> 说明：偏移低 2 位用来放类型，因此偏移必须按「半字」为单位存放：
表项 = (offset / 2) << 2 | kind。Thumb 的 32 位指令只保证
半字对齐，MOVW/MOVT 指令对完全可能落在 2 mod 4 的偏移上；
若按字节直接存放偏移，低 2 位会被类型覆盖，这类表项会静默
少 2 字节（内存里的表是对的，写进镜像再读出来才错）。

### `#define ARKRTOS_APP_RELOC_KIND_ROM_MOVW  (2u)`

```c
#define ARKRTOS_APP_RELOC_KIND_ROM_MOVW  (2u)
```

**MOVW/MOVT 立即数对里的 ROM 类地址（表项占 8 字节：两条指令） */**

### `#define ARKRTOS_APP_RELOC_KIND_RAM_MOVW  (3u)`

```c
#define ARKRTOS_APP_RELOC_KIND_RAM_MOVW  (3u)
```

**MOVW/MOVT 立即数对里的 RAM 类地址（表项占 8 字节：两条指令） */**

### `#define ARKRTOS_APP_RELOC_KIND_32    (0u)`

```c
#define ARKRTOS_APP_RELOC_KIND_32    (0u)
```

**重定位表编码版本：偏移按字节存放（已废弃，低 2 位会被类型截断） */**

### `#define ARKRTOS_APP_RELOC_KIND_HALF  (1u)`

```c
#define ARKRTOS_APP_RELOC_KIND_HALF  (1u)
```

**重定位表编码版本：偏移按半字存放（当前版本） */**

### `#define ARKRTOS_APP_RELOC_KIND_CUR   (ARKRTOS_APP_RELOC_KIND_HALF)`

```c
#define ARKRTOS_APP_RELOC_KIND_CUR   (ARKRTOS_APP_RELOC_KIND_HALF)
```

**内核唯一接受的表编码版本 */**

### `#define ARKRTOS_APP_RELOC_OFF_UNIT   (2u)`

```c
#define ARKRTOS_APP_RELOC_OFF_UNIT   (2u)
```

**表项里偏移的存放单位（字节）：2 = 按半字 */**

### `#define ARKRTOS_APP_RELOC_OFF(e) \ (((e) & ~ARKRTOS_APP_RELOC_KIND_MAS`

```c
#define ARKRTOS_APP_RELOC_OFF(e) \ (((e) & ~ARKRTOS_APP_RELOC_KIND_MASK) >> 1)
```

**从条目取出偏移（相对镜像起始，按半字存放，恒为偶数） */**

### `#define ARKRTOS_APP_RELOC_ENTRY(off, kind) \ ((((uint32)(off) / ARKRTO`

```c
#define ARKRTOS_APP_RELOC_ENTRY(off, kind) \ ((((uint32)(off) / ARKRTOS_APP_RELOC_OFF_UNIT) << 2) | \ ((uint32)(kind) & ARKRTOS_APP_RELOC_KIND_MASK))
```

**由偏移与类型组装条目（与 ARKRTOS_APP_RELOC_OFF 互逆） */**

### `#define ARKRTOS_APP_RELOC_KIND(e)    ((e) & ARKRTOS_APP_RELOC_KIND_MAS`

```c
#define ARKRTOS_APP_RELOC_KIND(e)    ((e) & ARKRTOS_APP_RELOC_KIND_MASK)
```

**从条目取出类型 */**

### `#define ARKRTOS_APP_RELOC_SIZE       (4u)`

```c
#define ARKRTOS_APP_RELOC_SIZE       (4u)
```

**一条重定位表目占用的字节数 */**

### `#define ARKRTOS_APP_RELOC_MOV_SIZE   (8u)`

```c
#define ARKRTOS_APP_RELOC_MOV_SIZE   (8u)
```

**MOVW/MOVT 表项覆盖的字节数（两条 4 字节指令） */**

### `#define ARKRTOS_APP_RELOC_NONE       (0u)`

```c
#define ARKRTOS_APP_RELOC_NONE       (0u)
```

**重定位表无需 CRC 归零的字段，故整表参与校验；本宏仅为可读性 */**

### `#define ARKRTOS_APP_RELOC_MAX        (1024u)`

```c
#define ARKRTOS_APP_RELOC_MAX        (1024u)
```

**重定位表条目数上限（内核用于边界校验；表本身不驻留 RAM）**
> 说明：表项按「需要修补的字」计数，不是按镜像字节数。4 KB 表 = 1024 个
条目，对 1 MB 池里最大的镜像（768 KB 负载）也留出足够余量。 */

### `#define ARKRTOS_APP_RELOC_TABLE_MAX  (ARKRTOS_APP_RELOC_MAX * ARKRTOS_`

```c
#define ARKRTOS_APP_RELOC_TABLE_MAX  (ARKRTOS_APP_RELOC_MAX * ARKRTOS_APP_RELOC_SIZE)
```

**重定位表字节数上限（= 条目上限 x 每项字节数） */**

### `#define ARKRTOS_APP_RELOC_OFFSET     (ARKRTOS_APP_HEADER_SIZE)`

```c
#define ARKRTOS_APP_RELOC_OFFSET     (ARKRTOS_APP_HEADER_SIZE)
```

**重定位表在镜像内的固定偏移（紧跟镜像头） */**

### `#define ARKRTOS_APP_PAYLOAD_OFFSET(p_hdr) \ (ARKRTOS_APP_HEADER_SIZE +`

```c
#define ARKRTOS_APP_PAYLOAD_OFFSET(p_hdr) \ (ARKRTOS_APP_HEADER_SIZE + ((p_hdr)->reloc_count * ARKRTOS_APP_RELOC_SIZE))
```

**负载相对镜像起始的偏移（= 头 + 重定位表）**
- `p_hdr`：指向 arkrtos_app_header_t 的指针

### `#define ARKRTOS_APP_TOTAL_LEN(p_hdr) \ (ARKRTOS_APP_PAYLOAD_OFFSET(p_h`

```c
#define ARKRTOS_APP_TOTAL_LEN(p_hdr) \ (ARKRTOS_APP_PAYLOAD_OFFSET(p_hdr) + (p_hdr)->image_size)
```

**镜像总长度（头 + 重定位表 + 负载）**
- `p_hdr`：指向 arkrtos_app_header_t 的指针

### `typedef struct {`

```c
typedef struct {
```

**App 镜像头（固定 256 字节）**
> 说明：CRC 覆盖范围：本头结构体（crc32 与 state 两个字段按 0 参与计算）
+ 负载 + 重定位表。之所以把 state 排除在外，是因为它必须能在写入
之后被单独改写（提交动作），而 CRC 不能因此失效。
crc32 字段自身的偏移见 ARKRTOS_APP_OFF_CRC32，state 见 ARKRTOS_APP_OFF_STATE。

### `#define ARKRTOS_APP_OFF_CRC32       (28u)`

```c
#define ARKRTOS_APP_OFF_CRC32       (28u)
```

**crc32 字段在头内的偏移（Loader 计算 CRC 时要把它按 0 处理） */**

### `#define ARKRTOS_APP_OFF_STATE       (100u)`

```c
#define ARKRTOS_APP_OFF_STATE       (100u)
```

**state 字段在头内的偏移（同上） */**

### `#define ARKRTOS_APP_OFF_RUNTIME_RAM_BASE (252u)`

```c
#define ARKRTOS_APP_OFF_RUNTIME_RAM_BASE (252u)
```

**runtime_ram_base 字段在头内的偏移。它必须与 crc32 / state 一样按四个 0**
字节代入镜像 CRC：这个值不是「传输来的内容」，而是安装方在落盘前
才决定并写进去的本地信息（安装时分配到的 RAM 块基址）。若把它算进
校验，落盘形态与打包工具算的 want 就永远对不上，每一次安装都会
误报 transfer crc 不符。 */

### `#define ARKRTOS_APP_OFF_TYPE        (4u)`

```c
#define ARKRTOS_APP_OFF_TYPE        (4u)
```

**type 字段在头内的偏移。卸载把镜像作废时清这一个字：**
0 -> 0 之外没有任何位要动，是合法的 1 -> 0 编程；
而「类型不是已知镜像」会被 check_header 拒收、上电扫描丢弃。
magic 故意留着，好让回收沿用「本区间有带头内容」的判断。 */

### `#define ARKRTOS_APP_OFF_SIGN        (32u)`

```c
#define ARKRTOS_APP_OFF_SIGN        (32u)
```

**signature 字段在头内的偏移（64 字节，见 arkrtos_sign.h） */**

### `typedef char arkrtos_app_header_size_check[ (sizeof(arkrtos_app_header`

```c
typedef char arkrtos_app_header_size_check[ (sizeof(arkrtos_app_header_t) == ARKRTOS_APP_HEADER_SIZE) ? 1 : -1];
```

**镜像头尺寸静态校验**
头结构与 ARKRTOS_APP_HEADER_SIZE 必须严格一致：打包工具与 Loader 分别按
“结构体布局”和“固定长度宏”解释镜像，一旦不一致就会静默错位。
此断言让不一致在编译期直接暴露。

### `typedef char arkrtos_app_off_crc32_check[ (__builtin_offsetof(arkrtos_`

```c
typedef char arkrtos_app_off_crc32_check[ (__builtin_offsetof(arkrtos_app_header_t, crc32) == ARKRTOS_APP_OFF_CRC32) ? 1 : -1];
```

**关键字段偏移静态校验**
Loader 计算 CRC 时按「偏移切片」而不是整体结构体来累加 CRC
（crc32 与 state 两个字段要按 0 参与），因此这些偏移一旦漂移
就会静默算出错误的校验值。这里逐个钉死。

### `typedef char arkrtos_app_off_sign_check[ (__builtin_offsetof(arkrtos_a`

```c
typedef char arkrtos_app_off_sign_check[ (__builtin_offsetof(arkrtos_app_header_t, signature) == ARKRTOS_APP_OFF_SIGN) ? 1 : -1];
```

**signature 字段的偏移断言（签名子系统按它取 64 字节） */**

### `uint32 arkrtos_crc32(const void *data, uint32 len, uint32 crc);`

```c
uint32 arkrtos_crc32(const void *data, uint32 len, uint32 crc);
```

**计算 CRC32（与打包工具使用同一算法：IEEE 802.3 反射多项式）**
- `data`：数据指针
- `len`：数据长度（字节）
- `crc`：前一段的 CRC 值（首段传 0）
**返回**：累积的 CRC32 值

## kernelsrc/include/arkrtos_apppack.h

**把「同一份源码的四遍链接结果」打成 Ark RTOS 可重定位镜像（.svcm）**
本文件只有纯计算：输入是四份负载字节 + 窗口参数，输出是一份镜像。
不读文件、不调编译器、不碰全局状态——四遍链接由调用方（板上是
`cc --pack`，宿主上是测试程序）负责，因此这份算法可以在 PC 上

### `#define ARKRTOS_APPPACK_ERR_PARAM   (-30)`

```c
#define ARKRTOS_APPPACK_ERR_PARAM   (-30)
```

**打包失败：参数非法（空指针 / 窗口不是 2 的幂 / payload 长度不一致） */**

### `#define ARKRTOS_APPPACK_ERR_DIFF    (-31)`

```c
#define ARKRTOS_APPPACK_ERR_DIFF    (-31)
```

**打包失败：某个位置无法用一个基址增量解释（不可重定位） */**

### `#define ARKRTOS_APPPACK_ERR_VERIFY  (-32)`

```c
#define ARKRTOS_APPPACK_ERR_VERIFY  (-32)
```

**打包失败：把表应用回 A 之后与 D 不逐字节相同（表是错的） */**

### `#define ARKRTOS_APPPACK_ERR_SIZE    (-33)`

```c
#define ARKRTOS_APPPACK_ERR_SIZE    (-33)
```

**打包失败：表项数超过 ARKRTOS_APP_RELOC_MAX，或输出缓冲装不下 */**

### `#define ARKRTOS_APPPACK_ERR_ENTRY   (-34)`

```c
#define ARKRTOS_APPPACK_ERR_ENTRY   (-34)
```

**打包失败：A 里没有可用的向量表（+4 不是本窗口内的地址） */**

### `typedef struct {`

```c
typedef struct {
```

**打包窗口：镜像被链接到的标称基址与两个窗口长度**
两个 size 都必须是 2 的幂且 >= 8：落点由池的分配粒度决定，非 2 的幂
的窗口在运行期永远对不上，与其在板上 Camb 不如在这里拒绝。

### `typedef struct {`

```c
typedef struct {
```

**打包统计（只读输出，用于「表项数 × 4 对镜像大小的影响」这条账本）**
四类计数分开报：某一类恒为 0 就说明这一类的绝对地址在测试程序里
根本没出现，等于那一类没被验证——这是反假绿要看的读数。

### `int32 arkrtos_apppack_build(const uint8 *a, const uint8 *b, const uint`

```c
int32 arkrtos_apppack_build(const uint8 *a, const uint8 *b, const uint8 *c, const uint8 *d, uint32 len, uint32 delta, const arkrtos_apppack_win_t *win, uint32 type, uint32 version, uint8 *out, uint32 out_cap, uint32 *out_len, arkrtos_apppack_stats_t *stats);
```

**由四份链接结果打出最终镜像**
- `a`：标称基址那份负载（镜像的最后内容就是它）
- `b`：ROM 基址 +delta 那份负载
- `c`：RAM 基址 +delta 那份负载
- `d`：ROM 基址 +2*delta、RAM 基址 +delta 那份负载（验证用）
- `len`：四份负载共用的长度（必须相等且为 4 的倍数）
- `delta`：差分链接的基址增量（必须 >= rom_size 且 >= ram_size，见下）
- `win`：窗口参数；ram_base 会写进镜像头的 nominal_ram_base
- `type`：镜像类型（小程序走 ARKRTOS_APP_TYPE_MINIAPP）
- `version`：镜像版本号（写进头里，仅供上层记账）
- `out`：输出缓冲；布局 = 头(256) + 重定位表 + 负载
- `out_cap`：输出缓冲容量，不够时报 ARKRTOS_APPPACK_ERR_SIZE（不截断）
- `out_len`：收镜像总长度
- `stats`：可为 0；非 0 时收四类表项计数与入口偏移
**返回**：0=成功；负值为 ARKRTOS_APPPACK_ERR_x
> 说明：delta 要求 >= 两个窗口：delta 小于窗口时 B/C 的地址段会与 A 重叠，
「该位置的值恰好差 delta」就不再是「它是个绝对地址」的证据——重叠段里
一个普通常量也可能满足。这不是保守，是判据成立的前提。
> 说明：入口有两种合法形态，由负载自己的字节判定（不接受「调用方声明」，因为
`cc --pack` 的同一条路径既打应用镜像也打小程序）：
① 带向量表：+0 = 初始 SP（落在 RAM 窗口内）、+4 = 入口（Thumb 位为 1、
落在载荷内、且不落在表本身那 8 字节上）⇒ entry_offset 取 +4 的值；
② 平铺小程序：没有复位向量（栈由 mini run 给），+0 就是启动桩的第一条
指令 ⇒ 只有在 +0 认出 THUMB PUSH（0xB4xx/0xB5xx，SDK 实测 0xB5E0）
时才接受，entry_offset = 0。
两条都不成立报 ARKRTOS_APPPACK_ERR_ENTRY，不猜入口。
> 说明：本函数不校验 D 的来源：D 必须由同一个 delta 与同一份源码链接得到，
否则 verify 会失败并报 ARKRTOS_APPPACK_ERR_VERIFY（这已经足够，
不需要额外声明一个「你是不是真的这么链的」参数）。

### `const char *arkrtos_apppack_error_name(int32 code);`

```c
const char *arkrtos_apppack_error_name(int32 code);
```

**错误码的可读名（给 shell 用；未知码如实回 "unknown"，不猜）**

## kernelsrc/include/arkrtos_apprun.h

**板上直跑（app run）：把 arkc 在板上编出来的裸可执行文件直接跑起来**
与小程序（arkrtos_mini.h）的差别只有一句话：
- 小程序跑的是**镜像**：256 字节头 + 重定位表 + 负载，装载时按 delta 打补丁，
所以负载可以链接在 Flash 的标称地址上、落到池里任意一块。

### `int32 arkrtos_apprun_run(const char *path);`

```c
int32 arkrtos_apprun_run(const char *path);
```

**把文件系统里的一个裸可执行文件装进窗口并起任务**
- `path`：卷内绝对路径，如 "/tmp/a.bin"
**返回**：0=已启动；负值为 ARKRTOS_LOADER_ERR_x / ARKRTOS_MINI_ERR_FS /
ARKRTOS_APPRUN_ERR_x
顺序是「先占窗、再读文件、再起任务」：读文件之前窗口必须已经在手，
否则一次失败的装载会把别人正在跑的代码覆盖掉。

### `int32 arkrtos_apprun_run_ex(const char *path, uint32 *task_out);`

```c
int32 arkrtos_apprun_run_ex(const char *path, uint32 *task_out);
```

**同 arkrtos_apprun_run，但把本次起跑的任务号带回来**
- `task_out`：非空时写回任务号（>0）
> 说明：为什么需要它：app 的优先级高于 shell，arkrtos_apprun_run() 内部一
activate 就可能把 CPU 交给 app，而 app 可能在 shell 打印「已启动」
之前就自己退出了 —— 这时再用 arkrtos_apprun_task_id() 去读只能读到 0，
界面上就会出现「started (task 0)」这种自相矛盾的话。任务号必须在
activate 之前带出来。

### `int32 arkrtos_apprun_on_task_exit(uint32 task_id);`

```c
int32 arkrtos_apprun_on_task_exit(uint32 task_id);
```

**主任务退出时的回收钩子（由 arkrtos_task.c 的任务退出路径调用）**
**返回**：0=已回收或与本次直跑无关；负值=归还失败（记账出错，要能被看见）
> 说明：只认自己的主任务号：别的任务（内核任务、App、小程序）退出原样返回 0。

### `uint32 arkrtos_apprun_task_id(void);`

```c
uint32 arkrtos_apprun_task_id(void);
```

**当前在跑的任务号；0 = 没有 */**

### `int32 arkrtos_apprun_info(uint32 *p_entry, uint32 *p_code_base, uint32`

```c
int32 arkrtos_apprun_info(uint32 *p_entry, uint32 *p_code_base, uint32 *p_ram_base);
```

**起跑时用的入口地址与两个窗口（调试 / shell 展示）；0 = 没有在跑 */**

### `const char *arkrtos_apprun_error_name(int32 rc);`

```c
const char *arkrtos_apprun_error_name(int32 rc);
```

**错误码的可读名字（只覆盖本路径独有的三个，其余交给调用方） */**

## kernelsrc/include/arkrtos_audit.h

**Kernel self-audit: does the shared state still say what it should?**
The kernel hands its partition table, its slot records and its task
table to code that was not written by the kernel (the loader, the
installer, the shell) and runs image code that shares the same

### `#define ARKRTOS_AUDIT_BIT_MAX     (ARKRTOS_AUDIT_SCHED)`

```c
#define ARKRTOS_AUDIT_BIT_MAX     (ARKRTOS_AUDIT_SCHED)
```

**Highest bit this module uses (kept explicit so the shell can loop) */**

### `typedef struct {`

```c
typedef struct {
```

**Result of one audit pass */**

### `#define ARKRTOS_AUDIT_INDEX_NONE  (0xFFFFFFFFu)`

```c
#define ARKRTOS_AUDIT_INDEX_NONE  (0xFFFFFFFFu)
```

**"no offender index applies" */**

### `void arkrtos_audit_run(arkrtos_audit_result_t *p_out);`

```c
void arkrtos_audit_run(arkrtos_audit_result_t *p_out);
```

**Run one full audit pass and write the verdict to @p p_out.**
- `p_out`：Receives mask / index / checks. Must not be NULL.
> 说明：Read-only: the audit never modifies the structures it inspects, so it
is safe to call from the shell while the system keeps running.

### `uint32 arkrtos_audit_last_mask(void);`

```c
uint32 arkrtos_audit_last_mask(void);
```

**One pass kept from the last run, for callers that only want the mask.**
**返回**：The mask from the most recent arkrtos_audit_run()/arkrtos_audit_boot().

### `uint32 arkrtos_audit_boot(void);`

```c
uint32 arkrtos_audit_boot(void);
```

**Audit once at boot and record ARKRTOS_FAULT_AUDITFAIL when it fails.**
**返回**：The failing mask (0 = clean). A non-zero value means the boot-time
state already violated an invariant, before any image ran.

### `const char *arkrtos_audit_name(uint32 bit);`

```c
const char *arkrtos_audit_name(uint32 bit);
```

**Human-readable name of one audit bit.**
- `bit`：A single ARKRTOS_AUDIT_x bit.
**返回**：Static string, or NULL when @p bit is not a defined audit bit.

## kernelsrc/include/arkrtos_blk.h

**Ark RTOS block device registry - one handle per non-volatile backend**
The image pool (and the filesystem on top of it) needs to talk to
"a chunk of non-volatile storage"without knowing whether that chunk
is the internal flash controller, an external SPI NOR, or something

### `#define ARKRTOS_BLK_NAME_MAX   (16u)`

```c
#define ARKRTOS_BLK_NAME_MAX   (16u)
```

Max bytes of a device name including the terminating zero */

### `#define ARKRTOS_BLK_MAX_DEVS   (4u)`

```c
#define ARKRTOS_BLK_MAX_DEVS   (4u)
```

How many backends a build can carry. Raise here, not at the call sites. */

### `typedef struct arkrtos_blk_dev {`

```c
typedef struct arkrtos_blk_dev {
```

**One storage backend.**
> 说明：Every function pointer except read/write/erase is optional. The
descriptor itself is expected to be static storage owned by the
backend, because the registry keeps a pointer, not a copy.

### `int32 arkrtos_blk_register(const arkrtos_blk_dev_t *dev);`

```c
int32 arkrtos_blk_register(const arkrtos_blk_dev_t *dev);
```

**Register a backend.**
**返回**：0 on success, -1 if the table is full, the name is empty/too long,
or the descriptor lacks read/write/erase.

### `uint32 arkrtos_blk_count(void);`

```c
uint32 arkrtos_blk_count(void);
```

**Number of registered backends (0 means the board registered none) */**

### `const arkrtos_blk_dev_t *arkrtos_blk_at(uint32 idx);`

```c
const arkrtos_blk_dev_t *arkrtos_blk_at(uint32 idx);
```

**Backend by index in registration order, NULL when out of range */**

### `const arkrtos_blk_dev_t *arkrtos_blk_find(const char *name);`

```c
const arkrtos_blk_dev_t *arkrtos_blk_find(const char *name);
```

**Backend by name, NULL when not found */**

### `int32 arkrtos_blk_init(const arkrtos_blk_dev_t *dev);`

```c
int32 arkrtos_blk_init(const arkrtos_blk_dev_t *dev);
```

**Bring a backend up. Idempotent: the second call is free.**
**返回**：0 when the device is usable, -1 otherwise (stay unavailable).

### `uint8 arkrtos_blk_is_ready(const arkrtos_blk_dev_t *dev);`

```c
uint8 arkrtos_blk_is_ready(const arkrtos_blk_dev_t *dev);
```

**1 when arkrtos_blk_init() has already succeeded for this backend */**

### `uint32 arkrtos_blk_size(const arkrtos_blk_dev_t *dev);`

```c
uint32 arkrtos_blk_size(const arkrtos_blk_dev_t *dev);
```

**Capacity in bytes; 0 when the device is not up or cannot answer */**

### `int32 arkrtos_blk_read (const arkrtos_blk_dev_t *dev, uint32 off, uint`

```c
int32 arkrtos_blk_read (const arkrtos_blk_dev_t *dev, uint32 off, uint8 *buf, uint32 len);
```

**Read len bytes at offset off.**
> 说明：A range check runs first, so a bad command cannot touch hardware.

### `int32 arkrtos_blk_write(const arkrtos_blk_dev_t *dev, uint32 off, cons`

```c
int32 arkrtos_blk_write(const arkrtos_blk_dev_t *dev, uint32 off, const uint8 *buf, uint32 len);
```

**Write len bytes at offset off (target must already be erased) */**

### `int32 arkrtos_blk_erase(const arkrtos_blk_dev_t *dev, uint32 off, uint`

```c
int32 arkrtos_blk_erase(const arkrtos_blk_dev_t *dev, uint32 off, uint32 len);
```

**Erase len bytes starting at off.**
> 说明：off and len must both be multiples of erase_unit. A non-aligned
request is rejected instead of being silently rounded, because
rounding up can wipe a neighbouring record the caller still needs.

## kernelsrc/include/arkrtos_cfg.h

**Ark RTOS 任务配置与任务表声明**
声明全局任务表、任务计数，以及任务配置加载与任务栈初始化接口。
arkrtos_cfg_load 在内核启动时加载任务配置，arkrtos_task_stack_init 初始化任务栈帧。

### `void arkrtos_task_stack_fill(uint32 *stack_bottom, uint32 stack_size);`

```c
void arkrtos_task_stack_fill(uint32 *stack_bottom, uint32 stack_size);
```

**用填充图案覆盖整段任务栈（栈底保护字除外）**
- `stack_bottom`：栈底地址
- `stack_size`：栈大小（字节）
> 说明：仅在任务创建/重建时调用，用于后续统计峰值栈用量。

## kernelsrc/include/arkrtos_crash.h

**Cross-reset crash journal: per-slot fault counts, disable state and**
the reason the kernel stopped trusting an image.
The restart policy has always had one hole. A fault is counted in
the partition table, which lives in shared RAM: shared RAM survives

### `#define ARKRTOS_CRASH_MAGIC       (0x5343524Au)`

```c
#define ARKRTOS_CRASH_MAGIC       (0x5343524Au)
```

**Journal magic "SCRJ" - an uninitialized RAM area holds garbage */**

### `#define ARKRTOS_CRASH_VERSION     (2u)`

```c
#define ARKRTOS_CRASH_VERSION     (2u)
```

**Journal layout version. A build that changes the struct must bump**
this: an older journal is then rejected as "not evidence" instead of
being interpreted with the wrong field offsets. */

### `typedef struct {`

```c
typedef struct {
```

**The journal itself, placed in the UNINIT region by the linker.**
> 说明：Size is checked at compile time against CRASH_LOG_SIZE; growing this
struct without growing the region is a build error, not a silent
overlap with the end of shared RAM.

### `void arkrtos_crash_init(void);`

```c
void arkrtos_crash_init(void);
```

**Validate the journal, or start a clean one.**
Called once per boot, after the partition table exists and before
any image is started. A journal that fails magic / version / checksum is
discarded rather than trusted: leftover RAM bytes and a half-written
record both look like data, and acting on either would disable an
image for something it never did.
> 说明：The slot arrays are ARKRTOS_SLOT_ARRAY_MAX long, so the journal can
never describe fewer slots than the partition table.

### `void arkrtos_crash_scan_apply(void);`

```c
void arkrtos_crash_scan_apply(void);
```

**Reconcile the journal with the slot table the pool scan just built.**
For a slot whose image still sits at the address the entry was
written for, the count is adopted. For anything else (free slot,
replaced image, moved image) the entry is dropped - a stale count
charged to a newcomer would disable an image for someone else's
fault. A slot whose adopted count already reached the limit is put
back into the disabled state the operator saw before the reset:
autostart cleared, slot state INVALID, and a line on the console.
> 说明：Must run after arkrtos_loader_scan(), before the autostart pass.

### `uint32 arkrtos_crash_fault(uint32 slot, uint32 reason);`

```c
uint32 arkrtos_crash_fault(uint32 slot, uint32 reason);
```

**Count one fault against a slot, and disable it when the limit is met.**
- `slot`：Slot record index.
- `reason`：ARKRTOS_CRASH_REASON_x to remember as this fault's reason.
**返回**：The new consecutive-fault count (0 when the slot cannot be
accounted for at all).
> 说明：The count is mirrored into the partition table so existing readers
(shell, host GUI) keep working; the journal is the copy that survives
a reset. A slot with no pool address has no identity to tag, and is
therefore counted in RAM only - the caller is told so by the return
value being the RAM count while the journal stays untouched.

### `uint32 arkrtos_crash_disabled(uint32 slot);`

```c
uint32 arkrtos_crash_disabled(uint32 slot);
```

**Is this slot disabled by the kernel?**
**返回**：ARKRTOS_CRASH_REASON_x (non-zero) when disabled, 0 when enabled.

### `uint32 arkrtos_crash_disabled_tick(uint32 slot);`

```c
uint32 arkrtos_crash_disabled_tick(uint32 slot);
```

**Kernel tick at which the slot was disabled (0 = before this boot). */**

### `uint32 arkrtos_crash_count(uint32 slot);`

```c
uint32 arkrtos_crash_count(uint32 slot);
```

**Consecutive faults on record for this slot.**
> 说明：This is the copy that survives a reset; slot_crash_cnt in the
partition table is mirrored from it, so both answer the same
question rather than two slightly different ones.

### `uint32 arkrtos_crash_last_reason(uint32 slot);`

```c
uint32 arkrtos_crash_last_reason(uint32 slot);
```

**Reason of the slot's most recent fault (ARKRTOS_CRASH_REASON_x). */**

### `void arkrtos_crash_forget(uint32 slot);`

```c
void arkrtos_crash_forget(uint32 slot);
```

**Forget one slot's crash history and clear its disabled state.**
Used by the two events that genuinely mean "start over": a fresh
install of the image, and an explicit human retry from the console.

### `uint32 arkrtos_crash_boots(void);`

```c
uint32 arkrtos_crash_boots(void);
```

**Boots recorded by the current journal (1 right after power-on). */**

### `uint32 arkrtos_crash_seq(void);`

```c
uint32 arkrtos_crash_seq(void);
```

**Committed writes to the journal (evidence that it is being kept). */**

### `const char *arkrtos_crash_reason_name(uint32 reason);`

```c
const char *arkrtos_crash_reason_name(uint32 reason);
```

**Human-readable name of a disable reason, or "none" for 0. */**

### `uint32 arkrtos_crash_area_base(void);`

```c
uint32 arkrtos_crash_area_base(void);
```

**Address and size of the journal region, for the shell and the audit. */**

### `uint32 arkrtos_crash_hash_path(const char *path);`

```c
uint32 arkrtos_crash_hash_path(const char *path);
```

**FNV-1a hash of an image path, the MiniApp identity. @return 0 only**
for a null/empty path (never a real path) so 0 can mean "free". */

### `uint32 arkrtos_crash_mini_fault(uint32 hash, uint32 reason);`

```c
uint32 arkrtos_crash_mini_fault(uint32 hash, uint32 reason);
```

**Count one fault against a MiniApp path, disabling it at the limit.**
- `hash`：arkrtos_crash_hash_path() of the path.
- `reason`：ARKRTOS_CRASH_REASON_x to remember.
**返回**：The new consecutive-fault count, or 0 when it could not be recorded
at all (hash 0, or every journal entry already held by a disabled
MiniApp - the caller is told so instead of being given a made-up
count).
> 说明：Runs from exception context on the fault path: critical section only.

### `uint32 arkrtos_crash_mini_disabled(uint32 hash);`

```c
uint32 arkrtos_crash_mini_disabled(uint32 hash);
```

**ARKRTOS_CRASH_REASON_x when this path is disabled, 0 when it may run. */**

### `uint32 arkrtos_crash_mini_count(uint32 hash);`

```c
uint32 arkrtos_crash_mini_count(uint32 hash);
```

**Consecutive faults on record for this path (0 when untracked). */**

### `uint32 arkrtos_crash_mini_disabled_tick(uint32 hash);`

```c
uint32 arkrtos_crash_mini_disabled_tick(uint32 hash);
```

**Tick at which this path was disabled (0 = not disabled / before boot). */**

### `uint32 arkrtos_crash_mini_at(uint32 idx, uint32 *hash, uint32 *cnt, ui`

```c
uint32 arkrtos_crash_mini_at(uint32 idx, uint32 *hash, uint32 *cnt, uint32 *last, uint32 *hold, uint32 *tick);
```

**Read one MiniApp journal entry by table index.**
The slot table can be listed because a slot has a number; a MiniApp
is identified by a path hash instead, so a listing has to walk the
table index by index. A free entry reads back as all zeroes so a
caller sees the same "nothing here" a slot listing sees for an
empty slot, rather than a path hash it then has to explain away.
- `idx`：0..ARKRTOS_CRASH_MINI_MAX-1.
- `hash`：receives the path hash (0 = free).
- `cnt`：receives the consecutive-fault count.
- `last`：receives the most recent fault reason (ARKRTOS_CRASH_REASON_x).
- `hold`：receives the disable reason (ARKRTOS_CRASH_REASON_x, 0 = enabled).
- `tick`：receives the tick it was disabled at (0 = not / before this boot).
**返回**：1 when the entry is in use, 0 when free or idx is out of range.
> 说明：Only the hash is kept, never the path: the journal must stay a fixed
size in UNINIT RAM, and a path is variable-length. A caller that wants
the path has to match the hash against a path it already knows.

### `void arkrtos_crash_mini_forget(uint32 hash);`

```c
void arkrtos_crash_mini_forget(uint32 hash);
```

**Forget this path's crash history and clear its disabled state. */**

## kernelsrc/include/arkrtos_dt.h

**Ark RTOS device tree query API.**
The data behind this API is *generated* from dts/<board>.dts by
tools/dts/dtc_lite.py into board/<chip>/arkrtos_dt.c.  The header
below is hand written and stable; only the data file changes when

### `typedef struct {`

```c
typedef struct {
```

**One property of a node.  Only the field for its kind is valid. */**

### `typedef struct {`

```c
typedef struct {
```

**One node of the compiled tree. */**

### `uint32 arkrtos_dt_count(void);`

```c
uint32 arkrtos_dt_count(void);
```

**Number of nodes in the compiled tree (root excluded). */**

### `const arkrtos_dt_node_t *arkrtos_dt_node(uint32 idx);`

```c
const arkrtos_dt_node_t *arkrtos_dt_node(uint32 idx);
```

**Node by index, or 0 when idx is out of range. */**

### `const arkrtos_dt_node_t *arkrtos_dt_find(const char *compatible);`

```c
const arkrtos_dt_node_t *arkrtos_dt_find(const char *compatible);
```

**First enabled node whose first compatible string matches. */**

### `const arkrtos_dt_node_t *arkrtos_dt_find_next(const char *compatible, `

```c
const arkrtos_dt_node_t *arkrtos_dt_find_next(const char *compatible, const arkrtos_dt_node_t *from);
```

**Iterate: pass the previous result (0 to start) to get the next. */**

### `const char *arkrtos_dt_model(void);`

```c
const char *arkrtos_dt_model(void);
```

**The root `model` string, or the board name when the tree has none. */**

### `const arkrtos_dt_prop_t *arkrtos_dt_prop(const arkrtos_dt_node_t *node`

```c
const arkrtos_dt_prop_t *arkrtos_dt_prop(const arkrtos_dt_node_t *node, const char *name);
```

**Property by name, or 0 when the node has no such property. */**

### `int32 arkrtos_dt_prop_u32(const arkrtos_dt_node_t *node, const char *n`

```c
int32 arkrtos_dt_prop_u32(const arkrtos_dt_node_t *node, const char *name, uint32 idx, uint32 *out);
```

**Read the idx-th u32 of a U32 property.  Returns -1 on any mismatch. */**

### `const char *arkrtos_dt_prop_str(const arkrtos_dt_node_t *node, const c`

```c
const char *arkrtos_dt_prop_str(const arkrtos_dt_node_t *node, const char *name);
```

**First string of a STR property, or 0. */**

### `int arkrtos_dt_str_eq(const char *a, const char *b);`

```c
int arkrtos_dt_str_eq(const char *a, const char *b);
```

**NULL-safe string equality; 1 when equal and both non-NULL. */**

## kernelsrc/include/arkrtos_elf.h

**从 VFS 里的 ELF64 可执行文件直接起一个隔离任务（M1：静态映射档）**
这一层回答的是「文件即程序」：给定一个路径，校验它是本架构的
ELF64、把 PT_LOAD 段读进 App 窗口、按 Linux 的约定铺好用户栈
（sp 指向 argc），再以降到非特权的任务把它跑起来。

### `typedef struct {`

```c
typedef struct {
```

**一次装载的可读结果（全部是实测值，不是推算值） */**

### `int32 arkrtos_elf_exec(const char *path, int32 argc, char *argv[]);`

```c
int32 arkrtos_elf_exec(const char *path, int32 argc, char *argv[]);
```

**装载并起跑一个 ELF64 可执行文件**
- `path`：VFS 路径（如 "/bin/hello"）
- `argc`：参数个数（含 argv[0]）；0 表示自动补一个 argv[0] = path
- `argv`：参数数组；传 0 时按 argc=0 处理
**返回**：0 = 已起跑；负值 = ARKRTOS_ELF_ERR_*
> 说明：起跑后立刻返回（不等待程序退出）。要等退出由调用者自己按任务号看。

### `int32 arkrtos_elf_exec_space(const char *path, int32 argc, char *argv[`

```c
int32 arkrtos_elf_exec_space(const char *path, int32 argc, char *argv[], uint32 *p_space);
```

**装进指定地址空间之外，多给一个"落到哪个空间"的出口**
- `p_space`：可为 0；成功时写回空间号
**返回**：0 = 已起跑；ARKRTOS_ELF_ERR_BUSY = 所有空间都有活实例
> 说明：挑空间是装载器的决定，调用方只读结果。U-2（2026-11-17）起，
"同一时间只能跑一个程序"变成"每个地址空间一个"。

### `int32 arkrtos_elf_exec_replace(const char *path, int32 argc, char *arg`

```c
int32 arkrtos_elf_exec_replace(const char *path, int32 argc, char *argv[], arkrtos_elf_info_t *p_info, unsigned long *p_tp);
```

**把**当前任务自己**的镜像原地换掉（Linux execve 的内核落地）**
- `path`：VFS 路径（内核侧命名空间路径）
- `argc`：参数个数（含 argv[0]）；0 表示补成 path
- `argv`：已由调用方从**旧镜像**拷出的内核侧字符串数组
- `p_info`：成功时填装载读数（task 字段是调用者的任务号）
- `p_tp`：成功时写回新镜像的 TLS 块地址（0 = 这份镜像没有预置 TLS）
**返回**：0 / 负错误码
语义与 Linux 的 execve 对齐到「同一个任务号继续跑新镜像」这一层：
- 不起新任务、不换任务号（前后 getpid 相同）；
- 旧镜像的段被新镜像覆盖，旧代码不再可达；
- fd 表**不清空**：execve 不是「起一条新命令」，文件描述符属于
任务而不是属于镜像；
- 入口 / 栈指针 / TLS 由调用方写回当前任务的 trap 帧（port 层）。
调用者必须**先把 argv 字符串拷进内核**：它们原本躺在旧镜像的
参数区，装载一开始那块内存就归新镜像了。
成功时不返回旧镜像（Linux 约定），失败返回负错误码、旧镜像继续跑。
fork / 地址空间复制**不在**这条接口里。

### `int32 arkrtos_elf_fork(void *p_svc_ctx, int32 *p_child_task, uint32 *p`

```c
int32 arkrtos_elf_fork(void *p_svc_ctx, int32 *p_child_task, uint32 *p_child_space);
```

**复制**当前任务**，返回子任务的 id 与它落到的空间（U-19）**
- `p_svc_ctx`：当前任务这次系统调用的 trap 帧（它同时是子空间里的同一 VA）
- `p_child_task`：可为 0；成功时写回子任务号
- `p_child_space`：可为 0；成功时写回子任务绑定的空间号
**返回**：ARKRTOS_ELF_OK / ARKRTOS_ELF_ERR_BUSY（没有空空间）/
ARKRTOS_ELF_ERR_NOMEM（帧池不够或任务表满）/ ARKRTOS_ELF_ERR_PARAM
子进程拿到父进程地址空间的**逐字副本**（8 KB 对级拷贝，父空间里
已经驻留的对才复制），子任务恢复的现场就是父这次系统调用的 trap
帧，只有 a0 被改成 0 —— 于是父返回子 pid、子返回 0。
不复制的东西（与 Linux 不同，文档写死）：fd 表、管道、信号表、
堆游标都是**内核全局单份**，子看到的是同一份，不是副本。
调用者必须是降权（is_priv == 0）用户任务；内核任务没有窗口。
> 说明：只在 ARKRTOS_USE_LINUX_FORK == 1 时存在。 */

### `int32 arkrtos_elf_info(arkrtos_elf_info_t *p_out);`

```c
int32 arkrtos_elf_info(arkrtos_elf_info_t *p_out);
```

**上一次成功装载的读数（最近一次的那个空间）；没有过成功装载时返回 ARKRTOS_ELF_ERR_PARAM */**

### `int32 arkrtos_elf_info_space(uint32 space, arkrtos_elf_info_t *p_out);`

```c
int32 arkrtos_elf_info_space(uint32 space, arkrtos_elf_info_t *p_out);
```

**指定空间的那次装载读数 */**

### `int32 arkrtos_elf_running_in(uint32 space);`

```c
int32 arkrtos_elf_running_in(uint32 space);
```

**1 = 指定空间里有活的 ELF 任务，0 = 该空间空着（或没有这个空间） */**

### `int32 arkrtos_elf_running(void);`

```c
int32 arkrtos_elf_running(void);
```

**1 = 至少有一个 ELF 任务在跑，0 = 所有空间都空着 */**

### `int32 arkrtos_elf_space_count(void);`

```c
int32 arkrtos_elf_space_count(void);
```

**可用的地址空间数（不分页的构建是 1） */**

### `int32 arkrtos_elf_last_space(void);`

```c
int32 arkrtos_elf_last_space(void);
```

**最近一次成功装载落到哪个空间；-1 = 还没有过 */**

### `const char *arkrtos_elf_error_name(int32 rc);`

```c
const char *arkrtos_elf_error_name(int32 rc);
```

**返回码的短名（只用于打印，不参与判定） */**

## kernelsrc/include/arkrtos_export.h

**内核符号导出表：可下载模块在装载时按名字解析外部引用**
这张表回答的是「模块能按名字调用哪些内核函数」。
它是**白名单**，不是「所有带导出符号的内核函数」：
没进表的符号，模块里的引用会在装载阶段被拒并点出名字，

### `typedef struct {`

```c
typedef struct {
```

**表项：名字 + 地址。地址用 unsigned long，64 位端口上不截断。 */**

### `int32 arkrtos_export_lookup(const char *name, unsigned long *p_addr);`

```c
int32 arkrtos_export_lookup(const char *name, unsigned long *p_addr);
```

**按名字查一个导出符号**
- `name`：符号名（不是路径，不是 "&name"）
- `p_addr`：收地址；找到时写入
**返回**：0 = 找到；<0 = 没有这个名字

### `const arkrtos_export_t *arkrtos_export_table(uint32 *p_count);`

```c
const arkrtos_export_t *arkrtos_export_table(uint32 *p_count);
```

**取整张表（只读）**
- `p_count`：收表项个数；可为 0
**返回**：表首地址；关闭档返回 0
> 说明：`lkup` 就是照着这张表打印的，不另存一份清单。

## kernelsrc/include/arkrtos_fdt.h

**Ark RTOS flattened device tree (DTB) reader.**
The MCU tier links a const C table generated from the DTS
(arkrtos_dt.h).  The MMU tier is the other shape: a standard
flattened blob sits in the image and is parsed at boot, exactly

### `typedef struct {`

```c
typedef struct {
```

**One node, as a view into the blob (no copy, no allocation). */**

### `typedef struct {`

```c
typedef struct {
```

**Walk cursor.  Treat the fields as private. */**

### `int arkrtos_fdt_init(const void *blob, uint32 size);`

```c
int arkrtos_fdt_init(const void *blob, uint32 size);
```

**Parse and validate the header; compute the node count. */**

### `int arkrtos_fdt_ready(void);`

```c
int arkrtos_fdt_ready(void);
```

**1 when the last init succeeded. */**

### `uint32 arkrtos_fdt_node_count(void);`

```c
uint32 arkrtos_fdt_node_count(void);
```

**Number of nodes, root excluded (matches arkrtos_dt_count()). */**

### `const char *arkrtos_fdt_model(void);`

```c
const char *arkrtos_fdt_model(void);
```

**The root "model" string, or 0 when the DTS has none. */**

### `int arkrtos_fdt_iter_init(arkrtos_fdt_iter_t *it);`

```c
int arkrtos_fdt_iter_init(arkrtos_fdt_iter_t *it);
```

**Start a depth-first walk.  Returns ARKRTOS_FDT_ERR_NOT_INIT if not ready. */**

### `int arkrtos_fdt_next(arkrtos_fdt_iter_t *it, arkrtos_fdt_node_t *out);`

```c
int arkrtos_fdt_next(arkrtos_fdt_iter_t *it, arkrtos_fdt_node_t *out);
```

**Next node in depth-first order.**
**返回**：1 and fill *out for a node, 0 at the end of the tree,
a negative ARKRTOS_FDT_ERR_* for a malformed blob.

### `const void *arkrtos_fdt_prop(const arkrtos_fdt_node_t *node, const cha`

```c
const void *arkrtos_fdt_prop(const arkrtos_fdt_node_t *node, const char *name, uint32 *len);
```

**Property value inside the blob.**
- `len`：Receives the byte length (may be 0 for a valueless property).
**返回**：The value pointer, or 0 when the node has no such property.

### `const char *arkrtos_fdt_prop_str(const arkrtos_fdt_node_t *node, const`

```c
const char *arkrtos_fdt_prop_str(const arkrtos_fdt_node_t *node, const char *name);
```

**First string of a string-list property, or 0. */**

### `int32 arkrtos_fdt_prop_u32(const arkrtos_fdt_node_t *node, const char `

```c
int32 arkrtos_fdt_prop_u32(const arkrtos_fdt_node_t *node, const char *name, uint32 idx, uint32 *out);
```

**The idx-th 32-bit cell of a property.  Returns -1 on any mismatch. */**

### `int arkrtos_fdt_node_enabled(const arkrtos_fdt_node_t *node);`

```c
int arkrtos_fdt_node_enabled(const arkrtos_fdt_node_t *node);
```

**1 when status is absent or "okay" - same rule as the C table. */**

### `const void *arkrtos_fdt_blob(void);`

```c
const void *arkrtos_fdt_blob(void);
```

**当前正在被解析的 blob 与它的字节数（未 init 时返回 0）。 */**

### `int arkrtos_fdt_load(void *dst, uint32 cap, const void *src, uint32 le`

```c
int arkrtos_fdt_load(void *dst, uint32 cap, const void *src, uint32 len);
```

**把 blob 复制进调用方 RAM 再解析——动态装载的落地形态。**
- `dst`：目标缓冲区（4 字节对齐，容量 >= len）
- `cap`：目标缓冲区字节数；len > cap 直接拒绝，不是截断
失败时**保持上一棵树可用**：解析是事务性的，坏 blob 只换来一个
错误码，不会让这台机器"突然一个设备都没有"。
**返回**：ARKRTOS_FDT_OK 或负的错误码（ERR_ARG 含容量不足）

### `int arkrtos_fdt_node_by_path(const char *path, arkrtos_fdt_node_t *out`

```c
int arkrtos_fdt_node_by_path(const char *path, arkrtos_fdt_node_t *out);
```

**按绝对路径取节点（"/soc/serial@1fe001e0"；"/" 是根节点）。 */**

### `int arkrtos_fdt_alias(const char *name, char *buf, uint32 cap);`

```c
int arkrtos_fdt_alias(const char *name, char *buf, uint32 cap);
```

**/aliases/<name> 指向的路径，拷进 buf（含 NUL）。 */**

### `const char *arkrtos_fdt_stdout_path(void);`

```c
const char *arkrtos_fdt_stdout_path(void);
```

**/chosen/stdout-path 的原始字符串；没有这个节点或属性就是 0。 */**

### `const void *arkrtos_fdt_stdout_raw(uint32 *len);`

```c
const void *arkrtos_fdt_stdout_raw(uint32 *len);
```

**/chosen/stdout-path 的原始字节与长度（可能是字符串，也可能是句柄）。 */**

### `int arkrtos_fdt_node_by_phandle(uint32 ph, arkrtos_fdt_node_t *out);`

```c
int arkrtos_fdt_node_by_phandle(uint32 ph, arkrtos_fdt_node_t *out);
```

**按 phandle 取节点（phandle / linux,phandle 两种属性都认）。 */**

### `int arkrtos_fdt_path_of_phandle(uint32 ph, char *buf, uint32 cap);`

```c
int arkrtos_fdt_path_of_phandle(uint32 ph, char *buf, uint32 cap);
```

**把一个 phandle 还原成绝对路径，拷进 buf（含 NUL）。 */**

### `typedef struct {`

```c
typedef struct {
```

**stdout-path 的解析结果（路径 + 选项）。 */**

### `int arkrtos_fdt_stdout_resolve(arkrtos_fdt_console_t *out);`

```c
int arkrtos_fdt_stdout_resolve(arkrtos_fdt_console_t *out);
```

**解析 /chosen/stdout-path（"serial0:115200n8" 或绝对路径形式）。**
**返回**：OK；ERR_NOT_INIT / ERR_NOT_FOUND（别名/路径不存在）/
ERR_PATH（选项写法不认识——不猜一个默认波特率）

## kernelsrc/include/arkrtos_fdt_console.h

**把 /chosen/stdout-path 变成"控制台绑到哪个设备"的判断**
设备树里说"控制台是这个设备"的字段是 /chosen/stdout-path。读了
它不等于消费它：消费的意思是拿它去决定控制台绑到哪儿，并且在不
一致时**明确拒绝**——把 DTS 与板级常量对不上的事静默吞掉，等于

### `typedef struct {`

```c
typedef struct {
```

**板级期望：控制台应该是什么。字段为 0/NULL 表示"这一项不检查"。 */**

### `typedef struct {`

```c
typedef struct {
```

**树里读出来、并与板级核对过的结果。 */**

### `int arkrtos_console_from_fdt(const arkrtos_console_expect_t *expect, a`

```c
int arkrtos_console_from_fdt(const arkrtos_console_expect_t *expect, arkrtos_console_bind_t *out);
```

**解析 /chosen/stdout-path 并与板级期望核对。**
- `expect`：期望（可为 0，表示只解析不核对）
- `out`：解析结果，失败时也尽量填（便于把"读到了什么"打出来）
**返回**：ARKRTOS_CONSOLE_OK 或上面某个负码
> 说明：只读：不改变控制台，也不碰硬件。

### `const char *arkrtos_console_reason(int rc);`

```c
const char *arkrtos_console_reason(int rc);
```

**原因码的稳定短名（用于日志），未知码返回 "?"。 */**

## kernelsrc/include/arkrtos_fdt_overlay.h

**最小子集的 dtb overlay：把一段 fragment 合并进一棵树，产出新的 blob**
设备树的 overlay 在 Linux 上是一族语法：target-path / target 句柄、
__overlay__ 容器、__fixups__ / __symbols__ 两段标签解析、加上
"删属性/删节点"。**这里只做其中一支，并且明确拒绝其余部分**：

### `int arkrtos_fdt_overlay(void *dst, uint32 cap, const void *base, uint3`

```c
int arkrtos_fdt_overlay(void *dst, uint32 cap, const void *base, uint32 base_len, const void *frag, uint32 frag_len, uint32 *out_len);
```

**把 fragment 合并进 base，写出一个新的 flattened blob。**
- `dst`：输出缓冲区（4 字节对齐，容量 cap）
- `cap`：输出缓冲区字节数
- `base`：基树 blob（含 magic 的完整 dtb）
- `base_len`：基树字节数
- `frag`：fragment blob（同上）
- `frag_len`：fragment 字节数
- `out_len`：成功时写入新 blob 的字节数（含 header）
**返回**：ARKRTOS_FDT_OK / -1..-10（基树或 fragment 本身不合法）/
-11..-15（overlay 语义不支持，见上面的错误码）

### `const char *arkrtos_fdt_overlay_reason(int rc);`

```c
const char *arkrtos_fdt_overlay_reason(int rc);
```

**错误码的文字解释（只用于日志，不参与判定）。 */**

## kernelsrc/include/arkrtos_fdt_res.h

**把设备树当**驱动配置源**用：定时器与计数时基的口径核对**
这一层回答的问题是「内核按什么频率在跑定时器」，而这个数字有两个
可能的来源：
1) 板级常量（编译进 port 的 ARKRTOS_PORT_TIMER_HZ 等）；

### `typedef struct {`

```c
typedef struct {
```

**板级期望。字段为 0 表示"这一项不检查"。 */**

### `typedef struct {`

```c
typedef struct {
```

**树里读出来的资源。has_* 区分「没写」与「值为 0」。 */**

### `int arkrtos_fdt_res_check(const arkrtos_fdt_res_expect_t *expect, arkr`

```c
int arkrtos_fdt_res_check(const arkrtos_fdt_res_expect_t *expect, arkrtos_fdt_res_t *out);
```

**读取并核对定时器 / 时基资源。**
- `expect`：期望（不可为 0；字段置 0 表示不检查那一项）
- `out`：读到的值（失败时也尽量填满，便于打印）
**返回**：ARKRTOS_RES_OK 或上面某个负码
> 说明：只读。

### `const char *arkrtos_fdt_res_reason(int rc);`

```c
const char *arkrtos_fdt_res_reason(int rc);
```

**原因码的稳定短名（用于日志），未知码返回 "?"。 */**

## kernelsrc/include/arkrtos_flashapp.h

**阶段 3c：把 arkc 产物烧进**片内 Flash**，并由内核引导器跳转执行**
与 app run（L1 固定窗口，arkrtos_apprun.h）的差别只有一句话：
- app run 跑的是**读进 RAM 的文件**：代码从 /tmp 拷到 8 KB 代码窗，
掉电即失，每次跑都要重新读一遍。

### `int32 arkrtos_flashapp_write(const char *path, uint32 *p_len);`

```c
int32 arkrtos_flashapp_write(const char *path, uint32 *p_len);
```

**把文件系统里的一个 arkc 裸产物烧进片内 Flash 应用区**
- `path`：卷内绝对路径，如 "/tmp/f1.bin"
- `p_len`：非空时写回本次写入的负载长度（字节）
**返回**：0=已写入并校验通过；负值为 ARKRTOS_FLASHAPP_ERR_x /
ARKRTOS_LOADER_ERR_x / ARKRTOS_MINI_ERR_FS
顺序是「先整份读一遍验形状（长度/向量表/CRC）→ 擦整个区 →
分块读+写 → 写元数据 → 回读逐字节比对」。
先验后擦：一份明显不对的文件不该把已经能跑的那份擦掉。
**不回滚**：擦除一旦开始，失败就是「区里现在是空的」——这一点
在 shell 输出里明说，不做「假装失败没发生」的补偿。

### `int32 arkrtos_flashapp_erase(void);`

```c
int32 arkrtos_flashapp_erase(void);
```

**擦掉 Flash 应用区（连元数据一起）**
**返回**：0=成功；ARKRTOS_FLASHAPP_ERR_FLASH=底层失败

### `int32 arkrtos_flashapp_info(uint32 *p_len, uint32 *p_crc, uint32 *p_en`

```c
int32 arkrtos_flashapp_info(uint32 *p_len, uint32 *p_crc, uint32 *p_entry);
```

**查 Flash 应用区里有没有一份有效产物**
- `p_len`：非空时写回负载长度
- `p_crc`：非空时写回负载 CRC32
- `p_entry`：非空时写回入口地址（向量表 +4，已带 Thumb 位）
**返回**：0=有效；ARKRTOS_FLASHAPP_ERR_META=没有（未烧 / 已擦 / 元数据坏）
只读元数据与向量表，不校验 CRC（CRC 要整份扫一遍，那是 write 的事）。
「有效」的定义是三条：元数据魔数+版本对、长度在 [8, MAX]、向量表
SP 等于 RAM 窗口顶。入口是否在区内由调用方（run）再核一次。

### `int32 arkrtos_flashapp_run(uint32 *task_out);`

```c
int32 arkrtos_flashapp_run(uint32 *task_out);
```

**把 Flash 里的产物跳起来（起任务，从 Flash 直接取指）**
- `task_out`：非空时写回本次起跑的任务号（必须在 activate 之前带出去）
**返回**：0=已启动；负值为 ARKRTOS_FLASHAPP_ERR_x / ARKRTOS_LOADER_ERR_x
顺序与 app run 一致：判并发 → 借窗 → 校验元数据与向量表 →
起任务（非特权、MPU 两窗口）→ 激活。
代码窗口记成 Flash 应用区（只读可执行），RAM 窗口记成借到的
数据块；任务退出时由 arkrtos_flashapp_on_task_exit() 还窗。

### `uint32 arkrtos_flashapp_task_id(void);`

```c
uint32 arkrtos_flashapp_task_id(void);
```

**当前在跑的 Flash 产物任务号；0 = 没有 */**

### `int32 arkrtos_flashapp_on_task_exit(uint32 task_id);`

```c
int32 arkrtos_flashapp_on_task_exit(uint32 task_id);
```

**主任务退出时的回收钩子（由 arkrtos_task.c 的任务退出路径调用） */**

### `const char *arkrtos_flashapp_error_name(int32 rc);`

```c
const char *arkrtos_flashapp_error_name(int32 rc);
```

**错误码的可读名字（只覆盖本路径独有的，其余交调用方） */**

## kernelsrc/include/arkrtos_fs.h

**Ark RTOS file system facade - littlefs over a registered block device**
One volume at a time, mounted explicitly. That is a deliberate
limit, not an oversight: the kernel has one caller (the shell today,
an SVC handler later) and a single mount point keeps the RAM cost

### `#define ARKRTOS_FS_PATH_MAX   (64u)`

```c
#define ARKRTOS_FS_PATH_MAX   (64u)
```

Longest path accepted, including the leading '/' and the terminator */

### `#define ARKRTOS_FS_CACHE_SIZE (256u)`

```c
#define ARKRTOS_FS_CACHE_SIZE (256u)
```

Bytes buffered for metadata/reads and for one open file */

### `int32 arkrtos_fs_mount(const char *dev, uint32 offset, uint32 size);`

```c
int32 arkrtos_fs_mount(const char *dev, uint32 offset, uint32 size);
```

**Mount the volume described by (dev, offset, size).**
- `dev`：name of a registered block device, e.g. "nor0"
- `offset`：byte offset of the volume inside that device
- `size`：volume length in bytes; 0 = to the end of the device
**返回**：0 on success, -1 on failure (not mounted, nothing in flash, or a
block device that does not exist / is not up)
> 说明：Mounting does NOT format. An unformatted volume fails here, which is
the honest answer - use arkrtos_fs_format() first, on purpose.

### `int32 arkrtos_fs_mount_default(void);`

```c
int32 arkrtos_fs_mount_default(void);
```

**Mount the board's default volume (see ARKRTOS_FS_* in the partition**
header). Returns -1 with last_error = -2 when no volume is configured.

### `int32 arkrtos_fs_unmount(void);`

```c
int32 arkrtos_fs_unmount(void);
```

**Drop the mount. Safe to call when nothing is mounted. */**

### `uint8 arkrtos_fs_mounted(void);`

```c
uint8 arkrtos_fs_mounted(void);
```

**1 while a volume is mounted */**

### `int32 arkrtos_fs_format(const char *dev, uint32 offset, uint32 size);`

```c
int32 arkrtos_fs_format(const char *dev, uint32 offset, uint32 size);
```

**Erase and create an empty file system on the volume.**
> 说明：Destructive: everything on the volume is lost. Works on a volume that
is not mounted yet, and mounts it on success.

### `int32 arkrtos_fs_stat(uint32 *total, uint32 *used);`

```c
int32 arkrtos_fs_stat(uint32 *total, uint32 *used);
```

**Total and used bytes of the mounted volume (used = blocks in use) */**

### `int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_`

```c
int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_dir);
```

**Size and type of one entry (file or directory).**
- `path`：absolute path inside the mounted volume
- `size`：receives the size in bytes, 0 for a directory (may be NULL)
- `is_dir`：receives 1 for a directory, 0 for a file (may be NULL)
**返回**：0 on success, -1 on failure (nothing mounted, bad path, not found)

### `int32 arkrtos_fs_truncate(const char *path, uint32 size);`

```c
int32 arkrtos_fs_truncate(const char *path, uint32 size);
```

**Set the length of one file, leaving its content alone up to @p size.**
Shrinking drops the tail, growing fills the new bytes with zeros
(POSIX truncate). The file must already exist: this call never
creates one. It shares the single file cache with the streaming
entry points and refuses with ARKRTOS_FS_ERR_BUSY while a stream is
open, because resizing underneath an open stream is exactly the
half-true state this facade does not hand out.
**返回**：0 on success, -1 on failure (nothing mounted, bad path, not found,
or a stream already open); arkrtos_fs_last_error() has the reason.

### `int32 arkrtos_fs_read_file(const char *path, uint8 *buf, uint32 max, u`

```c
int32 arkrtos_fs_read_file(const char *path, uint8 *buf, uint32 max, uint32 *out_len);
```

**Read at most max bytes; *out_len gets the real length */**

### `int32 arkrtos_fs_open_read(const char *path);`

```c
int32 arkrtos_fs_open_read(const char *path);
```

**Open one file for chunked (streaming) reading.**
A 20 KiB image does not fit a fixed scratch buffer, so the file
stays open across calls and the caller drives it. While a stream
is open, the one-shot entry points that share the single file cache
(arkrtos_fs_read_file / arkrtos_fs_write_file) refuse to run instead of
silently corrupting whichever is in flight - that refusal is
ARKRTOS_FS_ERR_BUSY.
**返回**：0 on success, -1 on failure (nothing mounted, bad path, not found,
a directory, or another stream already open)

### `int32 arkrtos_fs_read_next(uint8 *buf, uint32 max);`

```c
int32 arkrtos_fs_read_next(uint8 *buf, uint32 max);
```

**Read the next chunk of the open read stream.**
- `buf`：destination, at least max bytes
- `max`：largest chunk to ask for
**返回**：bytes read (1..max), 0 at end of file, -1 on failure
> 说明：0 means "the file ended", never "nothing right now": the call is
synchronous, so a caller loop may stop on it.

### `int32 arkrtos_fs_close_read(void);`

```c
int32 arkrtos_fs_close_read(void);
```

**Close the read stream. Closing when none is open is not an error. */**

### `int32 arkrtos_fs_open_write(const char *path);`

```c
int32 arkrtos_fs_open_write(const char *path);
```

**Open one file for chunked writing (creates or truncates).**
**返回**：0 on success, -1 on failure; the rules of arkrtos_fs_open_read apply

### `int32 arkrtos_fs_write_next(const uint8 *buf, uint32 len);`

```c
int32 arkrtos_fs_write_next(const uint8 *buf, uint32 len);
```

**Append one chunk to the open write stream. @return 0 on success */**

### `int32 arkrtos_fs_close_write(void);`

```c
int32 arkrtos_fs_close_write(void);
```

**Close the write stream, which is what commits the file.**
**返回**：0 on success, -1 on failure (littlefs reports the reason); closing
when none is open is not an error

### `int32 arkrtos_fs_remove(const char *path);`

```c
int32 arkrtos_fs_remove(const char *path);
```

**Remove a file. @return 0 on success, -1 (not found / is a dir) */**

### `int32 arkrtos_fs_mkdir(const char *path);`

```c
int32 arkrtos_fs_mkdir(const char *path);
```

**Create a directory. @return 0 on success, -1 (exists / bad path) */**

### `typedef int (*arkrtos_fs_list_cb_t)(const char *name, uint32 size, uin`

```c
typedef int (*arkrtos_fs_list_cb_t)(const char *name, uint32 size, uint8 is_dir, void *arg);
```

**Callback for arkrtos_fs_list(); returns 0 to continue, -1 to stop */**

### `int32 arkrtos_fs_list(const char *dir, arkrtos_fs_list_cb_t cb, void *`

```c
int32 arkrtos_fs_list(const char *dir, arkrtos_fs_list_cb_t cb, void *arg);
```

**List one directory ("/" for the root) */**

### `int32 arkrtos_fs_read_at(const char *path, uint8 *buf, uint32 len, uin`

```c
int32 arkrtos_fs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off, uint32 *out_len);
```

**Read len bytes starting at byte offset off (random access read).**
A partial read at end of file returns fewer bytes than len, with that
number in *out_len - never an error. */

### `int32 arkrtos_fs_rename(const char *old_path, const char *new_path);`

```c
int32 arkrtos_fs_rename(const char *old_path, const char *new_path);
```

**Move/rename one entry. Both paths are inside the volume. */**

### `int32 arkrtos_fs_list_names(const char *dir, char *out, uint32 out_siz`

```c
int32 arkrtos_fs_list_names(const char *dir, char *out, uint32 out_size, uint32 *count);
```

**Copy the names of one directory into a caller buffer, one per line.**
Only complete entries are copied: when the buffer runs out, the last
partial name is dropped and *count counts what actually fits, so the
caller can tell a full listing from a truncated one. */

### `int32 arkrtos_fs_volume(const char **dev, uint32 *offset, uint32 *size`

```c
int32 arkrtos_fs_volume(const char **dev, uint32 *offset, uint32 *size);
```

**Geometry of the volume that is mounted, so a caller can check a**
request against it instead of silently reusing whatever is up.
- `dev/offset/size`：receive the values the mount actually used; a
requested size of 0 is already resolved to "to the end of the device".
**返回**：0 while a volume is mounted, -1 when there is nothing to report.

### `uint8 arkrtos_fs_last_is_noent(void);`

```c
uint8 arkrtos_fs_last_is_noent(void);
```

**1 = the last call failed only because the entry does not exist**
"No such entry" is a normal answer, not a fault: a caller that has
to choose between opening and creating (the VFS bridge does) must
be able to tell it apart from a real I/O failure.  It is a separate
predicate instead of "stat failures leave last_error alone",
because clearing the recorded cause would erase the one number an
operator is told to look at.
**返回**：1 when arkrtos_fs_last_error() is LFS_ERR_NOENT, 0 otherwise.

### `uint8 arkrtos_fs_last_is_nospc(void);`

```c
uint8 arkrtos_fs_last_is_nospc(void);
```

**1 = the last call failed only because the backend is out of space**
Resizing a file is the one call where "too big" is a normal answer
a caller can act on (it may retry smaller). littlefs reports it as
LFS_ERR_NOSPC, and this predicate is the single place that number
is turned into a decision, so no other file has to know it.
**返回**：1 when arkrtos_fs_last_error() is LFS_ERR_NOSPC, 0 otherwise.

### `uint8 arkrtos_fs_err_is_unformatted(int32 lfs_err);`

```c
uint8 arkrtos_fs_err_is_unformatted(int32 lfs_err);
```

**1 = this error number means "there is no littlefs volume here"**
A mount over blank NOR, over another file system, or over a volume
whose superblock is damaged returns LFS_ERR_CORRUPT (or INVAL) -
littlefs never formats on its own, and neither does this layer.
The predicate exists so a caller can print the *next step*
("format it on purpose") instead of guessing a cause from the
number.  It is a diagnosis of the failure, never a licence to
write: nothing in this file formats except arkrtos_fs_format().
**返回**：1 when lfs_err is LFS_ERR_CORRUPT or LFS_ERR_INVAL, 0 otherwise.

### `const char *arkrtos_fs_error_name(int32 lfs_err);`

```c
const char *arkrtos_fs_error_name(int32 lfs_err);
```

**Human readable name of an error code, for reports.**
Callers must print the number the file system returned, not a
guessed cause; this only adds the short symbol on top of it.

## kernelsrc/include/arkrtos_guard.h

**Watchdog gating and the per-slot heartbeat contract**
Two different jobs share one file because they are one decision:
"is the system still making the progress we expect, and if not,
should the hardware put an end to it?"

### `#define ARKRTOS_GUARD_OK          (0u) #define ARKRTOS_GUARD_BAD_AUDIT`

```c
#define ARKRTOS_GUARD_OK          (0u) #define ARKRTOS_GUARD_BAD_AUDIT   (1u << 0)   /* a structure failed the self-audit */ #define ARKRTOS_GUARD_BAD_BEAT    (1u << 1)   /* a declared heartbeat window lapsed */
```

**Verdict bits (ARKRTOS_GUARD_OK means "nothing known to be wrong") */**

### `#define ARKRTOS_GUARD_MIN_PERIOD_MS   (100u)`

```c
#define ARKRTOS_GUARD_MIN_PERIOD_MS   (100u)
```

**Shortest contract the kernel can actually check (equal to the**
service period: a window shorter than the sampling interval could
not be judged, and pretending to judge it would be a false verdict). */

### `typedef struct {`

```c
typedef struct {
```

**One entry of the per-task heartbeat table, as reported to callers */**

### `typedef struct {`

```c
typedef struct {
```

**Module-level status, for the shell and for a boot report */**

### `void arkrtos_guard_init(void);`

```c
void arkrtos_guard_init(void);
```

**Bring up the watchdog (when the board asks for it) and clear the**
heartbeat table. Called once, after the partition table is populated.

### `void arkrtos_guard_tick(void);`

```c
void arkrtos_guard_tick(void);
```

**Per-tick bookkeeping: records the running task, and every service**
period re-evaluates the contract and decides whether to feed.
> 说明：Called from the tick handler; does no I/O and no allocation.

### `void arkrtos_guard_svc(int32 task_id);`

```c
void arkrtos_guard_svc(int32 task_id);
```

**Record free evidence: this task called into the kernel.**
- `task_id`：1-based task id; values <= 0 (idle/kernel) are ignored.

### `int32 arkrtos_guard_beat(int32 task_id, uint32 period_ms);`

```c
int32 arkrtos_guard_beat(int32 task_id, uint32 period_ms);
```

**Record an explicit heartbeat, and optionally declare/replace the**
period the caller promises to keep.
- `task_id`：1-based task id; <= 0 is rejected.
- `period_ms`：0 = withdraw the contract; otherwise >= ARKRTOS_GUARD_MIN_PERIOD_MS.
**返回**：0 on success, -1 when the task id or the period cannot be accepted.

### `void arkrtos_guard_status(arkrtos_guard_status_t *p_out);`

```c
void arkrtos_guard_status(arkrtos_guard_status_t *p_out);
```

**Fill in the module status.**

### `int32 arkrtos_guard_task_at(uint32 idx, arkrtos_guard_task_t *p_out);`

```c
int32 arkrtos_guard_task_at(uint32 idx, arkrtos_guard_task_t *p_out);
```

**Walk the heartbeat table.**
- `idx`：0-based table index.
- `p_out`：Receives the entry; task_id is 0 once the table is exhausted.
**返回**：0 on success, -1 when @p idx is past the end.

### `uint32 arkrtos_guard_task_slots(void);`

```c
uint32 arkrtos_guard_task_slots(void);
```

**Number of entries the walk in arkrtos_guard_task_at() covers. */**

## kernelsrc/include/arkrtos_hal.h

**Ark RTOS 硬件抽象层接口**
定义内核与硬件的完整解耦接口，分为四个层次：
1) CPU指令层 - CPU核心指令封装（WFI/ISB/DSB等）
2) 上下文层 - 任务上下文结构与栈帧初始化

### `void arkrtos_port_resume_task(unsigned long stack_ptr);`

```c
void arkrtos_port_resume_task(unsigned long stack_ptr);
```

**在异常处理程序内部直接恢复目标任务上下文并异常返回**
- `stack_ptr`：目标任务栈指针（指向其保存的 R4-R11[/EXC_RETURN]）
故障恢复等“已经处于异常处理程序中、不能依赖 PendSV”的场景专用：
Cortex-M 上 HardFault 优先级高于 PendSV，故障处理程序活动期间
PendSV 不会被服务，必须由本函数直接完成恢复 + 异常返回。
本函数不返回；stack_ptr 为 0 时调用方应自行停机，不要调用。

### `unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx);`

```c
unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx);
```

**读取系统调用上下文中的第 idx 个参数（0~3）**
- `p_exc_ctx`：系统调用入口传给内核的上下文指针
- `idx`：参数序号（0 对应 a0/r0）
**返回**：参数值
各架构的栈帧/trap 帧布局不同，由 port 层负责解析，
内核只通过本接口与上面的宏访问参数，不感知任何寄存器布局。

### `void   arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value);`

```c
void   arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value);
```

**写回系统调用返回值（第 0 个参数寄存器）**
- `p_exc_ctx`：系统调用上下文指针
- `value`：返回值

### `uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx);`

```c
uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx);
```

**本次异常是否是 Linux ABI 系统调用，是则返回它的系统调用号**
- `p_exc_ctx`：系统调用入口传给内核的上下文指针
**返回**：Linux 系统调用号；不是 Linux ABI 调用时返回 0xFFFFFFFF
两个 ABI 在同一颗核上用同一条陷入指令，只有号的位置不同：
- LoongArch：Ark 的号在 `syscall` 立即数里（非 0），Linux 用
`syscall 0` + $a7，因此「立即数为 0」是分界；
- RISC-V：两边都经 a7，只能按号段让位（0x10..0x22 归 Ark）。
- Cortex-M / LA32：恒返回 0xFFFFFFFF（没有 Linux ABI 载体）。
由 port 层判定，内核不感知任何寄存器布局。

### `void   arkrtos_port_svc_set_ret_signed(void *p_exc_ctx, int32 value);`

```c
void   arkrtos_port_svc_set_ret_signed(void *p_exc_ctx, int32 value);
```

**按 Linux 约定写回返回值（第 0 个参数寄存器，带符号扩展）**
- `p_exc_ctx`：系统调用上下文指针
- `value`：返回值；负值表示 -errno
与 arkrtos_port_svc_set_ret 的区别只有符号扩展：Linux 的错误
是负数，64 位下若只写低 32 位，程序会把 0x00000000FFFFFFEA
读成一个很大的正数。宁可让返回值正确，也不给看似成功的错答案。

### `unsigned long arkrtos_port_svc_user_reg(void *p_exc_ctx, uint32 id);`

```c
unsigned long arkrtos_port_svc_user_reg(void *p_exc_ctx, uint32 id);
```

**读取用户态 trap 帧里的一个寄存器（信号帧保存/恢复用）**
- `p_exc_ctx`：系统调用入口传给内核的上下文指针
- `id`：寄存器编号：0 = 返回地址（era / pc）、1 = 栈指针、
2 = 链接寄存器（ra / lr）、3 = 线程指针（tp）
**返回**：寄存器当前值；框架不认识该编号时返回 0
与 ARKRTOS_SVC_ARG 同一口径：寄存器布局只在 port 层解析，
内核只按语义编号取值。信号投递要在用户栈上铺一帧、把
「被打断的位置」原样存下来，这一对原语就是那条缝。

### `int32 arkrtos_port_svc_set_user_reg(void *p_exc_ctx, uint32 id, unsign`

```c
int32 arkrtos_port_svc_set_user_reg(void *p_exc_ctx, uint32 id, unsigned long value);
```

**写回用户态 trap 帧里的一个寄存器（信号投递/返回用）**
- `p_exc_ctx`：系统调用上下文指针
- `id`：寄存器编号，同 arkrtos_port_svc_user_reg
- `value`：新值
**返回**：0 = 已写入；-1 = 编号不可用或端口没有这个能力（一个字节都不动）
端口没有该能力时由 arkrtos_linux_abi.c 里的弱定义顶住，
信号相关的号一律回 -ENOSYS，不假装投递成功。

### `uint32 arkrtos_port_svc_frame_bytes(void);`

```c
uint32 arkrtos_port_svc_frame_bytes(void);
```

**trap 帧在内存里的字节数（信号快照用）**
**返回**：帧长；含 FPU 时是 528，LSX 档 1040，默认档 256
信号要「原样保存被打断的现场」，而现场不止四个寄存器：把
帧整块存进用户栈，返回时整块装回去，才不会被处理函数打断
一半的调用约定（$t0 一类的临时寄存器）害死。

### `unsigned long arkrtos_port_svc_frame_reserve(void *p_exc_ctx, uint32 b`

```c
unsigned long arkrtos_port_svc_frame_reserve(void *p_exc_ctx, uint32 bytes);
```

**在用户栈上给信号帧找一块**不和 trap 帧重叠**的地方**
- `p_exc_ctx`：系统调用上下文指针
- `bytes`：这一帧要多少字节（头 + 快照）
**返回**：帧基址（16 字节对齐）；栈不够放时返回 0
内核的 trap 帧就铺在用户 sp 下面 LA_FR_SIZE 字节处，这一块
是内核的、不是用户栈的「空闲区」。信号帧若从 sp 直接往下放
就会整块盖在 trap 帧上——实测症状是返回时 frame 里的 $t5 变成
了信号 magic，随后一条 ret 跳到 0x210（= 帧长 528）。所以
让端口算这个地址：帧长、对齐、保留区都只在端口里出现一次。

### `void arkrtos_port_svc_frame_export(void *p_exc_ctx, void *p_dst);`

```c
void arkrtos_port_svc_frame_export(void *p_exc_ctx, void *p_dst);
```

**把整份 trap 帧拷进内核缓冲（信号投递用）**
- `p_exc_ctx`：系统调用上下文指针
- `p_dst`：目标缓冲，至少 arkrtos_port_svc_frame_bytes() 字节
只做整块拷贝，不改帧。

### `int32 arkrtos_port_svc_frame_import(void *p_exc_ctx, const void *p_src`

```c
int32 arkrtos_port_svc_frame_import(void *p_exc_ctx, const void *p_src);
```

**把内核缓冲里的帧整块装回当前上下文（rt_sigreturn 用）**
- `p_exc_ctx`：系统调用上下文指针
- `p_src`：源缓冲，至少 arkrtos_port_svc_frame_bytes() 字节
**返回**：0 = 已装回；-1 = 端口没有这个能力
唯一被丢弃的字段是 PRMD 的特权级两位：特权级由当前上下文
决定，用户写在栈上的快照不许把它抬高（否则一个用户程序可以
自己铺一帧 PRMD=PLV0 再 sigreturn）。

### `int32  arkrtos_port_svc_exec_frame(void *p_exc_ctx, unsigned long entr`

```c
int32  arkrtos_port_svc_exec_frame(void *p_exc_ctx, unsigned long entry, unsigned long sp, unsigned long tp);
```

**把当前任务的 trap 帧改写成「新镜像的第一条指令」（execve 的落地）**
- `p_exc_ctx`：当前系统调用的上下文指针
- `entry`：新入口（必须在 App 窗口内、4 字节对齐）
- `sp`：新用户栈指针（指向 argc）
- `tp`：新镜像的 TLS 块地址（0 = 这份镜像没有预置 TLS）
**返回**：0 = 帧已改写；-1 = 参数不可用（此时一个字节都不动）
这张帧就是 ertn 要恢复的那一张：改完之后内核直接返回用户态，
**不再回到旧镜像**。通用寄存器一律清零（Linux 在 execve 之后
不保留任何寄存器），PRMD 只留 PPLV、把 PIE 置 1。
port 没有「同任务换镜像」能力时（目前 RISC-V / Cortex-M / LA32），
由 arkrtos_linux_abi.c 里的弱定义顶住，execve 回 -ENOSYS。

### `uint32 arkrtos_port_enter_critical(void);`

```c
uint32 arkrtos_port_enter_critical(void);
```

**进入临界区（保存中断使能状态并关中断）**
**返回**：进入前的中断状态，必须原样传给 arkrtos_port_exit_critical
与 ARKRTOS_DISABLE_IRQ 的区别：本接口保存并恢复状态，
支持嵌套，且适配 RISC-V/LoongArch 等需要保存状态寄存器的架构。
新代码建议优先使用本接口。

### `void   arkrtos_port_exit_critical(uint32 state);`

```c
void   arkrtos_port_exit_critical(uint32 state);
```

**退出临界区（恢复此前保存的中断状态）**
- `state`：arkrtos_port_enter_critical 的返回值

### `uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,`

```c
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value);
```

**比较并交换（原子操作）**
- `p_addr`：目标地址（必须 4 字节对齐）
- `expect`：期望的当前值
- `new_value`：期望成立时写入的新值
**返回**：1=交换成功，0=当前值与 expect 不符（未修改）

### `uint32 arkrtos_port_cpu_id(void);`

```c
uint32 arkrtos_port_cpu_id(void);
```

**获取当前 CPU 编号（单核恒返回 0，多核返回硬件核号）**
**返回**：CPU 编号

### `void   arkrtos_port_spin_hint(void);`

```c
void   arkrtos_port_spin_hint(void);
```

**自旋等待提示（降低自旋总线压力，可插入 NOP/WFE/PAUSE）**

### `int32  arkrtos_port_cpu_start(uint32 cpu);`

```c
int32  arkrtos_port_cpu_start(uint32 cpu);
```

**Start a secondary CPU (SMP scaffolding only).**
- `cpu`：hardware core id, 1..ARKRTOS_CPU_NUM-1
**返回**：0 = start request accepted; -1 = this port has no secondary-core path
> 说明：A single-core port returns -1 rather than pretending the core is up.
The started core must enter arkrtos_smp_secondary_main(cpu) as its
first C code, with its stack already in place.

### `int32  arkrtos_port_send_ipi(uint32 cpu);`

```c
int32  arkrtos_port_send_ipi(uint32 cpu);
```

**Send an inter-processor interrupt (SMP scaffolding only).**
- `cpu`：target core id, already checked online by the caller
**返回**：0 = sent; -1 = this port has no IPI path

### `void arkrtos_port_set_psp(uint32 val);`

```c
void arkrtos_port_set_psp(uint32 val);
```

**设置进程栈指针PSP**
- `val`：PSP值

### `int32 arkrtos_port_swap_frame_ok(const void *p_exc_ctx);`

```c
int32 arkrtos_port_swap_frame_ok(const void *p_exc_ctx);
```

**判断当前异常帧是不是本架构认可的「可改写帧」**
- `p_exc_ctx`：SVC/trap 的帧指针
**返回**：1 = 可以改写；0 = 形状不认识（拒绝而不是猜）

### `uint32 arkrtos_port_swap_frame_bytes(void);`

```c
uint32 arkrtos_port_swap_frame_bytes(void);
```

**本架构一帧占多少字节（用于「帧是否整块落在旧栈里」的边界检查）**
**返回**：32（Cortex-M）/ 144（RV32IMAC）/ 256（LA64）

### `void arkrtos_port_swap_rewrite(void *p_exc_ctx, uint32 entry);`

```c
void arkrtos_port_swap_rewrite(void *p_exc_ctx, uint32 entry);
```

**把帧就地改写成「新镜像入口的第一行」**
- `p_exc_ctx`：帧指针（已经在新的数据块里）
- `entry`：新入口地址
只改与「从哪里取指、用哪段栈、以什么特权级返回」有关的那些字，
其余寄存器按架构语义保留或清零，见各端口的实现注释。

### `void arkrtos_port_swap_activate(void *p_old_ctx, void *p_new_ctx);`

```c
void arkrtos_port_swap_activate(void *p_old_ctx, void *p_new_ctx);
```

**让「本次异常返回」用新帧**
- `p_old_ctx`：旧帧（换下来的那一份）
- `p_new_ctx`：新帧（改写好的那一份）
Cortex-M 把 PSP 挪过去；RISC-V / LoongArch 的恢复帧是 trap C 入口
的返回值，端口在这里记账，由各自的 trap 出口取走。

### `uint32 arkrtos_port_get_control(void);`

```c
uint32 arkrtos_port_get_control(void);
```

**获取CONTROL寄存器值**
**返回**：CONTROL寄存器当前值

### `void arkrtos_port_set_control(uint32 val);`

```c
void arkrtos_port_set_control(uint32 val);
```

**设置CONTROL寄存器值**
- `val`：要写入的值

### `unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*`

```c
unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*entry)(void));
```

**初始化任务栈帧**
- `stack_top`：栈顶地址（高地址，已8字节对齐）
- `entry`：任务入口函数
**返回**：初始化后的栈指针（将存入task->stack_ptr）
不同架构的异常栈帧格式不同：
- ARM Cortex-M: 压入xPSR/PC/LR/R12/R3-R0(硬件) + R4-R11(软件)
- RISC-V: 压入mepc/mstatus/x1-x31
此函数由port层实现，内核无需关心具体布局

### `void arkrtos_port_task_set_tp(unsigned long stack_ptr, unsigned long t`

```c
void arkrtos_port_task_set_tp(unsigned long stack_ptr, unsigned long tp);
```

**把初始栈帧里的线程指针（TLS 用）写成指定值**
- `stack_ptr`：arkrtos_port_stack_init() 返回的帧指针
- `tp`：TLS 块起始地址
只有实现了「TP 写入」的端口才有这个函数（目前是 LoongArch64，
写的是帧里 $r2 的槽）。装载器在 ARKRTOS_USE_ELF_TLS=1 且镜像带
PT_TLS 时调用它；关档时这个声明与实现都不存在，端口体积不变。
其它端口打开 ARKRTOS_USE_ELF_TLS 是编译期错误，不是空操作。

### `void arkrtos_port_enter_idle(unsigned long stack_ptr, uint32 use_priv)`

```c
void arkrtos_port_enter_idle(unsigned long stack_ptr, uint32 use_priv);
```

**切换到空闲任务上下文**
设置PSP/CONTROL寄存器，切换到PSP栈模式
不同架构的特权级切换机制不同，由port层实现
- `psp`：空闲任务栈顶地址
- `use_priv`：是否使用特权分离模式

### `void   arkrtos_port_switch_enable(void);`

```c
void   arkrtos_port_switch_enable(void);
```

**Enable task switching and request the first switch**
Must be called after arkrtos_port_enter_idle() (CPU on PSP) and
arkrtos_port_start_timer(). Switching requests issued before this
call are ignored on purpose.

### `uint32 arkrtos_port_get_system_clock(void);`

```c
uint32 arkrtos_port_get_system_clock(void);
```

**获取系统主频(Hz)**
**返回**：系统主频，单位Hz
不同平台获取方式不同：
- STM32: SystemCoreClock变量
- 其他MCU: 可能是宏或函数
内核通过此接口统一获取，不直接依赖SystemCoreClock

### `uint32 arkrtos_port_get_timer_counter(void);`

```c
uint32 arkrtos_port_get_timer_counter(void);
```

**获取系统节拍定时器当前计数值（自由递减计数器）**
**返回**：当前计数值
ARM 为 SysTick->VAL，RISC-V 可为 mtime 低 32 位，
LoongArch 可为恒定频率定时器计数，由 port 层映射。

### `uint32 arkrtos_port_get_cycle_clock(void);`

```c
uint32 arkrtos_port_get_cycle_clock(void);
```

**周期计数器的频率（Hz），用来把周期差折算成时间**
**返回**：每秒多少个 get_cyccnt() 计数；0 = 未知（调用方必须报"不可用"）
ARM 上 DWT CYCCNT 与内核时钟同源，返回 get_system_clock()；
RISC-V / LoongArch 上 mcycle / TSC 与节拍时基（mtime / rdtime）
不同源，频率由 cyccnt_init() 在开机时用已知时基实测校准，
所以这里返回的是"量出来的比值"，不是抄一个常数。
0 是合法返回值：校准失败就报不可用，不得拿节拍频率顶替
（两者差一个数量级，会把所有微秒读数算错）。

### `int32  arkrtos_port_cycle_clock_measured(void);`

```c
int32  arkrtos_port_cycle_clock_measured(void);
```

**周期计数器频率的来源**
**返回**：0 = 板级常量（ARM：与内核时钟同源）；1 = 运行期实测校准

### `uint32 arkrtos_port_get_timer_reload(void);`

```c
uint32 arkrtos_port_get_timer_reload(void);
```

**获取系统节拍定时器重载值（计数上限）**
**返回**：重载值
ARM 为 SysTick->LOAD，其他架构为等效周期值。

### `void arkrtos_port_start_timer(uint32 tick_period_us);`

```c
void arkrtos_port_start_timer(uint32 tick_period_us);
```

**启动系统节拍定时器**
- `tick_period_us`：节拍周期（微秒）

### `void arkrtos_port_start_timer_counts(uint32 counts);`

```c
void arkrtos_port_start_timer_counts(uint32 counts);
```

**以原始计数编程节拍定时器（tickless 用）**
- `counts`：一个"长节拍"的计数值（= n 个正常节拍）
与 arkrtos_port_start_timer(us) 的区别只在于单位：这里给的是
时钟计数，port 层不再做微秒换算，避免 n*TICK_PERIOD_US 的整数
除法丢相位。计数值必须落在定时器自身位宽内（Cortex-M 的 SysTick
是 24 位），越界由调用者负责（arkrtos_tickless_idle 会先夹住）。

### `uint32 arkrtos_port_timer_countflag_take(void);`

```c
uint32 arkrtos_port_timer_countflag_take(void);
```

**读取并清除节拍定时器的"计到过 0"标志**
**返回**：1 = 自上次读取以来计数器到过 0；0 = 没有
ARM 上是 SysTick->CTRL 的 COUNTFLAG（读 CTRL 会顺带清它）。
tickless 用它区分"睡满了整段"和"被别的中断提前叫醒"：前者
计数器已经回卷，后者没有。没有这个位的架构返回 0，调用方按
"提前唤醒"处理（结果仍然正确，只是少省一次）。

### `uint32 arkrtos_port_timer_sleep_max(void);`

```c
uint32 arkrtos_port_timer_sleep_max(void);
```

**一次 tickless 睡眠最多能编程多少个计数**
**返回**：定时器位宽允许的上限（ARM SysTick = 24 位，RISC-V mtimecmp 用
32 位的一半，LoongArch TCFG = 30 位）
tickless 用它夹住睡眠长度。把上限放在 port 层，是因为"计数器
位宽"和"有没有回卷标志"都是 port 的私事；内核那边只关心
"这次能睡多长"。

### `uint32 arkrtos_port_timer_elapsed_counts(uint32 programmed);`

```c
uint32 arkrtos_port_timer_elapsed_counts(uint32 programmed);
```

**自上次 arkrtos_port_start_timer_counts() 以来实际走过了多少个计数**
- `programmed`：当时编程下去的计数值
**返回**：已走过的计数（>= 1）；计数器不动时返回 1
ARM 用 SysTick 的 COUNTFLAG + VAL（24 位）；RISC-V 的 mtimecmp
是"比较值"、没有回卷标志，所以 port 在编程时留一个时基快照，
用差值算；LoongArch 的 TCFG 是周期定时器，同理用 rdtime 快照
按两个时钟的频率比折算成 TCFG 计数。返回值允许超过 programmed
（被拖得太久），调用方按整拍补。

### `void arkrtos_port_delay_us(uint32 us);`

```c
void arkrtos_port_delay_us(uint32 us);
```

**微秒级忙等延时**
- `us`：延时微秒数
使用硬件定时器实现精确延时，
由port层根据具体定时器实现

### `int32 arkrtos_port_wdg_init(uint32 timeout_ms);`

```c
int32 arkrtos_port_wdg_init(uint32 timeout_ms);
```

**Start the independent watchdog with an approximate timeout.**
- `timeout_ms`：Nominal timeout in milliseconds (0 = reject).
**返回**：0 on success, -1 when the port has no watchdog or cannot
represent the requested timeout.
Called at most once, by whoever owns the decision to enable it
(ARKRTOS_WDG_ENABLE). On STM32 the IWDG cannot be stopped once it
runs, so a wrong timeout here is not recoverable in software.
The timeout the hardware actually got is reported by
arkrtos_port_wdg_timeout_ms() - callers must read that rather than
assume the number they asked for.

### `void arkrtos_port_wdg_feed(void);`

```c
void arkrtos_port_wdg_feed(void);
```

**Reload the watchdog counter.**
Deliberately does nothing else: the kernel guard decides *whether*
to feed, this only performs the register write.

### `uint32 arkrtos_port_wdg_timeout_ms(void);`

```c
uint32 arkrtos_port_wdg_timeout_ms(void);
```

**Timeout the watchdog is actually running with (0 = not started).**

### `void arkrtos_port_board_init(void);`

```c
void arkrtos_port_board_init(void);
```

**板卡硬件初始化**
初始化CPU协处理器（如FPU）、时钟等

### `void arkrtos_port_irq_init(void);`

```c
void arkrtos_port_irq_init(void);
```

**中断控制器初始化**
配置NVIC优先级分组，设置PendSV/SysTick/SVC优先级

### `void arkrtos_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, ui`

```c
void arkrtos_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size);
```

**设置空闲任务MPU区域**
当 ARKRTOS_USE_MPU=1 时由port层实现
- `task_func`：任务函数地址
- `stack_addr`：栈地址
- `stack_size`：栈大小

### `int32 arkrtos_port_paging_init(void);`

```c
int32 arkrtos_port_paging_init(void);
```

**建立/确认分页映射**
**返回**：0 = 映射可用；-1 = 本 port 不具备分页通路
LoongArch 的静态映射在 la_mmu_init() 里建好（开中断之前），
本函数只报告那份映射是否已经生效，不重建它。 */

### `int32 arkrtos_port_paging_check(unsigned long base, uint32 size, arkrt`

```c
int32 arkrtos_port_paging_check(unsigned long base, uint32 size, arkrtos_mpu_mem_t mem);
```

**审计一段地址跨度**
- `base`：起始虚拟地址
- `size`：字节数
- `mem`：期望的内存属性（取自 arkrtos_mpu_mem_t）
**返回**：0 = 已映射且权限满足；-1 = 未映射 / 权限不足 / 该属性本 port 无对应窗口 */

### `int32 arkrtos_port_paging_alias(uint32 va, uint32 pa, arkrtos_mpu_mem_`

```c
int32 arkrtos_port_paging_alias(uint32 va, uint32 pa, arkrtos_mpu_mem_t mem);
```

**追加一条动态映射（VA -> PA）**
**返回**：0 = 已建立；-1 = 本 port 未实现
动态映射是登记欠账，这里不假装支持。 */

### `int32 arkrtos_port_paging_unalias(uint32 va);`

```c
int32 arkrtos_port_paging_unalias(uint32 va);
```

**撤销一条动态映射（U-15）**
- `va`：登记的虚拟地址（4 KB 对齐）
**返回**：0 = 已撤销（该页条目写成无效，同对另一页保持原样）；
-1 = 未 4 KB 对齐；-2 = 落在静态跨度里（静态映射从不撤销）；
-4 = 这个 VA 本来就没登记过
撤销靠「按同一个绝对索引把这条条目重写空」完成：动态条目落
MTLB 的绝对槽位，所以不需要 invtlb —— 覆盖后旧翻译就不存在了。
两半都空了才把槽位标成空闲，否则只清被撤的那一半。 */

### `uint32 arkrtos_port_paging_dyn_used(void);`

```c
uint32 arkrtos_port_paging_dyn_used(void);
```

**当前占用了几条动态映射槽位（pmap 读数用）**
**返回**：已占用槽位数（0..上限） */

### `void arkrtos_port_paging_flush(uint32 base, uint32 size);`

```c
void arkrtos_port_paging_flush(uint32 base, uint32 size);
```

**让一段地址的翻译失效**
静态映射从不撤销，所以本 port 是空操作；留这个口是为了让
将来真做动态重映射时有地方落，而不是到时候改契约。 */

### `int32 arkrtos_port_paging_activate(uint32 space);`

```c
int32 arkrtos_port_paging_activate(uint32 space);
```

**把一个 App 地址空间装进 App 窗口（U-1）**
- `space`：0 .. arkrtos_port_paging_space_count()-1
**返回**：0 = 已是该空间或切换成功；-1 = 空间号越界 / 本 port 无 TLB 通路
本 port 没有硬件页表遍历，所以「每进程页表」= 每个 App 一组
条目、切换时重写 App 窗口那一条 PS=20 条目。空间号即帧号：
space 0 是 PA = ARKRTOS_QEMU_APP_BASE（行为与之前完全一致），
space n 是 ARKRTOS_QEMU_APP_BASE + n * APP_SIZE。
内核的恒等映射与 UART 条目一条都不动。 */

### `int32 arkrtos_port_paging_switch_to(uint32 space);`

```c
int32 arkrtos_port_paging_switch_to(uint32 space);
```

**调度器用的「装进窗口」：必要时推迟到恢复现场那一刻（U-2）**
- `space`：目标空间号
**返回**：0 = 已装上 / 已记下待装；-1 = 空间号越界 / 本 port 无 TLB 通路
与 activate() 只差一条，但那条是致命的：调度器调它的时候内核
还跑在**被换下任务自己的栈上**，而 App 任务的栈就在 App 窗口里。
此刻重写窗口条目，紧接着的一次 ld.d / 函数返回就会从另一个 App
的帧里取值 —— 实测症状是两个 hello 并发跑时 era=0xcdcdcdcd
（栈填充图案）停机。所以本 port 在「调用者 sp 落在 App 窗口内」
时只记下空间号，由 arkrtos_context.S 的恢复现场路径在读目标帧
之前补上这一刀（la_switch_prepare）。内核任务的 sp 在恒等映射
里，那种情况立即生效，与 activate() 完全等价。 */

### `uint32 arkrtos_port_paging_space(void);`

```c
uint32 arkrtos_port_paging_space(void);
```

**当前装进 App 窗口的空间号 */**

### `uint32 arkrtos_port_paging_space_frame(uint32 space);`

```c
uint32 arkrtos_port_paging_space_frame(uint32 space);
```

**空间号对应的物理帧基址；越界返回 0（不猜一个地址出去） */**

### `uint32 arkrtos_port_paging_space_count(void);`

```c
uint32 arkrtos_port_paging_space_count(void);
```

**板级给了几个 App 地址空间（>= 1） */**

### `int32 arkrtos_port_paging_space_release(uint32 space);`

```c
int32 arkrtos_port_paging_space_release(uint32 space);
```

**Give a space's frames back to the pool (U-3 leftover)**
- `space`：0 .. arkrtos_port_paging_space_count()-1
**返回**：0 = returned (also when the space never took any frames);
-1 = bad space; -2 = this build has no frame pool behind the
window; -3 = the caller is standing inside the window VA
(nothing touched); -4 = the pool refused (its own -1/-3 passed on)
空间回收：把该空间从帧池拿走的帧还回去，并且**先把窗口里属于它的
条目撤掉**。不撤的话，帧已经回到池子、旧条目还指着那块物理页 ——
它被别的空间拿走之后两个空间就共享内存，而且没有任何报错。
只有页级窗口（ARKRTOS_USE_PAGING_PAGES=1）有帧可还；PS=20 的静态
窗口是板级固定地址，没有可回收的东西，返回 -2。
归还之后空间仍然存在（space_count 不变），下一次 activate() 会
重新要帧 —— 语义是「这个空间空着」，不是「这个空间没了」。 */

### `uint32 arkrtos_port_paging_lookup(unsigned long va, uint32 *index, uin`

```c
uint32 arkrtos_port_paging_lookup(unsigned long va, uint32 *index, uint32 *ppn, uint32 *ps);
```

**读回一个 VA 当前由哪条 TLB 条目翻译（诊断用）**
**返回**：1 = 命中（index/ppn/ps 被写）；0 = 未命中（输出参数不动）
只读，不建映射。用于分辨「tlbwr 没生效」与「生效了但查找命中
另一条陈旧条目」——地址空间切换静默失效时就靠它定位。 */

### `int32 arkrtos_port_paging_frame_alloc(uint32 count, uint32 *p_base);`

```c
int32 arkrtos_port_paging_frame_alloc(uint32 count, uint32 *p_base);
```

**Allocate a run of contiguous 4 KB page frames (U-3)**
- `count`：frames wanted (> 0)
- `p_base`：on success, physical base of the first frame
**返回**：0 = allocated; -1 = bad argument; -2 = no run that long
Bookkeeping only: it does not install any mapping and does not
touch the App window.  See ARKRTOS_USE_PAGING_FRAMES. */

### `int32 arkrtos_port_paging_frame_free(uint32 base, uint32 count);`

```c
int32 arkrtos_port_paging_frame_free(uint32 base, uint32 count);
```

**Return a run of contiguous 4 KB page frames**
**返回**：0 = returned; -1 = outside the pool / unaligned / bad count;
-3 = the run contains a frame that was not allocated (nothing moved)
All-or-nothing: one stray free frame refuses the whole run, so a
frame can never end up owned by two callers. */

### `uint32 arkrtos_port_paging_frame_total(void);`

```c
uint32 arkrtos_port_paging_frame_total(void);
```

**Total frames in the board pool (0 when there is none) */**

### `uint32 arkrtos_port_paging_frame_used(void);`

```c
uint32 arkrtos_port_paging_frame_used(void);
```

**Frames currently allocated from the board pool */**

### `uint32 arkrtos_port_paging_pair_total(void);`

```c
uint32 arkrtos_port_paging_pair_total(void);
```

**How many 8 KB pairs the App window is made of (0 = no window)**
One PS=12 TLB entry covers two adjacent 4 KB pages, so the
window's accounting unit is a pair, not a page. The number is
the same for every space (the VA side never moves). */

### `uint32 arkrtos_port_paging_pair_count(uint32 space);`

```c
uint32 arkrtos_port_paging_pair_count(uint32 space);
```

**How many of those pairs a space currently has mapped**
**返回**：pair count; 0 for a bad space or a space that was never installed
In the demand build this counts the pairs whose two frames have
actually been paid for; in the page-level build an installed
space owns the whole 2 MB run, so it is every pair. */

### `uint32 arkrtos_port_paging_pair_pa(uint32 space, uint32 idx);`

```c
uint32 arkrtos_port_paging_pair_pa(uint32 space, uint32 idx);
```

**Physical base of one pair; 0 = that pair is not mapped**
- `space`：address space
- `idx`：0 .. arkrtos_port_paging_pair_total()-1
Read-only: it never faults a pair in, so asking "what is
mapped" cannot change the answer. A pair that has not been
touched yet reports 0 instead of a made-up address. */

### `int32 arkrtos_port_paging_space_clone(uint32 src, uint32 dst);`

```c
int32 arkrtos_port_paging_space_clone(uint32 src, uint32 dst);
```

**Copy one space's whole window content into another (U-19, fork)**
- `src`：source space, @param dst destination space (must be empty)
**返回**：0 = copied; -1 = bad space / same space; -2 = dst already has
frames (never overwritten); -3 = frame pool refused, rolled back
Copies the pairs the source actually owns, frame by frame,
through the pool's identity mapping - the App window is NOT
switched, because the caller is running inside it. A pair the
source has swapped out is copied in from the swap area, so the
child sees the same bytes the parent would see, resident.
On failure every frame this call took is returned to the pool.
> 说明：只在 ARKRTOS_USE_LINUX_FORK == 1 时存在。 */

### `int32 arkrtos_port_paging_write_space(uint32 space, unsigned long va, `

```c
int32 arkrtos_port_paging_write_space(uint32 space, unsigned long va, const unsigned long *words, uint32 n);
```

**Write a few words into a space by physical address (U-19, fork)**
- `space`：target space, @param va window VA (8-byte aligned)
- `words`：source words (8 bytes each), @param n word count
**返回**：0 = written; -1 = out of range, or that pair has no frames
No demand faulting here on purpose: the caller must write into
a pair the parent already made resident, otherwise it rolls
back. Kernel paths do not quietly allocate frames behind the
bookkeeping's back.
> 说明：只在 ARKRTOS_USE_LINUX_FORK == 1 时存在。 */

### `int32 arkrtos_port_paging_clone_frame(void *p_exc_ctx, uint32 space);`

```c
int32 arkrtos_port_paging_clone_frame(void *p_exc_ctx, uint32 space);
```

**Turn the parent trap frame into the child's first frame (U-19)**
- `p_exc_ctx`：parent's syscall trap frame (also its VA in the child)
- `space`：child space
**返回**：0 = written; <0 = arkrtos_port_paging_write_space's reason
Copies every register slot verbatim and rewrites a0 to 0, so
the child resumes at the same PC with the same stack and sees
clone() returning 0. ERA/sp/tp/fp and the FPU/vector area are
inherited - that IS fork.
> 说明：只在 ARKRTOS_USE_LINUX_FORK == 1 时存在。 */

### `#if (ARKRTOS_USE_PAGING_SWAP == 1) uint32 arkrtos_port_paging_swap_sta`

```c
#if (ARKRTOS_USE_PAGING_SWAP == 1) uint32 arkrtos_port_paging_swap_stat(uint32 which);
```

**Page replacement readout (U-16) - only exists when the swap switch is on**
- `which`：0 = swap-outs, 1 = swap-ins, 2 = slots in use, 3 = failures,
4 = resident pairs, 5 = slot count
**返回**：the reading; 0 for an unknown selector
关档时连符号都不给（宏在 arkrtos_mpu.h 里回 0）：一个「永远返回 0
的统计函数」会被读成「一次都没换出过」，那是错答案。 */

### `unsigned long arkrtos_port_paging_window_va(void);`

```c
unsigned long arkrtos_port_paging_window_va(void);
```

**Virtual base of the App window (same VA for every address space)**
**返回**：the window VA base (0 when the board has no paging window)
The physical side of the window differs per space and comes from
arkrtos_port_paging_space_frame(); the VA side never moves.  Code
that pokes the window must use this instead of the frame base:
with ARKRTOS_USE_PAGING_PAGES=1 the frames come from the pool and
no longer coincide with the window VA. */

### `#if (ARKRTOS_USE_PAGING_ASID == 1) unsigned long arkrtos_port_paging_a`

```c
#if (ARKRTOS_USE_PAGING_ASID == 1) unsigned long arkrtos_port_paging_asid_current(void);
```

**Hardware ASID currently in CSR.ASID (U-7); 0 when ASID is off**
关档（ARKRTOS_USE_PAGING_ASID=0）时恒为 0：本 port 把每个 App 空间
摊在一组固定的 TLB way 上、切换靠重写条目，压根没有 ASID 这个
概念，返回一个非零值就是编的。 */

### `unsigned long arkrtos_port_paging_asid_raw_swap(unsigned long asid);`

```c
unsigned long arkrtos_port_paging_asid_raw_swap(unsigned long asid);
```

**只写一次 CSR.ASID 并返回换之前的值（诊断用，U-7b）**
- `asid`：目标 ASID（只取低 10 位）
**返回**：换之前的 CSR.ASID
与 arkrtos_port_paging_activate() 的差别只有一条但很要紧：
activate 会读回校验、缺什么补什么，所以拿它测「条目还在不在」
测不出来。本函数只做一次 csrwr，不记账不补铺，调用方负责换回
去。生产路径不该用它。 */

### `void arkrtos_port_paging_inval_va(unsigned long va);`

```c
void arkrtos_port_paging_inval_va(unsigned long va);
```

**按 VA 让翻译失效（invtlb op=3：不看 ASID，global 条目不动）**
- `va`：要失效的虚拟地址
留着它是为了做对照：op=6 的「定向」只有跟 op=3 的连带行为摆
在一起才算证据。生产路径没有调用方。 */

### `int32  arkrtos_port_flash_erase(uint32 addr, uint32 size);`

```c
int32  arkrtos_port_flash_erase(uint32 addr, uint32 size);
```

**擦除 Flash 区间（按扇区擦除，自动对齐到扇区边界）**
- `addr`：起始地址
- `size`：字节长度
**返回**：0=成功，-1=失败

### `int32  arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32`

```c
int32  arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len);
```

**写入 Flash（按字节编程，写入前该区间必须已擦除）**
- `addr`：起始地址
- `data`：数据指针
- `len`：字节长度
**返回**：0=成功，-1=失败

### `uint32 arkrtos_port_flash_sector_size(uint32 addr);`

```c
uint32 arkrtos_port_flash_sector_size(uint32 addr);
```

**获取指定地址所在扇区的大小（字节）**
- `addr`：Flash 地址
**返回**：扇区大小；地址非法返回 0

### `typedef struct {`

```c
typedef struct {
```

**片内 Flash 的保护状态（只读，不解锁、不写选项字节）**
由板级实现；内核的安全基线自检用它把"读保护/写保护到底什么档"
摆出读数，而不是报一句"没通路"。

### `int32  arkrtos_port_flash_protection(arkrtos_flash_prot_t *p_out);`

```c
int32  arkrtos_port_flash_protection(arkrtos_flash_prot_t *p_out);
```

**读取片内 Flash 的保护状态**
- `p_out`：输出（三个字段全部由实现填写）
**返回**：0=读取成功；其他=读不到（板级未实现 / 该芯片没有这些寄存器）
> 说明：只读操作：不解除 OPTLOCK、不擦除、不改变任何保护位。

## kernelsrc/include/arkrtos_hwsafe.h

**Ark RTOS 硬件安全态（"遗言"）机制 —— 内核内部头文件**
App 控制硬件（电机 / 机床 / 机械臂 / 继电器……）时崩溃，绝不能让硬件
带着崩溃前的最后一条指令继续跑。本模块把"设备归谁"和"所有者死掉时
怎么把它带回安全态"记在内核侧，由**内核自己**在任务死亡时执行——

### `typedef int32 (*arkrtos_hwsafe_fn)(arkrtos_dev_hdr_t *obj, uint32 owne`

```c
typedef int32 (*arkrtos_hwsafe_fn)(arkrtos_dev_hdr_t *obj, uint32 owner);
```

**设备安全态回调（"中和"函数）**
- `obj`：该设备已被打开的实例（驱动的私有结构，hdr 在开头）
- `owner`：触发本次中和的（已死）任务号，仅作日志/判断用，可为 0
**返回**：0 = 已带到安全态；非 0 = 驱动自报失败（内核只记账，不重试）
`@warning 调用契约（必须遵守，否则本机制会比不加还危险）：`
- 运行在**任务退出 / 故障恢复路径**，中断可能处于屏蔽态；
- 因此必须**非阻塞**：只做有界的寄存器 / GPIO 写，不得等待、不得
延时、不得调用任何 SVC、不得触碰文件系统或 Flash；
- 不得依赖"当前运行的是哪个任务"这类上下文（此刻上下文正处在死亡
任务的收尾里）；
- 允许被并发/重入调用（内核不保证只调一次），应写成幂等。

### `int32  arkrtos_hwsafe_register(const char *dev_name, arkrtos_hwsafe_fn`

```c
int32  arkrtos_hwsafe_register(const char *dev_name, arkrtos_hwsafe_fn fn);
```

**驱动登记某设备的安全态回调**
- `dev_name`：设备名（与 arkrtos_dev_register 用的一致，最多 7 字符）
- `fn`：中和函数，见 arkrtos_hwsafe_fn 契约
**返回**：0 = 成功；-1 = 表满；-2 = 参数非法 / 同名设备已登记

### `int32  arkrtos_hwsafe_unregister(const char *dev_name);`

```c
int32  arkrtos_hwsafe_unregister(const char *dev_name);
```

**注销某设备的安全态回调**
- `dev_name`：设备名
**返回**：0 = 已注销；-1 = 该设备没有登记过回调
> 说明：设备被 arkrtos_dev_unregister() 注销时，内核会自动调用一次，避免留下
指向已卸载镜像的回调指针（"回调还挂着，代码已经不在了"）。

### `int32  arkrtos_hwsafe_set_will(const char *dev_name, int32 owner, int3`

```c
int32  arkrtos_hwsafe_set_will(const char *dev_name, int32 owner, int32 cmd, int32 val);
```

**所有者（App）为某设备留下一句"遗言"：我死后请对它执行这条 ctrl**
- `dev_name`：设备名（所有者必须当前正持有该设备的 claim）
- `owner`：任务号；内核在它死亡时执行该遗言
- `cmd/val`：传给设备驱动 drv_ctrl(obj, cmd, val) 的命令与参数
**返回**：0 = 已登记；-1 = 该 owner 并未拥有该设备（必须先 open 过）；-2 = 参数非法
> 说明：这是 **App 侧** 的通道：App 的代码在它死后不可依赖，所以遗言是"一条
内核能替它执行的控制命令"，而不是一个回调。执行时调用的仍是**驱动**的
drv_ctrl（常驻代码），因此与"驱动注册回调"走的是同一条可信路径。
若该设备同时登记过回调，遗言优先（更具体）。

## kernelsrc/include/arkrtos_init.h

**Ark RTOS 内核启动初始化接口**
把"内核需要初始化哪些模块、以什么顺序"收敛到唯一一处实现
（arkrtos_init.c 的 arkrtos_kernel_module_init），避免出现两份启动流程
（内核默认入口与工程 main）各自演化、

### `void arkrtos_kernel_module_init(void);`

```c
void arkrtos_kernel_module_init(void);
```

**初始化全部内核模块，并注册内置服务任务**
顺序：事件 → 同步 → 消息队列 → 软定时器 → 故障记录 → 设备 → 板级设备，
然后注册软定时器服务任务，最后（需要时）初始化 MPU。
必须在 arkrtos_cfg_load() 之后、调度启动之前调用。
> 说明：任何启动路径都应调用本函数，不要在工程侧重复逐个调用各模块 init。

## kernelsrc/include/arkrtos_installer.h

**Ark RTOS 内核内安装任务（方案A）**
常驻任务从设备（默认 ttyS0）接收 Ark RTOS 镜像流并安装到空闲槽位，
使 App 的落位方式从「烧录器刷固件」变为「设备自己安装」。
接收协议（与 tools/pack_app.py 的输出直接对接）：

### `int32 arkrtos_installer_init(void);`

```c
int32 arkrtos_installer_init(void);
```

**初始化并注册安装任务**
**返回**：成功返回任务号（>0）；INSTALLER_ENABLE 为 0 时返回 0
需在调度启动之前调用（建议跟在 arkrtos_loader_scan() 之后）。
开启内核 Shell（SHELL_ENABLE == 1）时不要调用本函数：常驻安装任务
会与控制台抢同一个串口 FIFO，此时改用 arkrtos_installer_run_once()
由 `install` 命令触发一次性窗口，串行占用串口。

### `int32 arkrtos_installer_run_once(int32 dev, uint32 timeout_ms);`

```c
int32 arkrtos_installer_run_once(int32 dev, uint32 timeout_ms);
```

**在指定设备上接收并安装一个镜像（阻塞式，带超时）**
- `dev`：已打开的镜像接收设备句柄（控制台复用同一个句柄）
- `timeout_ms`：等待镜像头的超时（ms）；超时后清空接收状态并返回
**返回**：成功返回槽位号（>=0），失败返回 ARKRTOS_LOADER_ERR_x
与常驻任务共用同一套同步/校验/落盘逻辑，区别只在调用方式：
本函数在一个任务上下文内同步跑完「等头 → 落盘 → 按自启标志启动」，
期间调用方不应再去读同一个设备。
是否自启由镜像头 flags（ARKRTOS_APP_FLAG_AUTOSTART）决定。

### `int32 arkrtos_installer_from_file(const char *path);`

```c
int32 arkrtos_installer_from_file(const char *path);
```

**Install an .svcapp that is already sitting in the mounted volume**
- `path`：absolute path inside the mounted volume, e.g. "/APP_DEMO.svcapp"
**返回**：slot id (>=0) on success, ARKRTOS_LOADER_ERR_x on failure
This is the "install it from the device's own storage" entrance:
the image is read in chunks from littlefs and handed to the same
streaming loader the serial window uses, so no image ever has to
fit in RAM. A mounted volume is required; with none mounted it
returns ARKRTOS_LOADER_ERR_PARAM rather than guessing.
Whether the image starts is decided by the header flags, exactly as
on the serial path and at boot - one rule, three entrances.

## kernelsrc/include/arkrtos_layout.h

**Kernel-side layout resolution: reads the device-side configuration**
record (if any), validates it and publishes the effective layout.
Called once during boot, right after arkrtos_ptable_init() and before
the pool is scanned: the loader, the installer and the shell all ask

### `void   arkrtos_layout_init(void);`

```c
void   arkrtos_layout_init(void);
```

**Resolve the effective layout and publish it into the partition table.**
Order: start from the compile-time default layout, then try to load
and validate the device-side record; a valid record overrides the
default, anything else (absent / bad magic / bad CRC / hw mismatch /
illegal slot table) keeps the default and stores a reason code that
the shell prints. Never returns an error: a device without a usable
layout still boots.
> 说明：Must be called after arkrtos_ptable_init() (it writes partition-table
fields) and before the pool scan (the scan uses the mode).

### `void   arkrtos_layout_apply_runtime(void);`

```c
void   arkrtos_layout_apply_runtime(void);
```

**Apply the runtime knobs of the effective configuration.**
Called at the end of arkrtos_layout_init(). Only the knobs this
kernel build really honours are applied -- today the log level
and the crash-restart budget, both of which the shared
partition table then carries for the Loader. Every other knob
in the record is rejected by arkrtos_layout_validate() with
ARKRTOS_CFG_ERR_NOT_IMPL rather than accepted and ignored: a
configuration that says one thing and makes the device do
another is the hardest kind of problem to find later.

### `const arkrtos_cfg_slot_t *arkrtos_layout_slots(void);`

```c
const arkrtos_cfg_slot_t *arkrtos_layout_slots(void);
```

**Effective fixed-slot table (ARKRTOS_CFG_SLOT_MAX entries, type 0 = unused). */**

### `const arkrtos_cfg_slot_t *arkrtos_layout_slot(uint32 index);`

```c
const arkrtos_cfg_slot_t *arkrtos_layout_slot(uint32 index);
```

**One entry of the effective table, or 0 when index is out of range. */**

### `int32  arkrtos_layout_slot_by_base(uint32 addr);`

```c
int32  arkrtos_layout_slot_by_base(uint32 addr);
```

**Index of the fixed slot whose Flash range contains addr.**
**返回**：Slot index, or -1 when no fixed slot covers addr.

### `const char *arkrtos_layout_reason_text(uint32 reason);`

```c
const char *arkrtos_layout_reason_text(uint32 reason);
```

**Human-readable name of a validation reason code (ASCII, for the shell). */**

### `uint32 arkrtos_layout_validate(const arkrtos_cfg_record_t *rec);`

```c
uint32 arkrtos_layout_validate(const arkrtos_cfg_record_t *rec);
```

**Validate a record: identity, CRC, hardware match and the whole slot table.**
**返回**：ARKRTOS_CFG_OK or one of the ARKRTOS_CFG_ERR_x codes.
> 说明：The slot table is checked against the kernel's own pool and RAM bounds,
so a record prepared for another board or another memory map is rejected
instead of being applied.

### `int32  arkrtos_layout_write(const uint8 *record, uint32 len, uint32 *p`

```c
int32  arkrtos_layout_write(const uint8 *record, uint32 len, uint32 *p_erased);
```

**Append a validated record to the CONFIG region and adopt it.**
- `record`：exactly ARKRTOS_CFG_RECORD_SIZE bytes as produced by tools/arkrtos_layout.py
- `len`：must equal ARKRTOS_CFG_RECORD_SIZE
- `p_erased`：optional: set to 1 when the region had to be erased first
**返回**：>= 0 = record index written, < 0 = error (see the ARKRTOS_LAYOUT_ERR_x codes)
The kernel stamps seq and hw_compat_id itself and recomputes the CRC,
so a host that sends a slightly stale hw id still gets a record that
matches this kernel. If the region is full it is erased once and
writing restarts at offset 0; a reset during that single erase falls
back to the default layout instead of losing the device.

### `int32  arkrtos_layout_erase(void);`

```c
int32  arkrtos_layout_erase(void);
```

**Erase the CONFIG region and fall back to the compile-time default layout.**
**返回**：0 on success, < 0 on error.

### `int32  arkrtos_layout_read(arkrtos_cfg_record_t *out, uint32 *p_seq);`

```c
int32  arkrtos_layout_read(arkrtos_cfg_record_t *out, uint32 *p_seq);
```

**Read the live record (highest valid seq) from the CONFIG region.**
- `out`：destination buffer
- `p_seq`：optional: seq of the record that was read
**返回**：0 on success, < 0 when the region holds no valid record.

### `uint32 arkrtos_layout_crc(const arkrtos_cfg_record_t *rec);`

```c
uint32 arkrtos_layout_crc(const arkrtos_cfg_record_t *rec);
```

**CRC32 of a record with its crc32 field taken as zero (zlib-compatible). */**

## kernelsrc/include/arkrtos_layout_def.h

**On-device layout + runtime configuration record. This is the single**
persistent place where a device's install strategy (fixed slots vs.
automatic placement), its fixed slot table and a small set of runtime
knobs are stored, so a customer can configure a board without

### `typedef struct {`

```c
typedef struct {
```

**One fixed slot: Flash placement + RAM window of a headed or bare image. */**

### `typedef struct {`

```c
typedef struct {
```

**The persistent layout + runtime configuration record (512 bytes, no padding). */**

## kernelsrc/include/arkrtos_linux_abi.h

**Linux 系统调用 ABI 兼容层（带 MMU / MPU 目标上的 ELF 程序）**
这里只回答一个问题：一个按 Linux 约定发起系统调用的**静态链接**
可执行文件，能不能在 Ark 的 ELF 用户任务里跑起来。答案是"能跑
一个最小集合"，不是"支持 Linux"：

### `int32 arkrtos_linux_abi_try(void *p_svc_ctx);`

```c
int32 arkrtos_linux_abi_try(void *p_svc_ctx);
```

**在 SVC 入口尝试按 Linux ABI 处理本次调用**
- `p_svc_ctx`：系统调用上下文（SVC_Server 收到的那一个）
**返回**：1 = 已处理（返回值已写回 a0）；0 = 不是 Linux 调用，交给 Ark 号表
必须在 Ark 自己的 switch 之前调用。判定完全由 port 层给出，
内核不感知寄存器布局；本函数也不会把 Ark 的 0x10..0x22 吃掉。

### `uint32 arkrtos_linux_abi_stat(uint32 which);`

```c
uint32 arkrtos_linux_abi_stat(uint32 which);
```

**读计数器（内核 / shell 用）**
- `which`：0 = 已处理，1 = 未知号（-ENOSYS），2 = 用户指针被拒，
3 = 其它失败，4 = mprotect 接受但未强制
**返回**：计数值

### `uint32 arkrtos_linux_abi_nr_slots(void);`

```c
uint32 arkrtos_linux_abi_nr_slots(void);
```

**已用槽数（前缀 [0, 返回值) 有效，按首次出现顺序）**

### `uint32 arkrtos_linux_abi_nr_slot(uint32 idx, uint32 *p_nr);`

```c
uint32 arkrtos_linux_abi_nr_slot(uint32 idx, uint32 *p_nr);
```

**读第 idx 个槽**
- `idx`：槽下标
- `p_nr`：出参：系统调用号（可为 NULL）
**返回**：该号的调用次数；越界返回 0

### `uint32 arkrtos_linux_abi_nr_overflow(void);`

```c
uint32 arkrtos_linux_abi_nr_overflow(void);
```

**槽用满之后未记名的调用次数（不静默丢）**

### `void arkrtos_linux_abi_reset(void);`

```c
void arkrtos_linux_abi_reset(void);
```

**清空每次 exec 都必须重置的状态（fd 表与堆游标）**
由 ELF 装载器在起跑前调用：fd 表是内核全局的（一次只跑一个
程序），上一次留下的句柄不该被下一次继承。

## kernelsrc/include/arkrtos_loader.h

**Ark RTOS App 镜像加载器（分区内加载 / 校验 / 启动 / 搬移 / 卸载）**
负责把 App 镜像写入池内空闲空间，并按镜像头信息校验后拉起为任务。
镜像格式见 arkrtos_app_image.h；分区布局运行期从共享内存分区表获取
（见 arkrtos_ptable.h），本模块不硬编码任何地址。

### `int32 arkrtos_loader_load_buffer(const uint8 *image, uint32 image_len)`

```c
int32 arkrtos_loader_load_buffer(const uint8 *image, uint32 image_len);
```

**从内存缓冲区加载完整 App 镜像**
- `image`：指向镜像起始（含 256 字节头）的缓冲区
- `image_len`：缓冲区长度（字节），必须 >= 镜像总长
**返回**：成功返回槽位号（>=0），失败返回 ARKRTOS_LOADER_ERR_x
与设备流式路径共用同一套判定：校验头 -> 找空闲区 -> 登记槽位 ->
应用重定位并写入 -> CRC 复核 -> 事务提交 -> 置 LOADED。

### `int32 arkrtos_loader_load_dev(int32 dev, uint32 image_len);`

```c
int32 arkrtos_loader_load_dev(int32 dev, uint32 image_len);
```

**从设备流式加载完整镜像（自行同步并读取镜像头）**
- `dev`：已打开的设备句柄（数据从镜像头之前开始）
- `image_len`：期望的镜像总长（含头）；传 0 表示由镜像头中的 image_size 决定
**返回**：成功返回槽位号（>=0），失败返回 ARKRTOS_LOADER_ERR_x
> 说明：边读边写 Flash，仅使用固定大小分块缓冲，不要求整镜像驻留 RAM。

### `uint32 arkrtos_loader_scan(void);`

```c
uint32 arkrtos_loader_scan(void);
```

**扫描整个镜像池，把 Flash 中已存在且校验通过的镜像标记为 LOADED**
**返回**：有效（可启动）的 App 数量
槽位状态原本只存在于共享 RAM，重启后会丢失；本函数在启动时按
镜像头魔数 + 硬件兼容签名 + CRC32 重新认定槽位，使“先烧录镜像、
再上电运行”的最小闭环成立。
扫描按分配粒度（ARKRTOS_POOL_ALLOC_UNIT）线性遍历整个池：
遇到带头镜像则按头的 type 归类为 App / 驱动；遇到裸镜像则查编译期
开发槽位表（ARKRTOS_DEV_SLOTn_*）判定类型与跨度。
同时执行断电恢复：
- state = UNCOMMITTED 的副本一律作废（搬移中途掉电）；
- 同一 image_id 出现两份有效副本时保留地址较低的那份。
两者都登记为 INVALID，随后由 arkrtos_loader_reclaim() 擦除回收。

### `uint32 arkrtos_loader_scan_driver(void);`

```c
uint32 arkrtos_loader_scan_driver(void);
```

**扫描镜像池，返回其中的驱动数量**
**返回**：有效（可启动）的驱动数量，0=无有效驱动
与 arkrtos_loader_scan() 共用同一份扫描结果（同一次扫描的驱动视图），
因此两者不区分调用顺序，可各自独立调用。

### `void  arkrtos_loader_slot_hint_set(int32 index);`

```c
void  arkrtos_loader_slot_hint_set(int32 index);
```

**指定下一次安装落在配置槽表的哪一条（-1 = 让内核按类型自己挑）**
- `index`：配置槽表下标（0 .. arkrtos_layout_slot_count()-1），-1 表示不指定
只在固定槽位模式下起作用：这条提示只对**下一次**安装有效，
安装一开始就被消费掉（无论成功与否），不会泄漏到后续安装。
自动选址模式忽略它（同时也会消费掉，避免留下一个陈旧的值）。
> 说明：安装窗口由 shell 调用，提示也由 shell 设置；本接口不加锁，
调用者必须保证同一时刻只有一次安装在进行。

### `int32 arkrtos_loader_slot_hint_get(void);`

```c
int32 arkrtos_loader_slot_hint_get(void);
```

**读取当前的安装槽位提示（-1 = 未指定）。 @see arkrtos_loader_slot_hint_set */**

### `int32 arkrtos_loader_reclaim(void);`

```c
int32 arkrtos_loader_reclaim(void);
```

**回收死区：把「不含活镜像的扇区」擦成 0xFF，使空间重新可用**
**返回**：本次擦除的扇区数；负值为 ARKRTOS_LOADER_ERR_x
Flash 擦除粒度是整扇区，因此一个扇区里只要有活镜像就不能擦。
本函数对每个含死字节的扇区执行：
1) 扇区内已无活镜像 -> 直接擦；
2) 扇区内仍有活镜像 -> 先把这些镜像搬到池内最低的已擦除空位
（应用重定位，走 UNCOMMITTED -> VALID 事务），再擦。
搬不动的镜像（正在运行、或没有足够空位）会让该扇区保留死区，
函数跳过它并继续处理其它扇区。卸载/安装后调用一次即可。

### `uint32 arkrtos_loader_pool_free(uint32 *p_largest);`

```c
uint32 arkrtos_loader_pool_free(uint32 *p_largest);
```

**查询池内还剩多少可装镜像的物理空间**
- `p_largest`：非空时写入最长的一段连续空闲字节数
**返回**：所有 0xFF 连续段的总字节数

### `int32 arkrtos_loader_uninstall(uint32 slot);`

```c
int32 arkrtos_loader_uninstall(uint32 slot);
```

**卸载一个镜像：停止任务、注销记录，然后回收它占用的空间**
- `slot`：槽位记录号
**返回**：0=成功，负值为 ARKRTOS_LOADER_ERR_x
正在运行的任务会被先停止（镜像字节仍在 Flash，可再次启动）；
随后注销槽位记录并调用 arkrtos_loader_reclaim() 尽量把空间收回来。
> 说明：卸载是破坏性操作：擦除后镜像不可恢复，需要重新安装。

### `int32 arkrtos_loader_start_driver_slot(uint32 slot);`

```c
int32 arkrtos_loader_start_driver_slot(uint32 slot);
```

**启动指定驱动槽位的驱动**
- `slot`：驱动槽位号
**返回**：成功返回任务号（>0），失败返回 ARKRTOS_LOADER_ERR_x
栈从该镜像分配到的 RAM 块顶部切出；RAM 块由伙伴分配器按镜像头
声明的 ram_size 现算，因此不同镜像永不共用 RAM。

### `int32 arkrtos_loader_start_driver_manual(uint32 slot);`

```c
int32 arkrtos_loader_start_driver_manual(uint32 slot);
```

**驱动槽位的人工重试启动（语义同 arkrtos_loader_start_manual）**
- `slot`：槽位号
**返回**：成功返回任务号（>0），失败返回 ARKRTOS_LOADER_ERR_x

### `int32 arkrtos_loader_start_driver(void);`

```c
int32 arkrtos_loader_start_driver(void);
```

**启动 0 号驱动槽（兼容包装）**
**返回**：成功返回任务号（>0），失败返回 ARKRTOS_LOADER_ERR_x

### `int32 arkrtos_loader_stop_driver_slot(uint32 slot);`

```c
int32 arkrtos_loader_stop_driver_slot(uint32 slot);
```

**停止指定驱动槽位的任务（镜像仍保留在 Flash）**
- `slot`：驱动槽位号
**返回**：0=成功，负值为 ARKRTOS_LOADER_ERR_x

### `uint32 arkrtos_loader_state_driver(uint32 slot);`

```c
uint32 arkrtos_loader_state_driver(uint32 slot);
```

**查询驱动槽位状态**
- `slot`：驱动槽位号
**返回**：ARKRTOS_APP_SLOT_x；槽位非法返回 0xffffffff

### `int32 arkrtos_loader_load_dev_hdr(int32 dev, const arkrtos_app_header_`

```c
int32 arkrtos_loader_load_dev_hdr(int32 dev, const arkrtos_app_header_t *p_hdr, uint32 image_len);
```

**从设备流式加载（镜像头已由调用方读出）**
- `dev`：已打开的设备句柄，位置正好在镜像头之后
- `p_hdr`：已读出的镜像头（调用方已完成魔数同步）
- `image_len`：期望镜像总长（含头），传 0 表示由镜像头决定
**返回**：成功返回槽位号（>=0），失败返回 ARKRTOS_LOADER_ERR_x
本函数是**唯一的安装实现**，其余安装入口均为其薄包装。流程：
1) 校验头部（魔数 / 类型 / 兼容签名 / 长度 / 入口 / 重定位表）；
2) 在池内找一段足够大的已擦除空间（首适配，天然紧邻排列）；
3) 登记槽位（INSTALLING）；
4) 写入镜像头（state=UNCOMMITTED）；
5) 流式接收重定位表并落盘；
6) 流式接收负载，逐块按 delta 打补丁后落盘（每块回一个 ACK）；
7) CRC32 复核（头按 crc32/state 归零计算）；
8) 唯一的一个字写入：state = VALID，提交完成；
9) 置 LOADED 并清零故障计数，返回槽位号；自启标志不在本函数内
写入，由调用方（安装模块）按镜像头 flags 填槽位表。
供安装任务使用：先逐字节同步到镜像头魔数，再把头交本函数延续。

### `int32 arkrtos_loader_load_driver(int32 dev, uint32 image_len);`

```c
int32 arkrtos_loader_load_driver(int32 dev, uint32 image_len);
```

**从设备安装驱动镜像（自行读头）**
- `dev`：已打开的设备句柄，位置在镜像头之前
- `image_len`：期望镜像总长（含头），传 0 表示由镜像头决定
**返回**：成功返回驱动槽位号（>=0），失败返回 ARKRTOS_LOADER_ERR_x
> 说明：镜像头的 type 必须为 ARKRTOS_APP_TYPE_DRIVER。

### `int32 arkrtos_loader_load_driver_dev(int32 dev, const arkrtos_app_head`

```c
int32 arkrtos_loader_load_driver_dev(int32 dev, const arkrtos_app_header_t *p_hdr, uint32 image_len);
```

**从设备安装驱动镜像（镜像头已由调用方读出）**
**返回**：成功返回驱动槽位号（>=0），失败返回 ARKRTOS_LOADER_ERR_x
校验 hdr.type 为 DRIVER 后转交 arkrtos_loader_load_dev_hdr()。

### `int32 arkrtos_loader_on_fault(int32 task_id);`

```c
int32 arkrtos_loader_on_fault(int32 task_id);
```

**按“崩溃重启上限”策略处理 App 任务故障**
- `task_id`：发生故障的任务号
**返回**：1=已达上限并已禁用该 App，0=未达上限（调用方应恢复/重启该 App），
-1=不属于任何槽位（内核任务，调用方沿用默认处理）
计数按槽位累计（从分区表反查 task_id 归属）：故障一次加一，
达到 APP_CRASH_RESTART_MAX 后将该槽位置为 INVALID 并让任务脱离调度。
重新安装镜像时计数清零。
> 说明：计数由 arkrtos_crash 记账：共享 RAM 里的 slot_crash_cnt 只反映本次上电，
跨复位活下来的那份在共享 RAM 尾部的 UNINIT 区（CRASH_LOG_BASE）里，
开机由 arkrtos_crash_scan_apply() 采用，因此“崩溃导致整机复位”的启动环
也能被挡住（上限一到即保持禁用、不再自启）。只有掉电才清零。

### `int32 arkrtos_loader_start(uint32 slot);`

```c
int32 arkrtos_loader_start(uint32 slot);
```

**把已加载的槽位拉起为任务（按槽位类型自动分流 App / 驱动）**
- `slot`：槽位号
**返回**：成功返回任务号（>0），失败返回 ARKRTOS_LOADER_ERR_x
> 说明：这个门不接受被内核禁用的槽位：禁用的意思是“除非有人明确要求，
否则别再自己跑起来”。控制台的人工重试用 arkrtos_loader_start_manual()。

### `int32 arkrtos_loader_swap(uint32 old_slot, uint32 new_slot);`

```c
int32 arkrtos_loader_swap(uint32 old_slot, uint32 new_slot);
```

**App 的无缝升级：让 old_slot 里正在跑的任务改跑 new_slot 里的镜像**
- `old_slot`：正在运行（RUNNING）的槽位
- `new_slot`：已装好、未启动（LOADED）的槽位
**返回**：0=已挂上（在目标任务的下一次系统调用里落地）；负值为
ARKRTOS_LOADER_ERR_x（PARAM / STATE）或 ARKRTOS_SWAP_ERR_x
A/B 双 Flash 槽：新镜像由普通的 `app install` 写进第二个槽（它自带
一块自己的 RAM 块），切换在系统调用点就地完成，不重启任务、不擦写
Flash。旧的代码镜像与 RAM 块**保留**，旧槽退回 LOADED —— 那正是
A/B 的另一半：回滚就是把两者反过来再切一次。
> 说明：两块 RAM 必须一样大、槽位类型必须同类：整块搬运的前提。数据段不重置
（这正是「无缝」的含义），因此新版本必须能接受旧版本留下的数据布局。
> 说明：同时只允许一份在途，且与小程序的热切换共用一个内核挂起槽。

### `int32 arkrtos_loader_swap_take(void *p_exc_ctx, int32 task_id);`

```c
int32 arkrtos_loader_swap_take(void *p_exc_ctx, int32 task_id);
```

**App 热切换的落地钩子（由 SVC_Server 在分派之前调用）**
- `p_exc_ctx`：硬件异常栈帧（SVC 入口的 p_svc_ctx）
- `task_id`：当前任务号
**返回**：0=与本模块无关（或已拒绝并清账）；1=已切换（这一帧已是新镜像的入口）

### `int32 arkrtos_loader_check_install(const arkrtos_app_header_t *p_hdr);`

```c
int32 arkrtos_loader_check_install(const arkrtos_app_header_t *p_hdr);
```

**安装前的版本把关：同一 image_id 的已装副本不允许被「不更高」的版本覆盖**
- `p_hdr`：待安装镜像的镜像头（调用方已完成魔数同步与基本校验）
**返回**：0=可以安装；负值为 ARKRTOS_LOADER_ERR_x
三态判定，只按「同一 image_id」比对（image_id 是负载身份，
同一应用两次打包只有版本号不同、image_id 相同）：
- 版本号高于已装最高版本 -> 放行（升级）；
- 版本号相同且 image_size / crc32 也相同 -> ERR_DUP（重复副本）；
- 其余（降级回灌、同版本重建）-> ERR_VERSION。
只统计 state 为 LOADED / RUNNING 的副本：被崩溃终局策略禁用
（INVALID）的副本不算「已装」——重装同一版本正是恢复手段。
> 说明：安装路径在写入任何字节之前调用本函数；上电扫描与搬移不调用。

### `int32 arkrtos_loader_start_manual(uint32 slot);`

```c
int32 arkrtos_loader_start_manual(uint32 slot);
```

**控制台口径的启动：与 arkrtos_loader_start() 是同一道门，多一个人工重试**
- `slot`：槽位号
**返回**：成功返回任务号（>0），失败返回 ARKRTOS_LOADER_ERR_x
若该槽位是被崩溃终局策略禁用的，先用启动扫描的同一套校验
（arkrtos_loader_accept）复验镜像：镜像仍然可用才清掉禁用与计数
（这是操作者说“重新开始”）；复验不过就保持禁用并返回 ERR_STATE。
悄悄放行一个已经损坏的镜像，比它被禁用更糟。

### `int32 arkrtos_loader_stop(uint32 slot);`

```c
int32 arkrtos_loader_stop(uint32 slot);
```

**停止槽位对应的任务（镜像仍保留在 Flash）**
- `slot`：槽位号
**返回**：0=成功，负值为 ARKRTOS_LOADER_ERR_x

### `uint32 arkrtos_loader_state(uint32 slot);`

```c
uint32 arkrtos_loader_state(uint32 slot);
```

**查询槽位状态**
- `slot`：槽位号
**返回**：ARKRTOS_APP_SLOT_x；槽位非法返回 0xffffffff

### `uint32 arkrtos_loader_start_autostart_driver(void);`

```c
uint32 arkrtos_loader_start_autostart_driver(void);
```

**按各驱动槽的自启标志批量启动驱动**
**返回**：实际启动的驱动数量
自启标志在扫描时由镜像头 flags 回填（裸镜像取开发槽位表配置）。
未设自启的驱动不会被拉起，需由上层（如 shell 的 drv start）显式启动。
必须在 arkrtos_loader_scan_driver() 之后调用。

### `uint32 arkrtos_loader_start_autostart(void);`

```c
uint32 arkrtos_loader_start_autostart(void);
```

**按各 App 槽的自启标志批量启动 App**
**返回**：实际启动的 App 数量
与 arkrtos_loader_start_autostart_driver() 同口径。
必须在 arkrtos_loader_scan() 之后调用。

### `int32 arkrtos_loader_check_header(const arkrtos_app_header_t *p_hdr, c`

```c
int32 arkrtos_loader_check_header(const arkrtos_app_header_t *p_hdr, const uint32 *p_rel, uint32 types_mask);
```

**校验镜像头：字段级约束 + 重定位表表体合法性**
- `p_hdr`：镜像头（调用方已读进可寻址内存）
- `p_rel`：重定位表表体地址；传 0 表示表体此刻还不在可寻址的地方，
此时只校字段，调用方必须在表体可用后补一次带地址的调用
- `types_mask`：接受的镜像类型位掩码（`ARKRTOS_APP_TYPE_MASK_x`）
**返回**：0=通过，负值为 ARKRTOS_LOADER_ERR_x
> 说明：表体校验覆盖：条目数上限、编码版本、`payload_offset` 自洽、每个条目的
对齐要求与「落在负载范围内」、以及偏移必须升序。这些规则只能有一份，
否则「池内安装」与「小程序装载」会对同一个文件给出不同判定。

### `uint32 arkrtos_loader_reloc_apply(uint8 *buf, uint32 buf_off, uint32 l`

```c
uint32 arkrtos_loader_reloc_apply(uint8 *buf, uint32 buf_off, uint32 len, const arkrtos_app_header_t *p_hdr, const uint32 *p_rel, uint32 *p_idx, uint32 delta_rom, uint32 delta_ram);
```

**给一段负载打重定位补丁，并给出本段可安全落盘的末尾**
- `buf`：装载缓冲（对应镜像内偏移 buf_off 起的连续 len 字节）
- `buf_off`：该缓冲在**镜像内**的起始偏移（不是内存地址）
- `len`：缓冲长度
- `p_hdr`：镜像头（提供表项数与两条标称基址）
- `p_rel`：重定位表表体（可以是整表，也可以是一段滑动窗口，只要
`p_rel[idx]` 是第 idx 条全局条目）
- `p_idx`：表前进指针（跨调用必须复用同一个变量）
- `delta_rom`：给 ROM 类表项叠加的增量
- `delta_ram`：给 RAM 类表项叠加的增量
**返回**：本段可以落盘的末尾偏移（镜像内）。正常等于 buf_off + len；若段尾
恰好切开一条 8 字节指令类表项，则等于该表项起始偏移，调用方必须把
这段之后的字节留到下一段开头一起处理。
> 说明：表项按偏移升序，所以只需要一个前进指针；但返回的"可落盘末尾"不是
可选的：忽略它会让被切开的表项在下一次调用里落到
「偏移落在本段之前」那条防御分支上而**静默丢掉一次修补**。

### `uint32 arkrtos_loader_crc_header(const arkrtos_app_header_t *p_hdr);`

```c
uint32 arkrtos_loader_crc_header(const arkrtos_app_header_t *p_hdr);
```

**算镜像头部（前 256 字节）的 CRC，供流式装载方接续计算**
- `p_hdr`：镜像头（调用方已读进可寻址内存）
**返回**：只覆盖头部 256 字节的 CRC 值（crc32 / state / runtime_ram_base 三个
字段按四个 0 字节代入）
> 说明：完整镜像 CRC = 本值继续喂入「重定位表」与「负载」两段**标称形态**的字节。
小程序的负载是边读边落 RAM、边打补丁的，所以只能在收到字节的那一刻
算 CRC，不能在打完补丁之后再回读——回读的是 relocation 之后的形态，
与打包工具算的标称形态必然不同。

### `#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1) int32 arkrtos_loader_crc_rech`

```c
#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1) int32 arkrtos_loader_crc_recheck(const uint8 *p_img, uint32 base, uint32 cap, uint32 *p_got);
```

**反算标称形态后复核镜像 CRC（开机/巡检用，SEC-G17）**
- `p_img`：镜像在 Flash 中的起始地址（必须等于 base）
- `base`：镜像落点（槽位基址），用于反算 ROM 类表项的增量
- `cap`：该槽位可读字节数（0 = 不检查）；小于镜像总长时返回 ERR_SIZE
- `p_got`：出参：按标称形态重算出来的 CRC（失败时也会被写，便于报数）
**返回**：0 = 与头里的 crc32 一致；ARKRTOS_LOADER_ERR_CRC = 不一致（真被改坏）；
其他负值为"算不出来"（头字段不成立 / runtime_ram_base 未固化 / 表项退化）
Flash 上躺着的是**打过重定位补丁**的字节，而头里的 crc32 覆盖的是
打包时的标称形态。本函数把两个增量（ROM 与 RAM）从头部字段与当前落点
反算出来，逐表项减回去，再按与打包工具同一套公式重算 CRC 并与头里的
值比对。因此它能在**安装之后**发现静默损坏，而不只是在安装期。
> 说明：返回 ERR_CRC 才是"不合格"；返回其他负值说明这一份镜像当前
无法复核（例如 runtime_ram_base 尚未固化），调用方必须把它
报成"没看到"而不是 PASS —— 两者混同就是假绿。
> 说明：反算沿用打包工具已证明的性质（表项升序、互不重叠）与 32 位
加减互逆，不需要任何额外的落盘记账，因此不改镜像头 ABI。
> 说明：只有 ARKRTOS_USE_IMAGE_CRC_RECHECK 打开时才编入（默认关）。

### `void arkrtos_loader_halt_image(uint32 task_id);`

```c
void arkrtos_loader_halt_image(uint32 task_id);
```

**硬停同一 RAM 窗口内的所有任务（镜像级停止）**
- `task_id`：该镜像的主任务号（1 起）
> 说明：RAM 窗口就是归属判据：镜像的代码块/数据块由装载方交给主任务，
它自己创建的线程其栈必须落在同一块内（由内核的创建校验保证），
内核内置任务住在内核 RAM 区，永远不会匹配。
池内卸载与小程序停止必须用同一份判据，不能各写一套。

## kernelsrc/include/arkrtos_log.h

**Ark RTOS 内核日志：分级输出 + 运行期过滤（内核侧接口）**
设计沿用 mcu_framework 的 log 组件思路，并补上 Ark RTOS 需要的一环：
- 级别：NONE(0) < ERROR(1) < WARNING(2) < INFO(3) < DEBUG(4)，
数值越大越啰嗦（定义见 arkrtos_log_defs.h，与用户态共用）。

### `void   arkrtos_log_init(void);`

```c
void   arkrtos_log_init(void);
```

**日志模块初始化（打开控制台串口，装载配置里的初始级别）**
需在设备框架初始化之后、调度启动之前调用；重复调用无害。

### `void   arkrtos_log_set_level(uint32 level);`

```c
void   arkrtos_log_set_level(uint32 level);
```

**设置运行期日志级别**
- `level`：ARKRTOS_LOG_NONE..ARKRTOS_LOG_DEBUG
> 说明：超出范围的值不改变当前级别（避免误配导致全体静音）。

### `uint32 arkrtos_log_get_level(void);`

```c
uint32 arkrtos_log_get_level(void);
```

**取当前运行期日志级别**

### `void   arkrtos_log_emit(uint32 level, const char *tag, uint32 line, co`

```c
void   arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg);
```

**输出一条已完成格式化的日志（内核内部与 SVC 0x19 共用此出口）**
- `level`：级别；高于当前运行期级别时直接丢弃
- `tag`：标签（短字符串，通常是大写模块名）
- `line`：源码行号；用户态传 0 时不打印行号
- `msg`：正文（已格式化，为空时直接返回）

### `int32  arkrtos_log_svc(uint32 level, const char *tag, const char *msg)`

```c
int32  arkrtos_log_svc(uint32 level, const char *tag, const char *msg);
```

**SVC 0x19 的服务入口（用户态 App / 驱动调用）**
- `level`：级别
- `tag`：标签字符串地址（已由分发层校验）
- `msg`：正文地址（已由分发层校验）
**返回**：0=已处理（含被级别过滤），-1=参数非法

### `int32  arkrtos_console_handle(void);`

```c
int32  arkrtos_console_handle(void);
```

**Console device handle, negative while the console is not open**

### `int32  arkrtos_console_is_handle(int32 handle);`

```c
int32  arkrtos_console_is_handle(int32 handle);
```

**Is this handle the console device?**
**返回**：1 = yes, 0 = no

### `int32  arkrtos_console_write(const uint8 *p_data, uint32 len);`

```c
int32  arkrtos_console_write(const uint8 *p_data, uint32 len);
```

**Write one complete output unit atomically**
- `p_data`：byte stream, need not be NUL terminated
- `len`：byte count
**返回**：bytes accepted, -1 when the console is not open

### `uint32 arkrtos_console_tx_drop_count(void);`

```c
uint32 arkrtos_console_tx_drop_count(void);
```

**Console bytes the writer gave up on (real output loss)**
Counts bytes that were still not accepted after the whole wait
budget, i.e. output that never left the MCU.  Distinguishes a
retry (FIFO full, room appeared) from a loss.  Non zero means
the transmit pipe stayed full for the entire budget; raise
ARKRTOS_CONSOLE_TX_WAIT or slow the producers down.

### `int32  arkrtos_console_lock(void);`

```c
int32  arkrtos_console_lock(void);
```

**Take the console across several write calls**
For composite output such as colour escape + prompt + reset.
**返回**：1 = held, 0 = console stayed busy and the caller must not release

### `void   arkrtos_console_unlock(void);`

```c
void   arkrtos_console_unlock(void);
```

**Release the console**
> 说明：Must only be called when arkrtos_console_lock() returned 1.

### `uint32 arkrtos_console_busy_count(void);`

```c
uint32 arkrtos_console_busy_count(void);
```

**How many times a console unit was written without the lock**
Non zero means the console was busy for longer than the bounded
spin budget; output is intact but may have been spliced.  A
diagnostic counter, not an error path.

### `uint32 arkrtos_log_ring_capacity(void);`

```c
uint32 arkrtos_log_ring_capacity(void);
```

**日志环容量（字节）；0 表示环被编译期关闭**

### `uint32 arkrtos_log_ring_used(void);`

```c
uint32 arkrtos_log_ring_used(void);
```

**环内当前有效的字节数**

### `uint32 arkrtos_log_ring_dump(void (*put)(const char *data, uint32 len,`

```c
uint32 arkrtos_log_ring_dump(void (*put)(const char *data, uint32 len, void *arg), void *arg);
```

**把环内容按「最旧→最新」顺序交给回调（可能分两段调用，跨越环尾）**
- `put`：回调：data/len 为一段连续字节（不保证 NUL 结尾）
- `arg`：透传给回调
**返回**：交给回调的总字节数（= arkrtos_log_ring_used()）

### `void   arkrtos_log_ring_clear(void);`

```c
void   arkrtos_log_ring_clear(void);
```

**清空日志环**

## kernelsrc/include/arkrtos_mempool.h

**Ark RTOS 固定块内存池模块（内核内部头文件）**
把一段**由调用者提供**的内存切成 N 个等长块，分配与释放都是 O(1)：
空闲块用侵入式单链表串起来（空闲块自己的前几个字节存 next），
所以池本身不额外占 RAM，也不需要堆。没有外部碎片，回收不合并。

### `typedef arkrtos_mpool_stat_t arkrtos_mempool_stat_t;`

```c
typedef arkrtos_mpool_stat_t arkrtos_mempool_stat_t;
```

**池的只读快照，供 shell / 自检使用（定义在 arkrtos.h，两侧共用一份） */**

### `void  arkrtos_mempool_module_init(void);`

```c
void  arkrtos_mempool_module_init(void);
```

**对象表初始化；由 arkrtos_kernel_init() 调用 */**

### `int32 arkrtos_mempool_create_internal(const char *name, void *pool, ui`

```c
int32 arkrtos_mempool_create_internal(const char *name, void *pool, uint32 block_size, uint32 block_count);
```

**在一个调用者提供的内存区上创建池**
- `name`：池名，非空，长度 < ARKRTOS_MEMPOOL_NAME_MAX
- `pool`：内存区首地址（调用者保证其生命周期不短于池）
- `block_size`：每块字节数，必须 >= sizeof(void*) 且按指针宽度对齐
- `block_count`：块数，必须 > 0
**返回**：>=0 句柄；-1 参数非法 / 名字重复 / 对象表已满

### `void *arkrtos_mempool_alloc_internal(int32 handle);`

```c
void *arkrtos_mempool_alloc_internal(int32 handle);
```

**取一块；无空闲块时返回 0（不阻塞）**
**返回**：块指针；0 表示当前无空闲块或句柄非法

### `int32 arkrtos_mempool_free_internal(int32 handle, void *p);`

```c
int32 arkrtos_mempool_free_internal(int32 handle, void *p);
```

**归还一块**
**返回**：0 = 归还成功；-1 = 句柄非法或指针不属于本池（池状态不变）

### `int32 arkrtos_mempool_delete_internal(int32 handle);`

```c
int32 arkrtos_mempool_delete_internal(int32 handle);
```

**销毁池（对象表项释放；调用者提供的内存区不受影响） @return 0/-1 */**

### `int32 arkrtos_mempool_is_alive(int32 handle);`

```c
int32 arkrtos_mempool_is_alive(int32 handle);
```

**1 = 句柄指向活着的池（不是仅落在下标范围内） */**

### `int32 arkrtos_mempool_query_internal(int32 handle, arkrtos_mempool_sta`

```c
int32 arkrtos_mempool_query_internal(int32 handle, arkrtos_mempool_stat_t *out);
```

**取池快照 @return 0 = 成功；-1 = 句柄非法 */**

### `void  arkrtos_mempool_release_task(int32 task_id);`

```c
void  arkrtos_mempool_release_task(int32 task_id);
```

**回收某个任务创建的池（任务下线时由内核调用）**
> 说明：只释放对象表项；池占用的内存区属于调用者，不在这里释放。
内核/无人认领（creator_id==0）的池不会被任何任务下线带走。

## kernelsrc/include/arkrtos_mini.h

**Ark RTOS 小程序（MiniApp）：文件系统里的负载按需 load 进 RAM 执行**
小程序与 App 的关系，一句话说清：
- 容器相同：同一种镜像（256 字节头 + 重定位表 + 负载），同一种入口约定，
同一个 App SDK（oslib 直接复用）。差别只在镜像头里的 type 字段。

### `int32 arkrtos_mini_last_read_error(void);`

```c
int32 arkrtos_mini_last_read_error(void);
```

**最近一次读镜像时底层给出的错误码（0 = 无错误）**
取值是读失败那一层（由 ARKRTOS_MINI_READ_VFS 决定是 ark_vfs 还是
arkrtos_fs）的错误码，或 ARKRTOS_MINI_ERR_SHORT_READ。只在
arkrtos_mini_run() 返回 ARKRTOS_MINI_ERR_FS 时有意义。

### `typedef struct {`

```c
typedef struct {
```

**一次小程序装载的结果（arkrtos_mini_info() 的输出） */**

### `int32 arkrtos_mini_run(const char *path);`

```c
int32 arkrtos_mini_run(const char *path);
```

**从文件系统装载并启动一个小程序**
- `path`：串口侧路径：文件系统卷内的绝对路径，如 "/mini/hello.svcm"
**返回**：0=已启动；负值为 ARKRTOS_LOADER_ERR_x、ARKRTOS_MINI_ERR_FS 或
ARKRTOS_MINI_ERR_DISABLED（这个程序崩溃到上限，已被禁用）
> 说明：同一时刻能跑几个由 ARKRTOS_MINI_MAX（编译期上限）与运行期收紧值共同
决定；项满了报 BUSY，**不会**挤掉正在跑的那个、也不会静默共用一个块。
> 说明：装载本身是串行的：装载期间持有调度器锁（与 SVC 层访问文件系统同一
套约定），文件系统只有一份缓存，两个任务交错读镜像会把彼此的窗口
顶偏。所以「多个」是**同时运行**多个，不是同时装载多个。

### `int32 arkrtos_mini_stop(void);`

```c
int32 arkrtos_mini_stop(void);
```

**停止**全部**在跑的小程序并归还它们的 RAM 块**
**返回**：0=至少停掉了一个；ARKRTOS_LOADER_ERR_STATE=本来就没有在跑
> 说明：逐个停（shell 想只停某一个用 arkrtos_mini_stop_at）
停止顺序不能反：先把镜像（含它自己创建的线程）踢出调度，再归还块。
反过来的话，块可能立刻被下一个装载者拿去写代码，而原任务还在跑。

### `int32 arkrtos_mini_info(arkrtos_mini_info_t *p_out);`

```c
int32 arkrtos_mini_info(arkrtos_mini_info_t *p_out);
```

**读出第一个在跑的小程序的状态（兼容旧调用）**
**返回**：0=有在跑的（*p_out 有效）；-1=没有在跑（*p_out 已清零）
> 说明：要列出全部用 arkrtos_mini_running() + arkrtos_mini_info_at()。

### `uint32 arkrtos_mini_running(void);`

```c
uint32 arkrtos_mini_running(void);
```

**当前在跑的小程序个数（0 = 没有）**
**返回**：运行中的实例数
> 说明：这是「密集编号」的上界：编号 0 .. 返回值-1 都可用 arkrtos_mini_info_at 查到
（编号按实例池顺序排列，中间不会因为停下来而空号）。

### `int32 arkrtos_mini_info_at(uint32 index, arkrtos_mini_info_t *p_out);`

```c
int32 arkrtos_mini_info_at(uint32 index, arkrtos_mini_info_t *p_out);
```

**按下标读一个小程序的状态**
- `index`：0 .. arkrtos_mini_running()-1（密集编号，不是实例池下标）
**返回**：0=*p_out 有效；-1=没有这么多在跑的

### `int32 arkrtos_mini_stop_at(uint32 index);`

```c
int32 arkrtos_mini_stop_at(uint32 index);
```

**停止其中一个（按 arkrtos_mini_info_at 的同一个编号）**
**返回**：0=已停止；ARKRTOS_LOADER_ERR_STATE=这个编号没有在跑的

### `uint32 arkrtos_mini_max_count(void);`

```c
uint32 arkrtos_mini_max_count(void);
```

**读 / 写「同一时刻最多跑几个」的运行期上限**
**返回**：arkrtos_mini_set_max_count: 0=已生效，ARKRTOS_LOADER_ERR_PARAM=超出 [1, ARKRTOS_MINI_MAX]
上限不能在运行期放宽到超过编译期的 ARKRTOS_MINI_MAX：那是个会把
「说好了 8 个」变成一句看不出来的谎话的设置（结构体、共享表数组、
每次崩溃记账的容量都是按编译期值算的），所以超范围直接报错。

### `int32 arkrtos_mini_forget(const char *path);`

```c
int32 arkrtos_mini_forget(const char *path);
```

**清除某个路径的崩溃记忆（让被禁用的小程序重新可跑）**
- `path`：与 arkrtos_mini_run 同一个路径
**返回**：0=已清；-1=路径为空

### `int32 arkrtos_mini_on_fault(int32 task_id);`

```c
int32 arkrtos_mini_on_fault(int32 task_id);
```

**故障钩子（由 arkrtos_loader_on_fault 对「没有槽位的任务」调用）**
**返回**：1=这是某个小程序的主任务，已按小程序的规则记账（可能已禁用）；
0=与本模块无关，调用方按原来的默认故障处理走

### `uint32 arkrtos_mini_max_bytes(void);`

```c
uint32 arkrtos_mini_max_bytes(void);
```

**读出「单个小程序能占用的最大块字节数」这个运行期上限**
**返回**：当前上限（字节，2 的幂）
默认值就是编译期的 SLOT_RAM_MAX_BLOCK。这个上限**可以在运行期收紧**
（shell: mini limit <字节>），但**不能放宽到超过 SLOT_RAM_MAX_BLOCK**：
那个常量同时还是池单块物理上限、App 镜像校验上限与 MPU 能给出的数据
窗口上限（ARKRTOS_MPU_RAM_BLOCK_MAX）。往上放会掉进 arkrtos_mpu.c 的
「内核 RAM 兜底窗口」分支——那是一种会静默把任务放进错误窗口的失败，
不应该由一次调参触发。要真正放开天花板，得改 SLOT_RAM_MAX_BLOCK
并同时满足池与 MPU 的静态断言。

### `int32 arkrtos_mini_set_max_bytes(uint32 bytes);`

```c
int32 arkrtos_mini_set_max_bytes(uint32 bytes);
```

**设置单个小程序的体积上限（块的字节数）**
- `bytes`：2 的幂，范围 [SLOT_RAM_MIN_BLOCK, SLOT_RAM_MAX_BLOCK]；0 = 恢复编译期默认
**返回**：0=已生效；ARKRTOS_LOADER_ERR_PARAM=不在范围内或不是 2 的幂（不静默夹取）
> 说明：只影响**下一次**装载；已经在跑的小程序不受影响（它的块已经借出去了）。

### `int32 arkrtos_mini_task_is_main(uint32 task_id);`

```c
int32 arkrtos_mini_task_is_main(uint32 task_id);
```

**判断 task_id 是否某个『正在跑』小程序的主任务（只读）**
- `task_id`：任务号
**返回**：1=是某个 RUN 状态小程序的主任务；0=不是
给硬件安全态机制用：小程序主任务「正常退出」是无缝升级的交接，
此刻只摘设备台账、不驱动硬件（见 ARKRTOS_MINI_EXIT_IS_HANDOVER）。
必须在 arkrtos_mini_on_task_exit 之前调用——后者会把状态改成 LOAD。

### `typedef int (*arkrtos_mini_auto_cb_t)(const char *path, void *arg);`

```c
typedef int (*arkrtos_mini_auto_cb_t)(const char *path, void *arg);
```

**遍历自启清单（只读）；callback 返回 0 继续、-1 提前结束**
- `cb`：每个条目调一次，path 是已去掉首尾空白的路径（'#' 注释与空行不回调）
**返回**：0=遍历完成；-1=清单读不了（不存在也是 -1，见 arkrtos_mini_autostart_now）
超长（> ARKRTOS_MINI_PATH_MAX-1）的条目**不截断也不回调**：截断会让两个
不同路径共享同一个身份，崩溃记账会串号。该条目会被记一条错误日志后跳过。

### `int32 arkrtos_mini_autostart_now(void);`

```c
int32 arkrtos_mini_autostart_now(void);
```

**按自启清单启动一遍**
**返回**：>=0=成功启动的个数；ARKRTOS_MINI_ERR_FS=清单读不了
> 说明：逐条调 arkrtos_mini_run()，所以并发上限、崩溃禁用、体积上限这些判据
与手工 `mini run` 完全同一条路径，不另立一套。

### `uint32 arkrtos_mini_boot_autostart(void);`

```c
uint32 arkrtos_mini_boot_autostart(void);
```

**卷刚挂上时的自启入口（内核在挂载成功后调用）**
**返回**：本次真正启动的个数；本次开机已经做过（或清单读不到）则为 0
只在本次开机的**第一次成功挂载**上做一次。小程序的可执行字节就在这个
卷里，所以「卷可用」是它唯一能起来的时刻；内核自己不去猜哪个设备上有
文件系统（那是板级知识），挂载由上层发起——谁把它挂上，就在这里被带上。

### `int32 arkrtos_mini_autostart_set(const char *path, uint32 on);`

```c
int32 arkrtos_mini_autostart_set(const char *path, uint32 on);
```

**把 path 加进（on!=0）/移出（on==0）自启清单**
- `path`：与 `mini run` 同一种写法（必须以 '/' 开头的卷内路径）
**返回**：0=已生效（含「本来就是这样、未改动」）；ARKRTOS_LOADER_ERR_PARAM=路径不合法；
ARKRTOS_LOADER_ERR_NOSPACE=清单文件装不下；ARKRTOS_MINI_ERR_FS=写盘失败
> 说明：空改动不落盘：已经在清单里再加、或本来就不在清单里再删，都不重写文件。

### `int32 arkrtos_mini_swap_at(uint32 index, const char *path);`

```c
int32 arkrtos_mini_swap_at(uint32 index, const char *path);
```

**小程序的无缝升级：把第 index 个在跑的小程序换成 path 这份镜像**
- `index`：与 arkrtos_mini_info_at / arkrtos_mini_stop_at 同一个密集编号
- `path`：新镜像（文件系统内的路径，写法与 mini run 相同）
**返回**：0=已挂上（在目标任务的下一次系统调用里落地）；负值为
ARKRTOS_LOADER_ERR_x / ARKRTOS_MINI_ERR_x / ARKRTOS_SWAP_ERR_LAYOUT
A/B 双缓冲：新版本装进**第二对**块，旧的一对在切换落地那一刻才还池，
因此峰值是两套（不是三套）。切换不重启任务、不进一次调度、不写 Flash。
新旧的代码块大小 / RAM 块大小 / 镜像头 RAM 需求必须全等：整块搬运的
前提。布局不一致的升级只能走交接协议（docs/无缝升级例程.md）。
> 说明：同时只允许一份在途（能排队第二份就等于承认需要第三份）。装载期间与
`mini run` 共用同一道串行闸门，所以两者不会同时进行。

### `int32 arkrtos_mini_swap_take(void *p_exc_ctx, int32 task_id);`

```c
int32 arkrtos_mini_swap_take(void *p_exc_ctx, int32 task_id);
```

**热切换的落地钩子（由 SVC_Server 在分派之前调用）**
- `p_exc_ctx`：硬件异常栈帧（SVC 入口的 p_svc_ctx）
- `task_id`：当前任务号
**返回**：0=与本模块无关（或已拒绝并清账）；1=已切换（这一帧已是新镜像的入口）

## kernelsrc/include/arkrtos_modld.h

**VxWorks 式可下载模块：把 ET_REL 目标文件在板上现场链接再跑起来**
与 arkrtos_elf.h 是**两种应用模型**，不是同一件事的两步：
|            | Linux 式（exec）        | VxWorks 式（ld）           |
|------------|-------------------------|----------------------------|

### `typedef struct {`

```c
typedef struct {
```

**一次装载的可读结果（全部是实测值，不是推算值） */**

### `int32 arkrtos_mod_load(const char *path, int32 argc, char *argv[]);`

```c
int32 arkrtos_mod_load(const char *path, int32 argc, char *argv[]);
```

**装载一个 ET_REL 模块并起跑它的入口**
- `path`：VFS 路径（如 "/mnt/ram/modhello.o"）
- `argc`：传给 module_main 的参数个数（含 argv[0]）；0 表示自动补 1 个
- `argv`：参数数组；0 时按 argc=0 处理
**返回**：0 = 已装并起跑；负值 = ARKRTOS_MOD_ERR_*
> 说明：起跑后立刻返回（不等待模块结束）。要等结束由调用者按 task 号看。

### `int32 arkrtos_mod_count(void);`

```c
int32 arkrtos_mod_count(void);
```

**当前驻留的模块个数 */**

### `int32 arkrtos_mod_info(int32 index, arkrtos_mod_info_t *p_out);`

```c
int32 arkrtos_mod_info(int32 index, arkrtos_mod_info_t *p_out);
```

**取第 index 个驻留模块（index 是「第几个」，不是槽号） @return 0/-1 */**

### `int32 arkrtos_mod_unload(int32 index);`

```c
int32 arkrtos_mod_unload(int32 index);
```

**卸掉一个模块（按装载顺序的下标） @return 0 / ARKRTOS_MOD_ERR_* */**

### `const char *arkrtos_mod_error_name(int32 rc);`

```c
const char *arkrtos_mod_error_name(int32 rc);
```

**返回码的短名（只用于打印，不参与判定） */**

### `const char *arkrtos_mod_missing_symbol(void);`

```c
const char *arkrtos_mod_missing_symbol(void);
```

**最近一次装载失败缺失的外部符号名**
**返回**：符号名；空串表示上次失败不是符号问题
> 说明：只在 ARKRTOS_MOD_ERR_SYMBOL 之后有意义，其余情况返回空串。

### `uint32 arkrtos_mod_bad_reloc(void);`

```c
uint32 arkrtos_mod_bad_reloc(void);
```

**最近一次装载失败遇到的重定位类型号**
**返回**：LoongArch 重定位类型号；0 表示上次失败不是重定位问题

## kernelsrc/include/arkrtos_mpu.h

**Ark RTOS memory protection manager (kernel internal interface)**
Kernel code calls only this layer; all MPU register access lives in
the port layer (arkrtos_port_mpu_*). Porting to a new architecture
therefore only needs a new port implementation.

### `int32 arkrtos_paging_map_span(uint32 va, uint32 pa, uint32 size, arkrt`

```c
int32 arkrtos_paging_map_span(uint32 va, uint32 pa, uint32 size, arkrtos_mpu_mem_t mem);
```

**Register a run-time dynamic mapping (4 KB granularity)**
- `va`：Virtual base, must be 4 KB aligned
- `pa`：Physical base, must be 4 KB aligned
- `size`：Bytes, must be a non-zero multiple of 4 KB and at most
ARKRTOS_PAGING_MAP_MAX_PAGES * 4 KB
- `mem`：One of ARKRTOS_MPU_MEM_RAM / _ROM / _PERIPH_RO / _PERIPH_RW
**返回**：0 = every page is in the TLB and in the audit span list;
<0 = refused, with the port's own reason passed through
(-1 parameter/attribute, -2 overlaps a static span, -3 table full)
This is the driver-side "register this MMIO region" entry. It is
deliberately restricted to kernel-visible mappings: App pages need
their own ASID and per-process tables, which is not built yet, and
pretending otherwise here would be an authoritative-looking wrong
answer. A failed call does not roll back the pages that already
went in - there is no unmap path yet (debt U-15), and the count
from arkrtos_paging_map_pages() is how a caller sees how far it got.

### `uint32 arkrtos_paging_map_pages(void);`

```c
uint32 arkrtos_paging_map_pages(void);
```

**How many pages this layer has registered (cumulative readout) */**

### `int32 arkrtos_paging_unmap_span(uint32 va, uint32 size);`

```c
int32 arkrtos_paging_unmap_span(uint32 va, uint32 size);
```

**Undo pages registered through arkrtos_paging_map_span() (U-15)**
- `va`：Base of the span, must be 4 KB aligned
- `size`：Bytes, must be a non-zero multiple of 4 KB
**返回**：0 = every page is gone; <0 = the port's own reason for the page
that failed (-1 parameter, -2 static span, -4 not registered)
Only pages registered through map_span() can be taken out: static
spans are the kernel's own and the port refuses them. A span that
was only partly mapped reports the reason instead of a "done". */

### `uint32 arkrtos_paging_dyn_used(void);`

```c
uint32 arkrtos_paging_dyn_used(void);
```

**How many dynamic mapping slots are in use right now (readout) */**

### `int32 arkrtos_copy_from_user(void *dst, const void *usrc, uint32 len);`

```c
int32 arkrtos_copy_from_user(void *dst, const void *usrc, uint32 len);
```

**Copy from a user buffer into a kernel buffer**
- `dst`：Kernel destination
- `usrc`：User source (validated against the calling task's windows)
- `len`：Bytes
**返回**：0 = copied; -1/-2/-3/-4 as listed above
The caller is arkrtos_task_get_current(). On a build where the
calling task is a kernel task there is no user window, and the
answer is -2 instead of an unchecked memcpy. */

### `int32 arkrtos_copy_to_user(void *udst, const void *src, uint32 len);`

```c
int32 arkrtos_copy_to_user(void *udst, const void *src, uint32 len);
```

**Copy from a kernel buffer into a user buffer**
**返回**：0 = copied; -1/-2/-3/-4 as listed above */

### `int32 arkrtos_copy_from_user_task(arkrtos_task_t *p_task, void *dst, c`

```c
int32 arkrtos_copy_from_user_task(arkrtos_task_t *p_task, void *dst, const void *usrc, uint32 len);
```

**Same, for a task named explicitly (kernel internal)**
Used where the kernel works on behalf of a task that is not the
current one - a loader writing a program's argument block, for
instance. The caller must have installed that task's address
space first; if it did not, the copy is refused with -4 instead
of quietly reaching into whatever frame is installed. */

### `int32 arkrtos_paging_user_copy(unsigned long rom_start, uint32 rom_siz`

```c
int32 arkrtos_paging_user_copy(unsigned long rom_start, uint32 rom_size, unsigned long ram_start, uint32 ram_size, uint32 space, uint32 installed, void *kern, const void *user, uint32 len, uint8 write);
```

**The check-and-copy body, with the task's windows passed as numbers**
- `rom_start/rom_size`：Code window (read only), 0 when there is none
- `ram_start/ram_size`：Data window (read/write), 0 when there is none
- `space`：Address space the task is bound to
- `installed`：Space currently installed in the App window
- `kern`：Kernel side buffer
- `user`：User side address
- `len`：Bytes
- `write`：1 = user is the destination, 0 = user is the source
**返回**：0 = copied; -1/-2/-3/-4 as listed above
This exists so the QEMU self-test can exercise every refusal
without manufacturing a fake TCB: the guards are the same code
the task-based entry points run, only the numbers arrive
directly. Kernel internal; Apps never see it. */

### `uint32 arkrtos_paging_space_count(void);`

```c
uint32 arkrtos_paging_space_count(void);
```

**How many address spaces the board provides (>= 1) */**

### `uint32 arkrtos_paging_space_frame(uint32 space);`

```c
uint32 arkrtos_paging_space_frame(uint32 space);
```

**Physical frame base behind a space; 0 when the space does not exist */**

### `unsigned long arkrtos_paging_window_va(void);`

```c
unsigned long arkrtos_paging_window_va(void);
```

**Virtual base of the App window (the same VA for every space)**
**返回**：the window VA base; 0 when the build has no paging backend
Use this - not arkrtos_paging_space_frame() - when poking the
window by address.  With ARKRTOS_USE_PAGING_PAGES=1 the frames come
from the frame pool, so a frame base is no longer a VA. */

### `uint32 arkrtos_paging_space_current(void);`

```c
uint32 arkrtos_paging_space_current(void);
```

**Which space is currently installed in the App window */**

### `int32 arkrtos_paging_space_activate(uint32 space);`

```c
int32 arkrtos_paging_space_activate(uint32 space);
```

**Install a space right now (kernel-side, e.g. before writing a load)**
**返回**：0 = installed or already there; -1 = bad space / no TLB path
The scheduler also does this on every switch to an App, from the
task's binding. Calling it from a kernel task is what lets a loader
write into a frame the kernel is not currently "in". */

### `int32 arkrtos_paging_bind_task(arkrtos_task_t *p_task, uint32 space);`

```c
int32 arkrtos_paging_bind_task(arkrtos_task_t *p_task, uint32 space);
```

**Bind a task to an address space (default for every task is 0)**
**返回**：0 = bound; -1 = bad task or bad space
Only App tasks (is_priv == 0) trigger a switch on schedule; a
kernel task leaves whatever space is installed alone, because
kernel access to a frame is a deliberate, momentary thing. */

### `uint32 arkrtos_paging_space_of(arkrtos_task_t *p_task);`

```c
uint32 arkrtos_paging_space_of(arkrtos_task_t *p_task);
```

**Space a task is bound to (0 when unknown) */**

### `int32 arkrtos_paging_space_clone(uint32 src, uint32 dst);`

```c
int32 arkrtos_paging_space_clone(uint32 src, uint32 dst);
```

**把一个空间的整份窗口内容复制到另一个空间（U-19 fork 的落地）**
- `src`：父空间，@param dst 子空间（必须空着）
**返回**：0 / -1 参数 / -2 dst 已有帧 / -3 帧池拒绝（已回滚）
纯搬运，不切窗口；只在按需分页的 LoongArch64 端口上实现。
开关条件（PAGING_PAGES + PAGING_DEMAND）由 features.h #error 把住。 */

### `int32 arkrtos_paging_space_release(uint32 space);`

```c
int32 arkrtos_paging_space_release(uint32 space);
```

**Give an address space's frames back to the pool (U-3 leftover)**
- `space`：0 .. arkrtos_paging_space_count()-1
**返回**：0 = returned (also when it never took any); -1 = bad space;
-2 = no frame pool behind the window; -3 = refused because the
caller is standing inside the window VA; -4 = pool refused
Call it only when no App task is alive in that space: the window
entries of that space are dropped first, so anyone still running
there would fault on its next fetch. The space itself stays
(space_count does not change); the next activate() asks for fresh
frames again, which is what makes repeated exec runs stop draining
the pool. */

### `int32 arkrtos_paging_frame_alloc(uint32 count, uint32 *p_base);`

```c
int32 arkrtos_paging_frame_alloc(uint32 count, uint32 *p_base);
```

**Allocate a run of contiguous 4 KB page frames */**

### `int32 arkrtos_paging_frame_free(uint32 base, uint32 count);`

```c
int32 arkrtos_paging_frame_free(uint32 base, uint32 count);
```

**Return a run of contiguous 4 KB page frames (all-or-nothing) */**

### `uint32 arkrtos_paging_frame_total(void);`

```c
uint32 arkrtos_paging_frame_total(void);
```

**Total frames in the board pool (0 when there is none) */**

### `uint32 arkrtos_paging_frame_used(void);`

```c
uint32 arkrtos_paging_frame_used(void);
```

**Frames currently allocated from the board pool */**

### `uint32 arkrtos_paging_pair_total(void);`

```c
uint32 arkrtos_paging_pair_total(void);
```

**Pair-level window readout (fine-grained demand-paging view)**
The window is accounted in 8 KB pairs because one PS=12 entry
covers two 4 KB pages. Read-only: asking what is mapped never
faults anything in. total() is 0 on a build without a paged
window; count()/pa() answer 0 for a space that is not installed
(or for a pair that has not been paid for yet). */

### `#if (ARKRTOS_USE_PAGING_SWAP == 1) uint32 arkrtos_paging_swap_stat(uin`

```c
#if (ARKRTOS_USE_PAGING_SWAP == 1) uint32 arkrtos_paging_swap_stat(uint32 which);
```

**Page replacement readout (U-16)**
- `which`：0 = swap-outs, 1 = swap-ins, 2 = slots in use, 3 = failures,
4 = resident pairs, 5 = slot count
只在 ARKRTOS_USE_PAGING_SWAP=1 的镜像里存在；关档回 0（宏），
调用点因此不必套 #if —— 但 shell 里仍然套了，因为「这台板上没有
页替换」和「有页替换、一次没换过」是两句话。 */

## kernelsrc/include/arkrtos_net.h

**内核侧网络服务（SVC 0x1E）的入口。**
为什么不是"SVC 里直接调 lwIP"：lwIP 的 socket API 会把请求投给
tcpip 线程并等它的完成信号量，也就是要阻塞；而 SVC 处理程序里
不能阻塞（PendSV 不会抢占 SVC，见 arkrtos_hal.h 的

### `#define ARKRTOS_NET_TASK_PRIO        (14u)`

```c
#define ARKRTOS_NET_TASK_PRIO        (14u)
```

**网络服务任务的优先级。tcpip 线程是 12（见 lwipopts.h），**
服务任务要低一些，让 tcpip 能先跑完它投递的消息。 */

### `#ifndef ARKRTOS_NET_TASK_STACK_WORDS #define ARKRTOS_NET_TASK_STACK_WO`

```c
#ifndef ARKRTOS_NET_TASK_STACK_WORDS #define ARKRTOS_NET_TASK_STACK_WORDS (384u) #endif
```

**服务任务栈（字）。只跑 lwIP 调用与一次 4 KB 以内的小拷贝。 */**

### `int32 arkrtos_net_start(void);`

```c
int32 arkrtos_net_start(void);
```

**起网络服务任务（幂等）。由 lwIP 任务在 tcpip_init() 之后调用，**
这样服务任务开始受理请求时 tcpip 线程一定已经在跑。
**返回**：0=成功，负值=服务未编入（ARKRTOS_USE_LWIP=0）/任务表满

### `uint8 arkrtos_net_ready(void);`

```c
uint8 arkrtos_net_ready(void);
```

**网络服务是否已经起来。App 侧用它区分"服务没编进去"和"请求失败"。**
**返回**：1=可用，0=不可用

### `int32 arkrtos_net_svc(void *p_svc_ctx);`

```c
int32 arkrtos_net_svc(void *p_svc_ctx);
```

**SVC 0x1E 的分发入口。**不阻塞**：登记请求或取回结果，随即返回。**
- `p_svc_ctx`：系统调用上下文（ARKRTOS_SVC_ARG/RET 用）
**返回**：见 arkrtos_net_abi.h 的返回约定

### `void arkrtos_net_release_task(int32 task_id);`

```c
void arkrtos_net_release_task(int32 task_id);
```

**归还某个任务名下的所有 socket。任务被卸载/故障回收时调用，**
否则泄漏的是内核侧 socket 槽 —— 一个 App 反复崩溃会把它们耗干。
- `task_id`：目标任务号（1 起）

### `void   arkrtos_net_host_step(void);`

```c
void   arkrtos_net_host_step(void);
```

**宿主用例专用：只承接队列里的一条消息并执行（生产构建里不存在）。 */**

### `uint32 arkrtos_net_host_req_state(void);`

```c
uint32 arkrtos_net_host_req_state(void);
```

**宿主用例专用：请求槽当前状态（IDLE / PENDING / DONE）。 */**

### `int32  arkrtos_net_host_sock_fd(uint32 index);`

```c
int32  arkrtos_net_host_sock_fd(uint32 index);
```

**宿主用例专用：socket 表第 index 行的 lwIP fd（-1 = 空行）。 */**

### `int32  arkrtos_net_host_sock_owner(uint32 index);`

```c
int32  arkrtos_net_host_sock_owner(uint32 index);
```

**宿主用例专用：socket 表第 index 行的 owner（-1 = 孤儿）。 */**

### `int32  arkrtos_net_host_req_result(void);`

```c
int32  arkrtos_net_host_req_result(void);
```

**宿主用例专用：请求槽里最后一次执行的原样结果（主人已死时也只能经此读到）。 */**

### `void   arkrtos_net_host_reset(void);`

```c
void   arkrtos_net_host_reset(void);
```

**宿主用例专用：把请求槽强制恢复成空闲，用例之间互不继承。 */**

## kernelsrc/include/arkrtos_ptable.h

**Ark RTOS 分区表运行期接口（内核侧）**
把 config/arkrtos_partition.h 中的编译期布局，在启动时镜像到共享内存的
arkrtos_partition_table_t 中，供内核、Loader、App 在运行期统一读取；
并集中维护统一镜像池的**空闲区分配**与镜像 RAM 池的**伙伴分配**。

### `void arkrtos_ptable_init(void);`

```c
void arkrtos_ptable_init(void);
```

**初始化共享内存中的分区表**
用编译期布局填充布局字段，并把所有槽位记录清为空。
必须在任务调度启动之前调用（建议在 arkrtos_kernel_init 阶段）。

### `arkrtos_partition_table_t *arkrtos_ptable_get(void);`

```c
arkrtos_partition_table_t *arkrtos_ptable_get(void);
```

**获取分区表指针**
**返回**：指向共享内存中分区表的指针；未初始化时同样返回有效地址

### `int32 arkrtos_ptable_range_check(uint32 base, uint32 size);`

```c
int32 arkrtos_ptable_range_check(uint32 base, uint32 size);
```

**校验一段镜像区间是否落在可分配范围内**
- `base`：镜像起始地址
- `size`：镜像占用的字节数
**返回**：0=合法；-1=越界或未按分配粒度对齐
只检查「池内 + 不越过压实余量 + 落点按 ARKRTOS_POOL_ALLOC_UNIT 对齐」。
是否与其它镜像重叠由 arkrtos_ptable_alloc 判定。

### `int32 arkrtos_ptable_find_free(uint32 size, uint32 *p_base, uint32 *p_`

```c
int32 arkrtos_ptable_find_free(uint32 size, uint32 *p_base, uint32 *p_aligned);
```

**在池里找一段能放下 size 字节的空闲区间（首适配）**
- `size`：需要的字节数
- `p_base`：输出落点
- `p_aligned`：输出落点是否是「2 的幂对齐落点」（1=是，可以构造精确 MPU 窗口；
0=落点只是为了塞进缺口，ROM 窗口要退化为包含窗口）
**返回**：0=找到；-1=没有足够大的空闲区间
落点策略（对应「优先精确隔离、退让给容量」）：
1. 优先把落点向上取整到 2^n（2^n >= 区间长度）后放置，使镜像
自身的跨度成为 2 的幂且基址按该跨度对齐 —— MPU 单区域可精确覆盖；
2. 只有当对齐落点放不下而缺口起点放得下时，才用缺口起点做落点，
并把 p_aligned 置 0。

### `int32 arkrtos_ptable_alloc(uint32 type, uint32 base, uint32 size);`

```c
int32 arkrtos_ptable_alloc(uint32 type, uint32 base, uint32 size);
```

**在池内登记一段镜像区间（不碰 Flash）**
- `type`：ARKRTOS_SLOT_APP / ARKRTOS_SLOT_DRIVER
- `base`：镜像起始地址
- `size`：镜像占用的字节数
**返回**：成功返回槽位记录号（>=0），失败返回 -1（区间非法/重叠/无空闲记录）
登记成功的记录状态为 INSTALLING：写入完成前不可启动，掉电则由扫描
依 CRC 判定为 INVALID。同一区间重复登记会复用原记录（覆盖安装）。
安装失败时调用方必须 arkrtos_ptable_free()。

### `int32 arkrtos_ptable_alloc_raw(uint32 type, uint32 base, uint32 size, `

```c
int32 arkrtos_ptable_alloc_raw(uint32 type, uint32 base, uint32 size, uint32 entry);
```

**登记一段「不参与分配」的区间（开发期裸镜像）**
- `type`：ARKRTOS_SLOT_APP / ARKRTOS_SLOT_DRIVER
- `base`：区间起始地址
- `size`：区间字节数
- `entry`：入口地址（裸镜像入口就是区间基址 | Thumb）
**返回**：槽位记录号（>=0）或 -1
状态置为 ARKRTOS_APP_SLOT_RAW：分配器把它当已占用，压实逻辑跳过它。

### `void arkrtos_ptable_free(uint32 slot);`

```c
void arkrtos_ptable_free(uint32 slot);
```

**释放一条槽位记录（同时释放它占用的 Flash 区间与 RAM 块）**
- `slot`：槽位记录号

### `int32 arkrtos_ptable_move(uint32 slot, uint32 new_base);`

```c
int32 arkrtos_ptable_move(uint32 slot, uint32 new_base);
```

**把一条记录的 Flash 落点改到别处（搬移镜像时用）**
- `slot`：槽位记录号
- `new_base`：新落点（必须已通过区间校验且不与其它记录重叠）
**返回**：0=成功，-1=失败
搬移时镜像内容被重新写到新落点，入口地址也要跟着平移，
由调用方负责（entry 不在这里算，因为只有 loader 知道负载偏移）。

### `int32 arkrtos_ptable_find_addr(uint32 addr, uint32 *p_base, uint32 *p_`

```c
int32 arkrtos_ptable_find_addr(uint32 addr, uint32 *p_base, uint32 *p_size);
```

**查找区间内的任意地址所属的槽位记录**
- `addr`：待查地址
- `p_base`：输出该记录的起始地址（可为 0）
- `p_size`：输出该记录的字节数（可为 0）
**返回**：槽位记录号（>=0），未命中返回 -1

### `int32 arkrtos_ptable_find_task(uint32 task_id);`

```c
int32 arkrtos_ptable_find_task(uint32 task_id);
```

**查找任务号对应的槽位记录**
**返回**：槽位记录号（>=0），未命中返回 -1

### `int32 arkrtos_ptable_slot_info(uint32 slot, uint32 *p_type, uint32 *p_`

```c
int32 arkrtos_ptable_slot_info(uint32 slot, uint32 *p_type, uint32 *p_base, uint32 *p_size);
```

**读取槽位记录的类型 / 基址 / 字节数**
**返回**：0=成功，-1=槽位非法

### `int32 arkrtos_ptable_read_header(uint32 slot, arkrtos_app_header_t *ou`

```c
int32 arkrtos_ptable_read_header(uint32 slot, arkrtos_app_header_t *out);
```

**读取指定槽位 Flash 中的镜像头**
- `slot`：槽位记录号
- `out`：输出镜像头（可为 0，仅做存在性校验）
**返回**：0=成功且镜像有效，-1=槽位非法或该槽位是裸镜像，-2=镜像头魔数错误
> 说明：Flash 已被映射到地址空间，可直接按地址读取。

### `int32 arkrtos_ptable_set_slot(uint32 slot, uint32 state, uint32 entry,`

```c
int32 arkrtos_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id);
```

**更新槽位运行期状态**
- `slot`：槽位记录号
- `state`：ARKRTOS_APP_SLOT_x
- `entry`：入口地址（直接赋值，清空槽位时传 0）
- `task_id`：任务号（直接赋值，未启动时传 0）
**返回**：0=成功，-1=槽位非法
> 说明：三个字段在自旋锁保护下作为一组更新：故障处理路径（可能运行在异常
上下文）与安装任务都可能同时改写同一个槽位，否则读者会看到
state/entry/task_id 互相不匹配的中间态。

### `int32 arkrtos_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_`

```c
int32 arkrtos_ptable_get_slot(uint32 slot, uint32 *p_state, uint32 *p_entry, uint32 *p_task_id);
```

**原子读取一个槽位的状态三元组**
- `slot`：槽位记录号
- `p_state`：输出状态（可为 0）
- `p_entry`：输出入口地址（可为 0）
- `p_task_id`：输出任务号（可为 0）
**返回**：0=成功，-1=槽位非法

### `int32 arkrtos_ptable_ram_alloc(uint32 bytes, uint32 *p_base, uint32 *p`

```c
int32 arkrtos_ptable_ram_alloc(uint32 bytes, uint32 *p_base, uint32 *p_size);
```

**从镜像 RAM 池里按 2 的幂分配一块**
- `bytes`：镜像头声明的 ram_size
- `p_base`：输出块基址
- `p_size`：输出块大小（2 的幂字节数）
**返回**：0=成功，-1=没有合适的块
块大小 = max(2^n >= bytes, slot_ram_min_block)，并受 slot_ram_max_block 限制。
落点必须按块大小对齐 —— 否则单个 MPU region 覆盖不了整块 RAM。

### `int32 arkrtos_ptable_ram_reserve(uint32 base, uint32 size);`

```c
int32 arkrtos_ptable_ram_reserve(uint32 base, uint32 size);
```

**按指定基址预留一块 RAM（复现「已固化在镜像头里的那一个块」）**
- `base`：要预留的块基址（必须按 size 对齐）
- `size`：块大小（必须是 2 的幂）
**返回**：0=成功，-1=越界 / 未对齐 / 与已占用的块重叠
与 arkrtos_ptable_ram_alloc() 的唯一区别是地址不是现算的、而是人给的。
存在的理由：RAM 重定位在安装时已经按某个基址固化进了 Flash，
上电重建必须原样占用同一个基址 —— 重新分配会给出另一个地址，
镜像里已固化的绝对地址就全部作废（一启动就 MemManage）。

### `int32 arkrtos_ptable_ram_bind(uint32 slot, uint32 base, uint32 size);`

```c
int32 arkrtos_ptable_ram_bind(uint32 slot, uint32 base, uint32 size);
```

**把 RAM 块记到槽位记录上**
**返回**：0=成功，-1=槽位非法

### `int32 arkrtos_ptable_ram_info(uint32 slot, uint32 *p_base, uint32 *p_s`

```c
int32 arkrtos_ptable_ram_info(uint32 slot, uint32 *p_base, uint32 *p_size);
```

**读出一条记录已绑定的 RAM 块**
**返回**：0=成功，-1=槽位非法（未绑定时 *p_base / *p_size 输出 0）

### `int32 arkrtos_ptable_mini_alloc(uint32 code_bytes, uint32 ram_bytes, u`

```c
int32 arkrtos_ptable_mini_alloc(uint32 code_bytes, uint32 ram_bytes, uint32 *p_index, uint32 *p_code_base, uint32 *p_code_size, uint32 *p_ram_base, uint32 *p_ram_size);
```

**借出**小程序的代码块与 RAM 块**（两块一起借、一起记账，同一把锁内）**
- `code_bytes`：代码块需要的字节数（负载长度按 8 字节上取整，调用方已算好）
- `ram_bytes`：RAM 块需要的字节数（镜像声明的 RAM 需求，本身就是 2 的幂）
- `p_code_base`：/ p_code_size 输出代码块基址与字节数（2 的幂）
- `p_index`：输出本次借出的块在共享表里占了哪一项（0..ARKRTOS_MINI_ARRAY_MAX-1）；
归还时必须把同一个下标交回来，它是「还哪两块」的唯一凭据
- `p_ram_base`：/ p_ram_size  输出 RAM 块基址与字节数（2 的幂）
**返回**：0=成功；-1=没有合适的块 / 参数非法 / 项已满
与 arkrtos_ptable_ram_alloc() 的区别：那个只算地址、记账要靠调用方
再调 ram_bind() 挂到某个槽位上；而小程序**没有槽位（它在文件系统里，
不在镜像池里）**，所以分配与记账必须在同一把锁里完成，否则两次
分配会拿到同一个块。借用期间两块对伙伴分配器都表现为占用
（见 arkrtos_pt_collect）。
为什么两块而不是一块：池的分配粒度是 2 的幂，把代码与 RAM 塞进
同一个 pow2 块会让「代码远小于 RAM」的常见情形接近翻倍
（代码 1KB + RAM 8KB -> 一块 16KB，而两块只要 1KB + 8KB）。
两块各自对齐自己的大小，正好各占一个 MPU 区域。
> 说明：两块**同生同死**：要么都借到、要么一块都不借（先给代码块找地方，
找不到合适的 RAM 块就把代码块的记账一并放弃）；没有「半借」状态，
否则归还路径要处理一种谁也没见过的组合。
> 说明：同一时刻不止一个：数组里有几项空着就能借几份（上限
ARKRTOS_MINI_ARRAY_MAX，运行期同时允许几个由 ARKRTOS_MINI_MAX 管）。
每一项各记一组块，互不共用。

### `int32 arkrtos_ptable_mini_free(uint32 index);`

```c
int32 arkrtos_ptable_mini_free(uint32 index);
```

**归还某一项的块（把该项的四个字段全部清回 0）**
- `index`：arkrtos_ptable_mini_alloc 交回来的下标
**返回**：0=成功，-1=下标越界或该项本就空（如实报错，不假装归还成功）
只清记账，不擦内存内容：调用方（装载失败路径与任务退出路径）必须
无论如何都把账还回去，否则每跑一次小程序漏两块 RAM。

### `int32 arkrtos_ptable_mini_info(uint32 index, uint32 *p_code_base, uint`

```c
int32 arkrtos_ptable_mini_info(uint32 index, uint32 *p_code_base, uint32 *p_code_size, uint32 *p_ram_base, uint32 *p_ram_size);
```

**读出某一项的两块（调试 / shell 展示）**
**返回**：0=成功（空项时对应输出为 0），-1=下标越界或四个指针全为空
> 说明：允许逐个传 0：只关心 RAM 块的调用者不必准备四个变量。

### `int32 arkrtos_ptable_mini_used(uint32 *p_used, uint32 *p_max);`

```c
int32 arkrtos_ptable_mini_used(uint32 *p_used, uint32 *p_max);
```

**统计小程序块的项数与在用项数**
- `p_used`：在用项数（可为 0）
- `p_max`：数组容量（= ARKRTOS_MINI_ARRAY_MAX）
**返回**：0=成功

### `int32 arkrtos_ptable_stats(uint32 *p_used, uint32 *p_total, uint32 *p_`

```c
int32 arkrtos_ptable_stats(uint32 *p_used, uint32 *p_total, uint32 *p_frag);
```

**取池的统计值（供 shell 的 info 命令展示）**
- `p_used`：已占用字节数（不含压实余量与裸镜像？含全部占用，便于对账）
- `p_total`：池可分配总字节数（= pool_usable_size）
- `p_frag`：空闲区间个数（>1 表示存在碎片）
**返回**：0=成功

## kernelsrc/include/arkrtos_rt.h

**实时性度量：中断入口、唤醒、抢占、临界区的最坏值（内核内部头文件）**
打开 ARKRTOS_USE_RTSTAT 后，内核在几个固定位置打时间戳，用 DWT
周期计数器（1 个 CPU 周期分辨率）累计一组"最坏值"读数，供 shell 的
`rt` 命令打印。它回答的是认证/验收会问的那几个问题：

### `typedef struct {`

```c
typedef struct {
```

**一行读数（周期 + 微秒两种单位，微秒按当前主频折算） */**

### `#define ARKRTOS_RT_ROWS                 5u`

```c
#define ARKRTOS_RT_ROWS                 5u
```

**读数行数上限（由 arkrtos_rt_snapshot 填） */**

### `void   arkrtos_rt_init(void);`

```c
void   arkrtos_rt_init(void);
```

**模块初始化：打开周期计数器、算出每拍周期数、清空统计 */**

### `int32  arkrtos_rt_counter_alive(void);`

```c
int32  arkrtos_rt_counter_alive(void);
```

**重新武装周期计数器并确认它真的在走（1 = 在走，0 = 转不动）**
不是纯读：会写 DEMCR.TRCENA / DWT.CYCCNTENA。调试器断开时
会把这两位清掉，所以每次开测前都调一次，而不是假定开机时
打开的就一直有效。不碰统计量。

### `void   arkrtos_rt_tick_enter(void);`

```c
void   arkrtos_rt_tick_enter(void);
```

**节拍中断入口/出口时间戳（由 arkrtos_kernel_tick_body 调用） */**

### `void   arkrtos_rt_crit_enter(void);`

```c
void   arkrtos_rt_crit_enter(void);
```

**临界区入口/出口（由移植层 enter/exit_critical 调用，带嵌套计数） */**

### `int32  arkrtos_rt_measure_wake(uint32 n);`

```c
int32  arkrtos_rt_measure_wake(uint32 n);
```

**场景一：当前任务自己按 1 ms 延时循环 n 次，量"节拍 → 本任务继续跑"**
- `n`：采样次数
**返回**：0 = 完成；-1 = 采样器未就绪（周期计数器不可用）
每醒来一次记一个 delta = 现在 - 最近一次节拍入口的时间戳。
一个正常样本必然小于一拍；超过一拍的样本不参与统计，计入
late_n 单独报出来——那是"低优先级任务被更高优先级挡了一会儿"，
不是唤醒延迟。

### `int32  arkrtos_rt_measure_preempt(uint32 n);`

```c
int32  arkrtos_rt_measure_preempt(uint32 n);
```

**场景二：最高优先级任务被 post 唤醒的延迟**
- `n`：采样次数
**返回**：0 = 完成；-1 = 采样器未就绪 / 辅助任务建不起来
第一次调用时按需创建一个优先级 1 的辅助任务（静态栈 1 KB），
本任务（调用者）自己当"发信号的一方"：记时间戳 → post → 等回执。
辅助任务在醒来后的第一行取时间戳，所以这个数包含 post 路径、
抢占判定、两层上下文切换。

### `uint32 arkrtos_rt_snapshot(arkrtos_rt_row_t *rows, uint32 max_rows);`

```c
uint32 arkrtos_rt_snapshot(arkrtos_rt_row_t *rows, uint32 max_rows);
```

**取一份读数快照（按名称固定顺序，未测量的行 n = 0）**
**返回**：行数

### `uint32 arkrtos_rt_jitter(int32 *min_c, int32 *max_c, uint32 *n);`

```c
uint32 arkrtos_rt_jitter(int32 *min_c, int32 *max_c, uint32 *n);
```

**节拍抖动（有符号周期，正 = 比标称晚）：返回 0 表示还没有样本 */**

### `void   arkrtos_rt_reset(void);`

```c
void   arkrtos_rt_reset(void);
```

**清空所有统计（不影响已经建好的辅助任务） */**

### `uint32 arkrtos_rt_cycles_per_tick(void);`

```c
uint32 arkrtos_rt_cycles_per_tick(void);
```

**一个节拍标称多少 CPU 周期（0 = 周期计数器不可用） */**

### `uint32 arkrtos_rt_to_us(uint32 c);`

```c
uint32 arkrtos_rt_to_us(uint32 c);
```

**周期数 -> 微秒（与读数行同一套换算，shell 也用这个）**
- `c`：周期数
**返回**：微秒；时钟未知时返回 0
内部按千赫拆开，不调用 64 位除法（RV32 没有 libgcc 那条）。

### `int32  arkrtos_rt_last_error(void);`

```c
int32  arkrtos_rt_last_error(void);
```

**上一次准备失败的原因码（0 = 没失败）**
1/2 = 两个信号量创建失败（多半是信号量池满）；
5 = 辅助任务登记失败（返回值 -1，窗口/优先级校验不过）；
其它非 0 只说明失败，具体码由实现给出。

## kernelsrc/include/arkrtos_sec.h

**安全基线自检（Security baseline self-check）。开关 ARKRTOS_USE_SECCHECK。**
这个模块回答一个问题：**这块板子现在这套构建，按安全基线该有的东西
有哪些真的在、有哪些不在了**。它做三件事，且只做这三件：
1. 读编译期开关，报「基线要求的那几项是不是都编进来了」；

### `#define ARKRTOS_SEC_ITEM_MAX        (16u)`

```c
#define ARKRTOS_SEC_ITEM_MAX        (16u)
```

**自检项上限（超过的检查不会进报告，见 arkrtos_sec_add 的返回值） */**

### `#define ARKRTOS_SEC_PASS            (0u) #define ARKRTOS_SEC_FAIL     `

```c
#define ARKRTOS_SEC_PASS            (0u) #define ARKRTOS_SEC_FAIL            (1u) #define ARKRTOS_SEC_FINDING         (2u) #define ARKRTOS_SEC_SKIP            (3u)
```

**单项结论 */**

### `#define ARKRTOS_SEC_ERR_NO_PORT     (-1)`

```c
#define ARKRTOS_SEC_ERR_NO_PORT     (-1)
```

**板级弱符号的返回值：本板没有实现这条自查通路（不是"检查通过"）。**
与上面的 PASS/FAIL/FINDING/SKIP 是两套语义：那是检查结果，这是
「根本没有读数」。值不得单独修改。 */

### `typedef struct {`

```c
typedef struct {
```

**一条自检项**
说明文字不做字符串拼接：模块只给出 printf 风格的格式串与最多 4 个
整数，由**外壳**去格式化。理由是内核里另起一套 snprintf 会把代码
体积翻着涨，而外壳本来就有格式化能力。

### `typedef struct {`

```c
typedef struct {
```

**一次自检的完整报告 */**

### `uint32 arkrtos_sec_run(arkrtos_sec_report_t *p_out);`

```c
uint32 arkrtos_sec_run(arkrtos_sec_report_t *p_out);
```

**跑一遍安全基线自检**
- `p_out`：报告输出（必须非空）
**返回**：FAIL 的条数（0 = 没有硬失败；FINDING 不计入）

### `uint32 arkrtos_sec_count(const arkrtos_sec_report_t *p_rep, uint32 sta`

```c
uint32 arkrtos_sec_count(const arkrtos_sec_report_t *p_rep, uint32 state);
```

**报告里某一档的条数（外壳打印用；state 越界返回 0） */**

### `const char *arkrtos_sec_state_name(uint32 state);`

```c
const char *arkrtos_sec_state_name(uint32 state);
```

**结论名：pass / fail / finding / skip */**

## kernelsrc/include/arkrtos_sha256.h

**SHA-256 (FIPS 180-4), shared by the image/payload signature code and**
the console login (SEC-G01).
This used to be a static block inside arkrtos_sign.c, which meant the
only way to hash a runtime string was to turn the *image signature*

### `#define ARKRTOS_SHA256_LEN          (32u)`

```c
#define ARKRTOS_SHA256_LEN          (32u)
```

**Digest length in bytes. */**

### `typedef struct {`

```c
typedef struct {
```

**Incremental hash context (caller owns it; may live on the stack). */**

### `ARKRTOS_SHA256_UNUSED static void arkrtos_sha256_hash(const uint8 *dat`

```c
ARKRTOS_SHA256_UNUSED static void arkrtos_sha256_hash(const uint8 *data, uint32 len, uint8 out[32]) {
```

**One shot: digest of a single buffer (console login, small payloads). */**

## kernelsrc/include/arkrtos_share.h

**Ark RTOS 共享内存分区表（内核 / Loader / App 运行期接口）**
分区布局在编译期由 config/arkrtos_partition.h 唯一定义，但该头文件
**只允许内核工程包含**。Loader 与 App 工程在运行期通过共享内存中的
本结构体获取布局信息（SVC 0x18 子命令 1 返回其地址），

### `#define ARKRTOS_PARTITION_MAGIC     (0x50415254u)`

```c
#define ARKRTOS_PARTITION_MAGIC     (0x50415254u)
```

**分区表魔数 "PART"，用于校验共享内存中的分区表是否有效 */**

### `#define ARKRTOS_PARTITION_VERSION   (10u)`

```c
#define ARKRTOS_PARTITION_VERSION   (10u)
```

**分区表结构版本。**
v1 -> v2: driver 区状态由「单槽四个平铺字段」改为「与 App 同构的槽位数组」。
v2 -> v3: 末尾追加每槽 autostart 标志。
v3 -> v4: 驱动区与 App 区合并为**统一镜像池**，槽位表按「镜像」而非
「固定分区槽」组织：每条记录带自己的类型（App/驱动）、基址与
占用单元数。
v4 -> v5: 池分配改为细粒度字节粒度（pool_alloc_unit），镜像可被搬移与
压实（新增 pool_usable_size / pool_reserve_sectors）；记录的
「占用单元数」换成「字节数」；镜像 RAM 由伙伴分配器按镜像
声明的 ram_size 现算，因此每条记录新增 ram_base / ram_size，
且槽位数组长度从 8 扩到 16。布局字段与记录字段再次全变，
版本必须升位。
v5 -> v6: 新增设备端布局配置区（安装模式 + 固定槽位表），分区表追加
layout_mode / layout_source / config_base / config_size /
cfg_slot_count 五个字段；Flash 布局变为
BOOT -> KERNEL -> CONFIG -> IMAGE_POOL，池基址随之后移
（旧镜像与旧工具必须同步升级），硬件兼容签名低 16 位升到 5。
v6 -> v7: 设备端配置区里原本只能写 0 的两个旋钮开始生效——
boot_delay_ms（自启前的调试器挂接窗口）与 flags 的
RAW_ALLOW 位（是否接受池内直接烧录的裸镜像）。分区表末尾
追加 cfg_boot_delay_ms / cfg_raw_allow 两个生效值，
让上位机能一眼看出设备到底按哪套配置在跑。
v7 -> v8: 小程序（MiniApp，见 arkrtos_mini.h）需要一个**瞬态** RAM 块：
运行时从镜像 RAM 池借、退出即归还，因此它不属于任何镜像槽位，
没法记在 slot_ram_* 里。末尾追加 mini_ram_base / mini_ram_size
两个字段专门给这个块记账，伙伴分配器把非零的它当作一个占用
区间（同一时刻最多一个小程序，所以一组字段就够）。
纯追加，已有字段偏移不变；硬件兼容签名不受影响（那是镜像侧的
代号，与分区表 ABI 无关）。
v8 -> v9: 小程序的内存布局由「一个 pow2 块」改成「代码块 + RAM 块」两块。
原来把代码与 RW/ZI+栈塞进同一块、块大小取
pow2_ceil(代码 + RAM)：pow2 是池的分配粒度，于是最常见的
「代码远小于 RAM」会被放大到接近两倍——代码 1KB + RAM 8KB 要
一块 16KB，分两块只要 1KB + 8KB。两块各自取 pow2、各自对齐
自己的大小，正好各占一个 MPU 区域（代码窗 / 数据窗），
数据窗也就不必再与代码窗共用「可执行」属性。
末尾追加 mini_code_base / mini_code_size；mini_ram_* 含义不变。
纯追加，已有字段偏移不变。
v9 -> v10: 小程序支持**同一时刻多个**：原来 4 个标量字段（mini_code_base /
mini_code_size / mini_ram_base / mini_ram_size）只能记一组块，
等于把「一个小程序」写进了 ABI。改成 4 个长度为
ARKRTOS_MINI_ARRAY_MAX 的数组，下标记同一个小程序的两块。
本次是**改字段**（标量变数组），不是纯追加：结构体尺寸变了，
共享 RAM 里的旧版本记录必须被识别成旧版本而不是按新布局读——
这正是 version 字段存在的理由，开机校验不过就整表重建。

### `#define ARKRTOS_MINI_ARRAY_MAX      (4u)`

```c
#define ARKRTOS_MINI_ARRAY_MAX      (4u)
```

**小程序块数组的固定长度（ABI 形状常量）。**
运行期同时能跑几个小程序由 ARKRTOS_MINI_MAX 决定（见 arkrtos_features.h），
它必须 <= 本值；数组长度写死是为了让结构体尺寸与偏移跨版本稳定。 */

### `#define ARKRTOS_SLOT_ARRAY_MAX      (16u)`

```c
#define ARKRTOS_SLOT_ARRAY_MAX      (16u)
```

**槽位数组的固定长度（ABI 形状常量）。**
实际使用的槽位数由运行期字段 slot_max 决定，必须 <= 本值；
数组长度写死是为了让结构体尺寸与偏移跨版本稳定。 */

### `#define ARKRTOS_SLOT_FREE           (0u)   /* 记录未使用，对应 Flash 区间可被分配 */`

```c
#define ARKRTOS_SLOT_FREE           (0u)   /* 记录未使用，对应 Flash 区间可被分配 */ #define ARKRTOS_SLOT_APP            (1u)   /* 用户应用镜像 */ #define ARKRTOS_SLOT_DRIVER         (2u)   /* 用户驱动镜像 */
```

**槽位类型（与 arkrtos_app_image.h 的 ARKRTOS_APP_TYPE_x 同值） */**

### `#define ARKRTOS_APP_SLOT_EMPTY      (0u)    /* 空槽位 */ #define ARKRTOS_`

```c
#define ARKRTOS_APP_SLOT_EMPTY      (0u)    /* 空槽位 */ #define ARKRTOS_APP_SLOT_LOADED     (1u)    /* 已写入镜像并通过校验 */ #define ARKRTOS_APP_SLOT_RUNNING    (2u)    /* 已注册为任务并运行 */ #define ARKRTOS_APP_SLOT_INVALID    (3u)    /* 槽位有内容但校验失败（魔数/兼容签名/CRC 不符） */ #define ARKRTOS_APP_SLOT_INSTALLING (4u)    /* 正在安装：写入未完成，不可启动（掉电后由 CRC 判定为 INVALID） */ #define ARKRTOS_APP_SLOT_RAW        (5u)    /* 开发期裸镜像（无镜像头）：占用区间不参与分配，也不参与压实 */
```

**槽位状态 */**

### `typedef struct {`

```c
typedef struct {
```

**分区表（存放于共享内存起始处）**
由内核在启动时填充静态布局，运行期由 Loader 更新槽位状态。
Loader / App 只读布局字段，写状态字段前须确认自己拥有该槽位。
Flash 统一镜像池：pool_base 起、pool_size 字节，按
pool_alloc_unit 粒度做首适配分配（镜像向后紧邻排列），
尾部 pool_reserve_sectors 个扇区始终留空作为压实的搬移余量。
pool_sector 只是芯片的物理擦除单位，与分配粒度无关。
RAM：镜像的 .data/.bss/栈由伙伴分配器从 slot_ram_base 起、
slot_ram_total 字节的池里按镜像头声明的 ram_size 切块，
块大小落在 [slot_ram_min_block, slot_ram_max_block]。
每条记录带自己的 ram_base / ram_size，两块镜像永不共用 RAM。

## kernelsrc/include/arkrtos_shell.h

**Ark RTOS 内核 Shell 控制台（ark-shell 移植）对外接口**
控制台任务独占 SHELL_DEV_NAME 串口，提供 App/驱动/任务的启停与
查询、故障读数、串口安装等人机命令。
串口归属约定：开启 shell 后不再注册常驻安装任务（见

### `int32 arkrtos_shell_init(void);`

```c
int32 arkrtos_shell_init(void);
```

**初始化并注册内核 Shell 控制台任务**
**返回**：成功返回任务号（>0）；SHELL_ENABLE 为 0 时返回 0
需在调度启动之前调用（紧跟 arkrtos_loader_start_autostart*() 之后）。

### `int32 arkrtos_shell_uart_handle(void);`

```c
int32 arkrtos_shell_uart_handle(void);
```

**取控制台已打开的串口句柄**
**返回**：设备句柄；尚未打开时返回 -1
安装窗口复用同一个句柄收镜像，语义上比再 open 一次更清楚。

### `int32 arkrtos_shell_uart_open(void);`

```c
int32 arkrtos_shell_uart_open(void);
```

**打开控制台串口（已打开则直接返回既有句柄）**
**返回**：设备句柄；打开失败返回 -1

### `int32 arkrtos_shell_ext_register(const arkrtos_ushell_cmd_t *cmd);`

```c
int32 arkrtos_shell_ext_register(const arkrtos_ushell_cmd_t *cmd);
```

**往内核控制台注册一条用户态（App / 驱动）命令**
- `cmd`：用户态描述符；name / help 会被复制进内核 RAM，
所以镜像卸载后不会留下指向已擦除 Flash 的悬垂指针
**返回**：ARKRTOS_USHELL_OK 或负错误码（见 arkrtos_ushell.h）
供内核分发层（SVC 0x1A 子命令 1）调用；用户态请用
arkrtos_shell_cmd_register()，不要直接调用本函数。

### `int32 arkrtos_shell_ext_unregister(const char *name);`

```c
int32 arkrtos_shell_ext_unregister(const char *name);
```

**注销一条用户态命令（按名字，不区分大小写）**
**返回**：ARKRTOS_USHELL_OK 或负错误码

### `int32 arkrtos_shell_release_task(uint32 task_id);`

```c
int32 arkrtos_shell_release_task(uint32 task_id);
```

**Drop every user command registered by one task**
- `task_id`：owner task id (0 = kernel, nothing to drop)
**返回**：number of entries released
Hooked into task teardown so an App cannot leave its command name
reserved after it is stopped or uninstalled.

### `int32 arkrtos_board_shell_register(int32 idx);`

```c
int32 arkrtos_board_shell_register(int32 idx);
```

**板级命令扩展点（板级实现，内核在命令表建好后调用一次）**
- `idx`：命令表里第一个空闲槽位
**返回**：追加完成后新的表长；没有追加就原样返回 idx
调用点在 register_kernel_commands() 末尾，也就是 ark_shell_init()
重建命令表之后。板级命令只能在这里登记：在那之前写 g_cmd_table
会被整表覆盖，现场表现是命令凭空消失（"sdram: command not found"）。
返回表长的约定与 arkvim_shell_register()/arkc_shell_register() 一致。

### `int32 arkrtos_shell_print_n(const char *msg, uint32 len);`

```c
int32 arkrtos_shell_print_n(const char *msg, uint32 len);
```

**向控制台输出一段有界长度的文本（不追加换行）**
- `msg`：文本首地址
- `len`：输出长度；由分发层先探测上限，避免在内核侧做无界 strlen
**返回**：ARKRTOS_USHELL_OK 或负错误码
SVC 处理运行在用户任务的 PSP 上（App 栈 4K、驱动栈 1K），
因此这里用内核静态缓冲分段输出，不在调用者栈上开临时数组。

### `int32 arkrtos_shell_resolve_path(const char *in, char *out, uint32 cap`

```c
int32 arkrtos_shell_resolve_path(const char *in, char *out, uint32 cap);
```

**把一个用户输入的路径解析成命名空间绝对路径**
- `in`：用户输入的路径（绝对、相对、~/ 都接受）
- `out`：输出缓冲；解析失败时内容不动
- `cap`：输出缓冲长度
**返回**：0=成功；-1=参数非法或解析结果装不下
相对路径按 shell 当前目录解析，"." 与 ".." 按 Linux 语义消化。
存在的理由是「一个 shell 只该有一个 cwd 语义」：组件（如 vim
命令）要接受用户给的路径时，必须和提示符下敲同一个路径得到同
一个文件，而不是各写一份略有差别的拼接规则。
装不下时返回 -1 而不是悄悄截断——截断后的路径会指向另一个文件。
> 说明：只在 ARKRTOS_USE_VFS=1 的构建里存在（分辨率依赖 VFS 路径语义）。

## kernelsrc/include/arkrtos_shell_auth.h

**Console login for the kernel shell (SEC-G01).**
Four entry points:
arkrtos_shell_auth_init()   - reset the state (boot starts locked)
arkrtos_shell_auth_gate()   - the shell's gate hook: 0 lets the

### `void arkrtos_shell_auth_init(void);`

```c
void arkrtos_shell_auth_init(void);
```

**Reset to "locked, no failures".  Called once from the shell task. */**

### `int arkrtos_shell_auth_gate(const char *name);`

```c
int arkrtos_shell_auth_gate(const char *name);
```

**Gate hook: 0 = allowed, -1 = refused (reason already printed). */**

### `int arkrtos_shell_auth_locked(void);`

```c
int arkrtos_shell_auth_locked(void);
```

**1 while the console refuses everything but login/logout/help. */**

### `int arkrtos_shell_auth_cmd_login(int argc, char **argv);`

```c
int arkrtos_shell_auth_cmd_login(int argc, char **argv);
```

**`login <password>` */**

### `int arkrtos_shell_auth_cmd_logout(int argc, char **argv);`

```c
int arkrtos_shell_auth_cmd_logout(int argc, char **argv);
```

**`logout` */**

## kernelsrc/include/arkrtos_sign.h

**App image signature check (HMAC-SHA256).  Optional: ARKRTOS_USE_IMAGE_SIGN.**
The image header has always carried a `signature[64]` placeholder that
nothing read.  This module finally fills it in: the packer computes an
HMAC-SHA256 over the image and stores it in the first 32 bytes of that

### `#define ARKRTOS_SIGN_LEN            (32u)`

```c
#define ARKRTOS_SIGN_LEN            (32u)
```

**Signature length in bytes (HMAC-SHA256 output).**
> 说明：Stored in signature[0..32); signature[32..64) stays zero. */

### `#define ARKRTOS_SIGN_KEY_LEN        (32u)`

```c
#define ARKRTOS_SIGN_KEY_LEN        (32u)
```

**Pre-shared key length in bytes. */**

### `void arkrtos_sign_image(const uint8 *image, const arkrtos_app_header_t`

```c
void arkrtos_sign_image(const uint8 *image, const arkrtos_app_header_t *p_hdr, uint8 out[ARKRTOS_SIGN_LEN]);
```

**Compute an image's signature into out[ARKRTOS_SIGN_LEN].**
- `image`：start of the image (header + reloc table + payload)
- `p_hdr`：its parsed header
- `out`：receives ARKRTOS_SIGN_LEN bytes

### `int32 arkrtos_sign_verify(const uint8 *image, const arkrtos_app_header`

```c
int32 arkrtos_sign_verify(const uint8 *image, const arkrtos_app_header_t *p_hdr);
```

**Check the signature stored in p_hdr->signature against the image.**
**返回**：0 = matches, -1 = does not (comparison is constant time).

### `void  arkrtos_sign_stream_begin(const uint8 *image);`

```c
void  arkrtos_sign_stream_begin(const uint8 *image);
```

Streaming MAC, for the install path that never holds the whole nominal
image in RAM.  Feed the header (as written to Flash), then the nominal
reloc table and payload bytes exactly as they arrive over the wire, then
compare against p_hdr->signature.  Same coverage as arkrtos_sign_image();
one such transfer is in flight at a time, so the state is module-static. */

### `void arkrtos_sign_payload(const uint8 *data, uint32 len, uint8 out[ARK`

```c
void arkrtos_sign_payload(const uint8 *data, uint32 len, uint8 out[ARKRTOS_SIGN_LEN]);
```

**一次算出 data[0..len) 的 tag；out 收 ARKRTOS_SIGN_LEN 字节 */**

### `int32 arkrtos_sign_payload_check(const uint8 *data, uint32 len, const `

```c
int32 arkrtos_sign_payload_check(const uint8 *data, uint32 len, const uint8 tag[ARKRTOS_SIGN_LEN]);
```

**比较 data[0..len) 的 MAC 与 tag（常量时间）**
**返回**：0 = 一致，-1 = 不符 */

### `void  arkrtos_sign_payload_begin(void);`

```c
void  arkrtos_sign_payload_begin(void);
```

**流式三件套（begin / update / end）：装载器不把整份文件读进 RAM 时用**
> 说明：一次只能有一路在算（状态是模块静态的），装载路径本身是串行的。 */

### `void  arkrtos_sign_payload_update(const uint8 *data, uint32 len);`

```c
void  arkrtos_sign_payload_update(const uint8 *data, uint32 len);
```

**喂一块数据给流式 MAC；没 begin 过就调用会被静默丢弃 */**

### `int32 arkrtos_sign_payload_end(const uint8 tag[ARKRTOS_SIGN_LEN]);`

```c
int32 arkrtos_sign_payload_end(const uint8 tag[ARKRTOS_SIGN_LEN]);
```

**返回**：0 = 一致，-1 = 不符（或没有 begin 过） */

## kernelsrc/include/arkrtos_smp.h

**SMP 骨架：每核状态、次核启动入口、核间中断通路（默认关）**
必须先把话说清楚：**本内核的调度器是单核设计**——当前任务号
是全局的（arkrtos_current_task_id）、就绪表是全局位图、延迟
队列也是全局一份。打开 ARKRTOS_USE_SMP 不会把这些变成每核一份，

### `typedef struct {`

```c
typedef struct {
```

**单核状态块**
online 由次核自己在启动入口里置位（CPU0 由 init 置位）；
started 只表示"port 层接受了启动请求"。

### `int32 arkrtos_smp_init(void);`

```c
int32 arkrtos_smp_init(void);
```

**引导次核**
CPU0（调用者）直接标记上线；1..ARKRTOS_CPU_NUM-1 逐个调用
arkrtos_port_cpu_start()。
**返回**：启动请求被接受的核数（含 CPU0）；port 层不支持的核不计入
> 说明：返回 N 只代表"N 个核的启动请求被接受"，上线与否要看掩码。

### `void arkrtos_smp_secondary_main(uint32 cpu);`

```c
void arkrtos_smp_secondary_main(uint32 cpu);
```

**次核启动入口（由 port 层在次核上调用）**
- `cpu`：硬件核号（必须落在 1..ARKRTOS_CPU_NUM-1）
> 说明：会把该核标记上线、开中断，然后进入空转循环，永不返回。
核号越界时什么都不标记，直接空转——宁可停在那里，也不要让一个
不知道自己是几号核的 CPU 去改别人的状态。

### `void arkrtos_smp_cpu_online(uint32 cpu);`

```c
void arkrtos_smp_cpu_online(uint32 cpu);
```

**把指定核标记为上线（幂等）**
- `cpu`：硬件核号，越界直接忽略
> 说明：次核启动入口内部调用它；移植层若自己完成了启动，也可以直接调。

### `arkrtos_smp_cpu_t *arkrtos_smp_cpu_self(void);`

```c
arkrtos_smp_cpu_t *arkrtos_smp_cpu_self(void);
```

**当前核的状态块**
> 说明：arkrtos_port_cpu_id() 返回值越界时收敛到核 0：核号读错不应该变成
越界写。

### `uint32 arkrtos_smp_online_count(void);`

```c
uint32 arkrtos_smp_online_count(void);
```

**在线核数（按掩码统计） */**

### `uint32 arkrtos_smp_online_mask_get(void);`

```c
uint32 arkrtos_smp_online_mask_get(void);
```

**在线核掩码 */**

### `void arkrtos_smp_tick(void);`

```c
void arkrtos_smp_tick(void);
```

**记一拍（由各核的 tick 中断调用），只累加当前核的计数 */**

### `int32 arkrtos_smp_send_ipi(uint32 cpu);`

```c
int32 arkrtos_smp_send_ipi(uint32 cpu);
```

**发核间中断**
- `cpu`：目标核号
**返回**：0 = 已交给 port 层发出；-1 = 核号越界 / 该核未上线 / port 不支持

### `void arkrtos_smp_ipi_handler(void);`

```c
void arkrtos_smp_ipi_handler(void);
```

**核间中断服务例程（由 port 层的 IPI 中断调用）**
> 说明：默认实现只累加当前核的 ipi_count。需要真实动作（例如踢调度）时，
应用提供同名强符号即可覆盖（本实现标了 __weak）。

### `void arkrtos_smp_idle_loop(void);`

```c
void arkrtos_smp_idle_loop(void);
```

**次核空转一步：累加 idle_count 后 WFI**
> 说明：默认次核循环就是它；将来接每核调度器时，把这一步换成 idle 任务的
调度入口。

## kernelsrc/include/arkrtos_smp_sched.h

**调度分核（AMP）：次核自己的运行队列 + 显式任务迁移（默认关）**
先把边界说清楚，免得被读成"支持对称多核"：
**全局调度器没有改**。arkrtos_current_task_id、就绪位图、延时链
仍然是 CPU0 那一份，单核行为逐字节不变。本模块只做一件事：

### `#define ARKRTOS_SMP_SCHED_NIL           (-1)`

```c
#define ARKRTOS_SMP_SCHED_NIL           (-1)
```

**"本核空闲"的哨兵值（队列里/当前任务号都用它表示没有任务） */**

### `void arkrtos_smp_sched_enter(uint32 cpu);`

```c
void arkrtos_smp_sched_enter(uint32 cpu);
```

**次核进入调度域（不返回）**
- `cpu`：硬件核号（必须 1..ARKRTOS_CPU_NUM-1）
由 arkrtos_smp_secondary_main() 调用：本核节拍定时器起来后，
在 WFI 循环里等第一拍——第一拍把空闲帧存进本核的 idle 槽，
之后的每一拍都从这里换到（或被换回）任务帧。
> 说明：第一次节拍之前本核不跑任何任务。本核栈由汇编入口
(arkrtos_smp_secondary_entry) 设好，这里不再动 sp。

### `int32 arkrtos_smp_sched_task_to(int32 task_id, uint32 cpu);`

```c
int32 arkrtos_smp_sched_task_to(int32 task_id, uint32 cpu);
```

**把任务迁到指定次核**
- `task_id`：任务号（1..task_count）
- `cpu`：目标核号（1..ARKRTOS_CPU_NUM-1；0 不走这里）
**返回**：0=已入次核运行队列；-1=任务号/核号非法；-2=目标核是 0（应调
arkrtos_smp_sched_task_back）；-3=任务不是 READY（RUNNING/WAIT/
DELAY/INVALID 一律拒绝）；-4=任务已归别的核；-5=目标队列满
> 说明：只在本核（CPU0）调用：它会摘 CPU0 的就绪链，那是全局结构。

### `int32 arkrtos_smp_sched_task_back(int32 task_id);`

```c
int32 arkrtos_smp_sched_task_back(int32 task_id);
```

**把迁出去的任务要回 CPU0**
- `task_id`：任务号
**返回**：0=请求已登记（下一拍生效）；-1=任务号非法；-4=该任务本来就在 CPU0
只是置请求位，不搬任何链表：次核在下一次节拍里把任务摘出来、
置回 CPU0 归属并登记归还；CPU0 每拍 arkrtos_smp_sched_poll()
把它重新挂回就绪链。请求登记后重复调用是幂等的。

### `uint32 arkrtos_smp_sched_switch(uint32 cpu, uint32 frame);`

```c
uint32 arkrtos_smp_sched_switch(uint32 cpu, uint32 frame);
```

**次核节拍上下文里的调度决策**
- `cpu`：本核号
- `frame`：本次异常压在被中断上下文栈上的帧指针
**返回**：要 ertn 回去的帧指针（可能已换成另一个任务，或换回本核空闲帧）
> 说明：只在次核的异常入口调用，且调用时中断已关（在异常里）。它自己
用本核自旋锁与 CPU0 的迁移/回收动作互斥。

### `void arkrtos_smp_sched_tick(uint32 cpu);`

```c
void arkrtos_smp_sched_tick(uint32 cpu);
```

**次核记一拍（由次核的定时器中断调用） */**

### `void arkrtos_smp_sched_poll(void);`

```c
void arkrtos_smp_sched_poll(void);
```

**CPU0 每拍回收"要回来"的任务**
> 说明：由 arkrtos_kernel_tick_handler 的节拍体调用（仅在开关打开时编译）。
没有待回收任务时是一次常数级判断。

### `int32 arkrtos_smp_sched_cur(uint32 cpu);`

```c
int32 arkrtos_smp_sched_cur(uint32 cpu);
```

**当前在指定核上运行的任务下标**
**返回**：-1=该核空闲（或核号非法）

### `uint8 arkrtos_smp_sched_owned_by_other(int32 task_idx);`

```c
uint8 arkrtos_smp_sched_owned_by_other(int32 task_idx);
```

**该任务是否归别的核**
- `task_idx`：任务下标（0 起）
**返回**：1=归次核（CPU0 的就绪集/自检必须跳过它），0=归 CPU0
> 说明：开关关着时不存在这个符号；CPU0 侧的调用点都在同一开关内。

### `int32 arkrtos_smp_sched_selftest(uint32 *out);`

```c
int32 arkrtos_smp_sched_selftest(uint32 *out);
```

**读数快照（给自测/串口打印用）**
- `out`：至少 8 个字的数组，依次写：
[0] 次核当前任务下标（-1=idle 转成 0xFFFFFFFF 报）
[1] 次核运行队列长度
[2] 次核切换次数
[3] 迁入次数
[4] 迁出次数
[5] 次核节拍计数
[6] 待回收队列非空标志
[7] 次核归属任务数（全表统计）
**返回**：0=成功；-1=out 为空或没有次核

## kernelsrc/include/arkrtos_softbus.h

**软总线（ark_softbus）在内核里的接入面**
组件本体是 ark_softbus 源仓库的同步副本（kernelsrc/components/
ark_softbus，见那边的 VENDOR.md）。内核这一层只做三件事：
1) 把板级三钩子装进总线 HAL（board/<板>/arkrtos_bus_port.c）；

### `int32 arkrtos_softbus_init(void);`

```c
int32 arkrtos_softbus_init(void);
```

**建立板内软总线（幂等）**
装 HAL、建 loopback 链路、起本节点与常驻对端节点。
重复调用只会把两个节点复位重来。
**返回**：0 = 成功，负值 = 失败

### `int32 arkrtos_softbus_pump(uint32 rounds);`

```c
int32 arkrtos_softbus_pump(uint32 rounds);
```

**推进一次总线事件循环（本节点 + 常驻对端各一轮）**
- `rounds`：轮数，0 表示 1 轮
**返回**：0

### `int32 arkrtos_softbus_info(void);`

```c
int32 arkrtos_softbus_info(void);
```

**打印本节点、常驻对端与计数**
**返回**：0

### `int32 arkrtos_softbus_selftest(void);`

```c
int32 arkrtos_softbus_selftest(void);
```

**端到端自检：协议编解码、发现、发布/订阅、定向+ACK、QoS 重传、**
RPC 请求应答、数据平面分块、错误路径
**返回**：失败项数（0 = 全部通过）；每项一行 PASS/FAIL 输出到控制台

### `int arkrtos_softbus_shell_cmd(int argc, char **argv);`

```c
int arkrtos_softbus_shell_cmd(int argc, char **argv);
```

**控制台命令 `bus` 的处理器（内核 Shell 命令表里注册的就是它）**

## kernelsrc/include/arkrtos_spin.h

**Ark RTOS 自旋锁与临界区（内核/驱动侧）**
提供架构无关的自旋锁实现，用于：
1) 内核内部短小临界区保护（对象表、设备表等）；
2) 中断服务程序与任务共享数据的保护；

### `typedef struct {`

```c
typedef struct {
```

**自旋锁对象**
lock 为原子访问的锁字（0 空闲 / 1 占用），owner 记录持有者 CPU，
nest 记录同一 CPU 的嵌套层数。

### `#define ARKRTOS_SPINLOCK_INIT          { 0u, 0xffffffffu, 0u }`

```c
#define ARKRTOS_SPINLOCK_INIT          { 0u, 0xffffffffu, 0u }
```

**Static initialiser for a global lock object.**
> 说明：Must match arkrtos_spin_init(): owner 0xffffffff means "no owner", so a
freshly defined lock cannot be mistaken for one held by CPU 0. */

### `#define ARKRTOS_SPINLOCK_DEFINE(name)  arkrtos_spinlock_t name = ARKRT`

```c
#define ARKRTOS_SPINLOCK_DEFINE(name)  arkrtos_spinlock_t name = ARKRTOS_SPINLOCK_INIT
```

**定义并初始化一把全局自旋锁**
- `name`：锁变量名
ARKRTOS_SPINLOCK_DEFINE(g_dev_lock);

### `static inline void arkrtos_spin_init(arkrtos_spinlock_t *p_lock) {`

```c
static inline void arkrtos_spin_init(arkrtos_spinlock_t *p_lock) {
```

**初始化（或重新初始化）自旋锁**
- `p_lock`：锁对象指针
> 说明：仅可在确认无人持有该锁时调用。

### `static inline int32 arkrtos_spin_trylock(arkrtos_spinlock_t *p_lock) {`

```c
static inline int32 arkrtos_spin_trylock(arkrtos_spinlock_t *p_lock) {
```

**尝试获取自旋锁（非阻塞）**
- `p_lock`：锁对象指针
**返回**：1=获取成功，0=锁已被其他 CPU 持有
> 说明：同一 CPU 重入时直接累加嵌套计数并返回成功，避免自死锁。

### `static inline void arkrtos_spin_lock(arkrtos_spinlock_t *p_lock) {`

```c
static inline void arkrtos_spin_lock(arkrtos_spinlock_t *p_lock) {
```

**获取自旋锁（阻塞自旋直到成功）**
- `p_lock`：锁对象指针
> 说明：只能在任务或中断上下文短暂自旋，禁止在持锁期间调用任何
可能引起任务切换或长时间阻塞的接口。

### `static inline void arkrtos_spin_unlock(arkrtos_spinlock_t *p_lock) {`

```c
static inline void arkrtos_spin_unlock(arkrtos_spinlock_t *p_lock) {
```

**释放自旋锁**
- `p_lock`：锁对象指针
> 说明：非持有者调用时直接返回（防御性处理，不破坏锁状态）。

### `static inline void arkrtos_spin_lock_irqsave(arkrtos_spinlock_t *p_loc`

```c
static inline void arkrtos_spin_lock_irqsave(arkrtos_spinlock_t *p_lock, uint32 *p_state) {
```

**关中断并获取自旋锁（中断与任务共用数据的标准做法）**
- `p_lock`：锁对象指针
- `p_state`：输出参数，保存进入前的中断状态，供解锁时恢复
> 说明：顺序为「先关中断，再抢锁」，与 unlock_irqrestore 严格对称。

### `static inline void arkrtos_spin_unlock_irqrestore(arkrtos_spinlock_t *`

```c
static inline void arkrtos_spin_unlock_irqrestore(arkrtos_spinlock_t *p_lock, uint32 state) {
```

**释放自旋锁并恢复中断状态**
- `p_lock`：锁对象指针
- `state`：arkrtos_spin_lock_irqsave 保存的中断状态

### `static inline int32 arkrtos_spin_is_locked(arkrtos_spinlock_t *p_lock)`

```c
static inline int32 arkrtos_spin_is_locked(arkrtos_spinlock_t *p_lock) {
```

**查询自旋锁当前是否被持有**
- `p_lock`：锁对象指针
**返回**：1=已持有，0=空闲

## kernelsrc/include/arkrtos_svc_abi.h

**SVC sub-command numbers: one table per service, shared by both sides.**
An SVC call carries a service number in the instruction immediate
(ARKRTOS_SVC_* in arkrtos_def.h) and, for every service that has
more than one operation, a sub-command in p[0] / parameters[0].

### `typedef unsigned long arkrtos_svc_arg_t;`

```c
typedef unsigned long arkrtos_svc_arg_t;
```

**参数块里的一个字：原生指针宽度（宽档 = 8 字节）。 */**

### `#define ARKRTOS_SVC_SUB(sub)            ((arkrtos_svc_arg_t)((unsigned`

```c
#define ARKRTOS_SVC_SUB(sub)            ((arkrtos_svc_arg_t)((unsigned long)(sub) | ARKRTOS_SVC_SUB_WORD))
```

**宽档写子命令字：置位标记，窄档内核据此拒绝而不是误读。 */**

### `#define ARKRTOS_SVC_SUB_OF(w)           ((unsigned long)(w) & ~ARKRTOS`

```c
#define ARKRTOS_SVC_SUB_OF(w)           ((unsigned long)(w) & ~ARKRTOS_SVC_SUB_WORD)
```

**从子命令字取回子命令号（两档都能用）。 */**

### `#define ARKRTOS_SVC_ARG_BYTES(n)        ((unsigned long)(n) * (sizeof(`

```c
#define ARKRTOS_SVC_ARG_BYTES(n)        ((unsigned long)(n) * (sizeof(arkrtos_svc_arg_t) / 4u)) #else typedef unsigned int arkrtos_svc_arg_t;
```

**参数块里 n 个 32 位字对应的字节数：窄档 n，宽档 2n。 */**

## kernelsrc/include/arkrtos_svc_call.h

**Toolchain-independent SVC entry helpers for the App / Driver SDK.**
The unprivileged side (App, Driver) never touches kernel internals:
every system service is reached with an SVC instruction carrying the
service number as its 8-bit immediate. How that instruction is

### `#define ARKRTOS_SVC_DECL_1(ret, num, name, t0)          ret __svc(num)`

```c
#define ARKRTOS_SVC_DECL_1(ret, num, name, t0)          ret __svc(num) name(t0);
```

Declare an SVC service that takes one argument and returns a value. */

### `#define ARKRTOS_SVC_DECL_2(ret, num, name, t0, t1)      ret __svc(num)`

```c
#define ARKRTOS_SVC_DECL_2(ret, num, name, t0, t1)      ret __svc(num) name(t0, t1);
```

Declare an SVC service that takes two arguments and returns a value. */

### `#define ARKRTOS_SVC_DECL_3(ret, num, name, t0, t1, t2)  ret __svc(num)`

```c
#define ARKRTOS_SVC_DECL_3(ret, num, name, t0, t1, t2)  ret __svc(num) name(t0, t1, t2);
```

Declare an SVC service that takes three arguments and returns a value. */

### `#define ARKRTOS_SVC_DECL_V1(num, name, t0)              void __svc(num`

```c
#define ARKRTOS_SVC_DECL_V1(num, name, t0)              void __svc(num) name(t0);
```

Declare an SVC service that takes one argument and returns nothing. */

### `#define ARKRTOS_SVC_DECL_V2(num, name, t0, t1)          void __svc(num`

```c
#define ARKRTOS_SVC_DECL_V2(num, name, t0, t1)          void __svc(num) name(t0, t1);
```

Declare an SVC service that takes two arguments and returns nothing. */

### `#define ARKRTOS_SVC_DECL_1(ret, num, name, t0)                        `

```c
#define ARKRTOS_SVC_DECL_1(ret, num, name, t0)                            \ ARKRTOS_INLINE ret name(t0 a0)                                        \ {                                                                     \ register unsigned long r_a0 __asm("a0") = (unsigned long)(a0);     \ register unsigned long r_a7 __asm("a7") = (unsigned long)(num);    \ __asm volatile ("ecall" : "+r"(r_a0) : "r"(r_a7) : "memory");      \
```

Declare an SVC service that takes one argument and returns a value. */

### `#define ARKRTOS_SVC_DECL_2(ret, num, name, t0, t1)                    `

```c
#define ARKRTOS_SVC_DECL_2(ret, num, name, t0, t1)                        \ ARKRTOS_INLINE ret name(t0 a0, t1 a1)                                 \ {                                                                     \ register unsigned long r_a0 __asm("a0") = (unsigned long)(a0);     \ register unsigned long r_a1 __asm("a1") = (unsigned long)(a1);     \ register unsigned long r_a7 __asm("a7") = (unsigned long)(num);    \
```

Declare an SVC service that takes two arguments and returns a value. */

### `#define ARKRTOS_SVC_DECL_3(ret, num, name, t0, t1, t2)                `

```c
#define ARKRTOS_SVC_DECL_3(ret, num, name, t0, t1, t2)                    \ ARKRTOS_INLINE ret name(t0 a0, t1 a1, t2 a2)                          \ {                                                                     \ register unsigned long r_a0 __asm("a0") = (unsigned long)(a0);     \ register unsigned long r_a1 __asm("a1") = (unsigned long)(a1);     \ register unsigned long r_a2 __asm("a2") = (unsigned long)(a2);     \
```

Declare an SVC service that takes three arguments and returns a value. */

### `#define ARKRTOS_SVC_DECL_V1(num, name, t0)                            `

```c
#define ARKRTOS_SVC_DECL_V1(num, name, t0)                                \ ARKRTOS_INLINE void name(t0 a0)                                       \ {                                                                     \ register unsigned long r_a0 __asm("a0") = (unsigned long)(a0);     \ register unsigned long r_a7 __asm("a7") = (unsigned long)(num);    \ __asm volatile ("ecall" : "+r"(r_a0) : "r"(r_a7) : "memory");      \
```

Declare an SVC service that takes one argument and returns nothing. */

### `#define ARKRTOS_SVC_DECL_V2(num, name, t0, t1)                        `

```c
#define ARKRTOS_SVC_DECL_V2(num, name, t0, t1)                            \ ARKRTOS_INLINE void name(t0 a0, t1 a1)                                \ {                                                                     \ register unsigned long r_a0 __asm("a0") = (unsigned long)(a0);     \ register unsigned long r_a1 __asm("a1") = (unsigned long)(a1);     \ register unsigned long r_a7 __asm("a7") = (unsigned long)(num);    \
```

Declare an SVC service that takes two arguments and returns nothing. */

### `#define ARKRTOS_SVC_DECL_1(ret, num, name, t0)                        `

```c
#define ARKRTOS_SVC_DECL_1(ret, num, name, t0)                            \ ARKRTOS_INLINE ret name(t0 a0)                                        \ {                                                                     \ register unsigned long r_a0 __asm("a0") = (unsigned long)(a0);     \ __asm volatile ("syscall " ARKRTOS_STR(num) : "+r"(r_a0) : : "memory"); \ return (ret)r_a0;                                                 \
```

Declare an SVC service that takes one argument and returns a value. */

### `#define ARKRTOS_SVC_DECL_2(ret, num, name, t0, t1)                    `

```c
#define ARKRTOS_SVC_DECL_2(ret, num, name, t0, t1)                        \ ARKRTOS_INLINE ret name(t0 a0, t1 a1)                                 \ {                                                                     \ register unsigned long r_a0 __asm("a0") = (unsigned long)(a0);     \ register unsigned long r_a1 __asm("a1") = (unsigned long)(a1);     \ __asm volatile ("syscall " ARKRTOS_STR(num)                        \
```

Declare an SVC service that takes two arguments and returns a value. */

### `#define ARKRTOS_SVC_DECL_3(ret, num, name, t0, t1, t2)                `

```c
#define ARKRTOS_SVC_DECL_3(ret, num, name, t0, t1, t2)                    \ ARKRTOS_INLINE ret name(t0 a0, t1 a1, t2 a2)                          \ {                                                                     \ register unsigned long r_a0 __asm("a0") = (unsigned long)(a0);     \ register unsigned long r_a1 __asm("a1") = (unsigned long)(a1);     \ register unsigned long r_a2 __asm("a2") = (unsigned long)(a2);     \
```

Declare an SVC service that takes three arguments and returns a value. */

### `#define ARKRTOS_SVC_DECL_V1(num, name, t0)                            `

```c
#define ARKRTOS_SVC_DECL_V1(num, name, t0)                                \ ARKRTOS_INLINE void name(t0 a0)                                       \ {                                                                     \ register unsigned long r_a0 __asm("a0") = (unsigned long)(a0);     \ __asm volatile ("syscall " ARKRTOS_STR(num) : "+r"(r_a0) : : "memory"); \ }
```

Declare an SVC service that takes one argument and returns nothing. */

### `#define ARKRTOS_SVC_DECL_V2(num, name, t0, t1)                        `

```c
#define ARKRTOS_SVC_DECL_V2(num, name, t0, t1)                            \ ARKRTOS_INLINE void name(t0 a0, t1 a1)                                \ {                                                                     \ register unsigned long r_a0 __asm("a0") = (unsigned long)(a0);     \ register unsigned long r_a1 __asm("a1") = (unsigned long)(a1);     \ __asm volatile ("syscall " ARKRTOS_STR(num)                        \
```

Declare an SVC service that takes two arguments and returns nothing. */

### `#define ARKRTOS_SVC_DECL_1(ret, num, name, t0)                        `

```c
#define ARKRTOS_SVC_DECL_1(ret, num, name, t0)                          \ ARKRTOS_INLINE ret name(t0 a0)                                     \ {                                                                 \ register unsigned int r0 __asm("r0") = (unsigned int)a0;      \ __asm volatile ("svc #" ARKRTOS_STR(num) : "+r"(r0) : : "memory"); \ return (ret)r0;                                               \
```

Declare an SVC service that takes one argument and returns a value. */

### `#define ARKRTOS_SVC_DECL_2(ret, num, name, t0, t1)                    `

```c
#define ARKRTOS_SVC_DECL_2(ret, num, name, t0, t1)                      \ ARKRTOS_INLINE ret name(t0 a0, t1 a1)                              \ {                                                                 \ register unsigned int r0 __asm("r0") = (unsigned int)a0;      \ register unsigned int r1 __asm("r1") = (unsigned int)a1;      \ __asm volatile ("svc #" ARKRTOS_STR(num)                        \
```

Declare an SVC service that takes two arguments and returns a value. */

### `#define ARKRTOS_SVC_DECL_3(ret, num, name, t0, t1, t2)                `

```c
#define ARKRTOS_SVC_DECL_3(ret, num, name, t0, t1, t2)                  \ ARKRTOS_INLINE ret name(t0 a0, t1 a1, t2 a2)                       \ {                                                                 \ register unsigned int r0 __asm("r0") = (unsigned int)a0;      \ register unsigned int r1 __asm("r1") = (unsigned int)a1;      \ register unsigned int r2 __asm("r2") = (unsigned int)a2;      \
```

Declare an SVC service that takes three arguments and returns a value. */

### `#define ARKRTOS_SVC_DECL_V1(num, name, t0)                            `

```c
#define ARKRTOS_SVC_DECL_V1(num, name, t0)                              \ ARKRTOS_INLINE void name(t0 a0)                                    \ {                                                                 \ register unsigned int r0 __asm("r0") = (unsigned int)a0;      \ __asm volatile ("svc #" ARKRTOS_STR(num) : "+r"(r0) : : "memory"); \ }
```

Declare an SVC service that takes one argument and returns nothing. */

### `#define ARKRTOS_SVC_DECL_V2(num, name, t0, t1)                        `

```c
#define ARKRTOS_SVC_DECL_V2(num, name, t0, t1)                          \ ARKRTOS_INLINE void name(t0 a0, t1 a1)                             \ {                                                                 \ register unsigned int r0 __asm("r0") = (unsigned int)a0;      \ register unsigned int r1 __asm("r1") = (unsigned int)a1;      \ __asm volatile ("svc #" ARKRTOS_STR(num)                        \
```

Declare an SVC service that takes two arguments and returns nothing. */

## kernelsrc/include/arkrtos_task.h

**Ark RTOS 任务与调度相关定义（内核内部头文件）**
定义任务控制块、任务状态、SVC 上下文结构，以及任务管理和调度器函数。
这些是内核内部接口，应用/驱动一般不直接包含本文件，而是通过 SDK 或 arkrtos.h 使用。
其中 stack_ptr 字段用于保存/恢复任务的栈指针，是上下文切换的关键。

### `typedef struct {`

```c
typedef struct {
```

**任务控制块（TCB）**
保存一个任务的全部运行信息：内存区域、优先级、栈、状态、调度计时等。
所有任务的 TCB 排成 arkrtos_task_table 数组。其中 stack_ptr 在任务被切走时
保存其 PSP，切回时据此恢复现场；mpu 是该任务的 MPU 区域快照。

### `int32 arkrtos_task_block_in_critical(uint32 timeout_ms);`

```c
int32 arkrtos_task_block_in_critical(uint32 timeout_ms);
```

**在调用方已持有的临界区内阻塞当前任务（供信号量/互斥锁/消息队列使用）**
- `timeout_ms`：>0 定时等待（ms）；<=0 无限等待（只能被显式唤醒）
> 说明：这是内核内部原语，不直接对外。调用方（sem/mtx/mq/event）必须先把
对外 API 的“0=不等待、负值=永久等待”约定归一化后再调用，
不能把对外的 0 直接传进来（那会被本原语解释成无限等待）。
**返回**：0=被显式唤醒（已获得资源），1=等待超时，-1=未能进入阻塞（调用方需自行摘除队列）
调用约定：进入时中断已关，返回时中断仍关（调用方在同一临界区内继续处理等待队列）。
与 arkrtos_task_wait_internal 的区别：后者会自行开关中断，
对“先把自己挂进等待队列、再睡下”的原语来说，那中间存在唤醒丢失窗口
（ISR 的唤醒会投给一个还没睡下的任务）。本函数把置状态与切走放在同一临界区内。

### `void arkrtos_task_release_resources(int32 task_id);`

```c
void arkrtos_task_release_resources(int32 task_id);
```

**任务下线收尸：清理该任务在同步对象/消息队列中的等待登记与锁持有关系**
- `task_id`：目标任务号（从 1 开始）
用于任务自杀（kill）、故障恢复、覆盖安装前硬停任务等场景。
不做收尸的后果：post/unlock 会把一个已经不在等待的任务置为 READY
（从旧栈“复活”）；它持有的互斥锁永久锁死，等待者全部饿死。
本函数自带保存-恢复语义的临界区，可在已有临界区内安全调用。

### `void   arkrtos_thread_exit_code_internal(int32 code);`

```c
void   arkrtos_thread_exit_code_internal(int32 code);
```

**带退出码结束当前任务（U-8）。**
- `code`：退出码，原样记进 TCB 的 exit_code 并置 exit_code_set=1。
> 说明：只对「当前任务」有意义；内核线程调用等价于 arkrtos_thread_exit_internal()。
ARKRTOS_USE_EXIT_CODE=0 时不提供这两个入口（不占一个字节）。

### `int32  arkrtos_task_exit_code(int32 task_id, int32 *p_code);`

```c
int32  arkrtos_task_exit_code(int32 task_id, int32 *p_code);
```

**读某个任务上报的退出码。**
- `task_id`：任务号（1 起）
- `p_code`：可为 0；成功时写回退出码
**返回**：1 = 该任务上报过退出码，0 = 没上报过 / 任务号非法

## kernelsrc/include/arkrtos_tickless.h

**Ark RTOS tickless idle（内核内部头文件）**
打开 ARKRTOS_USE_TICKLESS 后，idle 循环不再一拍一次 WFI，而是
先算"下一次真正要干活"离现在还有几拍，再把 SysTick 重新编程成
一段更长的节拍、睡完一次、醒来把这几十拍一次补齐。

### `#define ARKRTOS_TICKLESS_NO_EVENT   0xFFFFFFFFu`

```c
#define ARKRTOS_TICKLESS_NO_EVENT   0xFFFFFFFFu
```

无事件（延时链空 / 没有活动定时器）的哨兵值，给 plan 用 */

### `uint32 arkrtos_tickless_plan(uint32 delay_ticks, uint32 timer_ticks, u`

```c
uint32 arkrtos_tickless_plan(uint32 delay_ticks, uint32 timer_ticks, uint32 cap);
```

**纯计算：这一次最多能睡几拍**
- `delay_ticks`：延时链最近到期还有几拍（无事件给 NO_EVENT）
- `timer_ticks`：软件定时器最近到期还有几拍（无事件给 NO_EVENT）
- `cap`：上限（ARKRTOS_TICKLESS_MAX_TICKS）
**返回**：>= 1 的拍数；< 2 表示"不值得跳"，调用方走普通 WFI
取两者最小，再夹到 [1, cap]。已经是 1 拍也返回 1：调用方据此
走普通 WFI，不做任何重新编程。

### `void arkrtos_tickless_split(uint32 elapsed_counts, uint32 counts_per_t`

```c
void arkrtos_tickless_split(uint32 elapsed_counts, uint32 counts_per_tick, uint32 *ticks, uint32 *remain_counts);
```

**纯计算：实际过去的计数折成"整拍 + 余数"**
- `elapsed_counts`：自编程以来过去的时钟计数
- `counts_per_tick`：一个正常节拍的计数
- `ticks`：输出：整拍数（可以为 0）
- `remain_counts`：输出：不足一拍的余数（相位补偿用）
elapsed < cpt 时 ticks = 0，表示"被提前叫醒但没有走满一拍"。

### `uint32 arkrtos_tickless_idle(void);`

```c
uint32 arkrtos_tickless_idle(void);
```

**idle 循环里的"睡一觉"入口（替代 ARKRTOS_WFI）**
**返回**：本次跳过并在醒来后补发掉的整拍数（0 = 没有跳）
只有在跑到这里时确定"至少两拍内没人需要 CPU"才会重新编程；
否则退化为一次普通 WFI。返回时所有被跳过的节拍都已经补发，
调用方可以直接进入下一轮。

### `uint32 arkrtos_tickless_wake_ticks(void);`

```c
uint32 arkrtos_tickless_wake_ticks(void);
```

**tick 冲突入口：本次 SysTick 中断该补几拍**
**返回**：0 = 没有处在长节拍里（按普通一拍处理）；n > 0 = 一共 n 拍
由 arkrtos_kernel_tick_handler 在中断入口调用一次。它顺带把
SysTick 恢复成正常周期（含相位余数），所以调用之后的中断间隔
立刻回到一拍。读序为"先读 COUNTFLAG 再读 VAL"：读 CTRL 会清
标志，两次读取必须来自同一次结账。
相位不为 0 时恢复出来的是"补满这一拍"的短周期，并在那一次
中断里自己恢复成整拍：短周期若不被收回，之后每一拍都会变短，
节拍就永久性地快了。

### `void arkrtos_tickless_stats(uint32 *isrs, uint32 *sleeps);`

```c
void arkrtos_tickless_stats(uint32 *isrs, uint32 *sleeps);
```

**累计读数：SysTick 中断次数与长睡眠次数**
- `isrs`：输出：进入 SysTick 中断的次数（一个中断算一次，不管它补了几拍）
- `sleeps`：输出：成功编程为长节拍并进 WFI 的次数（0 = 没睡过）
板端唯一能证明"tickless 真的在睡"的读数：没有它，"节拍没变慢"
与"开关没生效"在串口上长得一模一样。两个指针都可以传空。
"跳过的拍数"不单独计数，直接用 `arkrtos_kernel_get_tick() - isrs`
算：每个中断至少贡献一拍，剩下的必然是补发路径走完的。
所以 `节拍总数 == isrs + skipped` 是定义而不是测得量。
真正有信息量的是 `isrs` 与节拍总数的比值（4 倍睡眠时约 1:4）。
计数只增不清，复位才归零。

## kernelsrc/include/arkrtos_ulog.h

**Ark RTOS 用户态服务接口：日志（SVC 0x19）与控制台 Shell（SVC 0x1A）**
这是 App / 驱动固件侧唯一需要包含的两个「内核外设」头文件之一
（另一个是 arkrtos.h / arkrtos_driver_sdk.h）。它同时解决两件事：
1) 日志

### `ARKRTOS_UFMT_INLINE int arkrtos_ufmt_v(char *out, uint32 size, const c`

```c
ARKRTOS_UFMT_INLINE int arkrtos_ufmt_v(char *out, uint32 size, const char *fmt, va_list ap) {
```

**格式化核心：参数已取成 va_list（供带 ... 的包装函数复用）**
**返回**：实际写入的字符数（不含结尾 '\0'）

### `ARKRTOS_UFMT_INLINE int arkrtos_ufmt(char *out, uint32 size, const cha`

```c
ARKRTOS_UFMT_INLINE int arkrtos_ufmt(char *out, uint32 size, const char *fmt, ...) {
```

**把格式化结果写入 out（最多 size-1 个字符 + '\0'）**
**返回**：实际写入的字符数（不含结尾 '\0'）

### `int32 arkrtos_log_print(uint32 level, const char *tag, const char *msg`

```c
int32 arkrtos_log_print(uint32 level, const char *tag, const char *msg);
```

**输出一条日志（级别过滤在核内统一完成）**
- `level`：ARKRTOS_LOG_NONE..ARKRTOS_LOG_DEBUG
- `tag`：标签（短字符串，NUL 结尾，位于调用者自身固件或 RAM 均可）
- `msg`：正文（NUL 结尾）
**返回**：0=已提交内核；-1=参数被内核拒绝

### `ARKRTOS_UFMT_INLINE int32 arkrtos_log_printf(uint32 level, const char `

```c
ARKRTOS_UFMT_INLINE int32 arkrtos_log_printf(uint32 level, const char *tag, const char *fmt, ...) {
```

**格式化并输出一条日志**
- `level`：级别
- `tag`：标签
- `fmt`：格式串（子集见文件头说明）
> 说明：在用户侧完成格式化，只需一条 SVC；不需要额外的符号实现。

### `ARKRTOS_UFMT_INLINE int32 arkrtos_shell_printf(const char *fmt, ...) {`

```c
ARKRTOS_UFMT_INLINE int32 arkrtos_shell_printf(const char *fmt, ...) {
```

**格式化并打印到内核 Shell 控制台**
> 说明：与 arkrtos_log_printf 同样在用户侧完成格式化。

## kernelsrc/include/arkrtos_ushell.h

**用户态 Shell 控制台服务：描述符与接口（SVC 0x1A）**
内核分发层需要按同样的内存布局读取用户传来的命令描述符，
用户侧要按同样的布局填写它，所以结构体与长度上限定义在这一个
头文件里，两侧共同包含，避免「各写一份、改一处忘一处」。

### `typedef struct {`

```c
typedef struct {
```

**用户态 Shell 命令描述符**
> 说明：注册时内核会复制 name / help 指向的字符串，所以用户侧可以用
字符串常量，也可以在注册完成后释放自己的缓冲。
func 必须是注册者自身固件区内的函数：内核会按「调用者自己的
ROM 区间」校验，防止借 Shell 的特权态去执行别人的代码。

### `int32 arkrtos_shell_cmd_register(const arkrtos_ushell_cmd_t *cmd);`

```c
int32 arkrtos_shell_cmd_register(const arkrtos_ushell_cmd_t *cmd);
```

**在内核 Shell 控制台上注册一条命令**
- `cmd`：描述符（必须位于用户可见 RAM）
**返回**：ARKRTOS_USHELL_OK 或负错误码

### `int32 arkrtos_shell_cmd_unregister(const char *name);`

```c
int32 arkrtos_shell_cmd_unregister(const char *name);
```

**注销一条命令（按名字，不区分大小写）**
**返回**：ARKRTOS_USHELL_OK 或负错误码

### `int32 arkrtos_shell_print(const char *msg);`

```c
int32 arkrtos_shell_print(const char *msg);
```

**向内核 Shell 控制台输出一段文本**
- `msg`：NUL 结尾字符串；是否换行由调用者决定（要换行就带 "\r\n"）
**返回**：ARKRTOS_USHELL_OK 或负错误码

## kernelsrc/include/arkrtos_utest.h

**板端单元测试框架（utest）——用例自动注册 + 断言记账 + 汇总报告**
形态对齐 RT-Thread 的 utest / Zephyr 的 ztest：
- 用例用 UTEST_CASE(id) 定义，由静态初始化期构造函数自动登记，
不需要维护任何中心清单；

### `typedef void (*ark_utest_fn_t)(void);`

```c
typedef void (*ark_utest_fn_t)(void);
```

用例函数：无参无返回，失败通过断言宏记账 */

### `typedef struct {`

```c
typedef struct {
```

一条用例：展示名（建议 "模块.场景"）+ 函数 */

### `typedef struct {`

```c
typedef struct {
```

一次运行的汇总账 */

### `typedef void (*ark_utest_out_t)(const char *s);`

```c
typedef void (*ark_utest_out_t)(const char *s);
```

输出汇：框架把每一行交给它；默认 NULL 表示丢弃输出 */

### `void ark_utest_register(const ark_utest_case_t *c);`

```c
void ark_utest_register(const ark_utest_case_t *c);
```

用例自注册。通常由 UTEST_CASE 宏自动调用；重复注册同一指针会被忽略 */

### `uint32_t ark_utest_list(void);`

```c
uint32_t ark_utest_list(void);
```

列出全部已注册用例（名字 + 序号）；返回用例数 */

### `uint32_t ark_utest_run(const char *pattern, ark_utest_summary_t *out);`

```c
uint32_t ark_utest_run(const char *pattern, ark_utest_summary_t *out);
```

**运行用例**
- `pattern`：子串过滤；NULL 或空串表示全部
- `out`：若非 NULL，填汇总账
**返回**：失败用例数（0 表示全过）

### `void ark_utest_bump(void);`

```c
void ark_utest_bump(void);
```

记一条断言（无论成败都计数）；仅可在用例函数内调用 */

### `void ark_utest_fail(const char *file, int line, const char *expr, cons`

```c
void ark_utest_fail(const char *file, int line, const char *expr, const char *detail);
```

记一条失败；detail 可为 NULL */

### `void ark_utest_fail_num(const char *file, int line, const char *expr, `

```c
void ark_utest_fail_num(const char *file, int line, const char *expr, long a, long b);
```

记一条数值不等；以有符号十进制与十六进制两种口径打印 */

### `int  ark_utest_ctx_failed(void);`

```c
int  ark_utest_ctx_failed(void);
```

当前用例是否已出现失败（供 UTEST_REQUIRE 提前收手） */

### `void ark_utest_skip(const char *reason);`

```c
void ark_utest_skip(const char *reason);
```

记一条跳过并置位；调用方随后 return */

### `int  ark_utest_ctx_skipped(void);`

```c
int  ark_utest_ctx_skipped(void);
```

当前用例是否已被跳过 */

### `#define ARK_UTEST_CASE_DEF(_ident)                                    `

```c
#define ARK_UTEST_CASE_DEF(_ident)                                        \ static void utest_body_##_ident(void);                                \ static const ark_utest_case_t utest_case_##_ident = {                 \ #_ident, utest_body_##_ident };                                   \ static void __attribute__((constructor)) utest_autoreg_##_ident(void) \ {                                                                     \
```

注册一条用例（文件作用域辅助）：供 UTEST_CASE 宏展开使用 */

### `#define UTEST_CASE(_ident) ARK_UTEST_CASE_DEF(_ident)`

```c
#define UTEST_CASE(_ident) ARK_UTEST_CASE_DEF(_ident)
```

**定义一条用例**
- `_ident`：唯一标识符（同一翻译单元内不得重名）；同时作为展示名
用法：
UTEST_CASE(mempool_alloc_free)
{
...
UTEST_CHECK_EQ(a, b);
}

### `#define UTEST_CHECK(_expr)                                        \ do`

```c
#define UTEST_CHECK(_expr)                                        \ do {                                                          \ ark_utest_bump();                                         \ if(!(_expr))                                              \ {                                                         \ ark_utest_fail(__FILE__, __LINE__, #_expr, (const char *)0); \
```

真值断言：失败只记账，继续跑后面的断言 */

### `#define UTEST_REQUIRE(_expr)                                      \ do`

```c
#define UTEST_REQUIRE(_expr)                                      \ do {                                                          \ UTEST_CHECK(_expr);                                       \ if(ark_utest_ctx_failed())                                \ {                                                         \ return;                                               \
```

前置断言：失败记一条账后**立即返回**（后续断言不再执行）。
用于"不满足就没法继续"的前置条件，避免拿坏状态继续跑出假绿。

### `#define UTEST_CHECK_EQ(_a, _b)                                    \ do`

```c
#define UTEST_CHECK_EQ(_a, _b)                                    \ do {                                                          \ long utest_a_ = (long)(_a);                               \ long utest_b_ = (long)(_b);                               \ ark_utest_bump();                                         \ if(utest_a_ != utest_b_)                                  \
```

数值相等（long 口径）*/

### `#define UTEST_CHECK_NE(_a, _b)                                    \ do`

```c
#define UTEST_CHECK_NE(_a, _b)                                    \ do {                                                          \ long utest_a_ = (long)(_a);                               \ long utest_b_ = (long)(_b);                               \ ark_utest_bump();                                         \ if(utest_a_ == utest_b_)                                  \
```

数值不等（long 口径）*/

### `#define UTEST_CHECK_RANGE(_lo, _x, _hi)                           \ do`

```c
#define UTEST_CHECK_RANGE(_lo, _x, _hi)                           \ do {                                                          \ long utest_lo_ = (long)(_lo);                             \ long utest_x_  = (long)(_x);                              \ long utest_hi_ = (long)(_hi);                             \ ark_utest_bump();                                         \
```

区间断言：lo <= x && x <= hi */

### `#define UTEST_FAIL(_msg)                                          \ do`

```c
#define UTEST_FAIL(_msg)                                          \ do {                                                          \ ark_utest_bump();                                         \ ark_utest_fail(__FILE__, __LINE__, "FAIL", (_msg));       \ } while(0)
```

无条件失败（用于"走到这里本不该发生"的分支）*/

### `#define UTEST_SKIP(_reason)                                       \ do`

```c
#define UTEST_SKIP(_reason)                                       \ do {                                                          \ ark_utest_skip(_reason);                                  \ return;                                                   \ } while(0)
```

主动跳过当前用例（其后内容不再执行）*/

## kernelsrc/include/arkrtos_version.h

**Ark RTOS release version, defined in exactly one place.**
The kernel, the shell and any application that includes arkrtos.h read
these macros, so the version never has to be retyped as a string.
The git tag and the README badge carry the same three numbers and

### `#define ARKRTOS_VERSION_MAJOR     0   /*!< Major, matches the first ta`

```c
#define ARKRTOS_VERSION_MAJOR     0   /*!< Major, matches the first tag field.  */ #define ARKRTOS_VERSION_MINOR     0   /*!< Minor, matches the second tag field. */ #define ARKRTOS_VERSION_PATCH     3   /*!< Patch, matches the third tag field.  */
```

### `#define ARKRTOS_VERSION_NUM       (((unsigned)ARKRTOS_VERSION_MAJOR <<`

```c
#define ARKRTOS_VERSION_NUM       (((unsigned)ARKRTOS_VERSION_MAJOR << 24) | \ ((unsigned)ARKRTOS_VERSION_MINOR << 16) | \ ((unsigned)ARKRTOS_VERSION_PATCH << 8))
```

The three numbers packed into one word: major << 24 | minor << 16 | patch << 8. */

## kernelsrc/include/arkrtos_vfs.h

**Ark RTOS 的 Linux 风格路径命名空间（基于 ark_vfs）**
内核里原本只有「设备名」这一种名字（arkrtos_dev_open("uart0")）和
littlefs 的「卷内路径」这一种路径，两者互不相通。本模块把
ark_vfs 挂到内核上，让所有资源都落在同一棵目录树里：

### `#define ARKRTOS_VFS_PATH_MAX   (64u)`

```c
#define ARKRTOS_VFS_PATH_MAX   (64u)
```

路径缓冲长度，含结尾 0。与 ark_vfs 的 ARK_VFS_PATH_MAX 必须一致 */

### `#define ARKRTOS_VFS_ENOSYS     (-12)`

```c
#define ARKRTOS_VFS_ENOSYS     (-12)
```

**本构建未启用 ARKRTOS_USE_VFS 时，全部门面调用返回此码。**
> 说明：值与 ark_vfs 的 ARK_E_NOSYS 相同，arkrtos_vfs.c 里有一条静态断言
盯着这个等式（开启构建里会检查），所以两个数字不会走偏。
用法上和 ARK_E_NOSYS 一样：这是「本构建没这个功能」，不是
「你写的路径不存在」，报告时不要混为一谈。

### `#define ARKRTOS_VFS_EINVAL     (-2)`

```c
#define ARKRTOS_VFS_EINVAL     (-2)
```

**参数不合法（路径指针/长度/缓冲越出调用者自己的 RAM，或标志组合**
本身矛盾）。值与 ark_vfs 的 ARK_E_INVAL 相同。
> 说明：内核侧的参数校验失败必须回这个码，不能回 -1：-1 是 ARK_E_NOENT，
回 -1 会让 App 把「你的路径太长」读成「文件不存在」——那正是
「看似权威的错答案」。所以 11..18 各分支的校验失败一律用它。

### `typedef struct {`

```c
typedef struct {
```

**挂载点的一行信息，用于 shell 的 mount 命令**
> 说明：fs/source 两个指针指向只读常量，调用者不得释放或改写。

### `typedef struct {`

```c
typedef struct {
```

**一个目录项／一个节点的元数据，字段含义与 POSIX struct stat 对齐。**
> 说明：uid/gid 是数字。这里没有用户数据库，所以不去把 0 翻译成 "root"——
那会是一个看着权威、却查无此人的答案。mtime 为 0 表示该文件系统
不存时间，不是「1970-01-01」。

### `typedef int (*arkrtos_vfs_list_cb_t)(const char *name, const arkrtos_v`

```c
typedef int (*arkrtos_vfs_list_cb_t)(const char *name, const arkrtos_vfs_stat_t *st, void *arg);
```

**目录列举回调，返回 0 继续、-1 停止 */**

### `int32 arkrtos_vfs_init(void);`

```c
int32 arkrtos_vfs_init(void);
```

**启用 VFS：装钩子、挂 / 与 /dev。**
**返回**：0=成功；负值为 ark_vfs 错误码（见 arkrtos_vfs_error_name）
> 说明：幂等：重复调用不会重复挂载。卷（/mnt/...）不在这里挂——哪块设备
上放着文件系统是板级知识，内核不该猜。

### `uint8 arkrtos_vfs_ready(void);`

```c
uint8 arkrtos_vfs_ready(void);
```

**1=arkrtos_vfs_init() 已经成功过 */**

### `int32 arkrtos_vfs_mount_volume(const char *dev, uint32 offset, uint32 `

```c
int32 arkrtos_vfs_mount_volume(const char *dev, uint32 offset, uint32 size, const char *target);
```

**把一块设备窗口作为持久卷挂到 target，并同时挂上 littlefs。**
- `dev`：已注册的块设备名，如 "nor0"
- `offset`：卷在设备内的起始字节
- `size`：卷长度，0=到设备末尾
- `target`：挂载点，如 "/mnt/nor"
**返回**：0=成功；ARK_E_INVAL 参数/窗口非法；ARK_E_NOENT 设备不存在；
ARK_E_IO 设备起不来；ARK_E_EXIST 该点已挂或已有卷占用；
ARK_E_NOSYS 本构建未启用文件系统或 littlefs 桥（见 arkrtos_features.h）
> 说明：必须先 arkrtos_vfs_init()。卷格式不对（未 format）会返回 ARK_E_IO：
挂载不格式化，这是有意的——格式化是破坏性动作，得有人明确下令。

### `int32 arkrtos_vfs_umount(const char *target);`

```c
int32 arkrtos_vfs_umount(const char *target);
```

**卸载 target 处的挂载点，并在它是持久卷时同步卸载 littlefs */**

### `int32 arkrtos_vfs_mount_info(uint32 idx, arkrtos_vfs_mount_info_t *out`

```c
int32 arkrtos_vfs_mount_info(uint32 idx, arkrtos_vfs_mount_info_t *out);
```

**取第 idx 个挂载点信息。**
**返回**：0=成功，-1=越界

### `int32 arkrtos_vfs_mount_count(void);`

```c
int32 arkrtos_vfs_mount_count(void);
```

**挂载点数量 */**

### `int32 arkrtos_vfs_read_file(const char *path, uint8 *buf, uint32 max, `

```c
int32 arkrtos_vfs_read_file(const char *path, uint8 *buf, uint32 max, uint32 *out_len);
```

**读整个文件；*out_len 收实际长度（可为 NULL）**
> 说明：缓冲区必须装得下整个文件，否则报错而不是给你一半——
要看大小不确定的文件（如 shell 的 cat）用下面三个流式入口。 */

### `int32 arkrtos_vfs_read_at(const char *path, uint8 *buf, uint32 len, ui`

```c
int32 arkrtos_vfs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off, uint32 *out_len);
```

**从指定偏移读一段字节（一次性开、读、关，不把句柄交给调用方）。**
- `path`：路径；buf 目标缓冲；len 想读的字节数；off 文件内起始偏移
- `out_len`：收实际读到的字节数（== len 表示读满；< len 表示到文件尾）
**返回**：0=成功（含「文件短于 off+len」这种正常情况）；负值为 ark_vfs 错误码
> 说明：为什么需要它、而不是让调用方 open+seek+read：装载路径（mini）读的是
「镜像头 / 重定位表 / 负载分块」三段定长片段，用 read_file 读整个文件
会把一份大镜像整份塞进 RAM；自己拼 open+seek+read 又要在三个地方各自
记一遍「lseek 的返回值不能当新偏移用」（ramfs 回新偏移、littlefs 桥回 0）。
门面只回「读到多少字节」，短读是读数而不是错误。
> 说明：线程/中断约束与 open_read 相同：同一时刻只能有一个读者拿着这份文件缓存，
调用方负责串行化（mini 在调度器锁内调用）。

### `int32 arkrtos_vfs_size(const char *path, uint32 *out_size);`

```c
int32 arkrtos_vfs_size(const char *path, uint32 *out_size);
```

**取一个文件的字节数（等价于 stat 里的 size 字段）。**
- `path`：路径；out_size 收字节数（失败时置 0）
**返回**：0=成功；负值为 ark_vfs 错误码
> 说明：为什么单独一个门面：装载器要判断「文件末尾 32 字节之外还有没有内容」，
需要文件总长，而 open+seek(END) 的返回值各后端约定不同（ramfs 回新
偏移、littlefs 桥回 0），拿它当长度会静默偏掉。这里只回 size，一个数。

### `int32 arkrtos_vfs_open_read(const char *path);`

```c
int32 arkrtos_vfs_open_read(const char *path);
```

**按路径只读打开一个文件。**
**返回**：>=0 文件句柄；负值为 ark_vfs 错误码
> 说明：只放出只读打开这一种：写清一色走 arkrtos_vfs_write_file()，
多一个半成品标志集只会让人以为那几种组合都试过了。
句柄用 arkrtos_vfs_close() 归还，不要跨挂载卸载一直拿着。

### `int32 arkrtos_vfs_read(int32 fd, uint8 *buf, uint32 len);`

```c
int32 arkrtos_vfs_read(int32 fd, uint8 *buf, uint32 len);
```

**从当前偏移读取。**
**返回**：>0 实际读到的字节数；0 文件尾；负值为 ark_vfs 错误码

### `int32 arkrtos_vfs_close(int32 fd);`

```c
int32 arkrtos_vfs_close(int32 fd);
```

**归还句柄；负值为 ark_vfs 错误码 */**

### `int32 arkrtos_vfs_write_file(const char *path, const uint8 *buf, uint3`

```c
int32 arkrtos_vfs_write_file(const char *path, const uint8 *buf, uint32 len);
```

**写整个文件（创建或截断）**
**返回**：0=成功；负值为 ark_vfs 错误码。**成功时不返回字节数**：
实现走 ark_vfs_write_all()，它的返回值是 ARK_VFS_OK(0)，
不是长度；拿 rc 跟 len 比会把写成功的文件判成失败。
容量不够这类短写由底层返回负码，不会给你“写了一半=成功”。 */

### `int32 arkrtos_vfs_remove(const char *path);`

```c
int32 arkrtos_vfs_remove(const char *path);
```

**删除一个文件 */**

### `int32 arkrtos_vfs_chmod(const char *path, uint32 mode);`

```c
int32 arkrtos_vfs_chmod(const char *path, uint32 mode);
```

**改访问权限位（只用 mode 的 ARKRTOS_VFS_S_PERM 部分）。**
**返回**：0=成功；负值为 ark_vfs 错误码。文件系统没有 chmod 回调时回
ARK_E_NOSYS（-12）——「这个卷不支持」而不是「改好了」。

### `int32 arkrtos_vfs_chown(const char *path, uint32 uid, uint32 gid);`

```c
int32 arkrtos_vfs_chown(const char *path, uint32 uid, uint32 gid);
```

**改属主（数字 uid/gid，这里没有用户数据库）。**
**返回**：0=成功；负值为 ark_vfs 错误码。文件系统没有 chown 回调时回
ARK_E_NOSYS（-12）。

### `int32 arkrtos_vfs_list(const char *dir, arkrtos_vfs_list_cb_t cb, void`

```c
int32 arkrtos_vfs_list(const char *dir, arkrtos_vfs_list_cb_t cb, void *arg);
```

**列一层目录 */**

### `const char *arkrtos_vfs_error_name(int32 rc);`

```c
const char *arkrtos_vfs_error_name(int32 rc);
```

**把 ark_vfs 错误码翻成短符号名，用于报告。**
> 说明：报告里必须同时打印数字：符号只是附加说明，不能代替设备/文件系统
自己给出的原因。

### `#define ARKRTOS_VFS_O_RDONLY     (0x0000u) #define ARKRTOS_VFS_O_WRONL`

```c
#define ARKRTOS_VFS_O_RDONLY     (0x0000u) #define ARKRTOS_VFS_O_WRONLY     (0x0001u) #define ARKRTOS_VFS_O_RDWR       (0x0002u) #define ARKRTOS_VFS_O_ACCMODE    (0x0003u) #define ARKRTOS_VFS_O_CREAT      (0x0004u) #define ARKRTOS_VFS_O_TRUNC      (0x0008u)
```

**open 标志。数值是 Ark RTOS 自己的，arkrtos_vfs.c 里逐位映射到**
ark_vfs 的 ARK_O_*——App 不 include ark_vfs 的头，所以上游的值
不能当 ABI 用。 */

### `#define ARKRTOS_VFS_S_IFCHR      (0x2000u) #define ARKRTOS_VFS_S_IFDIR`

```c
#define ARKRTOS_VFS_S_IFCHR      (0x2000u) #define ARKRTOS_VFS_S_IFDIR      (0x4000u) #define ARKRTOS_VFS_S_IFREG      (0x8000u) #define ARKRTOS_VFS_S_IFMT       (0xF000u)
```

**stat 的文件类型位，与 POSIX 的 S_IFMT 同形，映射到 ARK_S_IF*。 */**

### `#define ARKRTOS_VFS_S_IRUSR      (0x0100u) #define ARKRTOS_VFS_S_IWUSR`

```c
#define ARKRTOS_VFS_S_IRUSR      (0x0100u) #define ARKRTOS_VFS_S_IWUSR      (0x0080u) #define ARKRTOS_VFS_S_IXUSR      (0x0040u) #define ARKRTOS_VFS_S_IRGRP      (0x0020u) #define ARKRTOS_VFS_S_IWGRP      (0x0010u) #define ARKRTOS_VFS_S_IXGRP      (0x0008u)
```

**权限位，布局与 POSIX 的 rwx 三元组一致，数值与 ark_vfs 的**
ARK_S_I* 相同（arkrtos_vfs.c 里有静态断言盯着这个等式）。
类型位在 ARKRTOS_VFS_S_PERM 之上，两者可分别用 S_IFMT / S_PERM
取出。设备节点（/dev 下）按惯例是 0666，普通文件 0644、目录 0755。 */

### `#define ARKRTOS_VFS_SEEK_SET     (0u) #define ARKRTOS_VFS_SEEK_CUR    `

```c
#define ARKRTOS_VFS_SEEK_SET     (0u) #define ARKRTOS_VFS_SEEK_CUR     (1u) #define ARKRTOS_VFS_SEEK_END     (2u)
```

**seek 起点，与 POSIX 的 SEEK_* 同值 */**

### `#define ARKRTOS_VFS_HANDLE_FLAG  (0x01300000)`

```c
#define ARKRTOS_VFS_HANDLE_FLAG  (0x01300000)
```

**句柄号的高位标志：句柄 = 标志 | (序号 << 8) | 槽号。**
序号 1..255（0 留给「从没用过」），槽号 < 256；槽被回收后重新
发出去时序号加一，所以旧号码在新持有者那里一律 BADF。
与设备句柄的 ARKRTOS_DEV_HANDLE_FLAG 不同值，两者不会互相误认。 */

### `#define ARKRTOS_VFS_HANDLE_MAX   (8u)`

```c
#define ARKRTOS_VFS_HANDLE_MAX   (8u)
```

**同时打开的 VFS 句柄数。注意底层 ark_vfs 的 fd 表全机器只有**
ARK_VFS_MAX_FDS 张，与内核自身（shell 的 cat/ls 等）共用，
所以打满时 open 回 ARK_E_FULL(-3) 而不是静默成功。 */

### `int32 arkrtos_vfs_app_open(const char *path, uint32 flags);`

```c
int32 arkrtos_vfs_app_open(const char *path, uint32 flags);
```

**按路径打开一个文件或目录。**
- `path`：命名空间路径，绝对路径（"/mnt/nor/a.txt"、"/dev/uart0"、"/tmp"）
- `flags`：ARKRTOS_VFS_O_* 的组合
**返回**：0 以上是句柄；负值为 ark_vfs 错误码
> 说明：带 ARKRTOS_VFS_O_DIRECTORY 时打开的是目录（用于 readdir）。
目录只能只读打开。

### `int32 arkrtos_vfs_app_read(int32 handle, uint8 *buf, uint32 len);`

```c
int32 arkrtos_vfs_app_read(int32 handle, uint8 *buf, uint32 len);
```

**从当前偏移读。返回值 = 读到的字节数，0 = 文件尾，负值 = 错误 */**

### `int32 arkrtos_vfs_app_write(int32 handle, const uint8 *buf, uint32 len`

```c
int32 arkrtos_vfs_app_write(int32 handle, const uint8 *buf, uint32 len);
```

**从当前偏移写。返回值 = 写入的字节数，负值 = 错误 */**

### `int32 arkrtos_vfs_app_seek(int32 handle, int32 off, uint32 whence);`

```c
int32 arkrtos_vfs_app_seek(int32 handle, int32 off, uint32 whence);
```

**移动读写偏移。whence 用 ARKRTOS_VFS_SEEK_*。**
**返回**：新的绝对偏移（>= 0）；负值为错误码，且失败时偏移不动。
> 说明：返回的是内核自己记账的位置，不是下层文件系统的返回值——
ramfs 与 littlefs 桥对 seek 的返回约定不一样（前者回新偏移、
后者回 0），直接用下层的会让 lseek 在某个卷上静默回错值。
允许 seek 越过文件尾（POSIX 语义）；能不能写在那里由下层决定。
越界或非法的移动由下层拒绝，此时位置保持不变。

### `int32 arkrtos_vfs_app_readdir(int32 handle, char *name, uint32 cap, ui`

```c
int32 arkrtos_vfs_app_readdir(int32 handle, char *name, uint32 cap, uint32 *mode, uint32 *size);
```

**取目录里的下一条。**
- `name`：输出名字的缓冲区（不含路径，只一层）
- `mode`：收文件类型位（可 NULL）
- `size`：收文件大小（可 NULL）
**返回**：0 = 取到一条；1 = 目录已列完（不是错误，POSIX 的 readdir 在这里回 NULL）；
负值 = 错误

### `int32 arkrtos_vfs_app_close(int32 handle);`

```c
int32 arkrtos_vfs_app_close(int32 handle);
```

**归还句柄。目录句柄与文件句柄走同一个入口 */**

### `int32 arkrtos_vfs_app_stat(const char *path, uint32 *size, uint32 *mod`

```c
int32 arkrtos_vfs_app_stat(const char *path, uint32 *size, uint32 *mode);
```

**按路径取类型与大小（不打开）。mode 收 ARKRTOS_VFS_S_IF* */**

### `int32 arkrtos_vfs_app_unlink(const char *path);`

```c
int32 arkrtos_vfs_app_unlink(const char *path);
```

**删除一个文件（目录不可用这个入口删） */**

### `int32 arkrtos_vfs_app_mkdir(const char *path);`

```c
int32 arkrtos_vfs_app_mkdir(const char *path);
```

**在命名空间里建一个目录（只一层，父目录必须已存在）**
**返回**：0=成功；负值为 ark_vfs 错误码

### `int32 arkrtos_vfs_app_rmdir(const char *path);`

```c
int32 arkrtos_vfs_app_rmdir(const char *path);
```

**删除一个空目录（非空时报 ARK_E_NOTEMPTY）**
> 说明：不能删文件：那是 arkrtos_vfs_app_unlink 的事。

### `int32 arkrtos_vfs_app_rename(const char *old_path, const char *new_pat`

```c
int32 arkrtos_vfs_app_rename(const char *old_path, const char *new_path);
```

**重命名/移动（同一挂载点内）**
**返回**：跨挂载点的重命名会被拒绝（ARK_E_NOSYS），不会悄悄做成一次拷贝

### `int32 arkrtos_vfs_app_truncate(int32 handle, uint32 size);`

```c
int32 arkrtos_vfs_app_truncate(int32 handle, uint32 size);
```

**改一个已打开文件的长度（POSIX 的 ftruncate）。**
- `size`：新长度；变小丢尾部，变大用 0 填充
**返回**：0=成功；ARK_E_ISDIR / ARK_E_ROFS / ARK_E_NOSPC / ARK_E_NOSYS
或下层错误码
> 说明：只改长度，不删条目：长度 0 的文件还在。日志、环形缓冲这类
「用过就清空」的用法靠它，不必 unlink 再 create。
偏移不动（POSIX）：落在新长度之外不算错，下一次读就是文件尾。
写流打开着的文件不支持（那个驱动一边顺写一边改长度会产出
半真半假的状态），会报 ARK_E_NOSYS 而不是近似一个答案。

### `int32 arkrtos_vfs_app_truncate_path(const char *path, uint32 size);`

```c
int32 arkrtos_vfs_app_truncate_path(const char *path, uint32 size);
```

**按路径改长度（POSIX 的 truncate）。**
**返回**：0=成功；名字不存在时 ARK_E_NOENT（**绝不**顺手建一个空文件）；
目录 ARK_E_ISDIR；只读挂载 ARK_E_ROFS；文件系统不支持 ARK_E_NOSYS

### `void arkrtos_vfs_app_task_exit(uint32 task_id);`

```c
void arkrtos_vfs_app_task_exit(uint32 task_id);
```

**关掉 task_id 名下全部句柄。任务退出/崩溃路径调用。**
> 说明：必须在任务还被标记为有效时调用（表里靠任务号比对），且对没有
句柄的任务是无副作用的空操作。

## kernelsrc/port/arm/cortex-m3/arkrtos_port.c

**Ark RTOS Cortex-M3 架构适配层**
实现 arkrtos_hal.h 中声明的所有架构相关函数。
Cortex-M3 无FPU、MPU可选。
此文件仅依赖CMSIS核心头文件，不依赖任何厂商HAL库。

### `uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,`

```c
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value) {
```

**比较并交换（原子操作）**
- `p_addr`：目标地址（4 字节对齐）
- `expect`：期望值
- `new_value`：期望成立时写入的新值
**返回**：1=成功，0=当前值不是 expect（未修改）

### `uint32 arkrtos_port_cpu_id(void) {`

```c
uint32 arkrtos_port_cpu_id(void) {
```

**获取当前 CPU 编号**
**返回**：单核 MCU 固定返回 0，多核移植时返回硬件核号

### `void arkrtos_port_spin_hint(void) {`

```c
void arkrtos_port_spin_hint(void) {
```

**自旋等待提示**
> 说明：单核无总线争用，用 NOP 即可；多核可改为 __WFE() 降低功耗与争用。

### `uint32 arkrtos_port_enter_critical(void) {`

```c
uint32 arkrtos_port_enter_critical(void) {
```

**进入临界区，保存 PRIMASK 并关中断**
**返回**：进入前的中断状态

### `void arkrtos_port_exit_critical(uint32 state) {`

```c
void arkrtos_port_exit_critical(uint32 state) {
```

**退出临界区，恢复进入前的中断状态**

### `unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx) {`

```c
unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx) {
```

**读取系统调用参数（0~3，对应 R0~R3）**
Cortex-M 硬件压栈顺序为 R0,R1,R2,R3,R12,LR,PC,xPSR，
使用栈帧指针直接索引即可。

### `void arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value) {`

```c
void arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value) {
```

**写回系统调用返回值（R0）**

### `int32 arkrtos_port_cyccnt_init(void) {`

```c
int32 arkrtos_port_cyccnt_init(void) {
```

**使能 DWT 周期计数器（不清零：它是自由运行计数器，**
清零会让另一处正在计时的时间戳突然对不上）
**返回**：0 成功

### `uint32 arkrtos_port_get_cyccnt(void) {`

```c
uint32 arkrtos_port_get_cyccnt(void) {
```

**读 DWT 周期计数器 (自然回绕)**
**返回**：当前周期计数

## kernelsrc/port/arm/cortex-m4/arkrtos_port.c

**Ark RTOS Cortex-M4 移植层实现**
实现 arkrtos_hal.h 中声明的所有硬件抽象接口，包括：
CPU 控制、中断开关、寄存器访问、任务栈帧初始化、
系统时钟与微秒延时、MPU 配置等。

### `uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,`

```c
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value) {
```

**比较并交换（原子操作）**
- `p_addr`：目标地址（4 字节对齐）
- `expect`：期望值
- `new_value`：期望成立时写入的新值
**返回**：1=成功，0=当前值不是 expect（未修改）

### `uint32 arkrtos_port_cpu_id(void) {`

```c
uint32 arkrtos_port_cpu_id(void) {
```

**获取当前 CPU 编号**
**返回**：单核 MCU 固定返回 0，多核移植时返回硬件核号

### `void arkrtos_port_spin_hint(void) {`

```c
void arkrtos_port_spin_hint(void) {
```

**自旋等待提示**
> 说明：单核无总线争用，用 NOP 即可；多核可改为 __WFE() 降低功耗与争用。

### `uint32 arkrtos_port_enter_critical(void) {`

```c
uint32 arkrtos_port_enter_critical(void) {
```

**进入临界区，保存 PRIMASK 并关中断**
**返回**：进入前的中断状态

### `void arkrtos_port_exit_critical(uint32 state) {`

```c
void arkrtos_port_exit_critical(uint32 state) {
```

**退出临界区，恢复进入前的中断状态**

### `unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx) {`

```c
unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx) {
```

**读取系统调用参数（0~3，对应 R0~R3）**
Cortex-M 硬件压栈顺序为 R0,R1,R2,R3,R12,LR,PC,xPSR，
使用栈帧指针直接索引即可。

### `void arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value) {`

```c
void arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value) {
```

**写回系统调用返回值（R0）**

### `int32 arkrtos_port_cyccnt_init(void) {`

```c
int32 arkrtos_port_cyccnt_init(void) {
```

**使能 DWT 周期计数器（不清零：它是自由运行计数器，**
清零会让另一处正在计时的时间戳突然对不上）
**返回**：0 成功

### `uint32 arkrtos_port_get_cyccnt(void) {`

```c
uint32 arkrtos_port_get_cyccnt(void) {
```

**读 DWT 周期计数器 (自然回绕)**
**返回**：当前周期计数

## kernelsrc/port/arm/cortex-m7/arkrtos_port.c

**Ark RTOS Cortex-M7 移植层实现**
实现 arkrtos_hal.h 中声明的所有硬件抽象接口，包括：
CPU 控制、中断开关、寄存器访问、任务栈帧初始化、
系统时钟与微秒延时、MPU 配置等。

### `uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,`

```c
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value) {
```

**比较并交换（原子操作）**
- `p_addr`：目标地址（4 字节对齐）
- `expect`：期望值
- `new_value`：期望成立时写入的新值
**返回**：1=成功，0=当前值不是 expect（未修改）

### `uint32 arkrtos_port_cpu_id(void) {`

```c
uint32 arkrtos_port_cpu_id(void) {
```

**获取当前 CPU 编号**
**返回**：单核 MCU 固定返回 0，多核移植时返回硬件核号

### `void arkrtos_port_spin_hint(void) {`

```c
void arkrtos_port_spin_hint(void) {
```

**自旋等待提示**
> 说明：单核无总线争用，用 NOP 即可；多核可改为 __WFE() 降低功耗与争用。

### `uint32 arkrtos_port_enter_critical(void) {`

```c
uint32 arkrtos_port_enter_critical(void) {
```

**进入临界区，保存 PRIMASK 并关中断**
**返回**：进入前的中断状态

### `void arkrtos_port_exit_critical(uint32 state) {`

```c
void arkrtos_port_exit_critical(uint32 state) {
```

**退出临界区，恢复进入前的中断状态**

### `unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx) {`

```c
unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx) {
```

**读取系统调用参数（0~3，对应 R0~R3）**
Cortex-M 硬件压栈顺序为 R0,R1,R2,R3,R12,LR,PC,xPSR，
使用栈帧指针直接索引即可。

### `void arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value) {`

```c
void arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value) {
```

**写回系统调用返回值（R0）**

### `int32 arkrtos_port_cyccnt_init(void) {`

```c
int32 arkrtos_port_cyccnt_init(void) {
```

**使能 DWT 周期计数器（不清零：它是自由运行计数器，**
清零会让另一处正在计时的时间戳突然对不上）
**返回**：0 成功

### `uint32 arkrtos_port_get_cyccnt(void) {`

```c
uint32 arkrtos_port_get_cyccnt(void) {
```

**读 DWT 周期计数器 (自然回绕)**
**返回**：当前周期计数

## kernelsrc/port/loongarch/la32/arkrtos_port.c

**Ark RTOS LoongArch32 移植层实现**
实现 arkrtos_hal.h 声明的全部硬件抽象接口。与 la64 版逐函数对应，
必须不同的只有两处：**帧槽 4 字节（帧 128 字节）**、以及
**`rdtime` 在 LA32 上按寄存器对返回**。

### `static uint32 la_rdtime(void) {`

```c
static uint32 la_rdtime(void) {
```

**读稳定计数器低 32 位**
LA32 的 `rdtime.d rd, rj` 是**寄存器对**形式：低 32 位写 rd、
高 32 位写 rj。这里只取低半部（HAL 的计数器就是 32 位），高半部
写进一个临时寄存器后丢弃。
> 说明：不能用 `rdtime.w`：本工具链的 binutils 在 la32 上不认这条助记符
（实测 `rdtime.w` / `rdtimeh.w` 均报 "no match insn"），而 `.d` 形式
会被接受——这也是本文件里唯一一处看起来像 64 位、实际合法的指令。

### `static uint32 la_us_to_counts(uint32 us) {`

```c
static uint32 la_us_to_counts(uint32 us) {
```

**微秒 -> 定时器计数**
> 说明：定时器时钟是 25 MHz 的整数倍关系，按「先约分再相乘」拆开避免
64 位乘除；与 RISC-V 版同一写法（那边是因为工具链没有对应 libgcc）。

### `uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,`

```c
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value) {
```

**比较并交换（LL/SC 实现）**
LoongArch 的 `ll.w` / `sc.w` 是真原子指令。单核下关中断也算
正确，但这套写法在 SMP 上同样成立，将来换多核不用重写。
> 说明：用 `.w` 而不是 `.d` 与内核的 uint32 宽度严格对应：内核的原子量
是 32 位，用 64 位原子会读写到相邻字段。

### `uint32 arkrtos_port_syscall_num(void *p_exc_ctx) {`

```c
uint32 arkrtos_port_syscall_num(void *p_exc_ctx) {
```

**从异常帧里取系统调用号**
LoongArch 的 `syscall code` 把号码编在指令里（15 位），所以这里
从「指令字」解出来，而不是从某个寄存器槽读。
**注意**: la_trap_c() 在调 SVC_Server() 之前已经把帧里的 ERA 加了 4
（syscall 指令占 4 字节），所以此刻 ERA 指向下一条指令，被调用的
syscall 在 ERA - 4 处。两条路径（谁先谁后）在文件头注释里写死，
改动时两处必须同时改。
解出的指令若不是 syscall 形式，返回 0xFFFFFFFF：调用方会落到
default 分支拒绝这次服务，而不是拿一个看似合法的号去执行。

### `uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx) {`

```c
uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx) {
```

**LA32 没有 Linux ABI 的载体**
Linux/LoongArch 只有 64 位；LA32 上不存在可供兼容的 Linux 用户
态 ABI，因此返回「不是 Linux 调用」，由 Ark 自己的号表继续处理。
这里不猜一个号，也不假装支持。

### `uint32 arkrtos_port_enter_critical(void) {`

```c
uint32 arkrtos_port_enter_critical(void) {
```

**进入临界区，返回进入前的 CRMD**
与 RISC-V 版「一次读改写」同一口径：先 csrrd 读旧值，再
csrxchg 清 IE。csrxchg 的 rd 同时是新值来源，不能一次搞定两者，
所以这里是两条指令 —— 它们之间若来中断也不改变语义：临界区
的调用方随后会发现自己拿到的 state 里 IE=1，退出时会重新打开。

### `void arkrtos_port_switch_task(void) {`

```c
void arkrtos_port_switch_task(void) {
```

**请求一次任务切换**
两种落点：
- 在异常处理程序里调用：只置 s_switch_pending，由 la_trap_c()
的出口结算（等价于 RISC-V 那次软中断，但不需要真的再进一次
trap）——绝不能在 trap 里就地 ertn 换栈，那会把异常处理程序
自己丢掉；
- 在任务上下文里调用：交给汇编的就地切换路径，构造同格式帧后
ertn（见 arkrtos_context.S）。
门闩未打开时两种落点都退化为「什么都不做」。

### `unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*`

```c
unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*entry)(void)) {
```

**构造任务初始 trap 帧**
- `stack_top`：栈顶（高地址）
- `entry`：任务入口
**返回**：帧指针（存入 task->stack_ptr）
帧格式与 arkrtos_context.S 完全一致。任务第一次被恢复时会走
「eentry 的恢复半段」：ERA←entry、PRMD←LA_PRMD_INIT（PIE=1、
PPLV=0，值为 0x4）、
sp←stack_top，然后 ertn 跳进任务，此时 CRMD.IE 已经打开。
ra 置 0：任务函数返回就等于跳到 0 取指，会立刻进致命异常，
与 Cortex-M 版（LR=0）同一处理口径。

### `void arkrtos_port_start_timer(uint32 tick_period_us) {`

```c
void arkrtos_port_start_timer(uint32 tick_period_us) {
```

**启动节拍定时器**
- `tick_period_us`：节拍周期（微秒）
TCFG 是自动重装的周期定时器（Periodic 位），硬件负这个责，
异常处理里只需要清 TICLR 并调内核节拍处理。
定时器中断的使能位（ECFG.LIE[11]）也在这里才放行 —— 与 RISC-V
版同一顺序：irq_init() 只开全局中断，节拍在调度数据结构就绪后
才允许进来。

### `la_frame_word *la_switch_inline(la_frame_word *frame) {`

```c
la_frame_word *la_switch_inline(la_frame_word *frame) {
```

**就地切换的 C 半段（由 arkrtos_context.S 的立即切换路径调用）**
- `frame`：刚构造好的当前帧
**返回**：要恢复的帧；不需要切换时原样返回

### `la_frame_word *la_trap_c(la_frame_word *frame) {`

```c
la_frame_word *la_trap_c(la_frame_word *frame) {
```

**异常帧的统一 C 入口**
- `frame`：当前上下文保存的帧指针（eentry 里由 s0 传入）
**返回**：要恢复的帧指针：发生任务切换时换成另一个任务的帧，否则原样返回
三条路径：
- 中断（Ecode = 0）：本移植只用定时器（IS bit11），清 TICLR 后
交给内核节拍处理；
- syscall（Ecode = 0xB = SYS）：ERA += 4 后进 SVC_Server；
- 其余 Ecode 一律按致命异常处理（报 Ecode/ESTAT/ERA 后停机，
不做假恢复）。
出口处结算 s_switch_pending：这是「没有软中断」的补偿点。

### `ARKRTOS_WEAK void arkrtos_port_irq_init(void) {`

```c
ARKRTOS_WEAK void arkrtos_port_irq_init(void) {
```

**安装异常入口并打开全局中断**
三件事：
1) EENTRY = eentry（异常入口只有这一个，Ecode 全在 ESTAT 里）；
2) 清 TICLR，避免复位值留下挂起位；
3) CRMD.IE = 1 —— 相当于 Cortex-M 复位后 PRIMASK 为 0。
定时器中断使能（ECFG.LIE[11]）**不**在这里开：那时内核的调度
数据结构还没初始化，节拍在 trap 里改调度状态是不安全的。

## kernelsrc/port/loongarch/la64/arkrtos_port.c

**Ark RTOS LoongArch64 移植层实现**
实现 arkrtos_hal.h 声明的全部硬件抽象接口。目标平台是 QEMU 的
`virt` 机器（单核、PLV0、无页表），但本文件里只有 CSR 编号、
定时器与 UART 的常量是平台相关的，其余都是 LoongArch 架构本身

### `static uint32 la_us_to_counts(uint32 us) {`

```c
static uint32 la_us_to_counts(uint32 us) {
```

**微秒 -> 定时器计数**
> 说明：定时器时钟是 25 MHz 的整数倍关系，按「先约分再相乘」拆开避免
64 位乘除；与 RISC-V 版同一写法（那边是因为工具链没有对应 libgcc）。

### `uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,`

```c
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value) {
```

**比较并交换（LL/SC 实现）**
LoongArch 的 `ll.w` / `sc.w` 是真原子指令。单核下关中断也算
正确，但这套写法在 SMP 上同样成立，将来换多核不用重写。
> 说明：用 `.w` 而不是 `.d` 与内核的 uint32 宽度严格对应：内核的原子量
是 32 位，用 64 位原子会读写到相邻字段。

### `static void la_tlb_fill(unsigned long vaddr, uint32 ps, uint32 index, `

```c
static void la_tlb_fill(unsigned long vaddr, uint32 ps, uint32 index, unsigned long lo0, unsigned long lo1) {
```

**填一条 TLB 条目**
- `vaddr`：虚拟地址基址（必须按 2^(ps+1) 对齐）
- `ps`：页大小指数（12 = 4 KB，22 = 4 MB）
- `lo0`：偶页 TLBELO（映射 vaddr）
- `lo1`：奇页 TLBELO（映射 vaddr + 2^ps）
走 `tlbfill` 而不是自己算索引：QEMU 的 tlbfill 按 PS 自动选表 ——
PS == STLBPS(12) 进 STLB（8 路 × 256 组），PS != 12 进 MTLB
（绝对索引 2048..2111）。而**查找**时 STLB 的每一路都按
stlb_ps(12) 去比 VPPN，所以大页条目一旦被塞进 STLB 就永远匹配
不上（症状是「地址明明有映射却一直报重填」）。tlbfill 替你
选了正确的表，这是用它而不是手工写索引的唯一理由。

### `static void la_tlb_write(unsigned long vaddr, uint32 ps, uint32 index,`

```c
static void la_tlb_write(unsigned long vaddr, uint32 ps, uint32 index, unsigned long lo0, unsigned long lo1) {
```

**确定性写一条 TLB 条目（不经过 tlbfill 的随机选路）**
- `vaddr`：虚拟地址基址
- `ps`：页大小指数
- `index`：绝对条目号（STLB 用 way*256+组，MTLB 用 2048 起）
`tlbfill` 在 PS == STLBPS 时会替调用方随机挑一路，好处是不用
知道 STLB 的索引布局，坏处是运行期反复调用会**随机顶掉同组里
已有的条目**（内核 UART 与动态页恰好同组时就踩到过：登记完一条
动态映射，紧接着控制台第一次打印就 KERNEL refill）。
运行期登记不能用那种「碰运气」的写法，所以这里直接 `tlbwr`，
索引由调用方算好。
4 KB 页刻意写进 **MTLB**（2048 起的绝对条目）而不是 STLB：
MTLB 是确定索引、查找时按条目自己的 PS 比 VPPN，不会和内核那几条
静态 4 KB 条目抢同一组的 8 路，也不需要知道 STLB 的 way 布局。
代价是 MTLB 只有 64 条，所以动态表的上限由它决定（见 LA_PG_DYN_PAIRS）。

### `static unsigned long la_asid_get(void) {`

```c
static unsigned long la_asid_get(void) {
```

**读 CSR.ASID（10 位，本 port 只用到 1..ARKRTOS_PAGING_APPS_MAX）**

### `static unsigned long la_asid_swap(unsigned long asid) {`

```c
static unsigned long la_asid_swap(unsigned long asid) {
```

**换 CSR.ASID，返回换之前的值**
- `asid`：目标 ASID（只取低 10 位）
`csrwr` 把旧值写回同一个寄存器，所以一次指令就够 —— 切换路径上
每一次多余的 CSR 访问都是白花钱。

### `static void la_tlb_fill_refill(unsigned long vaddr, unsigned long lo0,`

```c
static void la_tlb_fill_refill(unsigned long vaddr, unsigned long lo0, unsigned long lo1) {
```

**在 TLB 重填上下文里装一条 4 KB 对（只能在 refill 入口里用）**
- `vaddr`：页对基址（8 KB 对齐）
- `lo0`：偶页 TLBELO、lo1 奇页 TLBELO
**为什么不能在这里用 la_tlb_write()**：重填异常里 TLBRERA.ISTLBR=1，
此刻 `tlbwr` 与 `tlbfill` 取的都不是 TLBEHI/TLBELO0/TLBELO1，而是
TLBR* 那一组镜像 CSR。实测（QEMU 9.2.3）：重填处理里写 TLBEHI 再
`tlbwr`，条目按硬件填的 TLBREHI（= 出错地址的 VPPN）落进去、
TLBELO 取 TLBRELO0/1 的旧值 ⇒ V=0，于是**下一次访问变成
TLBRET_INVALID 走 eentry 的 PIS**（不是重填，因为 tlbfill 只对
"查不到"置 ISTLBR），直接撞上「PLV0 致命异常」停机。
正确形态就是硬件重填处理惯用的那一套：写 TLBREHI/TLBRELO0/
TLBRELO1 再 `tlbfill`。TLBREHI 的 PS 字段在 [5:0]，VPPN 从 bit13
起 ⇒ 页基址直接或上 12 就是完整值。
索引由 tlbfill 按 VA 自己算（PS==STLBPS 时进 STLB，way 0..7 由
硬件随机挑）：窗口里每个 8 KB 对占一个 STLB 组、一组 8 路只用 1
路，所以随便落哪一路都不会与别的对撞。

### `#if ((ARKRTOS_USE_PAGING_PAGES == 1) && (ARKRTOS_USE_PRIV == 1) && \ (`

```c
#if ((ARKRTOS_USE_PAGING_PAGES == 1) && (ARKRTOS_USE_PRIV == 1) && \ (ARKRTOS_USE_PAGING == 1))
```

**建立静态映射并打开分页**
必须在开中断之前调用（节拍一进来就要过 TLB），也必须在
所有任务注册之后调用不需要 —— 帧内容与映射方式无关，
帧所在的地址在 DA 模式下写和分页模式下写是同一块内存。

### `static void la_frame_apply_priv(la_frame_word *frame) {`

```c
static void la_frame_apply_priv(la_frame_word *frame) {
```

**把「下一个任务的特权级」写进要恢复的帧**
- `frame`：即将被 ertn 恢复的帧
只改 PRMD.PPLV（低 2 位），PIE/PWE 原样保留：ertn 按
PRMD.PPLV 写 CRMD.PLV、按 PRMD.PIE 写 CRMD.IE，所以「回到
PLV3 且开中断」= PPLV=3 + PIE=1 = 0x7。

### `static uint32 la_pg_app_frame(uint32 space) {`

```c
static uint32 la_pg_app_frame(uint32 space) {
```

空间号 -> 物理帧基址（2 MB 对齐，满足 PS=20 条目要求） */

### `static void la_tlb_inval_page(unsigned long va) {`

```c
static void la_tlb_inval_page(unsigned long va) {
```

**让某个 VA 的翻译失效（含 QEMU 的地址翻译缓存）**
`invtlb` op=3：按 VA 清掉命中的 TLB 条目，并整表 flush 掉 QEMU
的 softmmu 缓存。（旧注释写「按 (ASID, VA)」是错的：pmap
asidinval 在 QEMU 上量出来的是「不看 ASID」，详见下面的 op 表。）
为什么必须显式做：QEMU 的 tlbwr 只清它覆盖到的地址范围，而它算
**奇页**地址的那句写错成 `addr & pagesize`（应为 `| pagesize`），
于是 PS=20 条目的上半 1 MB 永远留在 softmmu 缓存里 —— 重写条目
后经那半页读写仍是旧物理帧，症状正是「切换静默失效」。
顺序要紧：先 invtlb 再写新条目（invtlb 会按旧条目的 PS 匹配）。
U-7b 实测（QEMU 9.2.3，pmap invop 逐 op 取证，别再试一遍）：
op 0 / 1 / 2 / 6 ：一次调用把**整张表**打掉（含内核恒等映射），
紧接着取指就 KERNEL refill fatal。op 6 不是
「按 (ASID, VA) 清一条」，别指望它。
op 4 / 5          ：活下来，窗口条目一个都没掉 —— 在本载体上
等价于空操作。
op 7              ：非法指令（estat ecode=0xd）。
op 3              ：按 VA 失效，只清这个 VA，内核不受影响 ——
这条是本 port 唯一敢用的失效手段。
于是「只清某个 ASID 的条目」在本载体上**做不到**：想只撤一个空间的
窗口，只能先切到它的 ASID 再按 VA 撤（撤完把当前空间补铺回来），
这就是 activate / space_release 现在走的那条路。 */

### `static int32 la_pg_space_ensure(uint32 space) {`

```c
static int32 la_pg_space_ensure(uint32 space) {
```

给空间从帧池里拿一整块 2 MB；已经拿过就什么都不做 */

### `static void la_pg_install_space(uint32 space) {`

```c
static void la_pg_install_space(uint32 space) {
```

把该空间的 512 页铺进窗口（256 条 PS=12 条目，占该空间自己那一路） */

### `static void la_pg_drop_window(void) {`

```c
static void la_pg_drop_window(void) {
```

把整段窗口 VA 的旧翻译清掉（逐 4 KB：QEMU 按 (ASID, VA) 匹配条目） */

### `static void la_pg_copy_pair(uint32 *dst, const uint32 *src) {`

```c
static void la_pg_copy_pair(uint32 *dst, const uint32 *src) {
```

8 KB 拷贝：重填上下文里没有 libc，也不能取锁/阻塞，只能是裸循环 */

### `static int32 la_pg_swap_slot_alloc(uint32 *p_slot) {`

```c
static int32 la_pg_swap_slot_alloc(uint32 *p_slot) {
```

找一个空交换槽并占住；没有返回 -1 */

### `static void la_pg_swap_slot_free(uint32 slot) {`

```c
static void la_pg_swap_slot_free(uint32 slot) {
```

放掉一个交换槽 */

### `static uint32 la_pg_swap_slots_used(void) {`

```c
static uint32 la_pg_swap_slots_used(void) {
```

占用的槽数（读数用） */

### `static uint32 la_pg_pair_resident_count(void) {`

```c
static uint32 la_pg_pair_resident_count(void) {
```

驻留（有帧）的对数（读数用） */

### `static int32 la_pg_swap_out_one(uint32 skip_space, uint32 skip_idx) {`

```c
static int32 la_pg_swap_out_one(uint32 skip_space, uint32 skip_idx) {
```

**换出一对：内容抄进交换槽、两帧还回池子**
- `skip_space`：正在缺页的空间（第一轮避开它）
- `skip_idx`：该空间里绝不换出的对号（正在换入的那一对）
**返回**：0 = 换出了；-1 = 没有驻留的对可换；-2 = 交换槽满；-3 = 帧池拒绝
两轮挑选：第一轮只挑**别的空间**的对 —— 它们此刻一条条目都没装着，
换掉不产生额外缺页；本空间的对可能正被这条取指路径用着，第二轮
才轮到它们（仍然避开 skip_idx）。轮转指针跨调用保持，免得每次都
盯着同一对换（那会退化成「只有一对在挪」）。

### `static int32 la_pg_take_pair(uint32 space, uint32 idx, uint32 *p_pa) {`

```c
static int32 la_pg_take_pair(uint32 space, uint32 idx, uint32 *p_pa) {
```

**给一对 8 KB 要到两帧：池里有就直接拿，没有就先换出一对**
- `space`：正在缺页的空间
- `idx`：正在缺页的对号（绝不换出它自己）
- `p_pa`：成功时写回首帧物理基址
**返回**：0 = 拿到；-1 = 池空且换不出东西
换出一对还回的是两帧连续帧，所以正常一次就够；留 8 次循环是给
「换出成功但那两帧拼不出一次两帧分配」的碎片情形当余量。

### `uint32 arkrtos_port_paging_swap_stat(uint32 which) {`

```c
uint32 arkrtos_port_paging_swap_stat(uint32 which) {
```

**页替换读数（U-16）**
- `which`：0 = 换出次数、1 = 换回次数、2 = 占用槽数、3 = 失败次数、
4 = 驻留对数、5 = 槽总数
**返回**：对应的读数；which 越界返回 0

### `uint32 arkrtos_port_paging_pair_total(void) {`

```c
uint32 arkrtos_port_paging_pair_total(void) {
```

**窗口由多少对 8 KB 组成（细粒度读数）**
一条 PS=12 条目管相邻两页，所以窗口的记账单位是「对」而不是
「页」；对每个空间是同一个数（VA 侧不动）。没有页级窗口的档
如实回 0，而不是把「一对 4 KB」这种不存在的粒度编出来。

### `uint32 arkrtos_port_paging_pair_count(uint32 space) {`

```c
uint32 arkrtos_port_paging_pair_count(uint32 space) {
```

**某个空间现在挂着几对 8 KB**
**返回**：对数；空间号越界（或这一档没有页级窗口）返回 0
按需档数的是**真的付过费**（拿了两帧并装了条目）的对；页级档
下装过的空间是整块 512 帧铺满窗口，所以等于总对数。

### `uint32 arkrtos_port_paging_pair_pa(uint32 space, uint32 idx) {`

```c
uint32 arkrtos_port_paging_pair_pa(uint32 space, uint32 idx) {
```

**某一对的物理基址；0 = 这一对没挂着**
- `space`：空间号
- `idx`：对号（0 .. pair_total()-1）
只读：按需档下它不会替调用方把这一对缺进来 —— 问「现在挂着
什么」不能改变答案。没挂的对如实回 0，不编一个地址。

### `static void la_fork_copy_pair(uint32 *dst, const uint32 *src) {`

```c
static void la_fork_copy_pair(uint32 *dst, const uint32 *src) {
```

8 KB 对拷贝：fork 档不开 swap 也要能搬，因此单独一份，不借用
la_pg_copy_pair()（那个只在 swap 档定义）。 */

### `int32 arkrtos_port_paging_space_clone(uint32 src, uint32 dst) {`

```c
int32 arkrtos_port_paging_space_clone(uint32 src, uint32 dst) {
```

**把 src 空间的整份窗口内容复制到 dst 空间**
**返回**：0 = 已复制；-1 = 参数非法；-2 = dst 已经有帧（不覆写别人）；
-3 = 帧池拒绝，本次已回滚
只复制**父空间真正挂着**的对（按需档下父也可能只摸了一部分）。
父的对若在交换区里（swap 档），子拿到的是该内容的**驻留副本**：
fork 的语义是「子看到与父此刻相同的内容」，父的换出状态不是子
该继承的东西。失败一律回滚成 dst 空着，绝不留半份地址空间。

### `int32 arkrtos_port_paging_write_space(uint32 space, unsigned long va, `

```c
int32 arkrtos_port_paging_write_space(uint32 space, unsigned long va, const unsigned long *words, uint32 n) {
```

**按 PA 往某个空间里写一小段字（不切窗口）**
- `space`：目标空间号
- `va`：窗口内的虚拟地址（调用方保证 8 字节对齐）
- `words`：源字数组（8 字节 / 字）
- `n`：字数
**返回**：0 = 已写；-1 = 参数越界，或目标 VA 所在的那一对没有帧
这一档**不做按需缺页**：调用方要写的地址必须已经驻留，否则
回 -1 让它回滚。在内核路径里临时拿帧会绕开「谁欠谁」的账本，
而且 fork 的初始帧本来就落在父任务已经摸过的栈页里。

### `int32 arkrtos_port_paging_clone_frame(void *p_exc_ctx, uint32 space) {`

```c
int32 arkrtos_port_paging_clone_frame(void *p_exc_ctx, uint32 space) {
```

**把父任务的 trap 帧复制成子任务的初始帧（同一 VA）**
- `p_exc_ctx`：父任务这次系统调用的 trap 帧（同时是它在子空间里的 VA）
- `space`：子空间
**返回**：0 = 已写；-1/-2 透出 write_space / 越界的原因
只改一格：a0。Linux 的 clone 约定是「父返回 pid、子返回 0」，
而子恢复现场时唯一不同的就是返回值寄存器。ERA/sp/tp 与所有
通用/浮点/向量寄存器原样继承 —— 包括 fp，于是子的调用栈与父
逐字相同（这正是 fork 要的）。

### `static int32 la_pg_demand_fault(unsigned long badv) {`

```c
static int32 la_pg_demand_fault(unsigned long badv) {
```

**按需缺页：给 badv 所在的那一对 8 KB 拿帧并装条目**
- `badv`：触发 refill 的地址（CSR.TLBRBADV）
**返回**：1 = 条目已装好，调用方应 ertn 回到出错的那条指令重试；
0 = 不归这一档管（不在窗口里 / 帧池空了），交给原来的故障路径
粒度是**一对**而不是一页：一条 PS=12 条目同时给出两页，所以
「装半条」根本表达不出来 —— 要么两帧一起拿，要么一条都不装。
这一档刻意不区分 PLV：内核经窗口拷贝（ELF 装载、pmap 自测）
与 App 自己访问走的是同一条路，否则装载器第一笔写就死在
「KERNEL refill -> fatal」上。
没有帧就返回 0，让原来的路径去报错/杀任务 —— 这里绝不假装
装好了（装一条 V=0 的条目会让 ertn 回到同一条指令上无限重填）。

### `static void la_pg_boot_map(void) {`

```c
static void la_pg_boot_map(void) {
```

开分页之前的首次铺窗口：只铺 space 0（其它空间等 activate 时再要帧） */

### `static void la_pg_assert_window(void) {`

```c
static void la_pg_assert_window(void) {
```

**把 App 窗口那一条 PS=20 条目按当前空间重写一遍**
映射内容不变，只是重新落一条有效条目。存在的理由是实测出来的：
QEMU 的 `invtlb 3`（按 VA 失效）**会连带打掉这条 PS=20 条目**
—— pmap test 末尾撤销动态映射之后再 pmap look 0x02000000 报
no entry，紧接着任何一次窗口访问都是 KERNEL refill (plv0)。
真机上按 VA 失效不该波及其他页，但这条修复在真机上也只是一次
tlbwr，代价可忽略，换来的是「invtlb 之后窗口一定还在」。

### `static uint32 la_pg_pair_expect(uint32 space, uint32 idx) {`

```c
static uint32 la_pg_pair_expect(uint32 space, uint32 idx) {
```

这一对 8 KB 在这个空间里「应该」翻译到哪；0 = 这一档说不出该是什么 */

### `static int32 la_pg_pair_check(uint32 space, uint32 idx) {`

```c
static int32 la_pg_pair_check(uint32 space, uint32 idx) {
```

读回校验一对 8 KB：期望值为 0（这一档还没付费）时视为通过 */

### `static uint32 la_sp_in_app_window(void) {`

```c
static uint32 la_sp_in_app_window(void) {
```

调用者此刻的 sp 是否落在 App 窗口里 */

### `int32 arkrtos_port_paging_switch_to(uint32 space) {`

```c
int32 arkrtos_port_paging_switch_to(uint32 space) {
```

**调度器换窗口：调用者自己站在窗口里时必须推迟（U-2）**
详见 arkrtos_hal.h 的声明。判据就是「我此刻的 sp 在不在窗口里」，
它精确对应危险条件：恢复现场要用的那一帧（以及本函数的返回地址）
正压在同一个窗口 VA 上。推迟不等于不换 —— 换的时机交给
arkrtos_context.S：那里已经不再碰被换下任务的栈了。

### `void la_switch_prepare(la_frame_word *new_frame, la_frame_word *old_fr`

```c
void la_switch_prepare(la_frame_word *new_frame, la_frame_word *old_frame) {
```

**恢复现场之前补做被推迟的窗口切换（由 arkrtos_context.S 调用）**
- `new_frame`：即将恢复的帧
- `old_frame`：被换下任务的帧（同一个 = 调度器最后没切走）
本函数由汇编入口在**一块内核静态栈**上调用：进入时 sp 可能还指向
App 窗口里的栈，而它自己一落地就要开始写栈 —— 所以汇编侧先把 sp
换到 la_switch_stack，再进来。返回之后汇编侧不再碰任何栈，只按
$s0 指向的目标帧恢复寄存器。
PRMD 也要在这里补：C 侧的 LA_PRIV_APPLY 在「窗口还没换」的时候
执行，它写进去的是旧窗口里的那块内存，不是目标帧。

### `int32 arkrtos_port_paging_space_release(uint32 space) {`

```c
int32 arkrtos_port_paging_space_release(uint32 space) {
```

**把一个地址空间的物理帧还给帧池**
- `space`：空间号
**返回**：0 = 已归还（本来就没拿过也算 0，幂等）；-1 = 空间号越界；
-2 = 这一档没有帧池可还（PS=20 静态窗口）；-3 = 调用者正站在
这个窗口里（调用者自己的栈就在这块窗口里），此时一个字节都不动；-4 = 帧池拒绝
（它的 -1/-3 原样透出，账本保持原样）
归还之前先把窗口里属于这个空间的东西撤掉，理由只有一条但足够：
帧还回池子之后会被别的空间拿走，而旧条目仍然指着它 —— 两个空间
共享同一块物理页，没有任何报错，只在很久以后显形。
撤法按档走：页级窗口整段逐页 invtlb（QEMU 的 softmmu 缓存按 VA
记翻译）；按需档没有「整块帧」，逐对 free 并清账本。
s_pg_app_space 保持原值：这个空间仍然存在，只是空着；下一次
activate() 会重新要帧（ensure 看到基址 0 就再要一块）。这与
「sid 不存在」是两件事，压成一个状态会让 activate 的短路判据骗人。

### `void la_switch_prepare(la_frame_word *new_frame, la_frame_word *old_fr`

```c
void la_switch_prepare(la_frame_word *new_frame, la_frame_word *old_frame) {
```

没有窗口可换：恢复现场前没有要补做的事 */

### `uint32 arkrtos_port_paging_space(void) {`

```c
uint32 arkrtos_port_paging_space(void) {
```

当前装进 App 窗口的空间号 */

### `uint32 arkrtos_port_paging_space_frame(uint32 space) {`

```c
uint32 arkrtos_port_paging_space_frame(uint32 space) {
```

空间号对应的物理帧基址；越界返回 0（不猜一个地址出去） */

### `uint32 arkrtos_port_paging_space_count(void) {`

```c
uint32 arkrtos_port_paging_space_count(void) {
```

板级给了几个地址空间（>= 1） */

### `unsigned long arkrtos_port_paging_window_va(void) {`

```c
unsigned long arkrtos_port_paging_window_va(void) {
```

App 窗口的 VA 基址：各空间共用同一个 VA，物理侧才按空间换 */

### `unsigned long arkrtos_port_paging_asid_current(void) {`

```c
unsigned long arkrtos_port_paging_asid_current(void) {
```

**读当前硬件的 CSR.ASID（U-7）**
**返回**：0..1023
只读，不改状态。地址空间切换之后用它回报「现在跑在谁的视图里」，
这是 ASID 档下判断「切换到底做了没有」的唯一直接读数。

### `uint32 arkrtos_port_paging_asid_of(uint32 space) {`

```c
uint32 arkrtos_port_paging_asid_of(uint32 space) {
```

**空间号对应的 ASID（空间 s -> ASID s+1，0 留给内核默认视图）**
- `space`：0 .. arkrtos_port_paging_space_count()-1
**返回**：ASID 值；空间号越界时返回 0（不猜一个别空间的号）

### `uint32 arkrtos_port_paging_window_way(uint32 space) {`

```c
uint32 arkrtos_port_paging_window_way(uint32 space) {
```

**空间的窗口条目落在哪一条 STLB way 上**
- `space`：0 .. arkrtos_port_paging_space_count()-1
**返回**：way 号；越界返回 0
ASID 档下每个空间占一条独立 way，所以「两个空间的窗口条目并存」
这件事在硬件上是看得见的（不同的 way）；关档时全部挤在 way 0，
切换必须重写条目。

### `unsigned long arkrtos_port_paging_asid_raw_swap(unsigned long asid) {`

```c
unsigned long arkrtos_port_paging_asid_raw_swap(unsigned long asid) {
```

**只写一次 CSR.ASID 并返回换之前的值（诊断用，U-7b）**
- `asid`：目标 ASID（只取低 10 位）
**返回**：换之前的 CSR.ASID
与 activate() 的差别只有一条但很要紧：activate 会读回校验、缺
什么补什么，所以拿它测「条目还在不在」测不出来 —— 它会当场把
缺的补上。这个函数只做一次 csrwr：不记账、不补铺、不改
s_pg_app_space，调用方负责换回去。生产路径不该用它。

### `void arkrtos_port_paging_inval_va(unsigned long va) {`

```c
void arkrtos_port_paging_inval_va(unsigned long va) {
```

**按 VA 让翻译失效：`invtlb` op=3，不看 ASID、global 条目不动**
- `va`：要失效的虚拟地址
这就是 U-7 里那个「会连带打掉别的空间同 VA 条目」的操作。留着
它不是给生产路径用的，而是给 pmap asidinval 做对照 —— 「op=6 是
定向的」这句话只有跟 op=3 的连带行为摆在一起才算证据。

### `uint32 arkrtos_port_paging_lookup(unsigned long va, uint32 *index, uin`

```c
uint32 arkrtos_port_paging_lookup(unsigned long va, uint32 *index, uint32 *ppn, uint32 *ps) {
```

**读回一个 VA 当前由哪条 TLB 条目翻译（诊断用）**
- `va`：要查的虚拟地址
- `index`：命中条目的绝对号（STLB 0..2047 / MTLB 2048..）
- `ppn`：命中条目的偶页物理基址（TLBELO0 的 PPN）
- `ps`：命中条目的页大小指数（12 = 4 KB，22 = 4 MB）
**返回**：1 = 命中；0 = 未命中（此时三个输出参数不被写）
`tlbsrch` 按 TLBEHI.VPPN 查找，命中时把命中条目的 INDEX/PS 写回
TLBIDX、把 TLBELO0/1 写回 CSR。未命中时 TLBIDX 的 EHINV 位
（bit 31）置 1。这条接口只读，不建映射、不改状态。
存在的理由：tlbwr 写完一条条目后，「查找到底命中谁」在 C 层
看不见；地址空间切换曾经**静默失效**（写进 space 1 的数据在
space 0 里读得到），就是靠它把「写进去了但没生效」和「生效了
但命中另一条」分开的。

### `static uint32 la_pg_static_hit(uint32 base, uint32 size) {`

```c
static uint32 la_pg_static_hit(uint32 base, uint32 size) {
```

这段 VA 是否落在静态跨度里（重叠即算） */

### `static unsigned long la_pg_dyn_lo(uint32 pa, arkrtos_mpu_mem_t mem) {`

```c
static unsigned long la_pg_dyn_lo(uint32 pa, arkrtos_mpu_mem_t mem) {
```

mem -> TLBELO；0 表示这条属性不提供动态映射
> 说明：开 ASID 档时四条都带 G（全局）：动态别名是**内核**的 MMIO 通路
（PLV0），它不属于任何一个 App 空间，绑死在某一个 ASID 上就会出现
「换个空间就 VFS 读不动 / 控制台写不出」这种只在开档后显形的形态。 */

### `static uint32 la_pg_dyn_mask(arkrtos_mpu_mem_t mem) {`

```c
static uint32 la_pg_dyn_mask(arkrtos_mpu_mem_t mem) {
```

属性掩码：RW 的页同时满足 RO 的审计，反过来不行 */

### `int32 arkrtos_port_paging_alias(uint32 va, uint32 pa, arkrtos_mpu_mem_`

```c
int32 arkrtos_port_paging_alias(uint32 va, uint32 pa, arkrtos_mpu_mem_t mem) {
```

**追加一条动态映射（一页 4 KB）**
**返回**：0 = 已进 TLB 且可审计（同一页同一属性重复登记也是 0）；
-1 = 参数/属性不支持；-2 = 与静态映射重叠；-3 = 动态表满；
-5 = 同一 VA 已登记成别的物理页/属性

### `int32 arkrtos_port_paging_unalias(uint32 va) {`

```c
int32 arkrtos_port_paging_unalias(uint32 va) {
```

**撤销一条动态映射（U-15）**
**返回**：0 = 已撤销；-1 = 未 4 KB 对齐；-2 = 落在静态跨度里；-4 = 没登记过
动态条目落在 MTLB 的绝对索引上（LA_MTLB_INDEX + LA_MTLB_DYN_FIRST
+ slot），撤销时按同一个索引 tlbwr 写回一条 V=0 的条目。
**但只重写 MTLB 不够**：这个 VA 一旦被访问过，STLB 与 QEMU 的
softmmu 缓存里还留着旧翻译，tlbwr 只改 machine TLB，不会回头看
STLB（实测症状：look 报 no entry，可经这个 VA 读出来的仍是旧物理
页）。所以两半都先 invtlb 再写。静态映射一律拒绝（-2）：那是内核
自己的条目，撤掉它下一次取指就 KERNEL refill。
只清被撤的那一半：一对 8 KB 里另一半还登记着就不能一起丢。两半
都空了才把槽位标回空闲，让下一个调用方能用上它 —— 这正是 U-15 缺
的那一半能力（「动态映射只增不减」）。 */

### `uint32 arkrtos_port_paging_dyn_used(void) {`

```c
uint32 arkrtos_port_paging_dyn_used(void) {
```

**已占用的动态映射槽位数**
只会数 used 标志，不去读 TLB：读回路径（tlbsrch/tlbrd）在
撤销之后不一定还能告诉你「槽位空着」，而槽位书是真的。 */

### `int32 arkrtos_port_paging_init(void) {`

```c
int32 arkrtos_port_paging_init(void) {
```

**报告分页通路可用**
**返回**：0 = 本 port 有分页通路
本函数**不**建映射：映射由 la_mmu_init() 在 arkrtos_port_irq_init()
里建好，而 arkrtos_mpu_module_init() 可能比它更早被调用。在这里
读 CRMD.PG 判断「是否已生效」会得到一个与调用时机有关的答案，
那不是这条接口要回答的问题。真正建映射失败时 la_mmu_init 之后的
首条取指就会 refill 致命，不会静默。

### `int32 arkrtos_port_paging_check(unsigned long base, uint32 size, arkrt`

```c
int32 arkrtos_port_paging_check(unsigned long base, uint32 size, arkrtos_mpu_mem_t mem) {
```

**审计一段地址跨度是否被静态映射覆盖**
- `base`：起始虚拟地址
- `size`：字节数
- `mem`：期望属性
**返回**：0 = 已覆盖且权限满足；-1 = 未覆盖 / 权限不足 / 参数非法
判定按「整段必须落在单条跨度之内」：跨两条条目的跨度不在这里
接受，因为调用方（TCB 窗口）本来就是单块连续内存，跨条目只可能
是记录坏了 —— 那种情况下报 -1 才是诚实的。

### `void arkrtos_port_paging_flush(uint32 base, uint32 size) {`

```c
void arkrtos_port_paging_flush(uint32 base, uint32 size) {
```

**让一段地址的翻译失效**
静态映射从不撤销：真去 invtlb 会把内核自己的条目打掉，下一次
取指就走 TLBRENTRY。所以这里是空操作，而不是「尽力而为」。

### `static uint32 la_fr_test(uint32 i) {`

```c
static uint32 la_fr_test(uint32 i) {
```

该帧是否已分配 */

### `static void la_fr_mark(uint32 i) {`

```c
static void la_fr_mark(uint32 i) {
```

标记已分配（调用方保证之前是空闲） */

### `static void la_fr_clear(uint32 i) {`

```c
static void la_fr_clear(uint32 i) {
```

标记空闲（调用方保证之前是已分配） */

### `int32 arkrtos_port_paging_frame_alloc(uint32 count, uint32 *p_base) {`

```c
int32 arkrtos_port_paging_frame_alloc(uint32 count, uint32 *p_base) {
```

**分配一段连续的 4 KB 页帧**
- `count`：需要的帧数（> 0）
- `p_base`：成功时写回首帧的物理基址
**返回**：0 = 成功；-1 = 参数非法；-2 = 没有这么长的连续空闲段
从低帧号往高扫，取第一段满足的 run（first-fit）。不做碎片整理：
整理会移动别人正在用的帧，那不是分配器该悄悄做的事。

### `int32 arkrtos_port_paging_frame_free(uint32 base, uint32 count) {`

```c
int32 arkrtos_port_paging_frame_free(uint32 base, uint32 count) {
```

**归还一段连续 4 KB 页帧**
- `base`：首帧物理基址（必须 4 KB 对齐、落在池内）
- `count`：帧数（> 0）
**返回**：0 = 已归还；-1 = 不在池内 / 未对齐 / count 越界；
-3 = 这段里本来就有没分配过的帧（此时**一帧都不动**）
「先全部验证再改」：只要有一帧本来空闲，整段拒绝。否则同一个
帧会被两个持有者同时以为归自己，那个 bug 只在很久以后显形。

### `uint32 arkrtos_port_paging_frame_total(void) {`

```c
uint32 arkrtos_port_paging_frame_total(void) {
```

帧总数 */

### `uint32 arkrtos_port_paging_frame_used(void) {`

```c
uint32 arkrtos_port_paging_frame_used(void) {
```

已分配帧数 */

### `uint32 arkrtos_port_syscall_num(void *p_exc_ctx) {`

```c
uint32 arkrtos_port_syscall_num(void *p_exc_ctx) {
```

**从异常帧里取系统调用号**
LoongArch 的 `syscall code` 把号码编在指令里（15 位），所以这里
从「指令字」解出来，而不是从某个寄存器槽读。
**注意**: la_trap_c() 在调 SVC_Server() 之前已经把帧里的 ERA 加了 4
（syscall 指令占 4 字节），所以此刻 ERA 指向下一条指令，被调用的
syscall 在 ERA - 4 处。两条路径（谁先谁后）在文件头注释里写死，
改动时两处必须同时改。
解出的指令若不是 syscall 形式，返回 0xFFFFFFFF：调用方会落到
default 分支拒绝这次服务，而不是拿一个看似合法的号去执行。

### `uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx) {`

```c
uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx) {
```

**Linux/LoongArch ABI 的系统调用号（syscall 0 + $a7）**
Ark 自己的服务号写在 `syscall` 的 15 位立即数里，Linux 用
`syscall 0` 并把号放进 $a7。因此「立即数是否为 0」就是分界，
不需要按号段猜，两边也不会撞号。

### `uint32 arkrtos_port_svc_frame_bytes(void) {`

```c
uint32 arkrtos_port_svc_frame_bytes(void) {
```

**整份 trap 帧的字节数与整块存取（信号快照用）**
信号不能只存 era/sp/ra/tp：处理函数是普通 C 函数，会把
$t0 一类的临时寄存器改成自己的东西，返回后被打断的调用
约定就断了（实测症状是恢复后 ld.w 用了一个已经变成 2 的
基址）。所以快照必须是整帧。
import 唯一不认的是 PRMD 的特权级两位：那一层由当前上下文
决定，用户铺的帧不许抬高特权级。

### `#define LA_UREG_ERA         (0u) #define LA_UREG_SP          (1u) #def`

```c
#define LA_UREG_ERA         (0u) #define LA_UREG_SP          (1u) #define LA_UREG_RA          (2u) #define LA_UREG_TP          (3u)
```

**读写用户态 trap 帧里的寄存器（信号帧保存/恢复用）**
帧布局见 arkrtos_context.S：era 在 [30]、sp 在 [2]、tp 在 [1]、
a0..a7 在 [3..10]；ra 就是 [0] 那格（`.S` 的恢复路径 `ld.d $ra`）。
语义编号只有四个：0 = era、1 = sp、2 = ra、3 = tp。写 era 不改
PRMD：信号处理程序与被打断的代码同处 PLV3，特权级不该被这一层动。

### `int32 arkrtos_port_svc_exec_frame(void *p_exc_ctx, unsigned long entry`

```c
int32 arkrtos_port_svc_exec_frame(void *p_exc_ctx, unsigned long entry, unsigned long sp, unsigned long tp) {
```

**把当前任务的 trap 帧改写成「新镜像的第一条指令」（execve 落地）**
帧布局见 arkrtos_context.S 文件头：ERA 在 [30]、PRMD 在 [31]、
sp 在 [2]、tp 在 [1]、a0..a7 在 [3..10]。这张帧就是 la_trap_c
返回给 eentry 恢复的那一张，所以改完之后 ertn 直接跳进新镜像，
中间不再经过任何用户态代码。
入口必须在 App 窗口内：窗口外的地址一恢复就会从别处取指，
那是「跑起来了但跑的不是这个文件」——宁可拒绝。
写代码之后必须 dbar + ibar：新镜像的指令刚刚由内核写进同一个
VA（execve 是**就地覆盖**，这些地址的旧指令很可能还在 I-cache 里）。
QEMU 会按页作废翻译块，看不出差别；真机上省掉这两条就是
「跑了旧程序的半截」。

### `uint32 arkrtos_port_enter_critical(void) {`

```c
uint32 arkrtos_port_enter_critical(void) {
```

**进入临界区，返回进入前的 CRMD**
与 RISC-V 版「一次读改写」同一口径：先 csrrd 读旧值，再
csrxchg 清 IE。csrxchg 的 rd 同时是新值来源，不能一次搞定两者，
所以这里是两条指令 —— 它们之间若来中断也不改变语义：临界区
的调用方随后会发现自己拿到的 state 里 IE=1，退出时会重新打开。

### `void arkrtos_port_switch_task(void) {`

```c
void arkrtos_port_switch_task(void) {
```

**请求一次任务切换**
两种落点：
- 在异常处理程序里调用：只置 s_switch_pending，由 la_trap_c()
的出口结算（等价于 RISC-V 那次软中断，但不需要真的再进一次
trap）——绝不能在 trap 里就地 ertn 换栈，那会把异常处理程序
自己丢掉；
- 在任务上下文里调用：交给汇编的就地切换路径，构造同格式帧后
ertn（见 arkrtos_context.S）。
门闩未打开时两种落点都退化为「什么都不做」。

### `void arkrtos_port_set_thread_priv(uint32 is_priv) {`

```c
void arkrtos_port_set_thread_priv(uint32 is_priv) {
```

**记录下一个任务的特权级（由 arkrtos_sched_activate 每次切换时调用）**
- `is_priv`：1 = 内核任务（PLV0），0 = App（PLV3）
这里**不能**直接写 CRMD.PLV：本函数在 sched_activate() 内部被
调用，此刻还不知道要恢复哪个帧；而 ertn 恢复特权级只认帧里的
PRMD.PPLV。所以只记下来，由拿帧的一方用 LA_PRIV_APPLY 补写。

### `unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*`

```c
unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*entry)(void)) {
```

**构造任务初始 trap 帧**
- `stack_top`：栈顶（高地址）
- `entry`：任务入口
**返回**：帧指针（存入 task->stack_ptr）
帧格式与 arkrtos_context.S 完全一致。任务第一次被恢复时会走
「eentry 的恢复半段」：ERA←entry、PRMD←LA_PRMD_INIT（PIE=1、
PPLV=0，值为 0x4）、
sp←stack_top，然后 ertn 跳进任务，此时 CRMD.IE 已经打开。
ra 置 0：任务函数返回就等于跳到 0 取指，会立刻进致命异常，
与 Cortex-M 版（LR=0）同一处理口径。

### `void arkrtos_port_task_set_tp(unsigned long stack_ptr, unsigned long t`

```c
void arkrtos_port_task_set_tp(unsigned long stack_ptr, unsigned long tp) {
```

**把初始栈帧里的 TP（$r2）写成给定值**
- `stack_ptr`：arkrtos_port_stack_init() 返回的帧指针
- `tp`：TLS 块起始地址
帧偏移 8 就是上下文里保存 $tp 的那一格（见
arkrtos_context.S 顶上的帧表）；第一次 ertn 进用户态时由
`ld.d $tp, ...` 还原。装载器在 PT_TLS 存在时调用这里，于是
程序第一句就可能读到的 __thread 变量已经是真值。
关档时本函数整段不编译：默认镜像与符号表逐字节不变。

### `void arkrtos_port_start_timer(uint32 tick_period_us) {`

```c
void arkrtos_port_start_timer(uint32 tick_period_us) {
```

**启动节拍定时器**
- `tick_period_us`：节拍周期（微秒）
TCFG 是自动重装的周期定时器（Periodic 位），硬件负这个责，
异常处理里只需要清 TICLR 并调内核节拍处理。
定时器中断的使能位（ECFG.LIE[11]）也在这里才放行 —— 与 RISC-V
版同一顺序：irq_init() 只开全局中断，节拍在调度数据结构就绪后
才允许进来。

### `la_frame_word *la_switch_inline(la_frame_word *frame) {`

```c
la_frame_word *la_switch_inline(la_frame_word *frame) {
```

**就地切换的 C 半段（由 arkrtos_context.S 的立即切换路径调用）**
- `frame`：刚构造好的当前帧
**返回**：要恢复的帧；不需要切换时原样返回
> 说明：这里还有一道栈帧合法性闸门：next_sp 必须落在内核 RAM 窗内且按
16 字节对齐，否则拒绝切换并计数（不拿野指针去 ld.d）。

### `static void la_fatal_dump(la_frame_word *frame) {`

```c
static void la_fatal_dump(la_frame_word *frame) {
```

**致命异常时把出错上下文的寄存器打出来**
ERA/ESTAT/BADI 只说明「在哪条指令、什么类型的错」，要定位「为什么」
还得看出错那一刻的通用寄存器。帧指针由 eentry 传进 la_trap_c，
这里直接从帧里取，不必再读硬件寄存器（那几个早被本函数改掉了）。
本函数只在致命路径上调用，不影响正常路径的性能与时序。
- `frame`：出错上下文保存的帧

### `static void la_fault_recover(void) {`

```c
static void la_fault_recover(void) {
```

**App（PLV3）故障的统一收尾：记录 → 交内核恢复 → 不返回**
与 RISC-V 的 arkrtos_port_fault_dispatch() 同一语义：
内核的 arkrtos_cpu_fault_handler() 负责计数、按崩溃策略
决定「重建栈帧重启」还是「禁用该 App」，并返回下一个要跑的
帧；本函数只负责把它恢复起来。
帧在这里已经是内核态产物（可能是重建后的新帧），不再回到
出错的那条指令。

### `void la_app_fault_c(la_frame_word *frame, uint32 ecode) {`

```c
void la_app_fault_c(la_frame_word *frame, uint32 ecode) {
```

**从 eentry 抓到的 App 故障（PPI / IPE / 其它）**
- `frame`：eentry 压好的帧（在 App 自己的栈上）
- `ecode`：ESTAT.Ecode
> 说明：不返回。调用者（arkrtos_context.S 的 la_app_fault_switch）已经先把
sp 换到静态故障栈 la_fault_stack：本函数末尾会走
arkrtos_task_recover_pending() -> arkrtos_task_stack_init()，那一步
把**整个任务栈**重填一遍，若还跑在 App 栈上，自己的 C 帧就被抹掉
（RV32 移植踩过，见 c05f1a6）。帧本身（frame 参数）仍在 App 栈上，
所以下面要先把要用的字段取成局部量。

### `void la_refill_c(void) {`

```c
void la_refill_c(void) {
```

**TLB 重填的处理（由 arkrtos_context.S 的 tlb_refill_entry 调入）**
硬件进这条入口时：不保存任何通用寄存器、CRMD.DA=1、PG=0、
PLV=0、IE=0，出错的信息只在 TLBRBADV / TLBRERA / TLBRPRMD 里。
所以入口第一件事是换到静态故障栈（被打断的 sp 本身就可能是坏的）。
两种来源必须分开：
- TLBRPRMD.PPLV != 0：App 摸了未映射地址 ⇒ 正常保护，记一条并恢复；
- PPLV == 0：内核自己摸了未映射地址 ⇒ 移植 bug，直接停机，
绝不能拿「重启任务」把它掩掉（那会让 bug 变成随机重启）。
**恢复时必须写 TLBRERA/TLBRPRMD**：ertn 看到 TLBRERA.ISTLBR=1 就
走重填的返回分支，只认这对镜像 CSR，并把 CRMD.DA 清 0、PG 置 1。
写错了的症状是「ertn 回到出错的那条指令」——无限重填。

### `int32 la_refill_demand_c(void) {`

```c
int32 la_refill_demand_c(void) {
```

**refill 入口在按需档下的 C 入口（由 arkrtos_context.S 调入）**
**返回**：1 = 条目已装好，汇编侧恢复被打断的全部寄存器后 ertn 回原指令；
0 = 不是这一档能处理的（调用方落回 la_refill_c，那条不返回）
与 la_refill_c() 的关键差别：**它要回到出错的那条指令**。
la_refill_c() 的出口只有「换到另一个任务」和「停机」两种，
所以它不需要、也从来没有保存过通用寄存器；按需缺页必须回到原
指令重试，因此保存/恢复放在汇编入口里（见 arkrtos_context.S
的 tlb_refill_entry），这里只负责装条目。
TLBRERA / TLBRPRMD 一个字都不碰：ertn 会按 TLBRERA.ISTLBR=1
回到 era 并把 CRMD.DA/PG 恢复成分页态。

### `static const char *la_ecode_name(uint32 ecode) {`

```c
static const char *la_ecode_name(uint32 ecode) {
```

**异常码的短名（只用于打印，不参与判定）**

### `la_frame_word *la_trap_c(la_frame_word *frame) {`

```c
la_frame_word *la_trap_c(la_frame_word *frame) {
```

**异常帧的统一 C 入口**
- `frame`：当前上下文保存的帧指针（eentry 里由 s0 传入）
**返回**：要恢复的帧指针：发生任务切换时换成另一个任务的帧，否则原样返回
三条路径：
- 中断（Ecode = 0）：本移植只用定时器（IS bit11），清 TICLR 后
交给内核节拍处理；
- syscall（Ecode = 0xB = SYS）：ERA += 4 后进 SVC_Server；
- 其余 Ecode 一律按致命异常处理（报 Ecode/ESTAT/ERA 后停机，
不做假恢复）。
出口处结算 s_switch_pending：这是「没有软中断」的补偿点。

### `static void la_smp_ipi_clear(void) {`

```c
static void la_smp_ipi_clear(void) {
```

**清本核的 IPI 挂起位（IOCSR 0x1000 状态 → 0x100C 清除）**
> 说明：这段序列原来是 arkrtos_context.S 里 la_smp_trap_entry 的内联拷贝，
打开调度分核后汇编不再自己清，两处只剩这一份。

### `la_frame_word *la_smp_trap_c(la_frame_word *frame, uint32 cpu) {`

```c
la_frame_word *la_smp_trap_c(la_frame_word *frame, uint32 cpu) {
```

**次核异常的统一 C 入口（只在调度分核打开时存在）**
- `frame`：次核入口在被打断上下文栈上压好的 256 字节帧
- `cpu`：本核号（汇编按 CSR.TID 传入）
**返回**：要 ertn 回去的帧指针（定时器路径可能换成另一个任务的帧）
三条路径，全都只用"本核"状态：
- 定时器中断：清 TICLR → 本核计一拍 → 调度器决定下一个帧；
- IPI：清挂起 → 默认处理函数（计数，应用可覆盖强符号）；
- 其余一律致命：次核不许发 SVC（对象表/文件系统/同步对象都没有
跨核保护），也不做 App 故障恢复（那套页表与窗口逻辑属于 CPU0）。
绝不把次核的 SYS 静默放行——放过去就是数据竞争，报错停机是唯一
诚实的处理。

### `ARKRTOS_WEAK void arkrtos_port_irq_init(void) {`

```c
ARKRTOS_WEAK void arkrtos_port_irq_init(void) {
```

**安装异常入口并打开全局中断**
三件事：
1) EENTRY = eentry（异常入口只有这一个，Ecode 全在 ESTAT 里）；
2) 清 TICLR，避免复位值留下挂起位；
3) CRMD.IE = 1 —— 相当于 Cortex-M 复位后 PRIMASK 为 0。
定时器中断使能（ECFG.LIE[11]）**不**在这里开：那时内核的调度
数据结构还没初始化，节拍在 trap 里改调度状态是不安全的。

## kernelsrc/port/riscv/rv32imac/arkrtos_port.c

**Ark RTOS RISC-V RV32IMAC 移植层实现**
实现 arkrtos_hal.h 声明的全部硬件抽象接口。目标平台是 QEMU 的
`virt` 机器（单 hart、M 模式、CLINT + 16550 UART），但本文件里
只有「内核移交点」与 CLINT 地址是平台相关的，其余都是 RISC-V

### `void arkrtos_port_plic_enable(uint32 irq, uint32 prio) {`

```c
void arkrtos_port_plic_enable(uint32 irq, uint32 prio) {
```

**打开一个 PLIC 中断源（优先级 > 0 + 使能位 + 阈值放行）**
- `irq`：中断号（virt 机器：UART0 = 10，UART1 = 11，VIRTIO = 1..8）
- `prio`：优先级 1..7，0 表示永久关闭
> 说明：阈值固定为 0（= 只要优先级 > 0 就放行）。做优先级分组需要它，
但这套内核的中断优先级在 ARM 侧是 NVIC 的寄存器，语义不通用，
所以这里不假装支持抢占级。

### `void arkrtos_port_plic_disable(uint32 irq) {`

```c
void arkrtos_port_plic_disable(uint32 irq) {
```

**关闭一个 PLIC 中断源**

### `static void rv_plic_dispatch(void) {`

```c
static void rv_plic_dispatch(void) {
```

**外部中断分发：claim -> 板级处理 -> complete**
必须先 claim 拿到 id、处理完再 complete，否则同一源会一直
挂起（电平触发），trap 退不出去。

### `uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,`

```c
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value) {
```

**比较并交换（LR/SC 实现）**
QEMU 的 rv32 带 A 扩展，`lr.w` / `sc.w` 是真原子指令，不需要
退化成「关中断近似」。单核下关中断也算正确，但这套写法在 SMP
上同样成立，将来换多 hart 时不用重写。

### `void rv_smp_trap_c(uint32 mcause) {`

```c
void rv_smp_trap_c(uint32 mcause) {
```

**次核异常入口的 C 部分（由 arkrtos_context.S 的 rv_smp_trap_entry 调用）**
次核只认一种来源：机器软件中断（msip = IPI）。其余原因一律按
致命异常处理并停机——次核没有可用的内核状态（共享的 s_in_trap、
g_rv_next_mpp、中断栈都是主核一份），把它放进完整 trap 分派
只会在别人的状态上乱写。
> 说明：次核不开机器定时器中断（mie.MTIE 保持 0），所以不会有「次核 tick」。

### `uint32 arkrtos_port_syscall_num(void *p_exc_ctx) {`

```c
uint32 arkrtos_port_syscall_num(void *p_exc_ctx) {
```

**从 trap 帧里取系统调用号**
RISC-V 的 `ecall` 没有立即数字段（ARKRTOS_ARCH_SVC_NUM_BITS = -1），
号由约定寄存器携带：本移植用 a7，与 Linux/RISC-V 的习惯一致。

### `uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx) {`

```c
uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx) {
```

**Linux/RISC-V ABI 的系统调用号（a7）**
本移植的 Ark 服务号本来就走 a7。两个 ABI 共用同一寄存器与同一条
`ecall`，只能按号段让位：0x10..0x22 是 Ark 自己的服务号
（ARKRTOS_SVC_LOG .. ARKRTOS_SVC_WAITMULTI_CTRL），永远归 Ark；
其余号交给 Linux 表，不认识的由兼容层回 -ENOSYS，不会静默当成功。

### `uint32 arkrtos_port_enter_critical(void) {`

```c
uint32 arkrtos_port_enter_critical(void) {
```

**进入临界区，返回进入前的 mstatus（只关心 MIE 位）**
用 csrrc 一次完成「读旧值 + 清 MIE」，比先读后写少一条指令，
也不会被两条指令之间的中断打断。

### `void arkrtos_port_switch_task(void) {`

```c
void arkrtos_port_switch_task(void) {
```

**触发一次任务切换**
置 CLINT 软件中断挂起位。这是 RISC-V 里 PendSV 的对应物：
- 在中断上下文里调用：此刻 mstatus.MIE = 0，中断被挂起，
等 mret 之后立刻进 trap → 切换，不会在 tick 处理中途换栈；
- 在线程上下文里调用：立刻进 trap 完成切换。
两种情形都与内核「置挂起、稍后切」的假设一致。

### `void arkrtos_port_set_thread_priv(uint32 is_priv) {`

```c
void arkrtos_port_set_thread_priv(uint32 is_priv) {
```

**设定「下一次 mret 回到哪个特权级」**
- `is_priv`：非 0 = 内核任务（M 模式），0 = App 任务（U 模式）
本函数在 trap 处理程序里被调用（切换路径），此刻 CPU 已在 M 模式，
所以改的是「即将恢复的任务」而不是自己。真正的落笔位置是汇编恢复
半段的 mstatus.MPP —— 见 arkrtos_context.S 的注释。
不在 C 里直接改帧的原因：arkrtos_sched_activate() 返回的帧指针
要到它返回之后才拿到，而本函数是在它内部被调用的。

### `unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*`

```c
unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*entry)(void)) {
```

**构造任务初始 trap 帧**
- `stack_top`：栈顶（高地址）
- `entry`：任务入口
**返回**：帧指针（存入 task->stack_ptr）
帧格式与 arkrtos_context.S 完全一致。任务第一次被恢复时会走
「trap_entry 的恢复半段」：mepc←entry、mstatus←0x1880、sp←stack_top，
然后 mret 跳进任务，此时 MIE 已经打开。
ra 置 0：任务函数返回就等于跳到 0 取指，会立刻进致命异常，
与 Cortex-M 版（LR=0）同一处理口径。
gp 取当前内核上下文里的值（start.S 设好了 __global_pointer$），
否则链接器的 gp 相对松弛在小任务里会读到错误地址。

### `void arkrtos_port_start_timer(uint32 tick_period_us) {`

```c
void arkrtos_port_start_timer(uint32 tick_period_us) {
```

**启动节拍定时器**
- `tick_period_us`：节拍周期（微秒）
用 CLINT 的 mtimecmp 做「下一次比较」：每进一次定时器中断就
重装 mtime + s_tick_counts，硬件负不了这个责，由 riscv_trap_c 补。

### `uint32 *riscv_trap_c(uint32 *frame) {`

```c
uint32 *riscv_trap_c(uint32 *frame) {
```

**trap 帧的统一 C 入口**
- `frame`：当前上下文保存的帧指针（trap_entry 里由 tp 传入）
**返回**：要恢复的帧指针：发生任务切换时换成另一个任务的帧，否则原样返回
三条路径：
- 机器定时器中断（0x80000007）：重装 mtimecmp 后交给内核节拍处理；
- 机器软件中断（0x80000003）：清挂起位，做一次调度决策；
- 环境调用（11 = M 模式 ecall）：mepc + 4 后进 SVC_Server。
其余原因一律按致命异常处理（打印后停机，不做假恢复）。

### `ARKRTOS_WEAK void arkrtos_port_irq_init(void) {`

```c
ARKRTOS_WEAK void arkrtos_port_irq_init(void) {
```

**安装异常入口并打开 M 模式中断**
三件事：
1) mtvec = trap_entry（直接模式，基址 4 字节对齐）；
2) mie 打开 MSIE（软件中断，任务切换用）与 MTIE（定时器节拍）；
3) mstatus.MIE = 1 —— 相当于 Cortex-M 复位后 PRIMASK 为 0。
不在这里打开的话，switch_enable 置的软件中断永远不会被取走，
第一个任务也就永远启动不了。

### `int32 arkrtos_port_mpu_encode(arkrtos_arch_mpu_t *p_mpu, uint32 idx, u`

```c
int32 arkrtos_port_mpu_encode(arkrtos_arch_mpu_t *p_mpu, uint32 idx, uint32 base, uint32 size, arkrtos_mpu_mem_t mem) {
```

**把一个区域编码进任务上下文（不碰硬件）**
只算 pmpaddr / pmpcfg 两个数。写 CSR 在 set_app() 里做，
因为切换时必须「先关后写」，中间态不能让 U 模式看到。

### `void arkrtos_port_mpu_set_app(const arkrtos_arch_mpu_t *p_mpu) {`

```c
void arkrtos_port_mpu_set_app(const arkrtos_arch_mpu_t *p_mpu) {
```

**把一份任务上下文写进 PMP**
顺序很关键：先把所有 entry 关掉，再写 pmpaddr，最后一次性写
pmpcfg。反过来的话，U 模式可能在「新地址 + 旧属性」的窗口里
访问到一块本来不该给它的内存。
传 0 表示「没有可给 U 模式的东西」——idle 任务与未配置的任务用它。

### `void arkrtos_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uin`

```c
void arkrtos_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size) {
```

**捕获空闲任务的保护上下文（与 M4 移植同名接口的语义对齐）**
本目标上空闲任务跑 M 模式，不需要任何 U 模式权限，所以这里
只接收参数不建 region。

### `void arkrtos_port_fault_dispatch(uint32 mcause, uint32 from_priv) {`

```c
void arkrtos_port_fault_dispatch(uint32 mcause, uint32 from_priv) {
```

**访问异常的实际处理体**
- `mcause`：硬件原因号（1/5/7）
- `from_priv`：出事时的特权级（0=U，3=M）
入口是汇编的 rv_fault_enter：它先把 sp 换成 rv_fault_stack_top_addr，
再 tail 进来。换栈的理由只有一个但很硬：内核的故障恢复会重建
肇事任务的栈（整段填 0xcdcdcdcd），如果处理程序还在那段栈上跑，
重建一完成，处理程序自己取回的 ra 就是填充图案，返回就飞了。
本函数永不返回：要么装上下一个任务的上下文（resume_task），
要么停机等调试器。

## kernelsrc/sdk/app_sdk/arkrtos_app_main.c

**Ark RTOS App SDK - 入口与退出胶水**
入口是**用户自己写的标准 C `main()`**：
#include <stdio.h>
int main(void)

### `void arkrtos_app_exit(int code) {`

```c
void arkrtos_app_exit(int code) {
```

**标准 C 的退出路径：结束当前线程（带退出码）**
- `code`：退出码；ARKRTOS_USE_EXIT_CODE=1 时交给内核（SVC 0x1B/4），
之后 shell 的 exec / wait 能读到；关档时忽略，行为与之前的
普通退出逐位相同
> 说明：不会返回。App 的其它线程不受影响。

## kernelsrc/sdk/app_sdk/arkrtos_stdio.c

**Ark RTOS App / 小程序 SDK - 标准 C 控制台输出**
让用户源码里最常见的这几句直接可用：
#include <stdio.h>
printf("Hello, World!\n");

### `static void stdio_out(const char *s) {`

```c
static void stdio_out(const char *s) {
```

**把一段文本送到内核控制台，`'\n'` → `"\r\n"`**
- `s`：NUL 结尾字符串（可为 NULL）
> 说明：分段发送：每段不超过 ARKRTOS_STDIO_CHUNK 字节，因为内核 SVC 0x1A
子命令 3 会拒收 128 字节及以上的串。分段是按字节边界切的，切点只可能
落在两个完整字节之间，不会把 `"\r\n"` 拆坏（`'\r'` 先入缓冲）。

### `int printf(const char *fmt, ...) {`

```c
int printf(const char *fmt, ...) {
```

**格式化并输出到内核控制台（标准 C `printf` 语义）**
**返回**：写入的字符数（不含结尾 NUL）；被截断时返回实际格式化的字符数

### `int puts(const char *s) {`

```c
int puts(const char *s) {
```

**输出字符串并追加换行（标准 C `puts` 语义）**
**返回**：成功返回非负数，参数为空返回 -1

### `int putchar(int c) {`

```c
int putchar(int c) {
```

**输出单个字符（标准 C `putchar` 语义）**
**返回**：写出的字符；参数为 EOF 时返回 EOF

## kernelsrc/sdk/app_sdk/tests/test_stdio_host.c

**App / 小程序 SDK 的 `printf` / `puts` / `putchar` 宿主门禁**
编 `kernelsrc/sdk/app_sdk/arkrtos_stdio.c`，用一份桩替换
`arkrtos_shell_print()`（板上是 SVC 0x1A），把送出去的字节收进缓冲，
再逐字节与期望值比对。

### `static void cap_reset(void) {`

```c
static void cap_reset(void) {
```

**清空捕获缓冲，并把它当作"一个屏幕"的开始 */**

### `static int cap_len(void) {`

```c
static int cap_len(void) {
```

**捕获内容里有多少个字节 */**

## kernelsrc/sdk/driver_sdk/arkrtos_driver_sdk.h

**Ark RTOS Driver SDK - 驱动开发接口**
驱动有两种部署形态，注册/注销/计数接口相同，
驱动代码不需要区分自己是哪种形态：
由宏 ARKRTOS_DRV_USER_MODE 选择：

### `typedef int32 (*arkrtos_hwsafe_fn)(void *obj, uint32 owner);`

```c
typedef int32 (*arkrtos_hwsafe_fn)(void *obj, uint32 owner);
```

**Hardware safe-state ("will") callback a driver declares for a device**
- `obj`：the device instance that was opened (driver private struct; hdr first)
- `owner`：the (already dead) task that owned it; for logging only
**返回**：0 = brought to its safe state; non-zero = the driver reports failure
`@warning Runs from the task-exit / fault-recovery path, possibly with`
interrupts masked. Must be non-blocking (bounded register / GPIO
writes only - no wait, no delay, no SVC, no filesystem or Flash)
and idempotent. See docs/App崩溃硬件安全与遗言机制.md.

## kernelsrc/sdk/mini_sdk/arkc/arkrtos.h

**小程序 SDK 的 arkc 版最小内核声明（只够编 miniapp）**
这份头为什么存在：`sdk/mini_sdk/arkrtos_mini_main.c` 里写的是
`#include "arkrtos.h"`，而完整的那一份（`kernelsrc/include/arkrtos.h`）
把内核对外的一整套类型、SVC 门面、日志接口都摊开，编进小程序时：

### `void arkrtos_thread_exit(void);`

```c
void arkrtos_thread_exit(void);
```

**结束当前小程序，把代码块与 RAM 块交还给内核**
> 说明：实现见 sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c：SVC 0x1B 子命令 3，
与 app_sdk 的 arkrtos_oslib.c 是同一条。

## kernelsrc/sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c

**小程序 SDK 的 arkc 运行时：POSIX 控制台写出 + 线程退出**
这个小程序为什么需要它，而 Keil / GCC 的小程序不需要：
Keil / GCC 的小程序把 `sdk/app_sdk/arkrtos_oslib.c` 链接进来，
那份门面把内核的每一个服务都封成 SVC 调用。arkc 编得动它（实测

### `int write(int fd, const void *buf, unsigned int len) {`

```c
int write(int fd, const void *buf, unsigned int len) {
```

**POSIX write：fd 1/2 写控制台，其余返回 -1**
**返回**：实际交给内核的字节数（= len），失败 -1
> 说明：已知限制：控制台接口按 NUL 结尾字符串输出，所以负载里出现 0 字节时
只有它之前的部分会出来。往控制台打二进制本来就没有意义，这里不假装
支持（不转义、不拆分），如实记在文档里。

### `void _exit(int code) {`

```c
void _exit(int code) {
```

**POSIX _exit：小程序没有进程，交还自己占的两块 RAM**
> 说明：与 app_sdk 的 arkrtos_thread_exit() 是同一条 SVC。main() 正常返回时
由启动桩调用它，程序里显式调用也合法（提前结束）。

### `void arkrtos_thread_exit(void) {`

```c
void arkrtos_thread_exit(void) {
```

**App SDK 的退出接口，供 sdk/mini_sdk/arkrtos_mini_main.c 使用**
> 说明：那个文件按 SDK 约定调 arkrtos_thread_exit()。Keil / GCC 的小程序由
oslib 提供它；arkc 的小程序由本文件提供，语义完全相同。

### `int arkrtos_mini_printf_words(const char *fmt, const u32 *w) {`

```c
int arkrtos_mini_printf_words(const char *fmt, const u32 *w) {
```

**printf 的格式化主体：按格式串从 w[] 里取参数**
- `fmt`：格式串
- `w`：12 个字：w[0] 是格式串本身（桩一起落进槽里），参数从 w[1] 起
**返回**：写出的字符数
> 说明：只给本文件的 printf 桩调用，不要直接call

## kernelsrc/sdk/mini_sdk/arkc/stdio.h

**小程序 SDK 的 POSIX 控制台输出子集（arkc 版）：printf / puts / putchar**
这里只有「打印到控制台」这一件事，目的是让一份标准写法的程序
（`#include <stdio.h>` + `printf("...")` + `int main(void)`）原样
在板上编出来跑。文件、格式化输入、缓冲区管理一概没有。

### `int printf(const char *fmt, ...);`

```c
int printf(const char *fmt, ...);
```

**按格式串输出到控制台**
- `fmt`：格式串，支持的转换见文件头
**返回**：写出的字符数
> 说明：一次调用最多 8 个参数（含格式串）。格式化结果按 96 字节一段写出，
不会因为调用方的缓冲而截断。

### `int puts(const char *s);`

```c
int puts(const char *s);
```

**输出一个字符串再加一个换行**
**返回**：非负值（成功）／-1（空指针）
> 说明：与 C 库一致：换行由 puts 加，字符串本身不要带。

### `int putchar(int c);`

```c
int putchar(int c);
```

**输出一个字符**
- `c`：要输出的字符（只用低 8 位）
**返回**：写出的字符（即 c）

## kernelsrc/sdk/mini_sdk/arkc/unistd.h

**小程序 SDK 的 POSIX 控制台子集（arkc 版）**
这里只有一件事：让「标准 POSIX 的 hello world」原样编译。
和 `kernelsrc/sdk/posix/unistd.h` 的关系：那一份是 **App SDK**
的完整兼容层，`write` 会被映射到 `arkrtos_posix_write()`，后面拖着

### `int write(int fd, const void *buf, unsigned int len);`

```c
int write(int fd, const void *buf, unsigned int len);
```

**把 buf 里的 len 个字节写到标准流**
- `fd`：1 或 2（都写同一个控制台）；其它值返回 -1
**返回**：交给内核的字节数（= len），失败 -1
> 说明：控制台接口收 NUL 结尾字符串，负载里的 0 字节会截断这一块输出；
往终端打二进制不在支持范围内（见 arkrtos_mini_rt_arkc.c 的说明）。

### `void _exit(int code);`

```c
void _exit(int code);
```

**结束本小程序，把代码块与 RAM 块交还内核**
> 说明：等价于 main() 正常返回；调用后不会再回到本程序。

## kernelsrc/sdk/mini_sdk/arkrtos_mini_main.c

### `#if defined(__ARK_CC__) #error "arkc: do not add mini_main.c; write in`

```c
#if defined(__ARK_CC__) #error "arkc: do not add mini_main.c; write int main(void) in your own source instead" #endif
```

**Ark RTOS 小程序 SDK - 入口与退出胶水（Keil / GCC 档）**
入口是**用户自己写的标准 C `main()`**：
#include <stdio.h>
int main(void)
{
printf("Hello, World!\n");
return 0;
}
启动桩（`arkrtos_mini_start.s` / `arkrtos_mini_start_gcc.s`）完成
`.data` 初值拷贝与 `.bss` 清零后直接调用 `main()`。SDK **不再
提供 `main()`**，也不再要求用户写 `MiniMain()`。
返回语义：小程序是"跑完就退"的一次性程序，`main` 返回（或调用
`exit()`）即结束任务，内核立刻归还代码块与 RAM 块——与标准 C
的进程退出一致。`ARKRTOS_USE_EXIT_CODE=1` 的构建里 `main` 的返回
值一路变成任务退出码（SVC 0x1B/4），shell 的 `wait <task id>` 能
读到；关档时忽略码、行为与改动前逐位相同。小程序没有"父进程"，
所以这个码是**旁观者读数**，不会有人被唤醒。
arkc 档**不要编这个文件**：arkc 的入口同样是工程自己的
`int main(void)`，但它的退出由启动桩
（`arkrtos_mini_start_arkc.c`）负责，不需要这里的胶水；更重要的
是 arkc 把每个 `.c` 编成自包含对象，同一翻译单元内的调用在汇编期
就绑死，多一份骨架只会让入口静默失效。这里直接 `#error`。

### `void arkrtos_mini_exit(int code) {`

```c
void arkrtos_mini_exit(int code) {
```

**标准 C 的退出路径：结束当前小程序任务（带退出码）**
- `code`：退出码；ARKRTOS_USE_EXIT_CODE=1 时交给内核（SVC 0x1B/4），
shell 的 wait 能读到；关档时忽略，行为与之前的普通退出逐位相同
> 说明：不会返回。内核在任务退出钩子里归还代码块与 RAM 块。

## kernelsrc/sdk/posix/arkrtos_libc_glue.c

**Wire the toolchain C library's bottom end to the kernel: printf works.**
The compatibility layer in this directory gives a program the POSIX
names. This file is the other half: it makes the names it does *not*
re-implement - the C library's own `printf`, `puts`, `strcpy`,

### `static void glue_open(void) {`

```c
static void glue_open(void) {
```

**Open the console once, on first use.**
> 说明：A failure is remembered on purpose: retrying on every byte would turn
one missing device into thousands of SVC calls, and the answer will
not change by itself.

### `void arkrtos_stdio_init(void) {`

```c
void arkrtos_stdio_init(void) {
```

**Prepare stdio. Optional - the first output call does it too.**

### `int arkrtos_stdio_ready(void) {`

```c
int arkrtos_stdio_ready(void) {
```

**1 while the console is usable */**

### `static int glue_write(const unsigned char *buf, unsigned len) {`

```c
static int glue_write(const unsigned char *buf, unsigned len) {
```

**Write a byte run to the console; returns bytes written or -1 */**

### `int fputc(int ch, FILE *f) {`

```c
int fputc(int ch, FILE *f) {
```

**The one hook every printf-family call funnels through.**
> 说明：Returning EOF on a failed write is the only honest answer: claiming
the character went out would hide a console that is not there.

### `void _ttywrch(int ch) {`

```c
void _ttywrch(int ch) {
```

**Char-by-char console output (used by debuggers and some library paths) */**

### `int fgetc(FILE *f) {`

```c
int fgetc(FILE *f) {
```

**Input side.**
> 说明：There is no stdin on this board: no line discipline, no echo, and the
console belongs to whoever opened it. Reporting end of input is the
truthful answer; inventing a blocking read here would take the
console away from the shell without saying so.

### `void arkrtos_glue_exit(int code) {`

```c
void arkrtos_glue_exit(int code) {
```

**Process exit: end the calling thread.**
`main` 返回与 `exit()` 在标准 C 里是同义语——进程结束。板上没有进程，
只有任务，所以这里结束调用线程（SVC 0x1B 子命令 3），由内核回收它的
栈；本 App 的其它线程继续运行。
这里**不能**改成"挂起任务等一会儿"：那会让 `return 0` 变成"永远不退"，
与 `arkrtos_app_main.c` / `arkrtos_mini_main.c` 给出的语义不一致，
而用户写的是一段标准 C 程序。
> 说明：本文件提供 `_sys_exit` / `_exit`，`arkrtos_app_main.c` 也提供，
两者不要编进同一个工程（用 libc_glue 的工程就不需要 app_main.c）。

### `static int glue_streq(const char *a, const char *b) {`

```c
static int glue_streq(const char *a, const char *b) {
```

**NUL terminated string equality, without <string.h>: this file is**
the bottom of the C library and should not depend on it. */

### `int _sys_istty(FILE *f) {`

```c
int _sys_istty(FILE *f) {
```

**1 = interactive terminal, so the library goes through fputc/fgetc.**
That is the path we want: unbuffered, one byte per write. */

### `int _sys_seek(FILE *f, long pos) {`

```c
int _sys_seek(FILE *f, long pos) {
```

**A console has no file position, so it cannot be seeked. */**

### `int _sys_ensure(FILE *f) {`

```c
int _sys_ensure(FILE *f) {
```

**Flush request. Output is unbuffered, so there is nothing to do. */**

### `long _sys_flen(FILE *f) {`

```c
long _sys_flen(FILE *f) {
```

**Length is unknown for a console stream. */**

## kernelsrc/sdk/posix/arkrtos_posix.c

### `#include <stdarg.h> #include "arkrtos_posix.h" #include "arkrtos_win_c`

```c
#include <stdarg.h> #include "arkrtos_posix.h" #include "arkrtos_win_compat.h" #include "arkrtos_features.h"  /* feature gates: ARKRTOS_USE_POSIX */ #include "arkrtos_net_abi.h"   /* wire contract of SVC 0x1E (sockets) */ #include "arkrtos_svc_call.h"  /* ARKRTOS_SVC_DECL_1 for the call itself */
```

**Ark RTOS POSIX compatibility layer - implementation.**
Three blocks, in this order:
1. errno        - one cell per thread, addressed through SVC 0x1B
2. heap         - first fit arena with coalescing, in App RAM
3. the facades  - pthread / semaphore / mqueue / unistd / time
pthread_create deserves a word. The kernel thread service takes a
bare "void (*)(void)", while POSIX hands over a start routine plus
an argument. The bridge is a trampoline plus one slot per thread:
the creator builds the slot, then starts the thread, and only then
releases a start gate semaphore. The trampoline finds its own slot
through the kernel task id - which is why thread creation cannot
simply run and hope the slot is filled in: the gate makes the
ordering explicit instead of relying on luck with preemption.
`@author xw`

## kernelsrc/sdk/posix/arkrtos_posix_types.h

**Base POSIX types for the Ark RTOS compatibility layer.**
Kept in one place so every POSIX header agrees on the width of
ssize_t / off_t / time_t. The widths are fixed (int32 / uint32)
because Ark RTOS is a 32-bit only kernel and pretending otherwise

### `#define ARKRTOS_CLOCK_REALTIME   0 #define ARKRTOS_CLOCK_MONOTONIC  1`

```c
#define ARKRTOS_CLOCK_REALTIME   0 #define ARKRTOS_CLOCK_MONOTONIC  1
```

POSIX clock ids. Only MONOTONIC is backed by anything here: Ark RTOS has a
boot relative tick, there is no wall clock, so REALTIME is aliased to it
rather than reported as a different (and wrong) time base. */

## kernelsrc/sdk/posix/arpa/inet.h

**Byte order and IPv4 text helpers for Ark RTOS Apps.**
All four conversions are userspace arithmetic - the kernel is not
involved - so they behave exactly as the names promise and cost
nothing but a few instructions.

### `in_addr_t    arkrtos_posix_inet_addr(const char *cp);`

```c
in_addr_t    arkrtos_posix_inet_addr(const char *cp);
```

"127.0.0.1" -> network order address, or INADDR_NONE on a bad string. */

### `char        *arkrtos_posix_inet_ntoa(struct in_addr in);`

```c
char        *arkrtos_posix_inet_ntoa(struct in_addr in);
```

Network order address -> text, in a static buffer this call owns. */

### `int          arkrtos_posix_inet_pton(int af, const char *src, void *ds`

```c
int          arkrtos_posix_inet_pton(int af, const char *src, void *dst);
```

af must be AF_INET; returns 1 on success, 0 on a bad address, -1 with
errno=ENOSYS for a family this layer does not have. */

### `const char  *arkrtos_posix_inet_ntop(int af, const void *src, char *ds`

```c
const char  *arkrtos_posix_inet_ntop(int af, const void *src, char *dst, socklen_t dst_size);
```

The reverse; dst_size must be at least 16 bytes for AF_INET. */

## kernelsrc/sdk/posix/dirent.h

**opendir()/readdir()/closedir() for Ark RTOS Apps.**
DIR is opaque and comes out of a small fixed pool: the App has no
way to know how much RAM it may spend on one directory stream, and
a bounded number is the honest answer. When the pool is empty,

### `#ifndef ARKRTOS_POSIX_NAME_MAX #define ARKRTOS_POSIX_NAME_MAX  (32u) #`

```c
#ifndef ARKRTOS_POSIX_NAME_MAX #define ARKRTOS_POSIX_NAME_MAX  (32u) #endif
```

Longest entry name a dirent can carry, NUL included. One path component,
not a path. */

### `#ifndef ARKRTOS_POSIX_DIR_MAX #define ARKRTOS_POSIX_DIR_MAX  (2u) #end`

```c
#ifndef ARKRTOS_POSIX_DIR_MAX #define ARKRTOS_POSIX_DIR_MAX  (2u) #endif
```

How many directory streams can be open at once. Tunable before including. */

## kernelsrc/sdk/posix/errno.h

**POSIX errno for Ark RTOS Apps.**
errno is per thread. Ark RTOS reaches the kernel through SVC, but
errno itself is pure userspace state - no kernel capability is
involved - so it is stored here, in the App's own RAM, in one cell

### `int32 *arkrtos_posix_errno_location(void);`

```c
int32 *arkrtos_posix_errno_location(void);
```

Address of the calling thread's errno cell; never NULL. */

## kernelsrc/sdk/posix/pthread.h

**POSIX threads for Ark RTOS Apps.**
Ark RTOS has threads, a scheduler and synchronization, but no user
mode way to create a thread: a thread needs a TCB and a scheduler
slot, and only the kernel owns those. pthread_create therefore goes

### `typedef struct {`

```c
typedef struct {
```

Thread attributes. Anything left at 0 takes the built-in default, so a
plain pthread_create(t, NULL, fn, arg) call needs no attributes at all. */

## kernelsrc/sdk/posix/semaphore.h

**POSIX unnamed semaphores for Ark RTOS Apps.**
Backed by the kernel semaphore service (SVC 0x15). sem_t holds the
kernel handle, so sem_init / sem_destroy are the only lifecycle
calls and no static storage is needed.

### `typedef struct {`

```c
typedef struct {
```

Opaque to the caller: only arkrtos_posix.c knows what is inside. */

## kernelsrc/sdk/posix/sys/stat.h

**stat()/fstat() for Ark RTOS Apps.**
The struct is deliberately small: the fields the VFS can actually
answer are here, and the ones it cannot (st_ino, st_nlink, st_uid,
st_mtime as a real clock) are absent rather than filled in with a

### `int arkrtos_posix_stat(const char *path, struct stat *st);`

```c
int arkrtos_posix_stat(const char *path, struct stat *st);
```

Stat a path in the namespace. Returns 0, or -1 with errno set. */

### `int arkrtos_posix_fstat(int fd, struct stat *st);`

```c
int arkrtos_posix_fstat(int fd, struct stat *st);
```

Stat an open descriptor. A device reports S_IFCHR with size 0. */

### `int arkrtos_posix_mkdir(const char *path, arkrtos_mode_t mode);`

```c
int arkrtos_posix_mkdir(const char *path, arkrtos_mode_t mode);
```

Create a directory (one level; the parent must exist). Returns 0, or -1
with errno set. The mode argument is accepted so plain POSIX source
compiles unchanged, but it is not applied: this namespace has no
per-file permission model, and pretending otherwise would be a lie. */

## kernelsrc/sdk/posix/unistd.h

**Minimal <unistd.h> for Ark RTOS Apps.**
Maps the POSIX names a portable C program actually uses onto the
App SDK: read/write/close/lseek act on a file descriptor from
open() (a path or a device), sleep/usleep map onto the scheduler,

### `arkrtos_off_t   arkrtos_posix_lseek(int fd, arkrtos_off_t off, int whe`

```c
arkrtos_off_t   arkrtos_posix_lseek(int fd, arkrtos_off_t off, int whence);
```

Move the offset of an open path. Returns the new absolute offset, or -1
with errno set; ESPIPE when the descriptor is a device (no offset). */

### `int           arkrtos_posix_unlink(const char *path);`

```c
int           arkrtos_posix_unlink(const char *path);
```

Remove a file by path. A directory needs rmdir, never unlink. */

### `int           arkrtos_posix_rmdir(const char *path);`

```c
int           arkrtos_posix_rmdir(const char *path);
```

Remove an empty directory. A file is refused, not silently taken. */

### `int           arkrtos_posix_rename(const char *old_path, const char *n`

```c
int           arkrtos_posix_rename(const char *old_path, const char *new_path);
```

Rename or move a path inside one mount. Returns 0, or -1 with errno set. */

### `int           arkrtos_posix_truncate(const char *path, arkrtos_off_t l`

```c
int           arkrtos_posix_truncate(const char *path, arkrtos_off_t length);
```

Set the length of a named file. The file must exist - "truncate" is not
"create empty". Growing the file zero-fills. With the current drivers a
file that is open for writing cannot be resized: the write stream is
sequential and was cleared at open, so a length change would leave a
half-true state, and the call refuses instead of approximating it. */

### `int           arkrtos_posix_ftruncate(int fd, arkrtos_off_t length);`

```c
int           arkrtos_posix_ftruncate(int fd, arkrtos_off_t length);
```

Set the length of an open file. EINVAL on a descriptor that is not open
for writing (POSIX: resizing is a write, even when the new length is 0),
EISDIR on a directory. The offset is pulled back if it now points past
the end. */

### `int           arkrtos_posix_link(const char *old_path, const char *new`

```c
int           arkrtos_posix_link(const char *old_path, const char *new_path);
```

Hard links do not exist in this namespace: always EPERM, never a silent
copy. POSIX's own answer for "filesystem without hard links". */

### `int           arkrtos_posix_symlink(const char *target, const char *li`

```c
int           arkrtos_posix_symlink(const char *target, const char *linkpath);
```

Symbolic links do not exist here either: always EPERM. */

### `arkrtos_ssize_t arkrtos_posix_readlink(const char *path, char *buf, ui`

```c
arkrtos_ssize_t arkrtos_posix_readlink(const char *path, char *buf, uint32 bufsiz);
```

Read a symbolic link. There is nothing to read: a missing path answers
with errno ENOENT and an existing non-link with EINVAL, exactly as POSIX
describes, rather than a fake empty success. */

### `uint32 arkrtos_posix_sleep(uint32 seconds);`

```c
uint32 arkrtos_posix_sleep(uint32 seconds);
```

Suspend the calling thread for whole seconds (scheduler based). */

### `int    arkrtos_posix_usleep(arkrtos_useconds_t usec);`

```c
int    arkrtos_posix_usleep(arkrtos_useconds_t usec);
```

Suspend the calling thread for microseconds. Ark RTOS waits in ms, so the
value is rounded UP: a sleep never returns early. */

### `int    arkrtos_posix_nanosleep(const struct timespec *req, struct time`

```c
int    arkrtos_posix_nanosleep(const struct timespec *req, struct timespec *rem);
```

Suspend until the given absolute time on the chosen clock. */

### `arkrtos_time_t arkrtos_posix_time(arkrtos_time_t *tloc);`

```c
arkrtos_time_t arkrtos_posix_time(arkrtos_time_t *tloc);
```

Wall clock seconds since boot - Ark RTOS has no RTC. */

## kernelsrc/src/arkrtos_apppack.c

**四遍链接结果的差分打包（纯计算，见 arkrtos_apppack.h 的设计说明）**
三个「为什么这么写」值得单独留在这里：
1. **按 Thumb 指令长度扫描，而不是按 4 字节字扫描。**
Thumb-2 指令是半字对齐、长度 2 或 4 字节的；32 位绝对地址既可能

### `static int32 apppack_scan(const uint8 *a, const uint8 *b, const uint8 `

```c
static int32 apppack_scan(const uint8 *a, const uint8 *b, const uint8 *c, uint32 len, uint32 delta, uint32 *table, apppack_count_t *count) {
```

**差分一遍：把 (相对偏移, 类型) 编码进 table，或只计数**
- `table`：非 0 时逐个写入编码后的表项（偏移相对负载，未加表长）
- `stats`：非 0 时按类型计数（table 为 0 的纯计数模式也用它）

## kernelsrc/src/arkrtos_cfg.c

**Ark RTOS 任务配置加载模块**
管理任务表和任务栈初始化。
栈帧格式由port层的 arkrtos_port_stack_init() 实现，
内核不再硬编码任何架构特定的寄存器布局。

### `void arkrtos_task_stack_fill(uint32 *stack_bottom, uint32 stack_size) `

```c
void arkrtos_task_stack_fill(uint32 *stack_bottom, uint32 stack_size) {
```

**用填充图案覆盖整段任务栈（跳过下标 0 的栈底保护字）**
- `stack_bottom`：栈底地址
- `stack_size`：栈大小（字节）
任务运行过程中会不断覆盖该图案，查询栈用量时扫描仍保持图案的
连续区域即可得到“从未使用过”的栈空间，换算后即为峰值用量。

## kernelsrc/src/arkrtos_dev.c

### `#include "arkrtos_dev.h" #if ARKRTOS_HW_SAFE #include "arkrtos_task.h"`

```c
#include "arkrtos_dev.h" #if ARKRTOS_HW_SAFE #include "arkrtos_task.h"      /* arkrtos_current_task_id: ownership ledger key */ #include "arkrtos_hwsafe.h"    /* App-crash hardware safe-state (will) mechanism */ #endif
```

**Ark RTOS 设备驱动框架实现**
提供统一的设备驱动接口和动态驱动注册机制
内置驱动和可安装驱动共用同一套设备表
`@author xw`
`@date 2026.05.03`

## kernelsrc/src/arkrtos_elf.c

**ELF 装载器（32/64 位）：把 VFS 里的可执行文件搬进 App 窗口并降权起跑**
装载是**流式**的：先读 64 字节 ELF 头，再按 e_phoff 读程序头表，
最后按每个 PT_LOAD 的 p_offset 用 arkrtos_vfs_read_at() 直接读进
目标地址。整份镜像从不进临时缓冲——窗口本身就只有 2 MB，

### `static int32 elf_check_header(const uint8 *h, elf_word_t *p_entry, elf`

```c
static int32 elf_check_header(const uint8 *h, elf_word_t *p_entry, elf_word_t *p_phoff, uint16 *p_phnum, uint16 *p_phentsize, uint16 *p_type) {
```

**校验 ELF 头，把 entry / phoff / phnum / phentsize 取出来**
> 说明：e_machine 单独返回 ERR_ARCH，是为了让「这是 x86 的 ELF」和
「这不是 ELF」在日志里能分开——两者的修法完全不同。

### `static int32 elf_measure_image(const char *path, elf_word_t phoff, uin`

```c
static int32 elf_measure_image(const char *path, elf_word_t phoff, uint16 phnum, uint16 phentsize, elf_word_t entry, unsigned long bias, elf_image_t *p_img) {
```

**量一遍段表：装载会落在哪一段地址上**
- `bias`：ET_DYN 的装载基址；ET_EXEC 传 0（vaddr 本身已经是绝对地址）
这里只读不写，所以任何不合法都能在"写坏字节之前"拒绝。
同时算出程序头表在内存里的地址（AT_PHDR 要它）：只有 phdr 真的
被某个 PT_LOAD 的 file range 盖住，这个地址才存在；否则留 0，
由调用方决定是拒绝还是不给 AT_PHDR。

### `static int32 elf_check_range(const elf_image_t *p_img, unsigned long l`

```c
static int32 elf_check_range(const elf_image_t *p_img, unsigned long lo_limit, unsigned long hi_limit) {
```

**镜像必须整段落在 [lo_limit, hi_limit) 里（含端点判据用减法写） */**

### `typedef struct {`

```c
typedef struct {
```

**一段「文件真的带着字节」的地址区间 [lo, hi) */**

### `static int32 elf_load_file_spans(const char *path, elf_word_t phoff, u`

```c
static int32 elf_load_file_spans(const char *path, elf_word_t phoff, uint16 phnum, uint16 phentsize, unsigned long bias, elf_span_t *spans, uint32 cap) {
```

**收集 PT_LOAD 里「文件背书」的那部分地址区间**
**返回**：区间条数（>=0）；读段表失败或条数超过 ELF_TLS_SPAN_MAX 时返回 -1
「这段字节镜像里已经有了吗」只能按 p_filesz 判，**不能按 p_memsz**：
.tbss 是 NOBITS，它在运行地址上会与后面的 .init_array/.data 重叠
（真实 glibc 静态二进制实测：.tbss 0x120083638+0x50 与 .init_array
同地址）。按 p_memsz 清零就是把构造表抹成 0，程序随后从空指针
call 出去 —— 那正是 2026-11-20 那条 PPI(pc=0) 死循环的根因。

### `static int32 elf_write_tls_piece(const char *path, uint32 space, const`

```c
static int32 elf_write_tls_piece(const char *path, uint32 space, const elf_span_t *spans, uint32 n, unsigned long va, uint32 len, uint32 file_off, uint32 zero, uint32 *p_wrote) {
```

**写（或清零）一段 TLS 字节，跳过镜像里已经文件背书的部分**
- `zero`：1 = 清零；0 = 从文件 file_off 处拷 len 字节
- `p_wrote`：真写了至少一个字节就置 1（调用方据此决定要不要预置 TP）
TLS 段可能**半覆盖**：.tdata 在被 LOAD 背书的区段里（那几字节
本来就等于文件内容，重抄一遍是空操作），.tbss 落在最后一个 LOAD
之后（夹具就是这种形态）。所以按「有没有被某个 LOAD 的 file range
盖住」逐段切，被盖住的直接跳过。

### `static int32 elf_write_tls(const char *path, uint32 space, elf_word_t `

```c
static int32 elf_write_tls(const char *path, uint32 space, elf_word_t phoff, uint16 phnum, uint16 phentsize, unsigned long bias, uint32 *p_placed) {
```

**把 PT_TLS 模板里「镜像还没有」的那部分铺进窗口**
语义从「整块照抄 + 整块清零」收紧成「只补缺口」：
一个字节如果已经被某个 PT_LOAD 的 file range 背书，就**一个字节
都不动** —— 镜像里那份就是文件里那份。只有落在背书范围之外的
部分（典型是最后一个 LOAD 之后的 .tbss）才清零/补数据。
这样处理的原因是一条实测：真实 glibc 静态二进制的 .tbss 与
.init_array 同地址（.tbss 是 NOBITS，文件里那个位置放的是
.init_array 的字节），旧实现按 p_memsz 清零会把构造表抹成 0，
程序在 __libc_csu_init 里 call 空指针，PLV3 取指 0x0 无限 PPI。
- `p_placed`：出参：真往窗口写过字节置 1。**只有置 1 时才允许给任务
预置 TP** —— 全程被镜像覆盖意味着那个地址上躺的是别的数据，
TLS 块由程序自己的 C 运行时去建（glibc 就是这么干的）。
**返回**：ARKRTOS_ELF_OK / 负错误码（读文件失败时原样透出）

### `static int32 elf_find_interp(const char *path, elf_word_t phoff, uint1`

```c
static int32 elf_find_interp(const char *path, elf_word_t phoff, uint16 phnum, uint16 phentsize, char *out, uint32 cap) {
```

**找 PT_INTERP，并把解释器路径拷出来**
**返回**：1 = 有解释器（out 里是路径）；0 = 静态 ELF；负值 = 路径不合法
路径按 Linux 约定是"文件里的字符串"，以 NUL 结尾。这里不信文件
的 NUL：读到 p_filesz 个字节后强制补一个，超长直接拒绝——把
截断的路径当成"另一个文件"去装是最难查的静默失败。

### `static int32 elf_build_stack(unsigned long win_top, unsigned long argv`

```c
static int32 elf_build_stack(unsigned long win_top, unsigned long argv_start, int32 argc, const char *const argv[], elf_aux_t *aux, uint32 auxn, unsigned long *p_sp, unsigned long *p_execfn) {
```

**把 argv 与 auxv 铺成 Linux 的形状，返回初始 sp**
布局（地址从高到低）：
字符串区（argv[i] 的字节）
AT_NULL 的 0/0、auxv 键值对、NULL(envp)、NULL(argv 结束)、
argv[argc-1..0]、argc  <- sp
字符串从窗口顶端往下写，数组紧贴在 argv_start 之上。
**返回**：0 = 成功（*p_sp / *p_execfn 有效）；否则为负错误码

### `static int32 elf_load_interp(elf_load_state_t *st, const char *interp_`

```c
static int32 elf_load_interp(elf_load_state_t *st, const char *interp_path, unsigned long interp_lo, unsigned long interp_hi, elf_image_t *p_interp) {
```

**把解释器（ld.so）装进板级给定的区间，并做完全部越界检查**
- `st`：交接块；只读 space，累加 bytes
- `interp_path`：PT_INTERP 解析出的解释器路径
- `interp_lo`：板级给定的解释器区间下界（ET_DYN 的装载基址）
- `interp_hi`：板级给定的解释器区间上界
- `p_interp`：成功时写回解释器的镜像账（lo/hi/entry/phdr）
**返回**：ARKRTOS_ELF_OK 或错误码；失败一律折成 ERR_INTERP，
因为对调用方而言「主程序没问题、解释器装不上」就是同一个结果
ET_DYN 解释器装到 interp_lo；ET_EXEC 解释器必须本来就链在那里
（它的 vaddr 是绝对的，装到别处等于让它跳进空地址）。
与主程序分开写成一份：两边共用的只有 elf_measure_image /
elf_check_range，而拒绝码不同（主程序给 ERR_LAYOUT，解释器给
ERR_INTERP），混在一起最容易把两种失败报成一种。

### `static int32 elf_load_dynamic(elf_load_state_t *st, const char *interp`

```c
static int32 elf_load_dynamic(elf_load_state_t *st, const char *interp_path) {
```

**动态档分支：装主程序、装解释器、铺 Linux 那套 auxv**
- `st`：交接块；img / bytes / segs / tp / task_entry / auxn 在这里落地
- `interp_path`：PT_INTERP 解析出的解释器路径
**返回**：ARKRTOS_ELF_OK 或错误码；写镜像之前的所有拒绝都发生在动窗口之前

### `static int32 elf_load_static(elf_load_state_t *st) {`

```c
static int32 elf_load_static(elf_load_state_t *st) {
```

**静态档分支：主程序按 e_vaddr 落位，入口就是 e_entry**
- `st`：交接块；字段含义同 elf_load_dynamic
行为与引入 DYNLOAD 之前逐位一致：判入口、量镜像、查范围、写段、
记 tp/segs，再铺一份含 AT_RANDOM / AT_CLKTCK 的 auxv。

### `static int32 elf_load_finish(elf_load_state_t *st) {`

```c
static int32 elf_load_finish(elf_load_state_t *st) {
```

**收尾：铺用户栈与 argv/auxv，填装载信息，按需起任务**
- `st`：交接块；两个装载分支已经写好 img / bytes / segs / tp / task_entry / auxn
**返回**：ARKRTOS_ELF_OK 或错误码
execve 档（start_task == 0）一个任务都不起，只把入口 / 栈指针 / TLS
交回调用方；起任务档里「清兼容层状态 → 注册 → 降权 → MPU 写回」
必须是原子段（理由见函数内注释）。

### `static int32 elf_load(uint32 space, const char *path, int32 argc, char`

```c
static int32 elf_load(uint32 space, const char *path, int32 argc, char *argv[], arkrtos_elf_info_t *p_info, uint32 start_task, unsigned long *p_tp) {
```

**把一份 ELF 装进指定地址空间（可选择顺手起一个降权任务）**
- `space`：目标地址空间（0 .. 空间数-1）；窗口 VA 在每个空间里相同
- `p_info`：成功时填装载读数（不接收失败值）
- `start_task`：1 = 起一个降权任务（exec / exec_space 这条老路）；
0 = 一个任务都不起，只把入口/栈指针/TLS 交回给调用方
（execve 要把**当前任务自己**的镜像换掉，见
arkrtos_elf_exec_replace）
- `p_tp`：可为 0；非 0 时写回新镜像的 TLS 块地址（0 = 没有预置）
静态档与动态档的区别只在「要不要先装一个解释器、入口跳谁」：
- 静态档：段按 e_vaddr 落位，入口就是 e_entry；
- 动态档（PT_INTERP 存在，ARKRTOS_USE_DYNLOAD=1）：主程序照样落位，
解释器装在板级给定的区间里，入口改成解释器的入口，并把
AT_PHDR / AT_BASE / AT_ENTRY 等 auxv 铺在用户栈上——这正是
Linux 传给 ld.so 的那一套。解释器自己负责加载共享库。
没有解释器文件或解释器路径不合法时返回 ERR_INTERP，不半跑。

### `int32 arkrtos_elf_exec_space(const char *path, int32 argc, char *argv[`

```c
int32 arkrtos_elf_exec_space(const char *path, int32 argc, char *argv[], uint32 *p_space) {
```

**装一个 ELF 程序，返回它落到哪个地址空间**
- `path`：VFS 路径
- `argc`：argv 个数（0 或缺省 argv 时按 Linux 习惯填成 path）
- `argv`：argv 数组
- `p_space`：可为 0；成功时写回这次装载用的空间号
**返回**：ARKRTOS_ELF_OK / 负错误码（含 ARKRTOS_ELF_ERR_BUSY：空间全占着）
U-2（2026-11-17）：装载不再假设"只有一块窗口"。先挑一个没有
活实例的空间，把它装进窗口，把段写进那块帧，任务绑上去，然后把
内核任务原来的视图装回去。挑空间只看台账，不看地址：窗口 VA
在所有空间里都是同一个值，这个值本身不携带任何身份。

### `int32 arkrtos_elf_fork(void *p_svc_ctx, int32 *p_child_task, uint32 *p`

```c
int32 arkrtos_elf_fork(void *p_svc_ctx, int32 *p_child_task, uint32 *p_child_space) {
```

**复制当前任务（U-19：Linux clone/fork 的内核落地）**
实现顺序刻意是「先复制、后注册」：
1) 挑一个没有活实例、且不是父自己的空间；
2) 先释放它上一轮的帧（一定没有活实例），再整份复制父空间；
3) 把父这次系统调用的 trap 帧写成子空间里的同一 VA，a0 = 0；
4) 最后才在一个临界区里注册任务、把 TCB 的窗口/栈/入口全部
改成父的值、绑定空间。
顺序不能反：register 会把任务直接挂进就绪集，而「降权 / 窗口
写回 / 绑定」是它之后的事 —— 被 tick 切走就会得到一个跑在错帧
上的半配置任务（与装载器那条路径同一个坑，见 elf_start_task）。
前三步都可回滚（失败就把子空间还回池子）；第四步之后不再失败。

### `int32 arkrtos_elf_exec(const char *path, int32 argc, char *argv[]) {`

```c
int32 arkrtos_elf_exec(const char *path, int32 argc, char *argv[]) {
```

**兼容入口：装一个 ELF 程序，不关心它落到哪个空间**

### `int32 arkrtos_elf_exec_replace(const char *path, int32 argc, char *arg`

```c
int32 arkrtos_elf_exec_replace(const char *path, int32 argc, char *argv[], arkrtos_elf_info_t *p_info, unsigned long *p_tp) {
```

**把当前任务自己的镜像原地换掉（Linux execve 的内核落地）**
三件事决定了它不能走 arkrtos_elf_exec_space()：
1) 不起新任务——execve 换的是镜像，不是进程；任务号必须保持不变，
否则程序里的 getpid() 前后不同，fd 与定时器也没了归属；
2) 写的是**自己现在这块空间**：新镜像覆盖旧镜像的段。窗口 VA 在
所有空间里相同，所以这一步不需要换页表；内核自己也跑在别的
地址上（低 16 MB），覆盖 App 页不影响正在执行的代码；
3) 旧镜像的用户栈只被**帧之外**的部分复用：新镜像的段必须落在
stack_bottom 以下（elf_check_range 已经守住），而 trap 帧与
内核 C 栈在 stack_bottom..argv_start 之间，一个字节都不重叠。
这一步之后旧镜像的代码/数据已经不可用，所以调用者必须在调用**之前**
把 argv 字符串拷到内核里（见 arkrtos_linux_abi.c 的 lx_execve）。
失败时窗口里可能已经躺着半个新镜像（与 arkrtos_elf_exec_space 同一
口径）：旧镜像的段若已被覆盖就回不去了，所以调用者要按 Linux 的
语义返回错误——这条路径上「失败」意味着程序状态已不可信。实际
顺序是：先只校验（elf_check_image）再写，校验不过时窗口一个字节
未动，旧镜像继续跑。

## kernelsrc/src/arkrtos_eset.c

**Ark RTOS 32 位事件集实现**
位掩码等待（OR / AND）+ 累加式置位 + 可选清除。
创建 / 等待 / 置位 / 清除 / 查询 / 删除 六条路径，
以及任务下线收尸。语义与边界见 arkrtos_eset.h 文件头。

### `#if defined(ARKRTOS_ESET_HOST_TEST) arkrtos_eset_obj_t arkrtos_esets[A`

```c
#if defined(ARKRTOS_ESET_HOST_TEST) arkrtos_eset_obj_t arkrtos_esets[ARKRTOS_ESET_NUM];
```

对象表。宿主门禁（-DARKRTOS_ESET_HOST_TEST）把它导出，好让测试直接摆
两个等待槽，验证「谁先拿」「带 CLEAR 的位只交给一个等待者」这类判定；
默认构建里它就是本文件内部的 static。 */

## kernelsrc/src/arkrtos_exports.c

**内核符号导出表：可下载模块能按名字用到的那一份白名单**
表的每一行都是「模块里写 extern，装载器按名字找到这里」。
三条约束写死在这里：
1. **只导出内核自己的符号**，不导出示例程序里的东西

### `int32 arkrtos_mod_printf(const char *fmt, ...);`

```c
int32 arkrtos_mod_printf(const char *fmt, ...);
```

**模块用的 printf：%s %c %d %i %u %x %p %% ，其余按字面输出**
**返回**：写进控制台的字节数
> 说明：不支持宽度/精度/浮点——模块要精确格式就自己拼。
这里故意不做「尽量支持」：半支持的 printf 会让人以为 %5.2f 能用。

## kernelsrc/src/arkrtos_fs.c

**littlefs port + Ark RTOS file system facade - see arkrtos_fs.h**
Structure of this file:
1) the three board callbacks littlefs needs (read/prog/erase),
each one a translation from "block + offset" into a call on a

### `int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_`

```c
int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_dir) {
```

**Size and type of one entry (file or directory).**
- `path`：absolute path inside the mounted volume
- `size`：receives the size in bytes, 0 for a directory (may be NULL)
- `is_dir`：receives 1 for a directory, 0 for a file (may be NULL)
**返回**：0 on success, -1 on failure (nothing mounted, bad path, not found)

## kernelsrc/src/arkrtos_guard.c

**Watchdog gating and the per-slot heartbeat contract - see arkrtos_guard.h**

### `#define ARKRTOS_GUARD_AGE_NONE       (0xFFFFFFFFu)`

```c
#define ARKRTOS_GUARD_AGE_NONE       (0xFFFFFFFFu)
```

**"no evidence recorded yet" */**

## kernelsrc/src/arkrtos_installer.c

**Ark RTOS 内核内安装模块实现（常驻任务 + 命令触发一次性窗口）**
安装流程：
1) 打开镜像接收设备（默认 ttyS0）；
2) 在字节流中搜索镜像头魔数 "SVCA" 完成逐字节同步；

### `static int32 arkrtos_installer_pump(int32 dev) {`

```c
static int32 arkrtos_installer_pump(int32 dev) {
```

**推进接收状态机**
- `dev`：设备句柄
**返回**：0=已收满一个完整镜像头（内容在 arkrtos_installer_hdr），-1=本轮还没收完
先用 4 字节窗口匹配魔数，不匹配则逐字节左移继续找，
因此串口上混杂的杂散字节会被自动跳过。
达到空转上限时**保留已收字节**直接返回，由调用方让出 CPU 后继续。

## kernelsrc/src/arkrtos_linux_abi.c

**Linux 系统调用 ABI 兼容层（静态 ELF 用户任务）**
目标只有一个：让按 Linux 约定写成的**静态链接**程序在 Ark 的
PLV3 / U 模式任务里跑起来，而不是让 Ark 变成 Linux。
实现原则（与全仓其它模块一致）：

### `static int32 lx_copy_user_str(unsigned long uaddr, char *out, uint32 c`

```c
static int32 lx_copy_user_str(unsigned long uaddr, char *out, uint32 cap) {
```

**把用户地址上的 NUL 结尾串拷进内核缓冲**
**返回**：0 成功；-EFAULT 指针非法；-ENAMETOOLONG 在 cap 内没有 NUL

### `static int32 lx_clone(void *p_ctx) {`

```c
static int32 lx_clone(void *p_ctx) {
```

**clone(2)：只做 fork，不做线程**
只接受 flags == 0 或 SIGCHLD、child_stack == 0。CLONE_VM /
CLONE_THREAD / CLONE_VFORK / CLONE_SETTLS / CLONE_FILES 等一律
-EINVAL —— 本内核没有共享地址空间的线程，也没有线程组。
成功时父返回子 pid（任务号），子返回 0（那一格已经在子帧里改好）。

### `static int32 lx_wait4(void *p_ctx) {`

```c
static int32 lx_wait4(void *p_ctx) {
```

**wait4(2)：只支持 options == 0 / WNOHANG**
三条与 Linux 不同的边界（文档里写死）：
1) **不能阻塞**：子进程还活着且没给 WNOHANG 时回 -EAGAIN，
因为本内核没有「在系统调用里睡眠等事件」这条通路；
2) 状态只有「上报过退出码」这一种：exit_code_set == 0（故障 /
在别处被杀）时回 -EIO 并放掉记账 —— 编一个 0 会看起来像成功；
3) 槽位被复用后回 -ECHILD（靠注册代次认出来），不把新任务当儿子。
rusage 指针忽略、一个字节都不写。

### `static int32 lx_execve(void *p_ctx) {`

```c
static int32 lx_execve(void *p_ctx) {
```

**execve(path, argv, envp)：把**当前任务自己**的镜像换掉**
与 Linux 一致的部分：任务号不变、fd 保留、成功不返回、失败
返回负 errno 且旧镜像继续跑。
与 Linux 不一致的部分（都在文档里写死）：
- envp 不消费：这台机器没有环境变量这一层，新镜像拿到的是
和刚起跑时同一张空表；
- 地址空间不复制（没有 fork），所以没有「父子」这回事；
- 旧镜像的堆游标作废（新镜像的 brk/mmap 从零开始），但**已分配
的地址不做任何回收声明**——旧镜像已经没了，没有谁还在用它们。
参数纪律：path 与 argv 都在用户内存里，且马上要被新镜像覆盖，
所以全部先拷进内核再装载。

## kernelsrc/src/arkrtos_loader.c

**在池内找一段足够长、且不越过压实余量的已擦除空间**
- `size`：需要的字节数
- `skip_base`：需要避开的区间起点（0 = 不回避）
- `skip_size`：需要避开的区间长度

### `#include "arkrtos_loader.h" #include "arkrtos_ptable.h" #include "arkr`

```c
#include "arkrtos_loader.h" #include "arkrtos_ptable.h" #include "arkrtos_crash.h" #include "arkrtos_share.h" #include "arkrtos_layout_def.h"   /* strategy constants (reclaim mode) */ #include "arkrtos_layout.h"       /* effective mode + fixed slot table */
```

**Ark RTOS 镜像加载器实现（动态装载：安装 / 扫描 / 重定位 / 搬移 / 回收 / 启停）**
加载流程：
1) 校验镜像头（魔数 / 兼容签名 / 长度 / 入口 / 重定位表）；
2) 在池里找第一段足够长的**已擦除**空间（首适配），
因此镜像按实际长度紧邻排列，密度 = 实际占用之和；
3) 分配镜像 RAM 块（2 的幂、按块对齐），登记槽位记录；
4) 写入镜像头（state = UNCOMMITTED）；
5) 流式接收重定位表并落盘；
6) 流式接收负载，逐块按 ROM / RAM 增量就地打补丁后落盘；
7) CRC32 复核；成功后唯一的一个字写入：state = VALID（提交）；
8) 置 LOADED，返回槽位号。
启动时把入口地址注册为内核任务（优先级 / 栈 / 周期按镜像类型取分区配置）。
为什么分配不维护任何元数据：Flash 里「连续 0xFF 段」就是可用空间，
而活镜像必然含非 0xFF 字节（镜像头魔数），所以一次物理扫描同时
完成了「找空位」与「避开已装镜像」。重启后不需要恢复分配器状态，
也不会出现「元数据与实际内容不一致」这种只能靠猜的故障。
回收同样受物理规律约束：擦除粒度是整扇区，因此一个扇区里只要还有
活镜像就不能擦。arkrtos_loader_reclaim() 先把该扇区的活镜像搬到池内
更低处的已擦除空位（应用重定位，走 UNCOMMITTED -> VALID 事务），
再擦扇区。搬不动的（正在运行 / 无空位）跳过，不影响其它扇区。
> 说明：本模块只能由内核特权态调用（经 SVC 0x18 分发）。

### `static uint32 arkrtos_loader_mov_imm16(uint16 h1, uint16 h2) {`

```c
static uint32 arkrtos_loader_mov_imm16(uint16 h1, uint16 h2) {
```

**从 MOVW/MOVT 的一对半字里取出 imm16**
- `h1`：前缀半字（11110 i 10 0100 imm4）
- `h2`：第二个半字（0 imm3 Rd imm8）

### `static void arkrtos_loader_mov_set_imm16(uint16 *p_h1, uint16 *p_h2, u`

```c
static void arkrtos_loader_mov_set_imm16(uint16 *p_h1, uint16 *p_h2, uint32 imm16) {
```

**把 imm16 写回 MOVW/MOVT 的一对半字（Rd 与指令种类保持不变）**

### `static void arkrtos_loader_reloc_movw(uint8 *p, uint32 delta) {`

```c
static void arkrtos_loader_reloc_movw(uint8 *p, uint32 delta) {
```

**给一对相邻的 MOVW+MOVT 立即数打补丁**
- `p`：指向 MOVW 指令（半字对齐）
- `delta`：要叠加到这条 32 位地址上的增量
编译器把 32 位绝对地址拆成「MOVW 低 16 位 + MOVT 高 16 位」两条指令时，
链接期填进去的是指令内的立即数，字节变化不是「整字加增量」，只能解码
成 32 位值、加上增量、再重新编码。低 16 位会被进位的连带影响，所以两条
必须一起算，不能拆成两个独立表项。

### `uint32 arkrtos_loader_reloc_apply(uint8 *buf, uint32 buf_off, uint32 l`

```c
uint32 arkrtos_loader_reloc_apply(uint8 *buf, uint32 buf_off, uint32 len, const arkrtos_app_header_t *p_hdr, const uint32 *p_rel, uint32 *p_idx, uint32 delta_rom, uint32 delta_ram) {
```

**给一段负载打重定位补丁，并给出本块可以安全落盘的末尾**
- `buf`：装载缓冲（对应镜像内偏移 buf_off 起的连续 len 字节）
- `buf_off`：该缓冲在镜像内的起始偏移
- `len`：缓冲长度
- `p_hdr`：镜像头（提供表项数与两条标称基址）
- `p_rel`：重定位表表体地址（流式安装指向刚落盘的 Flash 表，
池内搬移指向旧副本的表，RAM 缓冲路径指向缓冲里的表）
- `idx`：重定位表前进指针（跨块调用时必须复用同一个变量）
- `delta_rom`：本次要给 ROM 类表项叠加的增量
- `delta_ram`：本次要给 RAM 类表项叠加的增量
**返回**：本块可以落盘的末尾偏移（镜像内）。正常等于 buf_off + len；若块尾
恰好切开一条指令类表项，则等于该表项的起始偏移，调用者必须把这段
之后的字节留到下一块开头一起处理。
表项按偏移升序，只要记住走到第几条，就能边收边改，不必把整表读进
RAM。数据类表项是整字、4 字节对齐，永远不会跨块；指令类表项
（MOVW/MOVT）长 8 字节且只保证半字对齐，「表项不跨块」这条对它们
不成立，所以要把可提交长度交回调用者。

### `int32 arkrtos_loader_check_install(const arkrtos_app_header_t *p_hdr) `

```c
int32 arkrtos_loader_check_install(const arkrtos_app_header_t *p_hdr) {
```

**安装前的版本把关：同一 image_id 的已装副本不允许被「不更高」的版本覆盖**
- `p_hdr`：待安装镜像的镜像头（调用方已完成魔数同步与基本校验）
**返回**：0=可以安装；负值为 ARKRTOS_LOADER_ERR_x
只按「同一 image_id」比对：image_id 是负载身份（标称形态的 crc32），
同一个应用的两次打包只有版本号不同、image_id 相同。三态判定：
- 新版本号 > 已装最高版本号 -> 放行（升级）；
- 版本号相同、且 image_size 与 crc32 也相同 -> ERR_DUP
（同一份镜像，装第二遍没有任何意义，还白占一份池空间）；
- 其余（降级回灌、同版本重建）-> ERR_VERSION。
只统计 state 为 LOADED / RUNNING 的副本：被崩溃终局策略禁用
（INVALID）的副本不算「已装」，否则一次崩溃风暴会让同一个镜像
再也装不回来——重装同一版本正是恢复手段。
> 说明：这里读的是 Flash 上的镜像头，只用到 version / image_id / image_size /
crc32 四个「标称形态」字段，它们不参与重定位，落盘后仍然可信。
不做 CRC 复核：那对不上（见 arkrtos_loader_accept 的说明）。

## kernelsrc/src/arkrtos_log.c

### `#include "arkrtos_config.h" #include "arkrtos_log.h" #include "arkrtos`

```c
#include "arkrtos_config.h" #include "arkrtos_log.h" #include "arkrtos_dev.h" #include "arkrtos_hal.h" #include "arkrtos_task.h"       /* arkrtos_kernel_get_time(): 日志时间戳 */ #if ARKRTOS_USE_LOG
```

**Ark RTOS 内核日志：分级输出与运行期过滤的实现**
输出目标是内核控制台串口（SHELL_DEV_NAME）。这里不复用 shell 的
平台层句柄，而是自己惰性打开一次：日志允许在 shell 之前使用
（比如启动横幅），也允许在 SHELL_ENABLE=0 的裁剪配置里单独存在。
两次 open 拿到的是同一个设备对象，写接口串行调用，互不干扰。
级别过滤分两道：
- 编译期：ARKRTOS_LOG_LEVEL 决定哪些宏展开成空语句（见 arkrtos_log.h）；
- 运行期：本文件的 g_log_level，可由 shell 的 `log <level>` 或
SVC 0x19 之外的内部调用改变，全局生效（内核 + 所有 App/驱动）。
行内不做重入保护：日志只能在任务上下文调用，这是设备框架的
写接口本身的前提（见 arkrtos_dev.h）。
> 说明：本模块由内核特权态调用；用户态经 SVC 0x19 进入 arkrtos_log_svc()。

## kernelsrc/src/arkrtos_mini.c

**Ark RTOS 小程序（MiniApp）：从文件系统按需 load 进 RAM 执行**
装载全程只有六步，每一步都在处理「RAM 是借来的、随时要还」这件事：
1. 读 256 字节镜像头 → 只校字段；
2. 读重定位表（<= 4KB）→ 再校一次表体；

### `typedef struct {`

```c
typedef struct {
```

**一个在跑（或在装载/拆除）的小程序 */**

## kernelsrc/src/arkrtos_modld.c

**ET_REL 目标文件在板上现场链接（节放置 + GOT + SOP 栈机重定位）**
这条路径与 arkrtos_elf.c 的区别不在「读文件」，而在**谁决定地址**：
ET_EXEC 的地址是链接期写死的，ET_REL 的地址由装载器现在给，
于是所有 PC 相对与 GOT 相对的重定位都得在这里现算。

### `static int32 mod_sym(const mod_link_t *c, uint32 idx, const char **p_n`

```c
static int32 mod_sym(const mod_link_t *c, uint32 idx, const char **p_name, uint32 *p_shndx, unsigned long *p_value) {
```

**取一个符号的 (名字, 节下标, 值)；符号表坏了返回 -1 */**

### `static int32 mod_sym_value(mod_link_t *c, uint32 idx, unsigned long *p`

```c
static int32 mod_sym_value(mod_link_t *c, uint32 idx, unsigned long *p_out) {
```

**求一个符号的最终地址**
三种情况分开算，不能合并：
- 空名字符号 0：值是 0（汇编器用它发 PUSH_ABSOLUTE 常量）；
- 定义在本模块里的符号：所在节的装载地址 + st_value；
- 未定义符号：查内核导出表；_GLOBAL_OFFSET_TABLE_ 指向本模块的 GOT 基址。

### `static int32 mod_got_alloc(mod_link_t *c) {`

```c
static int32 mod_got_alloc(mod_link_t *c) {
```

**按「首次被 GPREL 引用的顺序」给外部数据符号分 GOT 槽**
只有**未定义**符号需要槽：模块里自己定义的数据，编译器发的是
pcalau12i 直接算地址；外部数据才走 `ld.d $r12,$r12,slot`。
给本地符号也开槽也能跑，但会多一层间接，且和真实链接器不同。

### `static int32 mod_fits(long v, uint32 bits) {`

```c
static int32 mod_fits(long v, uint32 bits) {
```

**有符号立即数是否装得下 bits 位 */**

### `static int32 mod_one_rela(mod_link_t *c, uint32 target, uint32 off, ui`

```c
static int32 mod_one_rela(mod_link_t *c, uint32 target, uint32 off, uint32 type, uint32 symidx, long addend, unsigned long *st, uint32 *p_sp) {
```

**一条 rela 记录：在 SOP 栈机上求值后写回指令字 */**

## kernelsrc/src/arkrtos_mpu.c

**Ark RTOS memory protection manager (the kernel's only MPU entry point)**
Kernel code calls this file; MPU register access lives entirely in the
port layer (arkrtos_port_mpu_*). Everything here is architecture
independent: this file decides *which* windows a task may touch, the

### `static uint8 arkrtos_mpu_code_in_ram(const arkrtos_task_t *p_task) {`

```c
static uint8 arkrtos_mpu_code_in_ram(const arkrtos_task_t *p_task) {
```

Is this task's code window in RAM instead of the flash image pool?
True for MiniApps only (they are loaded into RAM and run there, see
arkrtos_mini.c); every App / Driver / kernel task answers no. The test is on
the recorded window, not on a flag: a window in the image RAM pool can only
have been put there by the MiniApp loader. */

## kernelsrc/src/arkrtos_paging.c

**Ark RTOS isolation backend #2: page table (MMU)**
This file implements exactly the same five entry points as
arkrtos_mpu.c (see arkrtos_mpu.h): the kernel keeps calling
arkrtos_mpu_module_init/_build_task/_set_app/_set_idle/_reset, and

### `static uint8 arkrtos_paging_code_in_ram(const arkrtos_task_t *p_task) `

```c
static uint8 arkrtos_paging_code_in_ram(const arkrtos_task_t *p_task) {
```

Is this task's code window in the RAM pool (a MiniApp block) rather than in
the flash image pool? Same test as the window backend uses, and for the same
reason: it is the recorded window that says so, not a flag that could lie. */

### `uint32 arkrtos_paging_space_count(void) {`

```c
uint32 arkrtos_paging_space_count(void) {
```

**How many address spaces the board provides**
1 means "only the frame this port has always used": nothing about
the build changes until the board asks for more.

### `uint32 arkrtos_paging_space_frame(uint32 space) {`

```c
uint32 arkrtos_paging_space_frame(uint32 space) {
```

**Physical frame base behind an address space**
**返回**：frame base, or 0 when the space does not exist (never a guess)

### `unsigned long arkrtos_paging_window_va(void) {`

```c
unsigned long arkrtos_paging_window_va(void) {
```

**Virtual base of the App window (the same VA for every space)**
窗口的 VA 是 port 的属性，物理侧才按空间换。页级窗口
（ARKRTOS_USE_PAGING_PAGES=1）下帧来自帧池，帧基址不再是 VA，
所以按地址摸窗口的代码必须问这一条。

### `uint32 arkrtos_paging_space_current(void) {`

```c
uint32 arkrtos_paging_space_current(void) {
```

**Which space is currently installed in the App window**

### `int32 arkrtos_paging_space_activate(uint32 space) {`

```c
int32 arkrtos_paging_space_activate(uint32 space) {
```

**Install an address space now**
- `space`：0 .. arkrtos_paging_space_count()-1
**返回**：0 = installed or already installed; -1 = bad space / no TLB path
The scheduler calls this indirectly on every switch to an App
(arkrtos_mpu_set_app -> the task's binding). A kernel task calls it
directly when it is about to write into a frame that is not the one
currently installed - the ELF loader will do exactly that once
second-instance loading exists (debt U-2).

### `int32 arkrtos_paging_bind_task(arkrtos_task_t *p_task, uint32 space) {`

```c
int32 arkrtos_paging_bind_task(arkrtos_task_t *p_task, uint32 space) {
```

**Bind a task to an address space**
**返回**：0 = bound; -1 = bad task pointer / bad space
Binding only stores a number; the switch happens lazily, at the next
schedule to that task. That is deliberate: binding inside the loader
must not change the translation of the frame it is still writing.

### `uint32 arkrtos_paging_space_of(arkrtos_task_t *p_task) {`

```c
uint32 arkrtos_paging_space_of(arkrtos_task_t *p_task) {
```

**Space a task is bound to (0 for unbound / unknown)**

### `#if (ARKRTOS_USE_LINUX_FORK == 1)`

```c
#if (ARKRTOS_USE_LINUX_FORK == 1)
```

**把一个地址空间的帧还给帧池（空间回收，U-3 遗留）**
- `space`：0 .. arkrtos_paging_space_count()-1
**返回**：0 = 已归还（本来就没拿过也算 0，幂等）；-1 = 空间号越界；
-2 = 这一档没有帧池（PS=20 静态窗口）；-3 = 调用者正站在窗口里，
本轮一个字节都不动；-4 = 帧池拒绝（原样透出它的 -1/-3）
调用点只有装载器：它挑中的空间一定没有活实例（台账判据），所以
撤窗口条目不会打断任何正在跑的程序。撤条目的理由见 port 侧注释
—— 帧回到池子里而旧条目还指着它，是「两个空间共享同一块物理页」
这种只在很久以后才显形的错。归还之后空间仍然存在，下一次
activate() 重新要帧，于是反复 exec 不再把池子一格一格吃掉。

### `int32 arkrtos_paging_space_clone(uint32 src, uint32 dst) {`

```c
int32 arkrtos_paging_space_clone(uint32 src, uint32 dst) {
```

**Copy one space's window content into another (U-19, fork)**
**返回**：0 = copied; -1 = bad space / same space; -2 = dst already has
frames; -3 = frame pool refused (everything taken was rolled back)
Thin forward. The caller (the ELF loader) has already checked that
dst has no live instance; the port re-checks that dst is empty so a
bug there cannot overwrite another process's memory silently.

### `int32 arkrtos_paging_frame_alloc(uint32 count, uint32 *p_base) {`

```c
int32 arkrtos_paging_frame_alloc(uint32 count, uint32 *p_base) {
```

**Allocate a run of contiguous 4 KB page frames from the board pool */**

### `int32 arkrtos_paging_frame_free(uint32 base, uint32 count) {`

```c
int32 arkrtos_paging_frame_free(uint32 base, uint32 count) {
```

**Return a run of contiguous 4 KB page frames (all-or-nothing) */**

### `uint32 arkrtos_paging_frame_total(void) {`

```c
uint32 arkrtos_paging_frame_total(void) {
```

**Total frames in the board pool (0 when there is none) */**

### `uint32 arkrtos_paging_frame_used(void) {`

```c
uint32 arkrtos_paging_frame_used(void) {
```

**Frames currently allocated from the board pool */**

### `uint32 arkrtos_paging_pair_total(void) {`

```c
uint32 arkrtos_paging_pair_total(void) {
```

**Pair-level window readout: forwards to the port's ledger**
This layer adds no arithmetic on purpose. "Which pairs are
mapped" is a property of the port's TLB/ledger, and a second
copy of that truth here is exactly how the two drift apart. */

### `uint32 arkrtos_paging_swap_stat(uint32 which) {`

```c
uint32 arkrtos_paging_swap_stat(uint32 which) {
```

**Page replacement readout (U-16): forwards to the port's ledger */**

### `int32 arkrtos_paging_map_span(uint32 va, uint32 pa, uint32 size, arkrt`

```c
int32 arkrtos_paging_map_span(uint32 va, uint32 pa, uint32 size, arkrtos_mpu_mem_t mem) {
```

**Register a run-time dynamic mapping, page by page**
- `va`：virtual base (4 KB aligned)
- `pa`：physical base (4 KB aligned)
- `size`：bytes (non-zero multiple of 4 KB)
- `mem`：RAM / ROM / PERIPH_RO / PERIPH_RW
**返回**：0 = all pages registered and auditable; -1 = refused
The port owns the TLB, so it also owns the decision of what may be
added at run time (static-overlap refusal, STLB way budget). This
layer only does the arithmetic: alignment, the page count cap, and
the page-by-page loop so a caller never has to remember that one
TLB entry covers two pages.
A failure part-way leaves the earlier pages in place. That is not a
silent partial success: the return code is -1 and
arkrtos_paging_map_pages() shows how many pages did land. The honest
reason is that there is no unmap path on this port yet (debt U-15),
and pretending to roll back would need one.

### `uint32 arkrtos_paging_map_pages(void) {`

```c
uint32 arkrtos_paging_map_pages(void) {
```

**How many pages have been registered through map_span()**
**返回**：cumulative page count (0 on any build without the paging backend)

### `int32 arkrtos_paging_unmap_span(uint32 va, uint32 size) {`

```c
int32 arkrtos_paging_unmap_span(uint32 va, uint32 size) {
```

**Undo pages registered through map_span() (U-15)**
- `va`：Base of the span, 4 KB aligned
- `size`：Bytes, non-zero multiple of 4 KB
**返回**：0 = every page is gone; <0 = the port's own reason for the page that
failed (-1 alignment, -2 static span, -4 not registered)
The loop stops at the first refusal on purpose: a caller that asks
to unmap a span it never mapped should hear about it instead of
getting a "done" that quietly skipped half the pages. Pages already
unmapped stay unmapped (same honest partial-progress rule as
map_span(), and for the same reason: no rollback without a re-map).
s_pg_map_pages counts pages that ARE mapped, so it drops by exactly
the number of pages this call took out.

### `uint32 arkrtos_paging_dyn_used(void) {`

```c
uint32 arkrtos_paging_dyn_used(void) {
```

**How many dynamic mapping slots are in use right now**
**返回**：used slot count (0 on any build without the paging backend)
This is the readout that was missing while maps could only be
added: a driver that registered eight 4 KB pages had no way to see
how close it was to the table being full.

### `static uint8 arkrtos_paging_span_has(unsigned long base, uint32 size, `

```c
static uint8 arkrtos_paging_span_has(unsigned long base, uint32 size, unsigned long uva, uint32 len) {
```

**整段是否落在给定窗口里**
- `base/size`：窗口（base 不为 0 才算有效）
- `uva/len`：待判的一段
**返回**：1 = 整段在窗口内；0 = 不在 / 窗口记录坏了
判据是「整段」不是「首地址」：只查首字节时，一个刚好跨出窗口
末尾的指针会让拷贝循环走进未映射页，在内核态变成 refill 致命
—— 那正是这条接口要消灭的「内核陪着 fault」。

### `int32 arkrtos_paging_user_copy(unsigned long rom_start, uint32 rom_siz`

```c
int32 arkrtos_paging_user_copy(unsigned long rom_start, uint32 rom_size, unsigned long ram_start, uint32 ram_size, uint32 space, uint32 installed, void *kern, const void *user, uint32 len, uint8 write) {
```

**受控访问用户地址的本体（读写两个方向共用一套判据）**
**返回**：0 = 已拷贝；-1 参数非法；-2 没有用户窗口；-3 整段不在窗口内；
-4 当前装着的不是调用者自己的地址空间
拷贝走的是「当前装着的那份翻译」，所以 -4 不是多余的保守：
空间不对时读到的是另一个 App 的帧，那是**静默错答案**，比报错
糟得多。判据全部在第一次 load/store 之前完成，因此拒绝一次非法
指针不会在内核态产生任何异常。

### `int32 arkrtos_copy_from_user_task(arkrtos_task_t *p_task, void *dst, c`

```c
int32 arkrtos_copy_from_user_task(arkrtos_task_t *p_task, void *dst, const void *usrc, uint32 len) {
```

**从指定任务的用户缓冲区拷到内核缓冲区（内核内部）**
**返回**：0 = 已拷贝；-1/-2/-3/-4 同上
内核任务（is_priv != 0）一律 -2：它没有用户窗口，拿一个「用户
指针」进来是契约错误，不应该被当成合法调用。

### `int32 arkrtos_copy_to_user_task(arkrtos_task_t *p_task, void *udst, co`

```c
int32 arkrtos_copy_to_user_task(arkrtos_task_t *p_task, void *udst, const void *src, uint32 len) {
```

**从内核缓冲区拷到指定任务的用户缓冲区（内核内部）**

### `int32 arkrtos_copy_from_user(void *dst, const void *usrc, uint32 len) `

```c
int32 arkrtos_copy_from_user(void *dst, const void *usrc, uint32 len) {
```

**从当前任务的用户缓冲区拷出**

### `int32 arkrtos_copy_to_user(void *udst, const void *src, uint32 len) {`

```c
int32 arkrtos_copy_to_user(void *udst, const void *src, uint32 len) {
```

**往当前任务的用户缓冲区拷入**

### `int32 arkrtos_paging_stats(uint32 *p_checked, uint32 *p_bad, uint32 *p`

```c
int32 arkrtos_paging_stats(uint32 *p_checked, uint32 *p_bad, uint32 *p_bad_base) {
```

**Read the audit counters (kernel/shell only).**
- `p_checked`：Receives the number of spans audited, may be 0.
- `p_bad`：Receives the number of spans no mapping covers, may be 0.
- `p_bad_base`：Receives the base of the first bad span, may be 0.
**返回**：0 when the port brought the page tables up, -1 when it did not.

## kernelsrc/src/arkrtos_smp.c

**SMP 骨架实现：每核状态、次核启动、核间中断（默认关）**
这个文件只做四件事，不多做一件：
1) 维护每核状态表与在线掩码；
2) 引导次核（CPU0 直接上线，其余交给 port 层）；

### `static void smp_mask_set(uint32 cpu) {`

```c
static void smp_mask_set(uint32 cpu) {
```

**原子置位在线掩码的某一位**
> 说明：用 CAS 重试而不是读-改-写：两个次核同时上线时，读-改-写会丢掉一个。

## kernelsrc/src/arkrtos_task.c

**Ark RTOS 任务调度与系统调用分发实现**
实现任务调度器、上下文切换的 C 侧逻辑、SVC 系统调用分发，以及任务控制接口。
中断入口（SysTick_Handler/HardFault_Handler）在 board/ 中实现，
它们最终调用本文件的 arkrtos_kernel_tick_handler / arkrtos_hardfault_handler。

### `int32 arkrtos_stack_guard_check(int32 slot) {`

```c
int32 arkrtos_stack_guard_check(int32 slot) {
```

**校验一个任务的栈底保护字（0 基槽位）**
**返回**：1 = 保护字已被踩，0 = 正常或该任务未布防
判据是「栈底那一个字还是不是 ARKRTOS_STACK_END_FLAG_VAL」。
和「保存的 PSP 低于栈底」不是同一件事：陷阱帧、越界指针、
DMA 写穿都能让栈底被改写而保存下来的 PSP 仍在上方。
报一次就置 reported，避免每个节拍都往故障环里塞同一条记录；
布防位保持不清，这样切换路径仍会据此把任务摘除。

### `static int32 arkrtos_sched_lock_blocks(void) {`

```c
static int32 arkrtos_sched_lock_blocks(void) {
```

**检查调度器锁是否被持有（阻塞类接口的编程错误防护）**
**返回**：1=已锁定（调用方应忽略本次阻塞请求），0=未锁定
在调度器锁定期间调用阻塞接口会破坏任务状态一致性，
此处忽略请求并记录一条故障记录，便于定位问题代码。

### `void arkrtos_sched_lock_internal(void) {`

```c
void arkrtos_sched_lock_internal(void) {
```

**获取调度器锁（用户任务经 SVC 调用）**
累加嵌套计数后禁止任务切换，但不关闭中断。
临界区内不得调用阻塞接口（会被忽略并记录故障）。

### `uint32 arkrtos_sched_unlock_internal(void) {`

```c
uint32 arkrtos_sched_unlock_internal(void) {
```

**释放调度器锁**
**返回**：剩余嵌套层数（0 表示已完全解锁）
计数归零时主动触发一次任务切换，补偿锁定期间被推迟的调度。

### `int32 arkrtos_sched_lock_count_internal(void) {`

```c
int32 arkrtos_sched_lock_count_internal(void) {
```

**查询调度器锁嵌套层数**
**返回**：当前嵌套层数（0 表示未锁定）

## kernelsrc/src/arkrtos_tickless.c

**Ark RTOS tickless idle 实现**
设计取舍见 arkrtos_tickless.h。这里只补四件实现上的事：
1) 相位（phase）必须由软件记住，不能只看硬件。
被别的中断提前叫醒时，走过的计数往往不是整拍，余数如果丢掉，

### `static uint32 arkrtos_tl_settle(void) {`

```c
static uint32 arkrtos_tl_settle(void) {
```

**结算这一次中断：算出走过几个整拍，并把硬件摆回正确相位**
**返回**：整拍数（可以为 0：这次中断本身就是那一拍，由调用方补 1 次节拍体）

## kernelsrc/src/arkrtos_vfs_lfs.c

**把 Ark RTOS 的老门面 arkrtos_fs（littlefs）接成 ark_vfs 的一个文件系统**
目的只有一个：让 /mnt/<卷> 这条 Linux 风格路径真的能读写文件，
而不是再造一个 littlefs 绑定。这里不 include 任何 lfs 头，
只用 arkrtos_fs.h 已经公开的那几个入口。

### `static int32_t p_abs(const char *rel, char *out, uint32_t cap) {`

```c
static int32_t p_abs(const char *rel, char *out, uint32_t cap) {
```

**rel（相对挂载点、无前导 '/'）-> 卷内绝对路径 */**

### `static int32_t p_io(const char *what, const char *path) {`

```c
static int32_t p_io(const char *what, const char *path) {
```

**出错时把原始错误码落到日志，再返回统一的 ARK_E_IO */**

### `static int32_t p_open_create_err(const char *path) {`

```c
static int32_t p_open_create_err(const char *path) {
```

**新建/截断失败时把原因说准：父目录不存在不是 backend error**
littlefs 在 LFS_O_CREAT 时，若父目录不存在就回 LFS_ERR_NOENT。
这一条曾经统一走 p_io()，于是在 /mnt/nor/nodir/x.txt 上打印的是
"FAILED (-11: backend error)"——把一个「父目录不存在」报成了
「驱动出错」。而父目录存在时同一个调用是成功的，所以这不是
「新建文件只能落卷根」，是错误码把原因吃掉了。 */
