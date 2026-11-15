#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""QEMU RV32 中断驱动串口的端到端检查。

为什么要有这个脚本，而不是 `make run` 之后肉眼看 out.txt：

  `-serial file:out.txt` 是**单向**的（guest -> host）。"中断接收"这条
  链路必须有 host -> guest 的字节流才可能被验证，否则只能验证发送，
  而发送本来就是轮询。所以这里把 16550 接到一个 TCP chardev 上：
  脚本既发字节又收回显，判据可以写成断言，而不是"看着像通了"。

跑法（仓库根目录）：

    python example/qemu_riscv32/uart_irq_check.py

判据（任一不满足即 FAIL，退出码非 0）：
  1. 启动横幅里出现 "[boot] uart irq on"；
  2. 发送 b"hello\\n" 之后出现 "[R] wake=" 行，且 isr= / rx= 都 > 0
     —— 这一条证明字节走的是中断（ISR 累加计数），不是轮询；
  3. payload 落到哪里，按编译档位分别判：
       - shell=0（控制台归示例的接收任务）：回显行里必须出现 "-> hello"；
       - shell=1（控制台归内核 Shell，同一个 FIFO 只能有一个读者）：
         必须能看到 shell 对这个 payload 的响应（"hello" 被回显并被执行，
         结果是 "command not found"）。此时"-> hello" 不可能出现，因为
         接收任务按设计**不取字节**（见 main.c 的 task_rx）。
  4. 全程没有 "UNEXPECTED" / "FATAL"。
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
    """先把端口抢一下再放掉。

    固定端口在反复跑的时候会撞上上一次没退干净的 QEMU：脚本会连上去、
    看到完整的启动横幅（那是上一个实例的），然后发字节发到一个已经没人
    收的 chardev 里，表现为“发了他不回” —— 实测踩过一次，很贵。
    """
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


PORT = int(os.environ.get("UART_IRQ_PORT", "0")) or _free_port()
PAYLOAD = b"hello\n"


def fail(msg, log=""):
    print("FAIL: " + msg)
    if log:
        print("---- 串口原始输出 ----")
        print(log)
    return 1


def main():
    if not os.path.isfile(ELF):
        return fail("找不到 " + ELF + "，先在仓库根目录 make -f example/qemu_riscv32/Makefile")

    cmd = [
        QEMU,
        "-machine", "virt",
        "-cpu", "rv32",
        "-bios", "none",
        # 注意给相对文件名 + cwd=BUILD：仓库路径里含非 ASCII（中文）字符，
        # 直接传绝对路径时 QEMU 在 Windows 上打不开内核文件（不报错，只是
        # 起不来一个 vCPU，表现为 socket 永远连不上）。
        "-kernel", os.path.basename(ELF),
        "-display", "none",
        # wait=on：QEMU 先监听、等脚本连上再开始执行。用 wait=off 的话 guest
        # 在 Python 完成 connect 之前就已经把启动横幅跑完了（实测丢了横幅，
        # 看起来像“中断没开起来”），判断失的对象就不存在了。
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
        # QEMU 的 socket server 起来需要一点时间；连不上就重试，不是失败。
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
            """读 seconds 秒，返回这段时间新增的输出。"""
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

        # 1) 等启动横幅（内核初始化 + 打开中断接收）
        pump(6.0)
        if "[boot] uart irq on" not in log:
            return fail("启动横幅里没有 '[boot] uart irq on'（串口中断没开起来）", log)

        # 2) 发字节，等回显
        sock.sendall(PAYLOAD)
        pump(3.0)

        # 3) 判据
        # 不能按行首匹配：回显是异步插进输出流的，很可能插在任务 A 正在
        # 打印的那一行中间（实测就是 "[A] [R] wake=1 ... -> hello."）。
        # 所以按子串切，取到行尾为止。
        echo_lines = []
        pos = 0
        while True:
            at = log.find("[R] ", pos)
            if at < 0:
                break
            end = log.find("\n", at)
            echo_lines.append(log[at:end if end > 0 else len(log)].rstrip("\r"))
            pos = at + 4
        if not echo_lines:
            return fail("没有收到任何 [R] 回显行（中断没有把字节送到任务）", log)

        # 控制台由谁读，决定了 payload 该出现在哪：shell=0 时是本示例的
        # 接收任务（边收边回显），shell=1 时是内核 Shell（响应成一条命令）。
        shell_on = " shell=0" not in log and "shell=" in log

        if shell_on:
            if "hello" not in log or "command not found" not in log:
                return fail("shell 没把 payload 当成一条命令执行（字节没到 shell）", log)
        else:
            has_payload = any("-> hello" in ln for ln in echo_lines)
            if not has_payload:
                return fail("回显行里没有 '-> hello'（字节收到了但内容不对）\n" + "\n".join(echo_lines), log)

        ok_isr = False
        for ln in echo_lines:
            if "isr=" in ln and "rx=" in ln:
                try:
                    isr = int(ln.split("isr=")[1].split()[0])
                    rx = int(ln.split("rx=")[1].split()[0])
                except (IndexError, ValueError):
                    continue
                if isr > 0 and rx > 0:
                    ok_isr = True
        if not ok_isr:
            return fail("回显行里 isr= 或 rx= 为 0（走了轮询而不是中断）\n" + "\n".join(echo_lines), log)

        if "UNEXPECTED" in log or "FATAL" in log:
            return fail("全程出现 UNEXPECTED / FATAL", log)

        print("PASS: 中断驱动串口端到端通")
        print("\n".join(echo_lines))
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
