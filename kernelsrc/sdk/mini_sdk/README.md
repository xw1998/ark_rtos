# Ark RTOS 小程序 SDK（mini_sdk）

小程序是**第三种用户态应用**（`type = 3`，见 `kernelsrc/include/arkrtos_app_image.h`）：
它以普通文件的形式常驻文件系统，`mini run <path>` 时被内核读进 RAM 执行，
退出后代码块与 RAM 块立刻归还——牺牲加载效率，换取"不用分区、不用安装、
改一个文件就能换一个程序"。

## 目录

| 文件 | 作用 |
|------|------|
| `arkrtos_mini_start.s` | `MINISTART` 桩：跳到 C 运行时 `__main`（.data/.bss 初始化后进 `main()`） |
| `arkrtos_mini_main.c` | `main()` 骨架：调用 `MiniMain()`，返回后 `arkrtos_thread_exit()` 交还内存 |
| `arkrtos_types.h` | 基础类型 + `ARKRTOS_WEAK`（与 App / 驱动 SDK 同名文件一致） |
| `arkrtos_mini_start_arkc.c` | **arkc 专用** `MINISTART` 桩：调 `__arkcc_init_globals()`（arkc 不生成数据段加载表，全局初值被编成一个函数）再进 `main()`。落在负载偏移 0，镜像的 `entry_offset` 就是 0 |
| `arkc/arkrtos_mini_rt_arkc.c` | **arkc 专用**运行时：POSIX `write()` / `_exit()` + `arkrtos_thread_exit()`，只两条 SVC（`0x1A`/3 打印、`0x1B`/3 退出），号与 `app_sdk/arkrtos_oslib.c` 逐位相同 |
| `arkc/arkrtos.h` | **arkc 专用**最小内核声明：`MiniMain()` 声明 + `arkrtos_thread_exit()`。不含类型全集与日志门面（那两样要 varargs / 拖长编译输入） |
| `arkc/unistd.h` | **arkc 专用** POSIX 控制台子集：`STDIN/STDOUT/STDERR_FILENO`、`write`、`_exit`；文件里如实列出没有的东西（stdio / open / read / fork） |

### arkc 档为什么要单独一套

1. **没有 varargs**：`arkrtos_ulog.h` 的格式化器在 `__ARK_CC__` 下整段排除，
   所以小程序里没有 `printf`，输出就是 `write()`。
2. **整份负载进 RAM**：`app_sdk/arkrtos_oslib.c` 编得动（实测 17 KB 对象），
   但 8 KB 代码块里不能放一整套用不到的服务门面；`arkrtos_posix.c`（3600 行）
   更是装不下。
3. **弱符号覆盖不到同一个翻译单元**：arkc 把每个 `.c` 编成一个自包含对象，
   **同一翻译单元内**对已定义符号的调用在汇编期就绑死，链接期不再改。所以
   `arkrtos_mini_main.c` 里那个 `ARKRTOS_WEAK void MiniMain(void) {}` 在 arkc
   档被 `#if !defined(__ARK_CC__)` 去掉：留着它，工程自己的强 `MiniMain()`
   永远进不来，表现是"程序创建当拍就退出、什么都不打印"，既不报错也没有
   诊断（板上实测）。arkc 档于是**必须**由工程定义 `MiniMain()`，没定义就是
   一个诚实的 `unresolved symbol "MiniMain"`。

`arkrtos_oslib.c`（SVC 门面实现）**与 App SDK 共用同一份**
（`kernelsrc/sdk/app_sdk/arkrtos_oslib.c`）。两处各放一份迟早会漂移，
而这份门面本来就不区分镜像类型。

## 怎么用

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
   # 文件系统里没有 mkdir 命令，路径直接用卷根下的文件名
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
