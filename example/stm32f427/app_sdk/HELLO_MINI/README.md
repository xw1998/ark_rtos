# HELLO_MINI —— 最小的小程序：同一个标准 C 程序

`Src/main.c` 与 `../HELLO_APP/Src/main.c` **逐字相同**：

```c
#include <stdio.h>

int main()
{
    printf("Hello, World!\n");
    return 0;
}
```

同一段源码，编成 App 还是小程序只差在工程里链接哪份启动桩与退出胶水：

| 项 | HELLO_APP | HELLO_MINI |
|----|-----------|-----------|
| SDK | `kernelsrc/sdk/app_sdk`（`arkrtos_app_start.s` + `arkrtos_app_main.c`） | `kernelsrc/sdk/mini_sdk`（`arkrtos_mini_start.s` + `arkrtos_mini_main.c`） |
| `printf` | `app_sdk/arkrtos_stdio.c`（两边共用同一份） | 同左 |
| 落点 | 镜像池的固定槽位（安装 + 版本管理） | 文件系统里的 `.svcapp`，`mini run` 装进 RAM |
| 退出 | 结束 App 任务，栈由内核回收 | 结束任务，**代码块与 RAM 块立刻归还** |
| 启动 | `app start <slot>` | `mini run /hello.svcm` |

## 编译

GCC 侧（产出调试裸镜像，直接烧录可断点）：

```
cd example/stm32f427/app_sdk/HELLO_MINI/GCC
make CROSS=<你的 arm-none-eabi- 前缀>
```

Keil 侧 `.sct` 由 `BeforeMake` 自动生成（`--type miniapp --dev-slot 1 --ram-size 8192`）。

## 打包成小程序并上板跑

```
python tools/pack_app.py --project example/stm32f427/app_sdk/HELLO_MINI/MDK-ARM/hello_mini.uvprojx \
        --type miniapp --no-autostart --name HELLO_MINI --out build/hello_mini.svcapp

python tools/fs_put.py --port COM3 --path /hello.svcm build/hello_mini.svcapp
```

设备侧：

```
mini run /hello.svcm
mini stat              # 两块 RAM 应当已经归还
```

`--no-autostart` 不能省：小程序默认类型是「自启」，打包工具会直接拒。

同一段源码在 arkc 档（板上编译）也是这个入口——见 `example/mini_hello/` 与
`docs/arkc板上直跑-M4-hello演示.md`。
