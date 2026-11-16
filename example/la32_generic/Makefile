# Ark RTOS —— LoongArch32 示例构建（参考目标，只验证编译/链接）
#
# 用法（在仓库根目录执行）：
#   make -f example/la32_generic/Makefile        # 构建 build/la32_generic/ark_la32_generic.elf
#   make -f example/la32_generic/Makefile size   # 打印 text/data/bss
#   make -f example/la32_generic/Makefile run    # **只会报错**，见下
#
# 功能面：与 la64 的 QEMU 目标对齐 —— Shell（ttyS0）/ 日志环 / 块设备
#   （RAM 模拟 NOR）/ littlefs / VFS / procfs / UTEST 全部编进来，代码路径与
#   la64 是同一套。**但只有编译 + 链接这一档**，没有任何运行期读数。
#
# 为什么没有真正的 run：
#   QEMU 9.2.3 只提供 qemu-system-loongarch64，上游没有 loongarch32 系统模拟器；
#   环境里也没有 LA32 板子。因此本目标只到「链接产出 ELF」这一步。run 目标
#   故意写成一条明确的错误，而不是假装跑起来或静默成功。
#
# 约束：
#   1) 工具链 la32-tool（GCC 8.3.0）只认 -march=loongarch32，ABI 用 ilp32d；
#      ABI 允许值实测为 ilp32d/ilp32f/ilp32s + lp64*，其中 lp64* 不可用（超集）；
#   2) 全程 -nostdlib，不链 newlib（内核不用 libc，示例用自带的 libc_shim.c）；
#   3) 该工具链在 -march=loongarch32 下**不拒绝** .d 形式的指令（st.d/ld.d/
#      addi.d 都能过汇编），所以「汇编通过」不能证明代码是 32 位合法的——
#      本目录所有汇编一律只用 .w 形式，评审时按这条查。

TOOLCHAIN ?= D:/LoongIDE/la32-tool/bin
CROSS     ?= loongarch32-newlib-elf-
EXT       ?= .exe
CC        := $(TOOLCHAIN)/$(CROSS)gcc$(EXT)
OBJCOPY   := $(TOOLCHAIN)/$(CROSS)objcopy$(EXT)
OBJDUMP   := $(TOOLCHAIN)/$(CROSS)objdump$(EXT)
SIZE      := $(TOOLCHAIN)/$(CROSS)size$(EXT)

ARCH_FLAGS := -march=loongarch32 -mabi=ilp32d

BUILD := build/la32_generic
ELF   := $(BUILD)/ark_la32_generic.elf
LDSCR := $(BUILD)/link.ld

INCLUDES := -Ikernelsrc/include \
            -Iconfig \
            -Iboard/la32_generic \
            -Ikernelsrc/port/loongarch/la32 \
            -Iexample/la32_generic \
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
          -DMDK_TRACE_CPU_HZ=100000000u
ASFLAGS := $(ARCH_FLAGS) -g

# 通配全部内核源，再剔除本平台没有对应硬件的模块：flashapp 在文件头无条件
# 包含 drvflash.h（本目标没有 Flash 控制器），只有它被剔除。fs / vfs_lfs 现在
# 是打开的功能（介质由板级 drvblk.c 在 RAM 里模拟 NOR），必须编进来。
KERNEL_SRCS := $(filter-out kernelsrc/src/arkrtos_flashapp.c, \
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
	kernelsrc/components/ark_vfs/src/ark_vfs_core.c \
	kernelsrc/components/ark_vfs/src/ark_vfs_ramfs.c \
	kernelsrc/components/ark_vfs/port/ark_vfs_port_arkrtos.c \
	kernelsrc/components/ark_vfs/port/ark_vfs_devfs.c \
	kernelsrc/components/ark_vfs/port/ark_vfs_procfs.c \
	kernelsrc/fs/littlefs/lfs.c \
	kernelsrc/fs/littlefs/lfs_util.c \
	kernelsrc/sdk/app_sdk/arkrtos_oslib.c \
	kernelsrc/port/loongarch/la32/arkrtos_port.c \
	$(SOFTBUS_SRCS) \
	board/la32_generic/arkrtos_board.c \
	board/la32_generic/arkrtos_dt.c \
	board/la32_generic/drvblk.c \
	board/la32_generic/drvuart.c \
	board/la32_generic/arkrtos_bus_port.c \
	example/la32_generic/main.c \
	example/la32_generic/libc_shim.c

SRCS_S := \
	kernelsrc/port/loongarch/la32/arkrtos_context.S \
	example/la32_generic/start.S

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

$(LDCONST): example/la32_generic/gen_ldconsts.py config/arkrtos_partition.h board/la32_generic/arkrtos_board_config.h
	@mkdir -p $(BUILD)
	# 末尾的分号是必需的：MinGW 版 make 3.82 对"没有 shell 元字符"的命令走
	# CreateProcess 快路径，而本机 python 装在含非 ASCII 字符的目录里，快路径
	# 定位不到；交给 sh 才能按 PATH 正常解析。
	python example/la32_generic/gen_ldconsts.py --output $@ ;

$(LDSCR): example/la32_generic/link.ld.in $(LDCONST)
	@mkdir -p $(BUILD)
	$(CC) -E -P -x c -I$(BUILD) $< -o $@

$(ELF): $(OBJS) $(LDSCR)
	$(CC) $(ARCH_FLAGS) -T $(LDSCR) -Wl,--gc-sections -nostdlib -nostartfiles -o $@ $(OBJS)

size: $(ELF)
	$(SIZE) $<

disasm: $(ELF)
	$(OBJDUMP) -d $< > $(BUILD)/ark_la32_generic.dis

# 明确报错，而不是假装运行：没有 LA32 执行载体（见文件头）。
run: $(ELF)
	@echo "ERROR: no LoongArch32 execution target is available."
	@echo "  - QEMU 9.2.3 ships only qemu-system-loongarch64; upstream has no"
	@echo "    loongarch32 system emulator."
	@echo "  - There is no LA32 board on this machine either."
	@echo "  This target only verifies build + link: $(ELF)"

clean:
	rm -rf $(BUILD) 2>/dev/null || true

# 让增量构建跟踪头文件依赖（含 -include 的板级配置头），
# 否则只改板级宏时旧 .o 会带着上一套开关被复用（本文件不在依赖图里）。
-include $(OBJS:.o=.d)
