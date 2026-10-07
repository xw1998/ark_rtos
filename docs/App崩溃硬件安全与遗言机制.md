# App 崩溃时的硬件安全态（"遗言"）机制

**这份文档回答一个问题：App 控制硬件（电机、机床、机械臂、继电器……）时崩溃了，
谁来保证硬件不会带着崩溃前的最后一条指令继续跑？**

MPU / 特权隔离解决的是"App 越权访问别人的内存"（见 [内存保护与App越权验证](内存保护与App越权验证.md)），
**不解决**"App 合法访问过的硬件，在 App 死掉后仍停在其最后一次设定上"。
一台正在转的电机不会因为控制它的 App 触了 fault 就停下来——除非有人**在 App 死后**把它停下来。
本机制就是那个"有人"：**内核**，而不是已经崩溃的 App。

---

## 一、设计前提（先说清边界）

1. **执行者必须是内核。** 崩溃的 App 已经不能跑一行代码；故障恢复路径甚至处于中断屏蔽态。
   因此"中和"这个动作由**内核**在任务收尸时执行，被调用的代码只能是**常驻镜像**里的代码
   （驱动的 `ctrl`、驱动/App 注册的安全态回调）。App 自己的线程代码在崩溃后**不可依赖**。
2. **不假装安全。** 一个设备没有任何人为它声明过安全态时，内核只能把它从台账里删掉，
   并**计数**（`hwsafe` 命令里能看到"dropped without callback"），绝不在日志里写"已中和"。
3. **默认关。** `ARKRTOS_HW_SAFE`（`kernelsrc/include/arkrtos_features.h`）默认 `0`；
   关闭时本模块**整体不进二进制**，设备 `open/close` 与任务终止路径与引入本机制前**完全一致**，
   体积与行为零变化。
4. **互补而非替代。** 与 MPU、看门狗守卫（`ARKRTOS_USE_KERNEL_GUARD`）是三层不同的防线：
   - MPU：拦"越权访问"；
   - 看门狗守卫：拦"任务不再喂狗"（整机级）；
   - 本机制：拦"所有者死了，硬件还在跑"（设备级、确定性中和）。

---

## 二、三个角色与两条通道

### 角色

| 角色 | 谁来做 | 做什么 |
|------|--------|--------|
| 登记安全态回调 | **驱动**（常驻） | 用 `arkrtos_hwsafe_register(设备名, 中和函数)` 声明"这台设备的安全态是什么" |
| 声明"遗言" | **App**（可崩溃方） | 用 `arkrtos_dev_set_will(设备名, ctrl 命令, 参数)` 声明"我死后请对这台设备执行这条 ctrl" |
| 记所有权台账 | **内核** | 设备被打开时按"设备名 + 所有者任务号"记一条；任务死亡时按此中和 |

"遗言"这个名字的来历：App 在**活着的时候**就用"打开设备"这个动作声明了
"我活着时归我用，我死了请收走"，再补一句"怎么收"——这就是它留下的遗言。
内核认的是这份遗言，而不是一个已经死掉的线程还能再跑一段代码。

### 两条通道

**通道 A —— 驱动注册回调（设备级、通用）**

```c
/* 驱动侧（用户态驱动走 SVC 0x14 sub 4；内核态驱动直接链接） */
int32 my_motor_safe(arkrtos_dev_hdr_t *obj, uint32 owner)
{
    my_motor_t *m = (my_motor_t *)obj;
    m->pwm = 0;                 /* 非阻塞、幂等、只写寄存器/GPIO */
    HAL_GPIO_WritePin(...);
    return 0;
}
arkrtos_drv_hwsafe_register("motor0", my_motor_safe);
```

回调契约（**必须遵守**，见 `arkrtos_hwsafe.h` 的 `arkrtos_hwsafe_fn` 注释）：
运行在任务退出 / 故障恢复路径，中断可能屏蔽；因此必须**非阻塞**（有界寄存器/GPIO 写，
不得等待、延时、调用 SVC、触碰文件系统或 Flash），且应写成**幂等**。

**通道 B —— App 声明遗言（所有者级、不用写回调）**

```c
/* App 侧：设备必须先 open（内核据此确认"你确实拥有它"） */
h = arkrtos_dev_open("motor0", 0);
...
arkrtos_dev_set_will("motor0", MOTOR_CTRL_STOP, 0);   /* 我死了把电机停掉 */
```

内核在所有者死亡时对该设备执行 `drv->drv_ctrl(obj, cmd, val)`。
对没有 `ctrl` 入口的设备，这条通道无效（内核返回错误，而不是假装成功）。

### 优先级

同一台设备两者都存在时，**先执行所有者遗言（通道 B），没有遗言才用驱动回调（通道 A）**，
两者都没有则只删台账并计数：

```
所有者死亡 → 该设备有遗言？ → drv_ctrl(obj, will_cmd, will_val)
                          └否→ 该设备有注册回调？ → fn(obj, owner)
                                             └否→ 只删条目 + 计数（不假装安全）
```

理由：遗言是**这台设备这一次的具体收尾动作**，比"设备默认安全态"更精确；而驱动回调是
设备出厂就定好的兜底。两者都是**内核可校验的常驻代码**。

---

## 三、实现（内核侧）

| 文件 | 内容 |
|------|------|
| `kernelsrc/include/arkrtos_hwsafe.h` / `src/arkrtos_hwsafe.c` | 回调表 + 所有权台账 + 两阶段中和 |
| `kernelsrc/src/arkrtos_dev.c` | 设备 `open` 成功后 `arkrtos_hwsafe_claim()`；`close` 时 `release()`；`unregister` 时注销该设备回调；提供 `arkrtos_dev_ctrl_obj()` 供遗言执行 |
| `kernelsrc/src/arkrtos_task.c` | `arkrtos_task_release_resources()` 里两阶段执行：临界区内 `arkrtos_hwsafe_on_death()`（只打标记），临界区外 `arkrtos_hwsafe_run_pending()`（真正回调） |
| `board/stm32f427/` | `drvled.c` 提供 `led_drv_safe()` 示例；`arkrtos_board.c` 为 led0/led1/led2 登记 |
| `kernelsrc/shell/arkrtos_shell.c` | `hwsafe` 命令：台账、已登记设备、累计中和次数、最近中和的任务号 |

**为什么分两阶段？** `arkrtos_task_release_resources()` 也会从**故障处理路径**被调用，此时中断是屏蔽的。
在临界区内执行驱动回调，若回调里有任何一个等待/延时就可能死锁。所以临界区里只做"标记待中和"（纯内存写），
出了临界区再执行回调（此时中断已开）。

**容量**：`ARKRTOS_HWSAFE_MAX_DEV`（回调表）与 `ARKRTOS_HWSAFE_MAX_OWN`（台账）默认各 8，编译期定容。
表满时**宁可不记也不覆盖别人**——覆盖会把另一台设备的安全态弄丢，必须靠配置把表配够。

---

## 四、对外接口

### 内核（`arkrtos_hwsafe.h`，仅内核/驱动内部使用）

| 函数 | 说明 |
|------|------|
| `arkrtos_hwsafe_reset()` | 模块初始化，清空两张表 |
| `arkrtos_hwsafe_register(name, fn)` | 登记设备安全态回调；同名重复登记**报错**（两套安全态是配置错误） |
| `arkrtos_hwsafe_unregister(name)` | 注销回调 |
| `arkrtos_hwsafe_claim/release(name, owner, obj)` | 台账增删（由设备框架调用） |
| `arkrtos_hwsafe_set_will/clear_will(name, owner, cmd, val)` | 所有者遗言 |
| `arkrtos_hwsafe_on_death(task_id)` / `run_pending()` | 两阶段中和 |
| `arkrtos_hwsafe_owner_count/dev_count/neutralized_count/last_owner/registered/info/will` | 观测 |

### 驱动 SDK（`arkrtos_driver_sdk.h`）

```c
typedef int32 (*arkrtos_hwsafe_fn)(void *obj, uint32 owner);
int32 arkrtos_drv_hwsafe_register(const char *dev_name, arkrtos_hwsafe_fn fn);
```

### App SDK（`arkrtos.h`）

```c
int32 arkrtos_dev_set_will(const char *dev_name, int32 cmd, int32 val);
```

### SVC 面

| SVC | sub | 功能 | 参数 |
|-----|-----|------|------|
| 0x14 `DRV_MGR` | 4 | 登记设备安全态回调 | `p[1]=name`、`p[2]=fn`（必须落在调用者自己的镜像窗口内） |
| 0x14 `DRV_MGR` | 5 | 查询设备是否已登记回调 | `p[1]=name` → `1/0` |
| 0x14 `DRV_MGR` | 6 | 注销设备安全态回调 | `p[1]=name` |
| 0x10 `DEV_IO` | 6 | 声明当前任务对某设备的遗言 | `p[1]=name`、`p[2]=cmd`、`p[3]=val` |

**安全校验**：登记的回调指针必须落在**调用者自己的镜像窗口**内（`arkrtos_kernel_cb_own_region_ok`），
与"驱动回调指针"同一条约束——**盖不住别人的权限**。遗言只允许声明**当前任务已经打开过**的设备，
避免一个 App 去给别人的设备留遗言。

---

## 五、shell 观测

```
ark> hwsafe
hwsafe: enabled, devs=3 owners=1 neutralized=0 last_owner=0
  dev0: led0  (callback registered)
  dev1: led1  (callback registered)
  dev2: led2  (callback registered)
  own0: dev=led0 owner=5 dead=0 will=none
```

`neutralized` 会在某个所有者死亡后 +1（无论是否执行了回调，因为"条目被收走"这件事发生了）；
**是否真的带了回调**要看设备行是否显示 `callback registered`，两者分开，避免把"没回调"读成"已中和"。

---

## 六、上板验证口径

`example/stm32f427/app_sdk/HWSAFE_DEMO/`：

1. App 打开 `led0`、点亮，并声明遗言（`ARKRTOS_DEV_LED_CTRL_OFF`）；
2. App 故意触发一次越权访问 → 被 MPU 拦下 → 故障恢复把任务杀掉；
3. 内核在收尸时执行遗言 → 点亮状态被强制关掉；
4. shell 里 `hwsafe` 显示 `neutralized=1`、`last_owner=<该 App 的任务号>`，`ls /dev` 与 LED 目视确认已灭。

**结论分级**：本页只记录"编译通过 / 上板验证过 / 未验证"三档，不做修饰。
见提交记录与 `hwsafe` 实测输出。
