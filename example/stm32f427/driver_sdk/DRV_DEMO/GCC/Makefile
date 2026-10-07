# =============================================================================
# Ark RTOS — 驱动（Driver）示例 DRV_DEMO（GNU Make + arm-none-eabi-gcc）
# -----------------------------------------------------------------------------
# 与同目录的 Keil 工程 (../MDK-ARM/drv_demo.uvprojx) 同一份文件集：
#   源清单由 tools/gen_gcc_sources.py 从 .uvprojx 派生（见 sources.mk），
#   链接脚本由 tools/gen_ld.py 从 config/arkrtos_partition.h 派生（见 build/drv_demo.ld）。
#   两者都是「地址/文件只定义一次」这条红线在 GCC 工具链下的落地，不要手改生成物。
#
# 这是「开发调试裸镜像」布局（--raw）：负载直接落在开发槽位单元基址，无镜像头、
# 无重定位表，专供 MDK/探针直接烧录 + 断点调试；RAM 窗口内最高一段留给内核设置的
# 任务栈（与 Keil 侧 scatter 同一口径，编译期不会撞栈）。
#
# 用法（在本目录下）：
#   make                  # 生成 ld + 编译 + 链接，产出 build/drv_demo.elf/.bin/.hex
#   make size             # 打印镜像体积
#   make gen-sources      # 从 .uvprojx 重新派生 sources.mk（改了 Keil 工程后跑）
#   make clean
#
# 工具链不在 PATH 上时：make CROSS=/c/Users/xxx/arm-gnu-toolchain-14.2/bin/arm-none-eabi-
# =============================================================================

ROOT     := ../../../../..
BUILD    := build
OBJDIR   := $(BUILD)/obj
TARGET   := drv_demo
LDSCRIPT := $(BUILD)/$(TARGET).ld
MAP      := $(BUILD)/$(TARGET).map
ELF      := $(BUILD)/$(TARGET).elf

# 本机私有工具链配置（不入库）
-include toolchain.local.mk

CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
SZ    := $(CROSS)size
OD    := $(CROSS)objcopy
PY    := env -u PYTHONHOME -u PYTHONPATH py -3

# 目标核：Cortex-M4F，硬浮点（必须与内核镜像的 ABI 一致）
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

DEFS := -DARKRTOS_DRV_USER_MODE

INCS := -I../Src \
  -I$(ROOT)/kernelsrc/include \
  -I$(ROOT)/kernelsrc/sdk/driver_sdk

CFLAGS := $(CPU) $(DEFS) $(INCS) -O2 -g3 -Wall -std=gnu11 \
          -ffunction-sections -fdata-sections -fno-common -Wno-unused-parameter

LDFLAGS := $(CPU) -T$(LDSCRIPT) -Wl,-Map=$(MAP) -Wl,--gc-sections \
           -Wl,--print-memory-usage --specs=nano.specs --specs=nosys.specs

include sources.mk
# GCC 专属 C 库桩：不提供堆（_sbrk 恒失败），与 Keil 侧「无堆」语义一致
SRCS := $(addprefix $(ROOT)/,$(SRCS)) \
        $(ROOT)/example/stm32f427/kernel/ARKRTOS_TEST/GCC/arkrtos_gcc_syscalls.c
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(sort $(dir $(SRCS)))

OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(basename $(notdir $(SRCS)))))

.PHONY: all size clean gen-sources bin hex
all: $(ELF) bin hex size

$(LDSCRIPT): $(ROOT)/config/arkrtos_partition.h $(ROOT)/tools/gen_ld.py
	@mkdir -p $(BUILD)
	$(PY) $(ROOT)/tools/gen_ld.py --target image --dev-slot 2 --raw --output $@

gen-sources:
	$(PY) $(ROOT)/tools/gen_gcc_sources.py \
	    --uvprojx ../MDK-ARM/drv_demo.uvprojx --out sources.mk

$(OBJDIR)/%.o: %.c
	@mkdir -p $(OBJDIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(OBJDIR)/%.o: %.s
	@mkdir -p $(OBJDIR)
	$(CC) -x assembler-with-cpp -c $(CFLAGS) $< -o $@

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
