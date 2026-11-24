#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""genboard - generate a board configuration header from Kconfig + defconfig.

Why this exists
---------------
On the MMU tier the board description is a normal project artifact: a Kconfig
fragment lists what the machine *has* (geometry, clocks, capability), a
defconfig lists the values this build wants, and the C header is generated.
That removes the "two sources of truth" problem the hand-written header had:
a number changed in the header but not in the reviewable menu, or the reverse.

The MCU + MDK tier deliberately keeps its hand-written
board/<chip>/arkrtos_board_config.h (see docs/Kconfig配置系统.md section 9):
uVision reads it directly, the capability set is fixed by the chip, and adding
a generator there would add a build step without removing real hand editing.

What it reads
-------------
  config/kconfig/board/Kconfig.<board>   board-owned symbols (geometry, caps)
  config/<board>_defconfig               the values this build asks for
  Kconfig (+ config/kconfig/Kconfig.*)   feature values (ELF, VFS, HW_SAFE ...)

What it writes
--------------
  board/<board>/arkrtos_board_config.h   the generated header

The two trees are merged in memory because the board fragment must not be
sourced from the top-level Kconfig: the top-level one produces a single
autoconf.h shared by every board, and a QEMU geometry macro must never appear
in an STM32 build.

Two invariants this tool enforces instead of trusting
-----------------------------------------------------
  1. Every value written into the defconfig must survive resolution
     unchanged.  A silently unsatisfied "depends on" would otherwise turn a
     requested y into n and quietly shrink the board - the classic Kconfig
     false green.
  2. Every symbol the renderer needs must be present with the right type.

Usage
-----
    py -3 tools/kconfig/genboard.py --board qemu_loongarch64
    py -3 tools/kconfig/genboard.py --board qemu_loongarch64 --check
    py -3 tools/kconfig/genboard.py --board qemu_loongarch64 --out -
"""
import argparse
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kconfig as kc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# --------------------------------------------------------------------------
# value rendering
# --------------------------------------------------------------------------

def _num(v):
    v = (v or "0").strip()
    return int(v, 0) if v else 0


def _size(num, suf=""):
    """Render a byte count as a product of 1024s: 8388608 -> (8 * 1024 * 1024)."""
    units = []
    x = num
    while x and x % 1024 == 0:
        units.append("1024" + suf)
        x //= 1024
    units.insert(0, str(x) + suf)
    return "(" + " * ".join(units) + ")"


def render_value(fmt, raw):
    if fmt.startswith("alias:"):
        return fmt.split(":", 1)[1]
    if fmt == "bool":
        return "1" if raw not in ("n", "", "0") else "0"
    if fmt == "hexu":
        n = _num(raw)
        return "0u" if n == 0 else "0x%08Xu" % n
    if fmt == "u":
        return "%du" % _num(raw)
    if fmt == "dec":
        return "%d" % _num(raw)
    if fmt == "paren":
        return "(%d)" % _num(raw)
    if fmt == "size":
        return _size(_num(raw))
    if fmt == "size_u":
        return _size(_num(raw), "u")
    if fmt == "stack1024":
        n = _num(raw)
        assert n % 1024 == 0, "stack1024 needs a multiple of 1024, got %d" % n
        return "(1024 * %d)" % (n // 1024)
    raise SystemExit("genboard: unknown format %r" % fmt)


# --------------------------------------------------------------------------
# the board description (order == output order)
#
# mode: "override" -> #undef + #define (beats autoconf.h)
#       "plain"    -> #define only (board-only macro, nothing above it)
#       "guard"    -> #ifndef + #define (a -D on the command line still wins)
# --------------------------------------------------------------------------

BLOCKS = [
    ("一、架构选择", [
        "必须在 arkrtos_arch.h 被包含之前定义 ARKRTOS_ARCH_CORE，否则 arch.h",
        "会按未定义的 ARKRTOS_CPU_ARCH 走 #error 分支。这里写的是",
        "ARKRTOS_CPU_CORE_LA64 的字面值，因为此刻那个宏还不存在。",
    ], [
        ("override", "ARKRTOS_ARCH_CORE", "hexu"),
    ]),

    ("二、芯片几何（QEMU virt / LoongArch64）", [
        "内核镜像区间：QEMU 的 ELF 装载器把 PT_LOAD 段直接拷到这里。基址 2 MB",
        "是硬要求：低 1 MB 被 boot_info 占着。",
        "",
        "静态数据 / 内核 RAM 池区间从镜像区间之上开始。",
        "KERNEL_RAM_BASE = CHIP_RAM_BASE + SHARE_RAM_SIZE（见分区头的推导）。",
        "",
        "没有 CCM / TCM。填 0 让分区头不派生 CCM 段，链接脚本也就不生成它。",
        "",
        "KERNEL_SIZE 是内核镜像在 Flash 区里占的长度，配置区与镜像池紧跟在",
        "它后面（分区头里 IMAGE_POOL_BASE = CHIP_FLASH_BASE + KERNEL_SIZE +",
        "CONFIG_SIZE 就是这么推的）。这条必须够大：板级每次复位会把镜像池",
        "擦成 0xFF，镜像一旦越界，擦除就是在抹自己的代码。",
    ], [
        ("plain", "CHIP_FLASH_BASE", "hexu"),
        ("plain", "CHIP_FLASH_SIZE", "size"),
        ("guard", "KERNEL_SIZE", "size"),
        ("plain", "CHIP_RAM_BASE", "hexu"),
        ("plain", "CHIP_RAM_SIZE", "size"),
        ("plain", "CHIP_CCM_BASE", "hexu"),
        ("plain", "CHIP_CCM_SIZE", "hexu"),
    ]),

    ("三、平台常量（供 port 与板级代码使用）", [
        "TCFG 定时器与 rdtime 的计数时钟都由实测标定，标定过程写在 Kconfig 的",
        "help 里：InitVal = 2,000,000 时 10 s 内 125 次中断（80 ms/次），",
        "rdtime.d 走 1e9 ticks 约 10.0 s。改这两个数之前先重做那次标定。",
        "",
        "ns16550a 寄存器按字节寻址，与 16550 布局一致。",
    ], [
        ("plain", "ARKRTOS_PORT_TIMER_HZ", "u"),
        ("plain", "ARKRTOS_PORT_RDTIME_HZ", "u"),
        ("plain", "ARKRTOS_QEMU_UART_BASE", "hexu"),
        ("guard", "ARKRTOS_QEMU_UART_CLK", "u"),
        ("plain", "ARKRTOS_QEMU_UART_REG_SHIFT", "u"),
        ("plain", "ARKRTOS_QEMU_UART2_BASE", "hexu"),
    ]),

    ("四、分页与用户态窗口", [
        "PLV3 探针页（App 的代码/数据/栈）。DRAM 128 MB，内核占",
        "0x00200000~0x00C00000（镜像 2 MB + RAM 8 MB），16 MB 以上是空地。",
        "取 0x02000000 同时满足 PS=20 条目要求的 2^(20+1) = 2 MB 对齐。",
        "",
        "App 的 UART 是同页 VA 别名：一页 4 KB 的 PLV3 条目把本页内 0x1E0",
        "偏移映射到 PA 0x1FE001E0。取 0x02202000 而不是 0x02200000 的原因：",
        "STLB 组号是 (VA >> 13) & 0xFF，0x02200000 与内核 UART 0x1FE00000",
        "是同一组，两条 4 KB 条目挤在一组里靠随机路号分家（1/8 概率互相",
        "覆盖）。挪一页就换到另一组。",
        "",
        "每 App 地址空间的帧数（U-1）：space n 的物理帧 = APP_BASE + n * APP_SIZE。",
        "取 2 只为在 QEMU 上把「同一 VA 映到不同 PA」跑出读数；帧是固定数组，",
        "没有页帧分配器（欠账 U-3）。",
        "",
        "4 KB 页帧池（U-3）：避开 App 区、内核区与 pmap test 的 0x03000000 测试",
        "VA；8 MB 对齐（PS=22 条目要求 2^(22+1) = 8 MB），port 给它单独补一条",
        "恒等映射；2048 帧正好是 ARKRTOS_USE_PAGING_FRAMES 的编译期上限。",
    ], [
        ("plain", "ARKRTOS_QEMU_APP_BASE", "hexu"),
        ("plain", "ARKRTOS_QEMU_APP_SIZE", "size_u"),
        ("plain", "ARKRTOS_QEMU_APP_UART", "hexu"),
        ("plain", "ARKRTOS_PAGING_APPS_MAX", "u"),
        ("plain", "ARKRTOS_QEMU_FRAME_BASE", "hexu"),
        ("plain", "ARKRTOS_QEMU_FRAME_COUNT", "u"),
        ("override", "ARKRTOS_USE_PAGING_FRAMES", "bool"),
        ("override", "ARKRTOS_USE_PAGING_PAGES", "bool"),
        ("guard", "ARKRTOS_USE_PAGING_DEMAND", "bool"),
        ("guard", "ARKRTOS_USE_PAGING_ASID", "bool"),
    ]),

    ("五、能力开关", [
        "LA64 硬件有 FPU。打开后做两件事：启动时（start.S）置 EUEN.FPE=1（不开就是",
        "PLV3 第一条 FP 指令落 ESTAT.Ecode=15 FPD），以及把 trap 帧 256 → 528",
        "字节、按任务保存 $f0..$f31（eentry / 就地切换 / resume_task 三处统一",
        "存取）。FCSR 与 LSX/LASX 仍不保存，理由见 Kconfig 的 help。",
        "",
        "特权分离打开：port 里建立静态 TLB 映射并打开 CRMD.PG，内核页 PLV0、",
        "App 页 PLV3。MPU 仍然关掉 —— LoongArch 没有 MPU。",
        "",
        "隔离后端选「页表」（MMU）而不是「窗口」：打开后 arkrtos_mpu_* 这套",
        "契约由 arkrtos_paging.c 实现，五个受限窗口变成「静态映射 + PLV」+",
        "装载期跨度审计。与 ARKRTOS_USE_MPU=1 互斥，arkrtos_config.h 里以",
        "#error 拦下。",
    ], [
        ("override", "ARKRTOS_USE_FPU", "bool"),
        ("override", "ARKRTOS_USE_MPU", "bool"),
        ("override", "ARKRTOS_USE_PAGING", "bool"),
        ("override", "ARKRTOS_USE_PRIV", "bool"),
    ]),

    ("六、可执行文件", [
        "ELF 装载器：打开后 shell 能直接把 ELF64 可执行文件当命令跑",
        "（exec /bin/hello world）。窗口就用 PLV3 探针那 2 MB（硬件映射已由",
        "port 建好，装载器只往里拷），所以两个宏直接指向 APP_BASE/SIZE。",
        "M1 是单实例 + 共享地址空间；每进程页表是 M2 的欠账。",
        "",
        "Linux 系统调用 ABI：这块板有 MMU、有 PLV3 用户态，是兼容层的第一个",
        "载体。打开后 ELF 程序可以用 syscall 0 + $a7 的 Linux 约定发请求。",
        "",
        "U-8：本板打开退出码通道（hello.elf 用 argc 当退出码，exec 打印的读数",
        "与程序自己打印的 argc 互为对证）。",
        "",
        "VxWorks 式可下载模块与上面的 ELF 装载器并列，不是替代关系。模块区是",
        "内核 .bss 里的一块静态缓冲，不用 heap；地址由链接器决定。代价写清楚：",
        "模块跑 PLV0，等于放弃 PLV3 隔离。",
        "",
        "动态链接（PT_INTERP）的解释器区：2 MB 窗口从中间劈开——",
        "  主程序  [APP_BASE + 0x000000, APP_BASE + 0x100000)  1 MB",
        "  解释器  [APP_BASE + 0x100000, APP_BASE + 0x110000)  64 KB",
        "  堆/库   [APP_BASE + 0x110000, APP_BASE + 0x1E0000)",
        "  栈      [.., APP_BASE + 0x1F4000)",
        "  argv/auxv 区 [APP_BASE + 0x1F4000, APP_BASE + 0x200000)",
        "解释器区与主程序区不重叠，装载器对两者都做区间判据；这两个宏只在",
        "ARKRTOS_USE_DYNLOAD=1 时才被读（关档零变化）。",
    ], [
        ("override", "ARKRTOS_USE_ELF", "bool"),
        ("override", "ARKRTOS_USE_LINUX_ABI", "bool"),
        ("guard", "ARKRTOS_LINUX_ABI_SYSSTAT", "bool"),
        ("guard", "ARKRTOS_USE_ELF_TLS", "bool"),
        ("override", "ARKRTOS_USE_EXIT_CODE", "bool"),
        # guard 而不是 plain：窗口 VA 要能被命令行 -D 覆盖（真实发行版
        # LA64 静态二进制按 0x120000000 链接），默认值仍是 APP_BASE ⇒
        # 不给 -D 时逐字节不变。2026-11-20。
        ("guard", "ARKRTOS_ELF_WINDOW_BASE", "alias:ARKRTOS_QEMU_APP_BASE"),
        ("guard", "ARKRTOS_ELF_WINDOW_SIZE", "alias:ARKRTOS_QEMU_APP_SIZE"),
        ("guard", "ARKRTOS_ELF_INTERP_BASE", "alias:(ARKRTOS_ELF_WINDOW_BASE + 0x00100000u)"),
        ("guard", "ARKRTOS_ELF_INTERP_SIZE", "alias:(64u * 1024u)"),
        ("override", "ARKRTOS_USE_MODLOAD", "bool"),
        ("plain", "ARKRTOS_MOD_AREA_SIZE", "size_u"),
        ("plain", "ARKRTOS_PLATFORM_REBOOT_PORT", "bool"),
    ]),

    ("七、设备树（MMU 档）", [
        "MMU 档按正常项目的做法走：镜像里放一块标准 .dtb，启动时解析——",
        "不是 MCU 档那张 const C 平表。两者来自同一份 dts/qemu_loongarch64.dts，",
        "由 dtc_lite.py 的两个出口分别产出，所以不会互相矛盾。",
        "",
        "打开后内核多一个 FDT 解析器与 fdt 命令（只读：不改任何结构、不碰",
        "硬件）。关掉时 arkrtos_fdt.c 不产生任何符号，镜像与命令表不变——",
        "这正是 MCU 档的默认状态。",
    ], [
        ("override", "ARKRTOS_USE_FDT", "bool"),
        ("override", "ARKRTOS_USE_FDT_CONSOLE", "bool"),
        # guard 而不是 override：这一档默认关（0），要开必须能被命令行 -D 打开；
        # 写成 #undef + #define 0 会让 -D 静默失效（2026-11-20 同 RTSTAT 踩过）。
        ("guard", "ARKRTOS_USE_FDT_RES", "bool"),
        ("override", "ARKRTOS_USE_FDT_OVERLAY", "bool"),
    ]),

    ("八、可靠性", [
        "本板没有 Cortex-M 的 AIRCR，复位由板级 arkrtos_board_reboot() 接管",
        "（loongarch virt 也没有复位设备，那里只能明确报「做不到」）。",
        "",
        "心跳合同：QEMU 没有看门狗，守卫真咬人的前提不存在，直接关掉。",
    ], [
        ("override", "ARKRTOS_USE_KERNEL_GUARD", "bool"),
        ("override", "ARKRTOS_WDG_ENABLE", "bool"),
    ]),

    ("九、功能裁剪（把需要真实硬件 / 需要 libc 格式化的模块全部关掉）", [
        "日志：控制台已经有一条可用的输出通路（ns16550a 轮询），日志走它。",
        "LOG_RING 打开后 log show / 崩溃转储能从环形缓冲里翻历史。",
        "",
        "存储栈：QEMU 上没有真实 NOR / eMMC，但可以用板级 RAM 介质把 blk 以上",
        "的整条软件栈（blk -> littlefs -> VFS -> procfs）真的跑起来。诚实边界：",
        "介质本身（编程时序、忙等待、掉电中间态）没有验证，那是 drvnor.c /",
        "drvflash.c 的事。",
    ], [
        ("override", "ARKRTOS_USE_LOG", "bool"),
        ("override", "ARKRTOS_USE_LOG_RING", "bool"),
        ("override", "ARKRTOS_USE_BLK", "bool"),
        ("override", "ARKRTOS_USE_FS", "bool"),
        ("override", "ARKRTOS_USE_VFS", "bool"),
        ("override", "ARKRTOS_USE_VFS_LFS", "bool"),
        ("override", "ARKRTOS_USE_PROCFS", "bool"),
        ("override", "ARKRTOS_USE_MINIAPP", "bool"),
        ("override", "ARKRTOS_USE_POSIX", "bool"),
        ("override", "ARKRTOS_USE_LWIP", "bool"),
        ("override", "ARKRTOS_USE_SOFTBUS", "bool"),
        ("override", "ARKRTOS_USE_SHELL", "bool"),
        ("override", "ARKRTOS_USE_APPPACK", "bool"),
        ("override", "ARKRTOS_USE_APPRUN", "bool"),
        ("override", "ARKRTOS_USE_FLASHAPP", "bool"),
        ("override", "ARKRTOS_USE_IMAGE_SIGN", "bool"),
        ("override", "ARKRTOS_USE_EXEC_SIGN", "bool"),
        ("override", "ARKRTOS_USE_STACK_GUARD", "bool"),
        ("plain", "ARKRTOS_SHELL_STACK_SIZE", "stack1024"),
        ("override", "ARKRTOS_USE_QSPI", "bool"),
        ("override", "ARKRTOS_AUTOMOUNT_NOR", "bool"),
    ]),

    ("十、读数与可观测性", [
        "RTSTAT 与 SCHED_STAT 默认关（关着时体积与行为零变化），但写成",
        "#ifndef 而不是 #undef + #define 0：本板有周期级计数器",
        "（rdtime / csrrd 0x40，100 MHz 板级常量，见 port 的",
        "arkrtos_port_cyccnt_init），要在命令行打开一份带仪表的构建：",
        "    EXTRA_CFLAGS=-DARKRTOS_USE_RTSTAT=1",
        "写成 #undef 会让这条 -D 静默失效（2026-11-20 实测踩过）。",
    ], [
        ("guard", "ARKRTOS_USE_RTSTAT", "bool"),
        ("guard", "ARKRTOS_USE_SCHED_STAT", "bool"),
        ("override", "ARKRTOS_USE_MDK_TRACE", "bool"),
        ("override", "ARKRTOS_USE_CM_BACKTRACE", "bool"),
        ("override", "ARKRTOS_USE_CRASH_LOG", "bool"),
        ("override", "ARKRTOS_USE_AUDIT", "bool"),
        ("override", "ARKRTOS_USE_UTEST", "bool"),
        ("override", "ARKRTOS_USE_SECCHECK", "bool"),
    ]),

    ("十一、遗言与栈", [
        "遗言机制（ARKRTOS_HW_SAFE）：机制与架构无关，本板打开。默认档下这块",
        "QEMU 板上没有驱动登记安全回调（devs=0），机制按契约工作由 UTEST 的合成",
        "回调取证；打开 ARKRTOS_DEMO_HWSAFE 后板级挂三台板内虚拟设备（led0/1/2）",
        "并声明安全态回调，示例任务走完「遗言 / 驱动回调 / 活着不被中和」三条通道",
        "（见 docs/跨架构机制矩阵.md §2.2 与 §2.3）。",
        "",
        "SMP 的次核栈用 #ifndef：默认档下它是「定义了但没用」的一个数，",
        "打开 SMP 时工程可以用 -D 覆盖，不必改本文件。",
        "",
        "次核「恢复现场前」的临时内核栈（R3）：装窗口与补帧特权级都要用它。",
        "4 KB 对两个 C 函数调用足够；两核各一块，见 arkrtos_smp_sched.c。",
    ], [
        ("override", "ARKRTOS_HW_SAFE", "bool"),
        ("guard", "ARKRTOS_DEMO_HWSAFE", "bool"),
        ("override", "ARKRTOS_TIMER_TASK_STACK_WORDS", "paren"),
        ("guard", "ARKRTOS_USE_SMP", "bool"),
        ("guard", "ARKRTOS_SMP_SEC_STACK_SIZE", "dec"),
        ("guard", "ARKRTOS_SMP_TRAP_STACK_SIZE", "dec"),
    ]),
]

GUARD = "__ARKRTOS_BOARD_CONFIG_H__"


# --------------------------------------------------------------------------
# tree loading
# --------------------------------------------------------------------------

def load_tree(board):
    frag = os.path.join("config", "kconfig", "board", "Kconfig." + board)
    if not os.path.exists(frag):
        raise SystemExit("genboard: no board fragment %s" % frag)
    main = kc.load("Kconfig")
    extra = kc.load(frag)
    clash = sorted(set(main.symbols) & set(extra.symbols))
    if clash:
        raise SystemExit("genboard: %s redefines symbol(s) from the top-level tree: %s"
                         % (frag, ", ".join(clash)))
    main.symbols.update(extra.symbols)
    return main


def check_requested(cfg, user, path):
    """Every value the defconfig asks for must survive resolution unchanged."""
    bad = []
    for name, want in sorted(user.items()):
        s = cfg.kc.symbols.get(name)
        if s is None:
            bad.append("%s: not declared in any Kconfig" % name)
            continue
        got = cfg.values.get(name, "")
        if s.type == kc.BOOL:
            a = "n" if want in ("n", "", "0") else "y"
            b = "n" if got in ("n", "", "0") else "y"
        elif s.type in (kc.INT, kc.HEX):
            a, b = _num(want), _num(got)
        else:
            a, b = want, got
        if a != b:
            bad.append("%s: defconfig asks %s, Kconfig resolved %s"
                       % (name, want, got if got != "" else "(empty)"))
    if bad:
        print("genboard: %s does not survive resolution - a depends-on is "
              "silently eating requested values:" % path, file=sys.stderr)
        for line in bad:
            print("  " + line, file=sys.stderr)
        return False
    return True


# --------------------------------------------------------------------------
# rendering
# --------------------------------------------------------------------------

def render(board, values, src):
    out = []
    w = out.append
    w("/*")
    w(" * @file arkrtos_board_config.h")
    w(" * @brief Ark RTOS 板级配置 - QEMU virt（LoongArch64，MMU 档）")
    w(" *")
    w(" * 本文件由 tools/kconfig/genboard.py 生成，不要手改 —— 改了下次")
    w(" * --check 就会判过期。要改一项配置，改下面这三个来源之一：")
    w(" *")
    w(" *   board symbols   config/kconfig/board/Kconfig.%s" % board)
    w(" *   values          config/%s_defconfig" % board)
    w(" *   feature values  顶层 Kconfig（config/kconfig/Kconfig.*）")
    w(" *")
    w(" * 重新生成：")
    w(" *   py -3 tools/kconfig/genboard.py --board %s" % board)
    w(" * 校验：")
    w(" *   py -3 tools/kconfig/genboard.py --board %s --check" % board)
    w(" *   （tools/ci_gate.py 的 board-config-generated 项做的就是这一句）")
    w(" *")
    w(" * 每一段的长说明写在 Kconfig 的 help 里：menuconfig 里看得见，这里给")
    w(" * 简版。MCU + MDK 档不走这套生成器，保留手写头，见")
    w(" * docs/Kconfig配置系统.md §9。")
    w(" *")
    w(" * @note 本文件先于 config/arkrtos_partition.h 被包含（见 arkrtos_config.h")
    w(" *       的加载顺序），所以这里定义的 CHIP_* 就是最终值 —— 分区头里的")
    w(" *       同名宏都带 #ifndef 守卫。")
    w(" * @author xw（生成器；版式由 tools/kconfig/genboard.py 决定）")
    w(" */")
    w("")
    w("#ifndef %s" % GUARD)
    w("#define %s" % GUARD)
    w("")
    for title, prose, entries in BLOCKS:
        w("/* ============================================================")
        w(" * %s" % title)
        for line in prose:
            w(" *" if line == "" else " * " + line)
        w(" * ============================================================ */")
        for mode, name, fmt in entries:
            raw = values.get(name, "")
            if fmt.startswith("alias:"):
                raw = fmt.split(":", 1)[1]
                if name in values:
                    raise SystemExit("genboard: %s is both a Kconfig symbol and "
                                     "an alias" % name)
                val = render_value(fmt, raw)
            else:
                if name not in values:
                    raise SystemExit("genboard: %s is requested by the renderer "
                                     "but no Kconfig symbol defines it" % name)
                val = render_value(fmt, raw)
            body = "#define %-28s %s" % (name, val)
            if mode == "plain":
                w(body)
            elif mode == "override":
                w("#undef  %s" % name)
                w(body)
            elif mode == "guard":
                w("#ifndef %s" % name)
                w(body)
                w("#endif")
            else:
                raise SystemExit("genboard: unknown mode %r" % mode)
        w("")
    w("#endif /* %s */" % GUARD)
    w("")
    return "\n".join(out)


def main(argv):
    ap = argparse.ArgumentParser(description="Ark RTOS Kconfig -> board header")
    ap.add_argument("--board", required=True)
    ap.add_argument("--out")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)

    os.chdir(ROOT)
    board = args.board
    frag = os.path.join("config", "kconfig", "board", "Kconfig." + board)
    defconfig = os.path.join("config", "%s_defconfig" % board)
    if not os.path.exists(defconfig):
        raise SystemExit("genboard: no defconfig %s" % defconfig)

    tree = load_tree(board)
    user = kc.parse_config(defconfig)
    cfg = kc.Config(tree)
    cfg.user = user
    cfg.resolve()

    if not check_requested(cfg, user, defconfig):
        return 1

    text = render(board, cfg.values, defconfig)
    out = args.out or os.path.join("board", board, "arkrtos_board_config.h")

    if out == "-":
        sys.stdout.write(text)
        return 0

    if args.check:
        old = ""
        if os.path.exists(out):
            with io.open(out, "r", encoding="utf-8") as f:
                old = f.read()
        if old == text:
            print("genboard: %s is up to date" % out)
            return 0
        print("genboard: %s is STALE - regenerate it" % out, file=sys.stderr)
        return 1

    with io.open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("genboard: %s (%d block(s))" % (out, len(BLOCKS)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))