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
  C. RV32 正向（U-4 起）：同一个 TU 用 32 位工具链强开 `-DARKRTOS_USE_ELF=1` 必须
     **编得过**。这里是 2026-11-17 的判据翻转：U-4 把装载器改成「按 port 的 long
     宽度选 ELF32/ELF64 字段表」，RV32 不再是被拒绝的档。旧的「必须编不过（只支持
     64 位 port）」判据留着会变成假绿——它要求的东西已经不存在了；
  D. 缺窗口负向：把板级里的两行窗口宏删掉后必须**编不过**，且错误必须点名
     `ARKRTOS_ELF_WINDOW_BASE`；
  G. 接线 pin（U-4 收尾）：ELF32 字段偏移表、Makefile 的链接基址与板级窗口宏
     对账、`arkrtos_mpu_rom_window()` 里的 ELF 窗口分支、装载期「注册→降权→
     写回」是否真在同一临界区内、以及 `hello32` 的退出兜底。每条都配一次变异
     自检（把被 pin 的字串改坏，pin 必须报错），防止 pin 空转。
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

import shell_src

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

    # --- C. RV32 正向：32 位工具链开 USE_ELF 必须编得过（U-4） -------------
    rv32_flags = RV32_ARCH + COMMON + [
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
        "-I" + key_dir,
    ] + _includes("qemu_riscv32", "riscv/rv32imac")
    # 判据在 U-4 翻转：32 位 port 走 ELF32 字段表，不再有「只支持 64 位」那条守卫。
    # 这一条必须真编，否则「RV32 也支持 ELF」这句话就没有编译器背过书。
    rc, out = _run(rv32, rv32_flags + [
        "-DARKRTOS_USE_ELF=1",
        "kernelsrc/src/arkrtos_elf.c",
    ])
    if rc != 0:
        failures.append("C RV32 正向：32 位工具链开 USE_ELF 编不过：\n%s" % out[-700:])

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
    # Shell 已按命令族拆分：pin 读全部 shell 源文件的并集（见 tools/shell_src.py）。
    text_shell = "\n".join(_read(rel) for rel in shell_src.rel_list(ROOT))
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
        # 这一串只属于 exec。写宽了（只匹配 "exit code %d"）会让下面的变异
        # 自检失去分辨力：wait 命令里也有同样的格式串，改坏 exec 那处仍能通过。
        if "finished after %u ms (space %u, exit code %d)" not in shell_text:
            bad.append("F: shell 的 exec 不打印退出码")
        if "arkrtos_thread_exit_code(int32 code);" not in text_user:
            bad.append("F: 用户 API 头里没有 arkrtos_thread_exit_code")
        if "ARK_THREAD_SUB_EXIT_CODE" not in text_hello or \
           "params[1] = (ark_u32)argc;" not in text_hello:
            bad.append("F: hello.elf 不再带退出码（QEMU 那轮的读数就失去对证）")
        return bad

    got = pin_f(text_shell)
    failures.extend(got)
    if not got and not pin_f(text_shell.replace(
            "finished after %u ms (space %u, exit code %d)",
            "finished after %u ms (space %u, exit code X%d)", 1)):
        failures.append("F 自检：把 shell 的退出码打印串改坏后 pin 仍然通过（空转）")

    # --- F2. U-8 的另一半：wait 命令 + App 侧 main 返回值转码（2026-11-17） --
    # 这两处最容易被拆成「只做了一半」：shell 有 wait 但读的是别人取走过的槽、
    # 或者 App SDK 的 exit(code) 又把码丢了（那样 return 3 与 return 0 在板上
    # 长得一模一样）。同样配一次变异自检。
    text_appmain = _read("kernelsrc/sdk/app_sdk/arkrtos_app_main.c")

    def pin_f2(shell_text, app_text):
        bad = []
        if "int cmd_wait(int argc, char *argv[])" not in shell_text:
            bad.append("F2: shell 没有 cmd_wait")
        if 'ARK_SHELL_CMD("wait", cmd_wait' not in shell_text:
            bad.append("F2: wait 没有注册进命令表")
        if "(ARKRTOS_USE_EXIT_CODE == 1) ? 1 : 0" not in shell_text:
            bad.append("F2: register 的 need 预算没有算上 wait（上限一满就静默丢命令）")
        if "arkrtos_task_table[id32 - 1u].status == ARKRTOS_TASK_INVALID" not in shell_text:
            bad.append("F2: wait 不是按 INVALID 判结束")
        if "arkrtos_task_exit_code((int32)id32, &code)" not in shell_text:
            bad.append("F2: wait 没有从 TCB 取退出码")
        if "no exit code reported" not in shell_text:
            bad.append("F2: wait 不区分「没上报过码」与「码是 0」")
        if "arkrtos_thread_exit_code(code);" not in app_text:
            bad.append("F2: App SDK 的 exit(code) 没有把码交给内核（main 返回值丢了）")
        if "#if (ARKRTOS_USE_EXIT_CODE == 1)" not in app_text:
            bad.append("F2: App SDK 的转码没有开关闸门")
        return bad

    got2 = pin_f2(text_shell, text_appmain)
    failures.extend(got2)
    if not got2 and not pin_f2(text_shell, text_appmain.replace(
            "arkrtos_thread_exit_code(code);", "arkrtos_thread_exit();", 1)):
        failures.append("F2 自检：把 App SDK 的转码改回普通退出后 pin 仍然通过（空转）")
    if not got2 and not pin_f2(text_shell.replace(
            "arkrtos_task_exit_code((int32)id32, &code)",
            "arkrtos_task_exit_code(id32, 0)", 1), text_appmain):
        failures.append("F2 自检：把 wait 的取码改成不落 code 后 pin 仍然通过（空转）")

    # F2b：另外两条退出路径——小程序 SDK 的 main 返回值、可下载模块的入口
    # 返回值。两处都是「码已经到手、只是没交出去」，所以 pin 必须落在
    # 「接住」和「交出去」两个动作上，只看有没有 #if 是抓不到的。
    text_mini  = _read("kernelsrc/sdk/mini_sdk/arkrtos_mini_main.c")
    text_modld = _read("kernelsrc/src/arkrtos_modld.c")

    def pin_f2b(mini, modld):
        bad = []
        if "#if (ARKRTOS_USE_EXIT_CODE == 1)" not in mini:
            bad.append("F2b: 小程序 SDK 的转码没有开关闸门（关档会多出符号）")
        if "arkrtos_thread_exit_code(code);" not in mini:
            bad.append("F2b: 小程序 SDK 的 exit(code) 没有把码交给内核")
        if "rc = (int32)p_fn((int)s_slots[i].argc, s_mod_argp[i]);" not in modld:
            bad.append("F2b: 模块入口的返回值没有被接住（码到手就丢）")
        if "arkrtos_thread_exit_code_internal(rc);" not in modld:
            bad.append("F2b: 模块退出路径没有把码交给内核")
        return bad

    got2b = pin_f2b(text_mini, text_modld)
    failures.extend(got2b)
    if not got2b and not pin_f2b(
            text_mini.replace("arkrtos_thread_exit_code(code);",
                              "arkrtos_thread_exit();", 1), text_modld):
        failures.append("F2b 自检：小程序 SDK 改回普通退出后 pin 仍然通过（空转）")
    if not got2b and not pin_f2b(
            text_mini,
            text_modld.replace("arkrtos_thread_exit_code_internal(rc);",
                               "arkrtos_thread_exit_internal();", 1)):
        failures.append("F2b 自检：模块退出改回不带码后 pin 仍然通过（空转）")

    # --- G. 接线 pin（U-4 收尾）-----------------------------------------
    # 这些 pin 防的都是「看起来做了、其实没接线」：字段偏移抄错一位、链接基址
    # 与板级窗口宏各写一个值、MPU 的 ELF 分支被谁删了、竞态修复的临界区只包了
    # 一半、示例程序退出路径没有兜底。每条都配一次变异自检，pin 空转要被抓到。
    import re as _re

    text_mpu    = _read("kernelsrc/src/arkrtos_mpu.c")
    text_board  = _read("board/qemu_riscv32/arkrtos_board_config.h")
    text_mk     = _read("example/qemu_riscv32/Makefile")
    text_hello32 = _read("example/qemu_riscv32/elf/hello32.c")

    # 32 位 ELF 头/程序头的字段偏移（一个都不能错：错了就是「装进去了但跑飞」）
    ELF32_FIELDS = [
        ("ELF_EHDR_SIZE", 52), ("ELF_PHDR_SIZE", 32),
        ("ELF_WORD_SIZE", 4),
        ("ELF_EH_ENTRY", 24), ("ELF_EH_PHOFF", 28),
        ("ELF_EH_PHENTSIZE", 42), ("ELF_EH_PHNUM", 44),
        ("ELF_PH_OFFSET", 4), ("ELF_PH_VADDR", 8),
        ("ELF_PH_FILESZ", 16), ("ELF_PH_MEMSZ", 20),
    ]

    def _elf_win(text):
        m = _re.search(r"ELF_WIN\s*:=\s*(0x[0-9A-Fa-f]+)", text)
        return int(m.group(1), 16) if m else None

    def _board_win(text):
        m = _re.search(r"#define\s+ARKRTOS_ELF_WINDOW_BASE\s+(0x[0-9A-Fa-f]+)u", text)
        return int(m.group(1), 16) if m else None

    def _func_body(text, name):
        i = text.find(name)
        if i < 0:
            return ""
        j = text.find("\nstatic ", i + 1)
        if j < 0:
            j = len(text)
        return text[i:j]

    def g_pins(elfc, mpu, board, mk, hello32):
        bad = []
        for name, val in ELF32_FIELDS:
            if not _re.search(r"#define\s+%s\s+\(%du\)" % (name, val), elfc):
                bad.append("G: 32 位字段表里 %s 不是 %du（偏移抄错就会跑飞）" % (name, val))
        w_mk, w_board = _elf_win(mk), _board_win(board)
        if w_mk is None:
            bad.append("G: RV32 Makefile 里找不到 ELF_WIN := 0x...（链接基址）")
        if w_board is None:
            bad.append("G: 板级里找不到 ARKRTOS_ELF_WINDOW_BASE 的数值定义")
        if (w_mk is not None) and (w_board is not None) and (w_mk != w_board):
            bad.append("G: Makefile ELF_WIN=0x%08X 与板级窗口基址 0x%08X 不一致"
                       "（装载器会报『段在窗口外』，其实只是链接基址挪了）"
                       % (w_mk, w_board))
        if "ARKRTOS_MPU_ELF_WINDOW_BASE" not in _func_body(mpu, "arkrtos_mpu_rom_window("):
            bad.append("G: arkrtos_mpu_rom_window() 里没有 ELF 窗口分支"
                       "（U 模式一取指就 fault，且没有任何报错）")
        # 装载期竞态：注册 -> 降权 -> MPU 写回 必须在同一个临界区内
        i_enter = elfc.find("crit = ARKRTOS_ENTER_CRITICAL();")
        i_reg   = elfc.find("id = arkrtos_task_register(")
        i_priv  = elfc.find(".is_priv = 0u;", i_reg if i_reg > 0 else 0)
        i_mpu   = elfc.find("arkrtos_mpu_build_task(p_task);", i_reg if i_reg > 0 else 0)
        # 取最后一处 EXIT：中途「注册失败」那条早退也在临界区内，
        # 用 find 会命中它，判据就假绿了。
        i_exit  = elfc.rfind("ARKRTOS_EXIT_CRITICAL(crit);")
        if min(i_enter, i_reg) < 0 or not (i_enter < i_reg < i_priv < i_mpu < i_exit):
            bad.append("G: 装载期竞态修复不成立——注册→降权→MPU 写回 没有整体落在"
                       " ARKRTOS_ENTER/EXIT_CRITICAL 之间")
        if not _re.search(r"#define\s+ARK_THREAD_SUB_EXIT\s+3u", hello32):
            bad.append("G: hello32 没有 EXIT（子 3）定义")
        if "params[0] = ARK_THREAD_SUB_EXIT;" not in hello32:
            bad.append("G: hello32 退出路径没有发子 3 兜底"
                       "（本档没编 EXIT_CODE 时程序会掉进死循环，shell 永不返回）")
        # 默认档零变化：给 MPU 的窗口别名只能在开 ELF 档才定义
        m_guard = _re.search(r"#if defined\(ARKRTOS_USE_ELF\) && \(ARKRTOS_USE_ELF == 1\)", board)
        m_alias = board.find("ARKRTOS_MPU_ELF_WINDOW_BASE")
        if (m_guard is None) or (m_alias < m_guard.start()):
            bad.append("G: 板级里 ARKRTOS_MPU_ELF_WINDOW_* 没被 USE_ELF 守住"
                       "（默认档会编进 ELF 窗口分支，体积就变了）")
        return bad

    got = g_pins(text_elfc, text_mpu, text_board, text_mk, text_hello32)
    failures.extend(got)
    if not got:
        # 变异自检 1：把 EXIT 兜底拿掉，pin 必须报错
        if not g_pins(text_elfc, text_mpu, text_board, text_mk,
                      text_hello32.replace("params[0] = ARK_THREAD_SUB_EXIT;", "params[0] = 0u;", 1)):
            failures.append("G 自检1：把 hello32 的退出兜底改坏后 pin 仍然通过（空转）")
        # 变异自检 2：把临界区入口改坏，pin 必须报错
        if not g_pins(text_elfc.replace("crit = ARKRTOS_ENTER_CRITICAL();", "crit = 0u;", 1),
                      text_mpu, text_board, text_mk, text_hello32):
            failures.append("G 自检2：把装载期临界区改坏后 pin 仍然通过（空转）")

    shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1

    print("ELF-GATE OK: 默认档=0 / LA64 打开编得过(2 TU) / RV32 打开也编得过(U-4) / "
          "缺窗口宏被拒 / 线上格式常量未漂 / U-8 退出码通道 F pin 与 F2/F2b（wait / App·小程序 SDK / 模块）成立且自检有效 / "
          "U-4 接线 G pin 成立且两项变异自检有效")
    return 0


if __name__ == "__main__":
    sys.exit(main())
