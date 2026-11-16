# Ark RTOS —— QEMU RISC-V RV32 (virt) 示例构建
#
# 用法（在仓库根目录执行）：
#   make -f example/qemu_riscv32/Makefile            # 构建 build/qemu_riscv32/ark_qemu_rv32.elf
#   make -f example/qemu_riscv32/Makefile run        # 构建并在 QEMU 上跑（串口落 build/qemu_riscv32/out.txt）
#
# 约束：工具链没有 rv32imac/ilp32 的 libgcc 多库，因此全程 -nostdlib，且禁止 64 位乘除。

TOOLCHAIN ?= D:/Artchip/luban-lite-master/toolchain/bin
CROSS     ?= riscv64-unknown-elf-
EXT       ?= .exe
CC        := $(TOOLCHAIN)/$(CROSS)gcc$(EXT)
OBJCOPY   := $(TOOLCHAIN)/$(CROSS)objcopy$(EXT)
OBJDUMP   := $(TOOLCHAIN)/$(CROSS)objdump$(EXT)
SIZE      := $(TOOLCHAIN)/$(CROSS)size$(EXT)

QEMU      ?= D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-riscv32.exe

ARCH_FLAGS := -march=rv32imac -mabi=ilp32 -mcmodel=medany -mno-relax

BUILD := build/qemu_riscv32
ELF   := $(BUILD)/ark_qemu_rv32.elf
LDSCR := $(BUILD)/link.ld

INCLUDES := -Ikernelsrc/include \
            -Iconfig \
            -Iboard/qemu_riscv32 \
            -Ikernelsrc/port/riscv/rv32imac \
            -Iexample/qemu_riscv32 \
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
            -Ikernelsrc/components/ark_softbus/port

# ark_softbus MCU 档位（清单见 kernelsrc/include/arkrtos_softbus.h 顶部注释）。
# 默认值与工程值必须同档，否则 arkrtos_softbus.h 里有 #error 闸门。
ARK_BUS_FLAGS := -DARK_BUS_MAX_TOPICS=12 -DARK_BUS_MAX_DEVICES=4 \
                 -DARK_BUS_MSG_MAX_SIZE=128 -DARK_BUS_TOPIC_NAME_MAX=24 \
                 -DARK_BUS_QOS_MAX_SESSIONS=2 -DARK_BUS_RPC_MAX_SLOTS=4 \
                 -DARK_BUS_RPC_MAX_METHODS=4 -DARK_BUS_TASK_ENABLED=0 \
                 -DARK_BUS_COLL_ENABLED=0 -DARK_BUS_INFER_ENABLED=0 \
                 -DARK_BUS_STREAM_TX_SLOTS=1 -DARK_BUS_STREAM_RX_SLOTS=1 \
                 -DARK_BUS_STREAM_RX_MAX=256 -DARK_BUS_STREAM_MAX_BYTES=256

CFLAGS := $(ARCH_FLAGS) -g -O2 -ffreestanding -fno-common -nostdlib -nostartfiles \
          -fno-builtin -ffunction-sections -fdata-sections -Wall -MMD -MP \
          -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
          -include arkrtos_board_config.h \
          -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
          $(ARK_BUS_FLAGS) \
          -DMDK_TRACE_ENABLE=1 -DMDK_TRACE_BACKEND_BUFF=1 \
          -DMDK_TRACE_USE_RDTIME=1 -DMDK_TRACE_CPU_HZ=10000000u
ASFLAGS := $(ARCH_FLAGS) -g

# 内核源：显式清单（不用通配，避免默认编入本平台用不到的功能模块）
# 通配全部内核源，再剔除本平台没有任何介质/驱动依赖的模块：
# 它们即使功能开关为 0 也在文件头无条件包含硬件头（drvflash.h / lfs.h），
# 而这套 QEMU 目标没有 Flash 控制器、也没有 littlefs 组件。
KERNEL_SRCS := $(filter-out kernelsrc/src/arkrtos_flashapp.c \
                            kernelsrc/src/arkrtos_fs.c \
                            kernelsrc/src/arkrtos_vfs_lfs.c, \
                          $(wildcard kernelsrc/src/*.c))

# 软总线组件本体（与 STM32F427 工程同一份清单）；transport_sock.c 不编，
# 它需要 POSIX socket。port/ark_bus_hal_arkrtos.c 是三个板级钩子的装配层。
SOFTBUS_SRCS := $(wildcard kernelsrc/components/ark_softbus/src/ark_bus_*.c) \
                kernelsrc/components/ark_softbus/src/transport/transport_loopback.c \
                kernelsrc/components/ark_softbus/src/transport/transport_uart.c \
                kernelsrc/components/ark_softbus/port/ark_bus_hal_arkrtos.c

SRCS_C := $(KERNEL_SRCS) \
	kernelsrc/components/mdk_trace/mdk_trace.c \
	kernelsrc/components/mdk_trace/mdk_trace_buff.c \
	kernelsrc/components/mdk_trace/mdk_trace_arkrtos.c \
	kernelsrc/shell/arkrtos_shell.c \
	kernelsrc/shell/platform_arkrtos.c \
	kernelsrc/shell/ark_shell/ark_shell.c \
	kernelsrc/shell/ark_shell/ark_shell_commands.c \
	kernelsrc/shell/ark_shell/ark_shell_eval.c \
	kernelsrc/shell/ark_shell/ark_shell_history.c \
	kernelsrc/shell/ark_shell/ark_shell_line_editor.c \
	kernelsrc/shell/ark_shell/ark_shell_autocomplete.c \
	kernelsrc/src/arkrtos_fs.c \
	kernelsrc/src/arkrtos_vfs_lfs.c \
	kernelsrc/components/ark_vfs/src/ark_vfs_core.c \
	kernelsrc/components/ark_vfs/src/ark_vfs_ramfs.c \
	kernelsrc/components/ark_vfs/port/ark_vfs_port_arkrtos.c \
	kernelsrc/components/ark_vfs/port/ark_vfs_devfs.c \
	kernelsrc/components/ark_vfs/port/ark_vfs_procfs.c \
	kernelsrc/fs/littlefs/lfs.c \
	kernelsrc/fs/littlefs/lfs_util.c \
	kernelsrc/sdk/app_sdk/arkrtos_oslib.c \
	kernelsrc/port/riscv/rv32imac/arkrtos_port.c \
	$(SOFTBUS_SRCS) \
	board/qemu_riscv32/arkrtos_board.c \
	board/qemu_riscv32/arkrtos_dt.c \
	board/qemu_riscv32/drvblk.c \
	board/qemu_riscv32/drvuart.c \
	board/qemu_riscv32/arkrtos_bus_port.c \
	example/qemu_riscv32/main.c \
	example/qemu_riscv32/libc_shim.c

SRCS_S := \
	kernelsrc/port/riscv/rv32imac/arkrtos_context.S \
	example/qemu_riscv32/start.S

OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(basename $(SRCS_C) $(SRCS_S))))

.PHONY: all clean run size disasm

all: $(ELF)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) $(INCLUDES) -c $< -o $@

LDCONST := $(BUILD)/ldconsts.h

$(LDCONST): example/qemu_riscv32/gen_ldconsts.py config/arkrtos_partition.h board/qemu_riscv32/arkrtos_board_config.h
	@mkdir -p $(BUILD)
	# 末尾的分号是必需的：MinGW 版 make 3.82 对"没有 shell 元字符"的命令走
	# CreateProcess 快路径，而本机 python 装在含非 ASCII 字符的目录里，快路径
	# 定位不到；交给 sh 才能按 PATH 正常解析。
	python example/qemu_riscv32/gen_ldconsts.py --output $@ ;

$(LDSCR): example/qemu_riscv32/link.ld.in $(LDCONST)
	@mkdir -p $(BUILD)
	$(CC) -E -P -x c -I$(BUILD) $< -o $@

$(ELF): $(OBJS) $(LDSCR)
	$(CC) $(ARCH_FLAGS) -T $(LDSCR) -Wl,--gc-sections -nostdlib -nostartfiles -o $@ $(OBJS)

size: $(ELF)
	$(SIZE) $<

disasm: $(ELF)
	$(OBJDUMP) -d $< > $(BUILD)/ark_qemu_rv32.dis

run: $(ELF)
	cd $(BUILD) && "$(QEMU)" -machine virt -cpu rv32 -bios none \
		-kernel $(notdir $(ELF)) -nographic -serial file:out.txt

clean:
	rm -rf $(BUILD) 2>/dev/null || true

# 让增量构建跟踪头文件依赖（含 -include 的板级配置头），
# 否则只改板级宏时旧 .o 会带着上一套开关被复用（本文件不在依赖图里）。
-include $(OBJS:.o=.d)
