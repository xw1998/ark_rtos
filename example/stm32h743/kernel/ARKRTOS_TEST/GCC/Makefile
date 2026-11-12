# =============================================================================
# Ark RTOS — STM32H743 内核示例工程（GNU Make + arm-none-eabi-gcc）
# -----------------------------------------------------------------------------
# 与 stm32f427 的同名工程同构，差异只有四处，逐条列出以便核对：
#   1. CPU：Cortex-M7 + fpv5-d16（H7 是双精度 FPU，M4 是 fpv4-sp-d16）；
#   2. 分区头：-Iconfig/stm32h743 在前，链接脚本从同一份头派生；
#   3. 文件集：只保留 H7 实际用到的 HAL 模块，没有 lwip / softbus / mdk_trace；
#   4. 源清单：只注册 board/stm32h743 与 kernelsrc/port/arm/cortex-m7。
#
# 用法（在本目录下）：
#   make                  # 生成 ld + 编译 + 链接，产出 build/ARKRTOS_TEST.elf/.bin/.hex
#   make size             # 打印固件体积
#   make clean
#
# 工具链不在 PATH 上时：make CROSS=/d/ST/.../bin/arm-none-eabi-
# =============================================================================

ROOT     := ../../../../..
BUILD    := build
OBJDIR   := $(BUILD)/obj
TARGET   := ARKRTOS_TEST
LDSCRIPT := $(BUILD)/kernel.ld
MAP      := $(BUILD)/$(TARGET).map
ELF      := $(BUILD)/$(TARGET).elf

# gen_ld.py 把相对路径按仓库根解析（不是按当前目录），所以这里同时保留
# 两种形态：REPO 相对路径交给工具，绝对形态只用于 make 的依赖判断。
PART_HDR_REL := config/stm32h743/arkrtos_partition.h
PART_HDR     := $(ROOT)/$(PART_HDR_REL)
BOARD_DIR := $(ROOT)/board/stm32h743

# 本机私有工具链配置（不入库）：可在 toolchain.local.mk 里写
#   CROSS := /d/ST/.../bin/arm-none-eabi-
# 或 make CROSS=... 直接覆盖。
-include toolchain.local.mk

# Optional component switches (0/1). Same macro as in
# kernelsrc/include/arkrtos_features.h: written into DEFS explicitly so that
# reading this Makefile tells you what this build actually contains.  At 0 the
# component's sources are not compiled at all, so the objects and the image are
# byte-for-byte what they were without the component.
USE_ARKVIM ?= 1
USE_ARKC   ?= 1
# ark make：工程描述 -> 一次构建，打包走 arkc 的 cc_pack。默认关，因为它是
# 一个**在有编译器之后才有意义**的组件：开着却没有 USE_ARKC/USE_APPPACK 会
# 在 arkrtos_features.h 里被 #error 挡下来，而不是在板上报一句"失败了"。
USE_ARKMAKE ?= 0
# 外置 SDRAM（FMC Bank1，32MB）：1 = 起来并让编译器和 ramfs 把大块工作区
# 放到片外。这一项不只影响一个 -D：它同时决定 DEFS 里的开关、编译器的
# 缓冲预算、ramfs 的槽数与段归属，三者必须一起动，所以放在同一个开关下。
#
# 默认 0，因为这一侧的接入还没调通，不是因为这块板不行：按用户口径，FMC 外部
# 存储窗是**通的**（厂商例程在本板可跑），本侧没调通属配置问题，**不作硬件层
# 结论**（细节与现场读数见 docs/stm32h743-external-memory.md）。按用户指示
# 「先不管」冻结；置 1 后 .ark_buf 与 ramfs 文件缓冲会落到 0xC0000000 起的分区里。
# 要重开这扇门就先 make USE_SDRAM=1，再用 sdram info/verify/fsm/pins 出读数。
USE_SDRAM  ?= 0
# 板上直跑（app run）：1 = 编进的裸文件可以丢进固定窗口直接执行。
# 关窗地址与大小在分区头 ARKRTOS_APPRUN_*，
# 不在这里。默认 0：关掉时那个源文件编出为空目标，
# 镜像与命令表逐字节不变。
USE_APPRUN ?= 0
# 片内 Flash 应用区（阶段 3c）：1 = 编入 `app flash write/run/erase/info`，
# 产物烧在 Flash 应用区（分区头 ARKRTOS_FLASHAPP_*，bank2 尾部 128K），
# 掉电不丢、复位后不必重烧。依赖 USE_APPRUN=1 与 USE_MINI_VFS=1：
# 写路径从 /tmp 读文件（VFS），跑路径与 app run 抢同一段 RAM 数据窗口。
# 默认 0：关掉时那个源文件编出为空目标，镜像与命令表逐字节不变。
USE_FLASHAPP ?= 0
# 小程序读镜像的后端：0 = arkrtos_fs（littlefs 卷，与引入开关前逐字节一致）；
# 1 = VFS 命名空间（含板上编译器写产物的 /tmp ramfs）。判据在
# docs/arkc编译产物上板运行-实施计划.md 的 M3-a：默认关时行为不变。
USE_MINI_VFS ?= 0
# 可重定位镜像打包（M2）：1 = 编入四遍差分打包器，给 arkc 的 `cc --pack` 用。
# 默认 0：关掉时 arkrtos_apppack.c 不进 SRCS，镜像与命令表逐字节不变。
USE_APPPACK ?= 0
# QSPI NOR（W25Q64）取证探针：1 = 编入 drvqspi.c（寄存器级，不走 HAL：H7 的
# HAL_QSPI 会拖进 MDMA）、注册 `nor` 命令。
# 默认 0：不碰 QUADSPI 的时钟/引脚，镜像与命令表逐字节不变。
USE_QSPI ?= 0
# 小程序任务优先级。内核默认沿用 APP_TASK_PRIORITY(=10)，那在控制台(shell=14)
# 之上：小程序一旦死循环，控制台就被永久饿死，连 `mini` 都查不了——本轮实测
# 就是 `mini run /tmp/m2.svcm` 之后提示符再也不回来，只能复位。
# 本工程是演示/回归工程，默认就放到控制台之下，与 apprun 用同一条规则
# （ARKRTOS_APPRUN_PRIORITY = SHELL_TASK_PRIORITY + 6，见分区头里的实测注释）。
# 要复现内核默认行为：make MINI_PRIORITY=
MINI_PRIORITY ?= 20


# 内核组件开关（与 kernelsrc/include/arkrtos_features.h 同名宏）。
# 默认 0 与「没有这个开关」逐字节一致：arkrtos_sbuf.c 整个文件被
# #if (ARKRTOS_USE_SBUF == 1) 包住，关档编出空目标，镜像与命令表不动。
USE_SBUF ?= 0

# 交叉工具链前缀；默认取 PATH 上的 arm-none-eabi-
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
SZ    := $(CROSS)size
OD    := $(CROSS)objcopy
PY    := env -u PYTHONHOME -u PYTHONPATH py -3

# 目标核：Cortex-M7，硬浮点，双精度单元（H743 的 FPU 是 fpv5-d16）
CPU := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard

# 板级 Shell 命令总开关：只要有一条板级诊断命令编进来，命令表就要为它留位置。
# 以前这里直接写 $(USE_SDRAM)：QSPI 进来之后那个式子会静默漏掉 nor 命令。
BOARD_CMDS := 0
ifeq ($(USE_SDRAM),1)
BOARD_CMDS := 1
endif
ifeq ($(USE_QSPI),1)
BOARD_CMDS := 1
endif

# 宏：内核开关全部显式写出，读 Makefile 就能看到这一版到底编进了什么
DEFS := \
  -DUSE_HAL_DRIVER -DSTM32H743xx \
  -DARKRTOS_USE_FPU=1 -DARKRTOS_USE_LWIP=0 -DARKRTOS_USE_SOFTBUS=0 \
  -DARKRTOS_HW_SAFE=0 \
  -DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\" \
  -DMDK_TRACE_SWD_EXTERNAL_PLATFORM -DMDK_TRACE_USE_CONFIG_FILE \
  -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
  -DARKRTOS_USE_ARKVIM=$(USE_ARKVIM) -DARKRTOS_USE_ARKC=$(USE_ARKC) \
  -DARKRTOS_USE_ARKMAKE=$(USE_ARKMAKE) \
  -DARKRTOS_USE_SDRAM=$(USE_SDRAM) -DARKRTOS_USE_QSPI=$(USE_QSPI) \
  -DARKRTOS_SHELL_BOARD_CMDS=$(BOARD_CMDS) \
  -DARKRTOS_USE_APPRUN=$(USE_APPRUN) \
  -DARKRTOS_USE_FLASHAPP=$(USE_FLASHAPP) \
  -DARKRTOS_MINI_READ_VFS=$(USE_MINI_VFS) \
  -DARKRTOS_USE_APPPACK=$(USE_APPPACK) \
  -DARKRTOS_PACK_IN_EXTRAM=$(USE_SDRAM) \
  -DARKRTOS_AUTOMOUNT_NOR=$(USE_QSPI) \
  -DARKRTOS_USE_SBUF=$(USE_SBUF)

ifneq ($(MINI_PRIORITY),)
DEFS += -DARKRTOS_MINI_TASK_PRIORITY=$(MINI_PRIORITY)
endif

# Editor (arkvim) sizing. Every limit upstream is an #ifndef, so the sizes are
# decided here and not by editing upstream: the editor struct is 31KB (16K text
# + 4K line table + 9.7K undo stacks) and this kernel only owns 440KB of RAM
# with arkc still to come, so text is 8K, the line table 512 lines, undo depth
# 32. That covers a few-hundred-line C file, and it is the same macro the
# "refuse to open a file larger than the buffer" check in
# arkvim_fileio_vfs.c uses, so the two can never disagree.
ARKVIM_DEFS := \
  -DARKVIM_GB_MAX_SIZE=8192 \
  -DARKVIM_LT_MAX_LINES=512 \
  -DARKVIM_UNDO_MAX_DEPTH=32

ifeq ($(USE_ARKVIM),1)
DEFS += $(ARKVIM_DEFS)
endif

# 编译器（arkc）的尺寸。上游每个上限都是 #ifndef，所以大小在工程侧决定，
# 不改上游源码。这一套把“自举编译一个中小型 .c”放得下，同时把工作区从
# 上游默认的 64K 池 / 16K 源码 / 64K 汇编收到 178 KB 总量 —— 再多就要和
# 内核的 440 KB RAM 抢地方了。改这些值只改这一处，不会动了上游。
#
# 池的实测标尺（板上 cc 报的 pool used/total）：一个两函数的最小文件 27752 B；
# 65 K 池下把 fib+sum+switch+float 的七行 demo 编译时报 exhausted（最后一次
# 成功分配停在 54880 B）—— 所以池取 80 KB，并让 ramfs 从 4 槽回到 3 槽把
# 这 16 KB 换回来，整块工作区仍是 128 KB，RAM 占用不变。
ifeq ($(USE_SDRAM),1)
# 外置 SDRAM 上线后，编译器的预算不再和内核抢片内 RAM：工作区（.ark_buf）
# 整体落在 0xC0000000 起的 12MB 分区里（见 config/stm32h743/arkrtos_partition.h），
# 所以这里可以给到「池 4MB / 源码 512KB / 汇编 2MB / 代码 1MB」这一档，
# 也就是宿主侧默认档的同一量级，而不是为了塞进 440KB 而反复裁剪的那一套。
ARKC_DEFS := \
  -DARKCC_PROFILE=ARKCC_PROFILE_STM32_MIN \
  -DARK_POOL_SIZE=4194304 \
  -DARK_SRC_BUF_SIZE=524288 \
  -DARK_ASM_BUF_SIZE=2097152 \
  -DARK_CODE_BUF_SIZE=1048576 \
  -DARK_LINK_BUF_SIZE=262144 \
  -DARK_OBJ_BUF_SIZE=262144 \
  -DPP_BUF_SIZE=1048576 \
  -DPP_MAX_INC_DEPTH=16 \
  -DPP_INC_BUF=65536 \
  -DARM_MAX_SYMBOLS=8192 \
  -DARM_MAX_RELOCS=8192 \
  -DARK_LINK_MAX_SYMS=512 \
  -DARK_LINK_MAX_RELOCS=512
else
# 片内 RAM 档（SDRAM 不可用时）：池 80KB、工作区 164KB，
# 实测标尺写在 docs/arkc编译产物上板运行-实施计划.md 里。
#
# 预算怎么定的（2026-11-05 实测，不再靠猜）：
#  - link 缓冲 4KB -> 8KB：M2 多文件链接里单个对象已经 2.6~3.0KB，加 1KB
#    向量表就超 4KB。链接期有两个这样的缓冲（输入 buf + 输出 out_buf），
#    所以这条要 8KB。
#  - 池 72KB -> 80KB：板上实测 `cc --pack k.c` 在 72KB 上报
#    `memory pool exhausted`（用掉 72928/73728 B 还差一次分配），而同一份
#    源码在宿主 64 位版只要 62520 B——32 位下结构体变小反而让 mem_realloc 的
#    「是否为最后一次分配」判定更容易失败，多出来的就是这些不可回收的旧块。
#    所以池的标尺只能从板上读，不能从宿主读。
#  - 代码缓冲 16KB -> 8KB：这块只放**一个编译单元**的机器码（实测最小的 .bin
#    1552 B，k.c 编出 1916 B），8KB 已有 4 倍余量；放不下时汇编器会返回负码、
#    走上层 "assembly failed"，不会静默截断。
# 两项相加为零，工作区（.ark_buf）总量与上一版完全相同。
ARKC_DEFS := \
  -DARKCC_PROFILE=ARKCC_PROFILE_STM32_MIN \
  -DARK_POOL_SIZE=81920 \
  -DARK_SRC_BUF_SIZE=8192 \
  -DARK_ASM_BUF_SIZE=24576 \
  -DARK_CODE_BUF_SIZE=8192 \
  -DARK_LINK_BUF_SIZE=8192 \
  -DARK_OBJ_BUF_SIZE=4096 \
  -DPP_BUF_SIZE=24576 \
  -DPP_MAX_INC_DEPTH=4 \
  -DPP_INC_BUF=4096 \
  -DARK_LINK_MAX_SYMS=64 \
  -DARK_LINK_MAX_RELOCS=64
endif

# ramfs 单文件容量与槽数。内核的 ramfs 是“每个文件一段定长缓冲”，两个
# 上限都是 #ifndef，所以属于板级决定：编译器的产物必须落得下。
# 实测（arkc，-O1）：最小 .bin 1552 B；同一份源码的 .s 是 13727 B——arkc
# 会把 __aeabi_* 运行库展开成汇编一起写出来，所以 4096 B 的槽只够 .bin，
# 写 .s 会在写满槽后返回 NOSPC（不会静默截断，但 -S -o 文件必然失败）。
# 取 16 KB/槽：13727 B 有 1.19x 余量，再大的源码靠 -S -o - 直接看屏幕。
# 槽数 3（源码 + 一份中间产物 + 最终产物）。代价 RAM 3 x 16384 = 48 KB；
# 这 16 KB 是从第 4 个槽里省出来补给编译池的，总预算不变。
ifeq ($(USE_SDRAM),1)
# 文件缓冲放在外置 SDRAM 的 .extram 段（SDRAMFS 分区，6MB）。
# 槽数不是随手取的：演示工程 /demo 一次要同时站着 4 个源/脚本 + 2 个对象
# + 1 个 .bin（cc -c 两遍 + -link），再跑一次 make 再加 1 个 .svcm，正好 8
# 个——零余量的上限在演示现场等于「随便多一个文件就全挂」，所以给到 11。
# 11 x 512KB = 5.5MB，加节点开销仍在 6MB 分区内；12 槽 = 6MB 正好顶满，
# 装不下结构体开销，所以是 11 而不是整档。
RAMFS_DEFS := -DARK_RAMFS_FILE_CAP=524288 -DARK_RAMFS_MAX_FILES=11 \
              -DARK_RAMFS_IN_EXTRAM=1
else
RAMFS_DEFS := -DARK_RAMFS_FILE_CAP=16384 -DARK_RAMFS_MAX_FILES=3
endif
DEFS += $(RAMFS_DEFS)

# 头文件搜索路径。
# 注意 -Iconfig/stm32h743 必须排在 -Iconfig 之前：两者都能提供
# arkrtos_partition.h，H743 的那份才是这块板的地址源头。
# ark make 要两个 -I：上游的 arkmake.h 在 include/ 下，接线文件
# （arkmake_port.h / arkmake_shell_cmd.h）在组件根目录。与 arkc
# 同一种分工，只是 arkc 的上游头用 CFLAGS_ARKC 单独给。
INCS := \
  -I../Core/Inc \
  -I../Drivers/STM32H7xx_HAL_Driver/Inc \
  -I../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy \
  -I../Drivers/CMSIS/Device/ST/STM32H7xx/Include \
  -I../Drivers/CMSIS/Include \
  -I$(BOARD_DIR) \
  -I$(ROOT)/config/stm32h743 \
  -I$(ROOT)/config \
  -I$(ROOT)/kernelsrc/include \
  -I$(ROOT)/kernelsrc/fs \
  -I$(ROOT)/kernelsrc/fs/littlefs \
  -I$(ROOT)/kernelsrc/shell \
  -I$(ROOT)/kernelsrc/shell/ark_shell \
  -I$(ROOT)/kernelsrc/components/ark_vfs/include \
  -I$(ROOT)/kernelsrc/components/ark_vfs/src \
  -I$(ROOT)/kernelsrc/components/ark_vfs/port \
  -I$(ROOT)/kernelsrc/components/mdk_trace \
  -I$(ROOT)/kernelsrc/components/arkvim \
  -I$(ROOT)/kernelsrc/components/arkvim/src \
  -I$(ROOT)/kernelsrc/components/arkc \
  -I$(ROOT)/kernelsrc/components/arkmake \
  -I$(ROOT)/kernelsrc/components/arkmake/include

OPTFLAGS := -O2 -g3 -Wall -std=gnu11 \
            -ffunction-sections -fdata-sections -fno-common -Wno-unused-parameter

# 头文件依赖自动跟踪。没有它，改一个结构体会只重编直接改过的 .c，
# 其它译以旧布局访问同一个对象——链接照过，运行期越界写，表现是乱码后
# BusFault（2026-11-05 踩过一次：ark_shell_t 加一个字段，arkrtos_shell.o
# 还是旧布局，help 打出乱码随即 PANIC BUS）。
# 注意它只管头文件；改 -D/开关仍然必须 make clean，因为编译选项不在依赖里。
DEPFLAGS := -MMD -MP

CFLAGS := $(CPU) $(DEFS) $(INCS) $(OPTFLAGS) $(DEPFLAGS)

# arkc 的 20 个上游源用这套头文件，和内核那套**完全不重叠**，原因只有一条：
# arkc 自带一个 include/platform.h，shell 也有一个 shell/platform.h。把两者放进
# 同一个 -I 集合，就会让某一方的 "platform.h" 拿到另一方的定义（谁先谁后决定，
# 属于静默错答案）。所以上游源只看到自己的 include；两个接线文件反过来只用
# 相对路径引 arkc 的头（"include/platform.h" / "../../shell/platform.h"），
# 靠 "" 的“先找本文件目录”规则把两份分清。
ARKC_INCS := -I$(ROOT)/kernelsrc/components/arkc/include
CFLAGS_ARKC := $(CPU) $(ARKC_DEFS) $(ARKC_INCS) $(OPTFLAGS) $(DEPFLAGS)

LDFLAGS := $(CPU) -T$(LDSCRIPT) -Wl,-Map=$(MAP) -Wl,--gc-sections \
           -Wl,--enable-non-contiguous-regions \
           -Wl,--enable-non-contiguous-regions-warnings \
           -Wl,--print-memory-usage --specs=nano.specs --specs=nosys.specs

# 源清单（见 sources.mk 头部说明）
include sources.mk
# GCC 专属的 C 库桩（不提供堆：_sbrk 恒失败，避免 nosys 版无边界长高踩到栈）
SRCS := $(addprefix $(ROOT)/,$(SRCS)) arkrtos_gcc_syscalls.c
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(sort $(dir $(SRCS)))

OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(basename $(notdir $(SRCS)))))

# 上游 arkc 源（components/arkc/src/**）单独用 CFLAGS_ARKC 编译；接线文件
# （arkc_port_arkrtos.c / arkc_shell_cmd.c，在组件根目录）仍走普通 CFLAGS，
# 因为它们要的就是内核头。object 名按 basename 生成，所以这里也用同一
# 种写法取回来。
# （用 findstring 而不是 filter：make 的 filter 模式只支持一个 %，
#  "%/components/arkc/src/%" 会一个也匹配不到，静默变成空清单。）
ARKC_SRCS := $(foreach s,$(SRCS),$(if $(findstring /components/arkc/src/,$(s)),$(s)))
ARKC_OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(basename $(notdir $(ARKC_SRCS)))))
$(ARKC_OBJS): CFLAGS := $(CFLAGS_ARKC)

# 两个接线文件用内核 CFLAGS，但必须看到同一套 ARKC_DEFS：它们会读
# ARK_POOL_SIZE（cc 报池用量）与 ARKCC_* 开关，不带就会按上游默认的
# HOST_FULL 分支算出一个 64 MB 的分母——数字看着正常、其实是错的。
# 这里只追加 -D，不追加 -I，所以不会抢掉 platform.h 的归属。
ARKC_WIRE_OBJS := $(OBJDIR)/arkc_port_arkrtos.o $(OBJDIR)/arkc_shell_cmd.o
$(ARKC_WIRE_OBJS): CFLAGS += $(ARKC_DEFS)

.PHONY: all size clean bin hex FORCE

all: $(ELF) bin hex size

# --- 生成物：链接脚本来自分区头（H743 那份，不是 config/ 下的默认） ---
# --ext-ram 也是输入：只改 USE_SDRAM 而 kernel.ld 比分区头新时，make 原来不会
# 重新生成，于是 .ark_buf 仍落在片内 RAM——链接照样成功、镜像照样跑，只是
# 编译器的池还留在 80KB 那一档（实测：USE_SDRAM=1 的内存报表与 =0 逐字节同）。
# 把开关值存成 stamp 参与依赖，切换开关就一定会重生成。
# 开关指纹：内容覆盖所有会改变镜像的 -D（make 不跟踪 -D，只有把值写成
# 文件参与依赖，切换开关才会真的重编）。只改 USE_SDRAM 而旧 .o 被当成最新
# 的，实测过两次：一次是 .ark_buf 含在片内 RAM（池只有 80 KB），一次是
# USE_APPPACK=1 但 `cc --pack` 报「not supported by this build」。
# ramfs 的两个数字同样是 -D。它们不进指纹时，改了 Makefile 里的槽数而
# 旧 .o 被当成最新的，板上仍是旧上限——本轮就踩过一次（改成 11 后
# 只有边缘文件的体积变了，说明只有那一个 .o 被重编）。
ifeq ($(USE_SDRAM),1)
ARK_RAMFS_SIG = 512k-11
else
ARK_RAMFS_SIG = 16k-3
endif

SWITCH_SIG = ark=$(USE_ARKC) vim=$(USE_ARKVIM) make=$(USE_ARKMAKE) sdram=$(USE_SDRAM) apprun=$(USE_APPRUN) flashapp=$(USE_FLASHAPP) apppack=$(USE_APPPACK) qspi=$(USE_QSPI) automount=$(USE_QSPI) ramfs=$(ARK_RAMFS_SIG) mini=$(MINI_PRIORITY)

$(BUILD)/.ldcfg: FORCE
	@mkdir -p $(BUILD)
	@echo "$(SWITCH_SIG)" | cmp -s - $@ || echo "$(SWITCH_SIG)" > $@

$(LDSCRIPT): $(PART_HDR) $(ROOT)/tools/gen_ld.py $(BUILD)/.ldcfg
	@mkdir -p $(BUILD)
	$(PY) $(ROOT)/tools/gen_ld.py --target kernel --header $(PART_HDR_REL) \
		--ext-ram $(USE_SDRAM) --output $@

# --- 编译 ---
# .ldcfg 也作为依赖：USE_SDRAM 同时改 ARKC_DEFS 与 ARK_RAMFS_*，而 make
# 不跟踪 -D（注释在文件头写过这条限制），所以切换开关时旧 .o 会被当成最新
# 的——实测过：USE_SDRAM=1 但 arkc 仍按 80KB 池编译，编译器一跑就 exhausted。
$(OBJDIR)/%.o: %.c $(BUILD)/.ldcfg
	@mkdir -p $(OBJDIR)
	$(CC) -c $(CFLAGS) $< -o $@

# 汇编（含 GNU as 版上下文切换 + CMSIS gcc 启动）：统一强制预处理
$(OBJDIR)/%.o: %.s
	@mkdir -p $(OBJDIR)
	$(CC) -x assembler-with-cpp -c $(CFLAGS) $< -o $@

# --- 链接 ---
$(ELF): $(OBJS) $(LDSCRIPT)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

bin: $(ELF)
	$(OD) -O binary $(ELF) $(BUILD)/$(TARGET).bin

hex: $(ELF)
	$(OD) -O ihex $(ELF) $(BUILD)/$(TARGET).hex

size: $(ELF)
	@$(SZ) $(ELF)

# 各目标文件的 .d 由 -MMD 生成，随对象一起放在 $(OBJDIR)。
-include $(OBJS:.o=.d) $(ARKC_OBJS:.o=.d)

# 空规则：让 .ldcfg 的配方每次都跑（内容不变时 cmp 保证 mtime 不动，
# 于是依赖它的东西不会无故重建）。
FORCE:

clean:
	rm -rf $(BUILD)
