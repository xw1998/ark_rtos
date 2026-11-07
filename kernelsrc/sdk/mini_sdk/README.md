# Ark RTOS 小程序 SDK（mini_sdk）

小程序是**第三种用户态应用**（`type = 3`，见 `kernelsrc/include/arkrtos_app_image.h`）：
它以普通文件的形式常驻文件系统，`mini run <path>` 时被内核读进 RAM 执行，
退出后代码块与 RAM 块立刻归还——牺牲加载效率，换取"不用分区、不用安装、
改一个文件就能换一个程序"。

## 目录

| 文件 | 作用 |
|------|------|
| `arkrtos_mini_start.s` | `MINISTART` 桩：跳到 C 运行时 `__main`（.data/.bss 初始化后进 `main()`） |
| `arkrtos_mini_main.c` | **Keil / GCC 档**的 `main()` 骨架：调用 `MiniMain()`，返回后 `arkrtos_thread_exit()` 交还内存。**arkc 档不要编它**（文件里有 `#error`）：arkc 档的入口是工程自己的 `int main(void)` |
| `arkrtos_types.h` | 基础类型 + `ARKRTOS_WEAK`（与 App / 驱动 SDK 同名文件一致） |
| `arkrtos_mini_start_arkc.c` | **arkc 专用** `MINISTART` 桩：调 `__arkcc_init_globals()`（arkc 不生成数据段加载表，全局初值被编成一个函数）再进 `main()`。落在负载偏移 0，镜像的 `entry_offset` 就是 0 |
| `arkc/arkrtos_mini_rt_arkc.c` | **arkc 专用**运行时：POSIX `write()` / `_exit()` + `arkrtos_thread_exit()`，只两条 SVC（`0x1A`/3 打印、`0x1B`/3 退出），号与 `app_sdk/arkrtos_oslib.c` 逐位相同 |
| `arkc/arkrtos.h` | **arkc 专用**最小内核声明：入口约定（工程写 `int main(void)`）+ `arkrtos_thread_exit()`。不含类型全集与日志门面（那两样要 varargs / 拖长编译输入） |
| `arkc/unistd.h` | **arkc 专用** POSIX 控制台子集：`STDIN/STDOUT/STDERR_FILENO`、`write`、`_exit`；文件里如实列出没有的东西（文件 I/O / open / read / fork） |
| `arkc/stdio.h` | **arkc 专用** `printf` / `puts` / `putchar`：支持的转换与硬上限写在头里，实现见 `arkc/arkrtos_mini_rt_arkc.c` |
| `tests/test_printf_host.c` | `printf` 的宿主门禁（`gcc -m32`，24 个用例），跑 `tools/mini_sdk_printf_gate.py` |

### arkc 档为什么要单独一套

1. **没有 va_list**：arkc 的源码里写不出 `va_start`/`va_arg`，所以 `arkrtos_ulog.h`
   的格式化器在 `__ARK_CC__` 下整段排除。`printf` 不是因此就没了——SDK 用一条
   `__asm` 取参桩把 `r0-r3` 与调用方压栈的参数落到自己栈上的 12 个槽，再交给
   普通 C 函数按格式串取值（见 `arkc/arkrtos_mini_rt_arkc.c` 与 `docs/arkc板上直跑-M4-hello演示.md` §3.3）。
2. **整份负载进 RAM**：`app_sdk/arkrtos_oslib.c` 编得动（实测 17 KB 对象），
   但 8 KB 代码块里不能放一整套用不到的服务门面；`arkrtos_posix.c`（3600 行）
   更是装不下。
3. **弱符号覆盖不到同一个翻译单元**：arkc 把每个 `.c` 编成一个自包含对象，
   **同一翻译单元内**对已定义符号的调用在汇编期就绑死，链接期不再改。历史上
   `arkrtos_mini_main.c` 里那个 `ARKRTOS_WEAK void MiniMain(void) {}` 就是这样
   把工程自己的强定义斯掉的：表现是“程序创建当拍就退出、什么都不打印”，既不
   报错也没有诊断（板上实测）。所以 arkc 档干脆不要那套骨架了：**入口就是工程
   自己的 `int main(void)`**，SDK 一个默认 `main` 都不提供，没定义就是诚实的
   `unresolved symbol "main"`。

`arkrtos_oslib.c`（SVC 门面实现）**与 App SDK 共用同一份**
（`kernelsrc/sdk/app_sdk/arkrtos_oslib.c`）。两处各放一份迟早会漂移，
而这份门面本来就不区分镜像类型。

## 怎么用

**arkc 档（板上 `make`）**：工程按标准 C 写一个 `int main(void)` 就行，SDK 只提供
启动桩与运行时：

```c
#include <stdio.h>

int main(void) {
    printf("Hello, World!\n");
    return 0;
}
```

工程描述（`ark make`）里 `SRCS` 的第一份必须是启动桩，且**不要**放
`arkrtos_mini_main.c`：

```
TARGET = hello
OUT    = hello.svcm
SRCS   = /mnt/nor/start.c /mnt/nor/rt.c hello.c
INCS   = /mnt/nor
```

完整演示（怎么把文件送进 NOR、板上 `make`、`mini run`、已知限制）见
`docs/arkc板上直跑-M4-hello演示.md`；`tools/mini_deploy.py --port COM3` 一键部署。

**Keil / GCC 档**：

1. 写你自己的 `MiniMain()`：

   ```c
   #include "arkrtos.h"

   void MiniMain(void)
   {
       int32 con = arkrtos_dev_open("ttyS0", 0);
       arkrtos_dev_write(con, (void *)"hello from mini\r\n", 17);
       /* 返回即退出，内存归还 */
   }
   ```

2. 工程里加进 `mini_sdk` 的两个源文件 + `app_sdk/arkrtos_oslib.c`，
   包含路径加 `kernelsrc/sdk/mini_sdk`、`kernelsrc/sdk/app_sdk`、`kernelsrc/include`。

3. 打包（四遍差分链接 + 重定位表，与 App 同一条流水线）：

   ```
   py -3 tools/pack_app.py --project <你的工程>.uvprojx --type miniapp \
           --name MINI_DEMO --out build/mini_demo.svcapp
   ```

4. 放进文件系统并运行：

   ```
   # 主机侧：py -3 tools/fs_put.py --port COM3 --path /mini_demo.app <镜像>
   # 卷里可以建目录（shell 的 mkdir，落点是 littlefs），也可以直接放卷根
   mini run /mini_demo.app
   mini stat
   ```

## 与 App 的差别（务必知道）

| 项 | App | 小程序 |
|----|-----|--------|
| 落点 | 镜像池的固定槽位（安装 + 版本管理） | 运行期从 RAM 池借两块 pow2 块，退出即还 |
| 常驻位置 | Flash 镜像池 | 文件系统（NOR 卷） |
| 内存占用 | 代码在 Flash，RAM = 声明的 ram_size | 代码块 + RAM 块**都在 RAM**，两块同生同死 |
| 体积上限 | 池可用空间 | 代码块 ≤ `SLOT_RAM_MAX_BLOCK`，可由 `mini limit` 运行期收紧 |
| 启动命令 | `app start <slot>` | `mini run <path>` |

上限、两块布局的由来与已知限制见 `docs/小程序设计.md`。
