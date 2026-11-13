# Ark RTOS API 参考

> 由 `tools/gen_api_doc.py` 自动生成，请勿手工修改。
> 安装 [Doxygen](https://www.doxygen.nl/) 后重新运行脚本，可得到带交叉引用与调用关系的 HTML 版本。

## 目录

- [kernelsrc/include/arkrtos.h](#kernelsrcincludearkrtosh)（98 项）
- [kernelsrc/include/arkrtos_app_image.h](#kernelsrcincludearkrtos_app_imageh)（38 项）
- [kernelsrc/include/arkrtos_apppack.h](#kernelsrcincludearkrtos_apppackh)（9 项）
- [kernelsrc/include/arkrtos_apprun.h](#kernelsrcincludearkrtos_apprunh)（6 项）
- [kernelsrc/include/arkrtos_audit.h](#kernelsrcincludearkrtos_audith)（7 项）
- [kernelsrc/include/arkrtos_blk.h](#kernelsrcincludearkrtos_blkh)（13 项）
- [kernelsrc/include/arkrtos_cfg.h](#kernelsrcincludearkrtos_cfgh)（1 项）
- [kernelsrc/include/arkrtos_crash.h](#kernelsrcincludearkrtos_crashh)（22 项）
- [kernelsrc/include/arkrtos_dt.h](#kernelsrcincludearkrtos_dth)（11 项）
- [kernelsrc/include/arkrtos_flashapp.h](#kernelsrcincludearkrtos_flashapph)（7 项）
- [kernelsrc/include/arkrtos_fs.h](#kernelsrcincludearkrtos_fsh)（29 项）
- [kernelsrc/include/arkrtos_guard.h](#kernelsrcincludearkrtos_guardh)（11 项）
- [kernelsrc/include/arkrtos_hal.h](#kernelsrcincludearkrtos_halh)（30 项）
- [kernelsrc/include/arkrtos_hwsafe.h](#kernelsrcincludearkrtos_hwsafeh)（4 项）
- [kernelsrc/include/arkrtos_init.h](#kernelsrcincludearkrtos_inith)（1 项）
- [kernelsrc/include/arkrtos_installer.h](#kernelsrcincludearkrtos_installerh)（3 项）
- [kernelsrc/include/arkrtos_layout.h](#kernelsrcincludearkrtos_layouth)（11 项）
- [kernelsrc/include/arkrtos_layout_def.h](#kernelsrcincludearkrtos_layout_defh)（2 项）
- [kernelsrc/include/arkrtos_loader.h](#kernelsrcincludearkrtos_loaderh)（31 项）
- [kernelsrc/include/arkrtos_log.h](#kernelsrcincludearkrtos_logh)（16 项）
- [kernelsrc/include/arkrtos_mempool.h](#kernelsrcincludearkrtos_mempoolh)（9 项）
- [kernelsrc/include/arkrtos_mini.h](#kernelsrcincludearkrtos_minih)（20 项）
- [kernelsrc/include/arkrtos_net.h](#kernelsrcincludearkrtos_neth)（6 项）
- [kernelsrc/include/arkrtos_ptable.h](#kernelsrcincludearkrtos_ptableh)（23 项）
- [kernelsrc/include/arkrtos_rt.h](#kernelsrcincludearkrtos_rth)（13 项）
- [kernelsrc/include/arkrtos_sec.h](#kernelsrcincludearkrtos_sech)（7 项）
- [kernelsrc/include/arkrtos_share.h](#kernelsrcincludearkrtos_shareh)（7 项）
- [kernelsrc/include/arkrtos_shell.h](#kernelsrcincludearkrtos_shellh)（9 项）
- [kernelsrc/include/arkrtos_sign.h](#kernelsrcincludearkrtos_signh)（5 项）
- [kernelsrc/include/arkrtos_softbus.h](#kernelsrcincludearkrtos_softbush)（5 项）
- [kernelsrc/include/arkrtos_spin.h](#kernelsrcincludearkrtos_spinh)（10 项）
- [kernelsrc/include/arkrtos_svc_call.h](#kernelsrcincludearkrtos_svc_callh)（10 项）
- [kernelsrc/include/arkrtos_task.h](#kernelsrcincludearkrtos_taskh)（3 项）
- [kernelsrc/include/arkrtos_tickless.h](#kernelsrcincludearkrtos_ticklessh)（5 项）
- [kernelsrc/include/arkrtos_ulog.h](#kernelsrcincludearkrtos_ulogh)（5 项）
- [kernelsrc/include/arkrtos_ushell.h](#kernelsrcincludearkrtos_ushellh)（4 项）
- [kernelsrc/include/arkrtos_utest.h](#kernelsrcincludearkrtos_utesth)（22 项）
- [kernelsrc/include/arkrtos_version.h](#kernelsrcincludearkrtos_versionh)（2 项）
- [kernelsrc/include/arkrtos_vfs.h](#kernelsrcincludearkrtos_vfsh)（43 项）
- [kernelsrc/port/arm/cortex-m3/arkrtos_port.c](#kernelsrcportarmcortex-m3arkrtos_portc)（9 项）
- [kernelsrc/port/arm/cortex-m4/arkrtos_port.c](#kernelsrcportarmcortex-m4arkrtos_portc)（9 项）
- [kernelsrc/port/arm/cortex-m7/arkrtos_port.c](#kernelsrcportarmcortex-m7arkrtos_portc)（9 项）
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
- [kernelsrc/sdk/posix/arkrtos_posix_types.h](#kernelsrcsdkposixarkrtos_posix_typesh)（1 项）
- [kernelsrc/sdk/posix/arpa/inet.h](#kernelsrcsdkposixarpaineth)（4 项）
- [kernelsrc/sdk/posix/errno.h](#kernelsrcsdkposixerrnoh)（1 项）
- [kernelsrc/sdk/posix/pthread.h](#kernelsrcsdkposixpthreadh)（1 项）
- [kernelsrc/sdk/posix/semaphore.h](#kernelsrcsdkposixsemaphoreh)（1 项）
- [kernelsrc/sdk/posix/sys/stat.h](#kernelsrcsdkposixsysstath)（3 项）
- [kernelsrc/sdk/posix/unistd.h](#kernelsrcsdkposixunistdh)（13 项）
- [kernelsrc/src/arkrtos_apppack.c](#kernelsrcsrcarkrtos_apppackc)（1 项）
- [kernelsrc/src/arkrtos_cfg.c](#kernelsrcsrcarkrtos_cfgc)（1 项）
- [kernelsrc/src/arkrtos_fs.c](#kernelsrcsrcarkrtos_fsc)（1 项）
- [kernelsrc/src/arkrtos_guard.c](#kernelsrcsrcarkrtos_guardc)（1 项）
- [kernelsrc/src/arkrtos_installer.c](#kernelsrcsrcarkrtos_installerc)（1 项）
- [kernelsrc/src/arkrtos_loader.c](#kernelsrcsrcarkrtos_loaderc)（5 项）
- [kernelsrc/src/arkrtos_log.c](#kernelsrcsrcarkrtos_logc)（1 项）
- [kernelsrc/src/arkrtos_mini.c](#kernelsrcsrcarkrtos_minic)（1 项）
- [kernelsrc/src/arkrtos_mpu.c](#kernelsrcsrcarkrtos_mpuc)（1 项）
- [kernelsrc/src/arkrtos_task.c](#kernelsrcsrcarkrtos_taskc)（4 项）
- [kernelsrc/src/arkrtos_tickless.c](#kernelsrcsrcarkrtos_ticklessc)（1 项）
- [kernelsrc/src/arkrtos_vfs_lfs.c](#kernelsrcsrcarkrtos_vfs_lfsc)（3 项）

---

## kernelsrc/include/arkrtos.h

**Ark RTOS 应用 API 头文件（用户态唯一需要包含的头文件）**
应用程序（用户态）包含本文件即可使用全部操作系统接口。
所有调用最终通过 SVC 指令陷入内核态执行，对用户透明，
用法与普通函数一致。

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

### `int32  arkrtos_event_create(char *name);`

```c
int32  arkrtos_event_create(char *name);
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

### `int32  arkrtos_sem_create(char *name, int32 init_count);`

```c
int32  arkrtos_sem_create(char *name, int32 init_count);
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

### `int32  arkrtos_mutex_create(char *name);`

```c
int32  arkrtos_mutex_create(char *name);
```

**创建互斥锁（带优先级继承，防止优先级反转）**
- `name`：互斥锁名
**返回**：互斥锁句柄，失败返回负值

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

### `int32  arkrtos_cond_create(char *name);`

```c
int32  arkrtos_cond_create(char *name);
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

### `int32  arkrtos_dev_open(char *name, uint32 param);`

```c
int32  arkrtos_dev_open(char *name, uint32 param);
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

### `int32  arkrtos_mq_create(char *name);`

```c
int32  arkrtos_mq_create(char *name);
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

### `int32  arkrtos_sbuf_create(char *name);`

```c
int32  arkrtos_sbuf_create(char *name);
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

### `int32  arkrtos_mpool_create(char *name, void *pool, uint32 block_size,`

```c
int32  arkrtos_mpool_create(char *name, void *pool, uint32 block_size, uint32 block_count);
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

### `int32  arkrtos_timer_create(char *name);`

```c
int32  arkrtos_timer_create(char *name);
```

**创建软定时器**
- `name`：定时器名
**返回**：定时器句柄，失败返回负值

### `int32  arkrtos_timer_start(int32 handle, uint32 period_ms, uint32 mode`

```c
int32  arkrtos_timer_start(int32 handle, uint32 period_ms, uint32 mode,
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
int32  arkrtos_thread_create(void (*entry)(void), void *stack, uint32 stack_size,
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
int32  arkrtos_file_list_names(const char *dir, char *out, uint32 out_size,
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
int32  arkrtos_path_readdir(int32 handle, char *name, uint32 name_size,
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

### `typedef char arkrtos_app_off_crc32_check[ (((uint32)&(((arkrtos_app_he`

```c
typedef char arkrtos_app_off_crc32_check[ (((uint32)&(((arkrtos_app_header_t *)0)->crc32)) == ARKRTOS_APP_OFF_CRC32) ? 1 : -1];
```

**关键字段偏移静态校验**
Loader 计算 CRC 时按「偏移切片」而不是整体结构体来累加 CRC
（crc32 与 state 两个字段要按 0 参与），因此这些偏移一旦漂移
就会静默算出错误的校验值。这里逐个钉死。

### `typedef char arkrtos_app_off_sign_check[ (((uint32)&(((arkrtos_app_hea`

```c
typedef char arkrtos_app_off_sign_check[ (((uint32)&(((arkrtos_app_header_t *)0)->signature)) == ARKRTOS_APP_OFF_SIGN) ? 1 : -1];
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

### `uint32 arkrtos_crash_mini_at(uint32 idx, uint32 *hash, uint32 *cnt,`

```c
uint32 arkrtos_crash_mini_at(uint32 idx, uint32 *hash, uint32 *cnt,
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

### `void arkrtos_port_resume_task(uint32 stack_ptr);`

```c
void arkrtos_port_resume_task(uint32 stack_ptr);
```

**在异常处理程序内部直接恢复目标任务上下文并异常返回**
- `stack_ptr`：目标任务栈指针（指向其保存的 R4-R11[/EXC_RETURN]）
故障恢复等“已经处于异常处理程序中、不能依赖 PendSV”的场景专用：
Cortex-M 上 HardFault 优先级高于 PendSV，故障处理程序活动期间
PendSV 不会被服务，必须由本函数直接完成恢复 + 异常返回。
本函数不返回；stack_ptr 为 0 时调用方应自行停机，不要调用。

### `uint32 arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx);`

```c
uint32 arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx);
```

**读取系统调用上下文中的第 idx 个参数（0~3）**
- `p_exc_ctx`：系统调用入口传给内核的上下文指针
- `idx`：参数序号（0 对应 a0/r0）
**返回**：参数值
各架构的栈帧/trap 帧布局不同，由 port 层负责解析，
内核只通过本接口与上面的宏访问参数，不感知任何寄存器布局。

### `void   arkrtos_port_svc_set_ret(void *p_exc_ctx, uint32 value);`

```c
void   arkrtos_port_svc_set_ret(void *p_exc_ctx, uint32 value);
```

**写回系统调用返回值（第 0 个参数寄存器）**
- `p_exc_ctx`：系统调用上下文指针
- `value`：返回值

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

### `void arkrtos_port_set_psp(uint32 val);`

```c
void arkrtos_port_set_psp(uint32 val);
```

**设置进程栈指针PSP**
- `val`：PSPֵ

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

### `uint32 arkrtos_port_stack_init(uint32 stack_top, void (*entry)(void));`

```c
uint32 arkrtos_port_stack_init(uint32 stack_top, void (*entry)(void));
```

**初始化任务栈帧**
- `stack_top`：栈顶地址（高地址，已8字节对齐）
- `entry`：任务入口函数
**返回**：初始化后的栈指针（将存入task->stack_ptr）
不同架构的异常栈帧格式不同：
- ARM Cortex-M: 压入xPSR/PC/LR/R12/R3-R0(硬件) + R4-R11(软件)
- RISC-V: 压入mepc/mstatus/x1-x31
此函数由port层实现，内核无需关心具体布局

### `void arkrtos_port_enter_idle(uint32 stack_ptr, uint32 use_priv);`

```c
void arkrtos_port_enter_idle(uint32 stack_ptr, uint32 use_priv);
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

### `int32  arkrtos_hwsafe_set_will(const char *dev_name, int32 owner,`

```c
int32  arkrtos_hwsafe_set_will(const char *dev_name, int32 owner,
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

### `int32 arkrtos_mempool_create_internal(const char *name, void *pool,`

```c
int32 arkrtos_mempool_create_internal(const char *name, void *pool,
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

### `int32 arkrtos_ptable_mini_alloc(uint32 code_bytes, uint32 ram_bytes,`

```c
int32 arkrtos_ptable_mini_alloc(uint32 code_bytes, uint32 ram_bytes,
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

### `int32 arkrtos_ptable_mini_info(uint32 index,`

```c
int32 arkrtos_ptable_mini_info(uint32 index,
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

### `#define ARKRTOS_RT_ROWS                 4u`

```c
#define ARKRTOS_RT_ROWS                 4u
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
#define ARKRTOS_SLOT_FREE           (0u)   /* 记录未使用，对应 Flash 区间可被分配 */
```

**槽位类型（与 arkrtos_app_image.h 的 ARKRTOS_APP_TYPE_x 同值） */**

### `#define ARKRTOS_APP_SLOT_EMPTY      (0u)    /* 空槽位 */`

```c
#define ARKRTOS_APP_SLOT_EMPTY      (0u)    /* 空槽位 */
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

### `typedef struct`

```c
typedef struct
```

一条用例：展示名（建议 "模块.场景"）+ 函数 */

### `typedef struct`

```c
typedef struct
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

### `void ark_utest_fail_num(const char *file, int line, const char *expr,`

```c
void ark_utest_fail_num(const char *file, int line, const char *expr,
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
#define ARK_UTEST_CASE_DEF(_ident)                                        \
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

### `#define UTEST_CHECK(_expr)                                        \`

```c
#define UTEST_CHECK(_expr)                                        \
```

真值断言：失败只记账，继续跑后面的断言 */

### `#define UTEST_REQUIRE(_expr)                                      \`

```c
#define UTEST_REQUIRE(_expr)                                      \
```

前置断言：失败记一条账后**立即返回**（后续断言不再执行）。
用于"不满足就没法继续"的前置条件，避免拿坏状态继续跑出假绿。

### `#define UTEST_CHECK_EQ(_a, _b)                                    \`

```c
#define UTEST_CHECK_EQ(_a, _b)                                    \
```

数值相等（long 口径）*/

### `#define UTEST_CHECK_NE(_a, _b)                                    \`

```c
#define UTEST_CHECK_NE(_a, _b)                                    \
```

数值不等（long 口径）*/

### `#define UTEST_CHECK_RANGE(_lo, _x, _hi)                           \`

```c
#define UTEST_CHECK_RANGE(_lo, _x, _hi)                           \
```

区间断言：lo <= x && x <= hi */

### `#define UTEST_FAIL(_msg)                                          \`

```c
#define UTEST_FAIL(_msg)                                          \
```

无条件失败（用于"走到这里本不该发生"的分支）*/

### `#define UTEST_SKIP(_reason)                                       \`

```c
#define UTEST_SKIP(_reason)                                       \
```

主动跳过当前用例（其后内容不再执行）*/

## kernelsrc/include/arkrtos_version.h

**Ark RTOS release version, defined in exactly one place.**
The kernel, the shell and any application that includes arkrtos.h read
these macros, so the version never has to be retyped as a string.
The git tag and the README badge carry the same three numbers and

### `#define ARKRTOS_VERSION_MAJOR     0   /*!< Major, matches the first ta`

```c
#define ARKRTOS_VERSION_MAJOR     0   /*!< Major, matches the first tag field.  */
```

### `#define ARKRTOS_VERSION_NUM       (((unsigned)ARKRTOS_VERSION_MAJOR <<`

```c
#define ARKRTOS_VERSION_NUM       (((unsigned)ARKRTOS_VERSION_MAJOR << 24) | \
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

### `typedef struct`

```c
typedef struct
```

**挂载点的一行信息，用于 shell 的 mount 命令**
> 说明：fs/source 两个指针指向只读常量，调用者不得释放或改写。

### `typedef struct`

```c
typedef struct
```

**一个目录项／一个节点的元数据，字段含义与 POSIX struct stat 对齐。**
> 说明：uid/gid 是数字。这里没有用户数据库，所以不去把 0 翻译成 "root"——
那会是一个看着权威、却查无此人的答案。mtime 为 0 表示该文件系统
不存时间，不是「1970-01-01」。

### `typedef int (*arkrtos_vfs_list_cb_t)(const char *name,`

```c
typedef int (*arkrtos_vfs_list_cb_t)(const char *name,
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
int32 arkrtos_vfs_mount_volume(const char *dev, uint32 offset, uint32 size,
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

### `int32 arkrtos_vfs_read_file(const char *path, uint8 *buf, uint32 max,`

```c
int32 arkrtos_vfs_read_file(const char *path, uint8 *buf, uint32 max,
```

**读整个文件；*out_len 收实际长度（可为 NULL）**
> 说明：缓冲区必须装得下整个文件，否则报错而不是给你一半——
要看大小不确定的文件（如 shell 的 cat）用下面三个流式入口。 */

### `int32 arkrtos_vfs_read_at(const char *path, uint8 *buf, uint32 len, ui`

```c
int32 arkrtos_vfs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off,
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

### `#define ARKRTOS_VFS_O_RDONLY     (0x0000u)`

```c
#define ARKRTOS_VFS_O_RDONLY     (0x0000u)
```

**open 标志。数值是 Ark RTOS 自己的，arkrtos_vfs.c 里逐位映射到**
ark_vfs 的 ARK_O_*——App 不 include ark_vfs 的头，所以上游的值
不能当 ABI 用。 */

### `#define ARKRTOS_VFS_S_IFCHR      (0x2000u)`

```c
#define ARKRTOS_VFS_S_IFCHR      (0x2000u)
```

**stat 的文件类型位，与 POSIX 的 S_IFMT 同形，映射到 ARK_S_IF*。 */**

### `#define ARKRTOS_VFS_S_IRUSR      (0x0100u)`

```c
#define ARKRTOS_VFS_S_IRUSR      (0x0100u)
```

**权限位，布局与 POSIX 的 rwx 三元组一致，数值与 ark_vfs 的**
ARK_S_I* 相同（arkrtos_vfs.c 里有静态断言盯着这个等式）。
类型位在 ARKRTOS_VFS_S_PERM 之上，两者可分别用 S_IFMT / S_PERM
取出。设备节点（/dev 下）按惯例是 0666，普通文件 0644、目录 0755。 */

### `#define ARKRTOS_VFS_SEEK_SET     (0u)`

```c
#define ARKRTOS_VFS_SEEK_SET     (0u)
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

### `int32 arkrtos_vfs_app_readdir(int32 handle, char *name, uint32 cap,`

```c
int32 arkrtos_vfs_app_readdir(int32 handle, char *name, uint32 cap,
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
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
```

**比较并交换（原子操作）**
- `p_addr`：目标地址（4 字节对齐）
- `expect`：期望值
- `new_value`：期望成立时写入的新值
**返回**：1=成功，0=当前值不是 expect（未修改）

### `uint32 arkrtos_port_cpu_id(void)`

```c
uint32 arkrtos_port_cpu_id(void)
```

**获取当前 CPU 编号**
**返回**：单核 MCU 固定返回 0，多核移植时返回硬件核号

### `void arkrtos_port_spin_hint(void)`

```c
void arkrtos_port_spin_hint(void)
```

**自旋等待提示**
> 说明：单核无总线争用，用 NOP 即可；多核可改为 __WFE() 降低功耗与争用。

### `uint32 arkrtos_port_enter_critical(void)`

```c
uint32 arkrtos_port_enter_critical(void)
```

**进入临界区，保存 PRIMASK 并关中断**
**返回**：进入前的中断状态

### `void arkrtos_port_exit_critical(uint32 state)`

```c
void arkrtos_port_exit_critical(uint32 state)
```

**退出临界区，恢复进入前的中断状态**

### `uint32 arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx)`

```c
uint32 arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx)
```

**读取系统调用参数（0~3，对应 R0~R3）**
Cortex-M 硬件压栈顺序为 R0,R1,R2,R3,R12,LR,PC,xPSR，
使用栈帧指针直接索引即可。

### `void arkrtos_port_svc_set_ret(void *p_exc_ctx, uint32 value)`

```c
void arkrtos_port_svc_set_ret(void *p_exc_ctx, uint32 value)
```

**写回系统调用返回值（R0）**

### `int32 arkrtos_port_cyccnt_init(void)`

```c
int32 arkrtos_port_cyccnt_init(void)
```

**使能 DWT 周期计数器（不清零：它是自由运行计数器，**
清零会让另一处正在计时的时间戳突然对不上）
**返回**：0 成功

### `uint32 arkrtos_port_get_cyccnt(void)`

```c
uint32 arkrtos_port_get_cyccnt(void)
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
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
```

**比较并交换（原子操作）**
- `p_addr`：目标地址（4 字节对齐）
- `expect`：期望值
- `new_value`：期望成立时写入的新值
**返回**：1=成功，0=当前值不是 expect（未修改）

### `uint32 arkrtos_port_cpu_id(void)`

```c
uint32 arkrtos_port_cpu_id(void)
```

**获取当前 CPU 编号**
**返回**：单核 MCU 固定返回 0，多核移植时返回硬件核号

### `void arkrtos_port_spin_hint(void)`

```c
void arkrtos_port_spin_hint(void)
```

**自旋等待提示**
> 说明：单核无总线争用，用 NOP 即可；多核可改为 __WFE() 降低功耗与争用。

### `uint32 arkrtos_port_enter_critical(void)`

```c
uint32 arkrtos_port_enter_critical(void)
```

**进入临界区，保存 PRIMASK 并关中断**
**返回**：进入前的中断状态

### `void arkrtos_port_exit_critical(uint32 state)`

```c
void arkrtos_port_exit_critical(uint32 state)
```

**退出临界区，恢复进入前的中断状态**

### `uint32 arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx)`

```c
uint32 arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx)
```

**读取系统调用参数（0~3，对应 R0~R3）**
Cortex-M 硬件压栈顺序为 R0,R1,R2,R3,R12,LR,PC,xPSR，
使用栈帧指针直接索引即可。

### `void arkrtos_port_svc_set_ret(void *p_exc_ctx, uint32 value)`

```c
void arkrtos_port_svc_set_ret(void *p_exc_ctx, uint32 value)
```

**写回系统调用返回值（R0）**

### `int32 arkrtos_port_cyccnt_init(void)`

```c
int32 arkrtos_port_cyccnt_init(void)
```

**使能 DWT 周期计数器（不清零：它是自由运行计数器，**
清零会让另一处正在计时的时间戳突然对不上）
**返回**：0 成功

### `uint32 arkrtos_port_get_cyccnt(void)`

```c
uint32 arkrtos_port_get_cyccnt(void)
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
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
```

**比较并交换（原子操作）**
- `p_addr`：目标地址（4 字节对齐）
- `expect`：期望值
- `new_value`：期望成立时写入的新值
**返回**：1=成功，0=当前值不是 expect（未修改）

### `uint32 arkrtos_port_cpu_id(void)`

```c
uint32 arkrtos_port_cpu_id(void)
```

**获取当前 CPU 编号**
**返回**：单核 MCU 固定返回 0，多核移植时返回硬件核号

### `void arkrtos_port_spin_hint(void)`

```c
void arkrtos_port_spin_hint(void)
```

**自旋等待提示**
> 说明：单核无总线争用，用 NOP 即可；多核可改为 __WFE() 降低功耗与争用。

### `uint32 arkrtos_port_enter_critical(void)`

```c
uint32 arkrtos_port_enter_critical(void)
```

**进入临界区，保存 PRIMASK 并关中断**
**返回**：进入前的中断状态

### `void arkrtos_port_exit_critical(uint32 state)`

```c
void arkrtos_port_exit_critical(uint32 state)
```

**退出临界区，恢复进入前的中断状态**

### `uint32 arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx)`

```c
uint32 arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx)
```

**读取系统调用参数（0~3，对应 R0~R3）**
Cortex-M 硬件压栈顺序为 R0,R1,R2,R3,R12,LR,PC,xPSR，
使用栈帧指针直接索引即可。

### `void arkrtos_port_svc_set_ret(void *p_exc_ctx, uint32 value)`

```c
void arkrtos_port_svc_set_ret(void *p_exc_ctx, uint32 value)
```

**写回系统调用返回值（R0）**

### `int32 arkrtos_port_cyccnt_init(void)`

```c
int32 arkrtos_port_cyccnt_init(void)
```

**使能 DWT 周期计数器（不清零：它是自由运行计数器，**
清零会让另一处正在计时的时间戳突然对不上）
**返回**：0 成功

### `uint32 arkrtos_port_get_cyccnt(void)`

```c
uint32 arkrtos_port_get_cyccnt(void)
```

**读 DWT 周期计数器 (自然回绕)**
**返回**：当前周期计数

## kernelsrc/sdk/app_sdk/arkrtos_app_main.c

**Ark RTOS App SDK - 入口与退出胶水**
入口是**用户自己写的标准 C `main()`**：
#include <stdio.h>
int main(void)

### `void arkrtos_app_exit(int code) {`

```c
void arkrtos_app_exit(int code) {
```

**标准 C 的退出路径：结束当前线程**
- `code`：退出码（保留；当前内核不按退出码做判定）
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
的进程退出一致。
arkc 档**不要编这个文件**：arkc 的入口同样是工程自己的
`int main(void)`，但它的退出由启动桩
（`arkrtos_mini_start_arkc.c`）负责，不需要这里的胶水；更重要的
是 arkc 把每个 `.c` 编成自包含对象，同一翻译单元内的调用在汇编期
就绑死，多一份骨架只会让入口静默失效。这里直接 `#error`。

### `void arkrtos_mini_exit(int code) {`

```c
void arkrtos_mini_exit(int code) {
```

**标准 C 的退出路径：结束当前小程序任务**
- `code`：退出码（保留；当前内核不按退出码做判定）
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
const char  *arkrtos_posix_inet_ntop(int af, const void *src, char *dst,
```

The reverse; dst_size must be at least 16 bytes for AF_INET. */

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

### `arkrtos_ssize_t arkrtos_posix_readlink(const char *path, char *buf,`

```c
arkrtos_ssize_t arkrtos_posix_readlink(const char *path, char *buf,
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

### `void arkrtos_task_stack_fill(uint32 *stack_bottom, uint32 stack_size)`

```c
void arkrtos_task_stack_fill(uint32 *stack_bottom, uint32 stack_size)
```

**用填充图案覆盖整段任务栈（跳过下标 0 的栈底保护字）**
- `stack_bottom`：栈底地址
- `stack_size`：栈大小（字节）
任务运行过程中会不断覆盖该图案，查询栈用量时扫描仍保持图案的
连续区域即可得到“从未使用过”的栈空间，换算后即为峰值用量。

## kernelsrc/src/arkrtos_fs.c

**littlefs port + Ark RTOS file system facade - see arkrtos_fs.h**
Structure of this file:
1) the three board callbacks littlefs needs (read/prog/erase),
each one a translation from "block + offset" into a call on a

### `int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_`

```c
int32 arkrtos_fs_stat_path(const char *path, uint32 *size, uint32 *is_dir)
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

## kernelsrc/src/arkrtos_loader.c

**Ark RTOS 镜像加载器实现（动态装载：安装 / 扫描 / 重定位 / 搬移 / 回收 / 启停）**
加载流程：
1) 校验镜像头（魔数 / 兼容签名 / 长度 / 入口 / 重定位表）；
2) 在池里找第一段足够长的**已擦除**空间（首适配），

### `static uint32 arkrtos_loader_mov_imm16(uint16 h1, uint16 h2)`

```c
static uint32 arkrtos_loader_mov_imm16(uint16 h1, uint16 h2)
```

**从 MOVW/MOVT 的一对半字里取出 imm16**
- `h1`：前缀半字（11110 i 10 0100 imm4）
- `h2`：第二个半字（0 imm3 Rd imm8）

### `static void arkrtos_loader_mov_set_imm16(uint16 *p_h1, uint16 *p_h2, u`

```c
static void arkrtos_loader_mov_set_imm16(uint16 *p_h1, uint16 *p_h2, uint32 imm16)
```

**把 imm16 写回 MOVW/MOVT 的一对半字（Rd 与指令种类保持不变）**

### `static void arkrtos_loader_reloc_movw(uint8 *p, uint32 delta)`

```c
static void arkrtos_loader_reloc_movw(uint8 *p, uint32 delta)
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
uint32 arkrtos_loader_reloc_apply(uint8 *buf, uint32 buf_off, uint32 len,
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

### `int32 arkrtos_loader_check_install(const arkrtos_app_header_t *p_hdr)`

```c
int32 arkrtos_loader_check_install(const arkrtos_app_header_t *p_hdr)
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

## kernelsrc/src/arkrtos_task.c

**Ark RTOS 任务调度与系统调用分发实现**
实现任务调度器、上下文切换的 C 侧逻辑、SVC 系统调用分发，以及任务控制接口。
中断入口（SysTick_Handler/HardFault_Handler）在 board/ 中实现，
它们最终调用本文件的 arkrtos_kernel_tick_handler / arkrtos_hardfault_handler。

### `static int32 arkrtos_sched_lock_blocks(void)`

```c
static int32 arkrtos_sched_lock_blocks(void)
```

**检查调度器锁是否被持有（阻塞类接口的编程错误防护）**
**返回**：1=已锁定（调用方应忽略本次阻塞请求），0=未锁定
在调度器锁定期间调用阻塞接口会破坏任务状态一致性，
此处忽略请求并记录一条故障记录，便于定位问题代码。

### `void arkrtos_sched_lock_internal(void)`

```c
void arkrtos_sched_lock_internal(void)
```

**获取调度器锁（用户任务经 SVC 调用）**
累加嵌套计数后禁止任务切换，但不关闭中断。
临界区内不得调用阻塞接口（会被忽略并记录故障）。

### `uint32 arkrtos_sched_unlock_internal(void)`

```c
uint32 arkrtos_sched_unlock_internal(void)
```

**释放调度器锁**
**返回**：剩余嵌套层数（0 表示已完全解锁）
计数归零时主动触发一次任务切换，补偿锁定期间被推迟的调度。

### `int32 arkrtos_sched_lock_count_internal(void)`

```c
int32 arkrtos_sched_lock_count_internal(void)
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
