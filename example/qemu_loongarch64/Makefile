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

# 窗口 VA 覆盖（WIN_VA=0x120000000）：真实发行版二进制就链在这一档，装载器
# 必须能把「窗口 VA」与「物理帧」拆开（物理帧仍由帧池给，PAGING_PAGES=1）。
# 非空时内核与夹具的链接基址一起抬到 WIN_VA；为空时与原来逐字节一致。
WIN_VA ?=
ifneq ($(WIN_VA),)
WIN_VA_DEF   := -DARKRTOS_ELF_WINDOW_BASE=$(WIN_VA)u
WIN_ELF_BASE := $(WIN_VA)
else
WIN_VA_DEF   :=
WIN_ELF_BASE := 0x02000000
endif

CFLAGS := $(ARCH_FLAGS) -g -O2 -ffreestanding -fno-common -nostdlib -nostartfiles \
          -fno-builtin -ffunction-sections -fdata-sections -Wall -MMD -MP \
          -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
          -include arkrtos_board_config.h \
          -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
          -DMDK_TRACE_ENABLE=1 -DMDK_TRACE_BACKEND_BUFF=1 -DMDK_TRACE_CPU_HZ=100000000u \
          $(ARK_BUS_FLAGS) -DARKRTOS_SIGN_KEY_LOCAL=1 $(ARK_AUTH_FLAGS) \
          $(WIN_VA_DEF) $(EXTRA_CFLAGS)
# 汇编走预处理器（.S 的大写后缀已经打开了 cpp），所以板级宏在 .S 里也可见 ——
# 按需缺页要按开关决定「refill 入口是否保存通用寄存器」，只能这样做。
# 「默认关时体积与行为零变化」已经实测过：把构建时间戳固定（SOURCE_DATE_EPOCH），
# 关掉这一档之后与改动前的镜像**逐字节相同**（测试只比剔除调试信息后的镜像，
# 因为 DWARF 行表会随源文件改动而变化）。这条 -include 本身不改任何默认档代码。
ASFLAGS := $(ARCH_FLAGS) -g -DARKRTOS_BOARD_CONFIG='"arkrtos_board_config.h"' \
           -include arkrtos_board_config.h $(WIN_VA_DEF) $(EXTRA_CFLAGS)

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
	board/qemu_loongarch64/arkrtos_dt_blob.c \
	board/qemu_loongarch64/arkrtos_fdt_overlay_blob.c \
	board/qemu_loongarch64/drvblk.c \
	board/qemu_loongarch64/drvuart.c \
	board/qemu_loongarch64/drvuart_line.c \
	board/qemu_loongarch64/arkrtos_bus_port.c \
	example/qemu_loongarch64/main.c \
	example/qemu_loongarch64/elf_demo.c \
	example/qemu_loongarch64/mod_demo.c \
	example/qemu_loongarch64/libc_shim.c

# --- 真实以太网网卡（virtio-net over PCI，默认关）--------------------------
# 只有 EXTRA_CFLAGS 里显式写了 ARKRTOS_USE_NET_VIRTIO=1 才把板级驱动与 lwIP
# 源码加进来。默认档下 SRCS_C / INCLUDES 与改动前逐字节相同，镜像也一样。
#
# lwIP 的源清单是显式列目录通配 + $(sort)：不许整树 wildcard —— 那会把
# apps/（未 vendored）与 ipv6/（本工程 LWIP_IPV6=0，编了也用不上）拖进来，
# 而且顺序跟着目录枚举走，镜像就不再可复现。
ARK_NET_ON := $(findstring ARKRTOS_USE_NET_VIRTIO=1,$(EXTRA_CFLAGS))
# socket 档要的是「有协议栈、没有网卡」：Linux socket 系统调用落在 lwIP 上，
# 但回环之外的收发在 QEMU 里没有硬件。只开 LWIP（不开 NET_VIRTIO）时编协议栈
# 与 port 层，不编板级网卡驱动与 net_demo —— 后者会去注册 virtio 网卡。
ARK_LWIP_ON := $(findstring ARKRTOS_USE_LWIP=1,$(EXTRA_CFLAGS))
ifneq ($(ARK_NET_ON),)
LWIP_DIR := kernelsrc/components/lwip
LWIP_SRCS := $(sort $(wildcard $(LWIP_DIR)/src/core/*.c) \
                    $(wildcard $(LWIP_DIR)/src/core/ipv4/*.c) \
                    $(wildcard $(LWIP_DIR)/src/api/*.c)) \
             $(LWIP_DIR)/src/netif/ethernet.c \
             $(LWIP_DIR)/port/lwip_port.c \
             $(LWIP_DIR)/port/sys_arch.c \
             $(LWIP_DIR)/port/lwip_netif_link.c
SRCS_C += board/qemu_loongarch64/drvnet.c $(LWIP_SRCS) \
          example/qemu_loongarch64/net_demo.c
INCLUDES += -I$(LWIP_DIR)/src/include -I$(LWIP_DIR)/port
else ifneq ($(ARK_LWIP_ON),)
LWIP_DIR := kernelsrc/components/lwip
LWIP_SRCS := $(sort $(wildcard $(LWIP_DIR)/src/core/*.c) \
                    $(wildcard $(LWIP_DIR)/src/core/ipv4/*.c) \
                    $(wildcard $(LWIP_DIR)/src/api/*.c)) \
             $(LWIP_DIR)/src/netif/ethernet.c \
             $(LWIP_DIR)/port/lwip_port.c \
             $(LWIP_DIR)/port/sys_arch.c \
             $(LWIP_DIR)/port/lwip_netif_link.c
SRCS_C += $(LWIP_SRCS)
INCLUDES += -I$(LWIP_DIR)/src/include -I$(LWIP_DIR)/port
endif

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

# 宽参 ABI（U-18 剩余项）的夹具侧开关：非空时 hello.elf 按原生字宽填参数块，
# 子命令字置 bit31。内核侧同一档用 EXTRA_CFLAGS=-DARKRTOS_USE_WIDE_SVC_ARGS=1
# 打开；两边必须是同一档，否则内核会拒绝窄块并打一行一次性日志。
WIDE_SVC ?=
ifneq ($(WIDE_SVC),)
HELLO_WIDE_DEF := -DARK_WIDE_ARGS=1
endif

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
	      -ffunction-sections -fdata-sections $(HELLO_WIDE_DEF) -Wl,--gc-sections \
	      -Wl,-Ttext=$(WIN_ELF_BASE) -Wl,-e,_start -Wl,--strip-all \
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

# 设备树（MMU 档）：dtc_lite.py 的第二个出口。
# MCU 档 link 的是 board/<b>/arkrtos_dt.c 那张 const 平表；MMU 档按正常项目
# 的做法把标准 .dtb 放进镜像、启动时解析。两者来自同一份 dts，所以不会
# 互相矛盾——这一点由 tools/fdt_gate.py 在 QEMU 上拿 dt 与 fdt 的输出对账。
FDT_DTB := $(BUILD)/arkrtos.dtb
FDT_H   := $(BUILD)/arkrtos_dt_blob.h

$(FDT_DTB): dts/qemu_loongarch64.dts tools/dts/dtc_lite.py
	@mkdir -p $(BUILD)
	python tools/dts/dtc_lite.py --dts dts/qemu_loongarch64.dts \
	       --out $@ --format dtb ;

$(FDT_H): $(FDT_DTB) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(FDT_DTB) \
	       --output $@ --name ark_qemu_dtb ;

$(BUILD)/board/qemu_loongarch64/arkrtos_dt_blob.o: $(FDT_H)

# overlay demo fragment（F-1c）：与整棵树走同一条"dts -> dtb -> 数组"链路。
# 它只被 `fdt overlay` 命令用到，所以单独成一个 blob，不和基树混在一起。
OVL_DTB := $(BUILD)/arkrtos_fdt_overlay_demo.dtb
OVL_H   := $(BUILD)/arkrtos_fdt_overlay_demo_blob.h

$(OVL_DTB): tools/dts/overlay/demo_label.dts tools/dts/dtc_lite.py
	@mkdir -p $(BUILD)
	python tools/dts/dtc_lite.py --dts tools/dts/overlay/demo_label.dts \
	       --out $@ --format dtb ;

$(OVL_H): $(OVL_DTB) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(OVL_DTB) \
	       --output $@ --name ark_overlay_demo_dtb ;

$(BUILD)/board/qemu_loongarch64/arkrtos_fdt_overlay_blob.o: $(OVL_H)

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(HELLO_H) $(HELLO_BAD_H)

# Linux 系统调用 ABI 示例（linux_hello.elf）：与 hello.elf 走完全相同的
# 装载路径（写文件 -> exec -> 降权任务），区别只在程序内部：它只发 Linux
# 号（a7），没有一行 Ark SDK 调用。签名链路同样是"先签名再转 blob"。
LXHELLO_SRC := example/qemu_loongarch64/elf/linux_hello.c
LXHELLO_ELF := $(BUILD)/linux_hello.elf
LXHELLO_SIGNED := $(BUILD)/linux_hello.signed.elf
LXHELLO_H   := $(BUILD)/linux_hello_blob.h

$(LXHELLO_ELF): $(LXHELLO_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -ffunction-sections -fdata-sections -Wl,--gc-sections \
	      -Wl,-Ttext=$(WIN_ELF_BASE) -Wl,-e,_start -Wl,--strip-all \
	      -Wl,-z,max-page-size=4096 -o $@ $< ;

$(LXHELLO_SIGNED): $(LXHELLO_ELF) tools/sign_payload.py
	python tools/sign_payload.py sign $(LXHELLO_ELF) --out $@ ;

$(LXHELLO_H): $(LXHELLO_SIGNED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(LXHELLO_SIGNED) \
	       --output $@ --name ark_example_linux_hello ;

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(LXHELLO_H)

# execve 夹具（linux_execve.elf）：同一份文件里的两段代码，stage1 用
# execve(221) 把自己换成新镜像、stage2 验任务号/fd/堆/.data 初值。
LXEXEC_SRC := example/qemu_loongarch64/elf/linux_execve.c
LXEXEC_ELF := $(BUILD)/linux_execve.elf
LXEXEC_SIGNED := $(BUILD)/linux_execve.signed.elf
LXEXEC_H   := $(BUILD)/linux_execve_blob.h

$(LXEXEC_ELF): $(LXEXEC_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -ffunction-sections -fdata-sections -Wl,--gc-sections \
	      -Wl,-Ttext=$(WIN_ELF_BASE) -Wl,-e,_start -Wl,--strip-all \
	      -Wl,-z,max-page-size=4096 -o $@ $< ;

$(LXEXEC_SIGNED): $(LXEXEC_ELF) tools/sign_payload.py
	python tools/sign_payload.py sign $(LXEXEC_ELF) --out $@ ;

$(LXEXEC_H): $(LXEXEC_SIGNED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(LXEXEC_SIGNED) \
	       --output $@ --name ark_example_linux_execve ;

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(LXEXEC_H)

# 管道夹具（linux_pipe.elf）：pipe2(59) 的六条行为（两个 fd / 数据往返 /
# 空读 -EAGAIN / 写端关掉读 EOF / O_CLOEXEC 回 -EINVAL / 满时短写与读端关掉
# 回 -EPIPE）。和上面两份夹具同一条构建路径，只在 Linux ABI 档存在。
LXPIPE_SRC := example/qemu_loongarch64/elf/linux_pipe.c
LXPIPE_ELF := $(BUILD)/linux_pipe.elf
LXPIPE_SIGNED := $(BUILD)/linux_pipe.signed.elf
LXPIPE_H   := $(BUILD)/linux_pipe_blob.h

$(LXPIPE_ELF): $(LXPIPE_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -ffunction-sections -fdata-sections -Wl,--gc-sections \
	      -Wl,-Ttext=$(WIN_ELF_BASE) -Wl,-e,_start -Wl,--strip-all \
	      -Wl,-z,max-page-size=4096 -o $@ $< ;

$(LXPIPE_SIGNED): $(LXPIPE_ELF) tools/sign_payload.py
	python tools/sign_payload.py sign $(LXPIPE_ELF) --out $@ ;

$(LXPIPE_H): $(LXPIPE_SIGNED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(LXPIPE_SIGNED) \
	       --output $@ --name ark_example_linux_pipe ;

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(LXPIPE_H)

# 信号夹具（linux_signal.elf）：rt_sigaction / rt_sigprocmask / kill / tkill /
# tgkill / rt_sigreturn 的九组行为（装动作与旧动作、投递点、a0 跨处理函数保留、
# 嵌套、屏蔽与解除、非法参数、SIG_IGN、SIG_DFL 退出码 138、浮点与向量区恢复）。
LXSIG_SRC := example/qemu_loongarch64/elf/linux_signal.c
LXSIG_ELF := $(BUILD)/linux_signal.elf
LXSIG_SIGNED := $(BUILD)/linux_signal.signed.elf
LXSIG_H   := $(BUILD)/linux_signal_blob.h

# 信号夹具在 LSX 档下要额外带 -mlsx 与 LX_SIG_LSX，才能验证向量寄存器区
# 是否也在信号帧里（默认档只验证浮点寄存器区）。
ARK_LSX_ON := $(findstring ARKRTOS_USE_LSX=1,$(EXTRA_CFLAGS))
ifeq ($(ARK_LSX_ON),)
LXSIG_EXTRA :=
else
LXSIG_EXTRA := -mlsx -DLX_SIG_LSX=1
endif

$(LXSIG_ELF): $(LXSIG_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -ffunction-sections -fdata-sections -Wl,--gc-sections \
	      -Wl,-Ttext=$(WIN_ELF_BASE) -Wl,-e,_start -Wl,--strip-all \
	      $(LXSIG_EXTRA) -Wl,-z,max-page-size=4096 -o $@ $< ;

$(LXSIG_SIGNED): $(LXSIG_ELF) tools/sign_payload.py
	python tools/sign_payload.py sign $(LXSIG_ELF) --out $@ ;

$(LXSIG_H): $(LXSIG_SIGNED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(LXSIG_SIGNED) \
	       --output $@ --name ark_example_linux_signal ;

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(LXSIG_H)

# dup/dup3 夹具（linux_dup.elf）：dup 新号 / 数据经副本往返 / 原写端关掉后副本
# 仍可写 / 全关后 EOF / dup3(pw,1) 重定向 stdout 与 close(1) 撤销 / dup3 覆盖
# 在用的 fd / dup(0) 的控制台引用 / 失败面（EBADF、old==new 的 EINVAL）/
# O_CLOEXEC 被接受且 F_GETFD 读回 / 第 6 组 fcntl(25) 的 FD 与状态旗标。
LXDUPD_SRC := example/qemu_loongarch64/elf/linux_dup.c
LXDUPD_ELF := $(BUILD)/linux_dup.elf
LXDUPD_SIGNED := $(BUILD)/linux_dup.signed.elf
LXDUPD_H   := $(BUILD)/linux_dup_blob.h

$(LXDUPD_ELF): $(LXDUPD_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -ffunction-sections -fdata-sections -Wl,--gc-sections \
	      -Wl,-Ttext=$(WIN_ELF_BASE) -Wl,-e,_start -Wl,--strip-all \
	      -Wl,-z,max-page-size=4096 -o $@ $< ;

$(LXDUPD_SIGNED): $(LXDUPD_ELF) tools/sign_payload.py
	python tools/sign_payload.py sign $(LXDUPD_ELF) --out $@ ;

$(LXDUPD_H): $(LXDUPD_SIGNED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(LXDUPD_SIGNED) \
	       --output $@ --name ark_example_linux_dup ;

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(LXDUPD_H)


# ---------------------------------------------------------------------------
# fork/clone 夹具（linux_fork.elf）：clone(220) 与 wait4(260) 的七组行为
# （父子返回值分流、子号不同于父号、地址空间真复制、退出码 42 经 wait4
#   回来、reap 后再问回 -ECHILD、child_stack/CLONE_VM 当场 -EINVAL、
#   认不出的号 -ECHILD）。只有 EXTRA_CFLAGS 里显式开 ARKRTOS_USE_LINUX_FORK=1
# 才构建：默认档一条命令都不多、镜像里一个字节都不多。
# ---------------------------------------------------------------------------
ARK_FORK_ON := $(findstring ARKRTOS_USE_LINUX_FORK=1,$(EXTRA_CFLAGS))

ifneq ($(ARK_FORK_ON),)
# 夹具里的 COW 段只在 EXTRA_CFLAGS 开了 ARKRTOS_USE_PAGING_COW=1 时才编：
# 默认关时夹具逐字节与原来相同（21 条 check），开时多 5 条（26 条）。
# 共享页段同理（ARKRTOS_USE_PAGING_SHM=1，多 8 条 -> 34 条）：不这样分档，
# 「关档时 mmap(MAP_SHARED) 仍回 -ENOSYS」这条反假绿就永远测不到。
# futex 段同理（ARKRTOS_USE_LINUX_FUTEX=1，多 9 条 -> 43 条）。
ARK_COW_ON := $(findstring ARKRTOS_USE_PAGING_COW=1,$(EXTRA_CFLAGS))
ARK_SHM_ON := $(findstring ARKRTOS_USE_PAGING_SHM=1,$(EXTRA_CFLAGS))
ARK_FUTEX_ON := $(findstring ARKRTOS_USE_LINUX_FUTEX=1,$(EXTRA_CFLAGS))
# 文件映射（U-21）第五档：夹具多出 18 条 check（43 -> 61）。同样只在
# EXTRA_CFLAGS 里显式开了才编 —— 关档时夹具与内核都不多一个字节。
ARK_FILEMAP_ON := $(findstring ARKRTOS_USE_PAGING_FILEMAP=1,$(EXTRA_CFLAGS))
# 页级写保护（U-22）第六档：夹具多出 11 条 check（63 -> 74）。同样只在
# EXTRA_CFLAGS 里显式开了才编 —— 关档时夹具与内核都不多一个字节，而且
# 「关档时 mprotect 仍然只记账、写保护页照写不误」这条反假绿才测得到。
ARK_MPROT_ON := $(findstring ARKRTOS_USE_PAGING_MPROTECT=1,$(EXTRA_CFLAGS))
# pselect6（U-22）第七档：夹具多出 15 条 check（107 -> 122）。同样只在
# EXTRA_CFLAGS 里显式开了才编 —— 关档时夹具与内核都不多一个字节，而且
# 「关档时 select 类号的系统调用如实回 -ENOSYS」这条反假绿才测得到。
ARK_SELECT_ON := $(findstring ARKRTOS_USE_LINUX_SELECT=1,$(EXTRA_CFLAGS))
# 按需分页（真 mmap 语义 MAP_FIXED 的前提）本身不是夹具的一档，但夹具里
# MAP_FIXED 那 6 条读数只在按需档才存在（关档时内核如实回 -ENOSYS，那正是
# 反假绿要钉的）。因此这里也必须把 DEMAND 镜像给夹具。
#
# 注意：$(LXFORK_ELF) 只用 $(ARCH_FLAGS) $(LXFORK_DEFS) 编译，不带 CFLAGS /
# EXTRA_CFLAGS / -include arkrtos_board_config.h。所以内核开关不能指望从
# 别处漏进来 —— 每加一个夹具要看的开关，就得在这张表里显式镜像一条，
# 漏镜像的表现是「那几行读数在六档里整段不出现」。
ARK_DEMAND_ON := $(findstring ARKRTOS_USE_PAGING_DEMAND=1,$(EXTRA_CFLAGS))
LXFORK_DEFS   :=
ifneq ($(ARK_COW_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_PAGING_COW=1
endif
ifneq ($(ARK_SHM_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_PAGING_SHM=1
endif
ifneq ($(ARK_FUTEX_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_LINUX_FUTEX=1
endif
ifneq ($(ARK_FILEMAP_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_PAGING_FILEMAP=1
endif
ifneq ($(ARK_MPROT_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_PAGING_MPROTECT=1
endif
ifneq ($(ARK_SELECT_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_LINUX_SELECT=1
endif
# socket 夹具段同时要看 LINUX_SOCKET 与 LWIP 两个开关：前者决定读哪些
# 调用，后者决定协议栈在不在（不在就逐条断言 -ENOSYS）。
ARK_SOCKET_ON := $(findstring ARKRTOS_USE_LINUX_SOCKET=1,$(EXTRA_CFLAGS))
ifneq ($(ARK_SOCKET_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_LINUX_SOCKET=1
endif
ifneq ($(ARK_LWIP_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_LWIP=1
endif
# CLONE_THREAD 夹具段：只认它自己的开关。它必须叠在 fork 档上（内核那边
# features.h 有 #error 兜着），但夹具只按「线程段读不读」决定。
ARK_THREAD_ON := $(findstring ARKRTOS_USE_LINUX_THREAD=1,$(EXTRA_CFLAGS))
ifneq ($(ARK_THREAD_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_LINUX_THREAD=1
endif

# SEC-G07 每 App 任务配额夹具段：只认它自己的开关。上限值也从 EXTRA_CFLAGS
# 透传过去 —— 夹具断言「第几个 fork 被拒」必须与内核用的是同一个数，
# 两边各写一个 8 只是碰巧对，改一处就会静默错位。
ARK_QUOTA_ON := $(findstring ARKRTOS_USE_APP_QUOTA=1,$(EXTRA_CFLAGS))
ARK_APPQ_N   := $(patsubst -DARKRTOS_APP_MAX_TASKS=%,%,$(filter -DARKRTOS_APP_MAX_TASKS=%,$(EXTRA_CFLAGS)))
ifneq ($(ARK_QUOTA_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_APP_QUOTA=1
LXFORK_DEFS   += -DARKRTOS_APP_MAX_TASKS=$(if $(ARK_APPQ_N),$(ARK_APPQ_N),8)
endif
ifneq ($(ARK_DEMAND_ON),)
LXFORK_DEFS   += -DARKRTOS_USE_PAGING_DEMAND=1
endif
LXFORK_SRC    := example/qemu_loongarch64/elf/linux_fork.c
LXFORK_ELF    := $(BUILD)/linux_fork.elf
LXFORK_SIGNED := $(BUILD)/linux_fork.signed.elf
LXFORK_H      := $(BUILD)/linux_fork_blob.h

$(LXFORK_ELF): $(LXFORK_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) $(LXFORK_DEFS) -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -ffunction-sections -fdata-sections -Wl,--gc-sections \
	      -Wl,-Ttext=$(WIN_ELF_BASE) -Wl,-e,_start -Wl,--strip-all \
	      -Wl,-z,max-page-size=4096 -o $@ $< ;

$(LXFORK_SIGNED): $(LXFORK_ELF) tools/sign_payload.py
	python tools/sign_payload.py sign $(LXFORK_ELF) --out $@ ;

$(LXFORK_H): $(LXFORK_SIGNED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(LXFORK_SIGNED) \
	       --output $@ --name ark_example_linux_fork ;

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(LXFORK_H)
endif

# ---------------------------------------------------------------------------
# PT_TLS 示例（tls_main.elf）：装载器读 PT_TLS、把模板铺进窗口、把 $tp 指过去。
#
# 和 dynload 一样的做法：只有 EXTRA_CFLAGS 里显式开 ARKRTOS_USE_ELF_TLS=1
# 才构建这一份夹具，默认档一条命令都不多、镜像里一个字节都不多。
#
# 链接基址沿用 hello 的 0x02000000（窗口内），所以它测的只有 TLS 这一道；
# 地址那道门槛（真实发行版二进制的 0x120000000）仍由探针工具单独报告。
# ---------------------------------------------------------------------------
ARK_ELFTLS_ON := $(findstring ARKRTOS_USE_ELF_TLS=1,$(EXTRA_CFLAGS))

ifneq ($(ARK_ELFTLS_ON),)
TLS_SRC    := example/qemu_loongarch64/elf/tls_main.c
TLS_ELF    := $(BUILD)/tls_main.elf
TLS_SIGNED := $(BUILD)/tls_main.signed.elf
TLS_H      := $(BUILD)/tls_main_blob.h

$(TLS_ELF): $(TLS_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -ffunction-sections -fdata-sections -Wl,--gc-sections \
	      -Wl,-Ttext=$(WIN_ELF_BASE) -Wl,-e,_start -Wl,--strip-all \
	      -Wl,-z,max-page-size=4096 -o $@ $< ;

$(TLS_SIGNED): $(TLS_ELF) tools/sign_payload.py
	python tools/sign_payload.py sign $(TLS_ELF) --out $@ ;

$(TLS_H): $(TLS_SIGNED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(TLS_SIGNED) \
	       --output $@ --name ark_example_tls_main ;

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(TLS_H)
endif
# ---------------------------------------------------------------------------
# Linux 动态链接示例（PT_INTERP + 自研 ld.so）：dynmain / dynmain_exec /
# libanswer.so / ld-ark.so。
#
# 为什么多一步 dynfixture：本机 ld 2.31.1 不支持 -shared / -pie（gcc -shared
# 会静默产出静态 ET_EXEC）。所以三个输入先按静态 ELF 链接（需要 .symtab 里的
# 链接期地址），再由 tools/elf/dynfixture.py 按 manifest 重写成带 PT_INTERP /
# PT_DYNAMIC / .dynsym / .rela 的动态镜像。重定位是 manifest 写明的，不是
# 链接器算出来的 —— 这样每条重定位路径都能单独被门禁打断。
#
# 只有 EXTRA_CFLAGS 里显式开 ARKRTOS_USE_DYNLOAD=1 才构建这些产物：默认档
# 一条命令都不多、镜像里一个字节都不多。
ARK_DYN_ON := $(findstring ARKRTOS_USE_DYNLOAD=1,$(EXTRA_CFLAGS))

ifneq ($(ARK_DYN_ON),)
DYN_SRC     := example/qemu_loongarch64/elf
DYN_MAN     := $(DYN_SRC)/dyn
DYNFIXTURE  := tools/elf/dynfixture.py

DYN_LIB_LINK   := $(BUILD)/libanswer.link.elf
DYN_MAIN_LINK  := $(BUILD)/dynmain.link.elf
DYN_EXEC_LINK  := $(BUILD)/dynmain_exec.link.elf
DYN_LDSO_LINK  := $(BUILD)/ld-ark.link.elf

DYN_LIB_SO   := $(BUILD)/libanswer.so
DYN_MAIN_BIN := $(BUILD)/dynmain
DYN_EXEC_BIN := $(BUILD)/dynmain_exec
DYN_BAD_BIN  := $(BUILD)/dynmain_bad
DYN_MAIN_SIGN := $(BUILD)/dynmain.signed
DYN_EXEC_SIGN := $(BUILD)/dynmain_exec.signed
DYN_BAD_SIGN := $(BUILD)/dynmain_bad.signed

DYN_LDSO_H := $(BUILD)/ld_ark_blob.h
DYN_LIB_H  := $(BUILD)/libanswer_blob.h
DYN_MAIN_H := $(BUILD)/dynmain_blob.h
DYN_EXEC_H := $(BUILD)/dynmain_exec_blob.h
DYN_BAD_H  := $(BUILD)/dynmain_bad_blob.h

# 三个 go 文件的链接基址各有含义：
#   库/主程序 ET_DYN  → 0x1000（vaddr 0 被头部占住，代码从 0x1000 起；
#                       运行时整体平移到窗口/ mmap 基址，只能有 PC 相对寻址）
#   主程序 ET_EXEC    → 0x02001000（.text 从 0x02001000 起，首个 LOAD
#                        仍是 0x02000000 —— 头部正好被它覆盖）
#   ld.so  ET_EXEC    → 0x02100000 == 板级 ARKRTOS_ELF_INTERP_BASE
$(DYN_LIB_LINK): $(DYN_SRC)/libanswer.c
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O1 -nostdlib -nostartfiles -ffreestanding -fno-builtin \
	      -Wl,-Ttext=0x1000 -Wl,-e,answer_add -o $@ $< ;

$(DYN_MAIN_LINK): $(DYN_SRC)/dynmain.c
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O1 -nostdlib -nostartfiles -ffreestanding -fno-builtin \
	      -Wl,-Ttext=0x1000 -Wl,-e,_start -o $@ $< ;

$(DYN_EXEC_LINK): $(DYN_SRC)/dynmain.c
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O1 -nostdlib -nostartfiles -ffreestanding -fno-builtin \
	      -Wl,-Ttext=0x02001000 -Wl,-e,_start -o $@ $< ;

$(DYN_LDSO_LINK): $(DYN_SRC)/ldso.c
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -nostdlib -nostartfiles -ffreestanding -fno-builtin \
	      -Wl,-Ttext=0x02100000 -Wl,-e,_start -o $@ $< ;

# 夹具生成：输入要有 .symtab（所以上面都没有 --strip-all）。
$(DYN_LIB_SO): $(DYN_LIB_LINK) $(DYN_MAN)/libanswer.json $(DYNFIXTURE)
	python $(DYNFIXTURE) --input $(DYN_LIB_LINK) \
	       --manifest $(DYN_MAN)/libanswer.json --output $@ ;

$(DYN_MAIN_BIN): $(DYN_MAIN_LINK) $(DYN_MAN)/dynmain.json $(DYNFIXTURE)
	python $(DYNFIXTURE) --input $(DYN_MAIN_LINK) \
	       --manifest $(DYN_MAN)/dynmain.json --output $@ ;

$(DYN_EXEC_BIN): $(DYN_EXEC_LINK) $(DYN_MAN)/dynmain_exec.json $(DYNFIXTURE)
	python $(DYNFIXTURE) --input $(DYN_EXEC_LINK) \
	       --manifest $(DYN_MAN)/dynmain_exec.json --output $@ ;

# 反假绿用的坏夹具：同一份主程序，只把 JUMP_SLOT 的目标换成一个谁都没定义的
# 符号。ld.so 必须在重定位阶段就喊出来（[ldso] FAIL: unresolved PLT symbol）
# 并以 127 退出——"装载器看着成功、其实把槽写成了 0" 正是要防的那种假绿。
$(DYN_BAD_BIN): $(DYN_MAIN_LINK) $(DYN_MAN)/dynmain_bad.json $(DYNFIXTURE)
	python $(DYNFIXTURE) --input $(DYN_MAIN_LINK) \
	       --manifest $(DYN_MAN)/dynmain_bad.json --output $@ ;

# 签名门开着时 exec 只收带尾 tag 的文件；和 hello 一样"先签名再转 blob"。
$(DYN_MAIN_SIGN): $(DYN_MAIN_BIN) tools/sign_payload.py
	python tools/sign_payload.py sign $(DYN_MAIN_BIN) --out $@ ;

$(DYN_EXEC_SIGN): $(DYN_EXEC_BIN) tools/sign_payload.py
	python tools/sign_payload.py sign $(DYN_EXEC_BIN) --out $@ ;

$(DYN_BAD_SIGN): $(DYN_BAD_BIN) tools/sign_payload.py
	python tools/sign_payload.py sign $(DYN_BAD_BIN) --out $@ ;

$(DYN_LDSO_H): $(DYN_LDSO_LINK) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(DYN_LDSO_LINK) \
	       --output $@ --name ark_example_ld_ark ;

$(DYN_LIB_H): $(DYN_LIB_SO) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(DYN_LIB_SO) \
	       --output $@ --name ark_example_dynlib ;

$(DYN_MAIN_H): $(DYN_MAIN_SIGN) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(DYN_MAIN_SIGN) \
	       --output $@ --name ark_example_dynmain ;

$(DYN_EXEC_H): $(DYN_EXEC_SIGN) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(DYN_EXEC_SIGN) \
	       --output $@ --name ark_example_dynmain_exec ;

$(DYN_BAD_H): $(DYN_BAD_SIGN) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(DYN_BAD_SIGN) \
	       --output $@ --name ark_example_dynmain_bad ;

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(DYN_LDSO_H) $(DYN_LIB_H) \
                                             $(DYN_MAIN_H) $(DYN_EXEC_H) $(DYN_BAD_H)
endif


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

# 隔离执行夹具（ushell_iso.elf）：SEC-G08 的端到端证据程序。
# 与 hello 同一条链路（链接 -> 签名 -> blob），区别只在内容：它注册一条
# 用户态命令 `hi` 后常驻不退出——App 一退出，空间和命令就一起被卸掉，
# 回调也就无从触发。演示任务只负责把它起起来，**不等它结束**。
USHELL_ISO_SRC := example/qemu_loongarch64/elf/ushell_iso.c
USHELL_ISO_ELF := $(BUILD)/ushell_iso.elf
USHELL_ISO_SIGNED := $(BUILD)/ushell_iso.signed.elf
USHELL_ISO_H := $(BUILD)/ushell_iso_blob.h

$(USHELL_ISO_ELF): $(USHELL_ISO_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(ARCH_FLAGS) -O2 -ffreestanding -nostdlib -nostartfiles -fno-builtin \
	      -ffunction-sections -fdata-sections -Wl,--gc-sections \
	      -Wl,-Ttext=$(WIN_ELF_BASE) -Wl,-e,_start -Wl,--strip-all \
	      -Wl,-z,max-page-size=4096 -o $@ $< ;

$(USHELL_ISO_SIGNED): $(USHELL_ISO_ELF) tools/sign_payload.py
	python tools/sign_payload.py sign $(USHELL_ISO_ELF) --out $@ ;

$(USHELL_ISO_H): $(USHELL_ISO_SIGNED) example/qemu_loongarch64/elf_blob.py
	python example/qemu_loongarch64/elf_blob.py --input $(USHELL_ISO_SIGNED) \
	       --output $@ --name ark_example_ushell_iso ;

$(BUILD)/example/qemu_loongarch64/elf_demo.o: $(USHELL_ISO_H)

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
