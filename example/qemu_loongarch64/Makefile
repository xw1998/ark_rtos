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

# 控制台登录（SEC-G01）的凭据头：盐 16 字节 + sha256(盐 || 口令)。
# 同样生成在构建目录、不进仓库；只有 EXTRA_CFLAGS 里显式开了
# ARKRTOS_USE_SHELL_AUTH=1 时才生成、才加 -DARKRTOS_SHELL_AUTH_LOCAL=1，
# 默认档的编译命令行与产物逐字节不变。
# 这里的演示口令是 arkrtos-demo（源码注释里明写，本来就是演示凭据）；
# 真板子用 tools/shell_auth_cred.py 自己生成一对，别抄这一份。
SHELL_AUTH_HDR := $(BUILD)/arkrtos_shell_auth_local.h
ARK_AUTH_ON    := $(findstring ARKRTOS_USE_SHELL_AUTH=1,$(EXTRA_CFLAGS))

ifneq ($(ARK_AUTH_ON),)
ARK_AUTH_FLAGS := -DARKRTOS_SHELL_AUTH_LOCAL=1
endif

$(SHELL_AUTH_HDR):
	@mkdir -p $(BUILD)
	@printf '#ifndef ARKRTOS_SHELL_AUTH_LOCAL_H\n#define ARKRTOS_SHELL_AUTH_LOCAL_H\n/* demo credentials: salt "ark-rtos-console", password "arkrtos-demo"; regenerate with tools/shell_auth_cred.py */\n#define ARKRTOS_SHELL_AUTH_SALT { 0x61, 0x72, 0x6B, 0x2D, 0x72, 0x74, 0x6F, 0x73, 0x2D, 0x63, 0x6F, 0x6E, 0x73, 0x6F, 0x6C, 0x65 }\n#define ARKRTOS_SHELL_AUTH_DIGEST { 0x04, 0xE5, 0xF6, 0x9B, 0x6F, 0x84, 0xB7, 0xAB, 0xD7, 0x5F, 0x71, 0xA9, 0xA8, 0x9C, 0xDA, 0x92, 0x12, 0xD7, 0x65, 0x56, 0x27, 0x8B, 0xD2, 0x60, 0xE2, 0x88, 0x14, 0x9A, 0x93, 0x2D, 0x87, 0x16 }\n#endif\n' > $@ ;

# 额外的 -D（测试变体用，例如按需缺页单独出一份镜像）。缺省为空，基线不变。
EXTRA_CFLAGS ?=

CFLAGS := $(ARCH_FLAGS) -g -O2 -ffreestanding -fno-common -nostdlib -nostartfiles \
          -fno-builtin -ffunction-sections -fdata-sections -Wall -MMD -MP \
          -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
          -include arkrtos_board_config.h \
          -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
          -DMDK_TRACE_ENABLE=1 -DMDK_TRACE_BACKEND_BUFF=1 -DMDK_TRACE_CPU_HZ=100000000u \
          $(ARK_BUS_FLAGS) -DARKRTOS_SIGN_KEY_LOCAL=1 $(ARK_AUTH_FLAGS) $(EXTRA_CFLAGS)
# 汇编走预处理器（.S 的大写后缀已经打开了 cpp），所以板级宏在 .S 里也可见 ——
# 按需缺页要按开关决定「refill 入口是否保存通用寄存器」，只能这样做。
# 「默认关时体积与行为零变化」已经实测过：把构建时间戳固定（SOURCE_DATE_EPOCH），
# 关掉这一档之后与改动前的镜像**逐字节相同**（测试只比剔除调试信息后的镜像，
# 因为 DWARF 行表会随源文件改动而变化）。这条 -include 本身不改任何默认档代码。
ASFLAGS := $(ARCH_FLAGS) -g -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
           -include arkrtos_board_config.h $(EXTRA_CFLAGS)

# 通配全部内核源，再剔除本平台没有任何介质/驱动依赖的模块：
# 它们即使功能开关为 0 也在文件头无条件包含硬件头（drvflash.h / lfs.h），
# 而这套 QEMU 目标没有 Flash 控制器、也没有 littlefs 组件。
# sort 不是装饰：$(wildcard) 的顺序来自文件系统的目录枚举顺序，往 kernelsrc/src
# 里增删一个 .c 就可能让顺序重排（实测：新增 arkrtos_shell_auth.c 之后
# arkrtos_task.c 换了个位置）。链接顺序一漂，默认档的镜像就不再逐字节可复现，
# 「零变化」也就没法比对。排序把链接顺序钉在文件名字典序上。
KERNEL_SRCS := $(sort $(filter-out kernelsrc/src/arkrtos_flashapp.c \
                            kernelsrc/src/arkrtos_fs.c \
                            kernelsrc/src/arkrtos_vfs_lfs.c, \
                          $(wildcard kernelsrc/src/*.c)))

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

# 同一条规则给控制台登录的凭据头（只在开档时成立，默认档不生成也不依赖）。
ifneq ($(ARK_AUTH_ON),)
$(OBJS): $(SHELL_AUTH_HDR)
endif

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
