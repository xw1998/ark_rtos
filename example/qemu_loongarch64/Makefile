# Ark RTOS —— QEMU LoongArch64 (virt) 示例构建
#
# 用法（在仓库根目录执行）：
#   make -f example/qemu_loongarch64/Makefile        # 构建 build/qemu_loongarch64/ark_qemu_la64.elf
#   make -f example/qemu_loongarch64/Makefile run    # 构建并在 QEMU 上跑（串口落 build/qemu_loongarch64/out.txt）
#
# 约束：
#   1) 工具链只有一个 64 位 multilib（lp64d），因此全程 -nostdlib，不链 newlib；
#   2) 内核的地址字段是 uint32，本 port 在指针处显式截断（RAM 全在低 4 GB 内）；
#   3) `-serial file:` 只接受相对路径，且不接受 `-bios none`（实测，见 docs）。

TOOLCHAIN ?= D:/LoongIDE/la64-tool/bin
CROSS     ?= loongarch64-newlib-elf-
EXT       ?= .exe
CC        := $(TOOLCHAIN)/$(CROSS)gcc$(EXT)
OBJCOPY   := $(TOOLCHAIN)/$(CROSS)objcopy$(EXT)
OBJDUMP   := $(TOOLCHAIN)/$(CROSS)objdump$(EXT)
SIZE      := $(TOOLCHAIN)/$(CROSS)size$(EXT)

QEMU      ?= D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe

ARCH_FLAGS := -march=loongarch64 -mabi=lp64d

BUILD := build/qemu_loongarch64
ELF   := $(BUILD)/ark_qemu_la64.elf
LDSCR := $(BUILD)/link.ld

INCLUDES := -Ikernelsrc/include \
            -Iconfig \
            -Iboard/qemu_loongarch64 \
            -Ikernelsrc/port/loongarch/la64 \
            -Iexample/qemu_loongarch64 \
            -Ikernelsrc/components/mdk_trace \
            -Ikernelsrc/shell \
            -Ikernelsrc/shell/ark_shell \
            -Ikernelsrc/components/ark_vfs/include \
            -Ikernelsrc/components/ark_vfs/port \
            -Ikernelsrc/components/ark_vfs/src \
            -Ikernelsrc/fs \
            -Ikernelsrc/fs/littlefs

CFLAGS := $(ARCH_FLAGS) -g -O2 -ffreestanding -fno-common -nostdlib -nostartfiles \
          -fno-builtin -ffunction-sections -fdata-sections -Wall -MMD -MP \
          -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
          -include arkrtos_board_config.h \
          -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
ASFLAGS := $(ARCH_FLAGS) -g

# 通配全部内核源，再剔除本平台没有任何介质/驱动依赖的模块：
# 它们即使功能开关为 0 也在文件头无条件包含硬件头（drvflash.h / lfs.h），
# 而这套 QEMU 目标没有 Flash 控制器、也没有 littlefs 组件。
KERNEL_SRCS := $(filter-out kernelsrc/src/arkrtos_flashapp.c \
                            kernelsrc/src/arkrtos_fs.c \
                            kernelsrc/src/arkrtos_vfs_lfs.c, \
                          $(wildcard kernelsrc/src/*.c))

SRCS_C := $(KERNEL_SRCS) \
	kernelsrc/components/mdk_trace/mdk_trace_arkrtos.c \
	kernelsrc/shell/arkrtos_shell.c \
	kernelsrc/src/arkrtos_fs.c \
	kernelsrc/components/ark_vfs/src/ark_vfs_core.c \
	kernelsrc/components/ark_vfs/src/ark_vfs_ramfs.c \
	kernelsrc/sdk/app_sdk/arkrtos_oslib.c \
	kernelsrc/port/loongarch/la64/arkrtos_port.c \
	board/qemu_loongarch64/arkrtos_board.c \
	example/qemu_loongarch64/main.c \
	example/qemu_loongarch64/libc_shim.c

SRCS_S := \
	kernelsrc/port/loongarch/la64/arkrtos_context.S \
	example/qemu_loongarch64/start.S

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

$(LDCONST): example/qemu_loongarch64/gen_ldconsts.py config/arkrtos_partition.h board/qemu_loongarch64/arkrtos_board_config.h
	@mkdir -p $(BUILD)
	# 末尾的分号是必需的：MinGW 版 make 3.82 对"没有 shell 元字符"的命令走
	# CreateProcess 快路径，而本机 python 装在含非 ASCII 字符的目录里，快路径
	# 定位不到；交给 sh 才能按 PATH 正常解析。
	python example/qemu_loongarch64/gen_ldconsts.py --output $@ ;

$(LDSCR): example/qemu_loongarch64/link.ld.in $(LDCONST)
	@mkdir -p $(BUILD)
	$(CC) -E -P -x c -I$(BUILD) $< -o $@

$(ELF): $(OBJS) $(LDSCR)
	$(CC) $(ARCH_FLAGS) -T $(LDSCR) -Wl,--gc-sections -nostdlib -nostartfiles -o $@ $(OBJS)

size: $(ELF)
	$(SIZE) $<

disasm: $(ELF)
	$(OBJDUMP) -d $< > $(BUILD)/ark_qemu_la64.dis

# `-serial file:` 只接受相对路径（走 QEMU 的当前目录），所以先 cd 到构建目录。
# 不加 `-bios none`：LoongArch 的 -kernel 路径与 RISC-V 不同，实测加了反而起不来。
run: $(ELF)
	cd $(BUILD) && "$(QEMU)" -machine virt -kernel $(notdir $(ELF)) \
		-nographic -serial file:out.txt

clean:
	rm -rf $(BUILD) 2>/dev/null || true

# 让增量构建跟踪头文件依赖（含 -include 的板级配置头），
# 否则只改板级宏时旧 .o 会带着上一套开关被复用（本文件不在依赖图里）。
-include $(OBJS:.o=.d)
