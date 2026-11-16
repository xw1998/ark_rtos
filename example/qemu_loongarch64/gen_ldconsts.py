#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Ark RTOS - 为链接脚本生成数值常量头（QEMU 平台通用）
====================================================

为什么需要这个脚本
------------------
链接脚本模板里要用到 CHIP_FLASH_BASE / CHIP_RAM_BASE / KERNEL_RAM_BASE 等
地址，这些值只允许在 config/arkrtos_partition.h 里定义一次，板级配置
（board/<板>/arkrtos_board_config.h）可以整体替换 CHIP_* 的取值。

直接把 `#include "arkrtos_partition.h"` 交给 C 预处理器展开是不行的：分区头
里含有 C 语言的编译期断言（`typedef char ...`），这些行会被原样搬进 ld
脚本，导致 ld 报 `syntax error`。因此这里复用 tools/gen_scatter.py 的宏
解析器，把需要的少数几个宏**求出数值**，写成一份只含 `#define NAME 数值`
的头文件，链接脚本再包含它。

用法（仓库根目录）：
    python example/qemu_loongarch64/gen_ldconsts.py --output build/qemu_loongarch64/ldconsts.h
"""

from __future__ import print_function

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

from gen_scatter import parse_defines, MacroEval  # noqa: E402

PARTITION = os.path.join(REPO_ROOT, "config", "arkrtos_partition.h")
BOARD = os.path.join(HERE, "..", "..", "board", "qemu_loongarch64",
                     "arkrtos_board_config.h")

# 链接脚本用到的常量。新增脚本常量时在这里加名字即可，数值一律由宏求值得到。
WANTED = [
    "CHIP_FLASH_BASE",
    "CHIP_FLASH_SIZE",
    "CHIP_RAM_BASE",
    "CHIP_RAM_SIZE",
    "SHARE_RAM_BASE",
    "SHARE_RAM_SIZE",
    "KERNEL_RAM_BASE",
    "KERNEL_RAM_SIZE",
    # PLV3 探针页：App 的代码/数据/栈落在镜像与内核 RAM 之外的空地，
    # 由链接脚本单独开一段 .appprobe（见 link.ld.in）。
    "ARKRTOS_QEMU_APP_BASE",
    "ARKRTOS_QEMU_APP_SIZE",
]


def collect(board_path):
    defines = parse_defines(PARTITION)
    if board_path:
        # 板级配置后解析，因此同名宏（CHIP_*）覆盖分区头的默认值
        defines.update(parse_defines(board_path))
    ev = MacroEval(defines)
    return [(name, ev.value(name)) for name in WANTED]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True)
    ap.add_argument("--board", default=os.path.normpath(BOARD),
                    help="板级配置头（默认 board/qemu_loongarch64/arkrtos_board_config.h）")
    args = ap.parse_args()

    values = collect(args.board if os.path.exists(args.board) else None)

    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    lines = [
        "/* 由 example/qemu_loongarch64/gen_ldconsts.py 生成，请勿手工编辑。 */",
        "/* 数值来源：config/arkrtos_partition.h + 板级配置头的 CHIP_* 覆盖 */",
    ]
    for name, value in values:
        lines.append("#define %-20s 0x%08X   /* %d */" % (name, value, value))
    lines.append("")

    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    for name, value in values:
        print("  %-20s 0x%08X" % (name, value))
    print("已生成 %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
