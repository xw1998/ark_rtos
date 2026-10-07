# =============================================================================
# Ark RTOS — STM32F427 内核示例工程（GNU Make + arm-none-eabi-gcc）
# -----------------------------------------------------------------------------
# 与同目录的 Keil 工程 (../MDK-ARM/ARKRTOS_TEST.uvprojx) 同一份文件集：
#   源清单由 tools/gen_gcc_sources.py 从 .uvprojx 派生（见 sources.mk），
#   链接脚本由 tools/gen_ld.py 从 config/arkrtos_partition.h 派生（见 build/kernel.ld）。
#   两者都是「地址/文件只定义一次」这条红线在 GCC 工具链下的落地，
#   不要手改生成物。
#
# 用法（在本目录下）：
#   make                  # 生成 ld + 编译 + 链接，产出 build/ARKRTOS_TEST.elf/.bin/.hex
#   make size             # 打印固件体积
#   make gen-sources      # 从 .uvprojx 重新派生 sources.mk（改了 Keil 工程后跑）
#   make clean
#
# 工具链不在 PATH 上时：make CROSS=/c/Users/xxx/arm-gnu-toolchain-14.2/bin/arm-none-eabi-
# =============================================================================

ROOT     := ../../../../..
BUILD    := build
OBJDIR   := $(BUILD)/obj
TARGET   := ARKRTOS_TEST
LDSCRIPT := $(BUILD)/kernel.ld
MAP      := $(BUILD)/$(TARGET).map
ELF      := $(BUILD)/$(TARGET).elf

# 本机私有工具链配置（不入库）：可在 toolchain.local.mk 里写
#   CROSS := /d/ST/.../bin/arm-none-eabi-
# 或 make CROSS=... 直接覆盖。
-include toolchain.local.mk

# 交叉工具链前缀；默认取 PATH 上的 arm-none-eabi-
CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
SZ    := $(CROSS)size
OD    := $(CROSS)objcopy
PY    := env -u PYTHONHOME -u PYTHONPATH py -3

# 目标核：Cortex-M4F，硬浮点（与 Keil 工程的 FPU2 一致）
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# 宏：与 Keil 工程的 Define + MiscControls 一致
DEFS := \
  -DUSE_HAL_DRIVER -DSTM32F427xx \
  -DARKRTOS_USE_FPU=1 -DARKRTOS_USE_LWIP=1 -DARKRTOS_HW_SAFE=1 -DARKRTOS_USE_SOFTBUS=1 \
  -DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\" \
  -DMDK_TRACE_SWD_EXTERNAL_PLATFORM -DMDK_TRACE_USE_CONFIG_FILE \
  -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
  -DARK_BUS_MAX_TOPICS=12 -DARK_BUS_MAX_DEVICES=4 -DARK_BUS_MSG_MAX_SIZE=128 \
  -DARK_BUS_TOPIC_NAME_MAX=24 -DARK_BUS_QOS_MAX_SESSIONS=2 -DARK_BUS_RPC_MAX_SLOTS=4 \
  -DARK_BUS_RPC_MAX_METHODS=4 -DARK_BUS_TASK_ENABLED=0 -DARK_BUS_COLL_ENABLED=0 \
  -DARK_BUS_INFER_ENABLED=0 -DARK_BUS_STREAM_TX_SLOTS=1 -DARK_BUS_STREAM_RX_SLOTS=1 \
  -DARK_BUS_STREAM_RX_MAX=256 -DARK_BUS_STREAM_MAX_BYTES=256

# 头文件搜索路径：与 Keil 工程的 IncludePath 一致
# （本目录与 MDK-ARM 同为 ARKRTOS_TEST 的子目录，所以 ../ 与 ../../../../../ 同样成立）
INCS := \
  -I$(ROOT)/kernelsrc/components/mdk_trace \
  -I../Core/Inc \
  -I../Drivers/STM32F4xx_HAL_Driver/Inc \
  -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy \
  -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include \
  -I../Drivers/CMSIS/Include \
  -I$(ROOT)/kernelsrc/include \
  -I$(ROOT)/kernelsrc/fs \
  -I$(ROOT)/kernelsrc/fs/littlefs \
  -I$(ROOT)/board/stm32f427 \
  -I$(ROOT)/config \
  -I$(ROOT)/kernelsrc/shell \
  -I$(ROOT)/kernelsrc/shell/ark_shell \
  -I$(ROOT)/kernelsrc/components/ark_vfs/include \
  -I$(ROOT)/kernelsrc/components/ark_vfs/src \
  -I$(ROOT)/kernelsrc/components/ark_vfs/port \
  -I$(ROOT)/kernelsrc/components/lwip/src/include \
  -I$(ROOT)/kernelsrc/components/lwip/port \
  -I$(ROOT)/kernelsrc/components/ark_softbus/include \
  -I$(ROOT)/kernelsrc/components/ark_softbus/src \
  -I$(ROOT)/kernelsrc/components/ark_softbus/port

CFLAGS := $(CPU) $(DEFS) $(INCS) -O2 -g3 -Wall -std=gnu11 \
          -ffunction-sections -fdata-sections -fno-common -Wno-unused-parameter

LDFLAGS := $(CPU) -T$(LDSCRIPT) -Wl,-Map=$(MAP) -Wl,--gc-sections \
           -Wl,--enable-non-contiguous-regions \
           -Wl,--enable-non-contiguous-regions-warnings \
           -Wl,--print-memory-usage --specs=nano.specs --specs=nosys.specs

# 源清单（从 .uvprojx 派生；见 sources.mk 头部说明）
include sources.mk
# GCC 专属的 C 库桩（不提供堆：_sbrk 恒失败，避免 nosys 版无边界长高踩到栈）
SRCS := $(addprefix $(ROOT)/,$(SRCS)) arkrtos_gcc_syscalls.c
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(sort $(dir $(SRCS)))

OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(basename $(notdir $(SRCS)))))

.PHONY: all size clean gen-sources bin hex
all: $(ELF) bin hex size

# --- 生成物：链接脚本来自分区头 ---
$(LDSCRIPT): $(ROOT)/config/arkrtos_partition.h $(ROOT)/tools/gen_ld.py
	@mkdir -p $(BUILD)
	$(PY) $(ROOT)/tools/gen_ld.py --target kernel --output $@

# 改了 Keil 工程后重新派生源清单
gen-sources:
	$(PY) $(ROOT)/tools/gen_gcc_sources.py \
	    --uvprojx ../MDK-ARM/ARKRTOS_TEST.uvprojx --out sources.mk

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
