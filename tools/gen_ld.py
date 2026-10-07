#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Ark RTOS GNU ld 链接脚本（.ld）生成器
=====================================

与 tools/gen_scatter.py 同源：都只读 config/arkrtos_partition.h，
把同一套推导出的地址翻译成各自工具链的链接描述，保证
「地址只定义一次，其余全部派生」这条红线在 GCC 工具链下同样成立。

同时支持 stdout 直出与文件输出，便于在 Makefile 里当构建步骤调用，
也便于 `--dump` 与 gen_scatter 的输出做交叉核对。

用法：
    # 内核
    python tools/gen_ld.py --target kernel --output build/kernel.ld

    # 开发调试裸镜像（无镜像头，负载直接落在单元基址）
    python tools/gen_ld.py --target image --unit 1 --raw --output build/dev1.ld

    # 与 gen_scatter 交叉核对（打印关键地址）
    python tools/gen_ld.py --dump
"""

from __future__ import print_function

import argparse
import os
import sys

# 复用 gen_scatter 的宏解析与布局模型，避免两套推导逻辑漂移
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_scatter import (  # noqa: E402
    REPO_ROOT, DEFAULT_HEADER, parse_defines, MacroEval, Layout, auto_int,
)

LD_BANNER = """\
/* *************************************************************
 * ***  本文件由 tools/gen_ld.py 自动生成，请勿手工编辑        ***
 * ***  配置源头: config/arkrtos_partition.h                    ***
 * ***  修改布局请改配置头，然后重新生成（不要改本文件）        ***
 * ************************************************************* */
"""


def _memory(name, attr, base, size):
    return "  %-9s (%s) : ORIGIN = 0x%08X, LENGTH = 0x%08X\n" % (name, attr, base, size)


def emit_ld(region, flash_base, flash_size, ram_base, ram_size, out,
            ccm=None, noinit=None, entry="Reset_Handler", want_stack=True,
            load_base=None, bss_spill_ccm=False):
    """输出一个映像的 GNU ld 脚本。

    flash_base/flash_size  负载的 FLASH 窗口（可执行段）
    ram_base/ram_size      RW/ZI/BSS 的 RAM 窗口
    load_base              .data 的加载地址所在区（默认与 flash 同区）；
                           镜像（App/驱动）场景下 .data 初值随负载一起在 FLASH
    want_stack             True 时定义 _estack = RAM 顶端（内核自持栈）；
                           镜像由内核设置栈，故不需要
    bss_spill_ccm          True 时把 .bss 拆成「RAM 段 + CCM 溢出段」两段，
                           并配合 --enable-non-contiguous-regions 复现 Keil
                           侧 .ANY 的填充顺序（RAM 填满后溢进 CCM）。
                           启动文件必须同时清零 _sbss.._ebss 与
                           _sbss_ccm.._ebss_ccm 两段
    """
    # .data 的装载地址窗口：内核里 .data 初值在 FLASH，镜像里也在负载 FLASH
    lb = load_base if load_base is not None else flash_base
    out.write(LD_BANNER)
    out.write("\n")
    out.write("ENTRY(%s)\n\n" % entry)
    out.write("MEMORY\n{\n")
    out.write(_memory("FLASH", "rx", flash_base, flash_size))
    out.write(_memory("RAM", "rwx", ram_base, ram_size))
    if ccm and ccm[1] > 0:
        out.write(_memory("CCMRAM", "rw", ccm[0], ccm[1]))
    if noinit and noinit[1] > 0:
        out.write(_memory("NOINIT", "rw", noinit[0], noinit[1]))
    out.write("}\n\n")

    if want_stack:
        out.write("_estack = ORIGIN(RAM) + LENGTH(RAM);   /* 内核自持栈顶（与内核推导一致） */\n")
    out.write("_Min_Heap_Size  = 0x0;   /* 内核不提供 C 库堆：malloc 应在链接期失败 */\n")
    out.write("_Min_Stack_Size = 0x0;\n\n")

    out.write("SECTIONS\n{\n")
    out.write("  .isr_vector :\n  {\n    . = ALIGN(4);\n"
              "    KEEP(*(.isr_vector))\n    . = ALIGN(4);\n  } >FLASH\n\n")
    out.write("  .text :\n  {\n    . = ALIGN(4);\n"
              "    *(.text)\n    *(.text*)\n    *(.glue_7)\n    *(.glue_7t)\n"
              "    *(.eh_frame)\n    KEEP(*(.init))\n    KEEP(*(.fini))\n"
              "    . = ALIGN(4);\n    _etext = .;\n  } >FLASH\n\n")
    out.write("  .rodata :\n  {\n    . = ALIGN(4);\n"
              "    *(.rodata)\n    *(.rodata*)\n    . = ALIGN(4);\n  } >FLASH\n\n")
    out.write("  .ARM.extab :\n  {\n    . = ALIGN(4);\n"
              "    *(.ARM.extab* .gnu.linkonce.armextab.*)\n  } >FLASH\n\n")
    out.write("  .ARM :\n  {\n    . = ALIGN(4);\n"
              "    __exidx_start = .;\n    *(.ARM.exidx*)\n"
              "    __exidx_end = .;\n    . = ALIGN(4);\n  } >FLASH\n\n")
    out.write("  .preinit_array :\n  {\n    . = ALIGN(4);\n"
              "    PROVIDE_HIDDEN(__preinit_array_start = .);\n"
              "    KEEP(*(.preinit_array*))\n"
              "    PROVIDE_HIDDEN(__preinit_array_end = .);\n  } >FLASH\n\n")
    out.write("  .init_array :\n  {\n    . = ALIGN(4);\n"
              "    PROVIDE_HIDDEN(__init_array_start = .);\n"
              "    KEEP(*(SORT(.init_array.*)))\n    KEEP(*(.init_array*))\n"
              "    PROVIDE_HIDDEN(__init_array_end = .);\n  } >FLASH\n\n")
    out.write("  .fini_array :\n  {\n    . = ALIGN(4);\n"
              "    PROVIDE_HIDDEN(__fini_array_start = .);\n"
              "    KEEP(*(SORT(.fini_array.*)))\n    KEEP(*(.fini_array*))\n"
              "    PROVIDE_HIDDEN(__fini_array_end = .);\n  } >FLASH\n\n")

    out.write("  /* .data 初值放在负载 FLASH，启动/装载期搬到 RAM */\n")
    out.write("  _sidata = LOADADDR(.data);\n\n")
    out.write("  .data :\n  {\n    . = ALIGN(4);\n    _sdata = .;\n"
              "    __data_start__ = _sdata;\n    *(.data)\n    *(.data*)\n"
              "    . = ALIGN(4);\n    _edata = .;\n    __data_end__ = _edata;\n"
              "  } >RAM AT> FLASH\n\n")

    if noinit and noinit[1] > 0:
        out.write("  /* 不初始化：跨复位保留（看门狗/软复位不碰它） */\n")
        out.write("  .noinit (NOLOAD) :\n  {\n    . = ALIGN(4);\n"
                  "    _snoinit = .;\n    *(.noinit)\n    *(.noinit*)\n"
                  "    . = ALIGN(4);\n    _enoinit = .;\n  } >NOINIT\n\n")

    if ccm and ccm[1] > 0:
        out.write("  /* CCM/TCM 快速 RAM：GCC 口径靠段属性显式归入（不同于 Keil 隐式 region） */\n")
        out.write("  .ccmram (NOLOAD) :\n  {\n    . = ALIGN(4);\n"
                  "    _sccmram = .;\n    *(.ccmram)\n    *(.ccmram*)\n"
                  "    . = ALIGN(4);\n    _eccmram = .;\n  } >CCMRAM\n\n")

    out.write("  .bss :\n  {\n    . = ALIGN(4);\n    _sbss = .;\n"
              "    __bss_start__ = _sbss;\n    *(.bss)\n    *(.bss*)\n"
              "    *(COMMON)\n    . = ALIGN(4);\n    _ebss = .;\n"
              "    __bss_end__ = _ebss;\n  } >RAM\n\n")

    if bss_spill_ccm and ccm and ccm[1] > 0:
        out.write("  /* 内核 .bss 溢出段：Keil 侧由 .ANY 自动溢进 CCM，GCC 侧靠\n"
                  "     --enable-non-contiguous-regions 复现同一填充顺序：RAM 段的\n"
                  "     同名通配符先被填满，放不下的 .bss 整段落入本段。启动文件须\n"
                  "     同时清零 _sbss.._ebss 与 _sbss_ccm.._ebss_ccm 两段。 */\n")
        out.write("  .bss.ccm (NOLOAD) :\n  {\n    . = ALIGN(4);\n    _sbss_ccm = .;\n"
                  "    *(.bss)\n    *(.bss*)\n    *(COMMON)\n"
                  "    . = ALIGN(4);\n    _ebss_ccm = .;\n  } >CCMRAM\n\n")

    out.write("  /DISCARD/ : { *(.note.*) }\n")
    out.write("}\n")


def main():
    ap = argparse.ArgumentParser(description="Ark RTOS GNU ld 链接脚本生成器")
    ap.add_argument("--target", choices=["kernel", "image"], default="kernel")
    ap.add_argument("--output", help="输出 .ld 路径；不给则写到 stdout")
    ap.add_argument("--header", default=DEFAULT_HEADER, help="分区配置头文件")
    ap.add_argument("--raw", action="store_true",
                    help="裸镜像布局（负载直接位于单元基址，无镜像头）")
    ap.add_argument("--unit", type=auto_int, default=0, help="起始分配单元号")
    ap.add_argument("--units", type=auto_int, default=1, help="占用单元数（2 的幂）")
    ap.add_argument("--type", choices=["app", "driver", "miniapp"], default="app")
    ap.add_argument("--dump", action="store_true", help="仅打印关键地址用于交叉核对")
    args = ap.parse_args()

    header = args.header
    if not os.path.isabs(header):
        header = os.path.join(REPO_ROOT, header)
    if not os.path.isfile(header):
        raise SystemExit("找不到分区配置头文件: %s" % header)

    layout = Layout(MacroEval(parse_defines(header)))
    v = layout.v

    if args.dump:
        print("[gen_ld] KERNEL_BASE=0x%08X KERNEL_SIZE=0x%08X" % (v("KERNEL_BASE"), v("KERNEL_SIZE")))
        print("[gen_ld] KERNEL_RAM_BASE=0x%08X KERNEL_RAM_SIZE=0x%08X"
              % (v("KERNEL_RAM_BASE"), v("KERNEL_RAM_SIZE")))
        print("[gen_ld] CHIP_CCM=0x%08X+0x%08X" % (v("CHIP_CCM_BASE"), v("CHIP_CCM_SIZE")))
        print("[gen_ld] CRASH_LOG=0x%08X+0x%08X" % (v("CRASH_LOG_BASE"), v("CRASH_LOG_SIZE")))
        if not args.output:
            return 0

    ccm = (v("CHIP_CCM_BASE"), v("CHIP_CCM_SIZE"))

    if args.target == "kernel":
        flash_base, flash_size = v("KERNEL_BASE"), v("KERNEL_SIZE")
        ram_base, ram_size = v("KERNEL_RAM_BASE"), v("KERNEL_RAM_SIZE")
        noinit = (v("CRASH_LOG_BASE"), v("CRASH_LOG_SIZE"))
        want_stack = True
        entry = "Reset_Handler"
    else:  # image
        sector = v("IMAGE_POOL_SECTOR")
        if args.units <= 0 or (args.units & (args.units - 1)) != 0:
            raise SystemExit("单元数 %d 不是 2 的幂" % args.units)
        if (args.unit % args.units) != 0:
            raise SystemExit("起始单元 %d 未按自身跨度 %d 对齐" % (args.unit, args.units))
        if args.unit + args.units > layout.units():
            raise SystemExit("起始单元 %d + %d 超出池范围（共 %d 单元）"
                             % (args.unit, args.units, layout.units()))
        base = layout.unit_base(args.unit)
        size = args.units * sector
        if args.raw:
            flash_base, flash_size = base, size
        else:
            hdr = v("APP_IMAGE_HEADER_SIZE")
            flash_base, flash_size = base + hdr, size - hdr
        ram_base = layout.dev_ram_base(args.unit)
        ram_size = v("ARKRTOS_DEV_RAM_WINDOW")
        noinit = None
        # 镜像由内核设置栈，链接脚本不再定义 _estack
        want_stack = False
        entry = "APPSTART"

    buf = []
    import io

    class _S(io.StringIO):
        pass

    sink = io.StringIO()
    emit_ld("_", flash_base, flash_size, ram_base, ram_size, sink,
            ccm=ccm, noinit=noinit, want_stack=want_stack, entry=entry,
            bss_spill_ccm=(args.target == "kernel"))
    text = sink.getvalue()

    if args.output:
        d = os.path.dirname(os.path.abspath(args.output))
        if d and not os.path.isdir(d):
            os.makedirs(d)
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("[gen_ld] %-7s -> %s" % (args.target, args.output))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
