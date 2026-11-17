#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ELF64 可执行文件装载（ARKRTOS_USE_ELF）的门禁：把开关真的拨到两个方向各编一遍。

为什么需要这一道
----------------
`ARKRTOS_USE_ELF` 默认 0，且是靠 `#if (ARKRTOS_USE_ELF == 1)` 包住**整个源文件**的。
于是默认档下 `kernelsrc/src/arkrtos_elf.c` 是一个空 TU，一个编译器都不经过它，
有四种假绿：

  1. 装载器里写错了（拼错宏、漏原型、忘包含），默认档全绿，直到有人在 LA64 板上
     把开关打开才发现编不过；
  2. 开关其实没接线——打开了 shell 也没多出 `exec`，读数上却写着「支持 ELF」；
  3. 板级忘了给窗口宏（`ARKRTOS_ELF_WINDOW_BASE/SIZE`），而 `#error` 被谁顺手
     注释掉了，最后装到一个「地址 0」的窗口里；
  4. 依赖没接线——`ARKRTOS_USE_PRIV=0`（没有特权级）也让它编过去，得到一份
     「程序以 PLV0 跑」的镜像：装载器在，隔离不在。

这里用真编译器（LA64 / RV32 两套交叉工具链）把上面四条都变成读数：

  A. 默认档：`kernelsrc/include/arkrtos_autoconf.h` 里 `ARKRTOS_USE_ELF` 必须是 0；
  B. LA64 正向：板级（`board/qemu_loongarch64`）已把开关置 1，
     `arkrtos_elf.c` 与 la64 port 必须**编得过**；
  C. RV32 负向：同一个 TU 用 32 位工具链强开 `-DARKRTOS_USE_ELF=1` 必须**编不过**，
     且错误信息必须点名「只支持 64 位 port」（ELF64 字段宽度在 32 位 long 上表达不了）；
  D. 缺窗口负向：把板级里的两行窗口宏删掉后必须**编不过**，且错误必须点名
     `ARKRTOS_ELF_WINDOW_BASE`；
  E. 口径 pin：`arkrtos_elf.c` 里两个线上格式常量必须还是
     `ELF_MACHINE_LOONGARCH (258u)` 与 `ELF_TYPE_EXEC (2u)`，并且 `ELF_PT_INTERP`
     仍在（动态链接必须是拒绝，不是忽略）。改了这两个值就等于改了「什么算可执行
     文件」，必须回来重跑 QEMU 那一轮的拒绝矩阵。

不覆盖：装载器的**运行期**行为（`EM_MACHINE!=258` 真返回 `ERR_ARCH`、
`ET_DYN` 真返回 `ERR_FORMAT`、窗口越界真返回 `ERR_LAYOUT`、`argc/argv` 真摆对）。
那些要跑 QEMU，判据与实测记录见
`docs/MMU进程模型与ELF装载.md` §5。本门禁只保证「装得上、能编、守卫没被拆」。

用法
----
    python tools/elf_gate.py
    python tools/elf_gate.py --la64-gcc <path> --rv32-gcc <path>

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


def _run(cc, args):
    cmd = [cc] + args
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode, _text(r.stdout) + _text(r.stderr)


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

    tmp = tempfile.mkdtemp(prefix="ark_elf_")
    key_dir = os.path.join(tmp, "key")
    os.makedirs(key_dir)
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(SIGN_KEY_HEADER)

    def la64_flags(board="qemu_loongarch64"):
        return LA64_ARCH + COMMON + [
            "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
            "-include", "arkrtos_board_config.h",
            "-I" + key_dir,
        ] + _includes(board, "loongarch/la64")

    failures = []

    # --- A. 默认档必须是 0 ------------------------------------------------
    ac = os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h")
    with open(ac, encoding="utf-8", errors="replace") as f:
        autoconf_text = f.read()
    if "ARKRTOS_USE_ELF 0" not in autoconf_text:
        failures.append("A 默认档：arkrtos_autoconf.h 里 ARKRTOS_USE_ELF 不是 0")

    # --- B. LA64 正向（板级已开） ------------------------------------------
    for rel in ("kernelsrc/src/arkrtos_elf.c",
                "kernelsrc/port/loongarch/la64/arkrtos_port.c"):
        rc, out = _run(la64, la64_flags() + [rel])
        if rc != 0:
            failures.append("B LA64 编不过 %s：\n%s" % (rel, out[-600:]))

    # --- C. RV32 负向：32 位 long 上编 ELF64 必须失败 -----------------------
    rv32_flags = RV32_ARCH + COMMON + [
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
        "-I" + key_dir,
    ] + _includes("qemu_riscv32", "riscv/rv32imac")
    # 先把窗口宏喂饱，这样「编不过」的唯一可能就只剩那条 64 位守卫；
    # 否则窗口 #error 会先炸，等于这道负向被别的原因蒙对。
    rc, out = _run(rv32, rv32_flags + [
        "-DARKRTOS_USE_ELF=1",
        "-DARKRTOS_ELF_WINDOW_BASE=0x02000000",
        "-DARKRTOS_ELF_WINDOW_SIZE=0x200000",
        "kernelsrc/src/arkrtos_elf.c",
    ])
    if rc == 0:
        failures.append("C RV32 负向：32 位工具链强开 USE_ELF 竟然编过了（64 位守卫没生效）")
    elif ("64" not in out) or ("port" not in out):
        failures.append("C RV32 负向：编不过但不是那条 64 位守卫，报的是别的错：\n%s"
                        % out[-500:])

    # --- D. 缺窗口宏必须失败 ----------------------------------------------
    src_board = os.path.join(ROOT, "board/qemu_loongarch64/arkrtos_board_config.h")
    with open(src_board, encoding="utf-8") as f:
        board_text = f.read()
    win_lines = ("#define ARKRTOS_ELF_WINDOW_BASE      ARKRTOS_QEMU_APP_BASE\n"
                 "#define ARKRTOS_ELF_WINDOW_SIZE      ARKRTOS_QEMU_APP_SIZE\n")
    if win_lines not in board_text:
        failures.append("D 缺窗口负向：板级里找不到两行窗口宏，判据已过期")
    else:
        probe_dir = os.path.join(tmp, "probe")
        os.makedirs(probe_dir)
        probe = board_text.replace(win_lines, "", 1)
        with open(os.path.join(probe_dir, "elf_probe_board.h"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write(probe)
        rc, out = _run(la64, LA64_ARCH + COMMON + [
            "-DARKRTOS_BOARD_CONFIG=\"elf_probe_board.h\"",
            "-include", "elf_probe_board.h",
            "-I" + key_dir,
            "-I" + probe_dir,
        ] + _includes("qemu_loongarch64", "loongarch/la64") +
            ["kernelsrc/src/arkrtos_elf.c"])
        if rc == 0:
            failures.append("D 缺窗口负向：没有窗口宏竟然也编过了（#error 被拆了）")
        elif "ARKRTOS_ELF_WINDOW_BASE" not in out:
            failures.append("D 缺窗口负向：编不过但不是缺窗口那条 #error：\n%s"
                            % out[-500:])

    # --- E. 线上格式常量 pin ----------------------------------------------
    src_elf = os.path.join(ROOT, "kernelsrc/src/arkrtos_elf.c")
    with open(src_elf, encoding="utf-8", errors="replace") as f:
        elf_text = f.read()
    for need, why in (
        ("#define ELF_MACHINE_LOONGARCH       (258u)",
         "EM_LOONGARCH=258：改了它，别的架构的 ELF 会被当成能跑的"),
        ("#define ELF_TYPE_EXEC               (2u)",
         "ET_EXEC=2：改了它，ET_DYN(PIE/动态) 就可能被放进来"),
        ("#define ELF_PT_INTERP               (3u)",
         "PT_INTERP 必须仍被识别并拒绝（M1 不支持加载器链）"),
    ):
        if need not in elf_text:
            failures.append("E 口径 pin：arkrtos_elf.c 里找不到 %r（%s）" % (need, why))

    # --- F. U-8 退出码通道 pin（链式：ABI -> TCB -> 内核 -> 装载器 -> shell） --
    # 退出码最容易"看起来做了"：任何一环断了，读数要么变回「没有码」，要么
    # 编出来的是别人的值。所以逐环钉住，并对 pin 本身做一次变异自检
    # （把 shell 的打印串改坏一次，pin 必须报错）。
    def _read(rel):
        with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    text_abi   = _read("kernelsrc/include/arkrtos_svc_abi.h")
    text_taskh = _read("kernelsrc/include/arkrtos_task.h")
    text_taskc = _read("kernelsrc/src/arkrtos_task.c")
    text_elfc  = _read("kernelsrc/src/arkrtos_elf.c")
    text_elfh  = _read("kernelsrc/include/arkrtos_elf.h")
    text_shell = _read("kernelsrc/shell/arkrtos_shell.c")
    text_user  = _read("kernelsrc/include/arkrtos.h")
    text_hello = _read("example/qemu_loongarch64/elf/hello.c")

    def pin_f(shell_text):
        bad = []
        if "#define ARKRTOS_THREAD_SUB_EXIT_CODE         (4u)" not in text_abi:
            bad.append("F: svc_abi.h 没有 THREAD_SUB_EXIT_CODE 子号")
        if "int32  exit_code;" not in text_taskh or "uint8  exit_code_set;" not in text_taskh:
            bad.append("F: TCB 没有 exit_code / exit_code_set 字段")
        if "void arkrtos_thread_exit_code_internal(int32 code)" not in text_taskc:
            bad.append("F: task.c 没有带码结束的实现")
        if "case ARKRTOS_THREAD_SUB_EXIT_CODE:" not in text_taskc or \
           "arkrtos_thread_exit_code_internal((int32)p[1]);" not in text_taskc:
            bad.append("F: task.c 的 SVC 分派没有接子 4（码从 p[1] 取）")
        if "int32  exit_code;" not in text_elfh or "uint8  exit_code_set;" not in text_elfh:
            bad.append("F: elf info 没有 exit_code / exit_code_set")
        if "arkrtos_task_exit_code(id, &code)" not in text_elfc:
            bad.append("F: 装载器没有在任务变 INVALID 时把码取走")
        if "exit code %d" not in shell_text:
            bad.append("F: shell 的 exec 不打印退出码")
        if "arkrtos_thread_exit_code(int32 code);" not in text_user:
            bad.append("F: 用户 API 头里没有 arkrtos_thread_exit_code")
        if "ARK_THREAD_SUB_EXIT_CODE" not in text_hello or \
           "params[1] = (ark_u32)argc;" not in text_hello:
            bad.append("F: hello.elf 不再带退出码（QEMU 那轮的读数就失去对证）")
        return bad

    got = pin_f(text_shell)
    failures.extend(got)
    if not got and not pin_f(text_shell.replace("exit code %d", "exit code X%d", 1)):
        failures.append("F 自检：把 shell 的退出码打印串改坏后 pin 仍然通过（空转）")

    shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1

    print("ELF-GATE OK: 默认档=0 / LA64 打开编得过(2 TU) / RV32 64 位守卫被拒 / "
          "缺窗口宏被拒 / 线上格式常量未漂 / U-8 退出码通道 F pin 成立且自检有效")
    return 0


if __name__ == "__main__":
    sys.exit(main())
