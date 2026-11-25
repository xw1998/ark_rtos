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
            -Ikernelsrc/components/ark_softbus/port \
            -I$(BUILD)

# ark_softbus MCU 档位（清单见 kernelsrc/include/arkrtos_softbus.h 顶部注释）。
# 默认值与工程值必须同档，否则 arkrtos_softbus.h 里有 #error 闸门。
# 软总线加密档位（认证 / 加密 / 重放）：唯一取值来源是 Kconfig，本工程用的
# 共享头 kernelsrc/include/arkrtos_autoconf.h 就是它的出口。组件内部只认
# ARK_BUS_*，看不到 Kconfig，所以在这里翻成 -D；两边值不一致时
# arkrtos_softbus.h 有 #error —— 编不过，而不是带着两种 struct ark_bus
# 布局上板踩内存。
BUS_CRYPTO  := $(or $(shell awk '/define ARKRTOS_USE_BUS_CRYPTO /{print $$3}' kernelsrc/include/arkrtos_autoconf.h),0)
BUS_ENCRYPT := $(or $(shell awk '/define ARKRTOS_USE_BUS_ENCRYPT /{print $$3}' kernelsrc/include/arkrtos_autoconf.h),0)
BUS_REPLAY  := $(or $(shell awk '/define ARKRTOS_USE_BUS_REPLAY /{print $$3}' kernelsrc/include/arkrtos_autoconf.h),0)

ARK_BUS_FLAGS := -DARK_BUS_MAX_TOPICS=12 -DARK_BUS_MAX_DEVICES=4 \
                 -DARK_BUS_MSG_MAX_SIZE=128 -DARK_BUS_TOPIC_NAME_MAX=24 \
                 -DARK_BUS_QOS_MAX_SESSIONS=2 -DARK_BUS_RPC_MAX_SLOTS=4 \
                 -DARK_BUS_RPC_MAX_METHODS=4 -DARK_BUS_TASK_ENABLED=0 \
                 -DARK_BUS_COLL_ENABLED=0 -DARK_BUS_INFER_ENABLED=0 \
                 -DARK_BUS_STREAM_TX_SLOTS=1 -DARK_BUS_STREAM_RX_SLOTS=1 \
                 -DARK_BUS_STREAM_RX_MAX=256 -DARK_BUS_STREAM_MAX_BYTES=256 \
                 -DARK_BUS_CRYPTO_ENABLED=$(BUS_CRYPTO) \
                 -DARK_BUS_ENCRYPT_ENABLED=$(BUS_ENCRYPT) \
                 -DARK_BUS_REPLAY_ENABLED=$(BUS_REPLAY)

# 默认目标显式写成 all：下面的密钥生成规则排在 all 之前，
# 不写这句 make 会把第一个出现的规则当成默认目标。
.DEFAULT_GOAL := all

# 镜像签名用的测试密钥（32 字节，值 0x00..0x1F，与 tools/sign_host_gate.py 的 KEY 一致）。
# 生成在构建目录里、不进仓库（.gitignore 忽略 *_sign_key_local.h）。
# 走 ARKRTOS_SIGN_KEY_LOCAL 通道而不是 -D 直接给初值：bash 会对花括号做展开。
SIGNKEY := $(BUILD)/arkrtos_sign_key_local.h

$(SIGNKEY):
	@mkdir -p $(BUILD)
	@printf '#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n#define ARKRTOS_SIGN_KEY_LOCAL_H\n#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }\n#endif\n' > $@ ;

# 额外编译选项（缺省为空，基线不变）。与 LA64 那套 Makefile 同名同义：
# 只在出「功能档」镜像时用，例如
#   make -f example/qemu_riscv32/Makefile BUILD=build/rv32_elf \
#        EXTRA_CFLAGS=-DARKRTOS_USE_ELF=1 -j8
EXTRA_CFLAGS ?=

CFLAGS := $(ARCH_FLAGS) -g -O2 -ffreestanding -fno-common -nostdlib -nostartfiles \
          -fno-builtin -ffunction-sections -fdata-sections -Wall -MMD -MP \
          -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
          -include arkrtos_board_config.h \
          -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
          $(ARK_BUS_FLAGS) -DARKRTOS_SIGN_KEY_LOCAL=1 \
          -DMDK_TRACE_ENABLE=1 -DMDK_TRACE_BACKEND_BUFF=1 \
          -DMDK_TRACE_USE_RDTIME=1 -DMDK_TRACE_CPU_HZ=10000000u \
          $(EXTRA_CFLAGS)
# 汇编侧同样收 EXTRA_CFLAGS：.S 里也有按开关分叉的段（SMP 档的次核停靠），
# 只给 CFLAGS 会让"镜像编出来了但 .S 那一半没变"。
# .S 也要吃板级配置头：SMP 档的次核停靠段用 #if (ARKRTOS_USE_SMP == 1)
# 分叉，而 ARKRTOS_SMP_SEC_STACK_SIZE 只在板级头里定义一处（见 LA64 那份
# Makefile 的同名字段）。与 CFLAGS 一样用 -include，不用 -D 逐个复制。
ASFLAGS := $(ARCH_FLAGS) -g -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
           -include arkrtos_board_config.h $(EXTRA_CFLAGS)

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
	kernelsrc/shell/arkrtos_shell_cfg.c \
	kernelsrc/shell/arkrtos_shell_diag.c \
	kernelsrc/shell/arkrtos_shell_fs.c \
	kernelsrc/shell/arkrtos_shell_image.c \
	kernelsrc/shell/arkrtos_shell_misc.c \
	kernelsrc/shell/arkrtos_shell_mod.c \
	kernelsrc/shell/arkrtos_shell_pmap.c \
	kernelsrc/shell/arkrtos_shell_ushell.c \
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

# ELF32 示例程序（hello32.elf）：按可执行窗口基址链接的静态 ELF32。
# ELF_WIN 必须与 board/qemu_riscv32/arkrtos_board_config.h 里的
# ARKRTOS_ELF_WINDOW_BASE 一致；tools/elf_gate.py 会把这两处对账 ——
# 两边不一致时装载器报的是"段在窗口外"，看起来像镜像坏了，实际是链接基址
# 挪了，最容易查错方向。
ELF_WIN     := 0x80A00000
HELLO32_SRC := example/qemu_riscv32/elf/hello32.c
HELLO32_ELF := $(BUILD)/hello32.elf
HELLO32_H   := $(BUILD)/hello32_blob.h

# 用 -Ttext-segment 而不是 -Ttext：实测 RV32 的 ld 在处理 -Ttext 时会把
# 第一个 LOAD 段**回退一页**（.text 在 0x80a00000，LOAD 却是 0x809ff000），
# 装载器的窗口判据随即报 "segment outside exec window"——看起来像装载器
# 的 bug，实际是链接基址少了一个页。
$(HELLO32_ELF): $(HELLO32_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -ffunction-sections -fdata-sections -Wl,--gc-sections \
	      -Wl,-Ttext-segment=$(ELF_WIN) -Wl,-e,_start -Wl,--strip-all -o $@ $< ;

$(HELLO32_H): $(HELLO32_ELF) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(HELLO32_ELF) \
	       --output $@ --name ark_example_hello32 ;

# 只有在开 ELF 的功能档里才需要这个 blob：默认档 main.c 里那段被
# ARKRTOS_USE_ELF 包着，不引用它。写成条件依赖，默认档的构建步骤与产物
# 与改动前完全一致（零变化）。
$(BUILD)/example/qemu_riscv32/main.o: $(if $(findstring ARKRTOS_USE_ELF=1,$(EXTRA_CFLAGS)),$(HELLO32_H),)

OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(basename $(SRCS_C) $(SRCS_S))))

# 密钥头变了就重编：它被 arkrtos_features.h 间接包含。
$(OBJS): $(SIGNKEY)

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
