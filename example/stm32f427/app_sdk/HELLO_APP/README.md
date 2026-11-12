# HELLO_APP —— 最小的 App：一个标准 C 程序

`Src/hello_app.c` 就是你在 Linux / Windows 上写的那段程序，逐字相同：

```c
#include <stdio.h>

int main()
{
    printf("Hello, World!\n");
    return 0;
}
```

- **没有 RTOS 头文件**，**没有 `AppMain()`**，**没有 `while(1)`**。
- `printf` 由 SDK（`kernelsrc/sdk/app_sdk/arkrtos_stdio.c`）接到内核控制台
  （SVC `0x1A`），格式化器是 SDK 自带的小实现（`%d %i %u %x %X %s %c %%`，
  单条上限 128 字节）。工程里**不要**再同时编 `arkrtos_libc_glue.c`——那是
  另一套（newlib/AC6 retarget）输出，两份 `printf` 只能选一套。
- `return 0` 等于进程结束：板上没有进程，于是结束这个 App 任务，栈由内核回收；
  启动桩（GCC 档）在 `main` 返回后调 `_exit`，Keil 档走 `__main` → `exit` →
  `_sys_exit`，两条链都由 `arkrtos_app_main.c` 接到 `arkrtos_thread_exit()`。

## 工程内容

| 文件 | 作用 |
|------|------|
| `Src/hello_app.c` | 用户程序（本文件） |
| `MDK-ARM/hello_app.uvprojx` | Keil 工程（AC6），源：`hello_app.c` + `arkrtos_app_main.c` + `arkrtos_stdio.c` + `arkrtos_oslib.c` + `arkrtos_app_start.s` |
| `GCC/Makefile`、`GCC/sources.mk` | 同一份文件集的 GCC 工程（`sources.mk` 由 `.uvprojx` 派生，不要手改） |

## 编译

Keil 侧 `.sct` 由 `BeforeMake` 自动生成（`tools/gen_app_sct.py --type app --dev-slot 1
--ram-size 8192`），输出在仓库根的 `build/hello_app.sct`。

GCC 侧：

```
cd example/stm32f427/app_sdk/HELLO_APP/GCC
make CROSS=<你的 arm-none-eabi- 前缀>
```

实测（arm-none-eabi-gcc 12.3.1、`-O2`、`-ffunction-sections --gc-sections`）：

```
FLASH:  492 B  (128 KB 的 0.38%)
RAM:     28 B  (12 KB 的 0.23%)
```

`printf("Hello, World!\n")` 被 GCC 折成了 `puts`，落到的正是 SDK 的 `puts`；
两条路径都由 `printf` 的单条上限与格式化规则约束。

`'\n'` 会按终端惯例补成 `"\r\n"`（串口是裸字节流，只发 `'\n'` 光标不回列）；
用户自己写 `"\r\n"` 不会被补成 `"\r\r\n"`。这条规则由
`tools/sdk_stdio_host_gate.py` 在宿主上逐字节核对。

## 怎么跑

App 是「装到镜像池里的独立固件」，走安装通道而不是直接烧录：

```
python tools/pack_app.py --project example/stm32f427/app_sdk/HELLO_APP/MDK-ARM/hello_app.uvprojx \
        --type app --name HELLO_APP --out build/hello_app.svcapp
# 送进设备并安装（端口按实际改）
python tools/send_image.py --port COM3 build/hello_app.svcapp
```

细节见 `docs/Ark RTOS应用安装与调试指南.md`。

## 想要它一直跑

`main` 返回即结束。要做常驻应用，在自己源码里写循环即可——那是用户的代码，
SDK 不替用户决定生命周期。

## 另一种跑法：小程序

同一段源码也能编成小程序（文件系统里的 `.svcapp`，跑完归还内存），
见 `../HELLO_MINI`：两个工程的 `Src/*.c` 逐字相同，差别只在链接哪份启动桩。
