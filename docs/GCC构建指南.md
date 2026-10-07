# GCC 构建指南（GNU Make + arm-none-eabi-gcc）

Ark RTOS 除了 Keil MDK（AC5/AC6）工程之外，为 **STM32F427** 例程提供了一套
**GNU Make + arm-none-eabi-gcc** 工程，用于「不想装 MDK / 想在 CI 里编译 /
想用命令行 + 开源工具链」的场景。

两份工程**共用同一份源码文件集**：源清单由 `tools/gen_gcc_sources.py` 从 `.uvprojx`
派生，链接脚本由 `tools/gen_ld.py` 从 `config/arkrtos_partition.h` 派生。
这是「地址只定义一次、其余全部派生」这条红线在 GCC 工具链下的落地——
**生成物（`sources.mk` / `*.ld`）不要手改**，改 Keil 工程或配置头后重新生成。

## 1. 工程位置

| 工程 | 目录 | 产物 |
|---|---|---|
| 内核 | `example/stm32f427/kernel/ARKRTOS_TEST/GCC/` | `build/ARKRTOS_TEST.elf/.bin/.hex` |
| 应用示例 | `example/stm32f427/app_sdk/APP_DEMO/GCC/` | `build/app_demo.elf/.bin/.hex` |
| 驱动示例 | `example/stm32f427/driver_sdk/DRV_DEMO/GCC/` | `build/drv_demo.elf/.bin/.hex` |
| 小程序示例 | `example/stm32f427/app_sdk/MINI_DEMO/GCC/` | `build/mini_demo.elf/.bin/.hex` |

## 2. 工具链

需要 `arm-none-eabi-gcc`（含 `ld` / `objcopy` / `size`）与 GNU Make。

- **GCC**：任意 arm-none-eabi-gcc（本机验证用的是 STM32CubeIDE 自带的
  `arm-none-eabi-gcc 12.3.rel1`）。GNU ld 必须 **≥ 2.35**（内核链接用到
  `--enable-non-contiguous-regions`，见 §5）。
- **Make**：GNU Make 3.82（本机为 MinGW make）及以上。

工具链不在 `PATH` 上时，二选一：

```bash
# 方式一：命令行覆盖（临时）
make CROSS=/path/to/bin/arm-none-eabi-

# 方式二：写进本机私有配置（不入库，已被 .gitignore 忽略）
echo 'CROSS := D:/path/to/bin/arm-none-eabi-' > toolchain.local.mk
```

> Windows 路径建议用 **Windows 风格**（`D:/...`）而不是 MSYS 风格（`/d/...`）：
> GNU Make 对不含 shell 元字符的配方行会走 `CreateProcess`，MSYS 路径会导致
> `make (e=2): 系统找不到指定的文件`。

## 3. 用法（在各工程 GCC 目录下）

```bash
make              # 生成 ld + 编译 + 链接，产出 elf/bin/hex + 打印体积
make size         # 只打印固件体积
make gen-sources  # 从 .uvprojx 重新派生 sources.mk（改了 Keil 工程后跑）
make clean
```

编译内核后可直接用 pyocd 烧录验证：

```bash
pyocd flash -t stm32f427vgtx --format hex build/ARKRTOS_TEST.hex
```

## 4. 与 Keil 工程的对应关系

| 关注点 | Keil 侧 | GCC 侧 |
|---|---|---|
| 源清单 | `.uvprojx` 文件集 | `sources.mk`（`gen_gcc_sources.py` 派生） |
| 链接布局 | `build/*.sct`（`gen_scatter.py` 派生） | `build/*.ld`（`gen_ld.py` 派生） |
| 启动入口 | `arkrtos_*_start.s`（armasm，跳 `__main`） | `arkrtos_*_start_gcc.s`（GNU as，自做 `.data` 拷贝 + `.bss` 清零 + `__libc_init_array`） |
| 上下文切换 | `arkrtos_context.s` | `arkrtos_context_gcc.s`（**小写 `.s`**，见 §5） |
| 入口符号 | App=`APPSTART` 驱动=`DRVSTART` 小程序=`MINISTART` | **同名**，两边必须一致 |

`gen_gcc_sources.py` 在派生源清单时，把 Keil 侧的启动文件与上下文切换文件
替换成对应的 GCC 版（`REPLACE` 表），所以两份工程能取到同一批业务源文件。

## 5. 三个必须知道的坑（GCC 侧特有）

**① 汇编扩展名统一用小写 `.s`。**
GNU Make 在 Windows 上同时存在 `%.s` 与 `%.S` 两条模式规则时会报
`No rule to make target`。因此仓库里的汇编一律用小写 `.s`；
差集预处理由 `-x assembler-with-cpp` 强制打开（与扩展名大小写无关）。

**② 内核 `.bss` 溢进 CCM 靠 `--enable-non-contiguous-regions`。**
Keil 侧靠 scatter 的 `.ANY (+RW +ZI)` 在 `RW_KERNEL`(56K) 填满后**自动溢写**进
`RW_CCM`(64K)；GNU ld 不会自动跨区。`gen_ld.py` 因此把 `.bss` 拆成
`.bss >RAM` 与 `.bss.ccm >CCMRAM` 两段，并在链接命令行加
`--enable-non-contiguous-regions(-warnings)` 复现同一填充顺序。
**配套要求**：启动文件必须**同时**清零 `_sbss.._ebss` 与 `_sbss_ccm.._ebss_ccm`
两段（否则单段清零循环会跨过不连续空洞）。

**③ 不放堆：自带恒失败的 `_sbrk`。**
`--specs=nosys.specs` 的 `_sbrk` **没有上界检查**（反汇编确认：首次用 `end`
初始化堆指针，之后只 `heap_end += incr`），一旦 stdio 链条拉进来就会踩栈。
故工程附带 `arkrtos_gcc_syscalls.c`，提供恒失败（`ENOMEM`）的 `_sbrk`，
与 Keil 侧「无堆」语义一致；同时消除 `undefined reference to 'end'`。

## 6. 镜像（App/驱动/小程序）的 GCC 布局

镜像目录的 Makefile 用 `gen_ld.py --target image` 生成链接脚本，
按 **开发调试裸镜像**（`--raw`）布局：负载直接落在开发槽位单元基址，
无镜像头、无重定位表，供 MDK/探针直接烧录 + 断点调试。

- `APP_DEMO` → `--dev-slot 1 --raw`（App 类型）
- `DRV_DEMO` → `--dev-slot 2 --raw`（驱动类型）
- `MINI_DEMO` → `--type miniapp --unit 3 --units 1 --raw`

`--dev-slot N` 从 `config/arkrtos_partition.h` 的开发槽位表取
「单元/单元数/类型」，与 `gen_scatter.py --dev-slot N` 语义一致，
保证两侧取到同一块区间与同一栈大小。

RAM 窗口内**最高一段留给内核设置的任务栈**（App/小程序 4K、驱动 1K）：
`gen_ld.py` 把 `.data/.bss` 区限制在窗口减栈大小，栈顶由内核在装载时设置——
与 Keil 侧 scatter 的 `RW_IMG`（上限已扣栈）+ `ARM_LIB_STACK` 同一口径，
**编译期就撞不到栈**。

> 注意：以上是「开发调试裸镜像」布局。要走**安装路径**（`.svcapp`，
> 由内核动态装载）时，镜像需要按 `gen_ld.py --target image`（不带 `--raw`，
> 含镜像头偏移）或标称路径链接，并由打包工具生成重定位表——这部分 GCC 侧
> 尚未接入（见 §7）。

## 7. 验证状态

| 项 | 状态 |
|---|---|
| 内核 GCC 工程 | **F427 上板验证通过**（shell `version` / `info` / `mpool test` 9/9） |
| App / 驱动 / 小程序 GCC 工程 | **仅编译通过**（链接成功、入口符号与内存布局已核对；未做烧录/装载运行验证） |

未做项：GCC 侧镜像的**安装路径打包**（`.svcapp` 头 + 重定位表）尚未实现，
目前只覆盖「调试裸镜像」；后续如需要，可在 `gen_ld.py` 补标称（nominal）路径
并接入 `tools/pack_app.py`。
