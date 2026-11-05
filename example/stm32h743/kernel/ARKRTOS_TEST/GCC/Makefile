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

# 交叉工具链前缀；默认取 PATH 上的 arm-none-eabi-
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
SZ    := $(CROSS)size
OD    := $(CROSS)objcopy
PY    := env -u PYTHONHOME -u PYTHONPATH py -3

# 目标核：Cortex-M7，硬浮点，双精度单元（H743 的 FPU 是 fpv5-d16）
CPU := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard

# 宏：内核开关全部显式写出，读 Makefile 就能看到这一版到底编进了什么
DEFS := \
  -DUSE_HAL_DRIVER -DSTM32H743xx \
  -DARKRTOS_USE_FPU=1 -DARKRTOS_USE_LWIP=0 -DARKRTOS_USE_SOFTBUS=0 \
  -DARKRTOS_HW_SAFE=0 \
  -DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\" \
  -DMDK_TRACE_SWD_EXTERNAL_PLATFORM -DMDK_TRACE_USE_CONFIG_FILE \
  -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
  -DARKRTOS_USE_ARKVIM=$(USE_ARKVIM) -DARKRTOS_USE_ARKC=$(USE_ARKC)

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
ARKC_DEFS := \
  -DARKCC_PROFILE=ARKCC_PROFILE_STM32_MIN \
  -DARK_POOL_SIZE=81920 \
  -DARK_SRC_BUF_SIZE=8192 \
  -DARK_ASM_BUF_SIZE=24576 \
  -DARK_CODE_BUF_SIZE=16384 \
  -DARK_LINK_BUF_SIZE=4096 \
  -DARK_OBJ_BUF_SIZE=4096 \
  -DPP_BUF_SIZE=24576 \
  -DPP_MAX_INC_DEPTH=4 \
  -DPP_INC_BUF=4096 \
  -DARK_LINK_MAX_SYMS=64 \
  -DARK_LINK_MAX_RELOCS=64

# ramfs 单文件容量与槽数。内核的 ramfs 是“每个文件一段定长缓冲”，两个
# 上限都是 #ifndef，所以属于板级决定：编译器的产物必须落得下。
# 实测（arkc，-O1）：最小 .bin 1552 B；同一份源码的 .s 是 13727 B——arkc
# 会把 __aeabi_* 运行库展开成汇编一起写出来，所以 4096 B 的槽只够 .bin，
# 写 .s 会在写满槽后返回 NOSPC（不会静默截断，但 -S -o 文件必然失败）。
# 取 16 KB/槽：13727 B 有 1.19x 余量，再大的源码靠 -S -o - 直接看屏幕。
# 槽数 3（源码 + 一份中间产物 + 最终产物）。代价 RAM 3 x 16384 = 48 KB；
# 这 16 KB 是从第 4 个槽里省出来补给编译池的，总预算不变。
RAMFS_DEFS := -DARK_RAMFS_FILE_CAP=16384 -DARK_RAMFS_MAX_FILES=3
DEFS += $(RAMFS_DEFS)

# 头文件搜索路径。
# 注意 -Iconfig/stm32h743 必须排在 -Iconfig 之前：两者都能提供
# arkrtos_partition.h，H743 的那份才是这块板的地址源头。
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
  -I$(ROOT)/kernelsrc/components/arkc

OPTFLAGS := -O2 -g3 -Wall -std=gnu11 \
            -ffunction-sections -fdata-sections -fno-common -Wno-unused-parameter

CFLAGS := $(CPU) $(DEFS) $(INCS) $(OPTFLAGS)

# arkc 的 20 个上游源用这套头文件，和内核那套**完全不重叠**，原因只有一条：
# arkc 自带一个 include/platform.h，shell 也有一个 shell/platform.h。把两者放进
# 同一个 -I 集合，就会让某一方的 "platform.h" 拿到另一方的定义（谁先谁后决定，
# 属于静默错答案）。所以上游源只看到自己的 include；两个接线文件反过来只用
# 相对路径引 arkc 的头（"include/platform.h" / "../../shell/platform.h"），
# 靠 "" 的“先找本文件目录”规则把两份分清。
ARKC_INCS := -I$(ROOT)/kernelsrc/components/arkc/include
CFLAGS_ARKC := $(CPU) $(ARKC_DEFS) $(ARKC_INCS) $(OPTFLAGS)

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

.PHONY: all size clean bin hex

all: $(ELF) bin hex size

# --- 生成物：链接脚本来自分区头（H743 那份，不是 config/ 下的默认） ---
$(LDSCRIPT): $(PART_HDR) $(ROOT)/tools/gen_ld.py
	@mkdir -p $(BUILD)
	$(PY) $(ROOT)/tools/gen_ld.py --target kernel --header $(PART_HDR_REL) --output $@

# --- 编译 ---
$(OBJDIR)/%.o: %.c
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

clean:
	rm -rf $(BUILD)
