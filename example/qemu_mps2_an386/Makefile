# Ark RTOS —— QEMU mps2-an386（Cortex-M4）示例构建
#
# 用法（在仓库根目录执行）：
#   make -f example/qemu_mps2_an386/Makefile          # 构建 build/qemu_mps2_an386/ark_qemu_m4.elf
#   make -f example/qemu_mps2_an386/Makefile run      # 构建并在 QEMU 上跑（串口落 build/qemu_mps2_an386/out.txt）
#
# 约束：工具链没有 libc，全程 -nostdlib；只链接 -lgcc（整数除法等助手）。
# 浮点：-mfloat-abi=soft，与板级 ARKRTOS_USE_FPU = 0 一致。

TOOLCHAIN ?= D:/env/tools/gnu_gcc/arm_gcc/bin
CROSS     ?= arm-none-eabi-
EXT       ?= .exe
CC        := $(TOOLCHAIN)/$(CROSS)gcc$(EXT)
OBJCOPY   := $(TOOLCHAIN)/$(CROSS)objcopy$(EXT)
OBJDUMP   := $(TOOLCHAIN)/$(CROSS)objdump$(EXT)
SIZE      := $(TOOLCHAIN)/$(CROSS)size$(EXT)

QEMU      ?= D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-arm.exe

# CMSIS 核心头（core_cm4.h / cmsis_gcc.h）。取仓库里已有的 STM32F427 工程
# 那份，避免再复制一份进仓库。
CMSIS_INC := example/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Core/Include

ARCH_FLAGS := -mthumb -mcpu=cortex-m4 -mfloat-abi=soft

BUILD := build/qemu_mps2_an386
ELF   := $(BUILD)/ark_qemu_m4.elf
LDSCR := $(BUILD)/link.ld

INCLUDES := -Ikernelsrc/include \
            -Iconfig \
            -Iboard/qemu_mps2_an386 \
            -Ikernelsrc/port/arm/cortex-m4 \
            -Iexample/qemu_mps2_an386 \
            -Ikernelsrc/components/mdk_trace \
            -Ikernelsrc/shell \
            -Ikernelsrc/shell/ark_shell \
            -Ikernelsrc/components/ark_vfs/include \
            -Ikernelsrc/components/ark_vfs/port \
            -Ikernelsrc/components/ark_vfs/src \
            -Ikernelsrc/fs \
            -Ikernelsrc/fs/littlefs \
            -Ikernelsrc/components/ark_softbus/include \
            -Ikernelsrc/components/ark_softbus/src \
            -Ikernelsrc/components/ark_softbus/port \
            -I$(CMSIS_INC) \
            -I$(BUILD)

.DEFAULT_GOAL := all

CFLAGS := $(ARCH_FLAGS) -g -O2 -ffreestanding -fno-common -nostdlib -nostartfiles \
          -fno-builtin -ffunction-sections -fdata-sections -Wall -MMD -MP \
          -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
          -include arkrtos_board_config.h \
          -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
          -DMDK_TRACE_ENABLE=1 -DMDK_TRACE_BACKEND_BUFF=1 \
          -DMDK_TRACE_USE_RDTIME=1 -DMDK_TRACE_CPU_HZ=25000000u

# .s 小写扩展名默认不走预处理器，而 port 的 GNU 版上下文切换用 #if 分档，
# 因此显式 -x assembler-with-cpp。不复制/改名仓库里的文件。
ASFLAGS := $(ARCH_FLAGS) -g -x assembler-with-cpp \
           -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
           -include arkrtos_board_config.h \
          -DMDK_TRACE_ENABLE=1 -DMDK_TRACE_BACKEND_BUFF=1 \
          -DMDK_TRACE_USE_RDTIME=1 -DMDK_TRACE_CPU_HZ=25000000u

# 内核源：通配全部，再剔除本档没有介质/驱动依赖的模块
# （它们即使功能开关为 0 也在文件头无条件包含 Flash / littlefs 头）。
KERNEL_SRCS := $(filter-out kernelsrc/src/arkrtos_flashapp.c \
                            kernelsrc/src/arkrtos_fs.c \
                            kernelsrc/src/arkrtos_vfs_lfs.c, \
                          $(wildcard kernelsrc/src/*.c)) \
	kernelsrc/src/arkrtos_fs.c

SRCS_C := $(KERNEL_SRCS) \
	kernelsrc/components/mdk_trace/mdk_trace.c \
	kernelsrc/components/mdk_trace/mdk_trace_buff.c \
	kernelsrc/components/mdk_trace/mdk_trace_arkrtos.c \
	kernelsrc/sdk/app_sdk/arkrtos_oslib.c \
	kernelsrc/port/arm/cortex-m4/arkrtos_port.c \
	board/qemu_mps2_an386/arkrtos_board.c \
	example/qemu_mps2_an386/main.c \
	example/qemu_mps2_an386/libc_shim.c \
	example/qemu_mps2_an386/kernel_stubs.c

SRCS_S := \
	kernelsrc/port/arm/cortex-m4/arkrtos_context_gcc.s \
	example/qemu_mps2_an386/start.S

OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(basename $(SRCS_C) $(SRCS_S))))

.PHONY: all clean run size disasm

all: $(ELF)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) $(INCLUDES) -c $< -o $@

# 链接脚本常量：数值由分区头 + 板级配置头求出来（与内核同源）
LDCONST := $(BUILD)/ldconsts.h

$(LDCONST): example/qemu_riscv32/gen_ldconsts.py config/arkrtos_partition.h board/qemu_mps2_an386/arkrtos_board_config.h
	@mkdir -p $(BUILD)
	# 末尾的分号是必需的：MinGW 版 make 3.82 对"没有 shell 元字符"的命令走
	# CreateProcess 快路径，而本机 python 装在含非 ASCII 字符的目录里，快路径
	# 定位不到；交给 sh 才能按 PATH 正常解析。
	python example/qemu_riscv32/gen_ldconsts.py --output $@ --board board/qemu_mps2_an386/arkrtos_board_config.h ;

$(LDSCR): example/qemu_mps2_an386/link.ld.in $(LDCONST)
	@mkdir -p $(BUILD)
	$(CC) -E -P -x c -I$(BUILD) $< -o $@

$(ELF): $(OBJS) $(LDSCR)
	$(CC) $(ARCH_FLAGS) -T $(LDSCR) -Wl,--gc-sections -nostdlib -nostartfiles -o $@ $(OBJS) -lgcc

size: $(ELF)
	$(SIZE) $<

disasm: $(ELF)
	$(OBJDUMP) -d $< > $(BUILD)/ark_qemu_m4.dis

# -kernel 只吃文件名：含中文的绝对路径 QEMU 打不开，所以先 cd 进构建目录。
run: $(ELF)
	cd $(BUILD) && "$(QEMU)" -M mps2-an386 -cpu cortex-m4 -nographic \
		-kernel $(notdir $(ELF)) -serial file:out.txt

clean:
	rm -rf $(BUILD) 2>/dev/null || true

# 让增量构建跟踪头文件依赖（含 -include 的板级配置头），
# 否则只改板级宏时旧 .o 会带着上一套开关被复用（本文件不在依赖图里）。
-include $(OBJS:.o=.d)
