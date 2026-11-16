#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""隔离后端切换（ARKRTOS_USE_PAGING）的门禁：把两个后端都真的编一遍。

为什么需要这一道
----------------
`ARKRTOS_USE_PAGING` 默认 0，而且是「二选一」的开关：默认档编的是
`arkrtos_mpu.c`（窗口后端），`arkrtos_paging.c` 整个文件是空的 TU，一个编译器
都不经过它；反过来打开它以后 `arkrtos_mpu.c` 变成空的。于是有三种假绿：

  1. 分页后端里写错了（拼写、漏原型、忘包含），默认档全绿，直到有人在
     LoongArch 上打开开关才发现编不过；
  2. 开关其实没接线——打开也还是窗口后端在跑，读数上却写着「分页」；
  3. `arkrtos_config.h` 那三条 #error 只是注释：无页表的核也让它编过去，
     两个后端同时开也不报，最后得到一份「看着有隔离、其实没有」的镜像。

这里用真编译器（LA64 / RV32 两套交叉工具链）把上面三条都变成读数：
  A. 默认档：`kernelsrc/include/arkrtos_autoconf.h` 里符号必须是 0；
  B. LA64 正向：板级把开关置 1，`arkrtos_paging.c` 与 la64 port 必须编得过；
  C. RV32 负向：同一个 TU 强开 `-DARKRTOS_USE_PAGING=1` 必须**编不过**，
     且错误信息必须点名 `ARKRTOS_USE_PAGING` 与 `HAS_PAGING_HW`；
  D. 互斥负向：把 LA64 板级改成 `USE_MPU=1` 与 `USE_PAGING=1` 同时成立，
     必须**编不过**，且错误信息必须点名「互斥」。

不覆盖：分页后端在板上给出的实际隔离效果（哪条地址被拦、PLV 是否真降）、
跨度审计计数是否正确。那些要跑 QEMU 或上板，见 docs/内存保护与App越权验证.md。

用法
----
    python tools/paging_gate.py
    python tools/paging_gate.py --la64-gcc <path> --rv32-gcc <path>

退出码 0 = 全过；1 = 有一步不成立或找不到编译器（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

LA64_FALLBACKS = [
    "D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe",
]
RV32_FALLBACKS = [
    "D:/Artchip/luban-lite-master/toolchain/bin/riscv64-unknown-elf-gcc.exe",
]

LA64_ARCH = ["-march=loongarch64", "-mabi=lp64d", "-mstrict-align"]
RV32_ARCH = ["-march=rv32imac", "-mabi=ilp32", "-mcmodel=medany", "-mno-relax"]

COMMON = [
    "-O0",
    "-Wall",
    "-ffreestanding",
    "-fno-common",
    "-nostdlib",
    "-nostartfiles",
    "-fno-builtin",
    "-fsyntax-only",
    "-DARKRTOS_SIGN_KEY_LOCAL=1",
]

# 与 example/*/Makefile 里的测试密钥同值；只为了让 arkrtos_features.h 的
# 「有签名开关就必须有密钥」那条 #error 过关，不参与任何判定。
SIGN_KEY_HEADER = """#ifndef ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }
#endif
"""


def _pick(cands, given, what):
    if given:
        return given
    for c in cands:
        if os.path.isfile(c):
            return c
    print("FAIL: 找不到 %s（缺编译器是 FAIL，不是跳过）" % what)
    sys.exit(1)


def _run(cc, args):
    cmd = [cc] + args
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode, _text(r.stdout) + _text(r.stderr)


def _text(raw):
    """内核头文件是 GBK，gcc 把源里的中文原样回显，所以两种编码都要试一遍。"""
    if not raw:
        return ""
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def _includes(board, port):
    return [
        "-Ikernelsrc/include",
        "-Iconfig",
        "-Iboard/" + board,
        "-Ikernelsrc/port/" + port,
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--la64-gcc", default="")
    ap.add_argument("--rv32-gcc", default="")
    a = ap.parse_args()

    la64 = _pick(LA64_FALLBACKS, a.la64_gcc, "LoongArch64 交叉编译器")
    rv32 = _pick(RV32_FALLBACKS, a.rv32_gcc, "RISCV32 交叉编译器")

    tmp = tempfile.mkdtemp(prefix="ark_paging_")
    key_dir = os.path.join(tmp, "key")
    os.makedirs(key_dir)
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(SIGN_KEY_HEADER)

    def board_flags(board):
        return [
            "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
            "-include", "arkrtos_board_config.h",
            "-I" + key_dir,
        ] + _includes(board, "loongarch/la64" if board == "qemu_loongarch64"
                      else "riscv/rv32imac")

    failures = []

    # --- A. 默认档必须是 0 ------------------------------------------------
    ac = os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h")
    with open(ac, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if "ARKRTOS_USE_PAGING 0" not in text and "ARKRTOS_USE_PAGING 0\n" not in text:
        failures.append("A 默认档：arkrtos_autoconf.h 里 ARKRTOS_USE_PAGING 不是 0")

    # --- B. LA64 正向 -----------------------------------------------------
    for rel in ("kernelsrc/src/arkrtos_paging.c",
                "kernelsrc/port/loongarch/la64/arkrtos_port.c"):
        rc, out = _run(la64, LA64_ARCH + COMMON + board_flags("qemu_loongarch64") + [rel])
        if rc != 0:
            failures.append("B LA64 编不过 %s：\n%s" % (rel, out[-500:]))

    # --- C. RV32 负向：无页表的核强开分页必须失败 -------------------------
    rv32_flags = RV32_ARCH + COMMON + \
        ["-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
         "-include", "arkrtos_board_config.h",
         "-I" + key_dir] + _includes("qemu_riscv32", "riscv/rv32imac")
    rc, out = _run(rv32, rv32_flags + ["-DARKRTOS_USE_PAGING=1",
                                      "kernelsrc/src/arkrtos_paging.c"])
    if rc == 0:
        failures.append("C RV32 负向：无页表的核强开 USE_PAGING 竟然编过了（守卫没生效）")
    elif ("ARKRTOS_USE_PAGING" not in out) or ("HAS_PAGING_HW" not in out):
        failures.append("C RV32 负向：编不过但不是那条守卫，报的是别的错：\n%s"
                        % out[-400:])

    # --- D. 互斥负向：两个后端同时开必须失败 -------------------------------
    src_board = os.path.join(ROOT, "board/qemu_loongarch64/arkrtos_board_config.h")
    with open(src_board, encoding="utf-8") as f:
        board_text = f.read()
    need = "#undef  ARKRTOS_USE_MPU\n#define ARKRTOS_USE_MPU              0"
    if need not in board_text:
        failures.append("D 互斥负向：板级里找不到 MPU 开关那两行，判据已过期")
    else:
        probe_dir = os.path.join(tmp, "probe")
        os.makedirs(probe_dir)
        probe = board_text.replace(
            need, "#undef  ARKRTOS_USE_MPU\n#define ARKRTOS_USE_MPU              1", 1)
        with open(os.path.join(probe_dir, "paging_probe_board.h"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write(probe)
        rc, out = _run(la64, LA64_ARCH + COMMON +
                       ["-DARKRTOS_BOARD_CONFIG=\"paging_probe_board.h\"",
                        "-include", "paging_probe_board.h",
                        "-I" + key_dir,
                        "-I" + probe_dir] +
                       _includes("qemu_loongarch64", "loongarch/la64") +
                       ["kernelsrc/src/arkrtos_paging.c"])
        if rc == 0:
            failures.append("D 互斥负向：USE_MPU=1 与 USE_PAGING=1 同时成立竟然编过了")
        elif "ARKRTOS_USE_MPU=1" not in out:
            failures.append("D 互斥负向：编不过但不是互斥那条 #error：\n%s" % out[-400:])

    shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1

    print("PAGING-GATE OK: 默认档=0 / LA64 打开编得过(2 TU) / RV32 负向被拒 / 两后端互斥被拒")
    return 0


if __name__ == "__main__":
    sys.exit(main())
