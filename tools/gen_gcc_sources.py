#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Ark RTOS GCC 源清单生成器
=========================

把某个 Keil (.uvprojx) 工程的文件清单翻译成 GNU Make 可 include 的
sources.mk，保证 GCC 构建与 Keil 构建**同一份文件集**（HAL 目录里躺着
几十个 .c，但工程只用其中一部分，绝不能靠 glob）。

四处必须替换：
  - 启动文件：Keil 用 MDK-ARM 下的 armasm 版；GCC 用 CMSIS 官方的 gcc 版；
  - 上下文切换：keil 版 arkrtos_context.S 是 armasm 语法，GCC 用
    arkrtos_context_gcc.s（GNU as 语法，逐条等价，见文件头说明）。
    注意用**小写 .s**：Windows 上的 GNU make（3.82）会把 %.s 与 %.S 两条模式
    规则视作冲突，导致 "No rule to make target"；本工程统一用 -x assembler-with-cpp
    强制预处理，扩展名大小写不影响行为。

用法：
    python tools/gen_gcc_sources.py \
        --uvprojx example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx \
        --out     example/stm32f427/kernel/ARKRTOS_TEST/GCC/sources.mk
"""

from __future__ import print_function

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 替换表：key/value 都是「相对仓库根」的正斜杠路径。
# Keil 的 <FilePath> 是相对 .uvprojx 所在目录（MDK-ARM）解析的，所以先把每个
# 条目归一成 repo 相对路径，再按这张表替换。
REPLACE = {
    # Keil 启动文件（armasm）→ CMSIS 官方 gcc 启动
    "example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/startup_stm32f427xx.s":
        "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Device/ST/"
        "STM32F4xx/Source/Templates/gcc/startup_stm32f427xx.s",
    "example/stm32f401/kernel/ARKRTOS_TEST/MDK-ARM/startup_stm32f401xe.s":
        "example/stm32f401/kernel/ARKRTOS_TEST/Drivers/CMSIS/Device/ST/"
        "STM32F4xx/Source/Templates/gcc/startup_stm32f401xe.s",
    # Keil armasm 上下文切换 → GNU as 版
    "kernelsrc/port/arm/cortex-m4/arkrtos_context.S":
        "kernelsrc/port/arm/cortex-m4/arkrtos_context_gcc.s",
    "kernelsrc/port/arm/cortex-m3/arkrtos_context.S":
        "kernelsrc/port/arm/cortex-m3/arkrtos_context_gcc.s",
    # 镜像（App / 驱动 / 小程序）armasm 入口 → GNU as 版入口
    "kernelsrc/sdk/app_sdk/arkrtos_app_start.s":
        "kernelsrc/sdk/app_sdk/arkrtos_app_start_gcc.s",
    "kernelsrc/sdk/driver_sdk/arkrtos_drv_start.s":
        "kernelsrc/sdk/driver_sdk/arkrtos_drv_start_gcc.s",
    "kernelsrc/sdk/mini_sdk/arkrtos_mini_start.s":
        "kernelsrc/sdk/mini_sdk/arkrtos_mini_start_gcc.s",
}


def repo_rel(path, base):
    """把 path（相对 base 目录）归一成「相对仓库根」的正斜杠路径"""
    return os.path.relpath(os.path.normpath(os.path.join(base, path)),
                           REPO_ROOT).replace("\\", "/")


def main():
    ap = argparse.ArgumentParser(description="从 .uvprojx 派生 GCC 源清单")
    ap.add_argument("--uvprojx", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    uv = args.uvprojx
    if not os.path.isabs(uv):
        uv = os.path.join(REPO_ROOT, uv)
    if not os.path.isfile(uv):
        raise SystemExit("找不到 .uvprojx: %s" % uv)

    # Keil 的 <FilePath> 相对 .uvprojx 所在目录（MDK-ARM）解析
    proj_dir = os.path.dirname(os.path.abspath(uv))

    with open(uv, "r", encoding="utf-8", errors="replace") as f:
        xml = f.read()
    raw_files = re.findall(r"<FilePath>(.*?)</FilePath>", xml)
    if not raw_files:
        raise SystemExit(".uvprojx 里没找到任何 <FilePath>")

    # 统一成「相对仓库根」的正斜杠路径
    rel = []
    for p in raw_files:
        p = p.replace("\\", "/").strip()
        rel.append(repo_rel(p, proj_dir))

    # 应用替换表（key/value 都是 repo 相对路径）
    replaced = [REPLACE.get(rp, rp) for rp in rel]

    # 去重（保序）
    seen = set()
    files = []
    for rp in replaced:
        if rp not in seen:
            seen.add(rp)
            files.append(rp)

    # files 已经是相对仓库根的路径，Makefile 里统一加 $(ROOT)/ 前缀
    out_files = files

    # 只列 arm-none-eabi-gcc 能直接吃的：.c / .S / .s
    compilable = [p for p in out_files if p.lower().endswith((".c", ".s"))]

    lines = []
    lines.append("# 本文件由 tools/gen_gcc_sources.py 自动生成，请勿手工编辑\n")
    lines.append("# 源头: %s\n" % os.path.relpath(uv, REPO_ROOT).replace("\\", "/"))
    lines.append("# 与 Keil 工程同一文件集；启动文件与上下文切换已按 GCC 工具链替换\n")
    lines.append("\nSRCS := \\\n")
    for i, p in enumerate(compilable):
        cont = " \\" if i + 1 < len(compilable) else ""
        lines.append("  %s%s\n" % (p, cont))

    text = "".join(lines)
    d = os.path.dirname(os.path.abspath(args.out))
    if d and not os.path.isdir(d):
        os.makedirs(d)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("[gen_gcc_sources] %d files (of %d) -> %s"
          % (len(compilable), len(out_files), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
