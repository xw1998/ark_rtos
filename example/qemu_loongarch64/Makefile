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

# -mstrict-align：LA64 的普通 ld/st 要求自然对齐。内核与 VFS/littlefs 里
# 有 u8 后面跟 u16/u32 的结构（例如 ark_ramfs_dir），GCC 默认会把相邻的
# u8 字段合并成一条 st.w，落在 offset 2 上就是一条非对齐 32 位写，QEMU 会
# 以 ADEF(ecode=8, EsubCode=1) 停机。加了这条 GCC 只生成对齐访问 + 逐字节
# 拼接（必要时用 ldx/stx 非对齐指令），与硬件对得上。
ARCH_FLAGS := -march=loongarch64 -mabi=lp64d -mstrict-align

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
            -Ikernelsrc/fs/littlefs \
            -Ikernelsrc/components/ark_softbus/include \
            -Ikernelsrc/components/ark_softbus/src \
            -Ikernelsrc/components/ark_softbus/port \
            -I$(BUILD)

# ark_softbus MCU 档位（清单见 kernelsrc/include/arkrtos_softbus.h 顶部注释）。
# 默认值与工程值必须同档，否则 arkrtos_softbus.h 里有 #error 闸门。
ARK_BUS_FLAGS := -DARK_BUS_MAX_TOPICS=12 -DARK_BUS_MAX_DEVICES=4 \
                 -DARK_BUS_MSG_MAX_SIZE=128 -DARK_BUS_TOPIC_NAME_MAX=24 \
                 -DARK_BUS_QOS_MAX_SESSIONS=2 -DARK_BUS_RPC_MAX_SLOTS=4 \
                 -DARK_BUS_RPC_MAX_METHODS=4 -DARK_BUS_TASK_ENABLED=0 \
                 -DARK_BUS_COLL_ENABLED=0 -DARK_BUS_INFER_ENABLED=0 \
                 -DARK_BUS_STREAM_TX_SLOTS=1 -DARK_BUS_STREAM_RX_SLOTS=1 \
                 -DARK_BUS_STREAM_RX_MAX=256 -DARK_BUS_STREAM_MAX_BYTES=256

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

CFLAGS := $(ARCH_FLAGS) -g -O2 -ffreestanding -fno-common -nostdlib -nostartfiles \
          -fno-builtin -ffunction-sections -fdata-sections -Wall -MMD -MP \
          -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
          -include arkrtos_board_config.h \
          -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
          -DMDK_TRACE_ENABLE=1 -DMDK_TRACE_BACKEND_BUFF=1 -DMDK_TRACE_CPU_HZ=100000000u \
          $(ARK_BUS_FLAGS) -DARKRTOS_SIGN_KEY_LOCAL=1
ASFLAGS := $(ARCH_FLAGS) -g

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
	kernelsrc/port/loongarch/la64/arkrtos_port.c \
	$(SOFTBUS_SRCS) \
	board/qemu_loongarch64/arkrtos_board.c \
	board/qemu_loongarch64/arkrtos_dt.c \
	board/qemu_loongarch64/drvblk.c \
	board/qemu_loongarch64/drvuart.c \
	board/qemu_loongarch64/arkrtos_bus_port.c \
	example/qemu_loongarch64/main.c \
	example/qemu_loongarch64/elf_demo.c \
	example/qemu_loongarch64/mod_demo.c \
	example/qemu_loongarch64/libc_shim.c

SRCS_S := \
	kernelsrc/port/loongarch/la64/arkrtos_context.S \
	example/qemu_loongarch64/start.S

OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(basename $(SRCS_C) $(SRCS_S))))

# 密钥头变了就重编：它被 arkrtos_features.h 间接包含。
$(OBJS): $(SIGNKEY)

# ELF 示例程序（hello.elf）：按窗口基址链接的标准 ELF64 静态可执行文件。
# 它走的是装载器真正读的那条路（VFS 里的一个文件），所以链接基址必须落在
# 板级给的窗口里；入口固定 _start，不链启动代码/libc。
#
# QEMU 没有宿主文件注入通道（ramfs 是启动时在 RAM 里 format 出来的），
# 所以再把它转成 C 数组编进镜像，由演示任务写进 ramfs —— 数组只是送文件的
# 手段，不是装载器的输入。
HELLO_SRC := example/qemu_loongarch64/elf/hello.c
HELLO_ELF := $(BUILD)/hello.elf

# 签名门（ARKRTOS_USE_EXEC_SIGN）：签名必须在**生成 blob 之前**做完。
# 先转数组后签名，镜像里装的就是未签名的副本，板端会拒绝自己刚写进去的
# 程序——这种假红最容易让人以为是装载器坏了。所以链路固定成
#   hello.elf（链接产物，未签名） -> hello.signed.elf（+32 字节 tag） -> blob
HELLO_SIGNED := $(BUILD)/hello.signed.elf
HELLO_H   := $(BUILD)/hello_blob.h

# 反例：同一份已签名文件把 tag 末字节翻掉。演示任务拿它去 exec，必须被拒
# （ERR_SIGN）；否则「签名门开着」只是文档里的一句话。
HELLO_TAMPERED := $(BUILD)/hello_tampered.elf
HELLO_BAD_H    := $(BUILD)/hello_bad_blob.h

$(HELLO_ELF): $(HELLO_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -ffunction-sections -fdata-sections -Wl,--gc-sections \
	      -Wl,-Ttext=0x02000000 -Wl,-e,_start -Wl,--strip-all \
	      -Wl,-z,max-page-size=4096 -o $@ $< ;

$(HELLO_SIGNED): $(HELLO_ELF) tools/sign_payload.py
	python tools/sign_payload.py sign $(HELLO_ELF) --out $@ ;

$(HELLO_TAMPERED): $(HELLO_SIGNED) tools/sign_payload.py
	python tools/sign_payload.py tamper $(HELLO_SIGNED) --out $@ ;

$(HELLO_H): $(HELLO_SIGNED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(HELLO_SIGNED) \
	       --output $@ --name ark_example_hello ;

$(HELLO_BAD_H): $(HELLO_TAMPERED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(HELLO_TAMPERED) \
	       --output $@ --name ark_example_hello_tampered ;

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(HELLO_H) $(HELLO_BAD_H)

# VxWorks 式示例模块（modhello.o）：这里**只编译不链接**——它就该是一个
# ET_REL 目标文件，地址留给板上装载器去填。所以规则里没有 -Ttext，也没有
# 任何链接步骤；如果哪天有人给它加了链接，这个示例就证明不了"装载期
# 定址"这件事了。
MOD_SRC := example/qemu_loongarch64/mod/modhello.c
MOD_OBJ := $(BUILD)/modhello.o
# 与 hello 同一条链路：签名之后再生成 blob（见上面 hello 的注释）。
MOD_SIGNED := $(BUILD)/modhello.signed.o
MOD_H   := $(BUILD)/modhello_blob.h
MOD_TAMPERED := $(BUILD)/modhello_tampered.o
MOD_BAD_H    := $(BUILD)/modhello_bad_blob.h

$(MOD_OBJ): $(MOD_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -ffreestanding -fno-builtin -fno-common -Wall \
	      -c $< -o $@ ;

$(MOD_SIGNED): $(MOD_OBJ) tools/sign_payload.py
	python tools/sign_payload.py sign $(MOD_OBJ) --out $@ ;

$(MOD_TAMPERED): $(MOD_SIGNED) tools/sign_payload.py
	python tools/sign_payload.py tamper $(MOD_SIGNED) --out $@ ;

$(MOD_H): $(MOD_SIGNED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(MOD_SIGNED) \
	       --output $@ --name ark_example_modhello ;

$(MOD_BAD_H): $(MOD_TAMPERED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(MOD_TAMPERED) \
	       --output $@ --name ark_example_modhello_tampered ;

$(BUILD)/example/qemu_loongarch64/mod_demo.o: $(MOD_H) $(MOD_BAD_H)

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
