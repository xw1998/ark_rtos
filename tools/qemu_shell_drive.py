#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在 QEMU 里交互式驱动 Ark RTOS 的 shell，并把「说过什么、回了什么」变成判据。

为什么需要它
------------
QEMU 的 `-serial file:xxx.log` 只能收输出，**喂不进去**；而 `-nographic`
的串口是 `mon:stdio` 复用口，管道 stdin 送进去的字节会走 monitor 复用层，
在实测里表现为「字符有回显但回车不生效」——命令一次都没真正执行过。

正确做法是把串口换成 **TCP chardev**：QEMU 监听一个本地端口，脚本连上去
按字节发，回车发 `\\r`。这条路已经实测跑通 `help / modules / lkup / ld /
unld`（见 docs/MMU平台可下载模块-VxWorks式.md §4.1）。

用法
----
    python tools/qemu_shell_drive.py --modld-check
    python tools/qemu_shell_drive.py --cmd "uname -a" --cmd "free"
    python tools/qemu_shell_drive.py --script my_cmds.txt --timeout 2

`--modld-check` 是本次模块功能的验收序列（每条都带期望子串，缺一条就 FAIL）；
`--cmd` 只发不判，用来手工看；`--script` 一行一条，`#` 开头跳过。

退出码 0 = 全部期望都出现过；1 = 有缺失 / 连不上串口 / QEMU 起不来。
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import time

QEMU_FALLBACKS = [
    "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
]

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# --modld-check 的序列：(命令, [必须出现的子串...])
# 栈底守卫：示例板上有一个任务故意踩自己的栈底字，切换时被摘除并记一条
# ARKRTOS_FAULT_STACKOVF；fault 命令把它打出来，任务号在 task 列。
STACK_GUARD_CHECK = [
    ("fault", ["STACKOVF"]),
]

MODLD_CHECK = [
    ("lkup", ["kernel exports (6):",
              "arkrtos_mod_printf",
              "arkrtos_console_write",
              "arkrtos_kernel_get_tick",
              "arkrtos_task_delay_internal",
              "arkrtos_task_get_current",
              "arkrtos_thread_exit_internal"]),
    ("lkup arkrtos_mod_printf", ["arkrtos_mod_printf = 0x"]),
    ("modules", ["idx", "name", "state", "modhello.o", "done"]),
    ("ld /mnt/ram/modhello.o alpha 42",
     ["[modhello] hello from a relocatable module",
      "[modhello] argc=3",
      # argv[0] 是**用户敲进去的那个路径**（cmd_ld 把 argv[1] 原样传给装载器，
      # VxWorks 的 ld 也是这样）。老期望写的是 modhello.o，那不是内核行为：
      # 当时 shell 的 3 KB 栈在 ld 期间被用到 100%，溢出写坏了装载器静态区里的
      # argv 指针，打印出来的才是被截短的 basename。见 QEMU 文档 §5.15。
      "[modhello] argv[0]=/mnt/ram/modhello.o",
      "[modhello] argv[1]=alpha",
      "[modhello] argv[2]=42",
      "[modhello] bye",
      "165 reloc(s), 8 import(s)"]),
    ("modules", ["modhello.o"]),
    ("unld 0", ["slot released"]),
    ("modules", ["modhello.o"]),
]


def _strip(s):
    return ANSI.sub("", s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default="")
    ap.add_argument("--elf", default="",
                    help="默认 <repo>/build/qemu_loongarch64/ark_qemu_la64.elf")
    ap.add_argument("--port", type=int, default=45553)
    ap.add_argument("--boot", type=float, default=30.0,
                    help="等 shell 提示符的最长秒数")
    ap.add_argument("--timeout", type=float, default=3.0,
                    help="每条命令等输出的秒数")
    ap.add_argument("--cmd", action="append", default=[],
                    help="只发不判的命令，可多次给")
    ap.add_argument("--script", default="", help="一行一条命令的文件")
    ap.add_argument("--stack-guard-check", action="store_true",
                    help="栈底守卫：fault 里必须出现 STACKOVF")
    ap.add_argument("--modld-check", action="store_true",
                    help="跑可下载模块的验收序列并判每条的期望子串")
    a = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    qemu = a.qemu
    if not qemu:
        for c in QEMU_FALLBACKS:
            if os.path.isfile(c):
                qemu = c
                break
    if not qemu or not os.path.isfile(qemu):
        print("FAIL: 找不到 QEMU（缺工具是 FAIL，不是跳过）")
        return 1

    elf = a.elf or os.path.join(root, "build", "qemu_loongarch64",
                                "ark_qemu_la64.elf")
    if not os.path.isfile(elf):
        print("FAIL: 找不到 %s（先 make -f example/qemu_loongarch64/Makefile）" % elf)
        return 1

    # QEMU 在 Windows 上打不开带非 ASCII 字符的路径（实测 "could not load
    # kernel ... Failed to load ELF"），所以切到 elf 所在目录、只用文件名。
    build_dir = os.path.dirname(os.path.abspath(elf))
    p = subprocess.Popen(
        [qemu, "-machine", "virt", "-kernel", os.path.basename(elf),
         "-display", "none", "-monitor", "none",
         # wait=off：QEMU 立刻起跑，不等客户端。代价是连上之前的开机输出
         # （含第一条提示符）会丢，所以下面用"敲回车再看有没有提示符"来判就绪。
         # 实测 wait=on 在这版 QEMU 上接上之后命令不进 shell，故不采用。
         "-serial", "tcp:127.0.0.1:%d,server=on,wait=off" % a.port],
        cwd=build_dir, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    try:
        s = None
        for _ in range(int(a.boot * 4) + 40):
            if p.poll() is not None:
                print("FAIL: QEMU 立刻退出了（rc=%d）——检查机器类型与 kernel" % p.returncode)
                return 1
            try:
                s = socket.create_connection(("127.0.0.1", a.port), timeout=1)
                break
            except OSError:
                time.sleep(0.25)
        if s is None:
            print("FAIL: 连不上 127.0.0.1:%d（端口被占或 QEMU 没起来）" % a.port)
            return 1
        s.settimeout(0.5)

        buf = []

        def pump(sec):
            end = time.time() + sec
            while time.time() < end:
                try:
                    d = s.recv(8192)
                except socket.timeout:
                    continue
                if not d:
                    return
                buf.append(_strip(d.decode("utf-8", "replace")))

        # 等提示符（形如 "mark@arkrtos:~$ "）。提示符带 ANSI 颜色，
        # 所以必须先去转义再找。
        deadline = time.time() + a.boot
        while time.time() < deadline:
            s.sendall(b"\r")          # 敲一下回车：既触发提示符，也确认输入通路
            pump(1.0)
            if "@arkrtos" in "".join(buf):
                break
        else:
            print("FAIL: %.0f s 内没等到 shell 提示符；最近的输出：\n%s"
                  % (a.boot, "".join(buf)[-800:]))
            return 1

        # 组装序列
        seq = []
        if a.modld_check:
            seq += MODLD_CHECK
        if a.stack_guard_check:
            seq += STACK_GUARD_CHECK
        if a.script:
            with open(a.script, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#"):
                        seq.append((line, []))
        for c in a.cmd:
            seq.append((c, []))

        results = []
        for cmd, expects in seq:
            mark = len("".join(buf))
            s.sendall((cmd + "\r").encode())
            # 固定等一段时间：内核日志（[A]/[B] 每秒一行）会插在提示符后面，
            # 靠"行尾是 $ "判断命令跑完并不可靠，宁可按时间收口。
            pump(a.timeout)
            got = "".join(buf)[mark:]
            missing = [e for e in expects if e not in got]
            results.append((cmd, got, missing))

        print("==== QEMU shell transcript（%d 条命令） ====" % len(seq))
        for cmd, got, missing in results:
            print("\n$ " + cmd)
            print(got.rstrip() or "(no output)")
            if missing:
                print("  ** 缺：" + " / ".join(missing))

        bad = [(c, m) for c, _g, m in results if m]
        if bad:
            print("\nFAIL: %d 条命令的期望没出现" % len(bad))
            for c, m in bad:
                print("  - %s：缺 %s" % (c, " / ".join(m)))
            return 1
        print("\nSHELL-DRIVE OK: %d 条命令全部符合预期" % len(seq))
        return 0
    finally:
        try:
            p.terminate()
            p.wait(timeout=5)
        except Exception:
            p.kill()


if __name__ == "__main__":
    sys.exit(main())
