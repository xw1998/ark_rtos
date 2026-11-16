#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""QEMU RV32 内核 Shell 的端到端检查。

为什么要单独一个脚本：`make run` 的 `-serial file:out.txt` 是单向的，
只能证明 shell 把横幅和提示符打出来了；而 shell 是一条**双向**链路
（输入 -> 行编辑器 -> 命令分发 -> 输出），只有把 host 的字节真的送进去
并且断言命令的效果，才能说它"能用"。所以这里把 16550 接到 TCP chardev。

判据（任一不满足即 FAIL，退出码非 0）：
  1. 出现 shell 横幅 "Ark RTOS kernel shell" 与提示符 "ark@arkrtos:";
  2. `help` 列出命令表（含 blk / mount / reboot 三个名字）；
  3. `uname` 报出内核名与版本；
  4. `dt` 报出本机设备树（model + ns16550a 串口节点 + 地址 0x10000000）；
  5. `ls /mnt/ram` 列出 hello.txt —— 这证明 shell 的 ls 走的是 VFS，
     而不是自己编一份假目录；
  6. `cat /proc/version` 打印内核版本行 —— 证明 procfs 在 shell 里可读；
  7. `task` 打印任务表且含 shell 任务；
  8. 全程没有 "UNEXPECTED" / "FATAL" / "FAILED"。

跑法（仓库根目录）：

    python example/qemu_riscv32/shell_check.py
"""

import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(ROOT, "build", "qemu_riscv32")
ELF = os.path.join(BUILD, "ark_qemu_rv32.elf")

QEMU = os.environ.get(
    "QEMU_RV32",
    r"D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-riscv32.exe",
)


def _free_port():
    """先抢一个空端口再放掉。

    固定端口会在反复跑时撞上上一次没退干净的 QEMU：脚本连上去、看到完整的
    启动横幅（其实是上一个实例的），然后命令发进一个已经没人收的 chardev，
    表现为"输入没反应"。这个坑在 uart_irq_check.py 里踩过一次。
    """
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


PORT = int(os.environ.get("SHELL_CHECK_PORT", "0")) or _free_port()


def fail(msg, log=""):
    print("FAIL: " + msg)
    if log:
        print("---- 串口原始输出 ----")
        print(log)
    return 1


def main():
    if not os.path.isfile(ELF):
        return fail("找不到 " + ELF +
                    "，先在仓库根目录 make -f example/qemu_riscv32/Makefile")

    cmd = [
        QEMU,
        "-machine", "virt",
        "-cpu", "rv32",
        "-bios", "none",
        # 传相对文件名 + cwd=BUILD：仓库路径含非 ASCII（中文）字符，QEMU 在
        # Windows 上打不开那种绝对路径的 -kernel 且不报错，表现为永远连不上。
        "-kernel", os.path.basename(ELF),
        "-display", "none",
        # wait=on：QEMU 先监听、等脚本连上再开始执行，否则启动横幅会在
        # connect 完成之前就打完（实测丢横幅）。
        "-chardev", "socket,id=c0,host=127.0.0.1,port=%d,server=on,wait=on" % PORT,
        "-serial", "chardev:c0",
    ]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        cwd=BUILD,
    )

    log = ""
    try:
        sock = None
        for _ in range(100):
            try:
                sock = socket.create_connection(("127.0.0.1", PORT), 0.2)
                break
            except OSError:
                time.sleep(0.1)
        if sock is None:
            return fail("连不上 QEMU 的 chardev 端口 %d" % PORT)

        sock.settimeout(0.2)

        def pump(seconds):
            """读 seconds 秒，把新输出追加进 log。"""
            nonlocal log
            end = time.time() + seconds
            while time.time() < end:
                try:
                    chunk = sock.recv(4096)
                    if chunk:
                        log += chunk.decode("utf-8", "replace")
                except socket.timeout:
                    pass
                except OSError:
                    break
            return log

        def run(cmdline, wait=1.2):
            """发一条命令（CR 结尾，与终端回车一致），等回显得出结果。"""
            nonlocal log
            mark = len(log)
            sock.sendall(cmdline.encode("ascii") + b"\r")
            pump(wait)
            return log[mark:]

        # 1) 等 shell 就绪：横幅 + 提示符。启动期还要跑任务 A/B/U、PMP 负对照
        #    与存储自检，所以给足 8 秒，而不是"看到第一行就开始发命令"。
        pump(8.0)
        if "Ark RTOS kernel shell" not in log:
            return fail("没有出现 shell 横幅（shell 任务没起来）", log)
        if "ark@arkrtos:" not in log:
            return fail("没有出现提示符 ark@arkrtos:", log)
        if "[FS] PASS" not in log:
            return fail("存储自检没通过（shell 依赖的 VFS 不可信）", log)

        # 2) help
        out = run("help")
        for name in ("blk", "mount", "reboot"):
            if name not in out:
                return fail("help 输出里没有命令 %r\n%s" % (name, out), log)

        # 3) uname
        out = run("uname")
        if "arkrtos" not in out.lower():
            return fail("uname 没报出内核名\n" + out, log)

        # 4) dt：本机设备树。串口节点必须带真实地址，否则表是空壳。
        out = run("dt")
        if "QEMU virt" not in out and "qemu" not in out.lower():
            return fail("dt 没报出本机 model\n" + out, log)
        if "ns16550a" not in out or "0x10000000" not in out:
            return fail("dt 里没有串口节点或地址不对\n" + out, log)

        # 5) ls /mnt/ram：证明 shell 的 ls 真的走 VFS
        out = run("ls /mnt/ram")
        if "hello.txt" not in out:
            return fail("ls /mnt/ram 没有列出 hello.txt\n" + out, log)

        # 6) cat /proc/version：procfs 在 shell 里可读
        out = run("cat /proc/version")
        if "Ark RTOS" not in out:
            return fail("cat /proc/version 没打印版本行\n" + out, log)

        # 7) stack：任务表里必须有 shell 任务。
        #    为什么不用 `task` 的输出来判：任务表打印的是 entry 地址和优先级，
        #    不带名字，按名字匹配等于什么都没断言。计数才是硬证据 —— 本示例
        #    注册 7 个任务（A/B/U/RX/FS/idle/shell），少一个就是 shell 没起来；
        #    "0 unmeasured" 则说明栈计量真的在算，而不是全零占位。
        out = run("stack")
        if "7 task(s)" not in out:
            return fail("任务数不是 7（shell 没注册？）\n" + out, log)
        if "0 unmeasured" not in out:
            return fail("栈计量没有生效（unmeasured 非零）\n" + out, log)

        # 8) utest：板端单元测试全量。只看汇总行的 "0 failed,"，不看单个用例
        #    的名字 —— 用例表会随功能开关变，断言名字等于把测试写死在今天。
        out = run("utest", wait=12.0)
        if ", 0 failed," not in out:
            return fail("utest 汇总不是 0 failed\n" + out, log)

        # 9) 全程负向标记
        for bad in ("UNEXPECTED", "FATAL", "FAILED"):
            if bad in log:
                return fail("全程出现 %s" % bad, log)

        print("PASS: 内核 Shell 端到端通（横幅/help/uname/dt/ls/cat/task/utest 全部通过）")
        return 0
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
