# Ark RTOS 移植手册

## 1. 架构概述

Ark RTOS 采用分层解耦架构，内核代码（kernelsrc/）与具体芯片完全分离。移植到新芯片时，只需实现 port 层和 board 层，内核代码无需任何修改。

### 1.1 分层架构图

```
┌──────────────────────────────────────────────────────────────┐
│            应用层 (App SDK)                                   │
├──────────────────────────────────────────────────────────────┤
│         Ark RTOS 内核 (kernelsrc/src/)      │ ← 纯 C，零硬件依赖│
│    任务调度、事件管理、设备框架、SVC分发                       │
├──────────────────────────────────────────────────────────────┤
│     arkrtos_hal.h (硬件抽象接口)             │ ← 纯函数声明     │
├──────────────────────────────────────────────────────────────┤
│      port/<架构>/ (架构适配层)             │ ← 架构相关实现   │
│   CPU指令、上下文切换、栈帧初始化、MPU操作                    │
│   例: port/arm/cortex-m4/arkrtos_port.c                        │
│       port/arm/cortex-m4/arkrtos_context.S                     │
├──────────────────────────────────────────────────────────────┤
│      board/<芯片>/ (板级适配层)             │ ← 芯片相关实现   │
│   板卡初始化、中断入口、设备驱动                              │
│   例: board/stm32f427/arkrtos_board.c                          │
│       board/stm32f427/drvuart.c                              │
├──────────────────────────────────────────────────────────────┤
│            具体MCU硬件                                        │
└──────────────────────────────────────────────────────────────┘
```

**解耦原则：**
- `kernelsrc/src/` 中的代码不包含任何芯片头文件
- `kernelsrc/src/` 中的代码不直接操作任何硬件寄存器
- 所有硬件访问均通过 `arkrtos_hal.h` 中声明的函数抽象
- 架构相关代码（上下文切换、栈帧初始化）在 `port/` 层实现
- 板级相关代码（中断入口、设备驱动）在 `board/` 层实现

### 1.2 支持的CPU架构

| 架构 | FPU | MPU | port目录 |
|------|-----|-----|----------|
| Cortex-M3 | - | 可选 | port/arm/cortex-m3 |
| Cortex-M4 | 有 | 有 | port/arm/cortex-m4 |
| Cortex-M7 | 有 | 有 | port/arm/cortex-m7 (待实现) |

---

## 2. 目录结构

```
ARKRTOS/
├── kernelsrc/                      # ★ 内核源码（零硬件依赖）
│   ├── include/                    # 内核头文件
│   │   ├── arkrtos.h                 # 应用API头文件
│   │   ├── arkrtos_hal.h             # ★ 硬件抽象接口（核心）
│   │   ├── arkrtos_port.h            # 兼容层（包含arkrtos_hal.h）
│   │   ├── arkrtos_config.h          # 内核配置文件
│   │   ├── arkrtos_types.h           # 基础类型定义
│   │   ├── arkrtos_def.h             # 内核公共定义
│   │   ├── arkrtos_task.h            # 任务管理（内核内部）
│   │   ├── arkrtos_event.h           # 事件模块（内核内部）
│   │   ├── arkrtos_fifo.h            # FIFO模块（内核内部）
│   │   ├── arkrtos_mpu.h             # MPU管理接口（内核内部）
│   │   ├── arkrtos_dev.h             # 设备驱动框架（内核内部）
│   │   └── arkrtos_cfg.h             # 任务配置加载（内核内部）
│   ├── src/                        # 内核源文件（纯C，零硬件操作）
│   │   ├── arkrtos_init.c            # 内核启动与初始化
│   │   ├── arkrtos_task.c            # 任务调度 + SVC 分发
│   │   ├── arkrtos_event.c           # 事件管理
│   │   ├── arkrtos_fifo.c            # FIFO 环形缓冲区
│   │   ├── arkrtos_dev.c             # 设备驱动框架
│   │   └── arkrtos_cfg.c             # 任务配置加载
│   ├── port/                       # ★ 架构适配层
│   │   └── arm/
│   │       ├── cortex-m3/
│   │       │   ├── arkrtos_port.c    # Cortex-M3 架构实现
│   │       │   └── arkrtos_context.S # 上下文切换汇编
│   │       └── cortex-m4/
│   │           ├── arkrtos_port.c    # Cortex-M4 架构实现
│   │           └── arkrtos_context.S # 上下文切换汇编
│   ├── sdk/                        # SDK 开发包
│   └── components/                 # 可选组件
│
├── board/                          # ★ 板级适配层
│   └── stm32f427/                  # STM32F427 移植示例
│       ├── arkrtos_board.c           # 板级初始化 + 中断入口
│       ├── arkrtos_board_config.h    # 板级配置覆盖
│       ├── drvuart.c/h             # UART驱动
│       └── drvled.c/h              # LED驱动
│
└── example/                        # 工程示例
    └── stm32f427/                  # MDK 工程
```

---

## 3. 移植步骤

### 3.1 第一步：创建板级配置文件

在 `board/<芯片>/` 下创建 `arkrtos_board_config.h`：

```c
#ifndef __ARKRTOS_BOARD_CONFIG_H__
#define __ARKRTOS_BOARD_CONFIG_H__

#define ARKRTOS_CPU_ARCH            ARKRTOS_ARCH_CORTEX_M4
#define ARKRTOS_SYSTEM_CLOCK_HZ     (168000000)
/* 注：不要在这里写任何分区/共享内存地址。
 * 全工程的 Flash / RAM 布局（含 SHARE_RAM_BASE / SHARE_RAM_SIZE）
 * 统一在 config/arkrtos_partition.h 定义一次，板级头文件只放芯片相关参数。 */

#undef  ARKRTOS_USE_FPU
#define ARKRTOS_USE_FPU             1

#undef  ARKRTOS_USE_MPU
#define ARKRTOS_USE_MPU             0

/* 独立看门狗：1 = 内核 guard 掌握计数（不变量被破坏即停止喂狗） */
#undef  ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE          0
#undef  ARKRTOS_WDG_TIMEOUT_MS
#define ARKRTOS_WDG_TIMEOUT_MS      4000


#endif
```

在编译选项中添加：`-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"`

### 3.2 第二步：选择架构适配层

根据目标CPU架构，选择对应的port目录：

| CPU架构 | port目录 | 说明 |
|---------|----------|------|
| Cortex-M3 | port/arm/cortex-m3 | 无FPU，MPU可选 |
| Cortex-M4 | port/arm/cortex-m4 | 有FPU和MPU |
| Cortex-M7 | port/arm/cortex-m7 | 待实现 |

将对应目录下的 `arkrtos_port.c` 和 `arkrtos_context.S` 加入工程。

### 3.3 第三步：实现板级适配文件

在 `board/<芯片>/` 下创建 `arkrtos_board.c`，覆盖port层的弱定义函数：

```c
#include "stm32f4xx.h"
#include "arkrtos_hal.h"
#include "arkrtos_dev.h"
#include "arkrtos_task.h"

/* 覆盖port层弱定义 */
void arkrtos_port_board_init(void)
{
    SCB->CPACR |= (3UL << 20) | (3UL << 22);  /* FPU使能 */
}

void arkrtos_port_irq_init(void)
{
    NVIC_SetPriorityGrouping(0);
    NVIC_SetPriority(PendSV_IRQn,  0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn,  0x01);
}

/* 板载设备注册 */
extern arkrtos_dev_drv_t usart_drv;
extern arkrtos_dev_drv_t led_drv;

void arkrtos_dev_board_init(void)
{
    arkrtos_dev_register("COM1", &usart_drv, 0);
    arkrtos_dev_register("LED",  &led_drv,  0);
}

/* 中断服务程序入口 */
void SysTick_Handler(void)
{
    /* 用 STM32 HAL 的板子：必须在这里把 HAL 的毫秒时基推进起来。
     * CubeMX 生成的 SysTick_Handler（内含 HAL_IncTick()）被本工程在
     * stm32f4xx_it.c 里整体屏蔽了，不补这一句，HAL_GetTick() 会永远是 0，
     * 任何 HAL_Delay() 都会死等（HAL_Delay 等的是 GetTick 变化）。
     * 内核节拍 500us = 2kHz，HAL 时基 1ms = 1kHz，所以每 2 个节拍补一次；
     * 同时要保证 ARKRTOS_TICK_PERIOD_US 能整除 1000，否则补出来的时基不是整毫秒。 */
    if((arkrtos_kernel_get_tick() % (1000u / ARKRTOS_TICK_PERIOD_US)) == 0u)
    {
        HAL_IncTick();
    }

    arkrtos_kernel_tick_handler();
}

void HardFault_Handler(void)
{
    /* 不要在这里 while(1) 死循环：交内核故障处理，才会写故障记录、
     * 按策略重建任务栈帧恢复，并在连续故障达到上限时禁用该 App。 */
    uint32 sp = arkrtos_hardfault_handler();

    if(sp != 0u)
    {
        arkrtos_port_resume_task(sp);   /* 内核选出了可运行任务，直接恢复（本函数不返回） */
    }

    while(1) { }
}
```

### 3.4 第四步：实现设备驱动

参考 `board/stm32f427/drvuart.c` 和 `drvled.c` 实现板载设备驱动。

---

## 4. arkrtos_hal.h 接口说明

### 4.1 CPU指令层

| 函数 | 说明 |
|------|------|
| `arkrtos_port_wfi()` | 等待中断 |
| `arkrtos_port_wfe()` | 等待事件 |
| `arkrtos_port_nop()` | 空操作 |
| `arkrtos_port_isb()` | 指令同步屏障 |
| `arkrtos_port_dsb()` | 数据同步屏障 |
| `arkrtos_port_dmb()` | 数据内存屏障 |

### 4.2 中断控制层与系统调用上下文

系统调用上下文由 port 层解析，内核只使用 `ARKRTOS_SVC_NUM / ARKRTOS_SVC_ARG / ARKRTOS_SVC_RET` 宏：

| 函数 | 说明 |
|------|------|
| `arkrtos_port_syscall_num()` | 从上下文提取系统调用号 |
| `arkrtos_port_svc_get_arg()` | 读取第 idx 个系统调用参数（0~3） |
| `arkrtos_port_svc_set_ret()` | 写回系统调用返回值 |

| 函数 | 说明 |
|------|------|
| `arkrtos_port_disable_irq()` | 关闭全局中断 |
| `arkrtos_port_enable_irq()` | 开启全局中断 |
| `arkrtos_port_switch_task()` | 触发任务切换 |

### 4.3 上下文层

| 函数 | 说明 |
|------|------|
| `arkrtos_port_set_psp()` | 设置PSP栈指针 |
| `arkrtos_port_get_control()` | 读取CONTROL寄存器 |
| `arkrtos_port_set_control()` | 写入CONTROL寄存器 |
| `arkrtos_port_stack_init()` | 初始化任务栈帧 |
| `arkrtos_port_enter_idle()` | 进入空闲任务上下文 |

### 4.4 定时器层

| 函数 | 说明 |
|------|------|
| `arkrtos_port_get_system_clock()` | 获取系统主频 |
| `arkrtos_port_get_timer_counter()` | 获取节拍定时器当前计数值（ARM: SysTick->VAL） |
| `arkrtos_port_get_timer_reload()` | 获取节拍定时器重载值（ARM: SysTick->LOAD） |
| `arkrtos_port_start_timer()` | 启动系统节拍定时器 |
| `arkrtos_port_delay_us()` | 微秒级忙等延时 |
| `arkrtos_port_enter_critical()` | 进入临界区，保存中断状态并关中断 |
| `arkrtos_port_exit_critical()` | 退出临界区，恢复中断状态 |

### 4.5 板级层（弱定义，可覆盖）

| 函数 | 说明 |
|------|------|
| `arkrtos_port_board_init()` | 板卡硬件初始化 |
| `arkrtos_port_irq_init()` | 中断控制器初始化 |
| `arkrtos_port_enable_fpu()` | FPU使能 |
| `arkrtos_port_set_idle_mpu()` | 空闲任务MPU设置 |

### 4.6 MPU接口层

| 函数 | 说明 |
|------|------|
| `arkrtos_port_mpu_init()` | MPU初始化 |
| `arkrtos_port_mpu_set_region()` | 设置MPU区域 |
| `arkrtos_port_mpu_set_app()` | 设置任务 MPU，入参为 `arkrtos_arch_mpu_t`（架构无关区域上下文） |
| `arkrtos_port_mpu_reset()` | 重置MPU |

---

### 4.7 看门狗层

| 函数 | 说明 |
|------|------|
| `arkrtos_port_wdg_init(timeout_ms)` | 按近似超时启动独立看门狗；返回 0 成功、-1 表示没有看门狗或该超时无法表示 |
| `arkrtos_port_wdg_feed()` | 重装看门狗计数 |
| `arkrtos_port_wdg_timeout_ms()` | 硬件实际拿到的超时（0 = 未启动） |

看门狗是**板级事实**：由板级实现，开关 `ARKRTOS_WDG_ENABLE` 与请求超时
`ARKRTOS_WDG_TIMEOUT_MS` 都写在 `arkrtos_board_config.h` 里，默认 0。两条移植注意：

1. **启动后不可关闭**（STM32 的 IWDG 除非改选项字节）。因此第一次点亮必须用长超时，
   确认喂狗路径无误后再收紧；否则喂狗逻辑一旦有 bug，设备会进入复位环，连烧录窗口都难插进去。
2. **不要用请求值反推硬件超时**。LSI 精度约 ±50%，`arkrtos_port_wdg_timeout_ms()`
   报的是按分频与重装值算出来的值；内核 guard 把它当"大约这么久"，不做毫秒级推理。

喂狗的决定权在内核（`arkrtos_guard.c`）：只有当结构自检一致、且没有槽位违反心跳契约时才喂。

---

## 5. MDK工程配置

### 5.1 添加源文件

**Group: Kernel**
- kernelsrc/src/arkrtos_init.c
- kernelsrc/src/arkrtos_task.c
- kernelsrc/src/arkrtos_event.c
- kernelsrc/src/arkrtos_fifo.c
- kernelsrc/src/arkrtos_dev.c
- kernelsrc/src/arkrtos_cfg.c

**Group: Port**
- kernelsrc/port/arm/cortex-m4/arkrtos_port.c
- kernelsrc/port/arm/cortex-m4/arkrtos_context.S

**Group: Board**
- board/stm32f427/arkrtos_board.c
- board/stm32f427/drvuart.c
- board/stm32f427/drvled.c

### 5.2 头文件路径

```
kernelsrc\include
kernelsrc\port\arm\cortex-m4
board\stm32f427
```

### 5.3 预定义宏

```
ARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"
```

---

## 6. 移植新架构指南

Ark RTOS 的架构抽象层把内核与指令集的耦合收敛为四类接口，移植新架构（RISC-V、LoongArch 等）
只需实现这四类，内核源码零改动。完整说明见 [port/README.md](port/README.md)。

### 6.1 第一步：登记核心与能力

在 `kernelsrc/include/arkrtos_arch.h` 中增加核心宏（如 `ARKRTOS_CPU_CORE_RV32IMAC`），
并补全该核心的能力派生分支：`ARKRTOS_ARCH_HAS_FPU`、`ARKRTOS_ARCH_HAS_MPU`、
`ARKRTOS_ARCH_HAS_PRIV`、`ARKRTOS_ARCH_SVC_NUM_BITS`（SVC 号位宽，-1 表示由寄存器传递）。

### 6.2 第二步：实现四类端口接口

| 类别 | 必须实现的接口 | 架构相关要点 |
|------|---------------|-------------|
| 系统调用 | `arkrtos_port_syscall_num` / `arkrtos_port_svc_get_arg` / `arkrtos_port_svc_set_ret` | 从各自的 trap 帧取调用号与参数；Cortex-M 从 SVC 指令立即数取号，RISC-V 建议约定 a7 传号、a0~a3 传参 |
| 上下文 | `arkrtos_port_stack_init` / `arkrtos_port_switch_task` / `arkrtos_port_enter_idle` | 构造初始栈帧；触发切换（PendSV / 软件中断） |
| 内存保护 | `arkrtos_port_mpu_init` / `arkrtos_port_mpu_set_region` / `arkrtos_port_mpu_set_app` / `arkrtos_port_mpu_reset` | 把 `arkrtos_arch_mpu_t` 的 `region_base` / `region_attr` 写到 PMP、TLB 或 MPU 寄存器 |
| 时钟与临界区 | `arkrtos_port_start_timer` / `arkrtos_port_get_timer_counter` / `arkrtos_port_get_timer_reload` / `arkrtos_port_get_system_clock` / `arkrtos_port_delay_us` / `arkrtos_port_enter_critical` / `arkrtos_port_exit_critical` | 节拍定时器；临界区需保存/恢复中断状态（RISC-V `mstatus.MIE`、LoongArch `CRMD.IE`） |

### 6.3 第三步：实现上下文切换汇编

参考 `kernelsrc/port/arm/cortex-m4/arkrtos_context.S`，完成三个入口：

1. 首次进入任务（`arkrtos_port_enter_idle` 的汇编配合）
2. 任务切换（保存被换出任务的寄存器到其 PSP/SP，从新任务栈恢复）
3. 系统调用入口（跳到 `SVC_Server`，入参为不透明的上下文指针）

### 6.4 第四步：板级适配与工程配置

1. 创建 `board/<芯片>/arkrtos_board_config.h`：设置 `ARKRTOS_ARCH_CORE`、主频、共享内存地址与大小，
   并按需覆盖 `ARKRTOS_USE_FPU / ARKRTOS_USE_MPU / ARKRTOS_USE_PRIV`
2. 创建 `board/<芯片>/arkrtos_board.c`：实现弱定义接口，提供节拍中断入口并调用
   `arkrtos_kernel_tick_handler()`
3. 工程中只加入**一个**核心目录的 `.c/.S`；头文件路径仍为 `kernelsrc/include` + `board/<芯片>`
4. 验收按 `port/README.md` 第 5 节清单逐项验证

---

## 7. 功能完整性验证

移植完成后，请验证以下功能：

- [ ] 任务创建与调度
- [ ] 任务延时（arkrtos_task_wait）
- [ ] 微秒延时（arkrtos_task_delay）
- [ ] 事件创建与等待
- [ ] 设备驱动注册与操作
- [ ] 栈溢出检测
- [ ] CPU负载统计
- [ ] MPU内存保护（如启用）
